#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace xrfg {
inline bool valid_shared_rife_runtime(const std::filesystem::path& path) {
    std::error_code error;
    return path.is_absolute() &&
        std::filesystem::is_regular_file(path/L"python/python.exe",error) &&
        std::filesystem::is_regular_file(path/L"app/worker.py",error);
}
inline std::filesystem::path find_shared_rife_runtime(std::filesystem::path directory) {
    if(directory.empty())return {};
    directory=std::filesystem::absolute(directory);
    for(unsigned i=0;i<12;++i){
        auto runtime=directory/L"runtime/rife";
        if(valid_shared_rife_runtime(runtime))return runtime.lexically_normal();
        const auto parent=directory.parent_path();
        if(parent==directory || parent.empty())break;
        directory=parent;
    }
    return {};
}
// The tray resolves from its ORIGINAL package before the layer is staged in
// AppData. Only its owned generated INI carries this transient locator; it is
// neither a per-game preference nor an installed/system Python search path.
inline std::string with_shared_rife_runtime(std::string configuration,
    const std::filesystem::path& package_directory) {
    const auto path=find_shared_rife_runtime(package_directory);
    if(path.empty())return configuration;
    const auto utf8=path.generic_u8string();
    std::string entry="rife_runtime_dir=";
    entry.append(reinterpret_cast<const char*>(utf8.data()),utf8.size());
    entry+="\r\n";
    const auto section=configuration.find("[ofxr]\r\n");
    if(section!=configuration.npos)configuration.insert(section+8,entry);
    return configuration;
}
inline std::filesystem::path staged_rife_runtime(const std::filesystem::path& module_directory) {
    std::ifstream file(module_directory/L"ofxr_bridge.ini",std::ios::binary);
    bool owned_section=false;
    for(std::string line;std::getline(file,line);){
        if(!line.empty() && line.back()=='\r')line.pop_back();
        if(!line.empty() && line.front()=='['){owned_section=line=="[ofxr]";continue;}
        constexpr std::string_view key="rife_runtime_dir=";
        if(owned_section && line.starts_with(key) && line.size()<32768){
            const auto path=std::filesystem::u8path(line.substr(key.size()));
            if(valid_shared_rife_runtime(path))return path;
        }
    }
    return find_shared_rife_runtime(module_directory);
}
}
