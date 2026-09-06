#include "xrfg/implicit_layer.hpp"
#include "xrfg/standalone_launcher.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

[[nodiscard]] bool contains(std::string_view text, std::string_view value) {
    return text.find(value) != std::string_view::npos;
}

} // namespace

int main() {
    using namespace xrfg::standalone;

    const LauncherSettings release_defaults;
    const std::string default_runtime_ini = build_runtime_ini(release_defaults);
    if (release_defaults.overlay_position != xrfg::FpsOverlayPosition::upper_right ||
        !contains(default_runtime_ini, "[overlay]\r\nposition=upper_right")) return 1;
    for (auto position : {xrfg::FpsOverlayPosition::off, xrfg::FpsOverlayPosition::upper_left,
        xrfg::FpsOverlayPosition::upper_right, xrfg::FpsOverlayPosition::lower_left, xrfg::FpsOverlayPosition::lower_right}) {
        LauncherSettings option;
        option.overlay_position = position;
        if (parse_settings(serialize_settings(option)).overlay_position != position ||
            !contains(build_runtime_ini(option), std::string("[overlay]\r\nposition=") +
                xrfg::overlay_position_name(position))) return 1;
    }
    if (release_defaults.backend != FlowBackend::fidelity_fx ||
        release_defaults.nvidia_preset != NvidiaPerformancePreset::medium ||
        release_defaults.nvidia_input_scale != NvidiaInputScale::half ||
        release_defaults.nvidia_bidirectional || release_defaults.diagnostics ||
        !contains(default_runtime_ini, "[ofxr]\r\nbackend=fidelityfx") ||
        !contains(default_runtime_ini, "nvidia_preset=medium") ||
        !contains(default_runtime_ini, "nvidia_input_scale=50") ||
        !contains(default_runtime_ini, "[diagnostics]\r\nlogging_enabled=0")) {
        std::cerr << "release defaults failed\n";
        return 1;
    }

    LauncherSettings settings;
    settings.backend = FlowBackend::nvidia;
    settings.nvidia_preset = NvidiaPerformancePreset::slow;
    settings.nvidia_input_scale = NvidiaInputScale::half;
    settings.nvidia_bidirectional = true;
    settings.diagnostics = true;
    const std::string serialized = serialize_settings(settings);
    const LauncherSettings parsed = parse_settings(serialized);
    if (parsed.backend != FlowBackend::nvidia ||
        parsed.nvidia_preset != NvidiaPerformancePreset::slow ||
        parsed.nvidia_input_scale != NvidiaInputScale::half ||
        !parsed.nvidia_bidirectional || !parsed.diagnostics) {
        std::cerr << "standalone settings round-trip failed\n";
        return 1;
    }

    const std::filesystem::path layer_path =
        L"C:\\Program Files\\OFXR Bridge\\RuntimeLayer\\v030\\layer.dll";
    const std::string manifest = build_implicit_layer_manifest(layer_path, 18);
    if (!contains(manifest, "C:\\\\Program Files\\\\OFXR Bridge") ||
        !contains(manifest, "\"implementation_version\": \"18\"") ||
        !contains(manifest, "manual persistent implicit layer") ||
        !contains(
            manifest,
            "\"disable_environment\": \"XRFG_DISABLE_OFXR_BRIDGE\"") ||
        contains(manifest, "XR_ENABLE_API_LAYERS") ||
        contains(manifest, "XR_API_LAYER_PATH")) {
        std::cerr << "manual implicit manifest contract failed\n";
        return 1;
    }

    const std::string runtime_ini = build_runtime_ini(settings);
    if (!contains(runtime_ini, "[ofxr]\r\nbackend=nvidia") ||
        !contains(runtime_ini, "nvidia_preset=slow") ||
        !contains(runtime_ini, "nvidia_input_scale=50") ||
        !contains(runtime_ini, "nvidia_bidirectional=1") ||
        !contains(runtime_ini, "[diagnostics]\r\nlogging_enabled=1") ||
        contains(runtime_ini, "one_shot") ||
        contains(runtime_ini, "manifest=")) {
        std::cerr << "manual runtime INI contract failed\n";
        return 1;
    }

    const std::filesystem::path runtime_directory =
        L"C:\\Users\\Test\\OFXR Bridge\\RuntimeLayer\\v030";
    const std::filesystem::path implicit_manifest = runtime_directory /
        L"XR_APILAYER_XRFrameBridge_manual-42-99.json";
    if (!xrfg::implicit_layer::owned_manifest_path(
            implicit_manifest, runtime_directory) ||
        xrfg::implicit_layer::owned_manifest_path(
            runtime_directory / L"different-layer.json", runtime_directory) ||
        xrfg::implicit_layer::owned_manifest_path(
            runtime_directory.parent_path() /
                L"XR_APILAYER_XRFrameBridge_manual-42-99.json",
            runtime_directory)) {
        std::cerr << "owned manifest path validation failed\n";
        return 1;
    }

    const std::wstring test_subkey =
        L"Software\\OFXRBridgeTest\\manual-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64());
    std::wstring registry_error;
    const bool registered = xrfg::implicit_layer::register_manifest(
        implicit_manifest, &registry_error, test_subkey);
    const bool visible_before_probe =
        xrfg::implicit_layer::manifest_registered(implicit_manifest, test_subkey);
    // V017 deliberately has no loader-negotiation consumption path: repeated
    // OpenXR probes must leave the manual arm untouched until tray disarm.
    const bool visible_after_probe =
        xrfg::implicit_layer::manifest_registered(implicit_manifest, test_subkey);
    const bool unregistered = xrfg::implicit_layer::unregister_manifest(
        implicit_manifest, &registry_error, test_subkey);
    const bool removed = !xrfg::implicit_layer::manifest_registered(
        implicit_manifest, test_subkey);
    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, test_subkey.c_str()));
    static_cast<void>(RegDeleteKeyW(
        HKEY_CURRENT_USER, L"Software\\OFXRBridgeTest"));
    if (!registered || !visible_before_probe || !visible_after_probe ||
        !unregistered || !removed) {
        std::wcerr << L"manual registry lifetime contract failed: "
                   << registry_error << L'\n';
        return 1;
    }

    const auto backend_directory =
        std::filesystem::temp_directory_path() /
        (L"ofxr-manual-backend-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(backend_directory);
    {
        std::ofstream ini(backend_directory / L"ofxr_bridge.ini");
        ini << "[ofxr]\nbackend=nvidia\n"
               "nvidia_preset=slow\n"
               "nvidia_input_scale=75\n"
               "nvidia_bidirectional=1\n";
    }
    const bool backend_from_ini =
        xrfg::implicit_layer::read_flow_backend(backend_directory) ==
        xrfg::implicit_layer::ConfiguredFlowBackend::nvidia;
    const auto nvidia_options =
        xrfg::implicit_layer::read_nvidia_options(backend_directory);
    bool preset_round_trips = true;
    for (auto preset : {NvidiaPerformancePreset::slow, NvidiaPerformancePreset::medium,
                        NvidiaPerformancePreset::fast}) {
        LauncherSettings option;
        option.backend = FlowBackend::nvidia;
        option.nvidia_preset = preset;
        const auto round_trip = parse_settings(serialize_settings(option));
        const auto text = build_runtime_ini(round_trip);
        {
            std::ofstream ini(backend_directory / L"ofxr_bridge.ini", std::ios::trunc);
            ini << text;
        }
        const auto loaded = xrfg::implicit_layer::read_nvidia_options(backend_directory);
        const auto expected = preset == NvidiaPerformancePreset::fast
            ? xrfg::implicit_layer::ConfiguredNvidiaPerformancePreset::fast
            : preset == NvidiaPerformancePreset::slow
                ? xrfg::implicit_layer::ConfiguredNvidiaPerformancePreset::slow
                : xrfg::implicit_layer::ConfiguredNvidiaPerformancePreset::medium;
        preset_round_trips = preset_round_trips && round_trip.nvidia_preset == preset &&
            contains(text, "nvidia_preset=" + nvidia_preset_ini_value(preset)) &&
            loaded.preset == expected &&
            loaded.input_scale == xrfg::implicit_layer::ConfiguredNvidiaInputScale::half &&
            !loaded.bidirectional;
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(backend_directory, cleanup_error);
    if (!backend_from_ini || !preset_round_trips ||
        nvidia_options.preset !=
            xrfg::implicit_layer::ConfiguredNvidiaPerformancePreset::slow ||
        nvidia_options.input_scale !=
            xrfg::implicit_layer::ConfiguredNvidiaInputScale::three_quarter ||
        !nvidia_options.bidirectional || cleanup_error) {
        std::cerr << "backend INI selection failed\n";
        return 1;
    }

    const auto default_nvidia_options =
        xrfg::implicit_layer::read_nvidia_options({});
    if (default_nvidia_options.preset !=
            xrfg::implicit_layer::ConfiguredNvidiaPerformancePreset::medium ||
        default_nvidia_options.input_scale !=
            xrfg::implicit_layer::ConfiguredNvidiaInputScale::half ||
        default_nvidia_options.bidirectional) {
        std::cerr << "NVIDIA option defaults failed\n";
        return 1;
    }

    const std::filesystem::path version_directory =
        runtime_version_directory(L"C:\\Users\\Test\\OFXR Bridge", 62);
    if (version_directory.filename() != L"v062") {
        std::cerr << "runtime version directory failed\n";
        return 1;
    }

    const auto replacement_directory =
        std::filesystem::temp_directory_path() /
        (L"ofxr-runtime-replacement-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(replacement_directory);
    const auto replacement_source = replacement_directory / L"source.dll";
    const auto replacement_destination = replacement_directory / L"runtime" /
        L"layer.dll";
    {
        std::ofstream source(replacement_source, std::ios::binary);
        source << "new-layer";
        std::filesystem::create_directories(replacement_destination.parent_path());
        std::ofstream destination(replacement_destination, std::ios::binary);
        destination << "stale-layer";
    }
    const bool replaced = install_runtime_layer_dll(
        replacement_source, replacement_destination);
    std::ifstream replaced_stream(replacement_destination, std::ios::binary);
    const std::string replaced_text(
        (std::istreambuf_iterator<char>(replaced_stream)),
        std::istreambuf_iterator<char>());
    replaced_stream.close();
    cleanup_error.clear();
    std::filesystem::remove_all(replacement_directory, cleanup_error);
    if (!replaced || replaced_text != "new-layer" || cleanup_error) {
        std::cerr << "runtime DLL replacement failed\n";
        return 1;
    }

    if (quote_windows_argument(L"C:\\Game\\game.exe") !=
            L"C:\\Game\\game.exe" ||
        quote_windows_argument(L"C:\\My Game\\game.exe") !=
            L"\"C:\\My Game\\game.exe\"" ||
        quote_windows_argument(L"C:\\Path With Space\\") !=
            L"\"C:\\Path With Space\\\\\"") {
        std::cerr << "Windows argument quoting failed\n";
        return 1;
    }

    std::cout << "OFXR manual persistent launcher support tests passed\n";
    return 0;
}
