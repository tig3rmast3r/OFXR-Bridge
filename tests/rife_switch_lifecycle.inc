// Small actual CUDA/D3D12 inference; no headset or owner profile. Each switch
// must release both eye providers before a new worker/context can be created.
void test_rife_switch_lifecycle(D3D12WarpFixture& fixture) {
    auto a=create_source_texture(fixture,256,128),b=create_source_texture(fixture,256,128);
    const auto first=translated_motion_pattern(256,128,{0,0}),last=translated_motion_pattern(256,128,{4,4});
    upload_pattern(fixture,a.Get(),first);upload_pattern(fixture,b.Get(),last);
    std::array<ComPtr<ID3D12Resource>,2> current,synthetic;
    std::array<std::shared_ptr<xrfg::D3D12SwapchainHistory>,2> histories;
    std::array<xrfg::D3D12FrameSynthesizer,2> providers;
    std::array<xrfg::D3D12FrameSynthesisTicket,2> receipts;
    auto options=xrfg::D3D12NvidiaOpticalFlowOptions{};options.rife_model=2;options.rife_async=true;
    options.rife_resolution_percent=18;
    for(unsigned eye=0;eye<2;++eye) {
        current[eye]=create_source_texture(fixture,256,128);synthetic[eye]=create_source_texture(fixture,256,128);
        ID3D12Resource* inputs[]{a.Get(),b.Get()},*c[]{current[eye].Get()},*s[]{synthetic[eye].Get()};
        histories[eye]=std::make_shared<xrfg::D3D12SwapchainHistory>();
        require_hresult(histories[eye]->initialize(fixture.device(),fixture.queue(),inputs,D3D12_RESOURCE_STATE_RENDER_TARGET),"switch history");
        require_hresult(providers[eye].initialize(fixture.device(),fixture.queue(),histories[eye],c,s,kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,options,false,
            xrfg::D3D12FrameGenerationMode::ofxr,false,true),"deferred RIFE switch provider");
    }
    const auto luid=fixture.device()->GetAdapterLuid();const auto views=make_reprojection_views();
    for(unsigned epoch=0;epoch<8;++epoch) {
        options.rife_resolution_percent=epoch%2?24:18;
        for(auto& provider:providers) {
            require_hresult(provider.reconfigure(xrfg::D3D12OpticalFlowBackend::nvidia,options),"reconfigure suspended RIFE");
            require(!provider.asynchronous_rife_enabled(),"suspended reconfiguration created an unused RIFE worker");
        }
        require(!xrfg::rife_memory_for_adapter(luid,false).active,"an old RIFE worker survived full provider suspension");
        for(unsigned eye=0;eye<2;++eye) {
            xrfg::D3D12HistoryCaptureTicket capture;
            require_hresult(histories[eye]->capture(0,&capture),"switch prime capture");
            require_hresult(histories[eye]->commit(capture),"switch prime commit");
            require_hresult(providers[eye].synchronize_producer_queue(fixture.queue()),"lazy RIFE exact producer queue");
            require(providers[eye].asynchronous_rife_enabled() && providers[eye].submission_queue().Get()!=fixture.queue(),
                "lazy RIFE upgrade did not establish its private queue before producer synchronization");
            require_hresult(providers[eye].submit_prime(capture,views,0,&receipts[eye]),"switch prime");
            require_hresult(providers[eye].wait_for_previous_submission(30000),"switch prime completion");
            require_hresult(histories[eye]->capture(1,&capture),"switch pair capture");
            require_hresult(histories[eye]->commit(capture),"switch pair commit");
            require_hresult(providers[eye].submit_pair(capture,views,views,0,0,&receipts[eye]),"switch actual RIFE inference");
            require_hresult(providers[eye].wait_for_previous_submission(30000),"switch actual inference completion");
            require(readback_pattern(fixture,current[eye].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==last,
                "switch changed the real source endpoint");
        }
        for(auto& provider:providers)require_hresult(provider.retire_previous(),"retire every eye before suspending a shared worker");
        for(auto& provider:providers)require_hresult(provider.wait_for_idle(),"drain every eye before suspension");
        for(unsigned eye=0;eye<2;++eye) {
            require_hresult(providers[eye].suspend_flow_resources(),"suspend RIFE working resources");
            require(!providers[eye].asynchronous_rife_enabled() && providers[eye].submission_queue().Get()==fixture.queue(),
                "RIFE suspension recreated the provider instead of returning to copy transport");
            const auto completed=providers[eye].submission_fence(receipts[eye]);
            require(completed && completed->GetCompletedValue()>=receipts[eye].fence_value,
                "suspension lost an already completed publication receipt");
        }
        require(!xrfg::rife_memory_for_adapter(luid,false).active,"last suspended eye retained CUDA process/context/graphs");
        std::cout<<"RIFE_SWITCH epoch="<<epoch<<" worker_released=1 real_endpoints=exact\n";
    }
}
