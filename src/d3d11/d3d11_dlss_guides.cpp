#include "xrfg/d3d11_dlss_guides.hpp"
#include <mutex>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <algorithm>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID receipt_guid{0x62935ac7, 0x5aac, 0x47d9, {0xaa,0x59,0x97,0x61,0x46,0x3f,0x96,0x28}};
constexpr GUID sr_origin_guid{0x8fdb3301, 0x6ed0, 0x45ab, {0x80,0x7a,0x3b,0x31,0xe9,0x6d,0x22,0x78}};
constexpr GUID sr_staging_guid{0x0b8a3333, 0x5c67, 0x4f71, {0x89,0xcd,0xd8,0x4e,0x32,0x46,0xa2,0x0c}};
struct SrDestination { std::uintptr_t session{}; std::vector<ComPtr<ID3D11Texture2D>> images; };
struct SrStagingTarget {
    ComPtr<ID3D11Resource> image;
    std::uint64_t source_token{};
    std::uintptr_t swapchain{};
    bool ambiguous{};
};
struct Owner { ComPtr<IUnknown> identity; ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::uint64_t generation{}; bool claimed{}; };
struct OwnerRegistry {
    std::mutex mutex;
    std::unordered_map<std::uintptr_t,Owner> values;
    std::unordered_map<std::uintptr_t,SrDestination> sr_destinations;
    std::unordered_map<std::uint64_t,std::weak_ptr<D3D11SuperResolutionFrame>> sr_frames;
    std::unordered_map<IUnknown*,SrStagingTarget> sr_staging;
    std::uint64_t generation{1}, sr_frame{1}, sr_staging_token{1};
    std::atomic<bool> sr_pending{};
};
// Late SessionState destructors may still unregister after static containers
// start tearing down. Session erasure releases all retained COM owners.
auto* const owner_registry=new OwnerRegistry;
auto& owner_mutex=owner_registry->mutex;
auto& owners=owner_registry->values;
auto& next_generation=owner_registry->generation;
ComPtr<IUnknown> identity(IUnknown* object) {
    ComPtr<IUnknown> result;
    if (object) object->QueryInterface(IID_PPV_ARGS(result.GetAddressOf()));
    return result;
}
bool shape(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& description) {
    ComPtr<ID3D11Texture2D> texture;
    if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())))) return false;
    texture->GetDesc(&description);
    return description.ArraySize == 1 && description.MipLevels == 1 && description.SampleDesc.Count == 1;
}
std::uintptr_t sr_swapchain_locked(IUnknown* key) {
    std::uintptr_t found{};
    if (!key) return 0;
    for (const auto& [swapchain,entry]:owner_registry->sr_destinations)
        for (const auto& image:entry.images) if (identity(image.Get()).Get()==key) {
            if (found && found!=swapchain) return 0;
            found=swapchain;
        }
    return found;
}
DXGI_FORMAT sr_copy_format_family(DXGI_FORMAT format) {
    if (format>=DXGI_FORMAT_R32G32B32A32_TYPELESS && format<=DXGI_FORMAT_R32G32B32A32_SINT) return DXGI_FORMAT_R32G32B32A32_TYPELESS;
    if (format>=DXGI_FORMAT_R16G16B16A16_TYPELESS && format<=DXGI_FORMAT_R16G16B16A16_SINT) return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    if (format>=DXGI_FORMAT_R10G10B10A2_TYPELESS && format<=DXGI_FORMAT_R10G10B10A2_UINT) return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    if (format>=DXGI_FORMAT_R8G8B8A8_TYPELESS && format<=DXGI_FORMAT_R8G8B8A8_SINT) return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_TYPELESS:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_TYPELESS:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default:return format;
    }
}
}
void register_d3d11_guide_owner(std::uintptr_t session, ID3D11Device* source, ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        auto key=identity(source); ComPtr<ID3D12Device> queue_device;
        if (!session || !key || !device || !queue || FAILED(queue->GetDevice(IID_PPV_ARGS(queue_device.GetAddressOf()))) ||
            identity(device).Get()!=identity(queue_device.Get()).Get()) return;
        std::scoped_lock lock(owner_mutex);
        if (!next_generation) return;
        owners[session]={key,device,queue,next_generation++,false};
    } catch (...) {}
}
void unregister_d3d11_guide_owner(std::uintptr_t session) noexcept {
    try {
        std::scoped_lock lock(owner_mutex); owners.erase(session);
        std::erase_if(owner_registry->sr_staging,[session](const auto& item) {
            const auto entry=owner_registry->sr_destinations.find(item.second.swapchain);
            return entry==owner_registry->sr_destinations.end() || entry->second.session==session;
        });
        std::erase_if(owner_registry->sr_destinations,[session](const auto& item) { return item.second.session==session; });
    } catch (...) {}
}

