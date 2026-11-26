static void background_pool_creation(unsigned transition) {
    independent_fixture_mode=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        const auto info=optional_surface_info();const auto source=reinterpret_cast<XrSwapchain>(0x1234);
        XrSwapchain owner{};
        xr_ok(f.transport->create_swapchain(&info,&owner,D3D12_RESOURCE_STATE_RENDER_TARGET,1,source),"seed generation pool");
        std::promise<void> entered,release;auto inside=entered.get_future();auto gate=release.get_future().share();
        f.diagnostic_work=[&](int stage,std::uint64_t,std::int64_t){if(stage==104){entered.set_value();gate.wait();}};
        XrSwapchain candidate{};
        auto creation=std::async(std::launch::async,[&] {
            return f.transport->create_background_swapchain(&info,&candidate,D3D12_RESOURCE_STATE_RENDER_TARGET,1,source);
        });
        check(inside.wait_for(2s)==std::future_status::ready,"background pool allocation gate");
        auto access=std::async(std::launch::async,[&] {
            xr_ok(f.transport->destroy_swapchain(owner),"retire last published pool owner during construction");
            if(transition){f.transport->stop();if(transition==2)xr_ok(f.transport->start(),"restart during unpublished generation");}
            else {XrFrameState prediction{XR_TYPE_FRAME_STATE};xr_ok(f.transport->predict_frame(&prediction),"native prediction during generator allocation");}
        });
        const bool progressed=access.wait_for(100ms)==std::future_status::ready;
        const auto native_count=progressed?f.chains.size():0;
        release.set_value();access.get();const auto result=creation.get();f.diagnostic_work={};
        check(progressed && native_count==1,"background generator blocked native progress or lost its publication pool pin");
        if(transition) {
            check(result==XR_ERROR_SESSION_NOT_RUNNING && !candidate && f.chains.empty(),
                "stop/start must cancel unpublished generation and retire its last pool pin");
        } else {
            xr_ok(result,"publish completed generation with retained pool");
            unsigned count{};xr_ok(f.transport->enumerate_images(candidate,0,&count,nullptr),"background generation image count");
            check(count==1 && f.chains.size()==1,"background construction changed generation image or pool counts");
            f.write(candidate,80);f.begin_virtual();xr_ok(f.submit(candidate,9),"constructed generation pixels and pose");
            xr_ok(f.transport->drain(),"background generation delivery");
            check(f.pixels==std::vector<unsigned>{80} && f.positions==std::vector<float>{9},"background publication changed pixels or pose");
            xr_ok(f.transport->destroy_swapchain(candidate),"background generation cleanup");
            check(f.chains.empty(),"background pool leaked its native endpoint");
        }
    }
    independent_fixture_mode=false;
    std::printf("BACKGROUND_POOL transition=%u native progress, pinned owner, cancellation, fixed image count PASS\n",transition);
}
