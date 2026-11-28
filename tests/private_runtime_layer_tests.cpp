// Shipping API/worker/generator code, with only the downstream runtime mocked.
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include <fstream>
#define XRFG_SOFTWARE_OPTICAL_FIXTURE 1
#include "../src/layer/openxr_layer.cpp"
#define main private_transport_core_main
#include "private_openxr_runtime_tests.cpp"
#undef main
#include "autonomous_layer_fixture.hpp"
#include <d3d12sdklayers.h>

#include "afw_only_session_test.inc"
#include "steamvr_vram_budget_test.inc"
#include "auxiliary_slot_selection_test.inc"
#include "independent_aer4_test.inc"
#include "producer_timelines_test.inc"
#include "independent_features_test.inc"
#include "private_input_resource_test.inc"
#include "private_completion_recovery_test.inc"
#include "uevr_camera_loss_test.inc"
#include "uevr_turn_stop_test.inc"
#include "uevr_aer_crop_test.inc"
#include "uevr_stock_bootstrap_test.inc"
#include "independent_source_queue_test.inc"
#include "independent_stereo_recovery_test.inc"
#include "independent_midpoint_test.inc"
#include "independent_async_creation_test.inc"
#include "independent_panel_budget_test.inc"
#include "independent_source_admission_test.inc"
#include "independent_optical_first_test.inc"
#include "independent_overlap_recovery.inc"

// Model the native timing owner used by the shipping SteamVR endpoint. The old
// fixture's instantaneous Wait only tested a consumer that imposed its own CPU
// floor. xrWaitFrame normally throttles; retain explicit fault/latency models.
static XrResult XRAPI_CALL paced_steamvr_fixture_wait(
    XrSession session,const XrFrameWaitInfo* info,XrFrameState* output) {
    auto& f=*current;
    const auto previous=f.native_wait_tick_ns.load();
    if(previous && !f.frame_wait_us && !f.slow_frame_wait_ms) {
        const auto target=previous+period;
        while(ns()<target) {
            const auto left=target-ns();
            if(left>150'000) {
                LARGE_INTEGER due{};due.QuadPart=-std::max<std::int64_t>(1,(left-100'000)/100);
                check(SetWaitableTimer(f.frame_wait_timer,&due,0,nullptr,nullptr,FALSE) &&
                    WaitForSingleObject(f.frame_wait_timer,1000)==WAIT_OBJECT_0,"native Wait throttle timer");
            } else YieldProcessor();
        }
    }
    XrTime prior{};{std::scoped_lock lock(f.protocol);prior=f.raw_prediction;}
    const auto result=Fixture::wait(session,info,output);
    if(XR_SUCCEEDED(result)) {
        std::scoped_lock lock(f.protocol);
        f.raw_prediction=std::max(f.raw_prediction,prior+period);
        output->predictedDisplayTime=f.raw_prediction;
        f.native_wait_tick_ns=ns();
    }
    return result;
}

#include "vdxr_multiframe_test.inc"

static bool framework_session_probe{},vdxr_legacy_probe{};
static int admission_credit_test() {
    {
        // Use the largest supported physical interval. Wall-clock elapsed is
        // accounted below, so scheduling delays cannot mimic an extra period.
        const auto saved_period=period;period=50'000'000;
        {
            Fixture f(true);f.throttled=false;f.unblock();f.begin_virtual();
            XrFrameState before{XR_TYPE_FRAME_STATE},after{XR_TYPE_FRAME_STATE};
            const auto entered=ns();xr_ok(f.transport->predict_frame(&before),"prediction before admission");
            std::shared_ptr<xrfg::PrivateOpenXrRuntime::EndFrameReservation> permit;
            xr_ok(f.transport->reserve_end_frame(permit),"reserve without accepting a packet");
            xr_ok(f.transport->predict_frame(&after),"prediction during owned admission");
            const auto elapsed=ns()-entered;
            check(after.predictedDisplayTime-before.predictedDisplayTime<=elapsed+period/2,
                "an unaccepted End reservation must not add a refresh to virtual prediction");
            permit.reset();xr_ok(f.transport->drain(),"cancelled prediction reservation drains");
        }
        period=saved_period;
    }
    {
        Fixture f(true);f.throttled=false;f.unblock();
        xr_ok(f.transport->set_buffer_capacity(2),"credit test uses normal capacity");
        const auto chain=f.make_chain();f.write(chain,77);f.begin_virtual();
        std::shared_ptr<xrfg::PrivateOpenXrRuntime::EndFrameReservation> permit;
        xr_ok(f.transport->reserve_end_frame(permit),"reserve owned End credit");
        auto drained=std::async(std::launch::async,[&]{return f.transport->drain();});
        check(drained.wait_for(40ms)==std::future_status::timeout,"drain includes an owned, not-yet-accepted reservation");
        permit.reset();
        check(drained.wait_for(2s)==std::future_status::ready,"cancelling reservation wakes drain");xr_ok(drained.get(),"cancelled reservation drain");
        xr_ok(f.transport->reserve_end_frame(permit),"reserve before policy change");
        auto policy=std::async(std::launch::async,[&]{return f.transport->set_director_enabled(false);});
        check(policy.wait_for(40ms)==std::future_status::timeout,"policy change serializes with an owned End credit");
        permit.reset();
        check(policy.wait_for(2s)==std::future_status::ready,"cancelled credit releases policy serialization");xr_ok(policy.get(),"policy change after cancellation");
        xr_ok(f.transport->reserve_end_frame(permit),"reserve before invalid commit");
        check(f.transport->end_frame_reserved(nullptr,{},permit)==XR_ERROR_VALIDATION_FAILURE,
            "invalid reserved End returns validation failure");
        xr_ok(f.transport->drain(),"invalid commit returned its admission count and credit");
        check(f.transport->end_frame_reserved(nullptr,{},permit)==XR_ERROR_CALL_ORDER_INVALID,
            "a consumed reservation cannot be committed twice");
        permit.reset();
        xr_ok(f.submit(chain,1),"normal End remains usable after cancelled and failed reservations");
        f.wait_count(1);xr_ok(f.transport->drain(),"valid frame after failed commit");
        xr_ok(f.transport->destroy_swapchain(chain),"reservation lifecycle source cleanup");
    }
    {
        Fixture f(true);f.throttled=false;
        xr_ok(f.transport->set_buffer_capacity(2),"stop reservation uses normal capacity");
        const auto chain=f.make_chain();f.write(chain,88);
        for(unsigned source=1;source<=2;++source) {
            f.begin_virtual();xr_ok(f.submit(chain,float(source)),"fill stop-test endpoint");
            if(source==1)f.wait_entered();
        }
        f.begin_virtual();
        auto waiting=std::async(std::launch::async,[&] {
            std::shared_ptr<xrfg::PrivateOpenXrRuntime::EndFrameReservation> permit;
            return f.transport->reserve_end_frame(permit);
        });
        check(waiting.wait_for(40ms)==std::future_status::timeout,"full endpoint blocks only reservation acquisition");
        auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
        check(waiting.wait_for(2s)==std::future_status::ready,"stop wakes blocked reservation before native End returns");
        check(waiting.get()==XR_ERROR_SESSION_NOT_RUNNING,"stopped reservation propagates the real session state");
        f.unblock();
        check(stopped.wait_for(3s)==std::future_status::ready,"stop joins after reservation cancellation and native release");stopped.get();
        xr_ok(f.transport->destroy_swapchain(chain),"stopped reservation source cleanup");
    }
    std::puts("End reservation: bounded credits, drain, policy serialization, invalid/double commit and stop wakeup PASS");
    return 0;
}
static int independent_meter_test() {
    xrfg_fixture::ControlProfile profile;
    const auto unavailable=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult {
        *out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
    };
    const auto downstream=+[](XrSession,const XrFrameEndInfo*)->XrResult{return XR_ERROR_RUNTIME_FAILURE;};
    xrfg::OpenXrFpsOverlay overlay(reinterpret_cast<XrInstance>(1),reinterpret_cast<XrSession>(2),0,
        unavailable,downstream,nullptr,nullptr,nullptr,xrfg::autonomous::configuration_path());
    overlay.set_runtime_acceptance_source([](auto)->std::optional<float>{return {};},false,{},true);
    std::vector<std::shared_ptr<xrfg::InterfaceDeliveryReceipt>> receipts;
    overlay.set_private_delivery_end([&](const auto*,const auto& receipt) {
        check(bool(receipt),"independent receipt must exist with logging/UI disabled");
        receipts.push_back(receipt);return XR_SUCCESS;
    });
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=1;end.layerCount=1;end.layers=&layer;
    for(unsigned i=0;i<20;++i)xr_ok(overlay.end_frame(&end,i%2!=0),"virtual admission without physical completion");
    auto empty=overlay.metrics();
    check(empty.real_fps==0 && empty.generated_fps==0 && empty.submitted_fps==0,
        "virtual acceptance fabricated delivered real/generated FPS");
    for(unsigned i=0;i<receipts.size();++i)receipts[i]->discarded();
    check(overlay.metrics().submitted_fps==0,"expired output was counted as delivered");
    for(unsigned i=0;i<4;++i)xr_ok(overlay.end_frame(&end,i%2!=0),"fresh native candidate admission");
    const auto now=xrfg::interface_clock_ns();
    for(unsigned i=0;i<4;++i)receipts[20+i]->completed(true,now-50'000'000+i*10'000'000,
        now-49'000'000+i*10'000'000,0);
    const auto fresh=overlay.metrics();
    check(fresh.real_fps>0 && fresh.generated_fps>0,"confirmed native outputs were not counted");
    check(receipts[21]->synthetic_confirmed && receipts[23]->synthetic_confirmed,
        "native completion lost generated classification");
    check(overlay.metrics().submitted_fps==fresh.submitted_fps,"polling a receipt counted the same image twice");
    auto bypass=std::make_shared<xrfg::D3D12NativeFrameResult>();bypass->outputs=1;bypass->mask=0;
    const std::array<std::shared_ptr<xrfg::D3D12NativeFrameResult>,1> results{bypass};
    xr_ok(overlay.end_frame(&end,true,true,results),"SDK fallback candidate admission");
    receipts.back()->completed(true,now-10'000'000,now-9'000'000,0);
    check(overlay.metrics().submitted_fps==fresh.submitted_fps && !receipts.back()->synthetic_confirmed,
        "confirmed native fallback was counted as newly generated content");
    std::puts("Independent meter: no virtual/expired/replayed counts; confirmed real/generated receipts only PASS");
    return 0;
}
static int overlay_admission_test(bool measured) {
    xrfg_fixture::ControlProfile profile;
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,measured);
    Fixture f(true);f.throttled=false;
    xr_ok(f.transport->set_buffer_capacity(2),"normal two-credit endpoint for overlay admission probe");
    const auto chain=f.make_chain();f.write(chain,80);
    for(unsigned source=1;source<=2;++source) {
        f.begin_virtual();xr_ok(f.submit(chain,float(source)),"fill private endpoint before overlay probe");
        if(source==1)f.wait_entered();
    }
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
    dispatch->end_frame=+[](XrSession,const XrFrameEndInfo* info)->XrResult {
        return current->transport->end_frame(info);
    };
    const auto unavailable=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult {
        *out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
    };
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->private_runtime=f.transport;
    std::atomic_uint input_polls{};
    const auto input=+[](void* user) {
        ++*static_cast<std::atomic_uint*>(user);
        xrfg::OverlayMenuInputFrame frame{};frame.focused=true;return frame;
    };
    state->fps_overlay=std::make_unique<xrfg::OpenXrFpsOverlay>(dispatch->instance,state->handle,0,
        unavailable,dispatch->end_frame,nullptr,nullptr,nullptr,
        xrfg::autonomous::configuration_path(),true,input,&input_polls);
    bind_private_delivery_metrics(state);
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.pose.position.x=3;view.subImage.swapchain=chain;
    view.subImage.imageRect.extent={1,1};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
    const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.layerCount=1;
    end.layers=&layer;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    xrfg::InterfaceFrameStamp stamp{};
    if(measured) {stamp.entered_ns=xrfg::interface_clock_ns();stamp.cycle=std::make_shared<xrfg::InterfaceCycle>(stamp.entered_ns);}
    f.begin_virtual();
    auto waiting=std::async(std::launch::async,[&]{return state->fps_overlay->end_frame(&end,false,true,{},0,stamp);});
    check(waiting.wait_for(50ms)==std::future_status::timeout,"endpoint pressure must hold the third submission");
    auto producer=std::async(std::launch::async,[&] {
        // Exactly the production lock order: UI refresh holds the game GPU
        // mutex, so waiting for the presenter's UI mutex also stalls synthesis.
        std::scoped_lock gpu_lock(state->gpu_mutex);
        state->fps_overlay->application_frame(&end);
    });
    const bool independent=producer.wait_for(250ms)==std::future_status::ready;
    const bool polled=input_polls.load()!=0;
    f.unblock();
    check(producer.wait_for(3s)==std::future_status::ready,"UI probe retires after native gate release");producer.get();
    check(input_polls.load()!=0,"UI probe must execute its actual input callback after the gate is released");
    check(waiting.wait_for(3s)==std::future_status::ready,"third endpoint submission retires after native gate release");
    xr_ok(waiting.get(),"held overlay submission accepted once");
    f.wait_count(3);xr_ok(f.transport->drain(),"overlay admission exact FIFO drain");
    if(measured) {
        float cycle{};
        check(stamp.cycle->read_minimum_cycle(xrfg::interface_clock_ns(),cycle) && cycle>=40.F,
            "moving the delivery-credit wait must not remove it from the measured native cycle");
    }
    std::printf("OVERLAY_ADMISSION measured=%u producer_returned_before_native=%u input_polled_before_native=%u total_input_polls=%u\n",
        unsigned(measured),unsigned(independent),unsigned(polled),input_polls.load());
    check(independent && polled,"application_frame and its real input callback waited for private delivery capacity");
    check(f.positions==std::vector<float>({1,2,3}) && f.pixels==std::vector<unsigned>({80,80,80}),
        "moving admission outside UI mutex changed frame ownership or delivery order");
    state->fps_overlay.reset();xr_ok(f.transport->destroy_swapchain(chain),"overlay admission source cleanup");
    std::printf("Overlay admission measured=%u: blocked native End retains credits while producer UI/input progresses PASS\n",unsigned(measured));
    return 0;
}
// This gate holds an actual native End, not RIFE or an artificial GPU sleep.
// The private endpoint keeps its two FIFO credits until that End returns.
static std::atomic_uint rife_overlap_hold_after{UINT_MAX};
static XrResult XRAPI_CALL rife_overlap_native_end(XrSession session,const XrFrameEndInfo* info) {
    if(info->layerCount && current->real_ends.load()==rife_overlap_hold_after.load()) {
        current->end_entered=false;
        current->block_end=true;
        rife_overlap_hold_after=UINT_MAX;
    }
    return Fixture::end(session,info);
}
// Star Citizen loads no implicit Vulkan layer, so the bridge declines its
// Vulkan session, which SteamVR then creates natively. The private endpoint
// has no D3D device to own; xrCreateSession must pass that session through,
// not destroy it with XR_ERROR_INITIALIZATION_FAILED.
static int unbridged_vulkan_session_test() {
    for(const auto binding:{SessionGraphicsBinding::vulkan,SessionGraphicsBinding::opengl}) {
        auto dispatch=std::make_shared<Dispatch>();
        dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=true;
        auto state=std::make_shared<SessionState>(dispatch);
        state->handle=reinterpret_cast<XrSession>(2);state->graphics_binding=binding;state->vdxr_legacy=false;
        check(comparison_delivery_runtime(*state),"SteamVR Auto selects the private endpoint");
        check(install_private_runtime(state)==S_FALSE && !state->private_runtime && state->dispatch==dispatch,
            "non-D3D session kept its native binding and runtime dispatch");
    }
    std::puts("Unbridged Vulkan/OpenGL session pass-through: PASS");
    return 0;
}

static int panel_metadata_contract() {
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    view.subImage.swapchain=reinterpret_cast<XrSwapchain>(50);view.subImage.imageRect.extent={4,4};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};q.space=projection.space;
    q.pose.orientation.w=1;q.pose.position.x=1;q.size={1,1};
    q.subImage.swapchain=reinterpret_cast<XrSwapchain>(99);q.subImage.imageRect.extent={2,4};
    auto other=q;other.pose.position.x=5;other.subImage.imageRect.offset.x=2;
    const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&other)};
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=3;end.layers=layers;
    ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"capture panel metadata");
    snapshot.panel_corrections=true;snapshot.previous_panels=snapshot.panels;
    snapshot.previous_panels[0].pose.position.x=0;
    std::array<ProjectionResourceDestination,2> destinations{{{view.subImage.swapchain,view.subImage.swapchain},
        {q.subImage.swapchain,reinterpret_cast<XrSwapchain>(77),false,true}}};
    for(const float fraction:{.25f,1.f/3,.5f,2.f/3,.75f}) {
        GeneratedFrameEndInfo frame;
        check(build_generated_frame_end_info(&end,snapshot,true,destinations,&frame,nullptr,fraction),"synthetic panel metadata");
        auto owned=make_presenter_owned_frame(std::move(frame));check(bool(owned),"own interpolated panel metadata");
        const auto* a=reinterpret_cast<const XrCompositionLayerQuad*>(owned->info.layers[1]);
        const auto* b=reinterpret_cast<const XrCompositionLayerQuad*>(owned->info.layers[2]);
        check(std::abs(a->pose.position.x-fraction)<1e-5f&&b->pose.position.x==5,"private atlas panel poses or fraction changed");
        check(a->subImage.swapchain==reinterpret_cast<XrSwapchain>(77)&&b->subImage.swapchain==a->subImage.swapchain,
            "generated atlas handle or second rectangle lost");
        check(b->subImage.imageRect.offset.x==2,"generated atlas rectangle lost");
        q.pose.position.x=999;check(std::abs(a->pose.position.x-fraction)<1e-5f,"async metadata borrowed the next application pose");q.pose.position.x=1;
    }
    GeneratedFrameEndInfo still;
    check(build_generated_frame_end_info(&end,snapshot,true,std::span(destinations.data(),1),&still),"stationary panel metadata");
    check(still.info.layers[2]==layers[2],"stationary passthrough panel was unnecessarily replaced");
    snapshot.panel_corrections=false;GeneratedFrameEndInfo unchanged;
    check(build_generated_frame_end_info(&end,snapshot,true,std::span(destinations.data(),1),&unchanged),"REAL VR panel passthrough");
    check(reinterpret_cast<const XrCompositionLayerQuad*>(unchanged.info.layers[1])->pose.position.x==1,
        "disabled panel corrections changed REAL VR placement");
    std::puts("Shipping panel metadata ownership, atlas rects, 2X/3X/4X fractions and REAL VR passthrough PASS");return 0;
}

#include "rife_output_banks_tests.inc"
#include "aer_alternate_delivery.inc"
#include "aer_native_guide_cache_fixture.inc"
#include "aer_stereo_fallback_test.inc"

static int independent_ready_admission_test(bool learned=false) {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;settings.enabled=true;
    check(xrfg::autonomous::request(settings),"ready admission control");xrfg::autonomous::apply_runtime_settings();
    const auto prior_mode=independent_fixture_mode;
    const auto prior_auto=independent_fixture_auto.load();
    if(learned){independent_fixture_mode=true;independent_fixture_auto=true;}
    Fixture f(true,true,{},false,true,false,8);f.throttled=false;
    const auto chain=f.make_chain();f.write(chain,80);
    if(learned) {
        f.unblock();
        for(unsigned i=0;i<4;++i){f.begin_virtual();f.transport->set_next_independent_deadline(ns()-4*period);
            xr_ok(f.submit(chain,float(20+i)),"learn genuine late-source allowance");xr_ok(f.transport->drain(),"retire allowance training packet");}
        for(unsigned i=0;i<1000 && f.transport->independent_load().reserve<3*period;++i)std::this_thread::sleep_for(1ms);
        check(f.transport->independent_load().reserve>=3*period,"ready budget probe needs a real learned expiry allowance");
        f.block_end=true;f.end_entered=false;
    }
    const auto before=f.real_ends.load();
    for(unsigned i=1;i<=2;++i){f.begin_virtual();if(learned)f.transport->set_next_independent_deadline(ns()+period);
        xr_ok(f.submit(chain,float(i)),"fill ready native credits");if(i==1)f.wait_entered();}
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->menu_enabled=state->presenter_active=state->independent_pacing=true;state->private_runtime=f.transport;
    auto admission=std::async(std::launch::async,[&]{return wait_for_independent_source_budget(*state);});
    const bool held=admission.wait_for(25ms)==std::future_status::timeout;
    f.unblock();const bool resumed=admission.wait_for(500ms)==std::future_status::ready;
    if(!resumed){state->presenter_stop_requested=true;state->presenter_condition.notify_all();}
    xr_ok(admission.get(),"pre-render admission resumes on native progress");
    f.wait_count(before+2);xr_ok(f.transport->drain(),"ready admission drains exact accepted images");
    check(held && resumed,"ready/native-held outputs were allowed to fill independent source pipeline");
    check(f.positions.size()>=2 && std::vector<float>(f.positions.end()-2,f.positions.end())==std::vector<float>({1,2}),"admission smoothing dropped or reordered accepted outputs");
    xr_ok(f.transport->destroy_swapchain(chain),"ready admission source cleanup");
    f.transport->stop();independent_fixture_mode=prior_mode;independent_fixture_auto=prior_auto;
    if(learned)std::puts("READY_ALLOWANCE learned expiry margin does not expand ready ownership; native progress drains accepted FIFO PASS");
    std::puts("READY_ADMISSION pre-render bound includes native-held images; native progress releases source without output drops PASS");return 0;
}
static int independent_d11_binding_test() {
    xrfg_fixture::ControlProfile profile(true);
    Fixture f(false);f.transport->stop();f.transport.reset();f.throttled=false;f.unblock();
    f.source_device11=f.device11;f.source_context11=f.context;
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=true;
    dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult{*out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;};
    dispatch->create_session=+[](XrInstance,const XrSessionCreateInfo* info,XrSession* out)->XrResult {
        auto& f=*current;const auto* binding=static_cast<const XrGraphicsBindingD3D11KHR*>(info->next);
        check(binding && binding->type==XR_TYPE_GRAPHICS_BINDING_D3D11_KHR && binding->device!=f.source_device11.Get(),"native XR must receive a separate D11 device");
        f.device11=binding->device;f.device11->GetImmediateContext(f.context.ReleaseAndGetAddressOf());
        *out=reinterpret_cast<XrSession>(2);return XR_SUCCESS;
    };
    dispatch->destroy_session=+[](XrSession)->XrResult{return XR_SUCCESS;};
    dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;dispatch->enumerate_swapchain_images=Fixture::enumerate;
    dispatch->acquire_swapchain_image=Fixture::acquire;dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;dispatch->convert_time_to_qpc=Fixture::to_qpc;
    {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;}
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=f.source_device11.Get();
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=&binding;info.systemId=1;XrSession handle{};
    xr_ok(layer_create_session_impl(dispatch->instance,&info,&handle),"actual independent D11 session");
    auto state=find_session(handle);
    check(state && state->independent_pacing && state->private_runtime && state->d3d11_device==f.source_device11 &&
        state->private_delivery_device11==f.device11,"separate native binding changed original app device or lost endpoint wiring");
    f.transport=state->private_runtime;xr_ok(f.transport->start(),"actual D11 endpoint starts");
    const auto chain=f.make_chain();f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,1),"actual D11 binding input");
    f.wait_count(1);xr_ok(f.transport->drain(),"actual D11 binding drain");
    check(f.pixels==std::vector<unsigned>({80}) && f.positions==std::vector<float>({1}),"actual session binding pixels/pose differ");
    xr_ok(f.transport->destroy_swapchain(chain),"actual D11 binding chain cleanup");
    xr_ok(layer_destroy_session_impl(handle),"actual D11 binding session cleanup");
    {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
    std::puts("D11_BINDING actual session isolates native device; original app images retained; native pixels/pose PASS");return 0;
}
static int independent_stage_overlap_test() {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;settings.enabled=true;
    check(xrfg::autonomous::request(settings),"stage overlap control");xrfg::autonomous::apply_runtime_settings();
    Fixture f(true,true,{},false,true,false,8);f.throttled=false;
    const auto chain=f.make_chain();f.write(chain,80);f.begin_virtual();
    xr_ok(f.submit(chain,1),"stage overlap native head");f.wait_entered();
    check(f.transport->independent_load().pending==1,"one native owner needed for overlap probe");
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->menu_enabled=state->presenter_active=state->independent_pacing=true;
    state->private_runtime=f.transport;state->independent_completion_busy=true;
    // One earlier GPU completion and one native head are different stages.
    // The app must be allowed to start the next render while BOTH remain held.
    auto admission=std::async(std::launch::async,[&]{return wait_for_independent_source_budget(*state);});
    const bool overlapped=admission.wait_for(40ms)==std::future_status::ready;
    const bool native_held=f.real_ends==0 && f.block_end;
    {std::scoped_lock lock(state->presenter_mutex);state->independent_completion_busy=false;}
    state->presenter_condition.notify_all();
    check(admission.wait_for(500ms)==std::future_status::ready,"stage overlap cleanup admission");
    xr_ok(admission.get(),"stage overlap admission result");f.unblock();f.wait_count(1);
    xr_ok(f.transport->drain(),"stage overlap native drain");
    xr_ok(f.transport->destroy_swapchain(chain),"stage overlap cleanup");
    check(overlapped && native_held,"ready limit serialized next render behind unfinished GPU and native stages");
    std::puts("STAGE_OVERLAP next render admitted with one unfinished GPU completion and one held native image PASS");return 0;
}
static int independent_source_admission_test() {
    xrfg_fixture::ControlProfile profile;
    auto settings=xrfg::autonomous::snapshot().desired;settings.enabled=true;
    check(xrfg::autonomous::request(settings),"source budget control");
    xrfg::autonomous::apply_runtime_settings();
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->menu_enabled=true;state->presenter_active=true;
    // Completed frames can all be awaiting a blocked native compositor.
    // They must not consume the GPU budget or stop a new source render.
    state->independent_outputs.resize(8);
    state->independent_completion_busy=true;
    xr_ok(wait_for_independent_source_budget(*state),"one GPU batch overlaps next game render");
    state->synthesis_busy=true;
    auto waiting=std::async(std::launch::async,[&]{return wait_for_independent_source_budget(*state);});
    const bool bounded=waiting.wait_for(25ms)==std::future_status::timeout;
    {std::scoped_lock lock(state->presenter_mutex);state->independent_completion_busy=false;}
    state->presenter_condition.notify_all();
    const bool resumed=waiting.wait_for(500ms)==std::future_status::ready;
    if(!resumed){std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;state->presenter_condition.notify_all();}
    xr_ok(waiting.get(),"source resumes on GPU completion");
    check(bounded && resumed && state->independent_outputs.size()==8,
        "source budget followed native delivery or admitted a third GPU batch");
    state->synthesis_busy=false;
    state->independent_sources.push_back(std::make_shared<SynthesisJob>());
    state->independent_completions.push_back(std::make_shared<IndependentSynthesisWork>());
    auto queued=std::async(std::launch::async,[&]{return wait_for_independent_source_budget(*state);});
    const bool queued_bounded=queued.wait_for(25ms)==std::future_status::timeout;
    // A control change can release admission without waiting for a vendor
    // fence or applying a new image at EndFrame first.
    settings.enabled=false;check(xrfg::autonomous::request(settings),"stop generation while admission is full");
    xrfg::autonomous::apply_runtime_settings();
    const bool disabled=queued.wait_for(500ms)==std::future_status::ready;
    if(!disabled){std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;state->presenter_condition.notify_all();}
    xr_ok(queued.get(),"disabled generator must not retain game admission");
    check(queued_bounded && disabled,"queued submissions ignored budget or FG off did not release it");
    state->presenter_stop_requested=true;state->menu_enabled=false;
    xr_ok(wait_for_independent_source_budget(*state),"retired generator is not a stopped OpenXR session when FG is off");
    settings.enabled=true;check(xrfg::autonomous::request(settings),"restart generation for shutdown guard");
    xrfg::autonomous::apply_runtime_settings();state->menu_enabled=true;
    state->presenter_active=false;
    xr_ok(wait_for_independent_source_budget(*state),"first source must bootstrap a re-enabled generator");
    state->presenter_active=true;
    check(wait_for_independent_source_budget(*state)==XR_ERROR_SESSION_NOT_RUNNING,"shutdown wakes source admission");
    std::puts("INDEPENDENT_SOURCE_BUDGET two unfinished batches; completed/native-held outputs ignored; GPU progress and FG-off release source PASS");
    return 0;
}

static int independent_stale_guide_worker_test() {
    Fixture fixture(true);
    auto dispatch = std::make_shared<Dispatch>();
    static std::atomic_uint released{};
    released = 0;
    dispatch->wait_swapchain_image = +[](XrSwapchain, const XrSwapchainImageWaitInfo*) -> XrResult {
        return XR_SUCCESS;
    };
    dispatch->release_swapchain_image = +[](XrSwapchain, const XrSwapchainImageReleaseInfo*) -> XrResult {
        ++released;
        return XR_SUCCESS;
    };
    auto state = std::make_shared<SessionState>(dispatch);
    state->independent_pacing = true;
    state->d3d12_device = fixture.device12;
    auto partial = std::make_shared<FrameGenerationSwapchainState>();
    const auto source = reinterpret_cast<XrSwapchain>(101);
    const auto output = reinterpret_cast<XrSwapchain>(102);
    partial->current[0].handle = output;
    partial->synthetic[0].handle = reinterpret_cast<XrSwapchain>(103);
    partial->current[0].phase = partial->synthetic[0].phase = PrivateOwnershipPhase::waited;
    partial->current_runtime_images_per_ring = partial->synthetic_runtime_images_per_ring = 1;
    partial->current_runtime_images.resize(1);
    partial->synthetic_runtime_images.resize(1);
    ComPtr<ID3D12Fence> fence;
    ok(fixture.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "partial pair fence");

    const auto make_work = [](std::uint64_t sequence) {
        auto work = std::make_shared<IndependentSynthesisWork>();
        work->job = std::make_shared<SynthesisJob>();
        work->job->current = std::make_shared<ThreeSlotFrame>();
        work->job->current->source_sequence = sequence;
        return work;
    };
    auto startup = make_work(1);
    startup->prepared.reason = GenerationPrepareReason::synthesis_stale_guides;
    startup->prepared.failed_swapchain = source;
    startup->prepared.resources.resize(2);
    for (auto& resource : startup->prepared.resources) {
        resource.generation.reason = startup->prepared.reason;
        resource.generation.stale_guides_recoverable = recoverable_stale_guides(
            xrfg::kD3D12FrameSynthesisStaleGuides, true, true);
    }

    auto dropped = make_work(2);
    dropped->job->pair = true;
    auto& prepared = dropped->prepared;
    prepared.reason = GenerationPrepareReason::synthesis_stale_guides;
    prepared.failed_swapchain = reinterpret_cast<XrSwapchain>(104);
    prepared.resources.push_back({source, {}});
    auto& ready_eye = prepared.resources.front().generation;
    ready_eye.kind = PreparedGenerationKind::pair;
    ready_eye.reason = GenerationPrepareReason::ready;
    ready_eye.worker_generation = partial;
    ready_eye.worker_fence = fence;
    ready_eye.worker_fence_value = 1;
    prepared.resources.push_back({prepared.failed_swapchain, {}});
    auto& stale_eye = prepared.resources.back().generation;
    stale_eye.reason = prepared.reason;
    check(!stale_worker_transaction(prepared), "stale guides cannot hide a failed output release");
    stale_eye.preserve_previous = true;
    stale_eye.stale_guides_recoverable = true;

    auto next = make_work(3);
    next->job->metadata = std::make_shared<GeneratedFrameEndInfo>();
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.subImage.swapchain = source;
    view.subImage.imageRect.extent = {8, 8};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 1;
    projection.views = &view;
    const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    auto& info = next->job->metadata->info;
    info.type = XR_TYPE_FRAME_END_INFO;
    info.layerCount = 1;
    info.layers = &layer;
    next->job->snapshot.layers.emplace_back().views.push_back(view);
    next->prepared.kind = PreparedGenerationKind::prime;
    next->prepared.reason = GenerationPrepareReason::ready;
    next->prepared.resources.push_back({source, {}});
    next->prepared.resources.front().generation.current_handle = output;

    state->independent_completions.push_back(std::move(startup));
    state->independent_completions.push_back(std::move(dropped));
    state->independent_completions.push_back(std::move(next));
    auto worker = std::thread([&] { synthesis_worker_main(state, true); });
    bool early_release{}, resumed{};
    {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait_for(lock, 15ms, [&] { return XR_FAILED(state->presenter_failure); });
        early_release = released.load() != 0;
        const auto signal = fence->Signal(1);
        resumed = SUCCEEDED(signal) && state->presenter_condition.wait_for(lock, 1s, [&] {
            return !state->independent_outputs.empty() || XR_FAILED(state->presenter_failure);
        });
        state->presenter_stop_requested = true;
        state->independent_submission_finished = true;
    }
    state->presenter_condition.notify_all();
    worker.join();
    check(!early_release && released == 2, "both partial outputs must retire after their fence");
    check(resumed && XR_SUCCEEDED(state->presenter_failure) && state->independent_outputs.size() == 1 &&
        state->independent_outputs.front()->source_sequence == 3 && !state->independent_completion_busy,
        "stale startup prime or stereo pair stopped the worker or published a partial batch");
    std::puts("STALE_GUIDE_WORKER stale startup prime skipped, partial stereo batch drained, next source published PASS");
    return 0;
}

