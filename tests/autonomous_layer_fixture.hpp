#pragma once
#include "xrfg/autonomous_control.hpp"
#include "xrfg/embedded_control.hpp"
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <cstdio>

namespace xrfg_fixture {
inline bool request_control(const xrfg::embedded::Settings& desired) {
    auto s=xrfg::autonomous::snapshot().desired;
    s.advanced_mode=true;
    s.pacing_mode=desired.pacing_mode;
    s.increase_buffer=desired.increase_buffer;
    s.enabled=desired.enabled; s.backend=desired.backend; s.preset=desired.preset;
    s.scale=desired.scale; s.backward=desired.backward; s.lower_vram=desired.lower_vram;
    s.separate_synthesis=desired.separate_synthesis;
    const int canonical_generation=desired.hybrid && desired.frame_generation==0
        ?3:desired.frame_generation;
    s.motion_vectors=desired.motion_vectors; s.frame_generation=canonical_generation;
    s.native_scale=static_cast<int>(desired.native_scale);
    const auto pipeline=xrfg::embedded::djules_pipeline_snapshot();
    s.prefer_fps=pipeline.prefer_fps; s.triple_frame_gen=pipeline.triple_frame_gen;
    if (!xrfg::autonomous::request(s)) return false;
    xrfg::autonomous::apply_runtime_settings();
    auto effective=desired;
    effective.hybrid=canonical_generation==3;
    effective.frame_generation=canonical_generation==3?0:canonical_generation;
    if (effective.frame_generation == 2) effective.backend = 0;
    effective.motion_vectors=xrfg::autonomous::snapshot().desired.motion_vectors;
    const auto actual=xrfg::embedded::snapshot().desired;
    if(actual!=effective) {
#define TRACE_SETTING(name) if(actual.name!=effective.name) std::fprintf(stderr,"fixture setting %s: actual=%d expected=%d\n",#name,int(actual.name),int(effective.name));
        TRACE_SETTING(enabled) TRACE_SETTING(increase_buffer) TRACE_SETTING(pacing_mode)
        TRACE_SETTING(backend) TRACE_SETTING(preset) TRACE_SETTING(scale) TRACE_SETTING(backward)
        TRACE_SETTING(separate_synthesis) TRACE_SETTING(lower_vram) TRACE_SETTING(motion_vectors)
        TRACE_SETTING(frame_generation) TRACE_SETTING(native_scale) TRACE_SETTING(hybrid)
#undef TRACE_SETTING
    }
    return actual==effective;
}
class ControlProfile {
    std::filesystem::path directory_;
public:
    ControlProfile() {
        directory_=std::filesystem::temp_directory_path()/
            (L"ofxr-autonomous-layer-test-"+std::to_wstring(GetCurrentProcessId())+
                L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory_);
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",directory_.c_str());
        auto initial=xrfg::embedded::snapshot().desired;initial.pacing_mode=2;initial.enabled=true;
        initial.increase_buffer=false; // independent fixture baseline, not the prior test's saved flag
        if (!request_control(initial))
            throw std::runtime_error("autonomous fixture publication failed");
    }
    ~ControlProfile() {
        auto restored=xrfg::embedded::snapshot().desired;restored.pacing_mode=2;
        static_cast<void>(request_control(restored));
        std::error_code error;
        std::filesystem::remove(directory_/L"ofxr-bridge.ini",error);
        if(directory_.is_absolute() && directory_.filename().wstring().starts_with(L"ofxr-autonomous-layer-test-"))std::filesystem::remove_all(directory_,error);
    }
};
}
