#pragma once
#include <algorithm>
#include <array>
#include <span>
#include <string_view>
namespace xrfg {
// Black bars the producer draws around the picture cover a mark placed at the
// title's inset. Luke's Optimize VFOV on The Last of Us Part I under VDXR drew
// 159 rows at the top, about 350 at the bottom and 80 columns at the left of a
// 3456x3773 eye, over the band at rows 34-42: no colour was paired with its
// guide and neither AFW nor OSR engaged (2026-11-02). The mark moves inside the
// picture by the measured bar plus a margin, never further in than a tenth of
// the eye; a larger bar leaves it at that limit.
struct LukeOriginBars { unsigned left{}, top{}, bottom{}; };
// GTA final-colour transforms can change without changing the eye dimensions.
// Use only complete CRC observations to measure proximity to the left edge;
// consecutive nonblack misses move the next written marker, never invent an ID.
struct LukeOriginCropGuard {
    unsigned width{},height{},inset{64},misses{};
    unsigned configure(unsigned w,unsigned h) noexcept {
        if(width!=w||height!=h) {width=w;height=h;inset=64;misses=0;}
        return inset;
    }
    bool observe(bool valid,bool nonblack,bool ambiguous,int first_column) noexcept {
        if(ambiguous) {misses=0;return false;}
        if(valid) {
            misses=0;
            if(first_column<0||first_column>=16)return false;
        } else {
            if(!nonblack) {misses=0;return false;}
            if(++misses<8)return false;
            misses=0;
        }
        const auto maximum=std::min(512U,width>260?(width-260)/2:0U);
        const auto next=std::min(maximum,inset+32);
        if(next<=inset)return false;
        inset=next;return true;
    }
};
inline constexpr unsigned kLukeOriginBarMargin = 8;
inline constexpr unsigned kLukeOriginBarLimitDivisor = 10;
inline constexpr std::array<unsigned,2> luke_origin_inset_with_bars(std::array<unsigned,2> title,
    LukeOriginBars bars, unsigned width, unsigned height, bool from_bottom) {
    const unsigned bar_y = from_bottom ? bars.bottom : bars.top;
    const unsigned limit_x = width / kLukeOriginBarLimitDivisor, limit_y = height / kLukeOriginBarLimitDivisor;
    const unsigned x = bars.left ? std::min(bars.left + kLukeOriginBarMargin, limit_x) : 0;
    const unsigned y = bar_y ? std::min(bar_y + kLukeOriginBarMargin, limit_y) : 0;
    return {std::max(title[0], x), std::max(title[1], y)};
}
// Exactly black samples from one end of a line (bars are written as zero; the
// scene next to them is not). A line black throughout measures nothing.
inline constexpr bool luke_origin_black_run(std::span<const float> line, bool from_end, unsigned& run) {
    run = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const float value = line[from_end ? line.size() - 1 - i : i];
        if (value != 0.F) return true;
        ++run;
    }
    run = 0;
    return false;
}
inline constexpr bool origin_title_equal(std::wstring_view a,std::wstring_view b) {
    if(a.size()!=b.size())return false;
    const auto lower=[](wchar_t c){return c>=L'A'&&c<=L'Z'?wchar_t(c+L'a'-L'A'):c;};
    for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;
    return true;
}
inline constexpr std::array<unsigned,2> luke_origin_inset_for_title(std::wstring_view name,
    unsigned color_height = 0) {
    if(origin_title_equal(name,L"Outlaws.exe"))return {256,32};
    // GTA's realtime scripted scenes cover the top eighth of the final eye.
    // Keep the same eight-row token below that mask, with a resolution-relative
    // margin. PID7020 on Luke2611.7.2 retained a CRC-valid payload but lost
    // the sync prefix past the left eye edge. Keep 64 pixels of horizontal
    // inset for that final-color translation. Writer and reader share the
    // full-size eye geometry, before AFW quality scaling.
    if(origin_title_equal(name,L"GTA5_Enhanced.exe"))return {64,color_height/6>32?color_height/6:32};
    if(origin_title_equal(name,L"tlou-i.exe"))return {32,32};
    // Uncharted: Legacy of Thieves Collection: its eye shows the output
    // magnified about 1.05 and shifted left, by up to 43 pixels at 3600 wide
    // (the band's first 24 bits fell off the edge with a 32-pixel inset).
    if(origin_title_equal(name,L"tll.exe")||origin_title_equal(name,L"tll-l.exe")||
        origin_title_equal(name,L"u4.exe")||origin_title_equal(name,L"u4-l.exe"))return {256,32};
    return {0,0};
}
// DOOM's HDR-to-SDR pass and GTA scripted camera grading crush the old
// 1/16 stamp. Reuse full-white float samples; keep geometry/CRC unchanged.
inline constexpr bool luke_origin_bright_primary_for_title(std::wstring_view name) {
    return origin_title_equal(name,L"DOOMTheDarkAges.exe") ||
        origin_title_equal(name,L"GTA5_Enhanced.exe");
}
// Titles whose band sits at the bottom-left: Uncharted draws its night sky
// and rain over the top of the image after DLSS, covering a top band entirely
// (F8: the stamped output held it, the eye image showed sky and rain there).
inline constexpr bool luke_origin_from_bottom_for_title(std::wstring_view name) {
    return origin_title_equal(name,L"tll.exe")||origin_title_equal(name,L"tll-l.exe")||
        origin_title_equal(name,L"u4.exe")||origin_title_equal(name,L"u4-l.exe");
}
}
