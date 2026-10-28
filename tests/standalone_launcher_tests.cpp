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
    if(release_defaults.crash_debug)return 1;
    for(bool enabled:{false,true}) {
        auto choice=release_defaults;choice.crash_debug=enabled;
        const auto key=enabled?"crash_debug=1":"crash_debug=0";
        if(parse_settings(serialize_settings(choice)).crash_debug!=enabled ||
            !contains(build_runtime_ini(choice),key))return 1;
    }
    if (!release_defaults.steamvr_separate_queue || release_defaults.afw_only) return 1;
    if(release_defaults.pacing!=RuntimePacing::automatic)return 1;
    for(auto mode:{RuntimePacing::automatic,RuntimePacing::force_vdxr,RuntimePacing::force_steamvr}) {
        auto choice=release_defaults;choice.pacing=mode;
        if(parse_settings(serialize_settings(choice)).pacing!=mode)return 1;
    }
    if(director_transport(RuntimePacing::automatic,true) || !director_transport(RuntimePacing::automatic,false) ||
       director_transport(RuntimePacing::force_vdxr,false) || !director_transport(RuntimePacing::force_steamvr,true))return 1;
    for(bool enabled:{false,true}) {
        xrfg::autonomous::Settings controls;controls.afw_only=enabled;
        if(!contains(build_runtime_ini(release_defaults,controls),enabled?"afw_only=1":"afw_only=0"))return 1;
    }
    for (bool separate : {false, true}) {
        auto queue_settings = release_defaults;
        queue_settings.steamvr_separate_queue = separate;
        if (parse_settings(serialize_settings(queue_settings)).steamvr_separate_queue != separate ||
            !contains(build_runtime_ini(queue_settings), separate ? "steamvr_separate_queue=1" : "steamvr_separate_queue=0")) return 1;
        for (int generator : {0, 1, 2}) for (int backend : {0, 1, 2, 3}) {
            xrfg::autonomous::Settings controls;
            controls.backend = backend; controls.frame_generation = generator;
            if (!contains(build_runtime_ini(queue_settings, controls), separate ? "steamvr_separate_queue=1" : "steamvr_separate_queue=0")) return 1;
        }
    }
    xrfg::autonomous::Settings autonomous;
    autonomous.backend = 3;
    autonomous.frame_generation = 1;
    autonomous.afw_mode = 2;
    autonomous.nr_enabled = true;
    autonomous.scale = 0;
    const auto autonomous_ini = build_runtime_ini(release_defaults, autonomous, 128, true);
    if (!contains(autonomous_ini, "nvidia_route=djules") ||
        !contains(autonomous_ini, "nvidia_input_scale=100") ||
        !contains(autonomous_ini, "frame_generation=fsr3") ||
        !contains(autonomous_ini, "motion_vectors=dlss") ||
        !contains(autonomous_ini, "luke_afw_mode=2") ||
        contains(autonomous_ini, "luke_afw=1") ||
        !contains(autonomous_ini, "max_file_mb=128") ||
        !contains(autonomous_ini, "single_swapchain_rings=0")) return 1;
    autonomous.lower_vram = 1;
    if (!contains(build_runtime_ini(release_defaults, autonomous), "single_swapchain_rings=1")) return 1;
    autonomous.lower_vram = 0;
    for (int separate : {0,1}) for (int reduce : {0,1}) {
        autonomous.separate_synthesis = separate; autonomous.lower_vram = reduce;
        const auto ini = build_runtime_ini(release_defaults, autonomous);
        if (!contains(ini, reduce ? "separate_synthesis=1" : "separate_synthesis=0") ||
            !contains(ini, reduce ? "single_swapchain_rings=1" : "single_swapchain_rings=0")) return 1;
    }
    autonomous.separate_synthesis = 0; autonomous.lower_vram = 0;
    if (!parse_settings("[tray]\nsingle_swapchain_rings=1\n").separate_synthesis ||
        parse_settings("[tray]\nsingle_swapchain_rings=1\nseparate_synthesis=0\n").separate_synthesis) return 1;

    const std::string default_runtime_ini = build_runtime_ini(release_defaults);
    if (!contains(default_runtime_ini,"timing_capture=0") ||
        !contains(default_runtime_ini,"pose_cadence_capture=0") ||
        !contains(default_runtime_ini,"gpu_instrumentation_enabled=0")) return 1;
    if (release_defaults.frame_generation != FrameGeneration::ofxr ||
        !contains(default_runtime_ini, "frame_generation=ofxr")) return 1;
    LauncherSettings native_settings;
    native_settings.frame_generation = FrameGeneration::native_dlss;
    if (parse_settings(serialize_settings(native_settings)).frame_generation != FrameGeneration::native_dlss ||
        !contains(build_runtime_ini(native_settings), "frame_generation=dlss") ||
        parse_settings("[tray]\nframe_generation=unknown\n").frame_generation != FrameGeneration::ofxr) return 1;
    // Native DLSS FG runs at full resolution unless chosen otherwise.
    if (release_defaults.native_scale != 100 ||
        !contains(default_runtime_ini, "dlssg_resolution=100")) return 1;
    LauncherSettings reduced_settings;
    reduced_settings.native_scale = 67;
    if (parse_settings(serialize_settings(reduced_settings)).native_scale != 67 ||
        !contains(build_runtime_ini(reduced_settings), "dlssg_resolution=67") ||
        parse_settings("[tray]\ndlssg_resolution=7\n").native_scale != 100) return 1;
    if (release_defaults.overlay_position != xrfg::FpsOverlayPosition::elliott_bottom ||
        !contains(default_runtime_ini, "[overlay]\r\nposition=elliott_bottom")) return 1;
    for (auto position : {xrfg::FpsOverlayPosition::off, xrfg::FpsOverlayPosition::upper_left,
        xrfg::FpsOverlayPosition::upper_right, xrfg::FpsOverlayPosition::lower_left, xrfg::FpsOverlayPosition::lower_right}) {
        LauncherSettings option;
        option.overlay_position = position;
        if (parse_settings(serialize_settings(option)).overlay_position != position ||
            !contains(build_runtime_ini(option), std::string("[overlay]\r\nposition=") +
                xrfg::overlay_position_name(position))) return 1;
    }
    if (release_defaults.backend != FlowBackend::nvidia ||
        release_defaults.nvidia_preset != NvidiaPerformancePreset::medium ||
        release_defaults.nvidia_input_scale != NvidiaInputScale::half ||
        release_defaults.nvidia_bidirectional || release_defaults.diagnostics ||
        !release_defaults.deep_pipeline ||
        release_defaults.triple_frame_gen ||
        release_defaults.excluded_processes != "PimaxHome-Win64-Shipping.exe" ||
        !contains(default_runtime_ini, "excluded_processes=PimaxHome-Win64-Shipping.exe") ||
        release_defaults.single_swapchain_rings ||
        !contains(default_runtime_ini, "single_swapchain_rings=0") ||
        !release_defaults.vulkan_session_bridge ||
        !contains(default_runtime_ini, "vulkan_session_bridge=1") ||
        !contains(default_runtime_ini, "triple_frame_gen=0") ||
        !contains(default_runtime_ini, "deep_pipeline=1") ||
        !release_defaults.vulkan_support ||
        !release_defaults.d3d11_bridge ||
        !contains(default_runtime_ini, "vulkan_bridge=1") ||
        !contains(default_runtime_ini, "d3d11_bridge=1") ||
        !release_defaults.capture_at_end_frame ||
        !contains(default_runtime_ini, "capture_at_end_frame=1") ||
        !contains(default_runtime_ini, "[ofxr]\r\nbackend=nvidia") ||
        !contains(default_runtime_ini, "motion_vectors=dlss") ||
        !contains(default_runtime_ini, "nvidia_preset=medium") ||
        !contains(default_runtime_ini, "nvidia_input_scale=50") ||
        !contains(default_runtime_ini, "[diagnostics]\r\nlogging_enabled=0")) {
        std::cerr << "release defaults failed\n";
        return 1;
    }

    // The two diagnostics knobs the tray has no UI for. They were hardcoded
    // here, so arming rewrote the file whole and a hand-edited max_file_mb
    // survived only until the next arm. The tray now reads them back out of the
    // existing runtime INI and passes them through.
    if (!contains(default_runtime_ini, "max_file_mb=32") ||
        !contains(default_runtime_ini, "flush_each_event=0")) {
        std::cerr << "diagnostics defaults failed\n";
        return 1;
    }
    const std::string carried = build_runtime_ini(release_defaults, 512, true);
    if (!contains(carried, "max_file_mb=512") ||
        !contains(carried, "flush_each_event=1")) {
        std::cerr << "diagnostics overrides not carried into the runtime ini\n";
        return 1;
    }

    LauncherSettings settings;
    // Every value away from its default, so the round trip proves parsing.
    settings.backend = FlowBackend::fidelity_fx;
    settings.nvidia_preset = NvidiaPerformancePreset::slow;
    settings.nvidia_input_scale = NvidiaInputScale::quarter;
    settings.nvidia_bidirectional = true;
    settings.deep_pipeline = false;
    settings.triple_frame_gen = true;
    settings.excluded_processes = "Home.exe; Other-Shipping.exe";
    settings.single_swapchain_rings = false;
    settings.vulkan_session_bridge = false;
    settings.vulkan_support = false;
    settings.d3d11_bridge = false;
    settings.capture_at_end_frame = false;
    settings.diagnostics = true;
    settings.pause_hotkey = "shift+scrolllock";
    const std::string serialized = serialize_settings(settings);
    const LauncherSettings parsed = parse_settings(serialized);
    if (parsed.backend != FlowBackend::fidelity_fx ||
        parsed.nvidia_preset != NvidiaPerformancePreset::slow ||
        parsed.nvidia_input_scale != NvidiaInputScale::quarter ||
        !parsed.nvidia_bidirectional || parsed.deep_pipeline ||
        !parsed.triple_frame_gen ||
        parsed.excluded_processes != "Home.exe; Other-Shipping.exe" ||
        parsed.single_swapchain_rings || parsed.vulkan_session_bridge ||
        parsed.vulkan_support || parsed.d3d11_bridge ||
        parsed.capture_at_end_frame || !parsed.diagnostics ||
        !contains(build_runtime_ini(parsed), "capture_at_end_frame=0")) {
        std::cerr << "standalone settings round-trip failed\n";
        return 1;
    }
    using xrfg::standalone::Hotkey;
    using xrfg::standalone::hotkey_display_name;
    using xrfg::standalone::parse_hotkey;
    if (release_defaults.pause_hotkey != "ctrl+alt+f7" ||
        parsed.pause_hotkey != "shift+scrolllock" ||
        parse_settings("[tray]\r\npause_hotkey= Ctrl+Alt+F9 \r\n").pause_hotkey != "ctrl+alt+f9" ||
        parse_hotkey("ctrl+alt+f7") != Hotkey{3, 0x76} ||
        parse_hotkey(" Alt + Control + F7 ") != Hotkey{3, 0x76} ||
        parse_hotkey("shift+scrolllock") != Hotkey{4, 0x91} ||
        parse_hotkey("f24") != Hotkey{0, 0x87} ||
        parse_hotkey("ctrl+shift+p") != Hotkey{6, 'P'} ||
        parse_hotkey("win+0") != Hotkey{8, '0'} ||
        hotkey_display_name("ctrl+alt+f7") != "Ctrl + Alt + F7" ||
        hotkey_display_name("shift+pagedown") != "Shift + Page Down" ||
        xrfg::standalone::hotkey_setting(Hotkey{3, 0x76}) != std::string("ctrl+alt+f7") ||
        xrfg::standalone::hotkey_setting(Hotkey{6, 'P'}) != std::string("ctrl+shift+p") ||
        xrfg::standalone::hotkey_setting(Hotkey{4, 0x22}) != std::string("shift+pagedown") ||
        xrfg::standalone::hotkey_setting(Hotkey{0, 0x76}) != std::string("f7") ||
        // Whatever the user picks: a bare letter, an arrow, a numpad key,
        // and a key with no name, by its code.
        xrfg::standalone::hotkey_setting(Hotkey{0, 'P'}) != std::string("p") ||
        xrfg::standalone::hotkey_setting(Hotkey{2, 0x25}) != std::string("ctrl+left") ||
        xrfg::standalone::hotkey_setting(Hotkey{0, 0x6B}) != std::string("numpadadd") ||
        xrfg::standalone::hotkey_setting(Hotkey{1, 0xBA}) != std::string("alt+vkba") ||
        parse_hotkey("p") != Hotkey{0, 'P'} ||
        parse_hotkey("7") != Hotkey{0, '7'} ||
        parse_hotkey("Alt+VKBA") != Hotkey{1, 0xBA} ||
        hotkey_display_name("numpad5") != "Num 5" ||
        hotkey_display_name("alt+vkba") != "Alt + Key 0xba" ||
        xrfg::standalone::hotkey_setting(Hotkey{2, 0}) ||
        xrfg::standalone::hotkey_setting(Hotkey{2, 0xFF}) ||
        std::string(xrfg::standalone::kDefaultPauseHotkey) != release_defaults.pause_hotkey ||
        !hotkey_display_name("off").empty()) {
        std::cerr << "pause hotkey parsing failed\n";
        return 1;
    }
    for (const char* refused : {"", "off", "none", "ctrl", "ctrl+", "+f7",
             "ctrl+ctrl+f7", "f7+ctrl", "f0", "f25", "ctrl+f7+f8", "ctrl+banana",
             "vk00", "vkff", "vkzz", "vk1"}) {
        if (parse_hotkey(refused)) {
            std::cerr << "pause hotkey accepted: " << refused << '\n';
            return 1;
        }
    }

    // A tray.ini from before Vulkan support was on by default carries the
    // vulkan_support=0 every save wrote; it must not keep the option off.
    // Only the new key, set by hand, turns it off.
    if (!parse_settings("[tray]\r\nvulkan_support=0\r\n").vulkan_support ||
        parse_settings("[tray]\r\nvulkan_bridge=0\r\n").vulkan_support ||
        !parse_settings("[tray]\r\nvulkan_bridge=1\r\n").vulkan_support ||
        !contains(serialize_settings(LauncherSettings{}), "\r\nvulkan_bridge=1\r\n") ||
        contains(serialize_settings(LauncherSettings{}), "vulkan_support")) {
        std::cerr << "vulkan setting migration failed\n";
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
    if (!contains(runtime_ini, "[ofxr]\r\nbackend=fidelityfx") ||
        !contains(runtime_ini, "nvidia_preset=slow") ||
        !contains(runtime_ini, "nvidia_input_scale=25") ||
        !contains(runtime_ini, "nvidia_bidirectional=1") ||
        !contains(runtime_ini, "deep_pipeline=0") ||
        !contains(runtime_ini, "triple_frame_gen=1") ||
        !contains(runtime_ini, "excluded_processes=Home.exe; Other-Shipping.exe") ||
        !contains(runtime_ini, "single_swapchain_rings=0") ||
        !contains(runtime_ini, "vulkan_session_bridge=0") ||
        !contains(runtime_ini, "vulkan_bridge=0") ||
        !contains(runtime_ini, "d3d11_bridge=0") ||
        !contains(runtime_ini, "[diagnostics]\r\nlogging_enabled=1") ||
        contains(runtime_ini, "one_shot") ||
        contains(runtime_ini, "manifest=")) {
        std::cerr << "manual runtime INI contract failed\n";
        return 1;
    }

    const std::filesystem::path runtime_directory =
        L"C:\\Users\\Test\\OFXR Bridge\\RuntimeLayer\\v030";
    const std::filesystem::path implicit_manifest = runtime_directory /
        L"XR_APILAYER_OFXRBridge_manual-42-99.json";
    if (!xrfg::implicit_layer::owned_manifest_path(
            implicit_manifest, runtime_directory) ||
        xrfg::implicit_layer::owned_manifest_path(
            runtime_directory / L"different-layer.json", runtime_directory) ||
        xrfg::implicit_layer::owned_manifest_path(
            runtime_directory.parent_path() /
                L"XR_APILAYER_OFXRBridge_manual-42-99.json",
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
        implicit_manifest, xrfg::implicit_layer::RegistryScope::current_user,
        &registry_error, test_subkey);
    const bool visible_before_probe =
        xrfg::implicit_layer::manifest_registered(
            implicit_manifest, xrfg::implicit_layer::RegistryScope::current_user,
            test_subkey);
    // V017 deliberately has no loader-negotiation consumption path: repeated
    // OpenXR probes must leave the manual arm untouched until tray disarm.
    const bool visible_after_probe =
        xrfg::implicit_layer::manifest_registered(
            implicit_manifest, xrfg::implicit_layer::RegistryScope::current_user,
            test_subkey);
    const bool unregistered = xrfg::implicit_layer::unregister_manifest(
        implicit_manifest, xrfg::implicit_layer::RegistryScope::current_user,
        &registry_error, test_subkey);
    const bool removed = !xrfg::implicit_layer::manifest_registered(
        implicit_manifest, xrfg::implicit_layer::RegistryScope::current_user,
        test_subkey);
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

    // A cached layer some process still has loaded: overwriting it fails,
    // which used to block re-arming. Mapping a copy of this executable as an
    // image is what loading a DLL does to the file - it can be renamed but not
    // overwritten - and runs none of its code.
    const auto loaded_directory =
        std::filesystem::temp_directory_path() /
        (L"ofxr-runtime-loaded-" +
         std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    const auto loaded_source = loaded_directory / L"source.dll";
    const auto loaded_destination = loaded_directory / L"runtime" / L"layer.dll";
    const auto write_file = [](const std::filesystem::path& path, const char* text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    };
    const auto read_file = [](const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    };
    const auto set_aside_count = [&] {
        std::size_t count = 0;
        for (const auto& entry :
             std::filesystem::directory_iterator(loaded_destination.parent_path())) {
            if (entry.path().filename().wstring().rfind(L"layer.dll.old-", 0) == 0) {
                ++count;
            }
        }
        return count;
    };
    wchar_t own_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own_path, MAX_PATH);
    std::filesystem::create_directories(loaded_destination.parent_path());
    std::filesystem::copy_file(own_path, loaded_source,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(own_path, loaded_destination,
                               std::filesystem::copy_options::overwrite_existing);
    const std::string loaded_image = read_file(loaded_destination);
    HMODULE held = LoadLibraryExW(
        loaded_destination.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    const bool identical_while_loaded =
        held != nullptr &&
        install_runtime_layer_dll(loaded_source, loaded_destination) &&
        read_file(loaded_destination) == loaded_image && set_aside_count() == 0;
    write_file(loaded_source, "newer-layer");
    const bool replaced_while_loaded =
        install_runtime_layer_dll(loaded_source, loaded_destination) &&
        read_file(loaded_destination) == "newer-layer" && set_aside_count() == 1;
    // Still loaded, so the copy set aside survives the next install...
    const bool kept_while_loaded =
        install_runtime_layer_dll(loaded_source, loaded_destination) &&
        set_aside_count() == 1;
    if (held != nullptr) FreeLibrary(held);
    // ...and goes at the first install after it was released.
    const bool cleaned_after_release =
        install_runtime_layer_dll(loaded_source, loaded_destination) &&
        set_aside_count() == 0;
    cleanup_error.clear();
    std::filesystem::remove_all(loaded_directory, cleanup_error);
    if (!identical_while_loaded || !replaced_while_loaded ||
        !kept_while_loaded || !cleaned_after_release) {
        std::cerr << "runtime DLL install while loaded failed: identical="
                  << identical_while_loaded << " replaced="
                  << replaced_while_loaded << " kept=" << kept_while_loaded
                  << " cleaned=" << cleaned_after_release << '\n';
        return 1;
    }

    // Complete helper staging is file-only: never load or register a runtime.
    const auto support_fixture = std::filesystem::temp_directory_path() /
        (L"ofxr-runtime-support-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    const auto support_source = support_fixture / L"package";
    const auto support_cache = support_fixture / L"cache";
    std::filesystem::create_directories(support_source);
    std::wstring support_error;
    if (stage_runtime_support_files(support_source, support_cache, &support_error) ||
        support_error.find(L"ofxr_amd_fidelityfx_framegeneration_dx12.dll") == std::wstring::npos ||
        std::filesystem::exists(support_cache)) return 1;
    const wchar_t* required_support[]{L"ofxr_amd_fidelityfx_framegeneration_dx12.dll",
        L"PDAFWPlugin.dll", L"nvngx.dll_dlssnr.dll", L"nvngx.dll_dlssg.dll"};
    for (const auto* name : required_support) write_file(support_source / name, "required-helper");
    if (!stage_runtime_support_files(support_source, support_cache, &support_error) ||
        std::filesystem::exists(support_cache / L"nvngx_dlssnr.dll")) return 1;
    for (const auto* name : required_support)
        if (read_file(support_cache / name) != "required-helper") return 1;
    write_file(support_source / L"nvngx_dlssnr.dll", "user-supplied-NR");
    if (!stage_runtime_support_files(support_source, support_cache, &support_error) ||
        read_file(support_cache / L"nvngx_dlssnr.dll") != "user-supplied-NR") return 1;
    for (const auto* name : required_support) {
        std::filesystem::remove(support_source / name);
        std::filesystem::remove(support_cache / name);
    }
    std::filesystem::remove(support_source / L"nvngx_dlssnr.dll");
    std::filesystem::remove(support_cache / L"nvngx_dlssnr.dll");
    std::filesystem::remove(support_source);
    std::filesystem::remove(support_cache);
    std::filesystem::remove(support_fixture);

    if (quote_windows_argument(L"C:\\Game\\game.exe") !=
            L"C:\\Game\\game.exe" ||
        quote_windows_argument(L"C:\\My Game\\game.exe") !=
            L"\"C:\\My Game\\game.exe\"" ||
        quote_windows_argument(L"C:\\Path With Space\\") !=
            L"\"C:\\Path With Space\\\\\"") {
        std::cerr << "Windows argument quoting failed\n";
        return 1;
    }

    // status.json: the top-level keys, null without a session, no settings
    // section, the running session's multiplier over the setting's, escaping,
    // null for a rate or latency not measured.
    {
        StatusInputs idle;
        idle.armed = true;
        idle.build = 710;
        idle.updated = "2026-10-28T09:12:03.512Z";
        const std::string without = build_status_json(idle);
        StatusInputs running = idle;
        running.paused = true;
        running.settings.triple_frame_gen = false;
        running.session_present = true;
        running.session.pid = 23456;
        running.session.process = "Flight\"Sim\\2024.exe";
        running.session.runtime = "SteamVR";
        running.session.multiplier = 3;
        running.session.generating = true;
        running.session.app_fps = 29.84f;
        running.session.output_fps = 89.36f;
        running.session.display_hz = 90.0f;
        running.session.latency_ms = 41.26f;
        running.session.latency_valid = true;
        const std::string with = build_status_json(running);
        StatusInputs unmeasured = running;
        unmeasured.session.rates_valid = unmeasured.session.output_valid = unmeasured.session.latency_valid = false;
        const std::string without_rates = build_status_json(unmeasured);
        if (!contains(without, "\"schema\": 1,") || !contains(without, "\"armed\": true,") ||
            !contains(without, "\"paused\": false,") ||
            !contains(without, "\"multiplier\": 2,") ||
            !contains(without, "\"generating\": null,") ||
            !contains(without, "\"output_fps\": null,") ||
            !contains(without, "\"latency_ms\": null,") ||
            !contains(without, "\"build\": 710") ||
            !contains(without, "\"updated\": \"2026-10-28T09:12:03.512Z\"") ||
            !contains(with, "\"paused\": true,") || !contains(with, "\"multiplier\": 3,") ||
            !contains(with, "\"generating\": true,") ||
            !contains(with, "\"app_fps\": 29.8,") ||
            !contains(with, "\"output_fps\": 89.4,") ||
            !contains(with, "\"display_hz\": 90.0,") ||
            !contains(with, "\"latency_ms\": 41.3,") ||
            !contains(with, "\"process\": \"Flight\\\"Sim\\\\2024.exe\"") ||
            !contains(with, "\"pid\": 23456,") ||
            !contains(without_rates, "\"app_fps\": null,") ||
            !contains(without_rates, "\"output_fps\": null,") ||
            !contains(without_rates, "\"latency_ms\": null,") ||
            !contains(without_rates, "\"display_hz\": 90.0,") ||
            contains(without, "settings") || contains(with, "settings") ||
            !contains(without, "\"session\": null\n}\n") ||
            !contains(with, "\"deep_pipeline\": false\n  }\n}\n")) {
            std::cerr << "status.json was not as documented\n";
            return 1;
        }
    }

    std::cout << "OFXR manual persistent launcher support tests passed\n";
    return 0;
}
