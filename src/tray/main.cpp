#include "xrfg/implicit_layer.hpp"
#include "xrfg/standalone_launcher.hpp"
#include "xrfg/rife_runtime_locator.hpp"
#include "xrfg/rife_setup.hpp"
#include "xrfg/autonomous_control.hpp"
#include "xrfg/desktop_profiles.hpp"
#include "xrfg/autonomous_ui.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/presented_output_rate.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(linker,                                                     \
    "\"/manifestdependency:type='win32' "                                  \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

namespace {

constexpr wchar_t kWindowClass[] = L"OFXRBridgeTrayWindow";
#define XRFG_STRING_IMPL(x) #x
#define XRFG_STRING(x) XRFG_STRING_IMPL(x)
#define XRFG_WIDE_IMPL(x) L##x
#define XRFG_WIDE(x) XRFG_WIDE_IMPL(x)
constexpr wchar_t kApplicationName[] = L"OFXR-BRIDGE 0.3.3 (v" XRFG_WIDE(XRFG_STRING(XRFG_IMPLEMENTATION_VERSION)) L")";
constexpr wchar_t kCleanupArgument[] = L"--cleanup-manual-arm";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayId = 1;
constexpr UINT_PTR kBenchmarkTimer = 558;
constexpr UINT_PTR kRifeSetupTimer = 837;
constexpr int kPauseHotkeyId = 1;
constexpr UINT kArmPollMilliseconds = 250;
constexpr std::uint32_t kImplementationVersion = XRFG_IMPLEMENTATION_VERSION;
// One Donate entry each, creator first, as in the About box and the README.
constexpr wchar_t kDonateCreatorUrl[] = L"https://ko-fi.com/tig3rmast3r";
constexpr wchar_t kDonateMaintainerUrl[] = L"https://ko-fi.com/djules";
// The pause symbol beside the menu's "Resume frame generation", orange so a
// glance at the menu says generation is off. The FPS overlay's pause symbol
// is the same colour.
constexpr COLORREF kPausedColor = RGB(230, 115, 0);

enum MenuCommand : UINT {
    toggle_arm = 90,
    toggle_pause = 91,
    configure_keybinds = 92,
    check_rife = 95,
    steamvr_queue_separate = 93,
    steamvr_queue_shared = 94,
    toggle_benchmark_mode = 96,
    pacing_auto = 97,
    pacing_vdxr = 110,
    pacing_steamvr = 111,
    toggle_crash_debug = 98,
    show_settings = 100,
    open_logs = 130,
    donate_creator = 138,
    donate_maintainer = 139,
    show_about = 140,
    exit_application = 150,
};

struct AppState {
    HWND window{};
    NOTIFYICONDATAW icon{};
    xrfg::standalone::LauncherSettings settings;
    std::filesystem::path executable_directory;
    std::filesystem::path local_directory;
    std::filesystem::path settings_path;
    std::filesystem::path armed_manifest;
    // The Vulkan layer's manifest, registered beside the OpenXR one while
    // Vulkan support is on; empty otherwise.
    std::filesystem::path armed_vulkan_manifest;
    xrfg::implicit_layer::RegistryScope armed_scope{
        xrfg::implicit_layer::RegistryScope::current_user};
    HANDLE arm_signal{};
    // Set while "Pause frame generation" is on. Made with the arm and closed
    // with it, so every arm starts resumed; null if it could not be made.
    HANDLE pause_signal{};
    bool paused{};
    // The `pause_hotkey` of tray.ini is held system-wide, while armed only.
    bool pause_hotkey_registered{};
    // Alternate namespace used by lifecycle tests, never read from user INI.
    std::wstring registry_subkey{xrfg::implicit_layer::kRegistrySubkey};
    HICON armed_icon{};
    HICON disarmed_icon{};
    UINT taskbar_created_message{};
    bool armed{};
    bool status_export_active{};
    std::wstring crash_debug_status;
    struct RifeOperation {
        HANDLE process{};
        bool installing{};
        ULONGLONG started{};
        std::filesystem::path result_path, console_log;
    } rife;
};

[[nodiscard]] std::wstring last_error_message(std::wstring_view action) {
    const DWORD code = GetLastError();
    wchar_t* system_message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&system_message),
        0,
        nullptr);
    std::wstring output(action);
    output += L" failed (" + std::to_wstring(code) + L")";
    if (length > 0 && system_message != nullptr) {
        output += L": ";
        output.append(system_message, length);
        LocalFree(system_message);
    }
    return output;
}

[[nodiscard]] std::filesystem::path executable_directory() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("GetModuleFileNameW failed");
    }
    return std::filesystem::path(path.data()).parent_path();
}

[[nodiscard]] std::filesystem::path local_app_data() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) {
        throw std::runtime_error("SHGetKnownFolderPath failed");
    }
    const std::filesystem::path output(value);
    CoTaskMemFree(value);
    return output;
}

[[nodiscard]] std::filesystem::path runtime_directory(
    const std::filesystem::path& local_directory) {
    return xrfg::standalone::runtime_version_directory(
        local_directory,
        kImplementationVersion);
}

[[nodiscard]] bool write_text_atomic(
    const std::filesystem::path& path,
    std::string_view text,
    std::wstring* error) {
    try {
        std::filesystem::create_directories(path.parent_path());
        const std::filesystem::path temporary = path.wstring() + L".tmp";
        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.flush();
            if (!stream) {
                if (error) *error = L"Unable to write " + temporary.wstring();
                return false;
            }
        }
        if (!MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            if (error) *error = last_error_message(L"Saving " + path.wstring());
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to save " + path.wstring();
        return false;
    }
}

void load_settings(AppState& state) {
    std::ifstream stream(state.settings_path, std::ios::binary);
    if (!stream) {
        return;
    }
    std::ostringstream text;
    text << stream.rdbuf();
    state.settings = xrfg::standalone::parse_settings(text.str());
}

void save_settings(const AppState& state) {
    static_cast<void>(xrfg::autonomous::write_configuration_sections(
        state.settings_path, xrfg::standalone::serialize_settings(state.settings)));
}

void show_error(HWND owner, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
}

void remove_manifest_file(const std::filesystem::path& manifest) {
    std::error_code ignored;
    std::filesystem::remove(manifest, ignored);
}

void log_lifecycle(const std::filesystem::path& local_directory,
                   std::wstring_view action, std::wstring_view error = {}) noexcept {
    try {
        std::filesystem::create_directories(local_directory);
        const auto path = local_directory / L"tray-lifecycle.log";
        std::error_code size_error;
        const auto size = std::filesystem::file_size(path, size_error);
        std::wofstream stream(path, !size_error && size > 262144
            ? std::ios::trunc : std::ios::app);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        stream << time.wYear << L'-' << time.wMonth << L'-' << time.wDay << L' '
               << time.wHour << L':' << time.wMinute << L':' << time.wSecond
               << L" pid=" << GetCurrentProcessId() << L" V" << kImplementationVersion
               << L' ' << action << (error.empty() ? L" OK" : L" FAILED: ") << error << L'\n';
    } catch (...) { /* Cleanup must not depend on diagnostic I/O. */ }
}

[[nodiscard]] bool write_runtime_configuration(
    const AppState& state,
    std::wstring* error,
    const std::filesystem::path& arm_manifest = {}) {
    const auto& manifest = arm_manifest.empty() ? state.armed_manifest : arm_manifest;
    // The single user INI owns diagnostics limits; runtime files are generated.
    const auto destination = runtime_directory(state.local_directory) / L"ofxr_bridge.ini";
    const auto source = state.settings_path;
    const unsigned max_file_mb = GetPrivateProfileIntW(
        L"diagnostics",
        L"max_file_mb",
        static_cast<INT>(xrfg::standalone::kDefaultMaxFileMb),
        source.empty() ? destination.c_str() : source.c_str());
    const bool flush_each_event = GetPrivateProfileIntW(
        L"diagnostics",
        L"flush_each_event",
        0,
        source.empty() ? destination.c_str() : source.c_str()) != 0;
    std::string configuration = xrfg::standalone::build_runtime_ini(
        state.settings, xrfg::autonomous::snapshot().desired, max_file_mb, flush_each_event);
    if (!manifest.empty()) {
        const auto control = xrfg::implicit_layer::arm_signal_name(manifest);
        std::string ascii_control;
        for (const wchar_t c : control) ascii_control.push_back(static_cast<char>(c));
        configuration.insert(std::string("[ofxr]\r\n").size(),
            "control_event=" + ascii_control + "\r\n");
    }
    return write_text_atomic(destination, configuration, error);
}

