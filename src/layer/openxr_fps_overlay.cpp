#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/benchmark_log.hpp"
#include "xrfg/elliott_meter.hpp"
#include "xrfg/presented_output_rate.hpp"
#include "xrfg/accepted_scene_rate.hpp"
#include "xrfg/steamvr_delivery.hpp"
#include "xrfg/overlay_menu_renderer.hpp"
#include "xrfg/settings_widgets.hpp"
#include "xrfg/menu_presentation.hpp"
#include "imgui.h"
#include <openxr/openxr_platform.h>
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
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
    ObservedEndFrame private_delivery_end;
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
    AcceptedSceneRate accepted_scenes;
    InterfaceTimingWindow timings;
    DisplayTimeConverter converter{};
    void* converter_user{};
    std::int64_t next_telemetry{};
    bool native_content_counter{};
    bool direct_acceptance{};
    std::function<std::optional<float>(std::int64_t)> runtime_acceptance;
    std::optional<float> runtime_rate(std::int64_t now) {
        if(runtime_acceptance)return runtime_acceptance(now);
        if(direct_acceptance){const auto s=counter.snapshot(now);if(s.sample_valid)return s.submitted_fps;}
        return {};
    }
    struct PendingNativeCount {
        std::vector<std::shared_ptr<D3D12NativeFrameResult>> results;
        std::int64_t submitted_ns{};
        unsigned output{};
        std::shared_ptr<InterfaceCycle> cycle;
        std::int64_t accepted_qpc{}, period{};
        SceneIdentity scene;
        std::shared_ptr<InterfaceDeliveryReceipt> delivery;
        std::int64_t entered_qpc{};
    };
    std::vector<PendingNativeCount> pending_native_counts;
    void poll_native_counts(std::int64_t now) noexcept {
        for (auto i=pending_native_counts.begin(); i!=pending_native_counts.end();) {
            bool pending=false;
            for(const auto& r:i->results) pending |= r && r->resolve()==D3D12NativeFrameResult::pending;
            if(pending && now-i->submitted_ns < 1'000'000'000) { ++i; continue; }
            if(!pending && classify_native_frame_content(i->results,i->output,true,true).synthetic) {
                counter.submitted(i->submitted_ns,true,true);
                if(luke_scene_measurement)accepted_scenes.accepted(i->submitted_ns,i->scene);
                if(i->cycle)i->cycle->synthetic_accepted(i->accepted_qpc,i->period,i->entered_qpc);
                if(i->delivery)i->delivery->synthetic_confirmed.store(true,std::memory_order_release);
            }
            i=pending_native_counts.erase(i);
        }
    }
    SteamVrDelivery* delivery{}; // Session-owned; overlay retires first.
    bool afw_active{};
    std::atomic<std::int64_t> display_period_ns{};
    std::int64_t physical_display_period_ns{}; // Cached existing SteamVR telemetry.
    std::int64_t estimate_period() const noexcept {
        return physical_display_period_ns>0 ? physical_display_period_ns : display_period_ns.load(std::memory_order_relaxed);
    }
    FpsOverlayPosition position{FpsOverlayPosition::upper_right};
    std::int64_t next_refresh{};
    bool attempted{}, initialized{}, fps_attempted{}, fps_initialized{}, image_valid{}, acquired{}, waited{}, disabled{};
    XrSpace space{};
    XrSwapchain swapchain{};
    std::uint32_t index{}, width{}, height{}, max_layers{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    OverlayPlacement placement{};
    bool placement_valid{};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Multithread> multithread11;
    ComPtr<ID3D11Fence> fence11;
    std::uint64_t next_fence{1}, last_fence{};
    bool fps_unfenced_submission{};
    bool unique_content_only{};
    bool luke_scene_measurement{};
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
    struct MeterSurface {
        XrSwapchain swapchain{};
        unsigned index{}, width{kElliottMeterWidth}, height{kElliottMeterHeight}, rows{};
        bool attempted{}, initialized{}, acquired{}, waited{}, image_valid{}, failed{};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        std::vector<Image> images;
    } meter;
    std::array<XrSpace,2> grips{};
    XrSpace local{};
    bool local_attempted{};
    PFN_xrLocateSpace locate_space{};
    FlipGesture gesture;
    struct MeterPaint { autonomous::Snapshot state; std::uint64_t now_ms{}; bool bgra{}, linear{}; };
    std::optional<MeterPaint> meter_paint;
    bool elliott_mode() const noexcept {
        return position==FpsOverlayPosition::elliott_controller || position==FpsOverlayPosition::elliott_bottom;
    }

    static constexpr std::uint32_t menu_width = 1024, menu_height = 768;
    bool menu_enabled{}, menu_attempted{}, menu_initialized{}, menu_open{}, menu_valid{}, menu_focused{true};
    bool menu_acquired{}, menu_waited{};
    XrSwapchain menu_swapchain{};
    std::uint32_t menu_index{};
    bool menu_unfenced_submission{};
    std::uint32_t menu_unfenced_index{};
    XrCompositionLayerQuad menu_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<Image> menu_images;
    OverlayMenuRenderer menu_renderer;
    std::unique_ptr<OverlayMenuInput> menu_input;
    OpenXrFpsOverlay::MenuInputSource menu_input_source{};
    void* menu_input_user{};
    OverlayMenuInputFrame pending_menu_input;
    autonomous::SettingsWidgetState menu_widgets;
    MenuPresentationFov menu_fov;
    MenuPresentationLayout menu_layout;
    MenuPresentationInteraction menu_interaction;

    void close_menu(bool release_input = true) noexcept {
        const bool was_open = menu_open || menu_valid || pending_menu_input.open;
        if (was_open) menu_event(0, 0);
        if (was_open) autonomous::set_interface_visible(autonomous::InterfaceSurface::headset, false);
        if (release_input && menu_input) menu_input->close();
        menu_open = menu_valid = false;
        pending_menu_input = {};
        if (was_open) {
            autonomous::finish_settings_widget_edits(menu_widgets);
            menu_renderer.reset_input();
        }
        reset_menu_drag(menu_interaction);
    }

    void menu_event(unsigned stage, std::int64_t result) noexcept {
        bridge_flight_logger().event(BridgeFlightOperation::menu_status, result, stage,
            reinterpret_cast<std::uintptr_t>(menu_swapchain), menu_index);
    }
    bool menu_failure(unsigned stage, std::int64_t result) noexcept {
        menu_event(stage, result); close_menu(); return false;
    }

    static void draw_menu(void* user) {
        auto& self = *static_cast<Impl*>(user);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(menu_width), static_cast<float>(menu_height)));
        ImGui::Begin("OFXR menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextDisabled("F10: menu   Esc: close   Mouse: click / scroll   Drag empty space to move");
        const auto footer_height = ImGui::GetFrameHeightWithSpacing() + 24.F;
        ImGui::BeginChild("OFXR options", ImVec2(0, -footer_height));
        autonomous::render_settings_widgets(self.menu_widgets, autonomous::snapshot(), {true, true});
        ImGui::EndChild();
        const auto drag = update_menu_drag(self.menu_interaction, self.menu_widgets, self.menu_layout, "OFXR menu");
        if (self.menu_input && (drag.consumed_dx != 0 || drag.consumed_dy != 0))
            self.menu_input->offset_pointer(-drag.consumed_dx, -drag.consumed_dy);
        render_menu_footer(self.menu_widgets, self.menu_interaction);
        ImGui::End();
    }

    bool initialize_menu() {
        if (menu_attempted) return menu_initialized;
        menu_attempted = true;
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_FAILED(system_properties(instance, system, &properties)) ||
            menu_width > properties.graphicsProperties.maxSwapchainImageWidth ||
            menu_height > properties.graphicsProperties.maxSwapchainImageHeight) return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = menu_width; info.height = menu_height;
        const auto created = create_swapchain(session, &info, &menu_swapchain);
        menu_event(1, created);
        if(XR_SUCCEEDED(created) || created==XR_ERROR_LIMIT_REACHED)
            autonomous::report_overlay_limit(autonomous::OverlaySwapchain::menu,created==XR_ERROR_LIMIT_REACHED);
        if (XR_FAILED(created)) {
            // A temporary runtime budget refusal must not permanently latch
            // F10 off. Closing releases input; the next explicit opening can
            // retry once after a route change has freed output handles.
            if (created == XR_ERROR_LIMIT_REACHED) menu_attempted = false;
            return false;
        }
        std::uint32_t count{};
        if (XR_FAILED(enumerate_images(menu_swapchain, 0, &count, nullptr)) || !count || count > 64) return false;
        menu_images.resize(count);
        if (device11) {
            std::vector<XrSwapchainImageD3D11KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(enumerate_images(menu_swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < menu_images.size(); ++i) {
                menu_images[i].texture11 = buffers[i].texture;
                if (!menu_images[i].texture11) return false;
            }
        } else {
            std::vector<XrSwapchainImageD3D12KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            if (XR_FAILED(enumerate_images(menu_swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < menu_images.size(); ++i) {
                auto& image = menu_images[i];
                image.texture12 = buffers[i].texture;
                if (!image.texture12 ||
                    FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&image.allocator))) ||
                    FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, image.allocator.Get(), nullptr,
                        IID_PPV_ARGS(&image.list))) || FAILED(image.list->Close())) return false;
            }
        }
        if (FAILED(menu_renderer.initialize(device12.Get(), queue12.Get(), device11.Get(), format,
                count, menu_width, menu_height))) return false;
        menu_quad.space = space;
        menu_quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        menu_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        menu_quad.subImage.swapchain = menu_swapchain;
        menu_quad.subImage.imageRect.extent = {menu_width, menu_height};
        menu_quad.pose.orientation.w = 1;
        menu_initialized = true;
        return true;
    }

    bool render_menu() {
        if (menu_unfenced_submission) { close_menu(); return false; }
        if (!menu_acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            const auto result = acquire(menu_swapchain, &info, &menu_index);
            if (XR_FAILED(result)) return menu_failure(2, result);
            menu_acquired = true; menu_waited = false;
        }
        if (menu_index >= menu_images.size()) { close_menu(); return false; }
        if (!menu_waited) {
            XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; info.timeout = 0;
            const auto result = wait(menu_swapchain, &info);
            if (result == XR_TIMEOUT_EXPIRED) return false;
            if (result != XR_SUCCESS) return menu_failure(3, result);
            menu_waited = true;
        }
        auto& image = menu_images[menu_index];
        const auto completed = fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
        if (completed == UINT64_MAX) return menu_failure(4, DXGI_ERROR_DEVICE_REMOVED);
        if (completed < image.fence_value) return false;
        const auto value = next_fence++;
        HRESULT result{};
        if (device11) {
            if (multithread11) multithread11->Enter();
            result = menu_renderer.render_d3d11(image.texture11.Get(), menu_index,
                pending_menu_input, draw_menu, this);
            if (result == S_OK) {
                menu_unfenced_submission = true; menu_unfenced_index = menu_index;
                result = context11->Signal(fence11.Get(), value);
                if (SUCCEEDED(result)) result = menu_renderer.submitted_d3d11(menu_index, fence11.Get(), value);
                if (SUCCEEDED(result)) menu_unfenced_submission = false;
                context11->Flush();
            }
            if (multithread11) multithread11->Leave();
        } else {
            if (FAILED(image.allocator->Reset()) || FAILED(image.list->Reset(image.allocator.Get(), nullptr))) {
                close_menu(); return false;
            }
            result = menu_renderer.render_d3d12(image.texture12.Get(), image.list.Get(), menu_index,
                pending_menu_input, draw_menu, this);
            const auto closed = image.list->Close();
            if (result == S_OK && SUCCEEDED(closed)) {
                ID3D12CommandList* lists[]{image.list.Get()};
                queue12->ExecuteCommandLists(1, lists);
                menu_unfenced_submission = true; menu_unfenced_index = menu_index;
                result = queue12->Signal(fence12.Get(), value);
                if (SUCCEEDED(result)) result = menu_renderer.submitted_d3d12(menu_index, fence12.Get(), value);
                if (SUCCEEDED(result)) menu_unfenced_submission = false;
            } else {
                // No execution: discard this recording before releasing its
                // immutable draw resources. Never cancel a submitted draw.
                image.list.Reset();
                menu_renderer.abandon_d3d12(menu_index);
                if (result == S_OK) result = closed;
                if (FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    image.allocator.Get(), nullptr, IID_PPV_ARGS(&image.list))) || FAILED(image.list->Close())) {
                    close_menu(); return false;
                }
            }
        }
        if (result == S_FALSE || result == E_PENDING) return false;
        if (FAILED(result)) return menu_failure(6, result);
        last_fence = image.fence_value = value;
        pending_menu_input.events.clear();
        pending_menu_input.wheel_x = pending_menu_input.wheel_y = 0;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto released = release(menu_swapchain, &info);
        if (XR_FAILED(released)) return menu_failure(7, released);
        menu_acquired = menu_waited = false;
        menu_valid = true;
        return true;
    }

    // Frame-thread cleanup polls receipts; never wait for the GPU or for a
    // runtime image that is still unavailable. Retry on the next app frame.
    bool release_unused(XrSwapchain chain,std::vector<Image>& owned,bool& held,bool& ready) {
        if(!chain)return true;
        if(menu_unfenced_submission || fps_unfenced_submission)return false;
        const auto completed=fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
        if(completed==UINT64_MAX)return false;
        for(const auto& image:owned)if(image.fence_value>completed)return false;
        if(held) {
            if(!ready) {
                XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};info.timeout=0;
                if(wait(chain,&info)!=XR_SUCCESS)return false;
                ready=true;
            }
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if(XR_FAILED(release(chain,&info)))return false;
            held=ready=false;
        }
        return true;
    }
    void release_unused_surfaces() {
        if((position==FpsOverlayPosition::off || elliott_mode()) && !swapchain)fps_attempted=false;
        if((position==FpsOverlayPosition::off || elliott_mode()) && swapchain) {
            image_valid=false;
            if(release_unused(swapchain,images,acquired,waited) && XR_SUCCEEDED(destroy_swapchain(swapchain))) {
                bridge_flight_logger().event(BridgeFlightOperation::menu_status,0,10,reinterpret_cast<std::uintptr_t>(swapchain),0);
                images.clear();swapchain={};quad.subImage.swapchain={};fps_attempted=fps_initialized=false;
            }
        }
        if(!elliott_mode()) {
            meter.image_valid=false;meter_paint.reset();
            if(!meter.swapchain)meter=MeterSurface{};
            if(meter.swapchain && release_unused(meter.swapchain,meter.images,meter.acquired,meter.waited) &&
                XR_SUCCEEDED(destroy_swapchain(meter.swapchain))) {
                bridge_flight_logger().event(BridgeFlightOperation::menu_status,0,11,reinterpret_cast<std::uintptr_t>(meter.swapchain),0);
                meter=MeterSurface{};
            }
        }
        if(!menu_open && menu_swapchain && release_unused(menu_swapchain,menu_images,menu_acquired,menu_waited) &&
            SUCCEEDED(menu_renderer.shutdown())) {
            menu_initialized=false;
            if(XR_SUCCEEDED(destroy_swapchain(menu_swapchain))) {
                menu_event(9,0);menu_images.clear();menu_swapchain={};menu_quad.subImage.swapchain={};
                menu_attempted=menu_valid=false;
            }
        }
    }

    bool retire() noexcept {
        close_menu();
        if ((last_fence || menu_unfenced_submission || fps_unfenced_submission) && event) {
            if ((device12 && FAILED(device12->GetDeviceRemovedReason())) ||
                (device11 && FAILED(device11->GetDeviceRemovedReason()))) return true;
            // A fresh owner-queue receipt also covers an executed menu draw
            // whose earlier Signal failed. No allocator/atlas/texture is freed
            // without proof of GPU retirement; frame submission never waits.
            const auto value = next_fence++;
            const auto signaled = fence12 ? queue12->Signal(fence12.Get(), value)
                : context11->Signal(fence11.Get(), value);
            if (FAILED(signaled)) return false;
            if (menu_unfenced_submission) {
                const auto recorded = fence12 ? menu_renderer.submitted_d3d12(menu_unfenced_index, fence12.Get(), value)
                    : menu_renderer.submitted_d3d11(menu_unfenced_index, fence11.Get(), value);
                if (FAILED(recorded)) return false;
                menu_unfenced_submission = false;
            }
            if (context11) context11->Flush();
            const auto completed = fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
            if (completed == UINT64_MAX) return true;
            if (completed < value) {
                const auto registered = fence12 ? fence12->SetEventOnCompletion(value, event)
                    : fence11->SetEventOnCompletion(value, event);
                if (FAILED(registered) || WaitForSingleObject(event, 5000) != WAIT_OBJECT_0) return false;
            }
        }
        return SUCCEEDED(menu_renderer.shutdown());
    }

    ~Impl() {
        autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,false);
        menu_renderer.shutdown();
        if (meter.swapchain && destroy_swapchain) destroy_swapchain(meter.swapchain);
        if (local && destroy_space) destroy_space(local);
        if (menu_swapchain && destroy_swapchain) destroy_swapchain(menu_swapchain);
        if (swapchain && destroy_swapchain) destroy_swapchain(swapchain);
        if (space && destroy_space) destroy_space(space);
        if (event) CloseHandle(event);
    }

    bool initialize() {
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
        XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_info.poseInReferenceSpace.orientation.w = 1;
        if (XR_FAILED(create_space(session, &space_info, &space))) return false;
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        if(device11) {
            ComPtr<ID3D11DeviceContext> immediate;device11->GetImmediateContext(&immediate);
            if(!immediate || FAILED(immediate.As(&context11)))return false;
            immediate.As(&multithread11);
            ComPtr<ID3D11Device5> device5;
            if(FAILED(device11.As(&device5)) || FAILED(device5->CreateFence(
                0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence11))))return false;
        } else if(FAILED(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence12))))return false;
        initialized=true;return true;
    }

    bool initialize_fps(std::uint32_t eye_width) {
        if(swapchain)return fps_initialized;
        if(fps_attempted)return false;
        fps_attempted=true;
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        std::uint32_t count{};
        if(XR_FAILED(system_properties(instance,system,&properties)))return false;
        width = overlay_texture_width(eye_width);
        height = width / 2;
        if (width > properties.graphicsProperties.maxSwapchainImageWidth ||
            height > properties.graphicsProperties.maxSwapchainImageHeight) return false;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = width;
        info.height = height;
        const auto created=create_swapchain(session,&info,&swapchain);
        if(XR_SUCCEEDED(created) || created==XR_ERROR_LIMIT_REACHED)
            autonomous::report_overlay_limit(autonomous::OverlaySwapchain::fps,created==XR_ERROR_LIMIT_REACHED);
        if(XR_FAILED(created)){fps_attempted=created!=XR_ERROR_LIMIT_REACHED;return false;}
        if (XR_FAILED(enumerate_images(swapchain, 0, &count, nullptr)) || !count || count > 64) return false;
        images.resize(count);
        if (device11) {
            std::vector<XrSwapchainImageD3D11KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(enumerate_images(swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < images.size(); ++i) {
                images[i].texture11 = buffers[i].texture;
                if (!images[i].texture11) return false;
            }
        } else {
            std::vector<XrSwapchainImageD3D12KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            if (XR_FAILED(enumerate_images(swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
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
        fps_initialized=true;image_valid = false;
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
            fps_unfenced_submission = true;
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
            fps_unfenced_submission = true;
            result = queue12->Signal(fence12.Get(), value);
        }
        if (FAILED(result)) { disabled = true; return false; }
        fps_unfenced_submission = false;
        last_fence = image.fence_value = value;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(release(swapchain, &info))) { disabled = true; return false; }
        acquired = waited = false;
        image_valid = true;
        return true;
    }


    bool initialize_meter() {
        if(meter.attempted)return meter.initialized;
        meter.attempted=true;
        XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
        if(XR_FAILED(system_properties(instance,system,&properties)) ||
            meter.width>properties.graphicsProperties.maxSwapchainImageWidth ||
            meter.height>properties.graphicsProperties.maxSwapchainImageHeight)return false;
        unsigned count{};
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
            XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = format;
        info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
        info.width = meter.width;
        info.height = meter.height;
        const auto created=create_swapchain(session,&info,&meter.swapchain);
        if(XR_SUCCEEDED(created) || created==XR_ERROR_LIMIT_REACHED)
            autonomous::report_overlay_limit(autonomous::OverlaySwapchain::meter,created==XR_ERROR_LIMIT_REACHED);
        if(XR_FAILED(created)){meter.attempted=created!=XR_ERROR_LIMIT_REACHED;return false;}
        if (XR_FAILED(enumerate_images(meter.swapchain, 0, &count, nullptr)) || !count || count > 64) return false;
        meter.images.resize(count);
        if (device11) {
            std::vector<XrSwapchainImageD3D11KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            if (XR_FAILED(enumerate_images(meter.swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < meter.images.size(); ++i) {
                meter.images[i].texture11 = buffers[i].texture;
                if (!meter.images[i].texture11) return false;
            }
        } else {
            std::vector<XrSwapchainImageD3D12KHR> buffers(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            if (XR_FAILED(enumerate_images(meter.swapchain, count, &count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(buffers.data())))) return false;
            for (std::size_t i = 0; i < meter.images.size(); ++i) {
                auto& image = meter.images[i];
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
        meter.quad.space = space;
        meter.quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        meter.quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        meter.quad.subImage.swapchain = meter.swapchain;
        meter.quad.subImage.imageRect.extent = {static_cast<std::int32_t>(meter.width), static_cast<std::int32_t>(meter.height)};
        meter.quad.pose.orientation.w = 1;
        meter.initialized = true;
        return true;
    }
    bool upload_meter(const std::vector<std::uint32_t>& pixels, unsigned rows) {
        if (!meter.acquired) {
            XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(acquire(meter.swapchain, &info, &meter.index))) { meter.failed = true; return false; }
            meter.acquired = true;
            meter.waited = false;
        }
        if (meter.index >= meter.images.size()) { meter.failed = true; return false; }
        if (!meter.waited) {
            XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            info.timeout = 0;
            const auto result = wait(meter.swapchain, &info);
            // XR_TIMEOUT_EXPIRED is positive, but does NOT grant ownership.
            if (result == XR_TIMEOUT_EXPIRED) return false;
            if (result != XR_SUCCESS) { meter.failed = true; return false; }
            meter.waited = true;
        }
        auto& image = meter.images[meter.index];
        const auto completed = fence12 ? fence12->GetCompletedValue() : fence11->GetCompletedValue();
        if (completed == UINT64_MAX) { meter.failed = true; return false; }
        if (completed < image.fence_value) return false;
        if (pixels.empty()) return false;
        HRESULT result = S_OK;
        const auto value = next_fence++;
        if (device11) {
            // Runs on the same application end-frame thread as bridge capture,
            // never from the asynchronous presenter. No context state changes.
            if (multithread11) multithread11->Enter();
            context11->UpdateSubresource(image.texture11.Get(), 0, nullptr, pixels.data(), meter.width * 4, 0);
            fps_unfenced_submission = true;
            result = context11->Signal(fence11.Get(), value);
            context11->Flush();
            if (multithread11) multithread11->Leave();
        } else {
            if (FAILED(image.allocator->Reset()) || FAILED(image.list->Reset(image.allocator.Get(), nullptr))) {
                meter.failed = true; return false;
            }
            void* mapped{};
            const D3D12_RANGE empty{};
            if (FAILED(image.upload->Map(0, &empty, &mapped))) { meter.failed = true; return false; }
            for (std::uint32_t row = 0; row < meter.height; ++row)
                std::memcpy(static_cast<std::uint8_t*>(mapped) + image.footprint.Offset +
                    static_cast<std::size_t>(row) * image.footprint.Footprint.RowPitch,
                    pixels.data() + static_cast<std::size_t>(row) * meter.width, meter.width * 4);
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
            if (FAILED(image.list->Close())) { meter.failed = true; return false; }
            ID3D12CommandList* lists[]{image.list.Get()};
            queue12->ExecuteCommandLists(1, lists);
            fps_unfenced_submission = true;
            result = queue12->Signal(fence12.Get(), value);
        }
        if (FAILED(result)) { meter.failed = true; return false; }
        fps_unfenced_submission = false;
        last_fence = image.fence_value = value;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(release(meter.swapchain, &info))) { meter.failed = true; return false; }
        meter.acquired = meter.waited = false;
        meter.rows=rows;
        meter.image_valid = true;
        return true;
    }


    bool locate_grip(int hand,XrTime time,Pose& pose) const {
        if(!grips[hand] || !local || !locate_space)return false;
        XrSpaceLocation l{XR_TYPE_SPACE_LOCATION};
        if(XR_FAILED(locate_space(grips[hand],local,time,&l)))return false;
        constexpr auto needed=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT|
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        if((l.locationFlags&needed)!=needed)return false;
        pose={{l.pose.orientation.x,l.pose.orientation.y,l.pose.orientation.z,l.pose.orientation.w},
            {l.pose.position.x,l.pose.position.y,l.pose.position.z}};
        return true;
    }
    bool place_meter(XrTime time,std::int64_t now) {
        if(!elliott_mode() || disabled || meter.failed || !menu_focused)return false;
        float width=.56F;
        if(position==FpsOverlayPosition::elliott_bottom){
            meter.quad.space=space;
            const float tilt=-std::atan2(.30F,1.F);
            meter.quad.pose.orientation={std::sin(tilt*.5F),0,0,std::cos(tilt*.5F)};
            meter.quad.pose.position={0,-.30F,-1.F};
        }else{
            std::array<Pose,2> pose{};std::array<bool,2> flipped{};
            for(int hand=0;hand<2;++hand)flipped[hand]=locate_grip(hand,time,pose[hand])&&grip_upside_down(pose[hand].orientation);
            const bool shown=gesture.observe(now,flipped[0],flipped[1]);
            const int hand=gesture.hand();
            if(!shown || hand<0 || !locate_grip(hand,time,pose[hand]))return false;
            width=.42F;
            const Pose placed=panel_pose_on_grip(pose[hand],width*float(std::max(meter.rows,1u))/float(kElliottMeterWidth));
            meter.quad.space=local;
            meter.quad.pose.orientation={placed.orientation.x,placed.orientation.y,placed.orientation.z,placed.orientation.w};
            meter.quad.pose.position={placed.position.x,placed.position.y,placed.position.z};
        }
        meter.quad.size={width,width*float(meter.rows)/float(kElliottMeterWidth)};
        meter.quad.subImage.imageRect.extent={int(kElliottMeterWidth),int(meter.rows)};
        return meter.image_valid && meter.rows;
    }
    void prepare_meter(const XrFrameEndInfo* info,bool refresh,std::int64_t now) {
        if(!elliott_mode() || disabled || !menu_focused || meter.failed){
            autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,false);return;
        }
        if(position==FpsOverlayPosition::elliott_controller && !local_attempted && (grips[0]||grips[1])){
            local_attempted=true;
            if(load(get,instance,"xrLocateSpace",locate_space)){
                XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
                ci.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;ci.poseInReferenceSpace.orientation.w=1;
                if(XR_FAILED(create_space(session,&ci,&local)))local=XR_NULL_HANDLE;
            }
        }
        const bool shown=place_meter(info->displayTime,now);
        const bool wanted=position==FpsOverlayPosition::elliott_bottom || shown || gesture.pending();
        autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,wanted);
        if(!wanted || !refresh)return;
        if(!initialize_meter()){meter.failed=meter.attempted;return;}
        const bool bgra=format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const bool linear=format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        meter_paint=MeterPaint{autonomous::snapshot(),GetTickCount64(),bgra,linear};
    }

    void refresh_telemetry() {
        if (!interface_measurement_requested()) return;
        const auto clock = interface_clock_ns();
        if (clock < next_telemetry) return;
        next_telemetry = clock + 250'000'000;
        const auto now = now_ns();
        poll_native_counts(now);
        const auto fps = counter.snapshot(now);
        InterfaceTelemetry out{};
        poll_presented_output(delivery, now, out);
        if(const auto accepted=runtime_rate(now)) {
            out.output_fps=*accepted;out.output_source=InterfaceSource::runtime_accepted_delivery;
        }
        out.game_fps = fps.real_fps;
        out.generated_fps = fps.generated_fps;
        out.rates_valid = fps.sample_valid;
        if(luke_scene_measurement) {
            // Preserve the established real/generated OFXR counters. Only the
            // total changes source: new accepted images rather than compositor
            // records (which include held images submitted again).
            out.output_fps=0;out.output_source=InterfaceSource::none;
            if(const auto scene=accepted_scenes.rate(now)) {
                out.output_fps=scene->output;
                out.output_source=InterfaceSource::accepted_scene_submissions;
                // Without OFXR the old real counter counts held AFW pairs/XR
                // calls. Use actual source/AFW production identities instead.
                if(!fps.active) {out.game_fps=scene->real;out.generated_fps=scene->generated;}
            } else if(bridge_flight_logger().enabled()) {
                const auto times=accepted_scenes.diagnostic_times();
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,fps.sample_valid,1241,times[1],times[2]);
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,0,1242,times[0],now);
            }
        }
        out.generating = fps.generated_fps > 0;
        auto period = display_period_ns.load(std::memory_order_relaxed);
        out.display_source = period > 0 ? InterfaceSource::openxr_period : InterfaceSource::none;
        if (delivery) {
            if (const auto gpu = delivery->gpu_render_timing(now)) {
                if (gpu->display_period) { period = gpu->display_period->count(); physical_display_period_ns=period; out.display_source = InterfaceSource::steamvr_display; }
            }
        }
        out.display_hz = period > 0 ? 1.0e9F / float(period) : 0;
        timings.snapshot(clock, out);
        log_benchmark_telemetry(out);
        autonomous::report_interface_telemetry(out);
    }

    void application_frame(const XrFrameEndInfo* info) {
        const auto now = now_ns();
        if (!info || info->type != XR_TYPE_FRAME_END_INFO) return;
        const bool refresh_fps = now >= next_refresh;
        if (refresh_fps) {
            next_refresh = now + 250'000'000;
            std::array<wchar_t, 32> setting{};
            GetPrivateProfileStringW(L"overlay", L"position", L"upper_right",
                setting.data(), static_cast<DWORD>(setting.size()), ini.c_str());
            std::string value;
            for (wchar_t c : setting) { if (!c) break; value += c < 128 ? static_cast<char>(c) : '?'; }
            const auto next=parse_overlay_position(value);
            if(next!=position)gesture.reset();
            position = next;
        }
        if (disabled || !info->layerCount || !info->layers) { close_menu();release_unused_surfaces();return; }
        if (menu_enabled && menu_focused) {
            auto input = menu_input_source ? menu_input_source(menu_input_user) : menu_input->poll(menu_width, menu_height);
            menu_open = input.open && input.focused;
            if (!menu_open) { close_menu(!input.requested_open); }
            else {
                // A busy runtime image may postpone drawing. Retain ordered
                // press/release pairs until a successful ImGui frame, so a
                // click entirely between two uploads is not lost.
                auto events = std::move(pending_menu_input.events);
                const auto wheel_x = pending_menu_input.wheel_x, wheel_y = pending_menu_input.wheel_y;
                pending_menu_input = std::move(input);
                events.insert(events.end(), pending_menu_input.events.begin(), pending_menu_input.events.end());
                pending_menu_input.events = std::move(events);
                pending_menu_input.wheel_x += wheel_x; pending_menu_input.wheel_y += wheel_y;
                if (pending_menu_input.events.size() > 2048) { close_menu(); }
            }
        }
        if (menu_open) autonomous::set_interface_visible(autonomous::InterfaceSurface::headset, true);
        if(!elliott_mode())autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,false);
        refresh_telemetry();
        release_unused_surfaces();
        if ((!refresh_fps || position == FpsOverlayPosition::off) && !menu_open && !elliott_mode()) return;
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
                // The quad uses physical visible bounds, not the source image's
                // vertical storage direction. Never reorder the submitted FOV.
                const float down_angle = std::min(view.fov.angleDown, view.fov.angleUp);
                const float up_angle = std::max(view.fov.angleDown, view.fov.angleUp);
                if (std::isfinite(view.fov.angleLeft) && std::isfinite(view.fov.angleRight) &&
                    std::isfinite(view.fov.angleDown) && std::isfinite(view.fov.angleUp) &&
                    view.fov.angleLeft < 0 && view.fov.angleRight > 0 &&
                    down_angle < 0 && up_angle > 0 &&
                    down_angle > -1.57079632679489661923F && up_angle < 1.57079632679489661923F) {
                    left = std::max(left, std::tan(view.fov.angleLeft));
                    right = std::min(right, std::tan(view.fov.angleRight));
                    down = std::max(down, std::tan(down_angle));
                    up = std::min(up, std::tan(up_angle));
                }
            }
        }
        if (!eye_width) return;
        placement = overlay_placement(position, left, right, down, up);
        placement_valid = true;
        if (!initialize()) { close_menu(); return; }
        if (info->layerCount >= max_layers) close_menu();
        if (menu_open) {
            if (!initialize_menu()) { close_menu(); }
            else {
                autonomous::synchronize_settings_widgets(menu_widgets, autonomous::snapshot());
                menu_fov = {left, right, down, up};
                menu_layout = calculate_menu_presentation(menu_fov, menu_widgets.draft);
                if (!menu_layout.valid) { close_menu(); return; }
                render_menu();
                // UI edits take effect on this same composed frame. The
                // framebuffer stays fixed, preserving pointer/hit coordinates.
                menu_layout = calculate_menu_presentation(menu_fov, menu_widgets.draft);
                menu_quad.pose.position = {menu_layout.x, menu_layout.y, menu_layout.z};
                menu_quad.size = {menu_layout.width, menu_layout.height};
            }
        }
        prepare_meter(info,refresh_fps,now);
        refresh_telemetry();
        if (!refresh_fps || position == FpsOverlayPosition::off || elliott_mode()) return;
        quad.pose.position = {placement.x, placement.y, placement.z};
        quad.size = {placement.width, placement.height};
        poll_native_counts(now);
        auto snapshot = counter.snapshot(now);
        snapshot.afw_active = afw_active;
        if (delivery && !luke_scene_measurement) {
            if (const auto measured = delivery->delivered_fps(now)) {
                // A compositor also counts copied B slots. Its delivery rate
                // can cap confirmed unique native content, never inflate it.
                snapshot.delivered_fps = *measured;
                snapshot.delivered_valid = true;
            }
        }
        if(const auto accepted=runtime_rate(now)){snapshot.delivered_fps=*accepted;snapshot.delivered_valid=true;}
        if(luke_scene_measurement) {
            snapshot.delivered_valid=false;
            if(const auto scene=accepted_scenes.rate(now)) {
                snapshot.delivered_fps=scene->output;snapshot.delivered_valid=true;
            }
        }
        if (const auto period = display_period_ns.load(std::memory_order_relaxed); period > 0) {
            const auto hz = 1.0e9f / static_cast<float>(period);
            snapshot.submitted_fps = displayed_rate(snapshot.submitted_fps, hz);
        }
        if(initialize_fps(eye_width))upload(snapshot);
    }
};

