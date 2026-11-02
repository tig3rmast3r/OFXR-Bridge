#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <safetyhook.hpp>
#include <vector>
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <utility>
#include "xrfg/d3d12_native_binding.hpp"
namespace d3d_native = xrfg::d3d_native;
#include "D3D_DeviceOwner.h"
#include "KcdCameraPacket.h"

namespace luke_scene_camera {
struct Receipt {
    bool installed{}, valid{};
    LONG install_error{};
    unsigned failure{}, attempts{}, reads{}, candidates{}, owner_mask{}, slot{};
    bool graphics{};
    HRESULT error{S_OK};
    std::uint32_t token{};
    std::uintptr_t resource{}, signature{};
    D3D12_GPU_VIRTUAL_ADDRESS location{};
    kcd_camera::Camera camera{};
    std::array<D3D12_GPU_VIRTUAL_ADDRESS,4> bound_addresses{};
    std::array<unsigned,4> bound_slots{};
    std::uint64_t observed_addresses{},observed_maps{},observed_creations{},resource_drops{};
    unsigned resource_count{},unknown_ranges{},resource_families{};
};
namespace detail {
struct Resource { D3D12_GPU_VIRTUAL_ADDRESS gpu{}; std::uint64_t size{}; };
struct Roots {
    std::array<ID3D12RootSignature*,2> signatures{};
    std::array<std::array<D3D12_GPU_VIRTUAL_ADDRESS,32>,2> bindings{};
    std::uint64_t last_bind{}; // order of the list's latest root binding
    std::array<std::array<std::uint64_t,32>,2> bound_at{}; // order of each slot's binding
};
inline std::uint64_t bind_sequence{};
inline std::recursive_mutex mutex;
inline std::recursive_mutex install_mutex;
inline std::unordered_map<ID3D12Resource*,Resource> resources;
inline std::unordered_map<ID3D12GraphicsCommandList*,Roots> roots;
inline bool installed{};
inline LONG install_error{};
inline std::uint32_t evaluations{};
inline std::vector<SafetyHookInline> hooks;
using Address=D3D12_GPU_VIRTUAL_ADDRESS(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ResourceRelease=ULONG(STDMETHODCALLTYPE*)(ID3D12Resource*);
using ListRelease=ULONG(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using Reset=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
using Bind=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,UINT,D3D12_GPU_VIRTUAL_ADDRESS);
using Signature=void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12RootSignature*);
using Map=HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*,UINT,const D3D12_RANGE*,void**);
using Committed=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_HEAP_PROPERTIES*,D3D12_HEAP_FLAGS,
    const D3D12_RESOURCE_DESC*,D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
using Placed=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,ID3D12Heap*,UINT64,const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
using Reserved=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_RESOURCE_DESC*,
    D3D12_RESOURCE_STATES,const D3D12_CLEAR_VALUE*,REFIID,void**);