// `x86_manifest` comes back set when the package carries the 32-bit layer
// (0.2 V576) and it was prepared; its failure is logged, never the arm's.
[[nodiscard]] bool prepare_runtime_layer(
    AppState& state,
    std::filesystem::path* manifest,
    std::filesystem::path* vulkan_manifest,
    std::filesystem::path* x86_manifest,
    std::wstring* error) {
    if (vulkan_manifest) vulkan_manifest->clear();
    if (x86_manifest) x86_manifest->clear();
    try {
        const auto source_dll = state.executable_directory / L"ofxr" /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!std::filesystem::is_regular_file(source_dll)) {
            if (error) {
                *error = L"The bundled OFXR layer is missing:\r\n" +
                         source_dll.wstring();
            }
            return false;
        }
        const auto directory = runtime_directory(state.local_directory);
        std::filesystem::create_directories(directory);
        const auto runtime_dll = directory /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (!xrfg::standalone::install_runtime_layer_dll(
                source_dll, runtime_dll)) {
            if (error) *error = last_error_message(L"Installing the runtime layer");
            return false;
        }
#ifdef XRFG_NATIVE_DLSSG
        const auto source_dlss = state.executable_directory / L"ofxr" / L"nvngx_dlssg.dll";
        if (!xrfg::standalone::install_runtime_layer_dll(source_dlss, directory / L"nvngx_dlssg.dll")) {
            if (error) *error = L"Installing the bundled NVIDIA DLSS Frame Generation runtime failed.";
            return false;
        }
#endif

        if (!xrfg::standalone::stage_runtime_support_files(
                state.executable_directory / L"ofxr", directory, error)) return false;

        static std::uint64_t last_arm_id = 0;
        last_arm_id = std::max<std::uint64_t>(last_arm_id + 1, GetTickCount64());
        const std::wstring manifest_name =
            std::wstring(xrfg::implicit_layer::kManifestPrefix) +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(last_arm_id) +
            xrfg::implicit_layer::kManifestSuffix;
        const auto generated_manifest = directory / manifest_name;
        *manifest = generated_manifest;
        state.arm_signal = xrfg::implicit_layer::create_arm_signal(generated_manifest, error);
        if (!state.arm_signal) return false;
        // Not worth refusing the arm over: without it the menu entry is grey.
        std::wstring pause_error;
        state.pause_signal = xrfg::implicit_layer::create_pause_signal(
            generated_manifest, &pause_error);
        state.paused = false;
        if (!state.pause_signal)
            log_lifecycle(state.local_directory, L"pause-control", pause_error);
        if (!write_text_atomic(
                generated_manifest,
                xrfg::standalone::build_implicit_layer_manifest(
                    runtime_dll, kImplementationVersion),
                error) ||
            !write_runtime_configuration(state, error, generated_manifest)) {
            remove_manifest_file(generated_manifest);
            return false;
        }
        *manifest = generated_manifest;
        if (state.settings.vulkan_support && vulkan_manifest) {
            // The queue-serialising Vulkan layer goes beside the OpenXR one,
            // in the same version folder, with a manifest of the same shape.
            const auto source_vulkan_dll = state.executable_directory / L"ofxr" /
                xrfg::implicit_layer::kVulkanLayerDll;
            if (!std::filesystem::is_regular_file(source_vulkan_dll)) {
                if (error) {
                    *error = L"The bundled OFXR Vulkan layer is missing:\r\n" +
                             source_vulkan_dll.wstring();
                }
                remove_manifest_file(generated_manifest);
                return false;
            }
            const auto runtime_vulkan_dll =
                directory / xrfg::implicit_layer::kVulkanLayerDll;
            if (!xrfg::standalone::install_runtime_layer_dll(
                    source_vulkan_dll, runtime_vulkan_dll)) {
                if (error) *error = last_error_message(L"Installing the Vulkan layer");
                remove_manifest_file(generated_manifest);
                return false;
            }
            const std::wstring vulkan_manifest_name =
                std::wstring(xrfg::implicit_layer::kVulkanManifestPrefix) +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(last_arm_id) +
                xrfg::implicit_layer::kManifestSuffix;
            const auto generated_vulkan_manifest = directory / vulkan_manifest_name;
            if (!write_text_atomic(
                    generated_vulkan_manifest,
                    xrfg::standalone::build_vulkan_layer_manifest(
                        runtime_vulkan_dll, kImplementationVersion),
                    error)) {
                remove_manifest_file(generated_manifest);
                return false;
            }
            *vulkan_manifest = generated_vulkan_manifest;
        }
        // 0.2 V576: the 32-bit layer beside the 64-bit one, sharing its ini.
        const auto source_x86_dll = state.executable_directory /
            xrfg::implicit_layer::kX86BundleDirectory /
            L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        if (x86_manifest && std::filesystem::is_regular_file(source_x86_dll)) {
            const auto runtime_x86_dll = directory / xrfg::implicit_layer::kX86LayerDllName;
            const auto generated_x86_manifest =
                xrfg::implicit_layer::x86_manifest_for(generated_manifest);
            std::wstring x86_error;
            if (!xrfg::standalone::install_runtime_layer_dll(source_x86_dll, runtime_x86_dll)) {
                log_lifecycle(state.local_directory, L"x86-layer",
                    last_error_message(L"Installing the 32-bit layer"));
            } else if (!write_text_atomic(generated_x86_manifest,
                           xrfg::standalone::build_implicit_layer_manifest(
                               runtime_x86_dll, kImplementationVersion),
                           &x86_error)) {
                log_lifecycle(state.local_directory, L"x86-layer", x86_error);
            } else {
                *x86_manifest = generated_x86_manifest;
            }
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to prepare the manual OpenXR layer.";
        return false;
    }
}

// 0.2 V576: registers the arm's 32-bit manifest, prepared by
// prepare_runtime_layer. Per-user only: a machine-wide arm is in HKLM's
// 64-bit view, which a 32-bit loader does not read. A failure is logged and
// leaves the 64-bit arm as it is.
void register_x86_manifest(
    const AppState& state,
    const std::filesystem::path& x86_manifest,
    xrfg::implicit_layer::RegistryScope scope) {
    if (x86_manifest.empty()) return;
    std::wstring x86_error;
    if (scope != xrfg::implicit_layer::RegistryScope::current_user) {
        x86_error = L"Not armed for 32-bit games: the arm is machine-wide.";
    } else if (xrfg::implicit_layer::register_manifest(
                   x86_manifest, scope, &x86_error, state.registry_subkey)) {
        return;
    }
    log_lifecycle(state.local_directory, L"x86-layer", x86_error);
    std::wstring ignored;
    static_cast<void>(xrfg::implicit_layer::retire_manifest(
        x86_manifest, scope, &ignored, state.registry_subkey));
}

[[nodiscard]] bool spawn_cleanup_helper(
    const AppState& state,
    const std::filesystem::path& manifest,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring* error,
    const std::filesystem::path& vulkan_manifest = {}) {
    std::wstring command = xrfg::standalone::quote_windows_argument(
        (state.executable_directory / L"OFXRBridgeTray.exe").wstring());
    command += L" ";
    command += kCleanupArgument;
    command += L" ";
    command += xrfg::standalone::quote_windows_argument(manifest.wstring());
    command += L" ";
    command += std::to_wstring(GetCurrentProcessId());
    command += L" ";
    command += xrfg::implicit_layer::registry_scope_name(scope);
    if (!vulkan_manifest.empty()) {
        command += L" ";
        command += xrfg::standalone::quote_windows_argument(vulkan_manifest.wstring());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto tray_executable =
        state.executable_directory / L"OFXRBridgeTray.exe";
    if (!CreateProcessW(
            tray_executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            state.executable_directory.c_str(),
            &startup,
            &process)) {
        if (error) *error = last_error_message(L"Starting the cleanup watchdog");
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

[[nodiscard]] bool cleanup_all_owned_registrations(
    const AppState& state,
    std::wstring* error) {
    if (error) error->clear();
    bool success = true;
    for (const auto scope : {
             xrfg::implicit_layer::RegistryScope::current_user,
             xrfg::implicit_layer::RegistryScope::local_machine}) {
        std::wstring detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &detail, state.registry_subkey)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L": ";
                *error += detail;
            }
        }
        // The Vulkan layer's registrations, in the loader's own key.
        std::wstring vulkan_detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &vulkan_detail,
                xrfg::implicit_layer::kVulkanRegistrySubkey,
                xrfg::implicit_layer::kVulkanManifestPrefix)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L" (Vulkan): ";
                *error += vulkan_detail;
            }
        }
        // 0.2 V576: the 32-bit layer's, in the OpenXR key.
        std::wstring x86_detail;
        if (!xrfg::implicit_layer::cleanup_owned_registrations(
                state.local_directory, scope, &x86_detail, state.registry_subkey,
                xrfg::implicit_layer::kX86ManifestPrefix)) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += xrfg::implicit_layer::registry_scope_name(scope);
                *error += L" (32-bit): ";
                *error += x86_detail;
            }
        }
    }
    return success;
}

[[nodiscard]] std::wstring tray_tooltip(const AppState& state) {
    std::wstring tooltip = std::wstring(kApplicationName) + (state.paused ? L" - paused"
        : state.armed ? L" - active" : L" - inactive");
    const auto desired = xrfg::autonomous::snapshot().desired;
    if (desired.frame_generation == 2) tooltip += L" - DLSS FG";
    else if (desired.frame_generation == 1) tooltip += L" - FSR 3.1";
    else if (desired.frame_generation == 3) tooltip += L" - Hybrid";
    else if (desired.frame_generation == 4) tooltip += L" - RIFE";
    else {
        const wchar_t* engines[]{L" - FidelityFX", L" - NVIDIA reference", L" - NVIDIA new", L" - NVIDIA djules"};
        tooltip += engines[std::clamp(desired.backend, 0, 3)];
    }
    return tooltip;
}