OpenXrFpsOverlay::OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
    PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
    ID3D12Device* device12, ID3D12CommandQueue* queue12,
    ID3D11Device* device11, const std::filesystem::path& ini,
    bool interactive_menu, MenuInputSource input_source, void* input_user,
    DisplayTimeConverter converter, void* converter_user) : impl_(std::make_unique<Impl>()) {
    impl_->instance = instance; impl_->session = session; impl_->system = system;
    impl_->get = get_proc; impl_->downstream_end = end_frame;
    impl_->converter = converter; impl_->converter_user = converter_user;
    impl_->device12 = device12; impl_->queue12 = queue12; impl_->device11 = device11;
    impl_->ini = ini;
    impl_->menu_enabled = interactive_menu;
    impl_->menu_input_source = input_source; impl_->menu_input_user = input_user;
    if (interactive_menu && !input_source) impl_->menu_input = std::make_unique<OverlayMenuInput>();
}
OpenXrFpsOverlay::~OpenXrFpsOverlay() {
    if (impl_ && !impl_->retire()) {
        // A live-device queue failure has no safe destruction boundary. Keep
        // this owner's GPU resources alive; cursor/input were already closed.
        static_cast<void>(impl_.release());
    }
}

void OpenXrFpsOverlay::application_frame(const XrFrameEndInfo* info) noexcept {
    try {
        std::optional<Impl::MeterPaint> paint;
        { std::scoped_lock lock(impl_->mutex);impl_->application_frame(info);paint=std::move(impl_->meter_paint);impl_->meter_paint.reset(); }
        // R19's raster runs outside the presenter lock; uploads retain zero-timeout ownership checks.
        if(paint){
            unsigned rows{};
            auto pixels=rasterize_elliott_meter(paint->state,paint->now_ms,paint->bgra,paint->linear,&rows);
            std::scoped_lock lock(impl_->mutex);
            if(impl_->elliott_mode() && !impl_->disabled && !impl_->meter.failed && !pixels.empty())impl_->upload_meter(pixels,rows);
        }
    }
    catch (...) {
        // An optional UI failure must release capture as well as preserving
        // the application frame. Never leave input blocked behind a hidden UI.
        try { std::scoped_lock lock(impl_->mutex); impl_->close_menu(); } catch (...) {}
    }
}
void OpenXrFpsOverlay::set_afw_active(bool active) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->afw_active = active;
}
void OpenXrFpsOverlay::set_delivery_source(SteamVrDelivery* delivery) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->delivery = delivery;
}
void OpenXrFpsOverlay::set_runtime_acceptance_source(std::function<std::optional<float>(std::int64_t)> source,bool direct) noexcept {
    std::scoped_lock lock(impl_->mutex);impl_->runtime_acceptance=std::move(source);impl_->direct_acceptance=direct;
}
void OpenXrFpsOverlay::set_unique_content_only(bool enabled) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->unique_content_only=enabled;
    if(enabled)impl_->native_content_counter=true;
}
void OpenXrFpsOverlay::set_grip_spaces(XrSpace left,XrSpace right) noexcept {
    std::scoped_lock lock(impl_->mutex);impl_->grips={left,right};
}
void OpenXrFpsOverlay::set_luke_scene_measurement(bool enabled) noexcept {
    std::scoped_lock lock(impl_->mutex);impl_->luke_scene_measurement=enabled;impl_->accepted_scenes={};
}
void OpenXrFpsOverlay::set_display_period(std::int64_t period_ns) noexcept {
    impl_->display_period_ns.store(period_ns > 0 ? period_ns : 0, std::memory_order_relaxed);
}

