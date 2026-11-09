// Shipping API/worker/generator code, with only the downstream runtime mocked.
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include <fstream>
#include "../src/layer/openxr_layer.cpp"
#define main private_transport_core_main
#include "private_openxr_runtime_tests.cpp"
#undef main
#include "autonomous_layer_fixture.hpp"
#include <d3d12sdklayers.h>

#include "afw_only_session_test.inc"
#include "steamvr_vram_budget_test.inc"
#include "auxiliary_slot_selection_test.inc"

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

int main(int argc,char** argv) {
    std::string route=argc>1?argv[1]:"new";
    if(route.starts_with("aer-alternate-"))return aer_alternate_delivery_test(route);
    const bool aer_path=route.starts_with("aer-");if(aer_path)route.erase(0,4);
    const bool aer_swap=aer_path&&route.starts_with("swap-");if(aer_swap)route.erase(0,5);
    if(route=="native-guide-cache")return aer_native_guide_cache_test();
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
            check(terminal.new_content==!held && terminal.interface_stamp.reconstruction_reference==!held &&
                snapshot.interface_stamp.reconstruction_reference==!held,
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
    if(route=="auxiliary-slot-available")return auxiliary_slot_selection_test(0);
    if(route=="auxiliary-slot-lazy")return auxiliary_slot_selection_test(1);
    if(route=="auxiliary-slot-all-busy")return auxiliary_slot_selection_test(2);
    if(route=="uevr-auxiliary-pooled-budget")return auxiliary_slot_selection_test(3);
    if(route=="overlay-admission" || route=="overlay-admission-measured")
        return overlay_admission_test(route.ends_with("measured"));
    if(route=="admission-credit")return admission_credit_test();
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
    if(route.starts_with("afw-only") || route=="luke-shared-session")return afw_only_session_test(route,argc>2?argv[2]:nullptr);
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
    const bool split_eyes=pooled || unpooled || rife_msfs || layout_alternation;
    const bool separate_queue=route.starts_with("separate-");if(separate_queue)route=route.substr(9);
    int a{},b{};if(route.size()>3 && route[route.size()-3]=='-'){a=route[route.size()-2]-'0';b=route.back()-'0';route.resize(route.size()-3);}
    const bool switching=route=="switch";if(switching)route="new";
    const bool luke=route.starts_with("luke-");if(luke)route=route.substr(5);
    const bool native12=!route.starts_with("d11-");if(!native12)route=route.substr(4);
    check(!steady_memory || native12,"steady VRAM mode currently uses the native D12 route fixtures");
    const bool triple=route.ends_with('3');if(triple)route.pop_back();
    xrfg_fixture::ControlProfile profile;
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
    if(rife_report || ((benchmark_sequence || rife_factor) && GetEnvironmentVariableW(L"XRFG_BENCH_TRACE",nullptr,0))) {
        const auto folder=xrfg::autonomous::configuration_path().parent_path()/L"trace";
        std::filesystem::create_directories(folder);
        WritePrivateProfileStringW(L"diagnostics",L"logging_enabled",L"1",(folder/L"ofxr_bridge.ini").c_str());
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
    Fixture f(native12);f.transport->stop();if(route!="memory" && route!="scope" && !framework_session_probe)f.transport.reset();f.throttled=false;f.real_common=true;f.expected_views=2;f.unblock();
    if(rife_latency)f.frame_wait_us=[](unsigned){return 11111;};
    if(rife_factor)f.prediction_horizon_ns=period*3; // cold-model/GPU-load fixture, unchanged physical refresh

    f.create_state=[] {std::scoped_lock lock(g_state_mutex);return g_swapchains.empty()?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COMMON;};
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=!vdxr_probe && !unknown_session && !pimax_session;dispatch->virtual_desktop_runtime=vdxr_probe;
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
    if(!native12)ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),f.device12.GetAddressOf(),f.queue.GetAddressOf()),"D11 generator interop device");
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->d3d11_device=f.device11;state->d3d11_context=f.context;
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
        auto desired=snapshot().desired;desired.benchmark_csv=true;desired.diagnostics=false;
        check(request(desired),"isolated memory test settings");
        RuntimeStatus runtime{};runtime.sessions=1;runtime.applied_revision=snapshot().revision;report_runtime(runtime);
        check(start_benchmark(false),"start isolated memory trial");
        runtime.applied_revision=snapshot().revision;report_runtime(runtime);
        const auto clock=GetTickCount64();tick_benchmark(clock);
        check(snapshot().benchmark.phase==BenchmarkPhase::running,"memory trial applied");
        tick_benchmark(clock+kBenchmarkMemoryRequestMs);
        sample_benchmark_memory(*state);
        check(!take_benchmark_memory_request(),"game sampler consumes one request only");
        check(state->video_memory_adapter!=nullptr,"sampler finds the actual rendering adapter with F9 off");
        DXGI_QUERY_VIDEO_MEMORY_INFO expected{};
        ok(state->video_memory_adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&expected),"independent process VRAM query");
        const auto path=std::filesystem::u8path(snapshot().benchmark.csv_path);
        tick_benchmark(clock+kBenchmarkDurationMs);
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

    if(ofxr_factor)state->nvidia_options.rife_multiplier=int(ofxr_factor);
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
    if(sole && !rife_factor)ok(apply_embedded_control(state),"initial settings match the real public route selection");
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
        if(!(rife_luke && rife_switch && !rife_pacing))
            check(fresh_luke_director_worker(*state) && !presenter_forbidden(*state),"HFW source filter incorrectly forces inline synthesis");
        state->single_threaded_d3d11=true;check(presenter_forbidden(*state),"single-threaded guard lost");state->single_threaded_d3d11=false;
        state->split_frame_loop=true;check(presenter_forbidden(*state),"split-loop guard lost");state->split_frame_loop=false;
        state->afw_only=true;check(presenter_forbidden(*state),"AFW-only enters OFXR worker");state->afw_only=false;
        state->dispatch->steamvr_runtime=false;if(!rife_luke)check(presenter_forbidden(*state),"VDXR pacing changed");state->dispatch->steamvr_runtime=true;
    }
    f.transport=state->private_runtime;state->uevr_application=pooled;
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
        auto saved=f.create_state;f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
        xr_ok(layer_create_swapchain_impl(state->handle,&info,&right),"separate right-eye application chain");
        f.create_state=std::move(saved);
        for(auto& image:right_images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        if(native12)xr_ok(layer_enumerate_swapchain_images_impl(right,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(right_images.data())),"right-eye capture setup");
        else {std::array<XrSwapchainImageD3D11KHR,3> ri;for(auto& i:ri)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(right,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(ri.data())),"right-eye D11 capture setup");}
    }
    XrSwapchain packed_layout{};
    if(layout_alternation) {
        auto packed_info=info;packed_info.width=image_width*2;packed_info.arraySize=1;
        auto saved=f.create_state;f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
        xr_ok(layer_create_swapchain_impl(state->handle,&packed_info,&packed_layout),"packed projection source");
        f.create_state=std::move(saved);
        if(native12){std::array<XrSwapchainImageD3D12KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(packed_layout,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"packed D12 capture setup");}
        else {std::array<XrSwapchainImageD3D11KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(packed_layout,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"packed D11 capture setup");}
    }
    XrSwapchain panel_chain{},second_panel_chain{},budget_meter{};
    if(panel_test) {
        const auto saved=f.create_state;f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
        auto ci=info;ci.arraySize=1;
        xr_ok(layer_create_swapchain_impl(state->handle,&ci,&panel_chain),"independent panel atlas");
        f.create_state=saved;
        std::array<XrSwapchainImageD3D12KHR,3> pi;for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        unsigned pc{};xr_ok(layer_enumerate_swapchain_images_impl(panel_chain,3,&pc,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"panel atlas capture setup");
        if(panel_budget_test) {
            const auto saved=f.create_state;f.create_state=[]{return D3D12_RESOURCE_STATE_RENDER_TARGET;};
            xr_ok(layer_create_swapchain_impl(state->handle,&ci,&second_panel_chain),"independent second UEVR panel");
            f.create_state=saved;
            for(auto& i:pi)i={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(second_panel_chain,3,&pc,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(pi.data())),"second panel capture setup");
            f.native_chain_limit=argc>2?std::stoul(argv[2]):budget_fallback_test?6:panel_lifecycle_test?32:10;
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
    f.release_callback=[&](XrSwapchain handle,unsigned index){
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
    const unsigned input_count=layout_alternation?24:rife_latency?64:phase_switch?5*rife_phase_frames:benchmark_sequence?benchmark_policy_phases*trial_frames:steady_memory?16:rife_msfs?64:panel_lifecycle_test?16:8;
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
    bool hfw_gate_proven=false;
    bool rife_overlap_proven=false;
    std::uint64_t rife_overlap_b_sequence{};
    ComPtr<ID3D12Fence> rife_overlap_compute_gate;
    XrFrameState overlap_prediction{XR_TYPE_FRAME_STATE};bool overlap_wait_ready=false;
    for(unsigned n=1;n<=input_count;++n) {
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
        } else if(!rife_msfs && !(rife_early_capture_done && n==6) && !(layout_alternation && n%2==0))f.write(chain,color);
        if(split_eyes && !rife_msfs && !(layout_alternation && n%2==0))f.write(right,color+7);
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
        xr_ok(rife_msfs?layer_begin_frame(state->handle,nullptr):layer_begin_frame_impl(state->handle,nullptr),"shipping virtual Begin");
        if(measured)if(auto cycle=state->interface_active_cycle.load())measured_cycles.push_back(std::move(cycle));
        if(rife_msfs) {
            if(next_wait.valid()){overlap_prediction=next_wait.get();overlap_wait_ready=true;}
            state->nvidia_options.rife_model=saved_rife_model;
            if(rife_inherited && n==3)check(state->pipelined_presenter_mode,"inherited optical overlap must exercise the production legacy promotion");
            f.write(chain,color);f.write(right,color+7);
        }
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye){auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.x=(sole?float(n):0.F)+(eye?.032F:-.032F);v.pose.position.y=float(n);v.fov={-.7F,.7F,.7F,-.7F};v.subImage.swapchain=split_eyes && eye?right:chain;v.subImage.imageArrayIndex=split_eyes?0:eye;v.subImage.imageRect.extent={int(image_width),int(image_height)};}
        if(layout_alternation && n%2==0)for(unsigned eye=0;eye<2;++eye){views[eye].subImage.swapchain=packed_layout;views[eye].subImage.imageArrayIndex=0;views[eye].subImage.imageRect.offset.x=int(eye*image_width);}
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        std::array<XrCompositionLayerQuad,2> panel_quads;
        std::array<const XrCompositionLayerBaseHeader*,3> panel_layers{layer};
        if(panel_test) {
            for(unsigned i=0;i<2;++i){auto& q=panel_quads[i];q={XR_TYPE_COMPOSITION_LAYER_QUAD};
                q.space=projection.space;q.pose.orientation.w=1;q.pose.position.x=i?5.f:.1f*n;
                q.size={1,1};q.subImage.swapchain=panel_budget_test&&i?second_panel_chain:panel_chain;q.subImage.imageRect={{int(i*image_width/2),0},{int(image_width/2),int(image_height)}};
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
        if(hfw) std::fprintf(stderr,"HFW before%u active%u worker%u request%u forbidden%u mode%u enabled%u\n",n,
            unsigned(continuous_presenter_active(state)),unsigned(state->three_slot_director),unsigned(state->presenter_start_requested),
            unsigned(presenter_forbidden(*state)),unsigned(state->single_director_requested.load()),unsigned(state->menu_enabled.load()));
        if(hfw && !toggling && !hfw_gate_proven && n>=4 && state->three_slot_director && continuous_presenter_active(state)) {
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
        } else if(blocked && n==2) {
            f.wait_count(1); // all three endpoint credits are free before the probe
            f.block_end=true;f.end_entered=false;
            auto producer=std::async(std::launch::async,[&]{return layer_end_frame_impl(state->handle,&end);});
            const bool independent=producer.wait_for(500ms)==std::future_status::ready;
            f.unblock();
            xr_ok(producer.get(),"shipping End while native End is withheld");
            check(independent,"bridge acceptance returns to game before blocked native End with spare credits");
        } else {
            const auto submitted=rife_msfs?layer_end_frame(state->handle,&end):layer_end_frame_impl(state->handle,&end);
            if(rife_inherited && n==5) {
                const auto ack=xrfg::embedded::snapshot();
                std::fprintf(stderr,"RIFE3_ACK pending=%u errors=%u active_factor=%u legacy=%u\n",ack.pending,ack.errors,rife_frames(*state),unsigned(state->pipelined_presenter_mode));
                check(!ack.pending && !ack.errors && rife_frames(*state)==3,"RIFE3 must finish Applying settings and activate the requested multiplier");
            }
            xr_ok(submitted,"shipping OFXR End handoff");
            if(budget_fallback_test && n==4)budget_meter=f.make_chain(false,1);
            if(panel_lifecycle_test && n==5) {
                check(!find_swapchain(second_panel_chain)->frame_generation,
                    "closed UEVR panel retained optional native generation rings");
                check(find_swapchain(panel_chain)->frame_generation!=nullptr,
                    "closing one panel retired the other visible panel");
            }
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
                check(generation && generation->asynchronous_luke_rife &&
                    generation->afw_temporal_synthesizer->asynchronous_rife_enabled(),
                    "Luke reconfiguration silently replaced the actual RIFE worker with synchronous execution");
                check(generation->afw_temporal_synthesizer->submission_queue().Get()!=state->luke_synthesis_queue.Get() &&
                    !generation->afw_source_synthesizer->asynchronous_rife_enabled(),
                    "Luke temporal worker must own its queue while copy-only ingress stays synchronous");
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
    auto generated=count>0 && f.real_ends>=(budget_fallback_test?input_count:steady_memory?input_count+4:(switching || hfw_offon)?10:12);
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
        if(!ofxr_switch)check(f.real_ends==2+(input_count-2)*ofxr_factor,"OFXR multiplier lost, duplicated or skipped intermediate/real outputs");
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
        check(f.chains.size()==4,"ordinary and UEVR separate eyes share one physical publication sink per source");
        for(auto source:{chain,right}) {
            auto c=find_swapchain(source);check(c->pooled_publication && c->frame_generation,"both non-Luke source output sets obey shared publication admission");
            check(!state->lower_vram && !state->separate_synthesis,"pool does not activate Lower VRAM");
        }
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
        if(f.native_chain_limit<=14)check(state->panel_budget_refused,
            "native panel allocation refusal must disable optional panel generation");
        if(panel_lifecycle_test)check(find_swapchain(second_panel_chain)->frame_generation!=nullptr,
            "reopened UEVR panel failed to rearm its generation");
        check(!state->generation_budget_exhausted,"optional panels must preserve world generation budget");
        check(f.real_ends>input_count,"world real and generated frames still reach native runtime");
        std::puts("Two independent UEVR panels: budget refusal preserves world delivery PASS");
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
    if(rife_app_overlap)check(!state->pipelined_presenter_mode,
        "private RIFE must not enter legacy presenter mode when application frames overlap");
    if(rife_inherited)check(f.real_ends>=1+(input_count-8)*3,
        "RIFE3 acknowledgement alone is insufficient: native outputs must include the extra intermediates");
    if(aer_path)check(state->nvidia_options.ofr && xrfg::embedded::ofr_last_success_ms.load()!=0,"AER actually reached the common output path");
    f.transport->stop();
    for(auto& c:find_swapchains(state)){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
    xr_ok(layer_destroy_swapchain_impl(chain),"shipping destroy");
    if(panel_test)xr_ok(layer_destroy_swapchain_impl(panel_chain),"panel atlas cleanup");
    if(panel_budget_test)xr_ok(layer_destroy_swapchain_impl(second_panel_chain),"second panel cleanup");
    if(budget_meter)xr_ok(f.transport->destroy_swapchain(budget_meter),"meter budget reservation cleanup");
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
