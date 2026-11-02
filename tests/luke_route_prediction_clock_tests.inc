void luke_route_prediction_clock_test() {
    constexpr XrDuration physical = 11'111'111;
    auto s = session();s->luke_afw_enabled = true;s->menu_enabled = true;
    s->dispatch->steamvr_runtime = true;s->presenter_display_period = physical;
    s->vdxr_legacy=false; // session creation resolves the physical runtime into its transport policy
    const auto initial_waits = waits.load();
    {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
    struct Registration {XrSession handle;~Registration(){std::scoped_lock lock(g_state_mutex);g_sessions.erase(handle);}} registration{s->handle};
    XrTime previous = 0;unsigned index = 0;
    // Replay the observed persistent state transition, without replacing the
    // application session or inventing new capture/AFW contexts.
    for (const int route : {3,1,0,3,-1,3}) {
        s->menu_enabled = route >= 0;s->nvidia_new_route = route == 3;
        s->djules_route = route == 3;s->three_slot_director = route == 3;
        s->optical_flow_backend = route == 0 ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
        update_luke_application_clock(*s,false,false);
        for (const int multiple : {1,2,1,4,3,1}) {
            s->application_wait_pending_begin = false;
            s->presenter_frame_state.predictedDisplayTime = 1'000'000'000 + (++index)*8*physical;
            s->presenter_frame_state.predictedDisplayPeriod = physical*multiple;
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&prediction)),"Luke route prediction Wait");
            const auto expected = route == 3 || route < 0 ? physical : physical*2;
            check(prediction.predictedDisplayPeriod == expected,
                "Luke route switch advertised the throttled runtime period");
            check(prediction.predictedDisplayTime > previous,"Luke route switch rewound application prediction");
            check(s->presenter_frame_state.predictedDisplayPeriod == physical*multiple,
                "Luke clock rewrote raw runtime metadata");
            previous=prediction.predictedDisplayTime;
            if(route==3) update_luke_application_clock(*s,true,false);
        }
    }
    // Duplicate admission uses the same physical period, not an inflated
    // runtime sample. Fresh input remains independent of director progress.
    s->presenter_frame_state.predictedDisplayPeriod=physical*4;
    s->application_wait_pending_begin=false;
    s->application_wait_returned_at=std::chrono::steady_clock::now();
    update_luke_application_clock(*s,true,true);
    const auto start=std::chrono::steady_clock::now();
    XrFrameState duplicate{XR_TYPE_FRAME_STATE};
    check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&duplicate)),"Luke duplicate clock Wait");
    check(duplicate.predictedDisplayPeriod==physical &&
        std::chrono::steady_clock::now()-start<std::chrono::milliseconds(35),
        "Luke duplicate used the throttled period as an extra hold");
    check(waits.load()==initial_waits,"Luke prediction introduced runtime polling");
    s->presenter_display_period=physical*2;
    check(adopt_luke_display_period(*s,std::chrono::nanoseconds(physical)) &&
        s->presenter_display_period==physical && s->history_capture_wait_period==physical,
        "restarted Luke presenter retained a throttled period");
    check(!adopt_luke_display_period(*s,std::chrono::nanoseconds(physical-11)) &&
        s->presenter_display_period==physical,"OpenXR clock rounding churned the HMD adoption");
    check(!adopt_luke_display_period(*s,std::nullopt) &&
        !adopt_luke_display_period(*s,std::chrono::nanoseconds(0)) &&
        !adopt_luke_display_period(*s,std::chrono::milliseconds(60)),"invalid HMD measurement adopted");
    s->dispatch->steamvr_runtime=false;s->vdxr_legacy=true;
    check(!adopt_luke_display_period(*s,std::chrono::milliseconds(8)),"VDXR acquired Luke SteamVR clock adoption");
    s->dispatch->steamvr_runtime=true;s->vdxr_legacy=false;s->luke_afw_enabled=false;
    check(!adopt_luke_display_period(*s,std::chrono::milliseconds(8)),"ordinary D11 acquired Luke clock adoption");
    s->luke_afw_enabled=true;s->graphics_binding=SessionGraphicsBinding::d3d12;
    check(!adopt_luke_display_period(*s,std::chrono::milliseconds(8)),"native or Vulkan bridge acquired D11 Luke clock adoption");
    std::cout<<"Luke actual Wait replay: Djules/Reference/Fidelity/Djules/Off/Djules; fixed cadence, elapsed clock and duplicate deadline PASS\n";
}
