#include "xrfg/implicit_layer.hpp"

#include <windows.h>

#include <array>
#include <cwchar>
#include <system_error>
#include <vector>
#include <cstdint>
#include <algorithm>

namespace xrfg::implicit_layer {
namespace {

[[nodiscard]] std::wstring registry_error(
    std::wstring_view action,
    LSTATUS status) {
    std::wstring output(action);
    output += L" failed (" + std::to_wstring(status) + L")";
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(status),
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    if (length > 0 && message != nullptr) {
        output += L": ";
        output.append(message, length);
        LocalFree(message);
    }
    return output;
}

[[nodiscard]] std::wstring normalized_path(
    const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal().wstring();
}

[[nodiscard]] bool equal_case_insensitive(
    std::wstring_view left,
    std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

[[nodiscard]] HKEY registry_root(RegistryScope scope) noexcept {
    return scope == RegistryScope::local_machine
        ? HKEY_LOCAL_MACHINE
        : HKEY_CURRENT_USER;
}

} // namespace

RegistryScope registry_scope_for_integrity_rid(
    std::uint32_t integrity_rid) noexcept {
    return integrity_rid >= SECURITY_MANDATORY_HIGH_RID
        ? RegistryScope::local_machine
        : RegistryScope::current_user;
}

RegistryScope preferred_registry_scope() noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return RegistryScope::current_user;
    }

    DWORD size = 0;
    static_cast<void>(GetTokenInformation(
        token, TokenIntegrityLevel, nullptr, 0, &size));
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        CloseHandle(token);
        return RegistryScope::current_user;
    }

    try {
        std::vector<BYTE> buffer(size);
        if (!GetTokenInformation(
                token,
                TokenIntegrityLevel,
                buffer.data(),
                size,
                &size)) {
            CloseHandle(token);
            return RegistryScope::current_user;
        }
        CloseHandle(token);
        token = nullptr;

        const auto* label =
            reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
        if (label->Label.Sid == nullptr || !IsValidSid(label->Label.Sid)) {
            return RegistryScope::current_user;
        }
        const auto* count = GetSidSubAuthorityCount(label->Label.Sid);
        if (count == nullptr || *count == 0) {
            return RegistryScope::current_user;
        }
        const auto* rid = GetSidSubAuthority(label->Label.Sid, *count - 1U);
        return rid == nullptr
            ? RegistryScope::current_user
            : registry_scope_for_integrity_rid(*rid);
    } catch (...) {
        if (token != nullptr) CloseHandle(token);
        return RegistryScope::current_user;
    }
}

std::wstring_view registry_scope_name(RegistryScope scope) noexcept {
    return scope == RegistryScope::local_machine ? L"HKLM" : L"HKCU";
}

namespace {
constexpr std::wstring_view kArmSignalPrefix = L"Local\\OFXRBridgeArmStop-";
constexpr std::wstring_view kPauseSignalPrefix = L"Local\\OFXRBridgePause-";
}

std::wstring arm_signal_name(const std::filesystem::path& manifest) {
    // Deterministic across tray/DLL versions; no user-controlled event name.
    const auto path = normalized_path(manifest);
    std::uint64_t hash = 14695981039346656037ULL;
    for (wchar_t c : path) {
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
        hash = (hash ^ static_cast<std::uint16_t>(c)) * 1099511628211ULL;
    }
    return std::wstring(kArmSignalPrefix) + std::to_wstring(hash);
}

std::wstring pause_signal_name(const std::filesystem::path& manifest) {
    return std::wstring(kPauseSignalPrefix) +
        arm_signal_name(manifest).substr(kArmSignalPrefix.size());
}

