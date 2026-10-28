#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include "xrfg/interface_metrics.hpp"

namespace xrfg::autonomous {

// Each generation menu owns its preferences. Flat fields below are the active
// editing view; select_generation saves/restores that view without cross-talk.
struct GenerationPreferences {
    int motion_vectors{};
    int preset{1};
    int scale{2};
    bool backward{};
    int separate_synthesis{}; // legacy field, follows combined Lower VRAM
    int lower_vram{};
    bool prefer_fps{true};
    bool triple_frame_gen{};
    int native_scale{100};
    bool operator==(const GenerationPreferences&) const = default;
};
constexpr std::size_t kGenerationProfileCount = 7; // four OFXR engines, FSR, DLSS-G

// Plain CPU values shared between the standalone UI and the in-process layer.
// No application resource pointers cross this boundary.
struct Settings {
    bool enabled{true};
    bool advanced_mode{}; // INI-only AdvancedMode; simplified UI by default
    bool afw_only{}; // per-game startup choice, independent of Neural Rendering
    bool increase_buffer{}; // private Framework/Director2->3; no effect on native Legacy
    int pacing_mode{1}; // 0 framework, 1 sole director, 2 framework + director
    int backend{2}; // 0 FidelityFX, 1 NVIDIA reference, 2 NVIDIA new, 3 NVIDIA djules
    int motion_vectors{}; // 0 optical flow, 1 native game guides
    int frame_generation{}; // 0 OFXR, 1 FSR 3, 2 native DLSS Frame Generation, 3 Hybrid
    int hybrid_backend{}; // legacy profile key; Hybrid fallback is always FidelityFX
    int preset{1}; // 0 slow, 1 medium, 2 fast
    int scale{2}; // 0 full, 1 three-quarter, 2 half, 3 quarter
    bool backward{};
    int separate_synthesis{}; // legacy field, follows combined Lower VRAM
    int lower_vram{}; // checkbox: 0 off (default), 1 on
    bool prefer_fps{true};
    bool triple_frame_gen{};
    int native_scale{100};
    int afw_mode{3}; // 0 off, 1 alternate, 2 previous, 3 combined
    bool afw_debug{};
    bool afw_clear_before_warping{};
    float afw_ignore_motion_threshold{2.5F};
    int afw_depth_dilate_mode{};
    int afw_shading_rate{}; // beta6 Combined; 0 full resolution, 4=2x2, 8=4x4
    bool nr_enabled{};
    bool nr_before_sr{true};
    float nr_intensity{1.F};
    int nr_preset{};
    int nr_style{};
    float nr_local_structure{1.F};
    float nr_local_tone{1.F};
    float nr_skin_structure{-1.F};
    bool nr_auto_mask{true};
    float nr_transfer_strength{1.F};
    float nr_colour_strength{1.F};
    float nr_max_ratio{2.F};
    float nr_working_scale{1.F};
    float nr_white_point_scale{1.F};
    int nr_debug_view{};
    int nr_compare{};
    float nr_compare_split{.5F};
    float nr_compare_zoom{1.F};
    bool nr_compare_swap{};
    bool r19_camera{}; // opt-in own camera for every FG route; live pair snapshot
    bool diagnostics{};
    bool status_json{}; // explicit external telemetry/export request; per profile, default off
    bool benchmark_csv{true}; // automatic suite CSV; independent of the flight log
    bool benchmark_auto_pick{true}; // per-profile, apply a measured winner only on completion
    int overlay_position{6}; // 0 off, 1 upper-left, 2 upper-right, 3 lower-left, 4 lower-right, 5 Elliott controller, 6 Elliott bottom
    float menu_scale{1.F}; // physical headset interface multiplier, 0.5 to 1.25
    float menu_offset_x{}; // common-FOV centre offset, normalized -1 to 1
    float menu_offset_y{};
    std::array<GenerationPreferences, kGenerationProfileCount> generation_profiles{};
    bool profiles_initialized{}; // legacy flat profile migration
    bool luke_shared_swapchains{true}; // INI-only startup policy: share compatible final native outputs
    bool operator==(const Settings&) const = default;
};

enum class GenerationState : std::uint32_t {
    waiting, ready, paused, missing_motion, missing_depth, invalid_guides,
    invalid_stereo, recovering, busy, unavailable, runtime_missing,
    initialization_failed, dispatch_failed, temporal_mismatch, output_mismatch,
    queue_mismatch, multi_frame_unsupported, native_declined,
};
// User-facing wording shared by both settings surfaces and exercised by the
// control fixtures. State describes actual generation, not just applied settings.
std::string_view generation_status_text(int mode, int motion_source, GenerationState state) noexcept;

struct SwapchainBudgetStatus {
    std::uint64_t updated_ms{};
    std::uint32_t application{}, projection{}, auxiliary{}, other{}, depth{};
    std::uint32_t current{}, synthetic{}, auxiliary_outputs{}, private_snapshots{};
    std::uint32_t fps{}, meter{}, menu{}, refused{}, total{};
    bool fallback{}, details_truncated{};
    char details[1024]{}; // problem app handles/roles/dimensions; no stack walk
    std::uint32_t unassigned_native{}; // observed endpoint without an assigned owner
};
std::string_view swapchain_budget_warning(const SwapchainBudgetStatus& value) noexcept;

// Last existing DXGI read in the game process. UI never queries the adapter.
struct VideoMemoryStatus {
    std::uint64_t updated_ms{}, used_bytes{}, budget_bytes{};
};
inline constexpr int kAfwOnlyPacingMode=3; // native AFW terminal delivery, no OFXR transport
struct RuntimeStatus {
    std::uint32_t process_id{};
    int pacing_mode{-1}; // effective session transport, independent of requested mode
    bool steamvr_runtime{};
    int afw_shading_rate{-1}; // active after successful evaluation; desired can be pending
    std::uint64_t heartbeat_ms{};
    std::uint64_t applied_revision{};
    std::uint32_t sessions{};
    std::uint32_t ready{};
    std::uint32_t pending{};
    std::uint32_t errors{};
    std::uint64_t afw_ready_frames{};
    std::uint64_t afw_projection_frames{};
    std::uint64_t nr_evaluations{};
    bool nr_running{};
    bool logging_active{};
    bool vulkan_capture_suspended{};
    bool luke_detected{}, vulkan_detected{};
    bool afw_running{}; // actual recent AFW output; IPC V487 rejects older reporters
    std::uint32_t provider_error{};
    std::uint32_t overlay_limit_mask{}; // 1 FPS, 2 meter, 4 headset menu; last refusal until successful retry
    InterfaceTelemetry telemetry{};
    int generation_mode{}, motion_source{};
    GenerationState generation_state{GenerationState::waiting};
    std::uint64_t upscaler_evaluations{}, upscaler_motion_inputs{}, upscaler_depth_inputs{}, upscaler_last_seen_ms{};
    bool upscaler_motion{}, upscaler_depth{};
    std::uint64_t guide_publications{}, guide_matches{}, guide_uses{};
    char message[192]{};
    SwapchainBudgetStatus swapchains{};
    unsigned dlssg_preset_request{}; // frozen process-start profile, not active NGX model proof
    VideoMemoryStatus video_memory{};
    bool luke_shared_swapchains{}, luke_shared_swapchains_forced{}; // active session, not requested next launch
    bool vdxr_runtime{}, vdxr_legacy{true}; // active startup transport, not next-launch preference
    std::uint32_t buffer_capacity{}; // active admission capacity; zero for native Legacy/AFW-only
    std::uint32_t eye_long_side{}; // minimum per-eye longest side of the submitted stereo scene
    bool shared_queue{}; // actual capture/OFXR queue fallback, not the requested pacing
    bool afw_flat_scene{};
};
inline bool pacing_comparison_runtime(const RuntimeStatus& r) noexcept {
    return !r.vdxr_legacy;
}

enum class BenchmarkPhase : unsigned { idle, applying, running, complete, stopped, error, restart_required };
struct BenchmarkStatus {
    BenchmarkPhase phase{};
    bool full{}, csv{}, luke{}, advanced{}; // frozen suite scope
    bool auto_pick{}; // frozen at start, never applies on Stop/error
    unsigned index{}, count{}, game_pid{}, controller_pid{};
    std::uint64_t run_id{}, revision{}, requested_ms{}, started_ms{};
    char label[80]{}, message[192]{}, csv_path[1024]{};
};
inline bool benchmark_active(const BenchmarkStatus& b) noexcept {
    return b.phase==BenchmarkPhase::applying || b.phase==BenchmarkPhase::running;
}
struct Snapshot {
    Settings desired{};
    std::uint64_t revision{1};
    RuntimeStatus runtime{};
    bool connected{}; // runtime reported within the last five seconds
    BenchmarkStatus benchmark{};
};

// Once per trial, captured in the game process on its rendering adapter.
// Unknown/unsupported reads stay invalid, never zero-filled measurements.
struct BenchmarkMemory {
    std::uint64_t revision{}, updated_ms{}, vram_bytes{}, shared_gpu_bytes{}, ram_working_set_bytes{}, ram_private_commit_bytes{};
    bool vram_valid{}, shared_gpu_valid{}, ram_valid{};
};
std::uint64_t take_benchmark_memory_request() noexcept;
void report_benchmark_memory(BenchmarkMemory memory) noexcept;

// Persistent values are independent of the original OFXR/OptiScaler profile.
// Runtime processes and the tray keep the shared mapping open; named-mutex
// publication prevents tearing and a versioned ABI rejects incompatible peers.
Snapshot snapshot() noexcept;
bool request(Settings desired) noexcept;
// Publish only fields edited since base, preserving changes from another UI.
// The shared-mapping mutex acquisition never blocks the end-frame thread.
bool request_changes(const Settings& base, const Settings& desired) noexcept;
Settings merge_changes(Settings current, const Settings& base, const Settings& desired) noexcept;
void report_runtime(RuntimeStatus status) noexcept;
void report_video_memory(VideoMemoryStatus value) noexcept;
std::string interface_vram_text(const Snapshot& value);
bool interface_generation_active(const Snapshot& value) noexcept;
bool interface_afw_only_active(const Snapshot& value,std::uint64_t now_ms) noexcept;
enum class OverlaySwapchain : unsigned { fps=1, meter=2, menu=4 };
void report_overlay_limit(OverlaySwapchain surface, bool refused) noexcept;
std::string_view overlay_limit_text(std::uint32_t mask) noexcept;
// Visibility leases are independent of settings revisions and persistence.
enum class InterfaceSurface : unsigned { desktop, headset, meter, status_export };
void set_interface_visible(InterfaceSurface surface, bool visible) noexcept;
bool interface_telemetry_requested() noexcept;
void report_interface_telemetry(InterfaceTelemetry value) noexcept;
std::string interface_status_text(const Snapshot& value);
std::string_view interface_state_text(const Snapshot& value) noexcept;
std::string_view interface_vectors_text(const Snapshot& value, std::uint64_t now_ms) noexcept;
// Global settings are never redirected by the active game.
std::filesystem::path global_configuration_path();
std::filesystem::path common_folder();
std::filesystem::path game_configuration_path(const std::filesystem::path& executable);
// Active game profile in-process / while attached on desktop; base otherwise.
std::filesystem::path configuration_path();
// Called before any runtime settings or startup-only resources are read.
bool initialize_game_configuration(const std::filesystem::path& executable = {}) noexcept;
bool reset_to_defaults() noexcept;
// One user configuration, with fresh defaults and no legacy file import.
bool initialize_user_configuration() noexcept;
bool write_configuration_sections(const std::filesystem::path& path, std::string_view sections) noexcept;
bool benchmark_enabled() noexcept; // controller/UI only; never called by frame sampling

constexpr int generation_ui_index(int mode) noexcept { return mode==3 ? 1 : mode==1 ? 2 : mode==2 ? 3 : 0; }
constexpr int generation_from_ui(int index) noexcept { return index==1 ? 3 : index==2 ? 1 : index==3 ? 2 : 0; }
void select_generation(Settings& value, int generation, int ofxr_engine) noexcept;
Settings sanitize(Settings value) noexcept;
Settings parse_settings(std::string_view text);
std::string serialize_settings(const Settings& value);

} // namespace xrfg::autonomous
