// Real layer/generator/native presenter; only the downstream OpenXR runtime is mocked.
static int pimax_ready_queue_test() {
    xrfg_fixture::ControlProfile profile;
    Fixture f(true); f.transport->stop(); f.transport.reset(); f.unblock();
    for (bool pimax : {false, true}) for (unsigned multiplier : {2U, 3U, 4U}) {
        auto dispatch = std::make_shared<Dispatch>();
        dispatch->runtime_name = pimax ? "Pimax OpenXR" : "VirtualDesktopXR";
        dispatch->virtual_desktop_runtime = !pimax;
        auto state = std::make_shared<SessionState>(dispatch);
        state->vdxr_legacy = true; state->graphics_binding = SessionGraphicsBinding::d3d12;
        state->nvidia_options.rife_multiplier = multiplier;
        state->d3d12_device = f.device12; state->d3d12_queue = f.queue;
        check(initialize_runtime_binding_queue(*state) == (pimax ? S_OK : S_FALSE), "native queue selection");
        ComPtr<ID3D12Fence> hold, receipt;
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&hold)), "future game work gate");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&receipt)), "runtime handoff receipt");
        // The current source is complete. Only the *following* game work is
        // pending. Its queue must not decide when a ready output reaches XR.
        mark_application_release(state.get());
        if (state->app_release_fence)
            ok(wait_composition_fence(state->app_release_fence.Get(),state->app_release_counter), "current source ready");
        ok(f.queue->Wait(hold.Get(),1), "block future game work");
        join_runtime_queue_to_application(state.get(),state->app_release_counter);
        ok(runtime_queue(*state)->Signal(receipt.Get(),1), "runtime handoff queued");
        const auto progress = wait_composition_fence(receipt.Get(),1,50);
        ok(hold->Signal(1), "release gate before any assertion");
        ok(wait_composition_fence(receipt.Get(),1), "handoff cleanup");
        check(pimax ? SUCCEEDED(progress) : progress == HRESULT_FROM_WIN32(ERROR_BUSY),
            "Pimax handoff inherited future game work, or VDXR queue policy changed");
        std::printf("NATIVE_READY_QUEUE pimax=%u factor=%u independent=%u PASS\n",pimax,multiplier,SUCCEEDED(progress));
        if (pimax) {
            state->three_slot_director = multiplier > 2;
            state->frame_generation_mode = xrfg::D3D12FrameGenerationMode::ofxr;
            state->nvidia_options.rife_model = -1;
            state->menu_enabled = true;
            ok(f.queue->Wait(hold.Get(),2), "hold new source before its release");
            const auto source = mark_application_release(state.get());
            auto release = std::async(std::launch::async,[&] { return prepare_pimax_application_release(*state,source); });
            const bool release_deferred = release.wait_for(20ms) == std::future_status::timeout;
            ok(runtime_queue(*state)->Signal(receipt.Get(),2), "previous batch handoff while new source pending");
            const auto previous_progress = wait_composition_fence(receipt.Get(),2,30);
            ok(hold->Signal(2), "release source gate before assertion");
            ok(release.get(), "source publication completion");
            ok(wait_composition_fence(receipt.Get(),2), "source publication cleanup");
            check(release_deferred == (multiplier > 2), "new source publication readiness not preserved");
            check((SUCCEEDED(previous_progress)) == (multiplier > 2),
                "future source release blocked the previous batch, or changed 2X asynchronous publication");
        }
    }
    return 0;
}

