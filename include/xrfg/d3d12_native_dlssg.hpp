#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace xrfg {

// Process-lifetime diagnostics for OFXR's own native FG handles, never the
// game's SR/RR features. No reset, resource ownership or NGX shutdown follows.
// Read at a quiescent/drained boundary when reconciling multiple fields.
struct D3D12NativeDlssGLifetime {
    std::uint64_t create_attempts{},create_successes{},create_failures{};
    // Every nonnull create result counts, including failure-returned handles.
    std::uint64_t created{},failed_create_handles{};
    std::uint64_t release_attempts{},released{},release_failures{},live{};
};
[[nodiscard]] D3D12NativeDlssGLifetime native_dlssg_lifetime_snapshot() noexcept;

// Native NGX integration. The caller drains the queue before destroying this
// object and owns the frame/guide leases until its completion fence passes.
class D3D12NativeDlssG final {
  public:
    struct Output {
        ID3D12Resource *image{};
        // The camera the output is shown from, per view, as the rotation
        // (x, y, z, w) that takes a ray of it into B's camera; the compose
        // samples NGX's frame along that ray, and A beyond B's view. None:
        // B's own camera.
        std::optional<std::array<std::array<float, 4>, 2>> camera_to_current{};
    };
    // Why the last record() returned S_FALSE.
    enum class Skip {
        none,
        guides,                  // missing, reset, discontinuous or invalid guides
        resizing,                // a resized eye waits for its previous submission
        multi_frame_unsupported, // more outputs than the adapter's feature generates
        feature_unavailable,     // NGX could not create the feature; retried later
        evaluate_failed,         // NGX rejected an evaluation; the history reseeds
    };
    D3D12NativeDlssG();
    ~D3D12NativeDlssG();
    // scale is the feature's resolution in percent of each eye's, 25 to 100;
    // XRFG_NATIVE_DLSSG_SCALE overrides it.
    HRESULT initialize(ID3D12Device *device, ID3D12CommandQueue *queue,
                       const D3D12_RESOURCE_DESC &source, DXGI_FORMAT view_format,
                       UINT scale = 100) noexcept;
    // S_FALSE means the pair was not generated and no output was written. The
    // list may still hold NGX history work, so call submitted() for any list
    // that is executed after record(), whatever it returned.
    HRESULT record(ID3D12GraphicsCommandList *list, UINT work_slot, ID3D12Resource *a,
                   ID3D12Resource *b, std::span<const D3D12ReprojectionView> a_views,
                   std::span<const D3D12ReprojectionView> b_views,
                   const DlssMotionVectorSet *a_guides, const DlssMotionVectorSet *b_guides,
                   std::span<const Output> outputs, D3D12_RESOURCE_STATES release_state) noexcept;
    void reset() noexcept;
    void submitted(ID3D12Fence *completion, std::uint64_t value, std::uint64_t decision_value=0) noexcept;
    [[nodiscard]] Skip last_skip() const noexcept;
    [[nodiscard]] std::shared_ptr<D3D12NativeFrameResult> last_result() const noexcept;
    // Eye evaluations that reseeded NGX's history with the aligned previous frame.
    [[nodiscard]] std::uint64_t reseeds() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Frames NGX can generate between two rendered frames on this device's
// adapter: 0 when native frame generation is unavailable, 1 for 2X only, and
// 2 or more when 3X is possible. The first call per adapter queries NGX.
[[nodiscard]] std::uint32_t native_dlssg_max_generated_frames(ID3D12Device *device) noexcept;

} // namespace xrfg
