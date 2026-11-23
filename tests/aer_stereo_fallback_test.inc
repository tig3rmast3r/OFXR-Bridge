// Exercise the DI2 live toggle with paired stereo and unusable AER guide placement.
int aer_stereo_fallback_test() {
    xrfg_fixture::ControlProfile profile(true);
    auto settings=xrfg::autonomous::snapshot().desired;
    xrfg::autonomous::select_generation(settings,3,0);
    settings.hybrid_backend=0;settings.motion_vectors=1;settings.frame_multiplier=4;
    settings.enabled=true;settings.ofr_enabled=false;settings.afw_mode=0;settings.nr_enabled=false;
    settings.r19_camera=false;settings.diagnostics=true;
    check(xrfg::autonomous::request(settings),"select ordinary Hybrid 4X before live AER toggle");
    xrfg::autonomous::apply_runtime_settings();
    const auto trace=xrfg::autonomous::configuration_path().parent_path()/L"trace";
    std::filesystem::create_directories(trace);
    WritePrivateProfileStringW(L"diagnostics",L"logging_enabled",L"1",(trace/L"ofxr_bridge.ini").c_str());
    xrfg::bridge_flight_logger().initialize(trace);

    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Fixture f(true);f.transport->stop();f.transport.reset();f.throttled=false;
    f.real_common=true;f.expected_views=2;f.unblock();
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
    dispatch->steamvr_runtime=true;dispatch->wait_frame=paced_steamvr_fixture_wait;
    dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;
    dispatch->enumerate_swapchain_images=Fixture::enumerate;dispatch->acquire_swapchain_image=Fixture::acquire;
    dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;
    dispatch->convert_time_to_qpc=Fixture::to_qpc;
    dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult {
        *out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
    };
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->graphics_binding=SessionGraphicsBinding::d3d12;state->independent_pacing=true;state->vdxr_legacy=false;
    state->graphics_binding_capabilities|=8ULL; // DI2's native synthesis binding validates AER guide placement.
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    D3D12_COMMAND_QUEUE_DESC queue_info{};queue_info.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"fallback synthesis queue");
    state->private_delivery_queue=f.delivery_queue;
    ok(apply_embedded_control(state),"apply initial Hybrid settings");
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;g_instances[dispatch->instance]=dispatch;}
    ok(install_private_runtime(state),"fallback native endpoint");
    f.transport=state->private_runtime;xr_ok(f.transport->start(),"start fallback endpoint");
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);

    constexpr UINT width=256,height=128;
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=width;info.height=height;info.arraySize=2;
    info.faceCount=info.mipCount=info.sampleCount=1;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
    XrSwapchain source{};xr_ok(layer_create_swapchain_impl(state->handle,&info,&source),"paired stereo source");
    f.create_state=[]{return D3D12_RESOURCE_STATE_COMMON;};
    std::array<XrSwapchainImageD3D12KHR,3> images;
    for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    unsigned count{};
    xr_ok(layer_enumerate_swapchain_images_impl(source,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"stereo source images");
    f.release_callback=[&](XrSwapchain handle,unsigned index)->XrResult {
        const auto chain=find_swapchain(handle);
        {std::scoped_lock lock(chain->mutex);chain->acquired_indices.push_back(index);++chain->waited_count;}
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        return layer_release_swapchain_image_impl(handle,&release);
    };
    const auto guide_texture=[&](DXGI_FORMAT format) {
        auto desc=images[0].texture->GetDesc();desc.Format=format;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&texture)),"native guide texture");
        return texture;
    };
    auto depth=guide_texture(DXGI_FORMAT_R32_FLOAT),motion=guide_texture(DXGI_FORMAT_R16G16_FLOAT);
    xrfg::configure_dlss_motion_vector_tracking(true);
    D3D12_DESCRIPTOR_HEAP_DESC heap_info{};heap_info.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap_info.NumDescriptors=2;
    ComPtr<ID3D12DescriptorHeap> guide_views;
    ok(f.device12->CreateDescriptorHeap(&heap_info,IID_PPV_ARGS(&guide_views)),"native guide RTVs");
    const auto depth_view=guide_views->GetCPUDescriptorHandleForHeapStart();auto motion_view=depth_view;
    motion_view.ptr+=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    f.device12->CreateRenderTargetView(depth.Get(),nullptr,depth_view);f.device12->CreateRenderTargetView(motion.Get(),nullptr,motion_view);
    f.commands([&](ID3D12GraphicsCommandList* list) {
        const float z[]{.5F,.5F,.5F,1},v[]{0,0,0,0};
        list->ClearRenderTargetView(depth_view,z,0,nullptr);list->ClearRenderTargetView(motion_view,v,0,nullptr);
        for(auto* texture:{depth.Get(),motion.Get()}) {
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
            list->ResourceBarrier(1,&barrier);
        }
        xrfg::DlssMotionVectorPublication publication;
        publication.stream=1063;publication.output=images[0].texture;
        publication.motion_vectors=motion.Get();publication.depth=depth.Get();
        publication.producer_queue=f.queue.Get();publication.producer_command_list=list;
        publication.output_width=publication.motion_width=publication.depth_width=width;
        publication.output_height=publication.motion_height=publication.depth_height=height;
        publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        check(bool(xrfg::publish_dlss_motion_vectors(publication)),"activate Hybrid with a real native guide publication");
    });
    bool warning_seen=false;
    for(unsigned frame=1;frame<=180;++frame) {
        if(frame==3 || frame==174) {
            settings=xrfg::autonomous::snapshot().desired;settings.ofr_enabled=frame==3;
            check(xrfg::autonomous::request(settings),"live manual AER toggle");
        }
        f.write(source,30+frame);
        const auto chain=find_swapchain(source);
        auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
        for(unsigned eye=0;eye<2;++eye) {
            auto guide=std::make_shared<xrfg::DlssMotionVectorFrame>();
            guide->stream=eye+1;guide->epoch=1;guide->serial=frame;guide->previous_serial=frame-1;
            guide->publication=2*frame+eye;guide->producer_queue=f.queue;
            guide->output=images[*chain->last_released_index].texture;guide->output_slice=eye;
            guide->depth=depth;guide->motion_vectors=motion;guide->motion_slice=eye;
            guide->output_width=settings.ofr_enabled?2*width:width;
            guide->depth_width=guide->motion_width=width;
            guide->output_height=guide->depth_height=guide->motion_height=height;
            guides->eyes[eye]=std::move(guide);
        }
        {std::scoped_lock lock(chain->mutex);chain->last_released_motion_vectors=guides;}
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        xr_ok(layer_wait_frame_impl(state->handle,nullptr,&prediction),"live stereo Wait");
        xr_ok(layer_begin_frame_impl(state->handle,nullptr),"live stereo Begin");
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye) {
            auto& view=views[eye];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.pose.orientation.w=1;view.pose.position.x=.1F*frame;view.fov={-.7F,.7F,.7F,-.7F};
            view.subImage={source,{{0,0},{width,height}},eye};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        xr_ok(layer_end_frame_impl(state->handle,&end),"live stereo End");
        xr_ok(wait_for_synthesis_handoff(state),"complete stereo source and recovery copy");
        if(continuous_presenter_active(state))
            xr_ok(wait_for_presenter_idle(state),"complete stereo native submission");
        xr_ok(f.transport->drain(),"drain stereo output");
        check(!f.pixels.empty() && f.pixels.back()==30+frame && f.right_pixels.back()==30+frame,
            "fresh pixels from both eyes must reach VR throughout the AER toggle");
        check(f.positions.back()==.1F*frame,"fallback uses its own source pose");
        const auto status=xrfg::autonomous::snapshot();
        check(state->nvidia_options.hybrid,"exercise the Hybrid native-guide path throughout the live toggle");
        if(frame>=3 && frame<174) {
            check(status.desired.ofr_enabled && state->nvidia_options.ofr,"recovery preserves manual AER");
            warning_seen|=status.runtime.aer_input_warning;
        }
        if(frame>=174)check(!status.runtime.aer_input_warning,"manual AER off clears the warning");
    }
    check(warning_seen,"three-second warning must reach the live runtime while VR continues");
    xrfg::bridge_flight_logger().flush();
    std::ifstream log(xrfg::bridge_flight_logger().log_path());
    const std::string records((std::istreambuf_iterator<char>(log)),{});
    check(records.find("op=synthesis_prime result=-2147024664")!=std::string::npos,
        "exercise the same stale-guide HRESULT as the DI2 freeze");
    check(!xrfg::embedded::ofr_last_success_ms.load(),"raw recovery must not claim successful AER generation");
    stop_continuous_presenter(state);xr_ok(f.transport->drain(),"final fallback drain");
    f.release_callback={};xr_ok(layer_destroy_swapchain_impl(source),"stereo source cleanup");
    f.transport->stop();
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,false);
    xrfg::bridge_flight_logger().shutdown();
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
    std::puts("DI2 paired stereo, live Hybrid AER toggle, stale guides, fresh GPU pixels and delayed warning PASS");
    return 0;
}
