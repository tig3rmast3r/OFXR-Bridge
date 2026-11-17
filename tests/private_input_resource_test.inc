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
