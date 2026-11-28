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
    return 0;
}
