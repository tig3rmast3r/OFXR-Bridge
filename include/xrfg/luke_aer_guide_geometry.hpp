#pragma once
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/fsr_guide_geometry.hpp"
#include <span>

namespace xrfg {
// A saved AER preference cannot suppress reconstruction guides while global
// frame generation is off. Callers pass effective AER, not the saved flag.
constexpr bool luke_guide_capture_requested(bool luke,bool aer,
    D3D12FrameGenerationMode mode,bool hybrid,bool game_motion) noexcept {
    return game_motion || mode!=D3D12FrameGenerationMode::ofxr ||
        (aer && hybrid) || (luke && !aer);
}
// AFW's transport/post-reconstruction path remains OFXR. Raw AER framegen
// must instantiate the selected provider, including at initial eye creation.
constexpr D3D12FrameGenerationMode luke_primary_generation_mode(bool luke,bool aer,
    D3D12FrameGenerationMode selected) noexcept {
    return luke && !aer?D3D12FrameGenerationMode::ofxr:selected;
}
// An exact colour token establishes production identity, not pixel placement.
// REAL VR can crop the completed SR output before releasing the XR eye. Keep
// the full native grids for N1 composition, then place their sampling windows
// by the native and submitted projections. No texture copy or eye reordering.
inline std::shared_ptr<const DlssMotionVectorSet> place_luke_aer_guides(
    const std::shared_ptr<const DlssMotionVectorSet>& input,
    std::span<const DlssEyeView> views) noexcept {
    try {
        if(!input || input->eye_count!=views.size() || views.empty() ||
            views.size()>kDlssMotionVectorEyeCount)return {};
        std::shared_ptr<DlssMotionVectorSet> output;
        for(std::size_t i=0;i<views.size();++i) {
            const auto& frame=input->eyes[i];const auto& view=views[i];
            if(!frame || view.x<0 || view.y<0 || !view.width || !view.height)return {};
            if(frame->output_x==unsigned(view.x) && frame->output_y==unsigned(view.y) &&
                frame->output_width==view.width && frame->output_height==view.height)continue;
            auto placed=place_guide_in_view(*frame,view);
            if(!placed)return {};
            if(!output)output=std::make_shared<DlssMotionVectorSet>(*input);
            output->eyes[i]=std::move(placed);
        }
        return output?output:input;
    } catch(...) {return {};}
}

// Luke's existing FSR crop already uses whole-texel copies. Retain that policy
// for camera-proven AER windows, rounding at most half a guide texel per value;
// other FSR integrations keep fsr_guide_regions' strict fractional refusal.
inline std::optional<FsrGuideRegions> fsr_luke_aer_guide_regions(
    const DlssMotionVectorFrame& frame) noexcept {
    const auto region=[](const std::array<float,4>& rect,unsigned x,unsigned y,
        unsigned width,unsigned height)->std::optional<std::array<unsigned,4>> {
        std::array<unsigned,4> result{};
        for(unsigned i=0;i<4;++i) {
            const double v=std::round(double(rect[i]));
            if(!std::isfinite(v) || v<0 || v>UINT32_MAX)return {};
            result[i]=unsigned(v);
        }
        if(!result[2] || !result[3] || result[0]<x || result[1]<y ||
            std::uint64_t(result[0])+result[2]>std::uint64_t(x)+width ||
            std::uint64_t(result[1])+result[3]>std::uint64_t(y)+height)return {};
        return result;
    };
    auto m=region(guide_motion_rect(frame),frame.motion_x,frame.motion_y,frame.motion_width,frame.motion_height);
    auto d=region(guide_depth_rect(frame),frame.depth_x,frame.depth_y,frame.depth_width,frame.depth_height);
    if(!m || !d)return {};
    return FsrGuideRegions{*m,*d};
}
} // namespace xrfg
