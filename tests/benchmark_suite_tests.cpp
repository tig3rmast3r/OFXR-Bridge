#include "xrfg/benchmark_suite.hpp"
#include "xrfg/benchmark_view_extent.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <set>
#include <tuple>
#include <limits>
using namespace xrfg::autonomous;
void check(bool condition,const char* text) {if(!condition)throw std::runtime_error(text);}
// Mock the private endpoint's applied capacity alongside the existing applied revision.
void report_test_runtime(RuntimeStatus& runtime) {
    const auto desired=snapshot().desired;
    runtime.buffer_capacity=runtime.vdxr_legacy ? 0 : runtime.steamvr_runtime && desired.enabled && desired.pacing_mode!=0 ? (desired.increase_buffer?4:3) : desired.increase_buffer?3:2;
    report_runtime(runtime);
}
#include "buffer_benchmark_plan_tests.inc"
void automatic_choice_rules() {
    const auto s=[](double fps,double latency,double fps_range=2,double latency_range=2) {
        return BenchmarkScore{fps,latency,fps_range,latency_range,true};
    };
    check(!benchmark_best({}),"empty results never invent a winner");
    check(benchmark_best({s(90,60),s(88.99,20)})==0,"more than one FPS loss is not hidden by low latency");
    check(benchmark_best({s(90,60),s(89,40)})==1,"one FPS is equivalent, so choose lower latency");
    check(benchmark_best({s(90,40,8),s(89.5,43,1)})==1,"within three ms prefer stable FPS range");
    check(benchmark_best({s(90,40,8),s(89.5,43.01,1)})==0,"stability cannot hide more than three ms latency");
    check(benchmark_best({s(90,40,2,8),s(90,41,2,1)})==1,"equal FPS ranges use latency range");
    check(benchmark_best({s(90,40),s(90,39)})==1,"identical stability chooses exact lower latency");
    check(benchmark_best({s(90,40),s(89.5,40)})==0,"remaining tie chooses exact higher FPS");
    check(benchmark_best({s(90,40),s(89.1,35),s(88.2,30)})==1,"near-tie chains cannot drift below global FPS tolerance");
    check(benchmark_best({s(90,40,5),s(90,41.5,4),s(90,44,1)})==1,"latency tolerance stays anchored to the global minimum");
    BenchmarkSamples data;data.fps={89,90,91};data.latency={39,40,41};
    const auto score=benchmark_score(data,true);
    check(score.valid && score.fps==90 && score.latency==40 && score.fps_range==2 && score.latency_range==2,
        "ranking uses the measured medians and complete min-max ranges");
    check(!benchmark_score(data,false).valid,"failed and partial measurements cannot win");
    data.fps={90};check(!benchmark_score(data,true).valid,"single sample does not establish stability");
    data.fps={89,90};data.latency.clear();check(!benchmark_score(data,true).valid,"missing estimated latency cannot win");
    data.latency={39,40};data.fps[1]=std::numeric_limits<double>::quiet_NaN();
    check(!benchmark_score(data,true).valid,"nonfinite metrics cannot win");
    data.fps={0,0};check(!benchmark_score(data,true).valid,"zero delivered FPS cannot win");
    std::vector<BenchmarkScore> reordered{s(90,60),s(89.5,40),s(89.8,41,1)};
    for(unsigned i=0;i<reordered.size();++i) {
        const auto best=benchmark_best(reordered);check(best && reordered[*best].latency==41,"ranking does not depend on trial ordering");
        std::rotate(reordered.begin(),reordered.begin()+1,reordered.end());
    }
}
void quality_choice_rules() {
    const auto score=[](double fps,double latency,int shading=0,unsigned side=4100,double hz=90,bool afw=false,bool vectors=false) {
        BenchmarkScore s{fps,latency,2,2,true};s.shading=shading;s.eye_long_side=side;s.display_hz=hz;s.afw_only=afw;s.vectors=vectors;return s;
    };
    check(benchmark_best({score(70,40),score(90,40,4,2999)},true)==1,"AFW2x2 is included at every resolution");
    check(benchmark_best({score(70,40),score(90,40,4,3000)},true)==1,"2x2 allowed at exactly 3000 pixels");
    check(benchmark_best({score(70,40),score(95,40,8,3999)},true)==0,"4x4 excluded below 4000 pixels");
    check(benchmark_best({score(70,40),score(95,40,8,4000)},true)==0,"4x4 is retired even at high resolution");
    check(benchmark_best({score(70,40),score(95,40,8,0)},true)==0,"unknown resolution never permits coarse AFW");
    check(benchmark_best({score(81,90),score(89,20,4)},true)==0,"1x1 at 90 percent Hz wins despite lower FPS and higher latency");
    check(benchmark_best({score(80.99,90),score(89,20,4)},true)==1,"1x1 below 90 percent does not override performance");
    check(benchmark_best({score(108,90,0,4100,120),score(119,20,4,4100,120)},true)==0,"quality preference follows actual refresh, not a fixed FPS cap");
    check(benchmark_best({score(81,90,0,4100,0),score(90,20,4,4100,0)},true)==1,"unknown refresh cannot trigger a relative preference");
    check(benchmark_best({score(81,90,0,4100,90,true),score(90,20)},true)==0,"AFW at exactly 90 percent overrides full-refresh generation");
    check(benchmark_best({score(80.99,90,0,4100,90,true),score(90,20)},true)==1,"AFW below 90 percent cannot override refresh lock");
    check(benchmark_best({score(81,90,4,3100,90,true),score(90,20)},true)==0,"half AFW can override refresh when full AFW unavailable");
    check(benchmark_best({score(81,90,0,4100,90,true),score(120,20,4,4100,90,true),score(90,10)},true)==0,"full AFW wins regardless of half gain when both qualify");
    check(benchmark_best({score(80,20,0,4100,90,true),score(87,30,4,4100,90,true),score(90,10)},true)==1,"only half qualifies against full-refresh generated route");
    check(benchmark_best({score(79,21,0,3370,90,true),score(87,19.5,4,3370,90,true),score(90,34)},true)==1,"KCD2 V758 selects AFW 2x2");
    check(benchmark_best({score(108,90,0,4100,120,true),score(120,20,4,4100,120,true)},true)==0,"AFW threshold scales at 120 Hz");
    check(benchmark_best({score(107.99,10,0,4100,120,true),score(120,20)},true)==1,"120 Hz AFW below threshold fails");
    check(benchmark_best({score(81,20,0,4100,90,true),score(90,30)},false)==0,"confirmed reconstruction-only priority applies in every suite");
    check(benchmark_best({score(81,20,0,4100,0,true),score(90,30)},true)==1,"unknown refresh never enables AFW priority");

}
int main() {
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-suite-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try {
        buffer_plan_contract();automatic_choice_rules();quality_choice_rules();
        for(bool modern:{false,true})for(bool advanced:{false,true})for(bool luke:{false,true})for(bool full:{false,true}) {
            const auto plan=benchmark_plan(full,luke,modern,advanced);
            const auto expected=modern?(luke?(advanced?112:148):full?(advanced?79:105):(advanced?61:81)):(luke?24:full?19:13);
            check(plan.size()==std::size_t(expected),"complete matrix count");
            std::set<std::tuple<int,int,int,int,int,int,int,int,bool,bool,int,int>> unique;
            for(const auto& step:plan) {
                check(unique.emplace(step.generation,step.backend,step.a,step.pacing,step.buffer,step.rife_multiplier,step.rife_quality,step.shading,step.disabled,step.triple,step.osr,step.osr_lower_quality).second,"route identities do not alias RIFE qualities/multipliers");
                Settings saved;saved.afw_mode=3;saved.afw_shading_rate=8;
                const auto applied=benchmark_settings(saved,step);
                check(applied.afw_shading_rate==(luke?step.shading:0) && saved.afw_shading_rate==8,"each reconstruction quality is applied without mutating saved settings");
                if(luke){
                    check(applied.luke_osr==step.osr && applied.luke_osr_lower_quality==bool(step.osr_lower_quality),"Luke explicitly selects engine and cheap mode");
                    check(step.shading==0 || (step.osr==0 && step.shading==4),"only AFW2x2 reduces provider quality");
                }
                if(step.generation==4)check(applied.rife_model==3 && applied.rife_resolution==step.rife_quality && applied.rife_multiplier==step.rife_multiplier && !applied.motion_vectors,"exact RIFE4.4 Medium/High multiplier, never labelled vectors");
                else if(!full)check(step.generation==0,"no-vector suite admits only optical or RIFE");
            }
        }
        for(bool modern:{false,true}){
            const auto plan=benchmark_execution_plan(false,true,modern,false,1000,modern?4:2);
            std::vector<std::string> references;std::set<std::tuple<int,int,int>> modes;
            bool high_tier=false;
            for(const auto& step:plan){
                modes.emplace(step.osr,step.osr_lower_quality,step.shading);
                if(step.disabled)references.push_back(step.label);
                if(step.rife_multiplier>2)high_tier=true;
                else check(!high_tier,"all2X reconstruction blocks finish before3X");
            }
            check(references==std::vector<std::string>{"AFW 1x1 / only","AFW 2x2 / only","OSR / only","OSR cheap / only"},"requested four reconstruction references in exact order at any resolution");
            check(modes.size()==4,"exactly four distinct reconstruction configurations");
        }
        auto normal=BenchmarkScore{81,60,1,1,true};normal.display_hz=90;normal.osr=true;normal.afw_only=true;
        auto cheap=normal;cheap.fps=89;cheap.latency=20;cheap.osr_lower_quality=true;
        check(benchmark_best({normal,cheap},true)==0,"normal OSR wins over cheap when both reconstruction-only results qualify90percent");
        normal.fps=80;check(benchmark_best({normal,cheap},true)==1,"cheap OSR can win when normal misses90percent");
        auto optical=BenchmarkScore{90,40,1,1,true};optical.display_hz=90;optical.fps_min=89.5;
        auto rife=optical;rife.rife=true;rife.latency=43;
        auto vector=rife;vector.rife=false;vector.vectors=true;
        check(benchmark_best({optical,rife})==1,"RIFE wins within3ms over other non-vectors");
        check(benchmark_best({optical,rife,vector})==2,"native vectors precede RIFE within3ms");
        rife.latency=43.01;vector.latency=43.01;
        check(benchmark_best({optical,rife,vector})==0,"no route preference exceeds3ms");
        rife.latency=40;rife.fps_min=60;
        check(!benchmark_at_refresh(rife) && benchmark_best({rife,optical})==1,"median90 with a deep drop loses stable full refresh");
        optical.display_hz=120;optical.fps=120;optical.fps_min=119;
        check(benchmark_at_refresh(optical),"full-refresh stability follows headset Hz");
        std::cout<<"Benchmark matrices, RIFE exact settings, four Luke reconstructions, stable refresh,3ms and vector/RIFE priorities PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
