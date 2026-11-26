static int independent_stereo_recovery_test() {
    const auto make_state=[] {
        auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
        state->independent_pacing=true;state->menu_enabled=true;
        state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        state->nvidia_options.rife_model=-1;state->nvidia_options.rife_multiplier=2;
        state->djules_route=false;
        return state;
    };
    for(const char* runtime:{"Pimax OpenXR","SteamVR/OpenXR","VirtualDesktopXR","Other OpenXR"})
    for(unsigned factor:{2U,3U,4U})for(bool hybrid:{false,true})for(auto backend:{xrfg::D3D12OpticalFlowBackend::fidelity_fx,xrfg::D3D12OpticalFlowBackend::nvidia}) {
        auto state=make_state();state->nvidia_options.rife_multiplier=int(factor);state->dispatch->runtime_name=runtime;
        state->dispatch->steamvr_runtime=std::string_view(runtime)=="SteamVR/OpenXR";
        state->dispatch->virtual_desktop_runtime=std::string_view(runtime)=="VirtualDesktopXR";
        state->hybrid_requested=state->nvidia_options.hybrid=hybrid;state->optical_flow_backend=backend;
        check(independent_stereo_recovery(*state),"runtime-independent optical/Hybrid2X/3X/4X recovery did not qualify");
        SynthesisJob job;job.stereo_recovery=independent_stereo_recovery(*state);
        job.independent_first_due_ns=1'000'000'000;job.independent_period_ns=11'111'128;
        for(unsigned index=0;index<factor;++index) {
            GeneratedFrameEndInfo frame;assign_independent_output_timing(job,frame,index);
            check(frame.independent_due_ns==job.independent_first_due_ns+index*job.independent_period_ns,
                "recovery accounting changed actual native slot");
            check(frame.independent_recovery.accelerated && frame.independent_recovery.protect_next_output &&
                frame.independent_recovery.scheduled_offset==index*job.independent_period_ns,
                "streamed first output/unpaired prime/later output accounting lost");
        }
        job.stereo_recovery=false;
        GeneratedFrameEndInfo cleared;assign_independent_output_timing(job,cleared,1);
        check(!cleared.independent_recovery.accelerated && !cleared.independent_recovery.scheduled_offset &&
            !cleared.independent_recovery.protect_next_output,
            "default batch inherited recovery metadata");
    }
    {auto s=make_state();s->independent_pacing=false;check(!independent_stereo_recovery(*s),"Framework policy changed");}
    {auto s=make_state();s->menu_enabled=false;check(!independent_stereo_recovery(*s),"disabled FG policy changed");}
    {auto s=make_state();s->luke_afw_enabled=true;check(!independent_stereo_recovery(*s),"Luke/OSR pair policy changed");}
    {auto s=make_state();s->nvidia_options.ofr=true;check(!independent_stereo_recovery(*s),"AER policy changed");}
    {auto s=make_state();s->nvidia_options.rife_model=0;check(!independent_stereo_recovery(*s),"RIFE policy changed");}
    {auto s=make_state();s->djules_route=true;check(!independent_stereo_recovery(*s),"legacy engine policy changed");}
    {auto s=make_state();s->graphics_binding=SessionGraphicsBinding::d3d11;check(!independent_stereo_recovery(*s),"D11 policy changed");}
    {auto s=make_state();s->d3d11_bridge=std::make_unique<SessionState::D3D11Bridge>();check(!independent_stereo_recovery(*s),"D11 interop policy changed");}
    {auto s=make_state();s->vulkan_bridge=std::make_unique<SessionState::VulkanBridge>();check(!independent_stereo_recovery(*s),"Vulkan interop policy changed");}
    for(auto mode:{xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
        auto s=make_state();s->frame_generation_mode=mode;check(!independent_stereo_recovery(*s),"FSR/DLSS-G policy changed");
    }
    std::puts("Independent native D12 stereo recovery: Pimax/SteamVR/VDXR/other runtimes, optical/Hybrid NVIDIA/FidelityFX2X/3X/4X; frozen batch metadata; protected route controls PASS");
    return 0;
}