static std::atomic_uint64_t native_last_output_release{};
static XrResult XRAPI_CALL native_publication_release(XrSwapchain chain,const XrSwapchainImageReleaseInfo* info) {
    const auto result=Fixture::release(chain,info);
    if(XR_SUCCEEDED(result) && !find_swapchain(chain))native_last_output_release=handle_value(chain);
    return result;
}
static XrResult XRAPI_CALL native_publication_end(XrSession session,const XrFrameEndInfo* info) {
    if(info->layerCount) {
        const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
        const auto target=p->views[0].subImage.swapchain;
        if(!find_swapchain(target))check(native_last_output_release.load()==handle_value(target),
            "native output was not the image published immediately before its own EndFrame");
    }
    return Fixture::end(session,info);
}
static std::atomic_uint native_batch_hold_after{UINT_MAX};
static XrResult XRAPI_CALL native_batch_end(XrSession session,const XrFrameEndInfo* info) {
    if(info->layerCount && current->real_ends.load()==native_batch_hold_after.load()) {
        current->end_entered=false;current->block_end=true;native_batch_hold_after=UINT_MAX;
    }
    return Fixture::end(session,info);
}
static int vdxr_multiframe_test(std::string route) {
    xrfg_fixture::ControlProfile profile;
    const bool steamvr=route.starts_with("steamvr-framework-");
    const bool pimax=route.starts_with("native-pimax-");
    const bool other=route.starts_with("native-other-");
    const bool publication=route.find("publication")!=std::string::npos;
    const bool batch=route.find("batch")!=std::string::npos;
    const bool fast=route.find("fast")!=std::string::npos;
    const bool switch_live=route.find("switch")!=std::string::npos;
    const bool overlap=route.find("overlap")!=std::string::npos;
    const bool unpaced=route.find("unpaced")!=std::string::npos;
    const bool hybrid=route.find("hybrid")!=std::string::npos;
    const bool fidelity=route.find("fidelity")!=std::string::npos;
    const bool reference=route.find("reference")!=std::string::npos;
    const unsigned initial=switch_live || route.ends_with("-2")?2:route.find("4")!=std::string::npos?4:3;
    auto desired=xrfg::autonomous::snapshot().desired;
    xrfg::autonomous::select_generation(desired,hybrid?3:0,fidelity?0:reference?1:2);
    desired.hybrid_backend=0;desired.frame_multiplier=initial;desired.enabled=true;desired.pacing_mode=0;
    // Exercise Fast on the new multi-output path. Keep the 2X control on
    // Normal: legacy native staging has an independent baseline pixel failure.
    desired.lower_vram=desired.separate_synthesis=fast && initial>2;desired.motion_vectors=hybrid;
    check(xrfg::autonomous::request(desired),"native VDXR route settings");
    xrfg::autonomous::apply_runtime_settings();
    Fixture f(true);f.transport->stop();f.transport.reset();f.throttled=false;f.unblock();
    f.real_common=true;f.expected_views=2;f.prediction_horizon_ns=period*4;
    f.create_state=[] {std::scoped_lock lock(g_state_mutex);return g_swapchains.empty()?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COMMON;};
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
    dispatch->steamvr_runtime=steamvr;
    dispatch->virtual_desktop_runtime=!steamvr && !pimax && !other;
    dispatch->inline_unless_pipelined=dispatch->virtual_desktop_runtime || pimax;
    dispatch->runtime_name=steamvr?"SteamVR/OpenXR":pimax?"Pimax OpenXR":other?"Other OpenXR":"VirtualDesktopXR";
    if(unpaced)f.frame_wait_us=[](unsigned){return 0U;};
    dispatch->wait_frame=paced_steamvr_fixture_wait;
    dispatch->begin_frame=Fixture::begin;dispatch->end_frame=publication?native_publication_end:batch?native_batch_end:Fixture::end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;
    dispatch->enumerate_swapchain_images=Fixture::enumerate;dispatch->acquire_swapchain_image=Fixture::acquire;
    dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=publication?native_publication_release:Fixture::release;
    dispatch->convert_time_to_qpc=Fixture::to_qpc;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->graphics_binding=SessionGraphicsBinding::d3d12;state->vdxr_legacy=!steamvr && !other;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ok(f.device12->CreateCommandQueue(&q,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"native synthesis lane");
    ok(apply_embedded_control(state),"native initial controls");
    if (!steamvr) {
        check(initialize_runtime_binding_queue(*state) == (pimax || other ? S_OK : S_FALSE), "actual native binding queue policy");
        // The mock compositor must use the queue the real runtime receives.
        // Fixture(true) otherwise silently gives every native runtime SteamVR's
        // independent delivery queue and hides future-application dependencies.
        f.delivery_queue = runtime_queue(*state);
    }
    {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;g_sessions[state->handle]=state;}
    if(steamvr){ok(install_private_runtime(state),"SteamVR Framework private endpoint");f.transport=state->private_runtime;xr_ok(f.transport->start(),"start Framework native delivery");}
    check(bool(state->private_runtime)==steamvr,"test uses the selected native/private transport");
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=256;ci.height=128;
    ci.arraySize=2;ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain chain{};xr_ok(layer_create_swapchain_impl(state->handle,&ci,&chain),"native application swapchain");
    std::array<XrSwapchainImageD3D12KHR,3> images;for(auto& i:images)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    unsigned count{};xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"native enumerate");
    ComPtr<ID3D12Resource> depth,motion;
    if(hybrid){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=256;d.Height=128;d.DepthOrArraySize=2;d.MipLevels=1;d.SampleDesc.Count=1;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES h{};h.Type=D3D12_HEAP_TYPE_DEFAULT;d.Format=DXGI_FORMAT_R32_FLOAT;
        ok(f.device12->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&depth)),"native guide depth");
        d.Format=DXGI_FORMAT_R16G16_FLOAT;
        ok(f.device12->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&motion)),"native guide motion");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
        ComPtr<ID3D12DescriptorHeap> views;ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"native guide RTVs");
        auto a=views->GetCPUDescriptorHandleForHeapStart(),b=a;b.ptr+=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        f.device12->CreateRenderTargetView(depth.Get(),nullptr,a);f.device12->CreateRenderTargetView(motion.Get(),nullptr,b);
        f.commands([&](ID3D12GraphicsCommandList* list){const float z[]{.5F,.5F,.5F,1},v[]{0,0,0,0};list->ClearRenderTargetView(a,z,0,nullptr);list->ClearRenderTargetView(b,v,0,nullptr);
            D3D12_RESOURCE_BARRIER barriers[2]{};for(auto& x:barriers){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;x.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}barriers[0].Transition.pResource=depth.Get();barriers[1].Transition.pResource=motion.Get();list->ResourceBarrier(2,barriers);});
    }
    constexpr unsigned per_phase=12;
    const unsigned phases=switch_live?5:1;
    unsigned source{},prior_outputs{};
    std::optional<XrFrameState> prefetched;
    for(unsigned phase=0;phase<phases;++phase) {
        constexpr unsigned factors[]{2,3,4,3,2};const auto factor=switch_live?factors[phase]:initial;
        if(phase){
            desired=xrfg::autonomous::snapshot().desired;desired.frame_multiplier=factor;
            desired.lower_vram=desired.separate_synthesis=fast && factor>2;
            check(xrfg::autonomous::request(desired),"native live multiplier");xrfg::autonomous::apply_runtime_settings();
            // Apply at the real End boundary below, with outstanding virtual tickets.
        }
        for(unsigned n=0;n<per_phase;++n) {
            ++source;
            if(batch && n==4) {
                xr_ok(wait_for_synthesis_handoff(state),"batch probe previous compute drain");
                xr_ok(wait_for_presenter_idle(state),"batch probe starts without old descriptors");
                f.end_entered=false;native_batch_hold_after=f.real_ends.load();
            }
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            if(prefetched){prediction=*prefetched;prefetched.reset();}
            else {auto wait=std::async(std::launch::async,[&]{XrFrameState v{XR_TYPE_FRAME_STATE};xr_ok(layer_wait_frame(state->handle,nullptr,&v),"native split Wait");return v;});prediction=wait.get();}
            xr_ok(layer_begin_frame(state->handle,nullptr),"native Begin");
            std::future<XrFrameState> next;
            if(overlap && n>=3 && n+1<per_phase)next=std::async(std::launch::async,[&]{XrFrameState v{XR_TYPE_FRAME_STATE};xr_ok(layer_wait_frame(state->handle,nullptr,&v),"native overlapping Wait");return v;});
            unsigned index{};xr_ok(layer_acquire_swapchain_image_impl(chain,nullptr,&index),"native acquire");
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
            xr_ok(layer_wait_swapchain_image_impl(chain,&wi),"native image wait");
            const unsigned color=20+(source%8)*20;
            {
                std::scoped_lock lock(f.gpu);
                ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
                ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"native RTV");
                f.device12->CreateRenderTargetView(images[index].texture,nullptr,heap->GetCPUDescriptorHandleForHeapStart());
                const float pixel[]{color/255.F,0,0,1};f.commands([&](ID3D12GraphicsCommandList* list){list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),pixel,0,nullptr);});
            }
            if(batch && n==5) {
                auto release=std::async(std::launch::async,[&]{return layer_release_swapchain_image_impl(chain,nullptr);});
                const bool independent=release.wait_for(1s)==std::future_status::ready;
                if(!independent)f.unblock();
                xr_ok(release.get(),"next source capture during native batch delivery");
                check(independent,"native optical capture waited for entire downstream batch");
            } else xr_ok(layer_release_swapchain_image_impl(chain,nullptr),"native capture");
            if(hybrid){auto c=find_swapchain(chain);auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
                for(unsigned eye=0;eye<2;++eye){auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=source;g->previous_serial=source-1;g->publication=2*source+eye;g->producer_queue=f.queue;g->output=images[index].texture;g->output_slice=eye;g->depth=depth;g->motion_vectors=motion;g->motion_slice=eye;g->output_width=g->depth_width=g->motion_width=256;g->output_height=g->depth_height=g->motion_height=128;guides->eyes[eye]=g;}c->last_released_motion_vectors=guides;}
            std::array<XrCompositionLayerProjectionView,2> views;
            for(unsigned eye=0;eye<2;++eye){auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.x=float(source)-.032F;v.fov={-.7F,.7F,.7F,-.7F};v.subImage.swapchain=chain;v.subImage.imageRect.extent={256,128};v.subImage.imageArrayIndex=eye;}
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.space=reinterpret_cast<XrSpace>(9);layer.viewCount=2;layer.views=views.data();
            auto* content=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&content;
            std::uint64_t preparing{};
            if(batch && n==5){std::scoped_lock lock(state->presenter_mutex);preparing=state->next_source_sequence;}
            xr_ok(layer_end_frame(state->handle,&end),"native generated End");
            if(batch && n==4) {
                std::unique_lock lock(f.mutex);
                check(f.changed.wait_for(lock,3s,[&]{return f.end_entered.load();}),"native first synthetic reaches held downstream End");
            }
            if(batch && n==5) {
                std::unique_lock lock(state->presenter_mutex);
                const bool ready=state->presenter_condition.wait_for(lock,3s,[&]{return state->synthesis_prepared_sequence>=preparing || XR_FAILED(state->presenter_failure);});
                const bool queued=ready && state->synthesis_prepared_sequence>=preparing && state->synthesis_busy &&
                    state->three_slots.slot(0) && state->three_slots.slot(0)->synthetic;
                lock.unlock();f.unblock();
                check(queued,"next optical batch must be GPU-ready while previous synthetic remains downstream");
                std::printf("NATIVE_BATCH_OVERLAP factor=%u fast=%u hybrid=%u source=%llu PASS\n",factor,fast,hybrid,preparing);
            }
            if(next.valid())prefetched=next.get();
        }
        xr_ok(wait_for_synthesis_handoff(state),"native phase synthesis drain");
        if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"native phase output drain");
        if(f.transport)xr_ok(f.transport->drain(),"Framework physical drain");
        const auto delivered=f.real_ends.load()-prior_outputs;
        std::printf("VDXR_NATIVE factor=%u fast=%u overlap=%u phase=%u outputs=%u split=%u owner=%u\n",factor,fast,overlap,phase,delivered,state->split_frame_loop,state->three_slot_director);
        check(state->split_frame_loop,"native fixture must exercise different Wait/End threads");
        check(native_frame_multiplier(*state)==factor,"native live multiplier fell back");
        check(xrfg::autonomous::snapshot().runtime.frame_multiplier==factor,"layer telemetry lost the active native/private multiplier");
        check(factor==2 || state->three_slot_director,"native multi must own an ordered synthesis worker");
        check(delivered>=1+(per_phase-4)*factor,"native phase lost generated outputs");
        if(factor>2 && !switch_live && !overlap)check(delivered==2+(per_phase-2)*factor,"native steady batch lost or duplicated a real/generated output");
        check(bool(state->private_runtime)==steamvr,"live control changed the selected native/private transport");
        prior_outputs=f.real_ends.load();
    }
    stop_continuous_presenter(state);
    std::fprintf(stderr,"NATIVE_CLOCK_COUNTS empty=%u discarded=%u waits=%u begins=%u ends=%u\n",f.empty_ends.load(),f.discarded_clocks.load(),f.real_waits.load(),f.real_begins.load(),f.real_ends.load());
    if(!steamvr)check(!f.empty_ends && !f.discarded_clocks,"native path emitted empty frames or discarded a begun cycle");
    check(std::is_sorted(f.positions.begin(),f.positions.end()),"native generated camera order crossed");
    check(f.pixels.size()==f.positions.size(),"native pixel/metadata receipt lost");
    for(unsigned n=1;n<=source;++n){
        const auto target=float(n)-.032F;
        bool found=false;
        for(unsigned i=0;i<f.positions.size();++i)if(std::abs(f.positions[i]-target)<.001F){
            if(std::abs(int(f.pixels[i])-int(20+(n%8)*20))<=2)found=true;}
        if(!found){std::fprintf(stderr,"MISSING_NATIVE_REAL source=%u expected_pixel=%u\n",n,20+(n%8)*20);for(unsigned i=0;i<f.positions.size();++i)if(std::abs(f.positions[i]-target)<1.01F)std::fprintf(stderr," output=%u pose=%.4f pixel=%u\n",i,f.positions[i],f.pixels[i]);}
        check(found,"native accepted real source missing after drain");
    }
    double min_gap=1e9;
    for(unsigned i=1;i<f.handoffs.size();++i){
        check(f.targets[i]>f.targets[i-1],"native output target times must advance");
        // Native slot identity is authoritative. CPU entry spacing includes
        // variable image preparation after Wait and need not equal one period.
        check(f.targets[i]-f.targets[i-1]>=period,"native outputs reused a display slot");
        min_gap=std::min(min_gap,(f.handoffs[i]-f.handoffs[i-1])/1e6);
    }
    std::fprintf(stderr,"MIN_NATIVE_GAP=%.6fms\n",min_gap);
    if(!steamvr)check(f.real_waits==f.real_begins && f.real_begins==f.real_ends,"native Wait/Begin/End calls are unbalanced");
    xr_ok(layer_destroy_swapchain_impl(chain),"native chain cleanup");
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_instances.clear();}
    std::puts("Native VDXR split-loop: real GPU outputs, native call order, image ownership and live multipliers PASS");
    return 0;
}
