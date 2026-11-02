// Actual layer quarantine/bypass plus a real D3D12 temporal history lease.
void quarantine_history_recovery_test() {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> device;
    check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"quarantine DXGI");
    check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),"quarantine WARP");
    check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"quarantine device");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"quarantine queue");
    std::array<ComPtr<ID3D12Resource>,3> images;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=64;
    desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for(auto& image:images)check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
        &desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image))),"quarantine resource");
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->handle=reinterpret_cast<XrSession>(0x745);
    state->d3d12_device=device;state->d3d12_queue=queue;
    auto chain=std::make_shared<SwapchainState>(state,XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO});chain->handle=reinterpret_cast<XrSwapchain>(0x7451);chain->session=state;
    auto generation=std::make_shared<FrameGenerationSwapchainState>();chain->frame_generation=generation;
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    ID3D12Resource* sources[]{images[0].Get()},*current[]{images[1].Get()},*synthetic[]{images[2].Get()};
    check(SUCCEEDED(history->initialize(device.Get(),queue.Get(),sources,D3D12_RESOURCE_STATE_COMMON)),"quarantine history");
    auto synth=std::make_shared<xrfg::D3D12FrameSynthesizer>();generation->synthesizer=synth;
    check(SUCCEEDED(synth->initialize(device.Get(),queue.Get(),history,current,synthetic,desc.Format,
        D3D12_RESOURCE_STATE_COMMON,xrfg::D3D12OpticalFlowBackend::fidelity_fx,{},false,
        xrfg::D3D12FrameGenerationMode::ofxr,false,true)),"quarantine deferred synthesizer");
    xrfg::D3D12ReprojectionView view{};view.fov={-.7F,.7F,.7F,-.7F};view.image_rect={0,0,64,64};
    {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;g_sessions[state->handle]=state;}
    struct Unregister {XrSwapchain h;~Unregister(){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(h);g_sessions.erase(reinterpret_cast<XrSession>(0x745));}} unregister{chain->handle};
    for(unsigned n=0;n<8;++n) {
        xrfg::D3D12HistoryCaptureTicket capture;
        check(SUCCEEDED(history->capture(0,&capture))&&SUCCEEDED(history->commit(capture)),"quarantine fresh capture");
        xrfg::D3D12FrameSynthesisTicket ticket;
        check(SUCCEEDED(synth->submit_prime(capture,{&view,1},0,&ticket)),
            "quarantine bypass left the old GPU anchor retained; fresh prime rejected");
        check(SUCCEEDED(wait_composition_fence(synth->submission_fence(ticket).Get(),ticket.fence_value,1000)),"quarantine prime completion");
        state->previous_projection=ProjectionSnapshot{};
        if(n%2==0) {
            schedule_generation_quarantine(state,GenerationQuarantineReason::generation_prepare_failed,12);
            check(!state->previous_projection,"quarantine CPU metadata reset");
        } else {
            XrfgPresentationTransformV1 transform{};transform.version=XRFG_PRESENTATION_TRANSFORM_VERSION_1;
            transform.active=state->presentation_transform.active?0:1;transform.view_count=1;
            check(layer_set_presentation_transform_ofxr(state->handle,&transform)==XR_SUCCESS,"cinema domain boundary");
        }
        check(state->generation_continuity_reset_pending.load(),"transition did not request retained GPU history reset");
        clear_bypassed_generation_continuity(state);
        check(!state->generation_continuity_reset_pending.load(),"history reset request did not complete");
        XrfgPresentationTransformV1 repeated{};repeated.version=XRFG_PRESENTATION_TRANSFORM_VERSION_1;
        repeated.active=state->presentation_transform.active;repeated.view_count=1;
        check(layer_set_presentation_transform_ofxr(state->handle,&repeated)==XR_SUCCESS &&
            !state->generation_continuity_reset_pending.load(),"unchanged presentation requested a per-frame drain");
    }
    check(SUCCEEDED(synth->wait_for_idle())&&SUCCEEDED(history->wait_for_idle()),"quarantine drain");
    std::cout<<"Eight quarantine/cinema/bypass cycles retire actual GPU temporal anchors and accept fresh priming\n";
}
