#include "xrfg/luke_origin_capture_policy.hpp"

int aer_native_guide_cache_test() {
    xrfg::embedded::Settings generation;
    xrfg::embedded::AfwSettings reconstruction;
    reconstruction.mode=0;generation.enabled=false;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"all-off producer records no protected colour markers");
    generation.enabled=true;generation.ofr=true;
    for(int method:{0,1,2})for(bool hybrid:{false,true}) {
        generation.frame_generation=method;generation.hybrid=hybrid;
        check(xrfg::luke_origin_capture_required(generation,reconstruction)==(method==1 || method==2 || hybrid),
            "native AER methods produce CRC origins with AFW explicitly off");
    }
    reconstruction.mode=3;generation.frame_generation=0;generation.hybrid=false;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"raw OFXR AER does not mark because of bypassed AFW preference");
    generation.rife_model=3;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"RIFE AER does not mark because of bypassed AFW preference");
    generation.enabled=false;
    check(xrfg::luke_origin_capture_required(generation,reconstruction),"turning AER generation off restores requested reconstruction origins");
    reconstruction.mode=0;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"saved AER preference does not mark while every effect is off");
    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    auto source=std::make_shared<SwapchainState>(session,info);
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    xrfg::D3D12HistoryCaptureTicket capture{11,23,1,2};
    AerNativeGuideCache::Eye eye;
    check(eye.observe(source,history,capture),"new release owns a new marker association");
    eye.attempted=true;eye.token=73;eye.guides=std::make_shared<xrfg::DlssMotionVectorSet>();
    const auto retained=eye.guides;
    check(!eye.observe(source,history,capture) && eye.attempted && eye.token==73 && eye.guides==retained,
        "held release keeps its exact token/owned guide without another marker read");
    auto fresh=capture;++fresh.serial;++fresh.fence_value;
    check(eye.observe(source,history,fresh) && !eye.attempted && !eye.token && !eye.guides,
        "fresh capture invalidates old colour/guide association");
    eye.token=74;eye.attempted=true;
    auto replaced_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(eye.observe(source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "history-resource epoch change invalidates token even with identical serial");
    eye.attempted=true;eye.token=75;
    auto replaced_source=std::make_shared<SwapchainState>(session,info);
    check(eye.observe(replaced_source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "replaced source domain cannot inherit a token association");
    eye.attempted=true;eye.token=76;++fresh.fence_value;
    check(eye.observe(replaced_source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "changed producer receipt invalidates cached colour identity");

    xrfg::DlssMotionVectorFrame current;
    current.stream=4;current.epoch=3;current.serial=21;current.previous_serial=19;
    check(!aer_native_same_eye_history(current),"missing same-eye history cannot be guessed from serial parity");
    auto previous=std::make_shared<xrfg::DlssMotionVectorFrame>();
    previous->stream=4;previous->epoch=3;previous->serial=19;current.previous_same_guide=previous;
    check(!aer_native_same_eye_history(current),"same-eye key alone cannot prove raw vector span");
    current.raw_motion_native_span_confirmed=true;current.raw_motion_native_span=1;
    check(!aer_native_same_eye_history(current),"adjacent-eye N1 vectors cannot enter raw same-eye native FG");
    current.raw_motion_native_span=2;
    check(aer_native_same_eye_history(current),"exact producer-owned same-eye history retained");
    previous->stream=5;
    check(!aer_native_same_eye_history(current),"opposite/other-stream history cannot authorize native AER");
    previous->stream=4;previous->epoch=4;
    check(!aer_native_same_eye_history(current),"old/new epoch history cannot authorize native AER");
    previous->epoch=3;previous->serial=18;
    check(!aer_native_same_eye_history(current),"missing same-eye generation cannot be bridged by a guessed older receipt");

    session->nvidia_options.ofr=true;session->luke_afw_enabled=true;session->menu_enabled=true;
    session->nvidia_options.rife_model=-1;session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
    check(aer_native_guides_requested(*session),"native AER consumer requests exact guide ingress");
    ProjectionSnapshot snapshot;
    check(refresh_aer_native_guides(session,snapshot,{})==S_FALSE && !session->aer_native_guide_cache,
        "unsupported projection refuses before marker allocation or source scanning");
    session->nvidia_options.rife_model=3;
    check(!aer_native_guides_requested(*session) && refresh_aer_native_guides(session,snapshot,{})==S_OK &&
        !session->aer_native_guide_cache,"RIFE never activates native marker/guide ingress");
    session->nvidia_options.rife_model=-1;session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
    check(!aer_native_guides_requested(*session),"plain optical flow never activates native marker ingress");
    session->hybrid_requested=true;check(aer_native_guides_requested(*session),"Hybrid requests native guides even before its first publication");
    session->menu_enabled=false;
    check(!aer_native_guides_requested(*session),"all-off path performs no native AER marker work");
    std::puts("AER native source cache, held reuse, exact same-eye history and route gating PASS");
    return 0;
}
