#include "xrfg/benchmark_suite.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
using namespace xrfg::autonomous;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void report(RuntimeStatus& r){
    const auto s=snapshot();r.applied_revision=s.revision;r.pacing_mode=s.desired.pacing_mode;
    r.afw_shading_rate=s.desired.afw_shading_rate;r.osr_mode=s.desired.luke_osr;r.osr_lower_quality=s.desired.luke_osr_lower_quality;
    r.buffer_capacity=r.vdxr_legacy?0:r.steamvr_runtime && s.desired.enabled && s.desired.pacing_mode!=0 ? (s.desired.increase_buffer?4:3):s.desired.increase_buffer?3:2;
    report_runtime(r);
}
std::string read(const std::filesystem::path& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
void controller(bool modern,bool full,bool luke,double base,double two,double three,double four,
    unsigned cap=4,bool confirmed=true,bool optimize=false,bool unstable=false,bool only=false,double hz=90){
    auto saved=snapshot().desired;saved.advanced_mode=false;saved.benchmark_auto_pick=true;
    saved.benchmark_csv=true;saved.afw_only=false;saved.nr_enabled=true;saved.afw_mode=3;
    require(request(saved),"save isolated settings");saved=snapshot().desired;
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.luke_detected=luke;
    r.vdxr_runtime=!modern;r.vdxr_legacy=!modern;r.steamvr_runtime=modern;r.eye_long_side=3300;
    r.rife_max_multiplier=modern?cap:(cap>3?3:cap);r.dlssg_preset_request=1;r.generation_state=GenerationState::ready;report(r);
    require(start_benchmark(full,luke),"start real controller");
    unsigned done{},triples{};std::array<unsigned,5> multipliers{};bool injected=false;
    while(benchmark_active(snapshot().benchmark)){
        auto state=snapshot();const auto settings=state.desired;const bool rife=settings.frame_generation==4;
        if(rife){
            ++multipliers[settings.rife_multiplier];require(settings.rife_model==3 && !settings.motion_vectors,"RIFE4.4 never asks for vectors");
            require(settings.rife_resolution==18 || settings.rife_resolution==21 || settings.rife_resolution==25,"RIFE Medium/High/VeryHigh only");
            if(settings.rife_multiplier==3)require(multipliers[2]==(modern?24:6),"all2X candidates must finish before3X");
            if(settings.rife_multiplier==4)require(multipliers[3]==24,"all3X candidates must finish before4X");
        }
        if(settings.triple_frame_gen)++triples;
        auto clock=GetTickCount64()-4000;r.telemetry={};r.afw_running=luke || only;
        r.rife_execution_active=rife && confirmed;r.pending=0;report(r);report_interface_telemetry({});
        if(optimize && rife && !injected){
            r.rife_optimizing=true;report(r);const auto index=state.benchmark.index;
            tick_benchmark(state.benchmark.requested_ms+700000);
            require(snapshot().benchmark.phase==BenchmarkPhase::applying && snapshot().benchmark.index==index,"optimization must wait without a setup timeout or measured trial");
            r.rife_optimizing=false;report(r);
        }
        tick_benchmark(clock);require(snapshot().benchmark.phase==BenchmarkPhase::running,"trial starts only after application");
        unsigned sample_index{};
        const auto sample=[&](double fps){
            const auto previous=GetTickCount64();do{Sleep(1);}while(GetTickCount64()==previous);
            xrfg::InterfaceTelemetry m{};m.updated_ms=GetTickCount64();m.rates_valid=true;
            m.output_source=modern?xrfg::InterfaceSource::steamvr_delivery:xrfg::InterfaceSource::runtime_accepted_delivery;
            m.output_fps=float(fps);m.generating=settings.enabled;m.cycle_estimated_valid=true;
            m.cycle_estimated_ms=rife?42:settings.enabled?40:25;m.display_hz=hz;
            r.telemetry=m;r.motion_source=full && settings.frame_generation>0 && settings.frame_generation<4?1:0;
            report(r);report_interface_telemetry(m);tick_benchmark(GetTickCount64()+sample_index++*300);
        };
        if(optimize && rife && !injected){
            sample(1);r.rife_optimizing=true;report(r);tick_benchmark(GetTickCount64());
            require(snapshot().benchmark.phase==BenchmarkPhase::applying,"late optimization discards partial samples");
            r.rife_optimizing=false;report(r);clock=GetTickCount64()-4000;tick_benchmark(clock);sample_index=0;injected=true;
        }
        const auto fps=!settings.enabled?(only?81:60):rife?(settings.rife_multiplier==2?two:settings.rife_multiplier==3?three:four):base;
        for(unsigned n=0;n<3;++n)sample(unstable && (!rife || settings.rife_multiplier==2) && n==1?60:fps);
        tick_benchmark(clock+kBenchmarkMemoryRequestMs);const auto revision=take_benchmark_memory_request();
        require(revision==snapshot().benchmark.revision && !take_benchmark_memory_request(),"one memory receipt per applied trial");
        BenchmarkMemory memory{};memory.revision=revision;memory.updated_ms=clock+kBenchmarkMemoryRequestMs;
        memory.vram_valid=memory.ram_valid=true;memory.vram_bytes=1048576ULL*(100+done);memory.ram_working_set_bytes=2097152;
        report_benchmark_memory(memory);tick_benchmark(clock+kBenchmarkDurationMs);++done;
        require(done<200,"adaptive suite must terminate");
    }
    const auto result=snapshot();require(result.benchmark.phase==BenchmarkPhase::complete,"suite completes");
    require(multipliers[2]==(modern?24:6),"both RIFE2X qualities and policies are always measured");
    const bool reached2=!unstable && (base>=hz || (confirmed && two>=hz));
    const bool higher=cap>=3;
    require((multipliers[3]>0)==(higher && !reached2),"3X gate follows stable preceding results and session capability");
    require(multipliers[4]==0,"temporary suite never runs4X");
    require(result.desired.nr_enabled && result.desired.overlay_enabled==saved.overlay_enabled,"unrelated settings retained");
    if(only)require(result.desired.afw_only && !result.desired.enabled,"confirmed reconstruction-only at90percent overrides full refresh in any suite");
    else if(!confirmed)require(result.desired.frame_generation!=4,"unconfirmed RIFE fallback cannot win");
    else if(base<hz && (two>=hz || (higher && three>=hz)))
        require(result.desired.frame_generation==4,"stable RIFE refresh wins over sub-refresh alternatives");
    const auto layout_path=std::filesystem::u8path(result.benchmark.csv_path);const auto layout=read(layout_path);
    require(layout.find("RIFE Medium 2X")!=std::string::npos && layout.find("RIFE High 2X")!=std::string::npos,"CSV distinguishes quality");
    require(layout.find("4x4")==std::string::npos,"AFW4x4 is retired");
    if(luke)require(layout.find("AFW 1x1")==std::string::npos && layout.find("AFW 2x2")==std::string::npos &&
        layout.find("OSR / only")!=std::string::npos && layout.find("OSR cheap")!=std::string::npos,"CSV distinguishes OSR normal/cheap without AFW");
    require(std::count(layout.begin(),layout.end(),'\n')==11,"CSV retains six statistics and four memory rows");
    if(!multipliers[3])require(layout.find("RIFE Very High 3X")!=std::string::npos && layout.find("n.a.")!=std::string::npos,"pruned3X tiers retain empty export cells");
    require(layout.find("4X")==std::string::npos,"no4X columns in temporary suite");
    const auto details=read(layout_path.parent_path()/(layout_path.stem().wstring()+L"-details.csv"));
    if(multipliers[3])require(details.find(",3,0,")!=std::string::npos,"details record the actual RIFE multiplier");
    require(r.dlssg_preset_request==1,"suite never changes the startup NVIDIA preset");
    std::cout<<"suite modern="<<modern<<" full="<<full<<" luke="<<luke<<" trials="<<done<<" RIFE="<<multipliers[2]<<'/'<<multipliers[3]<<'/'<<multipliers[4]<<" PASS\n";
}
void setup_edge_cases(bool optimizing){
    auto saved=snapshot().desired;saved.benchmark_auto_pick=true;saved.benchmark_csv=true;
    require(request(saved),"save setup-edge settings");saved=snapshot().desired;
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.steamvr_runtime=true;
    r.vdxr_legacy=false;r.rife_max_multiplier=4;report(r);
    require(start_benchmark(false),"start setup-edge suite");
    while(benchmark_active(snapshot().benchmark) && snapshot().desired.frame_generation!=4){
        report(r);const auto now=GetTickCount64()-4000;tick_benchmark(now);tick_benchmark(now+kBenchmarkDurationMs);
    }
    require(benchmark_active(snapshot().benchmark),"reach RIFE setup");
    r.pending=optimizing?0:1;r.rife_optimizing=optimizing;report(r);
    tick_benchmark(snapshot().benchmark.requested_ms+31000);
    if(optimizing){
        require(snapshot().benchmark.phase==BenchmarkPhase::applying,"optimization never consumes normal setup timeout");
        stop_benchmark();tick_benchmark();
        require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==saved,"Stop remains available while optimizing");
    }else{
        const auto result=snapshot();
        require(result.benchmark.phase==BenchmarkPhase::complete && result.desired==saved,"unavailable RIFE skips its tier without destroying ordinary results or restoring a partial winner");
        const auto path=std::filesystem::u8path(result.benchmark.csv_path);
        require(read(path.parent_path()/(path.stem().wstring()+L"-details.csv")).find("RIFE setup unavailable / skipped")!=std::string::npos,"RIFE timeout is explicit in CSV");
    }
}
void cancellation(){
    RuntimeStatus r{};r.process_id=GetCurrentProcessId();r.sessions=1;r.steamvr_runtime=true;r.vdxr_legacy=false;r.rife_max_multiplier=4;
    const auto saved=snapshot().desired;report(r);require(start_benchmark(false),"start Stop case");stop_benchmark();tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==saved,"Stop restores original settings");
    report(r);require(start_benchmark(false),"start manual edit case");auto edited=snapshot().desired;edited.nr_enabled=!edited.nr_enabled;
    require(request(edited),"manual settings override");tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired.nr_enabled==edited.nr_enabled,"manual edit wins");
    report(r);require(start_benchmark(false),"start disconnect case");r.sessions=0;report(r);tick_benchmark();
    require(snapshot().benchmark.phase==BenchmarkPhase::error,"disconnect never applies a partial winner");
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-adaptive-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try{
        for(bool modern:{false,true})for(bool full:{false,true})for(bool luke:{false,true})
            controller(modern,full,luke,70,90,90,90);
        controller(true,false,false,70,85,90,90,4,true,true);
        controller(true,true,false,70,85,88,90);
        controller(true,false,true,70,85,88,90);
        controller(true,false,false,90,88,90,90);
        controller(true,false,false,90,90,90,90,4,true,false,true);
        controller(true,false,false,70,90,90,90,4,false);
        controller(true,false,false,70,80,90,90,2);
        controller(true,false,false,70,90,90,90,4,true,false,false,true);
        controller(false,false,false,95,105,120,120,3,true,false,false,false,120);
        controller(false,false,true,95,120,120,120,3,true,false,false,false,120);
        setup_edge_cases(false);setup_edge_cases(true);
        cancellation();std::cout<<"Adaptive RIFE live-controller, stable refresh, optimization, CSV, capability and cancellation PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
