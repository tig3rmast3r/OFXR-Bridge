#pragma once
#include <openxr/openxr.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>

namespace xrfg {
struct RifeGameShape {
    unsigned width{},height{},eyes{};
    auto operator<=>(const RifeGameShape&) const = default;
};
struct RifeGameLayout {
    std::array<RifeGameShape,4> shapes{};
    unsigned count{};
    bool operator==(const RifeGameLayout&) const = default;
};
// Primary projection only. Overlay quads, texture padding, viewport offsets,
// poses and swapchain handle changes are not new inference dimensions.
inline RifeGameLayout rife_game_layout(const XrFrameEndInfo* frame) noexcept {
    RifeGameLayout out;
    if(!frame || !frame->layers)return out;
    for(unsigned layer=0;layer<frame->layerCount;++layer) {
        const auto* base=frame->layers[layer];
        if(!base || base->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
        const auto& p=*reinterpret_cast<const XrCompositionLayerProjection*>(base);
        if(!p.views || p.viewCount<2 || p.viewCount>4)continue;
        std::array<bool,4> seen{};
        for(unsigned i=0;i<p.viewCount;++i) {
            const auto& a=p.views[i].subImage;
            if(!a.swapchain || a.imageRect.extent.width<=0 || a.imageRect.extent.height<=0 ||
                a.imageRect.extent.width>16384 || a.imageRect.extent.height>16384)return {};
        }
        for(unsigned i=0;i<p.viewCount;++i) {
            if(seen[i])continue;
            const auto& a=p.views[i].subImage;
            unsigned eyes=1;
            for(unsigned j=i+1;j<p.viewCount;++j) {
                const auto& b=p.views[j].subImage;
                if(a.swapchain!=b.swapchain)continue;
                seen[j]=true;bool duplicate=false;
                for(unsigned k=i;k<j;++k) {
                    const auto& previous=p.views[k].subImage;
                    if(previous.swapchain==b.swapchain && previous.imageArrayIndex==b.imageArrayIndex &&
                        previous.imageRect.offset.x==b.imageRect.offset.x && previous.imageRect.offset.y==b.imageRect.offset.y &&
                        previous.imageRect.extent.width==b.imageRect.extent.width && previous.imageRect.extent.height==b.imageRect.extent.height)
                        duplicate=true;
                }
                if(!duplicate)++eyes;
            }
            if(eyes>2)return {}; // RIFE's accepted transaction has at most two views
            // Native RIFE takes its network dimensions from the first view of
            // the resource, even when its peer has a different source viewport.
            out.shapes[out.count++]={unsigned(a.imageRect.extent.width),unsigned(a.imageRect.extent.height),eyes};
        }
        std::sort(out.shapes.begin(),out.shapes.begin()+out.count);
        const auto end=std::unique(out.shapes.begin(),out.shapes.begin()+out.count);
        out.count=unsigned(end-out.shapes.begin());
        std::fill(out.shapes.begin()+out.count,out.shapes.end(),RifeGameShape{});
        return out;
    }
    return out;
}
inline std::string rife_json_string(const std::string& value) {
    std::string out="\"";constexpr char hex[]="0123456789abcdef";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else out+=char(c);
    }
    return out+'"';
}
class RifeGameHistory {
    std::mutex mutex_;
    RifeGameLayout saved_{};
    ULONGLONG retry_{};
public:
    // First valid submission / changed layout only. No logging, GPU readback,
    // per-frame file polling or growing history. Failure never affects XR.
    template<class Adapter>
    bool observe(const RifeGameLayout& layout,const std::filesystem::path& folder,
        const std::filesystem::path& executable,Adapter adapter) noexcept {
        if(!layout.count)return false;
        try {
            std::scoped_lock lock(mutex_);
            if(saved_==layout || GetTickCount64()<retry_)return false;
            retry_=GetTickCount64()+5000;
            auto identity=executable.lexically_normal().wstring();
            std::uint64_t hash=14695981039346656037ULL;
            for(auto c:identity){c=std::towlower(c);hash^=unsigned(c);hash*=1099511628211ULL;}
            std::ostringstream key;key.imbue(std::locale::classic());key<<std::hex<<hash;
            std::filesystem::create_directories(folder);
            const auto path=folder/(key.str()+".json");
            const auto temporary=folder/(key.str()+"."+std::to_string(GetCurrentProcessId())+".tmp");
            const auto utf8=[](const std::filesystem::path& p){const auto s=p.generic_u8string();return std::string(s.begin(),s.end());};
            std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
            file.imbue(std::locale::classic()); // games may change the process-wide numeric locale
            file<<"{\"schema\":1,\"game\":"<<rife_json_string(utf8(executable.filename()))
                <<",\"executable\":"<<rife_json_string(utf8(executable))
                <<",\"adapter_luid\":"<<rife_json_string(std::to_string(adapter()))<<",\"shapes\":[";
            for(unsigned i=0;i<layout.count;++i){const auto s=layout.shapes[i];if(i)file<<',';
                file<<"{\"width\":"<<s.width<<",\"height\":"<<s.height<<",\"eyes\":"<<s.eyes<<'}';}
            file<<"]}\n";file.close();
            // Atomic buffered replacement: do not force a physical disk flush
            // into the game's resolution-change path for disposable metadata.
            if(!file || !MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING)) {
                std::error_code ec;std::filesystem::remove(temporary,ec);retry_=GetTickCount64()+5000;return false;
            }
            saved_=layout;retry_=0;return true;
        }catch(...){return false;}
    }
    template<class Adapter> void observe(const XrFrameEndInfo* frame,Adapter adapter) noexcept {
        const auto layout=rife_game_layout(frame);
        if(!layout.count)return;
        try {
            static const auto folder=[] {wchar_t p[32768]{};
                const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",p,32768);
                return n && n<32768?std::filesystem::path(p)/L"ofxr-bridge/RIFE/games":std::filesystem::path{};}();
            static const auto executable=[] {wchar_t p[32768]{};
                const auto n=GetModuleFileNameW(nullptr,p,32768);
                return n && n<32768?std::filesystem::path(p):std::filesystem::path{};}();
            if(!folder.empty()&&!executable.empty())observe(layout,folder,executable,adapter);
        }catch(...){}
    }
};
}
