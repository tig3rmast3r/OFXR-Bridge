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

static int independent_async_creation_test(bool cancel,bool refuse=false) {
    independent_fixture_mode=true;independent_fixture_binding_queue_handoff=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        auto dispatch=std::make_shared<Dispatch>();dispatch->runtime_name="Pimax OpenXR";
        dispatch->create_swapchain=private_runtime_create;dispatch->destroy_swapchain=private_runtime_destroy;
        dispatch->enumerate_swapchain_images=private_runtime_enumerate;
        dispatch->acquire_swapchain_image=private_runtime_acquire;dispatch->wait_swapchain_image=private_runtime_wait_image;
        dispatch->release_swapchain_image=private_runtime_release;
        auto session=std::make_shared<SessionState>(dispatch);session->handle=reinterpret_cast<XrSession>(2);
        session->independent_pacing=session->menu_enabled=true;
        session->vdxr_legacy=false;
        session->graphics_binding=SessionGraphicsBinding::d3d12;
        session->d3d12_device=f.device12;session->d3d12_queue=f.queue;
        session->private_runtime=f.transport;session->private_delivery_queue=f.delivery_queue;
        session->nvidia_options.rife_model=-1;session->nvidia_new_route=true;
        session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        session->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(f.device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&session->d3d12_synthesis_queue)),"startup synthesis queue");
        const auto app=f.make_chain();f.write(app,80);f.gpu_wait();
        if(refuse)f.native_chain_limit=f.chains.size();
        auto chain=std::make_shared<SwapchainState>(session,optional_surface_info());chain->handle=app;
        chain->enumerated_image_count=3;chain->generation_eligible_pending=true;chain->pooled_publication=true;
        std::array<XrSwapchainImageD3D12KHR,3> images{};for(auto& image:images)image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        unsigned count{};xr_ok(f.transport->enumerate_images(app,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"startup source images");
        std::array<ID3D12Resource*,3> textures{};for(unsigned i=0;i<3;++i)textures[i]=images[i].texture;
        chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ok(chain->d3d12_history->initialize(f.device12.Get(),f.queue.Get(),textures,D3D12_RESOURCE_STATE_RENDER_TARGET),"startup history");
        {std::scoped_lock lock(g_state_mutex);g_sessions[session->handle]=session;g_swapchains[app]=chain;}
        std::promise<void> entered,release;auto inside=entered.get_future();auto gate=release.get_future().share();
        std::atomic_bool first{true};
        f.create_callback=[&]{if(first.exchange(false)){entered.set_value();gate.wait();}};
        ProjectionResourceMapping mapping{};mapping.application_swapchain=app;
        const auto began=ns();
        auto arming=std::async(std::launch::async,[&]{return ensure_projection_frame_generation(session,std::span(&mapping,1));});
        check(inside.wait_for(2s)==std::future_status::ready,"generator entered gated native allocation");
        const bool early=arming.wait_for(100ms)==std::future_status::ready;
        if(!early)release.set_value(); // negative control must also unblock its real allocation
        check(arming.get(),"start unpublished generator");const auto returned=ns();
        check(early,"startup construction synchronously blocked arming");
        std::unique_lock call_lock(chain->call_mutex,std::try_to_lock);
        std::unique_lock gpu_lock(session->gpu_mutex,std::try_to_lock);
        const bool locks_free=call_lock.owns_lock() && gpu_lock.owns_lock();
        if(call_lock.owns_lock())call_lock.unlock();if(gpu_lock.owns_lock())gpu_lock.unlock();
        for(unsigned n=1;n<=3;++n) {
            f.begin_virtual();xr_ok(f.submit(app,float(n)),"fresh real frame while generator construction is gated");
            xr_ok(f.transport->drain(),"native real frame continues during startup");
            check(ensure_projection_frame_generation(session,std::span(&mapping,1)),"poll unfinished construction without waiting");
        }
        check(generation_creation_pending(session) && !chain->frame_generation,"unfinished generation escaped publication");
        {std::scoped_lock lock(chain->mutex);chain->last_projection_use=std::chrono::steady_clock::now()-std::chrono::seconds(3);}
        ProjectionResourceMapping absent{};
        check(ensure_projection_frame_generation(session,std::span(&absent,1)) && chain->projection_history_needed,
            "projection retirement discarded a history still owned by its unfinished generator");
        std::future<void> teardown;
        if(cancel) {
            teardown=std::async(std::launch::async,[&]{destroy_frame_generation_swapchains(chain);});
            check(teardown.wait_for(20ms)==std::future_status::timeout,"teardown released an unfinished generator owner");
            f.transport->stop();
        }
        release.set_value();
        if(cancel)teardown.get();else finish_session_generation_creation(session);
        f.create_callback={};
        check(locks_free && returned-began<100'000'000,"startup construction held the application/resource mutex or synchronously blocked arming");
        check(f.pixels==std::vector<unsigned>({80,80,80}) && f.positions==std::vector<float>({1,2,3}),
            "startup construction stalled or changed the live scene pixels/poses");
        if(refuse) {
            check(!chain->frame_generation && chain->generation_declined && budget_refusal(chain->generation_refusal),
                "asynchronous native refusal lost its original budget classification");
            check(projection_frame_generation_pending(session,std::span(&mapping,1)),"completed async refusal escaped budget retirement");
            check(!ensure_projection_frame_generation(session,std::span(&mapping,1)) && session->generation_budget_exhausted,
                "async refusal did not retire the session output budget");
            release_session_generation_budget(session);
            check(!projection_frame_generation_pending(session,std::span(&mapping,1)) && f.chains.size()==1,
                "refused startup retried allocations or lost the application's real image");
        } else if(cancel) {
            check(!chain->frame_generation && !generation_creation_pending(chain) && f.chains.size()==1,
                "canceled construction leaked private/native generation ownership");
        } else {
            check(chain->frame_generation && !chain->generation_eligible_pending,"finished generator was not atomically adopted");
            const auto native_count=f.chains.size();
            const auto private_count=f.transport->native_swapchain_mapping()->size();
            f.transport->stop();destroy_frame_generation_swapchains(chain);
            check(f.chains.size()==1,"async generation retirement leaked native pools");
            // Compare actual allocations against the original synchronous
            // factory, after retiring the first owner so peak GPU storage does
            // not increase during this oracle either.
            SwapchainEligibilityReason reason{};std::uint64_t detail{};
            chain->frame_generation=create_d3d12_frame_generation_swapchains(chain,&reason,&detail);
            check(chain->frame_generation && f.chains.size()==native_count &&
                f.transport->native_swapchain_mapping()->size()==private_count,
                "asynchronous startup changed native/private image allocation counts");
            destroy_frame_generation_swapchains(chain);
        }
        f.transport->stop();xr_ok(f.transport->destroy_swapchain(app),"startup source cleanup");
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(app);g_sessions.erase(session->handle);}
    }
    independent_fixture_mode=false;independent_fixture_binding_queue_handoff=false;
    std::printf("ASYNC_GENERATION cancel=%d refuse=%d live native pixels/poses, free frame locks, exact pool counts and teardown PASS\n",cancel,refuse);
    return 0;
}
