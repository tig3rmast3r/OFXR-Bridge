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
};

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
    // Where the game's motion and depth rectangles started before the
    // snapshot cropped them: what tells eyes sharing one target apart.
    std::uint32_t source_motion_x{};
    std::uint32_t source_depth_x{};
    // R22: preceding opposite-eye evaluation in a shared stream. Attached only
    // to resolved frames, never to stored producer frames or recursively.
    std::shared_ptr<const DlssMotionVectorFrame> bridge;
    std::uint32_t raw_motion_native_span{1};
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
