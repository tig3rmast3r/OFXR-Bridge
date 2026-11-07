#include "xrfg/benchmark_suite.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace xrfg::autonomous;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void report(RuntimeStatus& runtime){
    const auto state=snapshot();
    runtime.rife_execution_active=runtime.rife_tensorrt=true;runtime.generation_state=GenerationState::ready;
    runtime.applied_revision=state.revision;runtime.pacing_mode=state.desired.pacing_mode;
    runtime.buffer_capacity=state.desired.enabled && state.desired.pacing_mode!=0?
        (state.desired.increase_buffer?4:3):state.desired.increase_buffer?3:2;
    report_runtime(runtime);
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-reconfigure-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try{
        RuntimeStatus runtime{};runtime.process_id=GetCurrentProcessId();runtime.sessions=1;
        runtime.steamvr_runtime=true;runtime.vdxr_legacy=false;
        auto saved=snapshot().desired;saved.benchmark_csv=true;saved.benchmark_auto_pick=false;
        saved.nr_enabled=true;require(request(saved),"save isolated NR configuration");saved=snapshot().desired;
        report(runtime);require(start_benchmark(false),"start actual controller");
        unsigned count{};
        while(snapshot().desired.frame_generation!=4){
            require(++count<40,"reach first RIFE route");
            report(runtime);const auto clock=GetTickCount64()-4000;
            tick_benchmark(clock);tick_benchmark(clock+kBenchmarkDurationMs);
            require(benchmark_active(snapshot().benchmark),"preceding routes keep suite active");
        }
        report(runtime);tick_benchmark();
        const auto before=snapshot();require(before.benchmark.phase==BenchmarkPhase::running,"first RIFE trial begins");
        // The owner's V895 trace has an 8308ms gap at the first RIFE transition.
        // Real heartbeat age is used: no GPU work, fake heartbeat or process termination.
        Sleep(8310);tick_benchmark();
        const auto waiting=snapshot();
        require(waiting.benchmark.phase==BenchmarkPhase::applying && waiting.benchmark.index==before.benchmark.index,
            "live process with 8.3s stale heartbeat waits instead of aborting");
        require(waiting.desired==before.desired && waiting.benchmark.started_ms==0,
            "gap does not restore settings or keep old timing");
        const auto memory_request=take_benchmark_memory_request();
        require(memory_request==0,"stale trial has no outstanding memory request");
        report(runtime);const auto resumed_at=GetTickCount64();tick_benchmark();
        const auto resumed=snapshot();
        require(resumed.benchmark.phase==BenchmarkPhase::running && resumed.benchmark.index==before.benchmark.index &&
            resumed.benchmark.started_ms>=resumed_at && resumed.benchmark.started_ms>before.benchmark.started_ms,
            "resume starts a fresh complete settle/measurement interval on same route");
        const auto path=std::filesystem::u8path(resumed.benchmark.csv_path);
        std::ifstream file(path.parent_path()/(path.stem().wstring()+L"-details.csv"));
        const std::string csv{std::istreambuf_iterator<char>(file),{}};
        require(csv.find("game disconnected")==std::string::npos && csv.find("RIFE Medium")==std::string::npos,
            "gap produces neither a fake disconnect nor a completed RIFE measurement");
        stop_benchmark();tick_benchmark();require(snapshot().desired==saved,"Stop still restores exact original settings");
        runtime.heartbeat_ms=1000;
        require(benchmark_reconfiguration_grace(runtime,9308,true),"RIFE trace fits bounded grace");
        require(benchmark_reconfiguration_grace(runtime,30999,false) && !benchmark_reconfiguration_grace(runtime,31000,false),
            "ordinary/Fast reconfiguration has a hard 30s heartbeat-age bound");
        require(benchmark_reconfiguration_grace(runtime,65999,true) && !benchmark_reconfiguration_grace(runtime,66000,true),
            "RIFE cold worker startup has a hard 65s bound");
        runtime.sessions=0;require(!benchmark_reconfiguration_grace(runtime,9308,true),"closed session never receives rebuild grace");
        report(runtime);require(!start_benchmark(false),"closed session cannot start a new suite");
        runtime.sessions=1;report(runtime);require(start_benchmark(false),"restart isolated suite");
        runtime.sessions=0;report(runtime);tick_benchmark();
        require(snapshot().benchmark.phase==BenchmarkPhase::error && snapshot().desired==saved,"real session disconnect immediately restores settings");
        std::cout<<"Real-controller 8.3s RIFE/NR gap, fresh restart, unchanged route/settings, no false CSV, Stop, disconnect and bounded grace PASS\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
