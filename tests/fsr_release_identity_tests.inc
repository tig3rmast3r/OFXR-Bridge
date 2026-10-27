// Actual layer release + guide refresh with owned WARP snapshots. No fake
// resolver or generator is substituted. First discovery deliberately is RIGHT.
void fsr_release_identity_test() {
    using Microsoft::WRL::ComPtr;
    const auto original_settings = xrfg::embedded::snapshot().desired;
    auto s = session(); s->presenter_active = false;
    s->graphics_binding = SessionGraphicsBinding::d3d12;
    s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
    s->dlss_motion_vectors = true; s->menu_enabled = true;
    check(strict_fsr_release_guides(*s), "ordinary native FSR opts into exact guides");
    s->graphics_binding = SessionGraphicsBinding::d3d11;
    check(!strict_fsr_release_guides(*s), "D11 receipt path preserved");
    s->graphics_binding = SessionGraphicsBinding::d3d12; s->luke_afw_enabled = true;
    check(!strict_fsr_release_guides(*s), "Luke/AFW path preserved"); s->luke_afw_enabled = false;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> adapter;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "FSR WARP adapter");
    check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&s->d3d12_device))), "FSR WARP device");
    D3D12_COMMAND_QUEUE_DESC queue{};
    check(SUCCEEDED(s->d3d12_device->CreateCommandQueue(&queue,
        IID_PPV_ARGS(&s->d3d12_queue))), "FSR producer queue");
    const auto texture = [&] {
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = 32; d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> r;
        check(SUCCEEDED(s->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
            &d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r))), "FSR fixture image");
        return r;
    };
    auto sr_color = texture(), motion = texture(), depth = texture();
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    s->dispatch->release_swapchain_image = [](XrSwapchain,const XrSwapchainImageReleaseInfo*) -> XrResult {return XR_SUCCESS;};
    for (unsigned eye=0;eye<2;++eye) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.width=info.height=32;info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;
        info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
        auto chain=std::make_shared<SwapchainState>(s,info);
        chain->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x4640+eye));
        chain->enumerated_d3d12_images.push_back(texture()); chain->enumerated_image_count=1;
        chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* image=chain->enumerated_d3d12_images[0].Get();
        check(SUCCEEDED(chain->d3d12_history->initialize(s->d3d12_device.Get(),s->d3d12_queue.Get(),
            std::span<ID3D12Resource* const>(&image,1),D3D12_RESOURCE_STATE_COMMON)), "FSR owned color history");
        chains[eye]=chain; std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;
    }
    struct Cleanup {
        std::array<std::shared_ptr<SwapchainState>,2>& chains;
        xrfg::embedded::Settings previous;
        ~Cleanup() {
            for(auto& chain:chains) if(chain && chain->d3d12_history)
                static_cast<void>(chain->d3d12_history->wait_for_idle());
            xrfg::configure_dlss_motion_vector_tracking(false);
            xrfg::retire_disabled_dlss_motion_vector_resources();
            static_cast<void>(xrfg_fixture::request_control(previous));
            std::scoped_lock lock(g_state_mutex);
            for(auto& chain:chains) if(chain) g_swapchains.erase(chain->handle);
        }
    } cleanup{chains,original_settings};
    ProjectionSnapshot projection;projection.layers.resize(1);projection.layers[0].views.resize(2);
    for(unsigned eye=0;eye<2;++eye) {
        auto& v=projection.layers[0].views[eye];v.pose.orientation.w=1;
        v.subImage.swapchain=chains[eye]->handle;v.subImage.imageRect.extent={32,32};
    }
    auto mappings=build_projection_resource_mappings(projection);
    check(mappings.ready(), "FSR stereo mappings");note_projection_eyes(projection,mappings);
    xrfg::configure_dlss_motion_vector_tracking(true);
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(s->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(s->d3d12_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))) &&
        SUCCEEDED(list->Close()) && SUCCEEDED(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))), "FSR recording context");
    std::uint64_t cursor{};
    const auto publish=[&](unsigned eye,bool reset=false) {
        check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr)), "FSR producer reset");
        xrfg::DlssMotionVectorPublication p{};p.stream=eye?46411:46422;
        p.output=sr_color.Get();p.motion_vectors=motion.Get();p.depth=depth.Get();
        p.output_width=p.output_height=p.motion_width=p.motion_height=p.depth_width=p.depth_height=32;
        p.producer_queue=s->d3d12_queue.Get();p.producer_command_list=list.Get();
        p.resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
        p.jitter_x=eye?.75F:.25F;p.jitter_y=.5F;p.reset=reset;
        auto frame=xrfg::publish_dlss_motion_vectors(p);check(bool(frame), "FSR native publication");
        check(SUCCEEDED(list->Close()), "FSR producer close");ID3D12CommandList* lists[]={list.Get()};
        s->d3d12_queue->ExecuteCommandLists(1,lists);
        xrfg::report_dlss_motion_vector_submission(s->d3d12_queue.Get(),1,lists);
        check(SUCCEEDED(s->d3d12_queue->Signal(fence.Get(),++cursor)) &&
            SUCCEEDED(wait_composition_fence(fence.Get(),cursor)), "FSR producer completion");
        return frame;
    };
    const auto release=[&](unsigned eye) {
        auto& chain=chains[eye];chain->acquired_indices.push_back(0);chain->waited_count=1;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&info)), "actual FSR release");
        check(chain->last_released_capture.has_value(), "FSR color captured at release");
    };
    const auto refresh=[&] {refresh_release_bound_projection_guides(*s,ProjectionSnapshot{},mappings);};
    const auto verify=[&] {
        for(unsigned eye=0;eye<2;++eye) {
            auto g=chains[eye]->last_released_motion_vectors;
            check(g && g->eye_count==1 && g->eyes[0]->stream==(eye?46411:46422), "FSR never follows discovery order");
            check(g->eyes[0]->jitter_x==(eye?.75F:.25F), "FSR uses this eye's exact jitter snapshot");
            check(g->eyes[0]->publication<=chains[eye]->release_publication &&
                g->eyes[0]->producer_fence && g->eyes[0]->producer_fence_value, "FSR keeps exact release/submission ownership");
        }
    };
    publish(1);publish(0);release(0);release(1);refresh();
    check(!chains[0]->last_released_motion_vectors && !chains[1]->last_released_motion_vectors,
        "ambiguous both-before-release startup must bypass");
    // The old path has a concrete wrong-eye result on the same snapshots.
    auto legacy=xrfg::resolve_dlss_motion_vectors(chains[0]->enumerated_d3d12_images[0].Get(),s->d3d12_queue.Get(),0);
    check(legacy && legacy->eyes[0]->stream==46411, "baseline reproduces right guides assigned to left");
    publish(1);release(1);publish(0);release(0);refresh();verify();
    publish(1);publish(0);release(0);release(1);refresh();verify();
    const auto left_identity=chains[0]->fsr_eye_identity;
    for (int mode : {2,1}) {
        auto desired=xrfg::embedded::snapshot().desired;desired.enabled=false;desired.frame_generation=mode;desired.motion_vectors=1;
        check(xrfg_fixture::request_control(desired), "real generator switch request");
        check(SUCCEEDED(apply_embedded_control(s)), "real generator switch application");
    }
    check(chains[0]->fsr_eye_identity.stream==left_identity.stream, "route switches retain verified stream identity");
    // The route switch deliberately paused the real canonical controls. Resume
    // through that same API: changing only the private session bool would leave
    // the owned-guide capture gate correctly disabled by the global OFF state.
    auto resumed=xrfg::embedded::snapshot().desired;
    resumed.enabled=true;resumed.frame_generation=1;resumed.hybrid=false;resumed.motion_vectors=1;
    check(xrfg_fixture::request_control(resumed),"resume actual canonical FSR controls");
    check(SUCCEEDED(apply_embedded_control(s)),"apply actual FSR resume after paused route switch");
    // Owned temporal snapshots were retired while OFF. The first exact
    // right/left releases establish the resumed epoch before both-before-release
    // ordering can use the learned identities again.
    publish(1);release(1);publish(0);release(0);refresh();verify();
    publish(1);publish(0);release(0);release(1);refresh();verify();
    publish(1);publish(0,true);release(0);release(1);refresh();
    check(!chains[0]->last_released_motion_vectors, "reset epoch cannot reuse learned eye identity");
    publish(1);release(1);publish(0);release(0);refresh();verify();
    std::swap(projection.layers[0].views[0].subImage,projection.layers[0].views[1].subImage);
    mappings=build_projection_resource_mappings(projection);note_projection_eyes(projection,mappings);
    check(chains[0]->fsr_eye_identity.stream==0 && chains[1]->fsr_eye_identity.stream==0, "changed eye layout clears learned identity");
    for(auto& chain:chains) check(SUCCEEDED(chain->d3d12_history->wait_for_idle()), "FSR final history drain");
    xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();
    {std::scoped_lock lock(g_state_mutex);for(auto& chain:chains)g_swapchains.erase(chain->handle);}
    std::cout<<"FSR actual layer: wrong-eye baseline reproduced; right-first releases corrected; learned both-before-release; native switches; reset/layout rejection; exact GPU fences PASS\n";
}
