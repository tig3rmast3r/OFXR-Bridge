// Real WARP command lists and readback: an unused output must retain its
// sentinel while the presented synthetic remains byte-identical to control.
void test_aer_unused_current() {
    D3D12WarpFixture fixture;
    const auto a=make_solid_pattern({RgbaBytes{20,40,60,255},RgbaBytes{40,60,80,255}});
    const auto b=make_solid_pattern({RgbaBytes{100,120,140,255},RgbaBytes{120,140,160,255}});
    const auto sentinel=make_solid_pattern({RgbaBytes{9,19,29,255},RgbaBytes{39,49,59,255}});
    const auto run=[&](bool unused,unsigned mask,bool aer,bool held,bool capture=false) {
        auto& active=xrfg::manager().active_frame;
        struct RestoreCapture {std::atomic_uint32_t& active;std::uint32_t before;~RestoreCapture(){active=before;}} restore{active,active.exchange(capture?1:0)};
        auto input_a=create_source_texture(fixture),input_b=create_source_texture(fixture);
        auto real=create_source_texture(fixture),synthetic=create_source_texture(fixture);
        upload_pattern(fixture,input_a.Get(),a);upload_pattern(fixture,input_b.Get(),b);
        std::array<ID3D12Resource*,2> inputs{input_a.Get(),input_b.Get()};
        ID3D12Resource* reals[]{real.Get()},*synthetics[]{synthetic.Get()};
        auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        require_hresult(history->initialize(fixture.device(),fixture.queue(),inputs,D3D12_RESOURCE_STATE_RENDER_TARGET),"unused history");
        xrfg::D3D12NvidiaOpticalFlowOptions options;options.ofr=aer;
        xrfg::D3D12FrameSynthesizer synth;
        require_hresult(synth.initialize(fixture.device(),fixture.queue(),history,reals,synthetics,kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::fidelity_fx,options),"unused generator");
        auto views=make_reprojection_views();for(unsigned eye=0;eye<2;++eye)views[eye].logical_eye=eye;
        xrfg::D3D12HistoryCaptureTicket ca,cb;xrfg::D3D12FrameSynthesisTicket ticket;
        require_hresult(history->capture(0,&ca),"unused capture A");require_hresult(history->commit(ca),"unused commit A");
        require_hresult(synth.submit_prime(ca,views,0,&ticket),"unused prime");
        require_hresult(synth.wait_for_previous_submission(30000),"unused prime complete");
        require(readback_pattern(fixture,real.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==a,"prime real copy must remain");
        require_hresult(history->capture(1,&cb),"unused capture B");require_hresult(history->commit(cb),"unused commit B");
        if(held) {
            require_hresult(synth.submit_pair(cb,views,views,0,0,&ticket,{}, {},false,.5F,{},std::nullopt,false,mask),"unused held anchor");
            require_hresult(synth.wait_for_previous_submission(30000),"unused held anchor complete");
        }
        upload_pattern(fixture,real.Get(),sentinel);
        require_hresult(synth.submit_pair(cb,views,views,0,0,&ticket,{}, {},false,.5F,{},std::nullopt,false,mask,unused),"unused pair");
        require_hresult(synth.wait_for_previous_submission(30000),"unused pair complete");
        const auto actual=readback_pattern(fixture,real.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto generated=readback_pattern(fixture,synthetic.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        if(unused && aer && mask!=3 && !capture)require(actual==sentinel,"unpresented AER current output was still written");
        else require(actual!=sentinel,"presented current output lost its write");
        require(ticket.held_capture==held,"unused write changed history identity");
        require_hresult(synth.wait_for_idle(),"unused pair drain");
        return std::pair{actual,generated};
    };
    for(unsigned mask:{1u,2u})for(bool held:{false,true}) {
        const auto control=run(false,mask,true,held),optimized=run(true,mask,true,held);
        require(control.second==optimized.second,"omitting current write changed presented stereo pixels");
    }
    for(bool aer:{false,true}) {
        const auto control=run(false,3,aer,false),requested=run(true,3,aer,false);
        require(control==requested,"ordinary stereo/paired AER changed with unused request");
    }
    for(bool held:{false,true}) {
        const auto control=run(false,1,true,held,true),requested=run(true,1,true,held,true);
        require(control==requested,"F8 capture lost the full current output");
    }
    fixture.require_no_debug_errors();
    std::cout<<"AER_UNUSED real sentinel untouched; fresh/held synthetic pixels identical; prime/stereo/paired/F8 current preserved PASS\n";
}
