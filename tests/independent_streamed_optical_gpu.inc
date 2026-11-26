// Prove a pixel boundary, not merely an earlier CPU receipt: while the
// already-submitted tail is GPU-gated, both S1 eyes exist and S2/B stay sentinel.
static void test_independent_streamed_optical(D3D12WarpFixture& f) {
    const auto control=fidelity_overlap_run(f,3,true,false);
    std::array<ComPtr<ID3D12Resource>,2> source,synthetic;
    std::array<ID3D12Resource*,2> sp{},yp{};
    for(unsigned i=0;i<2;++i){source[i]=create_source_texture(f);sp[i]=source[i].Get();upload_pattern(f,sp[i],make_pattern(17+40*i));
        synthetic[i]=create_source_texture(f);yp[i]=synthetic[i].Get();upload_pattern(f,yp[i],make_pattern(203));}
    auto current=create_source_texture(f);ID3D12Resource* cp[]={current.Get()};
    auto motion=create_and_upload_game_motion_field(f,kWidth,kHeight,[](UINT,UINT,UINT){return std::array<float,2>{0,0};});
    const auto guides=[&](unsigned serial) {
        auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=kEyeCount;
        for(unsigned eye=0;eye<kEyeCount;++eye){auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=311+eye;g->epoch=3;
            g->serial=serial;g->previous_serial=serial-1;g->motion_vectors=motion;g->producer_queue=f.queue();
            g->output_width=g->motion_width=kWidth;g->output_height=g->motion_height=kHeight;g->output_slice=g->motion_slice=eye;
            g->resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;set->eyes[eye]=g;}
        return set;
    };
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(f.device(),f.queue(),sp,D3D12_RESOURCE_STATE_RENDER_TARGET),"stream history");
    ComPtr<ID3D12CommandQueue> queue;auto* proxy=new QueueWithReportedDeviceAlias(f.queue(),f.device());queue.Attach(proxy);
    xrfg::D3D12NvidiaOpticalFlowOptions options;options.hybrid=true;xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(f.device(),queue.Get(),history,cp,yp,kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,options),"stream engine");
    const auto views=make_reprojection_views();xrfg::D3D12HistoryCaptureTicket ca{},cb{};
    require_hresult(history->capture(0,&ca),"stream A");require_hresult(history->commit(ca),"commit stream A");
    xrfg::D3D12FrameSynthesisTicket prime;require_hresult(synth.submit_prime(ca,views,0,&prime,guides(1)),"stream prime");
    require_hresult(synth.wait_for_previous_submission(3000),"stream prime completion");upload_pattern(f,current.Get(),make_pattern(203));
    require_hresult(history->capture(1,&cb),"stream B");require_hresult(history->commit(cb),"commit stream B");
    ComPtr<ID3D12Fence> gate;require_hresult(f.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"tail-only GPU gate");
    struct ReleaseGate {ID3D12Fence* gate;~ReleaseGate(){gate->Signal(1);}} release{gate.Get()};
    auto fence=synth.submission_fence(prime);bool gated{};unsigned executions{};
    proxy->after_execute=[&]{++executions;};
    proxy->after_signal=[&](ID3D12Fence* signaled,UINT64){if(signaled==fence.Get()&&!gated){gated=true;require_hresult(f.queue()->Wait(gate.Get(),1),"hold only later S2/B work");}};
    auto moved=views;for(auto& v:moved){v.pose.orientation.y=.02F;v.pose.orientation.w=std::sqrt(1-.02F*.02F);}
    xrfg::D3D12ExtraSynthetic extra;extra.destination_index=1;extra.interpolation_fraction=2.f/3.f;
    xrfg::D3D12FrameSynthesisTicket pair;
    require_hresult(synth.submit_pair(cb,moved,moved,0,0,&pair,std::nullopt,guides(2),true,1.f/3.f,{},extra,false,3,false,false,true,true),"stream 3X pair");
    require_hresult(synth.flush_current_copy(),"immediately submit existing tail, without waiting for presentation");
    proxy->after_signal={};proxy->after_execute={};
    f.wait_for_fence(fence.Get(),pair.synthetic_fence_value);
    const bool pending=fence->GetCompletedValue()<pair.fence_value;
    const auto reuse=synth.wait_for_previous_submission(0);
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> observe;
    require_hresult(f.device()->CreateCommandQueue(&q,IID_PPV_ARGS(&observe)),"observe exact S1 pixels independently of the held tail");
    const auto first=readback_pattern(f,yp[0],D3D12_RESOURCE_STATE_RENDER_TARGET,observe.Get());
    const auto second_before=readback_pattern(f,yp[1],D3D12_RESOURCE_STATE_RENDER_TARGET,observe.Get());
    const auto real_before=readback_pattern(f,current.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,observe.Get());
    require_hresult(gate->Signal(1),"complete queued S2/B tail");require_hresult(synth.wait_for_previous_submission(3000),"drain full streamed source");
    require(gated && executions>=2 && pending && reuse==HRESULT_FROM_WIN32(ERROR_BUSY),"streamed S1 must not retire full-slot ownership");
    require(second_before==make_pattern(203) && real_before==make_pattern(203),"first synthetic receipt still includes later synthetic draws");
    require(first==control[0] && readback_pattern(f,yp[1],D3D12_RESOURCE_STATE_RENDER_TARGET)==control[1],"streamed moving-camera Hybrid pixels differ from combined execution");
    require(pair.used_game_motion && readback_pattern(f,current.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==make_pattern(57),"streamed guides/real pixels invalid");
    require_hresult(synth.wait_for_idle(),"streamed source lifetime retirement");f.require_no_debug_errors();
    // Separate eye swapchains can use the same captured motion texture. Queue
    // both first stages before either tail, as the shipping worker does.
    for(unsigned factor:{2U,3U}) {
        const auto expected=factor==3?control:fidelity_overlap_run(f,2,true,false);
        std::array<xrfg::D3D12FrameSynthesizer,2> engines;
        std::array<std::shared_ptr<xrfg::D3D12SwapchainHistory>,2> eye_history;
        std::array<xrfg::D3D12HistoryCaptureTicket,2> eye_a,eye_b;
        std::array<ComPtr<ID3D12Resource>,2> real;
        std::array<std::array<ComPtr<ID3D12Resource>,2>,2> generated;
        std::array<xrfg::D3D12FrameSynthesisTicket,2> tickets;
        for(unsigned eye=0;eye<2;++eye) {
            eye_history[eye]=std::make_shared<xrfg::D3D12SwapchainHistory>();
            require_hresult(eye_history[eye]->initialize(f.device(),f.queue(),sp,D3D12_RESOURCE_STATE_RENDER_TARGET),"separate eye history");
            require_hresult(eye_history[eye]->capture(0,&eye_a[eye]),"separate eye A");require_hresult(eye_history[eye]->commit(eye_a[eye]),"commit separate A");
            require_hresult(eye_history[eye]->capture(1,&eye_b[eye]),"separate eye B");require_hresult(eye_history[eye]->commit(eye_b[eye]),"commit separate B");
            real[eye]=create_source_texture(f);ID3D12Resource* rp[]={real[eye].Get()};
            std::array<ID3D12Resource*,2> gp{};
            for(unsigned i=0;i<2;++i){generated[eye][i]=create_source_texture(f);gp[i]=generated[eye][i].Get();}
            require_hresult(engines[eye].initialize(f.device(),f.queue(),eye_history[eye],rp,gp,kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,
                xrfg::D3D12OpticalFlowBackend::fidelity_fx,options),"shared-guide eye engine");
            xrfg::D3D12FrameSynthesisTicket first;
            require_hresult(engines[eye].submit_prime(eye_a[eye],views,0,&first,guides(1)),"shared-guide prime");
            require_hresult(engines[eye].wait_for_previous_submission(3000),"shared-guide prime drain");
        }
        for(unsigned eye=0;eye<2;++eye) {
            require_hresult(engines[eye].submit_pair(eye_b[eye],moved,moved,0,0,&tickets[eye],std::nullopt,guides(2),true,1.f/factor,{},
                factor==3?std::optional<xrfg::D3D12ExtraSynthetic>{extra}:std::nullopt,false,3,false,false,true,true),"shared-guide first stages");
        }
        for(auto& engine:engines)require_hresult(engine.flush_current_copy(),"submit both eye tails after both first stages");
        for(unsigned eye=0;eye<2;++eye) {
            require_hresult(engines[eye].wait_for_previous_submission(3000),"shared-guide tails drain");
            for(unsigned output=0;output<factor-1;++output)
                require(readback_pattern(f,generated[eye][output].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==expected[output],"interleaving eyes altered guided pixels");
            require(readback_pattern(f,real[eye].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==make_pattern(57),"interleaving eyes altered real pixels");
            require_hresult(engines[eye].wait_for_idle(),"shared-guide retirement");
        }
        f.require_no_debug_errors();
    }
    std::puts("STREAMED_OPTICAL_3X both S1 eyes pixel-complete before GPU-held S2/B, immediate tail submission, exact moving-camera control and full-fence reuse PASS");
}
