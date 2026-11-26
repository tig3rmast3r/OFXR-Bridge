static int independent_midpoint_test() {
    constexpr XrDuration physical=11'111'128;
    ProjectionSnapshot previous{},current_frame{};
    previous.display_time=1'000'000'000;
    const auto make_state=[] {
        auto dispatch=std::make_shared<Dispatch>();
        dispatch->runtime_name="Pimax OpenXR";
        auto state=std::make_shared<SessionState>(dispatch);
        state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->independent_pacing=true;state->menu_enabled=true;state->djules_route=false;
        state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        state->nvidia_new_route=true;state->nvidia_options.rife_model=-1;
        state->nvidia_options.rife_multiplier=2;
        state->minimum_runtime_display_period=physical;
        return state;
    };
    // Actual consecutive Pimax gaps from PID52924, plus the renderer's
    // near-zero-fraction boundary and invalid-cadence/prime cases.
    for(const char* runtime:{"Pimax OpenXR","SteamVR/OpenXR","VirtualDesktopXR","Somnium OpenXR","Other OpenXR"})
    for(bool hybrid:{false,true})for(auto backend:{xrfg::D3D12OpticalFlowBackend::fidelity_fx,
            xrfg::D3D12OpticalFlowBackend::nvidia}) {
        auto state=make_state();state->hybrid_requested=state->nvidia_options.hybrid=hybrid;
        state->dispatch->runtime_name=runtime;
        state->dispatch->steamvr_runtime=std::string_view(runtime)=="SteamVR/OpenXR";
        state->dispatch->virtual_desktop_runtime=std::string_view(runtime)=="VirtualDesktopXR";
        state->optical_flow_backend=backend;
        for(XrDuration gap:{27'312'000LL,17'572'700LL,29'393'500LL,11'200'000LL,
                22'222'256LL,66'666'768LL,12'666'000LL,31'274'000LL,11'152'000LL,0LL,-1LL}) {
            current_frame.display_time=previous.display_time+gap;
            const auto fraction=synthetic_interpolation_fraction(state,previous,current_frame);
            if(fraction!=.5F)std::fprintf(stderr,"MIDPOINT runtime=%s gap=%lld fraction=%.9f\n",runtime,gap,fraction);
            check(fraction==.5F,"Independent optical 2X must retain a fixed midpoint across runtimes and prediction jitter");
        }
        check(synthetic_interpolation_fraction(state,{},current_frame)==.5F,"Independent prime has fixed midpoint");
        // The same fraction must place the reconstructed camera between the
        // two source eye poses, rather than matching a jittered prediction gap.
        auto from=previous,to=current_frame;
        from.layers.resize(1);to.layers.resize(1);
        from.layers[0].views.resize(2);to.layers[0].views.resize(2);
        for(unsigned eye=0;eye<2;++eye) {
            from.layers[0].views[eye].pose={{0,0,0,1},{float(eye)*.064F,0,0}};
            to.layers[0].views[eye].pose={{0,std::sin(.04F),0,std::cos(.04F)},{float(eye)*.064F+.08F,0,0}};
        }
        const auto fraction=synthetic_interpolation_fraction(state,from,to);
        const auto camera=synthetic_camera_snapshot(from,to,fraction);
        check(camera.has_value(),"midpoint camera interpolation rejected valid eye poses");
        for(unsigned eye=0;eye<2;++eye) {
            const auto& pose=camera->layers[0].views[eye].pose;
            check(std::abs(pose.position.x-(float(eye)*.064F+.04F))<1e-6F &&
                std::abs(pose.orientation.y-std::sin(.02F))<1e-5F,
                "synthetic image and eye camera lost the shared temporal midpoint");
        }
    }
    current_frame.display_time=previous.display_time+27'312'000;
    const float adaptive=1.F-float(physical)/27'312'000.F;
    const auto unchanged=[&](const auto& state,const char* message) {
        check(std::abs(synthetic_interpolation_fraction(state,previous,current_frame)-adaptive)<1e-6F,message);
    };
    for(const char* runtime:{"Pimax OpenXR","SteamVR/OpenXR","VirtualDesktopXR","Somnium OpenXR","Other OpenXR"}) {
        auto s=make_state();s->dispatch->runtime_name=runtime;s->independent_pacing=false;
        unchanged(s,"Framework temporal policy changed");
    }
    {auto s=make_state();s->independent_pacing=false;unchanged(s,"legacy Pimax temporal policy changed");}
    {auto s=make_state();s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;unchanged(s,"FSR temporal policy changed");}
    {auto s=make_state();s->nvidia_options.rife_model=0;unchanged(s,"RIFE temporal policy changed");}
    {auto s=make_state();s->luke_afw_enabled=true;unchanged(s,"Luke temporal policy changed");}
    {auto s=make_state();s->nvidia_options.ofr=true;unchanged(s,"AER temporal policy changed");}
    {auto s=make_state();s->djules_route=true;unchanged(s,"legacy engine temporal policy changed");}
    {auto s=make_state();s->d3d11_bridge=std::make_unique<SessionState::D3D11Bridge>();unchanged(s,"D11 bridge policy changed");}
    {auto s=make_state();s->vulkan_bridge=std::make_unique<SessionState::VulkanBridge>();unchanged(s,"Vulkan bridge policy changed");}
    {auto s=make_state();s->graphics_binding=SessionGraphicsBinding::d3d11;unchanged(s,"native D11 policy changed");}
    {auto s=make_state();s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::native_dlss;
     check(synthetic_interpolation_fraction(s,previous,current_frame)==.5F,"DLSS-G fixed policy changed");}
    for(int factor:{3,4}) {
        auto s=make_state();s->nvidia_options.rife_multiplier=factor;
        check(native_frame_multiplier(*s)==unsigned(factor),"multiframe policy fixture lost its multiplier");
        for(unsigned i=0;i<unsigned(factor-1);++i)
            check(std::abs(synthetic_interpolation_fraction(s,previous,current_frame,i)-float(i+1)/factor)<1e-6F,
                "existing multiframe fractions changed");
    }
    std::puts("INDEPENDENT_MIDPOINT Pimax/SteamVR/VDXR/Somnium/other, recorded prediction jitter, image/eye camera alignment, FFX/NVIDIA Hybrid/optical; Framework and protected routes preserved PASS");
    return 0;
}