constexpr unsigned family_capacity=8;
struct ResourceMethods {
    std::atomic<void*> address_entry{},map_entry{},release_entry{};
    std::atomic<Address> address{};std::atomic<Map> map{};std::atomic<ResourceRelease> release{};
    std::atomic_bool active{};
};
struct DeviceMethods {
    void* committed_entry{};void* placed_entry{};void* reserved_entry{};
    Committed committed{};Placed placed{};Reserved reserved{};
};
inline std::array<ResourceMethods,family_capacity> resource_methods;
inline std::array<DeviceMethods,family_capacity> device_methods;
struct HookRecord {void* entry{};void* original{};std::size_t index{};bool active{};};
inline std::vector<HookRecord> hook_records;
#ifdef XRFG_NATIVE_SCENE_CAMERA_TESTS
inline void* denied_entry{};
inline unsigned denied_entry_count{};
#endif
inline std::uint64_t observed_addresses{},observed_maps{},observed_creations{},resource_drops{};
inline std::atomic_uint resource_family_count{};
inline thread_local bool observing_resource{},reading_camera{};
// Two list families: the lists the application records on, and, under a
// proxy (REAL VR wraps lists, then records its own work on the native one),
// the native lists behind them.
constexpr unsigned list_families=2;
inline std::array<ListRelease,list_families> original_list_release{};
inline std::array<Reset,list_families> original_reset{};
inline std::array<Bind,list_families> original_compute{},original_graphics{};
inline std::array<Signature,list_families> original_compute_signature{},original_graphics_signature{};
inline std::array<void*,6*list_families> root_entries{};
template<class Child> inline Microsoft::WRL::ComPtr<ID3D12Device> device_of(Child* child) {
    Microsoft::WRL::ComPtr<ID3D12Device> reported,retained;
    if(FAILED(child->GetDevice(IID_PPV_ARGS(reported.GetAddressOf()))))return retained;
    IUnknown* native=nullptr;
    if(d3d_native::get_borrowed(reported.Get(),&native))native->QueryInterface(IID_PPV_ARGS(retained.GetAddressOf()));
    return retained;
}
inline void record_resource(ID3D12Resource* resource,D3D12_GPU_VIRTUAL_ADDRESS gpu,unsigned source) {
    if(!resource||!gpu||observing_resource||reading_camera)return;
    const auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER)return;
    const auto release=(*reinterpret_cast<void***>(resource))[2];bool lifecycle{};
    for(const auto& family:resource_methods)lifecycle|=family.active.load(std::memory_order_acquire)&&
        family.release_entry.load(std::memory_order_acquire)==release;
    std::scoped_lock lock(mutex);if(!installed)return;
    if(!lifecycle){++resource_drops;return;}
    if(source==0)++observed_addresses;else if(source==1)++observed_maps;else ++observed_creations;
    if(resources.contains(resource))return;
    if(resources.size()>=4096){++resource_drops;return;}
    resources.emplace(resource,Resource{gpu,desc.Width});
}
inline Address unhooked_address(ID3D12Resource* resource) {
    const auto entry=(*reinterpret_cast<void***>(resource))[11];
    for(const auto& family:resource_methods)if(family.address_entry.load(std::memory_order_acquire)==entry)
        return family.address.load(std::memory_order_acquire);
    return reinterpret_cast<Address>(entry);
}
inline void observe_resource(ID3D12Resource* resource,unsigned source) {
    if(!resource||observing_resource||reading_camera)return;
    const auto release=(*reinterpret_cast<void***>(resource))[2];
    bool lifecycle=false;for(const auto& family:resource_methods)
        lifecycle|=family.active.load(std::memory_order_acquire)&&family.release_entry.load(std::memory_order_acquire)==release;
    if(!lifecycle)return; // Never retain a range whose public final release is unobserved.
    struct Restore {~Restore(){observing_resource=false;}} restore;
    observing_resource=true;const auto gpu=unhooked_address(resource)(resource);observing_resource=false;
    record_resource(resource,gpu,source);
}
template<unsigned Index>inline D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE address(ID3D12Resource* resource) {
    const auto gpu=resource_methods[Index].address.load(std::memory_order_acquire)(resource);
    record_resource(resource,gpu,0);return gpu;
}
template<unsigned Index>inline HRESULT STDMETHODCALLTYPE map(ID3D12Resource* resource,UINT subresource,
    const D3D12_RANGE* range,void** data) {
    const auto result=resource_methods[Index].map.load(std::memory_order_acquire)(resource,subresource,range,data);
    if(SUCCEEDED(result))observe_resource(resource,1);return result;
}
template<unsigned Index>inline ULONG STDMETHODCALLTYPE release_resource(ID3D12Resource* resource) {
    std::scoped_lock lock(mutex);
    const auto result=resource_methods[Index].release.load(std::memory_order_acquire)(resource);
    if(!result) {
        if(const auto it=resources.find(resource);it!=resources.end()) {
            const auto range=it->second;
            for(auto& [list,ctx]:roots)for(auto& domain:ctx.bindings)for(auto& gpu:domain)
                if(gpu>=range.gpu&&gpu-range.gpu<range.size)gpu=0;
            resources.erase(it);
        }
        roots.erase(reinterpret_cast<ID3D12GraphicsCommandList*>(resource));
    }
    return result;
}
template<unsigned Family>inline ULONG STDMETHODCALLTYPE release_list(ID3D12GraphicsCommandList* list) {
    std::scoped_lock lock(mutex);const auto result=original_list_release[Family](list);
    if(!result)roots.erase(list);return result;
}
// Entries leave at a list's final Release or its Reset, so this only bounds a
// leak. A list first bound once the map is full gets no record, and a DLSS
// production on it has no camera: Uncharted keeps 280-430 lists alive, and
// at 256 REAL VR's fix binding was lost on 20-60% of frames. About 1 KB each.
inline constexpr std::size_t kMaxTrackedLists = 4096;
inline Roots* context(ID3D12GraphicsCommandList* list) {
    if(auto it=roots.find(list);it!=roots.end())return &it->second;
    if(roots.size()>=kMaxTrackedLists)return nullptr;
    return &roots.emplace(list,Roots{}).first->second;
}
inline void bind(ID3D12GraphicsCommandList* list,bool graphics,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    if(slot>=32)return;
    std::scoped_lock lock(mutex);
    if(installed)if(auto* ctx=context(list)){
        ctx->bindings[graphics?1:0][slot]=gpu;ctx->last_bind=++bind_sequence;ctx->bound_at[graphics?1:0][slot]=bind_sequence;
    }
}
template<unsigned Family>inline void STDMETHODCALLTYPE compute(ID3D12GraphicsCommandList* list,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    bind(list,false,slot,gpu);original_compute[Family](list,slot,gpu);
}
template<unsigned Family>inline void STDMETHODCALLTYPE graphics(ID3D12GraphicsCommandList* list,UINT slot,D3D12_GPU_VIRTUAL_ADDRESS gpu) {
    bind(list,true,slot,gpu);original_graphics[Family](list,slot,gpu);
}
inline void signature(ID3D12GraphicsCommandList* list,bool graphics,ID3D12RootSignature* value) {
    std::scoped_lock lock(mutex);
    if(!installed)return;
    if(auto* ctx=context(list)) {
        const unsigned domain=graphics?1:0;
        if(ctx->signatures[domain]!=value)ctx->bindings[domain]={};
        ctx->signatures[domain]=value;
    }
}
template<unsigned Family>inline void STDMETHODCALLTYPE compute_signature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* value) {
    signature(list,false,value);original_compute_signature[Family](list,value);
}
template<unsigned Family>inline void STDMETHODCALLTYPE graphics_signature(ID3D12GraphicsCommandList* list,ID3D12RootSignature* value) {
    signature(list,true,value);original_graphics_signature[Family](list,value);
}
template<unsigned Family>inline HRESULT STDMETHODCALLTYPE reset(ID3D12GraphicsCommandList* list,ID3D12CommandAllocator* allocator,ID3D12PipelineState* pipeline) {
    const auto result=original_reset[Family](list,allocator,pipeline);
    if(SUCCEEDED(result)){std::scoped_lock lock(mutex);roots.erase(list);}return result;
}
template<class Function>inline Function attach_entry(void* entry,Function callback) {
    for(const auto& record:hook_records)if(record.entry==entry)return reinterpret_cast<Function>(record.original);
#ifdef XRFG_NATIVE_SCENE_CAMERA_TESTS
    if(denied_entry_count&&entry==denied_entry){--denied_entry_count;return nullptr;}
#endif
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(entry),&pinned))return nullptr;
    auto hook=safetyhook::create_inline(entry,reinterpret_cast<void*>(callback),SafetyHookInline::StartDisabled);
    if(!hook)return nullptr;
    auto original=hook.template original<Function>();
    hook_records.push_back({entry,reinterpret_cast<void*>(original),hooks.size(),false});
    hooks.push_back(std::move(hook));return original;
}
inline bool activate_entry(void* entry) {
    for(auto& record:hook_records)if(record.entry==entry) {
        if(!record.active){if(!hooks[record.index].enable())return false;record.active=true;}return true;
    }
    return false;
}
template<std::size_t... Index>inline auto address_callbacks(std::index_sequence<Index...>) {
    return std::array<Address,sizeof...(Index)>{&address<Index>...};
}
template<std::size_t... Index>inline auto map_callbacks(std::index_sequence<Index...>) {
    return std::array<Map,sizeof...(Index)>{&map<Index>...};
}
template<std::size_t... Index>inline auto release_callbacks(std::index_sequence<Index...>) {
    return std::array<ResourceRelease,sizeof...(Index)>{&release_resource<Index>...};
}
inline bool install_resource_methods(ID3D12Resource* resource) {
    if(!resource)return false;
    std::scoped_lock installation(install_mutex);auto** table=*reinterpret_cast<void***>(resource);
    static const auto addresses=address_callbacks(std::make_index_sequence<family_capacity>{});
    static const auto maps=map_callbacks(std::make_index_sequence<family_capacity>{});
    static const auto releases=release_callbacks(std::make_index_sequence<family_capacity>{});
    unsigned selected=family_capacity;
    for(unsigned index=0;index<family_capacity;++index) {
        const auto& family=resource_methods[index];
        if(family.address_entry.load()==table[11]&&family.map_entry.load()==table[8]&&family.release_entry.load()==table[2]){selected=index;break;}
    }
    if(selected==family_capacity)for(unsigned index=0;index<family_capacity;++index)
        if(!resource_methods[index].address_entry.load()){selected=index;break;}
    if(selected!=family_capacity) {
        const auto index=selected;
        auto& family=resource_methods[index];
        // Reserve this index before any dormant callback can refer to it.
        // Each successful original remains immutable even if a later entry
        // fails and another family shares/activates that method first.
        family.address_entry.store(table[11],std::memory_order_release);family.map_entry.store(table[8],std::memory_order_release);
        family.release_entry.store(table[2],std::memory_order_release);
        if(!family.address.load())family.address.store(attach_entry(table[11],addresses[index]),std::memory_order_release);
        if(!family.map.load())family.map.store(attach_entry(table[8],maps[index]),std::memory_order_release);
        if(!family.release.load())family.release.store(attach_entry(table[2],releases[index]),std::memory_order_release);
        if(!family.address.load()||!family.map.load()||!family.release.load())return false;
        if(!activate_entry(table[11])||!activate_entry(table[8])||!activate_entry(table[2]))return false;
        if(!family.active.exchange(true,std::memory_order_release))resource_family_count.fetch_add(1,std::memory_order_relaxed);
        return true;
    }
    return false;
}
inline void observe_created(void** output) {
    if(!output||!*output)return;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if(SUCCEEDED(static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(resource.GetAddressOf())))&&
        resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&
        install_resource_methods(resource.Get()))observe_resource(resource.Get(),2);
}
template<unsigned Index>inline HRESULT STDMETHODCALLTYPE committed(ID3D12Device* device,
    const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,const D3D12_RESOURCE_DESC* desc,
    D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    const auto result=device_methods[Index].committed(device,heap,flags,desc,state,clear,iid,output);
    if(SUCCEEDED(result))observe_created(output);return result;
}
template<unsigned Index>inline HRESULT STDMETHODCALLTYPE placed(ID3D12Device* device,ID3D12Heap* heap,
    UINT64 offset,const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,
    const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    const auto result=device_methods[Index].placed(device,heap,offset,desc,state,clear,iid,output);
    if(SUCCEEDED(result))observe_created(output);return result;
}
template<unsigned Index>inline HRESULT STDMETHODCALLTYPE reserved(ID3D12Device* device,
    const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,REFIID iid,void** output) {
    const auto result=device_methods[Index].reserved(device,desc,state,clear,iid,output);
    if(SUCCEEDED(result))observe_created(output);return result;
}
template<std::size_t... Index>inline auto committed_callbacks(std::index_sequence<Index...>) {
    return std::array<Committed,sizeof...(Index)>{&committed<Index>...};
}
template<std::size_t... Index>inline auto placed_callbacks(std::index_sequence<Index...>) {
    return std::array<Placed,sizeof...(Index)>{&placed<Index>...};
}
template<std::size_t... Index>inline auto reserved_callbacks(std::index_sequence<Index...>) {
    return std::array<Reserved,sizeof...(Index)>{&reserved<Index>...};
}
inline bool install_creation_methods(ID3D12Device* device) {
    if(!device)return false;
    std::scoped_lock installation(install_mutex);auto** table=*reinterpret_cast<void***>(device);
    static const auto committed=committed_callbacks(std::make_index_sequence<family_capacity>{});
    static const auto placed=placed_callbacks(std::make_index_sequence<family_capacity>{});
    static const auto reserved=reserved_callbacks(std::make_index_sequence<family_capacity>{});
    unsigned selected=family_capacity;
    for(unsigned index=0;index<family_capacity;++index) {
        const auto& family=device_methods[index];if(family.committed_entry==table[27]&&
            family.placed_entry==table[29]&&family.reserved_entry==table[30]){selected=index;break;}
    }
    if(selected==family_capacity)for(unsigned index=0;index<family_capacity;++index)
        if(!device_methods[index].committed_entry){selected=index;break;}
    if(selected!=family_capacity) {
        const auto index=selected;
        auto& family=device_methods[index];
        family.committed_entry=table[27];family.placed_entry=table[29];family.reserved_entry=table[30];
        if(!family.committed)family.committed=attach_entry(table[27],committed[index]);
        if(!family.placed)family.placed=attach_entry(table[29],placed[index]);
        if(!family.reserved)family.reserved=attach_entry(table[30],reserved[index]);
        if(!family.committed||!family.placed||!family.reserved)return false;
        return activate_entry(table[27])&&activate_entry(table[29])&&activate_entry(table[30]);
    }
    return false;
}
} // detail
inline bool install(ID3D12Device* device) {
    std::scoped_lock installation(detail::install_mutex);
    if (detail::installed) return true;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    const auto cleanup = [&] {
        if (list) { list->Close(); list->Release(); }
        if (allocator) allocator->Release();
        if (resource) resource->Release();
    };
    if (!device || FAILED(device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&resource))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator, nullptr, IID_PPV_ARGS(&list)))) {
        detail::install_error = ERROR_NOT_SUPPORTED;
        cleanup();
        return false;
    }
    auto** list_vtable = *reinterpret_cast<void***>(list);
    LONG error = detail::install_resource_methods(resource)?NO_ERROR:ERROR_NOT_SUPPORTED;
    // Allocation-owner QI is a public, backing-verified native contract. Probe
    // that family's resource methods as well as the caller's proxy methods;
    // a cached native pointer must retain its own original trampoline.
    Microsoft::WRL::ComPtr<ID3D12Device> owner;
    if(SUCCEEDED(d3d_native::allocation_owner(device,owner))&&owner.Get()!=device) {
        Microsoft::WRL::ComPtr<ID3D12Resource> native_resource;
        if(FAILED(owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(native_resource.GetAddressOf())))||
            !detail::install_resource_methods(native_resource.Get()))error=ERROR_NOT_SUPPORTED;
    }
    if(!detail::install_creation_methods(device)||
        (owner&&owner.Get()!=device&&!detail::install_creation_methods(owner.Get())))error=ERROR_NOT_SUPPORTED;
    const auto attach = [&](void** vtable,unsigned index,unsigned slot,auto& original,auto callback) {
        if(error!=NO_ERROR)return;
        if(detail::root_entries[index]&&detail::root_entries[index]!=vtable[slot]){error=ERROR_NOT_SUPPORTED;return;}
        const auto resolved=detail::attach_entry(vtable[slot],callback);
        if(!resolved){error=ERROR_NOT_SUPPORTED;return;}
        original=resolved;detail::root_entries[index]=vtable[slot];
    };
    // A shared release entry already retires both exact resource and list keys.
    const auto attach_family=[&]<unsigned Family>(void** vtable) {
        attach(vtable,Family*6+0,2,detail::original_list_release[Family],detail::release_list<Family>);
        attach(vtable,Family*6+1,10,detail::original_reset[Family],detail::reset<Family>);
        attach(vtable,Family*6+2,29,detail::original_compute_signature[Family],detail::compute_signature<Family>);
        attach(vtable,Family*6+3,30,detail::original_graphics_signature[Family],detail::graphics_signature<Family>);
        attach(vtable,Family*6+4,37,detail::original_compute[Family],detail::compute<Family>);
        attach(vtable,Family*6+5,38,detail::original_graphics[Family],detail::graphics<Family>);
    };
    attach_family.template operator()<0>(list_vtable);
    // The native lists behind a proxy device's: a proxy's own recording
    // (REAL VR's DLSS fix) reaches only these entries.
    ID3D12CommandAllocator* owner_allocator = nullptr;
    ID3D12GraphicsCommandList* owner_list = nullptr;
    if(error==NO_ERROR&&owner&&owner.Get()!=device&&
        SUCCEEDED(owner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&owner_allocator)))&&
        SUCCEEDED(owner->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,owner_allocator,nullptr,IID_PPV_ARGS(&owner_list)))) {
        auto** owner_vtable=*reinterpret_cast<void***>(owner_list);
        bool distinct=false;
        for(const unsigned slot:{2u,10u,29u,30u,37u,38u})distinct|=owner_vtable[slot]!=list_vtable[slot];
        if(distinct)attach_family.template operator()<1>(owner_vtable);
    }
    if(owner_list){owner_list->Close();owner_list->Release();}
    if(owner_allocator)owner_allocator->Release();
    if (error == NO_ERROR) {
        for(const auto entry:detail::root_entries)if(entry&&!detail::activate_entry(entry))error=ERROR_NOT_SUPPORTED;
    }
    {
        std::scoped_lock lock(detail::mutex);
        detail::install_error = error;
        detail::installed = error == NO_ERROR;
    }
    cleanup();
    return detail::installed;
}

