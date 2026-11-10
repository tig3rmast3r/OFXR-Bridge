#pragma once
#include "xrfg/autonomous_control.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/neural_rendering.hpp"
#include <mutex>
#include <cstdio>
#include <windows.h>
#include <atomic>
#include "xrfg/native_ingress.hpp"
#include "xrfg/upscaler_observation.hpp"
#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/diagnostic_hotkeys.hpp"
#include "xrfg/luke_vulkan_scope.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/d3d12_rife.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"

namespace xrfg::autonomous {
// UEVR's Alternating/AFR is attached as OSR's eye source (uevr_afr_osr.inc).
inline std::atomic_bool uevr_osr_attached{};
inline std::atomic_uint64_t applied_runtime_revision{};
inline std::atomic_bool r19_camera_enabled{};
inline void apply_toggle_shortcuts(bool log_pressed,bool nr_pressed,bool counter_pressed) {
    if(!log_pressed && !nr_pressed && !counter_pressed)return;
    const auto base = snapshot().desired;
    auto edited = base;
    if(log_pressed)edited.diagnostics=!base.diagnostics;
    if(nr_pressed)edited.nr_enabled=!base.nr_enabled;
    if(counter_pressed)edited.overlay_enabled=!base.overlay_enabled;
    request_changes(base, edited);
}
inline void poll_diagnostic_logging_key() {
    static DiagnosticKeyEdge logging,nr,counter;
    apply_toggle_shortcuts(logging.poll(KeybindAction::logging),nr.poll(KeybindAction::nr),
        counter.poll(KeybindAction::fps_counter));
}
inline void apply_runtime_settings() {
    static std::mutex mutex;
    static std::uint64_t applied{};
    std::scoped_lock lock(mutex);
    if(!initialize_game_configuration())return;
    const auto incoming=snapshot();
    if (applied==incoming.revision) return;
    const auto& s=incoming.desired;
    embedded::Settings e{};
    e.ofr=s.ofr_enabled;e.ofr_backend=s.ofr_backend;e.ofr_scale=s.ofr_scale;e.ofr_swap=s.ofr_swap;
    e.pacing_mode=s.pacing_mode;
    e.increase_buffer=s.increase_buffer;
    e.enabled=s.enabled; e.backend=s.backend; e.preset=s.preset; e.scale=s.scale;
    e.backward=s.backward; e.separate_synthesis=s.separate_synthesis; e.lower_vram=s.lower_vram;
    e.motion_vectors=s.motion_vectors; e.frame_generation=s.frame_generation>=3 ? 0 : s.frame_generation;
    e.hybrid=s.frame_generation==3;
    // RIFE's multiplier, or OFXR/Hybrid's 2X-4X; the layer tells them apart by rife_model.
    e.rife_multiplier=runtime_multiplier(s);
    set_hybrid_depth_search(s.frame_generation==3 && s.hybrid_depth_search);
    set_hybrid_hud_fix(s.frame_generation==3 && s.hybrid_hud_fix);
    e.rife_model=s.frame_generation==4?s.rife_model:-1;
    e.rife_resolution=s.rife_resolution;
    if(e.hybrid) e.backend=s.hybrid_backend; // Hybrid's own engine, independent of OFXR's.
    e.native_scale=static_cast<unsigned>(s.native_scale);
    if (!embedded::request(e)) return;
    embedded::AfwSettings a{};
    // Basic-mode sanitation resets hidden AFW controls; keep this final debug
    // guard for compatibility with older control mappings.
    a.mode=s.afw_mode; a.debug=s.afw_debug&&s.advanced_mode;
    a.clear_before_warping=s.afw_clear_before_warping;
    a.ignore_motion_threshold=s.afw_ignore_motion_threshold;
    a.depth_dilate_mode=s.afw_depth_dilate_mode; a.shading_rate=s.afw_shading_rate;
    a.osr_lower_quality=s.luke_osr_lower_quality;
    a.ofr=s.ofr_enabled;a.ofr_backend=s.ofr_backend;a.ofr_scale=s.ofr_scale;a.ofr_swap=s.ofr_swap;
    a.osr=s.luke_osr; a.osr_cover=s.luke_osr_cover; a.osr_trim=s.luke_osr_trim; a.osr_fill=s.luke_osr_fill;
    if (!embedded::request_afw(a)) return;
    if (!embedded::set_djules_triple_frame_gen(s.triple_frame_gen&&!s.ofr_enabled)) return;
    if (!embedded::set_djules_prefer_fps(s.prefer_fps || s.triple_frame_gen)) return;
    if (!embedded::set_overlay_setting(s.overlay_enabled?s.overlay_position:0)) return;
    if (!embedded::set_logging_setting(s.diagnostics)) return;
    neural_rendering::Settings n{};
    n.enabled=s.nr_enabled; n.before_sr=s.nr_before_sr; n.second_pass=s.nr_second_pass;
    n.intensity=s.nr_intensity; n.preset=s.nr_preset; n.style=s.nr_style;
    n.local_structure=s.nr_local_structure; n.local_tone=s.nr_local_tone;
    n.skin_structure=s.nr_skin_structure; n.auto_mask=s.nr_auto_mask;
    n.transfer_strength=s.nr_transfer_strength; n.colour_strength=s.nr_colour_strength;
    n.max_ratio=s.nr_max_ratio; n.working_scale=s.nr_working_scale;
    n.white_point_scale=s.nr_white_point_scale; n.diagnostic_logging=s.diagnostics;
    // Startup-only: DRED must precede the game's device creation. F9 controls
    // verbose logging independently and must not turn this crash recorder off.
    static const bool crash_debug=GetPrivateProfileIntW(L"tray",L"crash_debug",0,
        global_configuration_path().c_str())!=0;
    n.crash_debug=crash_debug;
    n.debug_view=s.nr_debug_view; n.compare=s.nr_compare;
    n.compare_split=s.nr_compare_split; n.compare_zoom=s.nr_compare_zoom;
    n.compare_swap=s.nr_compare_swap;
    neural_rendering::configure(n);
    r19_camera_enabled.store(s.r19_camera,std::memory_order_release);
    applied=incoming.revision;
    applied_runtime_revision.store(applied,std::memory_order_release);
}
inline void publish_runtime_status(unsigned native_generated_frames=0,
    GenerationState generation=GenerationState::waiting, HRESULT initialization_result=S_OK,
    bool vulkan_detected=false, std::uint32_t provider_error=0,
    const SwapchainBudgetStatus* swapchains=nullptr,int pacing_mode=-1,bool steamvr=false,bool afw_only=false,
    bool luke_shared=false,bool luke_shared_forced=false,bool vdxr=false,bool legacy_vdxr=true,unsigned eye_long_side=0,bool shared_queue=false,unsigned buffer_capacity=0,unsigned rife_max_multiplier=2,std::uint64_t rife_after_ms=0,std::uint64_t consumer_guides_ms=0) {
    const auto control=embedded::snapshot();
    const auto afw=embedded::afw_snapshot();
    const auto nr=neural_rendering::status();
    RuntimeStatus out{};out.rife_max_multiplier=rife_max_multiplier;
    out.pacing_mode=afw_only?kAfwOnlyPacingMode:pacing_mode;out.steamvr_runtime=steamvr;
    out.vdxr_runtime=vdxr;out.vdxr_legacy=legacy_vdxr;
    out.buffer_capacity=buffer_capacity;
    out.eye_long_side=eye_long_side;
    out.shared_queue=shared_queue;
    out.luke_shared_swapchains=luke_shared;out.luke_shared_swapchains_forced=luke_shared_forced;
    out.dlssg_preset_request=xrfg::standalone::process_dlssg_preset();
    if (swapchains) out.swapchains=*swapchains;
    out.process_id=GetCurrentProcessId();
    out.osr_mode=afw.active_osr_mode;out.osr_lower_quality=afw.active_osr_lower_quality;
    out.afw_shading_rate=afw.active_shading_rate;
    out.applied_revision=applied_runtime_revision.load(std::memory_order_acquire);
    out.sessions=control.sessions; out.ready=control.ready;
    out.pending=control.pending; out.errors=control.errors;
    out.afw_projection_frames=afw.projection_frames; out.afw_ready_frames=afw.ready_frames;
    const auto now=GetTickCount64();
    out.afw_flat_scene=afw.flat_scene;
    if(const auto evidence=std::atomic_load(&embedded::ofr_evidence))out.ofr_evidence=evidence->snapshot();
    const auto ofr_last=out.ofr_evidence.last_output_ms;
    out.ofr_running=afw.desired.ofr && control.desired.enabled && ofr_last && now>=ofr_last && now-ofr_last<1000 &&
        ofr_last>=out.ofr_evidence.last_bypass_ms;
    out.ofr_delivery=embedded::ofr_delivery.load(std::memory_order_relaxed);
    if(afw.desired.ofr && control.desired.enabled && bridge_flight_logger().enabled()) {
        static std::atomic_uint64_t next_aer_report{};auto next=next_aer_report.load();
        if(now>=next && next_aer_report.compare_exchange_strong(next,now+1000))
            bridge_flight_logger().event(BridgeFlightOperation::presenter_transition,9231,
                out.ofr_evidence.outputs,out.ofr_evidence.bypasses,
                (out.ofr_evidence.unknown<<2)|out.ofr_evidence.delivery);
    }
    out.afw_running=!(afw.desired.ofr&&control.desired.enabled) && !afw.flat_scene && afw.desired.mode!=0 && afw.last_success_ms &&
        now>=afw.last_success_ms && now-afw.last_success_ms<2000;
    out.nr_running=nr.running;
    out.nr_model_missing=nr.enabled && nr.model_missing;
    out.logging_active=bridge_flight_logger().enabled();
    out.vulkan_capture_suspended=vulkan_detected && !native_vulkan::luke_vulkan_process();
    out.nr_evaluations=nr.evaluations;
    if (nr.enabled && !nr.reason.empty()) snprintf(out.message,sizeof(out.message),"%s",nr.reason.c_str());
    const auto ingress=native_ingress::statistics();
    if(ingress.luke&&(bridge_flight_logger().enabled()||bridge_flight_logger().ram_enabled())) {
        static std::atomic_uint64_t last_camera_report{};auto previous=last_camera_report.load(std::memory_order_relaxed);
        if(now>=previous+1000&&last_camera_report.compare_exchange_strong(previous,now,std::memory_order_relaxed))
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
                ingress.camera_rejected&&!ingress.camera_accepted?1:0,1347,ingress.camera_accepted,ingress.camera_rejected);
    }
    out.luke_detected = ingress.luke != 0;
    out.uevr_osr = uevr_osr_attached.load(std::memory_order_acquire);
    out.vulkan_detected = vulkan_detected || (ingress.luke && native_vulkan::luke_vulkan_process());
    out.provider_error = control.desired.frame_generation == 2 ? provider_error : 0;
    if (ingress.luke && ingress.camera_rejected && !ingress.camera_accepted)
        snprintf(out.message,sizeof(out.message),"Luke: awaiting an exact native camera packet");
    const auto desired=snapshot().desired;
    if(desired.enabled && desired.frame_generation==4 && !afw_only) {
        const auto rife=xrfg::rife_execution_status();
        out.rife_execution_active=rife.active && rife.updated_ms>=rife_after_ms;
        out.rife_optimizing=rife.active && rife.optimizing;
        out.rife_tensorrt=rife.active && rife.tensorrt;
        out.rife_vulkan=rife.active && rife.vulkan;
        out.rife_error=rife.active?rife.error:0U;
    }
    const auto observed=native_ingress::upscaler_observation();
    const auto guides=dlss_motion_vector_statistics();
    out.upscaler_evaluations=observed.evaluations;
    out.upscaler_motion_inputs=observed.motion_inputs; out.upscaler_depth_inputs=observed.depth_inputs;
    out.upscaler_last_seen_ms=observed.last_seen_ms;
    out.upscaler_motion=observed.motion; out.upscaler_depth=observed.depth;
    out.guide_publications=guides.published; out.guide_matches=guides.matched; out.guide_uses=guides.used;
    out.generation_mode=desired.frame_generation; out.motion_source=control.desired.motion_vectors;
    const bool post_afw = out.luke_detected && afw.desired.mode != 0;
    if (post_afw && (out.generation_mode == 0 || out.generation_mode == 3))
        out.motion_source = 0; // Complete post-AFW color pair, no native peer-eye guides.
    if (!control.desired.enabled) generation=GenerationState::paused;
    const bool consumer_guides=consumer_guides_ms && now>=consumer_guides_ms && now-consumer_guides_ms<2000;
    if ((generation==GenerationState::ready || generation==GenerationState::waiting) &&
        (out.motion_source==1 || out.generation_mode==1 || out.generation_mode==2) &&
        !(generation==GenerationState::ready && (out.generation_mode==2 || consumer_guides))) {
        switch(guides.status) {
        case DlssMotionVectorStatus::waiting_for_dlss: generation=GenerationState::missing_motion; break;
        case DlssMotionVectorStatus::waiting_for_depth: generation=GenerationState::missing_depth; break;
        case DlssMotionVectorStatus::invalid_input: generation=GenerationState::invalid_guides; break;
        case DlssMotionVectorStatus::temporal_mismatch: generation=GenerationState::temporal_mismatch; break;
        case DlssMotionVectorStatus::output_not_direct: generation=GenerationState::output_mismatch; break;
        case DlssMotionVectorStatus::queue_mismatch: generation=GenerationState::queue_mismatch; break;
        case DlssMotionVectorStatus::native_unavailable: if(out.generation_mode==2) generation=GenerationState::unavailable; break;
        case DlssMotionVectorStatus::native_evaluate_failed: if(out.generation_mode==2) generation=GenerationState::dispatch_failed; break;
        case DlssMotionVectorStatus::native_resizing: if(out.generation_mode==2) generation=GenerationState::recovering; break;
        case DlssMotionVectorStatus::multi_frame_unsupported: generation=GenerationState::multi_frame_unsupported; break;
        default: break;
        }
        if(out.generation_mode==1) switch(fsr_frame_generation_statistics().status) {
        case FsrFrameGenerationStatus::runtime_missing: generation=GenerationState::runtime_missing; break;
        case FsrFrameGenerationStatus::initialization_failed: generation=GenerationState::initialization_failed; break;
        case FsrFrameGenerationStatus::dispatch_failed: generation=GenerationState::dispatch_failed; break;
        case FsrFrameGenerationStatus::waiting_for_guides: generation=GenerationState::temporal_mismatch; break;
        case FsrFrameGenerationStatus::stereo_unavailable: generation=GenerationState::invalid_stereo; break;
        case FsrFrameGenerationStatus::stereo_recovering: generation=GenerationState::recovering; break;
        default: break;
        }
    }
    if (out.generation_mode==2 && native_generated_frames==0 &&
        generation!=GenerationState::missing_motion && generation!=GenerationState::missing_depth &&
        generation!=GenerationState::paused && generation!=GenerationState::invalid_stereo)
        generation=GenerationState::unavailable;
    if (generation==GenerationState::ready && out.generation_mode==2 && desired.triple_frame_gen && native_generated_frames<2)
        generation=GenerationState::multi_frame_unsupported;
    if (control.errors) generation=GenerationState::initialization_failed;
    const HRESULT failure=control.errors ? static_cast<HRESULT>(control.last_error) : initialization_result;
    if (control.desired.enabled && FAILED(failure)) {
        generation=out.generation_mode==2 && failure==DXGI_ERROR_UNSUPPORTED
            ? GenerationState::unavailable : GenerationState::initialization_failed;
        snprintf(out.message,sizeof(out.message),"%s initialization failed (0x%08lX). Real-frame bypass; select another generator to retry.",
            out.generation_mode==2 ? "DLSS-G" : out.generation_mode==1 ? "FSR" : "OFXR",static_cast<unsigned long>(failure));
    }
    if(afw_only) {
        generation=GenerationState::paused;out.generation_mode=0;out.motion_source=0;out.provider_error=0;
        char detail[sizeof(out.message)]{};std::copy(std::begin(out.message),std::end(out.message),std::begin(detail));
        snprintf(out.message,sizeof(out.message),"Frame Reconstruction only: direct OpenXR, no frame generation.%s%.120s",detail[0]?" ":"",detail);
    }
    out.generation_state=generation;
    report_runtime(out);
}
}
