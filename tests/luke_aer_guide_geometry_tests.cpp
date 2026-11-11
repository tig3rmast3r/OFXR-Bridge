#include "xrfg/luke_aer_guide_geometry.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
int main() {
    try {
        using namespace xrfg;
        for(bool enabled:{false,true})for(bool saved_aer:{false,true}) {
            const bool aer=enabled && saved_aer;
            require(luke_guide_capture_requested(true,aer,D3D12FrameGenerationMode::ofxr,false,false)==!aer,
                "global framegen off must restore OSR guides despite saved AER");
            require(luke_guide_capture_requested(true,aer,D3D12FrameGenerationMode::ofxr,true,false),
                "OSR or Hybrid must retain guide capture across global enable transitions");
        }
        for(auto mode:{D3D12FrameGenerationMode::ofxr,D3D12FrameGenerationMode::fsr3,D3D12FrameGenerationMode::native_dlss}) {
            require(luke_primary_generation_mode(true,true,mode)==mode,"Luke AER must create/reconfigure the selected provider");
            require(luke_primary_generation_mode(true,false,mode)==D3D12FrameGenerationMode::ofxr,"ordinary Luke AFW stays OFXR");
            require(luke_primary_generation_mode(false,false,mode)==mode,"native non-Luke provider remains selected");
        }
        auto input=std::make_shared<DlssMotionVectorSet>();input->eye_count=1;input->resolved_eye=1;
        auto frame=std::make_shared<DlssMotionVectorFrame>();input->eyes[0]=frame;
        frame->stream=7;frame->epoch=3;frame->serial=41;frame->previous_serial=39;frame->publication=93;
        frame->output_width=3856;frame->output_height=3088;
        frame->motion_width=frame->depth_width=1928;frame->motion_height=frame->depth_height=1544;
        frame->camera_matrices_valid=true;
        // Actual failing GTA F8 projection: native3856 versus centered XR3370.
        frame->camera_view_to_clip={.893913209F,0,0,0,0,1.11623359F,0,0,0,0,.00000500002488F,-1,0,0,.0500002503F,0};
        const float native_x=1/.893913209F,native_y=1/1.11623359F;
        DlssEyeView view{-native_x*3370/3856,native_x*3370/3856,native_y,-native_y,0,0,3370,3088};
        auto placed=place_luke_aer_guides(input,{&view,1});
        require(placed && placed!=input && placed->resolved_eye==1,"exact physical eye must survive placement");
        const auto& g=*placed->eyes[0];const auto mr=guide_motion_rect(g),dr=guide_depth_rect(g);
        require(g.stream==7 && g.epoch==3 && g.serial==41 && g.previous_serial==39 && g.publication==93,"placement cannot select a different temporal origin");
        require(g.output_width==3370 && g.output_height==3088 && g.motion_width==1928,"crop metadata keeps complete native texture dimensions");
        require(std::abs(mr[0]-121.5F)<.002F && std::abs(mr[2]-1685)<.002F,"GTA native guides need243 output pixels of left crop");
        require(dr==mr && std::abs(g.scale_x*g.output_width/mr[2]-2)<.001F,"depth and vector magnitudes must share cropped colour mapping");
        // A distinctive edge at native motion x1000 belongs to XR x1757,
        // not x2000. This is the owner's large right-shifted silhouette.
        const float edge=(1000-mr[0])*g.output_width/mr[2];
        require(std::abs(edge-1757)<.01F,"native edge remains shifted right of XR colour");
        require(!fsr_guide_regions(g),"generic FSR fractional crop refusal stays strict");
        auto fsr=fsr_luke_aer_guide_regions(g);
        require(fsr && fsr->motion[0]==122 && fsr->motion[2]==1685 && fsr->depth==fsr->motion,"Luke FSR uses bounded whole-texel crop rather than rejecting every half-texel origin");
        require(!frame->placed && frame->output_width==3856,"placement cannot mutate the full grids used by the N1 composer");
        require(place_luke_aer_guides(placed,{&view,1})==placed,"reusing a placed source cannot crop it twice");
        DlssEyeView equal=view;equal.width=3856;
        require(place_luke_aer_guides(input,{&equal,1})==input,"matching extents preserve existing mapping");
        auto unknown=std::make_shared<DlssMotionVectorFrame>(*frame);unknown->camera_matrices_valid=false;
        auto invalid=std::make_shared<DlssMotionVectorSet>(*input);invalid->eyes[0]=unknown;
        require(!place_luke_aer_guides(invalid,{&view,1}),"unknown crop geometry cannot be guessed from sizes");
        std::cout<<"Luke AER selected-provider routing and captured GTA guide crop PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