void OpenXrFpsOverlay::reset_metrics() noexcept {
    if(bridge_flight_logger().enabled())bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,0,1239,0,0);
    std::scoped_lock lock(impl_->mutex);
    impl_->counter.reset();
    impl_->accepted_scenes={};
    impl_->timings = {};
    impl_->next_telemetry = 0;
    impl_->native_content_counter=impl_->unique_content_only;
    impl_->pending_native_counts.clear();
    impl_->image_valid = false;
    impl_->meter.image_valid=false;impl_->gesture.reset();
    impl_->next_refresh = 0;
    impl_->placement_valid = false;
}

void OpenXrFpsOverlay::suspend() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->disabled = true;
    autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,false);
    impl_->image_valid = false;
    impl_->counter.reset();
    impl_->accepted_scenes={};
    impl_->native_content_counter=impl_->unique_content_only;
    impl_->pending_native_counts.clear();
    impl_->close_menu();
}

void OpenXrFpsOverlay::close_menu() noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->close_menu();
}

void OpenXrFpsOverlay::set_session_focused(bool focused) noexcept {
    std::scoped_lock lock(impl_->mutex);
    impl_->menu_focused = focused;
    if (!focused) {impl_->close_menu();impl_->gesture.reset();autonomous::set_interface_visible(autonomous::InterfaceSurface::meter,false);}
}

