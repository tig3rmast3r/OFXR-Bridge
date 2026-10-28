#include "xrfg/native_ingress.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/neural_rendering.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include "xrfg/native_ingress/LukeNativeSceneCamera.h"
#include "xrfg/native_ingress/RealVrDlssCamera.h"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include <safetyhook.hpp>
#include <windows.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <mutex>
#include <optional>
#include <vector>
#include <atomic>

namespace xrfg::native_ingress {
namespace {
using Microsoft::WRL::ComPtr;
struct Hooks {
    std::mutex mutex;
    SafetyHookInline execute, create_queue, reset_list;
    std::uintptr_t execute_entry{}; // diagnostics only, assigned on hook installation
    bool execute_probe_active{};
    ULONGLONG execute_probe_retry_at{};
    using Execute=void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*,UINT,ID3D12CommandList* const*);
    using CreateQueue=HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*,const D3D12_COMMAND_QUEUE_DESC*,REFIID,void**);
    using ResetList=HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*,ID3D12CommandAllocator*,ID3D12PipelineState*);
    Execute submit{};
    CreateQueue make_queue{};
    ResetList reset{};
    std::vector<ComPtr<ID3D12CommandQueue>> queues;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> luke_lists;
    ComPtr<ID3D12Device> luke_device;
    ComPtr<ID3D12CommandQueue> luke_queue;
};
Hooks& hooks(){static auto* value=new Hooks;return *value;}
std::atomic_uint64_t camera_accepted{},camera_rejected{},submissions{};
std::atomic_uint32_t handles{};
std::atomic_uint pending_luke_lists{};
std::atomic_bool has_luke_context{};
std::atomic_bool has_reset_hook{};
std::atomic<ULONGLONG> reset_hook_retry_at{};
template<std::size_t N> bool finite(const std::array<float,N>& values) {
    for(float v:values)if(!std::isfinite(v))return false;
    return true;
}
void attach_real_vr_cameras(unsigned count,ID3D12CommandList* const* lists) noexcept;
void STDMETHODCALLTYPE execute(ID3D12CommandQueue* queue,UINT count,ID3D12CommandList* const* lists) {
    ++submissions;
    std::vector<ComPtr<ID3D12CommandList>> owners;
    std::vector<ID3D12CommandList*> native_lists;
    bool normalization_failed=count && !lists;
    owners.reserve(count);native_lists.reserve(count);
    for(UINT i=0;lists&&i<count;++i) {
        IUnknown* native{};ComPtr<ID3D12CommandList> owner;
        if(d3d_native::get_borrowed(lists[i],&native)&&SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&owner)))) {
            native_lists.push_back(owner.Get());owners.push_back(std::move(owner));
        } else normalization_failed=true;
    }
    auto receipts=neural_rendering::take_submission_receipts(
        static_cast<unsigned>(native_lists.size()),native_lists.data());
    if(normalization_failed) {
        // The original still receives EVERY application list. An omitted
        // identity may own a pending copy, so none may remain Reset-cancelable.
        // A null identity invokes the allocation-free conservative quarantine.
        ID3D12CommandList* unidentified[]{nullptr};
        begin_dlss_motion_vector_submission(1,unidentified);
    }
    begin_dlss_motion_vector_submission(static_cast<unsigned>(native_lists.size()),native_lists.data());
    try {hooks().submit(queue,count,lists);}
    catch(...) {
        abandon_dlss_motion_vector_submission(static_cast<unsigned>(native_lists.size()),native_lists.data());
        throw;
    }
    // REAL VR's queue proxy fills its DLSS-fix constants inside its own
    // ExecuteCommandLists, which this entry may sit on or behind: after the
    // forwarded call they are complete either way, and stay until its next.
    attach_real_vr_cameras(count,lists);
    IUnknown* unwrapped{};ComPtr<ID3D12CommandQueue> actual;
    if(d3d_native::get_borrowed(queue,&unwrapped))unwrapped->QueryInterface(IID_PPV_ARGS(&actual));
    if(actual) {
        // The original Execute has accepted these exact lists. Resolve original
        // ownership before publishing its context; never substitute our queue.
        if(pending_luke_lists.load(std::memory_order_acquire)) {
            auto& h=hooks();std::scoped_lock lock(h.mutex);
            for(auto* list:native_lists) {
                auto it=std::find_if(h.luke_lists.begin(),h.luke_lists.end(),
                    [&](const auto& pending){return pending.Get()==list;});
                if(it==h.luke_lists.end())continue;
                ComPtr<ID3D12Device> owner,reported,canonical;
                IUnknown* native_device{};
                if(SUCCEEDED((*it)->GetDevice(IID_PPV_ARGS(&owner))) &&
                    actual->GetDesc().Type==D3D12_COMMAND_LIST_TYPE_DIRECT &&
                    SUCCEEDED(actual->GetDevice(IID_PPV_ARGS(&reported))) &&
                    d3d_native::get_borrowed(reported.Get(),&native_device) &&
                    SUCCEEDED(native_device->QueryInterface(IID_PPV_ARGS(&canonical))) &&
                    SUCCEEDED(d3d_native::allocation_owner(reported.Get(),canonical)) &&
                    d3d_owner::same(owner.Get(),canonical.Get())) {
                    // Accepted V30063 backing-owner challenge handles native
                    // device aliases; adapter equality alone never proves it.
                    static std::atomic_uint owner_reports{};
                    if(owner_reports.fetch_add(1,std::memory_order_relaxed)<16)
                        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                            S_OK,960,reinterpret_cast<std::uintptr_t>(reported.Get()),
                            reinterpret_cast<std::uintptr_t>(canonical.Get()));
                    h.luke_device=std::move(canonical);h.luke_queue=actual;
                    has_luke_context.store(true,std::memory_order_release);
                }
                h.luke_lists.erase(it);
            }
            pending_luke_lists.store(static_cast<unsigned>(h.luke_lists.size()),std::memory_order_release);
        }
        report_dlss_motion_vector_submission(actual.Get(),static_cast<unsigned>(native_lists.size()),native_lists.data(),true);
        two_frame_capture_submission(actual.Get(),static_cast<unsigned>(native_lists.size()),native_lists.data());
        neural_rendering::complete_submission(actual.Get(),receipts);
    } else abandon_dlss_motion_vector_submission(static_cast<unsigned>(native_lists.size()),native_lists.data());
}
HRESULT STDMETHODCALLTYPE create_queue(ID3D12Device* device,const D3D12_COMMAND_QUEUE_DESC* desc,
    REFIID iid,void** output) {
    const auto result=hooks().make_queue(device,desc,iid,output);
    if(SUCCEEDED(result)&&output&&*output&&desc&&desc->Type==D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ComPtr<ID3D12CommandQueue> queue;
        static_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&queue));
        if(queue)configure_queue(queue.Get());
    }
    return result;
}
HRESULT STDMETHODCALLTYPE reset_command_list(ID3D12GraphicsCommandList* list,
    ID3D12CommandAllocator* allocator,ID3D12PipelineState* initial) {
    const auto result=hooks().reset(list,allocator,initial);
    if(SUCCEEDED(result))report_dlss_motion_vector_reset(list);
    return result;
}
bool kcd() {
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    auto* basename=wcsrchr(path,L'\\');basename=basename?basename+1:path;
    return _wcsicmp(basename,L"KingdomCome.exe")==0;
}
// A guide published without its camera: REAL VR's DLSS-fix buffer bound on
// its list, to be read when that list executes.
struct PendingRealVrCamera {
    std::uint64_t stream{},publication{};
    ComPtr<ID3D12Resource> resource;std::uint64_t offset{};
    ComPtr<IUnknown> list; // canonical identity of the producer list
    ULONGLONG recorded_ms{};
    std::uint32_t motion_width{},motion_height{}; // the guide's, to recognise REAL VR's buffers
};
struct RealVrCameraState {
    std::mutex mutex;
    std::vector<PendingRealVrCamera> pending;
    std::optional<PendingRealVrCamera> taken; // the binding found by the capture in progress
    real_vr_dlss_camera::Chain chain; // attach order is publication order
    std::vector<std::pair<ComPtr<ID3D12Resource>,std::uint64_t>> slots; // REAL VR's constant buffers seen bound
    ULONGLONG last_discovery{};
    std::atomic_uint attached{},unreadable{},restarts{};
};
RealVrCameraState& real_vr_camera(){static auto* value=new RealVrCameraState;return *value;}
bool scene_camera_wanted() {
    return kcd() || (luke_detected() && GetModuleHandleW(L"sl.interposer.dll")==nullptr);
}
}
// Caller owns Hooks::mutex. Installing an entry point does not establish a
// producer: only an observed SR/RR list submitted through it can do that.
static bool install_execute_hook(Hooks& s, ID3D12CommandQueue* queue) {
    if (!s.execute) {
        auto** table=*reinterpret_cast<void***>(queue);
        s.execute=safetyhook::create_inline(table[10],reinterpret_cast<void*>(&execute),SafetyHookInline::StartDisabled);
        if(s.execute){s.execute_entry=reinterpret_cast<std::uintptr_t>(table[10]);s.submit=s.execute.original<Hooks::Execute>();(void)s.execute.enable();}
    }
    return bool(s.execute);
}
void configure_queue(ID3D12CommandQueue* input) noexcept {
    try {
        IUnknown* native{};ComPtr<ID3D12CommandQueue> queue;
        if(!d3d_native::get_borrowed(input,&native)||FAILED(native->QueryInterface(IID_PPV_ARGS(&queue))))return;
        auto& s=hooks();std::scoped_lock lock(s.mutex);
        if(std::none_of(s.queues.begin(),s.queues.end(),[&](auto& q){return q.Get()==queue.Get();}))s.queues.push_back(queue);
        static_cast<void>(install_execute_hook(s,queue.Get()));
        ComPtr<ID3D12Device> device;if(SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))&&scene_camera_wanted())
            luke_scene_camera::install(device.Get());
    }catch(...) {}
}
void install_device(ID3D12Device* input) noexcept {
    bool probing=false;
    try {
        IUnknown* native{};ComPtr<ID3D12Device> device;
        if(!d3d_native::get_borrowed(input,&native)||FAILED(native->QueryInterface(IID_PPV_ARGS(&device))))return;
        auto& s=hooks();Hooks::CreateQueue create{};
        {
            std::scoped_lock lock(s.mutex);
            if(!s.create_queue) {
                auto** table=*reinterpret_cast<void***>(device.Get());
                s.create_queue=safetyhook::create_inline(table[8],reinterpret_cast<void*>(&create_queue),SafetyHookInline::StartDisabled);
                if(s.create_queue){s.make_queue=s.create_queue.original<Hooks::CreateQueue>();(void)s.create_queue.enable();}
            }
            // A failed D11 helper-device creation leaves no registered queue.
            // The game may have created all its queues before hooks existed.
            if(!s.execute && !s.execute_probe_active) {
                const auto now=GetTickCount64();
                if(now>=s.execute_probe_retry_at) {
                    probing=s.execute_probe_active=true;
                    s.execute_probe_retry_at=now+1000;
                    create=s.make_queue;
                }
            }
        }
        if(probing) {
            // Discover the existing native implementation through a temporary
            // queue on the proven SR device, not another D3D12CreateDevice.
            // No lists are executed; the probe is not retained or published.
            D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> probe,canonical;
            HRESULT result=create?create(device.Get(),&desc,IID_PPV_ARGS(&probe)):
                device->CreateCommandQueue(&desc,IID_PPV_ARGS(&probe));
            IUnknown* borrowed{};
            if(SUCCEEDED(result)) {
                if(!d3d_native::get_borrowed(probe.Get(),&borrowed))result=E_NOINTERFACE;
                else result=borrowed->QueryInterface(IID_PPV_ARGS(&canonical));
            }
            {
                std::scoped_lock lock(s.mutex);
                if(SUCCEEDED(result) && !install_execute_hook(s,canonical.Get()))result=E_FAIL;
                s.execute_probe_active=false;probing=false;
            }
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,result,961,
                reinterpret_cast<std::uintptr_t>(device.Get()),0);
        }
        if(scene_camera_wanted())luke_scene_camera::install(device.Get());
    }catch(...) {
        if(probing){auto& s=hooks();std::scoped_lock lock(s.mutex);s.execute_probe_active=false;}
    }
}
void observe_guide_list(ID3D12GraphicsCommandList* input) noexcept {
    if(!input || has_reset_hook.load(std::memory_order_acquire))return;
    const auto now=GetTickCount64();
    if(now<reset_hook_retry_at.load(std::memory_order_relaxed))return;
    try {
        IUnknown* borrowed{};ComPtr<ID3D12GraphicsCommandList> list;
        if(!d3d_native::get_borrowed(input,&borrowed) ||
            FAILED(borrowed->QueryInterface(IID_PPV_ARGS(&list))))return;
        auto& state=hooks();std::scoped_lock lock(state.mutex);
        if(has_reset_hook.load(std::memory_order_acquire))return;
        reset_hook_retry_at.store(now+1000,std::memory_order_relaxed);
        auto** table=*reinterpret_cast<void***>(list.Get());
        state.reset_list=safetyhook::create_inline(table[10],reinterpret_cast<void*>(&reset_command_list),SafetyHookInline::StartDisabled);
        if(state.reset_list) {
            state.reset=state.reset_list.original<Hooks::ResetList>();
            if(state.reset_list.enable())has_reset_hook.store(true,std::memory_order_release);
            else {state.reset_list={};state.reset=nullptr;}
        }
    }catch(...){}
}
void observe_luke_production(ID3D12GraphicsCommandList* input) noexcept {
    observe_guide_list(input);
    if(!input || !luke_detected())return;
    try {
        IUnknown* borrowed{};ComPtr<ID3D12GraphicsCommandList> list;
        if(!d3d_native::get_borrowed(input,&borrowed) ||
            FAILED(borrowed->QueryInterface(IID_PPV_ARGS(&list))))return;
        ComPtr<ID3D12Device> device;
        if(FAILED(list->GetDevice(IID_PPV_ARGS(&device))))return;
        install_device(device.Get());
        auto& h=hooks();std::scoped_lock lock(h.mutex);
        if(std::none_of(h.luke_lists.begin(),h.luke_lists.end(),
            [&](const auto& pending){return pending.Get()==list.Get();})) {
            if(h.luke_lists.size()==32)h.luke_lists.erase(h.luke_lists.begin());
            h.luke_lists.push_back(std::move(list));
            pending_luke_lists.store(static_cast<unsigned>(h.luke_lists.size()),std::memory_order_release);
        }
    }catch(...) {}
}
namespace {
bool real_vr_camera_source_wanted() noexcept {
    return luke_detected() && !kcd() && GetModuleHandleW(L"sl.interposer.dll")==nullptr;
}
// Reads `bytes` of the buffer at `offset` through the resource's own Map.
bool read_buffer(ID3D12Resource* resource,std::uint64_t offset,unsigned char* out,std::size_t bytes) noexcept {
    try {
        void* cpu=nullptr;const D3D12_RANGE range{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset+bytes)};
        if(FAILED(resource->Map(0,&range,&cpu))||!cpu)return false;
        SIZE_T read{};
        const bool copied=ReadProcessMemory(GetCurrentProcess(),static_cast<const unsigned char*>(cpu)+offset,out,bytes,&read)&&read==bytes;
        const D3D12_RANGE no_write{};resource->Unmap(0,&no_write);
        return copied;
    }catch(...){return false;}
}
void report_real_vr_camera(std::int64_t result,std::uint64_t code,std::uint64_t b,std::uint64_t c) noexcept {
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,result,code,b,c);
}
}
std::uint64_t real_vr_bind_mark() noexcept {
    if(!real_vr_camera_source_wanted())return 0;
    try {std::scoped_lock scene(luke_scene_camera::detail::mutex);return luke_scene_camera::detail::bind_sequence;}
    catch(...){return 0;}
}
bool take_real_vr_camera_binding(ID3D12GraphicsCommandList* list,
    std::uint32_t motion_width,std::uint32_t motion_height,std::uint64_t bound_after) noexcept {
    auto& state=real_vr_camera();
    // Lock order is the scene tracker's mutex, then state.mutex. Never
    // release a list or resource while holding state.mutex alone: their
    // Release is hooked and takes the scene mutex (Uncharted froze at start).
    std::optional<PendingRealVrCamera> stale;
    {std::scoped_lock lock(state.mutex);stale=std::move(state.taken);state.taken.reset();}
    stale.reset();
    if(!list || !real_vr_camera_source_wanted())return false;
    try {
        IUnknown* native=nullptr;
        if(!d3d_native::get_borrowed(list,&native))return false;
        auto* actual=reinterpret_cast<ID3D12GraphicsCommandList*>(native);
        ComPtr<IUnknown> identity;
        if(FAILED(actual->QueryInterface(IID_PPV_ARGS(identity.GetAddressOf()))))return false;
        std::scoped_lock scene(luke_scene_camera::detail::mutex);
        if(!luke_scene_camera::detail::installed)return false;
        // REAL VR records its fix dispatch inside its own EvaluateFeature
        // hook, just before this call, on the native list behind the proxy
        // it hands the game: not necessarily the list this call sees. The
        // most recently bound compute constants of that shape are its, and
        // only if bound since this evaluation began (bound_after): when REAL
        // VR leaves a frame unfixed, an older binding belongs to another
        // frame. The bytes hold an older frame's constants, enough for the shape.
        std::vector<std::pair<std::uint64_t,D3D12_GPU_VIRTUAL_ADDRESS>> recent;
        for(const auto& [unused,ctx]:luke_scene_camera::detail::roots)
            for(unsigned slot=0;slot<ctx.bindings[0].size();++slot)
                if(ctx.bindings[0][slot]&&ctx.bound_at[0][slot]>bound_after)
                    recent.emplace_back(ctx.bound_at[0][slot],ctx.bindings[0][slot]);
        std::sort(recent.begin(),recent.end(),[](const auto& a,const auto& b){return a.first>b.first;});
        for(const auto& [unused,gpu]:recent) {
            for(const auto& [resource,range]:luke_scene_camera::detail::resources) {
                if(gpu<range.gpu || gpu-range.gpu>=range.size)continue;
                const auto offset=gpu-range.gpu;
                if(range.size-offset<real_vr_dlss_camera::buffer_bytes)break;
                std::array<unsigned char,real_vr_dlss_camera::buffer_bytes> bytes{};
                if(!read_buffer(resource,offset,bytes.data(),bytes.size()))break;
                if(!real_vr_dlss_camera::looks_like(bytes.data(),bytes.size(),motion_width,motion_height))break;
                PendingRealVrCamera taken{};
                taken.resource=resource;taken.offset=offset;taken.list=identity;taken.recorded_ms=GetTickCount64();
                std::scoped_lock lock(state.mutex);
                // Remember REAL VR's constant buffers for evaluations whose
                // binding is not seen.
                if(std::none_of(state.slots.begin(),state.slots.end(),[&](const auto& slot) {
                        return slot.first.Get()==resource&&slot.second==offset;}) && state.slots.size()<64)
                    state.slots.emplace_back(resource,offset);
                state.taken=std::move(taken);
                return true;
            }
        }
        std::scoped_lock lock(state.mutex);
        // Not seen bound: still publish, and at execution take whichever of
        // REAL VR's buffers holds the frame that continues the chain. Its
        // buffers are 464 bytes exactly; any tracked one makes that possible.
        if(state.slots.empty() && std::none_of(luke_scene_camera::detail::resources.begin(),luke_scene_camera::detail::resources.end(),
                [](const auto& entry){return entry.second.size>=real_vr_dlss_camera::buffer_bytes&&entry.second.size<=4096;}))
            return false;
        PendingRealVrCamera taken{};
        taken.list=identity;taken.recorded_ms=GetTickCount64(); // no resource: chosen by content
        taken.motion_width=motion_width;taken.motion_height=motion_height;
        state.taken=std::move(taken);
        return true;
    }catch(...){return false;}
}
void register_pending_real_vr_camera(std::uint64_t stream,std::uint64_t publication) noexcept {
    auto& state=real_vr_camera();
    std::optional<PendingRealVrCamera> dropped; // released after the lock (see take_real_vr_camera_binding)
    try {
        std::scoped_lock lock(state.mutex);
        if(!state.taken)return;
        auto pending=std::move(*state.taken);state.taken.reset();
        pending.stream=stream;pending.publication=publication;
        if(state.pending.size()>=16){dropped=std::move(state.pending.front());state.pending.erase(state.pending.begin());}
        state.pending.push_back(std::move(pending));
    }catch(...){}
}
namespace {
// At a submission that executes a pending guide's list: read the buffer REAL
// VR has filled for it and attach the camera.
void attach_real_vr_cameras(unsigned count,ID3D12CommandList* const* lists) noexcept {
    auto& state=real_vr_camera();
    // Everything holding a list or resource reference is released after the
    // lock: see take_real_vr_camera_binding.
    std::vector<PendingRealVrCamera> due,expired;
    std::vector<ComPtr<IUnknown>> identities;
    try {
        {std::scoped_lock lock(state.mutex);if(state.pending.empty())return;}
        identities.reserve(count);
        for(unsigned i=0;i<count;++i){ComPtr<IUnknown> id;if(lists[i])lists[i]->QueryInterface(IID_PPV_ARGS(id.GetAddressOf()));identities.push_back(std::move(id));}
        std::scoped_lock lock(state.mutex);
        const auto now=GetTickCount64();
        for(auto it=state.pending.begin();it!=state.pending.end();) {
            bool submitted=false;
            for(unsigned i=0;i<count && !submitted;++i)submitted=identities[i]&&identities[i].Get()==it->list.Get();
            if(submitted){due.push_back(std::move(*it));it=state.pending.erase(it);}
            else if(now-it->recorded_ms>2000) {
                // 1184: a pending guide's list was never seen executing here.
                if(state.unreadable.fetch_add(1,std::memory_order_relaxed)<8)report_real_vr_camera(-1,1184,it->publication,count);
                expired.push_back(std::move(*it));it=state.pending.erase(it);
            }
            else ++it;
        }
    }catch(...){return;}
    for(const auto& entry:due) {
        std::array<unsigned char,real_vr_dlss_camera::buffer_bytes> bytes{};
        real_vr_dlss_camera::Camera relative{};
        if(!entry.resource) {
            // Binding not seen: the known buffer whose constants are a new
            // frame stepping from a held one, the nearest such step first.
            // REAL VR creates four 464-byte constant buffers per DLSS instance
            // ("DLSS constant buffer #"), likely rounded up to 512; after a
            // handle switch the new instance's may never have been seen
            // bound. At most twice a second, take every tracked buffer of
            // 464 bytes to 4 KB whose start holds constants of REAL VR's
            // shape for this frame's motion size.
            const auto now=GetTickCount64();
            bool discover{};
            {std::scoped_lock lock(state.mutex);
                discover=now-state.last_discovery>=500;if(discover)state.last_discovery=now;}
            if(discover)try {
                std::vector<ComPtr<ID3D12Resource>> sized;
                {std::scoped_lock scene(luke_scene_camera::detail::mutex);
                    for(const auto& [resource,range]:luke_scene_camera::detail::resources)
                        if(range.size>=real_vr_dlss_camera::buffer_bytes&&range.size<=4096)sized.emplace_back(resource);}
                for(auto& resource:sized) {
                    std::array<unsigned char,real_vr_dlss_camera::buffer_bytes> head{};
                    if(!read_buffer(resource.Get(),0,head.data(),head.size()) ||
                        !real_vr_dlss_camera::looks_like(head.data(),head.size(),entry.motion_width,entry.motion_height))continue;
                    std::scoped_lock lock(state.mutex);
                    if(std::none_of(state.slots.begin(),state.slots.end(),[&](const auto& slot){return slot.first==resource&&slot.second==0;})
                        && state.slots.size()<64)state.slots.emplace_back(std::move(resource),0);
                }
            }catch(...){}
            std::vector<std::pair<ComPtr<ID3D12Resource>,std::uint64_t>> slots;
            {std::scoped_lock lock(state.mutex);slots=state.slots;}
            unsigned best{};
            for(const auto& [resource,offset]:slots) {
                std::array<unsigned char,real_vr_dlss_camera::buffer_bytes> candidate{};
                if(!read_buffer(resource.Get(),offset,candidate.data(),candidate.size()))continue;
                const auto decoded=real_vr_dlss_camera::decode(candidate.data(),candidate.size());
                if(!decoded.valid)continue;
                unsigned back{};
                {std::scoped_lock lock(state.mutex);back=state.chain.continues(decoded);}
                if(back && (!best || back<best)){best=back;relative=decoded;}
            }
            if(!best)continue; // the guide stays without a camera
        } else {
            if(!read_buffer(entry.resource.Get(),entry.offset,bytes.data(),bytes.size())) {
                if(state.unreadable.fetch_add(1,std::memory_order_relaxed)<8)report_real_vr_camera(-1,1181,entry.publication,1);
                continue;
            }
            relative=real_vr_dlss_camera::decode(bytes.data(),bytes.size());
        }
        if(!relative.valid) {
            if(state.unreadable.fetch_add(1,std::memory_order_relaxed)<8)report_real_vr_camera(-1,1181,entry.publication,2);
            continue;
        }
        real_vr_dlss_camera::Camera camera;
        int depth{};std::uint64_t world{};
        {std::scoped_lock lock(state.mutex);depth=state.chain.advance(relative,camera);world=state.chain.world();}
        // 1185: the world starts on this frame.
        if(depth==real_vr_dlss_camera::Chain::kRestarted && state.restarts.fetch_add(1,std::memory_order_relaxed)<16)
            report_real_vr_camera(0,1185,entry.publication,0);
        DlssCameraAttachment attachment{};
        attachment.position=camera.position;attachment.right=camera.right;attachment.up=camera.up;attachment.forward=camera.forward;
        attachment.view_to_clip=camera.view_to_clip;attachment.clip_to_view=camera.clip_to_view;
        attachment.near_z=camera.near_z;attachment.far_z=camera.far_z;attachment.fov_vertical=camera.fov_vertical;
        attachment.world=world;
        const bool attached=attach_dlss_motion_vector_camera(entry.stream,entry.publication,attachment);
        const auto n=state.attached.fetch_add(1,std::memory_order_relaxed);
        if(n<4 || n%512==0) {
            // 1182: a camera attached (result 1) or refused (0); b packs the
            // world position x/z as float bits, c the near plane and FOV.
            const auto pair=[](float a,float b){return (std::uint64_t(std::bit_cast<std::uint32_t>(a))<<32)|std::bit_cast<std::uint32_t>(b);};
            report_real_vr_camera(attached?1:0,1182,pair(camera.position[0],camera.position[2]),pair(camera.near_z,camera.fov_vertical));
        }
    }
}
}
bool query_luke_context(ID3D12Device** device,ID3D12CommandQueue** queue) noexcept {
    if(device)*device=nullptr;if(queue)*queue=nullptr;
    if(!device || !queue || !has_luke_context.load(std::memory_order_acquire) || !luke_detected())return false;
    try {
        auto& h=hooks();std::scoped_lock lock(h.mutex);
        if(!h.luke_device || !h.luke_queue)return false;
        if(FAILED(h.luke_device.CopyTo(device)))return false;
        if(FAILED(h.luke_queue.CopyTo(queue))){(*device)->Release();*device=nullptr;return false;}
        return true;
    }catch(...) {return false;}
}
void stop_queue(ID3D12CommandQueue* queue) noexcept {
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    std::erase_if(s.queues,[&](const auto& owner){return owner.Get()==queue;});
    if(s.luke_queue.Get()==queue){
        s.luke_queue.Reset();s.luke_device.Reset();s.luke_lists.clear();
        has_luke_context.store(false,std::memory_order_release);
        pending_luke_lists.store(0,std::memory_order_release);
    }
}
bool apply_camera(DlssMotionVectorPublication& g,ID3D12GraphicsCommandList* list) noexcept {
    const bool is_luke=luke_detected();g.protect_snapshot_lifetime=is_luke;
    if(is_luke&&kcd()) {
        const auto packet=luke_scene_camera::take(list,g.motion_width,g.motion_height);
        if(!packet.valid){++camera_rejected;return false;}
        const auto& c=packet.camera;
        g.camera_view_to_clip=c.projection;g.camera_clip_to_view=c.inverse_projection;
        g.camera_position=c.position;g.camera_up=c.up;g.camera_right=c.right;g.camera_forward=c.forward;
        g.camera_near=c.near_plane;g.camera_far=c.far_plane;g.camera_fov_vertical=c.fov;
        g.depth_inverted=true;g.depth_infinite=c.far_plane==0;
        g.camera_frame_token=packet.token;g.camera_frame_token_valid=true;
        g.camera_data_valid=g.camera_matrices_valid=true;++camera_accepted;return true;
    }
    const auto c=streamline_camera::current(is_luke);if(!c.valid){++camera_rejected;return false;}
    g.camera_view_to_clip=c.projection;g.camera_clip_to_view=c.inverse;
    g.camera_position=c.position;g.camera_up=c.up;g.camera_right=c.right;g.camera_forward=c.forward;
    g.camera_near=c.near_z;g.camera_far=c.far_z;g.camera_fov_vertical=c.fov;
    g.depth_inverted=c.inverted;g.depth_infinite=c.far_z==0;
    g.camera_frame_token=c.frame;g.camera_frame_token_valid=true;
    g.raw_camera_motion_included=c.camera_motion;
    if(finite(c.previous)&&finite(c.to_current)){g.raw_clip_to_previous_clip=c.previous;g.raw_previous_clip_to_clip=c.to_current;}
    g.camera_data_valid=g.camera_matrices_valid=true;++camera_accepted;return true;
}
Statistics statistics() noexcept {
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    return {luke_detected(),streamline_camera::hooked(),camera_accepted.load(),camera_rejected.load(),submissions.load()};
}
LukeContextDiagnostics luke_context_diagnostics() noexcept {
    auto& s=hooks();std::scoped_lock lock(s.mutex);
    return {has_luke_context.load(std::memory_order_acquire),bool(s.execute),bool(s.create_queue),
        static_cast<std::uint32_t>(s.luke_lists.size()),static_cast<std::uint32_t>(s.queues.size()),
        s.execute_entry,reinterpret_cast<std::uintptr_t>(s.luke_device.Get()),
        reinterpret_cast<std::uintptr_t>(s.luke_queue.Get()),submissions.load()};
}
std::uint32_t allocate_handle_key() noexcept {
    auto prior=handles.load();
    while(prior!=UINT32_MAX)if(handles.compare_exchange_weak(prior,prior+1))return prior+1;
    return 0;
}
}

extern "C" __declspec(dllexport) int OFXR_GetLukeRossD3D12ContextV1(
    ID3D12Device** device,ID3D12CommandQueue** queue) noexcept {
    return xrfg::native_ingress::query_luke_context(device,queue)?1:0;
}
