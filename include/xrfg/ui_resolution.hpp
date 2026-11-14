#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace xrfg {
inline constexpr std::uint32_t kMenuLogicalWidth=1024,kMenuLogicalHeight=768;
struct MenuResolution {std::uint32_t width{},height{};float density{};};
// Select physical UI pixels from the current eye viewport, with bounded memory.
// Quarter steps keep both texture dimensions integral and avoid churn from tiny changes.
inline MenuResolution menu_resolution(std::uint32_t eye_width,std::uint32_t limit_width,std::uint32_t limit_height) noexcept {
    if(limit_width<kMenuLogicalWidth || limit_height<kMenuLogicalHeight)return {};
    const float requested=std::clamp(std::ceil(float(eye_width)*.75F/kMenuLogicalWidth*4.F)/4.F,1.F,2.F);
    const float limit=std::floor(std::min(float(limit_width)/kMenuLogicalWidth,float(limit_height)/kMenuLogicalHeight)*4.F)/4.F;
    const float density=std::min(requested,limit);
    return {std::uint32_t(kMenuLogicalWidth*density),std::uint32_t(kMenuLogicalHeight*density),density};
}
}
