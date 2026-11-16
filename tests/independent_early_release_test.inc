void independent_early_release_test(bool d12,bool cancel=false) {
    const auto saved=period;period=50'000'000;independent_fixture_mode=true;
    {
        Fixture f(d12,true,{},false,true,false,8);f.throttled=false;
        const auto chain=f.make_chain();
        std::promise<void> second_ready,third_ready;
        auto second=second_ready.get_future(),third=third_ready.get_future();
        std::atomic<std::int64_t> third_at{};std::atomic_uint ended_at_third{UINT_MAX};
        f.prepared_callback=[&](std::uint64_t sequence) {
            if(sequence==2)second_ready.set_value();
            if(sequence==3){third_at=ns();ended_at_third=f.real_ends.load();third_ready.set_value();}
        };
        f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"early release first packet");f.wait_entered();
        f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"early release second packet");
        f.write(chain,120);f.begin_virtual();xr_ok(f.submit(chain,3),"early release third packet");
        check(second.wait_for(3s)==std::future_status::ready,"one future target prepares during prior End");
        const bool third_held=third.wait_for(5ms)==std::future_status::timeout;
        const auto unblocked=ns();f.unblock();
        check(third.wait_for(3s)==std::future_status::ready,"third target prepares after prior End returns");
        const bool early=third_at-unblocked<period/2 && ended_at_third==1;
        if(cancel && early) {
            f.prepared_callback={};f.transport->stop();
            check(!f.cycle_begun && !f.cycle_waited && !f.chains.at(chain).owned && f.real_ends==1 &&
                f.native_releases==f.real_acquires,"stop must balance early released and next waited images without presenting them");
            // The stopped packet is gone; a new allocation may reuse its
            // metadata address without being a heartbeat of the old run.
            f.previous_end_layers=nullptr;
            xr_ok(f.transport->start(),"restart after early release stop");
            f.write(chain,160);f.begin_virtual();xr_ok(f.submit(chain,4),"fresh source after early release restart");
            f.wait_count(2);xr_ok(f.transport->drain(),"early release restart drain");
            check(f.pixels==std::vector<unsigned>({40,160}) && f.positions==std::vector<float>({1,4}),
                "stopped early output appeared after restart");
            xr_ok(f.transport->destroy_swapchain(chain),"early stop source cleanup");
            std::printf("EARLY_STOP %s exact release balance/no phantom frame/restart pixels PASS\n",d12?"D12":"D11");
        } else {
        f.wait_count(3);xr_ok(f.transport->drain(),"early release packets drain");
        check(third_held,"release advanced while a prior End still used the old released image");
        check(early,"ready head stayed unreleased until its presentation slot, blocking next native copy");
        check(f.pixels==std::vector<unsigned>({40,80,120}) && f.positions==std::vector<float>({1,2,3}),
            "early release replaced last-released pixels or reordered poses");
        for(unsigned i=1;i<f.handoffs.size();++i)
            check(f.handoffs[i]-f.handoffs[i-1]>=period-500'000,"early Release advanced physical End cadence");
        xr_ok(f.transport->destroy_swapchain(chain),"early release source cleanup");
        std::printf("EARLY_RELEASE %s priorEnd protected; next copy ready before next slot; FIFO pixels/poses/cadence PASS\n",d12?"D12":"D11");
        }
    }
    independent_fixture_mode=false;period=saved;
}
