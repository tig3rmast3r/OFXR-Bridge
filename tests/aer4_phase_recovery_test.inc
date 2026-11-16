// Native consumer remains held; the source phase may drain ready work without
// waiting for the independent generator or dropping/reordering accepted images.
static int aer4_phase_recovery_test(unsigned factor=2,bool aer_double=true,unsigned ready_count=2,bool gpu_pending=true) {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;settings.enabled=true;
    check(xrfg::autonomous::request(settings),"phase recovery controls");xrfg::autonomous::apply_runtime_settings();
    const auto prior=independent_fixture_mode;independent_fixture_mode=true;
    Fixture f(true,true,{},false,true,false,8);f.throttled=false;
    const auto chain=f.make_chain();f.write(chain,80);
    for(unsigned i=1;i<=ready_count;++i){f.begin_virtual();xr_ok(f.submit(chain,float(i)),"fill exact ready/native outputs");if(i==1)f.wait_entered();}
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->menu_enabled=state->presenter_active=state->independent_pacing=true;state->private_runtime=f.transport;
    state->independent_completion_busy=gpu_pending; // one independent, unfinished GPU batch
    const auto origin=ns();state->independent_source_clock.claim(origin,period,factor);
    const auto before=f.transport->independent_load().pending;
    std::unique_lock lock(state->frame_call_mutex);
    const auto entered=ns();const auto result=recover_independent_aer4_source_phase(*state,factor,period,aer_double,lock);
    const auto left=ns();const auto next=state->independent_source_clock.claim(left,period,factor);
    const auto after=f.transport->independent_load().pending;lock.unlock();
    const bool producer_free=state->independent_completion_busy==gpu_pending; // helper did not need GPU completion
    state->independent_completion_busy=false;
    f.unblock();f.wait_count(ready_count);xr_ok(f.transport->drain(),"phase recovery preserves accepted FIFO");
    xr_ok(f.transport->destroy_swapchain(chain),"phase recovery chain cleanup");f.transport->stop();independent_fixture_mode=prior;
    xr_ok(result,"source phase recovery status");
    const bool should_recover=aer_double && ready_count>=2 && gpu_pending;
    const bool phase=should_recover?left-entered>=period/2 && next>=origin+factor*period+period/2:next==origin+factor*period;
    std::printf("AER4_PHASE factor=%u active=%u shifted=%u pending_before=%zu after=%zu generator_independent=%u\n",factor,aer_double,phase,before,after,producer_free);std::fflush(stdout);
    check(phase && producer_free && before==ready_count && after==ready_count && f.positions==(ready_count==2?std::vector<float>({1,2}):std::vector<float>({1})),
        "AER4 excess queue must rephase before render, preserving GPU independence, exact FIFO and unchanged2X");
    return 0;
}