static int legacy_latency_scope_test() {
    xrfg_fixture::ControlProfile profile;
    {Fixture f(true);auto dispatch=std::make_shared<Dispatch>();
     auto state=std::make_shared<SessionState>(dispatch);
     state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
     state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
     state->nvidia_options.rife_model=-1;
     for(unsigned multiplier:{3U,4U}) {
         state->nvidia_options.rife_multiplier=multiplier;
         for(bool director:{false,true}) {
             state->single_director_requested=director;state->independent_pacing=false;
             check(optical_source_admission(*state),"legacy Framework/Director admits ordinary optical multiframe");
             state->independent_pacing=true;
             check(!optical_source_admission(*state),"Independent cannot enter the legacy source gate");
             check(!state->source_admission_timer && !state->source_admission_event && !state->native_batch_admission.pending(),
                 "Independent receives no legacy timer or output credits");
         }
     }
     state->independent_pacing=false;state->nvidia_options.rife_multiplier=2;
     check(!optical_source_admission(*state),"2X retains its existing source admission");
     state->nvidia_options.rife_multiplier=3;state->nvidia_options.rife_model=0;
     check(!optical_source_admission(*state),"RIFE retains its existing source admission");
    }
    independent_fixture_mode=true;
    {Fixture f(true);independent_fixture_mode=false;f.throttled=false;f.unblock();
     std::atomic<unsigned> started{},retired{};
     f.transport->set_output_start_observer([&](std::int64_t){++started;});
     f.transport->set_output_retirement_observer([&](std::int64_t,std::int64_t,std::int64_t,std::int64_t){++retired;});
     auto chain=f.make_chain();f.write(chain,40);f.begin_virtual();
     f.transport->tag_next_end_for_source_batch(true,ns()+period);
     xr_ok(f.submit(chain,1),"Independent ignores legacy output tagging");f.wait_count(1);
     xr_ok(f.transport->drain(),"Independent drain after ignored tagging");f.transport->stop();
     check(!started && !retired,"Independent must not invoke legacy timing/credit observers");
    }
    std::puts("Legacy optical admission scope: Framework/Director only; Independent/2X/RIFE preserved PASS");return 0;
}

static int aer_input_warning_test() {
    xrfg_fixture::ControlProfile profile;
    auto controls=xrfg::autonomous::snapshot().desired;
    controls.enabled=true;controls.ofr_enabled=true;
    check(xrfg::autonomous::request(controls),"manual AER warning setup");
    xrfg::autonomous::apply_runtime_settings();
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    std::array<XrCompositionLayerProjectionView,2> views;
    for(unsigned eye=0;eye<2;++eye) {
        chains[eye]=std::make_shared<SwapchainState>(state,XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO});
        auto& chain=*chains[eye];
        chain.handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(100+eye));
        chain.last_released_index=0;chain.eye_frame_release_serial=1;
        views[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[eye].subImage.swapchain=chain.handle;
        std::scoped_lock lock(g_state_mutex);g_swapchains[chain.handle]=chains[eye];
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount=2;projection.views=views.data();
    const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&layer;
    update_aer_input(*state,&end,1000);
    update_aer_input(*state,&end,3999);
    check(!state->aer_input_warning,"initial scene detection gets three seconds");
    update_aer_input(*state,&end,4000);
    check(state->aer_input_warning,"unrecognized paired input produces the delayed warning");
    check(xrfg::autonomous::snapshot().desired.ofr_enabled &&
        effective_session_settings(*state,xrfg::embedded::snapshot().desired).ofr,
        "warning leaves manually selected AER enabled on stereo input");
    for(unsigned tick=0;tick<4;++tick) {
        ++chains[tick%2]->eye_frame_release_serial;
        update_aer_input(*state,&end,4100+tick);
    }
    check(!state->aer_input_warning,"alternating input clears the warning immediately");
    ++chains[0]->eye_frame_release_serial;++chains[1]->eye_frame_release_serial;
    update_aer_input(*state,&end,5000);
    update_aer_input(*state,&end,7999);
    check(!state->aer_input_warning,"returning stereo input gets a fresh detection period");
    update_aer_input(*state,&end,8000);check(state->aer_input_warning,"persistent stereo warns again");
    for(unsigned eye=0;eye<2;++eye) {
        auto& frame=chains[eye]->released_eye_frame.frame;
        frame.producer=OFXR_EYE_SOURCE_MUTAR;frame.eye_count=1;
        frame.eyes[0]={7,10+eye,0,eye};
    }
    update_aer_input(*state,&end,8100);
    check(!state->aer_input_warning,"paired input with staggered render metadata is recognized as AER");
    chains[1]->released_eye_frame.frame.eyes[0].render_id=10;
    update_aer_input(*state,&end,9000);update_aer_input(*state,&end,12000);
    check(state->aer_input_warning,"same-render metadata alone does not establish AER");
    XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
    update_aer_input(*state,&empty,12100);
    check(!state->aer_input_warning && !state->aer_warning_started_ms,"empty loading frames clear the warning and timer");
    update_aer_input(*state,&end,20000);update_aer_input(*state,&end,22999);
    check(!state->aer_input_warning,"loading time does not consume the detection period");
    update_aer_input(*state,&end,23000);check(state->aer_input_warning,"warning returns after scene observation");
    controls.enabled=false;check(xrfg::autonomous::request(controls),"turn framegen off");
    xrfg::autonomous::apply_runtime_settings();update_aer_input(*state,&end,24000);
    check(!state->aer_input_warning && !state->aer_warning_started_ms,"global off clears the warning and timer");
    controls.enabled=true;check(xrfg::autonomous::request(controls),"turn framegen on");
    xrfg::autonomous::apply_runtime_settings();update_aer_input(*state,&end,25000);
    check(!state->aer_input_warning,"re-enabling AER starts a fresh delay");
    update_aer_input(*state,&end,28000);check(state->aer_input_warning,"re-enabled AER eventually warns");
    controls.ofr_enabled=false;check(xrfg::autonomous::request(controls),"turn AER off");
    xrfg::autonomous::apply_runtime_settings();update_aer_input(*state,&end,29000);
    check(!state->aer_input_warning,"AER off clears the warning");
    controls.ofr_enabled=true;check(xrfg::autonomous::request(controls),"restore manual AER");
    xrfg::autonomous::apply_runtime_settings();state->luke_afw_enabled=true;
    update_aer_input(*state,&end,30000);update_aer_input(*state,&end,40000);
    check(!state->aer_input_warning,"known REAL VR input avoids an unrecognized-input warning");
    {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();}
    std::puts("AER advisory detection, three-second delay, metadata, loading and manual control PASS");
    return 0;
}

