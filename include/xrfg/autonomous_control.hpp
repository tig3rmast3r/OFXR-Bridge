#pragma once
#include "xrfg/aer_framegen_evidence.hpp"
#include "xrfg/gpu_usage_status.hpp"
#include "xrfg/cpu_usage_status.hpp"
#include "xrfg/keybinds.hpp"

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
    int frame_multiplier{2}; // OFXR engines but 0.2.13, and Hybrid: 2X, 3X or 4X
    bool operator==(const GenerationPreferences&) const = default;
};
constexpr std::size_t kGenerationProfileCount = 8; // four OFXR engines, FSR, DLSS-G, Hybrid, RIFE
constexpr auto default_generation_profiles() noexcept {
    std::array<GenerationPreferences,kGenerationProfileCount> profiles{};
    profiles[7].scale=5;
    return profiles;
}

// Plain CPU values shared between the standalone UI and the in-process layer.
// No application resource pointers cross this boundary.
// The F10 panel keeps its edges within easier eye reach at this size.
inline constexpr float kDefaultMenuScale = .75F;
struct Settings {
    bool enabled{true};
    bool advanced_mode{}; // Global INI-only AdvancedMode; mirrored into each UI snapshot
    bool afw_only{}; // per-game startup choice, independent of Neural Rendering
    bool increase_buffer{}; // Framework2->3; native SteamVR Director already uses3; Legacy unaffected
    int pacing_mode{1}; // 0 framework, 1 sole director, 2 framework + director
    int backend{2}; // 0 FidelityFX, 1 NVIDIA reference, 2 NVIDIA new, 3 NVIDIA djules
    int motion_vectors{}; // 0 optical flow, 1 native game guides
    int rife_multiplier{2}; // experimental RIFE 2X/3X/4X
    int rife_resolution{18}; // Medium; inference presets10/15/18/21/25/30/36, native output unchanged
    int rife_model{3}; // 2 4.6, 3 4.4, 4 4.3; retired Lite IDs0/1 map to2
    int frame_generation{}; // 0 OFXR, 1 FSR 3, 2 native DLSS Frame Generation, 3 Hybrid, 4 RIFE
    int hybrid_backend{}; // Hybrid's own OFXR engine: 0 FidelityFX, 1 0.2.1, 2 0.3.0 (never 0.2.13)
    int preset{1}; // 0 slow, 1 medium, 2 fast
    int scale{2}; // 0 full, 1 three-quarter, 2 half, 3 quarter, 4 fifth (RIFE only)
    bool backward{};
    int separate_synthesis{}; // legacy field, follows combined Lower VRAM
    int lower_vram{}; // checkbox: 0 off (default), 1 on
    bool prefer_fps{true};
    bool triple_frame_gen{};
    int native_scale{100};
    int frame_multiplier{2}; // OFXR engines but 0.2.13, and Hybrid: 2X, 3X or 4X
    bool hybrid_depth_search{true}; // Hybrid: the game's depth keeps thin near surfaces (0.2 V505/V555)
    bool hybrid_hud_fix{}; // Hybrid: hold a HUD and cockpit instruments still (0.2 V559-V563, V573)
    int afw_mode{3}; // 0 off, 1 alternate, 2 previous, 3 combined
    bool afw_debug{};
    bool afw_clear_before_warping{};
    float afw_ignore_motion_threshold{2.5F};
    int afw_depth_dilate_mode{};
    int afw_shading_rate{}; // beta6 Combined; 0 full resolution, 4=2x2, 8=4x4
    int luke_osr{2};       // 2 OSR (default), 0 AFW (legacy); retired value 1 migrates to OSR
    bool luke_osr_lower_quality{}; // lower-rate geometry, native-resolution colour
    int luke_osr_cover{};  // OSR: band cover in depth texels beyond the hole, 0..8
    int luke_osr_trim{};   // OSR: object-edge texels replaced too, 0..2
    int luke_osr_fill{2};  // OSR: band fill gain, 1..8
    bool ofr_enabled{}; // Advanced AER framegen modifier, common to all 2X methods.
    int ofr_backend{}; // 0 FidelityFX, 1 OFXR optical flow (NVIDIA).
    int ofr_scale{2}; // 0 100%, 1 75%, 2 50%, 3 25%; per-axis flow size.
    bool ofr_swap{}; // Reverse temporal phase, never the logical eye mapping.
    bool luke_advanced{};  // Luke Ross tab: show the provider and OSR parameters
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
    bool overlay_enabled{true};
    int overlay_position{6}; // selected type 1..6; legacy 0 migrates to disabled/bottom
    float menu_scale{kDefaultMenuScale}; // physical headset interface multiplier, 0.5 to 1.25
    float menu_offset_x{}; // common-FOV centre offset, normalized -1 to 1
    float menu_offset_y{};
    std::array<GenerationPreferences, kGenerationProfileCount> generation_profiles{default_generation_profiles()};
    bool profiles_initialized{}; // legacy flat profile migration
    bool luke_shared_swapchains{true}; // INI-only startup policy: share compatible final native outputs
    bool nr_second_pass{}; // optional second NR evaluation, sharing the current model settings
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
    std::uint64_t game_bytes{}, rife_private_bytes{}, rife_reserved_bytes{};
    std::uint32_t rife_process_id{};
    bool rife_active{}, rife_valid{};
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
    bool nr_model_missing{}; // in the padding before provider_error: the IPC layout stays
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
    GpuUsageStatus gpu_usage{};
    CpuUsageStatus cpu_usage{};
    bool luke_shared_swapchains{}, luke_shared_swapchains_forced{}; // active session, not requested next launch
    bool vdxr_runtime{}, vdxr_legacy{true}; // active startup transport, not next-launch preference
    std::uint32_t buffer_capacity{}; // active admission capacity; zero for native Legacy/AFW-only
    std::uint32_t eye_long_side{}; // minimum per-eye longest side of the submitted stereo scene
    bool shared_queue{}; // actual capture/OFXR queue fallback, not the requested pacing
    bool afw_flat_scene{};
    bool rife_optimizing{}; // asynchronous engine compilation; existing generation can continue
    bool rife_tensorrt{}; // backend used by the latest RIFE inference, independent of desired state
    bool rife_vulkan{};
    unsigned rife_max_multiplier{2}; // actual active session capability, independent of requested engine
    bool rife_execution_active{}; // a fresh, live worker receipt exists (false is not proof of Torch)
    unsigned rife_error{}; // fresh receipt: 1 timeout, 2 unavailable, 3 optimization failure
    int osr_mode{-1},osr_lower_quality{-1}; // last successfully submitted reconstruction
    AerFramegenEvidenceSnapshot ofr_evidence{};
    bool ofr_running{}; // confirmed retimed output, not preparation or flag alone
    unsigned ofr_delivery{}; // 0 learning, 1 alternating, 2 paired.
    bool uevr_osr{}; // UEVR Alternating/AFR attached as OSR's eye source, as Luke Ross is
};
inline bool pacing_comparison_runtime(const RuntimeStatus& r) noexcept {
    return !r.vdxr_legacy;
}
// A source of eyes for AFW/OSR to reconstruct: Luke Ross, or UEVR's Alternating/AFR.
inline bool reconstruction_source(const RuntimeStatus& r) noexcept {
    return r.luke_detected || r.uevr_osr;
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
// Tray-global bindings: never redirected by a selected/bound game profile.
KeybindSnapshot keybinds() noexcept;
bool publish_keybinds(Keybinds value) noexcept;
bool suspend_keybinds(bool value) noexcept; // short renewable capture lease
bool request(Settings desired) noexcept;
// Publish only fields edited since base, preserving changes from another UI.
// The shared-mapping mutex acquisition never blocks the end-frame thread.
bool request_changes(const Settings& base, const Settings& desired) noexcept;
Settings merge_changes(Settings current, const Settings& base, const Settings& desired) noexcept;
void report_runtime(RuntimeStatus status) noexcept;
void report_video_memory(VideoMemoryStatus value) noexcept;
void report_gpu_usage(GpuUsageStatus value) noexcept;
void report_cpu_usage(CpuUsageStatus value) noexcept;
std::string interface_cpu_text(const Snapshot& value,std::uint64_t now_ms);
std::string interface_vram_text(const Snapshot& value);
std::string interface_gpu_text(const Snapshot& value,std::uint64_t now_ms);
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
std::string_view interface_reconstruction_text(const Snapshot& value,std::uint64_t now_ms) noexcept;
std::string interface_status_text(const Snapshot& value,bool include_reconstruction=true);
bool interface_aer_active(const Snapshot&,std::uint64_t now_ms) noexcept;
std::string interface_aer_text(const Snapshot&,std::uint64_t now_ms);
std::string_view interface_aer_source_text(const Snapshot&) noexcept;
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

constexpr int generation_ui_index(int mode) noexcept { return mode==3 ? 1 : mode==1 ? 2 : mode==2 ? 3 : mode==4 ? 4 : 0; }
constexpr int generation_from_ui(int index) noexcept { return index==1 ? 3 : index==2 ? 1 : index==3 ? 2 : index==4 ? 4 : 0; }
void select_generation(Settings& value, int generation, int ofxr_engine) noexcept;
Settings sanitize(Settings value) noexcept;
// OFXR's FidelityFX, 0.2.1 and 0.3.0 engines and Hybrid: the routes whose
// frame_multiplier (2X-4X) applies. 0.2.13 and DLSS-G keep triple_frame_gen.
[[nodiscard]] bool optical_multiplier_route(const Settings& s) noexcept;
// The output multiplier the selected route asks for.
[[nodiscard]] int generation_multiplier(const Settings& s) noexcept;
// The multiplier the layer receives as rife_multiplier: RIFE's, OFXR/Hybrid's
// 2X-4X, or DLSS-G's 4X (its 3X stays on triple_frame_gen).
[[nodiscard]] int runtime_multiplier(const Settings& s) noexcept;
Settings parse_settings(std::string_view text);
std::string serialize_settings(const Settings& value,bool include_global_ui=false);

} // namespace xrfg::autonomous
