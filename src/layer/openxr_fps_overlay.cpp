#include "xrfg/openxr_fps_overlay.hpp"
#include <openxr/openxr_platform.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

namespace xrfg {
using Microsoft::WRL::ComPtr;
namespace {
std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<class T> bool load(PFN_xrGetInstanceProcAddr get, XrInstance instance,
                          const char* name, T& output) {
    PFN_xrVoidFunction function{};
    if (!get || XR_FAILED(get(instance, name, &function)) || !function) return false;
    output = reinterpret_cast<T>(function);
    return true;
}
}

struct OpenXrFpsOverlay::Impl {
    XrInstance instance{};
    XrSession session{};
    XrSystemId system{};
    PFN_xrGetInstanceProcAddr get{};
    PFN_xrEndFrame downstream_end{};
    PFN_xrCreateReferenceSpace create_space{};
    PFN_xrDestroySpace destroy_space{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainFormats enumerate_formats{};
    PFN_xrEnumerateSwapchainImages enumerate_images{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrGetSystemProperties system_properties{};
    std::filesystem::path ini;
    std::mutex mutex;
    FpsCounter counter;
    FpsOverlayPosition position{FpsOverlayPosition::upper_right};
    std::int64_t next_refresh{};
    bool attempted{}, initialized{}, image_valid{}, acquired{}, waited{}, disabled{};
    XrSpace space{};
    XrSwapchain swapchain{};
    std::uint32_t index{}, width{}, height{}, max_layers{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    OverlayPlacement placement{};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Multithread> multithread11;
    ComPtr<ID3D11Fence> fence11;
    std::uint64_t next_fence{1}, last_fence{};
    HANDLE event{};
    struct Image {
        ComPtr<ID3D12Resource> texture12, upload;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D11Texture2D> texture11;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        std::uint64_t fence_value{};
    };
    std::vector<Image> images;

    ~Impl() {
        // Only teardown waits for this overlay's last upload. No global queue
        // drain, no runtime frame calls, no application-owned handle retention.
        if (last_fence && event) {
            HRESULT result = S_OK;
            if (fence12 && fence12->GetCompletedValue() < last_fence)
                result = fence12->SetEventOnCompletion(last_fence, event);
            else if (fence11 && fence11->GetCompletedValue() < last_fence)
                result = fence11->SetEventOnCompletion(last_fence, event);
            else result = S_FALSE;
            if (result == S_OK) WaitForSingleObject(event, INFINITE);
        }
        if (swapchain && destroy_swapchain) destroy_swapchain(swapchain);
        if (space && destroy_space) destroy_space(space);
        if (event) CloseHandle(event);
    }

    bool initialize(std::uint32_t eye_width) {
        if (attempted) return initialized;
        attempted = true;
        if ((!device11 && (!device12 || !queue12)) || !system) return false;
        if (!load(get, instance, "xrCreateReferenceSpace", create_space) ||
            !load(get, instance, "xrDestroySpace", destroy_space) ||
            !load(get, instance, "xrCreateSwapchain", create_swapchain) ||
            !load(get, instance, "xrDestroySwapchain", destroy_swapchain) ||
            !load(get, instance, "xrEnumerateSwapchainFormats", enumerate_formats) ||
            !load(get, instance, "xrEnumerateSwapchainImages", enumerate_images) ||
            !load(get, instance, "xrAcquireSwapchainImage", acquire) ||
            !load(get, instance, "xrWaitSwapchainImage", wait) ||
            !load(get, instance, "xrReleaseSwapchainImage", release) ||
            !load(get, instance, "xrGetSystemProperties", system_properties)) return false;
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_FAILED(system_properties(instance, system, &properties))) return false;
        max_layers = std::min(properties.graphicsProperties.maxLayerCount, 128u);
        if (max_layers < 2) return false;
        std::uint32_t count{};
        if (XR_FAILED(enumerate_formats(session, 0, &count, nullptr)) || !count || count > 1024) return false;
        std::vector<std::int64_t> formats(count);
        if (XR_FAILED(enumerate_formats(session, count, &count, formats.data()))) return false;
        for (auto candidate : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                               DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
            if (std::find(formats.begin(), formats.end(), candidate) != formats.end()) { format = candidate; break; }
        }
        if (format == DXGI_FORMAT_UNKNOWN) return false;
        width = overlay_texture_width(eye_width);
        height = width / 2;
        if (width > properties.graphicsProperties.maxSwapchainImageWidth ||
            height > properties.graphicsProperties.maxSwapchainImageHeight) return false;
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_info.poseInReferenceSpace.orientation.w = 1;
        if (XR_FAILED(create_space(session, &space_info, &space))) return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = width;
        info.height = height;
        if (XR_FAILED(create_swapchain(session, &info, &swapchain))) return false;
        if (XR_FAILED(enumerate_images(swapchain, 0, &count, nullptr)) || !count || count > 64) return false;
        images.resize(count);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        if (device11) {
            std::vector<XrSwapchainImageD3D11KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(enumerate_images(swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < images.size(); ++i) {
                images[i].texture11 = buffers[i].texture;
                if (!images[i].texture11) return false;
            }
            ComPtr<ID3D11DeviceContext> immediate;
            device11->GetImmediateContext(&immediate);
            if (!immediate || FAILED(immediate.As(&context11))) return false;
            immediate.As(&multithread11);
            ComPtr<ID3D11Device5> device5;
            if (FAILED(device11.As(&device5)) || FAILED(device5->CreateFence(
                0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence11)))) return false;
        } else {
            std::vector<XrSwapchainImageD3D12KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            if (XR_FAILED(enumerate_images(swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence12)))) return false;
            for (std::size_t i = 0; i < images.size(); ++i) {
                auto& image = images[i];
                image.texture12 = buffers[i].texture;
                if (!image.texture12) return false;
                const auto desc = image.texture12->GetDesc();
                UINT64 bytes{};
                device12->GetCopyableFootprints(&desc, 0, 1, 0, &image.footprint, nullptr, nullptr, &bytes);
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC upload_desc{};
                upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                upload_desc.Width = bytes;
                upload_desc.Height = upload_desc.DepthOrArraySize = upload_desc.MipLevels = 1;
                upload_desc.SampleDesc.Count = 1;
                upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (FAILED(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&image.upload))) ||
                    FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&image.allocator))) ||
                    FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, image.allocator.Get(),
                        nullptr, IID_PPV_ARGS(&image.list))) || FAILED(image.list->Close())) return false;
            }
        }
        quad.space = space;
        quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = swapchain;
        quad.subImage.imageRect.extent = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
        quad.pose.orientation.w = 1;
        initialized = true;
        return true;
    }

    bool upload(const FpsSnapshot& snapshot) {
        if (!acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(acquire(swapchain, &info, &index))) { disabled = true; return false; }
            acquired = true;
            waited = false;
        }
        if (index >= images.size()) { disabled = true; return false; }
        if (!waited) {
            XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            info.timeout = 0;
            const auto result = wait(swapchain, &info);
            // XR_TIMEOUT_EXPIRED is positive, but does NOT grant ownership.
            if (result == XR_TIMEOUT_EXPIRED) return false;
            if (result != XR_SUCCESS) { disabled = true; return false; }
            waited = true;
        }
        auto& image = images[index];
        const auto completed = fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
        if (completed == UINT64_MAX) { disabled = true; return false; }
        if (completed < image.fence_value) return false;
        const bool bgra = format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const auto pixels = rasterize_fps_overlay(width, height, snapshot, bgra);
        if (pixels.empty()) return false;
        HRESULT result = S_OK;
        const auto value = next_fence++;
        if (device11) {
            // Runs on the same application end-frame thread as bridge capture,
            // never from the asynchronous presenter. No context state changes.
            if (multithread11) multithread11->Enter();
            context11->UpdateSubresource(image.texture11.Get(), 0, nullptr, pixels.data(), width * 4, 0);
            result = context11->Signal(fence11.Get(), value);
            context11->Flush();
            if (multithread11) multithread11->Leave();
        } else {
            if (FAILED(image.allocator->Reset()) || FAILED(image.list->Reset(image.allocator.Get(), nullptr))) {
                disabled = true; return false;
            }
            void* mapped{};
            const D3D12_RANGE empty{};
            if (FAILED(image.upload->Map(0, &empty, &mapped))) { disabled = true; return false; }
            for (std::uint32_t row = 0; row < height; ++row)
                std::memcpy(static_cast<std::uint8_t*>(mapped) + image.footprint.Offset +
                    static_cast<std::size_t>(row) * image.footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<std::size_t>(row) * width, width * 4);
            image.upload->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = image.texture12.Get();
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            image.list->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = image.upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = image.footprint;
            destination.pResource = image.texture12.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            image.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            image.list->ResourceBarrier(1, &barrier);
            if (FAILED(image.list->Close())) { disabled = true; return false; }
            ID3D12CommandList* lists[]{image.list.Get()};
            queue12->ExecuteCommandLists(1, lists);
            result = queue12->Signal(fence12.Get(), value);
        }
        if (FAILED(result)) { disabled = true; return false; }
        last_fence = image.fence_value = value;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(release(swapchain, &info))) { disabled = true; return false; }
        acquired = waited = false;
        image_valid = true;
        return true;
    }

    void application_frame(const XrFrameEndInfo* info) {
        const auto now = now_ns();
        if (!info || info->type != XR_TYPE_FRAME_END_INFO) return;
        if (now < next_refresh) return;
        next_refresh = now + 250'000'000;
        std::array<wchar_t, 32> setting{};
        GetPrivateProfileStringW(L"overlay", L"position", L"upper_right",
            setting.data(), static_cast<DWORD>(setting.size()), ini.c_str());
        std::string value;
        for (wchar_t c : setting) { if (!c) break; value += c < 128 ? static_cast<char>(c) : '?'; }
        position = parse_overlay_position(value);
        if (position == FpsOverlayPosition::off || disabled || !info->layerCount || !info->layers) return;
        float left = -1, right = 1, down = -1, up = 1;
        std::uint32_t eye_width = 0;
        for (std::uint32_t i = 0; i < info->layerCount; ++i) {
            const auto* layer = info->layers[i];
            if (!layer || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            if (!projection->views || projection->viewCount > 16) continue;
            for (std::uint32_t v = 0; v < projection->viewCount; ++v) {
                const auto& view = projection->views[v];
                if (view.subImage.imageRect.extent.width <= 0) continue;
                const auto w = static_cast<std::uint32_t>(view.subImage.imageRect.extent.width);
                eye_width = eye_width ? std::min(eye_width, w) : w;
                if (std::isfinite(view.fov.angleLeft) && std::isfinite(view.fov.angleRight) &&
                    std::isfinite(view.fov.angleDown) && std::isfinite(view.fov.angleUp) &&
                    view.fov.angleLeft < 0 && view.fov.angleRight > 0 &&
                    view.fov.angleDown < 0 && view.fov.angleUp > 0) {
                    left = std::max(left, std::tan(view.fov.angleLeft));
                    right = std::min(right, std::tan(view.fov.angleRight));
                    down = std::max(down, std::tan(view.fov.angleDown));
                    up = std::min(up, std::tan(view.fov.angleUp));
                }
            }
        }
        if (!eye_width || !initialize(eye_width)) return;
        placement = overlay_placement(position, left, right, down, up);
        quad.pose.position = {placement.x, placement.y, placement.z};
        quad.size = {placement.width, placement.height};
        upload(counter.snapshot(now));
    }
};

