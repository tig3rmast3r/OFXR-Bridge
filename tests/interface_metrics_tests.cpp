#include "xrfg/interface_metrics.hpp"
#include "xrfg/frame_production.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/autonomous_control.hpp"
#include "xrfg/fps_overlay_model.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <fstream>
using namespace xrfg;
using namespace xrfg::autonomous;
namespace {
unsigned checks{};
void check(bool pass, const char* text) { ++checks; if (!pass) throw std::runtime_error(text); }
#include "timing_cohorts_test.inc"
void buffer_preferences() {
    check(!Settings{}.increase_buffer,"new profiles default to two private deliveries");
    auto base=snapshot().desired;auto larger=base;larger.increase_buffer=true;
    check(request(larger) && snapshot().desired.increase_buffer,"larger buffer is a normal per-game preference");
    std::ifstream file(configuration_path());const std::string text(std::istreambuf_iterator<char>(file),{});
    file.close(); // Windows atomic profile replacement needs the reader closed.
    check(text.find("increase_buffer=1")!=std::string::npos,"buffer preference persists in existing INI");
    select_generation(larger,0,1);check(larger.increase_buffer,"changing framework does not replace buffer preference");
    check(request(base) && snapshot().desired.increase_buffer==base.increase_buffer,"buffer preference restores without touching other settings");
}
void timing() {
    InterfaceTimingWindow window;
    const InterfaceFrameStamp frame{1, 100'000'000, 80'000'000};
    check(!window.submitted(frame, 110'000'000, false,true,false,true), "failed submit does not consume identity");
    check(!window.submitted(frame, 110'000'000, true,false,false,true), "empty real excluded");
    check(!window.submitted(frame, 110'000'000, true,true,true,true), "synthetic excluded");
    check(!window.submitted(frame, 110'000'000, true,true,false,false), "repeat/native copied output excluded");
    check(window.submitted(frame, 150'000'000, true,true,false,true, 145'000'000), "pipeline hold retained on first successful real");
    check(!window.submitted(frame, 160'000'000, true,true,false,true), "identity cannot double count");
    InterfaceTelemetry m{}; window.snapshot(160'000'000,m);
    check(m.delay_valid && m.ofxr_delay_ms == 50 && m.cpu_valid && m.cpu_render_ms == 20, "real entry and begin own their exact interval");
    check(m.presentation_valid && m.presentation_ms == -5, "late presentation retains negative sign");
    check(window.submitted({2,180'000'000,0}, 200'000'000,true,true,false,true), "missing clock converter remains eligible for OFXR time");
    window.snapshot(210'000'000,m);
    check(m.ofxr_delay_ms == 35 && m.presentation_ms == -5 && m.cpu_render_ms == 20, "median ignores unavailable sources independently");
    window.snapshot(1'210'000'001,m);
    check(!m.delay_valid && !m.presentation_valid && !m.cpu_valid, "one-second sample window expires");
    check(!window.submitted(frame, 1'220'000'000,true,true,false,true), "expired frame identity still cannot be re-counted");
    check(!window.submitted({3,300,0},299,true,true,false,true), "clock regression rejected");
    check(!window.submitted({4,0,0},400,true,true,false,true), "no entry clock yields no latency");
    LARGE_INTEGER qpc{}; QueryPerformanceCounter(&qpc);
    const auto converted=interface_qpc_ns(qpc.QuadPart), now=interface_clock_ns();
    check(converted>0 && now>=converted && now-converted<50'000'000, "converted predicted display uses same QPC domain");
    FpsCounter counter;
    counter.submitted(100'000'000,false); counter.submitted(200'000'000,true);
    counter.submitted(300'000'000,true,false); counter.submitted(400'000'000,false,false);
    const auto fps=counter.snapshot(1'000'000'000);
    check(fps.sample_valid && fps.real_fps>0 && fps.generated_fps>0 && fps.submitted_fps==fps.real_fps+fps.generated_fps, "real/generated split excludes repeats and native bypass copies");
}
void complete_cycle() {
    auto cycle=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);
    cycle->call(100'000'000,110'000'000); // Wait includes runtime/OFXR waits.
    cycle->call(115'000'000,117'000'000); // Begin; game gaps remain outside OFXR time.
    cycle->call(120'000'000,124'000'000);
    cycle->call(122'000'000,126'000'000); // Concurrent eyes: union, not sum.
    cycle->call(140'000'000,165'000'000); // End includes tail after actual submission.
    InterfaceTimingWindow window;
    InterfaceFrameStamp frame{10,140'000'000,117'000'000,cycle};
    check(window.submitted(frame,150'000'000,true,true,false,true,180'000'000,155'000'000), "real acceptance retains same Wait ticket");
    InterfaceTelemetry out{};window.snapshot(160'000'000,out);
    check(!out.cycle_valid && !out.ofxr_time_valid, "producer still inside End is not a completed sample");
    cycle->producer_returned(165'000'000);window.snapshot(170'000'000,out);
    check(out.cycle_valid && out.cycle_length_ms==65 && out.ofxr_time_ms==43, "production plus waits include post-submit producer tail; eye overlaps counted once");
    check(out.cycle_source==InterfaceSource::frame_cycle_qpc && out.ofxr_time_source==InterfaceSource::producer_call_qpc && !out.gpu_valid, "CPU boundary never claims GPU completion");
    auto next=std::make_shared<InterfaceCycle>(151'000'000,151'000'000);
    next->call(151'000'000,171'000'000);next->producer_returned(195'000'000);
    window.snapshot(180'000'000,out);
    check(out.cycle_length_ms==65 && out.ofxr_time_ms==43, "overlapping next Wait cannot overwrite retained previous frame");
    auto held=std::make_shared<InterfaceCycle>(200'000'000,200'000'000);
    held->call(200'000'000,205'000'000);held->call(240'000'000,245'000'000);
    held->producer_returned(245'000'000);
    InterfaceTimingWindow asynchronous;
    check(asynchronous.submitted({11,240'000'000,205'000'000,held},280'000'000,true,true,false,true,0,285'000'000), "held real accepted after producer returns");
    asynchronous.snapshot(290'000'000,out);
    check(out.cycle_length_ms==85 && out.ofxr_time_ms==10, "director retention contributes full age even outside producer calls");
    check(!asynchronous.submitted({11,240'000'000,205'000'000,held},300'000'000,true,true,false,true), "held real cannot double count");
    asynchronous.snapshot(1'290'000'001,out);
    check(!out.cycle_valid && !out.ofxr_time_valid, "new counters expire after one second");
    InterfaceCycle overflow(1);
    for(std::int64_t i=0;i<65;++i)overflow.call(1+i*2,2+i*2);
    overflow.producer_returned(200);float a{},b{};
    check(!overflow.read(201,a,b), "bounded interval overflow is N/A rather than an invented partial total");
    InterfaceCycle regression(100);regression.call(99,105);regression.producer_returned(200);
    check(!regression.read(201,a,b), "clock/identity regression has no timing");
}
void native_delivery_cycle() {
    auto c=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);
    c->call(100'000'000,103'000'000);c->producer_returned(109'000'000);
    c->synthetic_accepted(103'500'000,11'111'111);
    float ms{},calls{};
    check(c->read_minimum_cycle(109'000'000,ms) && ms<15,"reproduce old virtual15ms estimate");
    auto generated=std::make_shared<InterfaceDeliveryReceipt>(true);
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);
    c->attach_native_delivery(generated);c->attach_native_delivery(real);
    InterfaceTimingWindow window;InterfaceFrameStamp stamp{607,105'000'000,100'000'000,c};
    check(window.submitted(stamp,109'000'000,true,true,false,true,110'000'000,109'000'000),"retain accepted virtual identity while native delivery is pending");
    InterfaceTelemetry pending;window.snapshot(110'000'000,pending);
    check(!pending.cycle_valid && !pending.cycle_estimated_valid && !pending.delay_valid,"queued virtual success cannot publish partial native timing");
    generated->completed(true,145'000'000,146'000'000,155'000'000);
    real->completed(true,156'111'111,157'000'000,166'111'111);
    check(c->read(109'000'000,calls,ms) && calls==3 && ms==57,"native full cycle includes47ms beyond virtual return, producer call total unchanged");
    check(c->read_minimum_cycle(109'000'000,ms),"actual real prediction is valid independently of generation confirmation");
    generated->synthetic_confirmed=true;
    check(c->read_minimum_cycle(109'000'000,ms) && ms==57,"native predicted real slot counted once, no additional refresh");
    InterfaceTelemetry actual;window.snapshot(160'000'000,actual);
    check(actual.cycle_source==InterfaceSource::native_frame_cycle_qpc && actual.cycle_estimated_source==InterfaceSource::native_minimum_cycle_qpc,"physical observations have distinct source labels");
    check(std::abs(actual.ofxr_delay_ms-51.111111F)<.001F && std::abs(actual.presentation_ms-10.F)<.001F,"delay and presentation use real native handoff rather than virtual submission");
    auto failed=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);failed->producer_returned(109'000'000);
    auto rejected=std::make_shared<InterfaceDeliveryReceipt>(false);failed->attach_native_delivery(rejected);
    rejected->completed(false,150'000'000,151'000'000,160'000'000);
    check(!failed->read(109'000'000,calls,ms),"failed or empty physical End never becomes a delivered-cycle sample");
    auto no_clock=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);no_clock->producer_returned(109'000'000);
    auto no_clock_real=std::make_shared<InterfaceDeliveryReceipt>(false);no_clock->synthetic_accepted(103'500'000,11'111'111);no_clock->attach_native_delivery(generated);no_clock->attach_native_delivery(no_clock_real);
    no_clock_real->completed(true,156'111'111,157'000'000,0);
    check(no_clock->read(109'000'000,calls,ms) && ms==57 && no_clock->read_minimum_cycle(109'000'000,ms) && ms==57,"native handoff remains measurable without inventing an unknown display target");
    auto triple=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);triple->producer_returned(109'000'000);
    auto second=std::make_shared<InterfaceDeliveryReceipt>(true);second->synthetic_confirmed=true;second->completed(true,156'111'111,157'000'000,166'111'111);
    auto triple_real=std::make_shared<InterfaceDeliveryReceipt>(false);triple_real->completed(true,167'222'222,168'000'000,177'222'222);
    triple->synthetic_accepted(103'500'000,11'111'111);
    triple->attach_native_delivery(generated);triple->attach_native_delivery(second);triple->attach_native_delivery(triple_real);
    check(triple->read_minimum_cycle(109'000'000,ms) && ms==68,"3X uses actual real target, two delivered synthetic slots are not added twice");
    window.snapshot(1'200'000'000,actual);check(!actual.cycle_valid,"native observations expire normally");
}
void stage_partition_and_baseline() {
    auto c=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);c->producer_returned(125'000'000);
    c->call(100'000'000,105'000'000);c->call(110'000'000,112'000'000);
    c->call(111'000'000,114'000'000); // overlapping eye calls count once
    c->call(120'000'000,125'000'000); // End preparation is outside the game phase
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);
    c->attach_native_delivery(real);real->completed(true,130'000'000,131'000'000,145'000'000,129'000'000);
    InterfaceTimingWindow window;
    check(window.submitted({608,120'000'000,105'000'000,c},123'000'000,true,true,false,true,0,125'000'000),"disabled baseline source identity");
    InterfaceTelemetry m;window.snapshot(150'000'000,m);
    check(m.cycle_estimated_valid && m.cycle_estimated_ms==31 && m.stages_valid,"baseline cycle needs no synthetic frame");
    check(m.game_time_valid && m.game_time_ms==20 && m.generation_time_ms==0 && m.delivery_time_ms==11,"disabled baseline partitions the engine source handoff without relabeling pre-handoff waits");
    auto generated=std::make_shared<InterfaceDeliveryReceipt>(true);c->attach_native_delivery(generated);
    generated->completed(true,127'000'000,128'000'000,138'000'000,126'000'000);c->synthetic_accepted(123'000'000,10'000'000);
    window.snapshot(150'000'000,m);check(!m.stages_valid && m.cycle_estimated_valid,"unconfirmed generated pixels cannot supply a preparation boundary");
    generated->synthetic_confirmed=true;window.snapshot(150'000'000,m);
    check(m.stages_valid && m.game_time_ms==20 && m.generation_time_ms==6 && m.delivery_time_ms==11,"native preparation and delivery partition one exact real cycle");
    auto extra=std::make_shared<InterfaceDeliveryReceipt>(true);extra->synthetic_confirmed=true;
    extra->completed(true,129'000'000,130'000'000,140'000'000,128'000'000);c->attach_native_delivery(extra);
    window.snapshot(150'000'000,m);
    check(m.stages_valid && m.generation_time_ms==6 && m.cycle_estimated_ms==47,"3X first-output preparation and actual real target are not added twice");
    auto legacy=std::make_shared<InterfaceCycle>(100'000'000,100'000'000);legacy->producer_returned(125'000'000);legacy->real_prediction(140'000'000);
    float estimated{},game{},gen{},delivery{};
    check(legacy->read_minimum_cycle(125'000'000,estimated) && estimated==25 &&
        legacy->stages(120'000'000,125'000'000,estimated,game,gen,delivery) && game==20 && gen==0 && delivery==5,
        "nonprivate baseline uses a converted actual real prediction, not a synthetic slot constant");
    legacy->real_prediction(0);check(legacy->read_minimum_cycle(125'000'000,estimated) && estimated==25,"runtime completion needs no display prediction");
    generated->prepared_ns=0;window.snapshot(150'000'000,m);
    check(!m.stages_valid && m.cycle_estimated_valid,"missing readiness cannot fabricate generation duration");
    window.snapshot(1'200'000'000,m);check(!m.stages_valid && !m.cycle_estimated_valid,"all four measurements expire");
}
#include "minimum_cycle_tests.inc"
void estimated_cycle() {
    auto c=std::make_shared<InterfaceCycle>(100'000'000);
    float ms{};c->producer_returned(180'000'000);c->real_prediction(220'000'000);
    check(c->read_minimum_cycle(125'000'000,ms) && ms==25,"direct runtime return ignores80ms producer tail and120ms display prediction");
    c->synthetic_accepted(120'000'000,11'111'111);
    c->synthetic_accepted(121'000'000,11'111'111);
    check(c->read_minimum_cycle(125'000'000,ms) && std::abs(ms-42.222222F)<.001F,"two generated submissions retain two physical refresh intervals");
    c->real_prediction(0);
    check(c->read_minimum_cycle(125'000'000,ms) && std::abs(ms-42.222222F)<.001F,"spacing floor needs no predicted display clock");
    check(!c->read_minimum_cycle(99'000'000,ms),"regressed runtime clock rejected");
    auto pending=std::make_shared<InterfaceCycle>(100'000'000);
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);pending->attach_native_delivery(real);
    check(!pending->read_minimum_cycle(125'000'000,ms),"private virtual return never substitutes native return");
    real->completed(true,144'000'000,145'000'000,200'000'000);
    check(pending->read_minimum_cycle(125'000'000,ms) && ms==45,"native return is sufficient before producer tail completes");
    pending->producer_returned(250'000'000);
    check(pending->read_minimum_cycle(125'000'000,ms) && ms==45,"later producer tail cannot inflate native return cycle");
    InterfaceTimingWindow window;
    check(window.submitted({50,120'000'000,0,c},120'050'000,true,true,false,true,0,125'000'000),"same real runtime completion recorded");
    InterfaceTelemetry m{};window.snapshot(130'000'000,m);
    check(m.cycle_valid && m.cycle_length_ms==80 && m.cycle_estimated_valid && std::abs(m.cycle_estimated_ms-42.222222F)<.001F &&
        m.cycle_estimated_source==InterfaceSource::minimum_cycle_qpc,"runtime cycle remains independent of producer-inclusive Cycle length");
    window.snapshot(1'130'000'001,m);
    check(!m.cycle_estimated_valid && !m.cycle_valid,"runtime counters expire with observed samples");
}
void production_boundaries() {
    auto c=std::make_shared<InterfaceCycle>(120'000'000);
    c->producer_returned(125'000'000);c->real_prediction(150'000'000);
    InterfaceTimingWindow w;check(w.submitted({201,121'000'000,0,c},125'000'000,true,true,false,true,150'000'000,125'000'000),"source handoff recorded");
    InterfaceTelemetry m{};w.snapshot(140'000'000,m);
    check(!m.game_time_valid && m.cycle_estimated_valid && m.cycle_estimated_ms==5 && m.stages_valid && m.generation_time_ms==0 && m.delivery_time_ms==4,"missing engine origin preserves the exact XR baseline estimate and independent delivery, without fake Game time");
    c->production_started(100'000'000);w.snapshot(140'000'000,m);
    check(m.game_time_valid && m.game_time_ms==21 && m.cycle_estimated_valid && m.cycle_estimated_ms==5 && m.delivery_time_ms==4,"Game includes pre-Wait production while Excel keeps its established Wait-origin interval");
    BenchmarkSamples excel;
    m.updated_ms=5000;excel.observe(m,1000,11000);
    c->production_started(0);w.snapshot(140'000'000,m);
    m.updated_ms=6000;excel.observe(m,1000,11000);
    check(excel.latency.size()==2 && excel.latency[0]==5 && excel.latency[1]==5,
        "benchmark receives the same value before and after loss of engine association");
    c->production_started(100'000'000);
    c->call(120'000'000,120'700'000);w.snapshot(140'000'000,m);
    check(m.game_time_ms==21 && m.delivery_time_ms==4,"pre-handoff calls remain inside chronological game interval");
    FrameProductionTracker tracker;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};locate.displayTime=500;locate.space=reinterpret_cast<XrSpace>(10);
    XrView views[2]{{XR_TYPE_VIEW},{XR_TYPE_VIEW}};views[0].pose.orientation.w=views[1].pose.orientation.w=1;
    views[0].pose.position.x=-.03F;views[1].pose.position.x=.03F;
    tracker.locate({8,1,100'000'000},locate,views,2);
    XrCompositionLayerProjectionView projected[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    for(int i=0;i<2;++i)projected[i].pose=views[i].pose;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=locate.space;projection.viewCount=2;projection.views=projected;
    const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.displayTime=500;frame.layerCount=1;frame.layers=&layer;
    check(tracker.match(frame,121'000'000).serial==1,"retained stereo projection identifies its producer tick");
    locate.displayTime=600;tracker.locate({8,2,115'000'000},locate,views,2);
    check(tracker.match(frame,121'000'000).serial==1,"a newer tick with identical stationary poses cannot replace the retained old frame");
    locate.displayTime=500;tracker.locate({8,3,116'000'000},locate,views,2);
    check(!tracker.match(frame,121'000'000),"ambiguous reused target and stationary poses stay unknown");
    frame.displayTime=600;projected[1].pose.position.x=.04F;
    check(!tracker.match(frame,121'000'000),"a different eye projection cannot borrow a tick");
    projected[1].pose=views[1].pose;
    check(!tracker.match(frame,3'000'000'000),"stale source origin is rejected");
}
void wording() {
    Snapshot s{};
    check(interface_status_text(s)=="No active game" && interface_state_text(s)=="Waiting", "disconnected UI");
    s.connected=true;s.runtime.ready=1;s.runtime.applied_revision=s.revision;
    check(interface_status_text(s)=="OFXR: Waiting | OSR: Off | Neural Render: Off", "clean non-Luke session reports default OSR not applicable");
    s.desired.afw_mode=3;
    check(interface_status_text(s).find("OSR: Luke Ross")==std::string::npos, "Luke selection cannot fake OSR activity");
    {
        // UEVR's Alternating/AFR as OSR's eye source: shown as Luke Ross OSR is.
        Snapshot u=s;
        u.desired.enabled=false;u.runtime.uevr_osr=true;
        check(interface_reconstruction_text(u,GetTickCount64())=="OSR: Off", "UEVR OSR attached, nothing reconstructed yet");
        u.runtime.heartbeat_ms=GetTickCount64();u.runtime.afw_running=true;
        check(interface_state_text(u)=="OSR" && interface_afw_only_active(u,GetTickCount64()), "UEVR OSR running reads as OSR");
        check(interface_reconstruction_text(u,GetTickCount64())=="OSR: UEVR", "UEVR named as OSR's source");
        check(interface_status_text(u).find("UEVR Alternating detected")!=std::string::npos, "UEVR detection in the status line");
    }
    s.runtime.luke_detected=s.runtime.vulkan_detected=true;
    s.runtime.generation_state=GenerationState::invalid_stereo;
    const auto text=interface_status_text(s);
    check(text.starts_with("Missing stereo pair") && text.find("Luke Ross detected")!=std::string::npos && text.find("Vulkan detected")!=std::string::npos, "simultaneous detections follow actual problem");
    std::strcpy(s.runtime.message,"Missing stereo pair");
    check(interface_status_text(s)==text, "duplicate reason omitted");
    s.runtime.vulkan_capture_suspended=true;
    s.runtime.generation_mode=0;
    check(interface_status_text(s).find("OFXR uses optical flow")!=std::string::npos, "Vulkan capture suspension reports optical flow");
    s.runtime.generation_mode=2;
    check(interface_status_text(s).find("real-frame bypass")!=std::string::npos, "Vulkan guide-dependent route reports bypass");
    s.runtime.vulkan_capture_suspended=false;
    for (auto state : {GenerationState::missing_motion,GenerationState::missing_depth,GenerationState::invalid_guides,
        GenerationState::temporal_mismatch,GenerationState::output_mismatch,GenerationState::queue_mismatch,
        GenerationState::unavailable,GenerationState::runtime_missing,GenerationState::initialization_failed,
        GenerationState::dispatch_failed,GenerationState::native_declined,GenerationState::multi_frame_unsupported,
        GenerationState::recovering,GenerationState::busy}) {
        s.runtime.message[0]=0;s.runtime.generation_state=state;
        check(!interface_status_text(s).starts_with("Luke"), "principal refusal has a distinct reason before detections");
    }
    s.runtime.upscaler_evaluations=1;s.runtime.upscaler_motion=true;s.runtime.upscaler_last_seen_ms=100;
    s.runtime.generation_mode=1;s.desired.frame_generation=1;
    check(interface_vectors_text(s,101)=="DLSS", "FSR FG never relabels actual DLSS vectors");
    check(interface_vectors_text(s,2100)=="None" && interface_vectors_text(s,99)=="None", "expired/regressing vector source");
    s.runtime.generation_state=GenerationState::paused;
    check(interface_state_text(s)=="Paused", "pause state");
    s.runtime.generation_state=GenerationState::ready;s.runtime.pending=1;
    check(interface_state_text(s)=="Applying settings", "pending state");
    s.runtime.pending=0;s.runtime.telemetry.updated_ms=GetTickCount64();s.runtime.telemetry.generating=true;
    check(interface_state_text(s)=="FSR", "confirmed generated content identifies producer");
    for(int backend=0;backend<4;++backend) {
        auto route=s;route.runtime.generation_mode=0;route.desired.backend=backend;
        const char* expected[]={"FidelityFX","Nvidia 0.2.1","Nvidia 0.3.0","Nvidia 0.2.13"};
        check(interface_state_text(route)==expected[backend],"state identifies the applied OFXR framework");
        check(interface_generation_active(route),"framework label preserves active colour");
    }
    auto hybrid=s;hybrid.runtime.generation_mode=3;hybrid.runtime.motion_source=0;
    check(interface_state_text(hybrid)=="Hybrid","hybrid label");
    auto dlssg=s;dlssg.runtime.generation_mode=2;
    check(interface_state_text(dlssg)=="DLSS-G","DLSS-G label");
    auto memory=s;
    check(interface_vram_text(memory)=="VRAM N/A","missing cached VRAM stays unknown");
    memory.runtime.video_memory={1,6ULL<<30,8ULL<<30};
    check(interface_vram_text(memory)=="VRAM 75%","percent uses cached process usage and DXGI budget");
    memory.runtime.video_memory.budget_bytes=0;
    check(interface_vram_text(memory)=="VRAM N/A","zero budget cannot invent a percent");
    s.desired.luke_osr=0;
    s.runtime.heartbeat_ms=GetTickCount64();s.runtime.afw_running=true;
    check(interface_state_text(s)=="FSR + AFW", "actual AFW is explicit");
    s.desired.nr_enabled=true;s.runtime.nr_running=true;
    check(interface_state_text(s)=="FSR + AFW + Neural", "actual neural processing is explicit");
    check(interface_status_text(s).find("AFW: Luke Ross | Neural Render: Active")!=std::string::npos, "all active effects reported independently");
    s.desired.luke_osr=1;
    check(interface_state_text(s)=="FSR + OSR + Neural", "retired band fill reports its migrated OSR mode");
    check(interface_status_text(s).find("OSR: Luke Ross")!=std::string::npos &&
        interface_status_text(s).find("AFW")==std::string::npos, "retired band fill reports OSR without legacy band label");
    s.desired.nr_enabled=false;s.runtime.nr_running=false;
    check(interface_state_text(s)=="FSR + OSR", "migrated OSR label without neural");
    s.desired.luke_osr=2;
    check(interface_state_text(s)=="FSR + OSR", "Full OSR replaces AFW in the mode label");
    {
        auto rife=s;rife.runtime.generation_mode=4;rife.desired.frame_generation=4;
        check(interface_state_text(rife)=="RIFE + OSR" && interface_generation_active(rife),
            "RIFE retains its own row while the nightly OSR renderer is active");
        rife.desired.nr_enabled=rife.runtime.nr_running=true;
        check(interface_state_text(rife)=="RIFE + OSR + Neural","RIFE OSR and NR are reported independently");
        rife.desired.luke_osr=0;
        check(interface_state_text(rife)=="RIFE + AFW + Neural","explicit AFW legacy stays available beside RIFE");
    }
    check(interface_status_text(s).find("OSR: Luke Ross")!=std::string::npos && interface_status_text(s).find("AFW")==std::string::npos, "Full OSR replaces AFW in the status line");
    s.desired.nr_enabled=true;s.runtime.nr_running=true;
    check(interface_state_text(s)=="FSR + OSR + Neural", "Full OSR label with neural");
    s.desired.luke_osr=0;s.desired.nr_enabled=true;s.runtime.nr_running=true;
    s.runtime.afw_running=false;s.runtime.nr_running=false;
    check(interface_status_text(s).find("AFW: Off | Neural Render: Waiting")!=std::string::npos, "enabled without output is waiting");
    s.runtime.afw_running=true;s.runtime.nr_running=true;s.runtime.heartbeat_ms=GetTickCount64()-2000;
    check(interface_status_text(s).find("AFW: Luke Ross")==std::string::npos && interface_status_text(s).find("Neural Render: Active")==std::string::npos, "stale activity expires");
    s.desired.afw_mode=0;s.desired.nr_enabled=false;
    check(interface_status_text(s).find("OSR: Off | Neural Render: Off")!=std::string::npos, "switches off override recent activity");
    {
        auto other=s;other.desired.afw_mode=3;other.desired.luke_osr=2;
        other.runtime.luke_detected=false;other.runtime.afw_running=true;
        other.runtime.afw_flat_scene=false;other.runtime.heartbeat_ms=1000;
        check(interface_reconstruction_text(other,1001)=="OSR: Other","active reconstruction without Luke is Other");
        other.runtime.luke_detected=true;
        check(interface_reconstruction_text(other,1001)=="OSR: Luke Ross","active Luke reconstruction identifies its source");
        other.desired.afw_mode=0;
        check(interface_reconstruction_text(other,1001)=="OSR: Off","Off overrides a still-fresh running receipt");
        other.desired.afw_mode=3;other.runtime.luke_detected=false;
        check(interface_reconstruction_text(other,3000)=="OSR: Off","expired reconstruction source is Off");
        other.runtime.afw_flat_scene=true;
        check(interface_reconstruction_text(other,1001)=="OSR: Off","flat scene is not reconstructed stereo");
        other.runtime.afw_flat_scene=false;other.desired.luke_osr=0;
        check(interface_reconstruction_text(other,1001)=="AFW: Other","legacy AFW uses the same honest source classification");
        check(interface_status_text(other,false).find("AFW:")==std::string::npos,"dedicated green source row is not duplicated in diagnostics");
        other.desired.enabled=false;other.runtime.applied_revision=other.revision;other.runtime.pending=false;
        other.runtime.heartbeat_ms=GetTickCount64();
        check(interface_afw_only_active(other,GetTickCount64()),"actual Other reconstruction can be active without frame generation");
    }
    s.runtime.generation_mode=0;s.runtime.motion_source=1;s.runtime.generation_state=GenerationState::invalid_guides;
    check(interface_state_text(s)=="Fallback", "actual optical-flow fallback");
    s.runtime.telemetry.updated_ms=GetTickCount64()-2000;
    check(interface_state_text(s)=="Real frames only", "stale generation evidence expires");
}
}
int main() {
    try {
        const auto fixture=std::filesystem::temp_directory_path()/(L"ofxr-v460-timing-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(fixture);
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",fixture.c_str());
        timing_sample_cohorts();
        buffer_preferences();minimum_spacing_rules();production_boundaries();timing();complete_cycle();estimated_cycle();native_delivery_cycle();stage_partition_and_baseline();wording();
        const auto before=snapshot();
        check(!interface_telemetry_requested(), "hidden by default");
        set_interface_visible(InterfaceSurface::desktop,true);
        check(interface_telemetry_requested(), "desktop demand");
        set_interface_visible(InterfaceSurface::headset,true);set_interface_visible(InterfaceSurface::desktop,false);
        check(interface_telemetry_requested(), "closing desktop preserves headset demand");
        set_interface_visible(InterfaceSurface::headset,false);
        check(!interface_telemetry_requested(), "closing last UI stops new sampling immediately");
        check(snapshot().revision==before.revision && snapshot().desired==before.desired, "visibility cannot reconfigure FG");
        const auto started=std::chrono::steady_clock::now();
        unsigned requested{};
        for(unsigned i=0;i<100000;++i) requested+=interface_telemetry_requested();
        std::cout<<"hidden visibility check ns/call: "<<std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count()/100000<<"; requests="<<requested<<'\n';
        std::filesystem::remove(configuration_path());std::filesystem::remove(fixture);
        std::cout<<checks<<" V465 identity/source/status/cycle checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
