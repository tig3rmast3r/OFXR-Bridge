void native_prediction_clock_test() {
    constexpr XrDuration physical = 11'111'111;
    const auto initial_waits = waits.load();
    for (unsigned route = 0; route < 9; ++route) {
        auto s = session();
        s->graphics_binding = SessionGraphicsBinding::d3d12;
        s->dispatch->steamvr_runtime = true;
        s->vdxr_legacy = false; // explicit comparison transport, as real SteamVR session setup selects
        s->menu_enabled = true;
        s->presenter_display_period = physical;
        s->three_slot_director = route == 2;
        s->queued_nvidia_presenter = route < 2 || route >= 5;
        s->nvidia_new_route = route != 0;
        s->djules_route = route == 1 || route == 6;
        s->fsr_two_slot_presenter = route == 3;
        if (route == 3) s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
        if (route == 4) s->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        if (route == 5) s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::native_dlss;
        if (route == 6) s->djules_triple_requested = true;
        if (route == 7) { s->hybrid_requested = true;s->nvidia_options.hybrid = true; }
        if (route == 8) {
            s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::native_dlss;
            s->djules_triple_requested = true;s->native_dlss_generated_frames = 2;
        }
        { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
        struct Registration {
            XrSession handle;
            ~Registration() { std::scoped_lock lock(g_state_mutex); g_sessions.erase(handle); }
        } registration{s->handle};
        unsigned index = 0;
        XrTime previous = 0;
        // Real SteamVR trace: the 90 Hz display remains constant while the
        // runtime's prediction period reports one or several missed slots.
        for (const int multiple : {1, 2, 1, 3, 1, 4, 2, 1}) {
            s->application_wait_pending_begin = false;
            const XrTime slot = 1'000'000'000 + (++index) * 3 * physical;
            s->presenter_frame_state.predictedDisplayTime = slot;
            s->presenter_frame_state.predictedDisplayPeriod = multiple * physical;
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &prediction)), "native physical-clock wait");
            const XrDuration pair = (route == 6 || route == 8 ? 3 : 2) * physical;
            const XrDuration advertised = route == 2 || route == 3 ? physical : pair;
            check(prediction.predictedDisplayPeriod == advertised,
                "runtime throttling multiplied the native application's advertised period");
            check(prediction.predictedDisplayTime == slot + pair && prediction.predictedDisplayTime > previous,
                "runtime throttling moved the application's camera prediction horizon");
            check(s->presenter_frame_state.predictedDisplayPeriod == multiple * physical,
                "application clock rewrote the raw runtime prediction");
            previous = prediction.predictedDisplayTime;
        }
        // Disabled generation still represents one physical display slot.
        s->menu_enabled = false;s->last_virtual_display_time = 0;s->application_wait_pending_begin = false;
        s->presenter_frame_state.predictedDisplayPeriod = physical * 2;
        XrFrameState disabled{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &disabled)) &&
            disabled.predictedDisplayPeriod == physical, "disabled native virtual clock doubled runtime throttling");
        // VDXR, native D11 and bridge/Luke contracts remain their own clocks.
        s->menu_enabled = true;s->djules_triple_requested = false;
        s->three_slot_director = false;s->fsr_two_slot_presenter = false;
        for (unsigned excluded = 0; excluded < 4; ++excluded) {
            s->dispatch->steamvr_runtime = excluded != 0;
            s->vdxr_legacy = excluded == 0;
            s->graphics_binding = excluded == 1 ? SessionGraphicsBinding::d3d11 : SessionGraphicsBinding::d3d12;
            s->d3d11_bridge.reset();s->vulkan_bridge.reset();
            if (excluded == 2) s->d3d11_bridge = std::make_unique<SessionState::D3D11Bridge>();
            if (excluded == 3) s->vulkan_bridge = std::make_unique<SessionState::VulkanBridge>();
            s->last_virtual_display_time = 0;s->application_wait_pending_begin = false;
            XrFrameState old{XR_TYPE_FRAME_STATE};
            check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &old)) &&
                old.predictedDisplayPeriod == physical * 4, "excluded transport acquired native SteamVR clock policy");
        }
        s->d3d11_bridge.reset();s->vulkan_bridge.reset();s->dispatch->steamvr_runtime = true;s->vdxr_legacy=false;
        for (const XrDuration unavailable : {0LL, 1LL, 60'000'000LL}) {
            s->presenter_display_period = unavailable;s->last_virtual_display_time = 0;
            s->application_wait_pending_begin = false;
            XrFrameState fallback{XR_TYPE_FRAME_STATE};
            check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle, nullptr, &fallback)) &&
                fallback.predictedDisplayPeriod == physical * 4, "missing physical clock invented a display period");
        }
    }
    check(waits.load() == initial_waits, "virtual clock entered runtime or added a wait");
    std::cout << "Native application prediction: nine routes, physical versus throttled runtime periods, 3X/disabled, missing clocks and excluded transports PASS\n";
}
