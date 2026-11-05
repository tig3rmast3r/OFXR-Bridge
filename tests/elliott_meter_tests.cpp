#include "xrfg/elliott_meter.hpp"
#include "xrfg/fps_overlay_model.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <windows.h>
#include <algorithm>
using namespace xrfg;
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
int main(int argc,char** argv){try{
    FlipGesture flip;
    check(grip_upside_down({0,0,1,0})&&!grip_upside_down({}),"upside down gesture");
    check(!flip.observe(0,true,false)&&!flip.observe(249'999'999,true,false),"no wrist flash");
    check(flip.observe(250'000'000,true,false)&&flip.hand()==0,"left flip opens after delay");
    check(flip.observe(260'000'000,true,true)&&flip.hand()==0,"stable hand while both flipped");
    check(flip.observe(270'000'000,false,true)&&flip.hand()==1,"other flipped hand takes panel");
    check(flip.observe(280'000'000,false,false)&&flip.observe(779'999'999,false,false),"hide delay");
    check(!flip.observe(780'000'000,false,false),"upright hides panel");
    const auto pose=panel_pose_on_grip({{0,0,1,0},{1,2,3}},.2F);
    check(std::abs(pose.position.y-2.15F)<1e-5F,"upstream grip clearance");
    check(parse_overlay_position("elliott_controller")==FpsOverlayPosition::elliott_controller&&
        std::string(overlay_position_name(FpsOverlayPosition::elliott_bottom))=="elliott_bottom","new persistent modes");
    check(autonomous::sanitize([]{autonomous::Settings s;s.overlay_position=6;return s;}()).overlay_position==6,"mode6 retained");
    autonomous::Snapshot s;s.connected=true;s.desired.enabled=true;s.runtime.sessions=1;s.runtime.ready=1;
    s.runtime.generation_state=autonomous::GenerationState::ready;
    auto& m=s.runtime.telemetry;m.updated_ms=1000;m.rates_valid=true;
    m.output_source=InterfaceSource::steamvr_delivery;m.output_fps=90;m.game_fps=45;m.generated_fps=45;m.display_hz=90;
    m.ofxr_time_ms=9;m.cycle_length_ms=17;m.cycle_estimated_ms=28.1F;
    m.ofxr_time_valid=m.cycle_valid=m.cycle_estimated_valid=true;
    m.game_time_ms=9;m.generation_time_ms=5;m.delivery_time_ms=14.1F;m.stages_valid=true;m.game_time_valid=true;
    unsigned rows{};auto pixels=rasterize_elliott_meter(s,1000,false,false,&rows);
    auto gpu_sample=s;gpu_sample.runtime.gpu_usage={1000,81,true};
    gpu_sample.runtime.video_memory={1000,60,100,40,20,10,123,true,true};
    auto gpu_pixels=rasterize_elliott_meter(gpu_sample,1000,false,false);
    check(gpu_pixels!=pixels,"GPU and VRAM telemetry are drawn in the meter header");
    check(std::equal(pixels.begin()+1024*50,pixels.end(),gpu_pixels.begin()+1024*50),
        "adjacent GPU label does not move or cover the existing metric rows");
    check(pixels.size()==1024*512&&rows>250&&rows<=512,"two metric rows and diagnostics fit");
    check(pixels[0]==0&&pixels[100*1024+30]!=0,"rounded upstream canvas");
    for(int stage=0;stage<3;++stage) {
        auto changed=s;auto& v=changed.runtime.telemetry;
        if(stage==0)v.ofxr_time_ms=65;
        if(stage==1)v.cycle_length_ms=65;
        if(stage==2)v.cycle_estimated_ms=65;
        check(rasterize_elliott_meter(changed,1000,false,false)!=pixels,"each of the three corrected timing counters is drawn");
    }
    auto stale=rasterize_elliott_meter(s,4000,false,false);
    check(stale!=pixels,"stale metrics show unavailable");
    auto bench=s;bench.runtime.process_id=7;auto& b=bench.benchmark;
    b.game_pid=7;b.count=51;b.index=0;b.phase=autonomous::BenchmarkPhase::applying;
    check(elliott_benchmark_text(bench,1000)=="Test 1/51 | applying settings","applying settings has progress without a fictitious countdown");
    b.phase=autonomous::BenchmarkPhase::running;b.started_ms=1000;
    check(elliott_benchmark_text(bench,2000)=="Test 1/51 | settling | 6.5s left","settling countdown uses the applied trial start");
    check(elliott_benchmark_text(bench,3500)=="Test 1/51 | measuring | 5.0s left","measuring phase starts exactly at 2.5 seconds");
    check(elliott_benchmark_text(bench,9500)=="Test 1/51 | measuring | 0.0s left","late controller never produces negative remaining time");
    unsigned bench_rows{};auto bench_pixels=rasterize_elliott_meter(bench,1000,false,false,&bench_rows);
    check(bench_rows>rows && bench_rows<=512 && bench_pixels!=pixels,"benchmark progress adds a visible row inside the existing surface");
    b.index=64;b.count=65;
    check(elliott_benchmark_text(bench,3500)=="Test 65/65 | measuring | 5.0s left","full matrix index64 is displayed as test65");
    b.phase=autonomous::BenchmarkPhase::complete;bench.desired.afw_only=true;
    check(elliott_benchmark_text(bench,3500)=="Tests complete | restart game: Frame Reconstruction only","AFW-only restart warning remains visible in the headset meter");
    b.phase=autonomous::BenchmarkPhase::stopped;
    check(elliott_benchmark_text(bench,3500).empty(),"Stop removes the progress row");
    b.phase=autonomous::BenchmarkPhase::running;b.game_pid=8;
    check(elliott_benchmark_text(bench,3500).empty(),"another game cannot show stale benchmark progress");
    bench.desired.afw_only=false;b.game_pid=7;b.index=0;b.count=51;
    auto active=s;
    const auto now=GetTickCount64();
    active.runtime.telemetry.updated_ms=active.runtime.heartbeat_ms=now;
    active.runtime.applied_revision=active.revision;
    active.runtime.telemetry.generating=true;
    active.desired.afw_mode=3; // Non-Luke default selection must not be an error.
    const unsigned green=0xff8ee05c, red=0xff6262ff;
    const auto good=rasterize_elliott_meter(active,now,false,false);
    const auto count=[](const auto& p,unsigned color){return std::count(p.begin(),p.end(),color);};
    const auto count_top=[](const auto& p,unsigned color){return std::count(p.begin(),p.begin()+std::min<std::size_t>(p.size(),1024*206),color);};
    check(count(good,green)>100 && count(good,red)==0,"working OFXR label and counters must be green");
    auto optimizing=active;optimizing.desired.frame_generation=4;
    optimizing.runtime.generation_mode=4;optimizing.runtime.rife_optimizing=true;
    constexpr unsigned yellow=0xff48c0ff;
    check(autonomous::interface_state_text(optimizing)=="Optimizing, please wait",
        "RIFE optimization uses the exact shared desktop/headset status");
    const auto optimizing_pixels=rasterize_elliott_meter(optimizing,now,false,false);
    check(count(optimizing_pixels,yellow)>100 && count(optimizing_pixels,green)>100,
        "optimization warning does not turn healthy fallback frame counters red");
    auto warming=optimizing;warming.runtime.ready=0;
    warming.runtime.generation_state=autonomous::GenerationState::waiting;
    warming.runtime.telemetry={};
    check(autonomous::interface_state_text(warming)=="Optimizing, please wait" &&
        count(rasterize_elliott_meter(warming,now,false,false),yellow)>100,
        "optimization remains visible before any ready frame or valid FPS exists");
    auto optimization_paused=optimizing;
    optimization_paused.runtime.generation_state=autonomous::GenerationState::paused;
    check(autonomous::interface_state_text(optimization_paused)=="Paused" &&
        count(rasterize_elliott_meter(optimization_paused,now,false,false),yellow)==0,
        "Pause takes priority over an in-progress optimization receipt");
    auto optimization_pending=optimizing;optimization_pending.runtime.pending=1;
    check(autonomous::interface_state_text(optimization_pending)=="Applying settings" &&
        count(rasterize_elliott_meter(optimization_pending,now,false,false),yellow)==0,
        "pending settings keep their normal header while an engine compiles");
    auto optimized=optimizing;optimized.runtime.rife_optimizing=false;
    auto failed=optimized;failed.runtime.rife_error=1;failed.runtime.rife_execution_active=true;
    check(autonomous::interface_state_text(failed)=="RIFE optimization error" &&
        autonomous::interface_status_text(failed).find("timed out")!=std::string::npos,
        "failed compilation reports its cause instead of silently labelling Torch healthy");
    const auto failed_pixels=rasterize_elliott_meter(failed,now,false,false);
    check(count_top(failed_pixels,red)>100 && count_top(failed_pixels,green)==0,
        "RIFE timeout turns the meter header and counters red even while fallback generates");
    auto failed_other=failed;failed_other.desired.frame_generation=0;
    check(count_top(rasterize_elliott_meter(failed_other,now,false,false),red)==0,
        "old RIFE failure cannot turn another route red");
    check(autonomous::interface_state_text(optimized)!="Optimizing, please wait" &&
        count(rasterize_elliott_meter(optimized,now,false,false),yellow)==0,
        "completed optimization restores the normal framework header");
    auto other_route=optimizing;other_route.desired.frame_generation=0;
    other_route.runtime.generation_mode=0;
    check(autonomous::interface_state_text(other_route)!="Optimizing, please wait" &&
        count(rasterize_elliott_meter(other_route,now,false,false),yellow)==0,
        "a stale RIFE optimization receipt cannot label another route");
    auto optimization_disabled=optimizing;optimization_disabled.desired.enabled=false;
    check(autonomous::interface_state_text(optimization_disabled)!="Optimizing, please wait" &&
        count(rasterize_elliott_meter(optimization_disabled,now,false,false),yellow)==0,
        "disabling frame generation clears the optimization header");
    auto optimization_disconnected=optimizing;optimization_disconnected.connected=false;
    check(autonomous::interface_state_text(optimization_disconnected)!="Optimizing, please wait" &&
        count(rasterize_elliott_meter(optimization_disconnected,now,false,false),yellow)==0,
        "a disconnected runtime cannot keep an optimization header visible");
    check(autonomous::interface_status_text(active).find("OSR: Off")!=std::string::npos,
        "default OSR outside Luke must be not applicable");
    auto idle=active;idle.runtime.telemetry.generating=false;
    check(count(rasterize_elliott_meter(idle,now,false,false),red)>100,"non-generating meter red");
    auto disabled=active;disabled.desired.enabled=false;
    check(count_top(rasterize_elliott_meter(disabled,now,false,false),green)==0,"disabled generation header not green; valid reconstruction state is independent");
    auto neural=active;neural.desired.nr_enabled=true;
    check(count(rasterize_elliott_meter(neural,now,false,false),red)>100,"Neural on but idle red");
    const auto neural_idle_red=count(rasterize_elliott_meter(neural,now,false,false),red);
    auto no_model=neural;no_model.runtime.nr_model_missing=true;
    const auto no_model_meter=rasterize_elliott_meter(no_model,now,false,false);
    check(autonomous::interface_status_text(no_model).find("Neural Render: Model missing")!=std::string::npos,
        "a missing NR model is named in the status");
    check(count(no_model_meter,red)>0 && count_top(no_model_meter,red)==0 &&
        count_top(no_model_meter,green)==neural_idle_red,"a missing NR model is one red line, the numbers keep their colour");
    neural.runtime.nr_running=1;
    check(count(rasterize_elliott_meter(neural,now,false,false),red)==0,"active Neural green");
    auto luke=active;luke.runtime.luke_detected=1;
    check(count(rasterize_elliott_meter(luke,now,false,false),red)>100,"requested Luke AFW idle red");
    luke.runtime.afw_running=1;
    check(count(rasterize_elliott_meter(luke,now,false,false),red)==0,"active Luke AFW green");
    auto afw_only=luke;afw_only.desired.enabled=false;afw_only.desired.luke_osr=0;
    afw_only.runtime.telemetry.generating=false;afw_only.runtime.generation_state=autonomous::GenerationState::paused;
    afw_only.runtime.provider_error=5; // a disabled OFXR failure is irrelevant to AFW
    check(autonomous::interface_state_text(afw_only)=="AFW" &&
        !autonomous::interface_generation_active(afw_only),"AFW-only label identifies actual work without claiming OFXR generation");
    check(count(rasterize_elliott_meter(afw_only,now,false,false),green)>100 &&
        count(rasterize_elliott_meter(afw_only,now,false,false),red)==0,"AFW alone is green even when OFXR is disabled/paused");
    auto osr_only=afw_only;osr_only.desired.luke_osr=2;
    check(autonomous::interface_state_text(osr_only)=="OSR" && !autonomous::interface_generation_active(osr_only) &&
        count(rasterize_elliott_meter(osr_only,now,false,false),green)>100,
        "OSR-only reconstruction is active without claiming OFXR frame generation");
    afw_only.desired.enabled=true;afw_only.runtime.pacing_mode=autonomous::kAfwOnlyPacingMode;
    check(count(rasterize_elliott_meter(afw_only,now,false,false),red)==0,"startup AFW-only bypass is also green");
    afw_only.runtime.afw_running=false;
    check(count_top(rasterize_elliott_meter(afw_only,now,false,false),green)==0,"idle AFW-only never falsely reports green");
    afw_only.runtime.afw_running=true;
    check(count_top(rasterize_elliott_meter(afw_only,now+2100,false,false),green)==0,"stale AFW-only receipt never reports green");
    luke.desired.afw_mode=0;luke.runtime.afw_running=0;
    check(count(rasterize_elliott_meter(luke,now,false,false),red)==0,"AFW off neutral");
    check(count_top(rasterize_elliott_meter(active,now+2100,false,false),green)==0,"stale meter not green");
    for(int source=0;source<3;++source) {
        auto source_state=active;source_state.desired.afw_mode=source?3:0;source_state.desired.luke_osr=2;
        source_state.runtime.luke_detected=source==1;source_state.runtime.afw_running=true;
        const auto frame=rasterize_elliott_meter(source_state,now,false,false);
        const auto first=frame.begin()+1024*214,last=frame.begin()+1024*236;
        check(std::count(first,last,green)>10,"Off, Luke Ross and Other source rows are all green");
    }
    check(rasterize_elliott_meter(s,1000,false,true)!=pixels,"UNORM linear palette conversion");
    check(rasterize_elliott_meter(s,1000,true,false)!=pixels,"BGRA channels supported");
    if(argc>1){auto preview=rasterize_elliott_meter(gpu_sample,1000,false,false,&bench_rows);
        std::ofstream out(argv[1],std::ios::binary);out<<"P6\n1024 "<<bench_rows<<"\n255\n";
        for(unsigned i=0;i<1024*bench_rows;++i){char rgb[]{char(preview[i]),char(preview[i]>>8),char(preview[i]>>16)};out.write(rgb,3);}}
    std::cout<<"Meter supplied metrics, palette, persistence, gesture and placement passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
