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
    runtime.buffer_capacity=runtime.vdxr_legacy ? 0 : desired.increase_buffer ? 3 : 2;
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
    check(benchmark_best({s(90,40,8),s(89.5,42,1)})==1,"within two ms prefer stable FPS range");
    check(benchmark_best({s(90,40,8),s(89.5,42.01,1)})==0,"stability cannot hide more than two ms latency");
    check(benchmark_best({s(90,40,2,8),s(90,41,2,1)})==1,"equal FPS ranges use latency range");
    check(benchmark_best({s(90,40),s(90,39)})==1,"identical stability chooses exact lower latency");
    check(benchmark_best({s(90,40),s(89.5,40)})==0,"remaining tie chooses exact higher FPS");
    check(benchmark_best({s(90,40),s(89.1,35),s(88.2,30)})==1,"near-tie chains cannot drift below global FPS tolerance");
    check(benchmark_best({s(90,40,5),s(90,41.5,4),s(90,43,1)})==1,"latency tolerance stays anchored to the global minimum");
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
    check(benchmark_best({score(70,40),score(90,40,4,2999)},true)==0,"2x2 excluded below 3000 pixels");
    check(benchmark_best({score(70,40),score(90,40,4,3000)},true)==1,"2x2 allowed at exactly 3000 pixels");
    check(benchmark_best({score(70,40),score(95,40,8,3999)},true)==0,"4x4 excluded below 4000 pixels");
    check(benchmark_best({score(70,40),score(95,40,8,4000)},true)==1,"4x4 allowed at exactly 4000 pixels");
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
    check(benchmark_best({score(81,20,0,4100,90,true),score(90,30)},false)==1,"AFW priority is Luke-only");
    check(benchmark_best({score(81,20,0,4100,0,true),score(90,30)},true)==1,"unknown refresh never enables AFW priority");

}
void automatic_choice_controller() {
    RuntimeStatus runtime{};runtime.sessions=1;runtime.dlssg_preset_request=1;
    // Original VDXR, private basic, private full and Luke all use the same chooser.
    for(unsigned scenario=0;scenario<12;++scenario) {
        const bool modern=scenario!=0,luke=scenario>=3 && scenario<=6,full=scenario==2 || scenario==7 || scenario>=10;
        auto saved=snapshot().desired;saved.advanced_mode=false;saved.pacing_mode=1;
        saved.benchmark_auto_pick=true;saved.nr_enabled=true;saved.afw_mode=3;saved.afw_shading_rate=0;
        saved.afw_only=scenario==6; // also test clearing an unapplied direct-AFW request when OFXR wins
        check(request(saved),"save auto-pick game preference");saved=snapshot().desired;
        runtime.vdxr_runtime=!modern;runtime.steamvr_runtime=modern;runtime.vdxr_legacy=!modern;runtime.luke_detected=luke;
        runtime.pacing_mode=modern?1:0;runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        runtime.eye_long_side=scenario==4?2800:4100;report_test_runtime(runtime);
        auto plan=benchmark_execution_plan(full,luke,modern,false,runtime.eye_long_side);
        if(scenario>=10)std::erase_if(plan,[](const auto& step){return step.triple;});
        auto winner=plan.size()-1;
        if(scenario==11) {
            winner=0;while(winner<plan.size() && (plan[winner].disabled || plan[winner].generation || plan[winner].backend!=2))++winner;
            check(winner<plan.size(),"full-refresh optical candidate exists");
        }
        if(scenario==4 || scenario==6) {
            winner=0;while(winner<plan.size() && (plan[winner].disabled || plan[winner].backend!=2 || plan[winner].shading!=0 || plan[winner].pacing!=1))++winner;
            check(winner<plan.size(),"full-quality OFXR candidate exists");
        }
        if(scenario==5)winner=0;
        if(scenario==8 || scenario==9) {
            winner=0;
            while(winner<plan.size() && (!plan[winner].triple || plan[winner].a))++winner;
            check(winner<plan.size(),"3X Normal candidate exists");
            if(scenario==9)winner=plan.size()-1; // the eligible non-3X route must win
        }
        check(start_benchmark(full,luke) && snapshot().benchmark.auto_pick,"freeze auto-pick at suite start");
        for(std::size_t i=0;i<plan.size() && benchmark_active(snapshot().benchmark);++i) {
            // Telemetry is timestamped by the real reporter. Start this trial's
            // simulated clock in the past, then sample that public interface.
            const auto clock=GetTickCount64()-4000;
            const auto current=snapshot();runtime.applied_revision=current.revision;
            check(current.desired.afw_only==saved.afw_only,"benchmark trials never toggle the startup-only AFW flag");
            runtime.pacing_mode=current.desired.pacing_mode;runtime.afw_shading_rate=current.desired.afw_shading_rate;
            runtime.motion_source=current.desired.frame_generation?1:0;runtime.generation_state=GenerationState::ready;
            runtime.afw_running=luke;runtime.telemetry={};report_test_runtime(runtime);report_interface_telemetry({});tick_benchmark(clock);
            check(snapshot().benchmark.phase==BenchmarkPhase::running,"auto-pick trial settings applied");
            for(unsigned sample=0;sample<3;++sample) {
                // GetTickCount64 may advance in ~16 ms steps. Wait for an actual
                // new tick so no boundary-case median loses a duplicate sample.
                const auto previous_tick=GetTickCount64();
                do {Sleep(1);} while(GetTickCount64()==previous_tick);
                auto& t=runtime.telemetry;t.updated_ms=clock+4000+sample*1000;
                t.rates_valid=true;t.output_source=modern?xrfg::InterfaceSource::steamvr_delivery:xrfg::InterfaceSource::runtime_accepted_delivery;
                // Step1 never generates. The remaining FidelityFX steps generate
                // at an impressive rate but are explicitly excluded for quality.
                const bool fidelity=current.desired.enabled && current.desired.frame_generation==0 && current.desired.backend==0;
                t.generating=current.desired.enabled && i!=1;
                t.output_fps=(i==1 || fidelity?120.F:i==0?70.F:i==winner?89.5F:90.F)+(float(sample)-1)*.5F;
                t.cycle_estimated_valid=true;t.cycle_estimated_ms=(i==1 || fidelity?1.F:i==winner?40.F:60.F)+float(sample);
                t.display_hz=(!luke && scenario<8)?180.F:120.F;
                if(scenario==4 && current.desired.afw_shading_rate!=0)t.output_fps=120+float(sample)-1;
                if(scenario==5)t.output_fps=(i==winner?108.F:current.desired.enabled?119.F:70.F)+(float(sample)-1)*.5F;
                if(scenario==6) {
                    t.output_fps=(i==winner?108.F:current.desired.enabled && current.desired.afw_shading_rate!=0?119.F:70.F)+(float(sample)-1)*.5F;
                    t.cycle_estimated_ms=(i==winner?80.F:40.F)+float(sample);
                }
                if(scenario==7) {
                    t.output_fps=(i==winner?87.F:current.desired.frame_generation?70.F:fidelity?120.F:current.desired.enabled?90.F:70.F)+(float(sample)-1)*.5F;
                    t.cycle_estimated_ms=(i==winner?52.F:current.desired.frame_generation?70.F:40.F)+float(sample);
                }
                if(scenario>=10) {
                    t.display_hz=90;
                    const bool vector=current.desired.frame_generation!=0;
                    t.output_fps=(vector ? (scenario==10 && i==winner ? 90.F : 89.5F) : 90.F)+(float(sample)-1)*.5F;
                    t.cycle_estimated_ms=(vector ? (scenario==10 ? 52.F:10.F) : i==winner || scenario==10 ? 40.F:60.F)+float(sample);
                }
                if(scenario==8 || scenario==9) {
                    const bool triple=current.desired.enabled && current.desired.triple_frame_gen;
                    t.output_fps=(triple?110.F:scenario==9 && i==winner?80.F:70.F)+(float(sample)-1)*.5F;
                    t.cycle_estimated_ms=(i==winner?40.F:60.F)+float(sample);
                }
                report_test_runtime(runtime);report_interface_telemetry(t);tick_benchmark(GetTickCount64()+sample*300);
            }
            tick_benchmark(clock+kBenchmarkDurationMs);
        }
        const auto result=snapshot();auto expected=benchmark_settings(saved,plan[winner]);
        if(luke)expected.afw_only=plan[winner].disabled && plan[winner].afw!=0;
        check(result.benchmark.phase==BenchmarkPhase::complete && result.desired==expected,
            "completion saves exact route, Fast, multiplier, pacing and AFW quality of the valid winner");
        check(std::string(result.benchmark.message).find("Selected "+plan[winner].label)!=std::string::npos,
            "result identifies the selected route with measured FPS and ms");
        if(luke)check(std::string(result.benchmark.message).find(plan[winner].shading==8?"4x4":"1x1")!=std::string::npos,"Luke result names selected AFW quality");
        check((std::string(result.benchmark.message).find("Restart the game to activate AFW Only.")!=std::string::npos)==(luke && expected.afw_only),
            "AFW-only completion explicitly requests restart; OFXR winners do not");
        check(parse_settings(serialize_settings(result.desired))==expected,"automatic choice persists with route preferences");
        std::ifstream file(configuration_path());std::stringstream contents;contents<<file.rdbuf();
        check(parse_settings(contents.str())==expected,"on-disk game configuration contains the selected settings");
        file.close(); // release Windows rename lock before starting another suite
        runtime.applied_revision=result.revision;report_test_runtime(runtime);
        check(start_benchmark(full,luke),"start cancellable automatic suite");stop_benchmark();tick_benchmark();
        check(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==expected,"Stop never applies a partial automatic choice");
        runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);check(start_benchmark(full,luke),"start manually cancelled automatic suite");
        const auto original=snapshot().desired;auto edit=original;edit.nr_enabled=false;
        check(request_changes(original,edit),"manual edit while auto-pick is armed");tick_benchmark();
        check(snapshot().benchmark.phase==BenchmarkPhase::stopped && !snapshot().desired.nr_enabled,"manual edits win over automatic selection");
    }
    // A fully completed suite with no measurable results restores the original.
    auto saved=snapshot().desired;saved.benchmark_auto_pick=true;check(request(saved),"save no-data fixture");saved=snapshot().desired;
    runtime.luke_detected=false;runtime.applied_revision=snapshot().revision;runtime.telemetry={};report_test_runtime(runtime);
    check(start_benchmark(false),"start no-data automatic suite");auto clock=GetTickCount64()-400000;
    const auto count=snapshot().benchmark.count;
    for(unsigned i=0;i<count;++i) {
        const auto current=snapshot();runtime.applied_revision=current.revision;runtime.pacing_mode=current.desired.pacing_mode;
        runtime.telemetry={};report_test_runtime(runtime);report_interface_telemetry({});tick_benchmark(clock);tick_benchmark(clock+kBenchmarkDurationMs);clock+=12000;
    }
    check(snapshot().benchmark.phase==BenchmarkPhase::complete && snapshot().desired==saved &&
        std::string(snapshot().benchmark.message).find("No valid automatic choice")!=std::string::npos,
        "no-data completion restores settings and reports why no result was applied");
}
int main() {
    const auto folder=std::filesystem::temp_directory_path()/(L"ofxr-suite-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",folder.c_str());
    try {
        buffer_plan_contract();
        automatic_choice_rules();
        quality_choice_rules();
        const auto optical=benchmark_plan(false,false,true,true),full=benchmark_plan(true,false,true,true);
        check(optical.size()==25 && full.size()==43,"SteamVR: three pacing modes, Normal/Fast, one Off reference");
        for(const bool luke:{false,true}) for(const bool complete:{false,true}) {
            const auto legacy=benchmark_plan(complete,luke,false);
            check(legacy.size()==(luke ? 12 : complete ? 15 : 9),"Legacy VDXR fits the dedicated spreadsheet columns");
            std::set<std::tuple<bool,int,int,bool,int,int>> unique;
            for(const auto& step:legacy) {
                Settings original;original.pacing_mode=2;
                check(step.pacing==0 && benchmark_settings(original,step).pacing_mode==0,
                    "Legacy VDXR always tests its Framework path");
                check(unique.emplace(step.disabled,step.generation,step.backend,step.triple,step.a,step.shading).second,
                    "Legacy has no duplicated or mislabelled trials");
                if(luke)check(!step.triple && !step.a && !step.b && step.afw==3 &&
                    (step.shading==0 || step.shading==4 || step.shading==8),
                    "Legacy Luke tests only three AFW qualities, no Fast or 3X");
                else if(!step.disabled) {
                    check(step.a==step.b,"Legacy Fast controls both memory flags");
                    check(std::count_if(legacy.begin(),legacy.end(),[&](const auto& peer) {
                        return !peer.disabled && peer.generation==step.generation &&
                            peer.backend==step.backend && peer.triple==step.triple && peer.a!=step.a;
                    })==1,"Every Legacy route has both Normal and Fast exactly once");
                }
            }
        }
        for(const bool complete:{false,true})for(const bool luke:{false,true}) {
            const auto basic=benchmark_plan(complete,luke,true,false);
            check(basic.size()==(luke?39:complete?57:33),"basic mode covers both buffer budgets and omits combined pacing");
            check(std::none_of(basic.begin(),basic.end(),[](const auto& step){return step.pacing==2;}),"basic plan never selects hidden combined pacing");
            const auto exported=benchmark_export_plan(complete,luke,true,false);
            check(exported.size()==basic.size(),"reduced CSV has only actually tested columns");
            for(const auto& step:basic)check(benchmark_settings(Settings{},step).pacing_mode==step.pacing,"basic policy cannot alias a combined export to Director");
            check(benchmark_plan(complete,luke,false,false).size()==benchmark_plan(complete,luke,false,true).size(),"standard VDXR suite is independent of Advanced mode");
        }
        RuntimeStatus vdxr{};vdxr.vdxr_runtime=true;
        check(!pacing_comparison_runtime(vdxr),"Legacy VDXR keeps its short suite");
        vdxr.vdxr_legacy=false;
        check(pacing_comparison_runtime(vdxr) && benchmark_plan(true,false,pacing_comparison_runtime(vdxr),true).size()==43,
            "VDXR opt-in uses all three pacing modes without a false SteamVR identity");
        std::set<std::tuple<bool,int,int,bool,int,int,int>> identities;
        std::set<std::string> names;
        for(const auto& step:full) {
            check(identities.emplace(step.disabled,step.generation,step.backend,step.triple,step.pacing,step.a,step.b).second,
                "Every SteamVR configuration occurs exactly once");
            check(names.insert(step.label).second,"Every main CSV column has an unambiguous name");
            check(step.a==step.b && step.pacing>=0 && step.pacing<=2,"Fast toggles both memory operations at every pacing");
        }
        for(const auto& step:optical)check(step.generation==0,"Optical suite must never request vector-only generators");
        unsigned dlssg_steps=0,prefer=0;
        Settings baseline;baseline.advanced_mode=true;baseline.diagnostics=false;baseline.nr_enabled=true;baseline.afw_mode=2;baseline.pacing_mode=1;baseline.benchmark_auto_pick=false;
        for(const auto& step:full) {
            auto s=benchmark_settings(baseline,step);
            check(!s.diagnostics && s.nr_enabled && s.afw_mode==2,"Keep diagnostics, Neural and AFW choices");
            check(s.pacing_mode==step.pacing,"Every SteamVR trial explicitly selects its pacing mode");
            check(!s.triple_frame_gen || s.separate_synthesis || !s.lower_vram,"Never silently test 2X as 3X");
            if(step.generation==2){
                ++dlssg_steps;
                check(step.label.starts_with("DLSS-G ") && !step.label.starts_with("DLSS-G C"),"Route labels must not invent a preset");
                for(int prior=0;prior<4;++prior) {
                    auto manual=baseline;manual.backend=prior;
                    select_generation(manual,2,prior);
                    manual.lower_vram=manual.separate_synthesis=step.b?1:0;
                    manual=sanitize(manual);
                    check(manual.backend==s.backend && manual.frame_generation==s.frame_generation &&
                        manual.lower_vram==s.lower_vram && manual.native_scale==s.native_scale,
                        "manual DLSS-G and benchmark select identical presenter, memory variant and native resolution");
                }
            }
            if(step.prefer && !step.triple)++prefer;
        }
        check(dlssg_steps==6 && prefer==0,"Three pacing modes times two DLSS-G memory variants and no Prefer FPS trial");
        BenchmarkSamples samples;xrfg::InterfaceTelemetry m{};
        m.rates_valid=true;m.output_source=xrfg::InterfaceSource::steamvr_delivery;
        m.cycle_estimated_valid=true;m.generating=true;
        for(const auto time:{3499ULL,3500ULL,4000ULL,5000ULL,8499ULL,8500ULL}) {
            m.updated_ms=time;m.output_fps=time==3499 ? 999.F : time==3500 ? 40.F : time==4000 ? 60.F : time==5000 ? 80.F : 100.F;
            m.cycle_estimated_ms=m.output_fps/2;
            samples.observe(m,1000,8500);
        }
        check(samples.fps.size()==4 && benchmark_median(samples.fps)==70 && benchmark_median(samples.latency)==35,"Discard leading 2.5s only; measure five seconds including the last pre-deadline sample");
        m.updated_ms=8499;samples.observe(m,1000,8500);check(samples.fps.size()==4,"Repeated telemetry cannot overweight the median");
        BenchmarkSamples unknown;m.updated_ms=5000;m.output_source=xrfg::InterfaceSource::accepted_submissions;
        unknown.observe(m,1000,8500);check(unknown.fps.empty(),"Submitted rate is never a presentation measurement");
        m.output_source=xrfg::InterfaceSource::steamvr_scene_delivery;
        ++m.updated_ms;
        unknown.observe(m,1000,8500);check(unknown.fps.size()==1,"Measured distinct Luke scene FPS enters benchmark");
        ++m.updated_ms;m.output_source=xrfg::InterfaceSource::accepted_scene_submissions;
        unknown.observe(m,1000,8500);check(unknown.fps.size()==2,"Distinct accepted Luke total enters benchmark with explicit source");
        baseline.benchmark_csv=true;check(request(baseline),"Save isolated test profile");baseline=snapshot().desired;
        RuntimeStatus runtime{};runtime.sessions=1;runtime.steamvr_runtime=true;runtime.vdxr_legacy=false;runtime.dlssg_preset_request=1;
        runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        check(benchmark_enabled(),"INI flag defaults on");
        check(WritePrivateProfileStringW(L"benchmark",L"enabled",L"0",configuration_path().c_str()),"disable isolated benchmarks");
        check(!benchmark_enabled() && !start_benchmark(true),"explicit INI disable blocks suite commands");
        check(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"enable isolated benchmarks in the single INI");
        runtime.pacing_mode=kAfwOnlyPacingMode;report_test_runtime(runtime);
        check(!start_benchmark(true) && snapshot().benchmark.phase==BenchmarkPhase::error,
            "AFW-only session cannot be labelled as an OFXR route matrix");
        runtime.pacing_mode=2;report_test_runtime(runtime);
        check(start_benchmark(true),"Start full suite with A without changing profile or requesting restart");
        auto state=snapshot();check(!start_benchmark(false),"A second UI cannot start a competing suite");
        const auto first_requested=state.benchmark.requested_ms;
        tick_benchmark(first_requested+250);
        check(snapshot().benchmark.phase==BenchmarkPhase::applying,"Do not start the 7.5-second clock before settings apply");
        runtime.applied_revision=state.revision;report_test_runtime(runtime);
        auto clock=first_requested+500;
        for(std::size_t i=0;i<full.size();++i) {
            state=snapshot();runtime.applied_revision=state.revision;runtime.pacing_mode=state.desired.pacing_mode;report_test_runtime(runtime);
            if(i==3) {
                runtime.pacing_mode=(state.desired.pacing_mode+1)%3;report_test_runtime(runtime);
                tick_benchmark(clock-1);
                check(snapshot().benchmark.phase==BenchmarkPhase::applying,"Wait for effective pacing, even after settings revision is applied");
                runtime.pacing_mode=state.desired.pacing_mode;report_test_runtime(runtime);
            }
            tick_benchmark(clock);state=snapshot();
            check(state.benchmark.phase==BenchmarkPhase::running && state.benchmark.index==i,"Advance after actual application without an open menu");
            check(interface_telemetry_requested(),"Hidden suite requests metrics even with diagnostics off");
            tick_benchmark(clock+kBenchmarkMemoryRequestMs-1);
            check(!take_benchmark_memory_request(),"VRAM is not requested before seven seconds");
            tick_benchmark(clock+kBenchmarkMemoryRequestMs);
            const auto request_revision=take_benchmark_memory_request();
            check(request_revision==state.benchmark.revision,"End-of-test memory request targets this applied revision");
            check(!take_benchmark_memory_request(),"Memory request is claimed only once");
            BenchmarkMemory memory{};memory.revision=request_revision;memory.updated_ms=clock+kBenchmarkMemoryRequestMs;
            memory.vram_valid=true;memory.vram_bytes=1048576ULL*(i+100);
            memory.ram_valid=true;memory.ram_working_set_bytes=2097152;
            report_benchmark_memory(memory);
            tick_benchmark(clock+kBenchmarkDurationMs-1);
            check(snapshot().benchmark.index==i,"Never shorten a configuration below 7.5 seconds");
            tick_benchmark(clock+kBenchmarkDurationMs);clock+=kBenchmarkDurationMs+200;
        }
        state=snapshot();check(state.benchmark.phase==BenchmarkPhase::complete && state.desired==baseline,"Completion restores every original setting/profile");
        std::ifstream csv(std::filesystem::u8path(state.benchmark.csv_path));std::stringstream text;text<<csv.rdbuf();
        check(text.str().find("Output FPS median")!=std::string::npos && text.str().find("Cycle minimum median ms")!=std::string::npos,"CSV has both medians");
        const auto csv_text=text.str();
        std::cout<<"FULL_CSV="<<std::filesystem::u8path(state.benchmark.csv_path).string()<<'\n';
        const auto details_path=std::filesystem::u8path(state.benchmark.csv_path);
        std::ifstream details(details_path.parent_path()/(details_path.stem().wstring()+L"-details.csv"));
        std::stringstream details_text;details_text<<details.rdbuf();
        check(details_text.str().find("Status,PacingMode,Runtime,LegacyVDXR,EyeLongSide,DisplayHz,NativeVectors,BridgeBuffer\n")!=std::string::npos,
            "details CSV identifies the requested pacing of every trial");
        check(details_text.str().find(",A startup request,")!=std::string::npos &&
            details_text.str().find("C startup request")==std::string::npos,"CSV records A receipt instead of inventing C");
        check(details_text.str().find(",2.500,5.000,")!=std::string::npos,
            "CSV truthfully records the 2.5-second exclusion and five-second measurement window");
        check(csv_text.find("VRAM end MiB,100.000,101.000")!=std::string::npos,"Memory belongs to the trial that just finished, before the next switch");
        check(!take_benchmark_memory_request(),"Completed suite leaves no outstanding sample request");
        check(std::count(csv_text.begin(),csv_text.end(),'\n')==11,"Layout CSV: headers, six statistics rows and four memory rows");
        // A manual edit wins instead of being overwritten at the next timed tick.
        runtime.applied_revision=state.revision;report_test_runtime(runtime);check(start_benchmark(false),"Start optical suite");
        state=snapshot();auto edited=state.desired;edited.nr_enabled=false;
        check(request_changes(state.desired,edited),"Manual edit");tick_benchmark();
        check(snapshot().benchmark.phase==BenchmarkPhase::stopped && !snapshot().desired.nr_enabled,"Manual changes cancel automation and are kept");
        runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);check(start_benchmark(false),"Restart optical suite");stop_benchmark();tick_benchmark();
        check(snapshot().benchmark.phase==BenchmarkPhase::stopped,"Explicit Stop");
        const auto luke=benchmark_plan(false,true,true,true);check(luke.size()==30,"Luke: AFW-only three qualities plus four routes times three pacing times three qualities");
        identities.clear();
        for(const auto& step:luke) {
            check(!step.triple && !step.prefer,"Luke omits unsupported 3X and Prefer FPS");
            check(identities.emplace(step.disabled,step.generation,step.backend,step.triple,step.pacing,step.shading,step.afw).second,
                "Luke never repeats an AFW-only or route/pacing/quality trial");
        }
        unsigned afw_only=0;
        for(const auto& step:luke) {
            check(step.afw==3,"Luke never tests AFW disabled");
            check(step.generation==0 && !step.a && !step.b,"Luke ends at 0.3.0 without vector generators or A/B permutations");
            if(step.disabled && step.afw==3)++afw_only;
        }
        check(afw_only==3,"AFW-only Full/Half/Quarter trials");
        runtime.luke_detected=true;runtime.eye_long_side=4100;runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        check(start_benchmark(false,true),"Start Luke suite");clock=snapshot().benchmark.requested_ms+500;
        for(std::size_t i=0;i<luke.size();++i) {
            state=snapshot();runtime.applied_revision=state.revision;
            runtime.pacing_mode=state.desired.pacing_mode;
            runtime.afw_shading_rate=state.desired.afw_shading_rate;
            runtime.afw_running=state.desired.afw_mode!=0;report_test_runtime(runtime);
            tick_benchmark(clock);check(snapshot().benchmark.phase==BenchmarkPhase::running,"Luke quality applied before timing");
            tick_benchmark(clock+kBenchmarkDurationMs);clock+=kBenchmarkDurationMs+200;
        }
        state=snapshot();check(state.benchmark.phase==BenchmarkPhase::complete,"Luke completes");
        std::cout<<"LUKE_CSV="<<std::filesystem::u8path(state.benchmark.csv_path).string()<<'\n';
        std::ifstream layout(std::filesystem::u8path(state.benchmark.csv_path));std::string header;std::getline(layout,header);
        const auto blank=header.find("0.2.1 Framework / blank"),one=header.find("0.2.1 Framework / 1x1"),two=header.find("0.2.1 Framework / 2x2"),four=header.find("0.2.1 Framework / 4x4");
        check(blank==std::string::npos && one<two && two<four && four!=std::string::npos,"Luke CSV groups only1x1/2x2/4x4; no disabled AFW tests");
        runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        check(start_benchmark(false),"start trial before disabling flag");
        check(WritePrivateProfileStringW(L"benchmark",L"enabled",L"0",configuration_path().c_str()),"disable active benchmark");
        tick_benchmark();
        check(snapshot().benchmark.phase==BenchmarkPhase::stopped && !take_benchmark_memory_request(),"disabling INI stops trial and clears memory requests");
        check(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"re-enable for preset compatibility checks");
        for(const auto preset:{0U,3U}) {
            runtime.dlssg_preset_request=preset;runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
            check(start_benchmark(true),"Full suite accepts default/unverified or explicit C without enforcing another preset");
            check(snapshot().runtime.dlssg_preset_request==preset,"Suite does not change the startup receipt");
            clock=snapshot().benchmark.requested_ms+500;
            for(std::size_t i=0;i<full.size();++i) {
                runtime.applied_revision=snapshot().revision;runtime.pacing_mode=snapshot().desired.pacing_mode;report_test_runtime(runtime);tick_benchmark(clock);
                check(snapshot().benchmark.phase==BenchmarkPhase::running,"A/C/default use the same uninterrupted suite");
                tick_benchmark(clock+kBenchmarkDurationMs);clock+=kBenchmarkDurationMs+200;
            }
            check(snapshot().benchmark.phase==BenchmarkPhase::complete,"Full suite completes without a forced preset restart");
            const auto path=std::filesystem::u8path(snapshot().benchmark.csv_path);
            std::ifstream preset_details(path.parent_path()/(path.stem().wstring()+L"-details.csv"));
            std::stringstream recorded;recorded<<preset_details.rdbuf();
            check(recorded.str().find(preset==3 ? ",C startup request," : ",Game default / unverified,")!=std::string::npos,
                "CSV preserves the actual startup receipt including an unverified default");
        }
        for(const bool reduced_luke:{false,true}) {
            auto simple=snapshot().desired;simple.advanced_mode=false;simple.pacing_mode=1;simple.increase_buffer=true;check(request(simple),"set basic suite policy");
            runtime.luke_detected=reduced_luke;runtime.applied_revision=snapshot().revision;runtime.pacing_mode=1;report_test_runtime(runtime);
            check(start_benchmark(!reduced_luke,reduced_luke),"start reduced full/Luke suite");
            const auto plan=benchmark_plan(!reduced_luke,reduced_luke,true,false);
            check(snapshot().benchmark.count==plan.size() && !snapshot().benchmark.advanced,"suite freezes basic mode at start");
            clock=snapshot().benchmark.requested_ms+500;
            for(const auto& step:plan) {
                auto s=snapshot();check(s.desired.pacing_mode==step.pacing && step.pacing!=2,"reduced execution follows exact two-mode sequence");
                runtime.applied_revision=s.revision;runtime.pacing_mode=s.desired.pacing_mode;runtime.afw_shading_rate=s.desired.afw_shading_rate;report_test_runtime(runtime);
                if(step.buffer==1) {
                    runtime.buffer_capacity=2;::xrfg::autonomous::report_runtime(runtime);
                    tick_benchmark(clock-1);
                    check(snapshot().benchmark.phase==BenchmarkPhase::applying,"buffer request/revision alone cannot start the trial before actual three-credit capacity");
                    report_test_runtime(runtime);
                }
                tick_benchmark(clock);check(snapshot().benchmark.phase==BenchmarkPhase::running,"reduced quality/pacing/buffer applied");
                check(snapshot().desired.increase_buffer==(step.buffer==1),"basic suite actually changes buffer instead of relabelling it");
                tick_benchmark(clock+kBenchmarkDurationMs-1);
                BenchmarkMemory memory{};memory.revision=take_benchmark_memory_request();memory.updated_ms=clock+kBenchmarkDurationMs-1;
                memory.vram_valid=true;memory.vram_bytes=1048576ULL*(700+snapshot().benchmark.index);
                report_benchmark_memory(memory);
                tick_benchmark(clock+kBenchmarkDurationMs);clock+=kBenchmarkDurationMs+200;
            }
            check(snapshot().benchmark.phase==BenchmarkPhase::complete && snapshot().desired==sanitize(simple),"reduced completion restores original basic settings");
            std::ifstream csv(std::filesystem::u8path(snapshot().benchmark.csv_path));std::string header;std::getline(csv,header);
            check(header.find("Framework+director")==std::string::npos,"basic CSV cannot fabricate combined-mode measurements");
            check(header.find(" Buffer")!=std::string::npos,"basic CSV labels the larger buffer distinctly");
            check(std::count(header.begin(),header.end(),',')==plan.size()+2,"CSV trial count matches every buffer/quality configuration");
            std::cout<<(reduced_luke?"BASIC_LUKE_CSV=":"BASIC_FULL_CSV=")
                <<std::filesystem::u8path(snapshot().benchmark.csv_path).string()<<'\n';
        }
        runtime.steamvr_runtime=false;runtime.vdxr_runtime=true;runtime.vdxr_legacy=true;runtime.luke_detected=false;
        runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        check(start_benchmark(false) && snapshot().benchmark.count==9,"VDXR controller selects the legacy optical suite");
        stop_benchmark();tick_benchmark();
        for(bool legacy_luke:{false,true}) {
            runtime.luke_detected=legacy_luke;runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
            check(start_benchmark(true,legacy_luke) && snapshot().benchmark.count==(legacy_luke?12:15),
                "Legacy controller selects complete Normal/Fast or three-quality Luke matrix");
            clock=snapshot().benchmark.requested_ms+500;
            for(unsigned i=0;i<(legacy_luke?12U:15U);++i) {
                state=snapshot();runtime.applied_revision=state.revision;runtime.pacing_mode=0;
                runtime.afw_shading_rate=state.desired.afw_shading_rate;report_test_runtime(runtime);
                tick_benchmark(clock);check(snapshot().benchmark.phase==BenchmarkPhase::running,"Legacy settings applied before timing");
                tick_benchmark(clock+4000);tick_benchmark(clock+kBenchmarkDurationMs);clock+=kBenchmarkDurationMs+200;
            }
            check(snapshot().benchmark.phase==BenchmarkPhase::complete,"Every Legacy trial completes");
            std::cout<<(legacy_luke?"LEGACY_LUKE_CSV=":"LEGACY_FULL_CSV=")
                <<std::filesystem::u8path(snapshot().benchmark.csv_path).string()<<'\n';
        }
        runtime.vdxr_legacy=false;runtime.luke_detected=false;runtime.applied_revision=snapshot().revision;report_test_runtime(runtime);
        check(start_benchmark(true) && snapshot().benchmark.count==57,"Forced private VDXR respects the current basic scope");
        stop_benchmark();tick_benchmark();
        automatic_choice_controller();
        std::cout<<"Automatic suite: full/optical with A/C/default, no preset enforcement, truthful A CSV, hidden sampling without F9, timing, median, restore/manual cancellation PASS\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
