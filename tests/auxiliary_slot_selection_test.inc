// Exercise production auxiliary capture/selection against real endpoint reader
// leases. The preparer gate is before its source copy, not a synthetic sleep.
static int auxiliary_slot_selection_test(unsigned mode) {
    const bool pooled_budget=mode==3;if(pooled_budget)mode=2;
    xrfg_fixture::ControlProfile profile;
    Fixture f(true); f.throttled=false;
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
    state->handle=reinterpret_cast<XrSession>(2);state->graphics_binding=SessionGraphicsBinding::d3d12;
    state->vdxr_legacy=false;state->private_runtime=f.transport;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->uevr_application=pooled_budget;
    if(pooled_budget)f.native_chain_limit=3; // app eye + app UI + one shared UI publication sink
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ci.width=ci.height=ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    auto app=std::make_shared<SwapchainState>(state,ci);app->handle=source;
    XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};uint32_t count{};
    xr_ok(f.transport->enumerate_images(source,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"enumerate auxiliary application source");
    app->enumerated_d3d12_images.emplace_back(image.texture);
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
        check(packet->composition_images.size()==1,"one auxiliary image owner");return packet;
    };
    // Application/private source images use RENDER_TARGET, while the auxiliary
    // production allocation's declared state is COMMON. The native mock keeps
    // the matching state per destination chain.
    f.real_common=true;
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