void register_d3d11_sr_destinations(std::uintptr_t session, std::uint64_t swapchain,
                                   ID3D11Texture2D* const* images, std::size_t count) noexcept {
    try {
        if (!session || !swapchain || !images || !count) return;
        SrDestination entry; entry.session=session;
        for (std::size_t i=0;i<count;++i) {
            D3D11_TEXTURE2D_DESC desc{};
            if (!shape(images[i],desc)) return;
            entry.images.emplace_back(images[i]);
        }
        std::scoped_lock lock(owner_mutex);
        std::erase_if(owner_registry->sr_staging,[swapchain](const auto& item) { return item.second.swapchain==swapchain; });
        owner_registry->sr_destinations[swapchain]=std::move(entry);
    } catch (...) {}
}
void unregister_d3d11_sr_destinations(std::uint64_t swapchain) noexcept {
    try {
        std::scoped_lock lock(owner_mutex);
        std::erase_if(owner_registry->sr_staging,[swapchain](const auto& item) { return item.second.swapchain==swapchain; });
        owner_registry->sr_destinations.erase(swapchain);
    } catch (...) {}
}
bool is_d3d11_sr_destination(ID3D11Resource* resource) noexcept {
    try {
        const auto key=identity(resource); if (!key) return false;
        std::scoped_lock lock(owner_mutex);
        for (const auto& [swapchain,entry]:owner_registry->sr_destinations) {
            static_cast<void>(swapchain);
            for (const auto& image:entry.images) if (identity(image.Get()).Get()==key.Get()) return true;
        }
    } catch (...) {} return false;
}
void observe_d3d11_sr_destination_copy(ID3D11Resource* destination,ID3D11Resource* source) noexcept {
    try {
        D3D11_TEXTURE2D_DESC src{},dst{};
        if (!shape(source,src) || !shape(destination,dst) || src.Width!=dst.Width || src.Height!=dst.Height ||
            sr_copy_format_family(src.Format)!=sr_copy_format_family(dst.Format)) return;
        const auto source_key=identity(source),destination_key=identity(destination);
        if (!source_key || !destination_key || source_key.Get()==destination_key.Get()) return;
        ComPtr<ID3D11Device> source_device,destination_device;
        source->GetDevice(source_device.GetAddressOf());destination->GetDevice(destination_device.GetAddressOf());
        if (identity(source_device.Get()).Get()!=identity(destination_device.Get()).Get()) return;
        std::scoped_lock lock(owner_mutex);
        const auto swapchain=sr_swapchain_locked(destination_key.Get());if (!swapchain) return;
        auto old=owner_registry->sr_staging.find(source_key.Get());
        std::uint64_t source_token{};UINT token_size=sizeof(source_token);
        if (FAILED(source->GetPrivateData(sr_staging_guid,&token_size,&source_token)) || token_size!=sizeof(source_token)) source_token=0;
        // Never retain application staging textures just for identity: that
        // would keep their VRAM alive after a resize. The resource-local nonce
        // rejects a newly allocated texture reusing an old COM address.
        if (old!=owner_registry->sr_staging.end() && old->second.source_token!=source_token) {
            owner_registry->sr_staging.erase(old);old=owner_registry->sr_staging.end();
        }
        if (old!=owner_registry->sr_staging.end()) {
            if (old->second.swapchain!=swapchain) old->second.ambiguous=true;
            if (!old->second.ambiguous) old->second.image=destination;
            return;
        }
        // Bound CPU metadata and references to already enumerated XR images.
        // XR/session teardown erases all links; unrelated copies never enter.
        if (owner_registry->sr_staging.size()>=64 || !owner_registry->sr_staging_token) return;
        source_token=owner_registry->sr_staging_token++;
        if (FAILED(source->SetPrivateData(sr_staging_guid,sizeof(source_token),&source_token))) return;
        owner_registry->sr_staging.emplace(source_key.Get(),SrStagingTarget{destination,source_token,swapchain,false});
    } catch (...) {}
}
HRESULT resolve_d3d11_sr_destination(ID3D11Resource* color_target,ID3D11Resource** actual_image) noexcept {
    if (!actual_image) return E_POINTER;*actual_image=nullptr;
    try {
        const auto key=identity(color_target);if (!key) return E_INVALIDARG;
        std::scoped_lock lock(owner_mutex);
        if (sr_swapchain_locked(key.Get())) {color_target->AddRef();*actual_image=color_target;return S_OK;}
        const auto found=owner_registry->sr_staging.find(key.Get());
        std::uint64_t source_token{};UINT token_size=sizeof(source_token);
        if (FAILED(color_target->GetPrivateData(sr_staging_guid,&token_size,&source_token)) || token_size!=sizeof(source_token)) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        if (found==owner_registry->sr_staging.end() || found->second.ambiguous ||
            found->second.source_token!=source_token ||
            sr_swapchain_locked(identity(found->second.image.Get()).Get())!=found->second.swapchain) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        return found->second.image.CopyTo(actual_image);
    } catch (...) {return E_FAIL;}
}
bool has_d3d11_sr_destinations(ID3D11Device* device) noexcept {
    try {
        const auto key=identity(device);if (!key) return false;
        std::scoped_lock lock(owner_mutex);
        for (const auto& [swapchain,entry]:owner_registry->sr_destinations) {
            static_cast<void>(swapchain);
            for (const auto& image:entry.images) {
                ComPtr<ID3D11Device> owner;image->GetDevice(owner.GetAddressOf());
                if (identity(owner.Get()).Get()==key.Get()) return true;
            }
        }
    } catch (...) {} return false;
}
std::uint64_t register_d3d11_sr_frame(const std::shared_ptr<D3D11SuperResolutionFrame>& frame) noexcept {
    try {
        if (!frame) return 0;
        std::scoped_lock lock(owner_mutex);
        if (!owner_registry->sr_frame) return 0;
        const auto id=owner_registry->sr_frame++;
        owner_registry->sr_frames.emplace(id,frame);
        owner_registry->sr_pending.store(true,std::memory_order_release); return id;
    } catch (...) {} return 0;
}
void retire_d3d11_sr_frame(std::uint64_t frame) noexcept {
    try {
        std::scoped_lock lock(owner_mutex);owner_registry->sr_frames.erase(frame);
        owner_registry->sr_pending.store(!owner_registry->sr_frames.empty(),std::memory_order_release);
    } catch (...) {}
}
bool d3d11_sr_pending() noexcept {return owner_registry->sr_pending.load(std::memory_order_acquire);}
std::shared_ptr<D3D11SuperResolutionFrame> resolve_d3d11_sr_frame(std::uint64_t frame) noexcept {
    try {
        std::scoped_lock lock(owner_mutex);const auto found=owner_registry->sr_frames.find(frame);
        return found==owner_registry->sr_frames.end()?nullptr:found->second.lock();
    } catch (...) {return {};}
}
void clear_d3d11_sr_origin(ID3D11Resource* resource) noexcept {
    if (resource) resource->SetPrivateData(sr_origin_guid,0,nullptr);
}
void set_d3d11_sr_origin(ID3D11Resource* resource,const D3D11SuperResolutionOrigin& origin) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    if (!origin.frame || !shape(resource,desc) || desc.Width!=origin.width || desc.Height!=origin.height) {
        clear_d3d11_sr_origin(resource);return;
    }
    resource->SetPrivateData(sr_origin_guid,sizeof(origin),&origin);
}
D3D11SuperResolutionOrigin get_d3d11_sr_origin(ID3D11Resource* resource) noexcept {
    D3D11SuperResolutionOrigin origin{};UINT size=sizeof(origin);D3D11_TEXTURE2D_DESC desc{};
    if (!resource || FAILED(resource->GetPrivateData(sr_origin_guid,&size,&origin)) || size!=sizeof(origin) ||
        !shape(resource,desc) || desc.Width!=origin.width || desc.Height!=origin.height ||
        !resolve_d3d11_sr_frame(origin.frame)) return {};
    return origin;
}
HRESULT acquire_d3d11_guide_owner(ID3D11Device* source, ID3D12Device** device, ID3D12CommandQueue** queue, std::uint64_t* generation) noexcept {
    if (!device || !queue || !generation) return E_INVALIDARG;
    *device=nullptr; *queue=nullptr; *generation=0;
    try {
        auto key=identity(source); std::scoped_lock lock(owner_mutex);
        const Owner* found{};
        for (const auto& [session,owner]:owners) {
            static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get()) { if (found) return E_UNEXPECTED; found=&owner; }
        }
        if (!found) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        found->device.CopyTo(device); found->queue.CopyTo(queue); *generation=found->generation; return S_OK;
    } catch (...) { return E_FAIL; }
}
bool claim_d3d11_guides(ID3D11Device* source, std::uint64_t generation) noexcept {
    try { auto key=identity(source); std::scoped_lock lock(owner_mutex);
        for (auto& [session,owner]:owners) { static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get() && owner.generation==generation) { owner.claimed=true; return true; }
        }
    } catch (...) {} return false;
}
bool d3d11_guides_claimed(ID3D11Device* source) noexcept {
    try {
        auto key=identity(source); std::scoped_lock lock(owner_mutex); const Owner* found{};
        for (const auto& [session,owner]:owners) {
            static_cast<void>(session);
            if (key && owner.identity.Get()==key.Get()) {if (found) return false;found=&owner;}
        }
        return found && found->claimed;
    } catch (...) {return false;}
}
void clear_d3d11_guide_receipt(ID3D11Resource* resource) noexcept {
    if (resource) resource->SetPrivateData(receipt_guid,0,nullptr);
}
void set_d3d11_guide_receipt(ID3D11Resource* resource, const D3D11GuideReceipt& receipt) noexcept {
    D3D11_TEXTURE2D_DESC description{};
    if (!receipt.owner || !receipt.stream || !receipt.publication || !shape(resource,description) ||
        description.Width!=receipt.width || description.Height!=receipt.height) { clear_d3d11_guide_receipt(resource); return; }
    resource->SetPrivateData(receipt_guid,sizeof(receipt),&receipt);
}
D3D11GuideReceipt get_d3d11_guide_receipt(ID3D11Resource* resource) noexcept {
    D3D11GuideReceipt receipt{}; UINT size=sizeof(receipt);
    if (!resource || FAILED(resource->GetPrivateData(receipt_guid,&size,&receipt)) || size!=sizeof(receipt)) return {};
    D3D11_TEXTURE2D_DESC description{};
    if (!shape(resource,description) || description.Width!=receipt.width || description.Height!=receipt.height) return {};
    return receipt;
}
bool propagate_d3d11_guide_receipt(ID3D11Resource* destination, ID3D11Resource* source) noexcept {
    const auto receipt=get_d3d11_guide_receipt(source);
    set_d3d11_guide_receipt(destination,receipt);
    return get_d3d11_guide_receipt(destination).publication!=0;
}
std::shared_ptr<const DlssMotionVectorSet> resolve_d3d11_dlss_guides(ID3D11Resource* resource, ID3D12CommandQueue* consumer) noexcept {
    try {
        const auto receipt=get_d3d11_guide_receipt(resource);
        ComPtr<ID3D11Device> source; if (!resource || !receipt.publication) return {};
        resource->GetDevice(source.GetAddressOf());
        ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; std::uint64_t generation{};
        if (FAILED(acquire_d3d11_guide_owner(source.Get(),device.GetAddressOf(),queue.GetAddressOf(),&generation)) || generation!=receipt.owner ||
            identity(queue.Get()).Get()!=identity(consumer).Get()) return {};
        auto frame=resolve_dlss_guide_publication(receipt.stream,receipt.publication,consumer);
        if (!frame) return {};
        auto result=std::make_shared<DlssMotionVectorSet>(); result->eye_count=1; result->eyes[0]=std::move(frame); return result;
    } catch (...) { return {}; }
}
}
