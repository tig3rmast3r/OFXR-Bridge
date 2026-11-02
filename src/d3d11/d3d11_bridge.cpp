#include "xrfg/d3d11_bridge.hpp"
#include "xrfg/two_frame_capture.hpp"

#include <array>
#include <limits>
#include <mutex>
#include <vector>

#include <wrl/client.h>

namespace xrfg {

using Microsoft::WRL::ComPtr;

namespace {

struct UniqueHandle {
    HANDLE value{};
    ~UniqueHandle() {
        if (value != nullptr) {
            CloseHandle(value);
        }
    }
};

[[nodiscard]] bool is_depth_format(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return true;
        default:
            return false;
    }
}

// The typeless family of a depth format: what a shared texture takes when
// the depth format itself cannot be shared, and what the application's own
// depth texture is made of so it can create its depth view on it.
[[nodiscard]] DXGI_FORMAT depth_typeless_format(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_D16_UNORM:
            return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        default:
            return format;
    }
}

constexpr std::size_t kCopyListCount = 4;

} // namespace

struct D3D11BridgeSwapchain::Impl {
    struct CopyList {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };

    std::mutex mutex;
    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext4> d3d11_context4;
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    ComPtr<ID3D12Fence> d3d12_fence;
    ComPtr<ID3D11Fence> d3d11_fence;
    HANDLE fence_event{};
    std::uint64_t next_fence_value{1};
    std::uint64_t last_signalled{};
    D3D11BridgePath path{D3D11BridgePath::direct};
    DXGI_FORMAT shared_format{DXGI_FORMAT_UNKNOWN};
    DXGI_FORMAT resolve_format{DXGI_FORMAT_UNKNOWN};
    bool runtime_depth{};
    std::vector<ComPtr<ID3D12Resource>> runtime_images;
    std::vector<ComPtr<ID3D12Resource>> shared_images;
    std::vector<ComPtr<ID3D11Texture2D>> d3d11_shared;
    // The application's own textures on the copy and resolve paths; empty
    // on the direct path.
    std::vector<ComPtr<ID3D11Texture2D>> d3d11_own;
    std::vector<ID3D12Resource*> shared_views;
    std::vector<ID3D11Texture2D*> application_views;
    // The fence value the layer's queue signalled after its last read of
    // each shared texture; the application's context waits on it before
    // writing that texture again.
    std::vector<std::uint64_t> last_read;
    std::array<CopyList, kCopyListCount> copy_lists{};
    std::size_t next_copy_list{};
    bool enabled{};

    ~Impl() {
        if (fence_event != nullptr) {
            CloseHandle(fence_event);
        }
    }

    [[nodiscard]] std::uint64_t allocate() noexcept {
        if (next_fence_value == std::numeric_limits<std::uint64_t>::max()) {
            enabled = false;
        }
        return next_fence_value++;
    }