void* create_pause_signal(const std::filesystem::path& manifest, std::wstring* error) noexcept {
    try {
        const auto name = pause_signal_name(manifest);
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        const auto status = GetLastError();
        if (event == nullptr || status == ERROR_ALREADY_EXISTS) {
            if (event) CloseHandle(event);
            if (error) *error = registry_error(L"Creating the OFXR pause control", status);
            return nullptr;
        }
        return event;
    } catch (...) {
        if (error) *error = L"Unable to create the OFXR pause control.";
        return nullptr;
    }
}

void* create_arm_signal(const std::filesystem::path& manifest, std::wstring* error) noexcept {
    try {
        const auto name = arm_signal_name(manifest);
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        const auto status = GetLastError();
        if (event == nullptr || status == ERROR_ALREADY_EXISTS) {
            if (event) CloseHandle(event);
            if (error) *error = registry_error(L"Creating the OFXR arm control", status);
            return nullptr;
        }
        return event;
    } catch (...) {
        if (error) *error = L"Unable to create the OFXR arm control.";
        return nullptr;
    }
}

bool signal_arm_stop(const std::filesystem::path& manifest, std::wstring* error) noexcept {
    try {
        const auto name = arm_signal_name(manifest);
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (!event) {
            const auto status = GetLastError();
            if (status == ERROR_FILE_NOT_FOUND) return true; // Older DLL/no active reader.
            if (error) *error = registry_error(L"Opening the OFXR stop control", status);
            return false;
        }
        const bool result = SetEvent(event) != FALSE;
        const auto status = GetLastError();
        CloseHandle(event);
        if (!result && error) *error = registry_error(L"Stopping the loaded OFXR layer", status);
        return result;
    } catch (...) {
        if (error) *error = L"Unable to signal the loaded OFXR layer.";
        return false;
    }
}

ManualArmControl::ManualArmControl(const std::filesystem::path& module_directory) noexcept {
    try {
        std::array<wchar_t, 128> name{};
        const auto ini = module_directory / L"ofxr_bridge.ini";
        const DWORD count = GetPrivateProfileStringW(L"ofxr", L"control_event", L"",
            name.data(), static_cast<DWORD>(name.size()), ini.c_str());
        managed_ = count != 0;
        const std::wstring_view value(name.data(), count);
        const std::wstring_view prefix = kArmSignalPrefix;
        if (managed_ && count < name.size() - 1 && value.starts_with(prefix) && value.size() > prefix.size()) {
            bool valid = true;
            for (wchar_t c : value.substr(prefix.size())) valid = valid && c >= L'0' && c <= L'9';
            if (valid) {
                event_ = OpenEventW(SYNCHRONIZE, FALSE, name.data());
                const std::wstring pause = std::wstring(kPauseSignalPrefix) +
                    std::wstring(value.substr(prefix.size()));
                pause_event_ = OpenEventW(SYNCHRONIZE, FALSE, pause.c_str());
            }
        }
    } catch (...) { managed_ = true; }
}
ManualArmControl::~ManualArmControl() {
    if (event_) CloseHandle(event_);
    if (pause_event_) CloseHandle(pause_event_);
}
bool ManualArmControl::pause_requested() const noexcept {
    // No pause control is not paused: an older tray, or a test without one.
    return pause_event_ && WaitForSingleObject(pause_event_, 0) == WAIT_OBJECT_0;
}
bool ManualArmControl::stop_requested() const noexcept {
    // Missing/invalid control in a managed session is Off, never fail-open.
    return managed_ && (!event_ || WaitForSingleObject(event_, 0) != WAIT_TIMEOUT);
}

ConfiguredFrameGeneration read_frame_generation(const std::filesystem::path& directory) noexcept {
    if (directory.empty()) return ConfiguredFrameGeneration::ofxr;
    wchar_t value[32]{};
    const auto ini=directory / L"ofxr_bridge.ini";
    GetPrivateProfileStringW(L"ofxr", L"frame_generation", L"ofxr", value, 32, ini.c_str());
    return _wcsicmp(value,L"dlss") == 0 ? ConfiguredFrameGeneration::native_dlss : ConfiguredFrameGeneration::ofxr;
}