void refresh_tray_icon(AppState& state) {
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    state.icon.uFlags = NIF_ICON | NIF_TIP;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

void show_balloon(
    AppState& state,
    std::wstring_view title,
    std::wstring_view message,
    DWORD flags = NIIF_INFO) {
    wcsncpy_s(state.icon.szInfoTitle, std::wstring(title).c_str(), _TRUNCATE);
    wcsncpy_s(state.icon.szInfo, std::wstring(message).c_str(), _TRUNCATE);
    state.icon.dwInfoFlags = flags;
    state.icon.uFlags = NIF_INFO;
    static_cast<void>(Shell_NotifyIconW(NIM_MODIFY, &state.icon));
}

[[nodiscard]] bool add_tray_icon(AppState& state) {
    state.icon = {};
    state.icon.cbSize = sizeof(state.icon);
    state.icon.hWnd = state.window;
    state.icon.uID = kTrayId;
    state.icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    state.icon.uCallbackMessage = kTrayMessage;
    state.icon.hIcon = state.armed ? state.armed_icon : state.disarmed_icon;
    const std::wstring tooltip = tray_tooltip(state);
    wcsncpy_s(state.icon.szTip, tooltip.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_ADD, &state.icon) != FALSE;
}

// Takes tray.ini's `pause_hotkey` for as long as the bridge is armed. The key
// is the tray's and not the layer's: nothing is added to the game, and the
// same chord works in every title. A key another program already holds is
// left to it; the menu entry still works.
void register_pause_hotkey(AppState& state) {
    if (state.pause_hotkey_registered || state.window == nullptr ||
        state.pause_signal == nullptr) return;
    const auto hotkey = xrfg::standalone::runtime_keybinds(state.settings)[xrfg::KeybindAction::framegen];
    if (!hotkey.key) return;
    state.pause_hotkey_registered = RegisterHotKey(
        state.window, kPauseHotkeyId, hotkey.modifiers | MOD_NOREPEAT,
        hotkey.key) != FALSE;
    if (!state.pause_hotkey_registered)
        log_lifecycle(state.local_directory, L"pause-hotkey",
            last_error_message(L"Registering the pause key"));
}

void unregister_pause_hotkey(AppState& state) {
    if (!state.pause_hotkey_registered) return;
    UnregisterHotKey(state.window, kPauseHotkeyId);
    state.pause_hotkey_registered = false;
}

// Ends the pause with the arm it belonged to. A running session keeps its own
// handle, and by now the arm signal has stopped it for good.
void close_pause_signal(AppState& state) {
    unregister_pause_hotkey(state);
    if (state.pause_signal) {
        CloseHandle(state.pause_signal);
        state.pause_signal = nullptr;
    }
    state.paused = false;
}

[[nodiscard]] bool disarm_bridge(
    AppState& state,
    std::wstring* error) {
    // Signal the active DLL BEFORE registry/file cleanup or window teardown.
    std::wstring signal_error;
    if (state.arm_signal && !SetEvent(state.arm_signal))
        signal_error = last_error_message(L"Stopping the loaded OFXR layer");
    std::wstring detail;
    const bool cleaned = cleanup_all_owned_registrations(state, &detail);
    if (!signal_error.empty()) {
        if (!detail.empty()) detail += L"\r\n";
        detail += signal_error;
    }
    if (!cleaned || !signal_error.empty()) {
        log_lifecycle(state.local_directory, L"disarm-all", detail);
        if (error) *error = detail;
        return false;
    }
    log_lifecycle(state.local_directory, L"disarm-all");
    if (state.arm_signal) {
        CloseHandle(state.arm_signal);
        state.arm_signal = nullptr;
    }
    close_pause_signal(state);
    state.armed = false;
    state.armed_manifest.clear();
    state.armed_vulkan_manifest.clear();
    state.armed_scope = xrfg::implicit_layer::RegistryScope::current_user;
    refresh_tray_icon(state);
    return true;
}

[[nodiscard]] bool arm_bridge(AppState& state, std::wstring* error) {
    // Reconcile disk/registry state, not just this tray process's memory.
    if (!disarm_bridge(state, error)) return false;

    std::filesystem::path manifest;
    std::filesystem::path vulkan_manifest;
    std::filesystem::path x86_manifest;
    const auto scope = xrfg::implicit_layer::preferred_registry_scope();
    if (!prepare_runtime_layer(state, &manifest, &vulkan_manifest, &x86_manifest, error) ||
        !xrfg::implicit_layer::register_manifest(
            manifest, scope, error, state.registry_subkey) ||
        (!vulkan_manifest.empty() &&
         !xrfg::implicit_layer::register_manifest(
             vulkan_manifest, scope, error,
             xrfg::implicit_layer::kVulkanRegistrySubkey))) {
        if (!vulkan_manifest.empty()) {
            std::wstring cleanup_error;
            if (!xrfg::implicit_layer::retire_manifest(
                    vulkan_manifest, scope, &cleanup_error,
                    xrfg::implicit_layer::kVulkanRegistrySubkey))
                log_lifecycle(state.local_directory, L"arm-failure-cleanup", cleanup_error);
        }
        if (!manifest.empty()) {
            std::wstring cleanup_error;
            if (!xrfg::implicit_layer::retire_manifest(
                    manifest, scope, &cleanup_error, state.registry_subkey))
                log_lifecycle(state.local_directory, L"arm-failure-cleanup", cleanup_error);
        }
        if (!x86_manifest.empty()) remove_manifest_file(x86_manifest);
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        close_pause_signal(state);
        return false;
    }
    // The 32-bit manifest is registered only once the helper runs, which
    // retires it with the arm.
    if (!spawn_cleanup_helper(state, manifest, scope, error, vulkan_manifest)) {
        if (!x86_manifest.empty()) remove_manifest_file(x86_manifest);
        std::wstring ignored;
        if (!vulkan_manifest.empty()) {
            static_cast<void>(xrfg::implicit_layer::retire_manifest(
                vulkan_manifest, scope, &ignored,
                xrfg::implicit_layer::kVulkanRegistrySubkey));
        }
        static_cast<void>(xrfg::implicit_layer::retire_manifest(
            manifest, scope, &ignored, state.registry_subkey));
        if (state.arm_signal) {
            SetEvent(state.arm_signal);
            CloseHandle(state.arm_signal);
            state.arm_signal = nullptr;
        }
        close_pause_signal(state);
        return false;
    }
    register_x86_manifest(state, x86_manifest, scope);
    state.armed = true;
    register_pause_hotkey(state);
    state.armed_manifest = manifest;
    state.armed_scope = scope;
    log_lifecycle(state.local_directory,
        scope == xrfg::implicit_layer::RegistryScope::local_machine
            ? L"arm-hklm"
            : L"arm-hkcu");
    refresh_tray_icon(state);
    show_balloon(
        state,
        L"OFXR 0.3.3 armed",
        L"The bridge will remain enabled for OpenXR applications until you disarm it or exit the tray.");
    return true;
}

[[nodiscard]] std::wstring key_display(std::string_view setting) {
    const std::string name = xrfg::standalone::hotkey_display_name(setting);
    if (name.empty()) return L"none";
    return std::wstring(name.begin(), name.end());
}

// The dialog edits a draft; only OK publishes and saves the six bindings.
struct KeybindDialog {
    std::array<std::string,xrfg::kKeybindCount> settings;
    int capturing{-1};
    bool updating{};
};

// The hotkey control's value for a chord, and back. Its modifier flags are
// not RegisterHotKey's, and it marks the navigation keys as extended.
[[nodiscard]] WORD hotkey_control_value(const xrfg::standalone::Hotkey& hotkey) {
    BYTE flags = 0;
    if (hotkey.modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (hotkey.modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (hotkey.modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    switch (hotkey.key) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_DIVIDE: case VK_NUMLOCK:
        flags |= HOTKEYF_EXT;
        break;
    default:
        break;
    }
    return MAKEWORD(static_cast<BYTE>(hotkey.key), flags);
}

[[nodiscard]] xrfg::standalone::Hotkey hotkey_from_control(WORD value) {
    xrfg::standalone::Hotkey hotkey;
    hotkey.key = LOBYTE(value);
    const BYTE flags = HIBYTE(value);
    if (flags & HOTKEYF_SHIFT) hotkey.modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_CONTROL) hotkey.modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) hotkey.modifiers |= MOD_ALT;
    return hotkey;
}

constexpr std::array<const wchar_t*,xrfg::kKeybindCount> kKeybindNames{L"Open interface",L"Toggle NR",L"Toggle framegen",L"Enable logging",L"Dump (debug)",L"Toggle FPS counter"};

void show_keybind_row(HWND dialog,KeybindDialog& state,unsigned row) {
    ShowWindow(GetDlgItem(dialog,IDC_KEYBIND_CAPTURE+row),SW_HIDE);
    SetDlgItemTextW(dialog,IDC_KEYBIND_VALUE+row,key_display(state.settings[row]).c_str());
    ShowWindow(GetDlgItem(dialog,IDC_KEYBIND_VALUE+row),SW_SHOW);
    SetDlgItemTextW(dialog,IDC_KEYBIND_CHANGE+row,L"Change");
}
void begin_keybind_capture(HWND dialog,KeybindDialog& state,unsigned row) {
    if(state.capturing>=0)show_keybind_row(dialog,state,unsigned(state.capturing));
    state.capturing=int(row);state.updating=true;
    const HWND box=GetDlgItem(dialog,IDC_KEYBIND_CAPTURE+row);
    SendMessageW(box,HKM_SETRULES,0,0);SendMessageW(box,HKM_SETHOTKEY,0,0);
    state.updating=false;
    ShowWindow(GetDlgItem(dialog,IDC_KEYBIND_VALUE+row),SW_HIDE);
    ShowWindow(box,SW_SHOW);SetFocus(box);
    SetDlgItemTextW(dialog,IDC_KEYBIND_CHANGE+row,L"Listening...");
    SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,(std::wstring(L"Press a key or combination for ")+kKeybindNames[row]+L".").c_str());
}
INT_PTR CALLBACK keybind_dialog_procedure(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<KeybindDialog*>(GetWindowLongPtrW(dialog,DWLP_USER));
    switch(message) {
    case WM_INITDIALOG:
        state=reinterpret_cast<KeybindDialog*>(lparam);SetWindowLongPtrW(dialog,DWLP_USER,lparam);
        for(unsigned i=0;i<xrfg::kKeybindCount;++i)show_keybind_row(dialog,*state,i);
        xrfg::autonomous::suspend_keybinds(true);SetTimer(dialog,1,500,nullptr);
        return TRUE;
    case WM_TIMER:
        xrfg::autonomous::suspend_keybinds(true);return TRUE;
    case WM_DESTROY:
        KillTimer(dialog,1);xrfg::autonomous::suspend_keybinds(false);return TRUE;
    case WM_COMMAND: {
        if(!state)return FALSE;
        const auto id=LOWORD(wparam);
        if(id>=IDC_KEYBIND_CHANGE && id<IDC_KEYBIND_CHANGE+xrfg::kKeybindCount) {
            begin_keybind_capture(dialog,*state,id-IDC_KEYBIND_CHANGE);return TRUE;
        }
        if(id>=IDC_KEYBIND_CLEAR && id<IDC_KEYBIND_CLEAR+xrfg::kKeybindCount) {
            if(state->capturing>=0)show_keybind_row(dialog,*state,unsigned(state->capturing));
            state->capturing=-1;const unsigned row=id-IDC_KEYBIND_CLEAR;
            state->settings[row]="off";show_keybind_row(dialog,*state,row);return TRUE;
        }
        if(id>=IDC_KEYBIND_CAPTURE && id<IDC_KEYBIND_CAPTURE+xrfg::kKeybindCount && HIWORD(wparam)==EN_CHANGE) {
            const unsigned row=id-IDC_KEYBIND_CAPTURE;
            if(state->updating || state->capturing!=int(row))return TRUE;
            const WORD value=LOWORD(SendDlgItemMessageW(dialog,id,HKM_GETHOTKEY,0,0));
            if(!LOBYTE(value))return TRUE;
            const auto text=xrfg::standalone::hotkey_setting(hotkey_from_control(value));
            if(!text){SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,L"That key cannot be used. Choose another one.");return TRUE;}
            state->settings[row]=*text;state->capturing=-1;show_keybind_row(dialog,*state,row);
            SetFocus(GetDlgItem(dialog,IDC_KEYBIND_CHANGE+row));
            SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,L"Binding captured. Click OK to save all changes.");return TRUE;
        }
        if(id==IDC_KEYBIND_DEFAULTS) {
            xrfg::standalone::LauncherSettings defaults;const auto values=xrfg::standalone::keybind_settings(defaults);
            state->capturing=-1;for(unsigned i=0;i<xrfg::kKeybindCount;++i){state->settings[i]=*values[i];show_keybind_row(dialog,*state,i);}
            SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,L"Defaults restored. Click OK to save.");return TRUE;
        }
        if(id==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
        if(id==IDOK) {
            std::array<xrfg::standalone::Hotkey,xrfg::kKeybindCount> keys{};
            for(unsigned i=0;i<xrfg::kKeybindCount;++i) {
                keys[i]=xrfg::standalone::parse_hotkey(state->settings[i]).value_or(xrfg::standalone::Hotkey{});
                for(unsigned j=0;j<i;++j)if(keys[i].key && keys[i]==keys[j]) {
                    SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,(std::wstring(kKeybindNames[i])+L" and "+kKeybindNames[j]+L" use the same binding. Change or clear one.").c_str());return TRUE;
                }
            }
            bool available=true;unsigned failed{};
            for(unsigned i=0;i<xrfg::kKeybindCount;++i)if(keys[i].key && !RegisterHotKey(dialog,0x7f01+i,keys[i].modifiers|MOD_NOREPEAT,keys[i].key)){available=false;failed=i;break;}
            for(unsigned i=0;i<xrfg::kKeybindCount;++i)UnregisterHotKey(dialog,0x7f01+i);
            if(!available){SetDlgItemTextW(dialog,IDC_KEYBIND_STATUS,(std::wstring(kKeybindNames[failed])+L": another program uses that combination. Choose another one.").c_str());return TRUE;}
            EndDialog(dialog,IDOK);return TRUE;
        }
        return FALSE;
    }
    default:return FALSE;
    }
}
bool apply_keybinds(AppState& state,const std::array<std::string,xrfg::kKeybindCount>& bindings) {
    auto updated=state.settings;const auto fields=xrfg::standalone::keybind_settings(updated);
    for(unsigned i=0;i<xrfg::kKeybindCount;++i)*fields[i]=bindings[i];
    if(!xrfg::autonomous::publish_keybinds(xrfg::standalone::runtime_keybinds(updated)))return false;
    if(!xrfg::autonomous::write_configuration_sections(state.settings_path,xrfg::standalone::serialize_settings(updated))) {
        xrfg::autonomous::publish_keybinds(xrfg::standalone::runtime_keybinds(state.settings));return false;
    }
    state.settings=std::move(updated);log_lifecycle(state.local_directory,L"keybinds-changed");return true;
}
void change_keybinds(AppState& state) {
    unregister_pause_hotkey(state);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_HOTKEY_CLASS};InitCommonControlsEx(&controls);
    KeybindDialog dialog;const auto fields=xrfg::standalone::keybind_settings(state.settings);
    for(unsigned i=0;i<xrfg::kKeybindCount;++i)dialog.settings[i]=*fields[i];
    const auto answer=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBINDS),state.window,keybind_dialog_procedure,reinterpret_cast<LPARAM>(&dialog));
    xrfg::autonomous::suspend_keybinds(false);
    if(answer==IDOK && !apply_keybinds(state,dialog.settings))show_error(state.window,L"Unable to save the keybinds. The previous bindings are still active.");
    if(state.armed)register_pause_hotkey(state);
}

