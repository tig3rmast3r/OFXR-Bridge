static void independent_native_binding_handoff(unsigned runtime,bool dedicated=true) {
    independent_fixture_mode=true;
    {
        Fixture f(true);f.throttled=false;f.transport->stop();
        if(!dedicated)f.delivery_queue.Reset();
        auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
        dispatch->steamvr_runtime=runtime==0;dispatch->virtual_desktop_runtime=runtime==2;
        dispatch->runtime_name=runtime==0?"SteamVR/OpenXR":runtime==1?"Pimax OpenXR":runtime==2?"VirtualDesktopXR":"Other OpenXR";
        dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
        dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;
        dispatch->enumerate_swapchain_images=Fixture::enumerate;dispatch->acquire_swapchain_image=Fixture::acquire;
        dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;
        dispatch->convert_time_to_qpc=Fixture::to_qpc;
        auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
        state->independent_pacing=state->menu_enabled=true;state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->vdxr_legacy=false;
        state->d3d12_device=f.device12;state->d3d12_queue=f.queue;state->private_delivery_queue=f.delivery_queue;
        {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;}
        check(install_private_runtime(state)==S_OK && state->private_runtime,"install actual native-runtime delivery policy");
        f.transport=state->private_runtime;xr_ok(f.transport->start(),"start installed native delivery policy");
        const auto app=f.make_chain(false,1);f.write(app,80);f.gpu_wait();
        ok((f.delivery_queue?f.delivery_queue.Get():f.queue.Get())->Wait(f.gate.Get(),1),"gate exact native binding queue");
        f.begin_virtual();xr_ok(f.submit(app,9),"submit to installed native handoff policy");
        const auto deadline=std::chrono::steady_clock::now()+100ms;
        while(!f.end_entered && std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(1ms);
        const bool reached_native=f.end_entered.load();
        unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        const bool read_pinned=f.transport->try_acquire_image(app,&acquire,&index)==XR_TIMEOUT_EXPIRED;
        ok(f.gate->Signal(1),"release native binding queue gate");f.unblock();
        xr_ok(f.transport->drain(),"installed native policy retires both copy and End ownership");
        check(reached_native==dedicated && read_pinned,
            "runtime-name gate delayed native handoff, or unproven queue/source ownership was bypassed");
        check(f.pixels==std::vector<unsigned>{80} && f.positions==std::vector<float>{9},"queue handoff changed native pixels/pose");
        xr_ok(f.transport->destroy_swapchain(app),"installed handoff source cleanup");f.transport->stop();
        {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
    }
    independent_fixture_mode=false;
    std::printf("NATIVE_BINDING runtime=%u dedicated=%d ordered pixels/poses and retained GPU readers PASS\n",runtime,dedicated);
}
