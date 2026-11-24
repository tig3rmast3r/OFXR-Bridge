#pragma once
#include "xrfg/autonomous_control.hpp"
#include "xrfg/embedded_control.hpp"
#include <windows.h>

namespace xrfg {
// Cached DXCore PCI identity, independent of OptiScaler's DXGI/User32 spoofing.
unsigned rendering_gpu_vendor() noexcept;
void set_rendering_gpu_luid(LUID luid) noexcept;
inline bool radeon_gpu(unsigned vendor) noexcept { return vendor==0x1002; }
constexpr bool framegen_requires_native_stereo(int generation) noexcept {
    return generation == 1 || generation == 2; // FSR and DLSS-G
}
inline int reconstruction_optical_backend(unsigned vendor) noexcept {
    return radeon_gpu(vendor) ? 0 : 1; // FidelityFX or NVIDIA OFA
}
inline bool select_luke_reconstruction_generator(autonomous::Settings& settings,
    const autonomous::RuntimeStatus& runtime) noexcept {
    if (!runtime.sessions || !runtime.luke_detected || runtime.afw_only_session ||
        !settings.enabled || settings.ofr_enabled || settings.afw_mode == 0 ||
        !framegen_requires_native_stereo(settings.frame_generation)) return false;
    const auto vendor = runtime.gpu_vendor ? runtime.gpu_vendor : rendering_gpu_vendor();
    const int backend = reconstruction_optical_backend(vendor);
    autonomous::select_generation(settings, 0, backend);
    settings.motion_vectors = 0;
    settings.generation_profiles[backend].motion_vectors = 0;
    return true;
}
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