inline bool uninstall() {
    std::scoped_lock installation(detail::install_mutex);
    if (!detail::installed) return true;
    std::scoped_lock lock(detail::mutex);
    // The standalone layer is process-pinned. Keep live trampolines outside the
    // loader lock and clear observation state after renderer quiescence.
    detail::installed = false;
    detail::resources.clear();
    detail::roots.clear();


    return true;
}



// Consume only this production's still-bound constants on its exact native list.
// All addresses are consumed once. No global newest camera, old packet, eye guess,
// time window, GPU readback, retained resource reference or camera fallback.
inline Receipt take(ID3D12GraphicsCommandList* list,unsigned width,unsigned height) {
    std::scoped_lock lock(detail::mutex);
    struct RestoreRead {bool previous{detail::reading_camera};RestoreRead(){detail::reading_camera=true;}
        ~RestoreRead(){detail::reading_camera=previous;}} reading;
    Receipt result{};result.installed=detail::installed;result.install_error=detail::install_error;
    result.resource_count=static_cast<unsigned>(detail::resources.size());
    result.resource_families=detail::resource_family_count.load(std::memory_order_relaxed);
    result.observed_addresses=detail::observed_addresses;result.observed_maps=detail::observed_maps;
    result.observed_creations=detail::observed_creations;result.resource_drops=detail::resource_drops;
    result.token=++detail::evaluations;
    if(!result.token)result.token=++detail::evaluations;
    IUnknown* native=nullptr;
    if(!detail::installed||!d3d_native::get_borrowed(list,&native)){result.failure=1;return result;}
    auto* actual=reinterpret_cast<ID3D12GraphicsCommandList*>(native);
    const auto it=detail::roots.find(actual);
    if(it==detail::roots.end()){result.failure=2;return result;}
    auto& ctx=it->second;
    const auto bindings=ctx.bindings;
    ctx.bindings={}; // A repeated NGX call cannot borrow the preceding production.
    const auto list_device=detail::device_of(actual);
    std::array<D3D12_GPU_VIRTUAL_ADDRESS,4> seen{};unsigned seen_count{};
    // The observed KCD2 per-view layout binds at slots1/2 in either domain.
    for(unsigned domain:{1u,0u})for(unsigned slot:{1u,2u}) {
        const auto gpu=bindings[domain][slot];
        if(!gpu||!ctx.signatures[domain])continue;
        bool duplicate=false;for(unsigned n=0;n<seen_count;++n)duplicate|=seen[n]==gpu;
        if(duplicate)continue;
        result.bound_addresses[seen_count]=gpu;result.bound_slots[seen_count]=(domain==1?0x100U:0U)|slot;
        seen[seen_count++]=gpu;++result.attempts;bool matched{};
        for(const auto& [resource,range]:detail::resources) {
            if(gpu<range.gpu||gpu-range.gpu>=range.size)continue;
            matched=true;
            const auto offset=gpu-range.gpu;
            if(range.size-offset<kcd_camera::packet_bytes){result.failure|=4;continue;}
            const auto resource_device=detail::device_of(resource);
            d3d_owner::Receipt proof;
            if(!d3d_owner::same(list_device.Get(),resource_device.Get(),&proof)) {
                result.failure|=8;result.error=E_ACCESSDENIED;continue;
            }
            std::array<unsigned char,kcd_camera::packet_bytes> bytes{};
            void* cpu=nullptr;D3D12_RANGE read_range{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset)+bytes.size()};
            const auto mapped=resource->Map(0,&read_range,&cpu);
            result.error=mapped;
            if(FAILED(mapped)){result.failure|=16;break;}
            SIZE_T read{};
            const bool copied=cpu&&ReadProcessMemory(GetCurrentProcess(),static_cast<const unsigned char*>(cpu)+offset,
                bytes.data(),bytes.size(),&read)&&read==bytes.size();
            D3D12_RANGE no_write{};resource->Unmap(0,&no_write);
            if(!copied){result.failure|=32;break;}
            ++result.reads;kcd_camera::Camera camera{};
            if(!kcd_camera::decode(bytes.data(),bytes.size(),width,height,camera)){result.failure|=64;break;}
            ++result.candidates;
            if(result.candidates==1) {
                result.camera=camera;result.resource=reinterpret_cast<std::uintptr_t>(resource);
                result.signature=reinterpret_cast<std::uintptr_t>(ctx.signatures[domain]);
                result.location=gpu;result.graphics=domain==1;result.slot=slot;result.owner_mask=proof.mask;
            } else if(!kcd_camera::equivalent(camera,result.camera))result.failure|=128;
            break;
        }
        if(!matched)++result.unknown_ranges;
    }
    result.valid=result.candidates!=0&&(result.failure&128)==0;
    if(!result.valid)result.camera={};
    return result;
}
} // namespace luke_scene_camera
