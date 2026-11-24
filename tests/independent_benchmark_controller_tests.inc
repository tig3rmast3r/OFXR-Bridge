void independent_controller(bool aer,bool full,bool luke,double two,double three,double four,unsigned cap=4,
    bool timeout_first=false,bool optimize=false,bool steamvr=true) {
    const auto original=snapshot().desired;auto saved=original;saved.advanced_mode=true;saved.independent_pacing=true;
    saved.ofr_enabled=aer;saved.ofr_swap=true;saved.afw_mode=luke?3:0;saved.afw_only=false;saved.nr_enabled=true;
    saved.rife_resolution=36;saved.benchmark_auto_pick=false;saved.benchmark_csv=true;
    saved.pacing_mode=1;saved.increase_buffer=true;saved.lower_vram=saved.separate_synthesis=1;
    require(request(saved),"Independent controller settings");saved=snapshot().desired;
    RuntimeStatus runtime{};runtime.process_id=GetCurrentProcessId();runtime.sessions=1;
    runtime.steamvr_runtime=steamvr;runtime.vdxr_runtime=!steamvr;runtime.vdxr_legacy=false;
    runtime.luke_detected=luke||aer;runtime.rife_max_multiplier=4;runtime.dlssg_max_multiplier=cap;
    const auto publish=[&] {
        const auto state=snapshot();const auto& s=state.desired;runtime.applied_revision=state.revision;
        runtime.pacing_mode=kIndependentPacingMode;runtime.buffer_capacity=8;runtime.generation_state=GenerationState::ready;
        runtime.rife_execution_active=runtime.rife_tensorrt=s.frame_generation==4;
        runtime.afw_running=s.afw_mode!=0;runtime.afw_shading_rate=s.afw_shading_rate;
        runtime.osr_mode=s.luke_osr;runtime.osr_lower_quality=s.luke_osr_lower_quality;
        runtime.motion_source=benchmark_requires_game_vectors(s)?1:0;
        report_runtime(runtime);
    };
    publish();require(start_benchmark(full,luke),"Independent suite starts");
    const auto plan=benchmark_execution_plan(full,luke,steamvr,true,3328,4,36,true,aer,cap);
    require(snapshot().benchmark.count==plan.size(),"count matches complete capability-filtered matrix");
    unsigned done{},rife_trials{},higher{},osr{},raw_aer{};bool timed_out=false,optimized=false;
    while(benchmark_active(snapshot().benchmark)) {
        auto state=snapshot();const auto settings=state.desired;const auto& step=plan.at(done);
        const auto factor=generation_multiplier(settings);const bool rife=settings.frame_generation==4;
        require(settings.frame_generation==step.generation && std::string_view(state.benchmark.label)==step.label,
            "controller applies and labels each method in execution order");
        require(settings.pacing_mode==saved.pacing_mode && settings.increase_buffer && settings.lower_vram==saved.lower_vram && settings.nr_enabled && settings.ofr_swap,"NR and dormant preferences unchanged");
        require(settings.ofr_enabled==bool(step.aer) && settings.afw_mode==step.afw,"per-trial mode applied");
        require(settings.frame_generation!=0 || settings.backend==1,"no FidelityFX");
        require(settings.frame_generation!=3 || (settings.hybrid_backend==1 && settings.afw_mode==0),"Hybrid allowed inAER,never withOSR");
        if(settings.ofr_enabled)require(factor!=3 && !settings.afw_mode,"no AER3X or reconstruction overlap");
        if(rife){++rife_trials;require(settings.rife_resolution==36,"chosen RIFE quality retained");}
        if(factor>2)++higher;if(settings.afw_mode)++osr;if(settings.ofr_enabled)++raw_aer;
        runtime.telemetry={};publish();report_interface_telemetry({});auto clock=GetTickCount64()-4000;
        if(rife && timeout_first && !timed_out) {
            runtime.rife_execution_active=runtime.rife_tensorrt=false;report_runtime(runtime);
            tick_benchmark(state.benchmark.requested_ms+66000);++done;timed_out=true;continue;
        }
        if(rife && optimize && !optimized) {
            runtime.rife_optimizing=true;report_runtime(runtime);
            tick_benchmark(state.benchmark.requested_ms+700000);
            require(snapshot().benchmark.phase==BenchmarkPhase::applying && snapshot().benchmark.index==done,"optimization not measured or timed out");
            runtime.rife_optimizing=false;publish();optimized=true;
        }
        tick_benchmark(clock);require(snapshot().benchmark.phase==BenchmarkPhase::running,"applied ready settings start interval");
        const auto fps=!settings.enabled?60.:factor==2?two:factor==3?three:four;
        for(unsigned n=0;n<3;++n) {
            const auto previous=GetTickCount64();do{Sleep(1);}while(GetTickCount64()==previous);
            xrfg::InterfaceTelemetry m{};m.updated_ms=GetTickCount64();m.rates_valid=true;
            m.output_source=steamvr?xrfg::InterfaceSource::steamvr_delivery:xrfg::InterfaceSource::runtime_accepted_delivery;
            m.output_fps=float(fps);m.display_hz=90;m.cycle_estimated_valid=true;
            m.cycle_estimated_ms=20+10*n;m.generating=settings.enabled;
            runtime.telemetry=m;publish();report_interface_telemetry(m);tick_benchmark(GetTickCount64()+n*300);
        }
        tick_benchmark(clock+benchmark_memory_request_ms(true));const auto revision=take_benchmark_memory_request();
        require(revision==snapshot().benchmark.revision,"memory sampled near10s end");
        BenchmarkMemory memory{};memory.revision=revision;memory.updated_ms=clock+9500;
        memory.vram_valid=memory.ram_valid=true;report_benchmark_memory(memory);
        tick_benchmark(clock+9999);require(snapshot().benchmark.phase==BenchmarkPhase::running,"full10s interval required");
        tick_benchmark(clock+10000);++done;require(done<=plan.size(),"suite bounded");
    }
    const auto finished=snapshot();require(finished.benchmark.phase==BenchmarkPhase::complete && finished.desired==saved,"complete restores exact original settings");
    require(done==plan.size() && higher>0,"every supported3X/4X runs even when2X reaches refresh");
    if(luke)require(osr==14 && raw_aer==11,"Luke executes OSR and full rawAER in same suite");
    if(timeout_first)require(timed_out && rife_trials==3,"one timeout never prunes later RIFE multipliers");
    if(optimize)require(optimized,"optimization case exercised");
    const auto path=std::filesystem::u8path(finished.benchmark.csv_path);const auto csv=read(path);
    const auto details=read(path.parent_path()/(path.stem().wstring()+L"-details.csv"));
    require(csv.find("FidelityFX")==std::string::npos && csv.find("Framework")==std::string::npos && csv.find("Director")==std::string::npos,"no excluded matrix columns");
    require(csv.find("Output FPS mean")!=std::string::npos && csv.find("Cycle minimum mean ms")!=std::string::npos,"mean rows exported");
    require(details.find("AERFramegen,CycleMinimumMeanMs")!=std::string::npos,"mode and latency mean explicitly recorded");
    require(details.find(",2.500,7.500,")!=std::string::npos,"correct measured duration");
    require(details.find("no confirmed")==std::string::npos,"confirmed test output not falsely rejected");
    if(timeout_first)require(details.find("RIFE setup unavailable / skipped")!=std::string::npos,"unavailable sample explicitly recorded");
    const auto columns=[](const std::string& line){bool quoted=false;unsigned n=1;for(char c:line)if(c=='\"')quoted=!quoted;else if(c==','&&!quoted)++n;return n;};
    std::istringstream records(details);std::string line;std::getline(records,line);require(columns(line)==53,"51 old fields retained plus2append-only measurements");
    unsigned rows{};while(std::getline(records,line))if(!line.empty()){require(columns(line)==53,"CSV row width");++rows;}
    require(rows==done,"every measured/skipped trial exported once");
    require(request(original),"restore caller");
    std::cout<<"Independent exhaustive aer="<<aer<<" full="<<full<<" luke="<<luke<<" trials="<<done<<" RIFE="<<rife_trials<<" PASS\n";
}
void independent_controller_cases() {
    auto policy=snapshot().desired;policy.advanced_mode=true;require(request(policy),"benchmark policy fixture");
    std::filesystem::create_directories(configuration_path().parent_path());
    require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"explicitly enable isolated benchmark");
    for(bool aer:{false,true})for(bool full:{false,true})for(bool luke:{false,true})
        independent_controller(aer,full,luke,90,90,90,2);
    independent_controller(false,false,false,70,80,90,4,true);
    independent_controller(false,false,false,90,90,90,4,false,true);
    independent_controller(false,true,false,90,90,90,4,false,false,false);
    auto settings=snapshot().desired;settings.independent_pacing=false;settings.advanced_mode=true;require(request(settings),"legacy preflight");
    RuntimeStatus runtime{};runtime.process_id=GetCurrentProcessId();runtime.sessions=1;runtime.steamvr_runtime=true;runtime.pacing_mode=0;report_runtime(runtime);
    const auto legacy=snapshot().desired;require(!start_benchmark(false) && snapshot().desired==legacy,"legacy benchmark refused without changing settings");
    settings.independent_pacing=true;require(request(settings),"Independent selected");report_runtime(runtime);
    require(!start_benchmark(false),"stale legacy session refused");
    runtime.pacing_mode=kIndependentPacingMode;runtime.afw_only_session=true;report_runtime(runtime);
    require(!start_benchmark(false),"startup reconstruction-only generation suite refused");
    runtime.afw_only_session=false;runtime.rife_max_multiplier=4;report_runtime(runtime);
    const auto before=snapshot().desired;require(start_benchmark(false),"Stop case starts");stop_benchmark();tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==before,"Stop restores settings");
    report_runtime(runtime);require(start_benchmark(false),"manual edit case starts");auto edited=snapshot().desired;edited.nr_enabled=!edited.nr_enabled;require(request(edited),"outside edit");tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired.nr_enabled==edited.nr_enabled,"manual edits win");
    report_runtime(runtime);require(start_benchmark(false),"disconnect case starts");runtime.sessions=0;report_runtime(runtime);tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::error,"disconnect cannot apply partial winner");
}
