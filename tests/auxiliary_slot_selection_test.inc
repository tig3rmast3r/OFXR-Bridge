// Exercise production auxiliary capture/selection against real endpoint reader
// leases. The preparer gate is before its source copy, not a synthetic sleep.
static int auxiliary_slot_selection_test(unsigned mode) {
    const bool pooled_budget=mode==3;if(pooled_budget)mode=2;
    xrfg_fixture::ControlProfile profile;
    const bool independent_route=mode>=4 && mode<=6, luke=mode==6 || mode==7, d11=mode==5 || luke;
    Fixture f(!d11,true,{},false,true,independent_route && d11); f.throttled=false;
    const auto projection=f.make_chain(); f.write(projection,10);
    const auto source=f.make_chain(false,1); f.write(source,40);
    auto dispatch=std::make_shared<Dispatch>();
    dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=true;
    dispatch->enumerate_swapchain_images=+[](XrSwapchain h,uint32_t cap,uint32_t* count,XrSwapchainImageBaseHeader* out) -> XrResult {
        return current->transport->enumerate_images(h,cap,count,out);
    };
    dispatch->acquire_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageAcquireInfo* info,uint32_t* index) -> XrResult {
        return current->transport->acquire_image(h,info,index);
    };
    dispatch->wait_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageWaitInfo* info) -> XrResult {
        return current->transport->wait_image(h,info);
    };
    dispatch->release_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageReleaseInfo* info) -> XrResult {
        return current->transport->release_image(h,info);
    };
    dispatch->destroy_swapchain=+[](XrSwapchain h) -> XrResult {return current->transport->destroy_swapchain(h);};
    auto state=std::make_shared<SessionState>(dispatch);
    state->handle=reinterpret_cast<XrSession>(2);state->graphics_binding=d11?SessionGraphicsBinding::d3d11:SessionGraphicsBinding::d3d12;
    state->independent_pacing=independent_route;state->d3d11_device=f.device11;state->d3d11_context=f.context;
    state->vdxr_legacy=false;state->private_runtime=f.transport;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->uevr_application=pooled_budget;
    state->luke_afw_enabled=luke;
    dispatch->end_frame=+[](XrSession,const XrFrameEndInfo* info) -> XrResult {
        return current->transport->end_frame(info);
    };
    if(pooled_budget || independent_route)f.native_chain_limit=3; // app eye + app UI + one shared UI publication sink
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ci.width=ci.height=ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    auto app=std::make_shared<SwapchainState>(state,ci);app->handle=source;
    uint32_t count{};
    if(d11) {
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xr_ok(f.transport->enumerate_images(source,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"enumerate auxiliary D11 source");
        app->enumerated_d3d11_images.emplace_back(image.texture);
    } else {
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        xr_ok(f.transport->enumerate_images(source,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"enumerate auxiliary application source");
        app->enumerated_d3d12_images.emplace_back(image.texture);
    }
    {std::scoped_lock lock(g_state_mutex);g_swapchains[source]=app;g_sessions[state->handle]=state;}
    const auto snapshot=[&](unsigned color) {
        f.write(source,color);ok(capture_composition_image(app,0),"capture auxiliary application image");
        app->composition_capture->valid=true;
        auto packet=std::make_shared<GeneratedFrameEndInfo>();
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.pose.orientation.w=1;
        quad.size={1,1};quad.subImage.swapchain=source;quad.subImage.imageRect.extent={1,1};
        packet->composition_layers.emplace_back(quad);return packet;
    };
    const auto publish=[&](unsigned color) {
        auto packet=snapshot(color);xr_ok(own_composition_images(state,*packet),"publish retained auxiliary image");
        check(packet->composition_images.size()==1,"one auxiliary image owner");
        if(luke && independent_route) {
            check(!packet->composition_images[0]->public_output && !app->composition_capture->public_output,
                "Independent Luke auxiliary must own its image without late shared-image publication");
            check(state->auxiliary_registry.empty(),"Independent Luke uses real private handles, never legacy snapshot tokens");
        }
        return packet;
    };
    // Application/private source images use RENDER_TARGET, while the auxiliary
    // production allocation's declared state is COMMON. The native mock keeps
    // the matching state per destination chain.
    f.real_common=true;
    if(mode==7) {
        std::vector<std::shared_ptr<GeneratedFrameEndInfo>> held;
        for(unsigned color:{40U,80U,120U})held.push_back(publish(color));
        const auto output=app->composition_capture->public_output;
        check(output && output->image.handle,"legacy Luke retains its shared auxiliary output");
        for(const auto& packet:held) {
            const auto& slot=packet->composition_images[0];
            check(slot->public_output==output && private_composition_slot(state,slot->image.handle)==slot,
                "legacy Luke retains snapshot tokens and publication ownership");
            ok(wait_composition_fence(slot->fence.Get(),slot->ready_value),"legacy snapshot copy completion");
        }
        check(f.chains.size()==3,"legacy Luke auxiliary uses one native output");
        held.clear();destroy_composition_capture(app);
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(source);g_sessions.erase(state->handle);}
        xr_ok(f.transport->destroy_swapchain(source),"legacy auxiliary source cleanup");
        xr_ok(f.transport->destroy_swapchain(projection),"legacy projection cleanup");
        std::puts("Legacy Luke D11 auxiliary snapshot-token/shared-publication contract preserved PASS");return 0;
    }
    if(independent_route) {
        std::vector<std::shared_ptr<GeneratedFrameEndInfo>> held;
        for(unsigned color:{40U,80U,120U,160U,200U,240U})held.push_back(publish(color));
        check(app->composition_capture->outputs.size()==20,"independent auxiliary capacity follows source+output ownership");
        check(f.chains.size()==3,"six private snapshots must use one native sink");
        if(!luke)f.unblock();
        for(unsigned i=0;i<held.size();++i) {
            f.begin_virtual();
            if(luke) {
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose.orientation.w=1;view.pose.position.x=float(i+1);
                view.subImage.swapchain=projection;view.subImage.imageRect.extent={1,1};
                XrCompositionLayerProjection eye{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                eye.space=reinterpret_cast<XrSpace>(9);eye.viewCount=1;eye.views=&view;
                auto& quad=held[i]->composition_layers[0];
                const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&eye),composition_layer_header(quad)};
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.layerCount=2;end.layers=layers;
                end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                xr_ok(end_frame_with_private_auxiliary(state,&end,{},{}),"Independent Luke accepts without late auxiliary copy");
                if(i==0) {f.wait_entered();continue;}
                if(i==1) {
                    check(f.real_ends==0,"next owned auxiliary packet accepted while prior native End remains blocked");
                    f.unblock();
                }
            } else xr_ok(f.submit(projection,float(i+1),held[i]->composition_images[0]->image.handle),"retained auxiliary native delivery");
            f.wait_count(i+1);
        }
        xr_ok(f.transport->drain(),"independent auxiliary drain");
        check(f.overlay_pixels==std::vector<unsigned>({40,80,120,160,200,240}),"all six retained auxiliary colors survive later source rewrites");
        if(luke) {
            check(f.positions==std::vector<float>({1,2,3,4,5,6}),"Independent Luke retains projection poses/order with exact auxiliary pixels");
            check(f.chains.size()==3,"Independent Luke delivery never allocates a second auxiliary native sink");
            std::puts("Independent Luke D11: six immutable auxiliary images/one sink; End accepts during blocked native delivery; exact pixels/poses PASS");
        }
        held.clear();destroy_composition_capture(app);
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(source);g_sessions.erase(state->handle);}
        xr_ok(f.transport->destroy_swapchain(source),"independent auxiliary source cleanup");
        xr_ok(f.transport->destroy_swapchain(projection),"independent projection cleanup");
        std::puts(d11?"Independent D11 six auxiliary snapshots/one sink/exact pixels PASS":"Independent D12 six auxiliary snapshots/one sink/exact pixels PASS");return 0;
    }
    std::array<std::shared_ptr<GeneratedFrameEndInfo>,3> retained;
    retained[0]=publish(40);
    const auto first=retained[0]->composition_images[0]->image.handle;
    XrSwapchain second{};
    if(mode!=1) {
        retained[1]=publish(80);second=retained[1]->composition_images[0]->image.handle;
        check(second!=first,"retained snapshots require distinct output slots");
    }
    if(mode==2)retained[2]=publish(120);
    f.gpu_wait();
    std::promise<void> preparer_entered,release_preparer;
    auto entered=preparer_entered.get_future();auto release=release_preparer.get_future().share();
    f.preparation_callback=[&](uint64_t sequence) {
        if(sequence==1){preparer_entered.set_value();release.wait();}
    };
    const unsigned queued=mode==2?3:1;
    for(unsigned i=0;i<queued;++i) {
        f.begin_virtual();xr_ok(f.submit(projection,float(i+1),retained[i]->composition_images[0]->image.handle),"accept auxiliary source with endpoint reader lease");
        if(!i)check(entered.wait_for(3s)==std::future_status::ready,"preparer reached source lease gate");
    }
    for(auto& packet:retained)packet.reset();
    check(app->composition_capture->outputs[0].use_count()==1,"CPU descriptor is free while its endpoint source reader remains held");
    auto newest=snapshot(200);
    auto selection=std::async(std::launch::async,[&]{return own_composition_images(state,*newest);});
    const bool independent=selection.wait_for(mode==2?60ms:500ms)==std::future_status::ready;
    // Always unblock and retire before asserting, so the negative-control
    // failure is a selection failure rather than a test shutdown deadlock.
    release_preparer.set_value();f.unblock();
    check(selection.wait_for(4s)==std::future_status::ready,"auxiliary selection completes after readers retire");
    xr_ok(selection.get(),"auxiliary selection result after reader retirement");
    const auto selected=newest->composition_images[0]->image.handle;
    f.begin_virtual();xr_ok(f.submit(projection,float(queued+1),selected),"submit newly selected auxiliary image");
    f.wait_count(queued+1);xr_ok(f.transport->drain(),"drain all retained auxiliary images");
    const std::vector<unsigned> expected=mode==2?std::vector<unsigned>{40,80,120,200}:std::vector<unsigned>{40,200};
    check(f.overlay_pixels==expected,"endpoint source leases preserve old and new auxiliary pixels in order");
    check(f.positions.size()==queued+1,"every auxiliary frame delivered once");
    for(unsigned i=0;i<=queued;++i)check(f.positions[i]==float(i+1),"projection geometry/order retained through auxiliary selection");
    unsigned allocated{};for(const auto& slot:app->composition_capture->outputs)allocated+=bool(slot);
    if(pooled_budget)check(f.chains.size()==3,"three retained UI snapshots must use one native destination");
    newest.reset();destroy_composition_capture(app);
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(source);g_sessions.erase(state->handle);}
    xr_ok(f.transport->destroy_swapchain(source),"auxiliary application source cleanup");
    xr_ok(f.transport->destroy_swapchain(projection),"auxiliary projection cleanup");
    check(allocated<=3,"auxiliary selection remains bounded to three outputs");
    if(mode==2)check(!independent && allocated==3,"all busy auxiliary outputs retain bounded blocking backpressure");
    else {
        check(independent,"a busy first auxiliary image must not block an available later or lazy slot");
        check(selected!=first,"endpoint reader held on first slot cannot be overwritten");
        check(mode==1?allocated==2:(allocated==2 && selected==second),"reuse existing free outputs before allocating additional slots");
    }
    std::printf("Auxiliary slot selection mode %u: reader leases, bounded allocation, ordered pixels and geometry PASS\n",mode);
    return 0;
}
