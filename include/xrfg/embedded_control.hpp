#pragma once
#include <cstdint>
#include <atomic>

namespace xrfg::embedded {
// Set by successful OpenXR negotiation; shared by layer and host publication.
void set_layer_active(bool active) noexcept;
bool layer_active() noexcept;
// CPU-only interface: no NGX/FFX structures cross the host/backend ABI boundary.
struct Settings {
    bool enabled{true};
    bool increase_buffer{};
    int pacing_mode{2}; // 0 framework, 1 sole director, 2 framework + director
    int backend{}; // 0 FidelityFX, 1 NVIDIA legacy, 2 NVIDIA new, 3 NVIDIA djules
    int preset{1}; // 0 slow, 1 medium, 2 fast
    int scale{2}; // 0 full, 1 three-quarter, 2 half, 3 quarter
    bool backward{};
    int separate_synthesis{}; // private output textures, independent of XR ring count
    int lower_vram{}; // checkbox: 0 off (default), 1 on
    int motion_vectors{}; // 0 optical flow, 1 DLSS ingress
    int frame_generation{}; // 0 OFXR synthesis, 1 FSR 3, 2 native DLSS-G
    unsigned native_scale{100};
    bool hybrid{};
    int rife_multiplier{2};
    int rife_resolution{18}; // Medium; RIFE-only motion inference preset
    int rife_model{-1}; // -1 ordinary routes; otherwise portable RIFE model
    bool operator==(const Settings&) const = default;
};
struct Snapshot {
    Settings desired{};
    std::uint64_t revision{1};
    unsigned sessions{}, pending{}, bypass{}, ready{}, errors{};
    long last_error{};
};

// Laboratory-only controls for the embedded Luke Ross / PureDark AFW path.
// This has an independent revision so changing a warp interpretation never
// drains or rebuilds the ordinary OFXR synthesis backend.
struct AfwSettings {
    int mode{3}; // 0 off, 1 alternate eye, 2 previous frame, 3 combined
    bool debug{};
    bool clear_before_warping{};
    float ignore_motion_threshold{2.5F};
    int shading_rate{}; // beta6 Combined: 0=1x1, 1=1x2, ... 8=4x4
    int depth_dilate_mode{}; // 0 off, 1 horizontal 1x3, 2 square 3x3
    int osr{2}; // 2 OSR (default), 0 AFW (legacy); retired value 1 migrates to OSR
    int osr_cover{};  // OSR: band cover in depth texels beyond the hole on the background side, 0..8
    int osr_trim{};   // OSR: depth texels of the object's smeared edge replaced too, 0..2
    int osr_fill{2};  // OSR: band fill gain on (1 - t), 1..8; 1 = history model only
    bool operator==(const AfwSettings&) const = default;
};

// PureDark encodes axis factors (1,2,4), not D3D12_SHADING_RATE.
constexpr std::uint32_t afw_shading_rate(int index) noexcept {
    constexpr std::uint32_t rates[]{9,10,12,17,18,20,33,34,36};
    return rates[index >= 0 && index < 9 ? index : 0];
}

struct AfwSnapshot {
    bool flat_scene{};
    AfwSettings desired{};
    int active_shading_rate{-1};
    std::uint64_t revision{1};
    std::uint64_t projection_frames{};
    std::uint64_t ready_frames{};
    std::uint64_t missing_capture_frames{};
    std::uint64_t busy_frames{};
    std::uint64_t failed_frames{};
    std::uint64_t evaluations{};
    std::uint64_t successful_evaluations{};
    std::uint64_t last_success_ms{};
    std::uint64_t ingress_frames{};
    std::uint64_t published_ingress_frames{};
    std::uint64_t missing_camera_ingress_frames{};
    std::uint64_t rejected_guide_pairs{};
    std::uint32_t last_guide_rejection{};
    std::uint64_t last_publication{};
    std::uint32_t last_source_eye{};
    std::uint32_t last_motion_width{};
    std::uint32_t last_motion_height{};
    std::uint32_t last_output_width{};
    std::uint32_t last_output_height{};
    float last_motion_scale_x{};
    float last_motion_scale_y{};
    long last_result{};
};

Snapshot snapshot();
bool request(Settings settings); // persists only owned keys; false means INI write failed
AfwSnapshot afw_snapshot();
bool request_afw(AfwSettings settings);
inline std::atomic_int active_afw_shading_rate{-1};
inline void report_afw_shading_rate(int index) noexcept {
    active_afw_shading_rate.store(index,std::memory_order_relaxed);
}
bool toggle_afw_debug();
void report_afw_prepare(std::int64_t reason);
void report_afw_ingress(bool published, bool camera);
void report_afw_guide_rejection(std::uint32_t mask);
void report_afw_evaluation(std::uint64_t publication,
    std::uint32_t source_eye, std::uint32_t motion_width,
    std::uint32_t motion_height, std::uint32_t output_width,
    std::uint32_t output_height, float motion_scale_x,
    float motion_scale_y, long result);
std::uint64_t attach();
void detach(std::uint64_t id);
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error);
bool logging_setting();
bool set_logging_setting(bool enabled); // next process: logger is initialized once
bool cadence_setting();
bool set_cadence_setting(bool enabled);
bool marker_setting();
bool set_marker_setting(bool enabled);
int overlay_setting();
bool set_overlay_setting(int position); // existing overlay polls its INI live

// The imported DJ deep policy is session-bound, as in the upstream tray.
// Editing it saves the next session's preference, without a renderer revision.
struct DjulesPipelineSnapshot {
    bool prefer_fps{true};
    bool triple_frame_gen{};
    bool pending{};
};
DjulesPipelineSnapshot djules_pipeline_snapshot();
bool set_djules_prefer_fps(bool enabled);
bool set_djules_triple_frame_gen(bool enabled);
void register_djules_pipeline(std::uint64_t id, bool prefer_fps, bool triple = false);
}
