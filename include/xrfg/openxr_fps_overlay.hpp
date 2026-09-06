#pragma once

#include "xrfg/fps_overlay_model.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <filesystem>
#include <memory>

namespace xrfg {

// Owns only its VIEW space and small color swapchain. Never modifies a game
// image, temporal history, game space, optical-flow job, or frame schedule.
class OpenXrFpsOverlay final {
public:
    OpenXrFpsOverlay(XrInstance instance, XrSession session, XrSystemId system,
        PFN_xrGetInstanceProcAddr get_proc, PFN_xrEndFrame end_frame,
        ID3D12Device* device12, ID3D12CommandQueue* queue12,
        ID3D11Device* device11, const std::filesystem::path& ini);
    ~OpenXrFpsOverlay();
    // Called on the application's end-frame thread, not the presenter thread.
    // Uploads at most 4 Hz; no explicit GPU fence wait, image-wait timeout zero.
    void application_frame(const XrFrameEndInfo* info) noexcept;
    [[nodiscard]] XrResult end_frame(const XrFrameEndInfo* info, bool synthetic);
    void reset_metrics() noexcept;
    [[nodiscard]] FpsSnapshot metrics() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xrfg