int read_native_dlssg_scale(const std::filesystem::path& directory) noexcept {
    if (directory.empty()) return 100;
    const auto ini = directory / L"ofxr_bridge.ini";
    const int value = static_cast<int>(GetPrivateProfileIntW(L"ofxr", L"dlssg_resolution", 100, ini.c_str()));
    return value >= kMinNativeDlssgScale && value <= 100 ? value : 100;
}

ConfiguredFlowBackend read_flow_backend(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return ConfiguredFlowBackend::fidelity_fx;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        std::array<wchar_t, 32> value{};
        const DWORD length = GetPrivateProfileStringW(
            L"ofxr",
            L"backend",
            L"nvidia",
            value.data(),
            static_cast<DWORD>(value.size()),
            ini_path.c_str());
        if (length > 0 && length < value.size() &&
            (_wcsicmp(value.data(), L"nvidia") == 0 ||
             _wcsicmp(value.data(), L"nvof") == 0)) {
            return ConfiguredFlowBackend::nvidia;
        }
        if (length > 0 && length < value.size() &&
            _wcsicmp(value.data(), L"fidelityfx") == 0) {
            return ConfiguredFlowBackend::fidelity_fx;
        }
    } catch (...) {
    }
    // No key, or an unreadable file: the default, which the layer falls
    // back from on its own where NVIDIA cannot initialise.
    return ConfiguredFlowBackend::nvidia;
}

bool read_deep_pipeline(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return true;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"deep_pipeline", 1, ini_path.c_str()) != 0;
    } catch (...) {
        return true;
    }
}

bool read_triple_frame_gen(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return false;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"triple_frame_gen", 0, ini_path.c_str()) != 0;
    } catch (...) {
        return false;
    }
}

bool read_vulkan_session_bridge(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return true;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"vulkan_session_bridge", 1, ini_path.c_str()) != 0;
    } catch (...) {
        return true;
    }
}

bool read_single_swapchain_rings(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return true;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"single_swapchain_rings", 0, ini_path.c_str()) != 0;
    } catch (...) {
        return true;
    }
}

bool read_capture_at_end_frame(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return true;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"capture_at_end_frame", 1, ini_path.c_str()) != 0;
    } catch (...) {
        return true;
    }
}

std::vector<std::wstring> read_excluded_processes(
    const std::filesystem::path& module_directory) noexcept {
    std::vector<std::wstring> names;
    try {
        std::wstring value = kDefaultExcludedProcesses;
        if (!module_directory.empty()) {
            const auto ini_path = module_directory / L"ofxr_bridge.ini";
            // A sentinel default tells an absent key from an empty one.
            std::array<wchar_t, 2048> buffer{};
            const DWORD count = GetPrivateProfileStringW(
                L"ofxr", L"excluded_processes", L"\x01",
                buffer.data(), static_cast<DWORD>(buffer.size()), ini_path.c_str());
            const std::wstring_view read(buffer.data(), count);
            if (read != L"\x01") {
                value.assign(read);
            }
        }
        std::wstring current;
        const auto flush = [&]() {
            while (!current.empty() && (current.back() == L' ' || current.back() == L'\t')) {
                current.pop_back();
            }
            std::size_t start = 0;
            while (start < current.size() && (current[start] == L' ' || current[start] == L'\t')) {
                ++start;
            }
            if (start < current.size()) {
                names.emplace_back(current.substr(start));
            }
            current.clear();
        };
        for (wchar_t character : value) {
            if (character == L';' || character == L',') {
                flush();
            } else {
                current.push_back(character);
            }
        }
        flush();
    } catch (...) {
        names.clear();
    }
    return names;
}

