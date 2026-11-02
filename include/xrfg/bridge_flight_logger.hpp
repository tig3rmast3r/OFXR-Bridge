#pragma once

#include <atomic>
#include <array>
#include <mutex>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace xrfg {
class RamTimingCapture;
inline std::filesystem::path benchmark_log_path(const std::filesystem::path& flight) {
    constexpr std::wstring_view prefix = L"ofxr-bridge-flight-";
    const auto stem = flight.stem().wstring();
    return flight.parent_path() / (L"ofxr-benchmark-" + stem.substr(prefix.size()) + L".benchmark.log");
}

enum class BridgeFlightOperation : std::uint32_t {
    rife_worker = 10000, rife_memory,
    // F9-only native RIFE phases: a=configuration id, b=width<<32|height,
    // c=eyes<<32|fraction float bits. Span result is the actual HRESULT.
    rife_pack_execute = 10002,
    rife_pack_wait = 10003,
    rife_infer = 10004,
    rife_compose_execute = 10005,
    rife_compose_wait = 10006,
    // Same a/b/c; result bit0=TensorRT, bit1=optimizing, bit2=valid receipt.
    rife_backend = 10007,
    logger = 0,
    negotiation,
    instance_create,
    instance_destroy,
    session_create,
    session_destroy,
    session_begin,
    session_end,
    application_wait_frame,
    application_begin_frame,
    application_end_frame,
    application_swapchain_acquire,
    application_swapchain_wait,
    application_swapchain_release,
    private_swapchain_acquire,
    private_swapchain_wait,
    private_swapchain_release,
    synthesis_initialize,
    synthesis_prime,
    synthesis_pair,
    downstream_first_end_frame,
    internal_wait_frame,
    internal_begin_frame,
    internal_end_frame,
    continuity_reset,
    gpu_drain,
    swapchain_create,
    swapchain_eligibility,
    projection_mapping,
    generation_prepare,
    session_binding,
    swapchain_image,
    d3d11_capture,
    d3d11_publish,
    runtime_identity,
    presenter_submission,
    // Correlates a presenter request with the original application display
    // time: a=request sequence, b=source XrTime, c=pair sequence (zero repeat).
    presenter_source,
    // Only when the application enabled the Win32 time conversion extension:
    // a=predicted XrTime, b=the same instant on this logger's microsecond
    // timeline, c=QPC frequency. Repeated occasionally to detect drift.
    presenter_clock_anchor,
    // result=1 if the app enabled the extension and the runtime exposed its
    // function, a=1 if the app requested it, b=1 if loading succeeded.
    presenter_clock_capability,
    presenter_queue_drop,
    presenter_transition,
    nvidia_gpu_stages,
    nvidia_gpu_total,
    // How long the presenter held itself back before starting a runtime frame
    // cycle, and how far it was from its own schedule when it did. Without
    // this the pace and the loop's other costs are indistinguishable inside
    // one gap between records.
    presenter_pace,
    synthesis_frame_start_wait,
    embedded_configuration,
    // When the GPU actually began and finished a pair's synthesis, on the
    // same timeline as every other record. Duration alone cannot say whether
    // the pixels existed when the presenter handed the frame over; only the
    // finish time against that submission can.
    synthesis_gpu_span,
    // The application asked for more frames than the presenter produced and
    // the virtual clock was held to the runtime's timeline instead of
    // stepping past it. a is how far the step overshot, in nanoseconds.
    virtual_clock_clamp,
    // A runtime session state transition, exactly as the application receives
    // it. result is the new XrSessionState; a is the session, b the event's
    // own time. A runtime that stops asking for frames says so here and
    // nowhere else the layer can see.
    session_state,
    // The once-per-pair hold releasing the application. result is the serial
    // surplus the release discarded - the presenter had run that many frames
    // beyond the two this hold is for, so the application had already missed
    // its slot and the hold throttled nothing. a is how long it waited in
    // microseconds, b the presenter's frame serial, c the serial the
    // application had been served to. Zero surplus is the hold working; a
    // surplus is the application running free, gated only by whatever it
    // blocks on next.
    presenter_pair_release,
    // Adaptive NVIDIA cadence: result=1 when a synthetic slot is budgeted;
    // a=application-end interval in ns, b=display period in ns,
    // c=accumulated synthetic credit in thousandths before consumption.
    // How hard the runtime-entry gate is actually working on a D3D11 session.
    // result is the number of entries in this window that had to wait, a the
    // mean wait and b the longest, both nanoseconds, c the longest any single
    // entry held the gate. Zero contention across a session means the two
    // threads were never inside the runtime together, so the gate is not what
    // is protecting it - the earlier device-section attempt reported exactly
    // that and it was read as a clean result rather than as a refutation.
    runtime_entry_section,
    // result is the HRESULT returned by history->capture; a is the
    // application swapchain, b the image index, c the captured slot on
    // success or UINT32_MAX on failure.
    history_capture,
    // Three-slot route: waits bracket the two producer handoffs. Handoff and
    // ready carry a=current source id, b=previous id. Present carries the same
    // identity, result=1 synthetic/0 real, c=runtime predicted display time.
    three_slot_worker_wait,
    three_slot_buffer_wait,
    three_slot_handoff,
    three_slot_gpu_ready,
    three_slot_present,
    // result=layer type, a=layer index, b=source swapchain,
    // c=0 supported, 1 unsupported structure, 2 missing release snapshot.
    composition_layer,
    // result=HRESULT, a=source, b=owned output, c=publication fence value.
    composition_publish,
    timing_worker_run,
    timing_slot_wait,
    timing_director_wait,
    timing_worker_publish,
    timing_runtime_gate,
    timing_worker_prepare,
    timing_synthesis_complete,
    timing_publication_complete,
    timing_aux_complete,
    timing_worker_build,
    gpu_history,
    gpu_synthesis,
    gpu_timing_error,
    luke_release,
    luke_filter,
    luke_cycle_wait,
    // Startup-only: a=session, b=requested separate bit0/actual bit1,
    // c=initial backend low8, generator next8, Hybrid bit16, enabled bit17.
    steamvr_queue_configuration,
    // Embedded-only diagnostics appended to preserve V223 operation IDs.
    fsr_eye_binding,
    fsr_eye_resources,
    fsr_eye_rects,
    fsr_eye_motion,
    fsr_eye_pose,
    fsr_eye_orientation,
    fsr_eye_fov,
    fsr_eye_clip_vertex,
    fsr_hudless_pair,
    fsr_context_lifecycle,
    fsr_submit_stage,
    fsr_recovery,
    // Luke/RealVR diagnostic-only contracts. These records connect the
    // post-ReShade colour capture to its physical OpenXR view, the DLSS guide
    // consumed with it, and the private swapchain eventually submitted.
    luke_guide_contract,
    luke_projection_contract,
    luke_output_contract,
    // V23286: immutable application frame serial through both runtime slots.
    luke_frame_contract,
    luke_pose_contract,
    // V23162: breadcrumb each PureDark AFW recording/submission boundary so a
    // process-ending plugin or driver fault retains the last completed stage.
    luke_afw_stage,
    gpu_fence_snapshot,
    gpu_device_status,
    // V30013: append-only RAM operation IDs; no image/pose scheduling changes.
    luke_inline_clock,       // result=XrResult, a=frame, b=XrTime, c=QPC (0 on failure)
    luke_physical_submit,    // result=boundary*100 or boundary*100+1(return), a=frame,b=time,c=result/count
    luke_publication_fence,  // result=boundary*100+resource*10+field(0 ready/1 copy target/2 completed)
    luke_publication_images, // a=frame,b=application swapchain,c=currentIndex<<32|syntheticIndex
    luke_publication_slots,  // a=frame,b=current private swapchain,c=synthetic private swapchain
    luke_input_guide,        // result=eye,a=frame,b=publication,c=epoch
    luke_input_camera,       // result=eye,a=frame,b=publication,c=guide camera hash
    luke_locate_context,    // result=XrResult,a=latest wait serial,b=requested time,c=space
    luke_locate_pose,       // result=view,a=latest wait serial,b=requested time,c=pose/FOV hash
    luke_inline_view,       // result=boundary*100+layer*10+view,a=frame,b=space,c=private swapchain
    luke_inline_pose_bits,  // result=viewCode*10+component,a=frame,b/c=raw float pairs
    luke_space_created,    // result=reference type(-1 action),a=space,b=session,c=0
    luke_space_locate,     // result=XrResult,a=sample ID,b=space,c=base space
    luke_space_time,       // result=location flags,a=sample ID,b=requested XrTime,c=latest app wait serial
    luke_render_camera,    // result=eye*100+matrix*10+row,a=guide publication,b/c=4 raw floats
    luke_temporal_pair,    // result=0,a=producer fence,b=previous capture,c=current capture; result1=destinations
    luke_temporal_pose,    // result=phase*100+view*10+pair,a=producer fence,b=capture,c=2 raw floats
    vram_usage, // D35: stage, current adapter usage, budget, subject.
    d3d11_bridge,
    steamvr_delivery_attach,
    steamvr_delivery,
    view_configuration,
    projection_view_rect,
    process_excluded,
    vulkan_negotiation,
    vulkan_bridge,
    compositor_vsync,
    compositor_presentation,
    compositor_timeline,
    // Passive NGX provider diagnostics retained for late logging. a=stage:
    // 1 initialize (result=NGX code), 2 capability query (result=NGX code),
    // 3 capability answer (result=feature init code, b=available, c=frames),
    // 4 feature creation (result=NGX code, b=width, c=height).
    native_dlssg_provider,
    native_dlssg_record, // result=Skip (0 recorded), a=publication, b=detail, c=eye
    native_dlssg_evaluate, // result=NGX result, a=reset, b=output index, c=direct
    native_dlssg_decision, // result=generated mask, a=publication, b=eyes/outputs, c=disable bits
    history_capture_wait, // V441: result HRESULT, a swapchain, b waited us, c budget/index
    menu_status, // result XR/HRESULT; a boundary (0 close,1 create,2 acquire,3 wait,4 fence,5 list,6 draw,7 release,8 end), b XR handle, c image
    benchmark_configuration, // a=revision,b=selected/effective route bits,c=quality options
    benchmark_rates, // a=output/game milliFPS,b=generated milliFPS/display milliHz,c=revision
    benchmark_cycles, // a=OFXR/cycle microseconds,b=estimated microseconds,c=revision; result validity bits
    gpu_afw, // a=provider context,b=guide publication,c=calibrated GPU QPC; result0..3 GPU stages,4 CPU submit
    benchmark_command, // V558: a=requested revision,b=automatic suite active,c=run ID. Closes preceding interval before apply.
    benchmark_stages, // V608: a=game/preparation microseconds,b=delivery microseconds,c=revision; result=valid
};

