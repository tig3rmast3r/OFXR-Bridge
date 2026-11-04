#pragma once
#include "xrfg/flow_input_encoding.hpp"
#include "xrfg/d3d12_native_result.hpp"

#include "xrfg/d3d12_history.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/fps_overlay_model.hpp"
#include "xrfg/pose.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <wrl/client.h>

namespace xrfg {

// AFW can submit an additional application frame before DLSS publishes a new
// guide. This recoverable result tells the OpenXR layer to pass that frame
// through without retiring the last real FSR anchor.
inline constexpr HRESULT kD3D12FrameSynthesisStaleGuides =
    HRESULT_FROM_WIN32(ERROR_NO_DATA);

struct D3D12FieldOfView {
    float angle_left{};
    float angle_right{};
    float angle_up{};
    float angle_down{};
};

struct D3D12ImageRect {
    std::uint32_t offset_x{};
    std::uint32_t offset_y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

struct D3D12ReprojectionView {
    Pose pose{};
    D3D12FieldOfView fov{};
    // A zero-sized rectangle preserves the legacy full-resource contract for
    // direct synthesizer callers. The OpenXR layer always supplies the exact
    // submitted XrSwapchainSubImage rectangle.
    D3D12ImageRect image_rect{};
    // OpenXR callers provide the physical texture-array slice explicitly.
    // The sentinel preserves the legacy direct-call contract where one view
    // per array slice is supplied in slice order.
    std::uint32_t array_slice{
        std::numeric_limits<std::uint32_t>::max()};
    std::int64_t display_time{};
};

struct D3D12PresentationTransform {
    bool active{};
    std::uint32_t view_count{};
    std::array<std::array<float, 16>, 2> clip_positions{};
};

// Immutable upstream GPU receipts for this submission. Completion of these
// permits a CPU output wait without requiring the application's later tail,
// which can itself be queued behind this output's runtime handover.
struct D3D12SynthesisInputs {
    struct Dependency {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
    };
    std::array<Dependency,3> dependencies{}; // A, B (pairs only), XR acquire
    bool captured{};
    [[nodiscard]] bool ready() const noexcept {
        if (!captured || !dependencies[0].fence || !dependencies[0].value) return false;
        for (const auto& dependency : dependencies) {
            if (!dependency.value) continue;
            if (!dependency.fence) return false;
            const auto completed=dependency.fence->GetCompletedValue();
            if (completed==UINT64_MAX || completed<dependency.value) return false;
        }
        return true;
    }
};

struct D3D12FrameSynthesisTicket {
    D3D12SynthesisInputs inputs;
    std::shared_ptr<D3D12NativeFrameResult> native_result;
    std::uint64_t previous_serial{};
    std::uint64_t current_serial{};
    // The value the whole pair completes at, which on the deferred path is
    // what the held-back current copy will signal - so it is not signalled
    // until flush_current_copy submits that copy, a display period later.
    std::uint64_t fence_value{};
    // The value at which the synthetic's pixels exist, signalled as soon as
    // the synthesis work is queued. Anything waiting for the synthetic and
    // only the synthetic must use this: waiting on fence_value instead
    // blocks until a copy that has not been submitted yet completes.
    std::uint64_t synthetic_fence_value{};
    std::uint32_t work_slot{};
    std::uint32_t synthetic_destination_index{
        std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t current_destination_index{};
    // A pair's synthetics are in the target cameras submit_pair was given.
    // False where the pair was not generated but filled with the current
    // frame - a native DLSS FG pair NGX skipped - so its synthetics show B's
    // camera and go to the runtime with B's pose.
    bool synthetics_in_target_camera{};
};

// Completed direct-queue GPU intervals for one NVIDIA Optical Flow pair.
// Values are expressed in microseconds and become available asynchronously
// after the pair's completion fence has passed.
struct D3D12NvidiaGpuTiming {
    std::uint64_t previous_serial{};
    std::uint64_t current_serial{};
    std::uint64_t pack_microseconds{};
    std::uint64_t eye0_microseconds{};
    std::uint64_t eye1_microseconds{};
    std::uint64_t composition_microseconds{};
    std::uint64_t total_microseconds{};
    // The pair's whole GPU span, as QueryPerformanceCounter values. Zero
    // when the device offered no calibration.
    std::uint64_t gpu_begin_qpc{};
    std::uint64_t gpu_end_qpc{};
    std::uint32_t eye_count{};
};

enum class D3D12OpticalFlowBackend {
    fidelity_fx,
    nvidia,
};

enum class D3D12FrameGenerationMode {
    ofxr,
    fsr3,
    native_dlss,
};

// R15 is deliberately excluded from the owner's accepted FSR and AFW paths.
[[nodiscard]] constexpr bool use_release_bound_game_guides(
    D3D12FrameGenerationMode mode, bool game_vectors, bool afw) noexcept {
    return game_vectors && !afw &&
        (mode == D3D12FrameGenerationMode::ofxr ||
         mode == D3D12FrameGenerationMode::native_dlss);
}

enum class D3D12NvidiaPerformancePreset {
    slow,
    medium,
    fast,
};

// Ratio the optical-flow input is packed at. Shared by both backends: the
// FidelityFX path ran at full resolution and honoured no scale at all, while
// NVIDIA has defaulted to half per axis all along. One control now covers both.
enum class D3D12OpticalFlowInputScale {
    full,
    three_quarter,
    half,
    quarter, // optional quarter flow; also used by Hybrid
    sixth = 5, third = 6, // RIFE-only exact unit fractions
    fifth = 4, // RIFE motion-inference grid, 20% per axis
};

// The name the NVIDIA options and the tray settings already use.
using D3D12NvidiaInputScale = D3D12OpticalFlowInputScale;

struct D3D12NvidiaOpticalFlowOptions {
    D3D12NvidiaPerformancePreset preset{
        D3D12NvidiaPerformancePreset::medium};
    // Applies to whichever backend is running: the FidelityFX pack and flow
    // honour it too, so there is one input-resolution control rather than
    // one per backend.
    D3D12OpticalFlowInputScale input_scale{
        D3D12OpticalFlowInputScale::half};
    bool bidirectional{};
    bool grayscale_input{}; // Optional optimization; reference stays BGRA8.
    D3D12OpticalFlowInputEncoding input_encoding{
        D3D12OpticalFlowInputEncoding::reference_linear};
    bool pose_compensated_input{}; // Optional djules input/composition contract.
    std::uint32_t native_scale{100};
    bool hybrid{}; // explicit optional Hybrid route only
    int rife_model{-1}; // experimental Python/CUDA generator, no OFA/engine guides
    int rife_multiplier{2};
    int rife_resolution_percent{}; //0 legacy exact fraction; otherwise10..40
    bool rife_async{}; // dedicated producer/consumer queues only; owned FIFO work
};

// The fraction submit_pair actually generates at for the one it is given:
// extrapolating, clamped to 1-3; interpolating, as given inside 0.05-0.95
// and one half outside it, since a degenerate interval says the pairing is
// not in a steady cadence. A caller that derives a synthetic's pose from its
// fraction uses this, so the pose and the content agree.
[[nodiscard]] float usable_synthesis_fraction(float fraction, bool extrapolate) noexcept;

// Owns a rolling exclusive history lease. Each resource supports at most two
// bounded projection views. They ordinarily map one-to-one to at most two
// array slices, while a single slice may also contain two non-overlapping
// viewports for double-wide stereo. Each logical view is extracted into
// independent mono A/B/S resources. FidelityFX uses one persistent optical-flow
// context and temporal history per view; NVIDIA uses the same explicit A/B/S
// boundary with its serialized OFA adapter. submit_pair subtracts the
// image-space component generated by the OpenXR orientation/FOV mapping and
// renders residual scene motion in B's camera plane. Each synthetic is then
// shown from its own target camera: B's field of view and rectangle, with an
// orientation of its own - the head's at the instant the synthetic is shown,
// between A's and B's - so it can be submitted with that pose and the
// runtime has nothing left to turn back. A target equal to B leaves the
// synthesis exactly as it was. current remains a bit-exact B copy. Translation
// remains depth-unaware. A repeated capture ticket is a
// legal unchanged-resource input: each view reuses only its own retained mono
// image while camera metadata advances. Ordinary routes never wait on the CPU.
// The experimental portable RIFE route waits its own exact interop receipts on
// the synthesis thread; it does not wait a game queue tail or alter pacing.
// D29: another output from the same optical-flow pair and completion fence.
// Hybrid's depth search for thin near surfaces (0.2 V505/V555), live for
// every synthesizer's next pair. On by default (0.2's menu switch was off).
void set_hybrid_depth_search(bool enabled) noexcept;
[[nodiscard]] bool hybrid_depth_search_enabled() noexcept;

struct D3D12ExtraSynthetic {
    std::uint32_t destination_index{};
    float interpolation_fraction{0.5F};
    std::span<const D3D12ReprojectionView> target_views{};
    std::uint32_t third_destination_index{UINT32_MAX};
    float third_fraction{.75F};
    std::span<const D3D12ReprojectionView> third_views{};
};

class D3D12FrameSynthesizer final {
public:
    D3D12FrameSynthesizer() noexcept;
    ~D3D12FrameSynthesizer();

