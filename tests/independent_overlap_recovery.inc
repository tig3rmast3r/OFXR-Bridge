static int independent_overlap_recovery_test() {
    Fixture f(true);f.unblock();
    for(unsigned factor:{2U,3U}) {
        SynthesisJob job;job.pair=job.stereo_recovery=true;job.independent_period_ns=period;
        job.current=std::make_shared<ThreeSlotFrame>();
        job.current->queued_at=std::chrono::steady_clock::now();
        auto input=std::make_shared<PanelInput>();input->generation=std::make_shared<FrameGenerationSwapchainState>();
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&input->generation->recovery_prior_fence)),"prior GPU receipt");
        input->generation->recovery_prior_value=1;job.sources.push_back(input);
        bool overlap_only{};
        check(independent_source_backlogged(job,job.current->queued_at,&overlap_only) && overlap_only,
            "normal previous-GPU overlap must be distinguished from queued source age");
        job.recovery_source_queued=true;job.recovery_source_overlap_only=overlap_only;
        GeneratedFrameEndInfo image;assign_independent_output_timing(job,image,0);
        auto timing=image.independent_recovery;
        xrfg::IndependentDeliveryReserve reserve;reserve.set_limit(4*period);
        for(unsigned i=0;i<2;++i) {
            const auto due=1'000'000'000LL+i*factor*period;
            check(reserve.observe_source(due+2*period,due,period,timing),"normal GPU overlap must be observed");
        }
        check(reserve.reserve()==0,"brief GPU overlap transient must not inflate reserve");
        {
            const auto due=1'000'000'000LL+2*factor*period;
            check(reserve.observe_source(due+2*period,due,period,timing),"sustained GPU overlap must be observed");
        }
        check(reserve.reserve()>0 && reserve.reserve()<=4*period,"sustained GPU overlap lateness must earn bounded reserve");
        xrfg::IndependentSourcePhase phase;std::int64_t trim{};
        const auto target=period/2+2'000'000;
        for(unsigned n=0;n<12;++n) {
            const auto now=10'000'000'000LL+(n+3)*factor*period;
            const auto entered=now-factor*period-period/2;
            xrfg::IndependentPairHead head{entered,entered-target-period+trim,
                xrfg::independent_phase_source_due(now,timing)};
            const auto step=phase.observe(now,period,factor,1,0,(n+1)*factor,0,0,false,&head,true);
            check(step>=0 && step<=entered-head.ready-target,"phase correction exceeds completed native surplus");
            trim+=step;
        }
        check(trim==period,"normal GPU overlap suppressed latency-reducing phase recovery");
        check(independent_source_backlogged(job,job.current->queued_at+std::chrono::nanoseconds(period),&overlap_only) && !overlap_only,
            "real CPU source backlog must retain its recovery veto");
        timing.source_overlap_only=overlap_only;
        xrfg::IndependentDeliveryReserve backlog_reserve;backlog_reserve.set_limit(4*period);
        for(unsigned i=0;i<6;++i) {
            const auto due=2'000'000'000LL+i*factor*period;
            check(!backlog_reserve.observe_source(due+2*period,due,period,timing),"real CPU backlog must retain its reserve veto");
        }
        check(backlog_reserve.reserve()==0,"real CPU backlog inflated expiry allowance");
        check(xrfg::independent_phase_source_due(1'000'000'000,timing)==0,"queued-source age authorized phase acquisition");
        std::printf("OVERLAP_RECOVERY factor=%u normal GPU overlap trims one measured slot, ignores brief spikes, learns bounded reserve on sustained lateness; real backlog veto PASS\n",factor);
    }
    // Sustained synchronous D3D11 delivery-copy expirations (where source_ready_at
    // is early but prepared_at repeatedly crosses due+reserve+period/2) must learn
    // bounded reserve after 4 distinct sources without inflating on 2-source stalls,
    // and delivered() must not cut reserve below prepared_at.
    {
        xrfg::IndependentDeliveryReserve sync_reserve;sync_reserve.set_limit(4*period);
        const xrfg::IndependentRecoveryTiming sync_timing{0,true,true,false,false};
        for(unsigned i=0;i<3;++i) {
            const auto due=3'000'000'000LL+i*2*period;
            check(!sync_reserve.observe_delivery_expiry(due+period*3/4,due,period,sync_timing),
                "brief 1-3 synchronous delivery-copy expirations must not inflate reserve");
        }
        check(sync_reserve.reserve()==0,"transient delivery-copy stall inflated reserve");
        {
            const auto due=3'000'000'000LL+3*2*period;
            check(sync_reserve.observe_delivery_expiry(due+period*3/4,due,period,sync_timing),
                "4th sustained synchronous delivery-copy expiration must earn bounded reserve");
        }
        const auto learned=sync_reserve.reserve();
        check(learned==period,"sustained synchronous delivery-copy overrun must earn one period of reserve");
        for(unsigned i=0;i<40;++i) {
            const auto due=4'000'000'000LL+i*period;
            const auto prepared=due+period*3/4; // only period/4 slack below due+learned
            sync_reserve.delivered(prepared,due+sync_reserve.reserve(),due+learned,period,true);
        }
        check(sync_reserve.reserve()==learned,"delivered() must not cut reserve underneath tight prepared_at");
        for(unsigned i=0;i<60;++i) {
            const auto due=5'000'000'000LL+i*period;
            const auto prepared=due-period/4; // comfortable slack >= period/2
            sync_reserve.delivered(prepared,due+sync_reserve.reserve(),due+period/4,period,true);
        }
        check(sync_reserve.reserve()<learned,"delivered() must still retire reserve once prepared_at finishes early");
        std::puts("SYNC_DELIVERY_RESERVE brief stall ignored, sustained delivery expiry learns 1 period, delivered() preserves prepared_at margin and retires when early PASS");
    }
    return 0;
}