struct RifeInstallDialog {
    std::filesystem::path destination;
    std::wstring reason;
};
INT_PTR CALLBACK rife_install_dialog_procedure(HWND dialog,UINT message,WPARAM wparam,LPARAM lparam) {
    if(message==WM_INITDIALOG) {
        auto* state=reinterpret_cast<RifeInstallDialog*>(lparam);
        SetWindowLongPtrW(dialog,DWLP_USER,lparam);
        SetDlgItemTextW(dialog,IDC_RIFE_REASON,state->reason.c_str());
        SetDlgItemTextW(dialog,IDC_RIFE_DESTINATION,state->destination.c_str());
        CheckRadioButton(dialog,IDC_RIFE_NVIDIA,IDC_RIFE_AMD,IDC_RIFE_NVIDIA);
        EnableWindow(GetDlgItem(dialog,IDC_RIFE_AMD),FALSE);
        return TRUE;
    }
    if(message==WM_COMMAND) {
        const auto id=LOWORD(wparam);
        if(id==IDOK) {
            if(IsDlgButtonChecked(dialog,IDC_RIFE_NVIDIA)!=BST_CHECKED)return TRUE;
            EndDialog(dialog,IDOK);return TRUE;
        }
        if(id==IDCANCEL){EndDialog(dialog,IDCANCEL);return TRUE;}
    }
    if(message==WM_CLOSE){EndDialog(dialog,IDCANCEL);return TRUE;}
    return FALSE;
}

void close_rife_process(AppState& state) {
    if(state.rife.process)CloseHandle(state.rife.process);
    state.rife.process=nullptr;
    if(state.window)KillTimer(state.window,kRifeSetupTimer);
}

