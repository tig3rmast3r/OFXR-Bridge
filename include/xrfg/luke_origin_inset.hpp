#pragma once
#include <array>
#include <string_view>
namespace xrfg {
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
    // margin. Both writer and reader use the observed full-size eye height;
    // AFW quality scaling happens later and must not move this location.
    if(origin_title_equal(name,L"GTA5_Enhanced.exe"))return {0,color_height/6>32?color_height/6:32};
    if(origin_title_equal(name,L"tlou-i.exe"))return {32,32};
    return {0,0};
}
}