    D3D12FrameSynthesizer(const D3D12FrameSynthesizer&) = delete;
    D3D12FrameSynthesizer& operator=(const D3D12FrameSynthesizer&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        std::shared_ptr<D3D12SwapchainHistory> history,
        std::span<ID3D12Resource* const> current_destination_images,
        std::span<ID3D12Resource* const> synthetic_destination_images,
        DXGI_FORMAT view_format,
        // Native D3D12 OpenXR destinations use RENDER_TARGET. Shared D3D11
        // interop mirrors use COMMON at both API ownership boundaries.
        D3D12_RESOURCE_STATES release_state,
        D3D12OpticalFlowBackend backend =
            D3D12OpticalFlowBackend::fidelity_fx,
        D3D12NvidiaOpticalFlowOptions nvidia_options = {},
        bool enable_nvidia_gpu_timing = false,
        D3D12FrameGenerationMode generation_mode =
            D3D12FrameGenerationMode::ofxr,
        bool allow_luke_guide_device_alias = false,
        bool defer_flow_resources = false) noexcept;

    // Bit-exact transport of one immutable history capture. This does not
    // seed or dispatch any temporal/optical-flow backend. The history lease is
    // retired against this copy's completion fence, preserving the rolling anchor.
    // Luke's disjoint AFW scratch may preserve an existing raw-OFXR anchor
    // while checking guides. The default rejects a copy with an active anchor.
    [[nodiscard]] HRESULT submit_copy(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* ticket,
        bool preserve_temporal_anchor = false) noexcept;