struct BridgeFlightToken {
    std::uint64_t sequence{};
    std::int64_t start_counter{};
    std::uint64_t recorder_epoch{};
};

class BridgeFlightLogger final {
public:
    BridgeFlightLogger() noexcept;
    ~BridgeFlightLogger();

    BridgeFlightLogger(const BridgeFlightLogger&) = delete;
    BridgeFlightLogger& operator=(const BridgeFlightLogger&) = delete;

    void initialize(const std::filesystem::path& module_directory) noexcept;
    void shutdown() noexcept;
    // Bounded live INI polling; invoke outside resource/presenter locks.
    void refresh_settings() noexcept;
    // Explicit diagnostic/teardown use only; never called on a render path.
    void flush() noexcept;
    [[nodiscard]] bool gpu_instrumentation_enabled() const noexcept {
        return gpu_instrumentation_enabled_.load(std::memory_order_relaxed) && (enabled() || ram_enabled());
    }

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool ram_enabled() const noexcept;
    [[nodiscard]] std::filesystem::path log_path() const;

    // Places a QueryPerformanceCounter value on this log's own timeline,
    // so a GPU timestamp calibrated to QPC can be compared directly with
    // the ms column of every other record.
    [[nodiscard]] std::int64_t microseconds_for_counter(
        std::int64_t counter) const noexcept;