bool executable_is_excluded(
    std::wstring_view executable,
    const std::vector<std::wstring>& excluded) noexcept {
    try {
        for (const std::wstring& name : excluded) {
            if (name.size() == executable.size() &&
                CompareStringOrdinal(
                    name.data(), static_cast<int>(name.size()),
                    executable.data(), static_cast<int>(executable.size()),
                    TRUE) == CSTR_EQUAL) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

std::wstring current_executable_name() noexcept {
    try {
        std::array<wchar_t, 32768> buffer{};
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
        return std::filesystem::path(std::wstring(buffer.data(), length)).filename().wstring();
    } catch (...) {
        return {};
    }
}

namespace {
constexpr wchar_t kFixedFrameMultiplierEvent[] =
    L"Local\\OFXRBridgeFixedFrameMultiplier";
}

FixedFrameMultiplierMarker::~FixedFrameMultiplierMarker() { release(); }

void FixedFrameMultiplierMarker::hold() noexcept {
    if (event_ == nullptr) {
        event_ = CreateEventW(nullptr, TRUE, FALSE, kFixedFrameMultiplierEvent);
    }
}

void FixedFrameMultiplierMarker::release() noexcept {
    if (event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
    }
}

bool running_session_needs_restart_for_3x() noexcept {
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kFixedFrameMultiplierEvent);
    if (event == nullptr) {
        return false;
    }
    CloseHandle(event);
    return true;
}

bool read_d3d11_bridge(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return false;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"d3d11_bridge", 1, ini_path.c_str()) == 1;
    } catch (...) {
        return false;
    }
}

bool read_vulkan_support(
    const std::filesystem::path& module_directory) noexcept {
    try {
        if (module_directory.empty()) {
            return false;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        return GetPrivateProfileIntW(
                   L"ofxr", L"vulkan_bridge", 1, ini_path.c_str()) != 0;
    } catch (...) {
        return false;
    }
}

ConfiguredNvidiaOptions read_nvidia_options(
    const std::filesystem::path& module_directory) noexcept {
    ConfiguredNvidiaOptions options;
    try {
        if (module_directory.empty()) {
            return options;
        }
        const auto ini_path = module_directory / L"ofxr_bridge.ini";
        std::array<wchar_t, 32> value{};
        const DWORD length = GetPrivateProfileStringW(
            L"ofxr",
            L"nvidia_preset",
            L"medium",
            value.data(),
            static_cast<DWORD>(value.size()),
            ini_path.c_str());
        if (length > 0 && length < value.size()) {
            if (_wcsicmp(value.data(), L"slow") == 0) {
                options.preset = ConfiguredNvidiaPerformancePreset::slow;
            } else if (_wcsicmp(value.data(), L"fast") == 0) {
                options.preset = ConfiguredNvidiaPerformancePreset::fast;
            }
        }
        const UINT input_scale = GetPrivateProfileIntW(
            L"ofxr",
            L"nvidia_input_scale",
            50,
            ini_path.c_str());
        options.input_scale = input_scale == 75
            ? ConfiguredNvidiaInputScale::three_quarter
            : input_scale == 50
                ? ConfiguredNvidiaInputScale::half
                : input_scale == 20 ? ConfiguredNvidiaInputScale::fifth
                : input_scale == 25 ? ConfiguredNvidiaInputScale::quarter
                : ConfiguredNvidiaInputScale::full;
        options.bidirectional = GetPrivateProfileIntW(
            L"ofxr",
            L"nvidia_bidirectional",
            0,
            ini_path.c_str()) != 0;
    } catch (...) {
    }
    return options;
}

bool register_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        const LSTATUS create_status = RegCreateKeyExW(
            registry_root(scope),
            subkey.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr,
            &key,
            nullptr);
        if (create_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Opening the OpenXR layer key", create_status);
            return false;
        }
        const std::wstring value_name = normalized_path(manifest);
        constexpr DWORD enabled = 0;
        LSTATUS set_status = RegSetValueExW(
            key,
            value_name.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled),
            sizeof(enabled));
        if (set_status == ERROR_SUCCESS) {
            DWORD value = 1, size = sizeof(value), type = 0;
            set_status = RegQueryValueExW(key, value_name.c_str(), nullptr, &type,
                reinterpret_cast<BYTE*>(&value), &size);
            if (set_status == ERROR_SUCCESS &&
                (type != REG_DWORD || size != sizeof(value) || value != 0))
                set_status = ERROR_INVALID_DATA;
        }
        RegCloseKey(key);
        if (set_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Arming the OpenXR layer", set_status);
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Arming the OpenXR layer failed.";
        return false;
    }
}

