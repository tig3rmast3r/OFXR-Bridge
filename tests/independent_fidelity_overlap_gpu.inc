// Exact slot-ownership reproduction: hold pair B on the GPU, then submit C
// using a different existing work slot/destination. Compare to a drained run.
static std::vector<StereoPattern> fidelity_overlap_run(D3D12WarpFixture& f,unsigned factor,bool hybrid,bool overlap) {
    std::array<ComPtr<ID3D12Resource>,3> source;
    std::array<ComPtr<ID3D12Resource>,2> current;
    std::array<ComPtr<ID3D12Resource>,4> synthetic;
    std::array<ID3D12Resource*,3> sp{};std::array<ID3D12Resource*,2> cp{};std::array<ID3D12Resource*,4> yp{};
    for(unsigned i=0;i<3;++i){source[i]=create_source_texture(f);sp[i]=source[i].Get();upload_pattern(f,sp[i],make_pattern(17+40*i));}
    for(unsigned i=0;i<2;++i){current[i]=create_source_texture(f);cp[i]=current[i].Get();}
    for(unsigned i=0;i<4;++i){synthetic[i]=create_source_texture(f);yp[i]=synthetic[i].Get();}
    auto motion=create_and_upload_game_motion_field(f,kWidth,kHeight,[](UINT,UINT,UINT){return std::array<float,2>{0,0};});
    const auto guides=[&](unsigned serial) {
        if(!hybrid)return std::shared_ptr<const xrfg::DlssMotionVectorSet>{};
        auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=kEyeCount;
        for(unsigned eye=0;eye<kEyeCount;++eye){auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();
            g->stream=311+eye;g->epoch=3;g->serial=serial;g->previous_serial=serial-1;
            g->motion_vectors=motion;g->producer_queue=f.queue();g->output_width=g->motion_width=kWidth;
            g->output_height=g->motion_height=kHeight;g->motion_slice=g->output_slice=eye;
            g->resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;set->eyes[eye]=g;}
        return std::shared_ptr<const xrfg::DlssMotionVectorSet>(set);
    };
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(f.device(),f.queue(),sp,D3D12_RESOURCE_STATE_RENDER_TARGET),"overlap history");
    xrfg::D3D12NvidiaOpticalFlowOptions options;options.hybrid=hybrid;
    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(f.device(),f.queue(),history,cp,yp,kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,options),"overlap synthesizer");
    auto views=make_reprojection_views();std::array<xrfg::D3D12HistoryCaptureTicket,3> captures;
    require_hresult(history->capture(0,&captures[0]),"overlap A");require_hresult(history->commit(captures[0]),"commit A");
    xrfg::D3D12FrameSynthesisTicket prime;
    require_hresult(synth.submit_prime(captures[0],views,0,&prime,guides(1)),"overlap prime");
    require_hresult(synth.wait_for_previous_submission(3000),"overlap prime completion");
    for(unsigned i=1;i<3;++i){require_hresult(history->capture(i,&captures[i]),"capture next source");require_hresult(history->commit(captures[i]),"commit next source");}
    f.execute_and_wait([](ID3D12GraphicsCommandList*){});
    ComPtr<ID3D12Fence> gate;require_hresult(f.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"overlap gate");
    if(overlap)require_hresult(f.queue()->Wait(gate.Get(),1),"hold B GPU completion");
    std::array<xrfg::D3D12FrameSynthesisTicket,2> tickets;std::array<HRESULT,2> result{};
    for(unsigned pair=0;pair<2;++pair){
        auto moved_views=views;
        for(auto& view:moved_views){view.pose.orientation.y=.02F*(pair+1);
            view.pose.orientation.w=std::sqrt(1-view.pose.orientation.y*view.pose.orientation.y);}
        std::optional<xrfg::D3D12ExtraSynthetic> extra;
        if(factor==3){extra.emplace();extra->destination_index=pair*2+1;extra->interpolation_fraction=2.f/3.f;}
        result[pair]=synth.submit_pair(captures[pair+1],moved_views,moved_views,pair*2,pair,&tickets[pair],
            std::nullopt,guides(pair+2),false,1.f/factor,{},extra,false,3,false,false,factor==2,overlap);
        if(!overlap)require_hresult(synth.wait_for_previous_submission(3000),"sequential control completion");
    }
    const bool gpu_still_pending=synth.submission_fence(prime)->GetCompletedValue()<tickets[0].fence_value;
    require_hresult(gate->Signal(1),"release overlap gate before assertions");
    require_hresult(synth.wait_for_previous_submission(3000),"queued pairs drain");
    require_hresult(result[0],"submit B");require_hresult(result[1],"free FidelityFX slot must accept C before B completes");
    if(overlap)require(gpu_still_pending,"overlap test did not hold the previous GPU submission");
    if(hybrid)require(tickets[0].used_game_motion && tickets[1].used_game_motion,"test silently bypassed guided Hybrid");
    std::vector<StereoPattern> output;
    for(unsigned i=0;i<2;++i){require(readback_pattern(f,cp[i],D3D12_RESOURCE_STATE_RENDER_TARGET)==make_pattern(57+40*i),"overlap altered exact real pixels");
        for(unsigned j=0;j<factor-1;++j)output.push_back(readback_pattern(f,yp[i*2+j],D3D12_RESOURCE_STATE_RENDER_TARGET));}
    require_hresult(synth.wait_for_idle(),"overlap retirement");f.require_no_debug_errors();return output;
}
static void test_independent_fidelity_overlap(D3D12WarpFixture& f) {
    for(unsigned factor:{2U,3U})for(bool hybrid:{false,true}) {
        const auto control=fidelity_overlap_run(f,factor,hybrid,false);
        const auto queued=fidelity_overlap_run(f,factor,hybrid,true);
        require(queued==control,"queued FidelityFX/Hybrid pixels differ from sequential execution");
        std::printf("FIDELITY_OVERLAP factor=%u hybrid=%u moving-camera exact synthetic/real pixels, pending prior GPU, existing-slot reuse PASS\n",factor,hybrid);
    }
}
