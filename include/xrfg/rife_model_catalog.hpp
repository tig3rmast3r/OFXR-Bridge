#pragma once
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace xrfg {
struct RifeModelChoice { int id; std::string name; };
// IDs are the worker protocol, not an assumed list of installed weights.
inline std::string rife_model_name(int id) {
    return id==2 ? "4.6" : id==4 ? "4.3" : "4.4";
}
inline std::vector<RifeModelChoice> available_rife_models(const std::filesystem::path& runtime) {
    std::vector<RifeModelChoice> result;
    std::error_code error;
    if(!runtime.is_absolute())return result;
    std::filesystem::directory_iterator files(runtime/L"models",error),end;
    for(;!error && files!=end;files.increment(error)) {
        if(!files->is_regular_file(error))continue;
        const auto utf8=files->path().filename().u8string();
        const std::string name(utf8.begin(),utf8.end());
        if(!name.starts_with("flownet_v") || !name.ends_with(".pkl"))continue;
        const auto version=name.substr(9,name.size()-13);
        const int id=version=="4.6" ? 2 : version=="4.4" ? 3 : version=="4.3" ? 4 : -1;
        // Unknown/retired networks cannot be offered to a worker which has no
        // compatible inference implementation. Never import arbitrary Python.
        if(id>=0)result.push_back({id,version});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    return result;
}
}