    [[nodiscard]] HRESULT submit_prime(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* ticket,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors = {},
        D3D12PresentationTransform presentation_transform = {}) noexcept;

    // Read-only preflight after the prior worker completed. A forward capture
    // gap requires re-priming the entire stereo transaction, not one eye.
    [[nodiscard]] bool has_capture_gap(const D3D12HistoryCaptureTicket& current) const noexcept;
    [[nodiscard]] bool can_pair_fsr(std::shared_ptr<const DlssMotionVectorSet> current) const noexcept;

    // synthetic_target_views is the camera the synthetic is generated in, one
    // per current source view: the same field of view and image rectangle,
    // any orientation. Each output pixel's ray is turned into B's camera and
    // the synthesis is evaluated there, in the same pass and with one
    // resample; a ray that leaves B's view takes A's, turned into the target
    // camera, where A sees it - the far side of a head turn - and B's nearest
    // edge where neither does, so no pixel is left without content.
    [[nodiscard]] HRESULT submit_pair(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::span<const D3D12ReprojectionView> synthetic_target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* ticket,
        std::optional<OverlayPlacement> debug_marker = std::nullopt,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors = {},
        // Records the current output's copy but leaves it unsubmitted for
        // flush_current_copy to hand over once the synthetic frame has gone
        // to the runtime. Defaults off: a caller that never flushes would
        // publish a stale current frame.
        bool defer_current_copy = false,
        // Where the synthetic sits between the previous capture and the
        // current one, 0 at the previous and 1 at the current. Half is right
        // only when the application runs at exactly half the display rate,
        // because only then are the two captures two display periods apart
        // and the synthetic shown one period before the current frame.
        // Clamped internally; out-of-range values fall back to half.
        float interpolation_fraction = 0.5F,
        D3D12PresentationTransform presentation_transform = {},
        std::optional<D3D12ExtraSynthetic> extra_synthetic = std::nullopt,
        bool allow_synthetic_camera = false) noexcept;

    // Executes the current output's copy, which submit_pair records but
    // deliberately leaves unsubmitted. The synthetic frame is handed to the
    // runtime a display period before the current one, and a runtime waits
    // for the whole queue when it takes a frame, so a copy left queued ahead
    // of the synthetic makes the frame with the tighter deadline wait for a
    // full-resolution copy it never reads. The caller submits the synthetic
    // first and calls this immediately afterwards, leaving the copy a whole
    // period to finish before the current frame needs it.
    //
    // Safe to call when nothing is pending. Submission entry points flush
    // any copy still outstanding themselves, so a caller that never gets
    // here costs a frame of latency rather than correctness.
    // Submits the held-back copy. consumer_queue, when the synthesis runs on
    // a queue of its own, is made to wait for that copy here rather than at
    // the pair's submission: at submission the copy has not been queued yet,
    // so the wait would park the consumer for a whole display period.
    [[nodiscard]] HRESULT flush_current_copy(
        ID3D12CommandQueue* consumer_queue = nullptr) noexcept;

