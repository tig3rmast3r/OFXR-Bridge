#pragma once

namespace xrfg {
// Mark the original native output for its actual consumer. AER suppresses
// reconstruction, but Hybrid and native FG still need exact colour identity.
constexpr bool luke_origin_marker_requested(bool framegen_enabled,bool saved_aer,
    int reconstruction_mode,int generation_mode,bool hybrid,int rife_model) noexcept {
    if(framegen_enabled && saved_aer)
        return rife_model<0 && (generation_mode==1 || generation_mode==2 || hybrid);
    return reconstruction_mode!=0;
}
}
