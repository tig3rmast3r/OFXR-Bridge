// Real D3D12/Vulkan regression: a new graph may take seconds to compile, but
// every frame must retire with a real-image fallback and an honest zero mask.
static void test_rife_vulkan_cold(D3D12WarpFixture& fixture,bool asynchronous,
    unsigned factor,UINT dimension,bool expect_cold,bool cancel=false) {
    const auto began=std::chrono::steady_clock::now();
    unsigned pending_frames=0;double max_ms=0;bool ready=false;
    {
    auto a=create_source_texture(fixture,dimension,dimension),b=create_source_texture(fixture,dimension,dimension);
    std::array<ComPtr<ID3D12Resource>,2> real{create_source_texture(fixture,dimension,dimension),create_source_texture(fixture,dimension,dimension)};
    std::array<ComPtr<ID3D12Resource>,3> generated;
    std::array<ID3D12Resource*,3> outputs{};
    for(unsigned i=0;i<3;++i){generated[i]=create_source_texture(fixture,dimension,dimension);outputs[i]=generated[i].Get();}
    auto pattern=translated_motion_pattern(dimension,dimension,{0,0});
    for(unsigned eye=0;eye<2;++eye)for(size_t i=0;i<pattern[eye].size();++i)pattern[eye][i]=i%4==3?255:70+eye*40;
    upload_pattern(fixture,a.Get(),pattern);upload_pattern(fixture,b.Get(),pattern);
    std::array<ID3D12Resource*,2> inputs{a.Get(),b.Get()},reals{real[0].Get(),real[1].Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),inputs,D3D12_RESOURCE_STATE_RENDER_TARGET),"cold history");
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    require_hresult(fixture.device()->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"cold queue");
    xrfg::D3D12NvidiaOpticalFlowOptions options{};options.rife_model=3;options.rife_multiplier=factor;
    options.rife_async=asynchronous;options.input_scale=xrfg::D3D12NvidiaInputScale::quarter;
    xrfg::D3D12FrameSynthesizer synth;
    require_hresult(synth.initialize(fixture.device(),asynchronous?queue.Get():fixture.queue(),history,reals,outputs,kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::nvidia,options,true),"cold initialize");
    const auto views=make_reprojection_views();xrfg::D3D12HistoryCaptureTicket capture{};xrfg::D3D12FrameSynthesisTicket ticket{};
    require_hresult(history->capture(0,&capture),"cold prime capture");require_hresult(history->commit(capture),"cold prime commit");
    require_hresult(synth.submit_prime(capture,views,0,&ticket),"cold prime");require_frame_start_gate(synth,"cold prime done");
    const auto deadline=GetTickCount64()+90000;
    while(GetTickCount64()<deadline){
        require_frame_start_gate(synth,"cold frame gate");
        require_hresult(history->capture(1,&capture),"cold pair capture");require_hresult(history->commit(capture),"cold pair commit");
        xrfg::D3D12ExtraSynthetic extra{1,2.F/factor,views,factor==4?2U:UINT32_MAX,3.F/factor,views};
        const auto start=std::chrono::steady_clock::now();
        require_hresult(synth.submit_pair(capture,views,views,0,1,&ticket,{}, {},false,1.F/factor,{},factor>2?std::optional(extra):std::nullopt),"cold pair submit");
        require_hresult(synth.wait_for_previous_submission(900),"cold frame blocked behind compilation");
        const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();max_ms=std::max(max_ms,ms);
        require(ms<900,"cold compiler blocked a frame past the runtime deadline");
        const auto mask=ticket.native_result?ticket.native_result->resolve():((1U<<(factor-1))-1U);
        require(readback_pattern(fixture,real[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==pattern,"cold fallback damaged real pixels");
        for(unsigned i=0;i<factor-1;++i){const auto actual=readback_pattern(fixture,generated[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
            for(unsigned eye=0;eye<2;++eye)require(actual[eye]==pattern[eye],"cold fractional output invalid");}
        if(mask){require(mask==((1U<<(factor-1))-1U),"cold partial output mask");ready=true;break;}
        if(!ticket.native_result||ticket.native_result->skip!=6)std::cerr<<"cold skip="<<(ticket.native_result?ticket.native_result->skip:999)<<" detail="<<(ticket.native_result?ticket.native_result->detail:0)<<std::endl;
        require(ticket.native_result&&ticket.native_result->skip==6,"cold frame was latched as a permanent worker failure");
        ++pending_frames;if(cancel)break;Sleep(50);
    }
    require(cancel||ready,"cold plan never resumed generation");
    require(!expect_cold||pending_frames>0,"cold negative control used an already prepared plan");
    // Cached graphs may still load model/pipelines in the background. Pending
    // is valid here too; the runner checks that cached plans were not rebuilt.
    fixture.require_no_debug_errors();
    }
    const double total=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
    if(cancel)require(total<1800,"leaving an unfinished preparation blocked teardown");
    std::cout<<"VULKAN_COLD async="<<asynchronous<<" factor="<<factor<<" dimension="<<dimension
        <<" pending_frames="<<pending_frames<<" max_frame_ms="<<max_ms<<" total_ms="<<total<<" cancelled="<<cancel<<std::endl;
}
