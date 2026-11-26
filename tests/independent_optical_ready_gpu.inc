
// Gate the real copy on the actual shipping execution queue. S must complete
// while B is GPU-pending, yet B must already be submitted without a flush.
static void test_independent_optical_ready(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>,3> sources,current,synthetic;
    std::array<ID3D12Resource*,3> source_ptrs{},current_ptrs{},synthetic_ptrs{};
    const auto a=make_pattern(17),b=make_pattern(71),c=make_pattern(119),sentinel=make_pattern(203);
    for(unsigned i=0;i<3;++i) {
        sources[i]=create_source_texture(fixture);current[i]=create_source_texture(fixture);synthetic[i]=create_source_texture(fixture);
        source_ptrs[i]=sources[i].Get();current_ptrs[i]=current[i].Get();synthetic_ptrs[i]=synthetic[i].Get();
        upload_pattern(fixture,sources[i].Get(),i==0?a:i==1?b:c);
        upload_pattern(fixture,current[i].Get(),sentinel);upload_pattern(fixture,synthetic[i].Get(),sentinel);
    }
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),source_ptrs,D3D12_RESOURCE_STATE_RENDER_TARGET),"early S history");
    ComPtr<ID3D12CommandQueue> queue;
    auto* proxy=new QueueWithReportedDeviceAlias(fixture.queue(),fixture.device());queue.Attach(proxy);
    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(fixture.device(),queue.Get(),history,current_ptrs,synthetic_ptrs,kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET),"early S engine");
    const auto views=make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket ca{},cb{},cc{};
    require_hresult(history->capture(0,&ca),"capture early A");require_hresult(history->commit(ca),"commit early A");
    xrfg::D3D12FrameSynthesisTicket prime;
    require_hresult(synth.submit_prime(ca,views,0,&prime),"prime early engine");
    require_hresult(synth.wait_for_previous_submission(3000),"drain prime");
    auto fence=synth.submission_fence(prime);
    ComPtr<ID3D12Fence> gate;require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"copy gate");
    require_hresult(history->capture(1,&cb),"capture early B");require_hresult(history->commit(cb),"commit early B");
    bool gated{};unsigned executions{};
    proxy->after_execute=[&]{++executions;};
    proxy->after_signal=[&](ID3D12Fence* f,UINT64) {
        if(f==fence.Get()&&!gated) {gated=true;require_hresult(fixture.queue()->Wait(gate.Get(),1),"gate only following real copy");}
    };
    xrfg::D3D12FrameSynthesisTicket pair;
    const auto submitted=synth.submit_pair(cb,views,views,1,1,&pair,std::nullopt,{},false,.5f,{},std::nullopt,false,3,false,false,true);
    HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(ready!=nullptr,"early S event");
    require_hresult(fence->SetEventOnCompletion(pair.synthetic_fence_value,ready),"observe exact S fence");
    const bool s_done=WaitForSingleObject(ready,1000)==WAIT_OBJECT_0;CloseHandle(ready);
    const bool separate=pair.synthetic_fence_value && pair.synthetic_fence_value<pair.fence_value;
    const bool real_pending=fence->GetCompletedValue()<pair.fence_value;
    const auto reuse=synth.wait_for_previous_submission(0);
    xrfg::D3D12HistoryConsumerLease lease;ID3D12Resource* resource{};ID3D12Fence* producer{};
    const auto history_busy=history->acquire_consumer(ca,&lease,&resource,&producer);
    if(SUCCEEDED(history_busy)){history->cancel_consumer(lease);if(resource)resource->Release();if(producer)producer->Release();}
    proxy->after_signal={};proxy->after_execute={};
    // Release every artificial dependency before an assertion can unwind GPU resources.
    require_hresult(gate->Signal(1),"release only the real copy");
    require_hresult(synth.wait_for_previous_submission(3000),"B already queued and completes without flush");
    require_hresult(submitted,"submit early stereo pair");
    require(gated && executions>=2 && s_done && separate && real_pending,
        "2X S must signal separately while its immediately submitted B copy remains pending");
    require(reuse==HRESULT_FROM_WIN32(ERROR_BUSY) && history_busy==HRESULT_FROM_WIN32(ERROR_BUSY),
        "early S must not authorize command allocator or source-history reuse before B");
    require(readback_pattern(fixture,current[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==b,"immediate B copy changed pixels");
    require(readback_pattern(fixture,synthetic[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)!=sentinel,"S was not generated");
    require_hresult(history->capture(2,&cc),"capture C after full retirement");require_hresult(history->commit(cc),"commit C");
    xrfg::D3D12FrameSynthesisTicket control;
    require_hresult(synth.submit_pair(cc,views,views,2,2,&control),"default pair remains unchanged");
    require(control.synthetic_fence_value==control.fence_value,"unrequested early readiness changed the default path");
    require_hresult(synth.wait_for_idle(),"control drains");
    require_hresult(synth.retire_previous(),"early anchor retirement");require_hresult(history->wait_for_idle(),"early history drains");
    fixture.require_no_debug_errors();
    std::cout<<"Independent 2X actual FidelityFX: S before gated B, B submitted immediately, exact real pixels, full-fence reuse and unchanged default PASS\n";
}
