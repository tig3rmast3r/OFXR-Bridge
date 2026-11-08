// CPU-only examples with known delivery times, not smoothed FPS fixtures.
void cadence_choice_rules() {
    using namespace xrfg;
    const auto measured=[](double hz,int every=1,bool burst=false,double tail=0.) {
        DeliveryCadenceLog log;BenchmarkCadenceSamples samples;
        const auto stamp=[&](int i){return std::int64_t(std::llround(i*1e9/hz));};
        for(int i=0;i<=int(hz*3);++i) {
            if(i%every==0)log.record(stamp(i)+(burst&&i%2?2'000'000:0));
            if(i%20==0)samples.observe(log.snapshot(stamp(i)),1,unsigned(i*1000/hz),0);
        }
        const auto end=stamp(int(hz*3))+std::int64_t(tail*1e9);
        samples.observe(log.snapshot(end),1,unsigned(end/1'000'000),0);
        return samples.stats(hz);
    };
    for(double hz:{72.,90.,120.,144.}) {
        const auto steady=measured(hz);
        check(steady.valid&&std::abs(steady.mean_fps-hz)<.001&&steady.missed_slots==0,
            "overlapping batches count every steady interval once at dynamic refresh");
        const auto half=measured(hz,2);
        check(half.valid&&std::abs(half.mean_fps-hz/2)<.001&&half.missed_slots>0,
            "held images are not counted as new deliveries");
        const auto stopped=measured(hz,1,false,.2);
        check(stopped.mean_fps<hz-1&&stopped.missed_slots>0,
            "a stopped tail cannot keep the earlier full-refresh rate");
    }
    check(measured(90,1,true).jitter_ms>1.9,"bursty output has measurable frame-interval jitter");
    DeliveryCadenceLog log;BenchmarkCadenceSamples lost;
    for(int i=0;i<30;++i)log.record(i*11'111'111LL);
    lost.observe(log.snapshot(29*11'111'111LL),1,322,0);
    for(int i=30;i<220;++i)log.record(i*11'111'111LL);
    lost.observe(log.snapshot(219*11'111'111LL),1,2433,0);
    check(!lost.stats(90).valid,"missing batches cannot masquerade as stable cadence");
    BenchmarkCadenceSamples changed;changed.observe(log.snapshot(219*11'111'111LL),1,2433,0);
    changed.observe(log.snapshot(219*11'111'111LL),2,2433,0);
    check(!changed.stats(90).valid,"mixed timing sources are not compared as one trace");
    BenchmarkCadenceSamples reset;reset.observe(log.snapshot(219*11'111'111LL),1,2433,0);
    log.record(0);reset.observe(log.snapshot(2'500'000'000LL),1,2500,0);
    check(!reset.stats(90).valid,"reordered or reset confirmations invalidate the trace");
    DeliveryCadenceLog warm;BenchmarkCadenceSamples after_warm;
    for(int i=0;i<=270;++i) {
        warm.record(i*11'111'111LL);
        if(i%20==0&&i>=100)after_warm.observe(warm.snapshot(i*11'111'111LL),1,
            std::uint64_t(std::llround(i*1000./90)),1000);
    }
    check(after_warm.stats(90).valid&&std::abs(after_warm.stats(90).mean_fps-90)<.001,
        "warmup receipts are excluded even in the first overlapping batch");
    BenchmarkSamples data;data.fps={90,90,90,90,72};data.latency={33,33};
    auto score=benchmark_score(data,true);
    check(score.fps_median==90&&std::abs(score.fps-86.4)<.0001&&score.mean_source==3,
        "arithmetic fallback exposes drops hidden by the median and is labelled estimated");
    data.fps={90,60,60};data.fps_times={0,100,1000};
    check(std::abs(benchmark_sample_mean(data)-61.5)<.0001,"uneven telemetry sampling is time weighted");
    auto a=BenchmarkScore{90,33,4,4,true};a.display_hz=90;a.fps_min=87;
    auto b=a;b.latency=37;b.fps_range=.1;b.latency_range=.1;b.fps_min=89.999;
    check(benchmark_best({a,b})==1,"steady full refresh beats a lower latency unstable median");
    b.latency=80;check(benchmark_best({a,b})==1,"stable cap takes precedence over every latency tolerance");
    a.fps=80;a.display_hz=90;b=a;b.latency=37;b.fps_range=.1;b.latency_range=.1;
    check(benchmark_best({a,b})==1,"within five ms stability precedes latency below refresh too");
    b.latency=38.001;check(benchmark_best({a,b})==0,"stability window remains anchored to the fastest latency");
    b.latency=37;a.vectors=true;
    check(benchmark_best({a,b})==1,"vectors are a quality tie break after stability");
    a.vectors=false;a.rife=true;check(benchmark_best({a,b})==1,"RIFE cannot override worse measured stability");
    a=b;a.rife=true;check(benchmark_best({a,b})==0,"RIFE still wins equivalent non-vector results");
    a.vectors=true;b.rife=true;check(benchmark_best({a,b})==0,"vectors win equivalent RIFE results");
    auto optical=BenchmarkScore{80,40,4,4,true};auto v=optical;v.vectors=true;v.fps=79;
    auto slower=v;slower.fps=77;slower.fps_range=.01;
    check(benchmark_best({optical,v,slower})==1,"optical-to-vector widening never widens vector-to-vector FPS tolerance");
    a.cadence_incomplete=true;a.fps_min=90;a.fps=90;
    check(!benchmark_at_refresh(a),"a broken receipt trace never falls back to a fixed-refresh claim");
    DeliveryCadenceLog output,unique;BenchmarkSamples real;real.fps={90,90};real.latency={33,33};real.refresh_hz={90};
    for(int i=0;i<=270;++i) {
        const auto at=i*11'111'111LL;output.record(at);if(i%2==0)unique.record(at);
        if(i%20==0) {real.delivery.observe(output.snapshot(at),1,unsigned(i*1000/90),0);
            real.accepted.observe(unique.snapshot(at),1,unsigned(i*1000/90),0);}
    }
    auto counted=benchmark_score(real,true);
    check(counted.cadence_valid&&std::abs(counted.fps-45)<.001&&counted.mean_source==2&&
        !benchmark_at_refresh(counted),"compositor timing cannot hide repeated application content");
}
