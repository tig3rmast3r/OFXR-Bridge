#pragma once

#include "xrfg/fps_overlay_model.hpp"
#include "xrfg/d3d12_native_result.hpp"
#include "xrfg/overlay_menu_input.hpp"
#include "xrfg/interface_metrics.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <filesystem>
#include <array>
#include <memory>
#include <functional>
#include <optional>

namespace xrfg {
class SteamVrDelivery;

// Owns its VIEW space and FPS/menu color swapchains. Never modifies a game
// image, temporal history, game space, optical-flow job, or frame schedule.
class OpenXrFpsOverlay final {
public:
    using MenuInputSource = OverlayMenuInputFrame(*)(void*);
    using DisplayTimeConverter = bool(*)(void*, XrTime, std::int64_t&);
    OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
        PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
        ID3D12Device* device12, ID3D12CommandQueue* queue12,
        ID3D11Device* device11, const std::filesystem::path& ini,
        bool interactive_menu = false, MenuInputSource input_source = nullptr, void* input_user = nullptr,
        DisplayTimeConverter converter = nullptr, void* converter_user = nullptr);
    ~OpenXrFpsOverlay();
    // Called on the application's end-frame thread, not the presenter thread.
    // FPS uploads at 4 Hz; an open menu draws on application frames. No explicit
    // GPU fence wait, and every runtime image-wait timeout remains zero.
    void application_frame(const XrFrameEndInfo* info) noexcept;
    void set_afw_active(bool active) noexcept;
    void set_delivery_source(SteamVrDelivery* delivery) noexcept;
    // Installed only for the known SteamVR native-handle budget. Other runtimes
    // retain direct, separate UI surfaces. A busy inventory defers admission.
    using NativeSwapchainCount=std::function<std::optional<unsigned>()>;
    void set_native_swapchain_count_source(NativeSwapchainCount source);
    void set_runtime_acceptance_source(std::function<std::optional<float>(std::int64_t)> source, bool direct=false,
        std::function<DeliveryCadenceBatch(std::int64_t)> cadence={}) noexcept;
    using ObservedEndFrame=std::function<XrResult(const XrFrameEndInfo*,const std::shared_ptr<InterfaceDeliveryReceipt>&)>;
    struct PreparedEndFrame { XrResult result{XR_SUCCESS}; ObservedEndFrame submit; };
    using PrepareEndFrame=std::function<PreparedEndFrame()>;
    void set_private_delivery_end(ObservedEndFrame,PrepareEndFrame prepare={});
    void set_unique_content_only(bool enabled) noexcept;
    void set_luke_scene_measurement(bool enabled) noexcept;
    [[nodiscard]] XrResult end_frame(const XrFrameEndInfo* info, bool synthetic, bool new_content = true,
        std::span<const std::shared_ptr<D3D12NativeFrameResult>> native_results = {}, unsigned native_output = 0,
        InterfaceFrameStamp stamp = {});
    void reset_metrics() noexcept;
    void set_grip_spaces(XrSpace left, XrSpace right) noexcept;
    void set_display_period(std::int64_t period_ns) noexcept;
    // Terminal for this session. Keep resources alive until normal teardown;
    // stop adding the quad or uploading textures immediately.
    void suspend() noexcept;
    // Closes interactive input without retiring the FPS/session resources.
    void close_menu() noexcept;
    void set_session_focused(bool focused) noexcept;
    [[nodiscard]] FpsSnapshot metrics() const noexcept;
    // CPU ownership only; a busy UI returns no sample instead of stalling.
    [[nodiscard]] std::optional<std::array<XrSwapchain,3>> swapchain_handles() const noexcept;
    // Angular counter bounds for a diagnostic burned into the S projection.
    // Available without a spare runtime quad layer; Off/suspend disables it.
    [[nodiscard]] std::optional<OverlayPlacement> marker_placement() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xrfg
