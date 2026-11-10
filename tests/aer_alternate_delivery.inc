// Actual shipping layer, D11 split-eye ingress, interop synthesizers, private
// admission and native mock consumer. Held application polls publish nothing.
int aer_alternate_delivery_test(std::string_view route) {
    const bool directed=route.find("director")!=std::string_view::npos;
    const bool buffered=route.find("buffer")!=std::string_view::npos;
    const bool fast=route.find("fast")!=std::string_view::npos;
    const bool missing_guides=route.find("missing-guides")!=std::string_view::npos;
    const bool static_projection=route.find("static-")!=std::string_view::npos;
    const bool static_first=route.find("static-first")!=std::string_view::npos;
    const bool native12=route.find("d12")!=std::string_view::npos;
    constexpr unsigned sources=18;
    constexpr float step=.002F,ipd=.032F;
    xrfg_fixture::ControlProfile profile;
    auto desired=xrfg::autonomous::snapshot().desired;
    // The missing-guide mode selects real FSR, with no mocked DLSS producer.
    // It must preserve raw input and report waiting, never invent valid guides.
    xrfg::autonomous::select_generation(desired,missing_guides?1:0,0);
    desired.enabled=true;desired.advanced_mode=true;desired.ofr_enabled=true;
    desired.afw_mode=0;desired.nr_enabled=false;desired.r19_camera=false;
    desired.pacing_mode=directed?1:0;desired.increase_buffer=buffered;
    desired.separate_synthesis=desired.lower_vram=fast?1:0;
    check(xrfg::autonomous::request(desired),"AER matrix selects actual public settings");
    xrfg::autonomous::apply_runtime_settings();
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Fixture f(native12);f.transport->stop();f.transport.reset();f.throttled=false;
    f.real_common=true;f.expected_views=2;f.unblock();
    f.extra_projection=static_projection;f.scene_projection_layer=static_projection&&static_first?1:0;
    f.create_state=[]{return D3D12_RESOURCE_STATE_COMMON;};
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
    state->graphics_binding=native12?SessionGraphicsBinding::d3d12:SessionGraphicsBinding::d3d11;
    state->vdxr_legacy=false; // model the actual SteamVR session, not default Legacy routing
    if(!native12)ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),f.device12.GetAddressOf(),f.queue.GetAddressOf()),"AER D11 interop device");
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->d3d11_device=f.device11;state->d3d11_context=f.context;
    if(native12) {
        D3D12_COMMAND_QUEUE_DESC queue_info{};queue_info.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"native session synthesis queue");
        state->private_delivery_queue=f.delivery_queue;
    }
    state->luke_afw_enabled=!native12;if(!native12)state->graphics_binding_capabilities|=8ULL;
    if(!native12)initialize_luke_synthesis_queue(*state);
    ok(apply_embedded_control(state),"AER matrix initial provider settings");
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;g_instances[dispatch->instance]=dispatch;}
    ok(install_private_runtime(state),"AER matrix real private native endpoint");
    f.transport=state->private_runtime;
    check(f.transport && f.transport->director_enabled()==directed,"matrix must use selected native policy");
    xr_ok(f.transport->start(),"AER matrix start endpoint");
    state->fps_overlay=std::make_unique<xrfg::OpenXrFpsOverlay>(dispatch->instance,state->handle,0,
        state->dispatch->get_instance_proc_addr,content_end_frame,f.device12.Get(),f.queue.Get(),f.device11.Get(),
        xrfg::autonomous::configuration_path(),false);
    state->fps_overlay->set_session_focused(false);
    bind_private_delivery_metrics(state);
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=256;info.height=128;
    info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    const auto create_source=[&](const XrSwapchainCreateInfo& desc) {
        XrSwapchain chain{};
        f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
        xr_ok(layer_create_swapchain_impl(state->handle,&desc,&chain),"AER application chain");
        f.create_state=[]{return D3D12_RESOURCE_STATE_COMMON;};
        unsigned count{};
        if(native12) {
            std::array<XrSwapchainImageD3D12KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"AER D12 capture setup");
        } else {
            std::array<XrSwapchainImageD3D11KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"AER D11 capture setup");
        }
        return chain;
    };
    std::array<XrSwapchain,2> app{create_source(info),create_source(info)};
    XrSwapchain fixed{};
    if(static_projection) {
        auto immutable=info;immutable.width=immutable.height=4;
        immutable.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
        fixed=create_source(immutable);
    }
    f.release_callback=[&](XrSwapchain handle,unsigned index)->XrResult {
        auto chain=find_swapchain(handle);check(bool(chain),"AER release source exists");
        {std::scoped_lock lock(chain->mutex);chain->acquired_indices.push_back(index);++chain->waited_count;}
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        return layer_release_swapchain_image_impl(handle,&release);
    };
    std::vector<std::array<float,2>> poses;
    f.inspect_layers=[&](const XrFrameEndInfo* end) {
        check(end && end->layerCount==(static_projection?2u:1u),"AER output preserves all projection layers");
        if(static_projection) {
            const auto* fixed_layer=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[static_first?0:1]);
            check(fixed_layer->viewCount==2,"static projection keeps both subimages");
            for(unsigned eye=0;eye<2;++eye) {
                const auto& sub=fixed_layer->views[eye].subImage;
                check(sub.imageRect.offset.x==int(eye*2) && sub.imageRect.extent.width==2 && sub.imageRect.extent.height==4,"static projection geometry preserved");
                check(f.read_pixel(f.chains.at(sub.swapchain),sub.imageRect.offset.x,0)==77,"AER static projection pixels preserved");
            }
        }
        const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[f.scene_projection_layer]);
        check(p->viewCount==2,"AER output keeps both logical eyes");
        poses.push_back({p->views[0].pose.position.x,p->views[1].pose.position.x});
    };
    const auto submit=[&](float time) {
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        xr_ok(layer_wait_frame_impl(state->handle,nullptr,&prediction),"AER source Wait");
        xr_ok(layer_begin_frame_impl(state->handle,nullptr),"AER source Begin");
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye) {
            auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
            v.pose.position.x=time+(eye?ipd:-ipd);v.fov={-.7F,.7F,.7F,-.7F};
            v.subImage.swapchain=app[eye];v.subImage.imageRect.extent={int(info.width),int(info.height)};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        auto fixed_views=views;
        for(unsigned eye=0;eye<2;++eye)fixed_views[eye].subImage={fixed,{{int(eye*2),0},{2,4}},0};
        auto fixed_projection=projection;fixed_projection.views=fixed_views.data();
        const auto* fixed_layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&fixed_projection);
        const XrCompositionLayerBaseHeader* layers[]={static_first?fixed_layer:layer,static_first?layer:fixed_layer};
        if(static_projection){end.layerCount=2;end.layers=layers;}
        xr_ok(layer_end_frame_impl(state->handle,&end),"AER source End");
    };
    if(static_projection)f.write(fixed,77);
    f.write(app[0],40);f.write(app[1],184);
    for(unsigned source=0;source<sources;++source) {
        if(source)f.write(app[(source-1)%2],source%2?40:184);
        submit(float(source)*step);
        f.wait_count(source+1);xr_ok(f.transport->drain(),"AER exact fresh output retirement");
        check(f.real_ends==source+1,"AER must deliver one output per fresh source, never a raw second endpoint");
        check(f.pixels.back()==40 && f.right_pixels.back()==184,"AER mixed output swapped or corrupted its eye pixels");
        if(source>=4 && !missing_guides) {
            const float time=float(source-1)*step;
            check(std::abs(poses.back()[0]-(time-ipd))<.0001F &&
                  std::abs(poses.back()[1]-(time+ipd))<.0001F,
                "AER output must be temporally coherent; raw fallback/held-camera update is forbidden after warmup");
        }
        if(source>=4) {
            if(missing_guides)check(state->generation_status==xrfg::autonomous::GenerationState::missing_motion,
                "AER native method without real guides must report waiting for motion");
            // A Luke loop may poll several times without a released new eye.
            // Those virtual cycles must not leak raw AER into the headset or
            // discard the generator anchor needed for the next fresh eye.
            for(unsigned repeat=1;repeat<=2;++repeat) {
                submit((float(source)+float(repeat)*.25F)*step);
                xr_ok(f.transport->drain(),"AER held poll retirement");
                check(f.real_ends==source+1,"held application poll must not emit another native/raw frame");
            }
        }
    }
    check(poses.size()==sources && f.real_ends==sources,"AER matrix retains exact fresh output count");
    if(missing_guides)check(!xrfg::embedded::ofr_last_success_ms.load(),"missing native guides must not claim successful AER generation");
    else {
        check(state->fps_overlay->metrics().aer_eye_equivalents,"AER actual output metrics must retain mixed-eye identity");
        check(xrfg::embedded::ofr_last_success_ms.load()!=0,"AER matrix actually generated coherent outputs");
    }
    stop_continuous_presenter(state);
    xr_ok(f.transport->drain(),"AER matrix final native drain");
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,false);
    state->fps_overlay.reset();f.release_callback={};
    for(auto chain:app)xr_ok(layer_destroy_swapchain_impl(chain),"AER matrix original chain cleanup");
    if(static_projection)xr_ok(layer_destroy_swapchain_impl(fixed),"immutable projection cleanup");
    f.transport->stop();
    ComPtr<ID3D12InfoQueue> messages;
    if(SUCCEEDED(f.device12.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());messages->GetMessage(i,message,&bytes);
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) {
            std::fprintf(stderr,"D3D12 AER: %s\n",message->pDescription);check(false,"AER matrix GPU resource ownership");
        }
    }
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
    std::printf("AER %s %s static%u first%u buffer%u storedFast%u missingGuides%u:18fresh outputs,28held polls,exact eye pixels,%s PASS\n",
        native12?"D12":"D11",directed?"Director":"Framework",unsigned(static_projection),unsigned(static_first),unsigned(buffered),unsigned(fast),unsigned(missing_guides),
        missing_guides?"honest waiting/raw fallback,no held duplicates":"coherent poses,no raw fallback");
    return 0;
}