bool unregister_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        const LSTATUS open_status = RegOpenKeyExW(
            registry_root(scope),
            subkey.c_str(),
            0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY,
            &key);
        if (open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (open_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Opening the OpenXR layer key", open_status);
            return false;
        }
        const std::wstring value_name = normalized_path(manifest);
        DWORD size = 0;
        auto status = RegQueryValueExW(key, value_name.c_str(), nullptr, nullptr, nullptr, &size);
        if (status == ERROR_SUCCESS) {
            // Disable first: an interrupted cleanup must not leave an enabled
            // registration. Then remove and verify the exact registry value.
            DWORD disabled = 1;
            status = RegSetValueExW(key, value_name.c_str(), 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled));
            if (status == ERROR_SUCCESS) {
                DWORD actual = 0, type = 0;
                size = sizeof(actual);
                status = RegQueryValueExW(key, value_name.c_str(), nullptr, &type,
                    reinterpret_cast<BYTE*>(&actual), &size);
                if (status == ERROR_SUCCESS &&
                    (type != REG_DWORD || size != sizeof(actual) || actual != 1))
                    status = ERROR_INVALID_DATA;
            }
            if (status == ERROR_SUCCESS) status = RegDeleteValueW(key, value_name.c_str());
        }
        if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) {
            size = 0;
            status = RegQueryValueExW(key, value_name.c_str(), nullptr, nullptr, nullptr, &size);
            if (status == ERROR_SUCCESS) status = ERROR_BUSY;
        }
        RegCloseKey(key);
        if (status == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (error) *error = registry_error(L"Disabling/removing OFXR registration", status);
        return false;
    } catch (...) {
        if (error) *error = L"Disarming the OpenXR layer failed.";
        return false;
    }
}

bool manifest_registered(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                registry_root(scope),
                subkey.c_str(),
                0,
                KEY_QUERY_VALUE | KEY_WOW64_64KEY,
                &key) != ERROR_SUCCESS) {
            return false;
        }
        const std::wstring value_name = normalized_path(manifest);
        DWORD type = 0;
        DWORD value = 1;
        DWORD size = sizeof(value);
        const LSTATUS status = RegQueryValueExW(
            key,
            value_name.c_str(),
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &size);
        RegCloseKey(key);
        return status == ERROR_SUCCESS && type == REG_DWORD &&
               size == sizeof(value) && value == 0;
    } catch (...) {
        return false;
    }
}

bool owned_manifest_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& runtime_directory,
    std::wstring_view prefix) noexcept {
    try {
        const auto normalized_manifest =
            std::filesystem::absolute(manifest).lexically_normal();
        const auto normalized_directory =
            std::filesystem::absolute(runtime_directory).lexically_normal();
        if (!equal_case_insensitive(
                normalized_manifest.parent_path().wstring(),
                normalized_directory.wstring())) {
            return false;
        }
        const std::wstring filename = normalized_manifest.filename().wstring();
        const std::wstring_view suffix(kManifestSuffix);
        return filename.size() > prefix.size() + suffix.size() &&
               equal_case_insensitive(
                   std::wstring_view(filename).substr(0, prefix.size()), prefix) &&
               equal_case_insensitive(
                   std::wstring_view(filename).substr(filename.size() - suffix.size()),
                   suffix);
    } catch (...) {
        return false;
    }
}

