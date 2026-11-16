static void queued_d11_publication_test(bool queued) {
    Fixture f(false,true,{},false,true,true,3);f.throttled=false;f.unblock();
    f.prediction_horizon_ns=period*10;f.expected_views=2;
    ComPtr<IDXGIDevice> dxgi;ok(f.device11.As(&dxgi),"queued publication DXGI device");
    ComPtr<IDXGIAdapter> adapter;ok(dxgi->GetAdapter(&adapter),"queued publication same adapter");
    ComPtr<ID3D12Device> device12;ok(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)),"same-adapter publication fence device");
    ComPtr<ID3D11Device5> device11;ComPtr<ID3D11DeviceContext4> context;
    ok(f.device11.As(&device11),"queued D11 device");ok(f.context.As(&context),"queued D11 context");
    auto shared=[&](ComPtr<ID3D12Fence>& fence){
        ok(device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"publication timeline");
        HANDLE h{};ok(device12->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&h),"publication timeline handle");
        ComPtr<ID3D11Fence> out;const auto hr=device11->OpenSharedFence(h,IID_PPV_ARGS(&out));CloseHandle(h);ok(hr,"publication D11 fence");return out;
    };
    ComPtr<ID3D12Fence> gate;auto gate11=shared(gate);
    std::array<ComPtr<ID3D12Fence>,2> publication;
    std::array<ComPtr<ID3D11Fence>,2> publication11{shared(publication[0]),shared(publication[1])};
    std::array<XrSwapchain,2> chains{f.make_chain(false,1),f.make_chain(false,1)};
    std::array<ComPtr<ID3D11Texture2D>,2> textures;
    std::array<ComPtr<ID3D11RenderTargetView>,2> rtvs;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    auto acquire_outputs=[&]{for(unsigned eye=0;eye<2;++eye){
        unsigned index{},count{};xr_ok(f.transport->acquire_image(chains[eye],&acquire,&index),"queued output acquire");check(index==0,"one-image output domain");
        xr_ok(f.transport->wait_image(chains[eye],&wait),"queued output wait");
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xr_ok(f.transport->enumerate_images(chains[eye],1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"queued exact texture");
        textures[eye]=image.texture;ok(f.device11->CreateRenderTargetView(image.texture,nullptr,&rtvs[eye]),"queued output RTV");
    }};
    auto await_fence=[](ID3D12Fence* fence,std::uint64_t value){
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(bool(event),"publication completion event");
        ok(fence->SetEventOnCompletion(value,event),"publication completion registration");
        const auto status=WaitForSingleObject(event,2000);CloseHandle(event);check(status==WAIT_OBJECT_0,"publication completion timeout");
    };
    auto submit=[&](float pose){
        f.begin_virtual();std::array<XrCompositionLayerProjectionView,2> views{};
        for(unsigned eye=0;eye<2;++eye){views[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[eye].pose.orientation.w=1;
            views[eye].pose.position.x=eye?-pose:pose;views[eye].subImage.swapchain=chains[eye];views[eye].subImage.imageRect.extent={1,1};}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=epoch+ns();info.layerCount=1;info.layers=&layer;info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xr_ok(f.transport->end_frame(&info),"queued stereo submission");
    };
    acquire_outputs();
    for(unsigned eye=0;eye<2;++eye){
        ok(context->Wait(gate11.Get(),eye+1),"hold independent eye publication");
        const float c[]={float(77+eye*56)/255,0,0,1};context->ClearRenderTargetView(rtvs[eye].Get(),c);
        ok(context->Signal(publication11[eye].Get(),1),"exact eye publication receipt");
    }
    ok(context->Wait(gate11.Get(),3),"unrelated future D11 work");context->Flush();
    check(f.transport->release_completed_image(chains[0],&release,0,textures[0].Get(),publication[0].Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"completed API accepted pending image");
    check(f.transport->release_queued_image(chains[0],&release,1,textures[0].Get(),publication[0].Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"queued API accepted foreign index");
    check(f.transport->release_queued_image(chains[0],&release,0,textures[1].Get(),publication[0].Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"queued API accepted foreign texture");
    auto producer=std::async(std::launch::async,[&]{for(unsigned eye=0;eye<2;++eye){
        if(!queued)await_fence(publication[eye].Get(),1);
        xr_ok(queued?f.transport->release_queued_image(chains[eye],&release,0,textures[eye].Get(),publication[eye].Get(),1)
            :f.transport->release_completed_image(chains[eye],&release,0,textures[eye].Get(),publication[eye].Get(),1),"precise publication release");
    }});
    const bool returned_early=producer.wait_for(40ms)==std::future_status::ready;
    check(returned_early==queued,"publication still blocks producer, or control bypassed GPU completion");
    if(queued){producer.get();submit(1);check(!f.source_ready_events,"pending eye image reached consumer");}
    ok(gate->Signal(1),"complete left eye only");await_fence(publication[0].Get(),1);
    check(publication[1]->GetCompletedValue()<1,"right eye completed through its independent gate");
    if(queued){check(!f.source_ready_events,"consumer ignored the right-eye fence");unsigned busy{};
        check(f.transport->try_acquire_image(chains[0],&acquire,&busy)==XR_TIMEOUT_EXPIRED,"pending source lease allowed overwrite");}
    ok(gate->Signal(2),"complete right eye");await_fence(publication[1].Get(),1);
    if(!queued){producer.get();submit(1);}
    const auto deadline=std::chrono::steady_clock::now()+200ms;
    while(!f.source_ready_events && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    check(f.source_ready_events!=0,"publication receipt inherited unrelated future context work");
    ok(gate->Signal(3),"release future work");f.wait_count(1);xr_ok(f.transport->drain(),"queued stereo drain");
    check(f.pixels==std::vector<unsigned>{77} && f.right_pixels==std::vector<unsigned>{133},"queued publication stereo pixels incorrect");
    const auto before=f.source_ready_events.load();ok(context->Wait(gate11.Get(),4),"hold new ordinary generation");context->Flush();
    f.write(chains[0],88);f.write(chains[1],188);submit(2);std::this_thread::sleep_for(15ms);
    check(f.source_ready_events==before,"ordinary rewrite inherited old queued receipt");
    ok(gate->Signal(4),"complete ordinary rewrite");f.wait_count(2);xr_ok(f.transport->drain(),"rewrite drain");
    check(f.pixels.back()==88 && f.right_pixels.back()==188,"ordinary rewrite used stale queued pixels");
    if(queued){
        acquire_outputs();ok(context->Wait(gate11.Get(),5),"hold cancellable publication");
        for(unsigned eye=0;eye<2;++eye){const float c[]={.5F,0,0,1};context->ClearRenderTargetView(rtvs[eye].Get(),c);ok(context->Signal(publication11[eye].Get(),2),"cancellable publication receipt");}
        context->Flush();for(unsigned eye=0;eye<2;++eye)xr_ok(f.transport->release_queued_image(chains[eye],&release,0,textures[eye].Get(),publication[eye].Get(),2),"cancellable queued output");
        submit(3);auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
        const bool cancelled=stopped.wait_for(500ms)==std::future_status::ready;
        ok(gate->Signal(5),"release stopped producer GPU work");stopped.get();
        for(auto& fence:publication)await_fence(fence.Get(),2);
        check(cancelled && f.real_ends==2,"stop waited for pending publication or presented incomplete content");
    }
    for(auto chain:chains)xr_ok(f.transport->destroy_swapchain(chain),"queued output cleanup");
    std::printf("D11_PUBLICATION queued=%u producer_before_gpu=%u exact_stereo=PASS independent_fences=PASS future_work=PASS rewrite=PASS stop=PASS device=%s\n",queued,returned_early,std::getenv("XRFG_TEST_WARP")?"WARP":"hardware");
}