std::optional<std::array<XrSwapchain,3>> OpenXrFpsOverlay::swapchain_handles() const noexcept {
    std::unique_lock lock(impl_->mutex,std::try_to_lock);
    if (!lock.owns_lock()) return std::nullopt;
    return std::array<XrSwapchain,3>{impl_->swapchain,impl_->meter.swapchain,impl_->menu_swapchain};
}

FpsSnapshot OpenXrFpsOverlay::metrics() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    const auto now=now_ns();
    impl_->poll_native_counts(now);
    auto result=impl_->counter.snapshot(now);
    if(impl_->luke_scene_measurement) {
        result.delivered_valid=false;
        if(const auto scene=impl_->accepted_scenes.rate(now)) {
            result.submitted_fps=result.delivered_fps=scene->output;
            if(!result.active) {result.real_fps=scene->real;result.generated_fps=scene->generated;}
            result.sample_valid=result.delivered_valid=true;
        }
    }
    return result;
}

std::optional<OverlayPlacement> OpenXrFpsOverlay::marker_placement() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->disabled || impl_->position == FpsOverlayPosition::off || impl_->elliott_mode() || !impl_->placement_valid)
        return std::nullopt;
    return impl_->placement;
}

void OpenXrFpsOverlay::set_private_delivery_end(ObservedEndFrame end) {
    std::scoped_lock lock(impl_->mutex);impl_->private_delivery_end=std::move(end);
}

