#pragma once

#include "d3d12_history.hpp"

#include <d3d12.h>

#include <cstdint>
#include <array>
#include <memory>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace xrfg {

// A single F8 press captures two completed AFW xrEndFrame
// intervals, or seals a partial capture after eight application intervals.
std::uint32_t two_frame_capture_begin_end_frame() noexcept;
void two_frame_capture_finish_end_frame(std::uint32_t frame) noexcept;
void two_frame_capture_mark_afw_evaluation() noexcept;
// Starts the bounded recorder immediately, only with F9 diagnostic logging.
// Caller handles episode recovery/cooldown; never injects a keyboard event.
[[nodiscard]] bool two_frame_capture_request_origin_loss() noexcept;
[[nodiscard]] bool two_frame_capture_active() noexcept;
// Full-resolution F8 session only; the rolling F9 ring has its own witnesses.
[[nodiscard]] bool two_frame_capture_images_active() noexcept;
// Full F8 only, one native pair per stream and at most two streams.
[[nodiscard]] bool two_frame_capture_claim_dlssg_pair(std::uint64_t stream) noexcept;
// At most one original image per application swapchain in a full F8 session.
[[nodiscard]] bool two_frame_capture_claim_application(std::uint64_t swapchain) noexcept;
void two_frame_capture_timeline(
    const char* stream,
    std::int64_t code,
    std::uint64_t serial,
    std::uint64_t display_time,
    std::uint64_t detail) noexcept;