    // Relinquishes the retained rolling source against its last GPU-use fence.
    // This is nonblocking and is required before history invalidation.
    [[nodiscard]] HRESULT retire_previous() noexcept;

    // Called after the runtime has paced the next application frame but before
    // that frame is returned to the application. Gives the previous complete
    // bridge transaction a bounded opportunity to leave the shared graphics
    // queue before the game records and submits its next frame. A timeout is
    // transient ERROR_BUSY; submission remains nonblocking and cannot enqueue
    // another transaction while this fence is pending.
    [[nodiscard]] HRESULT wait_for_previous_submission(
        std::uint32_t timeout_milliseconds) noexcept;

    // Flushes any deferred current copy, then polls the last submission fence.
    // S_OK means another transaction may begin; ERROR_BUSY leaves the real
    // frame to the caller without blocking the application thread.
    [[nodiscard]] HRESULT previous_submission_available() noexcept;
    // Poll this owned output ticket, never the newest pair or a deferred copy.
    [[nodiscard]] HRESULT ticket_available(const D3D12FrameSynthesisTicket& ticket) noexcept;
    // Retain the immutable completion owner while this exact output is submitted.
    // Exact queue owning the submitted GPU receipts. In asynchronous RIFE this
    // differs from the initialization queue; useful for ownership diagnostics.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12CommandQueue> submission_queue() const noexcept;

    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D12Fence> submission_fence(
        const D3D12FrameSynthesisTicket& ticket) noexcept;


    [[nodiscard]] HRESULT wait_for_idle() noexcept;
    // Caller excludes presenter use and new history captures. Keeps XR destinations.
    [[nodiscard]] HRESULT reconfigure(D3D12OpticalFlowBackend backend,
        D3D12NvidiaOpticalFlowOptions options,
        D3D12FrameGenerationMode generation_mode =
            D3D12FrameGenerationMode::ofxr) noexcept;

    // Retire OFXR's optical provider after its exact GPU fences while keeping
    // the existing device, queue, history and XR destinations copy-ready.
    // Completed publication fence identities remain valid. Only a later real
    // prime/pair recreates flow resources; this never creates D11/XR images.
    // Returns S_FALSE for another generation mode; an already-cold OFXR
    // transport is unchanged. Caller excludes new captures/presenter use.
    [[nodiscard]] HRESULT suspend_flow_resources() noexcept;

    // Returns S_OK and consumes one completed NVIDIA timing record, S_FALSE
    // when none is ready, or an error for an invalid output pointer/readback.
    [[nodiscard]] HRESULT consume_nvidia_gpu_timing(
        D3D12NvidiaGpuTiming* timing) noexcept;

    // Makes another queue wait on this ticket's completion before its own
    // later work runs. Required when the synthesizer owns a queue of its own
    // and writes into swapchain images: an OpenXR D3D12 runtime synchronizes
    // those images against the queue the application supplied at session
    // create, and sees nothing submitted elsewhere. Joining the two queues
    // here is what makes the runtime's own ordering cover the synthesis. This
    // is a GPU-side wait queued on the target; it never blocks the CPU.
    [[nodiscard]] HRESULT synchronize_consumer_queue(
        ID3D12CommandQueue* queue,
        const D3D12FrameSynthesisTicket& ticket) noexcept;

    // The other half of that join, and the one a fence alone cannot be
    // assumed to cover. xrWaitSwapchainImage makes an image safe to write on
    // the queue the application supplied at session create; the runtime knows
    // nothing about a private synthesis queue and inserts no wait for it. So
    // the guarantee has to be carried across explicitly: the application's
    // queue signals once the acquire has returned, and the synthesis queue
    // waits on that before it writes. Without it the synthesis can land in an
    // image the compositor has not finished with, which shows up as a
    // synthetic that was generated, paced and complete and still never
    // reached the headset. Call once per pair, after the images are acquired
    // and before the pair is submitted.
    [[nodiscard]] HRESULT synchronize_producer_queue(
        ID3D12CommandQueue* queue) noexcept;

    [[nodiscard]] bool initialized() const noexcept;

    // Panel-only endpoint selection (djules75 V510). Remains off for eye images.
    void set_flat_panel(bool enabled) noexcept;

    // Failure-only observations; never signal, wait, flush or release GPU work.
    void report_pending_submission() const noexcept;

private:
    HRESULT ensure_flow_resources_locked();
    HRESULT replace_resources_locked(D3D12OpticalFlowBackend backend,
        D3D12NvidiaOpticalFlowOptions options,D3D12FrameGenerationMode mode,
        bool defer_flow,bool preserve_completed_tickets);
    struct Impl;

    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
    std::atomic_bool flat_panel_{false};
};

}  // namespace xrfg