    [[nodiscard]] HRESULT create_shared_texture(
        const D3D12_RESOURCE_DESC& description,
        ComPtr<ID3D12Resource>* shared,
        ComPtr<ID3D11Texture2D>* opened,
        std::uint32_t* failure_stage) noexcept {
        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;
        HRESULT result = d3d12_device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_SHARED,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(shared->GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 10;
            return result;
        }
        UniqueHandle handle;
        result = d3d12_device->CreateSharedHandle(
            shared->Get(), nullptr, GENERIC_ALL, nullptr, &handle.value);
        if (FAILED(result)) {
            *failure_stage = 11;
            return result;
        }
        ComPtr<ID3D11Device1> device1;
        result = d3d11_device.As(&device1);
        if (FAILED(result)) {
            *failure_stage = 12;
            return result;
        }
        result = device1->OpenSharedResource1(
            handle.value, IID_PPV_ARGS(opened->GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 13;
        }
        return result;
    }

    // The shared texture's description for one runtime image on a path.
    // Both devices write and read it with no state transitions between
    // them; simultaneous access is what makes a D3D12 resource legal to use
    // that way. On the depth-copy path the shared texture is the typeless
    // family of the depth format and carries no render or depth flag: it is
    // only ever a copy source and destination.
    [[nodiscard]] static D3D12_RESOURCE_DESC shared_description(
        const D3D12_RESOURCE_DESC& runtime_description,
        D3D11BridgePath path,
        const D3D11BridgeSwapchainDescription& requested) noexcept {
        D3D12_RESOURCE_DESC description = runtime_description;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.SampleDesc.Count = 1;
        description.SampleDesc.Quality = 0;
        description.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (path == D3D11BridgePath::mip_copy) {
            description.MipLevels = 1;
        }
        if (path == D3D11BridgePath::depth_copy) {
            // D3D11 opens a shared texture only when it can bind it to
            // something; a typeless texture with no flags at all is refused
            // (E_INVALIDARG from OpenSharedResource1, measured). The R32 and
            // R16 families have a render-target member, so they carry the
            // flag; the packed depth-stencil families have none and are left
            // to the driver.
            description.Format = depth_typeless_format(runtime_description.Format);
            if (description.Format == DXGI_FORMAT_R32_TYPELESS ||
                description.Format == DXGI_FORMAT_R16_TYPELESS) {
                description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            return description;
        }
        if (requested.depth_stencil) {
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        } else {
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        }
        if (requested.unordered_access) {
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        return description;
    }

    // The application's own D3D11 texture on the copy and resolve paths:
    // the shape the application asked for, in the typeless family so it can
    // create any view of its own on it, as a runtime's D3D11 session would
    // have given it.
    [[nodiscard]] HRESULT create_own_texture(
        const D3D12_RESOURCE_DESC& runtime_description,
        const D3D11BridgeSwapchainDescription& requested,
        ComPtr<ID3D11Texture2D>* output,
        std::uint32_t* failure_stage) noexcept {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(runtime_description.Width);
        description.Height = runtime_description.Height;
        description.MipLevels = runtime_description.MipLevels;
        description.ArraySize = runtime_description.DepthOrArraySize;
        description.Format = requested.depth_stencil
            ? depth_typeless_format(runtime_description.Format)
            : runtime_description.Format;
        description.SampleDesc.Count = requested.requested_sample_count;
        description.SampleDesc.Quality = 0;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = requested.depth_stencil
            ? D3D11_BIND_DEPTH_STENCIL
            : (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        // A game that asks for mips may fill them with GenerateMips, which
        // needs this flag; a runtime's own mipmapped D3D11 images carry it.
        if (description.MipLevels > 1 && !requested.depth_stencil) {
            description.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
        }
        if (requested.unordered_access && requested.requested_sample_count == 1) {
            description.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        }
        if (requested.depth_stencil && requested.requested_sample_count == 1) {
            description.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
        }
        const HRESULT result = d3d11_device->CreateTexture2D(
            &description, nullptr, output->GetAddressOf());
        if (FAILED(result)) {
            *failure_stage = 14;
        }
        return result;
    }

    [[nodiscard]] HRESULT create_images(
        std::span<ID3D12Resource* const> input_runtime_images,
        const D3D11BridgeSwapchainDescription& requested,
        std::uint32_t* failure_stage) noexcept {
        shared_images.clear();
        d3d11_shared.clear();
        d3d11_own.clear();
        for (ID3D12Resource* image : input_runtime_images) {
            const D3D12_RESOURCE_DESC runtime_description = image->GetDesc();
            ComPtr<ID3D12Resource> shared;
            ComPtr<ID3D11Texture2D> opened;
            HRESULT result = create_shared_texture(
                shared_description(runtime_description, path, requested),
                &shared, &opened, failure_stage);
            if (FAILED(result)) {
                return result;
            }
            if (path != D3D11BridgePath::direct) {
                ComPtr<ID3D11Texture2D> own;
                result = create_own_texture(
                    runtime_description, requested, &own, failure_stage);
                if (FAILED(result)) {
                    return result;
                }
                d3d11_own.push_back(std::move(own));
            }
            shared_format = shared->GetDesc().Format;
            shared_images.push_back(std::move(shared));
            d3d11_shared.push_back(std::move(opened));
        }
        return S_OK;
    }

    // depth_private: the application's textures only; the runtime's images
    // are never written and never submitted.
    [[nodiscard]] HRESULT create_private_depth_images(
        std::span<ID3D12Resource* const> input_runtime_images,
        const D3D11BridgeSwapchainDescription& requested,
        std::uint32_t* failure_stage) noexcept {
        shared_images.clear();
        d3d11_shared.clear();
        d3d11_own.clear();
        for (ID3D12Resource* image : input_runtime_images) {
            ComPtr<ID3D11Texture2D> own;
            const HRESULT result = create_own_texture(
                image->GetDesc(), requested, &own, failure_stage);
            if (FAILED(result)) {
                return result;
            }
            d3d11_own.push_back(std::move(own));
        }
        shared_format = DXGI_FORMAT_UNKNOWN;
        return S_OK;
    }

    [[nodiscard]] HRESULT initialize(
        ID3D11Device* input_d3d11_device,
        ID3D11DeviceContext* input_d3d11_context,
        ID3D12Device* input_d3d12_device,
        ID3D12CommandQueue* input_d3d12_queue,
        std::span<ID3D12Resource* const> input_runtime_images,
        const D3D11BridgeSwapchainDescription& requested,
        std::uint32_t* failure_stage) noexcept {
        *failure_stage = 0;
        if (input_d3d11_device == nullptr || input_d3d11_context == nullptr ||
            input_d3d12_device == nullptr || input_d3d12_queue == nullptr ||
            input_runtime_images.empty() ||
            requested.requested_sample_count == 0) {
            *failure_stage = 1;
            return E_INVALIDARG;
        }
        d3d11_device = input_d3d11_device;
        d3d12_device = input_d3d12_device;
        d3d12_queue = input_d3d12_queue;
        HRESULT result = input_d3d11_context->QueryInterface(
            IID_PPV_ARGS(d3d11_context4.GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 2;
            return result;
        }
        ComPtr<ID3D11Device5> device5;
        result = d3d11_device.As(&device5);
        if (FAILED(result)) {
            *failure_stage = 3;
            return result;
        }
        for (ID3D12Resource* image : input_runtime_images) {
            if (image == nullptr) {
                *failure_stage = 4;
                return E_INVALIDARG;
            }
            runtime_images.emplace_back(image);
        }
        runtime_depth = is_depth_format(runtime_images.front()->GetDesc().Format);
        resolve_format = requested.requested_format;

        // The path. Multisampling can never be shared, so it resolves.
        // Depth is tried shared first; the driver decides, and the copy
        // path takes over if it refuses.
        const bool mipmapped = runtime_images.front()->GetDesc().MipLevels > 1;
        if (requested.requested_sample_count > 1) {
            path = D3D11BridgePath::resolve;
            result = create_images(input_runtime_images, requested, failure_stage);
        } else if (mipmapped && !requested.depth_stencil) {
            path = D3D11BridgePath::mip_copy;
            result = create_images(input_runtime_images, requested, failure_stage);
        } else if (requested.depth_stencil) {
            path = D3D11BridgePath::direct;
            result = create_images(input_runtime_images, requested, failure_stage);
            if (FAILED(result)) {
                path = D3D11BridgePath::depth_copy;
                result = create_images(input_runtime_images, requested, failure_stage);
            }
            if (FAILED(result)) {
                path = D3D11BridgePath::depth_private;
                result = create_private_depth_images(
                    input_runtime_images, requested, failure_stage);
            }
        } else {
            path = D3D11BridgePath::direct;
            result = create_images(input_runtime_images, requested, failure_stage);
        }
        if (FAILED(result)) {
            return result;
        }
        for (const auto& image : shared_images) shared_views.push_back(image.Get());
        if (path == D3D11BridgePath::direct) {
            for (const auto& image : d3d11_shared) application_views.push_back(image.Get());
        } else {
            for (const auto& image : d3d11_own) application_views.push_back(image.Get());
        }
        last_read.assign(runtime_images.size(), 0);

        result = d3d12_device->CreateFence(
            0, D3D12_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(d3d12_fence.GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 20;
            return result;
        }
        UniqueHandle fence_handle;
        result = d3d12_device->CreateSharedHandle(
            d3d12_fence.Get(), nullptr, GENERIC_ALL, nullptr,
            &fence_handle.value);
        if (FAILED(result)) {
            *failure_stage = 21;
            return result;
        }
        result = device5->OpenSharedFence(
            fence_handle.value, IID_PPV_ARGS(d3d11_fence.GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 22;
            return result;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            *failure_stage = 23;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        for (CopyList& copy : copy_lists) {
            result = d3d12_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(copy.allocator.GetAddressOf()));
            if (FAILED(result)) {
                *failure_stage = 30;
                return result;
            }
            result = d3d12_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(),
                nullptr, IID_PPV_ARGS(copy.list.GetAddressOf()));
            if (FAILED(result)) {
                *failure_stage = 31;
                return result;
            }
            result = copy.list->Close();
            if (FAILED(result)) {
                *failure_stage = 32;
                return result;
            }
        }
        enabled = true;
        return S_OK;
    }

    // A copy list whose previous use the queue has finished with. Bounded
    // by the ring: if every list is still in flight this waits on the
    // oldest, which cannot happen while the application's own frame loop
    // paces the releases four per swapchain apart.
    [[nodiscard]] HRESULT take_copy_list(CopyList** output) noexcept {
        CopyList& copy = copy_lists[next_copy_list];
        next_copy_list = (next_copy_list + 1) % kCopyListCount;
        if (copy.fence_value != 0 &&
            d3d12_fence->GetCompletedValue() < copy.fence_value) {
            const HRESULT result = d3d12_fence->SetEventOnCompletion(
                copy.fence_value, fence_event);
            if (FAILED(result)) {
                return result;
            }
            if (WaitForSingleObject(fence_event, 2000) != WAIT_OBJECT_0) {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
        }
        HRESULT result = copy.allocator->Reset();
        if (FAILED(result)) {
            return result;
        }
        result = copy.list->Reset(copy.allocator.Get(), nullptr);
        if (FAILED(result)) {
            return result;
        }
        *output = &copy;
        return S_OK;
    }

    // The application's own texture into the shared one, on its context,
    // before the signal that hands the shared texture to the layer's queue.
    void move_own_to_shared(std::uint32_t index) noexcept {
        if (path == D3D11BridgePath::direct) {
            return;
        }
        ID3D11Texture2D* const own = d3d11_own[index].Get();
        ID3D11Texture2D* const shared = d3d11_shared[index].Get();
        if (path == D3D11BridgePath::depth_copy) {
            d3d11_context4->CopyResource(shared, own);
            return;
        }
        if (path == D3D11BridgePath::mip_copy) {
            D3D11_TEXTURE2D_DESC own_description{};
            own->GetDesc(&own_description);
            for (UINT slice = 0; slice < own_description.ArraySize; ++slice) {
                d3d11_context4->CopySubresourceRegion(
                    shared, D3D11CalcSubresource(0, slice, 1), 0, 0, 0,
                    own, D3D11CalcSubresource(0, slice, own_description.MipLevels),
                    nullptr);
            }
            return;
        }
        D3D11_TEXTURE2D_DESC description{};
        own->GetDesc(&description);
        for (UINT slice = 0; slice < description.ArraySize; ++slice) {
            const UINT subresource = D3D11CalcSubresource(0, slice, description.MipLevels);
            d3d11_context4->ResolveSubresource(
                shared, subresource, own, subresource, resolve_format);
        }
    }

    [[nodiscard]] HRESULT release(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == D3D11BridgePath::depth_private) {
            return S_OK;
        }
        move_own_to_shared(index);
        // The application's rendering into the shared texture, complete on
        // its context, before the layer's queue reads it.
        const std::uint64_t rendered = allocate();
        HRESULT result = d3d11_context4->Signal(d3d11_fence.Get(), rendered);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        d3d11_context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        result = d3d12_queue->Wait(d3d12_fence.Get(), rendered);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        CopyList* copy = nullptr;
        result = take_copy_list(&copy);
        if (FAILED(result)) {
            return result;
        }
        ID3D12Resource* const runtime_image = runtime_images[index].Get();
        // XR_KHR_D3D12_enable: the runtime hands images out in and expects
        // them back in RENDER_TARGET (DEPTH_WRITE for depth). The shared
        // texture is a simultaneous-access resource and needs no barrier.
        const D3D12_RESOURCE_STATES resting = runtime_depth
            ? D3D12_RESOURCE_STATE_DEPTH_WRITE
            : D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = runtime_image;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = resting;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copy->list->ResourceBarrier(1, &barrier);
        const D3D12_RESOURCE_DESC runtime_description = runtime_image->GetDesc();
        if (path == D3D11BridgePath::mip_copy) {
            // The single-mip shared texture into mip 0 of each slice.
            for (UINT slice = 0; slice < runtime_description.DepthOrArraySize; ++slice) {
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = runtime_image;
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                // Mip 0 of the slice: mip + slice * mip count, plane 0.
                destination.SubresourceIndex =
                    slice * static_cast<UINT>(runtime_description.MipLevels);
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = shared_images[index].Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                source.SubresourceIndex = slice;
                copy->list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            }
        } else {
            copy->list->CopyResource(runtime_image, shared_images[index].Get());
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = resting;
        copy->list->ResourceBarrier(1, &barrier);
        result = copy->list->Close();
        if (FAILED(result)) {
            return result;
        }
        ID3D12CommandList* const lists[] = {copy->list.Get()};
        d3d12_queue->ExecuteCommandLists(1, lists);
        const std::uint64_t copied = allocate();
        result = d3d12_queue->Signal(d3d12_fence.Get(), copied);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        copy->fence_value = copied;
        last_signalled = copied;
        last_read[index] = copied;
        return S_OK;
    }

    [[nodiscard]] HRESULT mark_read(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == D3D11BridgePath::depth_private) {
            return S_OK;
        }
        const std::uint64_t value = allocate();
        const HRESULT result = d3d12_queue->Signal(d3d12_fence.Get(), value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        last_signalled = value;
        last_read[index] = value;
        return S_OK;
    }

    [[nodiscard]] HRESULT before_write(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == D3D11BridgePath::depth_private || last_read[index] == 0) {
            return S_OK;
        }
        const HRESULT result =
            d3d11_context4->Wait(d3d11_fence.Get(), last_read[index]);
        if (FAILED(result)) {
            enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT wait_for_idle() noexcept {
        if (!d3d12_fence || last_signalled == 0 ||
            d3d12_fence->GetCompletedValue() >= last_signalled) {
            return S_OK;
        }
        const HRESULT result =
            d3d12_fence->SetEventOnCompletion(last_signalled, fence_event);
        if (FAILED(result)) {
            return result;
        }
        return WaitForSingleObject(fence_event, 2000) == WAIT_OBJECT_0
            ? S_OK
            : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
};

D3D11BridgeSwapchain::D3D11BridgeSwapchain() : impl_(std::make_unique<Impl>()) {}
D3D11BridgeSwapchain::~D3D11BridgeSwapchain() = default;

HRESULT D3D11BridgeSwapchain::diagnostic_capture_application(
    std::uint32_t index,const TwoFrameImageInfo& info) noexcept {
    if (!two_frame_capture_images_active()) return S_FALSE;
    try {
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->enabled || index>=impl_->shared_images.size() ||
            impl_->path==D3D11BridgePath::depth_private) return E_INVALIDARG;
        return two_frame_capture_record_queue(info,impl_->d3d12_device.Get(),
            impl_->d3d12_queue.Get(),impl_->shared_images[index].Get(),
            D3D12_RESOURCE_STATE_COMMON);
    } catch (...) { return E_FAIL; }
}

HRESULT D3D11BridgeSwapchain::initialize(
    ID3D11Device* d3d11_device,
    ID3D11DeviceContext* d3d11_context,
    ID3D12Device* d3d12_device,
    ID3D12CommandQueue* d3d12_queue,
    std::span<ID3D12Resource* const> runtime_images,
    const D3D11BridgeSwapchainDescription& description,
    std::uint32_t* failure_stage) noexcept {
    std::uint32_t stage = 0;
    HRESULT result = E_FAIL;
    try {
        std::scoped_lock lock(impl_->mutex);
        result = impl_->initialize(
            d3d11_device, d3d11_context, d3d12_device, d3d12_queue,
            runtime_images, description, &stage);
    } catch (...) {
        stage = 99;
        result = E_FAIL;
    }
    if (failure_stage != nullptr) {
        *failure_stage = stage;
    }
    return result;
}

D3D11BridgePath D3D11BridgeSwapchain::path() const noexcept {
    return impl_->path;
}

DXGI_FORMAT D3D11BridgeSwapchain::shared_format() const noexcept {
    return impl_->shared_format;
}

std::span<ID3D11Texture2D* const> D3D11BridgeSwapchain::d3d11_images() const noexcept {
    return impl_->application_views;
}

std::span<ID3D12Resource* const> D3D11BridgeSwapchain::shared_images() const noexcept {
    return impl_->shared_views;
}

HRESULT D3D11BridgeSwapchain::before_write(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        return impl_->before_write(index);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11BridgeSwapchain::release(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        return impl_->release(index);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11BridgeSwapchain::mark_read(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        return impl_->mark_read(index);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11BridgeSwapchain::wait_for_idle() noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        return impl_->wait_for_idle();
    } catch (...) {
        return E_FAIL;
    }
}

} // namespace xrfg
