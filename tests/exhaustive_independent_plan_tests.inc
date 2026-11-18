void exhaustive_independent_plan_cases() {
    for(bool full:{false,true})for(bool luke:{false,true})for(bool aer:{false,true})for(int quality:xrfg::kRifeResolutions) {
        const auto plan=benchmark_execution_plan(full,luke,true,true,3328,4,quality,true,aer,4);
        check(plan.size()==std::size_t(luke?25:aer?(full?11:5):(full?14:7)),"exhaustive suite exact supported maximum");
        std::set<std::tuple<int,int,int,bool,int,int>> seen;
        std::set<int> methods;unsigned osr_refs{},aer_refs{};
        Settings original;original.advanced_mode=true;original.independent_pacing=true;original.ofr_enabled=aer;
        original.afw_mode=3;original.luke_osr=2;original.nr_enabled=true;original.ofr_swap=true;
        original.pacing_mode=1;original.increase_buffer=true;original.lower_vram=original.separate_synthesis=1;
        check(std::is_sorted(plan.begin(),plan.end(),[](const auto& a,const auto& b){return a.rife_multiplier<b.rife_multiplier;}),"all2X complete before3X and4X");
        for(const auto& step:plan) {
            check(seen.emplace(step.generation,step.backend,step.rife_multiplier,step.disabled,step.osr_lower_quality,step.aer).second,"no duplicate method/multiplier/reconstruction combinations");
            check(step.pacing==-1 && step.buffer==-1 && !step.a && !step.b,"no legacy pacing permutations");
            if(step.generation==0 || step.generation==3)check(step.backend==1,"FidelityFX excluded;one NVIDIA optical engine");
            if(step.afw) {
                check(luke && step.afw==3 && step.osr==2 && !step.aer,"only OSR reconstruction;no AFW or OSR+AER");
                check(step.generation==0 || step.generation==4,"Hybrid/native vector methods excluded after OSR");
                if(step.disabled)++osr_refs;
            } else if(step.disabled && step.aer)++aer_refs;
            if(step.aer)check(step.rife_multiplier!=3,"AER skips unsupported3X");
            if(step.generation==4)check(step.rife_quality==quality,"one user-selected RIFE quality in every mode");
            const auto applied=benchmark_settings(original,step);
            check(applied.ofr_enabled==bool(step.aer) && applied.afw_mode==step.afw,"trial explicitly sets AER and reconstruction mode");
            check(applied.independent_pacing && applied.nr_enabled && applied.ofr_swap && applied.pacing_mode==original.pacing_mode &&
                applied.increase_buffer==original.increase_buffer && applied.lower_vram==original.lower_vram,"NR and dormant pacing/swap preferences retained");
            if(!step.disabled)check(generation_multiplier(applied)==step.rife_multiplier,"requested multiplier reaches provider");
            methods.insert(step.generation);
        }
        if(luke)check(osr_refs==2 && aer_refs==1 && methods==std::set<int>({0,1,2,3,4}),"Luke includes both OSR-alone references and full rawAER");
        if(full && aer)check(std::any_of(plan.begin(),plan.end(),[](const auto& s){return s.aer && s.generation==3;}),"Hybrid+AER retained by owner correction");
    }
    auto capped=benchmark_execution_plan(true,false,true,true,3328,4,18,true,false,2);
    check(capped.size()==12,"DLSS-G2X cap removes only its unsupported higher stereo tiers");
    check(benchmark_execution_plan(true,false,true,true,3328,4,18,true,true,2).size()==11,"AER4 second stage remains available with native2X cap");
    auto plan=benchmark_execution_plan(true,false,true,true,3328,4,18,true,false);
    const auto first=plan.front().label;benchmark_limit_independent(plan,1,2,2,false);
    check(plan.front().label==first && plan.size()==10,"late capability changes preserve completed prefix");
    check(benchmark_mean({10,20,60})==30,"CSV latency mean is arithmetic mean,not median");
}
