#pragma once
#include "xrfg/fps_overlay_model.hpp"
#include "xrfg/autonomous_control.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace xrfg::standalone {

inline constexpr wchar_t kLayerName[] =
    L"XR_APILAYER_OFXRBridge";

enum class FlowBackend {
    fidelity_fx,
    nvidia,
};
enum class FrameGeneration { ofxr, native_dlss };
enum class RuntimePacing { automatic, force_vdxr, force_steamvr };
constexpr bool director_transport(RuntimePacing mode, bool actual_vdxr) noexcept {
    return mode==RuntimePacing::force_steamvr || (mode==RuntimePacing::automatic && !actual_vdxr);
}

enum class NvidiaPerformancePreset {
    slow,
    medium,
    fast,
};

enum class NvidiaInputScale {
    full,
    three_quarter,
    half,
    quarter,
};

struct LauncherSettings {
    bool crash_debug{}; // Opt-in crash recorder + per-game startup DRED.
    FrameGeneration frame_generation{FrameGeneration::ofxr};
    // Native DLSS Frame Generation's resolution, in percent of each eye's:
    // the layer's dlssg_resolution. 100 by default; below it the generated
    // frames trade sharpness on moving content for GPU time.
    int native_scale{100};
    // Standalone backend preference. The autonomous control transaction
    // supplies the active route before the layer creates its synthesizer.
    FlowBackend backend{FlowBackend::nvidia};
    NvidiaPerformancePreset nvidia_preset{NvidiaPerformancePreset::medium};
    NvidiaInputScale nvidia_input_scale{NvidiaInputScale::half};
    bool nvidia_bidirectional{};
    // "Prefer FPS over latency": the layer's deeper pipeline. On by default.
    bool deep_pipeline{true};
    // "3X Frame Gen": two synthetic frames per application frame. Off by
    // default.
    bool triple_frame_gen{};
    // Executables the layer declines to load in, ';'-separated; written to
    // the layer's ini as excluded_processes. Pimax Home by default (see
    // implicit_layer.hpp). No menu entry; edit tray.ini.
    std::string excluded_processes{"PimaxHome-Win64-Shipping.exe"};
    // One private swapchain per output (see the layer's
    // single_swapchain_rings): "Lower VRAM" checkbox. Off by default.
    // Enabling private output staging reduces XR surface allocations and
    // adds a hand-over copy, which can affect smoothness.
    bool separate_synthesis{};
    bool single_swapchain_rings{};
    // Tray-only session choice; independent of the active generator and A/B.
    // Native D3D12 SteamVR only. Changing it requires restarting the game.
    bool steamvr_separate_queue{true};
    // Tray-only startup experiment: Luke AFW goes straight to native OpenXR.
    // OFXR is disabled for this session; Neural Rendering remains independent.
    bool afw_only{}; // runtime-INI serialization only; user preference lives in game Settings
    RuntimePacing pacing{RuntimePacing::automatic}; // global, startup-only transport selection
    // The Vulkan bridge (see the layer's vulkan_session_bridge): a Vulkan
    // game's session is handed to the runtime as D3D12. On by default; no
    // menu entry.
    bool vulkan_session_bridge{true};
    // Vulkan support: generate for Vulkan sessions and register the
    // queue-serialising Vulkan layer while armed. On by default and not in
    // the menu, like d3d11_bridge: vulkan_bridge=0 in tray.ini turns it off. The
    // key was vulkan_support while the option was off by default, and a
    // stored vulkan_support=0 is ignored (see parse_settings).
    // While armed, the implicit Vulkan layer loads into every Vulkan process
    // on the machine; it is removed at disarm.
    bool vulkan_support{true};
    // The D3D11 bridge: a D3D11 game's session is given to the runtime as a
    // D3D12 one on the layer's own device, so the runtime never works the
    // game's D3D11 device from the presenter thread. On by default and not
    // in the menu: the runtime ini is rewritten from this at every arm, so
    // d3d11_bridge=0 in tray.ini is the one place to turn it off, for
    // diagnosing a title against the direct D3D11 path.
    bool d3d11_bridge{true};
    // The layer's capture_at_end_frame: a D3D12 game's eye images are copied
    // when it ends its frame, not when it releases them. On by default and
    // not in the menu; capture_at_end_frame=0 in tray.ini goes back to the
    // copy at release, for comparing the two on one build.
    bool capture_at_end_frame{true};
    bool diagnostics{};
    FpsOverlayPosition overlay_position{FpsOverlayPosition::elliott_bottom};
    // The system-wide key for "Pause frame generation" while armed, in
    // parse_hotkey's form. No menu entry; edit tray.ini. Empty, "off" or
    // anything parse_hotkey refuses means no key.
    std::string pause_hotkey{"ctrl+alt+f7"}; // kDefaultPauseHotkey
};

