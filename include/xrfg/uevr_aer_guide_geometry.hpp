#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include <cmath>

namespace xrfg {
enum class UevrAerCropReason : unsigned {
    ready, missing_guide, ambiguous_eyes, invalid_rectangle, unknown_full_image,
    invalid_grid, incompatible_placement
};

// The caller supplies an already release-bound, whole-image guide for one
// UEVR AFR eye. Its XR subimage is a literal crop of those same colour pixels.
// Only sampling metadata changes; camera/temporal identity and GPU ownership
// are retained. This is not a resolver or a size-based choice of DLSS stream.
inline std::shared_ptr<const DlssMotionVectorSet> place_uevr_aer_guides(
    const std::shared_ptr<const DlssMotionVectorSet>& input,
    unsigned image_width, unsigned image_height, const DlssEyeView& view,
    unsigned eye, UevrAerCropReason* reason = nullptr, bool validate_only = false) noexcept {
    const auto refuse = [&](UevrAerCropReason why)->std::shared_ptr<const DlssMotionVectorSet> {
        if(reason)*reason=why; return {};
    };
    if(reason)*reason=UevrAerCropReason::ready;
    try {
        if(!input || !input->eyes[0])return refuse(UevrAerCropReason::missing_guide);
        // The release resolver can retain a second stream for eye-order
        // comparison after assigning the primary to this logical eye. Keep
        // that companion intact; it is not a second primary guide.
        if(input->eye_count!=1 || input->array_guides_in_eye_order || eye>1 ||
            (input->resolved_eye>=0 && unsigned(input->resolved_eye)!=eye) ||
            (input->alternate && input->resolved_eye!=int(eye)))
            return refuse(UevrAerCropReason::ambiguous_eyes);
        if(!image_width || !image_height || view.x<0 || view.y<0 || !view.width || !view.height ||
            std::uint64_t(view.x)+view.width>image_width || std::uint64_t(view.y)+view.height>image_height)
            return refuse(UevrAerCropReason::invalid_rectangle);
        const auto& frame=*input->eyes[0];
        if(!frame.motion_width || !frame.motion_height ||
            std::uint64_t(frame.motion_x)+frame.motion_width>UINT32_MAX ||
            std::uint64_t(frame.motion_y)+frame.motion_height>UINT32_MAX ||
            std::uint64_t(frame.depth_x)+frame.depth_width>UINT32_MAX ||
            std::uint64_t(frame.depth_y)+frame.depth_height>UINT32_MAX)
            return refuse(UevrAerCropReason::invalid_grid);
        const double sx=double(frame.motion_width)/image_width, sy=double(frame.motion_height)/image_height;
        const std::array<float,4> crop{float(frame.motion_x+view.x*sx),float(frame.motion_y+view.y*sy),
            float(view.width*sx),float(view.height*sy)};
        if(!(crop[2]>.5F) || !(crop[3]>.5F))return refuse(UevrAerCropReason::invalid_grid);
        if(frame.placed) {
            if(frame.output_x!=unsigned(view.x) || frame.output_y!=unsigned(view.y) ||
                frame.output_width!=view.width || frame.output_height!=view.height)
                return refuse(UevrAerCropReason::incompatible_placement);
            for(unsigned i=0;i<4;++i)if(!(std::abs(frame.placed_motion_rect[i]-crop[i])<=.001F))
                return refuse(UevrAerCropReason::incompatible_placement);
            return input; // The held eye keeps the same placement; never crop it twice.
        }
        if(frame.output_x || frame.output_y || frame.output_width!=image_width || frame.output_height!=image_height ||
            frame.alignment_cropped)
            return refuse(UevrAerCropReason::unknown_full_image);
        if(validate_only)return input;
        if(!view.x && !view.y && view.width==image_width && view.height==image_height)return input;
        auto placed=std::make_shared<DlssMotionVectorFrame>(frame);
        placed->output_x=unsigned(view.x);placed->output_y=unsigned(view.y);
        placed->output_width=view.width;placed->output_height=view.height;
        placed->placed=true;placed->placed_motion_rect=crop;
        auto output=std::make_shared<DlssMotionVectorSet>(*input);
        output->eyes[0]=std::move(placed);
        return output;
    } catch(...) { return refuse(UevrAerCropReason::missing_guide); }
}

inline std::shared_ptr<const DlssMotionVectorSet> bind_uevr_aer_guides(
    const std::shared_ptr<const DlssMotionVectorSet>& input,
    unsigned image_width, unsigned image_height, const DlssEyeView& view,
    unsigned eye, UevrAerCropReason* reason = nullptr) noexcept {
    if(!place_uevr_aer_guides(input,image_width,image_height,view,eye,reason,true))return {};
    if(input->uevr_aer_image_extent==std::array<std::uint32_t,2>{image_width,image_height})return input;
    try {
        auto bound=std::make_shared<DlssMotionVectorSet>(*input);
        bound->uevr_aer_image_extent={image_width,image_height};
        return bound;
    } catch(...) { if(reason)*reason=UevrAerCropReason::missing_guide;return {}; }
}
} // namespace xrfg
