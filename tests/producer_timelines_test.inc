// Force two producer queues to signal in the reverse order from CPU submission.
// The generation queue stays gated until the rewind is observable. All rescue
// signals below are test-only and happen after BOTH producers really completed.
static int producer_timelines_test(bool delayed_second=false) {
    xrfg_fixture::ControlProfile profile;
    Fixture f(true);f.unblock();
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> slow,fast,generator;
    for(auto* q:{std::addressof(slow),std::addressof(fast),std::addressof(generator)})ok(f.device12->CreateCommandQueue(&desc,IID_PPV_ARGS(q->GetAddressOf())),"producer timeline queue");
    ComPtr<ID3D12Fence> slow_gate,generator_gate,slow_done,fast_done,future_gate;
    for(auto* fence:{std::addressof(slow_gate),std::addressof(generator_gate),std::addressof(slow_done),std::addressof(fast_done),std::addressof(future_gate)})
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence->GetAddressOf())),"producer timeline fence");
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=texture.Height=64;texture.DepthOrArraySize=texture.MipLevels=1;
    texture.SampleDesc.Count=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto input=IndependentAerDouble::resource(f.device12.Get(),texture);
    auto current=IndependentAerDouble::resource(f.device12.Get(),texture);
    auto synthetic=IndependentAerDouble::resource(f.device12.Get(),texture);
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();ID3D12Resource* inputs[]{input.Get()};
    ok(history->initialize(f.device12.Get(),f.queue.Get(),inputs,D3D12_RESOURCE_STATE_COMMON),"timeline history initialize");
    xrfg::D3D12HistoryCaptureTicket captured;ok(history->capture(0,&captured),"timeline capture");ok(history->commit(captured),"timeline capture commit");
    ok(history->wait_for_idle(),"only acquire receipts remain pending in this experiment");
    xrfg::D3D12FrameSynthesizer synth;ID3D12Resource* real[]{current.Get()},*generated[]{synthetic.Get()};
    ok(synth.initialize(f.device12.Get(),generator.Get(),history,real,generated,texture.Format,
        D3D12_RESOURCE_STATE_COMMON,xrfg::D3D12OpticalFlowBackend::fidelity_fx),"timeline generator initialize");
    ok(generator->Wait(generator_gate.Get(),1),"hold only consumer of producer receipts");
    ok(slow->Wait(slow_gate.Get(),1),"delay the first producer signal");
    ok(synth.synchronize_producer_queue(delayed_second?fast.Get():slow.Get()),"first producer receipt");
    ok(synth.synchronize_producer_queue(delayed_second?slow.Get():fast.Get()),"second producer receipt");
    ok(fast->Signal(fast_done.Get(),1),"fast producer sentinel");
    ok(IndependentAerDouble::wait(fast_done.Get(),1),"fast producer completed before first");
    xrfg::D3D12ReprojectionView view;view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    view.image_rect={0,0,64,64};view.array_slice=0;view.logical_eye=0;
    xrfg::D3D12FrameSynthesisTicket ticket;
    ok(synth.submit_prime(captured,std::span(&view,1),0,&ticket),"capture immutable pair input proofs");
    const bool falsely_ready=ticket.inputs.ready(); // first producer is still held
    ok(slow->Signal(slow_done.Get(),1),"slow producer sentinel follows its receipt");
    ok(slow_gate->Signal(1),"release first producer after the second completed");
    ok(IndependentAerDouble::wait(slow_done.Get(),1),"both producers actually complete");
    const bool ready_after_both=ticket.inputs.ready();
    std::vector<ID3D12Fence*> proofs;
    for(std::size_t i=2;i<ticket.inputs.dependencies.size();++i) {
        const auto& p=ticket.inputs.dependencies[i];if(p.value && p.fence)proofs.push_back(p.fence.Get());
    }
    const bool separate=proofs.size()==2 && proofs[0]!=proofs[1];
    // Old code otherwise deadlocks on a value overwritten by the slower queue.
    // Every producer is now proved complete; rescue only the synthetic fixture.
    for(const auto& p:ticket.inputs.dependencies)if(p.fence && p.value && p.fence->GetCompletedValue()<p.value)
        ok(p.fence->Signal(p.value),"negative-control test-only rewind rescue");
    ok(generator_gate->Signal(1),"release generation queue");
    auto completion=synth.submission_fence(ticket);ok(IndependentAerDouble::wait(completion.Get(),ticket.fence_value),"generation completes after both receipts");
    // Updating a producer's next receipt cannot mutate the old transaction.
    ok(slow->Wait(future_gate.Get(),1),"hold next unrelated producer work");
    ok(synth.synchronize_producer_queue(slow.Get()),"next receipt on existing timeline");
    const bool old_still_ready=ticket.inputs.ready();
    ok(future_gate->Signal(1),"release fixture's later work before teardown");
    ok(synth.wait_for_idle(),"timeline idle proof");
    std::printf("PRODUCER_TIMELINES early_ready=%u both_ready=%u separate=%u immutable=%u delayed_second=%u\n",falsely_ready,ready_after_both,separate,old_still_ready,delayed_second);
    std::fflush(stdout);
    check(!falsely_ready && ready_after_both && separate && old_still_ready,
        "independent producer queues must not overwrite or rewind one another's acquire proof");
    return 0;
}