// Desktop subprocess only: the window thread never waits for Python, CUDA,
// downloads or installation. Check output is retained even on helper startup
// errors; the install has its own visible console and backend log.
bool begin_rife_operation(AppState& state,bool installing,std::wstring* error) {
    if(state.rife.process){if(error)*error=L"A RIFE operation is already running.";return false;}
    HANDLE output{},input{};
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(STARTUPINFOW);
    std::vector<std::byte> attributes;bool attribute_initialized=false;
    PROCESS_INFORMATION process{};
    const auto cleanup=[&]{
        if(attribute_initialized){DeleteProcThreadAttributeList(startup.lpAttributeList);attribute_initialized=false;}
        if(output&&output!=INVALID_HANDLE_VALUE)CloseHandle(output);output=nullptr;
        if(input&&input!=INVALID_HANDLE_VALUE)CloseHandle(input);input=nullptr;
        if(process.hThread){CloseHandle(process.hThread);process.hThread=nullptr;}
        if(process.hProcess){CloseHandle(process.hProcess);process.hProcess=nullptr;}
    };
    try {
        const auto script=state.executable_directory/L"tools/Setup-RifeRuntime.ps1";
        if(!std::filesystem::is_regular_file(script))throw std::runtime_error("Missing tools/Setup-RifeRuntime.ps1. Extract the complete OFXR archive first.");
        const auto root=xrfg::managed_rife_runtime();
        if(root.empty())throw std::runtime_error("Local application data is unavailable.");
        const auto status=state.local_directory/L"RIFE/status";
        std::filesystem::create_directories(status);
        static std::atomic_uint request{};
        const auto name=(installing?L"install-":L"check-")+std::to_wstring(GetCurrentProcessId())+L"-"+
            std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(++request);
        const auto result=status/(name+L".json");
        const auto console=status/(name+L".console.log");
        std::array<wchar_t,32768> system{};
        if(!GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size())))throw std::runtime_error("Cannot locate Windows PowerShell.");
        const auto powershell=std::filesystem::path(system.data())/L"WindowsPowerShell/v1.0/powershell.exe";
        auto command=xrfg::rife_setup::command_line(powershell,script,root,result,!installing);
        DWORD flags=xrfg::rife_setup::process_flags(installing);
        startup.StartupInfo.dwFlags=STARTF_USESHOWWINDOW;startup.StartupInfo.wShowWindow=installing?SW_SHOWNORMAL:SW_HIDE;
        if(!installing) {
            SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
            output=CreateFileW(console.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_DELETE,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(output==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create the RIFE check log.");
            SIZE_T bytes{};InitializeProcThreadAttributeList(nullptr,1,0,&bytes);attributes.resize(bytes);
            startup.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if(!InitializeProcThreadAttributeList(startup.lpAttributeList,1,0,&bytes))throw std::runtime_error("Cannot prepare RIFE check handles.");
            attribute_initialized=true;HANDLE handles[]{output,input};
            if(!UpdateProcThreadAttribute(startup.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))
                throw std::runtime_error("Cannot isolate RIFE check handles.");
            startup.StartupInfo.cb=sizeof(startup);startup.StartupInfo.dwFlags|=STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdOutput=startup.StartupInfo.hStdError=output;startup.StartupInfo.hStdInput=input;
            flags|=EXTENDED_STARTUPINFO_PRESENT;
        }
        if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,installing?FALSE:TRUE,flags,nullptr,
            state.executable_directory.c_str(),&startup.StartupInfo,&process)) {
            const auto message=last_error_message(L"Starting the RIFE helper");cleanup();if(error)*error=message;return false;
        }
        state.rife={process.hProcess,installing,GetTickCount64(),result,installing?std::filesystem::path{}:console};
        process.hProcess=nullptr;cleanup();
        if(state.window)SetTimer(state.window,kRifeSetupTimer,250,nullptr);
        return true;
    } catch(const std::exception& e) {
        cleanup();if(error)*error=xrfg::rife_setup::utf8_to_wide(e.what());return false;
    }
}

struct RifeCompletion {
    bool installing{};
    std::optional<xrfg::rife_setup::Result> result;
    std::wstring error;
    std::filesystem::path fallback_log;
};
std::optional<RifeCompletion> collect_rife_completion(AppState& state) {
    if(!state.rife.process)return {};
    const auto waited=WaitForSingleObject(state.rife.process,0);
    const bool timeout=GetTickCount64()-state.rife.started>=xrfg::rife_setup::operation_timeout(state.rife.installing);
    if(waited==WAIT_TIMEOUT&&!timeout)return {};
    RifeCompletion completion;completion.installing=state.rife.installing;
    completion.fallback_log=state.rife.console_log.empty()?state.rife.result_path.parent_path().parent_path():state.rife.console_log;
    DWORD exit_code=1;
    if(waited!=WAIT_OBJECT_0) {
        completion.error=timeout?L"The RIFE operation timed out. It will not be restarted automatically.":L"Unable to read the RIFE helper status.";
    } else if(!GetExitCodeProcess(state.rife.process,&exit_code)) {
        completion.error=L"Unable to read the RIFE helper exit code.";
    } else {
        try {
            std::ifstream file(state.rife.result_path,std::ios::binary);
            std::array<char,64*1024+1> bytes{};file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
            completion.result=xrfg::rife_setup::parse_result(std::string_view(bytes.data(),static_cast<std::size_t>(file.gcount())),exit_code);
        } catch(...) {}
        if(!completion.result)completion.error=L"The RIFE helper did not return a valid result. No installation was started.";
    }
    // Closing the tray or its monitor never kills an accepted visible install.
    // Hidden health checks have their own bounded backend child cleanup.
    close_rife_process(state);return completion;
}
void report_rife_completion(AppState& state,const RifeCompletion& completion) {
    if(!completion.result) {
        show_error(state.window,completion.error+L"\r\n\r\nDiagnostics: "+completion.fallback_log.wstring());return;
    }
    const auto& result=*completion.result;
    std::wstring text=result.message;
    if(!result.root.empty())text+=L"\r\n\r\nLocation: "+result.root.wstring();
    const auto& log=result.log_path.empty()?completion.fallback_log:result.log_path;
    if(!log.empty())text+=L"\r\n\r\nLog: "+log.wstring();
    if(result.status==xrfg::rife_setup::Status::ready) {
        if(text.empty())text=L"RIFE is ready.";
        MessageBoxW(state.window,text.c_str(),L"Check RIFE",MB_OK|MB_ICONINFORMATION);return;
    }
    if(!completion.installing&&xrfg::rife_setup::offers_install(result.status)) {
        RifeInstallDialog dialog{xrfg::managed_rife_runtime(),result.message};
        const auto answer=DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_RIFE_INSTALL),state.window,
            rife_install_dialog_procedure,reinterpret_cast<LPARAM>(&dialog));
        if(answer==IDOK) {
            std::wstring error;if(!begin_rife_operation(state,true,&error))show_error(state.window,error);
        }
        return;
    }
    MessageBoxW(state.window,text.c_str(),L"Check RIFE",MB_OK|MB_ICONWARNING);
}
void poll_rife_operation(AppState& state) {
    if(auto completion=collect_rife_completion(state))report_rife_completion(state,*completion);
}

void show_context_menu(AppState& state) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, show_settings, L"Settings...");
    SetMenuDefaultItem(menu, show_settings, FALSE);
    AppendMenuW(menu, MF_STRING | (state.armed ? MF_CHECKED : MF_UNCHECKED),
        toggle_arm, L"Enable");
    AppendMenuW(menu, MF_STRING | (state.armed ? MF_UNCHECKED : MF_CHECKED),
        toggle_arm, L"Disable");
    AppendMenuW(menu, MF_STRING, configure_keybinds, L"Configure keybinds...");
    AppendMenuW(menu,MF_STRING | (state.rife.process?MF_GRAYED:0),check_rife,
        state.rife.process?(state.rife.installing?L"RIFE installation running...":L"Checking RIFE..."):L"Check RIFE...");
    HMENU pacing_menu=CreatePopupMenu();
    if(pacing_menu) {
        using xrfg::standalone::RuntimePacing;
        AppendMenuW(pacing_menu,MF_STRING | (state.settings.pacing==RuntimePacing::automatic?MF_CHECKED:0),pacing_auto,L"Auto");
        AppendMenuW(pacing_menu,MF_STRING | (state.settings.pacing==RuntimePacing::force_vdxr?MF_CHECKED:0),pacing_vdxr,L"Force VDXR");
        AppendMenuW(pacing_menu,MF_STRING | (state.settings.pacing==RuntimePacing::force_steamvr?MF_CHECKED:0),pacing_steamvr,L"Force SteamVR");
        AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(pacing_menu),L"Pacing (restart game)");
    }
    AppendMenuW(menu,MF_STRING | (state.settings.crash_debug ? MF_CHECKED : MF_UNCHECKED),
        toggle_crash_debug,L"Crash debug (restart game)");
    if(!state.crash_debug_status.empty())
        AppendMenuW(menu,MF_STRING | MF_GRAYED,0,state.crash_debug_status.c_str());
    AppendMenuW(menu,MF_STRING | (xrfg::autonomous::benchmark_enabled() ? MF_CHECKED : MF_UNCHECKED),
        toggle_benchmark_mode,L"Enable benchmarks");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, open_logs, L"Open folder");
    AppendMenuW(menu, MF_STRING, show_about, L"About");
    AppendMenuW(menu, MF_STRING, exit_application, L"Exit");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(state.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, state.window, nullptr);
    PostMessageW(state.window, WM_NULL, 0, 0);
    DestroyMenu(menu);

}

[[nodiscard]] bool set_benchmark_preference(const std::filesystem::path& ini, bool enabled, std::wstring* error) {
    if(!xrfg::autonomous::write_configuration_sections(ini,
        enabled ? "[benchmark]\nenabled=1\n" : "[benchmark]\nenabled=0\n")) {
        if(error)*error=L"Could not save benchmark mode in ofxr-bridge.ini.";
        return false;
    }
    if(!enabled) {
        xrfg::autonomous::stop_benchmark();
        xrfg::autonomous::tick_benchmark(); // retire the sampler lease immediately
    }
    return true;
}