bool owned_registration_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory,
    std::wstring_view prefix) noexcept {
    try {
        if (!manifest.is_absolute() || !local_directory.is_absolute()) return false;
        for (const auto& part : manifest) {
            if (part == L".." || part == L".") return false;
        }
        const auto root = local_directory.lexically_normal() / L"RuntimeLayer";
        const auto parent = manifest.parent_path().lexically_normal();
        if (!equal_case_insensitive(parent.wstring(), root.wstring())) {
            if (!equal_case_insensitive(parent.parent_path().wstring(), root.wstring()))
                return false;
            const auto version = parent.filename().wstring();
            if (version.size() < 4 || (version.front() != L'v' && version.front() != L'V'))
                return false;
            for (std::size_t i = 1; i < version.size(); ++i)
                if (version[i] < L'0' || version[i] > L'9') return false;
        }
        if (!owned_manifest_path(manifest, parent, prefix)) return false;
        const auto name = manifest.filename().wstring();
        const std::wstring_view suffix(kManifestSuffix);
        const auto id = std::wstring_view(name).substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        const auto dash = id.find(L'-');
        if (dash == 0 || dash == id.npos || dash + 1 == id.size()) return false;
        for (std::size_t i = 0; i < id.size(); ++i)
            if (i != dash && (id[i] < L'0' || id[i] > L'9')) return false;
        return true;
    } catch (...) { return false; }
}

