#pragma once
#include <algorithm>
#include <array>
#include <cstdlib>

namespace xrfg {
inline constexpr std::array<int,7> kRifeResolutions{10,15,18,21,25,30,36};
inline constexpr std::array<const char*,7> kRifeQualityNames{"Potato","Low","Medium","High","Very high","Ultra","Godlike"};
inline int rife_resolution_index(int value) noexcept {
    value=std::clamp(value,kRifeResolutions.front(),kRifeResolutions.back());
    int best{},distance=100;
    for(int i=0;i<int(kRifeResolutions.size());++i) {
        const int d=std::abs(value-kRifeResolutions[i]);
        if(d<distance){best=i;distance=d;}
    }
    return best;
}
inline int rife_resolution_preset(int value) noexcept {
    return kRifeResolutions[rife_resolution_index(value)];
}
}