[[nodiscard]] bool set_pacing_preference(AppState& state,xrfg::standalone::RuntimePacing pacing,std::wstring* error) {
    const auto previous=state.settings.pacing;state.settings.pacing=pacing;
    if(!xrfg::autonomous::write_configuration_sections(state.settings_path,xrfg::standalone::serialize_settings(state.settings))) {
        state.settings.pacing=previous;if(error)*error=L"Could not save Pacing in ofxr-bridge.ini.";return false;
    }
    if(state.armed) {
        const auto ini=runtime_directory(state.local_directory)/L"ofxr_bridge.ini";
        const wchar_t* value=pacing==xrfg::standalone::RuntimePacing::force_vdxr?L"force_vdxr":pacing==xrfg::standalone::RuntimePacing::force_steamvr?L"force_steamvr":L"auto";
        if(!WritePrivateProfileStringW(L"ofxr",L"pacing",value,ini.c_str())) {
            state.settings.pacing=previous;save_settings(state);if(error)*error=last_error_message(L"Saving Pacing");return false;
        }
    }
    return true;
}

[[nodiscard]] bool set_steamvr_queue_preference(AppState& state, bool separate, std::wstring* error) {
    const bool previous = state.settings.steamvr_separate_queue;
    state.settings.steamvr_separate_queue = separate;
    if (!xrfg::autonomous::write_configuration_sections(state.settings_path, xrfg::standalone::serialize_settings(state.settings))) {
        if (error) *error=L"Could not save ofxr-bridge.ini.";
        state.settings.steamvr_separate_queue = previous;
        return false;
    }
    if (state.armed) {
        const auto ini = runtime_directory(state.local_directory) / L"ofxr_bridge.ini";
        // Update this startup-only key; retain every current renderer/debug value.
        if (!WritePrivateProfileStringW(L"ofxr", L"steamvr_separate_queue", separate ? L"1" : L"0", ini.c_str())) {
            if (error) *error = last_error_message(L"Saving the SteamVR queue choice");
            state.settings.steamvr_separate_queue = previous;
            save_settings(state);
            return false;
        }
    }
    log_lifecycle(state.local_directory, separate ? L"steamvr-queue-separate-next-session" : L"steamvr-queue-shared-next-session");
    return true;
}

// Desktop-only preparation. Never runs on a game/render thread. The helper
// journals exact owned registrations/settings and restores them when disabled.
void prepare_crash_debug(AppState& state,const std::filesystem::path& game={}) {
    const auto journal=state.local_directory/L"CrashDiagnostics/dred-state.json";
    if(!state.settings.crash_debug && !std::filesystem::exists(journal)) {
        state.crash_debug_status.clear();return;
    }
    try {
        const auto script=state.executable_directory/L"scripts/Set-CrashDebug.ps1";
        if(!std::filesystem::is_regular_file(script))throw std::runtime_error("Missing crash debug helper");
        std::array<wchar_t,32768> system{};
        if(!GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size())))throw std::runtime_error("System directory");
        const auto powershell=std::filesystem::path(system.data())/L"WindowsPowerShell/v1.0/powershell.exe";
        auto command=xrfg::standalone::quote_windows_argument(powershell.wstring())+
            L" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "+
            xrfg::standalone::quote_windows_argument(script.wstring())+
            L" -StateDirectory "+xrfg::standalone::quote_windows_argument((state.local_directory/L"CrashDiagnostics").wstring());
        if(state.settings.crash_debug)command+=L" -Enable";
        if(!game.empty())command+=L" -GamePath "+xrfg::standalone::quote_windows_argument(game.wstring());
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION process{};
        if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process))
            throw std::runtime_error("Cannot start crash debug helper");
        const auto waited=WaitForSingleObject(process.hProcess,10000);
        DWORD result=1;
        if(waited==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&result);
        else TerminateProcess(process.hProcess,1);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);
        state.crash_debug_status=result?L"DRED not ready - see CrashDiagnostics/readiness.txt":
            state.settings.crash_debug?(game.empty()?L"DRED waiting for game discovery":L"DRED prepared - restart running game"):L"";
        log_lifecycle(state.local_directory,L"crash-debug",state.crash_debug_status);
    } catch (...) {
        state.crash_debug_status=L"DRED not ready - crash debug setup failed";
        log_lifecycle(state.local_directory,L"crash-debug",state.crash_debug_status);
    }
}

// status.json beside tray.ini (docs/STATUS_JSON.md): the tray's state and the
// running game as its layer reports it through the control channel, the
// settings. Written to a temporary file and moved over the old one, so a
// reader never sees half a file; a reader holding it open only delays the
// next write. `final`: the tray is exiting, so no session is reported.
void write_status(const AppState& state, bool final = false) {
    try {
        if(!xrfg::autonomous::snapshot().desired.status_json && !(final&&state.status_export_active))return;
        xrfg::standalone::StatusInputs inputs;
        inputs.armed = state.armed;
        inputs.paused = state.paused;
        inputs.build = kImplementationVersion;
        inputs.settings = state.settings;
        SYSTEMTIME now{};
        GetSystemTime(&now);
        char updated[40]{};
        std::snprintf(updated, sizeof(updated), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            now.wMilliseconds);
        inputs.updated = updated;
        const auto current = xrfg::autonomous::snapshot();
        if (!final && current.connected && current.runtime.process_id) {
            const auto& runtime = current.runtime;
            const auto& m = runtime.telemetry;
            const auto tick = GetTickCount64();
            const bool fresh = m.updated_ms && tick >= m.updated_ms && tick - m.updated_ms < 2000;
            inputs.session_present = true;
            auto& session = inputs.session;
            session.pid = runtime.process_id;
            const auto name = xrfg::standalone::game_executable(runtime.process_id).filename().wstring();
            if (!name.empty()) {
                const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                    nullptr, 0, nullptr, nullptr);
                session.process.assign(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
                if (bytes > 0)
                    WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                        session.process.data(), bytes, nullptr, nullptr);
            }
            // As the benchmark CSV names it; the control channel carries no
            // runtime name string.
            session.runtime = runtime.vdxr_runtime ? "VDXR" : runtime.steamvr_runtime ? "SteamVR" : "Other";
            // Luke Ross's R.E.A.L. VR runs at 2X whatever the multiplier asked for.
            session.multiplier = runtime.luke_detected ? 2U : current.desired.triple_frame_gen ? 3U : 2U;
            session.deep_pipeline = current.desired.prefer_fps;
            const bool runtime_fresh = runtime.heartbeat_ms && tick >= runtime.heartbeat_ms &&
                tick - runtime.heartbeat_ms < 2000;
            session.rife_execution_active = runtime_fresh && current.desired.enabled &&
                current.desired.frame_generation == 4 && runtime.generation_mode == 4 &&
                !runtime.pending && runtime.applied_revision == current.revision &&
                runtime.generation_state != xrfg::autonomous::GenerationState::paused &&
                runtime.rife_execution_active;
            session.rife_optimizing = session.rife_execution_active && runtime.rife_optimizing;
            session.rife_tensorrt = session.rife_execution_active && runtime.rife_tensorrt;
            // Frames generated in the last second, by OFXR or by AFW alone.
            session.generating = (fresh && m.generating) ||
                xrfg::autonomous::interface_afw_only_active(current, tick);
            session.rates_valid = fresh && m.rates_valid && std::isfinite(m.game_fps);
            session.app_fps = m.game_fps;
            session.output_valid = fresh && xrfg::output_rate_valid(m);
            session.output_fps = m.output_fps;
            session.display_hz = fresh && std::isfinite(m.display_hz) && m.display_hz > 0 ? m.display_hz : 0.f;
            // The settings window's "Cycle minimum": WaitFrame entry through
            // the real frame's observed runtime completion, one-second median.
            session.latency_valid = fresh && m.cycle_estimated_valid &&
                std::isfinite(m.cycle_estimated_ms) && m.cycle_estimated_ms >= 0;
            session.latency_ms = m.cycle_estimated_ms;
        }
        const std::string json = xrfg::standalone::build_status_json(inputs);
        const auto target = state.local_directory / L"status.json";
        auto temporary = target;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) return;
            file.write(json.data(), static_cast<std::streamsize>(json.size()));
            if (!file) return;
        }
        static_cast<void>(MoveFileExW(temporary.c_str(), target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    } catch (...) {
    }
}