bool retire_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error,
    std::wstring_view registry_subkey) noexcept {
    try {
        std::wstring signal_error;
        const bool signalled = signal_arm_stop(manifest, &signal_error);
        if (!unregister_manifest(manifest, scope, error, registry_subkey)) return false;
        if (!signalled) {
            if (error) *error = signal_error;
            return false;
        }
        const auto attributes = GetFileAttributesW(manifest.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (error) *error = L"Refusing to remove a directory masquerading as an OFXR manifest: " + manifest.wstring();
            return false;
        }
        std::error_code removal_error;
        std::filesystem::remove(manifest, removal_error);
        if (removal_error) {
            if (error) *error = L"OFXR registration is disabled, but its manifest could not be removed: " +
                manifest.wstring() + L" (" + std::to_wstring(removal_error.value()) + L")";
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Unable to retire the OFXR manifest.";
        return false;
    }
}

bool cleanup_owned_registrations(
    const std::filesystem::path& local_directory,
    RegistryScope scope,
    std::wstring* error,
    std::wstring_view registry_subkey,
    std::wstring_view prefix) noexcept {
    try {
        if (error) error->clear();
        bool success = true;
        const auto fail = [&](const std::wstring& message) {
            success = false;
            if (error) {
                if (!error->empty()) *error += L"\r\n";
                *error += message;
            }
        };
        std::vector<std::filesystem::path> manifests;
        HKEY key = nullptr;
        const std::wstring subkey(registry_subkey);
        const auto opened = RegOpenKeyExW(registry_root(scope), subkey.c_str(), 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (opened == ERROR_SUCCESS) {
            // Snapshot names before deleting values so enumeration cannot skip.
            std::vector<wchar_t> name(32768);
            for (DWORD index = 0;; ++index) {
                DWORD count = static_cast<DWORD>(name.size());
                const auto status = RegEnumValueW(key, index, name.data(), &count,
                    nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) break;
                if (status != ERROR_SUCCESS) {
                    fail(registry_error(L"Enumerating OFXR registrations", status));
                    break;
                }
                const std::filesystem::path path(std::wstring(name.data(), count));
                if (owned_registration_path(path, local_directory, prefix)) manifests.push_back(path);
            }
            RegCloseKey(key);
        } else if (opened != ERROR_FILE_NOT_FOUND && opened != ERROR_PATH_NOT_FOUND) {
            fail(registry_error(L"Reading OFXR registrations", opened));
        }

        // Include orphaned JSONs, but never traverse a junction/symlink or
        // recursively visit unrelated directories. Registry values are still
        // removed when their JSON is missing.
        const auto root = local_directory / L"RuntimeLayer";
        const auto safe_directory = [](const std::filesystem::path& path) {
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        };
        // Nonthrowing iteration is important here: a locked/inaccessible cache
        // must not abort retirement of registrations already found above.
        const auto scan_directory = [&](const std::filesystem::path& directory, const auto& visit) {
            std::error_code scan_error;
            std::filesystem::directory_iterator cursor(directory, scan_error), end;
            while (!scan_error && cursor != end) {
                visit(cursor->path());
                cursor.increment(scan_error);
            }
            if (scan_error) fail(L"Unable to inspect OFXR manifest cache: " + directory.wstring());
        };
        if (safe_directory(local_directory) && safe_directory(root)) {
            const std::wstring probe = std::wstring(prefix) + L"1-1.json";
            scan_directory(root, [&](const std::filesystem::path& entry) {
                if (owned_registration_path(entry, local_directory, prefix)) {
                    manifests.push_back(entry);
                } else if (safe_directory(entry) &&
                    owned_registration_path(entry / probe, local_directory, prefix)) {
                    scan_directory(entry, [&](const std::filesystem::path& child) {
                        if (owned_registration_path(child, local_directory, prefix))
                            manifests.push_back(child);
                    });
                }
            });
        }
        std::sort(manifests.begin(), manifests.end());
        manifests.erase(std::unique(manifests.begin(), manifests.end()), manifests.end());
        for (const auto& manifest : manifests) {
            // Refuse file removal through reparse-point directories. We still
            // unregister the value, which alone prevents future discovery.
            const bool safe_parent = safe_directory(local_directory) && safe_directory(root) &&
                (equal_case_insensitive(manifest.parent_path().wstring(), root.wstring()) ||
                 safe_directory(manifest.parent_path()));
            std::wstring detail;
            if (!safe_parent) {
                if (!signal_arm_stop(manifest, &detail)) fail(detail);
                if (!unregister_manifest(manifest, scope, &detail, registry_subkey)) fail(detail);
                const auto attributes = GetFileAttributesW(manifest.c_str());
                const auto status = GetLastError();
                if (attributes != INVALID_FILE_ATTRIBUTES ||
                    (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND))
                    fail(L"OFXR manifest not removed through an unsafe cache directory: " + manifest.wstring());
            } else if (!retire_manifest(manifest, scope, &detail, registry_subkey)) {
                fail(detail);
            }
        }
        // Do not report Off if an owned registration survived or appeared while
        // cleanup was running. Values disabled with DWORD 1 count as leftovers too.
        key = nullptr;
        const auto verified = RegOpenKeyExW(registry_root(scope), subkey.c_str(), 0,
            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
        if (verified == ERROR_SUCCESS) {
            std::vector<wchar_t> name(32768);
            for (DWORD index = 0;; ++index) {
                DWORD count = static_cast<DWORD>(name.size());
                const auto status = RegEnumValueW(key, index, name.data(), &count,
                    nullptr, nullptr, nullptr, nullptr);
                if (status == ERROR_NO_MORE_ITEMS) break;
                if (status != ERROR_SUCCESS) {
                    fail(registry_error(L"Verifying OFXR cleanup", status));
                    break;
                }
                const std::filesystem::path path(std::wstring(name.data(), count));
                if (owned_registration_path(path, local_directory, prefix))
                    fail(L"OFXR registration remains after cleanup: " + path.wstring());
            }
            RegCloseKey(key);
        } else if (verified != ERROR_FILE_NOT_FOUND && verified != ERROR_PATH_NOT_FOUND) {
            fail(registry_error(L"Verifying OFXR registration removal", verified));
        }
        return success;
    } catch (...) {
        if (error) *error = L"Unable to inspect or clean OFXR registrations.";
        return false;
    }
}

} // namespace xrfg::implicit_layer
