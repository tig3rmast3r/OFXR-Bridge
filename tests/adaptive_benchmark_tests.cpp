#include "xrfg/benchmark_suite.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace xrfg::autonomous;
void require(bool ok,const char* text) {if(!ok)throw std::runtime_error(text);}
void report(RuntimeStatus& r) {
    const auto s=snapshot();r.applied_revision=s.revision;r.pacing_mode=s.desired.pacing_mode;
    r.afw_shading_rate=s.desired.afw_shading_rate;
    r.buffer_capacity=r.vdxr_legacy?0:s.desired.increase_buffer?3:2;report_runtime(r);
}
void policy() {
    for(bool modern:{false,true})for(bool full:{false,true}) {
        for(const auto& step:benchmark_plan(full,false,modern))
            require(step.disabled || step.generation!=0 || step.backend!=0,"optical Fidelity is never benchmarked");
        for(const auto side:{0U,2999U,3000U,3999U,4000U}) {
            const auto p=benchmark_execution_plan(full,true,modern,false,side);
            require(p.size()==(modern?13U:4U)*(side>=4000?3:side>=3000?2:1),"resolution gates prune execution only");
            for(const auto& step:p)require(benchmark_afw_quality_allowed(step.shading,side),"ineligible AFW quality scheduled");
            require(benchmark_export_plan(full,true,modern).size()==(modern?39U:12U),"export layout retains omitted quality cells");
        }
    }
    Settings s;s.enabled=true;s.backend=1;BenchmarkScore score{59.4,35,1,1,true};score.display_hz=90;
    require(benchmark_2x_sufficient(s,score),"exact 66 percent skips 3X");
    score.fps=59.39;require(!benchmark_2x_sufficient(s,score),"below threshold keeps 3X");
    score.fps=90;score.valid=false;require(!benchmark_2x_sufficient(s,score),"failed trial cannot skip 3X");
    score.valid=true;score.display_hz=0;require(!benchmark_2x_sufficient(s,score),"unknown refresh keeps 3X");
    score.display_hz=90;s.backend=2;require(!benchmark_2x_sufficient(s,score),"later 0.3.0 does not gate early 3X");
    s.backend=3;s.triple_frame_gen=true;require(!benchmark_2x_sufficient(s,score),"3X cannot qualify itself");
    s.triple_frame_gen=false;s.enabled=false;require(!benchmark_2x_sufficient(s,score),"disabled reference cannot skip 3X");
}
// Drives the real shared controller with public runtime/telemetry reports. No
// game, headset, live profile, worker thread, or wall-clock benchmark wait.
void controller(bool modern,bool full,bool luke,unsigned side,double hz,
    double generated_fps,double afw_full=60,double afw_half=65,bool confirmed=true,bool csv=true) {
    auto saved=snapshot().desired;saved.advanced_mode=false;saved.benchmark_auto_pick=true;
    saved.benchmark_csv=csv;saved.afw_only=false;saved.nr_enabled=true;
    require(request(saved),"save isolated adaptive settings");saved=snapshot().desired;
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.luke_detected=luke;
    r.vdxr_runtime=!modern;r.vdxr_legacy=!modern;r.steamvr_runtime=modern;
    r.eye_long_side=side;r.generation_state=GenerationState::ready;report(r);
    require(start_benchmark(full,luke),"start adaptive controller");
    unsigned done{},triples{},half_refs{};bool after_half{};
    while(benchmark_active(snapshot().benchmark)) {
        const auto current=snapshot();const auto settings=current.desired;
        require(!settings.enabled || settings.frame_generation!=0 || settings.backend!=0,"Fidelity reached live controller");
        if(settings.triple_frame_gen)++triples;
        if(luke && settings.afw_shading_rate==4 && !settings.enabled){++half_refs;after_half=true;}
        else if(luke && !after_half)require(settings.afw_shading_rate==0,"whole 1x1 pass must precede half reference");
        const auto clock=GetTickCount64()-4000;
        r.telemetry={};r.afw_running=luke;report(r);report_interface_telemetry({});tick_benchmark(clock);
        require(snapshot().benchmark.phase==BenchmarkPhase::running,"adaptive step not applied");
        for(unsigned n=0;n<3;++n) {
            const auto previous=GetTickCount64();do{Sleep(1);}while(GetTickCount64()==previous);
            xrfg::InterfaceTelemetry m{};m.updated_ms=GetTickCount64();m.rates_valid=true;
            m.output_source=modern?xrfg::InterfaceSource::steamvr_delivery:xrfg::InterfaceSource::runtime_accepted_delivery;
            m.output_fps=settings.enabled?generated_fps:settings.afw_shading_rate==4?afw_half:afw_full;
            m.generating=settings.enabled && confirmed;m.cycle_estimated_valid=true;
            m.cycle_estimated_ms=settings.enabled?40:25;m.display_hz=hz;
            report(r);report_interface_telemetry(m);tick_benchmark(GetTickCount64()+n*300);
        }
        tick_benchmark(clock+kBenchmarkDurationMs);++done;require(done<100,"adaptive suite did not terminate");
    }
    const auto result=snapshot();require(result.benchmark.phase==BenchmarkPhase::complete,"early exit is a completed suite");
    if(!luke) {
        const bool skip=confirmed && hz>0 && generated_fps+1e-3>=hz*.66;
        require((triples==0)==skip,"live 3X pruning disagrees with measured gate");
        const auto original=benchmark_plan(full,false,modern).size();
        require(done==original-(skip?(modern?8:2):0),"pruning skipped unrelated route/fast/buffer trials");
        require(result.benchmark.count==done,"meter total does not reflect actual pruned plan");
        if(csv && skip) {
            std::ifstream f(std::filesystem::u8path(result.benchmark.csv_path));std::string text((std::istreambuf_iterator<char>(f)),{});
            require(text.find("Djules 3X")!=std::string::npos && text.find("n.a.")!=std::string::npos,"omitted X3 lost its export columns");
        }
    } else {
        const bool early=hz>0 && (generated_fps>=hz-1 || afw_full>=hz*.9 || (side>=3000 && afw_half>=hz*.9));
        const auto pass=modern?13U:4U;
        require(done==(early && side>=3000?pass+1:benchmark_execution_plan(full,true,modern,false,side).size()),"Luke early checkpoint or quality pruning wrong");
        require(half_refs==(side>=3000?1U:0U),"half reference missing or duplicated");
        if(hz>0 && afw_full>=hz*.9) {
            require(result.desired.afw_only && result.desired.afw_shading_rate==0,"full-quality AFW preference lost at early finish");
            require(std::string(result.benchmark.message).find("Restart the game")!=std::string::npos,"AFW-only restart notice lost");
        }
        if(hz>0 && afw_half>=hz*.9 && afw_full<hz*.9)
            require(result.desired.afw_only && result.desired.afw_shading_rate==4,"winning half-quality AFW not applied");
    }
    require(result.desired.nr_enabled && result.desired.luke_shared_swapchains==saved.luke_shared_swapchains,
        "suite changed unrelated Neural or INI-only sharing");
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-adaptive-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try {
        policy();std::filesystem::create_directories(root);
        require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"enable fixture benchmark");
        for(bool modern:{false,true})for(bool full:{false,true}) {
            controller(modern,full,false,3300,90,70);
            controller(modern,full,false,3300,90,50);
        }
        controller(true,false,false,3300,90,90,60,65,false);
        controller(true,false,false,3300,0,90);
        controller(true,false,false,3300,90,70,60,65,true,false);
        for(bool modern:{false,true}) {
            controller(modern,false,true,3376,90,90);
            controller(modern,false,true,4100,90,90);
            controller(modern,false,true,3376,90,90,81,89);
            controller(modern,false,true,3376,90,90,80,81);
            controller(modern,false,true,3376,90,85);
            controller(modern,false,true,2999,90,85);
            controller(modern,false,true,4100,0,85);
        }
        std::cout<<"Adaptive benchmark policy and real controller: 25 suites PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