XrResult OpenXrFpsOverlay::end_frame(const XrFrameEndInfo* info, bool synthetic, bool new_content,
    std::span<const std::shared_ptr<D3D12NativeFrameResult>> native_results, unsigned native_output,
    InterfaceFrameStamp stamp) {
    std::scoped_lock lock(impl_->mutex);
    std::array<const XrCompositionLayerBaseHeader*, 128> layers{};
    XrFrameEndInfo composed{};
    const bool nonempty = info && info->type == XR_TYPE_FRAME_END_INFO && info->layers && info->layerCount;
    if (impl_->menu_open && (!nonempty || info->layerCount >= impl_->max_layers)) impl_->close_menu();
    const XrFrameEndInfo* submitted = info;
    if (nonempty && impl_->initialized && !impl_->disabled && info->layerCount < impl_->max_layers) {
        std::copy_n(info->layers, info->layerCount, layers.begin());
        composed = *info; // Preserve every app layer, next chain and display time.
        composed.layers = layers.data();
        const bool menu = impl_->menu_open && impl_->menu_valid;
        // Reserve the final spare slot for interactive controls on runtimes
        // that expose only one extra layer. Preserve every application layer.
        if (impl_->image_valid && !impl_->elliott_mode() && impl_->position != FpsOverlayPosition::off &&
            composed.layerCount + (menu ? 1u : 0u) < impl_->max_layers)
            layers[composed.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl_->quad);
        if(impl_->elliott_mode() && impl_->place_meter(info->displayTime,now_ns()) &&
            composed.layerCount+(menu?1u:0u)<impl_->max_layers)
            layers[composed.layerCount++]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl_->meter.quad);
        if (menu && composed.layerCount < impl_->max_layers)
            layers[composed.layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&impl_->menu_quad);
        submitted = &composed;
    }
    const bool measure = interface_measurement_requested() && new_content && stamp.entered_ns;
    std::int64_t display{};
    if (measure && !synthetic && nonempty && impl_->converter)
        if (!impl_->converter(impl_->converter_user, info->displayTime, display)) display = 0;
    std::shared_ptr<InterfaceDeliveryReceipt> delivery;
    if(measure && nonempty && stamp.cycle && impl_->private_delivery_end) {
        stamp.cycle->require_native_delivery();
        try {delivery=std::make_shared<InterfaceDeliveryReceipt>(synthetic);
            stamp.cycle->attach_native_delivery(delivery);}catch(...) {} // Missing receipt stays N/A.
    }
    const auto submit_clock = measure ? interface_clock_ns() : 0;
    const auto result = delivery?impl_->private_delivery_end(submitted,delivery):impl_->downstream_end(impl_->session, submitted);
    const auto accepted_clock = measure ? interface_clock_ns() : 0;
    if (measure && !synthetic) {
        if(result==XR_SUCCESS && nonempty && stamp.cycle)stamp.cycle->real_prediction(display);
        impl_->timings.submitted(stamp, submit_clock, result == XR_SUCCESS,
            nonempty, synthetic, new_content, display, accepted_clock);
    }
    if (result != XR_SUCCESS) {
        if (impl_->menu_open) impl_->menu_event(8, result);
        impl_->close_menu();
    }
    if (result == XR_SUCCESS && nonempty) {
        const auto now=now_ns();
        impl_->poll_native_counts(now);
        impl_->native_content_counter=impl_->unique_content_only || !native_results.empty();
        const auto metrics=classify_native_frame_content(native_results,native_output,synthetic,new_content);
        SceneIdentity scene{stamp.scene_owner,stamp.scene_serial,metrics.synthetic?native_output+1:0};
        if(impl_->luke_scene_measurement && metrics.new_content) {
            if(!scene&&bridge_flight_logger().enabled())bridge_flight_logger().event(
                BridgeFlightOperation::luke_afw_stage,scene.owner?1:0,1240,info->displayTime,scene.serial);
            impl_->accepted_scenes.accepted(now,scene);
        }
        impl_->counter.submitted(now,metrics.synthetic,metrics.new_content);
        if(measure && stamp.cycle && metrics.synthetic)
            stamp.cycle->synthetic_accepted(accepted_clock,impl_->estimate_period(),submit_clock);
        if(delivery && metrics.synthetic)delivery->synthetic_confirmed.store(true,std::memory_order_release);
        if(synthetic && new_content && !metrics.synthetic) {
            bool pending=false;
            for(const auto& r:native_results) pending |= r && r->resolve()==D3D12NativeFrameResult::pending;
            if(pending && impl_->pending_native_counts.size()<64)
                impl_->pending_native_counts.push_back({{native_results.begin(),native_results.end()},now,native_output,
                    measure ? stamp.cycle : nullptr,accepted_clock,impl_->estimate_period(),
                    {stamp.scene_owner,stamp.scene_serial,native_output+1},delivery,submit_clock});
        }
    }
    return result;
}

} // namespace xrfg