int main(int argc,char** argv) {
    if(argc>1 && std::string_view(argv[1])=="independent-required-ui-budget")return independent_panel_budget_test(4);
    if(argc>1 && std::string_view(argv[1])=="independent-panel-priority")return independent_panel_budget_test(3);
    if(argc>1 && std::string_view(argv[1])=="independent-budget-rearm-legacy")return independent_budget_rearm_probe(true);
    if(argc>1 && std::string_view(argv[1])=="independent-budget-rearm-control")return independent_budget_rearm_probe();
    if(argc>1 && std::string_view(argv[1])=="independent-budget-rearm-probe")return independent_budget_rearm_probe();
    if(argc>1 && std::string_view(argv[1])=="independent-panel-budget") {
        for(unsigned order=0;order<3;++order)independent_panel_budget_test(order);
        return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="uevr-aer-crop")return uevr_aer_crop_test();
    if(argc>1 && std::string_view(argv[1])=="uevr-stock-bootstrap")return uevr_stock_bootstrap_test();
    if(argc>1 && std::string_view(argv[1])=="uevr-turn-stop")return uevr_turn_stop_test();
    if(argc>1 && std::string_view(argv[1])=="independent-overlap-recovery")return independent_overlap_recovery_test();
if(argc>1 && std::string_view(argv[1])=="independent-optical-first")return independent_optical_first_test();
    if(argc>1 && std::string_view(argv[1])=="independent-pipelined-admission")return independent_pipelined_admission_test();
    if(argc>1 && std::string_view(argv[1])=="independent-credit-lifecycle")return independent_source_credit_lifecycle_test();
    if(argc>1 && std::string_view(argv[1])=="independent-native-binding-handoff"){for(unsigned runtime=0;runtime<4;++runtime)independent_native_binding_handoff(runtime);independent_native_binding_handoff(1,false);return 0;}
    if(argc>1 && std::string_view(argv[1])=="independent-async-generation"){independent_async_creation_test(false);independent_async_creation_test(true);return independent_async_creation_test(false,true);}
    if(argc>1 && std::string_view(argv[1])=="independent-midpoint")return independent_midpoint_test();
    if(argc>1 && std::string_view(argv[1])=="independent-stereo-recovery")return independent_stereo_recovery_test();
    if(argc>1 && std::string_view(argv[1])=="aer-stereo-fallback")return aer_stereo_fallback_test();
    if(argc>1 && std::string_view(argv[1])=="aer-input-warning")return aer_input_warning_test();
    if(argc>1 && std::string_view(argv[1])=="legacy-latency-scope")return legacy_latency_scope_test();
    std::string route=argc>1?argv[1]:"new";
    if(route.starts_with("late-2x-"))return private_completion_recovery_test(route.substr(8));
    if(route=="reclaim-sampling")return private_pressure_sampling_test();
    const bool reclaim_test=route.starts_with("reclaim-");
    if(reclaim_test)route=route.substr(8);
    const bool reclaim_transport=reclaim_test && route.starts_with("transport-");
    const bool reclaim_budget=reclaim_test && route.starts_with("budget-");
    if(reclaim_transport)route=route.substr(10);
    if(reclaim_budget)route=route.substr(7);
    const bool reclaim_held=reclaim_test && route.starts_with("held-");
    if(reclaim_held)route=route.substr(5);
    if(reclaim_test && route=="policy") {
        xrfg_fixture::ControlProfile profile;
        auto controls=xrfg::autonomous::snapshot().desired;controls.enabled=true;controls.afw_mode=0;controls.nr_enabled=false;
        check(xrfg::autonomous::request(controls),"recovery policy controls");xrfg::autonomous::apply_runtime_settings();
        xrfg::PrivateResourceRecovery p;
        p.completed(150000,true,100);p.completed(150000,false,200);check(!p.requested,"prime counted as generation pressure");
        p.completed(150000,true,2500);p.completed(150000,true,4800);check(p.requested,"repeated slow work did not request release");
        p.begin();p.released(5000);std::uint64_t tick=1000000000;
        for(unsigned n=0;n<32;++n){tick+=8000000;check(!p.real_frame(6000,tick,50000000),"stable frames bypassed minimum recovery interval");}
        tick+=200000000;check(!p.real_frame(9000,tick,50000000),"long gap counted as stable progress");
        for(unsigned n=0;n<15;++n){tick+=8000000;check(!p.real_frame(9000,tick,50000000),"too few stable observations triggered rebuild");}
        tick+=8000000;check(p.real_frame(9000,tick,50000000),"fast high-resolution observations failed to rearm");
        p.resumed();check(!p.active && !p.requested,"rearm retained stale pressure");
        for(unsigned n=0;n<3;++n){p.completed(150000,true,10000+n*11000);check(!p.requested,"isolated stalls triggered resource cycling");}
        // 120Hz: a 60ms stereo completion misses 2X/3X/4X budgets,
        // although all samples remain below the old 100ms trigger.
        for(const auto mult:{2U,3U,4U}) {
            p.resumed();const auto budget=8333*mult;
            for(unsigned n=0;n<5;++n){p.completed(60000,true,100+n*60,budget);check(!p.requested,"budget recovery is not sustained");}
            p.completed(60000,false,450,budget);check(!p.requested,"prime advanced budget streak");
            p.completed(60000,true,460,budget);check(p.requested,"below-100ms sustained deficit missed selected frame budget");
            p.resumed();
            for(unsigned n=0;n<12;++n)p.completed(budget,true,100+n*30,budget);
            check(!p.requested,"on-budget completion cycled resources");
            for(unsigned n=0;n<5;++n)p.completed(60000,true,500+n*60,budget);
            p.completed(budget,true,900,budget);p.completed(60000,true,960,budget);
            check(!p.requested,"healthy completion failed to reset deficit streak");
            p.resumed();for(unsigned n=0;n<5;++n)p.completed(60000,true,100+n*60,budget);
            p.completed(60000,true,3000,budget);check(!p.requested,"separated bursts combined into sustained pressure");
            p.resumed();for(unsigned n=0;n<5;++n)p.completed(60000,true,100+n*60,budget);
            p.completed(60000,true,500,budget+10000);check(!p.requested,"changed multiplier budget inherited old streak");
        }
        p.resumed();
        for(unsigned n=0;n<6;++n)p.completed(60000,true,100+n*60,33332+(n%2?4:0));
        check(p.requested,"tiny physical-clock measurement jitter erased sustained pressure");
        // Replay consecutive paired worker costs from PID5628 at 120Hz/4X.
        // Preparation plus completion matters: fence wait alone misses the
        // sustained deficit until much later in this reporter capture.
        p.resumed();
        const std::uint64_t folios_us[]={49853,67210,51410,48111,63012,45576};
        for(unsigned n=0;n<6;++n)p.completed(folios_us[n],true,225167+n*85,33332);
        check(p.requested,"Folios recorded worker-cost replay failed to request recovery");
        // Recovery must respect independent AFW/NR rather than force them off.
        { SessionState state(std::make_shared<Dispatch>());state.menu_enabled=true;
          state.pressure_resources_active=true;
          check(private_clean_presentation(state),"recovery did not select the Off presentation policy");
          state.independent_pacing=true;
          check(!private_clean_presentation(state),"Framework recovery policy leaked into Independent transport");
          state.independent_pacing=false;
          controls.afw_mode=1;check(xrfg::autonomous::request(controls),"independent AFW policy");xrfg::autonomous::apply_runtime_settings();
          check(!private_clean_presentation(state),"recovery forced independent AFW into effects-Off policy");
          controls.afw_mode=0;controls.nr_enabled=true;check(xrfg::autonomous::request(controls),"independent NR policy");xrfg::autonomous::apply_runtime_settings();
          check(!private_clean_presentation(state),"recovery forced independent NR into effects-Off policy");
          controls.nr_enabled=false;check(xrfg::autonomous::request(controls),"restore raw recovery policy");xrfg::autonomous::apply_runtime_settings();
          state.pressure_resources_active=false;check(!private_clean_presentation(state),"restart retained Off transport"); }
        std::puts("Resource recovery thresholds, prime exclusion, minimum time, hitch reset, sub-millisecond clock independence and retry PASS");return 0;
    }

    const bool late_shipping=route.starts_with("late-shipping-");
    if(late_shipping)route=route.substr(14);
    const bool binding_probe=route.starts_with("binding-probe-");
    bool binding_probe_pair{};
    if(binding_probe) {
        route.erase(0,14);binding_probe_pair=route.starts_with("pair-");
        route.erase(0,binding_probe_pair?5:6);route="separate-"+route;
    }
    const bool invalid_live=route.starts_with("invalid-live-");
    if(invalid_live)route.erase(0,13);
    if(route=="invalid-private-rect")return invalid_private_rect_test();
    if(route=="private-depth-trim")return private_depth_trim_test();
    if(route=="optical-pressure-recovery") {
        xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();
        auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
        auto state=std::make_shared<SessionState>(dispatch);
        state->d3d12_device=f.device12;state->d3d12_queue=f.queue;state->private_runtime=f.transport;
        state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->nvidia_options.rife_model=-1;state->nvidia_options.rife_multiplier=4;
        state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        check(optical_pressure_recovery_enabled(*state),"shipping optical 4X recovery scope");
        observe_optical_memory_pressure(*state,96,100);
        check(!optical_pressure_real_only(*state),"high memory alone must not disable generation");
        {
            xrfg::OpticalGpuPressure policy;
            policy.observe(99,100,100);
            check(!policy.real_only(100),"resident memory invented a delay");
            check(policy.completed(150000,true,100),"slow synthetic completion must back off");
            for(std::uint64_t t=101;t<2100;t+=100){policy.observe(99,100,t);check(policy.real_only(t),"bounded backoff ended early");}
            check(!policy.completed(200000,false,2000),"real-only prime extended backoff");
            policy.observe(99,100,2100);
            check(!policy.real_only(2100),"high resident memory blocked retry after cooldown");
            check(!policy.completed(30000,true,2100) && !policy.real_only(2100),"healthy retry must restore generation");
        }
        ComPtr<ID3D12Fence> fence;ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"late worker fence");
        PreparedProjectionFrame prepared;prepared.kind=PreparedGenerationKind::pair;
        PreparedGeneration generation;generation.kind=PreparedGenerationKind::pair;
        generation.worker_generation=std::make_shared<FrameGenerationSwapchainState>();
        generation.worker_fence=fence;generation.worker_fence_value=1;
        prepared.resources.push_back({reinterpret_cast<XrSwapchain>(7),std::move(generation),false});
        bool delayed{};
        auto waiting=std::async(std::launch::async,[&]{return finish_worker_outputs(state,prepared,1,&delayed);});
        const bool retained=waiting.wait_for(1150ms)==std::future_status::timeout;
        ok(fence->Signal(1),"complete delayed GPU submission");
        const bool completed=waiting.get();
        check(retained,"one-second timeout must not fail or return unfinished output");
        check(completed && delayed,"late complete must recover the shipping worker completion path");
        check(XR_SUCCEEDED(state->presenter_failure) && optical_pressure_real_only(*state),"late completion must enter cooldown without latching failure");
        {std::scoped_lock lock(state->optical_pressure_mutex);state->optical_pressure.recover_after_ms=0;}
        prepared.resources.front().generation.worker_fence_value=2;bool slow_delayed{};
        std::promise<void> entered;auto entered_future=entered.get_future();
        auto slow=std::async(std::launch::async,[&]{entered.set_value();return finish_worker_outputs(state,prepared,2,&slow_delayed);});
        entered_future.wait();const bool slow_retained=slow.wait_for(250ms)==std::future_status::timeout;
        ok(fence->Signal(2),"complete sub-timeout slow synthetic batch");
        const bool slow_completed=slow.get();
        check(slow_retained && slow_completed && slow_delayed && optical_pressure_real_only(*state),
            "measured sub-timeout GPU delay must initiate bounded backoff");
        bool missing_delayed{};
        check(wait_recoverable_private_fence(state,nullptr,1,missing_delayed)==E_INVALIDARG && !missing_delayed,
            "structural error must not enter recovery");
        state->nvidia_options.rife_multiplier=2;
        check(!optical_pressure_real_only(*state),"2X excluded from trial admission policy");
        state->nvidia_options.rife_multiplier=4;state->nvidia_options.rife_model=3;
        check(!optical_pressure_recovery_enabled(*state),"RIFE excluded from trial recovery");
        state->nvidia_options.rife_model=-1;
        ok(fence->Signal(3),"normal worker fence");bool normal_delayed{};
        check(wait_recoverable_private_fence(state,fence.Get(),3,normal_delayed)==S_OK && !normal_delayed,
            "normal completed work must retain fast path");
        {std::scoped_lock lock(state->presenter_mutex);state->presenter_stop_requested=true;}
        bool stopped_delayed{};
        check(wait_recoverable_private_fence(state,fence.Get(),4,stopped_delayed)==E_ABORT,
            "recovery must be interruptible by session stop");
        std::puts("Memory-only admission, bounded retry at persistent high VRAM, actual delayed D3D12 completion, normal/error/excluded paths and stop PASS");return 0;
    }
    const bool optical_pressure_delivery=route.starts_with("pressure-delivery-");
    if(optical_pressure_delivery)route.erase(0,18);
    const bool optical_high_memory_fast=route.starts_with("pressure-fast-");
    if(optical_high_memory_fast)route.erase(0,14);
    if(route=="pimax-ready-queue")return pimax_ready_queue_test();
    if(route.starts_with("native-vdxr-") || route.starts_with("native-pimax-") || route.starts_with("native-other-") || route.starts_with("steamvr-framework-"))return vdxr_multiframe_test(route);
    if(route=="producer-timelines")return producer_timelines_test();
    if(route=="producer-timelines-second")return producer_timelines_test(true);
    if(route=="independent-feature-settings")return independent_feature_settings_test();
    if(route=="independent-guide-inputs")return independent_feature_inputs_test();
    if(route=="independent-post-afw-inputs")return independent_feature_inputs_test(true);
    if(route=="aer4-stage-native-reader")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,true);
    if(route=="aer4-stage-d11-native-reader")return independent_aer4_stage_test(false,false,false,1,false,false,false,false,true);
    if(route=="aer4-failed-gap-owner")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,4);
    if(route=="aer4-disabled-gap-owner")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,5);
    if(route=="aer4-resized-gap-owner")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,6);
    if(route=="aer4-soft-gap-d12")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,1);
    if(route=="aer4-soft-gap-d11")return independent_aer4_stage_test(false,false,false,1,false,false,false,false,false,1);
    if(route=="aer4-soft-gap-transfer")return independent_aer4_stage_test(false,false,true,1,false,false,false,false,false,1);
    if(route=="aer4-nonmixed-gap")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,2);
    if(route=="aer4-layout-gap")return independent_aer4_stage_test(true,false,false,1,false,false,false,false,false,3);
    if(route=="aer4-disarm-retirement")return independent_aer4_stage_test(false,false,false,1,false,false,false,false,false,7);
    if(route=="aer4-stage")return independent_aer4_stage_test();
    if(route=="aer4-stage-d11")return independent_aer4_stage_test(false);
    if(route=="aer4-stage-d11-bgra")return independent_aer4_stage_test(false,true);
    if(route=="aer4-stage-d11-unmarked")return independent_aer4_stage_test(false,false,true);
    if(route=="aer4-stage-d11-bgra-unmarked")return independent_aer4_stage_test(false,true,true);
    if(route=="aer4-stage-d11-mips")return independent_aer4_stage_test(false,false,false,4);
    if(route=="aer4-stage-d11-bgra-mips")return independent_aer4_stage_test(false,true,false,4);
    if(route=="aer4-stage-d11-credits")return independent_aer4_stage_test(false,false,false,1,true);
    if(route=="aer4-stage-held")return independent_aer4_stage_test(true,false,false,1,false,true);
    if(route=="aer4-stage-d11-held")return independent_aer4_stage_test(false,false,true,1,false,true);
    if(route=="aer4-stage-d11-publication")return independent_aer4_stage_test(false,false,true,1,false,false,true);
    if(route=="aer4-stage-d11-alias")return independent_aer4_stage_test(false,false,false,1,false,false,false,true);
    if(route=="aer4-stage-d11-bgra-alias")return independent_aer4_stage_test(false,true,false,1,false,false,false,true);
    if(route=="independent-source-budget")return independent_source_admission_test();
    if(route=="independent-stale-guide-worker")return independent_stale_guide_worker_test();
    if(route=="independent-ready-admission")return independent_ready_admission_test();
    if(route=="independent-ready-admission-learned")return independent_ready_admission_test(true);
    if(route=="independent-stage-overlap")return independent_stage_overlap_test();
    if(route=="independent-d11-binding")return independent_d11_binding_test();
    const bool independent=route.starts_with("independent-");if(independent)route=route.substr(12);
    const bool independent_paired_split=independent && route.starts_with("paired-split-");if(independent_paired_split)route=route.substr(13);
    const bool independent_resize=independent && route.starts_with("resize-");if(independent_resize)route=route.substr(7);
    const bool independent_offon=independent && route.starts_with("offon-");if(independent_offon)route=route.substr(6);
    const bool independent_tickets=independent && route.starts_with("tickets-");if(independent_tickets)route=route.substr(8);
    const bool independent_runtime_tail=independent && route.starts_with("runtime-tail-");if(independent_runtime_tail)route=route.substr(13);
    const bool independent_runtime_tail_legacy=independent_runtime_tail && route.starts_with("legacy-");if(independent_runtime_tail_legacy)route=route.substr(7);
    const bool independent_input_timing=independent && route.starts_with("input-timing-");if(independent_input_timing)route=route.substr(13);
    const bool independent_input_timing_off=independent_input_timing && route.starts_with("off-");if(independent_input_timing_off)route=route.substr(4);
    const bool independent_vdxr=independent && route.starts_with("native-vdxr-");if(independent_vdxr)route=route.substr(12);
    const bool independent_other=independent && route.starts_with("native-other-");if(independent_other)route=route.substr(13);
    const bool independent_held=independent && route.starts_with("worker-held-");if(independent_held)route=route.substr(12);
    const bool independent_aer=independent && route.starts_with("aer-");if(independent_aer)route=route.substr(4);
    const bool independent_aer4=independent_aer && route.starts_with("4x-");if(independent_aer4)route=route.substr(3);
    const bool independent_alternate=independent_aer && route.starts_with("alt-");if(independent_alternate)route=route.substr(4);
    const bool independent_stream_hold=independent_paired_split && route.starts_with("stream-");if(independent_stream_hold)route=route.substr(7);
    if(route.starts_with("aer-alternate-"))return aer_alternate_delivery_test(route);
    const bool aer_path=route.starts_with("aer-");if(aer_path)route.erase(0,4);
    const bool aer_swap=aer_path&&route.starts_with("swap-");if(aer_swap)route.erase(0,5);
    if(route=="native-guide-cache")return aer_native_guide_cache_test();
    if(route=="motion-interval")return aer_motion_interval_gpu_test();
    if(route=="native-guide-join")return aer_native_guide_join_test();
    if(route=="rife-memory-recovery") {
        xrfg_fixture::ControlProfile profile;Fixture f(true);f.unblock();
        auto dispatch=std::make_shared<Dispatch>();auto state=std::make_shared<SessionState>(dispatch);
        state->d3d12_device=f.device12;state->d3d12_queue=f.queue;state->private_runtime=f.transport;
        state->nvidia_options.rife_model=3;state->handle=reinterpret_cast<XrSession>(77);
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        auto memory=std::make_shared<SwapchainState>(state,create),structural=std::make_shared<SwapchainState>(state,create),old=std::make_shared<SwapchainState>(state,create);
        memory->handle=f.make_chain();structural->handle=f.make_chain();old->handle=f.make_chain();
        memory->generation_declined=true;memory->generation_refusal=SwapchainEligibilityReason::synthesis_initialize_failed;memory->generation_refusal_detail=static_cast<std::uint64_t>(E_OUTOFMEMORY);
        structural->generation_declined=true;structural->generation_refusal=SwapchainEligibilityReason::protected_content;
        old->frame_generation=std::make_shared<FrameGenerationSwapchainState>();std::weak_ptr<FrameGenerationSwapchainState> retired=old->frame_generation;
        {std::scoped_lock lock(g_state_mutex);g_swapchains[memory->handle]=memory;g_swapchains[structural->handle]=structural;g_swapchains[old->handle]=old;}
        state->rife_memory_retry_pending=true;state->generation_initialization_result=E_OUTOFMEMORY;
        ok(recover_rife_memory(state),"healthy allocation refusal may recover once");
        check(retired.expired() && !old->frame_generation,"old working set retired before any replacement allocation");
        check(!memory->generation_declined && memory->generation_eligible_pending,"exact memory refusal reopens source admission");
        check(structural->generation_declined && structural->generation_refusal==SwapchainEligibilityReason::protected_content,"structural refusals stay quarantined");
        check(state->control_reconfigure_required && state->generation_initialization_result==S_OK,"recovery requests real reconfiguration and clears old error");
        state->rife_memory_retry_pending=true;
        check(recover_rife_memory(state)==S_FALSE,"second failure in same configuration cannot spin on allocation retries");
        auto desired=xrfg::embedded::snapshot().desired;desired.rife_model=3;desired.rife_resolution=21;desired.enabled=true;
        check(xrfg::embedded::request(desired),"new explicit provider revision");
        state->control_revision=xrfg::embedded::snapshot().revision;
        state->rife_memory_retry_pending=true;
        ok(apply_embedded_control(state),"recovery must bypass unchanged-revision early return and really apply");
        check(!state->control_reconfigure_required && state->menu_enabled && state->rife_ready_after_ms,
            "one-shot recovery finishes real control application and demands a fresh inference");
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();}
        xr_ok(f.transport->destroy_swapchain(memory->handle),"memory source cleanup");xr_ok(f.transport->destroy_swapchain(structural->handle),"structural source cleanup");xr_ok(f.transport->destroy_swapchain(old->handle),"old source cleanup");
        state->private_runtime.reset();std::puts("RIFE memory retry: old set retirement, exact refusal, original source ownership and hard one-attempt bound PASS");return 0;
    }
    if(route=="overlay-image-identity" || route=="overlay-multiframe-identity") {
        const bool multi=route=="overlay-multiframe-identity";
        xrfg_fixture::ControlProfile profile;
        for(bool d12:{true,false})for(unsigned factor:(multi?std::vector<unsigned>{2,3,4}:std::vector<unsigned>{1})) {
            Fixture f(d12);f.throttled=false;f.native_image_count=multi?3:1;f.unblock();
            xr_ok(f.transport->set_buffer_capacity(3,true),"buffered UI test capacity");
            const auto projection_chain=f.make_chain(),menu_chain=f.make_chain(),meter_chain=f.make_chain();
            f.inspect_layers=[&](const XrFrameEndInfo* info) {
                const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
                const auto frame=unsigned(p->views[0].pose.position.x);
                std::scoped_lock gpu(f.gpu);
                for(unsigned i=1;i<info->layerCount;++i) {
                    const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
                    const auto expected=q->pose.position.x==100?50+frame:150+(frame/3)*3;
                    check(f.read_pixel(f.chains.at(q->subImage.swapchain))==expected,
                        "equal-sized menu and meter pixels must follow source identity, not layer ordinal/native image index");
                }
            };
            for(unsigned frame=1;frame<=(multi?36U:12U);++frame) {
                if(frame%4==1)xr_ok(f.transport->set_director_enabled(((frame-1)/4)%2),"switch UI transport policy");
                f.write(projection_chain,20+frame);f.write(menu_chain,50+frame);
                if(frame==1 || frame%3==0)f.write(meter_chain,150+(frame/3)*3);
                f.begin_virtual();
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose.orientation.w=1;view.pose.position.x=float(frame);view.subImage.swapchain=projection_chain;view.subImage.imageRect.extent={1,1};
                XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
                XrCompositionLayerQuad menu{XR_TYPE_COMPOSITION_LAYER_QUAD};menu.space=projection.space;menu.pose.orientation.w=1;menu.pose.position.x=100;menu.size={1,1};menu.subImage.swapchain=menu_chain;menu.subImage.imageRect.extent={1,1};
                auto meter=menu;meter.pose.position.x=200;meter.subImage.swapchain=meter_chain;
                std::array<const XrCompositionLayerBaseHeader*,3> layers={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(frame%2?&menu:&meter),reinterpret_cast<const XrCompositionLayerBaseHeader*>(frame%2?&meter:&menu)};
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns()+period;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=3;end.layers=layers.data();
                xr_ok(f.transport->end_frame(&end),"two independent UI sources accepted");
                if(!multi || frame%factor==0)f.wait_count(frame);
            }
            xr_ok(f.transport->drain(),"UI identity drain");
        }
        std::puts("Menu/meter source identity, reordered equal-sized quads, repeated images and policy changes D11+D12 PASS");return 0;
    }
    if(route=="ordinary-swapchain-inventory") {
        xrfg_fixture::ControlProfile profile;
        Fixture f(true);
        auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
        auto state=std::make_shared<SessionState>(dispatch);
        state->handle=reinterpret_cast<XrSession>(2);state->private_runtime=f.transport;
        state->luke_afw_enabled=false;
        const auto chain=f.make_chain();
        const auto count=f.chains.size();
        const auto inventory=session_swapchain_inventory(state,nullptr);
        check(inventory.updated_ms && inventory.total==1 && inventory.unassigned_native==1,
            "non-Luke/UEVR session must report its observed native endpoint");
        check(f.chains.size()==count,"inventory must not allocate any XR images");
        xr_ok(f.transport->destroy_swapchain(chain),"inventory source cleanup");
        state->next_swapchain_inventory_ms=0;
        check(session_swapchain_inventory(state,nullptr).total==0,"inventory drops destroyed native endpoints");
        std::puts("Ordinary/UEVR swapchain inventory without Luke PASS");return 0;
    }
    if(route=="rife-output-banks")return rife_output_banks_test(false);
    if(route=="rife-output-bank-policy")return rife_output_banks_test(true);
    if(route=="rife-source-handoff") {
        auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        auto left=std::make_shared<SwapchainState>(state,info),right=std::make_shared<SwapchainState>(state,info);
        // The production admission predicate must include leased raw Luke AER,
        // while rejecting shared ingress, reconstructed inputs and auxiliaries.
        state->nvidia_options.rife_model=0;state->nvidia_options.ofr=true;
        state->graphics_binding=SessionGraphicsBinding::d3d11;state->luke_afw_enabled=true;
        left->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        left->frame_generation=std::make_shared<FrameGenerationSwapchainState>();
        auto& generation=*left->frame_generation;
        generation.d3d11_interop=std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        check(!early_source_capture(*left),"shared Luke interop retains strict completion");
        generation.isolated_luke_synthesis=true;
        check(early_source_capture(*left),"isolated raw Luke AER can capture after owned-input signal");
        state->nvidia_options.ofr=false;
        check(!early_source_capture(*left),"ordinary Luke/post-AFW path retains strict completion");
        state->nvidia_options.ofr=true;left->composition_referenced=true;
        check(!early_source_capture(*left),"shared auxiliary layer cannot consume early source credit");
        left->composition_referenced=false;
        state->three_slot_director=true;state->synthesis_busy=true;state->synthesis_capture_sequence=1;
        auto a=std::async(std::launch::async,[&]{return wait_for_synthesis_handoff(state,left.get());});
        check(a.wait_for(30ms)==std::future_status::timeout,"capture cannot race source snapshot/lease acquisition");
        {std::scoped_lock lock(state->presenter_mutex);state->synthesis_capture_ready=true;}
        state->presenter_condition.notify_all();
        check(a.wait_for(1s)==std::future_status::ready,"owned inputs must release next capture before GPU completion");
        xr_ok(a.get(),"left early capture");
        xr_ok(wait_for_synthesis_handoff(state,right.get()),"right early capture");
        check(state->synthesis_busy,"early capture must not admit another EndFrame synthesis job");
        auto duplicate=std::async(std::launch::async,[&]{return wait_for_synthesis_handoff(state,left.get());});
        auto strict=std::async(std::launch::async,[&]{return wait_for_synthesis_handoff(state);});
        check(duplicate.wait_for(30ms)==std::future_status::timeout,"only one early capture per eye/job");
        check(strict.wait_for(30ms)==std::future_status::timeout,"bridges/composition/non-RIFE retain full completion wait");
        {std::scoped_lock lock(state->presenter_mutex);state->synthesis_busy=false;}
        state->presenter_condition.notify_all();xr_ok(duplicate.get(),"bounded next capture");xr_ok(strict.get(),"strict completion");
        {std::scoped_lock lock(state->presenter_mutex);state->synthesis_busy=true;state->synthesis_capture_ready=false;++state->synthesis_capture_sequence;}
        auto next=std::async(std::launch::async,[&]{return wait_for_synthesis_handoff(state,left.get());});
        check(next.wait_for(30ms)==std::future_status::timeout,"new job must not inherit the old owned-input signal");
        {std::scoped_lock lock(state->presenter_mutex);state->synthesis_capture_ready=true;}
        state->presenter_condition.notify_all();check(next.wait_for(1s)==std::future_status::ready,"next job has a fresh capture credit");
        xr_ok(next.get(),"fresh early capture");
        auto failure=std::async(std::launch::async,[&]{return wait_for_synthesis_handoff(state,left.get());});
        check(failure.wait_for(30ms)==std::future_status::timeout,"credit remains bounded on second job");
        {std::scoped_lock lock(state->presenter_mutex);state->presenter_failure=XR_ERROR_RUNTIME_FAILURE;}
        state->presenter_condition.notify_all();check(failure.get()==XR_ERROR_RUNTIME_FAILURE,"worker failure wakes a pending capture with its error");
        std::puts("RIFE source snapshot ownership, per-eye capture credit, strict paths, restart and failure PASS");return 0;
    }
    if(route=="consumer-status") {
        xrfg_fixture::ControlProfile profile;
        auto desired=xrfg::autonomous::snapshot().desired;
        xrfg::autonomous::select_generation(desired,1,0);
        desired.enabled=true;desired.advanced_mode=true;desired.ofr_enabled=true;
        check(xrfg::autonomous::request(desired),"native consumer status setup");
        xrfg::autonomous::apply_runtime_settings();
        xrfg::report_dlss_motion_vector_status(xrfg::DlssMotionVectorStatus::temporal_mismatch);
        const auto publish=[&](xrfg::autonomous::GenerationState state,std::uint64_t proof) {
            xrfg::autonomous::publish_runtime_status(0,state,S_OK,false,0,nullptr,-1,false,false,
                false,false,false,true,0,false,0,2,0,proof);
            return xrfg::autonomous::snapshot().runtime.generation_state;
        };
        using S=xrfg::autonomous::GenerationState;
        check(publish(S::ready,GetTickCount64())==S::ready,"actual native consumer success outranks unrelated global pair mismatch");
        check(publish(S::ready,0)!=S::ready,"without consumer proof a genuine missing/mismatched pair remains visible");
        check(publish(S::dispatch_failed,GetTickCount64())==S::dispatch_failed,"fresh success cannot hide current provider failure");
        check(publish(S::ready,GetTickCount64()-3000)!=S::ready,"old native success cannot mask later guide loss");
        std::puts("Native consumer status: exact success, missing proof, provider failure and expiry PASS");return 0;
    }
    if(route=="ofr-controls") {
        xrfg_fixture::ControlProfile profile;
        auto dispatch=std::make_shared<Dispatch>();auto session=std::make_shared<SessionState>(dispatch);
        for(int method:{0,1,2,3,4})for(bool swap:{false,true}) {
            auto controls=xrfg::autonomous::snapshot().desired;
            controls.advanced_mode=true;controls.ofr_enabled=true;controls.ofr_swap=swap;
            controls.enabled=true;controls.frame_generation=method;controls.backend=2;
            controls.scale=3;controls.rife_multiplier=4;controls.frame_multiplier=4;controls.triple_frame_gen=true;
            check(xrfg::autonomous::request(controls),"AER controls request");
            xrfg::autonomous::apply_runtime_settings();
            auto requested=xrfg::embedded::snapshot().desired;
            check(requested.ofr && requested.ofr_swap==swap,"AER reaches shipping runtime");
            auto effective=effective_session_settings(*session,requested);
            check(effective.enabled && effective.backend==requested.backend && effective.scale==requested.scale &&
                effective.hybrid==(method==3) && effective.rife_model==(method==4?controls.rife_model:-1) &&
                effective.frame_generation==(method>=3?0:method) && effective.rife_multiplier==2,
                "AER preserves chosen generator and quality, limits only multiplier");
            check(!xrfg::embedded::djules_pipeline_snapshot().triple_frame_gen,"AER suppresses normal triple request");
            check(selected_afw_mode()==0,"AER bypasses OSR/AFW while framegen runs");
            session->nvidia_options.ofr=true;session->nvidia_options.ofr_swap=swap;
            session->menu_enabled=true;session->djules_triple_requested=true;
            session->ofr_delivery=2;check(djules_application_period(*session,11'111'111)==22'222'222,"paired physical pacing");
            session->ofr_delivery=1;check(djules_application_period(*session,11'111'111)==11'111'111,"alternate physical pacing");
            check(!djules_triple_active(*session),"ordinary triple preference cannot add OFR outputs");
            // A generator/menu reset must not reset source-eye observation.
            session->ofr_source_tracker.reset();
            std::array<xrfg::ofr::Source,2> observed{{{11,101,1},{22,202,1}}};
            session->ofr_source_tracker.observe(observed);
            for(unsigned frame=0;frame<24;++frame) {
                session->previous_projection=ProjectionSnapshot{};
                clear_generation_continuity(session);
                ++observed[frame%2].serial;
                const auto decision=session->ofr_source_tracker.observe(observed);
                check(decision.fresh==(1u<<(frame%2)) && decision.continuity,
                    "shipping continuity reset preserves alternating source observation");
            }
            GeneratedFrameEndInfo first,second;ProjectionSnapshot previous;
            first.projections.resize(1);second.projections.resize(1);previous.layers.resize(1);
            first.projections[0].views.resize(2);second.projections[0].views.resize(2);previous.layers[0].views.resize(2);
            for(unsigned eye=0;eye<2;++eye) {
                first.projections[0].views[eye].pose.position.x=float(10+eye);
                second.projections[0].views[eye].pose.position.x=float(20+eye);
                previous.layers[0].views[eye].pose.position.x=float(eye);
                first.projections[0].views[eye].subImage.imageArrayIndex=eye;
                second.projections[0].views[eye].subImage.imageArrayIndex=eye;
            }
            check(realign_ofr_paired_metadata(first,second,previous,swap),"OFR metadata accepted");
            check(first.interface_stamp.aer_mixed && second.interface_stamp.aer_mixed,
                "both paired AER outputs retain mixed content telemetry");
            const auto newer=xrfg::ofr::newer_eye(swap),older=newer^1;
            check(first.projections[0].views[newer].pose.position.x==float(newer) &&
                second.projections[0].views[newer].pose.position.x==float(10+newer),"endpoint cameras follow selected pixels");
            check(first.projections[0].views[older].pose.position.x==float(10+older) &&
                second.projections[0].views[older].pose.position.x==float(20+older),"other eye cameras retained");
            for(unsigned eye=0;eye<2;++eye)check(first.projections[0].views[eye].subImage.imageArrayIndex==eye &&
                second.projections[0].views[eye].subImage.imageArrayIndex==eye,"Swap never exchanges physical eyes");
            controls.enabled=false;
            check(xrfg::autonomous::request(controls),"AER master off");xrfg::autonomous::apply_runtime_settings();
            check(!effective_session_settings(*session,xrfg::embedded::snapshot().desired).enabled &&
                selected_afw_mode()==xrfg::embedded::afw_snapshot().desired.mode,"FG master off disables AER and restores reconstruction policy");
            controls.enabled=true;controls.ofr_enabled=false;
            check(xrfg::autonomous::request(controls),"AER deselect");xrfg::autonomous::apply_runtime_settings();
            effective=effective_session_settings(*session,xrfg::embedded::snapshot().desired);
            check(!effective.ofr && effective.enabled && effective.rife_multiplier==xrfg::autonomous::runtime_multiplier(xrfg::autonomous::snapshot().desired),"ordinary multiplier preference restored");
        }
        auto disabled=xrfg::autonomous::snapshot().desired;
        disabled.enabled=false;disabled.afw_mode=0;disabled.nr_enabled=false;disabled.r19_camera=true;
        check(xrfg::autonomous::request(disabled),"all image effects off");xrfg::autonomous::apply_runtime_settings();
        check(all_image_effects_off() && !blackbar_gate(*session),"all-off disables prediction correction despite stored camera preference");
        disabled.nr_enabled=true;
        check(xrfg::autonomous::request(disabled),"NR-only is not all-off");xrfg::autonomous::apply_runtime_settings();
        check(!all_image_effects_off(),"NR remains a real image effect while FG is disabled");
        std::puts("OFR shipping controls, effective engine, camera endpoints and physical pacing PASS");return 0;
    }
    if(route=="reconstruction-controls") {
        xrfg_fixture::ControlProfile profile;
        for(int engine:{0,2}) {
            auto controls=xrfg::autonomous::snapshot().desired;
            controls.luke_osr=engine;controls.afw_mode=3;
            check(xrfg::autonomous::request(controls),"enable selected reconstruction");
            xrfg::autonomous::apply_runtime_settings();
            check(xrfg::embedded::afw_snapshot().desired.mode==3 &&
                xrfg::embedded::afw_snapshot().desired.osr==engine,"selected engine reaches runtime controls");
            controls.afw_mode=0;
            check(xrfg::autonomous::request(controls),"primary Off request");
            xrfg::autonomous::apply_runtime_settings();
            const auto actual=xrfg::embedded::afw_snapshot();
            check(actual.desired.mode==0,"Off reaches the guard shared by both reconstruction engines");
            check(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(controls)).afw_mode==0,
                "saved Off remains Off on profile reload");
            xrfg::autonomous::publish_runtime_status();
            const auto status=xrfg::autonomous::snapshot();
            check(!status.runtime.afw_running,"Off cannot report running reconstruction");
        }
        std::puts("Primary reconstruction Off reaches shipping runtime controls and persists for OSR/AFW PASS");return 0;
    }

    if(route=="fast-panel-source") {
        auto dispatch=std::make_shared<Dispatch>();auto state=std::make_shared<SessionState>(dispatch);
        state->graphics_binding=SessionGraphicsBinding::d3d12;
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        check(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"panel device");
        D3D12_COMMAND_QUEUE_DESC desc{};
        check(SUCCEEDED(device->CreateCommandQueue(&desc,IID_PPV_ARGS(&state->d3d12_queue))),"panel queue");
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};create.arraySize=1;
        auto chain=std::make_shared<SwapchainState>(state,create);chain->handle=reinterpret_cast<XrSwapchain>(991);
        {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;view.fov={-.7F,.7F,.7F,-.7F};
        view.subImage.swapchain=reinterpret_cast<XrSwapchain>(990);view.subImage.imageRect.extent={4,4};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
        XrCompositionLayerQuad panel{XR_TYPE_COMPOSITION_LAYER_QUAD};panel.space=projection.space;panel.pose.orientation.w=1;panel.size={1,1};
        panel.subImage.swapchain=chain->handle;panel.subImage.imageRect.extent={4,4};
        const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&panel)};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=2;end.layers=layers;
        ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"panel snapshot");snapshot.panel_corrections=true;
        check(panel_sources(*state,snapshot,{}).size()==1,"normal mode must retain panel generation");
        for(auto flags:{std::pair{1,0},std::pair{0,1},std::pair{1,1}}) {
            state->lower_vram=flags.first;state->separate_synthesis=flags.second;
            check(panel_sources(*state,snapshot,{}).empty(),"Fast must not acquire an unsignalled generated panel image");
        }
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(chain->handle);}
        check(!chain->panel_source && !chain->frame_generation,"selection test created hidden panel work");
        std::puts("Fast panel source exclusion and unchanged normal admission PASS");return 0;
    }
    if(route=="osr-peer-pose") {
        std::array<XrCompositionLayerProjectionView,2> views{};
        for(unsigned i=0;i<2;++i) {
            auto& v=views[i];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
            v.pose.position={float(i),.1F,.2F};v.fov={-.7F,.7F,.7F,-.7F};
            v.subImage.swapchain=reinterpret_cast<XrSwapchain>(12+i);v.subImage.imageRect.extent={64,64};
        }
        views[1].pose.orientation={0,.1F,0,std::sqrt(.99F)};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(1);
        projection.viewCount=2;projection.views=views.data();const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=123;end.layerCount=1;end.layers=&layer;
        ProjectionSnapshot original;check(capture_projection_snapshot(&end,&original),"capture original AER metadata");
        auto pair=std::make_shared<HeldAfwPair>();pair->projection=original;
        ProjectionResourceMapping mapping;mapping.views.push_back({0,1});
        check(apply_osr_peer_position(pair->projection,mapping,{.068F,.1F,.2F}),"correct only reconstructed peer position");
        check(std::memcmp(&pair->projection.layers[0].views[0],&original.layers[0].views[0],sizeof(views[0]))==0,"real source eye unchanged");
        const auto& peer=pair->projection.layers[0].views[1];
        check(std::memcmp(&peer.pose.orientation,&views[1].pose.orientation,sizeof(XrQuaternionf))==0 &&
            std::memcmp(&peer.fov,&views[1].fov,sizeof(XrFovf))==0,"peer orientation and FOV unchanged");
        PreparedProjectionFrame produced;produced.kind=PreparedGenerationKind::prime;produced.scene_serial=5;
        for(unsigned i=0;i<2;++i){PreparedProjectionResource r;r.application_swapchain=views[i].subImage.swapchain;
            r.generation.current_handle=reinterpret_cast<XrSwapchain>(100+i);produced.resources.push_back(r);}
        produced.reconstructed_afw_pair=pair;
        check(!produced.held_afw_pair,"fresh corrected metadata must not signal a held source");
        for(bool held:{false,true}) {
            if(held){produced.held_afw_pair=pair;produced.reconstructed_afw_pair.reset();}
            GeneratedFrameEndInfo terminal;ProjectionSnapshot snapshot;
            check(build_afw_terminal_submission(&end,original,produced,terminal,snapshot),"fresh/held terminal ownership");
            check(terminal.new_content==!held,
                "fresh OSR owns a timing reference; held reconstruction cannot become a new latency sample");
            check(snapshot.layers[0].views[1].pose.position.x==.068F && snapshot.layers[0].views[0].pose.position.x==0,
                "terminal and downstream snapshot preserve the exact corrected position");
            check(terminal.info.displayTime==123,"metadata correction does not reschedule displayTime");
        }
        produced.held_afw_pair.reset();GeneratedFrameEndInfo legacy;ProjectionSnapshot legacy_snapshot;
        check(build_afw_terminal_submission(&end,original,produced,legacy,legacy_snapshot) &&
            legacy_snapshot.layers[0].views[1].pose.position.x==1,"absent OSR receipt preserves the existing AFW/pass-through metadata");
        std::puts("OSR fresh/held terminal metadata, unchanged source/orientation/FOV/time and legacy PASS");return 0;
    }
    if(route=="rife-unsupported-ack") {
        xrfg_fixture::ControlProfile profile;
        auto desired=xrfg::autonomous::snapshot().desired;
        xrfg::autonomous::select_generation(desired,4,2);desired.rife_multiplier=3;
        check(xrfg::autonomous::request(desired),"unsupported legacy RIFE3 UI request");
        xrfg::autonomous::apply_runtime_settings();
        auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());state->handle=reinterpret_cast<XrSession>(2);
        state->graphics_binding=SessionGraphicsBinding::d3d11;
        state->menu_enabled=true;state->nvidia_options.rife_model=3;state->nvidia_options.rife_multiplier=2;
        check(apply_embedded_control(state)==E_NOTIMPL,"legacy transport must refuse unsupported multi-output RIFE");
        const auto receipt=xrfg::embedded::snapshot();
        check(!receipt.pending && receipt.errors && receipt.last_error==E_NOTIMPL,
            "unsupported RIFE revision must finish pending with an explicit error");
        check(state->menu_enabled && state->nvidia_options.rife_multiplier==2,
            "refused RIFE3 request must preserve the previous enable state and multiplier");
        std::puts("Unsupported RIFE3: explicit acknowledgement/error, no perpetual Applying settings, previous2X preserved PASS");
        return 0;
    }
    const bool rife_bank_switch=route.starts_with("rife-bank-switch");
    if(rife_bank_switch)route.replace(0,std::string("rife-bank-switch").size(),"rife-switch");
    const bool rife_msfs=route.starts_with("rife-msfs");
    const bool rife_inherited=rife_msfs && route.find("inherited")!=std::string::npos;
    const auto overlap_tag=route.find("-app-overlap");
    const bool rife_app_overlap=overlap_tag!=std::string::npos || rife_msfs;
    if(overlap_tag!=std::string::npos)route.erase(overlap_tag,std::string("-app-overlap").size());
    const bool panel_publication_test=route.starts_with("panel-pooled-");
    if(panel_publication_test)route="panel-budget-"+route.substr(13);
    const bool budget_fallback_test=route.starts_with("budget-fallback-");
    if(budget_fallback_test)route="panel-budget-"+route.substr(16);
    const bool panel_lifecycle_test=route.starts_with("panel-close-");
    if(panel_lifecycle_test)route="panel-budget-"+route.substr(12);
    const bool panel_budget_test=route.starts_with("panel-budget-");
    if(panel_budget_test)route="panels-"+route.substr(13);
    const bool panel_test=route.starts_with("panels-");if(panel_test)route=route.substr(7);
    const bool adaptive_framework=route.starts_with("adaptive-framework-");
    const bool adaptive_director=route.starts_with("adaptive-director-");
    if(adaptive_framework)route="large-"+route.substr(19);
    if(adaptive_director)route="large-"+route.substr(18);
    if(independent && route=="aux-d12")return auxiliary_slot_selection_test(4);
    if(independent && route=="aux-d11")return auxiliary_slot_selection_test(5);
    if(independent && route=="luke-aux-d11")return auxiliary_slot_selection_test(6);
    if(!independent && route=="luke-aux-legacy")return auxiliary_slot_selection_test(7);
    if(route=="auxiliary-slot-available")return auxiliary_slot_selection_test(0);
    if(route=="auxiliary-slot-lazy")return auxiliary_slot_selection_test(1);
    if(route=="auxiliary-slot-all-busy")return auxiliary_slot_selection_test(2);
    if(route=="uevr-auxiliary-pooled-budget")return auxiliary_slot_selection_test(3);
    if(route=="auxiliary-retirement")return auxiliary_slot_selection_test(4);
    if(route=="auxiliary-retirement-pooled")return auxiliary_slot_selection_test(5);
    if(route=="overlay-admission" || route=="overlay-admission-measured")
        return overlay_admission_test(route.ends_with("measured"));
    if(route=="admission-credit")return admission_credit_test();
    if(route=="meter-native-confirmation")return independent_meter_test();
    if(route=="uevr-camera-loss")return uevr_camera_loss_test();
    if(route=="uevr-source-binding")return uevr_camera_loss_test(true);
    if(route=="uevr-independent-source-fence")return independent_source_queue_test();
    const bool rife_luke=route.starts_with("rife-luke");
    const unsigned rife_luke_factor=route.find("luke4")!=std::string::npos?4:route.find("luke2")!=std::string::npos?2:3;
    const bool rife_tail=route.starts_with("rife-tail");
    const bool rife_overlap=route.starts_with("rife-overlap");
    const bool rife_overlap_capture=rife_overlap && route.find("capture")!=std::string::npos;
    bool rife_early_capture_done=false;
    const bool rife_overlap_retire=rife_overlap && (rife_overlap_capture || route.find("retire")!=std::string::npos);
    const unsigned rife_overlap_factor=route.find("overlap4")!=std::string::npos?4:3;
    const unsigned rife_tail_factor=route.find("tail3")!=std::string::npos?3:4;
    const bool rife_latency=route.find("rife-latency")!=std::string::npos;
    const unsigned rife_latency_factor=route.find("latency3")!=std::string::npos?3:4;
    const bool rife_report=route=="rife-report";
    const bool rife_replay=route=="rife-replay";
    const bool rife_switch=(rife_msfs && route.find("switch")!=std::string::npos) || route.starts_with("rife-switch") || (rife_luke && route.find("switch")!=std::string::npos);
    const bool rife_stress=route.find("stress")!=std::string::npos;
    const bool rife_large=route.find("headset")!=std::string::npos;
    const bool rife_buffer3=route.find("buffer3")!=std::string::npos;
    const unsigned rife_phase_frames=rife_stress?64:12;
    const int rife_pacing=route.ends_with("director")?1:0;
    const bool rife_fast=route.find("fast")!=std::string::npos;
    const unsigned rife_factor=rife_msfs?2:rife_luke?(rife_switch?2:rife_luke_factor):rife_overlap?rife_overlap_factor:rife_tail?rife_tail_factor:rife_latency?rife_latency_factor:(rife_report||rife_replay)?4:rife_switch?2:route=="rife3"?3:route=="rife4"?4:route=="rife2"?2:0;
    if(rife_factor)route=rife_luke?(rife_pacing?"hfw-solo-luke-d11-new":"hfw-luke-d11-new"):rife_overlap?"solo-new":"new";
    // OFXR/Hybrid at 3X/4X on RIFE's ordered route: multi3-new, multi4-fidelity, ...
    // multiswitch-<route> changes the multiplier live, 2-3-4-3-2, as rife-switch;
    // a -director suffix runs Director pacing.
    const bool ofxr_switch=route.starts_with("multiswitch-");
    const unsigned ofxr_factor=ofxr_switch?2:route.starts_with("multi3-")?3:route.starts_with("multi4-")?4:0;
    if(ofxr_switch)route=route.substr(12);else if(ofxr_factor)route=route.substr(7);
    const int ofxr_pacing=ofxr_factor && route.ends_with("-director")?1:0;
    if(ofxr_pacing)route.resize(route.size()-9);
    const bool phase_switch=rife_switch || ofxr_switch;

    if(route=="vram-budget")return steamvr_vram_budget_test(argc,argv);
    if(route=="unbridged-vulkan-session")return unbridged_vulkan_session_test();
    if(route=="panel-metadata")return panel_metadata_contract();
    const bool steady_benchmark=route=="steady-benchmark-sequence" || route=="vram-steady-benchmark-sequence";
    const bool steady_memory=route.starts_with("vram-steady-") || steady_benchmark;
    if(steady_benchmark)route="benchmark-sequence";
    else if(steady_memory)route=route.substr(12);
    constexpr unsigned memory_phase_frames=16;
    const UINT image_width=rife_large?3736:steady_memory?(argc>2?UINT(std::stoul(argv[2])):2048):256;
    const UINT image_height=rife_large?3424:steady_memory?image_width:128;
    check(!steady_memory || (image_width>=128 && image_width<=4096),"steady VRAM per-eye extent 128..4096");
    const bool hfw=route.starts_with("hfw-");if(hfw)route=route.substr(4);
    const bool hfw_early=hfw && route.starts_with("early-");if(hfw_early)route=route.substr(6);
    const bool hfw_offon=hfw && route.starts_with("offon-");if(hfw_offon)route=route.substr(6);
    const bool buffer_toggling=route.starts_with("buffer-");if(buffer_toggling)route=route.substr(7);
    const bool large_buffer=route.starts_with("large-");if(large_buffer)route=route.substr(6);
    const bool force_vdxr_steamvr=route=="force-vdxr-steamvr-session";
    const bool force_steamvr_vdxr=route=="force-steamvr-vdxr-session";
    const bool unknown_session=route=="other-session";
    const bool pimax_session=route=="pimax-session" || route=="force-framework-pimax-session";
    const bool force_framework_pimax=route=="force-framework-pimax-session";
    const bool vdxr_probe=route.starts_with("vdxr-") || force_steamvr_vdxr;
    if(route.starts_with("vdxr-"))route=route.substr(5);
    if(force_vdxr_steamvr || force_steamvr_vdxr || unknown_session || pimax_session)route="session";
    if(vdxr_probe)period=8'333'333; // 120Hz runtime predictions, no SteamVR timing API
    vdxr_legacy_probe=force_vdxr_steamvr || route=="legacy-session" || (pimax_session && !force_framework_pimax);if(vdxr_legacy_probe)route="session";
    if(route.starts_with("afw-only") || route=="luke-shared-session")return afw_only_session_test(route,argc>2?argv[2]:nullptr,independent);
    const bool shared_budget=route=="luke-shared-budget",unshared_budget=route=="luke-unshared-budget" || route=="outlaws-unshared-budget";
    const bool outlaws_budget=route=="outlaws-budget" || route=="outlaws-unshared-budget";
    const bool publication_budget=outlaws_budget || shared_budget || unshared_budget;
    if(publication_budget)route="d11-ref";
    const bool benchmark_dlssg_transition=route=="benchmark-dlssg-transition";
    const bool benchmark_dlssg_sequence=route=="benchmark-dlssg-sequence" || benchmark_dlssg_transition;
    const bool benchmark_sequence=route=="benchmark-sequence" || benchmark_dlssg_sequence;
    if(benchmark_sequence)route=benchmark_dlssg_sequence?"solo-dlssg3":"solo-djules3";
    const bool off_session_probe=route=="off-pacing-session";if(off_session_probe)route="session";
    framework_session_probe=route=="framework-session";if(framework_session_probe)route="session";
    const bool measured=route.starts_with("metrics-")||rife_latency||rife_msfs;if(route.starts_with("metrics-"))route=route.substr(8);
    if(route.starts_with("no-origin-"))route=route.substr(10);
    const bool deep=route.starts_with("deep-");if(deep)route=route.substr(5);
    const bool toggling=route.starts_with("toggle-");if(toggling)route=route.substr(7);
    const bool blocked=route.starts_with("blocked-");if(blocked)route=route.substr(8);
    const bool layout_alternation=route.starts_with("layout-");if(layout_alternation)route=route.substr(7);
    const bool sole_prefix=route.starts_with("solo-");if(sole_prefix)route=route.substr(5);
    const bool sole=sole_prefix || steady_memory;
    const bool canonical=route.starts_with("canonical-");if(canonical)route=route.substr(10);
    const bool pooled=route.starts_with("pooled-");if(pooled)route=route.substr(7);
    const bool unpooled=route.starts_with("unpooled-");if(unpooled)route=route.substr(9);
    const bool split_eyes=independent_paired_split || independent_alternate || pooled || unpooled || rife_msfs || layout_alternation;
    const bool separate_queue=route.starts_with("separate-");if(separate_queue)route=route.substr(9);
    int a{},b{};if(route.size()>3 && route[route.size()-3]=='-'){a=route[route.size()-2]-'0';b=route.back()-'0';route.resize(route.size()-3);}
    const bool switching=route=="switch";if(switching)route="new";
    const bool luke=route.starts_with("luke-");if(luke)route=route.substr(5);
    const bool native12=!route.starts_with("d11-");if(!native12)route=route.substr(4);
    check(!steady_memory || native12,"steady VRAM mode currently uses the native D12 route fixtures");
    const bool independent_nvidia=independent && route.starts_with("nvidia-");if(independent_nvidia)route=route.substr(7);
    const bool triple=route.ends_with('3');if(triple)route.pop_back();
    xrfg_fixture::ControlProfile profile(independent);
    if(reclaim_test) {
        auto controls=xrfg::autonomous::snapshot().desired;controls.afw_mode=0;controls.nr_enabled=false;
        check(xrfg::autonomous::request(controls),"ordinary recovery fixture has no independent reconstruction");
        xrfg::autonomous::apply_runtime_settings();
    }
    if(independent) {
        auto desired=xrfg::autonomous::snapshot().desired;
        xrfg::autonomous::select_generation(desired,route=="fsr"?1:route=="dlssg"?2:route=="hybrid"?3:0,route=="fidelity"?0:route=="ref"?1:2);
        if(route=="hybrid")desired.hybrid_backend=independent_nvidia?2:0; // stale NVIDIA ID must select the single Independent engine
        desired.independent_pacing=true;desired.ofr_enabled=independent_aer;desired.afw_mode=luke?3:0;desired.nr_enabled=true;desired.r19_camera=true;desired.motion_vectors=route=="fsr" || route=="dlssg" || route=="hybrid";
        desired.independent_acquire_fix=!independent_runtime_tail_legacy;
        if(independent_aer4)desired.frame_multiplier=4;
        check(xrfg::autonomous::request(desired),"independent production selection");xrfg::autonomous::apply_runtime_settings();
        check(xrfg::embedded::afw_snapshot().desired.mode==(luke?3:0) && xrfg::neural_rendering::status().enabled &&
            xrfg::autonomous::r19_camera_enabled.load() && xrfg::embedded::snapshot().desired.motion_vectors==desired.motion_vectors,
            "Independent restores selected reconstruction/camera/vectors alongside NR");
        const auto original=desired;
        for(bool generation:{false,true}) {
            desired=original;desired.enabled=generation;
            check(xrfg::autonomous::request(desired),"independent NR generation state");xrfg::autonomous::apply_runtime_settings();
            const auto generation_settings=xrfg::embedded::snapshot().desired;
            const auto afw_settings=xrfg::embedded::afw_snapshot().desired;
            for(bool nr:{false,true}) {
                desired.nr_enabled=nr;desired.nr_before_sr=nr;
                check(xrfg::autonomous::request(desired),"independent NR live toggle");xrfg::autonomous::apply_runtime_settings();
                const auto status=xrfg::neural_rendering::status();
                check(status.enabled==nr && status.before_sr==nr,"NR live selection reaches its host with FG on or off");
                check(xrfg::embedded::snapshot().desired==generation_settings &&
                    xrfg::embedded::afw_snapshot().desired==afw_settings,
                    "NR selection changed independent cadence/generation/reconstruction settings");
            }
        }
        check(xrfg::autonomous::request(original),"restore independent optical selection");xrfg::autonomous::apply_runtime_settings();
    }
    if(rife_factor){
        auto desired=xrfg::autonomous::snapshot().desired;
        xrfg::autonomous::select_generation(desired,4,2);
        desired.rife_model=3;desired.rife_multiplier=rife_factor;desired.scale=5;
        desired.enabled=true;desired.pacing_mode=rife_pacing;
        desired.lower_vram=rife_fast;desired.separate_synthesis=rife_fast;
        desired.increase_buffer=rife_buffer3;
        check(xrfg::autonomous::request(desired),"RIFE production UI request");
        xrfg::autonomous::apply_runtime_settings();
    }

    if(ofxr_factor){
        auto desired=xrfg::autonomous::snapshot().desired;
        const bool hybrid_route=route=="hybrid",dlssg_route=route=="dlssg";
        xrfg::autonomous::select_generation(desired,dlssg_route?2:hybrid_route?3:0,route=="fidelity"?0:route=="ref"?1:2);
        if(hybrid_route)desired.hybrid_backend=0;
        if(dlssg_route)desired.triple_frame_gen=true; // DLSS-G 4X holds 3X's switch
        desired.frame_multiplier=int(ofxr_factor);desired.enabled=true;desired.pacing_mode=ofxr_pacing;
        check(xrfg::autonomous::request(desired),"OFXR multiplier production UI request");
        xrfg::autonomous::apply_runtime_settings();
        check(xrfg::embedded::snapshot().desired.rife_multiplier==(route=="dlssg" && ofxr_factor<4?2:int(ofxr_factor)) &&
            xrfg::embedded::snapshot().desired.rife_model<0,"OFXR multiplier reaches the layer without RIFE");
    }
    if(large_buffer) {
        auto desired=xrfg::autonomous::snapshot().desired;desired.increase_buffer=true;
        check(xrfg::autonomous::request(desired),"saved larger buffer before session");xrfg::autonomous::apply_runtime_settings();
    }
    const auto startup_ini=current_layer_directory()/L"ofxr_bridge.ini";
    struct StartupRestore {
        std::filesystem::path p;bool existed{};std::string bytes;
        StartupRestore(std::filesystem::path path):p(path),existed(std::filesystem::exists(path)) {
            if(existed){std::ifstream f(p,std::ios::binary);bytes.assign(std::istreambuf_iterator<char>(f),{});}
        }
        ~StartupRestore(){if(existed){std::ofstream f(p,std::ios::binary);f<<bytes;}else{std::error_code e;std::filesystem::remove(p,e);}WritePrivateProfileStringW(nullptr,nullptr,nullptr,p.c_str());}
    } startup_restore(startup_ini);
    if(force_vdxr_steamvr)check(WritePrivateProfileStringW(L"tray",L"pacing",L"force_vdxr",xrfg::autonomous::global_configuration_path().c_str()),"force VDXR on real SteamVR identity");
    if(pimax_session)check(WritePrivateProfileStringW(L"tray",L"pacing",force_framework_pimax?L"force_steamvr":L"auto",xrfg::autonomous::global_configuration_path().c_str()),"Pimax auto/explicit framework transport preference");
    if(vdxr_probe)check(WritePrivateProfileStringW(L"tray",L"pacing",vdxr_legacy_probe?L"auto":L"force_steamvr",xrfg::autonomous::global_configuration_path().c_str()),"isolated VDXR transport preference");
    if(independent_input_timing || rife_report || ((benchmark_sequence || rife_factor) && GetEnvironmentVariableW(L"XRFG_BENCH_TRACE",nullptr,0))) {
        const auto folder=xrfg::autonomous::configuration_path().parent_path()/L"trace";
        std::filesystem::create_directories(folder);
        WritePrivateProfileStringW(L"diagnostics",L"logging_enabled",L"1",(folder/L"ofxr_bridge.ini").c_str());
        if(independent_input_timing)WritePrivateProfileStringW(L"diagnostics",L"gpu_instrumentation_enabled",independent_input_timing_off?L"0":L"1",(folder/L"ofxr_bridge.ini").c_str());
        xrfg::bridge_flight_logger().initialize(folder);
        std::fprintf(stderr,"TRACE=%s\n",xrfg::bridge_flight_logger().log_path().string().c_str());
    }
    if(off_session_probe) {
        auto desired=xrfg::autonomous::snapshot().desired;desired.enabled=false;desired.pacing_mode=2;desired.afw_mode=3;
        check(xrfg::autonomous::request(desired),"select AFW-only with combined pacing stored before session");
        xrfg::autonomous::apply_runtime_settings();
    }
    if(framework_session_probe) {
        auto desired=xrfg::autonomous::snapshot().desired;desired.pacing_mode=0;
        check(xrfg::autonomous::request(desired),"select framework transport before session");
        xrfg::autonomous::apply_runtime_settings();
    }
    if((sole && !rife_factor) || (split_eyes && route=="hybrid")) {
        auto desired=xrfg::autonomous::snapshot().desired;
        xrfg::autonomous::select_generation(desired,route=="fsr"?1:route=="dlssg"?2:route=="hybrid"?3:0,
            route=="ref"?1:route=="new"?2:route=="djules"?3:0);
        if(layout_alternation && route=="hybrid")desired.hybrid_backend=2;
        desired.enabled=!steady_memory;desired.pacing_mode=adaptive_framework?0:1;
        desired.lower_vram=b;desired.separate_synthesis=a;
        desired.motion_vectors=route=="fsr" || route=="hybrid" || route=="dlssg";
        desired.triple_frame_gen=triple;desired.prefer_fps=triple || deep;
        if(aer_path){desired.advanced_mode=true;desired.ofr_enabled=true;desired.ofr_swap=aer_swap;}
        check(xrfg::autonomous::request(desired),"publish the global experiment with its actual route");
        xrfg::autonomous::apply_runtime_settings();
    }
    if(luke) {
        auto desired=xrfg::autonomous::snapshot().desired;desired.afw_mode=0;
        check(xrfg::autonomous::request(desired),"isolate Luke color-only OFXR");
        xrfg::autonomous::apply_runtime_settings();
    }
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Fixture f(native12);f.independent_mode=independent;f.transport->stop();if(route!="memory" && route!="scope" && !framework_session_probe)f.transport.reset();f.throttled=false;f.expected_views=2;f.unblock();
    std::atomic_uint panel_identity_checks{};
    if(panel_publication_test) {
        f.native_image_count=3;
        f.inspect_layers=[&](const XrFrameEndInfo* info) {
            std::array<const XrCompositionLayerQuad*,2> quads{};unsigned count{};
            for(unsigned i=0;i<info->layerCount;++i)if(info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_QUAD) {
                if(count<2)quads[count]=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);++count;
            }
            if(count!=2)return;
            check(quads[0]->subImage.swapchain!=quads[1]->subImage.swapchain,"independent panels aliased a native publication sink");
            const auto left=f.read_pixel(f.chains.at(quads[0]->subImage.swapchain));
            const auto right=f.read_pixel(f.chains.at(quads[1]->subImage.swapchain));
            check(left>=31 && left<=38 && right>=71 && right<=78,"source-pooled publication exchanged or overwrote panel pixels");
            ++panel_identity_checks;
        };
    }
    if(rife_latency)f.frame_wait_us=[](unsigned){return 11111;};
    if(rife_factor)f.prediction_horizon_ns=period*3; // cold-model/GPU-load fixture, unchanged physical refresh

    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=!independent_vdxr && !independent_other && !vdxr_probe && !unknown_session && !pimax_session;dispatch->virtual_desktop_runtime=independent_vdxr || vdxr_probe;
    if(pimax_session){dispatch->runtime_name="Pimax OpenXR";dispatch->inline_unless_pipelined=true;}
    if(measured)dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult{*out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;};
    // Deliberately unthrottled fault model for OFF: detects a client-added
    // cadence floor independently of any legitimate runtime throttling.
    dispatch->wait_frame=dispatch->steamvr_runtime && !off_session_probe?paced_steamvr_fixture_wait:Fixture::wait;
    dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    if(rife_overlap)dispatch->end_frame=rife_overlap_native_end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;dispatch->enumerate_swapchain_images=Fixture::enumerate;
    dispatch->acquire_swapchain_image=Fixture::acquire;dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;dispatch->convert_time_to_qpc=Fixture::to_qpc;
    if(route=="session") {
        dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult{*out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;};
        dispatch->create_session=+[](XrInstance,const XrSessionCreateInfo* info,XrSession* out)->XrResult{
            auto& f=*current;auto* binding=static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
            check(binding && binding->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR,"actual raw D12 binding");
            if(!framework_session_probe && !vdxr_legacy_probe)check(binding->queue!=f.queue.Get(),"real XR must not receive producer queue");
            f.delivery_queue=binding->queue;*out=reinterpret_cast<XrSession>(2);return XR_SUCCESS;
        };
        dispatch->destroy_session=+[](XrSession)->XrResult{return XR_SUCCESS;};
        {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;}
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};binding.device=f.device12.Get();binding.queue=f.queue.Get();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=&binding;info.systemId=1;
        XrSession handle{};xr_ok(layer_create_session_impl(dispatch->instance,&info,&handle),"shipping session creates private delivery binding");
        auto created=find_session(handle);
        if(pimax_session)check(created && !created->dispatch->steamvr_runtime && !created->dispatch->virtual_desktop_runtime &&
            created->dispatch->runtime_name=="Pimax OpenXR","transport selection must retain native Pimax identity");
        if(pimax_session && force_framework_pimax)
            check(created->runtime_acceptance && !created->runtime_acceptance->rate(xrfg::interface_clock_ns()),
                "Pimax Framework must observe native accepted output without inventing an initial rate");
        if(!vdxr_legacy_probe)check(created && created->private_runtime && created->private_runtime->buffer_capacity()==((created->dispatch->steamvr_runtime && created->private_runtime->director_enabled()) || large_buffer?3:2),"session uses Director3 or Framework requested2/3 private deliveries");
        if(vdxr_legacy_probe) {
            check(created && created->vdxr_legacy && !created->private_runtime && !created->private_delivery_queue,
                "Legacy runtime keeps original native binding without a private director");
            check(f.delivery_queue==f.queue,"Legacy keeps the exact application runtime queue");
            if(pimax_session) {
                // The actual session entry must connect its native output
                // counter. A detached overlay test would miss this wiring bug.
                check(bool(created->fps_overlay),"Pimax session has no output meter");
                const auto original_end=created->dispatch->end_frame;
                created->dispatch->end_frame=+[](XrSession,const XrFrameEndInfo*)->XrResult{return XR_ERROR_RUNTIME_FAILURE;};
                XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                const auto* content=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&content;
                check(XR_FAILED(created->fps_overlay->end_frame(&end,false)) && !created->fps_overlay->metrics().sample_valid,
                    "failed native End fabricated Pimax output FPS");
                created->dispatch->end_frame=+[](XrSession,const XrFrameEndInfo*)->XrResult{return XR_SUCCESS;};
                end.layerCount=0;
                xr_ok(created->fps_overlay->end_frame(&end,false),"empty Pimax native End");
                check(!created->fps_overlay->metrics().sample_valid,"empty native End fabricated Pimax output FPS");
                end.layerCount=1;
                xr_ok(created->fps_overlay->end_frame(&end,false),"real Pimax native End");
                const auto content_counts=created->fps_overlay->metrics();
                created->fps_overlay->set_session_focused(false);
                check(WritePrivateProfileStringW(L"overlay",L"position",L"off",startup_ini.c_str()),"isolate meter telemetry from GPU drawing");
                xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
                created->fps_overlay->application_frame(&end);
                const auto measured_output=xrfg::autonomous::snapshot().runtime.telemetry;
                xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,false);
                check(xrfg::output_rate_valid(measured_output) && measured_output.output_fps>0 &&
                    measured_output.output_source==xrfg::InterfaceSource::runtime_accepted_delivery &&
                    content_counts.generated_fps==0 && !content_counts.active,
                    "Pimax output FPS remains N/A after accepted real output or falsely claims generation");
                created->dispatch->end_frame=original_end;
            }
            if(pimax_session) {
                // The restored auto route must expose V545's actual shape rule,
                // not a parallel test-only decision helper.
                created->menu_enabled=true;created->nvidia_new_route=true;created->djules_route=true;
                created->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
                created->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
                for(auto graphics:{SessionGraphicsBinding::d3d12,SessionGraphicsBinding::d3d11}) {
                    created->graphics_binding=graphics;created->split_frame_loop=false;
                    created->application_wait_thread_id=created->application_end_thread_id=GetCurrentThreadId();
                    check(!djules_inline_runtime(*created) && three_slot_generation_session(created),"Pimax single-thread shape must acquire presenter at startup");
                    created->split_frame_loop=true;created->application_end_thread_id=GetCurrentThreadId()+1;
                    check(djules_inline_runtime(*created),"Pimax split frame loop must retain native inline policy");
                    created->presenter_restore_after_bypass=true;
                    check(!djules_inline_runtime(*created),"previously active Pimax presenter must recover after bypass");
                    created->presenter_restore_after_bypass=false;
                }
                created->graphics_binding=SessionGraphicsBinding::d3d12;
            }
            check(!xrfg::autonomous::snapshot().runtime.buffer_capacity,"Legacy never reports or creates a bridge buffer even when requested");
            if(force_vdxr_steamvr)check(created->dispatch->steamvr_runtime && !created->dispatch->virtual_desktop_runtime,"forcing transport preserves true SteamVR identity");
            xr_ok(layer_destroy_session_impl(handle),"Legacy VDXR session cleanup");
            {std::scoped_lock lock(g_state_mutex);g_instances.clear();}return 0;
        }
        if(vdxr_probe)check(created && !created->vdxr_legacy && created->dispatch->virtual_desktop_runtime &&
            !created->dispatch->steamvr_runtime && created->runtime_acceptance,"VDXR session identity and actual-End measurement remain distinct");
        if(off_session_probe) {
            check(created && created->private_runtime && !created->menu_enabled,"OFXR-off startup installed the stable private endpoint");
            auto transport=created->private_runtime;f.transport=transport;f.expected_views=1;
            check(!transport->director_enabled() && !single_director_pacing(*created),"OFXR-off startup ignores stored combined pacing");
            // Model the already-produced AFW pair at the delivery boundary.
            // Actual AFW image math is covered by the pinned-provider fixtures.
            created->luke_afw_enabled=true;
            ok(apply_embedded_control(created),"qualify AFW-only output ownership");
            created->luke_afw_last_evaluated_publication=77;
            xr_ok(transport->start(),"start the actual bypass workers");auto chain=f.make_chain();unsigned serial{};
            auto batch=[&](unsigned count) {
                for(unsigned i=0;i<count;++i) {++serial;f.write(chain,serial*8);f.begin_virtual();xr_ok(f.submit(chain,float(serial)),"owned AFW pair delivered");}
                f.wait_count(serial);xr_ok(transport->drain(),"retire each policy batch");
            };
            for(int mode:{0,1,2,1,0,2}) {
                auto desired=xrfg::autonomous::snapshot().desired;desired.pacing_mode=mode;
                check(xrfg::autonomous::request(desired),"store pacing preference while OFXR is OFF");xrfg::autonomous::apply_runtime_settings();
                ok(apply_embedded_control(created),"apply OFF pacing choice");
                check(created->private_runtime==transport && !transport->director_enabled() && !single_director_pacing(*created),"all OFF choices remain unpaced on the same binding");
                check(created->luke_afw_last_evaluated_publication==77,"OFF pacing change reset AFW temporal history");
                batch(3);
            }
            for(unsigned i=0;i<serial;++i)check(!f.director_policies[i] && f.positions[i]==float(i+1) && f.pixels[i]==(i+1)*8,"OFF delivery lost pixels, order or native bypass");
            auto shortest=period;for(unsigned i=1;i<serial;++i)shortest=std::min(shortest,f.handoffs[i]-f.handoffs[i-1]);
            check(shortest<period/2,"OFF path retains an artificial refresh floor");
            for(int mode:{1,2,0,2}) {
                auto desired=xrfg::autonomous::snapshot().desired;desired.enabled=true;desired.pacing_mode=mode;
                check(xrfg::autonomous::request(desired),"enable OFXR with stored policy");xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(created),"restore enabled policy");
                check(transport->director_enabled()==(mode!=0) && single_director_pacing(*created)==(mode==1),"ON did not restore selected Framework/Director/combined policy");
                batch(2);
                desired.enabled=false;check(xrfg::autonomous::request(desired),"disable OFXR retaining AFW");xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(created),"return to OFF bypass");
                check(!transport->director_enabled() && !single_director_pacing(*created) && created->luke_afw_last_evaluated_publication==77,"disable left pacing active or reset AFW");
                batch(3);
            }
            check(created->private_runtime==transport && f.chains.size()==1,"OFF/ON recreated session binding or allocated extra native swapchains");
            xr_ok(transport->destroy_swapchain(chain),"release policy fixture chain");xr_ok(layer_destroy_session_impl(handle),"OFF session cleanup");
            {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
            std::puts("OFXR OFF/AFW-only: every stored pacing bypasses, exact FIFO GPU pixels, no refresh floor; ON restores selection and AFW history survives PASS");return 0;
        }
        if(framework_session_probe) {
            check(created && created->private_runtime && created->private_delivery_queue,"Framework keeps the stable private binding in bypass");
            auto transport=created->private_runtime;
            check(!transport->director_enabled() && !single_director_pacing(*created),"Framework startup bypasses final director pacing");
            for(int mode:{1,2,0,2,1,0}) {
                auto next=xrfg::autonomous::snapshot().desired;next.pacing_mode=mode;next.increase_buffer=mode==2;
                check(xrfg::autonomous::request(next),"request a live pacing policy");
                xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(created),"apply live policy without recreating session");
                if(transport->director_enabled()!=(mode!=0) || single_director_pacing(*created)!=(mode==1)) {
                    const auto diagnostic=xrfg::autonomous::snapshot();
                    std::fprintf(stderr,"POLICY mode%d desired%d advanced%d embedded%d director%d single%d enabled%d revision%llu applied%llu profile=%ls\n",
                        mode,diagnostic.desired.pacing_mode,int(diagnostic.desired.advanced_mode),xrfg::embedded::snapshot().desired.pacing_mode,
                        int(transport->director_enabled()),int(single_director_pacing(*created)),int(diagnostic.desired.enabled),
                        diagnostic.revision,diagnostic.runtime.applied_revision,xrfg::autonomous::configuration_path().c_str());
                }
                check(created->private_runtime==transport && transport->director_enabled()==(mode!=0) &&
                    single_director_pacing(*created)==(mode==1),"every pacing choice applies to the original endpoint");
                check(transport->buffer_capacity()==((created->dispatch->steamvr_runtime && mode!=0) || next.increase_buffer?3:2),"capacity applies before session start, preserving Framework2/3 and Director3");
            }
            f.transport=transport;
            if(vdxr_probe) {
                f.expected_views=1;xr_ok(transport->start(),"VDXR start from native prediction clock only");
                auto chain=f.make_chain();
                for(unsigned n=1;n<=12;++n){f.write(chain,n*7);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"VDXR ordered content packet");}
                f.wait_count(12);xr_ok(transport->drain(),"VDXR FIFO drained");
                for(unsigned n=0;n<12;++n)check(f.pixels[n]==(n+1)*7 && f.positions[n]==float(n+1),"VDXR exact GPU pixels and pose order");
                xr_ok(transport->destroy_swapchain(chain),"VDXR publication cleanup");
            }
            xr_ok(layer_destroy_session_impl(handle),"framework session cleanup");
            {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
            std::puts("Framework startup and live three-mode switch retain one native binding PASS");return 0;
        }
        check(created && created->private_runtime,"shipping session installed transport");
        f.transport=created->private_runtime; // fixture teardown owns a valid stopped endpoint
        check(created->private_delivery_queue.Get()==f.delivery_queue.Get() &&
              created->private_delivery_queue.Get()!=runtime_queue(*created),"raw binding and private preparation queues are distinct");
        D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY priority{D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_QUEUE_PRIORITY_HIGH,FALSE};
        if(SUCCEEDED(f.device12->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&priority,sizeof(priority))) && priority.PriorityForTypeIsSupported) {
            auto desc=binding.queue->GetDesc();desc.Priority=D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            ComPtr<ID3D12CommandQueue> probe;
            if(SUCCEEDED(f.device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&probe))))
                check(f.delivery_queue->GetDesc().Priority==D3D12_COMMAND_QUEUE_PRIORITY_HIGH,"shipping final binding must use supported HIGH rather than normal priority behind synthesis");
        }
        check(binding.queue==f.queue.Get() && info.next==&binding,"application binding metadata remains owned by application");
        xr_ok(layer_destroy_session_impl(handle),"shipping private delivery session cleanup");
        {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
        std::puts("Shipping xrCreateSession receives dedicated consumer queue; private producer queue retained PASS");return 0;
    }
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);state->graphics_binding=native12?SessionGraphicsBinding::d3d12:SessionGraphicsBinding::d3d11;
    state->independent_pacing=independent;
    if(independent)state->history_capture_wait_period=period; // mirrors shipping application Wait before capture
    if(!native12)ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),f.device12.GetAddressOf(),f.queue.GetAddressOf()),"D11 generator interop device");
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->d3d11_device=f.device11;state->d3d11_context=f.context;
    if(independent) {D3D12_COMMAND_QUEUE_DESC queue{};queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(f.device12->CreateCommandQueue(&queue,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"independent compute queue");}
    state->vdxr_legacy=false;
    state->single_director_requested=sole;
    state->luke_afw_enabled=luke;
    if(hfw) {
        check(luke && !native12 && (sole || rife_luke),"HFW fixture requires Luke D11 with an independent worker");
        state->luke_fresh_only=true;state->luke_source_filter_armed=true;
        state->graphics_binding_capabilities|=hfw_early?4ULL:8ULL;
        initialize_luke_synthesis_queue(*state);
        check(state->luke_synthesis_queue!=nullptr,"HFW separate lane");
        auto afw=xrfg::embedded::afw_snapshot().desired;afw.mode=0;
        check(xrfg::embedded::request_afw(afw),"HFW raw control without native AFW guide fixture");
    }
    if(route=="scope") {
        state->single_director_requested=true;
        check(!single_director_pacing(*state),"a request alone cannot bypass a native runtime's pacing");
        state->private_runtime=f.transport;
        dispatch->steamvr_runtime=false;dispatch->virtual_desktop_runtime=true;state->vdxr_legacy=true;
        check(!single_director_pacing(*state),"VDXR remains unchanged with the experimental flag requested");
        state->vdxr_legacy=false;
        check(single_director_pacing(*state),"opt-in private VDXR enables sole-director pacing");
        state->vdxr_legacy=false;
        dispatch->steamvr_runtime=true;dispatch->virtual_desktop_runtime=false;
        check(single_director_pacing(*state),"only private SteamVR enables sole-director pacing");
        state->single_director_requested=false;
        check(!single_director_pacing(*state),"OFF restores the existing route policy");
        state->djules_route=true;state->nvidia_new_route=true;
        state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
        state->djules_triple_requested=true;state->lower_vram=state->separate_synthesis=1;
        check(extra_output_capacity(*state) && native_current_rings(*state)==2 && native_synthetic_rings(*state)==2,
            "SteamVR private Fast 3X keeps independent A/B/S1/S2 capacity");
        state->private_runtime.reset();dispatch->steamvr_runtime=false;dispatch->virtual_desktop_runtime=true;
        check(staged_native_outputs(*state) && extra_output_capacity(*state) &&
            native_current_rings(*state)==1 && native_synthetic_rings(*state)==1,
            "VDXR staged Fast 3X retains its earlier one-ring runtime contract");
        state->private_runtime=f.transport;dispatch->steamvr_runtime=true;dispatch->virtual_desktop_runtime=false;
        state->djules_triple_requested=false;
        check(!extra_output_capacity(*state) && native_current_rings(*state)==1 && native_synthetic_rings(*state)==1,
            "SteamVR Fast 2X still reduces both runtime rings");
        std::puts("Single director scope: OFF/private SteamVR/VDXR/nonprivate PASS");return 0;
    }
    if(route=="memory") {
        using namespace xrfg::autonomous;
        check(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",global_configuration_path().c_str()),"enable isolated memory benchmark");
        auto desired=snapshot().desired;desired.benchmark_csv=true;desired.diagnostics=false;desired.advanced_mode=true;desired.independent_pacing=true;
        check(request(desired),"isolated memory test settings");
        RuntimeStatus runtime{};runtime.sessions=1;runtime.pacing_mode=kIndependentPacingMode;runtime.applied_revision=snapshot().revision;report_runtime(runtime);
        check(start_benchmark(false),"start isolated memory trial");
        runtime.applied_revision=snapshot().revision;report_runtime(runtime);
        const auto clock=GetTickCount64();tick_benchmark(clock);
        check(snapshot().benchmark.phase==BenchmarkPhase::running,"memory trial applied");
        tick_benchmark(clock+benchmark_memory_request_ms(true));
        sample_benchmark_memory(*state);
        check(!take_benchmark_memory_request(),"game sampler consumes one request only");
        check(state->video_memory_adapter!=nullptr,"sampler finds the actual rendering adapter with F9 off");
        DXGI_QUERY_VIDEO_MEMORY_INFO expected{};
        ok(state->video_memory_adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&expected),"independent process VRAM query");
        const auto path=std::filesystem::u8path(snapshot().benchmark.csv_path);
        tick_benchmark(clock+benchmark_duration_ms(true));
        std::ifstream csv(path);std::string line;bool gpu{},ram{};
        while(std::getline(csv,line)) {
            for(const auto& key:{std::string("VRAM end MiB,"),std::string("RAM working set end MiB,")}) {
                const auto at=line.find(key);if(at==std::string::npos)continue;
                const double value=std::stod(line.substr(at+key.size()));
                if(key.starts_with("VRAM")) {gpu=true;check(std::abs(value-double(expected.CurrentUsage)/1048576.0)<0.01,"CSV VRAM equals current-process DXGI usage on the rendering adapter");}
                else {ram=true;check(value>0,"real game-process RAM is observed");}
            }
        }
        check(gpu&&ram,"CSV contains actual VRAM and RAM end samples");
        stop_benchmark();tick_benchmark();
        std::puts("Memory: one end-of-trial game-process DXGI/working-set snapshot, F9 off, CSV correspondence PASS");return 0;
    }
    D3D12_COMMAND_QUEUE_DESC queue_info{};queue_info.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(native12)ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"separate generator queue");
    if(native12)state->private_delivery_queue=f.delivery_queue;
    if(separate_queue) {
        ok(f.device12->CreateCommandQueue(&queue_info,IID_PPV_ARGS(&state->binding_queue)),"independent real binding queue");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->app_release_fence)),"input publication fence");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->reacquire_fence)),"legacy binding reacquire fence");
    }
    state->optical_flow_backend=route=="fidelity"?xrfg::D3D12OpticalFlowBackend::fidelity_fx:xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route=route!="ref" && route!="fidelity";state->djules_route=route=="djules";
    state->separate_synthesis=a;state->lower_vram=b;
    state->djules_triple_requested=triple;state->djules_deep_pipeline=triple || deep;
    if(rife_factor){
        state->nvidia_options.rife_model=3;state->nvidia_options.rife_multiplier=rife_factor;
        state->nvidia_options.rife_async=true;state->nvidia_options.input_scale=xrfg::D3D12NvidiaInputScale::sixth;
    }

    if(ofxr_factor && !independent)state->nvidia_options.rife_multiplier=int(ofxr_factor);
    if(benchmark_sequence)state->djules_pipeline_switchable=true; // shipping xrCreateSession enables live multiplier polling
    const bool vectors=route=="fsr" || route=="hybrid" || route=="dlssg";
    if(vectors)state->dlss_motion_vectors=true;
    if(route=="fsr")state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
    if(route=="dlssg")state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::native_dlss;
    if(canonical || route=="dlssg") {
        check(route=="dlssg","canonical route fixture is DLSS-G");
        state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        state->nvidia_new_route=false;state->djules_route=false;
    }
    if(route=="hybrid"){state->optical_flow_backend=layout_alternation?xrfg::D3D12OpticalFlowBackend::nvidia:xrfg::D3D12OpticalFlowBackend::fidelity_fx;state->hybrid_requested=true;state->nvidia_options.hybrid=true;state->nvidia_new_route=layout_alternation;}
    if(publication_budget) {
        state->luke_afw_enabled=true;state->luke_outlaws_preframe=outlaws_budget;
        state->luke_shared_swapchains=shared_budget || (outlaws_budget && !unshared_budget);
        state->graphics_binding_capabilities|=8ULL;
    }
    if((sole || independent) && !rife_factor)ok(apply_embedded_control(state),"initial settings match the real public route selection");
    else state->control_revision=xrfg::embedded::snapshot().revision;
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;g_instances[dispatch->instance]=dispatch;}
    ok(install_private_runtime(state),"install real SteamVR private dispatch");
    if(rife_factor){state->control_revision=0;ok(apply_embedded_control(state),"actual RIFE production control after private session installation");}
    if(ofxr_factor){if(route=="dlssg")refresh_native_generation_capacity(*state); // as xrCreateSession
        state->control_revision=0;ok(apply_embedded_control(state),"actual OFXR multiplier control after private session installation");
        check(ofxr_factor<3 || route=="dlssg" || (optical_multiframe_requested(*state) && rife_frames(*state)==ofxr_factor),"OFXR multiplier admitted on the private route");
        check(route!="dlssg" || ofxr_factor<4 || state->native_dlss_generated_frames<3 ||
            (native_multiframe_requested(*state) && rife_frames(*state)==4),"DLSS-G 4X admitted on the private route");}
    if(benchmark_dlssg_sequence) {
        // Manual SessionState fixtures must make the same real SDK capacity
        // query as xrCreateSession. Never grant a synthetic 3X capability.
        refresh_native_generation_capacity(*state);
        std::fprintf(stderr,"DLSS-G benchmark actual max-generated-frames=%u admitted-output-multiplier=%u\n",
            state->native_dlss_generated_frames,state->native_dlss_generated_frames>=2?3U:2U);std::fflush(stderr);
        check(state->native_dlss_generated_frames>=1,"DLSS-G benchmark requires actual SDK frame-generation support");
    }
    if(hfw) {
        if(state->private_runtime)
            check(!state->luke_fresh_only && !presenter_forbidden(*state),
                "private endpoint must replace the old HFW inline admission filter");
        else if(!(rife_luke && rife_switch && !rife_pacing))
            check(fresh_luke_director_worker(*state) && !presenter_forbidden(*state),"HFW source filter incorrectly forces inline synthesis");
        state->single_threaded_d3d11=true;check(presenter_forbidden(*state),"single-threaded guard lost");state->single_threaded_d3d11=false;
        state->split_frame_loop=true;check(presenter_forbidden(*state),"split-loop guard lost");state->split_frame_loop=false;
        state->afw_only=true;check(presenter_forbidden(*state),"AFW-only enters OFXR worker");state->afw_only=false;
        state->dispatch->steamvr_runtime=false;if(!rife_luke)check(presenter_forbidden(*state),"VDXR pacing changed");state->dispatch->steamvr_runtime=true;
    }
    f.transport=state->private_runtime;state->uevr_application=pooled || panel_publication_test || budget_fallback_test;
    if(rife_overlap)check(single_director_pacing(*state)==bool(rife_pacing) &&
        f.transport->director_enabled()==bool(rife_pacing),
        "overlap probe did not select its actual Framework/Director delivery policy");
    if(pooled)f.native_chain_limit=4; // two app chains plus two physical publication sinks
    if(publication_budget) {
        f.native_chain_limit=16;
        const unsigned source_count=unshared_budget?2:3;
        std::array<XrSwapchain,3> app{};
        for(unsigned n=0;n<source_count;++n) {
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=64;ci.height=n==2?40:32;
            ci.arraySize=ci.faceCount=ci.sampleCount=ci.mipCount=1;ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
            ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
            xr_ok(layer_create_swapchain_impl(state->handle,&ci,&app[n]),"Outlaws original application color chain");
            unsigned count{};xr_ok(layer_enumerate_swapchain_images_impl(app[n],0,&count,nullptr),"Outlaws app count");
            std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            xr_ok(layer_enumerate_swapchain_images_impl(app[n],count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"Outlaws initial complete enum");
            auto chain=find_swapchain(app[n]);
            if(!outlaws_budget)check(ensure_frame_generation(chain),"ordinary deferred Luke output initialization");
            std::fprintf(stderr,"OUTLAWS source%u: native=%zu generation=%d budget=%d\n",n,f.chains.size(),bool(chain->frame_generation),bool(state->generation_budget_exhausted));
            check(chain->frame_generation && chain->d3d12_history && !state->generation_budget_exhausted,
                "Outlaws eager startup exhausted16 native chains and discarded already-ready eyes");
        }
        const auto expected_native=source_count*(unshared_budget?6U:2U);
        check(f.chains.size()==expected_native,"startup flag preserves old native allocation or opts into one sink per source");
        state->next_swapchain_inventory_ms=0;
        const auto budget=session_swapchain_inventory(state,nullptr);
        check(budget.total==f.chains.size() && budget.application==source_count &&
            budget.current+budget.synthetic==expected_native-source_count && !budget.unassigned_native,
            "inventory deduplicates private output tokens into actual native owners");
        seal_outlaws_initialization(*state);
        for(auto handle:app)if(handle)check(find_swapchain(handle)->frame_generation!=nullptr,"startup seal lost warmed eye outputs");
        if(outlaws_budget) {
            // The owner starts with both switches Off, then enables AFW/OFXR.
            // These are the real control calls AFTER Outlaws sealed cold setup.
            std::array<XrCompositionLayerProjectionView,2> views{};
            for(unsigned eye=0;eye<2;++eye) {
                views[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[eye].pose.orientation.w=1;
                views[eye].fov={-.7f,.7f,.7f,-.7f};views[eye].subImage.swapchain=app[eye];
                views[eye].subImage.imageRect.extent={64,32};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);
            projection.viewCount=2;projection.views=views.data();
            const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frame.layerCount=1;frame.layers=&header;frame.displayTime=epoch+ns();
            std::array<std::shared_ptr<FrameGenerationSwapchainState>,3> warmed;
            for(unsigned n=0;n<source_count;++n)warmed[n]=find_swapchain(app[n])->frame_generation;
            for(const auto [on,afw]:{std::pair{false,0},std::pair{false,3},std::pair{true,3},
                                    std::pair{false,3},std::pair{false,0},std::pair{true,0},std::pair{true,3}}) {
                auto desired=xrfg::autonomous::snapshot().desired;
                desired.enabled=on;desired.afw_mode=afw;desired.backend=1;
                check(xrfg::autonomous::request(desired),"Outlaws owner's Off/AFW/OFXR control sequence");
                xrfg::autonomous::apply_runtime_settings();state->control_revision=0;
                ok(apply_embedded_control(state,false,&frame),"actual sealed Outlaws control application");
                for(unsigned n=0;n<source_count;++n)check(find_swapchain(app[n])->frame_generation==warmed[n],
                    "sealed Outlaws warm outputs were retired by an Off/AFW/OFXR switch and cannot be rearmed");
                ProjectionSnapshot snapshot{};
                check(capture_projection_snapshot(&frame,&snapshot),"Outlaws submitted stereo projection");
                const auto mappings=build_projection_resource_mappings(snapshot);
                check(mappings.ready() && !projection_frame_generation_pending(state,mappings.mappings),
                    "valid warmed Outlaws eyes remain trapped in pending generation");
                check(f.chains.size()==expected_native,"control sequence allocated or destroyed native publication sinks");
            }
        }
        xr_ok(f.transport->start(),"start Outlaws native publication consumer");
        std::vector<unsigned> expected_left,expected_right;
        for(unsigned slot:{0U,1U,2U,3U,4U,0U}) {
            const unsigned identity=static_cast<unsigned>(expected_left.size()+1);
            std::array<XrCompositionLayerProjectionView,2> views{};
            for(unsigned eye=0;eye<2;++eye) {
                const auto generation=find_swapchain(app[eye])->frame_generation;
                const auto output=slot<3?generation->current[slot].handle:generation->synthetic[slot-3].handle;
                const unsigned color=eye?140+identity*8:30+identity*8;
                f.write(output,color);
                (eye?expected_right:expected_left).push_back(color);
                auto& view=views[eye];view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                view.pose.orientation.w=1;view.pose.position.x=float(identity);
                view.subImage.swapchain=output;view.subImage.imageRect.extent={64,32};
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=2;projection.views=views.data();
            const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.layerCount=1;end.layers=&header;
            f.begin_virtual();xr_ok(f.transport->end_frame(&end),"each independent warmed output reaches native XR");
        }
        f.wait_count(6);xr_ok(f.transport->drain(),"all Outlaws warmed outputs retire");
        check(f.pixels==expected_left && f.right_pixels==expected_right && f.chains.size()==expected_native,
            "Outlaws shared sinks overwrite another eye/frame or allocate new native handles");
        release_session_generation_budget(state);
        for(auto handle:app)if(handle)xr_ok(layer_destroy_swapchain_impl(handle),"Outlaws original chain cleanup");
        check(f.chains.empty(),"Outlaws pooled publications leaked after all owners retired");
        {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_instances.clear();}
        std::puts("Outlaws actual D11/private endpoint startup under16: three app sources, six native handles, preserved independent output storage and full cleanup PASS");return 0;
    }
    xr_ok(f.transport->start(),"start common consumer");
    if(route=="entry-gate") {
        check(!native12,"entry gate probe requires D11");f.expected_views=1;
        auto chain=f.make_chain(),spare=f.make_chain();f.write(chain,77);
        unsigned spare_index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        xr_ok(f.transport->acquire_image(spare,&acquire,&spare_index),"spare virtual acquire");
        xr_ok(f.transport->wait_image(spare,&wait),"spare virtual wait");
        f.block_end=true;f.end_entered=false;
        const auto buffer=static_cast<unsigned>(f.transport->buffer_capacity());
        for(unsigned i=1;i<=buffer;++i){f.begin_virtual();xr_ok(with_runtime_entry(state,[&]{return f.submit(chain,float(i));}),"fill bounded private FIFO");}
        f.wait_entered();f.begin_virtual();
        auto blocked=std::async(std::launch::async,[&]{return with_runtime_entry(state,[&]{return f.submit(chain,float(buffer+1));});});
        check(blocked.wait_for(30ms)==std::future_status::timeout,"full FIFO must retain backpressure");
        auto release=std::async(std::launch::async,[&]{return with_runtime_entry(state,[&]{
            XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};return f.transport->release_image(spare,&info);});});
        const bool independent=release.wait_for(100ms)==std::future_status::ready;
        f.unblock();xr_ok(blocked.get(),"queued packet accepted after retirement");xr_ok(release.get(),"spare release");
        check(independent,"private EndFrame FIFO credit blocked unrelated D11 producer release");
        f.wait_count(buffer+1);xr_ok(f.transport->drain(),"gate fixture drain");
        std::vector<float> expected;for(unsigned i=1;i<=buffer+1;++i)expected.push_back(float(i));
        check(f.positions==expected && f.pixels==std::vector<unsigned>(buffer+1,77),"gate fix changed FIFO ownership/order/pixels");
        // Native/non-private entries still serialize. The private exception
        // must not remove the protection required by inline D11 runtimes.
        auto native=std::make_shared<SessionState>(dispatch);native->d3d11_device=f.device11;
        std::promise<void> entered,leave;auto leave_future=leave.get_future();
        auto holder=std::async(std::launch::async,[&]{with_runtime_entry(native,[&]{entered.set_value();leave_future.wait();});});
        entered.get_future().wait();auto contender=std::async(std::launch::async,[&]{with_runtime_entry(native,[]{});});
        const bool serialized=contender.wait_for(30ms)==std::future_status::timeout;
        leave.set_value();holder.get();contender.get();check(serialized,"native D11 gate protection lost");
        xr_ok(f.transport->destroy_swapchain(chain),"gate source cleanup");xr_ok(f.transport->destroy_swapchain(spare),"gate spare cleanup");
        {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_instances.clear();}
        std::puts("Private D11 full FIFO does not block producer release; native gate retained; exact ordered GPU pixels PASS");return 0;
    }
    if(measured) {
        state->fps_overlay=std::make_unique<xrfg::OpenXrFpsOverlay>(dispatch->instance,state->handle,0,
            state->dispatch->get_instance_proc_addr,content_end_frame,f.device12.Get(),f.queue.Get(),f.device11.Get(),
            xrfg::autonomous::configuration_path(),false);
        bind_private_delivery_metrics(state);
        xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
    }
    if(separate_queue) {
        ComPtr<ID3D12Fence> gate,progress;
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"binding dependency gate");
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress)),"app progress fence");
        ok(state->binding_queue->Wait(gate.Get(),1),"hold real binding work");
        carry_reacquire_to_application(state.get());
        ok(f.queue->Signal(progress.Get(),1),"app must proceed independently of real binding work");
        const auto independent=wait_composition_fence(progress.Get(),1,30);
        ok(gate->Signal(1),"release real binding independently");
        ok(wait_composition_fence(progress.Get(),1),"reacquire proof drain");
        check(SUCCEEDED(independent),"private reacquire must not join the real runtime queue");
    }
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=image_width;info.height=image_height;info.arraySize=split_eyes?1:2;info.faceCount=info.mipCount=info.sampleCount=1;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain chain{};xr_ok(layer_create_swapchain_impl(state->handle,&info,&chain),"shipping application swapchain");
    std::array<XrSwapchainImageD3D12KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};unsigned count{};
    if(native12)xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"shipping enumerate/capture setup");
    else{std::array<XrSwapchainImageD3D11KHR,3> d11;for(auto& image:d11)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};xr_ok(layer_enumerate_swapchain_images_impl(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(d11.data())),"shipping D11 enumerate/capture setup");}
    XrSwapchain right=chain;std::array<XrSwapchainImageD3D12KHR,3> right_images{};
    if(split_eyes) {
        xr_ok(layer_create_swapchain_impl(state->handle,&info,&right),"separate right-eye application chain");
        for(auto& image:right_images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        if(native12)xr_ok(layer_enumerate_swapchain_images_impl(right,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(right_images.data())),"right-eye capture setup");
        else {std::array<XrSwapchainImageD3D11KHR,3> ri;for(auto& i:ri)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(right,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(ri.data())),"right-eye D11 capture setup");}
    }
    XrSwapchain packed_layout{};
    if(layout_alternation) {
        auto packed_info=info;packed_info.width=image_width*2;packed_info.arraySize=1;
        xr_ok(layer_create_swapchain_impl(state->handle,&packed_info,&packed_layout),"packed projection source");
        if(native12){std::array<XrSwapchainImageD3D12KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(packed_layout,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"packed D12 capture setup");}
        else {std::array<XrSwapchainImageD3D11KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(packed_layout,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"packed D11 capture setup");}
    }
    XrSwapchain panel_chain{},second_panel_chain{},budget_meter{};
    unsigned independent_panel_checks{},independent_panel_fractional{};
    std::vector<XrSwapchain> extra_budget_meters;
    if(panel_test) {
        auto ci=info;ci.arraySize=1;
        xr_ok(layer_create_swapchain_impl(state->handle,&ci,&panel_chain),"independent panel atlas");
        std::array<XrSwapchainImageD3D12KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        unsigned pc{};xr_ok(layer_enumerate_swapchain_images_impl(panel_chain,3,&pc,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"panel atlas capture setup");
        if(panel_budget_test) {
            xr_ok(layer_create_swapchain_impl(state->handle,&ci,&second_panel_chain),"independent second UEVR panel");
            for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(second_panel_chain,3,&pc,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"second panel capture setup");
            f.native_chain_limit=argc>2?std::stoul(argv[2]):budget_fallback_test?6:panel_lifecycle_test?32:10;
            // Exercise exhausted world-generation recovery after optional panel
            // generation has already stood down; there is no optional bank
            // left to return when the late second menu needs its copy sink.
            if(budget_fallback_test)state->panel_budget_refused=true;
        }
    }
    ComPtr<IDXGIAdapter3> memory_adapter;
    std::unique_ptr<steamvr_vram_test::Ledger> memory_ledger;
    std::vector<XrSwapchain> memory_applications{chain};if(split_eyes)memory_applications.push_back(right);
    UINT64 memory_app_baseline{},memory_game_baseline{},memory_peak{},memory_native_baseline{},memory_native_peak{},memory_private_peak{};
    std::array<UINT64,7> memory_phase_peaks{};
    if(steady_memory) {
        ComPtr<IDXGIFactory4> memory_factory;ok(CreateDXGIFactory1(IID_PPV_ARGS(&memory_factory)),"steady VRAM adapter factory");
        ok(memory_factory->EnumAdapterByLuid(f.device12->GetAdapterLuid(),IID_PPV_ARGS(&memory_adapter)),"steady VRAM actual rendering adapter");
        memory_ledger=std::make_unique<steamvr_vram_test::Ledger>(f.device12.Get());
    }
    const auto sample_memory=[&](const char* phase,unsigned frame) {
        if(!steady_memory)return;
        steamvr_vram_test::observe_resources(f,state,memory_applications,*memory_ledger);
        const auto process=steamvr_vram_test::residency(memory_adapter.Get());
        const auto native_bytes=memory_ledger->bytes(steamvr_vram_test::Kind::native);
        const auto application_bytes=memory_ledger->bytes(steamvr_vram_test::Kind::application);
        const auto generated_bytes=memory_ledger->bytes(steamvr_vram_test::Kind::generated);
        const auto interop_bytes=memory_ledger->bytes(steamvr_vram_test::Kind::interop);
        const auto control=xrfg::autonomous::snapshot().desired;
        const auto phase_index=benchmark_sequence && frame?(frame-1)/memory_phase_frames:0;
        if(phase_index<memory_phase_peaks.size())memory_phase_peaks[phase_index]=std::max(memory_phase_peaks[phase_index],process);
        memory_peak=std::max(memory_peak,process);memory_native_peak=std::max(memory_native_peak,native_bytes);
        memory_private_peak=std::max(memory_private_peak,application_bytes+generated_bytes+interop_bytes);
        std::printf("VRAM_STEADY route=%s eye=%ux%u phase=%s phase_index=%u frame=%u backend=%d frame_generation=%d hybrid=%u pacing=%d separate=%d lower_vram=%d triple=%u buffer=%u physical_outputs=%u generation_state=%u process_bytes=%llu native_bytes=%llu application_private_bytes=%llu generated_private_bytes=%llu interop_bytes=%llu\n",
            route.c_str(),image_width,image_height,phase,unsigned(phase_index),frame,control.backend,control.frame_generation,
            unsigned(control.frame_generation==3),control.pacing_mode,control.separate_synthesis,control.lower_vram,
            unsigned(control.triple_frame_gen),control.increase_buffer?3U:2U,f.real_ends.load(),unsigned(state->generation_status.load()),
            process,native_bytes,application_bytes,generated_bytes,interop_bytes);std::fflush(stdout);
        const auto features=xrfg::native_dlssg_lifetime_snapshot();
        std::printf("VRAM_NATIVE_FEATURES phase=%s phase_index=%u frame=%u create_attempts=%llu created=%llu failed_create_handles=%llu release_attempts=%llu released=%llu release_failures=%llu live=%llu\n",
            phase,unsigned(phase_index),frame,features.create_attempts,features.created,features.failed_create_handles,
            features.release_attempts,features.released,features.release_failures,features.live);std::fflush(stdout);
    };
    if(steady_memory) {
        sample_memory("application-enumerated",0);memory_app_baseline=memory_peak;
        memory_native_baseline=memory_ledger->bytes(steamvr_vram_test::Kind::native);
    }
    ComPtr<ID3D12Resource> depth,motion;
    // Native D11's fixture exposes D11 output identities, not D12 game guide
    // publications. Exercise Hybrid's missing-guide fallback there; native
    // D12 publishes real per-eye guides and switches to fallback when packed.
    if(vectors && !(layout_alternation && !native12)) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=image_width;desc.Height=image_height;desc.DepthOrArraySize=2;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;desc.Format=DXGI_FORMAT_R32_FLOAT;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&depth)),"owned depth guide");desc.Format=DXGI_FORMAT_R16G16_FLOAT;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&motion)),"owned motion guide");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;ComPtr<ID3D12DescriptorHeap> views;ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)),"guide RTVs");
        auto d=views->GetCPUDescriptorHandleForHeapStart(),m=d;m.ptr+=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);f.device12->CreateRenderTargetView(depth.Get(),nullptr,d);f.device12->CreateRenderTargetView(motion.Get(),nullptr,m);
        f.commands([&](ID3D12GraphicsCommandList* list){const float z[]={.5F,.5F,.5F,1},v[]={0,0,0,0};list->ClearRenderTargetView(d,z,0,nullptr);list->ClearRenderTargetView(m,v,0,nullptr);D3D12_RESOURCE_BARRIER b[2]{};for(auto& x:b){x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;x.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;x.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;}b[0].Transition.pResource=depth.Get();b[1].Transition.pResource=motion.Get();list->ResourceBarrier(2,b);});
    }
    if(split_eyes && vectors && native12)xrfg::configure_dlss_motion_vector_tracking(true);
    unsigned stream_base_pixels{};bool stream_probe_armed{},stream_probe_done{};
    ComPtr<ID3D12Fence> stream_input_gate;std::unique_ptr<std::jthread> stream_rescue;
    if(independent_stream_hold)ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&stream_input_gate)),"stream layer gate");
    f.release_callback=[&](XrSwapchain handle,unsigned index){
        if(stream_probe_armed && handle==right) {
            ok(f.queue->Wait(stream_input_gate.Get(),1),"block newer-eye history capture after its game rendering");stream_probe_armed=false;
        }
        auto c=find_swapchain(handle);
        if(split_eyes && vectors && native12 && handle!=packed_layout) {
            ComPtr<ID3D12GraphicsCommandList> recorded;
            f.commands([&](ID3D12GraphicsCommandList* list) {
                recorded=list;xrfg::DlssMotionVectorPublication p{};
                p.stream=handle==chain?100:200;p.output=c->enumerated_d3d12_images[index].Get();
                p.motion_vectors=motion.Get();p.depth=depth.Get();p.producer_queue=f.queue.Get();p.producer_command_list=list;
                p.output_width=p.motion_width=p.depth_width=image_width;p.output_height=p.motion_height=p.depth_height=image_height;
                check(bool(xrfg::publish_dlss_motion_vectors(p)),"real per-eye release-bound guide publication");
            });
            ID3D12CommandList* lists[]={recorded.Get()};xrfg::report_dlss_motion_vector_submission(f.queue.Get(),1,lists);
        }
        {std::scoped_lock lock(c->mutex);c->acquired_indices.push_back(index);++c->waited_count;}
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if(hfw) {static std::uint64_t serial{};
            return release_swapchain_with_identity(c,&release,xrfg::LukeRenderIdentity{0x649,++serial});}
        return layer_release_swapchain_image_impl(handle,&release);
    };
    if(steady_memory) {
        sample_memory("game-guides-ready",0);memory_game_baseline=steamvr_vram_test::residency(memory_adapter.Get());
        auto enabled=xrfg::autonomous::snapshot().desired;enabled.enabled=true;
        check(xrfg::autonomous::request(enabled),"steady memory real OFF-to-ON activation");xrfg::autonomous::apply_runtime_settings();
        ok(apply_embedded_control(state),"steady memory apply active generator after game-only baseline");
    }
    unsigned phase_outputs{};
    const unsigned trial_frames=memory_phase_frames;
    const bool completed_frame_memory=steady_memory && (!benchmark_sequence ||
        (benchmark_dlssg_sequence && !benchmark_dlssg_transition && state->native_dlss_generated_frames<2));
    // Six phases already cover Framework/Director/Combined x Normal/Fast.
    // The seventh exists only to test a REAL 3X->2X capacity transition. On an
    // SDK limited to one generated frame it would instead test an unrelated
    // prefer_fps scheduling change while repeating the same 2X Fast policy.
    const unsigned benchmark_policy_phases=benchmark_dlssg_sequence && !benchmark_dlssg_transition && state->native_dlss_generated_frames<2?6:7;
    const unsigned input_count=reclaim_test?32:late_shipping?16:(independent && blocked) || layout_alternation || independent_offon || independent_tickets || independent_input_timing || (independent && panel_budget_test)?24:independent_aer4 && !native12?16:rife_latency?64:phase_switch?5*rife_phase_frames:benchmark_sequence?benchmark_policy_phases*trial_frames:steady_memory?16:rife_msfs?64:panel_lifecycle_test?16:8;
    if(independent && panel_test)f.inspect_layers=[&](const XrFrameEndInfo* frame) {
        check(frame->layerCount>=3,"Independent output lost one of its original panels");
        const auto progress=reinterpret_cast<const XrCompositionLayerQuad*>(frame->layers[1])->pose.position.x*10.f;
        const auto source=unsigned(std::ceil(progress-.001f));
        check(source>=1 && source<=input_count,"panel correction escaped its admitted source pair");
        if(independent_aer)check(std::abs(progress*2.f-std::round(progress*2.f))<.002f,"AER panel correction has an invalid temporal fraction");
        if(std::abs(progress-std::round(progress))>.01f)++independent_panel_fractional;
        for(unsigned i=0;i<2;++i) {
            const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(frame->layers[i+1]);
            check(q && q->type==XR_TYPE_COMPOSITION_LAYER_QUAD,"Independent changed panel layer type");
            const auto captured_pixel=f.read_pixel(f.chains.at(q->subImage.swapchain),q->subImage.imageRect.offset.x,q->subImage.imageRect.offset.y,0);
            check(captured_pixel>=(panel_budget_test&&i?70u:30u),"HUD and counter source pixels were confused");
            const unsigned base=panel_budget_test&&i?70:30;
            check(captured_pixel>=base+source-1 && captured_pixel<=base+source,"panel pixel must belong to its exact current/previous source pair");
            const auto captured_source=source;
            if(panel_budget_test && i)check(q->subImage.swapchain!=
                reinterpret_cast<const XrCompositionLayerQuad*>(frame->layers[1])->subImage.swapchain,
                "distinct HUD and counter sources share one native publication image");
            const XrExtent2Df expected_size=independent_resize
                ? XrExtent2Df{captured_source%2?1.25f:3.f,captured_source%3?2.f:.75f}:XrExtent2Df{1,1};
            check(q->space==reinterpret_cast<XrSpace>(9) && q->pose.orientation.w==1.f &&
                q->size.width==expected_size.width && q->size.height==expected_size.height,
                "Independent froze stale panel size after a source resize");
            check(q->subImage.imageRect.offset.x==int(i*image_width/2) &&
                q->subImage.imageRect.extent.width==int(image_width/2) &&
                q->subImage.imageRect.offset.y==(independent_resize?int((captured_source%2)*image_height/2):0) &&
                q->subImage.imageRect.extent.height==int(independent_resize?image_height/2:image_height) && q->subImage.imageArrayIndex==0,
                "Independent changed original panel atlas or array slice");
            if(i)check(q->pose.position.x==5.f,"Independent moved the stationary peer panel");
        }
        ++independent_panel_checks;
    };
    if(benchmark_sequence) {
        std::fprintf(stderr,"BENCH policies=%u real_inputs=%u actual_3x_to_2x_transition=%u explicit_prefer_fps_transition_diagnostic=%u memory_sampling=%s\n",
            benchmark_policy_phases,input_count,unsigned(benchmark_policy_phases==7 &&
                (!benchmark_dlssg_sequence || state->native_dlss_generated_frames>=2)),
            unsigned(benchmark_dlssg_transition && state->native_dlss_generated_frames<2),
            completed_frame_memory?"completed-frame-drain":"overlapped-phase-drain");std::fflush(stderr);
    }
    std::vector<std::shared_ptr<xrfg::InterfaceCycle>> measured_cycles;
    std::vector<std::int64_t> rife_source_ends;
    std::optional<XrTime> rife_replay_time;
    unsigned rife_phase_start{},rife_bank_epoch_start{};
    unsigned rife_phase_factor=rife_factor?rife_factor:ofxr_factor;
    unsigned late_recovery_outputs_before{};
    bool hfw_gate_proven=false;
    bool independent_ahead=false;
    std::weak_ptr<FrameGenerationSwapchainState> reclaimed_generation;
    std::optional<xrfg::D3D12HistoryCaptureTicket> reclaimed_capture;
    std::size_t native_before{};std::uint64_t work_bytes_before{};
    unsigned rearm_native_before{};bool reclaimed_observed{},rearmed_observed{};

    bool rife_overlap_proven=false;
    std::uint64_t rife_overlap_b_sequence{};
    ComPtr<ID3D12Fence> rife_overlap_compute_gate;
    XrFrameState overlap_prediction{XR_TYPE_FRAME_STATE};bool overlap_wait_ready=false;
    ComPtr<ID3D12Fence> independent_runtime_gate;
    for(unsigned n=1;n<=input_count;++n) {
        if(independent_stream_hold && n==4) {
            xr_ok(wait_for_presenter_idle(state),"drain previous stream metadata");xr_ok(f.transport->drain(),"drain previous stream native output");
            {std::scoped_lock lock(f.gpu);stream_base_pixels=unsigned(f.pixels.size());}
            stream_probe_armed=true;
            stream_rescue=std::make_unique<std::jthread>([gate=stream_input_gate]{std::this_thread::sleep_for(1500ms);gate->Signal(1);});
        }
        if(independent && n==8) {
            const auto generation=find_swapchain(chain)->frame_generation;
            check(generation!=nullptr,"live cap probe requires warmed generator");
            for(int level:{3,0,2,1,0}) {
                auto settings=xrfg::autonomous::snapshot().desired;settings.pacing_buffer=level;
                check(xrfg::autonomous::request(settings),"live independent cap request");
                xrfg::autonomous::apply_runtime_settings();
                ok(apply_embedded_control(state),"live independent cap apply");
                check(find_swapchain(chain)->frame_generation==generation &&
                    state->applied_generation_settings->pacing_buffer==level,
                    "live cap change reset generator/history or failed to reach layer");
            }
            for(bool enabled:{false,true,!independent_runtime_tail_legacy}) {
                auto settings=xrfg::autonomous::snapshot().desired;settings.independent_acquire_fix=enabled;
                check(xrfg::autonomous::request(settings),"live acquire A/B request");xrfg::autonomous::apply_runtime_settings();
                ok(apply_embedded_control(state),"live acquire A/B apply");
                check(find_swapchain(chain)->frame_generation==generation && state->applied_generation_settings->independent_acquire_fix==true,
                    "retired acquire key must not disable the correction or reset the warmed generator");
            }
        }
        if(optical_pressure_delivery && n==3) {
            observe_optical_memory_pressure(*state,96,100);
            std::scoped_lock lock(state->optical_pressure_mutex);
            state->optical_pressure.completed(150000,true,GetTickCount64());
        }
        if(optical_high_memory_fast)observe_optical_memory_pressure(*state,99,100);
        if(optical_pressure_delivery && n==6) {
            {std::scoped_lock lock(state->optical_pressure_mutex);state->optical_pressure.recover_after_ms=0;}
            observe_optical_memory_pressure(*state,96,100);
            check(!optical_pressure_real_only(*state),"retry must not require a menu or lower memory usage");
        }
        if(rife_overlap && n==4) {
            xr_ok(wait_for_synthesis_handoff(state),"overlap probe drains the previous compute job");
            xr_ok(wait_for_presenter_idle(state),"overlap probe drains previous upper descriptors");
            xr_ok(f.transport->drain(),"overlap probe starts with both endpoint credits free");
            check(f.transport->buffer_capacity()==((state->dispatch->steamvr_runtime && f.transport->director_enabled()) || rife_buffer3?3:2),"overlap probe uses Director3 or Framework selected2/3 credits");
            // X3: hold S1, queue S2, leave B at the upper front. X4: let S1
            // retire, hold S2 and queue S3, leaving the same one-real geometry.
            f.end_entered=false;
            rife_overlap_hold_after=f.real_ends.load()+rife_factor-3;
        }
        if(phase_switch && (n-1)%rife_phase_frames==0) {
            if(rife_luke && n==rife_phase_frames+1) {
                auto optical=xrfg::autonomous::snapshot().desired;
                xrfg::autonomous::select_generation(optical,0,2);
                check(xrfg::autonomous::request(optical),"Luke switch back to optical flow");
                xrfg::autonomous::apply_runtime_settings();
                ok(apply_embedded_control(state),"drained Luke optical-flow provider switch");
                const auto generation=find_swapchain(chain)->frame_generation;
                check(generation && !generation->asynchronous_luke_rife &&
                    !generation->afw_temporal_synthesizer->asynchronous_rife_enabled(),
                    "optical flow retained the old RIFE worker or wrapper policy");
            }
            constexpr unsigned factors[]{2,3,4,3,2};
            rife_phase_factor=factors[(n-1)/rife_phase_frames];
            auto desired=xrfg::autonomous::snapshot().desired;
            if(rife_luke)xrfg::autonomous::select_generation(desired,4,2);
            if(ofxr_switch)desired.frame_multiplier=int(rife_phase_factor);
            else desired.rife_multiplier=rife_phase_factor;
            if(rife_luke){constexpr int percentages[]{10,15,21,30,15};desired.rife_resolution=percentages[(n-1)/rife_phase_frames];}
            check(xrfg::autonomous::request(desired),"live RIFE multiplier request");
            xrfg::autonomous::apply_runtime_settings();
            ok(apply_embedded_control(state),"live RIFE output ownership transition");
            check(state->private_runtime==f.transport,"RIFE switch retains private endpoint");
            if(rife_luke)check(state->nvidia_options.rife_resolution_percent==desired.rife_resolution,
                "RIFE percentage change did not reach the native model setup");
            rife_phase_start=f.real_ends.load();
        }
        if(rife_luke && rife_switch && n==5) {
            xr_ok(wait_for_synthesis_handoff(state),"Quality-only old worker completion");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"Quality-only old presenter drain");
            xr_ok(f.transport->drain(),"Quality-only old physical outputs drain");
            auto desired=xrfg::autonomous::snapshot().desired;
            desired.rife_resolution=18; // same multiplier/output shape: no creation-policy rescue
            check(xrfg::autonomous::request(desired),"Luke Quality-only change");
            xrfg::autonomous::apply_runtime_settings();
            const auto quality_result=apply_embedded_control(state);
            if(FAILED(quality_result))std::fprintf(stderr,"LUKE_QUALITY_RESULT=%08x\n",unsigned(quality_result));
            ok(quality_result,"drained Luke Quality-only reconfiguration");
        }
        if(rife_bank_switch && ((n-1)%rife_phase_frames==0 || (n-1)%rife_phase_frames==4 || (n-1)%rife_phase_frames==8)) {
            const unsigned epoch_offset=(n-1)%rife_phase_frames;
            const bool fast=epoch_offset==4;
            if(epoch_offset) {
                xr_ok(wait_for_synthesis_handoff(state),"previous bank epoch GPU complete");
                const auto g=find_swapchain(chain)->frame_generation;const auto factor=rife_frames(*state);
                const bool old_fast=state->lower_vram>0;
                const unsigned expected=factor>2?(factor-1)*(old_fast?1U:2U):(old_fast?1U:2U);
                check(g && g->synthetic_runtime_rings==expected,"actual prior Normal/Fast bank allocation");
                for(unsigned i=0;i<expected;++i){check(g->synthetic[i].handle!=XR_NULL_HANDLE,"missing allocated bank image");
                    for(unsigned j=0;j<i;++j)check(g->synthetic[i].handle!=g->synthetic[j].handle,"allocated bank handles alias");}
            }
            auto desired=xrfg::autonomous::snapshot().desired;desired.lower_vram=desired.separate_synthesis=fast;
            check(xrfg::autonomous::request(desired),"live Normal/Fast output-bank request");
            xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(state),"drained Normal/Fast bank replacement");
            xr_ok(f.transport->drain(),"old bank epoch physical delivery drained");
            if(epoch_offset) {
                const auto delivered=f.real_ends.load()-rife_bank_epoch_start;
                check(delivered>=4+(rife_frames(*state)-1),"each four-source Normal/Fast epoch must deliver a complete generated batch after warmup");
                std::fprintf(stderr,"RIFE_BANK_EPOCH factor=%u sources=4 delivered=%u new_fast=%u PASS\n",rife_frames(*state),delivered,unsigned(fast));
            }
            rife_bank_epoch_start=f.real_ends.load();
        }
        if(rife_report && n==5) {
            xr_ok(wait_for_synthesis_handoff(state),"fault injection waits for old worker");
            xr_ok(wait_for_presenter_idle(state),"fault injection drains old generated images");
            std::scoped_lock lock(state->presenter_mutex);
            state->three_slots.configure(2); // deliberate impossible4X logical capacity
        }
        if(hfw_early && n==4) {
            auto app=find_swapchain(chain);check(bool(app),"cold projection owner");
            const auto cold_generation=app->frame_generation;
            const auto cold_queue=state->luke_synthesis_queue;
            check(cold_generation && cold_generation->isolated_luke_synthesis &&
                !state->luke_shared_generation_count && !(state->graphics_binding_capabilities&8),
                "loading-screen projection was initialized on shared queue before AFW");
            state->graphics_binding_capabilities=(state->graphics_binding_capabilities&~4ULL)|8ULL;
            initialize_luke_synthesis_queue(*state);
            check(state->luke_synthesis_queue==cold_queue && app->frame_generation==cold_generation &&
                !session_uses_shared_queue(*state),"AFW readiness replaced early lane/history or exposed shared projection");
        }
        if(independent_offon && (n==3||n==5||n==6||n==7)) {
            auto desired=xrfg::autonomous::snapshot().desired;desired.enabled=n==5||n==7;
            check(xrfg::autonomous::request(desired),"Independent actual generation off/on");
            xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(state),"Independent worker stop/start keeps XR session");
        }
        if(independent_offon && state->nvidia_options.ofr && (n==8 || n==10)) {
            check(separate_native_publications(*state),"AER always retains separate native publications");
            const auto app=find_swapchain(chain);
            check(app && app->frame_generation && app->frame_generation->separate_native_publications,
                "active AER generation owns separate native publication resources after off/on");
        }
        if(hfw_offon && (n==6 || n==7)) {
            auto desired=xrfg::autonomous::snapshot().desired;desired.enabled=n==7;
            check(xrfg::autonomous::request(desired),"HFW actual OFXR off/on control");
            xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(state),"HFW drain/restart off/on");
        }
        if(benchmark_sequence && n%trial_frames==1) {
            const auto phase=(n-1)/trial_frames;
            auto desired=xrfg::autonomous::snapshot().desired;
            desired.pacing_mode=phase<6?phase/2:2;
            desired.lower_vram=desired.separate_synthesis=phase<6?phase%2:1;
            desired.triple_frame_gen=phase<6 && (!benchmark_dlssg_sequence || state->native_dlss_generated_frames>=2);
            desired.prefer_fps=phase<6;
            check(xrfg::autonomous::request(desired),"actual benchmark pacing/Fast request");
            xrfg::autonomous::apply_runtime_settings();
            if(phase==6 || (phase==0 && benchmark_dlssg_sequence && state->native_dlss_generated_frames<2)) {
                // Exercise both actual pipeline observations without a wall-clock
                // sleep: first records the candidate, second reconciles capacity.
                for(int observation=0;observation<2;++observation) {
                    state->djules_pipeline_poll={};
                    xr_ok(poll_djules_pipeline(state),"actual multiplier capacity reconciliation");
                }
            }
            phase_outputs=f.real_ends.load();
        }
        if(measured)xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
        if(buffer_toggling && (n==3 || n==5 || n==7)) {
            auto generation=find_swapchain(chain)->frame_generation;
            check(bool(generation),"buffer hot-switch requires existing engine history");
            const auto afw_publication=state->luke_afw_last_evaluated_publication;
            const auto engine_epoch=state->djules_configuration_epoch.load();
            const bool directed=f.transport->director_enabled();
            auto desired=xrfg::autonomous::snapshot().desired;desired.increase_buffer=n!=5;
            check(xrfg::autonomous::request(desired),"request only larger/smaller private buffer");
            xrfg::autonomous::apply_runtime_settings();ok(apply_embedded_control(state),"apply buffer without generator reset");
            check(state->private_runtime==f.transport && f.transport->buffer_capacity()==((state->dispatch->steamvr_runtime && directed) || desired.increase_buffer?3:2) && f.transport->director_enabled()==directed,"live buffer retains endpoint and pacing policy");
            check(find_swapchain(chain)->frame_generation==generation && state->luke_afw_last_evaluated_publication==afw_publication,"buffer-only control discarded engine/AFW history");
            check(state->djules_configuration_epoch.load()==engine_epoch,"buffer-only control reset framework pacing/history epoch");
        }
        if(toggling && (n==3 || n==5 || n==7)) {
            auto desired=xrfg::autonomous::snapshot().desired;
            desired.pacing_mode=n==3?0:n==5?2:1;
            check(xrfg::autonomous::request(desired),"publish only the single-director hot switch");
            xrfg::autonomous::apply_runtime_settings();
            ok(apply_embedded_control(state),"drained single-director hot switch");
            check(single_director_pacing(*state)==(desired.pacing_mode==1),"active policy matches live Framework/Director/combined choice");
            check(state->private_runtime==f.transport && f.transport->director_enabled()==(desired.pacing_mode!=0),"live bypass retains the sole physical endpoint");
        }
        if(late_shipping && n==6) {
            auto desired=xrfg::embedded::snapshot().desired;desired.backend=route=="fidelity"?2:0;
            check(xrfg_fixture::request_control(desired),"actual provider change after late completion");
            ok(apply_embedded_control(state),"late work drained before provider reconciliation");
            check(XR_SUCCEEDED(state->presenter_failure),"provider change inherited late-completion failure");
        }
        if(late_shipping && n==8) {
            xr_ok(wait_for_synthesis_handoff(state),"late recovery warm-up worker drained");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"late recovery warm-up outputs drained");
            xr_ok(f.transport->drain(),"late recovery warm-up physically retired");
            late_recovery_outputs_before=f.real_ends.load();
        }
        if(reclaim_test && n==4) {
            xr_ok(wait_for_synthesis_handoff(state),"pressure warm worker complete");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"pressure warm descriptors drained");
            xr_ok(f.transport->drain(),"pressure warm endpoint retired");
            auto source=find_swapchain(chain);reclaimed_generation=source->frame_generation;reclaimed_capture=source->last_released_capture;
            check(!reclaimed_generation.expired() && reclaimed_capture,"real generation/history must be warm before pressure fault");
            native_before=f.chains.size();
            const auto count_work=[&](const auto& outputs) {for(const auto& output:outputs)if(output.handle) {
                unsigned count{};xr_ok(f.transport->enumerate_images(output.handle,0,&count,nullptr),"pressure work count");
                std::vector<XrSwapchainImageD3D12KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                xr_ok(f.transport->enumerate_images(output.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"pressure work images");
                for(const auto& image:images){const auto desc=image.texture->GetDesc();work_bytes_before+=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;}
            }};
            count_work(source->frame_generation->current);count_work(source->frame_generation->synthetic);
            // Repeat real GPU completion delays through the shipping worker
            // completion boundary. The scene's warmed allocations stay intact.
            for(unsigned fault=0;fault<(reclaim_budget?6U:3U);++fault) {
                ComPtr<ID3D12Fence> gate,done;ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"pressure fault gate");
                ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done)),"pressure completion");
                ok(f.queue->Wait(gate.Get(),1),"hold pressure producer");ok(f.queue->Signal(done.Get(),1),"pressure exact completion");
                PreparedProjectionFrame job;job.kind=PreparedGenerationKind::pair;PreparedGeneration g;g.kind=job.kind;
                g.worker_generation=std::make_shared<FrameGenerationSwapchainState>();g.worker_fence=done;g.worker_fence_value=1;
                job.resources.push_back({chain,std::move(g),false});bool delayed{};
                auto waiter=std::async(std::launch::async,[&]{return finish_worker_outputs(state,job,900+fault,&delayed);});
                const bool pending=waiter.wait_for(reclaim_budget?60ms:140ms)==std::future_status::timeout;
                ok(gate->Signal(1),"complete pressure fault safely");check(waiter.get() && pending,"pressure injection must finish exact GPU work");
            }
        }
        if(reclaim_test && n==5) {
            const auto source=find_swapchain(chain);
            const bool freed=reclaimed_generation.expired() && !source->frame_generation && f.chains.size()<native_before;
            xrfg::D3D12HistoryConsumerLease lease;ComPtr<ID3D12Resource> image;ComPtr<ID3D12Fence> producer;
            const auto old=source->d3d12_history->acquire_consumer(*reclaimed_capture,&lease,&image,&producer);
            if(SUCCEEDED(old))source->d3d12_history->cancel_consumer(lease);
            std::printf("RECLAIM native_before=%zu native_after=%zu work_bytes_before=%llu freed=%u old_history_invalid=%u\n",native_before,f.chains.size(),work_bytes_before,freed,unsigned(FAILED(old)));
            check(freed && FAILED(old),"sustained slow completion retained generation/native/history resources");reclaimed_observed=true;
            if(reclaim_transport || reclaim_budget) {
                check(f.transport->clean_bypass_enabled(),"recovery retains generation transport instead of the effects-Off policy");
                check(xrfg::embedded::snapshot().desired.enabled,"internal recovery changed the saved enable preference");
            }
        }
        if(reclaim_held && n==4) {
            f.block_end=true;f.end_entered=false;f.begin_virtual();
            XrFrameState held_clock{XR_TYPE_FRAME_STATE};xr_ok(f.transport->predict_frame(&held_clock),"held native clock");
            std::array<XrCompositionLayerProjectionView,2> held_views{};
            for(unsigned eye=0;eye<2;++eye){auto& view=held_views[eye];view={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;
                view.subImage={chain,{{0,0},{int(image_width),int(image_height)}},eye};}
            XrCompositionLayerProjection held_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};held_layer.viewCount=2;held_layer.views=held_views.data();
            const auto* held_header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&held_layer);
            XrFrameEndInfo held_info{XR_TYPE_FRAME_END_INFO};held_info.displayTime=held_clock.predictedDisplayTime;
            held_info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;held_info.layerCount=1;held_info.layers=&held_header;
            xr_ok(f.transport->end_frame(&held_info),"hold a real native consumer before resource release");f.wait_entered();
        }
        if(reclaim_test && n==8)std::this_thread::sleep_for(3100ms);
        if(reclaim_test && n==10) {
            check(!find_swapchain(chain)->frame_generation,"elapsed cooldown retried before stable real-frame observations");
            if(reclaim_transport || reclaim_budget)check(f.transport->clean_bypass_enabled(),"Wait reset recovery transport before real cadence stabilized");
        }
        if(reclaim_test && n==24)rearm_native_before=f.real_ends.load();
        if(reclaim_test && n>=25 && find_swapchain(chain)->frame_generation)rearmed_observed=true;
        if(switching && (n==3 || n==5 || n==7)) {
            auto desired=xrfg::embedded::snapshot().desired;desired.enabled=true;desired.backend=n==3?1:n==5?3:0;
            desired.frame_generation=0;desired.separate_synthesis=0;desired.lower_vram=0;
            check(xrfg_fixture::request_control(desired),"request route hot handoff");
            ok(apply_embedded_control(state),"apply route hot handoff");check(state->private_runtime==f.transport,"route switch must retain one common distributor");
        }
        if(rife_latency)std::this_thread::sleep_for(16ms);
        std::fprintf(stderr,"frame %u write (physical=%u)\n",n,f.real_ends.load());
        const auto color=layout_alternation?(n%2?40u:176u):((n-1)%8+1)*20;
        if(layout_alternation && n==12) {
            std::scoped_lock lock(state->mutex);
            for(auto& entry:state->projection_streams)
                if(entry.snapshot.layers.front().views.front().subImage.swapchain==packed_layout)
                    entry.observed=std::chrono::steady_clock::now()-kProjectionSourceRetention-1ms;
        }
        if(layout_alternation && n==15) {
            clear_generation_continuity(state);
            check(state->projection_streams.empty(),"structural reset retained cached stream metadata");
        }
        if(rife_overlap && n==5) {
            // Exercise the real application Release/capture path as well as
            // EndFrame: native D12 Release waits for synthesis_busy itself.
            auto released=std::async(std::launch::async,[&]{f.write(chain,color);});
            const bool captured=released.wait_for(1500ms)==std::future_status::ready;
            if(!captured) {
                f.unblock();
                check(released.wait_for(5s)==std::future_status::ready,
                    "source Release did not wake after the blocked native batch was released");
            }
            released.get();
            check(captured,"RIFE next source Release/capture waited for old batch delivery admission after GPU completion");
        } else if(!(independent_ahead && n==4) && (!independent_alternate || n==1 || n%2) && !rife_msfs && !(rife_early_capture_done && n==6) && !(layout_alternation && n%2==0))f.write(chain,color);
        if(split_eyes && (!independent_alternate || n==1 || n%2==0) && !rife_msfs && !(layout_alternation && n%2==0))f.write(right,color+7);
        if(layout_alternation && n%2==0)f.write(packed_layout,color);
        if(layout_alternation && n==11)f.write(chain,color); // one eye skips a released capture
        if(panel_test)f.write(panel_chain,30+n);
        if(panel_budget_test)f.write(second_panel_chain,70+n);
        if(vectors && !split_eyes){auto c=find_swapchain(chain);auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
            for(unsigned eye=0;eye<2;++eye){auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=n;g->previous_serial=n-1;g->publication=2*n+eye;g->producer_queue=f.queue;g->output=images[*c->last_released_index].texture;g->output_slice=eye;g->depth=depth;g->motion_vectors=motion;g->motion_slice=eye;g->output_width=g->depth_width=g->motion_width=image_width;g->output_height=g->depth_height=g->motion_height=image_height;guides->eyes[eye]=g;}c->last_released_motion_vectors=guides;}
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        if(overlap_wait_ready){prediction=overlap_prediction;overlap_wait_ready=false;}
        else xr_ok(rife_msfs?layer_wait_frame(state->handle,nullptr,&prediction):layer_wait_frame_impl(state->handle,nullptr,&prediction),"shipping virtual Wait");
        std::future<XrFrameState> next_wait;
        const auto saved_rife_model=state->nvidia_options.rife_model;
        if(rife_inherited && n==3)state->nvidia_options.rife_model=-1; // legacy optical owner before the RIFE selection
        if(rife_msfs && n<input_count) {
            std::promise<void> entered;auto started=entered.get_future();
            next_wait=std::async(std::launch::async,[&state,entered=std::move(entered)]() mutable {
                XrFrameState next{XR_TYPE_FRAME_STATE};entered.set_value();
                xr_ok(layer_wait_frame(state->handle,nullptr,&next),"MSFS next-thread public Wait");return next;
            });
            started.wait();std::this_thread::sleep_for(300us);
        }
        std::uint64_t begun_ticket{},next_ticket{};
        if(independent_tickets){std::scoped_lock lock(state->mutex);check(!state->pending_frames.empty(),"begun source retains its returned Wait ticket");begun_ticket=state->pending_frames.back().serial;}
        xr_ok(rife_msfs?layer_begin_frame(state->handle,nullptr):layer_begin_frame_impl(state->handle,nullptr),"shipping virtual Begin");
        if(measured)if(auto cycle=state->interface_active_cycle.load())measured_cycles.push_back(std::move(cycle));
        if(rife_msfs) {
            if(next_wait.valid()){overlap_prediction=next_wait.get();overlap_wait_ready=true;}
            state->nvidia_options.rife_model=saved_rife_model;
            if(rife_inherited && n==3)check(state->pipelined_presenter_mode,"inherited optical overlap must exercise the production legacy promotion");
            f.write(chain,color);f.write(right,color+7);
        }
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye){auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.x=((sole || independent)?float(n):0.F)+(eye?.032F:-.032F);v.pose.position.y=float(n);v.fov={-.7F,.7F,.7F,-.7F};v.subImage.swapchain=split_eyes && eye?right:chain;v.subImage.imageArrayIndex=split_eyes?0:eye;v.subImage.imageRect.extent={int(image_width),int(image_height)};}
        if(independent_aer4)for(unsigned eye=0;eye<2;++eye) {
            views[eye].pose.position.x=.1f*n+(eye?.032F:-.032F);views[eye].pose.position.y=.1f*n;
        } // stay within the production camera-discontinuity limit
        if(layout_alternation && n%2==0)for(unsigned eye=0;eye<2;++eye){views[eye].subImage.swapchain=packed_layout;views[eye].subImage.imageArrayIndex=0;views[eye].subImage.imageRect.offset.x=int(eye*image_width);}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        auto invalid_views=views;
        XrCompositionLayerProjection invalid_projection=projection;
        std::array<const XrCompositionLayerBaseHeader*,2> invalid_layers{layer,reinterpret_cast<const XrCompositionLayerBaseHeader*>(&invalid_projection)};
        if(invalid_live) {
            f.inspect_layers=[](const XrFrameEndInfo* submitted) {
                for(unsigned l=0;l<submitted->layerCount;++l) {
                    if(submitted->layers[l]->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
                    const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(submitted->layers[l]);
                    check(p->viewCount==2,"a stereo projection was partially forwarded");
                    for(unsigned v=0;v<p->viewCount;++v) {
                        const auto& rect=p->views[v].subImage.imageRect;
                        check(rect.offset.x>=0 && rect.offset.y>=0 && rect.extent.width>0 && rect.extent.height>0,
                            "native runtime received invalid application rectangle");
                    }
                }
            };
            state->uevr_application=true;
            if(n==6) {
                // One invalid eye removes this whole additional stereo layer;
                // the valid scene and its subsequent generation must survive.
                invalid_views[1].subImage.imageRect={{INT_MIN,INT_MIN},{INT_MIN,INT_MIN}};
                invalid_projection.views=invalid_views.data();end.layerCount=2;end.layers=invalid_layers.data();
            }
        }
        if(binding_probe && n==4) {
            xr_ok(wait_for_synthesis_handoff(state),"binding probe prior worker done");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"binding probe prior presenter idle");
            xr_ok(f.transport->drain(),"binding probe private readers retired");
            ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"probe source metadata");
            auto mapping=build_projection_resource_mappings(snapshot);check(!mapping.mappings.empty(),"probe source mapping");
            ComPtr<ID3D12Fence> gate,app_progress;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"runtime gate");
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&app_progress)),"independent app receipt");
            ok(state->binding_queue->Wait(gate.Get(),1),"hold unrelated runtime queue");
            ok(f.queue->Signal(app_progress.Get(),1),"game queue keeps progressing");
            const bool app_independent=SUCCEEDED(wait_composition_fence(app_progress.Get(),1,100));
            // Production prepare submits real GPU work and its exact fence.
            auto prepared=prepare_projection_frame(snapshot,mapping.mappings,binding_probe_pair,.5F,true,&state->presentation_transform);
            bool submitted=prepared.kind!=PreparedGenerationKind::none && !prepared.resources.empty();
            bool independent=submitted;
            for(const auto& resource:prepared.resources) {
                const auto& g=resource.generation;
                independent &= g.worker_fence && g.worker_fence_value &&
                    SUCCEEDED(wait_composition_fence(g.worker_fence.Get(),g.worker_fence_value,100));
            }
            // Always release the fault before cleanup/assertions. No forged
            // synthesis completion or signal of the producer's own fence.
            const bool gate_held=gate->GetCompletedValue()==0;
            ok(gate->Signal(1),"release injected native backlog");
            bool completed=submitted && finish_worker_outputs(state,prepared,400);
            unsigned pixel{};
            if(completed) {
                const auto output=prepared.resources.front().generation.current_handle;
                unsigned count{};xr_ok(f.transport->enumerate_images(output,0,&count,nullptr),"probe private output count");
                std::vector<XrSwapchainImageD3D12KHR> result(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
                xr_ok(f.transport->enumerate_images(output,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(result.data())),"probe private output resources");
                Chain image;image.state=D3D12_RESOURCE_STATE_COMMON;image.d12.emplace_back(result[0].texture);
                pixel=f.read_pixel(image);
            }
            std::printf("BINDING_PROBE pair=%u submitted=%u app_independent=%u synthesis_independent=%u gate_held=%u recovered=%u pixel=%u expected=%u\n",
                unsigned(binding_probe_pair),unsigned(submitted),unsigned(app_independent),unsigned(independent),unsigned(gate_held),unsigned(completed),pixel,color);
            prepared={};
            // Negative control: an actual pending source-history copy MUST
            // still gate synthesis, independently of the runtime exemption.
            ComPtr<ID3D12Fence> source_gate;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&source_gate)),"source dependency gate");
            auto source=find_swapchain(chain);auto history=source->d3d12_history;
            ok(f.queue->Wait(source_gate.Get(),1),"withhold actual history producer");
            xrfg::D3D12HistoryCaptureTicket source_ticket;
            ok(history->capture(*source->last_released_index,&source_ticket),"pending source history copy");
            ok(history->commit(source_ticket),"pending source history commit");
            {std::scoped_lock lock(source->mutex);source->last_released_capture=source_ticket;}
            auto protected_output=prepare_projection_frame(snapshot,mapping.mappings,false,.5F,true,&state->presentation_transform);
            bool source_protected=protected_output.kind!=PreparedGenerationKind::none && !protected_output.resources.empty();
            for(const auto& resource:protected_output.resources) {
                const auto& g=resource.generation;
                source_protected &= g.worker_fence && g.worker_fence_value &&
                    wait_composition_fence(g.worker_fence.Get(),g.worker_fence_value,100)==HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            ok(source_gate->Signal(1),"complete actual history producer");
            const bool source_resumed=finish_worker_outputs(state,protected_output,401);
            std::printf("SOURCE_CONTROL protected=%u resumed=%u\n",unsigned(source_protected),unsigned(source_resumed));
            protected_output={};stop_continuous_presenter(state);f.transport->stop();
            for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
            xr_ok(layer_destroy_swapchain_impl(chain),"probe application source cleanup");
            {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
            check(app_independent && gate_held && submitted && completed && pixel==color,"fault probe input/completion/pixels invalid");
            check(source_protected && source_resumed,"private source ownership/fence protection lost");
            check(independent,"private synthesis inherited unrelated runtime queue backlog");
            return 0;
        }
        std::array<XrCompositionLayerQuad,2> panel_quads;
        std::array<const XrCompositionLayerBaseHeader*,3> panel_layers{layer};
        if(panel_test) {
            for(unsigned i=0;i<2;++i){auto& q=panel_quads[i];q={XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.space=projection.space;q.pose.orientation.w=1;q.pose.position.x=i?5.f:.1f*n;
                q.size={1,1};q.subImage.swapchain=panel_budget_test&&i?second_panel_chain:panel_chain;q.subImage.imageRect={{int(i*image_width/2),0},{int(image_width/2),int(image_height)}};
                if(independent_resize){q.size={n%2?1.25f:3.f,n%3?2.f:.75f};q.subImage.imageRect.offset.y=int((n%2)*image_height/2);q.subImage.imageRect.extent.height=int(image_height/2);}
                panel_layers[i+1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);}
            end.layerCount=3;end.layers=panel_layers.data();
            if(panel_lifecycle_test && (n==5||n==6))end.layerCount=2;
            if(budget_fallback_test && n<5)end.layerCount=2;
        }
        if(rife_inherited && n==5) {
            auto desired=xrfg::autonomous::snapshot().desired;desired.rife_multiplier=3;
            check(xrfg::autonomous::request(desired),"actual GUI RIFE3 request while legacy overlap owner exists");
        }
        if(rife_msfs)end.displayTime+=period;
        if(rife_replay) {
            // Exact app timestamp gap of the first failing V739 pair; image
            // captures and real runtime delivery remain independently owned.
            if(rife_replay_time)end.displayTime=*rife_replay_time+(n%2?34'935'700:34'294'989);
            rife_replay_time=end.displayTime;
        }
        if(independent_runtime_tail && n==4) {
            xr_ok(wait_for_synthesis_handoff(state),"prior generator submitted before binding-tail probe");
            xr_ok(wait_for_presenter_idle(state),"prior outputs retired before binding-tail probe");
            xr_ok(f.transport->drain(),"prior native reads complete before binding-tail probe");
            check(state->binding_queue && state->private_runtime,"probe requires private targets and distinct legacy binding queue");
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&independent_runtime_gate)),"unrelated binding-tail gate");
            ok(state->binding_queue->Wait(independent_runtime_gate.Get(),1),"hold only the old consumer queue tail");
        }
        if(rife_latency)rife_source_ends.push_back(xrfg::interface_clock_ns());
        if(rife_overlap_retire && n==5) {
            check(bool(state->d3d12_synthesis_queue),"retirement race requires the real independent synthesis queue");
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&rife_overlap_compute_gate)),
                "retirement-race compute gate");
            const auto compute=rife_overlap_capture
                ? find_swapchain(chain)->frame_generation->synthesizer->submission_queue()
                : state->d3d12_synthesis_queue;
            check(bool(compute),"exact RIFE queue for capture overlap proof");
            ok(compute->Wait(rife_overlap_compute_gate.Get(),1),
                "hold C compute while the presenter retires B");
        }
        if(rife_app_overlap && !rife_msfs && n<input_count) {
            xr_ok(layer_wait_frame_impl(state->handle,nullptr,&overlap_prediction),"next application Wait precedes current End");
            overlap_wait_ready=true;
            check(continuous_presenter_active(state) || state->application_frame_has_overlapping_wait,
                "private RIFE keeps overlapping application-frame bookkeeping");
        }
        if(independent_tickets && n>=3) {
            // The next Wait sometimes finishes before this End, sometimes
            // after it. Rewriting displayTime must not change ticket ownership.
            if(n<input_count && n%3!=0) {
                xr_ok(layer_wait_frame_impl(state->handle,nullptr,&overlap_prediction),"Independent next Wait before current End");
                overlap_wait_ready=true;
                std::scoped_lock lock(state->mutex);next_ticket=state->pending_frames.back().serial;
            }
            end.displayTime+=period+1234;
            const auto selected=latest_pending_application_frame(state,end.displayTime,false,true);
            check(selected && selected->serial==begun_ticket,"Independent End selects its begun source, never the newer Wait or rewritten prediction");
        }
        if(hfw) std::fprintf(stderr,"HFW before%u active%u worker%u request%u forbidden%u mode%u enabled%u\n",n,
            unsigned(continuous_presenter_active(state)),unsigned(state->three_slot_director),unsigned(state->presenter_start_requested),
            unsigned(presenter_forbidden(*state)),unsigned(state->single_director_requested.load()),unsigned(state->menu_enabled.load()));
        if(reclaim_held && n==4) {
            auto returning=std::async(std::launch::async,[&]{return layer_end_frame_impl(state->handle,&end);});
            const bool waiting=returning.wait_for(100ms)==std::future_status::timeout;
            const bool retained=!reclaimed_generation.expired() && f.chains.size()==native_before;
            f.unblock();xr_ok(returning.get(),"resource drain resumes after real native consumer retires");
            check(waiting && retained,"resource recovery freed an image still owned by native consumer");
        } else if(late_shipping && n==4) {
            xr_ok(wait_for_synthesis_handoff(state),"late shipping previous worker idle");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"late shipping previous consumer idle");
            xr_ok(f.transport->drain(),"late shipping previous private readers retired");
            ComPtr<ID3D12Fence> gate;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"late shipping GPU gate");
            ok(state->d3d12_synthesis_queue->Wait(gate.Get(),1),"hold real 2X synthesis commands");
            auto accepted=std::async(std::launch::async,[&]{return layer_end_frame_impl(state->handle,&end);});
            std::this_thread::sleep_for(1150ms);
            bool healthy{};{std::scoped_lock lock(state->presenter_mutex);healthy=XR_SUCCEEDED(state->presenter_failure);}
            const bool held=gate->GetCompletedValue()==0;
            ok(gate->Signal(1),"finish delayed actual synthesis commands");
            xr_ok(accepted.get(),"shipping End returns after late 2X completion");
            check(held && healthy,"shipping 2X path latched failure at one second");
            std::puts("Shipping 2X timeout survives and actual provider switch follows PASS");
        } else if(hfw && !toggling && !hfw_gate_proven && n>=4 && state->three_slot_director && continuous_presenter_active(state)) {
            xr_ok(wait_for_synthesis_handoff(state),"prior HFW generation finished");
            xr_ok(wait_for_presenter_idle(state),"prior HFW upper output retired");
            xr_ok(f.transport->drain(),"prior HFW native delivery drained");
            ComPtr<ID3D12Fence> gate;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"HFW OFXR gate");
            ok(state->luke_synthesis_queue->Wait(gate.Get(),1),"park OFXR while game hands over next source");
            HANDLE returned=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(returned!=nullptr,"HFW return event");
            std::thread safety([&]{WaitForSingleObject(returned,300);gate->Signal(1);});
            const auto accepted=layer_end_frame_impl(state->handle,&end);
            const bool independent=gate->GetCompletedValue()==0;
            bool producer_lock_free=true;
            if(rife_luke && independent) {
                // While the real GPU input is withheld, wait until the worker
                // has submitted its transaction. CPU waits must not retain the
                // session lock needed by capture/OSR for the next game frame.
                std::this_thread::sleep_for(20ms);
                producer_lock_free=state->gpu_mutex.try_lock();
                if(producer_lock_free)state->gpu_mutex.unlock();
            }
            gate->Signal(1);SetEvent(returned);safety.join();CloseHandle(returned);
            check(producer_lock_free,"RIFE GPU input wait retained the producer GPU mutex");
            xr_ok(accepted,"HFW accepted next game source");
            check(independent,"HFW game waited for OFXR synthesis/publication completion");
            hfw_gate_proven=true;
            std::puts("HFW game returns while actual OFXR GPU queue is still blocked PASS");
        } else if(independent_held && n==3) {
            ComPtr<ID3D12Fence> gate;ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"independent worker gate");
            ok(state->d3d12_synthesis_queue->Wait(gate.Get(),1),"hold only generator GPU queue");
            auto submitted=std::async(std::launch::async,[&]{return layer_end_frame_impl(state->handle,&end);});
            const bool returned=submitted.wait_for(500ms)==std::future_status::ready;
            if(!returned)gate->Signal(1);
            xr_ok(submitted.get(),"source handoff while generator GPU is held");
            check(returned,"source End inherited generation completion");
            bool handed_to_consumer=false;
            const auto handoff_limit=std::chrono::steady_clock::now()+500ms;
            while(std::chrono::steady_clock::now()<handoff_limit) {
                {std::scoped_lock lock(state->presenter_mutex);
                 handed_to_consumer=!state->synthesis_busy && state->independent_sources.empty() &&
                     (state->independent_completion_busy || !state->independent_completions.empty());}
                if(handed_to_consumer)break;
                std::this_thread::yield();
            }
            const bool gpu_still_pending=gate->GetCompletedValue()==0;
            if(!handed_to_consumer)gate->Signal(1);
            check(handed_to_consumer && gpu_still_pending,
                "submission worker retained transaction until GPU completion instead of handing it to consumer");
            auto captured=std::async(std::launch::async,[&]{f.write(chain,80);});
            const bool free=captured.wait_for(500ms)==std::future_status::ready;
            gate->Signal(1);captured.get();
            check(free,"next game source capture inherited blocked worker queue/mutex");
            std::puts("INDEPENDENT_SUBMIT pending GPU output is consumer-owned while submitting worker is free PASS");
            independent_ahead=true;
            std::puts("INDEPENDENT_GPU_HELD End and next capture return before generator GPU completes PASS");
        } else if(blocked && n==(independent?3U:2U)) {
            f.wait_count(1); // one confirmed native prime; another cold source may legitimately expire
            f.block_end=true;f.end_entered=false;
            auto producer=std::async(std::launch::async,[&]{return layer_end_frame_impl(state->handle,&end);});
            const bool returned=producer.wait_for(500ms)==std::future_status::ready;
            bool worker_free=!state->independent_pacing;
            if(state->independent_pacing) {
                const auto limit=std::chrono::steady_clock::now()+500ms;
                while(std::chrono::steady_clock::now()<limit) {
                    {std::scoped_lock lock(state->presenter_mutex);
                     worker_free=!state->synthesis_busy && state->independent_sources.empty() && f.end_entered;}
                    if(worker_free)break;
                    std::this_thread::yield();
                }
            }
            const bool native_held=f.block_end;
            f.unblock();
            xr_ok(producer.get(),"shipping End while native End is withheld");
            check(returned,"bridge acceptance returns to game before blocked native End with spare credits");
            if(state->independent_pacing) {
                check(worker_free && native_held,"generation submission must be free while native presentation is still blocked");
                std::puts("INDEPENDENT_NATIVE_HELD game and generation submitter are free before native End returns PASS");
            }
        } else {
            const auto submitted=rife_msfs?layer_end_frame(state->handle,&end):layer_end_frame_impl(state->handle,&end);
            if(rife_inherited && n==5) {
                const auto ack=xrfg::embedded::snapshot();
                std::fprintf(stderr,"RIFE3_ACK pending=%u errors=%u active_factor=%u legacy=%u\n",ack.pending,ack.errors,rife_frames(*state),unsigned(state->pipelined_presenter_mode));
                check(!ack.pending && !ack.errors && rife_frames(*state)==3,"RIFE3 must finish Applying settings and activate the requested multiplier");
            }
            xr_ok(submitted,"shipping OFXR End handoff");
            // This oracle starts from warmed generation. Cold construction is
            // exercised separately with a gated worker and live native frames.
            finish_session_generation_creation(state);
            if(panel_test && independent && !independent_aer && !state->generation_budget_exhausted) {
                // Finish optional warm-up only AFTER the eye futures above.
                // The output-count oracle below starts warm; it must not
                // assume panels can race unfinished eyes on the first frame.
                ProjectionSnapshot warmed;
                if(capture_projection_snapshot(&end,&warmed)) {
                    warmed.panel_corrections=true;
                    const auto mapped=build_projection_resource_mappings(warmed);
                    const auto optional=panel_sources(*state,warmed,mapped.mappings);
                    if(!arm_panel_generation(state,optional)) {
                        state->panel_budget_refused=true;
                        retire_panel_generation(state);
                    }
                }
            }
            if(budget_fallback_test && n==4) {
                budget_meter=f.make_chain(false,1);
                // Pooling saves native handles. Fill the actual remaining
                // budget so the late second UI still exercises refusal and
                // safe real-frame recovery instead of becoming a success test.
                while(f.chains.size()<f.native_chain_limit)extra_budget_meters.push_back(f.make_chain(false,1));
            }
            if(panel_lifecycle_test && n==5) {
                check(!find_swapchain(second_panel_chain)->frame_generation,
                    "closed UEVR panel retained optional native generation rings");
                check(find_swapchain(panel_chain)->frame_generation!=nullptr,
                    "closing one panel retired the other visible panel");
            }
        }
        if(independent_stream_hold && n==4) {
            bool first=false,only_first=false;unsigned right_pixel{};
            const auto deadline=std::chrono::steady_clock::now()+300ms;
            while(std::chrono::steady_clock::now()<deadline) {
                {std::scoped_lock lock(f.gpu);
                 if(f.pixels.size()>stream_base_pixels){first=true;only_first=f.pixels.size()==stream_base_pixels+1;
                     right_pixel=f.right_pixels[stream_base_pixels];break;}}
                std::this_thread::sleep_for(1ms);
            }
            const bool input_still_blocked=stream_input_gate->GetCompletedValue()==0;
            bool second_pending{};{std::scoped_lock lock(state->presenter_mutex);second_pending=state->independent_completion_busy;}
            ok(stream_input_gate->Signal(1),"release newer source after first native presentation probe");stream_rescue.reset();
            check(first && only_first && input_still_blocked && second_pending,
                "first coherent native AER presentation waited for second eye or published second prematurely");
            check(right_pixel==67,"early native AER eye is not exact prior same-eye color");stream_probe_done=true;
            std::puts("AER_NATIVE_FIRST first reached native while newer input/second generation pending; exact previous eye PASS");
        }
        if(independent_runtime_tail && n==4) {
            std::unique_lock lock(state->presenter_mutex);
            const bool ready=state->presenter_condition.wait_for(lock,500ms,[&] {
                return state->independent_sources.empty() && !state->synthesis_busy &&
                    state->independent_completions.empty() && !state->independent_completion_busy;
            });
            lock.unlock();
            // Always release before any assertion/teardown. The final consumer
            // may still need this queue; only generation must be independent.
            ok(independent_runtime_gate->Signal(1),"release binding-tail probe");
            check(ready && XR_SUCCEEDED(state->presenter_failure),"retired acquire setting restored the unrelated binding-tail dependency");
            std::printf("INDEPENDENT_RUNTIME_TAIL AB fix=%d gated_completion=%d exact capture/ownership PASS\n",!independent_runtime_tail_legacy,ready);
        }
        if(independent_tickets && n>=3) {
            std::scoped_lock lock(state->mutex);
            check(std::none_of(state->pending_frames.begin(),state->pending_frames.end(),[&](const auto& t){return t.serial<=begun_ticket;}),"End retires its begun ticket");
            if(next_ticket)check(state->pending_frames.size()==1 && state->pending_frames.front().serial==next_ticket,"Independent End must preserve the next unbegun Wait ticket");
            else check(state->pending_frames.empty(),"late next Wait does not invent a pending ticket");
        }
        if(layout_alternation && n>=4)check(state->generation_resume_wall_time.time_since_epoch().count()==0,
            "alternating valid projection layouts entered a one-second quarantine");
        if(rife_overlap && n==4) {
            std::unique_lock lock(state->presenter_mutex);
            rife_overlap_b_sequence=state->next_source_sequence-1;
            check(state->presenter_condition.wait_for(lock,5s,[&] {
                const auto& front=state->three_slots.slot(0);
                if(f.transport->buffer_capacity()>=3)
                    return !state->synthesis_busy && XR_SUCCEEDED(state->presenter_failure) && state->three_slots.can_accept_real();
                return front && !front->synthetic && front->value->frame &&
                    front->value->source_sequence==rife_overlap_b_sequence && state->three_slots.can_accept_real() &&
                    state->presenter_inflight_frame==front->value->frame;
            }),"native End gate did not leave old B completed and the next source admissible");
            lock.unlock();
            // In4X Director S1 may finish and B reach the upper front before
            // S2 enters its later native refresh slot. Do not accidentally
            // unblock S1 and then arm a brand-new S2 hold after the test moved on.
            std::unique_lock native_lock(f.mutex);
            check(f.changed.wait_for(native_lock,5s,[&] {
                return rife_overlap_hold_after==UINT_MAX && f.block_end && f.end_entered;
            }),"target native synthetic End did not consume the overlap gate");
        }
        if(rife_overlap && n==5) {
            bool computed{};
            if(rife_overlap_retire) {
                f.unblock();
                {
                    std::unique_lock lock(state->presenter_mutex);
                    computed=state->presenter_condition.wait_for(lock,rife_overlap_capture?500ms:5s,[&] {
                        if(rife_overlap_capture)return state->synthesis_busy && state->synthesis_capture_ready;
                        const auto& current=state->three_slots.slot(state->three_slots.capacity()-2);
                        return !state->three_slots.slot(0) && current && !current->synthetic &&
                            current->value->source_sequence==rife_overlap_b_sequence+1 &&
                            !current->value->frame;
                    });
                    if(!computed) {
                        std::fprintf(stderr,"RETIRE_GATE physical=%u busy=%u failure=%d inflight=%p queue=%p delivery=%p gate=%llu\n",
                            f.real_ends.load(),unsigned(state->synthesis_busy),int(state->presenter_failure),
                            state->presenter_inflight_frame.get(),state->d3d12_synthesis_queue.Get(),
                            f.delivery_queue.Get(),rife_overlap_compute_gate->GetCompletedValue());
                        for(std::size_t index=0;index<state->three_slots.capacity();++index) {
                            const auto& entry=state->three_slots.slot(index);
                            std::fprintf(stderr,"RETIRE_SLOT index=%zu present=%u synthetic=%u source=%llu frame=%p\n",
                                index,unsigned(bool(entry)),unsigned(entry && entry->synthetic),
                                entry?entry->value->source_sequence:0,entry?entry->value->frame.get():nullptr);
                        }
                    }
                }
                if(rife_overlap_capture && computed) {
                    // Real public Release while the previous generation's GPU
                    // receipt is deliberately withheld. First suppress the new
                    // ownership signal as the old-path negative control.
                    bool owned=false;
                    {std::unique_lock lock(state->presenter_mutex);
                        owned=state->presenter_condition.wait_for(lock,2s,[&]{return state->synthesis_capture_ready && state->synthesis_busy;});
                        if(owned)state->synthesis_capture_ready=false;}
                    HANDLE rescued=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(rescued!=nullptr,"capture rescue event");
                    auto gate=rife_overlap_compute_gate;
                    std::thread rescue([gate,rescued]{WaitForSingleObject(rescued,3000);gate->Signal(1);});
                    auto next_capture=std::async(std::launch::async,[&]{f.write(chain,color+20);});
                    const bool old_blocked=next_capture.wait_for(30ms)==std::future_status::timeout;
                    {std::scoped_lock lock(state->presenter_mutex);state->synthesis_capture_ready=owned;}
                    state->presenter_condition.notify_all();
                    const bool captured=next_capture.wait_for(500ms)==std::future_status::ready;
                    const bool gpu_pending=gate->GetCompletedValue()==0;
                    gate->Signal(1);SetEvent(rescued);rescue.join();CloseHandle(rescued);next_capture.get();
                    check(owned,"real worker must publish source ownership before GPU completion");
                    check(old_blocked,"negative control must reproduce full-worker Release blocking");
                    check(captured && gpu_pending,"public source capture must return while previous generation GPU is still held");
                    rife_early_capture_done=true;
                    std::puts("Real public RIFE Release: old gate blocked, owned input captured with prior GPU still pending PASS");
                }
                ok(rife_overlap_compute_gate->Signal(1),"release C compute after B has advanced the logical head");
                check(computed,"old real B did not retire while C compute was independently held");
                {
                    std::unique_lock lock(state->presenter_mutex);
                    computed=state->presenter_condition.wait_for(lock,5s,[&] {
                        return !state->synthesis_busy || XR_FAILED(state->presenter_failure);
                    }) && XR_SUCCEEDED(state->presenter_failure);
                }
                check(computed,"RIFE shifted current C twice after old B retired during its compute");
            } else {
                std::unique_lock lock(state->presenter_mutex);
                const auto complete=[&] {
                    const auto& front=state->three_slots.slot(0);
                    const auto& tail=state->three_slots.slot(state->three_slots.capacity()-1);
                    if(f.transport->buffer_capacity()==2)
                        return front && !front->synthetic && front->value->source_sequence==rife_overlap_b_sequence &&
                            tail && !tail->synthetic && tail->value->source_sequence==rife_overlap_b_sequence+1 &&
                            bool(tail->value->frame);
                    // With three endpoint credits old B itself may already be
                    // accepted behind the held native synthetic output. Its
                    // upper descriptor can retire; completed C may shift out
                    // of the tail while its outputs await those same credits.
                    // Still require C's actual completed GPU publication while
                    // the old native batch is held, not merely source capture.
                    for(std::size_t i=0;i<state->three_slots.capacity();++i) {
                        const auto& entry=state->three_slots.slot(i);
                        if(entry && !entry->synthetic && entry->value->source_sequence==rife_overlap_b_sequence+1 && entry->value->frame)return true;
                    }
                    return false;
                };
                // The production worker need not notify a test observer between
                // setting C.frame and waiting for B's insertion slot.
                const auto limit=std::chrono::steady_clock::now()+5s;
                while(!(computed=complete()) && std::chrono::steady_clock::now()<limit)
                    state->presenter_condition.wait_for(lock,10ms);
                if(computed && f.transport->buffer_capacity()==2)check(!state->three_slots.can_accept_real(),
                    "overlap admitted a second future real source beyond bounded B/C capacity");
            }
            const bool native_still_held=rife_overlap_retire || f.block_end.load();
            f.unblock();
            check(computed && native_still_held,
                "RIFE next pair compute/publication waited for old batch delivery admission");
            rife_overlap_proven=true;
            std::printf("RIFE_OVERLAP factor=%u pacing=%d retire_during_compute=%u next_source_capture_and_compute_independent_of_old_batch_delivery PASS\n",
                rife_factor,rife_pacing,unsigned(rife_overlap_retire));
        }
        if(rife_tail && (n==4 || n==8)) {
            // No drain request, extra source, control change or Director clock:
            // completed B must reach native End while the producer is stopped.
            xr_ok(wait_for_synthesis_handoff(state),"RIFE ready batch independent of a future source");
            const unsigned expected=2+(n-2)*rife_factor;
            f.wait_count(expected);
            check(f.real_ends==expected,"RIFE tail repeated or lost an output");
            std::printf("RIFE_NO_FUTURE factor=%u source=%u outputs=%u PASS\n",rife_factor,n,f.real_ends.load());
        }
        if(rife_report && n==5) {
            // The deliberately impossible4X buffer faults asynchronously after
            // this accepted End. The terminal error can already reach the next
            // Release, so do not insist on another successful f.write first.
            const auto failure=wait_for_synthesis_handoff(state);
            check(failure==XR_ERROR_RUNTIME_FAILURE,"injected impossible4X batch must fail with its real worker error");
            unsigned failed_index{};
            xr_ok(f.transport->acquire_image(chain,nullptr,&failed_index),"fault probe acquires the next application image");
            XrSwapchainImageWaitInfo failed_wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};failed_wait.timeout=XR_INFINITE_DURATION;
            xr_ok(f.transport->wait_image(chain,&failed_wait),"fault probe waits for its application image");
            auto failed_source=find_swapchain(chain);
            {std::scoped_lock lock(failed_source->mutex);failed_source->acquired_indices.push_back(failed_index);++failed_source->waited_count;}
            XrSwapchainImageReleaseInfo failed_release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            check(layer_release_swapchain_image_impl(chain,&failed_release)==failure,
                "next application Release must propagate the terminal synthesis failure rather than fabricate success");
            stop_continuous_presenter(state);
            const auto directory=xrfg::bridge_flight_logger().log_path().parent_path();
            bool report{};
            for(const auto& file:std::filesystem::directory_iterator(directory)) {
                if(!file.path().filename().wstring().starts_with(L"ofxr-worker-failure-"))continue;
                if(file.path().extension()!=L".txt")continue;
                std::ifstream stream(file.path());std::string text{std::istreambuf_iterator<char>(stream),{}};
                std::fprintf(stderr,"WORKER_FAILURE_REPORT\n%s",text.c_str());
                check(text.find("stage=8")!=std::string::npos && text.find("RIFE4X buffer admission failed")!=std::string::npos,
                    "cold report retains actual worker stage and exception text");
                check(std::filesystem::exists(file.path().parent_path()/(file.path().stem().wstring()+L".flight.log")),
                    "first-error log snapshot survives later circular-log overwrites");report=true;
            }
            check(report,"first worker error persisted before logger wrap");
            f.transport->stop();
            for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
            xr_ok(layer_destroy_swapchain_impl(chain),"faulted source cleanup");
            {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
            std::puts("Injected4X worker failure: stage, exception and durable F9 report PASS");
            return 0;
        }
        check(xrfg::autonomous::snapshot().runtime.eye_long_side==std::max(image_width,image_height),
            "shipping runtime publishes the actual per-eye projection extent for benchmark quality limits");
        if(sole && !benchmark_sequence && !blocked && !toggling && n==2 && state->three_slot_director) {
            xr_ok(wait_for_synthesis_handoff(state),"second real generated without a future source");
            f.wait_count(2);
            check(f.real_ends>=2,"ready real is delivered before a third application source exists");
        }
        std::fprintf(stderr,"frame %u accepted\n",n);
        if(completed_frame_memory) {
            // Measure actual completed prime/pair work, including lazily
            // allocated provider intermediates. These drains belong only to
            // the memory probe; the normal asynchronous fixture is unchanged.
            xr_ok(wait_for_synthesis_handoff(state),"steady memory completed synthesis");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"steady memory completed presenter");
            xr_ok(f.transport->drain(),"steady memory completed native FIFO");
            f.gpu_wait();if(f.delivery_queue)f.gpu_wait(f.delivery_queue.Get());
            sample_memory("completed-frame",n);
        }
        if(benchmark_sequence && n%trial_frames==0) {
            xr_ok(wait_for_synthesis_handoff(state),"phase synthesis complete");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"phase presenter complete");
            xr_ok(f.transport->drain(),"phase physical FIFO complete");
            const auto phase=n/trial_frames-1,outputs=f.real_ends.load()-phase_outputs;
            const bool actual_triple=phase<6 && (!benchmark_dlssg_sequence || state->native_dlss_generated_frames>=2);
            const bool phase_fast=phase<6?phase%2!=0:true;
            const unsigned expected_rings=(actual_triple || !phase_fast)?2U:1U;
            std::fprintf(stderr,"BENCH phase%u mode%u fast%u physical%u expected3X=%u state%u director%u queue%u extra%u\n",
                phase,phase<6?phase/2:2,phase<6?phase%2:1,outputs,unsigned(actual_triple),unsigned(state->generation_status.load()),
                unsigned(state->three_slot_director),unsigned(state->queued_nvidia_presenter),unsigned(extra_output_capacity(*state)));
            xrfg::bridge_flight_logger().flush();
            auto c=find_swapchain(chain);
            std::fprintf(stderr,"CHAIN fg%u pending%u declined%u refusal%u capture%u active%u forbidden%u cooldown%u\n",
                unsigned(bool(c->frame_generation)),unsigned(c->generation_eligible_pending),unsigned(c->generation_declined),
                unsigned(c->generation_refusal),unsigned(bool(c->last_released_capture)),unsigned(state->presenter_active),
                unsigned(presenter_forbidden(*state)),unsigned(state->generation_resume_display_time!=0));
            check(c->frame_generation && c->frame_generation->current_runtime_rings==expected_rings &&
                c->frame_generation->synthetic_runtime_rings==expected_rings &&
                c->frame_generation->direct_extra_outputs==actual_triple,
                "actual SDK multiplier and Fast policy must retain distinct live output ownership");
            check(outputs>=(actual_triple?2*trial_frames+1:trial_frames+1),
                "each actual 3X/2X phase must generate beyond its real sources, including after Fast");
            if(steady_memory) {
                f.gpu_wait();if(f.delivery_queue)f.gpu_wait(f.delivery_queue.Get());
                sample_memory("completed-phase",n);
                const auto control=xrfg::autonomous::snapshot().desired;
                std::printf("VRAM_STEADY_PHASE_SUMMARY phase=%u real_inputs=%u physical_outputs=%u backend=%d frame_generation=%d pacing=%d lower_vram=%d separate=%d triple=%u actual_multiplier=%u process_peak_bytes=%llu\n",
                    phase,trial_frames,outputs,control.backend,control.frame_generation,control.pacing_mode,
                    control.lower_vram,control.separate_synthesis,unsigned(control.triple_frame_gen),actual_triple?3U:2U,memory_phase_peaks[phase]);std::fflush(stdout);
            }
        }
        if(phase_switch && n%rife_phase_frames==0) {
            xr_ok(wait_for_synthesis_handoff(state),"RIFE phase worker complete");
            if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"RIFE phase presenter drained");
            xr_ok(f.transport->drain(),"RIFE phase physical output drained");
            if(rife_luke) {
                const auto generation=find_swapchain(chain)->frame_generation;
                const bool post_afw=selected_afw_mode()!=0;
                const auto active=generation ? (post_afw ? generation->afw_temporal_synthesizer : generation->synthesizer) : nullptr;
                check(generation && generation->asynchronous_luke_rife &&
                    active && active->asynchronous_rife_enabled(),
                    "Luke reconfiguration silently replaced the actual RIFE worker with synchronous execution");
                check(active->submission_queue().Get()!=state->luke_synthesis_queue.Get() &&
                    !generation->afw_source_synthesizer->asynchronous_rife_enabled(),
                    "Luke temporal worker must own its queue while copy-only ingress stays synchronous");
                if(!post_afw)check(!generation->afw_temporal_synthesizer->asynchronous_rife_enabled(),
                    "raw Luke route retained an unused post-AFW RIFE worker");
                std::puts("LUKE_RIFE_RECONFIGURE actual asynchronous worker + independent queue + physical outputs PASS");
            }
            const auto delivered=f.real_ends.load()-rife_phase_start;
            std::fprintf(stderr,"RIFE_PHASE factor=%u pacing=%d fast=%u inputs=%u delivered=%u\n",rife_phase_factor,rife_pacing,unsigned(rife_fast),rife_phase_frames,delivered);
            if(rife_bank_switch) {
                // Three four-source epochs, each restarting history through an
                // explicit Normal/Fast switch. Judge complete output batches
                // in every epoch rather than steady-state FPS across restarts.
                check(f.real_ends.load()-rife_bank_epoch_start>=4+(rife_phase_factor-1),"last bank epoch did not deliver a complete generated batch");
                check(delivered>=rife_phase_frames+3*(rife_phase_factor-1),"bank switches lost a complete epoch of generated outputs");
            } else check(delivered>=1+(rife_phase_frames-4)*rife_phase_factor,"RIFE live phase lost its intermediate outputs");
            if(rife_bank_switch) {
                const auto g=find_swapchain(chain)->frame_generation;const auto factor=rife_frames(*state);
                const auto count=factor>2?2*(factor-1):2;
                check(g && g->synthetic_runtime_rings==count && g->synthetic_batch_width==(factor>2?factor-1:1),
                    "Normal bank allocation/stride lost after Fast or multiplier switch");
                for(unsigned i=0;i<count;++i){check(g->synthetic[i].handle!=XR_NULL_HANDLE,"missing actual bank handle");
                    for(unsigned j=0;j<i;++j)check(g->synthetic[i].handle!=g->synthetic[j].handle,"bank handles alias");}
            }
            if(panel_test) {
                const auto panels=find_swapchain(panel_chain)->frame_generation;
                check(panels && panels->synthetic_runtime_rings==source_synthetic_rings(*state,true),
                    "panel bank count did not follow live RIFE multiplier");
                check(panels->synthetic_batch_width==(rife_phase_factor>2?rife_phase_factor-1:1),
                    "panel batch stride did not follow live RIFE multiplier");
            }
        }
    }
    if(independent_stream_hold)check(stream_probe_done,"streamed native first-output dependency probe missing");
    if(independent_paired_split) {
        std::scoped_lock lock(state->presenter_mutex);
        if(std::getenv("XRFG_TEST_NVIDIA_OPTICAL") && !independent_aer4)check(state->independent_streamed_first>0,
            "paired split-source NVIDIA stream never published a first output");
    }
    if(hfw && !toggling)check(hfw_gate_proven,"HFW never promoted to actual synthesis worker");
    xr_ok(wait_for_synthesis_handoff(state),"worker complete");
    if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"generator transport drained");
    xr_ok(f.transport->drain(),"common physical FIFO drained");
    if(rife_overlap) {
        check(rife_overlap_proven,"RIFE blocked-native overlap was never exercised");
        check(f.real_ends==2+(input_count-2)*rife_factor,
            "RIFE overlap lost or duplicated a real/intermediate output after bounded backpressure");
        for(unsigned source=1;source<=input_count;++source) {
            const auto index=source<3?source-1:1+(source-2)*rife_factor;
            check(index<f.pixels.size() && f.pixels[index]==((source-1)%8+1)*20 &&
                std::abs(f.positions[index]-(float(source)-.032F))<.001F,
                "RIFE overlap replaced an older real's own pixels/pose with a later real ring publication");
        }
    }
    stop_continuous_presenter(state);
    if(layout_alternation) {
        xr_ok(f.transport->drain(),"alternating layout physical drain");
        check(f.real_ends>input_count+8,"alternating layouts never generated sustained physical outputs");
        std::array<unsigned,25> source_outputs{};
        check(f.pixels.size()==f.right_pixels.size(),"alternating layouts lost an eye");
        for(std::size_t i=0;i<f.pixels.size();++i) {
            const auto source=unsigned(std::lround(f.positions[i]+.032F));
            check(source>0 && source<=input_count,"alternating output lost its own pose/source identity");
            ++source_outputs[source];
            const int expected=source%2?40:176;
            check(std::abs(int(f.pixels[i])-expected)<=2 &&
                std::abs(int(f.right_pixels[i])-(expected+(source%2?7:0)))<=2,
                "alternating output paired pixels from different source layouts or eyes");
        }
        unsigned split_pairs{},packed_pairs{};
        for(unsigned source:{11u,12u,15u,16u})check(source_outputs[source]==1,
            "capture gap, expired metadata or reset incorrectly reused a temporal pair");
        for(unsigned source=5;source<=input_count;++source)if(source_outputs[source]>=2)
            (source%2?split_pairs:packed_pairs)++;
        check(split_pairs>=4 && packed_pairs>=4,"both recurring layouts must independently generate pairs");
        check(state->projection_streams.size()<=4,"projection metadata cache is unbounded");
        for(const auto& entry:state->projection_streams) {
            check(!projection_stream_capture_advanced(entry.snapshot,entry.snapshot),"repeated capture can be paired");
            auto altered=entry.snapshot;altered.layers.front().views.front().subImage.imageRect.offset.x++;
            check(!disjoint_projection_streams(entry.snapshot,altered) &&
                !projection_resource_layout_compatible(entry.snapshot,altered),"changed viewport bypassed the existing layout guard");
        }
        f.transport->stop();
        for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        for(auto handle:{chain,right,packed_layout})xr_ok(layer_destroy_swapchain_impl(handle),"alternating source teardown");
        check(f.chains.empty(),"alternating layouts leaked runtime sources or outputs");
        {std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}
        std::printf("Alternating layouts: split_pairs=%u packed_pairs=%u physical=%u; no quarantine/cross-source pixels/leaks PASS\n",split_pairs,packed_pairs,f.real_ends.load());
        return 0;
    }
    if(argc>1 && std::string_view(argv[1])=="rife-msfs-memory") {
        const auto status=xrfg::autonomous::snapshot();const auto& m=status.runtime.video_memory;
        std::fprintf(stderr,"RIFE_MEMORY game=%llu private=%llu reserved=%llu active=%u valid=%u stamp=%llu\n",
            m.game_bytes,m.rife_private_bytes,m.rife_reserved_bytes,unsigned(m.rife_active),unsigned(m.rife_valid),m.updated_ms);
        check(m.updated_ms && GetTickCount64()-m.updated_ms<2500 && m.rife_active && m.rife_valid,
            "visible interface must periodically request and publish real RIFE worker memory without a benchmark");
        check(m.budget_bytes && m.rife_private_bytes && m.used_bytes==m.game_bytes+m.rife_private_bytes,
            "total includes worker exactly once, not its Torch-reserved subset");
        check(xrfg::autonomous::interface_vram_text(status)!="VRAM N/A","RIFE VRAM must remain readable after warmup");
    }
    if(independent) {
        check(state->independent_pacing && state->next_source_sequence>3,"independent source jobs never entered the new queue");
        check(!state->resource_recovery.active && !state->resource_recovery.requested &&
            !state->pressure_resources_active.load() && !optical_pressure_real_only(*state),
            "legacy completion recovery interfered with Independent generation");
        check(state->three_slots.empty() && state->independent_sources.empty() && state->independent_outputs.empty(),"independent queue drain retained an old-slot dependency");
        check(state->independent_completions.empty() && !state->independent_completion_busy,
            "independent drain returned while submitted GPU transactions were still consumer-owned");
        check(!state->private_runtime->director_enabled() && state->private_runtime->buffer_capacity()==8,"old delivery policy leaked into independent consumer");
        if(independent_aer)check(state->ofr_delivery.load()==(independent_alternate?1U:2U),"AER release cadence did not reach the actual independent route");
        const auto expired=state->private_runtime->independent_load().expired;
        // Off frames3,4,6 each omit one synthetic. The one-frame enabled
        // epoch5 and the restarted epoch7 each prime again (one+two outputs
        // omitted). The remaining long epoch must actually generate again.
        const auto base_outputs=independent_alternate?input_count:
            2+(ofxr_factor?ofxr_factor:2)*(input_count-2)-(independent_offon?6:0);
        // Native D11's lazy output initialization can pass more cold real
        // frames before coherent reconstruction begins. Every original output
        // must still be accounted for, plus each genuinely paired endpoint.
        const auto direct_aer4=independent_aer4 ? state->independent_aer_direct_4x : nullptr;
        const auto stage_endpoints=independent_aer4 && state->independent_aer_double
            ?state->independent_aer_double->ordinal:0;
        const auto extra_aer4_outputs=direct_aer4 ? unsigned(direct_aer4->generated_midpoints)
            : (independent_aer4 && stage_endpoints ? unsigned(stage_endpoints-1) : 0U);
        const auto expected=base_outputs+extra_aer4_outputs;
        if(independent_aer4) {
            if(direct_aer4) {
                std::fprintf(stderr,"AER4 direct paired=%llu midpoints=%llu selected=%u mapped=%u\n",
                    direct_aer4->paired_frames,direct_aer4->generated_midpoints,
                    unsigned(xrfg::autonomous::generation_multiplier(xrfg::autonomous::snapshot().desired)),
                    unsigned(xrfg::embedded::snapshot().desired.independent_aer_double));
                check(!state->independent_aer_double,"direct AER 4X must not allocate the second-stage optical-flow helper");
                check(direct_aer4->paired_frames>2 && direct_aer4->generated_midpoints>2 &&
                    xrfg::embedded::independent_aer_double_state==2,"direct 4X AER never generated actual coherent quarter-step stereo pairs");
                std::printf("AER4 direct paired=%llu midpoints=%llu final outputs=%u expired=%llu expected=%u\n",
                    direct_aer4->paired_frames,direct_aer4->generated_midpoints,f.real_ends.load(),expired,expected);
            } else {
                const auto stage=state->independent_aer_double;
                std::fprintf(stderr,"AER4 state helper=%u hr=%08x ordinal=%llu selected=%u mapped=%u\n",unsigned(bool(stage)),stage?unsigned(stage->failure):0,stage?stage->ordinal:0,unsigned(xrfg::autonomous::generation_multiplier(xrfg::autonomous::snapshot().desired)),unsigned(xrfg::embedded::snapshot().desired.independent_aer_double));
                check(stage && SUCCEEDED(stage->failure) && stage->ordinal>2 &&
                    xrfg::embedded::independent_aer_double_state==2,"4X AER never generated actual coherent stereo midpoints");
                std::printf("AER4 accepted coherent endpoints=%llu final outputs=%u expired=%llu expected=%u\n",stage->ordinal,f.real_ends.load(),expired,expected);
            }
        }
        if(independent_offon && independent_aer)
            std::printf("FIXED_AER_PUBLICATIONS native=%u expired=%llu expected=%u\n",
                f.real_ends.load(),static_cast<unsigned long long>(expired),unsigned(expected));
        check(f.real_ends+expired==expected,"every produced output must be confirmed native or explicitly expired, never lost/faked");
        check(f.real_ends>=2,"independent route never recovered a fresh scene after warmup/stall expiration");
        if(!independent_aer) {
            check(std::is_sorted(f.positions.begin(),f.positions.end()),"deadline culling reordered source poses");
            check(f.positions.back()>input_count/2,"expired warmup backlog prevented recent sources from being displayed");
        }
        if(independent_held)check(independent_ahead,"held generator isolation was not exercised");
        if(route=="fsr" || route=="dlssg" || route=="hybrid") {
            check(state->guided_generation_ms.load()!=0,"requested vector generator must consume matching game guides, never optical fallback");
            std::puts("INDEPENDENT_GUIDES matching game-vector generation confirmed PASS");
        }
        std::puts("INDEPENDENT_QUEUE_PROOF source/jobs/drain/native-policy PASS");
        if(independent_tickets)std::puts("INDEPENDENT_TICKETS early/late next Wait and rewritten End time preserve exact begun identity; full generated count PASS");
    }
    if(reclaim_test) {
        xr_ok(wait_for_synthesis_handoff(state),"reclaimed generator restart worker complete");
        if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"reclaimed generator restart descriptors complete");
        xr_ok(f.transport->drain(),"recovered outputs physically retired");
        check(reclaimed_observed && rearmed_observed && f.real_ends.load()>rearm_native_before+8,
            "resource release did not resume sustained generation after stable raw delivery");
        check(!f.pixels.empty() && f.pixels.back()==160,"recovery lost final real source pixels");
        if(reclaim_transport || reclaim_budget) {
            check(!f.transport->clean_bypass_enabled(),"recovery failed to restore generation transport on restart");
            check(xrfg::embedded::snapshot().desired.enabled,"recovery restart rewrote saved enable preference");
        }
        stop_continuous_presenter(state);f.transport->stop();
        for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        xr_ok(layer_destroy_swapchain_impl(chain),"pressure source cleanup");
        {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
        check(f.chains.empty(),"pressure restart leaked native allocations");
        std::puts("Sustained pressure: actual resource release, raw-frame stabilization, fresh generation and pixels PASS");return 0;
    }
    if(late_shipping)check(f.real_ends.load()>late_recovery_outputs_before+9,
        "late completion/provider transition did not resume synthetic delivery beyond nine subsequent real inputs");
    auto generated=count>0 && (independent ? f.real_ends>=2 && f.real_ends+state->private_runtime->independent_load().expired>=input_count
        : f.real_ends>=(budget_fallback_test?input_count:steady_memory?input_count+4:(switching || hfw_offon)?10:12));
    std::printf("%s physical outputs=%u generation state=%u\n",route.c_str(),f.real_ends.load(),unsigned(state->generation_status.load()));
    if(!generated) {
        const auto ext=xrfg::autonomous::snapshot();const auto inner=xrfg::embedded::snapshot();const auto chain_state=find_swapchain(chain);
        std::fprintf(stderr,"CONTROL external=%d/%d advanced=%d enabled=%d rev=%llu internal=%d/%d enabled=%d rev=%llu state=%d motion=%d control=%llu luke=%d capture=%d guides=%d refused=%d pending=%d detail=%llu\n",
            ext.desired.frame_generation,ext.desired.backend,ext.desired.advanced_mode,ext.desired.enabled,ext.revision,
            inner.desired.frame_generation,inner.desired.backend,inner.desired.enabled,inner.revision,
            int(state->frame_generation_mode),state->dlss_motion_vectors,state->control_revision,state->luke_afw_enabled,
            bool(chain_state->last_released_capture),bool(chain_state->last_released_motion_vectors),int(chain_state->generation_refusal),chain_state->generation_eligible_pending,chain_state->generation_refusal_detail);
    }
    if(rife_factor){
        if(rife_luke) {
            const auto generation=find_swapchain(chain)->frame_generation;
            check(state->private_runtime->ready_clock_enabled(),"shipping RIFE route did not enable prepared native clocks");
            check(generation && generation->afw_temporal_synthesizer && generation->isolated_luke_synthesis &&
                generation->asynchronous_luke_rife,
                "RIFE Luke bypassed completed post-AFW history or isolated queue");
            const auto factor=rife_frames(*state);
            check(generation->synthetic_runtime_rings==(factor>2?factor-1:kSyntheticSlotCount) &&
                generation->direct_extra_outputs==(factor>2),"RIFE Luke intermediate destinations missing or aliased");
        }
        std::fprintf(stderr,"RIFE transport factor=%u outputs=%u state=%u private=%u slotcap=%zu worker=%u\n",
            rife_factor,f.real_ends.load(),unsigned(state->generation_status.load()),bool(state->private_runtime),state->three_slots.capacity(),state->three_slot_director);
        check(f.real_ends>=1+5*rife_factor,"RIFE full transport lost one or more intermediate/real outputs");
    }
    if(ofxr_factor){
        std::fprintf(stderr,"OFXR multiframe route=%s factor=%u outputs=%u state=%u slotcap=%zu worker=%u ready_clock=%u\n",
            route.c_str(),ofxr_factor,f.real_ends.load(),unsigned(state->generation_status.load()),state->three_slots.capacity(),
            state->three_slot_director,state->private_runtime->ready_clock_enabled());
        // The worker has stopped by now (three_slot_director is cleared): judge by the frames.
        if(optical_pressure_delivery) {
            check(f.real_ends==2+4+(input_count-6)*ofxr_factor,"pressure must retain each real, skip only pressured synthetics, re-prime then restore 4X");
            for(unsigned source=3;source<=6;++source)
            {
                const auto found=std::find_if(f.positions.begin(),f.positions.end(),[&](float x){return std::abs(x-(float(source)-.032F))<.001F;});
                check(found!=f.positions.end(),"pressure bypass lost the real source's own pose");
                check(f.pixels[std::size_t(found-f.positions.begin())]==source*20,"pressure bypass changed the real source's own pixels");
            }
            check(!XR_FAILED(state->presenter_failure),"pressure bypass latched runtime failure");
        } else if(!ofxr_switch)check(f.real_ends+(independent?state->private_runtime->independent_load().expired:0)==2+(input_count-2)*ofxr_factor,"OFXR multiplier lost, duplicated or skipped intermediate/real outputs");
    }
    check(generated,"actual generator must produce synthetic outputs beyond accepted real inputs");
    if(sole) {
        check(std::is_sorted(f.positions.begin(),f.positions.end()),"all generated and real metadata leave in source order");
        for(unsigned source=1;source<=input_count;++source)
            check(std::any_of(f.positions.begin(),f.positions.end(),[&](float x){return std::abs(x-(float(source)-.032F))<.001F;}),
                "every accepted real source reaches native End, including final drain");
        check(!f.pixels.empty() && f.pixels.back()==((input_count-1)%8+1)*20,"last real retains its own pixels through bridge/drain");
    }
    check(f.director_policies.size()==f.handoffs.size(),"native handoffs record their effective delivery policy");
    if(state->dispatch->steamvr_runtime && state->private_runtime) {
        // Native-owned pacing has distinct display slots; CPU callback entry
        // can vary with preparation after Wait. Do not reimpose the removed
        // five-microsecond CPU grid as a supposed OpenXR requirement.
        check(f.targets.size()==f.handoffs.size(),"every native image retains its display target");
        for(unsigned i=1;i<f.targets.size();++i)
            check(f.targets[i]-f.targets[i-1]>=period,"native display targets occupy distinct ordered refresh slots");
        if(f.handoffs.size()>2)
            check(f.handoffs.back()-f.handoffs.front()+period>=
                static_cast<std::int64_t>(f.handoffs.size()-1)*period,
                "throttling native runtime cannot deliver an accelerated sustained cadence");
    } else {
    for(unsigned i=1;i<f.handoffs.size();++i) {
        if(!f.director_policies[i])continue; // Framework explicitly bypasses the final cadence.
        // The deadline uses the consumer's pre-call QPC stamp; this fixture
        // samples after the indirect native call enters. Allow5us variation
        // in that sampling boundary, not a shorter scheduled display period.
        // Exact slot/period/no-catch-up policy is checked by the clock tests.
        constexpr std::int64_t native_entry_sampling_slop=5'000;
        const auto elapsed=f.handoffs[i]-f.handoffs[i-1];
        if(elapsed+native_entry_sampling_slop<period)std::fprintf(stderr,"handoff interval%lldns required%lldns (native entry sampling5us)\n",elapsed,period);
        check(elapsed+native_entry_sampling_slop>=period,"native callback cadence exceeds5us pre-call sampling uncertainty");
    }
    }
    if(rife_latency) {
        std::vector<double> latency,minimum;
        for(std::size_t i=8;i+4<measured_cycles.size() && i<rife_source_ends.size();++i) {
            bool required{};xrfg::InterfaceNativeTiming native;
            if(measured_cycles[i]->native_timing(required,native) && native.returned_ns>=rife_source_ends[i]) {
                latency.push_back(double(native.returned_ns-rife_source_ends[i])*1e-6);
                float cycle{};if(measured_cycles[i]->read_minimum_cycle(native.returned_ns,cycle))minimum.push_back(cycle);
            }
        }
        check(latency.size()>=32,"RIFE latency probe lacks same-source native delivery receipts");
        std::sort(latency.begin(),latency.end());std::sort(minimum.begin(),minimum.end());
        const auto native_outputs=f.real_ends.load();
        const auto first_native=std::min(f.handoffs.size()-1,std::size_t(8*rife_factor));
        const auto last_native=f.handoffs.size()-std::min(f.handoffs.size()-1,std::size_t(4*rife_factor))-1;
        const double native_fps=double(last_native-first_native)*1e9/double(f.handoffs[last_native]-f.handoffs[first_native]);
        const auto generation=find_swapchain(chain)->frame_generation;
        std::uint64_t synthetic_bytes{};unsigned private_images{};
        for(unsigned i=0;i<generation->synthetic_runtime_rings;++i) {
            unsigned count{};xr_ok(f.transport->enumerate_images(generation->synthetic[i].handle,0,&count,nullptr),"private VRAM count");
            std::vector<XrSwapchainImageD3D12KHR> resources(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xr_ok(f.transport->enumerate_images(generation->synthetic[i].handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(resources.data())),"private VRAM resources");
            for(auto& resource:resources){const auto desc=resource.texture->GetDesc();synthetic_bytes+=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;}
            private_images+=count;
        }
        std::printf("RIFE_RESOURCE fps=%.4f native_chains=%zu synthetic_images=%u synthetic_MiB=%.4f\n",native_fps,f.chains.size(),private_images,double(synthetic_bytes)/1048576.);
        std::printf("RIFE_LATENCY factor=%u pacing=%d fast=%u buffer=%u samples=%zu median_ms=%.4f p95_ms=%.4f min_ms=%.4f max_ms=%.4f cycle_ms=%.4f outputs=%u\n",
            rife_factor,rife_pacing,unsigned(rife_fast),unsigned(rife_buffer3?3:2),latency.size(),latency[latency.size()/2],latency[latency.size()*95/100],latency.front(),latency.back(),minimum.empty()?0:minimum[minimum.size()/2],native_outputs);
    }
    if(measured && !rife_latency) {
        // Refresh observations after physical FIFO drain. This is test-only CPU
        // time; no timing policy or sleep is added to the production director.
        std::this_thread::sleep_for(260ms);
        xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
        XrCompositionLayerProjection probe{XR_TYPE_COMPOSITION_LAYER_PROJECTION};auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&probe);
        XrFrameEndInfo sample{XR_TYPE_FRAME_END_INFO};sample.layerCount=1;sample.layers=&header;
        state->fps_overlay->application_frame(&sample);
        const auto m=xrfg::autonomous::snapshot().runtime.telemetry;
        std::fprintf(stderr,"Measured scalar check: minimum=%.9f delay=%.9f producer=%.9f cycle=%.9f ms\n",
            m.cycle_estimated_ms,m.ofxr_delay_ms,m.ofxr_time_ms,m.cycle_length_ms);
        unsigned have_native{},required_count{},readable{},minimum_ok{};
        for(const auto& c:measured_cycles) {
            bool required{};xrfg::InterfaceNativeTiming native;const auto have=c->native_timing(required,native);
            have_native+=have;required_count+=required;float a{},b{},e{};
            readable+=c->read(have?native.returned_ns:xrfg::interface_clock_ns(),a,b);
            minimum_ok+=c->read_minimum_cycle(have?native.returned_ns:xrfg::interface_clock_ns(),e);
        }
        std::fprintf(stderr,"CYCLE_DIAG total=%zu required=%u native=%u readable=%u minimum=%u active=%p pending=%zu mode=%u\n",measured_cycles.size(),required_count,have_native,readable,minimum_ok,state->interface_active_cycle.load().get(),state->pending_frames.size(),unsigned(state->pipelined_presenter_mode));
        if(rife_msfs)check(have_native>=32,"steady MSFS timing must keep native receipts beyond the first warm frames");
        check(m.cycle_valid && m.cycle_estimated_valid,"shipping generator publishes XR interval and runtime completion independently of engine origin");
        check(!m.game_time_valid,"abandoned Game timing stays absent without hiding the corrected Excel estimate");
        check(m.cycle_source==xrfg::InterfaceSource::native_frame_cycle_qpc && m.cycle_estimated_source==xrfg::InterfaceSource::native_minimum_cycle_qpc,"all shipping route measurements reach native director endpoint");
        // These aggregate medians can cover different samples: missing native
        // synthetic-period metadata excludes Minimum, but not native delay.
        // Validate the actual same-cycle bounds instead of ordering medians
        // from different cohorts (which hid behind V658's longer residence).
        unsigned native_samples{},minimum_samples{};
        for(const auto& cycle:measured_cycles) {
            bool required{};xrfg::InterfaceNativeTiming native;
            if(!cycle->native_timing(required,native))continue;
            ++native_samples;float minimum{},producer{},complete{};
            if(cycle->read_minimum_cycle(native.returned_ns,minimum)) {
                ++minimum_samples;
                check(minimum+.001F>=float(native.returned_ns-cycle->started_ns)*1e-6F,
                    "same-cycle minimum cannot omit observed runtime completion");
            }
            if(cycle->read(native.returned_ns,producer,complete))
                check(producer<=complete+.001F,"same-cycle producer interval exceeds its complete cycle");
        }
        std::fprintf(stderr,"Measured sample cohorts: native=%u minimum=%u\n",native_samples,minimum_samples);
        check(native_samples && minimum_samples,"shipping measurement lacks native same-source cycle samples");
        check(m.ofxr_time_ms<=m.cycle_length_ms+.001F,"producer and complete medians use the same valid cycle samples");
        check(m.stages_valid && m.game_time_ms>=0 && m.generation_time_ms>=0 && m.delivery_time_ms>=0,
            "all shipping frameworks expose same-source production, preparation and delivery stages");
        std::printf("Native measured cycle %.3fms, runtime completed %.3fms, producer calls %.3fms\n",m.cycle_length_ms,m.cycle_estimated_ms,m.ofxr_time_ms);
        xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,false);
    }
    if(split_eyes) {
        const bool separate_publications=separate_native_publications(*state);
        check(f.chains.size()==(separate_publications?6:4),"separate eyes retain bounded native publications per source and delivery lane");
        auto inventory=state->private_runtime->native_swapchain_mapping();
        for(unsigned retry=0;!inventory && retry<1000;++retry){std::this_thread::yield();inventory=state->private_runtime->native_swapchain_mapping();}
        check(inventory.has_value(),"native lane inventory available");
        const auto native=[&](XrSwapchain handle)->XrSwapchain{for(auto& item:*inventory)if(item.handle==handle)return item.native;return XR_NULL_HANDLE;};
        std::vector<XrSwapchain> sinks;
        for(auto source:{chain,right}) {
            auto c=find_swapchain(source);check(c->pooled_publication && c->frame_generation,"both non-Luke source output sets obey shared publication admission");
            const auto real=native(c->frame_generation->current[0].handle),generated=native(c->frame_generation->synthetic[0].handle);
            check(real && generated,"both actual generation outputs retain native publication owners");
            check(separate_publications?real!=generated:real==generated,"native stereo separates publications while AER shares each source sink");
            if(std::find(sinks.begin(),sinks.end(),real)==sinks.end())sinks.push_back(real);
            if(std::find(sinks.begin(),sinks.end(),generated)==sinks.end())sinks.push_back(generated);
            check(!state->lower_vram && !state->separate_synthesis,"pool does not activate Lower VRAM");
        }
        check(sinks.size()==(separate_publications?4:2),"native publications never merge different eye sources");
    }
    if(steady_memory) {
        auto disabled=xrfg::autonomous::snapshot().desired;disabled.enabled=false;disabled.afw_mode=0;
        check(xrfg::autonomous::request(disabled),"steady memory actual OFF request");xrfg::autonomous::apply_runtime_settings();
        ok(apply_embedded_control(state),"steady memory drained OFF provider retirement");
        f.gpu_wait();if(f.delivery_queue)f.gpu_wait(f.delivery_queue.Get());
        sample_memory("off-application-alive",input_count);
        check(memory_ledger->bytes(steamvr_vram_test::Kind::native)==memory_native_baseline &&
            !memory_ledger->bytes(steamvr_vram_test::Kind::generated) && !memory_ledger->bytes(steamvr_vram_test::Kind::interop),
            "actual generation OFF kept physical/private/interop output allocations");
        for(const auto& app:find_swapchains(state))check(!app->frame_generation,"actual generation OFF retained its provider context");
    }
    if(budget_fallback_test) {
        check(state->generation_budget_exhausted,"second UI must exercise whole-generation budget recovery");
        check(!XR_FAILED(state->presenter_failure),"budget recovery must not latch missing-generation worker failure");
        check(f.real_ends>=input_count,"every application frame must survive budget recovery");
        check(f.pixels.size()>=4 && std::equal(f.pixels.end()-4,f.pixels.end(),
            std::array<unsigned,4>{100,120,140,160}.begin()),
            "budget fallback lost, duplicated or reordered a real image after the menu opened");
        for(unsigned i=0;i<4;++i)check(std::abs(f.positions[f.positions.size()-4+i]-(float(i+5)-.032F))<.001F,
            "budget fallback changed a late real frame's own pose");
        std::puts("UEVR meter + late menu: exhausted budget keeps real frame delivery PASS");
    } else if(panel_budget_test) {
        if(panel_publication_test) {
            check(!state->panel_budget_refused,"two 4X panels must fit the source-pooled native budget");
            auto mapping=f.transport->native_swapchain_mapping();
            for(unsigned retry=0;!mapping && retry<100;++retry){std::this_thread::sleep_for(1ms);mapping=f.transport->native_swapchain_mapping();}
            check(bool(mapping),"drained panel allocation inventory available");
            std::vector<XrSwapchain> panel_sinks;
            for(auto source:{panel_chain,second_panel_chain}) {
                const auto panel=find_swapchain(source);
                check(panel && panel->panel_source && panel->pooled_publication && panel->frame_generation,
                    "each panel must retain its independent generator and source-scoped publication");
                const auto gen=panel->frame_generation;std::vector<XrSwapchain> handles;
                for(unsigned i=0;i<gen->current_runtime_rings;++i)handles.push_back(gen->current[i].handle);
                for(unsigned i=0;i<gen->synthetic_runtime_rings;++i)handles.push_back(gen->synthetic[i].handle);
                check(handles.size()==8,"4X panel must retain two real and six independent synthetic work images");
                XrSwapchain sink{};UINT64 work_bytes{};
                for(auto handle:handles) {
                    const auto found=std::find_if(mapping->begin(),mapping->end(),[&](const auto& entry){return entry.handle==handle;});
                    check(found!=mapping->end() && (!sink || sink==found->native),"one source must use one native output sink");sink=found->native;
                    XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};unsigned count{};
                    xr_ok(f.transport->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"independent panel work image");
                    check(count==1,"panel work-image count changed");const auto desc=image.texture->GetDesc();
                    work_bytes+=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
                }
                const auto& native=f.chains.at(sink);check(native.d12.size()==3,"measured panel runtime has three native images");
                UINT64 native_bytes{};for(const auto& image:native.d12){const auto desc=image->GetDesc();native_bytes+=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;}
                check(std::find(panel_sinks.begin(),panel_sinks.end(),sink)==panel_sinks.end(),"different source panels shared a sink");panel_sinks.push_back(sink);
                std::printf("PANEL_VRAM work_images=8 native_images=3 allocated_output_bytes=%llu direct_bank_equivalent_bytes=%llu\n",work_bytes+native_bytes,work_bytes+native_bytes*8);
            }
            check(panel_identity_checks>=input_count,"native panel pixel identity was not exercised");
            check(f.chains.size()<=10,"pooled menu sources exceeded the tested native handle budget");
        } else if(f.native_chain_limit>=8 && f.native_chain_limit<=14)check(!state->panel_budget_refused,
            "source-pooled panels should fit the ordinary small native budget");
        if(panel_lifecycle_test)check(find_swapchain(second_panel_chain)->frame_generation!=nullptr,
            "reopened UEVR panel failed to rearm its generation");
        check(!state->generation_budget_exhausted,"optional panels must preserve world generation budget");
        check(f.real_ends>input_count,"world real and generated frames still reach native runtime");
        std::puts("Two independent UEVR panels: budget refusal preserves world delivery PASS");
    } else if(panel_test && independent) {
        const auto panel=find_swapchain(panel_chain);
        check(panel && panel->panel_source && panel->frame_generation,"Independent did not restore its panel engine");
        if(!independent_aer)check(!state->panel_generated.empty(),"Independent never generated a corrected auxiliary image");
        check(independent_panel_checks>=4,"Independent corrected-panel native checks were not exercised");
        if(!independent_resize)check(independent_panel_fractional>0,"Independent lost moving-panel temporal correction");
        std::printf("Independent panel correction: exact source pairs/size/atlas, stationary peer, %u fractional poses PASS\n",independent_panel_fractional);
    } else if(panel_test) {
        xr_ok(wait_for_synthesis_handoff(state),"panel worker finished");
        if(continuous_presenter_active(state))xr_ok(wait_for_presenter_idle(state),"panel presenter finished");
        xr_ok(f.transport->drain(),"panel physical FIFO drained");
        const auto p=find_swapchain(panel_chain);
        check(p&&p->panel_source&&p->frame_generation,"independent panel engine armed");
        check(!state->panel_generated.empty(),"panel engine actually produced output");
        unsigned fractional{};check(!f.panel_positions.empty(),"physical frames retain both kneeboards");
        for(const auto& v:f.panel_positions){check(v[1]==5.f,"second kneeboard moved toward first");
            if(std::abs(v[0]*10-std::round(v[0]*10))>.05f)++fractional;}
        check(fractional>0,"private outputs lost interpolated panel poses");
        std::printf("Private panel atlas: %zu physical frames, %u fractional poses PASS\n",f.panel_positions.size(),fractional);
    }
    if(independent_input_timing)check(!xrfg::bridge_flight_logger().ram_enabled(),"input timestamps use ordinary disk logging, not RAM capture");
    if(rife_app_overlap)check(!state->pipelined_presenter_mode,
        "private RIFE must not enter legacy presenter mode when application frames overlap");
    if(rife_inherited)check(f.real_ends>=1+(input_count-8)*3,
        "RIFE3 acknowledgement alone is insufficient: native outputs must include the extra intermediates");
    if(aer_path)check(state->nvidia_options.ofr && xrfg::embedded::ofr_last_success_ms.load()!=0,"AER actually reached the common output path");
    f.transport->stop();
    for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
    xr_ok(layer_destroy_swapchain_impl(chain),"shipping destroy");
    if(independent_input_timing) {
        const auto path=xrfg::bridge_flight_logger().log_path();
        xrfg::bridge_flight_logger().shutdown();
        const auto read=[](const auto& p){std::ifstream f(p,std::ios::binary);return std::string{std::istreambuf_iterator<char>(f),{}};};
        const auto text=read(path);
        // The compact sidecar uses a distinct benchmark prefix.
        std::string compact;
        for(const auto& file:std::filesystem::directory_iterator(path.parent_path()))if(file.path().filename().wstring().find(L"benchmark")!=std::wstring::npos)compact+=read(file.path());
        const bool has_input=text.find("op=gpu_history result=1 ")!=std::string::npos;
        const bool has_clock=text.find("op=gpu_history result=4 ")!=std::string::npos;
        const bool has_join=text.find("op=presenter_transition result=110400 ")!=std::string::npos;
        check(has_input==!independent_input_timing_off && has_clock==!independent_input_timing_off && has_join==!independent_input_timing_off,"input GPU timestamps/clock/acquire receipts follow the optional instrumentation switch");
        if(!independent_input_timing_off)check(compact.find("op=gpu_history result=1 ")!=std::string::npos && compact.find("op=gpu_history result=4 ")!=std::string::npos,"compact F9 retains calibrated GPU input completion even after flight wrap");
        std::printf("INDEPENDENT_INPUT_TIMING enabled=%u ordinary F9 input timestamps/clock/acquire receipts; no RAM capture PASS\n",unsigned(!independent_input_timing_off));
    }
    if(panel_test)xr_ok(layer_destroy_swapchain_impl(panel_chain),"panel atlas cleanup");
    if(panel_budget_test)xr_ok(layer_destroy_swapchain_impl(second_panel_chain),"second panel cleanup");
    if(budget_meter)xr_ok(f.transport->destroy_swapchain(budget_meter),"meter budget reservation cleanup");
    for(auto meter:extra_budget_meters)xr_ok(f.transport->destroy_swapchain(meter),"additional budget reservation cleanup");
    if(split_eyes){xr_ok(layer_destroy_swapchain_impl(right),"shipping right destroy");check(f.chains.empty(),"shipping pool cleanup leaves no native handles");}
    if(steady_memory) {
        depth.Reset();motion.Reset();f.release_callback={};sample_memory("application-teardown",input_count);
        check(!memory_ledger->bytes(steamvr_vram_test::Kind::native) && !memory_ledger->bytes(steamvr_vram_test::Kind::application) &&
            !memory_ledger->bytes(steamvr_vram_test::Kind::generated) && !memory_ledger->bytes(steamvr_vram_test::Kind::interop),
            "steady generation teardown retained a measured color allocation");
        const auto features=xrfg::native_dlssg_lifetime_snapshot();
        check(!features.release_failures && !features.failed_create_handles && !features.live &&
            features.created==features.released,
            "steady generation teardown retained an NGX feature or silently failed to release it");
        std::printf("VRAM_STEADY_SUMMARY route=%s eye=%ux%u real_inputs=%u physical_outputs=%u application_baseline_bytes=%llu game_baseline_bytes=%llu process_peak_bytes=%llu generation_delta_bytes=%llu native_peak_bytes=%llu private_peak_bytes=%llu\n",
            route.c_str(),image_width,image_height,input_count,f.real_ends.load(),memory_app_baseline,memory_game_baseline,
            memory_peak,memory_peak>memory_game_baseline?memory_peak-memory_game_baseline:0,memory_native_peak,memory_private_peak);std::fflush(stdout);
    }
    ComPtr<ID3D12InfoQueue> messages;
    if(SUCCEEDED(f.device12.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T bytes{};messages->GetMessage(i,nullptr,&bytes);std::vector<char> data(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());messages->GetMessage(i,message,&bytes);if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12: %s\n",message->pDescription);check(false,"actual generator resource state/ownership validation");}}
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_swapchains.clear();g_instances.clear();}
    std::printf("%s shipping generator -> private bridge -> common consumer PASS\n",route.c_str());return 0;
}
