#include "xrfg/implicit_layer.hpp"
#include "xrfg/standalone_launcher.hpp"
#include "resource.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
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
constexpr wchar_t kApplicationName[] = L"OFXR Bridge";
constexpr wchar_t kCleanupArgument[] = L"--cleanup-manual-arm";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTrayId = 1;
constexpr UINT kArmPollMilliseconds = 250;
constexpr std::uint32_t kImplementationVersion = 59;
constexpr wchar_t kDonateUrl[] = L"https://ko-fi.com/tig3rmast3r";

enum MenuCommand : UINT {
    toggle_arm = 90,
    backend_fidelity_fx = 110,
    backend_nvidia_slow = 111,
    backend_nvidia_medium = 112,
    toggle_nvidia_bidirectional = 114,
    nvidia_scale_full = 115,
    nvidia_scale_three_quarter = 116,
    nvidia_scale_half = 117,
    toggle_diagnostics = 120,
    open_logs = 130,
    donate = 139,
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
    HICON armed_icon{};
    HICON disarmed_icon{};
    UINT taskbar_created_message{};
    bool armed{};
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
    std::wstring ignored;
    static_cast<void>(write_text_atomic(
        state.settings_path,
        xrfg::standalone::serialize_settings(state.settings),
        &ignored));
}

void show_error(HWND owner, const std::wstring& error) {
    MessageBoxW(owner, error.c_str(), kApplicationName, MB_OK | MB_ICONERROR);
}

void remove_manifest_file(const std::filesystem::path& manifest) {
    std::error_code ignored;
    std::filesystem::remove(manifest, ignored);
}

void cleanup_stale_manifests(const std::filesystem::path& local_directory) {
    const auto directory = runtime_directory(local_directory);
    std::error_code iterator_error;
    for (std::filesystem::directory_iterator iterator(directory, iterator_error), end;
         !iterator_error && iterator != end;
         iterator.increment(iterator_error)) {
        if (!iterator->is_regular_file(iterator_error)) {
            continue;
        }
        const auto manifest = iterator->path();
        if (!xrfg::implicit_layer::owned_manifest_path(manifest, directory)) {
            continue;
        }
        static_cast<void>(xrfg::implicit_layer::unregister_manifest(manifest));
        remove_manifest_file(manifest);
    }
}

[[nodiscard]] bool write_runtime_configuration(
    const AppState& state,
    std::wstring* error) {
    return write_text_atomic(
        runtime_directory(state.local_directory) / L"ofxr_bridge.ini",
        xrfg::standalone::build_runtime_ini(state.settings),
        error);
}

[[nodiscard]] bool prepare_runtime_layer(
    const AppState& state,
    std::filesystem::path* manifest,
    std::wstring* error) {
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

        const std::wstring manifest_name =
            std::wstring(xrfg::implicit_layer::kManifestPrefix) +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) +
            xrfg::implicit_layer::kManifestSuffix;
        const auto generated_manifest = directory / manifest_name;
        if (!write_text_atomic(
                generated_manifest,
                xrfg::standalone::build_implicit_layer_manifest(
                    runtime_dll, kImplementationVersion),
                error) ||
            !write_runtime_configuration(state, error)) {
            remove_manifest_file(generated_manifest);
            return false;
        }
        *manifest = generated_manifest;
        return true;
    } catch (...) {
        if (error) *error = L"Unable to prepare the manual OpenXR layer.";
        return false;
    }
}

