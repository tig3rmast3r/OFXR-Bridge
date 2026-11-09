static void rife_handoff_matrix() {
    native_contract_fixture_mode=native_adaptive_wait_fixture_mode=native_director_only_fixture_mode=true;
    for(bool gated:{false,true})for(bool director:{false,true})for(bool fast:{false,true})for(bool buffer:{false,true}){
        Fixture f(true,true,{},true,true,false,buffer?3:2,true);f.throttled=false;f.unblock();
        f.prediction_horizon_ns=period*3;
        f.prediction_age_limit=gated?period:0;
        xr_ok(f.transport->set_director_enabled(director),"RIFE native phase mode");
        xr_ok(f.transport->set_buffer_capacity(buffer?3:2,true),"RIFE native phase capacity");
        f.transport->set_ready_clock(true);f.transport->set_rife_handoff(true);
        std::unordered_map<std::uint64_t,std::int64_t> targets;
        unsigned witnessed{};std::vector<std::int64_t> deadlines;
        f.diagnostic_work=[&](int stage,std::uint64_t sequence,std::int64_t at){
            if(stage==46)targets[sequence]=at;
            if(stage==47)++witnessed;
        };
        const auto native_origin=ns()+period/3;std::int64_t native_turn=-1;
        HANDLE gate_timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
        if(gated)f.begin_callback=[&](unsigned){
            // Independent native compositor running-start. A late call returns
            // immediately; spacing successes alone cannot prove consumption.
            const auto now=ns();
            auto turn=std::max(native_turn+1,(now-native_origin)/period);
            turn=std::max(std::int64_t{},turn);
            const auto ready=native_origin+turn*period;
            auto remaining=ready-ns();
            if(remaining>300'000){LARGE_INTEGER due{};due.QuadPart=-(remaining-200'000)/100;
                check(SetWaitableTimer(gate_timer,&due,0,nullptr,nullptr,FALSE)!=FALSE,"native gate timer");
                check(WaitForSingleObject(gate_timer,1000)==WAIT_OBJECT_0,"native running-start wake");}
            while(ns()<ready)YieldProcessor();
            native_turn=turn;f.mock_consumption_deadline=ready+period/2;deadlines.push_back(f.mock_consumption_deadline);
        };
        const unsigned rings=fast?2:4,count=32;
        std::vector<XrSwapchain> chains;
        for(unsigned n=0;n<rings;++n)chains.push_back(f.make_chain());
        const auto origin=std::chrono::steady_clock::now();
        std::vector<unsigned> expected;
        for(unsigned n=0;n<count;++n){
            if(n%2==0)std::this_thread::sleep_until(origin+std::chrono::nanoseconds((n/2)*30'303'030LL));
            auto chain=chains[n%rings];const auto color=40+n*3;expected.push_back(color);
            f.write(chain,color);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"RIFE paired source submit");
        }
        xr_ok(f.transport->drain(),"RIFE native drain");
        check(f.pixels==expected && f.handoffs.size()==count,"RIFE lost a FIFO image");
        for(unsigned n=0;n<count;++n)check(f.positions[n]==float(n),"RIFE changed the camera identity");
        const auto rate=(count-1)*1e9/double(f.handoffs.back()-f.handoffs.front());
        check(rate>58.,"RIFE native phase lost sustainable 66fps throughput");
        if(gated){
            check(witnessed && targets.size()>count/2,"native-gate policy was never learned");
            if(f.mock_consumed<count-2){
                for(unsigned n=0;n<count;++n)std::fprintf(stderr,"window n=%u handoff=%.3f close=%.3f target=%.3f\n",n,(f.handoffs[n]-native_origin)/1e6,(f.mock_end_deadlines[n]-native_origin)/1e6,targets.count(n+1)?(targets[n+1]-native_origin)/1e6:-999.);
                std::fprintf(stderr,"consumed=%u missed=%u witnessed=%u learned=%zu rate=%.3f\n",f.mock_consumed,f.mock_missed,witnessed,targets.size(),rate);
            }
            check(f.mock_consumed>=count-2,"native End succeeded but missed modeled compositor windows");
        }else check(!witnessed && targets.empty(),"nonblocking runtime invented a vsync handoff phase");
        f.transport->set_rife_handoff(false);
        for(auto chain:chains)xr_ok(f.transport->destroy_swapchain(chain),"RIFE source cleanup");
        CloseHandle(gate_timer);
        std::printf("RIFE_HANDOFF gated=%u director=%u fast=%u buffer=%u outputs=%u native_fps=%.3f modeled_consumed=%u missed=%u learned=%zu exact_pixels_poses=PASS device=%s\n",gated,director,fast,buffer,count,rate,f.mock_consumed,f.mock_missed,targets.size(),std::getenv("XRFG_TEST_WARP")?"WARP":"hardware");
    }
    native_contract_fixture_mode=native_adaptive_wait_fixture_mode=native_director_only_fixture_mode=false;
}
