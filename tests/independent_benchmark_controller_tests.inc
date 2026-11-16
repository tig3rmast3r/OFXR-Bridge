void independent_controller(bool aer,bool full,bool luke,double two,double three,double four,unsigned cap=4) {
    auto original=snapshot().desired;auto saved=original;saved.independent_pacing=true;
    saved.ofr_enabled=aer;saved.afw_mode=luke?3:0;saved.afw_only=false;saved.nr_enabled=true;
    saved.rife_resolution=36;saved.benchmark_auto_pick=false;saved.benchmark_csv=true;
    saved.pacing_mode=1;saved.increase_buffer=true;saved.lower_vram=saved.separate_synthesis=1;
    require(request(saved),"Independent controller settings");saved=snapshot().desired;
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.steamvr_runtime=true;
    r.vdxr_legacy=false;r.luke_detected=luke||aer;r.rife_max_multiplier=4;r.dlssg_max_multiplier=cap;
    const auto publish=[&] {
        const auto state=snapshot();const auto& s=state.desired;r.applied_revision=state.revision;
        r.pacing_mode=kIndependentPacingMode;r.buffer_capacity=8;r.generation_state=GenerationState::ready;
        r.rife_execution_active=r.rife_tensorrt=s.frame_generation==4;
        r.afw_running=s.afw_mode!=0;r.afw_shading_rate=s.afw_shading_rate;
        r.osr_mode=s.luke_osr;r.osr_lower_quality=s.luke_osr_lower_quality;
        r.motion_source=s.frame_generation>0 && s.frame_generation<4?1:0;
        report_runtime(r);
    };
    publish();require(start_benchmark(full,luke),"Independent suite starts in active session");
    const auto plan=benchmark_execution_plan(full,luke,true,false,3328,4,36,true,aer,cap);
    require(snapshot().benchmark.count==plan.size(),"controller exposes actual compact capability-filtered count");
    unsigned done{},last_factor=2;bool full_refresh=false;std::string path;
    while(benchmark_active(snapshot().benchmark)) {
        auto state=snapshot();const auto settings=state.desired;const auto factor=generation_multiplier(settings);
        require(settings.pacing_mode==saved.pacing_mode && settings.increase_buffer && settings.lower_vram==saved.lower_vram && settings.nr_enabled,"suite never edits dormant pacing/NR preferences");
        require(settings.frame_generation!=0 || settings.backend==1,"no standalone Fidelity trial");
        require(settings.frame_generation!=3 || settings.hybrid_backend==1,"one Hybrid NVIDIA trial");
        require(!aer || (factor!=3 && settings.afw_mode==0),"raw AER never mixes reconstruction or3X");
        if(factor>last_factor)require(!full_refresh,"higher tier cannot start after stable refresh in an earlier tier");
        last_factor=factor;
        r.telemetry={};publish();report_interface_telemetry({});auto clock=GetTickCount64()-4000;
        tick_benchmark(clock);require(snapshot().benchmark.phase==BenchmarkPhase::running,"settings acknowledgements start Independent interval");
        const auto fps=!settings.enabled?60.:factor==2?two:factor==3?three:four;
        for(unsigned n=0;n<3;++n) {
            const auto previous=GetTickCount64();do{Sleep(1);}while(GetTickCount64()==previous);
            xrfg::InterfaceTelemetry m{};m.updated_ms=GetTickCount64();m.rates_valid=true;
            m.output_source=xrfg::InterfaceSource::steamvr_delivery;m.output_fps=float(fps);m.display_hz=90;
            m.cycle_estimated_valid=true;m.cycle_estimated_ms=40;m.generating=settings.enabled;
            r.telemetry=m;publish();report_interface_telemetry(m);tick_benchmark(GetTickCount64()+n*300);
        }
        if(settings.enabled && fps>=90)full_refresh=true;
        tick_benchmark(clock+benchmark_memory_request_ms(true));const auto revision=take_benchmark_memory_request();
        require(revision==snapshot().benchmark.revision,"one memory sample near Independent10s end");
        BenchmarkMemory memory{};memory.revision=revision;memory.updated_ms=clock+benchmark_memory_request_ms(true);
        memory.vram_valid=memory.ram_valid=true;report_benchmark_memory(memory);
        tick_benchmark(clock+9999);require(snapshot().benchmark.phase==BenchmarkPhase::running,"Independent does not end at the old7.5s deadline or before10s");
        tick_benchmark(clock+10000);++done;require(done<=plan.size(),"compact suite terminates");
    }
    const auto finished=snapshot();require(finished.benchmark.phase==BenchmarkPhase::complete && finished.desired==saved,"completion restores full Independent preferences");
    const unsigned base=luke?6:full?6:3;
    const auto tier3=std::count_if(plan.begin(),plan.end(),[](const auto& step){return step.rife_multiplier==3;});
    const auto expected=two>=90?base:!aer && three>=90?base+unsigned(tier3):unsigned(plan.size());
    require(done==expected,"actual trial count follows completed full tiers, not the first successful candidate");
    const auto layout_path=std::filesystem::u8path(finished.benchmark.csv_path);const auto csv=read(layout_path);
    const auto details=read(layout_path.parent_path()/(layout_path.stem().wstring()+L"-details.csv"));
    require(csv.find("FidelityFX")==std::string::npos && csv.find("Framework")==std::string::npos && csv.find("Director")==std::string::npos,"compact summary has no legacy or excluded columns");
    require(details.find(",2.500,7.500,")!=std::string::npos,"CSV records exact Independent settle/measurement duration");
    require(details.find(",\"complete\",4,SteamVR,")!=std::string::npos,"CSV records actual Independent delivery instead of saved legacy pacing");
    if(done<plan.size())require(csv.find("n.a.")!=std::string::npos,"pruned higher tiers remain explicit empty export cells");
    require(request(original),"restore caller after Independent controller test");
    std::cout<<"Independent benchmark aer="<<aer<<" full="<<full<<" luke="<<luke<<" trials="<<done<<" PASS\n";
}
void independent_controller_cases() {
    std::filesystem::create_directories(configuration_path().parent_path());
    require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"enable isolated controller");
    independent_controller(false,false,false,90,90,90);
    independent_controller(false,true,false,90,90,90,2);
    independent_controller(false,false,false,80,90,90);
    independent_controller(false,true,false,80,85,90,2);
    independent_controller(true,false,false,90,90,90);
    independent_controller(true,true,false,80,85,90,2);
    independent_controller(false,false,true,90,90,90);
    independent_controller(false,false,true,80,90,90);
    auto s=snapshot().desired;s.independent_pacing=true;require(request(s),"route preflight setup");
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.steamvr_runtime=true;r.pacing_mode=0;report_runtime(r);
    require(!start_benchmark(false),"saved Independent cannot benchmark a stale legacy session");
    r.pacing_mode=kIndependentPacingMode;r.afw_only_session=true;report_runtime(r);
    require(!start_benchmark(false),"Independent reconstruction-only startup cannot run a generation matrix");
    r.afw_only_session=false;r.rife_max_multiplier=4;report_runtime(r);
    const auto before=snapshot().desired;require(start_benchmark(false),"Independent Stop case starts");
    stop_benchmark();tick_benchmark();require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==before,"Independent Stop restores exact original settings");
    std::cout<<"Independent controller timing, all2X-first gating, AER, capabilities, CSV and restore PASS\n";
}
