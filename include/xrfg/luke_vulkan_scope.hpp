#pragma once
#include <windows.h>
#include <cwchar>

namespace xrfg::native_vulkan {
inline bool luke_vulkan_process_name(const wchar_t* name) noexcept {
    return name && (_wcsicmp(name,L"DOOMTheDarkAges.exe")==0 || _wcsicmp(name,L"TheGreatCircle.exe")==0 ||
        _wcsicmp(name,L"RDR2.exe")==0);
}
inline bool luke_vulkan_process() noexcept {
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(nullptr, path, 32768)) return false;
    const auto* name = std::wcsrchr(path, L'\\');
    return luke_vulkan_process_name(name ? name + 1 : path);
}
}
