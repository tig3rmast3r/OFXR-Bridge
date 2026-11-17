static int private_completion_recovery_test(std::string_view mode) {
    xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();f.throttled=false;
    auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
    dispatch->wait_frame=private_runtime_wait;dispatch->begin_frame=private_runtime_begin;dispatch->end_frame=private_runtime_end;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
    state->nvidia_options.rife_multiplier=2;state->nvidia_options.rife_model=mode=="worker-rife"?3:-1;
    state->optical_flow_backend=mode=="worker-fidelity"?xrfg::D3D12OpticalFlowBackend::fidelity_fx:xrfg::D3D12OpticalFlowBackend::nvidia;
    ComPtr<ID3D12Fence> gate,completion;
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"late completion gate");
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion)),"late completion fence");
    // Real GPU queue fence, not a fake success or a signaled completion fence.
    ok(f.queue->Wait(gate.Get(),1),"hold exact GPU completion");
    ok(f.queue->Signal(completion.Get(),1),"enqueue completion behind fault");
    if(mode=="cancel") {
        bool delayed{};auto waiting=std::async(std::launch::async,[&]{return wait_recoverable_private_fence(state,completion.Get(),1,delayed);});
        const bool retained=waiting.wait_for(1150ms)==std::future_status::timeout;
        {std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;}
        const bool cancelled=waiting.wait_for(250ms)==std::future_status::ready;
        const auto result=cancelled?waiting.get():S_OK;const bool protected_work=completion->GetCompletedValue()==0;
        ok(gate->Signal(1),"release cancelled GPU work safely");if(!cancelled)waiting.get();
        ok(wait_composition_fence(completion.Get(),1,1000),"cancelled work really retires");
        check(retained && cancelled && result==E_ABORT && protected_work && XR_SUCCEEDED(state->presenter_failure),"late wait stop failed or falsely completed GPU work");
        std::puts("Late 2X stop wakes recovery without forging completion or latching failure PASS");return 0;
    }
    if(mode.starts_with("worker-")) {
        PreparedProjectionFrame prepared;prepared.kind=PreparedGenerationKind::pair;
        PreparedGeneration g;g.kind=PreparedGenerationKind::pair;
        g.worker_generation=std::make_shared<FrameGenerationSwapchainState>();g.worker_fence=completion;g.worker_fence_value=1;
        prepared.resources.push_back({reinterpret_cast<XrSwapchain>(7),std::move(g),false});
        bool delayed{};
        auto worker=std::async(std::launch::async,[&]{return finish_worker_outputs(state,prepared,1,&delayed);});
        const bool retained=worker.wait_for(1150ms)==std::future_status::timeout;
        const bool pending=completion->GetCompletedValue()==0;
        ok(gate->Signal(1),"allow exact GPU completion");
        const bool recovered=worker.get();
        std::printf("LATE_WORKER %.*s retained=%u pending=%u recovered=%u delayed=%u failure=%d\n",int(mode.size()),mode.data(),retained,pending,recovered,delayed,int(state->presenter_failure));
        check(retained && pending,"one-second timeout prematurely abandoned the 2X transaction");
        check(recovered && delayed && XR_SUCCEEDED(state->presenter_failure),"late completion latched worker failure");
        // A drained provider transition must not inherit failure. Switch model
        // and multiplier only after the protected transaction has completed.
        state->nvidia_options.rife_model=state->nvidia_options.rife_model<0?3:-1;
        state->nvidia_options.rife_multiplier=4;bool next_delayed{};
        check(finish_worker_outputs(state,prepared,2,&next_delayed) && !next_delayed,"drained provider/multiplier handoff inherits failure");
        return 0;
    }
    // Warm source before placing the artificial dependency on its queue.
    ok(gate->Signal(1),"release setup dependency");ok(wait_composition_fence(completion.Get(),1,1000),"setup drains");
    const auto chain=f.make_chain();f.write(chain,77);
    ok(f.queue->Wait(gate.Get(),2),"hold presenter readiness");ok(f.queue->Signal(completion.Get(),2),"exact readiness receipt");
    const auto frame=[&](float pose) {
        auto result=std::make_shared<GeneratedFrameEndInfo>();result->projections.emplace_back();auto& p=result->projections.back();
        p.layer.viewCount=1;p.views.resize(1);auto& v=p.views.front();v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        v.pose.orientation.w=1;v.pose.position.x=pose;v.subImage={chain,{{0,0},{1,1}},0};p.layer.views=p.views.data();
        result->layer_pointers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p.layer));
        result->info.layerCount=1;result->info.layers=result->layer_pointers.data();result->info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        return result;
    };
    auto first=std::make_shared<PresenterSubmission>();first->owned_frame=frame(3);first->sequence=1;
    first->owned_frame->reference_readiness.push_back({completion,2,false});
    state->presenter_submissions.push_back(first);state->outstanding_presenter_submissions=1;state->presenter_active=true;state->presenter_display_period=period;
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;}
    std::thread consumer([&]{three_slot_director_main(state);});
    bool retained{};
    {std::unique_lock lock(state->presenter_mutex);retained=!state->presenter_condition.wait_for(lock,1150ms,[&]{return first->completed || XR_FAILED(state->presenter_failure);});}
    const bool pending=completion->GetCompletedValue()<2;
    if(mode=="presenter-stop") {
        {std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;}
        state->presenter_condition.notify_all();consumer.join();
        const bool safe=completion->GetCompletedValue()<2 && f.real_ends==0 && XR_SUCCEEDED(state->presenter_failure);
        ok(gate->Signal(2),"retire stopped presenter work");ok(wait_composition_fence(completion.Get(),2,1000),"stopped presenter completion");
        {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
        check(retained && pending && safe,"presenter stop poisoned session or submitted unfinished image");
        std::puts("Late presenter stop preserves pending work and clean failure state PASS");return 0;
    }
    ok(gate->Signal(2),"complete presenter GPU work");
    bool recovered{};
    {std::unique_lock lock(state->presenter_mutex);recovered=state->presenter_condition.wait_for(lock,3s,[&]{return first->completed;}) && XR_SUCCEEDED(first->result);}
    bool second_ok{};
    if(recovered) {
        f.wait_count(1);xr_ok(f.transport->drain(),"late frame physically retired");f.write(chain,94);
        auto second=std::make_shared<PresenterSubmission>();second->owned_frame=frame(7);second->sequence=2;
        {std::scoped_lock lock(state->presenter_mutex);state->nvidia_options.rife_model=3;state->presenter_submissions.push_back(second);++state->outstanding_presenter_submissions;}
        state->presenter_condition.notify_all();
        {std::unique_lock lock(state->presenter_mutex);second_ok=state->presenter_condition.wait_for(lock,3s,[&]{return second->completed;}) && XR_SUCCEEDED(second->result);}
        if(second_ok){f.wait_count(2);xr_ok(f.transport->drain(),"next provider receipt physically retired");}
    }
    {std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;}
    state->presenter_condition.notify_all();consumer.join();
    {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
    std::printf("LATE_PRESENTER retained=%u pending=%u recovered=%u next=%u failure=%d outputs=%u\n",retained,pending,recovered,second_ok,int(state->presenter_failure),f.real_ends.load());
    check(retained && pending,"presenter readiness timeout latched failure before GPU completion");
    check(recovered && second_ok && XR_SUCCEEDED(state->presenter_failure),"late presenter/provider continuation failed");
    check(f.pixels==std::vector<unsigned>({77,94}) && f.positions==std::vector<float>({3,7}),"late recovery lost/repeated pixels or poses");
    return 0;
}

// Alternate actual completed GPU faults with ordinary presentation of already
// ready images. Empty readiness must not erase a real worker pressure streak.
static int private_pressure_sampling_test() {
    xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();f.throttled=false;
    auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
    dispatch->wait_frame=private_runtime_wait;dispatch->begin_frame=private_runtime_begin;dispatch->end_frame=private_runtime_end;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->nvidia_options.rife_multiplier=4;state->nvidia_options.rife_model=-1;
    state->history_capture_wait_period=period;state->presenter_display_period=period;state->presenter_active=true;
    // Independent uses its own asynchronous completion/admission policy.
    state->independent_pacing=true;
    for(unsigned n=0;n<8;++n)observe_private_completion(*state,150000,true);
    check(!private_completion_recovery_enabled(*state) && !optical_pressure_recovery_enabled(*state) &&
        !state->resource_recovery.requested && !state->resource_recovery.active,
        "late/slow completed work armed legacy resource recovery on Independent");
    state->independent_pacing=false;
    const auto chain=f.make_chain();
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;}
    std::thread consumer([&]{three_slot_director_main(state);});
    bool completed=true;
    for(unsigned n=1;n<=6 && completed;++n) {
        ComPtr<ID3D12Fence> gate,done;ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"sample gate");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"sample completion");
        ok(f.queue->Wait(gate.Get(),1),"sample real GPU hold");ok(f.queue->Signal(done.Get(),1),"sample GPU receipt");
        PreparedProjectionFrame prepared;prepared.kind=PreparedGenerationKind::pair;PreparedGeneration g;g.kind=prepared.kind;
        g.worker_generation=std::make_shared<FrameGenerationSwapchainState>();g.worker_fence=done;g.worker_fence_value=1;
        prepared.resources.push_back({chain,std::move(g),false});
        auto work=std::async(std::launch::async,[&]{return finish_worker_outputs(state,prepared,n);});
        const bool held=work.wait_for(60ms)==std::future_status::timeout;ok(gate->Signal(1),"retire sample GPU hold");
        completed=work.get() && held;
        if(!completed)break;
        f.write(chain,n*10);
        auto frame=std::make_shared<GeneratedFrameEndInfo>();frame->synthetic=true;frame->projections.emplace_back();
        auto& p=frame->projections.back();p.layer.viewCount=1;p.views.resize(1);auto& v=p.views.front();
        v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.subImage={chain,{{0,0},{1,1}},0};p.layer.views=p.views.data();
        frame->layer_pointers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p.layer));
        frame->info.layerCount=1;frame->info.layers=frame->layer_pointers.data();frame->info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        auto request=std::make_shared<PresenterSubmission>();request->owned_frame=frame;request->sequence=n;
        {std::scoped_lock lock(state->presenter_mutex);state->presenter_submissions.push_back(request);++state->outstanding_presenter_submissions;}
        state->presenter_condition.notify_all();
        {std::unique_lock lock(state->presenter_mutex);completed=state->presenter_condition.wait_for(lock,2s,[&]{return request->completed;}) && XR_SUCCEEDED(request->result);}
        if(completed)xr_ok(f.transport->drain(),"already-ready sample physically retires");
    }
    bool requested{};{std::scoped_lock lock(state->optical_pressure_mutex);requested=state->resource_recovery.requested;}
    {std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;}
    state->presenter_condition.notify_all();consumer.join();
    {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
    check(completed && requested && f.real_ends==6,"presentation bookkeeping erased actual completed worker pressure");
    std::puts("Actual GPU pressure survives empty presentation-readiness observations PASS");return 0;
}