struct TwoFrameImageInfo {
    const char* stage{};
    std::uint64_t stream{};
    std::uint64_t display_time{};
    std::uint64_t swapchain{};
    std::uint64_t capture_serial{};
    std::int32_t eye{-1};
    std::uint32_t slice{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
    float scale_x{1.0F};
    float scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
};

struct TwoFrameGuideContract {
    // Current / exact previous-opposite / exact previous-same producer receipt.
    // Diagnostic metadata only; original cameras, guides and XR poses unchanged.
    std::array<std::uint64_t,3> gta_tracking_render{};
    std::array<int,3> gta_tracking_mode{};
    std::array<float,3> gta_tracking_yaw{};
    std::array<bool,3> gta_tracking_valid{};
    std::array<std::array<float,28>,3> gta_tracking_poses{};
    std::uint32_t native_camera_token{}, color_origin_token{};
    std::uint64_t native_production_sequence{};
    bool interleaved_history{};
    std::uint32_t raw_motion_native_span{1};
    const char* stage{};
    std::uint64_t stream{};
    std::uint64_t publication{};
    std::uint64_t serial{};
    std::uint64_t previous_serial{};
    std::int32_t source_eye{-1};
    std::int32_t destination_eye{-1};
    std::int32_t mode{};
    std::int32_t motion_vectors_type{};
    float ignore_motion_threshold{}, camera_fov_vertical{};
    bool debug{}, clear_before_warping{}, is_hudless_color{};
    int depth_dilate_mode{};
    std::uint32_t motion_width{}, motion_height{};
    std::uint32_t depth_width{}, depth_height{};
    std::uint32_t output_width{}, output_height{};
    std::uint32_t input_color_width{}, input_color_height{};
    std::uint32_t input_depth_width{}, input_depth_height{};
    std::uint32_t input_motion_width{}, input_motion_height{};
    std::uint32_t output_color_width{}, output_color_height{};
    std::uint32_t depth_crop_x{}, depth_crop_y{};
    std::uint32_t motion_crop_x{}, motion_crop_y{};
    float scale_x{}, scale_y{};
    float jitter_x{}, jitter_y{};
    float motion_scale_x{}, motion_scale_y{};
    float frame_time_delta_ms{}, camera_near{}, camera_far{};
    bool reset{}, jittered{}, depth_inverted{}, depth_infinite{};
    std::uint64_t native_previous_publication{},native_same_publication{};
    std::array<float,16> native_clip_to_opposite{},native_clip_to_same{};
    float native_history_jitter_x{},native_history_jitter_y{};
    int raw_camera_motion_included{-1};
    std::array<float,16> raw_clip_to_previous_clip{}, raw_previous_clip_to_clip{};
    bool motion_temporal_valid{};
    std::uint64_t motion_previous_publication{}, motion_middle_publication{};
    std::array<float,16> motion_current_to_middle_same{}, motion_current_to_previous_same{},
        motion_current_to_middle_opposite{};
    bool depth_native_history_valid{};
    int camera_forward_sign{};
    int camera_up_sign{};
    std::uint64_t depth_native_previous_publication{};
    std::array<float,16> depth_current_to_native_previous{};
    std::array<float, 3> camera_position{}, camera_right{}, camera_up{},
        camera_forward{};
    std::array<float, 3> source_pose{}, destination_pose{};
    std::array<float, 4> source_orientation{}, destination_orientation{};
    std::array<float, 16> view_to_clip{}, clip_to_view{};
    std::array<float, 16> source_world_to_view{}, source_view_to_world{},
        source_view_to_clip{}, source_clip_to_view{},
        destination_world_to_view{}, destination_view_to_world{},
        destination_view_to_clip{}, destination_clip_to_view{},
        camera_world_to_view{}, camera_view_to_world{},
        camera_view_to_clip{}, camera_clip_to_view{};
};

void two_frame_capture_guide_contract(
    const TwoFrameGuideContract& info) noexcept;

// Exact values at the native NGX call, rather than reconstructed ingress data.
// Only the full F8 pair claimant records these; no always-on observation.
struct TwoFrameNativeDlssgEvaluation {
    std::uint32_t frame{}, index{}, count{}, flags{}, result{};
    std::uint64_t stream{}, publication{}, serial{}, previous_serial{};
    bool reset{}, direct{};
    std::uint64_t color{}, motion{}, depth{}, generated{}, disable{};
    std::uint32_t width{}, height{}, color_format{}, input_color_format{};
    std::array<std::uint32_t,4> backbuffer_rect{}, motion_rect{}, depth_rect{};
    std::array<float,2> motion_scale{}, jitter{};
    float depth_scale{}, near_far_partition{}, object_separation{}, invalid_motion{};
    float near_plane{}, far_plane{}, fov{}, aspect{};
    std::array<float,3> position{}, right{}, up{}, forward{};
    std::array<float,16> view_to_clip{}, clip_to_view{}, clip_to_lens{}, clip_to_previous{}, previous_to_clip{};
};
void two_frame_capture_native_dlssg_evaluation(const TwoFrameNativeDlssgEvaluation& info) noexcept;

void two_frame_capture_note(
    const TwoFrameImageInfo& info, HRESULT status) noexcept;

// Called on the producer command list while the source state is known.
void two_frame_capture_record_ng_x(
    const TwoFrameImageInfo& info,
    ID3D12Device* device,
    ID3D12GraphicsCommandList* list,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state) noexcept;

// Diagnostic copy on an already ordered queue. Retains its allocator/list
// through the readback fence and leaves the source in its original state.
[[nodiscard]] HRESULT two_frame_capture_record_queue(
    const TwoFrameImageInfo& info, ID3D12Device* device,
    ID3D12CommandQueue* queue, ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state) noexcept;

// F8 only, on the existing D11 owner/release thread. Copies the exact native
// application image to staging; the writer receives CPU bytes and never calls
// the single-threaded D11 context. D12 is used only to describe row footprints.
[[nodiscard]] HRESULT two_frame_capture_record_d3d11(
    const TwoFrameImageInfo& info, ID3D12Device* footprint_device,
    ID3D11DeviceContext* context, ID3D11Texture2D* source) noexcept;

// Called after the native ExecuteCommandLists hook, never before it.
void two_frame_capture_submission(
    ID3D12CommandQueue* queue,
    std::uint32_t count,
    ID3D12CommandList* const* lists) noexcept;

// Associates a release-time final-eye image with its xrEndFrame projection.
void two_frame_capture_annotate_projection(
    std::uint32_t frame,
    std::uint64_t swapchain,
    std::uint64_t capture_serial,
    std::uint64_t display_time,
    std::int32_t eye,
    std::uint32_t slice,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept;

}  // namespace xrfg
