#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <memory>

#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/uevr_afr_osr.hpp"

namespace xrfg {
// One reconstruction for UEVR's Alternating/AFR rendering: the eye UEVR just
// rendered (source) is turned into the other eye (destination) with Full OSR,
// from the DLSS depth and vectors of that frame and the destination eye's own
// last frame. Cameras are world cameras relative to the caller's origin.
struct UevrOsrFrame {
    const DlssMotionVectorFrame* guide{};
    unsigned source_eye{};
    UevrMatrix source_view_to_clip{}, destination_view_to_clip{};
    UevrMatrix source_view_to_world{}, destination_view_to_world{};
    // The source eye itself at the pose of this frame's display slot: the
    // real eye is relocated there too, so each frame's content is at one pose
    // (UEVR samples the head once per pair). Same projection as the source.
    UevrMatrix same_view_to_world{};
    bool relocate_same{};
    // The game camera each eye hangs on (uevr_engine_to_world), for what is
    // attached to it: the first-person weapon follows it in this eye's history.
    UevrMatrix source_engine_to_world{}, destination_engine_to_world{};
    bool engine_valid{};
    bool cameras_valid{};  // world cameras known: the history can be reprojected
    bool history_motion{}; // the vectors point at this eye's previous frame (UEVR Ghosting Fix)
    bool reset{};          // forget both eyes' history (origin moved, scene cut)
    int mode{3};           // AFW mode: 1 other eye only, 2/3 with the eye's own previous frame
    bool debug{};
    bool lower_quality{};  // OSR quality: lower (OsrFullWarp::Inputs::lower_quality)
    float display_period_ms{11.111F}; // carried by the guides as the frame time
    float units_per_metre{100.0F};
};

struct UevrOsrResult {
    HRESULT result{E_FAIL};
    bool history_used{};
    bool history_motion_used{};
    std::uint32_t reason{}; // nonzero: why the inputs were refused
    // Receipts in world units: the destination eye's offset from the source
    // eye along the source's right axis (negative when the left eye is
    // reconstructed from the right), and the destination camera's distance
    // from the camera of the history frame it was compared with (NaN: none).
    float baseline_right{};
    float history_distance{std::numeric_limits<float>::quiet_NaN()};
    bool same_rendered{}; // the source eye was relocated into same_destination
    bool lower_quality{}; // the other eye was resolved at the lower OSR quality
    // Frame generation guides for both eyes of this frame, by swapchain eye
    // index (uevr_osr_guide.hlsl): each eye's shown depth and its motion since
    // that eye's previous frame, one stream per eye with an unbroken serial
    // chain. Empty when the real eye was not relocated.
    std::array<std::shared_ptr<const DlssMotionVectorSet>, 2> guides{};
    // GPU time of the last completed capture and reconstruction, microseconds
    // (a few frames old: read when their command allocator is reused). 0: none.
    // [0] eye copy out of the swapchain, [1] depth/vector conversion, [2] other
    // eye OSR, [3] copy into its swapchain, [4] real-eye relocation + copy,
    // [5] history copy.
    std::array<std::uint32_t, 6> gpu_us{};
};

class UevrOsrRenderer {
public:
    UevrOsrRenderer() noexcept;
    ~UevrOsrRenderer();
    UevrOsrRenderer(const UevrOsrRenderer&) = delete;
    UevrOsrRenderer& operator=(const UevrOsrRenderer&) = delete;

    // Copies the eye image UEVR is releasing into private storage, on the
    // application's queue, while the application still owns the image.
    // `state` is the image's state at release (render target for OpenXR D3D12
    // colour images); it is restored.
    HRESULT capture_source(ID3D12Device* device, ID3D12CommandQueue* queue,
        ID3D12Resource* image, D3D12_RESOURCE_STATES state, unsigned eye) noexcept;

    // Reconstructs the destination eye from the last captured source eye and
    // writes it into `destination` (an acquired swapchain image in `state`).
    // same_destination (optional, in same_state) receives the source eye
    // relocated to frame.same_view_to_world when frame.relocate_same is set.
    UevrOsrResult render(ID3D12Device* device, ID3D12CommandQueue* queue,
        const UevrOsrFrame& frame, ID3D12Resource* destination,
        D3D12_RESOURCE_STATES state, ID3D12Resource* same_destination = nullptr,
        D3D12_RESOURCE_STATES same_state = D3D12_RESOURCE_STATE_RENDER_TARGET) noexcept;

    void reset_history() noexcept;

    // For F8 captures, after render(): the captured source eye and the
    // reconstruction, both in COMMON.
    [[nodiscard]] ID3D12Resource* source_image(unsigned eye) const noexcept;
    [[nodiscard]] ID3D12Resource* output() const noexcept;
    [[nodiscard]] ID3D12Resource* same_output() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace xrfg
