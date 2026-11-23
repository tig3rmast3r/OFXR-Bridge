// Exercise the shipping reconstruction guard and virtual-End retirement with
// real private swapchains and DLSS guide publications, not a pose-clock model.
static int uevr_camera_loss_test(bool source_binding=false) {
    xrfg_fixture::ControlProfile profile(true);
    auto desired=xrfg::embedded::snapshot().desired;desired.enabled=false;
    check(xrfg_fixture::request_control(desired),"OSR-only control profile");
    independent_fixture_mode=true;
    Fixture f(true);f.throttled=false;f.unblock();
    const auto left=f.make_chain(),right=f.make_chain();
    f.write(left,77);f.write(right,88);
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
    dispatch->end_frame=+[](XrSession,const XrFrameEndInfo* info)->XrResult {
        return current->transport->end_frame(info);
    };
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->graphics_binding=SessionGraphicsBinding::d3d12;state->independent_pacing=true;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->private_runtime=f.transport;state->menu_enabled=false;
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.width=create.height=create.arraySize=create.mipCount=create.faceCount=create.sampleCount=1;
    create.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    std::array<XrSwapchainImageD3D12KHR,3> images;
    for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    unsigned count{};xr_ok(f.transport->enumerate_images(left,3,&count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"camera-loss source images");
    {
        std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;
        for(auto handle:{left,right}) {
            auto chain=std::make_shared<SwapchainState>(state,create);chain->handle=handle;
            g_swapchains[handle]=chain;
        }
    }
    ComPtr<ID3D12Resource> motion,depth;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width=desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    desc.Format=DXGI_FORMAT_R16G16_FLOAT;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&motion)),"camera-loss motion");
    desc.Format=DXGI_FORMAT_R32_FLOAT;
    ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&depth)),"camera-loss depth");
    ComPtr<ID3D12DescriptorHeap> guide_views;D3D12_DESCRIPTOR_HEAP_DESC view_desc{};
    view_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;view_desc.NumDescriptors=2;
    ok(f.device12->CreateDescriptorHeap(&view_desc,IID_PPV_ARGS(&guide_views)),"binding guide RTVs");
    auto mv=guide_views->GetCPUDescriptorHandleForHeapStart(),z=mv;
    z.ptr+=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    f.device12->CreateRenderTargetView(motion.Get(),nullptr,mv);f.device12->CreateRenderTargetView(depth.Get(),nullptr,z);
    f.commands([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;}
        barriers[0].Transition.pResource=motion.Get();barriers[1].Transition.pResource=depth.Get();
        list->ResourceBarrier(2,barriers);const float zero[4]{},far_depth[4]{.01F,.01F,.01F,1};
        list->ClearRenderTargetView(mv,zero,0,nullptr);list->ClearRenderTargetView(z,far_depth,0,nullptr);
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(2,barriers);
    });
    xrfg::configure_dlss_motion_vector_tracking(true);
    const auto publish=[&] {
        ComPtr<ID3D12GraphicsCommandList> recorded;
        f.commands([&](ID3D12GraphicsCommandList* list) {
            recorded=list;xrfg::DlssMotionVectorPublication p{};
            p.stream=100;p.output=images[0].texture;p.motion_vectors=motion.Get();p.depth=depth.Get();
            p.producer_queue=f.queue.Get();p.producer_command_list=list;
            p.output_width=p.output_height=p.motion_width=p.motion_height=p.depth_width=p.depth_height=1;
            p.depth_inverted=true;p.camera_near=10;
            check(bool(xrfg::publish_dlss_motion_vectors(p)),"camera-loss fresh guide publication");
        });
        ID3D12CommandList* lists[]={recorded.Get()};xrfg::report_dlss_motion_vector_submission(f.queue.Get(),1,lists);
        const auto guides=xrfg::resolve_dlss_motion_vectors(1,f.queue.Get());
        check(guides && guides->eye_count && guides->eyes[0],"camera-loss guide resolved");
        return guides->eyes[0]->publication;
    };
    if(source_binding) {
        state->uevr_afr=std::make_unique<UevrAfrOsrState>();
        auto& bound=*state->uevr_afr;bound.active=bound.layout=true;
        bound.eye_swapchain={left,right};bound.space=reinterpret_cast<XrSpace>(9);
        bound.display_period=period;bound.application_display_time=epoch+ns();
        for(auto handle:{left,right}) {
            auto chain=find_swapchain(handle);
            std::array<XrSwapchainImageD3D12KHR,3> owned;
            for(auto& image:owned)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(f.transport->enumerate_images(handle,3,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(owned.data())),"bound source images");
            for(const auto& image:owned)chain->enumerated_d3d12_images.emplace_back(image.texture);
            chain->acquired_indices.push_back(0);
        }
        auto& feed=uevr_camera_feed();
        const auto cameras=[&](unsigned first) {
            std::scoped_lock lock(feed.mutex);feed.queue.clear();
            for(unsigned eye=0;eye<2;++eye) {
                xrfg::UevrEyeCamera camera{};camera.sequence=first+eye;camera.ticks=GetTickCount64();
                camera.xr_valid=true;camera.xr_time=bound.application_display_time;camera.world_to_meters=100;
                for(unsigned e=0;e<2;++e)camera.xr_eyes[e].position={e?.032F:-.032F,0,0};
                camera.base_location={double(first*100),0,0};
                camera.location={camera.base_location[0],eye?3.2:-3.2,0};
                feed.queue.push(camera,eye);
            }
        };
        const auto release_pair=[&] {
            uevr_afr_before_release(find_swapchain(left));
            uevr_afr_before_release(find_swapchain(right));
        };
        cameras(1);bound.reset_pending=false;
        release_pair();
        check(feed.queue.size()==0 && bound.reset_pending,
            "missing guides retire their source camera observations through the real release hook");
        const auto stale=publish();bound.last_publication=stale;
        cameras(3);bound.reset_pending=false;release_pair();
        check(feed.queue.size()==0 && bound.reset_pending,
            "stale guides cannot leave orphan camera observations for the next source image");
        const auto fresh=publish();cameras(5);bound.reset_pending=false;
        uevr_afr_before_release(find_swapchain(left));
        const auto next=feed.queue.take_eye(1,GetTickCount64(),200);
        check(next && next->camera.sequence==6 && feed.queue.size()==0,
            "the fresh left source consumes only its own camera, leaving the fresh right source intact");
        check(bound.last_publication==fresh && bound.reset_pending && bound.reconstructions==0,
            "an unavailable slot consumes the bound guide without generating a fallback");
        const auto frozen_publication=publish();cameras(7);
        const auto frozen=uevr_afr_capture_source(*state,bound,left,0,0,images[0].texture);
        check(SUCCEEDED(frozen.result) && frozen.guide && frozen.guide->publication==frozen_publication &&
            frozen.camera && frozen.camera->sequence==7 && bound.renderer.source_matches(frozen.pixels),
            "one source receipt owns its exact colour capture, guide and render camera");
        const auto newer_publication=publish();cameras(9);
        const auto queued=feed.queue.size();
        check(!uevr_afr_reconstruct(*state,bound,frozen) && feed.queue.size()==queued &&
            bound.last_publication==frozen_publication,
            "reconstruction cannot retake a camera or replace the bound guide with newer globals");
        const auto newer=uevr_afr_capture_source(*state,bound,left,0,0,images[0].texture);
        check(newer.camera && newer.camera->sequence==9 && newer.guide->publication==newer_publication &&
            bound.renderer.source_matches(newer.pixels) && !bound.renderer.source_matches(frozen.pixels),
            "a recapture invalidates the old receipt even when it reuses the same private texture");
        xrfg::UevrOsrFrame wrong_source{};wrong_source.source=frozen.pixels;
        wrong_source.source_eye=0;wrong_source.guide=frozen.guide.get();
        const auto refused=bound.renderer.render(f.device12.Get(),f.queue.Get(),wrong_source,
            images[0].texture,D3D12_RESOURCE_STATE_RENDER_TARGET);
        check(FAILED(refused.result) && refused.reason==20,
            "the GPU renderer refuses a replaced capture instead of combining it with old pose metadata");
        const auto generation=newer.generation;
        uevr_afr_reset_sources(bound);
        check(bound.source_generation!=generation && feed.queue.size()==0 &&
            !uevr_afr_reconstruct(*state,bound,newer),
            "rearming/layout changes invalidate old source receipts and queued observations together");
        dispatch->acquire_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageAcquireInfo* i,unsigned* n)->XrResult {
            return current->transport->acquire_image(h,i,n);
        };
        dispatch->wait_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageWaitInfo* i)->XrResult {
            return current->transport->wait_image(h,i);
        };
        dispatch->release_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageReleaseInfo* i)->XrResult {
            return current->transport->release_image(h,i);
        };
        dispatch->locate_views=+[](XrSession,const XrViewLocateInfo*,XrViewState* s,unsigned,unsigned* n,XrView* v)->XrResult {
            s->viewStateFlags=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;*n=2;
            for(unsigned e=0;e<2;++e){v[e].pose.orientation.w=1;v[e].pose.position={e?.032F:-.032F,0,0};}
            return XR_SUCCESS;
        };
        for(unsigned e=0;e<2;++e) {
            bound.fov[e]={-.7F,.7F,.7F,-.7F};bound.rect[e].extent={1,1};bound.image_width[e]=bound.image_height[e]=1;
        }
        for(auto handle:{left,right})find_swapchain(handle)->acquired_indices.clear();
        bound.application_display_time=epoch+ns()+period;
        publish();cameras(11);
        const auto recovered=uevr_afr_capture_source(*state,bound,left,0,0,images[0].texture);
        check(uevr_afr_reconstruct(*state,bound,recovered) && bound.frame_pose_valid &&
            bound.frame_time==bound.application_display_time && bound.frame_pose[0].position.x==-.032F,
            "the recovered receipt produces real GPU stereo with its matching pose and timestamp");
        f.commands([](ID3D12GraphicsCommandList*){});
        Chain inspected;inspected.d12.emplace_back(images[0].texture);
        check(std::abs(int(f.read_pixel(inspected))-77)<=1,
            "identity-pose recovery preserves the actual captured source pixels");
        const auto first_output_guide=bound.source_guide;
        check(first_output_guide && first_output_guide->eyes[0],"recovered output owns its generated guide");
        bound.reconstructed_eye=0;
        uevr_afr_after_release(find_swapchain(left),XR_ERROR_RUNTIME_FAILURE);
        check(bound.pair_reconstructed && !bound.second_pose_valid,
            "a failed application release cannot disguise already reconstructed pixels as a raw pair");
        bound.eye_releases=0;bound.pair_reconstructed=false;
        for(auto handle:{left,right})find_swapchain(handle)->acquired_indices={0};
        cameras(13);
        uevr_afr_before_release(find_swapchain(left));
        uevr_afr_after_release(find_swapchain(left),XR_SUCCESS);
        uevr_afr_before_release(find_swapchain(right));
        uevr_afr_after_release(find_swapchain(right),XR_SUCCESS);
        check(bound.reconstructions>0 && !bound.pair_reconstructed && !bound.second_pose_valid && feed.queue.size()==0,
            "historical OSR success cannot retire an untouched raw pair when both current guides are stale");
        publish();cameras(15);
        uevr_afr_before_release(find_swapchain(left));
        const auto next_output_guide=bound.source_guide;
        check(next_output_guide && next_output_guide->eyes[0] && next_output_guide->eyes[0]->reset &&
            next_output_guide->eyes[0]->previous_serial==0 &&
            next_output_guide->eyes[0]->epoch>first_output_guide->eyes[0]->epoch,
            "recovery resets the generated guide/pose history together with the source-image history");
        f.transport->stop();
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(left);g_swapchains.erase(right);g_sessions.erase(state->handle);}
        xr_ok(f.transport->destroy_swapchain(left),"binding left cleanup");
        xr_ok(f.transport->destroy_swapchain(right),"binding right cleanup");
        xrfg::configure_dlss_motion_vector_tracking(false);
        std::puts("UEVR source binding: real release hooks retire missing/stale sources and recover camera identity PASS");
        return 0;
    }
    UevrAfrOsrState afr;afr.eye_swapchain={left,right};afr.display_period=period;
    afr.application_display_time=epoch+ns();afr.space=reinterpret_cast<XrSpace>(9);
    // The old fallback used this nearest pose when no camera matched, so
    // supply it rather than letting an empty fixture hide the unsafe path.
    auto& feed=uevr_camera_feed();
    {
        std::scoped_lock lock(feed.mutex);feed.queue.clear();
        feed.located[0].time=afr.application_display_time;
        for(auto& pose:feed.located[0].poses)pose.orientation.w=1;
    }
    for(unsigned fault=0;fault<3;++fault) {
        const auto publication=publish();afr.reset_pending=false;
        xrfg::UevrEyeCamera camera{};camera.ticks=GetTickCount64();camera.sequence=fault+1;
        camera.world_to_meters=100;camera.xr_time=afr.application_display_time;
        camera.xr_valid=fault==2;
        {
            std::scoped_lock lock(feed.mutex);feed.queue.clear();
            if(fault)feed.queue.push(camera,0);
        }
        const auto captured=uevr_afr_capture_source(*state,afr,left,0,0,images[0].texture);
        check(!uevr_afr_reconstruct(*state,afr,captured),
            "missing camera, missing camera pose or unavailable slot must refuse reconstruction");
        check(afr.last_publication==publication && afr.reset_pending && afr.reconstructions==0,
            "rejected guide is consumed and history is reset, without a fallback frame");
    }
    // Retiring the application End must close only its begun ticket, retain
    // an overlapping next Wait, and never submit an empty or mixed-eye End.
    f.begin_virtual();state->application_frame_in_progress=true;
    state->pending_frames={{afr.application_display_time,period,1,{}},
        {afr.application_display_time+period,period,2,{}}};
    state->independent_begun_frame=state->pending_frames.front();
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=afr.application_display_time;
    end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xr_ok(layer_end_frame_impl(state->handle,&end,nullptr,true),"unsafe OSR End retired by shipping layer");
    check(!state->application_frame_in_progress && !state->independent_begun_frame &&
        state->pending_frames.size()==1 && state->pending_frames.front().serial==2,
        "retirement balances Begin and preserves the next application ticket");
    xr_ok(f.transport->drain(),"discarded OSR End drains");
    check(f.real_ends==0 && f.empty_ends==0,"unsafe OSR frame never reached native End");
    f.begin_virtual();xr_ok(f.submit(left,1),"valid frame recovers after camera-loss retirement");
    f.wait_count(1);xr_ok(f.transport->drain(),"camera-loss recovery drains");
    check(f.pixels.size()==1 && std::abs(int(f.pixels.front())-77)<=1,
        "the first delivered content is the valid recovery image");
    f.transport->stop();
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(left);g_swapchains.erase(right);g_sessions.erase(state->handle);}
    xr_ok(f.transport->destroy_swapchain(left),"camera-loss left cleanup");
    xr_ok(f.transport->destroy_swapchain(right),"camera-loss right cleanup");
    xrfg::configure_dlss_motion_vector_tracking(false);
    std::puts("UEVR camera loss: no camera/pose/slot fallback; balanced virtual discard and real-image recovery PASS");
    return 0;
}