OpenXrFpsOverlay::OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
    ID3D12Device* device12, ID3D12CommandQueue* queue12,
    ID3D11Device* device11, const std::filesystem::path& ini) : impl_(std::make_unique<Impl>()) {
    impl_->instance = instance; impl_->session = session; impl_->system = system;
    impl_->get = get_proc; impl_->downstream_end = end_frame;
    impl_->device12 = device12; impl_->queue12 = queue12; impl_->device11 = device11;
    impl_->ini = ini;
}
OpenXrFpsOverlay::~OpenXrFpsOverlay() = default;

void OpenXrFpsOverlay::application_frame(const XrFrameEndInfo* info) noexcept {
    try { std::scoped_lock lock(impl_->mutex); impl_->application_frame(info); }
    catch (...) { /* Overlay is optional: never fail the game's frame. */ }
}
void OpenXrFpsOverlay::reset_metrics() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->counter.reset();
    impl_->image_valid = false;
    impl_->next_refresh = 0;
}

FpsSnapshot OpenXrFpsOverlay::metrics() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->counter.snapshot(now_ns());
}

XrResult OpenXrFpsOverlay::end_frame(const XrFrameEndInfo* info, bool synthetic) {
    std::scoped_lock lock(impl_->mutex);
    std::array<const XrCompositionLayerBaseHeader*, 128> layers{};
    XrFrameEndInfo composed{};
    const bool nonempty = info && info->type == XR_TYPE_FRAME_END_INFO && info->layers && info->layerCount;
    const XrFrameEndInfo* submitted = info;
    if (nonempty && impl_->initialized && impl_->image_valid && !impl_->disabled &&
        impl_->position != FpsOverlayPosition::off && info->layerCount < impl_->max_layers) {
        std::copy_n(info->layers, info->layerCount, layers.begin());
        layers[info->layerCount] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl_->quad);
        composed = *info; // Preserve every app layer, next chain and display time.
        composed.layers = layers.data();
        ++composed.layerCount;
        submitted = &composed;
    }
    const auto result = impl_->downstream_end(impl_->session, submitted);
    if (result == XR_SUCCESS && nonempty) impl_->counter.submitted(now_ns(), synthetic);
    return result;
}

} // namespace xrfg
