#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace xrfg {
// A working canvas, NOT a guess about which side the producer cropped. All
// original pixel centres keep their original rays: add storage at right/bottom
// and extend only those tangent-plane edges by exactly the added pixel pitch.
// The submitted XR FOV/rect are never changed. Copy out the original peer rect.
struct LukeEyeCanvas {
    unsigned width{}, height{};
    std::array<float,4> source_fov{}, destination_fov{}; // left,right,up,down
};
inline bool luke_canvas_full_rect(std::array<unsigned,4> rect,unsigned w,unsigned h) noexcept {
    return rect[0]==0 && rect[1]==0 &&
        ((rect[2]==w && rect[3]==h) || (rect[2]==0 && rect[3]==0));
}
inline std::optional<LukeEyeCanvas> luke_eye_canvas(unsigned sw,unsigned sh,unsigned dw,unsigned dh,
    std::array<float,4> source,std::array<float,4> destination,
    std::array<unsigned,4> source_rect={},std::array<unsigned,4> destination_rect={}) noexcept {
    if(!sw||!sh||!dw||!dh || std::max(sw,dw)>16384 || std::max(sh,dh)>8192 ||
        std::max(sw,dw)-std::min(sw,dw)>4 || std::max(sh,dh)-std::min(sh,dh)>4 ||
        !luke_canvas_full_rect(source_rect,sw,sh)||!luke_canvas_full_rect(destination_rect,dw,dh))return {};
    LukeEyeCanvas out{std::max(sw,dw),std::max(sh,dh),source,destination};
    const auto expand=[&](std::array<float,4>& f,unsigned w,unsigned h) {
        for(float a:f)if(!std::isfinite(a)||std::abs(a)>=1.5707963267948966)return false;
        if(!(f[0]<f[1] && f[3]<f[2]))return false;
        // Keep unchanged axes bit-identical, including the equal-size fast path.
        if(w!=out.width)f[1]=static_cast<float>(std::atan(std::tan(double(f[0]))+
            (std::tan(double(f[1]))-std::tan(double(f[0])))*double(out.width)/w));
        if(h!=out.height)f[3]=static_cast<float>(std::atan(std::tan(double(f[2]))+
            (std::tan(double(f[3]))-std::tan(double(f[2])))*double(out.height)/h));
        return std::isfinite(f[1])&&std::isfinite(f[3]);
    };
    if(!expand(out.source_fov,sw,sh)||!expand(out.destination_fov,dw,dh))return {};
    return out;
}
struct LukeCanvasCopy {unsigned left{},top{},right{},bottom{},x{},y{};};
struct LukeCanvasCopies {std::array<LukeCanvasCopy,25> regions{};unsigned count{};};
inline LukeCanvasCopies luke_canvas_copies(unsigned w,unsigned h,unsigned cw,unsigned ch) noexcept {
    LukeCanvasCopies out{};
    if(!w||!h||cw<w||ch<h||cw-w>4||ch-h>4)return out;
    out.regions[out.count++]={0,0,w,h,0,0};
    for(unsigned x=w;x<cw;++x)out.regions[out.count++]={w-1,0,w,h,x,0};
    for(unsigned y=h;y<ch;++y) {
        out.regions[out.count++]={0,h-1,w,h,0,y};
        for(unsigned x=w;x<cw;++x)out.regions[out.count++]={w-1,h-1,w,h,x,y};
    }
    return out;
}
} // namespace xrfg