[[nodiscard]] bool spawn_cleanup_helper(
    const AppState& state,
    const std::filesystem::path& manifest,
    std::wstring* error) {
    std::wstring command = xrfg::standalone::quote_windows_argument(
        (state.executable_directory / L"OFXRBridgeTray.exe").wstring());
    command += L" ";
    command += kCleanupArgument;
    command += L" ";
    command += xrfg::standalone::quote_windows_argument(manifest.wstring());
    command += L" ";
    command += std::to_wstring(GetCurrentProcessId());
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

[[nodiscard]] std::wstring tray_tooltip(const AppState& state) {
    std::wstring tooltip = state.armed ? L"OFXR Bridge ARMED - " : L"OFXR Bridge - ";
    if (state.settings.backend == xrfg::standalone::FlowBackend::nvidia) {
        switch (state.settings.nvidia_preset) {
        case xrfg::standalone::NvidiaPerformancePreset::slow:
            tooltip += L"NVIDIA Slow";
            break;
        case xrfg::standalone::NvidiaPerformancePreset::medium:
        default:
            tooltip += L"NVIDIA Medium";
            break;
        }
        tooltip += state.settings.nvidia_bidirectional
            ? L" + bidirectional"
            : L" + forward";
        switch (state.settings.nvidia_input_scale) {
        case xrfg::standalone::NvidiaInputScale::three_quarter:
            tooltip += L" @ 75%";
            break;
        case xrfg::standalone::NvidiaInputScale::half:
            tooltip += L" @ 50%";
            break;
        case xrfg::standalone::NvidiaInputScale::full:
        default:
            tooltip += L" @ 100%";
            break;
        }
    } else {
        tooltip += L"FidelityFX";
    }
    if (state.settings.diagnostics) {
        tooltip += L" - recorder on";
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

[[nodiscard]] bool disarm_bridge(
    AppState& state,
    std::wstring* error,
    bool remove_manifest = true) {
    if (!state.armed) {
        return true;
    }
    if (!xrfg::implicit_layer::unregister_manifest(state.armed_manifest, error)) {
        return false;
    }
    if (remove_manifest) {
        remove_manifest_file(state.armed_manifest);
    }
    state.armed = false;
    state.armed_manifest.clear();
    refresh_tray_icon(state);
    return true;
}

[[nodiscard]] bool arm_bridge(AppState& state, std::wstring* error) {
    if (state.armed && xrfg::implicit_layer::manifest_registered(
                           state.armed_manifest)) {
        return true;
    }
    if (state.armed) {
        std::wstring ignored;
        static_cast<void>(disarm_bridge(state, &ignored));
    }

    std::filesystem::path manifest;
    if (!prepare_runtime_layer(state, &manifest, error) ||
        !xrfg::implicit_layer::register_manifest(manifest, error)) {
        remove_manifest_file(manifest);
        return false;
    }
    if (!spawn_cleanup_helper(state, manifest, error)) {
        std::wstring ignored;
        static_cast<void>(xrfg::implicit_layer::unregister_manifest(
            manifest, &ignored));
        remove_manifest_file(manifest);
        return false;
    }
    state.armed = true;
    state.armed_manifest = manifest;
    refresh_tray_icon(state);
    show_balloon(
        state,
        L"OFXR Bridge armed",
        L"The bridge will remain enabled for OpenXR applications until you disarm it or exit the tray.");
    return true;
}

void update_runtime_options(AppState& state) {
    save_settings(state);
    if (state.armed) {
        std::wstring error;
        if (!write_runtime_configuration(state, &error)) {
            show_error(state.window, error);
        }
    }
    refresh_tray_icon(state);
    if (state.armed) {
        show_balloon(
            state,
            L"OFXR options saved",
            L"The new optical-flow settings will be used by the next OpenXR session.");
    }
}

void show_context_menu(AppState& state) {
    HMENU menu = CreatePopupMenu();
    HMENU backend_menu = CreatePopupMenu();
    HMENU nvidia_scale_menu = CreatePopupMenu();
    if (menu == nullptr || backend_menu == nullptr ||
        nvidia_scale_menu == nullptr) {
        if (nvidia_scale_menu) DestroyMenu(nvidia_scale_menu);
        if (backend_menu) DestroyMenu(backend_menu);
        if (menu) DestroyMenu(menu);
        return;
    }

    AppendMenuW(
        menu,
        MF_STRING | (state.armed ? MF_CHECKED : MF_UNCHECKED),
        toggle_arm,
        state.armed
            ? L"Disarm bridge"
            : L"Arm bridge until manual disarm");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::fidelity_fx
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_fidelity_fx,
        L"FidelityFX");
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::nvidia &&
                     state.settings.nvidia_preset ==
                         xrfg::standalone::NvidiaPerformancePreset::medium
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_nvidia_medium,
        L"NVIDIA Medium");
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.backend == xrfg::standalone::FlowBackend::nvidia &&
                     state.settings.nvidia_preset ==
                         xrfg::standalone::NvidiaPerformancePreset::slow
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        backend_nvidia_slow,
        L"NVIDIA Slow (best quality)");
    AppendMenuW(backend_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        backend_menu,
        MF_STRING |
            (state.settings.nvidia_bidirectional ? MF_CHECKED : MF_UNCHECKED),
        toggle_nvidia_bidirectional,
        L"NVIDIA bidirectional consistency");
    AppendMenuW(
        menu,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(backend_menu),
        L"Optical flow backend");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::full
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_full,
        L"100% (full resolution)");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::three_quarter
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_three_quarter,
        L"75%");
    AppendMenuW(
        nvidia_scale_menu,
        MF_STRING |
            (state.settings.nvidia_input_scale ==
                     xrfg::standalone::NvidiaInputScale::half
                 ? MF_CHECKED
                 : MF_UNCHECKED),
        nvidia_scale_half,
        L"50%");
    AppendMenuW(
        menu,
        MF_POPUP,
        reinterpret_cast<UINT_PTR>(nvidia_scale_menu),
        L"NVIDIA OFA resolution");
    AppendMenuW(
        menu,
        MF_STRING | (state.settings.diagnostics ? MF_CHECKED : MF_UNCHECKED),
        toggle_diagnostics,
        L"Bridge flight recorder");
    AppendMenuW(menu, MF_STRING, open_logs, L"Open bridge logs");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, donate, L"Donate");
    AppendMenuW(menu, MF_STRING, show_about, L"About");
    AppendMenuW(menu, MF_STRING, exit_application, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(state.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, state.window, nullptr);
    PostMessageW(state.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
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
        break;
    case backend_fidelity_fx:
        state.settings.backend = xrfg::standalone::FlowBackend::fidelity_fx;
        update_runtime_options(state);
        break;
    case backend_nvidia_slow:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::slow;
        update_runtime_options(state);
        break;
    case backend_nvidia_medium:
        state.settings.backend = xrfg::standalone::FlowBackend::nvidia;
        state.settings.nvidia_preset =
            xrfg::standalone::NvidiaPerformancePreset::medium;
        update_runtime_options(state);
        break;
    case toggle_nvidia_bidirectional:
        state.settings.nvidia_bidirectional =
            !state.settings.nvidia_bidirectional;
        update_runtime_options(state);
        break;
    case nvidia_scale_full:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::full;
        update_runtime_options(state);
        break;
    case nvidia_scale_three_quarter:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::three_quarter;
        update_runtime_options(state);
        break;
    case nvidia_scale_half:
        state.settings.nvidia_input_scale =
            xrfg::standalone::NvidiaInputScale::half;
        update_runtime_options(state);
        break;
    case toggle_diagnostics:
        state.settings.diagnostics = !state.settings.diagnostics;
        update_runtime_options(state);
        break;
    case open_logs: {
        const auto directory = runtime_directory(state.local_directory);
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);
        ShellExecuteW(
            state.window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case donate:
        ShellExecuteW(
            state.window, L"open", kDonateUrl, nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case show_about: {
        TASKDIALOGCONFIG dialog{};
        dialog.cbSize = sizeof(dialog);
        dialog.hwndParent = state.window;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                         TDF_ENABLE_HYPERLINKS |
                         TDF_SIZE_TO_CONTENT |
                         TDF_USE_HICON_MAIN;
        dialog.dwCommonButtons = TDCBF_CLOSE_BUTTON;
        dialog.pszWindowTitle = kApplicationName;
        dialog.pszMainInstruction = L"OFXR Bridge v0.1.0 (V059)";
        dialog.pszContent =
            L"Licensed under LGPL-3.0-or-later.\r\n\r\n"
            L"<a href=\"https://github.com/tig3rmast3r/OFXR-Bridge\">"
            L"github.com/tig3rmast3r/OFXR-Bridge</a>";
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
            MessageBoxW(
                state.window,
                L"OFXR Bridge v0.1.0 (V059)\r\n\r\n"
                L"License: LGPL-3.0-or-later\r\n"
                L"https://github.com/tig3rmast3r/OFXR-Bridge",
                kApplicationName,
                MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case exit_application:
        DestroyWindow(state.window);
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
            handle_command(*state, toggle_arm);
        }
        return 0;
    case WM_COMMAND:
        handle_command(*state, LOWORD(wparam));
        return 0;
    case WM_QUERYENDSESSION: {
        std::wstring ignored;
        static_cast<void>(disarm_bridge(*state, &ignored));
        return TRUE;
    }
    case WM_DESTROY: {
        std::wstring ignored;
        static_cast<void>(disarm_bridge(*state, &ignored));
        Shell_NotifyIconW(NIM_DELETE, &state->icon);
        PostQuitMessage(0);
        return 0;
    }
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
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
    if (argument_count != 4) {
        LocalFree(arguments);
        return EXIT_FAILURE;
    }
    const std::filesystem::path manifest(arguments[2]);
    wchar_t* end = nullptr;
    const unsigned long parsed_pid = std::wcstoul(arguments[3], &end, 10);
    const bool valid_pid = end != arguments[3] && end != nullptr && *end == L'\0' &&
                           parsed_pid > 0 && parsed_pid <= MAXDWORD;
    LocalFree(arguments);
    if (!valid_pid) {
        return EXIT_FAILURE;
    }

    std::filesystem::path directory;
    try {
        directory = runtime_directory(local_app_data() / L"OFXR Bridge");
    } catch (...) {
        return EXIT_FAILURE;
    }
    if (!xrfg::implicit_layer::owned_manifest_path(manifest, directory)) {
        return EXIT_FAILURE;
    }

    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parsed_pid));
    while (xrfg::implicit_layer::manifest_registered(manifest)) {
        if (parent == nullptr ||
            WaitForSingleObject(parent, kArmPollMilliseconds) == WAIT_OBJECT_0) {
            break;
        }
    }
    if (parent != nullptr) {
        CloseHandle(parent);
    }
    static_cast<void>(xrfg::implicit_layer::unregister_manifest(manifest));
    remove_manifest_file(manifest);
    return EXIT_SUCCESS;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (const auto helper_result = run_cleanup_helper()) {
        return *helper_result;
    }

    HANDLE single_instance = CreateMutexW(nullptr, TRUE, L"Local\\OFXRBridgeTray");
    if (single_instance == nullptr) {
        return EXIT_FAILURE;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge is already running in the notification area.",
            kApplicationName,
            MB_OK | MB_ICONINFORMATION);
        CloseHandle(single_instance);
        return EXIT_SUCCESS;
    }

    AppState state;
    try {
        state.executable_directory = executable_directory();
        state.local_directory = local_app_data() / L"OFXR Bridge";
        state.settings_path = state.local_directory / L"tray.ini";
        cleanup_stale_manifests(state.local_directory);
        load_settings(state);
    } catch (...) {
        MessageBoxW(
            nullptr,
            L"OFXR Bridge could not initialize its local configuration.",
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
            L"OFXR Bridge could not load its notification icons.",
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

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CloseHandle(single_instance);
    return static_cast<int>(message.wParam);
}
