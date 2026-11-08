// Actual layer + private endpoint + meter admission. No headset or game.
void luke_private_startup_test(bool installed=false) {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    check(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"startup device");
    D3D12_COMMAND_QUEUE_DESC desc{};
    check(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue))),"startup queue");
    for(bool fresh:{false,true})for(bool meter:{false,true}) {
        auto state=session();state->presenter_active=false;state->three_slot_director=false;
        state->luke_afw_enabled=true;state->luke_fresh_only=fresh;
        state->menu_enabled=false;state->graphics_binding=SessionGraphicsBinding::d3d12;
        RegisteredFixtureSession registered(state);
        xrfg::PrivateOpenXrRuntime::Options options;
        options.device12=device.Get();options.queue12=queue.Get();
        options.real.session=state->handle;
        options.real.wait=fake_wait;options.real.begin=fake_begin;options.real.end=fake_end;
        // These callbacks must never run: empty startup frames own no images.
        options.real.create=+[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain*)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        options.real.destroy=+[](XrSwapchain)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        options.real.enumerate=+[](XrSwapchain,uint32_t,uint32_t*,XrSwapchainImageBaseHeader*)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        options.real.acquire=+[](XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t*)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        options.real.wait_image=+[](XrSwapchain,const XrSwapchainImageWaitInfo*)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        options.real.release=+[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult{return XR_ERROR_FUNCTION_UNSUPPORTED;};
        std::shared_ptr<xrfg::PrivateOpenXrRuntime> endpoint;
        if(installed) {
            state->d3d12_device=device;state->d3d12_queue=queue;
            state->vdxr_legacy=false;
            state->dispatch->create_swapchain=options.real.create;
            state->dispatch->destroy_swapchain=options.real.destroy;
            state->dispatch->enumerate_swapchain_images=options.real.enumerate;
            state->dispatch->acquire_swapchain_image=options.real.acquire;
            state->dispatch->wait_swapchain_image=options.real.wait_image;
            state->dispatch->release_swapchain_image=options.real.release;
            check(install_private_runtime(state)==S_OK,"production endpoint installation");
            endpoint=state->private_runtime;
            check(endpoint && !state->luke_fresh_only,"installed endpoint must own frame admission, not HFW's inline counter filter");
            // The render identity may be held for arbitrarily many app calls
            // during a level transition. It is not private output admission.
            state->luke_source_filter_armed=true;
            state->luke_last_source=xrfg::LukeRenderIdentity{0x12340000,4190};
        } else {
            endpoint=std::make_shared<xrfg::PrivateOpenXrRuntime>(std::move(options));
            check(SUCCEEDED(endpoint->initialize()),"startup endpoint initialize");
        }
        check(XR_SUCCEEDED(endpoint->start()),"startup endpoint start");
        struct Stop {std::shared_ptr<xrfg::PrivateOpenXrRuntime> endpoint;~Stop(){endpoint->stop();}} stop{endpoint};
        state->private_runtime=endpoint;
        state->dispatch->wait_frame=private_runtime_wait;
        state->dispatch->begin_frame=private_runtime_begin;
        state->dispatch->end_frame=private_runtime_end;
        if(meter) {
            state->fps_overlay=std::make_unique<xrfg::OpenXrFpsOverlay>(XR_NULL_HANDLE,state->handle,0,
                nullptr,content_end_frame,nullptr,nullptr,nullptr,std::filesystem::path{});
            bind_private_delivery_metrics(state);
        }
        for(unsigned n=0;n<(installed?12u:3u);++n) {
            XrFrameState predicted{XR_TYPE_FRAME_STATE};
            check(XR_SUCCEEDED(layer_wait_frame_impl(state->handle,nullptr,&predicted)),"startup application Wait");
            check(XR_SUCCEEDED(layer_begin_frame_impl(state->handle,nullptr)),"startup application Begin");
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=predicted.predictedDisplayTime;
            end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            const auto result=meter?state->fps_overlay->end_frame(&end,false):content_end_frame(state->handle,&end);
            if(XR_FAILED(result))std::cerr<<"startup fresh="<<fresh<<" meter="<<meter<<" frame="<<n<<" result="<<result<<'\n';
            check(XR_SUCCEEDED(result),"startup private meter admission must follow its Wait/Begin");
            consume_application_frame(state,predicted.predictedDisplayTime,true,false);
            check(XR_SUCCEEDED(endpoint->drain()),"startup packet drain");
            check(!state->luke_output_begun,"startup output ownership closed");
        }
    }
    std::cout<<"Luke first/repeated frames: ordinary/fresh x meter absent/present accepted; installed="<<installed<<'\n';
}
