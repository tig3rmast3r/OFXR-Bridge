static void aer_optical_handoff_test(bool d12,bool paced) {
    native_contract_fixture_mode=native_adaptive_wait_fixture_mode=native_director_only_fixture_mode=true;
    {
        Fixture f(d12,true,{},false,true,false,2,false);
        f.throttled=false;f.unblock();f.prediction_horizon_ns=period*3;
        xr_ok(f.transport->set_director_enabled(false),"AER optical Framework mode");
        f.transport->set_aer_optical_handoff(paced);
        std::array<XrSwapchain,2> chains{f.make_chain(),f.make_chain()};
        constexpr unsigned count=24;
        std::vector<unsigned> expected;
        for(unsigned n=0;n<count;++n) {
            // Fast optical work can fill both native credits immediately.
            // Alternate source/pose identities; include one late input so the
            // successor must not catch up by being delivered beside it.
            if(paced && n==12)std::this_thread::sleep_for(40ms);
            const auto chain=chains[n%2];const unsigned color=40+n*3;
            expected.push_back(color);f.write(chain,color);f.begin_virtual();
            xr_ok(f.submit(chain,float(n)),"AER alternating optical packet");
        }
        xr_ok(f.transport->drain(),"AER optical drain");
        check(f.pixels==expected && f.handoffs.size()==count,"AER optical lost/reordered source images");
        std::vector<std::int64_t> gaps;
        for(unsigned n=0;n<count;++n) {
            check(f.positions[n]==float(n),"AER optical changed alternating-eye camera metadata");
            if(n)gaps.push_back(f.handoffs[n]-f.handoffs[n-1]);
        }
        std::sort(gaps.begin(),gaps.end());
        if(paced)check(gaps.front()>=period-250'000,"AER optical collapsed alternating eyes into a catch-up burst");
        else check(gaps[gaps.size()/2]<period/2,"unselected native route unexpectedly acquired AER pacing");
        if(paced)for(bool enabled:{false,true}) {
            f.transport->set_aer_optical_handoff(enabled);
            const auto start=f.handoffs.size();
            for(unsigned n=0;n<8;++n) {
                const auto chain=chains[n%2];const unsigned color=140+n*3;
                f.write(chain,color);f.begin_virtual();
                xr_ok(f.submit(chain,float(100+n)),"AER optical live policy transition");
            }
            xr_ok(f.transport->drain(),"AER optical policy transition drain");
            std::vector<std::int64_t> changed_gaps;
            for(unsigned n=0;n<8;++n) {
                check(f.pixels[start+n]==140+n*3 && f.positions[start+n]==float(100+n),
                    "AER optical live policy changed image/camera identity");
                if(n)changed_gaps.push_back(f.handoffs[start+n]-f.handoffs[start+n-1]);
            }
            std::sort(changed_gaps.begin(),changed_gaps.end());
            if(enabled)check(changed_gaps.front()>=period-250'000,"AER optical pacing did not resume after mode switch");
            else check(changed_gaps[changed_gaps.size()/2]<period/2,"AER optical pacing leaked into another mode");
        }
        for(auto chain:chains)xr_ok(f.transport->destroy_swapchain(chain),"AER optical source cleanup");
        std::printf("AER optical handoff %s paced=%u min=%.3f median=%.3f ms exact images/poses PASS\n",
            d12?"D12":"D11",paced,gaps.front()/1e6,gaps[gaps.size()/2]/1e6);
    }
    native_contract_fixture_mode=native_adaptive_wait_fixture_mode=native_director_only_fixture_mode=false;
}