// status.json (docs/STATUS_JSON.md): the tray's state and the running game as
// its layer reports it; the settings only for the configured multiplier,
// written without a game. `session_present` false writes the session and its
// rates as null; a rate whose `*_valid` is false is null too.
struct StatusSession {
    std::uint32_t pid{};
    std::string process;  // UTF-8
    std::string runtime;  // UTF-8
    std::uint32_t multiplier{2};
    bool deep_pipeline{};
    bool generating{};
    float app_fps{};
    float output_fps{};
    float display_hz{};
    float latency_ms{};
    bool rates_valid{true}, output_valid{true}, latency_valid{};
};
struct StatusInputs {
    bool armed{};
    bool paused{};
    std::uint32_t build{};
    std::string updated;  // ISO 8601 UTC, e.g. 2026-10-17T09:12:03.512Z
    bool session_present{};
    StatusSession session;
    LauncherSettings settings;
};
[[nodiscard]] std::string build_status_json(const StatusInputs& inputs);

// A key chord for RegisterHotKey: `modifiers` in its MOD_ALT (1),
// MOD_CONTROL (2), MOD_SHIFT (4), MOD_WIN (8) bits and a virtual-key code.
struct Hotkey {
    unsigned modifiers{};
    unsigned key{};
    bool operator==(const Hotkey&) const = default;
};
// "ctrl+alt+f7": '+'-separated, any case, the key last. Modifiers are ctrl,
// alt, shift and win, and none is required: the choice is the user's. Keys
// are f1-f24, a letter, a digit, a named key (scrolllock, pause, insert,
// delete, home, end, pageup, pagedown, left, up, right, down, numpad0-9,
// numpadmultiply/add/subtract/decimal/divide) or any other key by its
// virtual-key code, "vk" and two hex digits ("vkba").
[[nodiscard]] std::optional<Hotkey> parse_hotkey(std::string_view text);
// "Ctrl + Alt + F7", for the menu; empty for no key.
[[nodiscard]] std::string hotkey_display_name(std::string_view text);
// The tray.ini spelling of a chord, "ctrl+alt+f7", or nothing for one
// parse_hotkey would refuse: the inverse of parse_hotkey.
[[nodiscard]] std::optional<std::string> hotkey_setting(Hotkey hotkey);
inline constexpr char kDefaultPauseHotkey[] = "ctrl+alt+f7";

[[nodiscard]] std::string backend_ini_value(FlowBackend backend);
[[nodiscard]] std::string nvidia_preset_ini_value(
    NvidiaPerformancePreset preset);
[[nodiscard]] std::string nvidia_input_scale_ini_value(
    NvidiaInputScale scale);
[[nodiscard]] LauncherSettings parse_settings(std::string_view text);
[[nodiscard]] std::string serialize_settings(const LauncherSettings& settings);

// The Vulkan implicit layer's manifest, for the loader's ImplicitLayers key.
[[nodiscard]] std::string build_vulkan_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version);

[[nodiscard]] std::string build_implicit_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version);

// Diagnostics knobs the tray has no UI for. The runtime INI is their source of
// truth: arming used to regenerate the whole file from LauncherSettings with
// these two hardcoded, so a hand-edited value survived until the next arm and
// no further. write_runtime_configuration reads them back out of the existing
// file and passes them here.
//
// 32 MB is the default because a capture that size covers the usual case and
// costs nothing to leave enabled. At the usual record density it wraps at about
// ninety seconds, keeping only the tail - so a long session that needs its
// early records raises this by hand, which now survives arming.
constexpr unsigned kDefaultMaxFileMb = 32;

[[nodiscard]] std::string build_runtime_ini(
    const LauncherSettings& settings,
    unsigned max_file_mb = kDefaultMaxFileMb,
    bool flush_each_event = false);

// The autonomous profile owns renderer, AFW and NR preferences.
[[nodiscard]] std::string build_runtime_ini(
    const LauncherSettings& launcher,
    const autonomous::Settings& controls,
    unsigned max_file_mb = kDefaultMaxFileMb,
    bool flush_each_event = false);

[[nodiscard]] std::filesystem::path runtime_version_directory(
    const std::filesystem::path& local_directory,
    std::uint32_t implementation_version);

// Replaces an existing cached layer instead of silently retaining it, unless
// it is already the same file. The cache folder is per version, so normally it
// is - and while armed the Vulkan layer loads into every Vulkan process, which
// keeps it loaded after a disarm and makes overwriting it fail with a sharing
// violation that blocked re-arming until those processes exited. A different
// file that is still loaded is renamed aside and the new one installed under
// the original name.
[[nodiscard]] bool install_runtime_layer_dll(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) noexcept;

// Copies the required standalone backend helpers into an inactive cache.
// A user-supplied NR vendor runtime is optional and is copied when present.
[[nodiscard]] bool stage_runtime_support_files(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& destination_directory,
    std::wstring* error = nullptr) noexcept;

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

} // namespace xrfg::standalone
