#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

namespace xrfg {
inline std::filesystem::path managed_rife_runtime() {
#ifdef _WIN32
    wchar_t value[32768]{};
    const auto size=GetEnvironmentVariableW(L"LOCALAPPDATA",value,32768);
    if(size && size<32768)return std::filesystem::path(value)/L"ofxr-bridge/RIFE/runtime";
#endif
    return {};
}
inline bool valid_rife_runtime(const std::filesystem::path& path) {
    std::error_code error;
    return path.is_absolute() &&
        std::filesystem::is_regular_file(path/L"python/python.exe",error) &&
        std::filesystem::is_regular_file(path/L"app/worker.py",error);
}
// AppData is the sole automatic location. A developer may set an absolute
// rife_runtime_dir explicitly in the generated [ofxr] INI. Never search paths.
inline std::filesystem::path staged_rife_runtime(const std::filesystem::path& module_directory) {
    std::ifstream file(module_directory/L"ofxr_bridge.ini",std::ios::binary);
    bool owned_section=false;
    for(std::string line;std::getline(file,line);){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(!line.empty() && line.front()=='['){owned_section=line=="[ofxr]";continue;}
        constexpr std::string_view key="rife_runtime_dir=";
        if(owned_section && line.starts_with(key) && line.size()<32768){
            if(line.size()==key.size())break;
            const auto path=std::filesystem::u8path(line.substr(key.size()));
            return valid_rife_runtime(path) ? path : std::filesystem::path{};
        }
    }
    const auto managed=managed_rife_runtime();
    return valid_rife_runtime(managed) ? managed : std::filesystem::path{};
}
}
