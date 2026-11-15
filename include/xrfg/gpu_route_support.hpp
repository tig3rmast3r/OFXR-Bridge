#pragma once
#include "xrfg/autonomous_control.hpp"
#include "xrfg/embedded_control.hpp"
#include <windows.h>

namespace xrfg {
// Cached DXCore PCI identity, independent of OptiScaler's DXGI/User32 spoofing.
unsigned rendering_gpu_vendor() noexcept;
void set_rendering_gpu_luid(LUID luid) noexcept;
inline bool radeon_gpu(unsigned vendor) noexcept { return vendor==0x1002; }
inline unsigned settings_gpu_vendor(const autonomous::Snapshot& s) noexcept {
    return s.connected&&s.runtime.gpu_vendor ? s.runtime.gpu_vendor : rendering_gpu_vendor();
}
inline bool constrain_radeon_routes(autonomous::Settings& s,unsigned vendor) noexcept {
    if(!radeon_gpu(vendor))return false;
    bool changed=false;
    if(s.frame_generation==2) {s.frame_generation=3;s.motion_vectors=1;s.triple_frame_gen=false;changed=true;}
    if(s.frame_generation==0 && s.backend!=0){s.backend=0;changed=true;}
    if(s.hybrid_backend!=0){s.hybrid_backend=0;changed=true;}
    return changed;
}
inline bool constrain_radeon_routes(embedded::Settings& s,unsigned vendor) noexcept {
    if(!radeon_gpu(vendor)||s.rife_model>=0)return false;
    bool changed=false;
    if(s.frame_generation==2){s.frame_generation=0;s.hybrid=true;s.motion_vectors=1;changed=true;}
    if(s.backend!=0){s.backend=0;changed=true;}
    return changed;
}
}