void handle_command(AppState& state, UINT command) {
    switch (command) {
    case toggle_arm:
        if (state.armed) {
            std::wstring error;
            if (!disarm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        } else {
            std::wstring error;
            if (!arm_bridge(state, &error)) {
                show_error(state.window, error);
            }
        }
        write_status(state);
        break;
    case toggle_pause:
        if (!state.armed || !state.pause_signal) break;
        if (state.paused ? ResetEvent(state.pause_signal)
                         : SetEvent(state.pause_signal)) {
            state.paused = !state.paused;
            log_lifecycle(state.local_directory, state.paused ? L"pause" : L"resume");
            refresh_tray_icon(state);
            if (state.paused) {
                show_balloon(
                    state,
                    L"Frame generation paused",
                    L"Running games still go through the bridge. To rule OFXR out, disarm and restart the game.");
            } else {
                show_balloon(
                    state,
                    L"Frame generation resumed",
                    L"Running games generate frames again.");
            }
        } else {
            show_error(
                state.window, last_error_message(L"Switching the frame generation pause"));
        }
        write_status(state);
        break;
    case configure_keybinds:
        change_keybinds(state);
        break;
    case check_rife: {
        std::wstring error;
        if(!begin_rife_operation(state,false,&error))show_error(state.window,error);
        break;
    }
    case toggle_benchmark_mode: {
        std::wstring error;
        if(!set_benchmark_preference(state.settings_path,!xrfg::autonomous::benchmark_enabled(),&error))
            show_error(state.window,error);
        return;
    }
    case toggle_crash_debug: {
        const bool previous=state.settings.crash_debug;
        state.settings.crash_debug=!state.settings.crash_debug;
        if(!xrfg::autonomous::write_configuration_sections(state.settings_path,
            xrfg::standalone::serialize_settings(state.settings))) {
            state.settings.crash_debug=previous;
            state.crash_debug_status=L"Could not save Crash debug preference";
            log_lifecycle(state.local_directory,L"crash-debug",state.crash_debug_status);
            return;
        }
        if(state.armed)WritePrivateProfileStringW(L"diagnostics",L"crash_debug",
            state.settings.crash_debug?L"1":L"0",(runtime_directory(state.local_directory)/L"ofxr_bridge.ini").c_str());
        prepare_crash_debug(state,xrfg::standalone::last_dlssg_game(state.settings_path));
        return;
    }
    case pacing_auto:
    case pacing_vdxr:
    case pacing_steamvr: {
        using xrfg::standalone::RuntimePacing;
        const auto mode=command==pacing_vdxr?RuntimePacing::force_vdxr:command==pacing_steamvr?RuntimePacing::force_steamvr:RuntimePacing::automatic;
        std::wstring error;if(!set_pacing_preference(state,mode,&error))show_error(state.window,error);
        return;
    }
    case steamvr_queue_separate:
    case steamvr_queue_shared: {
        std::wstring error;
        if (!set_steamvr_queue_preference(state, command == steamvr_queue_separate, &error))
            show_error(state.window, error);
        break;
    }
    case show_settings:
        if (!xrfg::autonomous::show_settings_window(state.window))
            show_error(state.window, L"Could not open the settings window.");
        break;
    case open_logs: {
        const auto directory = state.local_directory;
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(
            state.window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case donate_creator:
    case donate_maintainer:
        ShellExecuteW(
            state.window, L"open",
            command == donate_creator ? kDonateCreatorUrl : kDonateMaintainerUrl,
            nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case show_about: {
        wchar_t version_label[64]{};
        swprintf_s(version_label, L"OFXR-BRIDGE 0.3.3 (v%03u)", kImplementationVersion);
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = state.window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                         TDF_ENABLE_HYPERLINKS |
                         TDF_SIZE_TO_CONTENT |
                         TDF_USE_HICON_MAIN;
        dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        dialog.pszWindowTitle = kApplicationName;
        dialog.pszMainInstruction = version_label;
        dialog.pszContent =
            L"Created by tig3rmast3r, the original author of OFXR.\r\n"
            L"Support him: <a href=\"https://ko-fi.com/tig3rmast3r\">"
            L"ko-fi.com/tig3rmast3r</a>\r\n\r\n"
            L"Further improvements and collaboration by Djules.\r\n"
            L"Support him: <a href=\"https://ko-fi.com/djules\">"
            L"ko-fi.com/djules</a>\r\n\r\n"
            L"DLSS and Hybrid Framegen Integration by Elliotttate.\r\n\r\n"
            L"Licensed under LGPL-3.0-or-later.\r\n"
            L"<a href=\"https://github.com/djules75/OFXR-Bridge\">"
            L"github.com/djules75/OFXR-Bridge</a>";
        dialog.hMainIcon = state.disarmed_icon;
        dialog.pfCallback = [](
            HWND window,
            UINT notification,
            WPARAM,
            LPARAM parameter,
            LONG_PTR) -> HRESULT {
                if (notification == TDN_HYPERLINK_CLICKED) {
                    ShellExecuteW(
                        window,
                        L"open",
                        reinterpret_cast<LPCWSTR>(parameter),
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);
                }
                return S_OK;
            };
        if (FAILED(TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr))) {
            const std::wstring message = std::wstring(version_label) +
                L"\r\n\r\nCreated by tig3rmast3r, the original author of OFXR.\r\n"
                L"Support him: https://ko-fi.com/tig3rmast3r\r\n\r\n"
                L"Further improvements and collaboration by Djules.\r\n"
                L"Support him: https://ko-fi.com/djules\r\n\r\n"
                L"DLSS and Hybrid Framegen Integration by Elliotttate.\r\n\r\n"
                L"License: LGPL-3.0-or-later\r\n"
                L"https://github.com/djules75/OFXR-Bridge";
            MessageBoxW(
                state.window,
                message.c_str(),
                kApplicationName,
                MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case exit_application:
        SendMessageW(state.window, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK window_procedure(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    xrfg::autonomous::ScopedConfigurationSelection automatic_routing;
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<AppState*>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
    }
    if (state == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == state->taskbar_created_message) {
        static_cast<void>(add_tray_icon(*state));
        return 0;
    }
    switch (message) {
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) {
            show_context_menu(*state);
        } else if (lparam == WM_LBUTTONDBLCLK) {
            handle_command(*state, show_settings);
        }
        return 0;
    case WM_TIMER:
        if(wparam==kRifeSetupTimer){poll_rife_operation(*state);return 0;}
        if(wparam==kBenchmarkTimer) {
            // Desktop controller only; no disk/profile work on the game's frame thread.
            static unsigned remembered_game{};
            static ULONGLONG next_game_check{};
            const auto now = GetTickCount64();
            if (now >= next_game_check) try {
                next_game_check = now + 1000;
                const auto current = xrfg::autonomous::snapshot();
                if (!current.connected) remembered_game=0;
                else if (current.runtime.process_id != remembered_game) {
                    const auto game = xrfg::standalone::game_executable(current.runtime.process_id);
                    if(state->settings.crash_debug)prepare_crash_debug(*state,game);
                    if (xrfg::standalone::remember_dlssg_game(
                            xrfg::autonomous::global_configuration_path(), game)) {
                        std::wstring preset_error;
                        if(!xrfg::standalone::apply_preferred_dlssg_preset(
                            xrfg::autonomous::game_configuration_path(game),game,preset_error))
                            log_lifecycle(state->local_directory,L"DLSS-G-preference",preset_error);
                        remembered_game = current.runtime.process_id;
                    }
                }
            } catch (...) {}
            const bool exporting=xrfg::autonomous::snapshot().desired.status_json;
            xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::status_export,exporting);
            static ULONGLONG next_status{};
            if(exporting) {
                state->status_export_active=true;
                if(now>=next_status){next_status=now+1000;write_status(*state);}
            } else if(state->status_export_active) {
                state->status_export_active=false;
                std::error_code ignored;
                std::filesystem::remove(state->local_directory/L"status.json",ignored);
            }
            xrfg::autonomous::tick_benchmark();return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    case WM_COMMAND:
        handle_command(*state, LOWORD(wparam));
        return 0;
    case WM_HOTKEY:
        if (wparam == kPauseHotkeyId) handle_command(*state, toggle_pause);
        return 0;
    case WM_CLOSE: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            show_error(window, error);
            return 0;
        }
        DestroyWindow(window);
        return 0;
    }
    case WM_QUERYENDSESSION: {
        std::wstring error;
        if (!disarm_bridge(*state, &error)) {
            ShutdownBlockReasonCreate(window, L"OFXR 0.3.3 could not disable its OpenXR registrations.");
            return FALSE;
        }
        ShutdownBlockReasonDestroy(window);
        return TRUE;
    }
    case WM_ENDSESSION:
        if (wparam != FALSE) {
            std::wstring error;
            if (!disarm_bridge(*state, &error))
                log_lifecycle(state->local_directory, L"end-session-cleanup", error);
            DestroyWindow(window);
        } else {
            // A cancelled shutdown remains disarmed; never silently re-arm.
            ShutdownBlockReasonDestroy(window);
        }
        return 0;
    case WM_DESTROY: {
        close_rife_process(*state);
        KillTimer(window,kBenchmarkTimer);
        xrfg::autonomous::stop_benchmark();xrfg::autonomous::tick_benchmark();
        xrfg::autonomous::close_settings_window();
        std::wstring error;
        if (!disarm_bridge(*state, &error))
            log_lifecycle(state->local_directory, L"destroy-cleanup", error);
        write_status(*state, true);
        Shell_NotifyIconW(NIM_DELETE, &state->icon);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

[[nodiscard]] bool watch_registered_arm(
    HANDLE parent, const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory, std::wstring* error,
    xrfg::implicit_layer::RegistryScope scope,
    std::wstring_view registry_subkey = xrfg::implicit_layer::kRegistrySubkey,
    const std::filesystem::path& vulkan_manifest = {}) {
    if (!xrfg::implicit_layer::owned_registration_path(manifest, local_directory)) {
        if (error) *error = L"Cleanup watchdog refused an unowned manifest.";
        return false;
    }
    if (!vulkan_manifest.empty() &&
        !xrfg::implicit_layer::owned_registration_path(
            vulkan_manifest, local_directory,
            xrfg::implicit_layer::kVulkanManifestPrefix)) {
        if (error) *error = L"Cleanup watchdog refused an unowned Vulkan manifest.";
        return false;
    }
    while (xrfg::implicit_layer::manifest_registered(manifest, scope, registry_subkey)) {
        if (parent == nullptr || WaitForSingleObject(parent, kArmPollMilliseconds) != WAIT_TIMEOUT)
            break;
    }
    // Scoped to this arm only: an old helper must never revoke a newer arm.
    bool retired = xrfg::implicit_layer::retire_manifest(
        manifest, scope, error, registry_subkey);
    // 0.2 V576: the arm's 32-bit manifest, if it made one.
    const auto x86_manifest = xrfg::implicit_layer::x86_manifest_for(manifest);
    if (xrfg::implicit_layer::owned_registration_path(
            x86_manifest, local_directory, xrfg::implicit_layer::kX86ManifestPrefix) &&
        (xrfg::implicit_layer::manifest_registered(x86_manifest, scope, registry_subkey) ||
         std::filesystem::exists(x86_manifest))) {
        std::wstring x86_error;
        if (!xrfg::implicit_layer::retire_manifest(
                x86_manifest, scope, &x86_error, registry_subkey)) {
            retired = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += x86_error;
            }
        }
    }
    if (!vulkan_manifest.empty()) {
        std::wstring vulkan_error;
        if (!xrfg::implicit_layer::retire_manifest(
                vulkan_manifest, scope, &vulkan_error,
                xrfg::implicit_layer::kVulkanRegistrySubkey)) {
            retired = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += vulkan_error;
            }
        }
    }
    return retired;
}

[[nodiscard]] std::optional<int> run_cleanup_helper() {
    int argument_count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (arguments == nullptr) {
        return EXIT_FAILURE;
    }
    if (argument_count < 2 || _wcsicmp(arguments[1], kCleanupArgument) != 0) {
        LocalFree(arguments);
        return std::nullopt;
    }
    if (argument_count != 5 && argument_count != 6) {
        LocalFree(arguments);
        return EXIT_FAILURE;
    }
    const std::filesystem::path manifest(arguments[2]);
    const std::filesystem::path vulkan_manifest(
        argument_count == 6 ? arguments[5] : L"");
    wchar_t* end = nullptr;
    const unsigned long parsed_pid = std::wcstoul(arguments[3], &end, 10);
    const bool valid_pid = end != arguments[3] && end != nullptr && *end == L'\0' &&
                           parsed_pid > 0 && parsed_pid <= MAXDWORD;
    const std::wstring_view scope_argument(arguments[4]);
    const auto scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0
        ? xrfg::implicit_layer::RegistryScope::local_machine
        : xrfg::implicit_layer::RegistryScope::current_user;
    const bool valid_scope = _wcsicmp(scope_argument.data(), L"HKLM") == 0 ||
                             _wcsicmp(scope_argument.data(), L"HKCU") == 0;
    LocalFree(arguments);
    if (!valid_pid || !valid_scope) {
        return EXIT_FAILURE;
    }

    std::filesystem::path directory;
    try {
        directory = runtime_directory(local_app_data() / L"ofxr-bridge");
    } catch (...) {
        return EXIT_FAILURE;
    }
    if (!xrfg::implicit_layer::owned_manifest_path(manifest, directory)) {
        return EXIT_FAILURE;
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parsed_pid));
    std::wstring error;
    const bool cleaned = watch_registered_arm(parent, manifest,
        local_app_data() / L"ofxr-bridge", &error, scope,
        xrfg::implicit_layer::kRegistrySubkey, vulkan_manifest);
    if (parent != nullptr) {
        CloseHandle(parent);
    }
    log_lifecycle(local_app_data() / L"ofxr-bridge", L"watchdog-cleanup", error);
    return cleaned ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR arguments, int) {
    if (!xrfg::autonomous::initialize_user_configuration()) return EXIT_FAILURE;
    // This entry point displays the panel without creating an activation,
    // changing registrations, writing tray configuration or loading the layer.
    if (arguments && (wcsstr(arguments, L"--settings-only") || wcsstr(arguments, L"--settings-smoke") ||
        wcsstr(arguments, L"--settings-preview"))) {
        if (!xrfg::autonomous::show_settings_window(nullptr)) return EXIT_FAILURE;
        if (wcsstr(arguments, L"--settings-preview")) {
            int argument_count = 0;
            auto** parsed_arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
            bool captured = false;
            if (parsed_arguments) {
                for (int index = 1; index + 1 < argument_count; ++index)
                    if (std::wstring_view(parsed_arguments[index]) == L"--settings-preview")
                        captured = xrfg::autonomous::save_settings_preview(parsed_arguments[index + 1]);
                LocalFree(parsed_arguments);
            }
            xrfg::autonomous::close_settings_window();
            return captured ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        UINT_PTR smoke_timer = 0;
        if (wcsstr(arguments, L"--settings-smoke"))
            smoke_timer = SetTimer(nullptr, 0, 1500,
                [](HWND, UINT, UINT_PTR timer, DWORD) { KillTimer(nullptr, timer); PostQuitMessage(0); });
        MSG panel_message{};
        while (GetMessageW(&panel_message, nullptr, 0, 0) > 0) {
            TranslateMessage(&panel_message);
            DispatchMessageW(&panel_message);
        }
        if (smoke_timer) KillTimer(nullptr, smoke_timer);
        xrfg::autonomous::close_settings_window();
        return static_cast<int>(panel_message.wParam);
    }
    if (const auto helper_result = run_cleanup_helper()) {
        return *helper_result;
    }

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\OFXRBridgeTrayV474");
    if (single_instance == nullptr) {
        return EXIT_FAILURE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"OFXR 0.3.3 is already running in the notification area.",
            kApplicationName,
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(single_instance);
        return EXIT_SUCCESS;
    }

    AppState state;
    try {
        state.executable_directory = executable_directory();
        state.local_directory = local_app_data() / L"ofxr-bridge";
        state.settings_path = xrfg::autonomous::global_configuration_path();
        std::wstring cleanup_error;
        if (!cleanup_all_owned_registrations(state, &cleanup_error)) {
            log_lifecycle(state.local_directory, L"startup-cleanup", cleanup_error);
            MessageBoxW(nullptr, cleanup_error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
            CloseHandle(single_instance);
            return EXIT_FAILURE;
        }
        log_lifecycle(state.local_directory, L"startup-cleanup");
        load_settings(state);
        xrfg::autonomous::publish_keybinds(xrfg::standalone::runtime_keybinds(state.settings));
        save_settings(state);
        const auto preset_target=xrfg::standalone::last_dlssg_game(state.settings_path);
        prepare_crash_debug(state,preset_target);
        if(!preset_target.empty()) {
            std::wstring preset_error;
            if(!xrfg::standalone::apply_preferred_dlssg_preset(
                std::filesystem::exists(xrfg::autonomous::game_configuration_path(preset_target))?xrfg::autonomous::game_configuration_path(preset_target):state.settings_path,preset_target,preset_error))
                log_lifecycle(state.local_directory,L"DLSS-G-preference",preset_error);
        }
    } catch (...) {
        MessageBoxW(
            nullptr,
            L"OFXR 0.3.3 could not initialize its local configuration.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }
    state.taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");
    state.armed_icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_OFXR_ARMED));
    state.disarmed_icon = LoadIconW(
        instance, MAKEINTRESOURCEW(IDI_OFXR_DISARMED));
    if (state.armed_icon == nullptr || state.disarmed_icon == nullptr) {
        MessageBoxW(
            nullptr,
            L"OFXR 0.3.3 could not load its notification icons.",
            kApplicationName,
            MB_OK | MB_ICONERROR);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = instance;
    window_class.hIcon = state.disarmed_icon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = state.disarmed_icon;
    if (RegisterClassExW(&window_class) == 0) {
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    const HWND window = CreateWindowExW(
        0,
        kWindowClass,
        kApplicationName,
        WS_OVERLAPPED,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instance,
        &state);
    if (window == nullptr || !add_tray_icon(state)) {
        if (window) DestroyWindow(window);
        CloseHandle(single_instance);
        return EXIT_FAILURE;
    }

    // Normal startup auto-arms through the same validated path as Enable.
    SetTimer(window,kBenchmarkTimer,100,nullptr);
    // Settings-only/preview and watchdog helpers returned before this point.
    std::wstring auto_arm_error;
    if (!arm_bridge(state, &auto_arm_error)) {
        log_lifecycle(state.local_directory, L"startup-auto-arm", auto_arm_error);
        show_error(state.window, auto_arm_error);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    std::wstring cleanup_error;
    const bool clean_exit = disarm_bridge(state, &cleanup_error);
    if (!clean_exit) log_lifecycle(state.local_directory, L"message-loop-exit", cleanup_error);
    CloseHandle(single_instance);
    return clean_exit ? static_cast<int>(message.wParam) : EXIT_FAILURE;
}