    [[nodiscard]] BridgeFlightToken begin(
        BridgeFlightOperation operation,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

    void end(
        BridgeFlightToken token,
        BridgeFlightOperation operation,
        std::int64_t result,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

    void event(
        BridgeFlightOperation operation,
        std::int64_t result = 0,
        std::uint64_t a = 0,
        std::uint64_t b = 0,
        std::uint64_t c = 0) noexcept;

private:
    struct Impl;
    struct StartupRecord {
        BridgeFlightOperation operation{};
        char phase{};
        std::int64_t counter{}, result{};
        std::uint64_t duration{}, a{}, b{}, c{};
        std::uint32_t thread{};
    };
    void start_disk_locked();
    void remember_startup(BridgeFlightOperation, char, std::int64_t, std::int64_t,
        std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t) noexcept;
    std::atomic<std::shared_ptr<Impl>> impl_;
    std::unique_ptr<RamTimingCapture> ram_;
    std::atomic<bool> ram_recording_{false};
    std::atomic<bool> gpu_instrumentation_enabled_{false}, disk_enabled_{false}, initialized_{false};
    std::atomic<std::uint64_t> next_poll_{0};
    std::mutex control_mutex_, startup_mutex_;
    std::filesystem::path module_directory_;
    std::array<StartupRecord, 128> startup_{};
    std::size_t startup_count_{};
    std::vector<std::shared_ptr<Impl>> retired_;
    std::int64_t base_frequency_{}, base_origin_{};
    std::uint64_t disk_epoch_{}, candidate_since_{};
    bool candidate_enabled_{}, ram_mode_{};
};

[[nodiscard]] BridgeFlightLogger& bridge_flight_logger() noexcept;
void initialize_bridge_flight_logger() noexcept;

} // namespace xrfg
