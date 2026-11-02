#pragma once
#include "xrfg/luke_gta_headlock.hpp"

#include <d3d12.h>
#include <wrl/client.h>
#include "dlss_snapshot_retention.hpp"

#include <cstdint>
#include <array>
#include <memory>
#include <optional>

namespace xrfg {

struct DlssMotionVectorPublication {
    std::uint64_t stream{};
    ID3D12Resource* output{};
    ID3D12Resource* motion_vectors{};
    ID3D12CommandQueue* producer_queue{};
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    D3D12_RESOURCE_STATES resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bool jittered{};
    bool reset{};
    ID3D12GraphicsCommandList* producer_command_list{};
    // Optional frame-generation guides. The established DLSS-vector route
    // remains valid when these fields are absent.
    ID3D12Resource* depth{};
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    D3D12_RESOURCE_STATES depth_resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    bool depth_inverted{};
    bool depth_infinite{};
    std::array<float, 3> camera_position{};
    std::array<float, 3> camera_up{};
    std::array<float, 3> camera_right{};
    std::array<float, 3> camera_forward{};
    float camera_fov_vertical{};
    bool camera_data_valid{};
    std::array<float, 16> camera_view_to_clip{};
    std::array<float, 16> camera_clip_to_view{};
    bool camera_matrices_valid{};
    // Same-frame producer-declared raw motion camera/history. In-process only.
    int raw_camera_motion_included{-1};
    std::array<float, 16> raw_clip_to_previous_clip{}, raw_previous_clip_to_clip{};
    D3D12_RESOURCE_STATES output_resource_state{
        D3D12_RESOURCE_STATE_COMMON};
    bool snapshot_output{};
    // F10-only pre-DLSS colour audit. Never retained by the guide history.
    ID3D12Resource* input_color{};
    std::uint32_t input_color_x{}, input_color_y{};
    std::uint32_t input_color_width{}, input_color_height{};
    D3D12_RESOURCE_STATES input_color_resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    // An in-process producer may supply the native D3D12 device after it has
    // unwrapped and identity-checked every object in the publication. This is
    // intentionally absent from the external provider ABI.
    ID3D12Device* verified_producer_device{};
    bool protect_snapshot_lifetime{};
    // Exact native D3D11 receipts pin snapshots without Luke color tokens,
    // AER camera calibration or the long color-origin history.
    bool direct_output_receipt{};
    std::uint32_t camera_frame_token{};
    bool camera_frame_token_valid{};
    // KCD scene-camera tokens identify read attempts, including extra failed
    // probes. Its temporal identity instead comes from the exact observed NGX
    // SR/RR production; failed productions still consume this sequence.
    std::uint64_t native_production_sequence{};
    // In-process receipt:1=current->N-1,2=current->same-eye N-2.
    // Append fields so existing aggregate publishers retain their contract.
    std::uint32_t raw_motion_native_span{1};
    // In-process NGX SR/RR receipt for Dead Space's RG16 typeless allocation.
    // DLSS consumes float XY; only our existing owned motion copy is typed.
    // Generic/external publishers keep the original resource format.
    bool dlss_float_motion{};
    // Original one-step GTA inputs need both immutable object-motion intervals.
    bool compose_native_object_history{};
    std::optional<LukeGtaHeadlock> gta_headlock;
    // The camera arrives at submission (attach_dlss_motion_vector_camera):
    // REAL VR fills its DLSS-fix constants when the list executes, after the
    // evaluation that publishes this guide. Calibration waits for it.
    bool camera_pending{};
};

// A camera attached to a published frame at its submission.
struct DlssCameraAttachment {
    std::array<float, 3> position{}, right{}, up{}, forward{};
    std::array<float, 16> view_to_clip{}, clip_to_view{};
    float near_z{}, far_z{}, fov_vertical{};
    std::uint64_t world{}; // see DlssMotionVectorFrame::camera_world
};
// Fills the camera of the frame `publication` of `stream`, published with
// camera_pending, and runs what its publication deferred. False when the
// frame is gone, already has a camera, or a later frame already read it.
bool attach_dlss_motion_vector_camera(std::uint64_t stream, std::uint64_t publication,
    const DlssCameraAttachment& camera) noexcept;

// Scalar native camera history: no retained older GPU resources or SL metadata.
struct DlssNativeCameraHistory {
    std::uint64_t stream{},epoch{},serial{},publication{};
    std::uint32_t token{},motion_width{},motion_height{},output_width{},output_height{};
    bool valid{},reset{},jittered{};
    float jitter_x{},jitter_y{};
    std::array<float,3> position{},right{},up{},forward{};
    std::array<float,16> projection{},inverse_projection{};
    std::uint64_t native_production_sequence{};
    std::uint32_t raw_motion_native_span{1};
};
struct DlssMotionVectorFrame {
    std::uint64_t stream{};
    std::uint64_t epoch{};
    std::uint64_t serial{};
    std::uint64_t previous_serial{};
    std::uint64_t publication{};
    // Process-monotonic key carried by protected Luke SR color. Zero is invalid;
    // it is never a truncated publication and never recycled by a stream reset.
    std::uint32_t color_origin_token{};
    Microsoft::WRL::ComPtr<ID3D12Resource> output;
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_vectors;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> producer_queue;
    Microsoft::WRL::ComPtr<ID3D12Device> verified_producer_device;
    // Device interfaces actually used to allocate these owned snapshots.
    // Receipts come from the resource slot, not current producer metadata.
    Microsoft::WRL::ComPtr<ID3D12Device> motion_snapshot_device;
    Microsoft::WRL::ComPtr<ID3D12Device> depth_snapshot_device;
    std::uint32_t output_x{};
    std::uint32_t output_y{};
    std::uint32_t output_width{};
    std::uint32_t output_height{};
    std::uint32_t output_slice{};
    std::uint32_t motion_x{};
    std::uint32_t motion_y{};
    std::uint32_t motion_width{};
    std::uint32_t motion_height{};
    std::uint32_t motion_slice{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    float previous_jitter_x{};
    float previous_jitter_y{};
    D3D12_RESOURCE_STATES resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    bool jittered{};
    bool reset{};
    Microsoft::WRL::ComPtr<ID3D12Resource> depth;
    std::uint32_t depth_x{};
    std::uint32_t depth_y{};
    std::uint32_t depth_width{};
    std::uint32_t depth_height{};
    D3D12_RESOURCE_STATES depth_resource_state{
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    bool depth_inverted{};
    bool depth_infinite{};
    std::array<float, 3> camera_position{};
    std::array<float, 3> camera_up{};
    std::array<float, 3> camera_right{};
    std::array<float, 3> camera_forward{};
    float camera_fov_vertical{};
    bool camera_data_valid{};
    std::array<float, 16> camera_view_to_clip{};
    std::array<float, 16> camera_clip_to_view{};
    bool camera_matrices_valid{};
    int raw_camera_motion_included{-1};
    std::array<float, 16> raw_clip_to_previous_clip{}, raw_previous_clip_to_clip{};
    // The snapshots are recorded into the producer command list.  The real
    // submission callback replaces the provisional queue with the queue that
    // actually executed that list and attaches a fence signalled immediately
    // after it.  This makes guide ownership safe across distinct D3D12 queues.
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> producer_command_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> producer_fence;
    Microsoft::WRL::ComPtr<ID3D12Device> producer_fence_device;
    std::uint64_t producer_fence_value{};
    // Exact preceding opposite-eye publication; its owned motion field feeds
    // stereo-depth consistency, independently of successful AFW color history.
    std::shared_ptr<const DlssMotionVectorFrame> previous_opposite_guide;
    std::optional<DlssNativeCameraHistory> previous_same_native_camera;
    // Exact native N-2 same-eye camera. Resolver results retain plain producer
    // frames, never recursive history chains; no additional GPU copy is needed.
    std::shared_ptr<const DlssMotionVectorFrame> previous_same_guide;
    // Resolver-proven chronological A/B/A cameras; original stream/epoch/
    // serial identities remain intact, never rewritten as a virtual feature.
    bool luke_interleaved_history{};
    bool compose_native_object_history{};
    std::optional<LukeGtaHeadlock> gta_headlock;
    // Calibrated same-time eye separation in the producer camera units.
    float calibrated_game_eye_baseline{};
    std::shared_ptr<DlssSnapshotRetention> motion_retention;
    std::shared_ptr<DlssSnapshotRetention> depth_retention;
    std::uint32_t camera_frame_token{};
    bool camera_frame_token_valid{};
    std::uint64_t native_production_sequence{};
    bool camera_pending{}; // see DlssMotionVectorPublication::camera_pending
    // Nonzero: the camera source places every frame in this one world,
    // whichever NGX handle produced it (REAL VR's chained DLSS-fix steps;
    // Uncharted switches between one handle and one per eye every second
    // or so). AFW keys its origin and calibration on it instead.
    std::uint64_t camera_world{};
    // Where the game's motion and depth rectangles started before the
    // snapshot cropped them: what tells eyes sharing one target apart.
    std::uint32_t source_motion_x{};
    std::uint32_t source_depth_x{};
    // R22: preceding opposite-eye evaluation in a shared stream. Attached only
    // to resolved frames, never to stored producer frames or recursively.
    std::shared_ptr<const DlssMotionVectorFrame> bridge;
    std::uint32_t raw_motion_native_span{1};
    // A guide placed into an eye's view by its camera (place_guide_in_view):
    // the output rectangle is then the eye's image rectangle, and
    // placed_motion_rect (x, y, width, height in vector texels, not whole)
    // the part of the vectors that lands on it. MSFS 2024's foveated
    // periphery spreads a few hundred render columns over a whole eye.
    bool placed{};
    std::array<float, 4> placed_motion_rect{};
};

// The vector and depth rectangles a consumer maps the guide's output onto:
// the placed part for a guide placed by its camera, the whole otherwise.
// Depth covers the same field as the vectors, at its own resolution.
[[nodiscard]] inline std::array<float, 4> guide_motion_rect(const DlssMotionVectorFrame& f) noexcept {
    if (f.placed) return f.placed_motion_rect;
    return {static_cast<float>(f.motion_x), static_cast<float>(f.motion_y),
        static_cast<float>(f.motion_width), static_cast<float>(f.motion_height)};
}
[[nodiscard]] inline std::array<float, 4> guide_depth_rect(const DlssMotionVectorFrame& f) noexcept {
    if (!f.placed || !f.motion_width || !f.motion_height)
        return {static_cast<float>(f.depth_x), static_cast<float>(f.depth_y),
            static_cast<float>(f.depth_width), static_cast<float>(f.depth_height)};
    const float sx = static_cast<float>(f.depth_width) / static_cast<float>(f.motion_width);
    const float sy = static_cast<float>(f.depth_height) / static_cast<float>(f.motion_height);
    const auto& m = f.placed_motion_rect;
    return {static_cast<float>(f.depth_x) + (m[0] - static_cast<float>(f.motion_x)) * sx,
        static_cast<float>(f.depth_y) + (m[1] - static_cast<float>(f.motion_y)) * sy,
        m[2] * sx, m[3] * sy};
}

// The field a DLSS evaluation rendered, from its camera's projection
// (Streamline's cameraViewToClip): the tangents of its image's edges - left
// and right where x runs from the image's left edge to its right, up and
// down at its top and bottom row - and where the camera sits. Not known
// without valid camera matrices or for a projection it cannot read.
struct DlssViewGeometry {
    bool known{};
    float tan_left{};
    float tan_right{};
    float tan_up{};
    float tan_down{};
    std::array<float, 3> position{};
    std::array<float, 3> right{};
};
[[nodiscard]] DlssViewGeometry dlss_view_geometry(const DlssMotionVectorFrame& frame) noexcept;

// An eye's view in its XR image: the tangents of its field (XrFovf) and its
// image rectangle.
struct DlssEyeView {
    float tan_left{};
    float tan_right{};
    float tan_up{};
    float tan_down{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

inline DlssNativeCameraHistory dlss_native_camera_history(const DlssMotionVectorFrame& f) noexcept {
    return {f.stream,f.epoch,f.serial,f.publication,f.camera_frame_token,
        f.motion_width,f.motion_height,f.output_width,f.output_height,
        f.camera_frame_token_valid&&f.camera_data_valid&&f.camera_matrices_valid,
        f.reset,f.jittered,f.jitter_x,f.jitter_y,f.camera_position,f.camera_right,
        f.camera_up,f.camera_forward,f.camera_view_to_clip,f.camera_clip_to_view,
        f.native_production_sequence,f.raw_motion_native_span};
}

constexpr std::uint32_t kDlssMotionVectorEyeCount = 2;

struct DlssMotionVectorSet {
    std::array<std::shared_ptr<const DlssMotionVectorFrame>,
               kDlssMotionVectorEyeCount>
        eyes;
    std::uint32_t eye_count{};
};

enum class DlssMotionVectorStatus : std::uint32_t {
    disabled,
    waiting_for_dlss,
    output_not_direct,
    queue_mismatch,
    invalid_input,
    temporal_mismatch,
    used,
    waiting_for_depth,
    native_unavailable,
    multi_frame_unsupported,
    native_evaluate_failed,
    native_resizing,
};

struct DlssMotionVectorStatistics {
    std::uint64_t published{};
    std::uint64_t depth_publications{};
    std::uint64_t invalid_publications{};
    std::uint64_t snapshot_copies{};
    std::uint64_t snapshot_failures{};
    std::uint64_t resolve_calls{};
    std::uint64_t resolve_missing_streams{};
    std::uint64_t resolve_stale_pairs{};
    std::uint64_t resolve_queue_mismatches{};
    std::uint64_t matched{};
    std::uint64_t temporal_rejections{};
    std::uint64_t invalid_rejections{};
    std::uint64_t used{};
    std::uint64_t last_used_publication{};
    DlssMotionVectorStatus status{DlssMotionVectorStatus::waiting_for_dlss};
};

// Returns this exact immutable publication, never a later stream head. The
// native Luke producer carries its process-monotonic token through rendered color.
std::shared_ptr<const DlssMotionVectorFrame> publish_dlss_motion_vectors(
    const DlssMotionVectorPublication& publication) noexcept;
void configure_dlss_motion_vector_tracking(bool enabled) noexcept;
// Request after the layer has drained its consumers on disable. Nonblocking:
// exact unsubmitted/in-flight producer copies and retained GPU readers remain
// protected. Repeat at a bounded cadence until the final writer receipts finish.
// Also collects already retired streams after a rapid re-enable. An empty
// retirement queue uses an atomic fast path and takes no registry lock.
void retire_disabled_dlss_motion_vector_resources() noexcept;
void retire_dlss_motion_vector_stream(std::uint64_t stream) noexcept;
// Failed native SR invalidates temporal joins immediately while preserving the
// owned slot allocations and in-flight GPU snapshot lifetimes.
void invalidate_dlss_motion_vector_stream_history(std::uint64_t stream) noexcept;
// Mark an exact original Execute attempt BEFORE forwarding it. No GPU receipt
// or guide admission is granted here; it only prevents a reentrant successful
// Reset inside a proxy Execute from canceling potentially submitted copies.
// Uses an atomic-only fast path when there are no cancelable recordings.
void begin_dlss_motion_vector_submission(std::uint32_t,ID3D12CommandList* const*) noexcept;
// Original Execute threw before its post-call receipt. Keep resources in an
// unknown-writer quarantine; a later reuse of the same COM list is not proof
// that the abandoned attempt completed.
void abandon_dlss_motion_vector_submission(std::uint32_t,ID3D12CommandList* const*) noexcept;
// Native hooks pass began_before_execute=true to consume only their exact
// pre-call markers. Legacy post-only callers are marked without allocation
// here before receipt bookkeeping; they do not admit already quarantined slots.
void report_dlss_motion_vector_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t command_list_count,
    ID3D12CommandList* const* command_lists,
    bool began_before_execute=false) noexcept;
// Call only AFTER the original GraphicsCommandList::Reset succeeds. Cancels
// copies recorded on that exact list which have never actually executed.
// Submitted GPU work and unknown completion receipts remain quarantined.
// Invalidates canceled active guide history; the next publication primes a
// fresh epoch. With no cancelable copies, this is an atomic-only fast path.
void report_dlss_motion_vector_reset(ID3D12GraphicsCommandList*) noexcept;
// Strict FSR joins never infer semantic eye identity from discovery order.
// A learned stream/epoch is scalar metadata, not a retained GPU snapshot.
struct DlssReleasedEyeIdentity {
    bool strict{};
    std::uint64_t stream{}, epoch{};
    // The eye's view is one horizontal half of its image, so an evaluation the
    // image's size covers both eyes (No Man's Sky: one side-by-side DLSS pass,
    // each eye in the matching half of a swapchain of its own). Both eyes take
    // the same evaluation, never the alternating pair of one shared stream,
    // each its own half of it: vectors and depth from the eye's half of the
    // evaluation, the output placed on the view, view_half (0 left, 1 right)
    // of the image.
    bool both_eyes{};
    std::uint32_t view_half{};
};
std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    ID3D12Resource* output,
    ID3D12CommandQueue* consumer_queue,
    int eye = -1,
    std::uint64_t released_at = 0,
    std::uint64_t other_released_at = 0,
    DlssReleasedEyeIdentity identity = {}) noexcept;
// Process-monotonic publication watermark, unaffected by tracking toggles.
// Nonzero release watermarks select exact released snapshots. Strict ordinary
// D3D12 FSR joins are opt-in; existing OFXR/DLSS-G and AFW contracts are preserved.
std::uint64_t dlss_motion_vector_publications() noexcept;
// A guide placed into an eye's view by its camera: a copy whose output
// rectangle is the view's rectangle and whose placed_motion_rect is the part
// of its vectors (and depth) that lands there. Null unless the guide's
// camera covers the view, to within a hundredth of its field.
std::shared_ptr<const DlssMotionVectorFrame> place_guide_in_view(
    const DlssMotionVectorFrame& frame,
    const DlssEyeView& view) noexcept;
// Both eyes' guides from evaluations whose cameras are known, for a game
// whose DLSS outputs are views of their own rather than its eye images -
// MSFS 2024's foveated rendering, four a frame: per eye a periphery over the
// whole field at a fraction of its pixels, and an inset. Each eye takes the
// recent stream whose camera covers its field and fits it best (the
// cameras' positions decide between two that fit alike), placed scaled; the
// evaluation is the stream's newest by the eye's release watermark. Both
// eyes or neither, from two streams. Ported from 0.2's V515.
std::array<std::shared_ptr<const DlssMotionVectorSet>, kDlssMotionVectorEyeCount>
resolve_view_placed_dlss_guides(
    ID3D12CommandQueue* consumer_queue,
    const std::array<DlssEyeView, kDlssMotionVectorEyeCount>& eyes,
    const std::array<std::uint64_t, kDlssMotionVectorEyeCount>& released_at) noexcept;
// Both eyes' guides from two recent streams whose outputs are each eye's
// view enlarged: smaller than the view, of its shape to within 1%, at most
// three times smaller (0.2's V537). Cheeky Foveated DLSS's foveated
// rendering evaluates per eye a periphery over the whole field at a fraction
// of the eye's pixels and an inset, with no camera to place them by. The
// periphery is placed over the whole view; the eye that evaluated first is
// the left, unless the inputs sit apart. Both eyes or neither.
std::array<std::shared_ptr<const DlssMotionVectorSet>, kDlssMotionVectorEyeCount>
resolve_enlarged_dlss_guides(
    ID3D12CommandQueue* consumer_queue,
    const std::array<DlssEyeView, kDlssMotionVectorEyeCount>& eyes,
    std::uint64_t released_at) noexcept;
// Diagnostics only: the live DLSS streams as a released image's guide
// resolution saw them, for an eye image that got none. presenter_transition
// 756, then 757 per stream (at most eight). Changes nothing.
void report_dlss_guide_census(ID3D12CommandQueue* consumer_queue,
    std::uint32_t image_width, std::uint32_t image_height, std::uint64_t detail) noexcept;
// Resolves by logical projection-view count when the OpenXR swapchain is
// D3D11 and therefore has no D3D12 image from which to derive array size.
std::shared_ptr<const DlssMotionVectorSet> resolve_dlss_motion_vectors(
    std::uint32_t logical_eye_count,
    ID3D12CommandQueue* consumer_queue) noexcept;
// Luke/AER's one NGX stream alternates physical-eye publications. Return the
// consecutive owned snapshots in publication order, without the legacy
// even-serial closing rule. Selection does not depend on OFXR activation.
std::shared_ptr<const DlssMotionVectorSet> resolve_luke_alternating_guides(
    ID3D12CommandQueue* consumer_queue) noexcept;
// Resolves only the token recovered from Luke's actual source color, using the
// immutable retained guide's full publication/stream/epoch metadata.
// The returned frame pins its exact N-1 opposite-eye guide and, when retained,
// its native N-2 same-eye camera independently of successful AFW evaluations.
// Unknown, evicted, or reset-epoch keys never fall back to the newest guide.
std::shared_ptr<const DlssMotionVectorFrame> resolve_luke_guide_token(
    std::uint32_t color_origin_token,
    ID3D12CommandQueue* consumer_queue,
    ID3D12Device* canonical_consumer_device) noexcept;
// Control-only reset preflight. These metadata queries do NOT return a guide,
// attach old history or authorize AFW generation. Tokens must still be decoded.
struct LukeNativeOriginMetadata {
    std::uint64_t stream{},epoch{},publication{},serial{};
};
bool luke_native_stream_reset(std::uint64_t stream,std::uint64_t accepted_epoch,
    ID3D12Device* device) noexcept;
std::optional<LukeNativeOriginMetadata> luke_native_origin_metadata(
    std::uint32_t token,ID3D12Device* device) noexcept;
void report_dlss_motion_vector_status(DlssMotionVectorStatus status) noexcept;
void report_dlss_motion_vector_use() noexcept;
DlssMotionVectorStatistics dlss_motion_vector_statistics() noexcept;

}  // namespace xrfg
