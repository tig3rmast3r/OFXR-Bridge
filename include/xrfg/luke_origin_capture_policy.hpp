#pragma once
#include "embedded_control.hpp"

namespace xrfg {
// AER owns raw real-eye colours; saved reconstruction preferences are bypassed
// while it is active. Only its native-guide consumers need protected origins.
constexpr bool luke_origin_capture_required(const embedded::Settings& generation,
    const embedded::AfwSettings& reconstruction) noexcept {
    if(generation.enabled && generation.ofr)
        return generation.rife_model<0 &&
            (generation.frame_generation==1 || generation.frame_generation==2 || generation.hybrid);
    return reconstruction.mode!=0;
}
}
