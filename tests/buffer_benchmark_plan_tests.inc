void buffer_plan_contract() {
    for(bool luke:{false,true})for(bool full:{false,true}) {
        auto plan=benchmark_plan(full,luke,true,false);
        std::set<std::tuple<bool,int,int,bool,int,int,int,int>> configurations;
        std::set<std::string> labels;
        unsigned references=0,two_credit=0,three_credit=0;
        for(const auto& step:plan) {
            check(step.pacing!=2 && (step.buffer==0 || step.buffer==1),"basic plan explicitly selects two/three without combined pacing");
            check(configurations.emplace(step.disabled,step.generation,step.backend,step.triple,step.pacing,step.a,step.shading,step.buffer).second,
                "each route/pacing/Fast/quality/buffer combination occurs once");
            auto label=step.label+" / "+std::to_string(step.shading);
            check(labels.insert(label).second,"portable CSV columns cannot alias buffer budgets");
            if(step.disabled) {++references;check(!step.buffer,"reference does not inherit a previously selected large buffer");}
            else step.buffer ? ++three_credit : ++two_credit;
            Settings saved; saved.increase_buffer=step.buffer==0;
            saved.nr_enabled=true;saved.afw_ignore_motion_threshold=7.5F;saved.afw_depth_dilate_mode=2;
            for(auto& p:saved.generation_profiles){p.scale=3;p.preset=2;p.native_scale=50;}
            saved.profiles_initialized=true;
            saved.scale=3;saved.preset=2;saved.native_scale=50;
            auto applied=benchmark_settings(saved,step);
            check(applied.increase_buffer==(step.buffer==1),"benchmark applies the explicit buffer to the same normal setting");
            check(applied.nr_enabled && applied.afw_ignore_motion_threshold==7.5F && applied.afw_depth_dilate_mode==2,
                "suite preserves unrelated Neural and AFW parameters");
            check(applied.scale==3 && applied.preset==2 && applied.native_scale==50,"suite keeps route-specific generation resolution and preset");
            if(luke)check(!step.a && !step.b && !step.triple && step.generation==0,"Luke has qualities and buffers, no Fast/vector/3X trial");
            else check(step.a==step.b,"normal Fast consistently selects both operations");
        }
        check(references==(luke?3:1) && two_credit==three_credit,"one reference per quality and equal two/three coverage");
        const auto exported=benchmark_export_plan(full,luke,true,false);
        if(luke)for(std::size_t i=0;i<exported.size();i+=3) {
            for(unsigned q=0;q<3;++q)check(exported[i+q].shading==int(q*4),"Luke export groups1x1/2x2/4x4 within each pacing/buffer");
            check(exported[i].buffer==exported[i+2].buffer && exported[i].pacing==exported[i+2].pacing,"Luke quality triplet cannot cross buffer or pacing");
        }
    }
    for(bool luke:{false,true})for(const auto& step:benchmark_plan(true,luke,false,false)) {
        Settings original;original.increase_buffer=true;
        check(step.buffer==-1 && benchmark_settings(original,step).increase_buffer,"Legacy does not sweep or change this ignored preference");
    }
}
