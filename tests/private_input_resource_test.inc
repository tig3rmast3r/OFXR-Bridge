static ID3D12Resource* private_depth_test_image{};
static int private_depth_trim_test() {
    xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();
    auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;state->generation_budget_exhausted=true;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=320;info.height=180;
    info.arraySize=info.mipCount=info.faceCount=info.sampleCount=1;info.format=DXGI_FORMAT_D32_FLOAT;
    info.usageFlags=XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    auto chain=std::make_shared<SwapchainState>(state,info);chain->handle=reinterpret_cast<XrSwapchain>(0x123456);
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=info.width;desc.Height=info.height;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_D32_FLOAT;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> depth;
    ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,
        nullptr,IID_PPV_ARGS(&depth)),"native depth source");private_depth_test_image=depth.Get();
    dispatch->enumerate_swapchain_images=+[](XrSwapchain,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out)->XrResult {
        *count=1;if(capacity)reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)->texture=private_depth_test_image;return XR_SUCCESS;};
    {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
    for(bool new_route:{false,true,false}) {
        state->nvidia_new_route=new_route;
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};uint32_t count{};
        xr_ok(layer_enumerate_swapchain_images_impl(chain->handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"shipping depth enumeration");
        check(image.texture==depth.Get() && chain->enumerated_d3d12_images.front().Get()==depth.Get(),"native depth and guide source retained");
        check(!chain->d3d12_history,"unused depth temporal ring allocated");
        ok(reconcile_depth_history_for_route(*chain),"depth route reconciliation");
        check(!chain->d3d12_history,"route switch recreated unused depth ring");
    }
    // Model history inherited from a pre-fix route: allocate all real slots,
    // then verify the normal reconciler retires their exact GPU writes.
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();ID3D12Resource* source=depth.Get();
    ok(history->initialize(f.device12.Get(),f.queue.Get(),{&source,1},D3D12_RESOURCE_STATE_DEPTH_WRITE),"legacy depth history");
    for(unsigned n=0;n<3;++n){xrfg::D3D12HistoryCaptureTicket ticket;ok(history->capture(0,&ticket),"legacy depth copy");ok(history->commit(ticket),"legacy depth commit");}
    chain->d3d12_history=history;std::weak_ptr<xrfg::D3D12SwapchainHistory> retired=history;history.reset();
    ok(reconcile_depth_history_for_route(*chain),"retire inherited depth ring");
    check(retired.expired() && !chain->d3d12_history,"inherited depth snapshots retained");
    state->private_runtime.reset();check(!omit_native_depth_history(*state),"native legacy route changed");
    state->private_runtime=f.transport;state->luke_afw_enabled=true;check(!omit_native_depth_history(*state),"Luke legacy route changed");
    state->luke_afw_enabled=false;state->graphics_binding=SessionGraphicsBinding::d3d11;check(!omit_native_depth_history(*state),"D11 legacy route changed");
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(chain->handle);}
    std::printf("Private depth: no unused history, exact native guide source, inherited GPU ring retirement PASS\n");return 0;
}
static int invalid_private_rect_test() {
    xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();f.throttled=false;
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->private_runtime=f.transport;state->uevr_application=true;state->handle=reinterpret_cast<XrSession>(2);
    const auto handle=f.make_chain();XrSwapchainCreateInfo shape{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    shape.width=shape.height=32;shape.arraySize=1;
    auto chain=std::make_shared<SwapchainState>(state,shape);chain->handle=handle;
    {std::scoped_lock lock(g_state_mutex);g_swapchains[handle]=chain;}
    std::array<XrCompositionLayerProjectionView,2> views{};auto& view=views[0];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.subImage={handle,{{0,0},{32,32}},0};
    views[1]=view;XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=2;projection.views=views.data();
    XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};panel.subImage={handle,{{0,0},{8,8}},0};
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<XrCompositionLayerBaseHeader*>(&panel)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=2;end.layers=layers;
    const auto good=view.subImage;
    for(unsigned kind=0;kind<6;++kind) {
        view.subImage=good;
        if(kind==0)view.subImage.imageRect={{INT_MIN,INT_MIN},{INT_MIN,INT_MIN}};
        if(kind==1)view.subImage.imageRect.extent.width=0;
        if(kind==2)view.subImage.imageRect.offset.x=-1;
        if(kind==3)view.subImage.imageRect.extent.height=33;
        if(kind==4)view.subImage.imageRect={{INT_MAX,0},{INT_MAX,32}};
        if(kind==5)view.subImage.imageArrayIndex=1;
        PrivateDepthStrip storage;const auto* result=filter_invalid_private_projections(*state,&end,storage);
        check(result!=&end && result->layerCount==1 && result->layers[0]==layers[1],"invalid projection leaked or valid panel removed");
        check(end.layerCount==2 && end.layers[0]==layers[0],"application composition mutated");
        check(state->generation_continuity_reset_pending.exchange(false),"invalid scene did not retire temporal continuity");
    }
    // Empty invalid scene must close its virtual frame without poisoning the
    // native endpoint; two later valid GPU sources still arrive exactly once.
    end.layerCount=1;view.subImage.imageRect=good.imageRect;view.subImage.imageRect.extent.width=0;
    PrivateDepthStrip invalid;const auto* empty=filter_invalid_private_projections(*state,&end,invalid);
    check(!empty->layerCount && !empty->layers,"all-invalid scene was forwarded");
    f.begin_virtual();XrFrameState prediction{XR_TYPE_FRAME_STATE};xr_ok(f.transport->predict_frame(&prediction),"invalid frame clock");
    auto close=*empty;close.displayTime=prediction.predictedDisplayTime;close.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xr_ok(f.transport->end_frame(&close),"close invalid scene");xr_ok(f.transport->drain(),"invalid scene drain");
    view.subImage=good;PrivateDepthStrip valid;check(filter_invalid_private_projections(*state,&end,valid)==&end,"valid scene altered");
    for(unsigned n=0;n<2;++n){f.write(handle,71+n*20);f.begin_virtual();xr_ok(f.submit(handle,float(n)),"valid scene resumes");f.wait_count(n+1);}
    xr_ok(f.transport->drain(),"valid recovered scenes drain");
    check(f.pixels==std::vector<unsigned>({71,91}),"recovery lost exact source pixels");
    state->uevr_application=false;view.subImage.imageRect.extent.width=0;PrivateDepthStrip excluded;
    check(filter_invalid_private_projections(*state,&end,excluded)==&end,"non-UEVR validation changed");
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);}
    std::puts("Invalid private projection filtering, panel retention, ownership and exact GPU recovery PASS");return 0;
}
