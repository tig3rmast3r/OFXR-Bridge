#include "xrfg/implicit_layer.hpp"

#include <windows.h>

#include <array>
#include <cwchar>
#include <system_error>

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

} // namespace

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
            L"fidelityfx",
            value.data(),
            static_cast<DWORD>(value.size()),
            ini_path.c_str());
        if (length > 0 && length < value.size() &&
            (_wcsicmp(value.data(), L"nvidia") == 0 ||
             _wcsicmp(value.data(), L"nvof") == 0)) {
            return ConfiguredFlowBackend::nvidia;
        }
    } catch (...) {
    }
    return ConfiguredFlowBackend::fidelity_fx;
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
    std::wstring* error,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        const LSTATUS create_status = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            subkey.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE | KEY_WOW64_64KEY,
            nullptr,
            &key,
            nullptr);
        if (create_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Opening the OpenXR layer key", create_status);
            return false;
        }
        const std::wstring value_name = normalized_path(manifest);
        constexpr DWORD enabled = 0;
        const LSTATUS set_status = RegSetValueExW(
            key,
            value_name.c_str(),
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&enabled),
            sizeof(enabled));
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
    std::wstring* error,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        const LSTATUS open_status = RegOpenKeyExW(
            HKEY_CURRENT_USER,
            subkey.c_str(),
            0,
            KEY_SET_VALUE | KEY_WOW64_64KEY,
            &key);
        if (open_status == ERROR_FILE_NOT_FOUND || open_status == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        if (open_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Opening the OpenXR layer key", open_status);
            return false;
        }
        const std::wstring value_name = normalized_path(manifest);
        const LSTATUS delete_status = RegDeleteValueW(key, value_name.c_str());
        RegCloseKey(key);
        if (delete_status == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (delete_status != ERROR_SUCCESS) {
            if (error) *error = registry_error(L"Disarming the OpenXR layer", delete_status);
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Disarming the OpenXR layer failed.";
        return false;
    }
}

bool manifest_registered(
    const std::filesystem::path& manifest,
    std::wstring_view registry_subkey) noexcept {
    try {
        const std::wstring subkey(registry_subkey);
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                HKEY_CURRENT_USER,
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
    const std::filesystem::path& runtime_directory) noexcept {
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
        const std::wstring_view prefix(kManifestPrefix);
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

} // namespace xrfg::implicit_layer
