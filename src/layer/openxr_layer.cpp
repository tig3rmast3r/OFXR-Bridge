#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/provider_api.hpp"
#include <atomic>
#include "xrfg/openxr_fps_overlay.hpp"

#include <windows.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace xrfg {
void set_optiscaler_embedded_delegation(bool active) noexcept;
}

namespace {

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept;

namespace optiscaler_bootstrap {

using ProviderIdentity = int (*)(OFXR_OptiScalerProviderIdentityV2*);
using SetEmbeddedLayerActive = int (*)(int);
using NegotiateLayer = XrResult(XRAPI_PTR *)(
    const XrNegotiateLoaderInfo*,
    const char*,
    XrNegotiateApiLayerRequest*);

struct Exports {
    HMODULE module{};
    SetEmbeddedLayerActive set_active{};
    NegotiateLayer negotiate{};
};

[[nodiscard]] bool enumerate_modules(
    HMODULE* modules, DWORD capacity, DWORD* bytes) noexcept {
    __try {
        return EnumProcessModules(
                   GetCurrentProcess(), modules, capacity, bytes) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] FARPROC find_export(HMODULE module, const char* name) noexcept {
    __try {
        return GetProcAddress(module, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

[[nodiscard]] bool retain_export_module(
    FARPROC address, HMODULE expected, HMODULE* retained) noexcept {
    __try {
        HMODULE module = nullptr;
        if (address == nullptr ||
            !GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(address), &module)) {
            return false;
        }
        if (module != expected) {
            FreeLibrary(module);
            return false;
        }
        *retained = module;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool query_identity(
    ProviderIdentity provider,
    OFXR_OptiScalerProviderIdentityV2* identity) {
    __try {
        return provider(identity) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool set_active(
    SetEmbeddedLayerActive function, int active, int* result) {
    __try {
        *result = function(active);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] bool delegate_negotiation(
    NegotiateLayer function,
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request,
    XrResult* result) {
    __try {
        *result = function(loader_info, layer_name, layer_request);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] std::optional<Exports> find_optiscaler() noexcept {
    std::array<HMODULE, 1024> modules{};
    DWORD bytes = 0;
    if (!enumerate_modules(
            modules.data(), static_cast<DWORD>(sizeof(modules)), &bytes)) {
        return std::nullopt;
    }
    const std::size_t count =
        std::min<std::size_t>(bytes / sizeof(HMODULE), modules.size());
    for (std::size_t index = 0; index < count; ++index) {
        const HMODULE candidate = modules[index];
        if (candidate == nullptr) continue;

        const auto provider = reinterpret_cast<ProviderIdentity>(
            find_export(candidate, "OFXR_OptiScalerProviderV2"));
        HMODULE retained = nullptr;
        if (provider == nullptr ||
            !retain_export_module(
                reinterpret_cast<FARPROC>(provider), candidate, &retained)) {
            continue;
        }

        const auto activation = reinterpret_cast<SetEmbeddedLayerActive>(
            find_export(retained, "OFXR_SetEmbeddedLayerActiveV1"));
        const auto negotiation = reinterpret_cast<NegotiateLayer>(
            find_export(retained, "xrNegotiateLoaderApiLayerInterface"));
        OFXR_OptiScalerProviderIdentityV2 identity{};
        constexpr std::uint64_t required_capabilities =
            OFXR_OPTISCALER_CAP_DLSS_GUIDES_V2;
        bool compatible = false;
        try {
            compatible = activation != nullptr && negotiation != nullptr &&
                query_identity(provider, &identity) &&
                identity.struct_size >= sizeof(identity) &&
                identity.api_version == OFXR_PROVIDER_API_VERSION_V2 &&
                identity.magic == OFXR_OPTISCALER_PROVIDER_MAGIC_V2 &&
                (identity.capabilities & required_capabilities) ==
                    required_capabilities;
        } catch (...) {
            compatible = false;
        }
        if (compatible) {
            return Exports{retained, activation, negotiation};
        }
        FreeLibrary(retained);
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<XrResult> negotiate(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request) noexcept {
    auto exports = find_optiscaler();
    if (!exports) return std::nullopt;

    bool activated = false;
    try {
        int activation_result = 0;
        if (!set_active(exports->set_active, 1, &activation_result) ||
            activation_result == 0) {
            FreeLibrary(exports->module);
            return std::nullopt;
        }
        activated = true;

        XrResult result = XR_ERROR_INITIALIZATION_FAILED;
        if (!delegate_negotiation(
                exports->negotiate, loader_info, layer_name, layer_request,
                &result) || XR_FAILED(result)) {
            int ignored = 0;
            (void)set_active(exports->set_active, 0, &ignored);
            FreeLibrary(exports->module);
            return std::nullopt;
        }

        ::xrfg::set_optiscaler_embedded_delegation(true);
        return result;
    } catch (...) {
        if (activated) {
            int ignored = 0;
            (void)set_active(exports->set_active, 0, &ignored);
        }
        FreeLibrary(exports->module);
        return std::nullopt;
    }
}

}  // namespace optiscaler_bootstrap

constexpr char kLayerName[] = "XR_APILAYER_XRFrameBridge_diagnostic";
constexpr XrVersion kLayerApiVersion = XR_MAKE_VERSION(1, 0, 0);
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;
constexpr auto kStructuralQuarantineDuration = std::chrono::seconds(1);
// The runtime's own xrWaitFrame pacing happens first. Only a bridge transaction
// still pending after that natural idle window may hold the application here.
// This prevents the next game/NGX frame from being queued behind unfinished
// synthesis while keeping a genuinely unhealthy GPU wait bounded.
constexpr std::uint32_t kFrameStartSynthesisWaitMilliseconds = 1000;

template <typename Handle>
[[nodiscard]] std::uint64_t handle_value(Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept {
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&current_layer_directory),
                &module)) {
            return {};
        }
        std::array<wchar_t, 32768> path{};
        const DWORD length = GetModuleFileNameW(
            module, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size()) {
            return {};
        }
        return std::filesystem::path(path.data()).parent_path();
    } catch (...) {
        return {};
    }
}

[[nodiscard]] std::uint64_t optical_flow_configuration_code(
    xrfg::D3D12OpticalFlowBackend backend,
    const xrfg::D3D12NvidiaOpticalFlowOptions& options) noexcept {
    if (backend != xrfg::D3D12OpticalFlowBackend::nvidia) {
        return 0;
    }
    std::uint64_t code = 2;
    if (options.preset == xrfg::D3D12NvidiaPerformancePreset::slow) {
        code = 1;
    } else if (options.preset == xrfg::D3D12NvidiaPerformancePreset::fast) {
        code = 3;
    }
    std::uint64_t scale_code = 0;
    if (options.input_scale ==
        xrfg::D3D12NvidiaInputScale::three_quarter) {
        scale_code = 0x10000U;
    } else if (options.input_scale == xrfg::D3D12NvidiaInputScale::half) {
        scale_code = 0x20000U;
    }
    return code | (options.bidirectional ? 0x100U : 0U) | scale_code;
}

struct Dispatch {
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrBeginSession begin_session{};
    PFN_xrEndSession end_session{};
    PFN_xrWaitFrame wait_frame{};
    PFN_xrBeginFrame begin_frame{};
    PFN_xrEndFrame end_frame{};
    PFN_xrCreateSwapchain create_swapchain{};
    PFN_xrDestroySwapchain destroy_swapchain{};
    PFN_xrEnumerateSwapchainImages enumerate_swapchain_images{};
    PFN_xrAcquireSwapchainImage acquire_swapchain_image{};
    PFN_xrWaitSwapchainImage wait_swapchain_image{};
    PFN_xrReleaseSwapchainImage release_swapchain_image{};
    // Best-effort: a runtime that does not expose it simply never has a space
    // destroyed underneath a queued submission through this layer.
    PFN_xrDestroySpace destroy_space{};
    bool steamvr_runtime{};
    XrVersion runtime_version{};
    std::string runtime_name;
};

[[nodiscard]] std::uint64_t runtime_name_hash(
    std::string_view name) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : name) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

struct GeneratedFrameEndInfo;

struct PresenterSubmission {
    std::uint64_t sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame;
    const XrFrameEndInfo* borrowed_frame{};
    XrResult result{XR_SUCCESS};
    bool completed{};
};

struct ProjectionLayerSnapshot {
    std::uint32_t layer_index{};
    XrCompositionLayerFlags layer_flags{};
    XrSpace space{XR_NULL_HANDLE};
    std::vector<XrCompositionLayerProjectionView> views;
};

struct ProjectionSnapshot {
    XrTime display_time{};
    XrEnvironmentBlendMode environment_blend_mode{};
    std::vector<ProjectionLayerSnapshot> layers;
};

struct ProjectionViewReference {
    std::size_t projection_index{};
    std::size_t view_index{};
};

struct ProjectionResourceMapping {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    // Canonical logical views for this resource. Ordinarily there is one per
    // array slice; UEVR Native Stereo may instead submit two non-overlapping
    // eye viewports in one physical slice.
    std::vector<ProjectionViewReference> views;
};

enum class ProjectionMappingReason : std::int64_t {
    ready = 0,
    no_projection_views = 1,
    unknown_swapchain = 2,
    unsupported_array_size = 3,
    zero_resource_extent = 4,
    array_slice_out_of_range = 5,
    negative_subimage_offset = 6,
    nonpositive_subimage_extent = 7,
    subimage_out_of_bounds = 8,
    missing_array_slice = 9,
    exception = 10,
    unsupported_view_layout = 11,
};

struct ProjectionMappingResult {
    std::vector<ProjectionResourceMapping> mappings;
    ProjectionMappingReason reason{ProjectionMappingReason::exception};
    std::uint64_t detail{};

    [[nodiscard]] bool ready() const noexcept {
        return reason == ProjectionMappingReason::ready && !mappings.empty();
    }
};

enum class SessionGraphicsBinding : std::int64_t {
    none = 0,
    d3d11 = 1,
    d3d12 = 2,
    vulkan = 3,
    opengl = 4,
};

struct PendingApplicationFrame {
    XrTime display_time{};
    XrDuration display_period{};
};

enum class GenerationQuarantineReason : std::int64_t {
    swapchain_created = 1,
    swapchain_destroyed = 2,
    d3d11_images_changed = 3,
    d3d12_images_changed = 4,
    projection_changed = 6,
    projection_mapping_failed = 7,
    generation_prepare_failed = 8,
    generated_end_info_failed = 9,
    presenter_composition_failed = 10,
    downstream_end_failed = 11,
};

// Windows 10 1803 and later. Declared here so the layer still builds against
// an SDK that predates it; the create call degrades to a coarse timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

struct SessionState {
    explicit SessionState(std::shared_ptr<Dispatch> next_dispatch)
        : dispatch(std::move(next_dispatch)), manual_control(current_layer_directory()) {}

    ~SessionState() {
        xrfg::embedded::detach(control_id);
        if (presenter_pace_timer != nullptr) {
            CloseHandle(presenter_pace_timer);
        }
    }

    SessionState(const SessionState&) = delete;
    SessionState& operator=(const SessionState&) = delete;

    std::shared_ptr<Dispatch> dispatch;
    xrfg::implicit_layer::ManualArmControl manual_control;
    std::uint64_t control_id{xrfg::embedded::attach()};
    std::uint64_t control_revision{}; // frame_call_mutex
    bool control_reconfigure_required{};
    std::atomic<bool> menu_enabled{true};
    std::atomic<bool> generation_steady_state_established{false};
    bool manual_stop_applied{}; // frame_call_mutex; terminal for this XrSession.
    std::unique_ptr<xrfg::OpenXrFpsOverlay> fps_overlay;
    // Serializes each generated synthetic/real frame pair atomically with
    // respect to application frame calls. A successful application wait owns
    // the next admission until its matching begin has been attempted, so a
    // pipelined second wait cannot overtake that begin and deadlock the runtime.
    std::mutex frame_call_mutex;
    std::condition_variable frame_call_condition;
    bool application_wait_pending_begin{};
    // Some applications run the next wait on a dedicated thread before the
    // current render thread ends its frame. Two consecutive overlaps select a
    // virtual application loop; the runtime-facing presenter then owns all
    // later physical frame cycles.
    bool application_frame_in_progress{};
    bool application_frame_has_overlapping_wait{};
    std::uint32_t pipelined_wait_streak{};
    bool pipelined_presenter_mode{};
    bool pipelined_presenter_start_requested{};
    bool steamvr_presenter_start_requested{};
    XrFrameState last_inline_frame_state{XR_TYPE_FRAME_STATE};
    bool last_inline_frame_state_valid{};
    std::mutex mutex;
    std::mutex gpu_mutex;
    std::deque<PendingApplicationFrame> pending_frames;
    std::optional<ProjectionSnapshot> previous_projection;
    XrTime generation_resume_display_time{};
    std::chrono::steady_clock::time_point generation_resume_wall_time{};
    XrDuration minimum_runtime_display_period{};
    std::uint32_t steamvr_throttled_wait_streak{};
    xrfg::D3D12OpticalFlowBackend optical_flow_backend{
        xrfg::D3D12OpticalFlowBackend::fidelity_fx};
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options{};
    bool dlss_motion_vectors{};
    SessionGraphicsBinding graphics_binding{SessionGraphicsBinding::none};
    std::uint64_t graphics_binding_capabilities{};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue;
    // Some SteamVR configurations throttle the inline second wait/begin/end
    // cycle to the application's half-rate interval. After that behavior is
    // measured, the dedicated presenter becomes the sole owner of downstream
    // calls while the application observes a virtual half-rate loop.
    std::mutex presenter_mutex;
    std::condition_variable presenter_condition;
    std::mutex presenter_content_mutex;
    std::deque<std::shared_ptr<PresenterSubmission>> presenter_submissions;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_last_frame;
    std::thread presenter_thread;
    // When the presenter last handed a frame to the runtime, and the period it
    // was told to expect. The presenter paces itself against these because
    // xrWaitFrame cannot be relied on to do it: see pace_presenter_submission.
    // Guarded by presenter_mutex.
    // When the next submission is due. Advanced by exactly one period per
    // submission rather than measured from the previous one, so the loop's own
    // cost lands as a constant offset instead of accumulating into the
    // cadence. Chaining each wait off the last submission added about 4.6 ms
    // of per-cycle overhead to a 10 ms pace and held the runtime to 64/s.
    std::chrono::steady_clock::time_point presenter_next_submit{};
    bool presenter_schedule_valid{};
    // A condition variable waits on the system tick, which is 15.6 ms by
    // default on Windows. Every pace wait rounded up to that, so an 11.11 ms
    // schedule produced 15.5 ms submissions and exactly 64/s no matter what
    // the schedule asked for. A high-resolution timer sleeps to well under a
    // millisecond without changing the process-wide timer period, which a
    // layer has no business doing to its host.
    HANDLE presenter_pace_timer{};
    // The smallest display period the runtime has reported, which is the one
    // the hardware actually scans at. Not the latest reported value: SteamVR
    // returns a multiple of the true period when it considers the caller
    // behind - 11.1, then 55.6, then 22.2 ms within a few frames - so pacing
    // against the latest value lets a slow frame widen the pace, which makes
    // the next frame later still. That spiral throttled the presenter to
    // 3.7 Hz and froze the session.
    XrDuration presenter_display_period{};
    XrFrameState presenter_frame_state{XR_TYPE_FRAME_STATE};
    XrTime last_virtual_display_time{};
    XrResult presenter_failure{XR_SUCCESS};
    std::uint64_t next_presenter_sequence{1};
    std::size_t outstanding_presenter_submissions{};
    bool presenter_frame_state_valid{};
    bool presenter_stop_requested{};
    bool presenter_active{};
    XrSession handle{XR_NULL_HANDLE};
};

enum class PrivateOwnershipPhase {
    idle,
    acquired,
    waited,
    release_pending,
};

struct PrivateSwapchainState {
    XrSwapchain handle{XR_NULL_HANDLE};
    PrivateOwnershipPhase phase{PrivateOwnershipPhase::idle};
    std::uint32_t acquired_index{};
};

// The current output alternates between two private swapchains, so the
// application can release frame N+1's output while the presenter still has
// frame N's current submission queued. A composition layer names a swapchain
// rather than an image index, and the runtime binds whichever image was
// released last when xrEndFrame runs, so one shared swapchain would repoint
// that queued frame at the newer image.
//
// The synthetic output needs no second slot. It is always the first of the
// pair to be submitted, so it has already left the queue by the time the
// application is admitted to build the next pair.
constexpr std::size_t kCurrentSlotCount = 2;

struct FrameGenerationSwapchainState {
    std::array<PrivateSwapchainState, kCurrentSlotCount> current{};
    // Destination images are addressed by a flat index across every slot, so
    // slot s image i is s * current_images_per_slot + i.
    std::uint32_t current_images_per_slot{};
    std::size_t current_slot{};
    PrivateSwapchainState synthetic;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
};

void log_completed_nvidia_gpu_timings(
    const std::shared_ptr<xrfg::D3D12FrameSynthesizer>& synthesizer) noexcept {
    if (!synthesizer || !xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    for (;;) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        const HRESULT result =
            synthesizer->consume_nvidia_gpu_timing(&timing);
        if (result == S_FALSE) {
            return;
        }
        if (FAILED(result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::nvidia_gpu_total,
                result);
            return;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_stages,
            timing.eye_count,
            timing.pack_microseconds,
            timing.eye0_microseconds,
            timing.eye1_microseconds);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::nvidia_gpu_total,
            0,
            timing.composition_microseconds,
            timing.total_microseconds,
            timing.current_serial);
    }
}

struct SwapchainState;

enum class SwapchainEligibilityReason : std::int64_t {
    ready = 0,
    no_d3d12_binding = 1,
    incomplete_enumeration = 2,
    invalid_d3d12_image = 3,
    ambiguous_attachment_usage = 4,
    protected_content = 5,
    history_initialize_failed = 6,
    depth_only = 7,
    static_image = 8,
    unsupported_face_count = 9,
    missing_dispatch_or_history = 10,
    current_private_swapchain_failed = 11,
    synthetic_private_swapchain_failed = 12,
    synthesis_initialize_failed = 13,
    exception = 14,
    d3d11_interop_initialize_failed = 15,
    invalid_d3d11_image = 16,
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail = 0,
    std::uint64_t auxiliary = 0) noexcept;

struct SwapchainState {
    SwapchainState(
        std::shared_ptr<SessionState> owner,
        const XrSwapchainCreateInfo& input_create_info)
        : session(std::move(owner)),
          create_info(input_create_info) {
        create_info.next = nullptr;
    }

    std::shared_ptr<SessionState> session;
    XrSwapchain handle{XR_NULL_HANDLE};
    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    // OpenXR permits acquire/wait/release calls from different threads. Keep the
    // downstream call and the matching ownership bookkeeping in one total order.
    std::mutex call_mutex;
    std::mutex mutex;
    std::deque<std::uint32_t> acquired_indices;
    bool front_waited{};
    bool ownership_tracking_valid{true};
    std::optional<std::uint32_t> last_released_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> d3d12_history;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> enumerated_d3d11_images;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> enumerated_d3d12_images;
    std::optional<xrfg::D3D12HistoryCaptureTicket> last_released_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> last_released_motion_vectors;
    std::shared_ptr<FrameGenerationSwapchainState> frame_generation;
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail,
    std::uint64_t auxiliary) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_eligibility,
        static_cast<std::int64_t>(reason),
        state ? handle_value(state->handle) : 0,
        detail,
        auxiliary);
}

std::mutex g_state_mutex;
std::unordered_map<XrInstance, std::shared_ptr<Dispatch>> g_instances;
std::unordered_map<XrSession, std::shared_ptr<SessionState>> g_sessions;
std::unordered_map<XrSwapchain, std::shared_ptr<SwapchainState>> g_swapchains;

template <typename Function>
[[nodiscard]] XrResult guard_c_api_boundary(Function&& function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] std::shared_ptr<Dispatch> find_dispatch(XrInstance instance) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_instances.find(instance);
    return iterator == g_instances.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SessionState> find_session(XrSession session) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_sessions.find(session);
    return iterator == g_sessions.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::shared_ptr<SwapchainState> find_swapchain(XrSwapchain swapchain) {
    std::scoped_lock lock(g_state_mutex);
    const auto iterator = g_swapchains.find(swapchain);
    return iterator == g_swapchains.end() ? nullptr : iterator->second;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<SessionState>& session) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session == session) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] std::vector<std::shared_ptr<SwapchainState>> find_swapchains(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SwapchainState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_swapchains) {
        (void)handle;
        if (state->session->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] HRESULT wait_for_previous_session_synthesis(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        HRESULT aggregate = S_OK;
        for (const auto& swapchain : find_swapchains(session)) {
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            const HRESULT result =
                generation->synthesizer->wait_for_previous_submission(
                    kFrameStartSynthesisWaitMilliseconds);
            if (FAILED(result)) {
                aggregate = result;
                if (result != HRESULT_FROM_WIN32(ERROR_BUSY)) {
                    break;
                }
            }
        }
        return aggregate;
    } catch (...) {
        return E_FAIL;
    }
}

[[nodiscard]] std::vector<std::shared_ptr<SessionState>> find_sessions(
    const std::shared_ptr<Dispatch>& dispatch) {
    std::vector<std::shared_ptr<SessionState>> matches;
    std::scoped_lock lock(g_state_mutex);
    for (const auto& [handle, state] : g_sessions) {
        (void)handle;
        if (state->dispatch == dispatch) {
            matches.push_back(state);
        }
    }
    return matches;
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame = nullptr,
    bool preserve_virtual_timeline = false) noexcept;
void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept;
// Prevent new enqueues, finish queued/in-flight submissions, then retire any
// autonomous repeat before invalidating an application-owned handle.
struct PresenterResourceLifetimeGuard {
    explicit PresenterResourceLifetimeGuard(const std::shared_ptr<SessionState>& state);
    std::unique_lock<std::mutex> frame_lock;
    std::unique_lock<std::mutex> content_lock;
};
void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;
void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept;
void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail = 0) noexcept;
[[nodiscard]] XrDuration doubled_display_period(XrDuration period) noexcept;
[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept;

void drain_swapchain_gpu(const std::shared_ptr<SwapchainState>& state) noexcept {
    const auto flight_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::gpu_drain,
        state && state->session ? handle_value(state->session->handle) : 0);
    try {
        std::scoped_lock gpu_lock(state->session->gpu_mutex);
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            generation = state->frame_generation;
        }
        HRESULT result = S_OK;
        if (generation && generation->synthesizer) {
            result = generation->synthesizer->wait_for_idle();
        }
        if (history) {
            const HRESULT history_result = history->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = history_result;
            }
        }
        if (generation && generation->d3d11_interop) {
            const HRESULT interop_result =
                generation->d3d11_interop->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = interop_result;
            }
        }
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            result);
    } catch (...) {
        xrfg::bridge_flight_logger().end(
            flight_token,
            xrfg::BridgeFlightOperation::gpu_drain,
            E_FAIL);
    }
}

[[nodiscard]] bool release_private_image(
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (image.handle == XR_NULL_HANDLE || image.phase == PrivateOwnershipPhase::idle) {
            return true;
        }
        if (!dispatch || dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase == PrivateOwnershipPhase::acquired) {
            XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait_info.timeout = XR_INFINITE_DURATION;
            const auto wait_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            const XrResult wait_result =
                dispatch->wait_swapchain_image(image.handle, &wait_info);
            xrfg::bridge_flight_logger().end(
                wait_token,
                xrfg::BridgeFlightOperation::private_swapchain_wait,
                wait_result,
                handle_value(image.handle),
                image.acquired_index,
                static_cast<std::uint64_t>(image.phase));
            if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
                return false;
            }
            image.phase = PrivateOwnershipPhase::waited;
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto release_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_release,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult release_result =
            dispatch->release_swapchain_image(image.handle, &release_info);
        xrfg::bridge_flight_logger().end(
            release_token,
            xrfg::BridgeFlightOperation::private_swapchain_release,
            release_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(release_result)) {
            image.phase = PrivateOwnershipPhase::release_pending;
            return false;
        }
        image.phase = PrivateOwnershipPhase::idle;
        return true;
    } catch (...) {
        return false;
    }
}

void destroy_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        {
            std::scoped_lock lock(state->mutex);
            generation = std::move(state->frame_generation);
        }
        if (!generation || !state->session || !state->session->dispatch) {
            return;
        }

        const auto& dispatch = state->session->dispatch;
        static_cast<void>(release_private_image(dispatch, generation->synthetic));
        for (PrivateSwapchainState& image : generation->current) {
            static_cast<void>(release_private_image(dispatch, image));
        }
        if (dispatch->destroy_swapchain == nullptr) {
            return;
        }
        std::array<PrivateSwapchainState*, kCurrentSlotCount + 1> owned{};
        owned[0] = &generation->synthetic;
        for (std::size_t slot = 0; slot < kCurrentSlotCount; ++slot) {
            owned[slot + 1] = &generation->current[slot];
        }
        for (PrivateSwapchainState* image : owned) {
            if (image->handle == XR_NULL_HANDLE) {
                continue;
            }
            static_cast<void>(dispatch->destroy_swapchain(image->handle));
            image->handle = XR_NULL_HANDLE;
        }
    } catch (...) {
    }
}

[[nodiscard]] std::optional<D3D12_RESOURCE_STATES> required_release_state(
    const SwapchainState& state) noexcept {
    const bool is_color =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
    const bool is_depth =
        (state.create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
    if (is_color == is_depth) {
        return std::nullopt;
    }
    return is_color ? D3D12_RESOURCE_STATE_RENDER_TARGET
                    : D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

struct CreatedPrivateSwapchain {
    PrivateSwapchainState state;
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
};

[[nodiscard]] bool create_private_swapchain(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    CreatedPrivateSwapchain* output) {
    if (!state || !state->session || !state->session->dispatch || output == nullptr) {
        return false;
    }
    const auto& dispatch = state->session->dispatch;
    XrSwapchain handle = XR_NULL_HANDLE;
    XrResult result = dispatch->create_swapchain(
        state->session->handle,
        &create_info,
        &handle);
    if (XR_FAILED(result) || handle == XR_NULL_HANDLE) {
        return false;
    }

    std::uint32_t image_count = 0;
    result = dispatch->enumerate_swapchain_images(handle, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        dispatch->destroy_swapchain(handle);
        return false;
    }

    if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
        std::vector<XrSwapchainImageD3D11KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d11_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d11_resources.clear();
                return false;
            }
            output->d3d11_resources[index] = images[index].texture;
        }
    } else {
        std::vector<XrSwapchainImageD3D12KHR> images(image_count);
        for (auto& image : images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            image.next = nullptr;
        }
        result = dispatch->enumerate_swapchain_images(
            handle,
            image_count,
            &image_count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result) || image_count != images.size()) {
            dispatch->destroy_swapchain(handle);
            return false;
        }
        output->d3d12_resources.resize(image_count);
        for (std::uint32_t index = 0; index < image_count; ++index) {
            if (images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                images[index].texture == nullptr) {
                dispatch->destroy_swapchain(handle);
                output->d3d12_resources.clear();
                return false;
            }
            output->d3d12_resources[index] = images[index].texture;
        }
    }
    output->state.handle = handle;
    return true;
}

struct CreatedCurrentRing {
    std::array<CreatedPrivateSwapchain, kCurrentSlotCount> slots{};
    // Every slot's destination images end to end, in slot order, which is the
    // flat addressing both the synthesizer and the D3D11 interop expect.
    std::vector<ID3D12Resource*> d3d12_resources;
    std::vector<ID3D11Texture2D*> d3d11_resources;
    std::uint32_t images_per_slot{};
};

void destroy_current_ring(
    const std::shared_ptr<Dispatch>& dispatch,
    CreatedCurrentRing* ring) noexcept {
    if (ring == nullptr || !dispatch || dispatch->destroy_swapchain == nullptr) {
        return;
    }
    for (CreatedPrivateSwapchain& created : ring->slots) {
        if (created.state.handle == XR_NULL_HANDLE) {
            continue;
        }
        static_cast<void>(dispatch->destroy_swapchain(created.state.handle));
        created.state.handle = XR_NULL_HANDLE;
    }
}

[[nodiscard]] bool create_current_ring(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainCreateInfo& create_info,
    CreatedCurrentRing* output) {
    if (output == nullptr) {
        return false;
    }
    for (std::size_t slot = 0; slot < kCurrentSlotCount; ++slot) {
        if (!create_private_swapchain(state, create_info, &output->slots[slot])) {
            return false;
        }
        const CreatedPrivateSwapchain& created = output->slots[slot];
        const std::size_t count = created.d3d12_resources.empty()
            ? created.d3d11_resources.size()
            : created.d3d12_resources.size();
        // A flat destination index assumes one stride for every slot, so a
        // runtime that hands out different image counts is not usable here.
        if (count == 0 ||
            (slot != 0 && count != output->images_per_slot)) {
            return false;
        }
        output->images_per_slot = static_cast<std::uint32_t>(count);
        output->d3d12_resources.insert(
            output->d3d12_resources.end(),
            created.d3d12_resources.begin(),
            created.d3d12_resources.end());
        output->d3d11_resources.insert(
            output->d3d11_resources.end(),
            created.d3d11_resources.begin(),
            created.d3d11_resources.end());
    }
    return true;
}

// Publishes a created ring into the shared generation state.
void adopt_current_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedCurrentRing& ring) noexcept {
    for (std::size_t slot = 0; slot < kCurrentSlotCount; ++slot) {
        generation->current[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->current_images_per_slot = ring.images_per_slot;
    generation->current_slot = 0;
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d12_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedCurrentRing current;
    CreatedPrivateSwapchain synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& dispatch = state->session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 ||
            state->session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            state->session->d3d12_device == nullptr ||
            state->session->d3d12_queue == nullptr ||
            !state->d3d12_history) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |=
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if (!create_current_ring(state, private_info, &current)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }
        if (!create_private_swapchain(state, private_info, &synthetic)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }

        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const xrfg::D3D12OpticalFlowBackend backend =
            state->session->optical_flow_backend;
        const auto initialize_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            handle_value(state->session->handle),
            (static_cast<std::uint64_t>(state->create_info.width) << 32) |
                state->create_info.height,
            optical_flow_configuration_code(
                backend, state->session->nvidia_options));
        const HRESULT gpu_result = synthesizer->initialize(
            state->session->d3d12_device.Get(),
            state->session->d3d12_queue.Get(),
            state->d3d12_history,
            std::span<ID3D12Resource* const>(
                current.d3d12_resources.data(),
                current.d3d12_resources.size()),
            std::span<ID3D12Resource* const>(
                synthetic.d3d12_resources.data(),
                synthetic.d3d12_resources.size()),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            state->session->nvidia_options,
            xrfg::bridge_flight_logger().enabled());
        if (SUCCEEDED(gpu_result)) {
        }
        xrfg::bridge_flight_logger().end(
            initialize_token,
            xrfg::BridgeFlightOperation::synthesis_initialize,
            gpu_result,
            current.d3d12_resources.size(),
            synthetic.d3d12_resources.size(),
            state->create_info.arraySize);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            dispatch->destroy_swapchain(synthetic.state.handle);
            synthetic.state.handle = XR_NULL_HANDLE;
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }

        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        adopt_current_ring(generation, current);
        generation->synthetic = synthetic.state;
        generation->synthesizer = std::move(synthesizer);
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            if (synthetic.state.handle != XR_NULL_HANDLE) {
                state->session->dispatch->destroy_swapchain(synthetic.state.handle);
            }
            destroy_current_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d11_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    std::span<ID3D11Texture2D* const> application_images,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedCurrentRing current;
    CreatedPrivateSwapchain synthetic;
    if (failure_reason != nullptr) {
        *failure_reason = SwapchainEligibilityReason::exception;
    }
    if (failure_detail != nullptr) {
        *failure_detail = 0;
    }
    try {
        const auto& session = state->session;
        const auto& dispatch = session->dispatch;
        const bool is_color =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0;
        const bool is_depth =
            (state->create_info.usageFlags &
             XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const bool protected_content =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (!is_color || is_depth || protected_content || static_image ||
            state->create_info.faceCount != 1 || application_images.empty() ||
            session->handle == XR_NULL_HANDLE ||
            dispatch->create_swapchain == nullptr ||
            dispatch->destroy_swapchain == nullptr ||
            dispatch->enumerate_swapchain_images == nullptr ||
            session->d3d11_device == nullptr ||
            session->d3d11_context == nullptr ||
            session->d3d12_device == nullptr ||
            session->d3d12_queue == nullptr) {
            if (failure_reason != nullptr) {
                *failure_reason = protected_content
                    ? SwapchainEligibilityReason::protected_content
                    : static_image
                        ? SwapchainEligibilityReason::static_image
                        : state->create_info.faceCount != 1
                            ? SwapchainEligibilityReason::unsupported_face_count
                            : SwapchainEligibilityReason::missing_dispatch_or_history;
            }
            return nullptr;
        }

        const auto destroy_private = [&]() noexcept {
            if (synthetic.state.handle != XR_NULL_HANDLE) {
                static_cast<void>(
                    dispatch->destroy_swapchain(synthetic.state.handle));
                synthetic.state.handle = XR_NULL_HANDLE;
            }
            destroy_current_ring(dispatch, &current);
        };

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if (!create_current_ring(state, private_info, &current)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            destroy_private();
            return nullptr;
        }
        if (!create_private_swapchain(state, private_info, &synthetic)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            destroy_private();
            return nullptr;
        }

        auto interop =
            std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
        xrfg::D3D11InteropInitializationStage interop_failure_stage =
            xrfg::D3D11InteropInitializationStage::complete;
        HRESULT gpu_result = interop->initialize(
            session->d3d11_device.Get(),
            session->d3d11_context.Get(),
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            application_images,
            std::span<ID3D11Texture2D* const>(
                current.d3d11_resources.data(),
                current.d3d11_resources.size()),
            std::span<ID3D11Texture2D* const>(
                synthetic.d3d11_resources.data(),
                synthetic.d3d11_resources.size()),
            &interop_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::d3d11_interop_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(interop_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        xrfg::D3D12HistoryInitializationStage history_failure_stage =
            xrfg::D3D12HistoryInitializationStage::complete;
        gpu_result = history->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            interop->source_images(),
            D3D12_RESOURCE_STATE_COMMON,
            &history_failure_stage);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::history_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail =
                    (static_cast<std::uint64_t>(history_failure_stage) << 32) |
                    static_cast<std::uint32_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        auto synthesizer = std::make_shared<xrfg::D3D12FrameSynthesizer>();
        const xrfg::D3D12OpticalFlowBackend backend =
            session->optical_flow_backend;
        const auto initialize_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::synthesis_initialize,
            handle_value(session->handle),
            (static_cast<std::uint64_t>(state->create_info.width) << 32) |
                state->create_info.height,
            optical_flow_configuration_code(
                backend, session->nvidia_options));
        gpu_result = synthesizer->initialize(
            session->d3d12_device.Get(),
            session->d3d12_queue.Get(),
            history,
            interop->current_destination_images(),
            interop->synthetic_destination_images(),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            D3D12_RESOURCE_STATE_COMMON,
            backend,
            session->nvidia_options,
            xrfg::bridge_flight_logger().enabled());
        if (SUCCEEDED(gpu_result)) {
        }
        xrfg::bridge_flight_logger().end(
            initialize_token,
            xrfg::BridgeFlightOperation::synthesis_initialize,
            gpu_result,
            interop->current_destination_images().size(),
            interop->synthetic_destination_images().size(),
            state->create_info.arraySize);
        if (FAILED(gpu_result)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthesis_initialize_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(gpu_result);
            }
            destroy_private();
            return nullptr;
        }

        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        adopt_current_ring(generation, current);
        generation->synthetic = synthetic.state;
        generation->synthesizer = std::move(synthesizer);
        generation->d3d11_interop = std::move(interop);
        {
            std::scoped_lock lock(state->mutex);
            state->d3d12_history = std::move(history);
        }
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            if (synthetic.state.handle != XR_NULL_HANDLE) {
                static_cast<void>(state->session->dispatch->destroy_swapchain(
                    synthetic.state.handle));
            }
            destroy_current_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

template <typename Function>
[[nodiscard]] bool load_function(
    PFN_xrGetInstanceProcAddr get_instance_proc_addr,
    XrInstance instance,
    const char* name,
    Function& output) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = get_instance_proc_addr(instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        output = nullptr;
        return false;
    }
    output = reinterpret_cast<Function>(function);
    return true;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state);
XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain);
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space);
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images);
XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index);
XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info);

template <typename Function>
XrResult expose_intercept(
    const std::shared_ptr<Dispatch>& dispatch,
    Function next_function,
    Function layer_function,
    PFN_xrVoidFunction* output) {
    if (!dispatch || next_function == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    *output = reinterpret_cast<PFN_xrVoidFunction>(layer_function);
    return XR_SUCCESS;
}

XrResult layer_get_instance_proc_addr_impl(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    if (name == nullptr || function == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    *function = nullptr;

    if (std::strcmp(name, "xrGetInstanceProcAddr") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(layer_get_instance_proc_addr);
        return XR_SUCCESS;
    }

    const auto dispatch = find_dispatch(instance);
    if (!dispatch) {
        return instance == XR_NULL_HANDLE ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_ERROR_HANDLE_INVALID;
    }

    if (std::strcmp(name, "xrDestroyInstance") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_instance, layer_destroy_instance, function);
    }
    if (std::strcmp(name, "xrCreateSession") == 0) {
        return expose_intercept(dispatch, dispatch->create_session, layer_create_session, function);
    }
    if (std::strcmp(name, "xrDestroySession") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_session, layer_destroy_session, function);
    }
    if (std::strcmp(name, "xrBeginSession") == 0) {
        return expose_intercept(dispatch, dispatch->begin_session, layer_begin_session, function);
    }
    if (std::strcmp(name, "xrEndSession") == 0) {
        return expose_intercept(dispatch, dispatch->end_session, layer_end_session, function);
    }
    if (std::strcmp(name, "xrWaitFrame") == 0) {
        return expose_intercept(dispatch, dispatch->wait_frame, layer_wait_frame, function);
    }
    if (std::strcmp(name, "xrBeginFrame") == 0) {
        return expose_intercept(dispatch, dispatch->begin_frame, layer_begin_frame, function);
    }
    if (std::strcmp(name, "xrEndFrame") == 0) {
        return expose_intercept(dispatch, dispatch->end_frame, layer_end_frame, function);
    }
    if (std::strcmp(name, "xrCreateSwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->create_swapchain, layer_create_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySwapchain") == 0) {
        return expose_intercept(dispatch, dispatch->destroy_swapchain, layer_destroy_swapchain, function);
    }
    if (std::strcmp(name, "xrDestroySpace") == 0) {
        return expose_intercept(
            dispatch, dispatch->destroy_space, layer_destroy_space, function);
    }
    if (std::strcmp(name, "xrEnumerateSwapchainImages") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->enumerate_swapchain_images,
            layer_enumerate_swapchain_images,
            function);
    }
    if (std::strcmp(name, "xrAcquireSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->acquire_swapchain_image,
            layer_acquire_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrWaitSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->wait_swapchain_image,
            layer_wait_swapchain_image,
            function);
    }
    if (std::strcmp(name, "xrReleaseSwapchainImage") == 0) {
        return expose_intercept(
            dispatch,
            dispatch->release_swapchain_image,
            layer_release_swapchain_image,
            function);
    }

    return dispatch->get_instance_proc_addr(instance, name, function);
}

XrResult layer_create_api_layer_instance_impl(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    if (create_info == nullptr || layer_info == nullptr || instance == nullptr ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion < XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize < sizeof(XrApiLayerCreateInfo) ||
        layer_info->nextInfo == nullptr ||
        layer_info->nextInfo->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        layer_info->nextInfo->structVersion < XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        layer_info->nextInfo->structSize < sizeof(XrApiLayerNextInfo) ||
        std::strcmp(layer_info->nextInfo->layerName, kLayerName) != 0 ||
        layer_info->nextInfo->nextGetInstanceProcAddr == nullptr ||
        layer_info->nextInfo->nextCreateApiLayerInstance == nullptr) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    XrApiLayerCreateInfo next_layer_info = *layer_info;
    next_layer_info.nextInfo = layer_info->nextInfo->next;

    const PFN_xrGetInstanceProcAddr next_get_instance_proc_addr =
        layer_info->nextInfo->nextGetInstanceProcAddr;
    const PFN_xrCreateApiLayerInstance next_create_api_layer_instance =
        layer_info->nextInfo->nextCreateApiLayerInstance;

    auto dispatch = std::make_shared<Dispatch>();
    dispatch->get_instance_proc_addr = next_get_instance_proc_addr;
    XrInstance created_instance = XR_NULL_HANDLE;
    const XrResult result = next_create_api_layer_instance(
        create_info,
        &next_layer_info,
        &created_instance);
    if (XR_FAILED(result)) {
        return result;
    }

    const bool loaded =
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroyInstance", dispatch->destroy_instance) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSession", dispatch->create_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySession", dispatch->destroy_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginSession", dispatch->begin_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndSession", dispatch->end_session) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrWaitFrame", dispatch->wait_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrBeginFrame", dispatch->begin_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrEndFrame", dispatch->end_frame) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrCreateSwapchain", dispatch->create_swapchain) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySwapchain", dispatch->destroy_swapchain) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, it just never has a space settled before destroy.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrDestroySpace",
             dispatch->destroy_space)),
         true) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrEnumerateSwapchainImages",
            dispatch->enumerate_swapchain_images) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrAcquireSwapchainImage",
            dispatch->acquire_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrWaitSwapchainImage",
            dispatch->wait_swapchain_image) &&
        load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrReleaseSwapchainImage",
            dispatch->release_swapchain_image);
    if (!loaded) {
        if (dispatch->destroy_instance != nullptr) {
            dispatch->destroy_instance(created_instance);
        }
        return XR_ERROR_INITIALIZATION_FAILED;
    }

    PFN_xrGetInstanceProperties get_instance_properties = nullptr;
    if (load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrGetInstanceProperties",
            get_instance_properties)) {
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        if (XR_SUCCEEDED(get_instance_properties(created_instance, &properties))) {
            dispatch->runtime_version = properties.runtimeVersion;
            dispatch->runtime_name = properties.runtimeName;
            dispatch->steamvr_runtime =
                std::string_view(dispatch->runtime_name).find("SteamVR") !=
                std::string_view::npos;
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::runtime_identity,
        dispatch->steamvr_runtime ? 1 : 0,
        dispatch->runtime_version,
        dispatch->runtime_name.size(),
        runtime_name_hash(dispatch->runtime_name));

    try {
        std::scoped_lock lock(g_state_mutex);
        g_instances[created_instance] = dispatch;
    } catch (const std::bad_alloc&) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_instance(created_instance);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *instance = created_instance;
    return result;
}

XrResult layer_destroy_instance_impl(XrInstance instance) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->destroy_instance == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    for (const auto& session_state : find_sessions(dispatch)) {
        stop_continuous_presenter(session_state);
        session_state->fps_overlay.reset();
    }
    for (const auto& swapchain_state : find_swapchains(dispatch)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    const XrResult result = dispatch->destroy_instance(instance);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_sessions.begin(); iterator != g_sessions.end();) {
            if (iterator->second->dispatch == dispatch) {
                iterator = g_sessions.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session->dispatch == dispatch) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_instances.erase(instance);
    }
    return result;
}

XrResult layer_create_session_impl(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto dispatch = find_dispatch(instance);
    if (!dispatch || dispatch->create_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (session == nullptr) {
        return dispatch->create_session(instance, create_info, session);
    }

    auto state = std::make_shared<SessionState>(dispatch);
    const auto initial_control = xrfg::embedded::snapshot();
    state->optical_flow_backend = static_cast<xrfg::D3D12OpticalFlowBackend>(initial_control.desired.backend);
    state->nvidia_options = {
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(initial_control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(initial_control.desired.scale),
        initial_control.desired.backward};
    state->menu_enabled = initial_control.desired.enabled;
    state->dlss_motion_vectors = initial_control.desired.motion_vectors == 1;
    state->control_revision = initial_control.revision;
    xrfg::embedded::applied(state->control_id, state->control_revision, state->menu_enabled, 0);
    XrStructureType binding_structure_type = XR_TYPE_UNKNOWN;
    if (create_info != nullptr) {
        auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
        while (next != nullptr) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                const auto* binding =
                    reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d11;
                binding_structure_type = next->type;
                state->d3d11_device = binding->device;
                if (state->d3d11_device) {
                    state->d3d11_device->GetImmediateContext(
                        state->d3d11_context.ReleaseAndGetAddressOf());
                    Microsoft::WRL::ComPtr<ID3D11Device5> device5;
                    if (SUCCEEDED(state->d3d11_device.As(&device5))) {
                        state->graphics_binding_capabilities |= 1ULL;
                    }
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4;
                    if (state->d3d11_context &&
                        SUCCEEDED(state->d3d11_context.As(&context4))) {
                        state->graphics_binding_capabilities |= 2ULL;
                    }
                }
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
                const auto* binding = reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(next);
                state->graphics_binding = SessionGraphicsBinding::d3d12;
                binding_structure_type = next->type;
                state->d3d12_device = binding->device;
                state->d3d12_queue = binding->queue;
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                state->graphics_binding = SessionGraphicsBinding::vulkan;
                binding_structure_type = next->type;
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
                state->graphics_binding = SessionGraphicsBinding::opengl;
                binding_structure_type = next->type;
                break;
            }
            next = next->next;
        }
    }

    XrSession created_session = XR_NULL_HANDLE;
    const XrResult result = dispatch->create_session(instance, create_info, &created_session);
    if (XR_FAILED(result)) {
        return result;
    }
    state->handle = created_session;
    // Optional instrumentation cannot fail an otherwise valid session.
    try {
        state->fps_overlay = std::make_unique<xrfg::OpenXrFpsOverlay>(
            instance, created_session, create_info ? create_info->systemId : 0,
            dispatch->get_instance_proc_addr, dispatch->end_frame,
            state->d3d12_device.Get(), state->d3d12_queue.Get(),
            state->d3d11_device.Get(), current_layer_directory() / L"ofxr_bridge.ini");
    } catch (...) {}
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state->graphics_binding_capabilities & 3ULL) == 3ULL) {
        const HRESULT bridge_device_result =
            xrfg::create_d3d12_device_for_d3d11(
                state->d3d11_device.Get(),
                state->d3d12_device.ReleaseAndGetAddressOf(),
                state->d3d12_queue.ReleaseAndGetAddressOf());
        if (SUCCEEDED(bridge_device_result)) {
            state->graphics_binding_capabilities |= 4ULL;
        } else {
            state->d3d12_device.Reset();
            state->d3d12_queue.Reset();
        }
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::session_binding,
        static_cast<std::int64_t>(state->graphics_binding),
        static_cast<std::uint64_t>(binding_structure_type) |
            (state->graphics_binding_capabilities << 32),
        state->d3d11_device
            ? handle_value(state->d3d11_device.Get())
            : handle_value(state->d3d12_device.Get()),
        state->d3d11_context
            ? handle_value(state->d3d11_context.Get())
            : handle_value(state->d3d12_queue.Get()));

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_sessions[created_session] = state;
        }
    } catch (const std::bad_alloc&) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        dispatch->destroy_session(created_session);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *session = created_session;
    return result;
}

XrResult layer_destroy_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->destroy_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    state->fps_overlay.reset();
    for (const auto& swapchain_state : find_swapchains(state)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    const XrResult result = state->dispatch->destroy_session(session);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session == state) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_sessions.erase(session);
    }
    return result;
}

void reset_frame_bookkeeping(const std::shared_ptr<SessionState>& state) {
    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    {
        std::scoped_lock frame_call_lock(state->frame_call_mutex);
        state->application_wait_pending_begin = false;
        state->application_frame_in_progress = false;
        state->application_frame_has_overlapping_wait = false;
        state->pipelined_wait_streak = 0;
        state->pipelined_presenter_mode = false;
        state->pipelined_presenter_start_requested = false;
        state->steamvr_presenter_start_requested = false;
        state->last_inline_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        state->last_inline_frame_state_valid = false;
        state->generation_steady_state_established = false;
    }
    state->frame_call_condition.notify_all();
    {
        std::scoped_lock presenter_lock(state->presenter_mutex);
        state->last_virtual_display_time = 0;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->pending_frames.clear();
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        state->generation_resume_wall_time = {};
        state->minimum_runtime_display_period = 0;
        state->steamvr_throttled_wait_streak = 0;
    }
}

void reset_swapchain_bookkeeping(const std::shared_ptr<SessionState>& session) noexcept {
    try {
        for (const auto& state : find_swapchains(session)) {
            std::scoped_lock call_lock(state->call_mutex);
            std::scoped_lock gpu_lock(session->gpu_mutex);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(state->mutex);
                state->acquired_indices.clear();
                state->front_waited = false;
                state->ownership_tracking_valid = true;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                history = state->d3d12_history;
                generation = state->frame_generation;
            }
            if (generation && generation->synthesizer) {
                static_cast<void>(generation->synthesizer->wait_for_idle());
            }
            if (history) {
                static_cast<void>(history->invalidate());
            }
            if (generation && generation->d3d11_interop) {
                static_cast<void>(generation->d3d11_interop->wait_for_idle());
            }
        }
    } catch (...) {
    }
}

XrResult layer_begin_session_impl(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    const XrResult result = state->dispatch->begin_session(session, begin_info);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

XrResult layer_end_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    const XrResult result = state->dispatch->end_session(session);
    if (XR_SUCCEEDED(result)) {
        reset_frame_bookkeeping(state);
        reset_swapchain_bookkeeping(state);
    }
    return result;
}

// frameWaitInfo and frameBeginInfo are optional="true" in the OpenXR registry,
// so a null pointer is valid input that a runtime must accept, and only a
// non-null one carries a type worth checking. This matters wherever the layer
// answers a frame call itself instead of forwarding it: the pass-through path
// hands the pointer to the runtime, which accepts null, while the virtual wait
// and begin used in presenter and pipelined modes have to accept it too.
// Rejecting null there fails applications that pass it and only on the runtimes
// that promote a presenter -- Luke Ross's mods call xrWaitFrame(session, NULL,
// &state) and drop back to 2D on the XR_ERROR_VALIDATION_FAILURE.
template <typename Info>
[[nodiscard]] bool valid_optional_frame_info(
    const Info* info,
    XrStructureType expected) noexcept {
    return info == nullptr || info->type == expected;
}

XrResult layer_wait_frame_impl(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto state = find_session(session);
    if (!state || state->dispatch->wait_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::unique_lock frame_call_lock(state->frame_call_mutex);
    state->frame_call_condition.wait(frame_call_lock, [&] {
        return !state->application_wait_pending_begin;
    });
    XrResult result = XR_SUCCESS;
    const bool use_continuous_presenter = continuous_presenter_active(state);
    bool use_provisional_pipelined_wait = false;
    if (!use_continuous_presenter && state->application_frame_in_progress) {
        if (!state->application_frame_has_overlapping_wait) {
            state->application_frame_has_overlapping_wait = true;
            ++state->pipelined_wait_streak;
        }
        if (state->pipelined_wait_streak >= 2 &&
            state->last_inline_frame_state_valid) {
            // The current runtime frame is still owned by the render thread,
            // so the transition cannot start its presenter until xrEndFrame
            // closes that boundary. Return the first virtual wait now and
            // preserve its timeline when the presenter starts at that end.
            state->pipelined_presenter_mode = true;
            state->pipelined_presenter_start_requested = true;
            use_provisional_pipelined_wait = true;
        }
    }
    if (use_continuous_presenter) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::unique_lock presenter_lock(state->presenter_mutex);
        state->presenter_condition.wait(presenter_lock, [&] {
            return state->presenter_frame_state_valid ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        if (!state->presenter_frame_state_valid ||
            state->presenter_stop_requested) {
            return XR_ERROR_SESSION_NOT_RUNNING;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->presenter_frame_state.predictedDisplayPeriod
            : doubled_display_period(state->presenter_frame_state.predictedDisplayPeriod);
        XrTime virtual_time = add_display_duration(
            state->presenter_frame_state.predictedDisplayTime,
            virtual_period);
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = add_display_duration(
                state->last_virtual_display_time,
                virtual_period);
        }
        state->last_virtual_display_time = virtual_time;
        frame_state->predictedDisplayTime = virtual_time;
        frame_state->predictedDisplayPeriod = virtual_period;
        frame_state->shouldRender = state->presenter_frame_state.shouldRender;
    } else if (use_provisional_pipelined_wait) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->last_inline_frame_state.predictedDisplayPeriod
            : doubled_display_period(state->last_inline_frame_state.predictedDisplayPeriod);
        XrTime virtual_time = add_display_duration(
            state->last_inline_frame_state.predictedDisplayTime,
            virtual_period);
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = add_display_duration(
                    state->last_virtual_display_time,
                    virtual_period);
            }
            state->last_virtual_display_time = virtual_time;
        }
        frame_state->predictedDisplayTime = virtual_time;
        frame_state->predictedDisplayPeriod = virtual_period;
        frame_state->shouldRender =
            state->last_inline_frame_state.shouldRender;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            100,
            state->pipelined_wait_streak,
            static_cast<std::uint64_t>(virtual_time),
            static_cast<std::uint64_t>(virtual_period));
    } else {
        result = state->dispatch->wait_frame(session, wait_info, frame_state);
        if (XR_SUCCEEDED(result) && frame_state != nullptr) {
            state->last_inline_frame_state = *frame_state;
            state->last_inline_frame_state.next = nullptr;
            state->last_inline_frame_state_valid = true;
        }
    }
    if (XR_SUCCEEDED(result) && frame_state != nullptr) {
        // V090 replaces V089's late submit-time wait with a frame-start gate.
        // This is after runtime pacing but before the application can record
        // or enqueue its next game/NGX workload. The submit path now performs
        // only a nonblocking check and drops generation while the same fence
        // remains pending, so a timeout cannot grow the queue.
        static_cast<void>(wait_for_previous_session_synthesis(state));
        state->application_wait_pending_begin = true;
        std::scoped_lock lock(state->mutex);
        if (state->dispatch->steamvr_runtime &&
            !use_continuous_presenter &&
            frame_state->predictedDisplayPeriod > 0 &&
            (state->minimum_runtime_display_period == 0 ||
             frame_state->predictedDisplayPeriod <
                 state->minimum_runtime_display_period)) {
            state->minimum_runtime_display_period =
                frame_state->predictedDisplayPeriod;
        }
        state->pending_frames.push_back({
            frame_state->predictedDisplayTime,
            frame_state->predictedDisplayPeriod,
        });
        constexpr std::size_t kMaximumPendingFrameRecords = 32;
        while (state->pending_frames.size() > kMaximumPendingFrameRecords) {
            state->pending_frames.pop_front();
        }
    }
    return result;
}

XrResult layer_begin_frame_impl(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        if (continuous_presenter_active(state)) {
            result = valid_optional_frame_info(
                    begin_info, XR_TYPE_FRAME_BEGIN_INFO)
                ? XR_SUCCESS
                : XR_ERROR_VALIDATION_FAILURE;
        } else if (state->pipelined_presenter_mode) {
            result = XR_ERROR_RUNTIME_FAILURE;
        } else {
            result = state->dispatch->begin_frame(session, begin_info);
        }
    } catch (...) {
        if (state->application_wait_pending_begin) {
            state->application_wait_pending_begin = false;
            frame_call_lock.unlock();
            state->frame_call_condition.notify_all();
        }
        throw;
    }
    if (XR_SUCCEEDED(result)) {
        state->application_frame_in_progress = true;
        state->application_frame_has_overlapping_wait = false;
    }
    if (state->application_wait_pending_begin) {
        state->application_wait_pending_begin = false;
        frame_call_lock.unlock();
        state->frame_call_condition.notify_all();
    }
    return result;
}

XrResult layer_create_swapchain_impl(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    const auto state = find_session(session);
    if (!state || state->dispatch->create_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (create_info == nullptr || swapchain == nullptr) {
        return state->dispatch->create_swapchain(session, create_info, swapchain);
    }

    PresenterResourceLifetimeGuard presenter_guard(state);
    const bool active_color_reconfiguration =
        state->generation_steady_state_established &&
        (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (create_info->usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    auto swapchain_state = std::make_shared<SwapchainState>(state, *create_info);
    XrSwapchain created_swapchain = XR_NULL_HANDLE;
    const XrResult result = state->dispatch->create_swapchain(
        session,
        create_info,
        &created_swapchain);
    if (XR_FAILED(result)) {
        return result;
    }
    swapchain_state->handle = created_swapchain;

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        0,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->createFlags),
        static_cast<std::uint64_t>(create_info->usageFlags));
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        1,
        handle_value(created_swapchain),
        (static_cast<std::uint64_t>(create_info->width) << 32) |
            create_info->height,
        (static_cast<std::uint64_t>(create_info->arraySize) << 32) |
            create_info->sampleCount);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::swapchain_create,
        2,
        handle_value(created_swapchain),
        static_cast<std::uint64_t>(create_info->format),
        (static_cast<std::uint64_t>(create_info->faceCount) << 32) |
            create_info->mipCount);

    try {
        {
            std::scoped_lock lock(g_state_mutex);
            g_swapchains[created_swapchain] = swapchain_state;
        }
    } catch (const std::bad_alloc&) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_OUT_OF_MEMORY;
    } catch (...) {
        state->dispatch->destroy_swapchain(created_swapchain);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *swapchain = created_swapchain;
    if (active_color_reconfiguration) {
        schedule_generation_quarantine(
            state,
            GenerationQuarantineReason::swapchain_created,
            handle_value(created_swapchain));
    }
    return result;
}

XrResult layer_destroy_swapchain_impl(XrSwapchain swapchain) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->destroy_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    PresenterResourceLifetimeGuard presenter_guard(state->session);
    const bool active_color_reconfiguration =
        state->session->generation_steady_state_established &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    if (active_color_reconfiguration) {
        schedule_generation_quarantine(
            state->session,
            GenerationQuarantineReason::swapchain_destroyed,
            handle_value(swapchain));
    }
    std::scoped_lock call_lock(state->call_mutex);
    drain_swapchain_gpu(state);
    destroy_frame_generation_swapchains(state);
    const XrResult result = state->session->dispatch->destroy_swapchain(swapchain);
    if (XR_SUCCEEDED(result)) {
        std::scoped_lock lock(g_state_mutex);
        g_swapchains.erase(swapchain);
    }
    return result;
}

XrResult layer_enumerate_swapchain_images_impl(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->enumerate_swapchain_images == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    // Resource/context creation must observe one applied menu configuration,
    // and no presenter-owned composition may retain the resources while an
    // application replaces them during a resize/reconfigure transaction.
    PresenterResourceLifetimeGuard presenter_guard(state->session);
    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = state->session->dispatch->enumerate_swapchain_images(
        swapchain,
        image_capacity_input,
        image_count_output,
        images);
    if (XR_FAILED(result) || image_count_output == nullptr || images == nullptr || image_capacity_input == 0) {
        return result;
    }

    try {
        if (state->session->graphics_binding == SessionGraphicsBinding::d3d11) {
            if (*image_count_output == 0 ||
                image_capacity_input < *image_count_output) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::incomplete_enumeration,
                    image_capacity_input,
                    *image_count_output);
                return result;
            }
            const std::uint32_t count = *image_count_output;
            std::vector<ID3D11Texture2D*> resources(count);
            auto* d3d11_images =
                reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
            for (std::uint32_t index = 0; index < count; ++index) {
                if (d3d11_images[index].type !=
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR ||
                    d3d11_images[index].texture == nullptr) {
                    log_swapchain_eligibility(
                        state,
                        SwapchainEligibilityReason::invalid_d3d11_image,
                        index,
                        static_cast<std::uint64_t>(d3d11_images[index].type));
                    return result;
                }
                resources[index] = d3d11_images[index].texture;
                D3D11_TEXTURE2D_DESC description{};
                d3d11_images[index].texture->GetDesc(&description);
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::swapchain_image,
                    static_cast<std::int64_t>(description.Format),
                    handle_value(state->handle),
                    handle_value(d3d11_images[index].texture),
                    (static_cast<std::uint64_t>(description.BindFlags) << 32) |
                        description.MiscFlags);
            }

            bool has_generation = false;
            bool resources_changed = false;
            {
                std::scoped_lock lock(state->mutex);
                resources_changed = !state->enumerated_d3d11_images.empty() &&
                    (state->enumerated_d3d11_images.size() != resources.size() ||
                     !std::equal(
                         state->enumerated_d3d11_images.begin(),
                         state->enumerated_d3d11_images.end(),
                         resources.begin(),
                         [](const auto& stored, ID3D11Texture2D* current) {
                             return stored.Get() == current;
                         }));
            }
            if (resources_changed) {
                schedule_generation_quarantine(
                    state->session,
                    GenerationQuarantineReason::d3d11_images_changed,
                    handle_value(state->handle));
                drain_swapchain_gpu(state);
                destroy_frame_generation_swapchains(state);
                std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
                {
                    std::scoped_lock lock(state->mutex);
                    retired_history = std::move(state->d3d12_history);
                    state->last_released_capture.reset();
                    state->last_released_motion_vectors.reset();
                }
                retired_history.reset();
            }
            {
                std::scoped_lock lock(state->mutex);
                state->enumerated_d3d11_images.clear();
                for (auto* resource : resources) state->enumerated_d3d11_images.emplace_back(resource);
                has_generation = static_cast<bool>(state->frame_generation);
            }
            SwapchainEligibilityReason eligibility_reason =
                SwapchainEligibilityReason::ready;
            std::uint64_t eligibility_detail = count;
            if (!has_generation) {
                std::uint64_t generation_detail = 0;
                auto candidate = create_d3d11_frame_generation_swapchains(
                    state,
                    std::span<ID3D11Texture2D* const>(
                        resources.data(), resources.size()),
                    &eligibility_reason,
                    &generation_detail);
                if (candidate) {
                    std::scoped_lock lock(state->mutex);
                    if (!state->frame_generation) {
                        state->frame_generation = std::move(candidate);
                    }
                    has_generation = static_cast<bool>(state->frame_generation);
                } else {
                    eligibility_detail = generation_detail;
                }
            }
            log_swapchain_eligibility(
                state,
                has_generation ? SwapchainEligibilityReason::ready
                               : eligibility_reason,
                eligibility_detail,
                (static_cast<std::uint64_t>(count) << 32) |
                    state->create_info.arraySize);
            return result;
        }
        if (state->session->d3d12_device.Get() == nullptr ||
            state->session->d3d12_queue.Get() == nullptr) {
            log_swapchain_eligibility(
                state, SwapchainEligibilityReason::no_d3d12_binding);
            return result;
        }
        if (*image_count_output == 0 || image_capacity_input < *image_count_output) {
            log_swapchain_eligibility(
                state,
                SwapchainEligibilityReason::incomplete_enumeration,
                image_capacity_input,
                *image_count_output);
            return result;
        }
        const std::uint32_t count = *image_count_output;

        std::vector<ID3D12Resource*> resources(count);
        auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (d3d12_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR ||
                d3d12_images[index].texture == nullptr) {
                log_swapchain_eligibility(
                    state,
                    SwapchainEligibilityReason::invalid_d3d12_image,
                    index,
                    static_cast<std::uint64_t>(d3d12_images[index].type));
                return result;
            }
            resources[index] = d3d12_images[index].texture;
        }
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        bool resources_changed = false;
        {
            std::scoped_lock lock(state->mutex);
            history = state->d3d12_history;
            resources_changed = !state->enumerated_d3d12_images.empty() &&
                (state->enumerated_d3d12_images.size() != resources.size() ||
                 !std::equal(
                     state->enumerated_d3d12_images.begin(),
                     state->enumerated_d3d12_images.end(),
                     resources.begin(),
                     [](const auto& stored, ID3D12Resource* current) {
                         return stored.Get() == current;
                     }));
            state->enumerated_d3d12_images.clear();
            for (auto* resource : resources) state->enumerated_d3d12_images.emplace_back(resource);
        }
        if (resources_changed) {
            schedule_generation_quarantine(
                state->session,
                GenerationQuarantineReason::d3d12_images_changed,
                handle_value(state->handle));
        }
        const bool reused_history =
            !resources_changed && history && history->initialized();
        if (!reused_history) {
            history.reset();
        }
        const auto release_state = required_release_state(*state);
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        HRESULT history_result = S_OK;
        bool history_attempted = false;
        if (!history && release_state && !protected_content) {
            history_attempted = true;
            auto candidate = std::make_shared<xrfg::D3D12SwapchainHistory>();
            history_result = candidate->initialize(
                state->session->d3d12_device.Get(),
                state->session->d3d12_queue.Get(),
                std::span<ID3D12Resource* const>(resources.data(), resources.size()),
                *release_state);
            if (SUCCEEDED(history_result)) {
                history = std::move(candidate);
            }
        }
        if (!reused_history) {
            drain_swapchain_gpu(state);
            destroy_frame_generation_swapchains(state);
            std::shared_ptr<xrfg::D3D12SwapchainHistory> retired_history;
            {
                std::scoped_lock lock(state->mutex);
                retired_history = std::move(state->d3d12_history);
                state->d3d12_history = history;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
            retired_history.reset();
        }

        bool has_generation = false;
        {
            std::scoped_lock lock(state->mutex);
            has_generation = static_cast<bool>(state->frame_generation);
        }
        SwapchainEligibilityReason eligibility_reason =
            SwapchainEligibilityReason::ready;
        std::uint64_t eligibility_detail = count;
        if (!release_state) {
            eligibility_reason =
                SwapchainEligibilityReason::ambiguous_attachment_usage;
            eligibility_detail = state->create_info.usageFlags;
        } else if (protected_content) {
            eligibility_reason = SwapchainEligibilityReason::protected_content;
            eligibility_detail = state->create_info.createFlags;
        } else if (!history) {
            eligibility_reason = SwapchainEligibilityReason::history_initialize_failed;
            eligibility_detail = history_attempted
                ? static_cast<std::uint64_t>(history_result)
                : 0;
        } else if (*release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        }
        if (history && release_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
            !has_generation) {
            SwapchainEligibilityReason generation_reason =
                SwapchainEligibilityReason::exception;
            std::uint64_t generation_detail = 0;
            auto candidate = create_d3d12_frame_generation_swapchains(
                state, &generation_reason, &generation_detail);
            if (candidate) {
                std::scoped_lock lock(state->mutex);
                if (!state->frame_generation) {
                    state->frame_generation = std::move(candidate);
                }
                has_generation = static_cast<bool>(state->frame_generation);
            } else {
                eligibility_reason = generation_reason;
                eligibility_detail = generation_detail;
            }
        }

        log_swapchain_eligibility(
            state,
            has_generation ? SwapchainEligibilityReason::ready
                           : eligibility_reason,
            eligibility_detail,
            (static_cast<std::uint64_t>(count) << 32) |
                state->create_info.arraySize);

    } catch (...) {
        log_swapchain_eligibility(
            state, SwapchainEligibilityReason::exception);
    }
    return result;
}

XrResult layer_acquire_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->acquire_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = state->session->dispatch->acquire_swapchain_image(
        swapchain,
        acquire_info,
        index);
    if (XR_SUCCEEDED(result) && index != nullptr) {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid) {
            try {
                state->acquired_indices.push_back(*index);
            } catch (...) {
                state->acquired_indices.clear();
                state->front_waited = false;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }
    }
    return result;
}

XrResult layer_wait_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->wait_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    const XrResult result = state->session->dispatch->wait_swapchain_image(swapchain, wait_info);
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid) {
            if (!state->front_waited && !state->acquired_indices.empty()) {
                state->front_waited = true;
            } else {
                state->acquired_indices.clear();
                state->front_waited = false;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }
    }
    return result;
}

XrResult layer_release_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->release_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    std::optional<std::uint32_t> candidate_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
    {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid && state->front_waited &&
            !state->acquired_indices.empty()) {
            candidate_index = state->acquired_indices.front();
            history = state->d3d12_history;
            if (state->frame_generation) {
                d3d11_interop = state->frame_generation->d3d11_interop;
            }
        }
    }

    std::unique_lock<std::mutex> gpu_lock;
    if (state->session->manual_control.stop_requested() || !state->session->menu_enabled) {
        // Preserve the application's acquire/wait/release bookkeeping, but
        // stop recording new bridge history after the manual arm is revoked.
        history.reset();
        d3d11_interop.reset();
    }
    if (candidate_index && history) {
        gpu_lock = std::unique_lock<std::mutex>(state->session->gpu_mutex);
    }

    std::optional<xrfg::D3D12HistoryCaptureTicket> pending_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> pending_motion_vectors;
    if (candidate_index && history) {
        if (state->session->dlss_motion_vectors) {
            std::scoped_lock lock(state->mutex);
            if (*candidate_index < state->enumerated_d3d12_images.size()) {
                pending_motion_vectors = xrfg::resolve_dlss_motion_vectors(
                    state->enumerated_d3d12_images[*candidate_index].Get(),
                    state->session->d3d12_queue.Get());
            }
        }
        xrfg::D3D12HistoryCaptureTicket ticket{};
        HRESULT capture_result = S_OK;
        if (d3d11_interop) {
            const auto interop_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::d3d11_capture,
                handle_value(state->handle),
                *candidate_index,
                0);
            capture_result = d3d11_interop->prepare_capture(*candidate_index);
            xrfg::bridge_flight_logger().end(
                interop_token,
                xrfg::BridgeFlightOperation::d3d11_capture,
                capture_result,
                handle_value(state->handle),
                *candidate_index,
                0);
        }
        if (SUCCEEDED(capture_result)) {
            capture_result = history->capture(*candidate_index, &ticket);
        }
        if (SUCCEEDED(capture_result) && d3d11_interop) {
            const HRESULT finish_result = d3d11_interop->finish_capture();
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_capture,
                finish_result,
                handle_value(state->handle),
                *candidate_index,
                1);
            capture_result = finish_result;
        }
        if (SUCCEEDED(capture_result)) {
            pending_capture = ticket;
        } else if (ticket.serial != 0) {
            history->discard(ticket);
        }
    }

    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        result = state->session->dispatch->release_swapchain_image(swapchain, release_info);
    } catch (...) {
        if (pending_capture && history) {
            history->discard(*pending_capture);
        }
        throw;
    }

    if (XR_SUCCEEDED(result)) {
        bool commit_capture = false;
        {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->front_waited &&
                !state->acquired_indices.empty()) {
                const std::uint32_t released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                state->front_waited = false;
                state->last_released_index = released_index;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                commit_capture = pending_capture &&
                                 pending_capture->source_index == released_index;
            } else if (state->ownership_tracking_valid) {
                state->acquired_indices.clear();
                state->front_waited = false;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->ownership_tracking_valid = false;
            }
        }

        if (pending_capture && history) {
            const bool committed =
                commit_capture && SUCCEEDED(history->commit(*pending_capture));
            if (committed) {
                std::scoped_lock lock(state->mutex);
                state->last_released_capture = pending_capture;
                state->last_released_motion_vectors = pending_motion_vectors;
            } else {
                history->discard(*pending_capture);
                std::scoped_lock lock(state->mutex);
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
            }
        }
    } else if (pending_capture && history) {
        history->discard(*pending_capture);
    }
    return result;
}

struct ProjectionLayerCopy {
    std::uint32_t layer_index{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> views;
};

using OwnedCompositionLayer = std::variant<
    XrCompositionLayerQuad,
    XrCompositionLayerCubeKHR,
    XrCompositionLayerCylinderKHR,
    XrCompositionLayerEquirectKHR,
    XrCompositionLayerEquirect2KHR,
    XrCompositionLayerPassthroughFB,
    XrCompositionLayerPassthroughHTC,
    XrCompositionLayerPassthroughANDROID>;

struct GeneratedFrameEndInfo {
    bool synthetic{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<ProjectionLayerCopy> projections;
    std::vector<OwnedCompositionLayer> composition_layers;
    std::vector<const XrCompositionLayerBaseHeader*> layer_pointers;
};

[[nodiscard]] std::optional<OwnedCompositionLayer>
copy_composition_layer_for_presenter(
    const XrCompositionLayerBaseHeader* source) {
    if (source == nullptr) {
        return std::nullopt;
    }

#define XRFG_COPY_COMPOSITION_LAYER(structure_type, structure_name)            \
    case structure_type: {                                                    \
        structure_name copy =                                                 \
            *reinterpret_cast<const structure_name*>(source);                 \
        copy.next = nullptr;                                                  \
        return OwnedCompositionLayer{copy};                                   \
    }
    switch (source->type) {
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_QUAD,
            XrCompositionLayerQuad)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CUBE_KHR,
            XrCompositionLayerCubeKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,
            XrCompositionLayerCylinderKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR,
            XrCompositionLayerEquirectKHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,
            XrCompositionLayerEquirect2KHR)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB,
            XrCompositionLayerPassthroughFB)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_HTC,
            XrCompositionLayerPassthroughHTC)
        XRFG_COPY_COMPOSITION_LAYER(
            XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_ANDROID,
            XrCompositionLayerPassthroughANDROID)
        default:
            return std::nullopt;
    }
#undef XRFG_COPY_COMPOSITION_LAYER
}

[[nodiscard]] const XrCompositionLayerBaseHeader* composition_layer_header(
    OwnedCompositionLayer& layer) noexcept {
    return std::visit(
        [](auto& value) {
            return reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &value);
        },
        layer);
}

[[nodiscard]] std::shared_ptr<GeneratedFrameEndInfo>
make_presenter_owned_frame(GeneratedFrameEndInfo&& source) {
    if (source.info.next != nullptr || source.projections.empty() ||
        source.info.layerCount != source.layer_pointers.size()) {
        return nullptr;
    }

    auto output = std::make_shared<GeneratedFrameEndInfo>(std::move(source));
    std::vector<bool> projection_indices(output->layer_pointers.size(), false);
    for (ProjectionLayerCopy& projection : output->projections) {
        if (projection.layer_index >= output->layer_pointers.size() ||
            projection.views.empty() ||
            projection_indices[projection.layer_index]) {
            return nullptr;
        }
        projection_indices[projection.layer_index] = true;
        projection.layer.next = nullptr;
        for (XrCompositionLayerProjectionView& view : projection.views) {
            view.next = nullptr;
        }
        projection.layer.views = projection.views.data();
        output->layer_pointers[projection.layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &projection.layer);
    }

    output->composition_layers.reserve(
        output->layer_pointers.size() - output->projections.size());
    for (std::size_t index = 0; index < output->layer_pointers.size(); ++index) {
        if (projection_indices[index]) {
            continue;
        }
        auto layer = copy_composition_layer_for_presenter(
            output->layer_pointers[index]);
        if (!layer) {
            return nullptr;
        }
        output->composition_layers.push_back(std::move(*layer));
        output->layer_pointers[index] = composition_layer_header(
            output->composition_layers.back());
    }

    output->info.next = nullptr;
    output->info.layers = output->layer_pointers.data();
    return output;
}

[[nodiscard]] XrDuration doubled_display_period(XrDuration period) noexcept {
    if (period <= 0) {
        return period;
    }
    constexpr XrDuration maximum = std::numeric_limits<XrDuration>::max();
    return period > maximum / 2 ? maximum : period * 2;
}

[[nodiscard]] XrTime add_display_duration(
    XrTime time,
    XrDuration duration) noexcept {
    if (duration <= 0) {
        return time;
    }
    constexpr XrTime maximum = std::numeric_limits<XrTime>::max();
    return time > maximum - duration ? maximum : time + duration;
}

void fail_pending_presenter_submissions_locked(
    SessionState& state,
    XrResult failure) noexcept {
    if (XR_FAILED(failure) && XR_SUCCEEDED(state.presenter_failure)) {
        state.presenter_failure = failure;
    }
    for (const auto& request : state.presenter_submissions) {
        request->result = failure;
        request->completed = true;
    }
    state.presenter_submissions.clear();
    state.outstanding_presenter_submissions = 0;
}

// Holds the presenter to one submission per display period.
//
// The layer submits two frames for every application frame and relies on
// xrWaitFrame to space them, one per scanout. That holds only while the
// runtime actually throttles the wait. Measured against MSFS 2024: VDXR
// blocks it for a metronomic 9.95-9.97 ms and the pair goes out 11.11 ms
// apart, exactly as intended; SteamVR with the Pimax driver returns it in
// 1.55 ms, so the presenter free-runs and submits the pair 1.12 ms apart -
// both inside one 11.11 ms window - followed by a 21.1 ms gap.
//
// A compositor holding one submitted frame at a time then never scans out the
// first of each pair: the second replaces it. Half the generated frames are
// discarded before they are ever displayed, which is why the layer's own
// overlay reported a steady 90 while the compositor reported 10% reprojection
// and about 80 FPS, and why neither pipeline depth nor synthesis readiness
// moved the result - the frames were being dropped for their timing, never
// for their contents.
//
// Pacing here rather than trusting the wait costs nothing where the wait
// already paces: the elapsed check passes immediately and the runtime's own
// blocking still sets the cadence. The wait is interruptible, and it happens
// before the runtime frame cycle is entered so no begun frame is held open
// across it. It must not run under presenter_content_mutex - waiting for
// presenter progress while holding that lock has deadlocked this layer twice.
void pace_presenter_submission(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        const auto entered = std::chrono::steady_clock::now();
        std::int64_t behind_us = 0;
        std::chrono::nanoseconds remaining{0};
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (!state->presenter_schedule_valid ||
                state->presenter_display_period <= 0 ||
                state->presenter_stop_requested) {
                return;
            }
            const auto period = std::chrono::nanoseconds(
                static_cast<std::int64_t>(state->presenter_display_period));
            auto ready_at = state->presenter_next_submit;
            behind_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    entered - ready_at)
                    .count();
            if (ready_at > entered) {
                // Never hold longer than one period, whatever the schedule
                // says. Pacing exists to stop submissions bunching up; it must
                // never be able to hold the presenter back instead.
                ready_at = std::min(ready_at, entered + period);
                remaining = ready_at - entered;
            }
        }
        // Slept without the lock: the application thread enqueues against this
        // mutex, and a paced presenter holding it would stall the very frame
        // it is waiting for.
        if (remaining > std::chrono::nanoseconds::zero()) {
            if (state->presenter_pace_timer == nullptr) {
                state->presenter_pace_timer = CreateWaitableTimerExW(
                    nullptr,
                    nullptr,
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                    TIMER_ALL_ACCESS);
                if (state->presenter_pace_timer == nullptr) {
                    // Pre-1803, or the flag was refused. A coarse timer still
                    // beats the condition variable's tick rounding.
                    state->presenter_pace_timer = CreateWaitableTimerExW(
                        nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                }
            }
            if (state->presenter_pace_timer != nullptr) {
                LARGE_INTEGER due{};
                // Negative is relative, in 100 ns units.
                due.QuadPart = -(remaining.count() / 100);
                if (SetWaitableTimer(
                        state->presenter_pace_timer,
                        &due,
                        0,
                        nullptr,
                        nullptr,
                        FALSE)) {
                    static_cast<void>(WaitForSingleObject(
                        state->presenter_pace_timer, INFINITE));
                }
            }
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pace,
            0,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            static_cast<std::uint64_t>(behind_us < 0 ? -behind_us : behind_us),
            behind_us > 0 ? 1u : 0u);
    } catch (...) {
    }
}

void continuous_presenter_main(
    const std::shared_ptr<SessionState>& state) noexcept {
    for (;;) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (state->presenter_stop_requested) {
                break;
            }
        }

        pace_presenter_submission(state);

        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        XrResult wait_result = XR_SUCCESS;
        // XR_ERROR_CALL_ORDER_INVALID is the one xrWaitFrame failure the
        // spec describes as a sequencing hazard rather than a lost
        // session/instance: it can surface if the runtime has not yet
        // finished processing the previous wait/begin/end cycle. A handful
        // of short, bounded retries lets the presenter recover from that
        // transient instead of permanently stopping generation for the
        // rest of the session, which is the same class of "never recovers
        // after a momentary hiccup" symptom already fixed elsewhere for the
        // virtual-time clock. Every other failure (session lost/not
        // running, instance lost, ...) is treated as before: fatal to this
        // presenter loop.
        constexpr int kCallOrderRetryLimit = 5;
        for (int attempt = 0; attempt <= kCallOrderRetryLimit; ++attempt) {
            frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
            const auto wait_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_wait_frame,
                handle_value(state->handle));
            wait_result = state->dispatch->wait_frame(
                state->handle,
                &wait_info,
                &frame_state);
            xrfg::bridge_flight_logger().end(
                wait_token,
                xrfg::BridgeFlightOperation::internal_wait_frame,
                wait_result,
                static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
                frame_state.shouldRender);
            if (wait_result != XR_ERROR_CALL_ORDER_INVALID ||
                attempt == kCallOrderRetryLimit) {
                break;
            }
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (state->presenter_stop_requested) {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (XR_FAILED(wait_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, wait_result);
            state->presenter_condition.notify_all();
            break;
        }

        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_frame_state = frame_state;
            state->presenter_frame_state.next = nullptr;
            state->presenter_frame_state_valid = true;
            // Keep the smallest plausible period seen, so a runtime that
            // inflates the value under load cannot inflate the pace with it.
            constexpr XrDuration kShortestCredibleDisplayPeriod = 2'000'000;
            constexpr XrDuration kLongestCredibleDisplayPeriod = 50'000'000;
            if (frame_state.predictedDisplayPeriod >=
                    kShortestCredibleDisplayPeriod &&
                frame_state.predictedDisplayPeriod <=
                    kLongestCredibleDisplayPeriod &&
                (state->presenter_display_period == 0 ||
                 frame_state.predictedDisplayPeriod <
                     state->presenter_display_period)) {
                state->presenter_display_period =
                    frame_state.predictedDisplayPeriod;
            }
        }
        state->presenter_condition.notify_all();

        // A successful runtime wait supplies the virtual application timing,
        // but do not begin a frame until there is valid composition to submit.
        // This prevents an unsupported first application layer list from being
        // alternated with empty presenter frames while still allowing the
        // virtualized application to advance and enqueue that list.
        {
            std::unique_lock lock(state->presenter_mutex);
            state->presenter_condition.wait(lock, [&] {
                return state->presenter_stop_requested ||
                       XR_FAILED(state->presenter_failure) ||
                       state->presenter_last_frame != nullptr ||
                       !state->presenter_submissions.empty();
            });
        }

        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const auto begin_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_begin_frame,
            handle_value(state->handle));
        const XrResult begin_result = state->dispatch->begin_frame(
            state->handle,
            &begin_info);
        xrfg::bridge_flight_logger().end(
            begin_token,
            xrfg::BridgeFlightOperation::internal_begin_frame,
            begin_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        if (XR_FAILED(begin_result)) {
            std::scoped_lock lock(state->presenter_mutex);
            fail_pending_presenter_submissions_locked(*state, begin_result);
            state->presenter_condition.notify_all();
            break;
        }

        std::shared_ptr<PresenterSubmission> request;
        std::shared_ptr<GeneratedFrameEndInfo> repeated_frame;
        XrResult end_result = XR_ERROR_RUNTIME_FAILURE;
        std::uint32_t submitted_layer_count = 0;
        {
            // Do not let the application release a newer private output while
            // the runtime is resolving the previous handle's last image.
            std::scoped_lock content_lock(state->presenter_content_mutex);
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (!state->presenter_stop_requested &&
                    !state->presenter_submissions.empty()) {
                    request = state->presenter_submissions.front();
                    state->presenter_submissions.pop_front();
                } else if (!state->presenter_stop_requested) {
                    repeated_frame = state->presenter_last_frame;
                }
            }

            const XrFrameEndInfo* source = request
                ? request->owned_frame
                    ? &request->owned_frame->info
                    : request->borrowed_frame
                : repeated_frame
                    ? &repeated_frame->info
                    : nullptr;
            XrFrameEndInfo submitted{XR_TYPE_FRAME_END_INFO};
            if (source != nullptr) {
                submitted = *source;
                submitted.next = nullptr;
                submitted.displayTime = frame_state.predictedDisplayTime;
            } else {
                submitted.displayTime = frame_state.predictedDisplayTime;
                submitted.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            }
            if (frame_state.shouldRender == XR_FALSE) {
                submitted.layerCount = 0;
                submitted.layers = nullptr;
            }
            submitted_layer_count = submitted.layerCount;
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_end_frame,
                handle_value(state->handle),
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount);
            const bool fresh_synthetic = request && request->owned_frame &&
                request->owned_frame->synthetic;
            if (state->fps_overlay && state->manual_control.stop_requested())
                state->fps_overlay->suspend();
            end_result = state->fps_overlay
                ? state->fps_overlay->end_frame(&submitted, fresh_synthetic)
                : state->dispatch->end_frame(state->handle, &submitted);
            {
                // Advance the schedule by exactly one period so this loop's
                // own cost does not compound into the cadence, and resync
                // rather than chase if a stall has put the schedule in the
                // past - catching up would submit a burst, which is the very
                // thing the pace exists to prevent.
                std::scoped_lock lock(state->presenter_mutex);
                const auto now = std::chrono::steady_clock::now();
                const auto period = std::chrono::nanoseconds(
                    static_cast<std::int64_t>(state->presenter_display_period));
                if (!state->presenter_schedule_valid ||
                    state->presenter_display_period <= 0) {
                    state->presenter_next_submit = now + period;
                    state->presenter_schedule_valid =
                        state->presenter_display_period > 0;
                } else {
                    state->presenter_next_submit += period;
                    // fresh full period for being late. Resetting to now+period
                    // instead made every cycle cost a period plus whatever the
                    // loop took, which is how a 11.11 ms pace produced 15.5 ms
                    // submissions and held the runtime to 64/s.
                    if (state->presenter_next_submit <= now) {
                        const auto behind = now - state->presenter_next_submit;
                        state->presenter_next_submit +=
                            (behind / period + 1) * period;
                    }
                    // Then step off any deadline that falls too soon after the
                    // frame just handed over. The schedule is an absolute grid
                    // and knows nothing about how long this submission took;
                    // when the runtime's own xrEndFrame ran long - 3.7 ms
                    // typical against 17 ms worst on SteamVR - the next grid
                    // point can be a couple of milliseconds away, so the pair
                    // lands inside one scanout window and the compositor keeps
                    // only the later one. That is the bunching the pace exists
                    // to prevent, reappearing after an overrun instead of at
                    // free-run: measured as a repeating on-grid, long, short
                    // cadence with 28.8% of gaps under 9 ms against an 11.11 ms
                    // period.
                    //
                    // Half a period is the threshold rather than a full one so
                    // this cannot fire in steady state, where the deadline
                    // already falls about 7 ms after the previous handover.
                    // Chaining every deadline off the handover would instead
                    // add the loop's own cost to each cycle, which is what held
                    // an earlier build to 64/s.
                    const auto earliest = now + period / 2;
                    while (state->presenter_next_submit < earliest) {
                        state->presenter_next_submit += period;
                    }
                }
            }
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::internal_end_frame,
                end_result,
                static_cast<std::uint64_t>(submitted.displayTime),
                submitted.layerCount,
                frame_state.shouldRender);
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_submission,
            end_result,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            submitted_layer_count);
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (request) {
                request->result = end_result;
                request->completed = true;
                if (state->outstanding_presenter_submissions != 0) {
                    --state->outstanding_presenter_submissions;
                }
                if (XR_SUCCEEDED(end_result) && request->owned_frame &&
                    !state->pipelined_presenter_mode) {
                    state->presenter_last_frame = request->owned_frame;
                } else if (state->pipelined_presenter_mode) {
                    // A persistent pipelined application may destroy or replace
                    // XrSpace and non-projection swapchain handles as soon as
                    // its xrEndFrame returns. Never repeat retained application
                    // handles beyond that call boundary; let the runtime hold
                    // the last submitted image until the next owned pair.
                    state->presenter_last_frame.reset();
                }
            }
            if (XR_FAILED(end_result)) {
                fail_pending_presenter_submissions_locked(*state, end_result);
            }
        }
        state->presenter_condition.notify_all();
        if (XR_FAILED(end_result)) {
            break;
        }
    }

    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_submissions.empty()) {
            const XrResult failure = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            fail_pending_presenter_submissions_locked(*state, failure);
        }
    }
    state->presenter_condition.notify_all();
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame,
    bool preserve_virtual_timeline) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_active) {
            return true;
        }
        state->presenter_submissions.clear();
        state->presenter_last_frame.reset();
        state->presenter_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        if (!preserve_virtual_timeline) {
            state->last_virtual_display_time = 0;
        }
        state->presenter_failure = XR_SUCCESS;
        state->next_presenter_sequence = 1;
        state->outstanding_presenter_submissions = 0;
        if (state->pipelined_presenter_mode && seed_frame) {
            auto request = std::make_shared<PresenterSubmission>();
            request->sequence = state->next_presenter_sequence++;
            request->owned_frame = std::move(seed_frame);
            state->presenter_submissions.push_back(std::move(request));
            state->outstanding_presenter_submissions = 1;
        } else {
            state->presenter_last_frame = std::move(seed_frame);
        }
        state->presenter_frame_state_valid = false;
        // A schedule left over from a previous presenter would stall the first
        // submission of this one.
        state->presenter_schedule_valid = false;
        state->presenter_display_period = 0;
        state->presenter_stop_requested = false;
        state->presenter_active = true;
        state->presenter_thread = std::thread(continuous_presenter_main, state);
        return true;
    } catch (...) {
        state->presenter_active = false;
        return false;
    }
}

void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept {
    if (!state) {
        return;
    }
    try {
        {
            std::unique_lock lock(state->presenter_mutex);
            if (!state->presenter_active) {
                return;
            }
            // Let a submission the application already handed over reach the
            // runtime instead of dropping it on the floor. The application no
            // longer waits for this queue to empty, so the last frame of a
            // session is normally still sitting in it. Bounded, because
            // teardown must not depend on the presenter being healthy.
            static_cast<void>(state->presenter_condition.wait_for(
                lock, std::chrono::milliseconds(100), [&] {
                    return state->outstanding_presenter_submissions == 0 ||
                           XR_FAILED(state->presenter_failure);
                }));
            state->presenter_stop_requested = true;
        }
        state->presenter_condition.notify_all();
        if (state->presenter_thread.joinable()) {
            // The presenter thread can be parked inside a blocking
            // state->dispatch->wait_frame() call to the OpenXR runtime (e.g.
            // SteamVR). A frozen or crashed compositor, or a headset that
            // was unplugged mid-session, can leave that call unreturned
            // indefinitely. presenter_stop_requested is only observed on
            // the next loop iteration or condition wait, never inside that
            // specific blocking call, so an unconditional join() here can
            // hang the application's own shutdown/session-destroy path.
            //
            // Give the thread a bounded chance to exit cleanly. If it does
            // not, hand the join off to a detached helper thread and return
            // anyway: state is kept alive by the presenter thread's own
            // shared_ptr copy, so the eventual exit (once wait_frame
            // returns, however late) remains safe even though this call no
            // longer waits for it.
            const auto join_completed = std::make_shared<std::atomic<bool>>(false);
            std::thread helper(
                [thread = std::move(state->presenter_thread),
                 join_completed]() mutable {
                    thread.join();
                    join_completed->store(true, std::memory_order_release);
                });
            constexpr auto kPresenterJoinBudget = std::chrono::milliseconds(500);
            const auto deadline =
                std::chrono::steady_clock::now() + kPresenterJoinBudget;
            while (!join_completed->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (join_completed->load(std::memory_order_acquire)) {
                helper.join();
            } else {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_transition,
                    -1,
                    handle_value(state->handle));
                helper.detach();
            }
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_active = false;
            state->presenter_frame_state_valid = false;
            state->presenter_last_frame.reset();
        }
        state->presenter_condition.notify_all();
    } catch (...) {
    }
}

[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::scoped_lock lock(state->presenter_mutex);
    return state->presenter_active && !state->presenter_stop_requested;
}

[[nodiscard]] XrResult wait_for_presenter_capacity(
    const std::shared_ptr<SessionState>& state,
    std::size_t maximum_outstanding) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions <=
                       maximum_outstanding ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] XrResult wait_for_presenter_idle(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->outstanding_presenter_submissions == 0 ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure)) {
            return state->presenter_failure;
        }
        return state->presenter_stop_requested
            ? XR_ERROR_SESSION_NOT_RUNNING
            : XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

PresenterResourceLifetimeGuard::PresenterResourceLifetimeGuard(
    const std::shared_ptr<SessionState>& state)
    : frame_lock(state->frame_call_mutex) {
    if (continuous_presenter_active(state)) {
        // The content lock must be acquired AFTER the drain: the presenter
        // needs it to complete the very submissions we are waiting for.
        static_cast<void>(wait_for_presenter_idle(state));
        content_lock = std::unique_lock<std::mutex>(state->presenter_content_mutex);
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
}

[[nodiscard]] std::shared_ptr<PresenterSubmission>
enqueue_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame,
    const XrFrameEndInfo* borrowed_frame) {
    auto request = std::make_shared<PresenterSubmission>();
    {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            request->result = XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
            request->completed = true;
            return request;
        }
        request->sequence = state->next_presenter_sequence++;
        request->owned_frame = std::move(owned_frame);
        request->borrowed_frame = borrowed_frame;
        state->presenter_submissions.push_back(request);
        ++state->outstanding_presenter_submissions;
    }
    state->presenter_condition.notify_all();
    return request;
}

[[nodiscard]] XrResult wait_for_presenter_submission(
    const std::shared_ptr<SessionState>& state,
    const std::shared_ptr<PresenterSubmission>& request) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return request->completed || XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        return request->completed
            ? request->result
            : XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

[[nodiscard]] XrResult enqueue_presenter_pair(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> synthetic,
    std::shared_ptr<GeneratedFrameEndInfo> current) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            return XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
        }
        auto first = std::make_shared<PresenterSubmission>();
        first->sequence = state->next_presenter_sequence++;
        first->owned_frame = std::move(synthetic);
        auto second = std::make_shared<PresenterSubmission>();
        second->sequence = state->next_presenter_sequence++;
        second->owned_frame = std::move(current);
        state->presenter_submissions.push_back(std::move(first));
        state->presenter_submissions.push_back(std::move(second));
        state->outstanding_presenter_submissions += 2;
        state->presenter_condition.notify_all();
        return XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
}

[[nodiscard]] bool capture_projection_snapshot(
    const XrFrameEndInfo* source,
    ProjectionSnapshot* output) {
    if (source == nullptr || output == nullptr || source->layerCount == 0 ||
        source->layers == nullptr) {
        return false;
    }

    ProjectionSnapshot snapshot{};
    snapshot.display_time = source->displayTime;
    snapshot.environment_blend_mode = source->environmentBlendMode;
    for (std::uint32_t layer_index = 0; layer_index < source->layerCount; ++layer_index) {
        const XrCompositionLayerBaseHeader* layer = source->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount == 0 || projection->views == nullptr ||
            projection->space == XR_NULL_HANDLE) {
            return false;
        }

        ProjectionLayerSnapshot stored{};
        stored.layer_index = layer_index;
        stored.layer_flags = projection->layerFlags;
        stored.space = projection->space;
        stored.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            const XrSwapchain application_swapchain =
                projection->views[view_index].subImage.swapchain;
            if (application_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            stored.views[view_index].next = nullptr;
        }
        snapshot.layers.push_back(std::move(stored));
    }
    if (snapshot.layers.empty()) {
        return false;
    }
    *output = std::move(snapshot);
    return true;
}

[[nodiscard]] bool matching_sub_image(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    return left.swapchain == right.swapchain &&
           left.imageArrayIndex == right.imageArrayIndex &&
           left.imageRect.offset.x == right.imageRect.offset.x &&
           left.imageRect.offset.y == right.imageRect.offset.y &&
           left.imageRect.extent.width == right.imageRect.extent.width &&
           left.imageRect.extent.height == right.imageRect.extent.height;
}

[[nodiscard]] bool overlapping_sub_images(
    const XrSwapchainSubImage& left,
    const XrSwapchainSubImage& right) noexcept {
    if (left.imageArrayIndex != right.imageArrayIndex) {
        return false;
    }
    const std::int64_t left_right =
        static_cast<std::int64_t>(left.imageRect.offset.x) +
        left.imageRect.extent.width;
    const std::int64_t left_bottom =
        static_cast<std::int64_t>(left.imageRect.offset.y) +
        left.imageRect.extent.height;
    const std::int64_t right_right =
        static_cast<std::int64_t>(right.imageRect.offset.x) +
        right.imageRect.extent.width;
    const std::int64_t right_bottom =
        static_cast<std::int64_t>(right.imageRect.offset.y) +
        right.imageRect.extent.height;
    return left.imageRect.offset.x < right_right &&
           right.imageRect.offset.x < left_right &&
           left.imageRect.offset.y < right_bottom &&
           right.imageRect.offset.y < left_bottom;
}

[[nodiscard]] ProjectionMappingResult
build_projection_resource_mappings(const ProjectionSnapshot& snapshot) noexcept {
    ProjectionMappingResult output{};
    try {
        if (snapshot.layers.empty() || snapshot.layers.front().views.empty()) {
            output.reason = ProjectionMappingReason::no_projection_views;
            return output;
        }
        for (std::size_t projection_index = 0;
             projection_index < snapshot.layers.size();
             ++projection_index) {
            const ProjectionLayerSnapshot& layer =
                snapshot.layers[projection_index];
            for (std::size_t view_index = 0;
                 view_index < layer.views.size();
                 ++view_index) {
                const XrSwapchainSubImage& sub_image =
                    layer.views[view_index].subImage;
                const auto swapchain = find_swapchain(sub_image.swapchain);
                if (!swapchain) {
                    output.reason = ProjectionMappingReason::unknown_swapchain;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const XrSwapchainCreateInfo& create_info =
                    swapchain->create_info;
                if (create_info.arraySize == 0 || create_info.arraySize > 2) {
                    output.reason = ProjectionMappingReason::unsupported_array_size;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (create_info.width == 0 || create_info.height == 0) {
                    output.reason = ProjectionMappingReason::zero_resource_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                if (sub_image.imageArrayIndex >= create_info.arraySize) {
                    output.reason = ProjectionMappingReason::array_slice_out_of_range;
                    output.detail =
                        (handle_value(sub_image.swapchain) << 8) |
                        sub_image.imageArrayIndex;
                    return output;
                }
                if (sub_image.imageRect.offset.x < 0 ||
                    sub_image.imageRect.offset.y < 0) {
                    output.reason = ProjectionMappingReason::negative_subimage_offset;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y);
                    return output;
                }
                if (sub_image.imageRect.extent.width <= 0 ||
                    sub_image.imageRect.extent.height <= 0) {
                    output.reason =
                        ProjectionMappingReason::nonpositive_subimage_extent;
                    output.detail = handle_value(sub_image.swapchain);
                    return output;
                }
                const std::uint64_t subimage_right =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.x) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.width);
                const std::uint64_t subimage_bottom =
                    static_cast<std::uint64_t>(sub_image.imageRect.offset.y) +
                    static_cast<std::uint32_t>(
                        sub_image.imageRect.extent.height);
                if (subimage_right > create_info.width ||
                    subimage_bottom > create_info.height) {
                    output.reason = ProjectionMappingReason::subimage_out_of_bounds;
                    output.detail =
                        (static_cast<std::uint64_t>(
                             static_cast<std::uint32_t>(
                                 sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(
                            sub_image.imageRect.extent.height);
                    return output;
                }

                auto mapping = std::find_if(
                    output.mappings.begin(),
                    output.mappings.end(),
                    [&](const ProjectionResourceMapping& candidate) {
                        return candidate.application_swapchain ==
                               sub_image.swapchain;
                    });
                if (mapping == output.mappings.end()) {
                    ProjectionResourceMapping created{};
                    created.application_swapchain = sub_image.swapchain;
                    output.mappings.push_back(std::move(created));
                    mapping = std::prev(output.mappings.end());
                }
                const auto existing_view = std::find_if(
                    mapping->views.begin(),
                    mapping->views.end(),
                    [&](const ProjectionViewReference& reference) {
                        return matching_sub_image(
                            snapshot.layers[reference.projection_index]
                                .views[reference.view_index]
                                .subImage,
                            sub_image);
                    });
                if (existing_view == mapping->views.end()) {
                    mapping->views.push_back({projection_index, view_index});
                }
            }
        }
        for (const ProjectionResourceMapping& mapping : output.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain || mapping.views.empty() || mapping.views.size() > 2) {
                output.reason = ProjectionMappingReason::unsupported_view_layout;
                output.detail = handle_value(mapping.application_swapchain);
                return output;
            }
            std::array<bool, 2> covered_slices{};
            for (std::size_t view_index = 0;
                 view_index < mapping.views.size();
                 ++view_index) {
                const ProjectionViewReference& reference = mapping.views[view_index];
                const XrSwapchainSubImage& sub_image =
                    snapshot.layers[reference.projection_index]
                        .views[reference.view_index]
                        .subImage;
                covered_slices[sub_image.imageArrayIndex] = true;
                for (std::size_t preceding = 0;
                     preceding < view_index;
                     ++preceding) {
                    const ProjectionViewReference& preceding_reference =
                        mapping.views[preceding];
                    const XrSwapchainSubImage& preceding_sub_image =
                        snapshot.layers[preceding_reference.projection_index]
                            .views[preceding_reference.view_index]
                            .subImage;
                    if (overlapping_sub_images(sub_image, preceding_sub_image)) {
                        output.reason =
                            ProjectionMappingReason::unsupported_view_layout;
                        output.detail = handle_value(mapping.application_swapchain);
                        return output;
                    }
                }
            }
            for (std::uint32_t slice = 0;
                 slice < swapchain->create_info.arraySize;
                 ++slice) {
                if (!covered_slices[slice]) {
                    output.reason = ProjectionMappingReason::missing_array_slice;
                    output.detail = handle_value(mapping.application_swapchain);
                    return output;
                }
            }
        }
        output.reason = ProjectionMappingReason::ready;
        return output;
    } catch (...) {
        output.mappings.clear();
        output.reason = ProjectionMappingReason::exception;
        return output;
    }
}

[[nodiscard]] bool matching_projection_camera(
    const XrPosef& left_pose,
    const XrFovf& left_fov,
    const XrPosef& right_pose,
    const XrFovf& right_fov) noexcept {
    return left_pose.orientation.x == right_pose.orientation.x &&
           left_pose.orientation.y == right_pose.orientation.y &&
           left_pose.orientation.z == right_pose.orientation.z &&
           left_pose.orientation.w == right_pose.orientation.w &&
           left_pose.position.x == right_pose.position.x &&
           left_pose.position.y == right_pose.position.y &&
           left_pose.position.z == right_pose.position.z &&
           left_fov.angleLeft == right_fov.angleLeft &&
           left_fov.angleRight == right_fov.angleRight &&
           left_fov.angleUp == right_fov.angleUp &&
           left_fov.angleDown == right_fov.angleDown;
}

[[nodiscard]] std::optional<std::vector<xrfg::D3D12ReprojectionView>>
build_reprojection_views(
    const ProjectionSnapshot& snapshot,
    const ProjectionResourceMapping& mapping) {
    if (snapshot.layers.empty() || mapping.views.empty()) {
        return std::nullopt;
    }
    // The application-submitted projection pose/FOV is the authoritative
    // render-camera contract for these pixels. An application may legitimately
    // transform or replace its xrLocateViews result before submission, so the
    // raw locate sample is neither intercepted nor an eligibility condition.
    for (const ProjectionLayerSnapshot& layer : snapshot.layers) {
        for (const XrCompositionLayerProjectionView& view : layer.views) {
            if (view.subImage.swapchain != mapping.application_swapchain) {
                continue;
            }
            const auto reference = std::find_if(
                mapping.views.begin(),
                mapping.views.end(),
                [&](const ProjectionViewReference& candidate) {
                    if (candidate.projection_index >= snapshot.layers.size() ||
                        candidate.view_index >=
                            snapshot.layers[candidate.projection_index].views.size()) {
                        return false;
                    }
                    return matching_sub_image(
                        view.subImage,
                        snapshot.layers[candidate.projection_index]
                            .views[candidate.view_index]
                            .subImage);
                });
            if (reference == mapping.views.end()) {
                return std::nullopt;
            }
            if (reference->projection_index >= snapshot.layers.size() ||
                reference->view_index >=
                    snapshot.layers[reference->projection_index].views.size()) {
                return std::nullopt;
            }
            const XrCompositionLayerProjectionView& canonical =
                snapshot.layers[reference->projection_index]
                    .views[reference->view_index];
            if (!matching_projection_camera(
                    view.pose,
                    view.fov,
                    canonical.pose,
                    canonical.fov)) {
                return std::nullopt;
            }
        }
    }

    std::vector<xrfg::D3D12ReprojectionView> output(
        mapping.views.size());
    for (std::size_t view_index = 0;
         view_index < mapping.views.size();
         ++view_index) {
        const ProjectionViewReference& reference =
            mapping.views[view_index];
        if (reference.projection_index >= snapshot.layers.size() ||
            reference.view_index >=
                snapshot.layers[reference.projection_index].views.size()) {
            return std::nullopt;
        }
        const XrCompositionLayerProjectionView& source =
            snapshot.layers[reference.projection_index]
                .views[reference.view_index];
        xrfg::D3D12ReprojectionView& destination = output[view_index];
        destination.pose.orientation = {
            source.pose.orientation.x,
            source.pose.orientation.y,
            source.pose.orientation.z,
            source.pose.orientation.w,
        };
        destination.pose.position = {
            source.pose.position.x,
            source.pose.position.y,
            source.pose.position.z,
        };
            destination.fov = {
            source.fov.angleLeft,
            source.fov.angleRight,
            source.fov.angleUp,
            source.fov.angleDown,
        };
        destination.image_rect = {
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.x),
            static_cast<std::uint32_t>(source.subImage.imageRect.offset.y),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.width),
            static_cast<std::uint32_t>(source.subImage.imageRect.extent.height),
        };
        destination.array_slice = source.subImage.imageArrayIndex;
    }
    return output;
}

[[nodiscard]] bool projection_snapshots_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        current.display_time <= previous.display_time ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size(); ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index || left.space != right.space ||
            left.layer_flags != right.layer_flags || left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size(); ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

// A space/flag transition only invalidates the interpolation pair and can be
// re-primed on the current application frame. A layout transition changes the
// resources or regions consumed by generation and must cross the quarantine.
[[nodiscard]] bool projection_resource_layout_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        current.display_time <= previous.display_time ||
        previous.layers.size() != current.layers.size()) {
        return false;
    }
    for (std::size_t layer_index = 0; layer_index < current.layers.size();
         ++layer_index) {
        const ProjectionLayerSnapshot& left = previous.layers[layer_index];
        const ProjectionLayerSnapshot& right = current.layers[layer_index];
        if (left.layer_index != right.layer_index ||
            left.views.size() != right.views.size()) {
            return false;
        }
        for (std::size_t view_index = 0; view_index < right.views.size();
             ++view_index) {
            if (!matching_sub_image(
                    left.views[view_index].subImage,
                    right.views[view_index].subImage)) {
                return false;
            }
        }
    }
    return true;
}

struct ProjectionResourceDestination {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain destination_swapchain{XR_NULL_HANDLE};
};

[[nodiscard]] bool build_generated_frame_end_info(
    const XrFrameEndInfo* source,
    const ProjectionSnapshot& current_snapshot,
    bool synthetic,
    std::span<const ProjectionResourceDestination> destinations,
    GeneratedFrameEndInfo* output) {
    if (source == nullptr || output == nullptr || destinations.empty() ||
        source->layerCount == 0 || source->layers == nullptr ||
        current_snapshot.layers.empty()) {
        return false;
    }

    output->info = *source;
    output->layer_pointers.assign(
        source->layers,
        source->layers + source->layerCount);
    output->projections.reserve(current_snapshot.layers.size());

    for (std::size_t projection_index = 0;
         projection_index < current_snapshot.layers.size();
         ++projection_index) {
        const ProjectionLayerSnapshot& metadata = current_snapshot.layers[projection_index];
        if (metadata.layer_index >= source->layerCount) {
            return false;
        }
        const XrCompositionLayerBaseHeader* layer = source->layers[metadata.layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            return false;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->viewCount != metadata.views.size() || projection->views == nullptr) {
            return false;
        }
        ProjectionLayerCopy copy{};
        copy.layer_index = metadata.layer_index;
        copy.layer = *projection;
        copy.views.assign(
            projection->views,
            projection->views + projection->viewCount);
        for (std::size_t view_index = 0; view_index < copy.views.size(); ++view_index) {
            XrCompositionLayerProjectionView& view = copy.views[view_index];
            const auto destination = std::find_if(
                destinations.begin(),
                destinations.end(),
                [&](const ProjectionResourceDestination& candidate) {
                    return candidate.application_swapchain ==
                           view.subImage.swapchain;
                });
            if (destination == destinations.end() ||
                destination->destination_swapchain == XR_NULL_HANDLE) {
                return false;
            }
            view.subImage.swapchain = destination->destination_swapchain;
            if (synthetic) {
                // The V008 shader generates the synthetic image in B's camera
                // reference. Its projection metadata must therefore remain
                // B's render pose/FOV.
                view.next = nullptr;
            }
        }
        output->projections.push_back(std::move(copy));
        ProjectionLayerCopy& stored = output->projections.back();
        if (synthetic) {
            stored.layer.next = nullptr;
        }
        stored.layer.views = stored.views.data();
        const auto* stored_header =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&stored.layer);
        output->layer_pointers[metadata.layer_index] = stored_header;
    }
    output->info.layerCount =
        static_cast<std::uint32_t>(output->layer_pointers.size());
    output->info.layers = output->layer_pointers.data();
    return true;
}

[[nodiscard]] bool acquire_and_wait_private_image(
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image) noexcept {
    try {
        if (!dispatch || image.handle == XR_NULL_HANDLE ||
            dispatch->acquire_swapchain_image == nullptr ||
            dispatch->wait_swapchain_image == nullptr ||
            dispatch->release_swapchain_image == nullptr) {
            return false;
        }
        if (image.phase != PrivateOwnershipPhase::idle &&
            !release_private_image(dispatch, image)) {
            return false;
        }

        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        std::uint32_t acquired_index = 0;
        const auto acquire_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            handle_value(image.handle),
            static_cast<std::uint64_t>(image.phase));
        const XrResult acquire_result = dispatch->acquire_swapchain_image(
            image.handle,
            &acquire_info,
            &acquired_index);
        xrfg::bridge_flight_logger().end(
            acquire_token,
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            acquire_result,
            handle_value(image.handle),
            acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (XR_FAILED(acquire_result)) {
            return false;
        }
        image.acquired_index = acquired_index;
        image.phase = PrivateOwnershipPhase::acquired;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait_info.timeout = XR_INFINITE_DURATION;
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        const XrResult wait_result =
            dispatch->wait_swapchain_image(image.handle, &wait_info);
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::private_swapchain_wait,
            wait_result,
            handle_value(image.handle),
            image.acquired_index,
            static_cast<std::uint64_t>(image.phase));
        if (wait_result != XR_SUCCESS && wait_result != XR_SESSION_LOSS_PENDING) {
            return false;
        }
        image.phase = PrivateOwnershipPhase::waited;
        return true;
    } catch (...) {
        return false;
    }
}

enum class PreparedGenerationKind {
    none,
    prime,
    pair,
};

enum class GenerationPrepareReason : std::int64_t {
    ready = 0,
    empty_mappings = 1,
    reprojection_views_failed = 2,
    unknown_swapchain = 3,
    missing_generation = 4,
    missing_capture = 5,
    invalid_private_swapchain = 6,
    retire_previous_failed = 7,
    current_private_acquire_failed = 8,
    synthetic_private_acquire_failed = 9,
    synthesis_failed = 10,
    private_release_failed = 11,
    mixed_resource_transaction = 12,
    generated_end_info_failed = 13,
    exception = 14,
    cooldown_active = 15,
    presenter_unsafe_composition = 16,
    manual_disarmed = 17,
    structural_quarantine_active = 18,
    synthesis_busy = 19,
};

[[nodiscard]] constexpr GenerationPrepareReason classify_synthesis_failure(
    HRESULT result) noexcept {
    return result == HRESULT_FROM_WIN32(ERROR_BUSY) ||
            result == DXGI_ERROR_WAS_STILL_DRAWING
        ? GenerationPrepareReason::synthesis_busy
        : GenerationPrepareReason::synthesis_failed;
}

static_assert(classify_synthesis_failure(DXGI_ERROR_WAS_STILL_DRAWING) ==
    GenerationPrepareReason::synthesis_busy);
struct PreparedGeneration {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain current_handle{XR_NULL_HANDLE};
    XrSwapchain synthetic_handle{XR_NULL_HANDLE};
    bool anchor_is_current{};
};

struct PreparedProjectionResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    PreparedGeneration generation;
};

struct PreparedProjectionFrame {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain failed_swapchain{XR_NULL_HANDLE};
    std::vector<PreparedProjectionResource> resources;
    bool anchor_is_current{};
};

[[nodiscard]] PreparedGeneration prepare_frame_generation(
    XrSwapchain application_swapchain,
    bool request_pair,
    std::span<const xrfg::D3D12ReprojectionView> current_source_views) noexcept {
    PreparedGeneration output{};
    try {
        const auto state = find_swapchain(application_swapchain);
        if (!state) {
            output.reason = GenerationPrepareReason::unknown_swapchain;
            return output;
        }

        std::scoped_lock call_lock(state->call_mutex);
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
        std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_vectors;
        {
            std::scoped_lock lock(state->mutex);
            generation = state->frame_generation;
            capture = state->last_released_capture;
            motion_vectors = state->last_released_motion_vectors;
        }
        if (!generation || !generation->synthesizer) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        if (!capture) {
            output.reason = GenerationPrepareReason::missing_capture;
            return output;
        }
        const std::size_t current_slot = generation->current_slot;
        PrivateSwapchainState& current_image =
            generation->current[current_slot];
        if (current_image.handle == XR_NULL_HANDLE ||
            generation->synthetic.handle == XR_NULL_HANDLE ||
            generation->current_images_per_slot == 0) {
            output.reason = GenerationPrepareReason::invalid_private_swapchain;
            return output;
        }

        if (!request_pair) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            const HRESULT retire_result = generation->synthesizer->retire_previous();
            if (FAILED(retire_result)) {
                output.reason = GenerationPrepareReason::retire_previous_failed;
                return output;
            }
        }

        if (!acquire_and_wait_private_image(
                state->session->dispatch,
                current_image)) {
            output.reason = GenerationPrepareReason::current_private_acquire_failed;
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(generation->synthesizer->retire_previous());
            }
            return output;
        }

        if (request_pair &&
            !acquire_and_wait_private_image(
                state->session->dispatch,
                generation->synthetic)) {
            output.reason =
                GenerationPrepareReason::synthetic_private_acquire_failed;
            static_cast<void>(release_private_image(
                state->session->dispatch,
                current_image));
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
            return output;
        }

        const std::uint32_t current_destination_index =
            static_cast<std::uint32_t>(current_slot) *
                generation->current_images_per_slot +
            current_image.acquired_index;

        xrfg::D3D12FrameSynthesisTicket ticket{};
        const auto debug_marker = request_pair && xrfg::bridge_flight_logger().enabled() &&
                state->session->fps_overlay
            ? state->session->fps_overlay->marker_placement()
            : std::nullopt;
        HRESULT submit_result = E_UNEXPECTED;
        const xrfg::BridgeFlightOperation synthesis_operation = request_pair
            ? xrfg::BridgeFlightOperation::synthesis_pair
            : xrfg::BridgeFlightOperation::synthesis_prime;
        const auto synthesis_token = xrfg::bridge_flight_logger().begin(
            synthesis_operation,
            handle_value(application_swapchain),
            capture->serial,
            (static_cast<std::uint64_t>(
                 generation->synthetic.acquired_index) << 32) |
                current_destination_index);
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            log_completed_nvidia_gpu_timings(generation->synthesizer);
            if (generation->d3d11_interop) {
                submit_result =
                    generation->d3d11_interop->prepare_synthesis();
            }
            if (!generation->d3d11_interop || SUCCEEDED(submit_result)) {
                submit_result = request_pair
                                    ? generation->synthesizer->submit_pair(
                                          *capture,
                                          current_source_views,
                                          current_source_views,
                                          generation->synthetic.acquired_index,
                                          current_destination_index,
                                          &ticket,
                                          debug_marker,
                                          motion_vectors)
                                    : generation->synthesizer->submit_prime(
                                          *capture,
                                          current_source_views,
                                          current_destination_index,
                                          &ticket,
                                          motion_vectors);
            }
            if (SUCCEEDED(submit_result) && generation->d3d11_interop) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? generation->synthetic.acquired_index
                                 : std::numeric_limits<std::uint32_t>::max());
                const HRESULT publish_result =
                    generation->d3d11_interop->publish(
                        current_destination_index,
                        request_pair
                            ? std::optional<std::uint32_t>(
                                  generation->synthetic.acquired_index)
                            : std::nullopt);
                xrfg::bridge_flight_logger().end(
                    publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    publish_result,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? generation->synthetic.acquired_index
                                 : std::numeric_limits<std::uint32_t>::max());
                if (FAILED(publish_result)) {
                    static_cast<void>(generation->synthesizer->retire_previous());
                    submit_result = publish_result;
                }
            }
        }
        xrfg::bridge_flight_logger().end(
            synthesis_token,
            synthesis_operation,
            submit_result,
            ticket.previous_serial,
            ticket.current_serial,
            ticket.fence_value);

        bool synthetic_released = true;
        if (request_pair) {
            synthetic_released = release_private_image(
                state->session->dispatch,
                generation->synthetic);
        }
        const bool current_released = release_private_image(
            state->session->dispatch,
            current_image);

        if (FAILED(submit_result)) {
            output.reason = classify_synthesis_failure(submit_result);
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(generation->synthesizer->retire_previous());
            }
            return output;
        }

        output.anchor_is_current = true;
        output.current_handle = current_image.handle;
        output.synthetic_handle = generation->synthetic.handle;
        if (current_released && synthetic_released) {
            // Hand the next frame the other slot, so it can release its output
            // while this one is still queued behind the presenter.
            generation->current_slot =
                (current_slot + 1) % kCurrentSlotCount;
            output.kind = request_pair ? PreparedGenerationKind::pair
                                       : PreparedGenerationKind::prime;
            output.reason = GenerationPrepareReason::ready;
        } else {
            output.reason = GenerationPrepareReason::private_release_failed;
        }

        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] PreparedProjectionFrame prepare_projection_frame(
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    bool request_pair) noexcept {
    PreparedProjectionFrame output{};
    try {
        if (mappings.empty()) {
            output.reason = GenerationPrepareReason::empty_mappings;
            return output;
        }
        const PreparedGenerationKind expected_kind = request_pair
            ? PreparedGenerationKind::pair
            : PreparedGenerationKind::prime;
        bool all_expected_kind = true;
        bool all_anchor_current = true;
        output.resources.reserve(mappings.size());
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto reprojection_views =
                build_reprojection_views(snapshot, mapping);
            if (!reprojection_views) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                return output;
            }
            PreparedGeneration generation = prepare_frame_generation(
                mapping.application_swapchain,
                request_pair,
                std::span<const xrfg::D3D12ReprojectionView>(
                    reprojection_views->data(),
                    reprojection_views->size()));
            all_expected_kind =
                all_expected_kind && generation.kind == expected_kind;
            all_anchor_current =
                all_anchor_current && generation.anchor_is_current;
            if (generation.reason != GenerationPrepareReason::ready &&
                output.failed_swapchain == XR_NULL_HANDLE) {
                output.reason = generation.reason;
                output.failed_swapchain = mapping.application_swapchain;
            }
            output.resources.push_back({
                mapping.application_swapchain,
                generation,
            });
        }
        output.kind = all_expected_kind
            ? expected_kind
            : PreparedGenerationKind::none;
        output.anchor_is_current = all_anchor_current;
        if (all_expected_kind) {
            output.reason = GenerationPrepareReason::ready;
        } else if (output.failed_swapchain == XR_NULL_HANDLE) {
            output.reason = GenerationPrepareReason::mixed_resource_transaction;
        }
        return output;
    } catch (...) {
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] std::optional<XrDuration> latest_pending_application_period(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order) noexcept {
    try {
        std::scoped_lock lock(state->mutex);
        if (consume_in_submission_order) {
            return state->pending_frames.empty()
                ? std::nullopt
                : std::optional<XrDuration>{
                      state->pending_frames.front().display_period};
        }
        // xrEndFrame::displayTime is an application-selected presentation
        // time, not an identity token for the preceding xrWaitFrame. UEVR's
        // Native Stereo Fix deliberately submits an older pipelined render
        // state while keeping a strictly sequential wait/begin/end call chain.
        // When exactly one wait is outstanding, call order identifies the
        // frame unambiguously even if those two times differ.
        if (state->pending_frames.size() == 1) {
            return state->pending_frames.front().display_period;
        }
        const auto frame = std::find_if(
            state->pending_frames.begin(),
            state->pending_frames.end(),
            [display_time](const PendingApplicationFrame& candidate) {
                return candidate.display_time == display_time;
            });
        if (frame == state->pending_frames.end() ||
            std::next(frame) != state->pending_frames.end()) {
            return std::nullopt;
        }
        return frame->display_period;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] XrTime generation_resume_time(XrTime display_time) noexcept {
    constexpr XrTime maximum_time = std::numeric_limits<XrTime>::max();
    if (display_time > maximum_time - kGenerationCooldownDuration) {
        return maximum_time;
    }
    return display_time + kGenerationCooldownDuration;
}

void schedule_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    if (!state) {
        return;
    }
    try {
        const auto deadline =
            std::chrono::steady_clock::now() + kStructuralQuarantineDuration;
        {
            std::scoped_lock lock(state->mutex);
            if (deadline > state->generation_resume_wall_time) {
                state->generation_resume_wall_time = deadline;
            }
            state->previous_projection.reset();
        }
        state->generation_steady_state_established = false;
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(reason),
            handle_value(state->handle),
            detail,
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    kStructuralQuarantineDuration)
                    .count()));
    } catch (...) {
    }
}

void clear_generation_continuity(
    const std::shared_ptr<SessionState>& state) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::continuity_reset,
        0,
        state ? handle_value(state->handle) : 0);
    try {
        {
            std::scoped_lock lock(state->mutex);
            state->previous_projection.reset();
        }
        for (const auto& swapchain : find_swapchains(state)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            std::scoped_lock gpu_lock(state->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
        }
    } catch (...) {
    }
}

void enter_generation_quarantine(
    const std::shared_ptr<SessionState>& state,
    GenerationQuarantineReason reason,
    std::uint64_t detail) noexcept {
    schedule_generation_quarantine(state, reason, detail);
    clear_generation_continuity(state);
}

[[nodiscard]] bool should_quarantine_generation_failure(
    GenerationPrepareReason reason) noexcept {
    switch (reason) {
        case GenerationPrepareReason::reprojection_views_failed:
        case GenerationPrepareReason::unknown_swapchain:
        case GenerationPrepareReason::invalid_private_swapchain:
        case GenerationPrepareReason::retire_previous_failed:
        case GenerationPrepareReason::synthesis_failed:
        case GenerationPrepareReason::mixed_resource_transaction:
        case GenerationPrepareReason::generated_end_info_failed:
        case GenerationPrepareReason::exception:
        case GenerationPrepareReason::presenter_unsafe_composition:
            return true;
        default:
            return false;
    }
}

struct InternalCycleResult {
    XrResult result{XR_SUCCESS};
    std::chrono::steady_clock::duration wait_elapsed{};
    XrDuration predicted_display_period{};
    bool completed{};
};

[[nodiscard]] InternalCycleResult submit_current_cycle(
    const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo& current_end_info) {
    InternalCycleResult output{};
    if (state->dispatch->wait_frame == nullptr ||
        state->dispatch->begin_frame == nullptr ||
        state->dispatch->end_frame == nullptr) {
        return output;
    }

    XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const auto wait_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_wait_frame,
        handle_value(state->handle));
    const auto wait_started = std::chrono::steady_clock::now();
    const XrResult wait_result = state->dispatch->wait_frame(
        state->handle,
        &wait_info,
        &frame_state);
    output.wait_elapsed = std::chrono::steady_clock::now() - wait_started;
    output.predicted_display_period = frame_state.predictedDisplayPeriod;
    xrfg::bridge_flight_logger().end(
        wait_token,
        xrfg::BridgeFlightOperation::internal_wait_frame,
        wait_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
        static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
        frame_state.shouldRender);
    if (XR_FAILED(wait_result)) {
        return output;
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_begin_frame,
        handle_value(state->handle));
    const XrResult begin_result = state->dispatch->begin_frame(
        state->handle,
        &begin_info);
    xrfg::bridge_flight_logger().end(
        begin_token,
        xrfg::BridgeFlightOperation::internal_begin_frame,
        begin_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    if (XR_FAILED(begin_result)) {
        output.result = begin_result;
        return output;
    }

    XrFrameEndInfo submitted = current_end_info;
    submitted.next = nullptr;
    submitted.displayTime = frame_state.predictedDisplayTime;
    if (frame_state.shouldRender == XR_FALSE) {
        submitted.layerCount = 0;
        submitted.layers = nullptr;
    }

    const auto end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_end_frame,
        handle_value(state->handle),
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount);
    const XrResult end_result = state->fps_overlay
        ? state->fps_overlay->end_frame(&submitted, false)
        : state->dispatch->end_frame(state->handle, &submitted);
    xrfg::bridge_flight_logger().end(
        end_token,
        xrfg::BridgeFlightOperation::internal_end_frame,
        end_result,
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount,
        frame_state.shouldRender);
    if (XR_FAILED(end_result)) {
        output.result = end_result;
        return output;
    }

    output.completed = true;
    return output;
}

[[nodiscard]] bool steamvr_wait_requires_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    const InternalCycleResult& cycle) noexcept {
    if (!state->dispatch->steamvr_runtime || !cycle.completed) {
        return false;
    }
    const auto elapsed_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            cycle.wait_elapsed).count();
    std::uint32_t streak = 0;
    XrDuration baseline = 0;
    bool throttled = false;
    {
        std::scoped_lock lock(state->mutex);
        baseline = state->minimum_runtime_display_period;
        if (baseline > 0) {
            // Only a wait that actually blocked is evidence that the runtime is
            // throttling the inline cycle. An inflated predictedDisplayPeriod
            // says something different: SteamVR widens it when it considers the
            // *caller* late, so counting it closed a feedback loop: a warm-up
            // hitch widened the period, the widened period promoted the
            // presenter, and the promotion hitched harder. UEVR reached the
            // promotion three frames after its first synthesis on two 20 us
            // waits, with SteamVR reporting 33.3 ms and then 22.2 ms against an
            // 11.1 ms baseline, and the GPU hung 4 ms later.
            const XrDuration half_period = baseline / 2;
            throttled = elapsed_nanoseconds >= half_period;
        }
        state->steamvr_throttled_wait_streak = throttled
            ? state->steamvr_throttled_wait_streak + 1
            : 0;
        streak = state->steamvr_throttled_wait_streak;
    }
    if (throttled) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_transition,
            streak,
            elapsed_nanoseconds > 0
                ? static_cast<std::uint64_t>(elapsed_nanoseconds)
                : 0,
            static_cast<std::uint64_t>(cycle.predicted_display_period),
            static_cast<std::uint64_t>(baseline));
    }
    return streak >= 3;
}

void consume_application_frame(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order) {
    std::scoped_lock lock(state->mutex);
    if (consume_in_submission_order) {
        if (!state->pending_frames.empty()) {
            state->pending_frames.pop_front();
        }
        return;
    }
    if (state->pending_frames.size() == 1) {
        state->pending_frames.pop_front();
        return;
    }
    const auto frame = std::find_if(
        state->pending_frames.begin(),
        state->pending_frames.end(),
        [display_time](const PendingApplicationFrame& candidate) {
            return candidate.display_time == display_time;
        });
    if (frame == state->pending_frames.end()) {
        return;
    }
    state->pending_frames.erase(state->pending_frames.begin(), std::next(frame));
}

// Called with frame ownership and presenter content excluded. No GUI thread
// touches GPU objects; no new captures may enter during this transaction.
void apply_embedded_control(const std::shared_ptr<SessionState>& state) {
    const auto control = xrfg::embedded::snapshot();
    if (state->control_revision == control.revision) return;
    state->menu_enabled = false;
    {
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
    }
    auto swapchains = find_swapchains(state);
    std::vector<std::unique_lock<std::mutex>> capture_locks;
    for (const auto& chain : swapchains) capture_locks.emplace_back(chain->call_mutex);
    std::scoped_lock gpu_lock(state->gpu_mutex);
    HRESULT result = S_OK;
    const auto backend = static_cast<xrfg::D3D12OpticalFlowBackend>(control.desired.backend);
    const xrfg::D3D12NvidiaOpticalFlowOptions options{
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(control.desired.scale), control.desired.backward};
    const bool changed = state->control_reconfigure_required || backend != state->optical_flow_backend ||
        options.preset != state->nvidia_options.preset ||
        options.input_scale != state->nvidia_options.input_scale ||
        options.bidirectional != state->nvidia_options.bidirectional;
    // Enumeration is excluded by frame_call_mutex, so newly created contexts
    // also see this tuple. A partial failure remains bypass, never mixed flow.
    state->optical_flow_backend = backend;
    state->nvidia_options = options;
    state->dlss_motion_vectors = control.desired.motion_vectors == 1;
    for (const auto& chain : swapchains) {
        std::unique_lock lock(chain->mutex);
        if (!chain->frame_generation && control.desired.enabled) {
            std::vector<ID3D11Texture2D*> sources;
            for (const auto& image : chain->enumerated_d3d11_images) sources.push_back(image.Get());
            lock.unlock();
            SwapchainEligibilityReason reason{};
            std::uint64_t detail{};
            auto candidate = state->graphics_binding == SessionGraphicsBinding::d3d11
                ? create_d3d11_frame_generation_swapchains(chain, sources, &reason, &detail)
                : create_d3d12_frame_generation_swapchains(chain, &reason, &detail);
            lock.lock();
            if (candidate) chain->frame_generation = std::move(candidate);
            else if (reason == SwapchainEligibilityReason::synthesis_initialize_failed)
                result = static_cast<HRESULT>(detail);
        }
        if (chain->frame_generation && chain->frame_generation->synthesizer) {
            auto& synthesis = chain->frame_generation->synthesizer;
            result = synthesis->wait_for_idle();
            if (SUCCEEDED(result) && changed) {
                result = synthesis->reconfigure(backend, options);
            }
        }
        if (SUCCEEDED(result) && chain->d3d12_history) result = chain->d3d12_history->wait_for_idle();
        if (SUCCEEDED(result) && chain->frame_generation && chain->frame_generation->d3d11_interop)
            result = chain->frame_generation->d3d11_interop->wait_for_idle();
        chain->last_released_capture.reset();
        chain->last_released_motion_vectors.reset();
        if (FAILED(result)) break;
    }
    // OptiScaler sees the desired menu value during its earlier DLSS evaluate.
    // A disable stops new publications there. Keep the private snapshot slots
    // alive: the DLSS command list that recorded their CopyResource operations
    // is owned and submitted by the game, so an OFXR fence cannot prove that an
    // open/not-yet-submitted producer list has finished referencing them.
    // Re-enable resets temporal ownership while reusing the retained slots.
    {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        if (SUCCEEDED(result)) {
            state->optical_flow_backend = backend;
            state->nvidia_options = options;
        }
    }
    state->control_revision = control.revision;
    state->control_reconfigure_required = FAILED(result);
    state->menu_enabled = SUCCEEDED(result) && control.desired.enabled;
    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::embedded::applied(state->control_id, control.revision, state->menu_enabled, result);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::embedded_configuration,
        result, control.revision, optical_flow_configuration_code(backend, options), state->menu_enabled ? 1 : 0);
}

XrResult layer_end_frame_impl(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock frame_call_lock(state->frame_call_mutex);
    const auto application_end_now = std::chrono::steady_clock::now();
    const bool frame_had_overlapping_wait =
        state->application_frame_has_overlapping_wait;
    const bool pipelined_presenter_mode = state->pipelined_presenter_mode;
    // Both promotions hand the presenter its first frame at the top of an
    // xrEndFrame, before the inline second cycle runs, so the handoff never
    // overlaps a frame this thread has already submitted.
    const bool pipelined_presenter_start_requested =
        state->pipelined_presenter_start_requested ||
        state->steamvr_presenter_start_requested;
    const bool consume_in_submission_order =
        pipelined_presenter_mode || frame_had_overlapping_wait;
    state->application_frame_in_progress = false;
    state->application_frame_has_overlapping_wait = false;
    if (!frame_had_overlapping_wait && !pipelined_presenter_mode) {
        state->pipelined_wait_streak = 0;
    }
    const bool use_continuous_presenter = continuous_presenter_active(state);
    if (use_continuous_presenter &&
        (end_info == nullptr || end_info->type != XR_TYPE_FRAME_END_INFO)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    std::unique_lock<std::mutex> presenter_content_lock;
    // Drains the presenter and takes the content lock. Used by the paths that
    // hand the runtime the application's own composition unchanged: they have
    // no private output of their own, so the retained repeat has to be settled
    // before they submit.
    const auto enter_presenter_exclusive = [&]() -> XrResult {
        if (!use_continuous_presenter || presenter_content_lock.owns_lock()) {
            return XR_SUCCESS;
        }
        const XrResult idle_result = wait_for_presenter_idle(state);
        if (XR_FAILED(idle_result)) {
            return idle_result;
        }
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
        return XR_SUCCESS;
    };

    apply_embedded_control(state);
    const bool manually_disarmed = state->manual_control.stop_requested();
    if (manually_disarmed && !state->manual_stop_applied) {
        // The existing queue has been drained before taking the content lock.
        // Do not unload hooks/resources, cancel submitted GPU work, or hand
        // overlapping frame calls to a different owner mid-cycle.
        clear_generation_continuity(state);
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            state->presenter_last_frame.reset();
        }
        state->manual_stop_applied = true;
        if (state->fps_overlay) state->fps_overlay->suspend();
    }
    if (state->fps_overlay && !manually_disarmed) {
        std::scoped_lock gpu_lock(state->gpu_mutex);
        state->fps_overlay->application_frame(end_info);
    }

    const auto submit_borrowed_to_presenter = [&]() -> XrResult {
        auto request = enqueue_presenter_submission(state, nullptr, end_info);
        if (presenter_content_lock.owns_lock()) {
            presenter_content_lock.unlock();
        }
        return wait_for_presenter_submission(state, request);
    };
    const auto start_requested_pipelined_presenter =
        [&](std::shared_ptr<GeneratedFrameEndInfo> seed_frame) -> XrResult {
            if (!pipelined_presenter_start_requested) {
                return XR_SUCCESS;
            }
            if (!start_continuous_presenter(
                    state,
                    std::move(seed_frame),
                    true)) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            state->pipelined_presenter_start_requested = false;
            state->steamvr_presenter_start_requested = false;
            return wait_for_presenter_idle(state);
        };
    const auto bypass_generation =
        [&](GenerationPrepareReason reason) -> XrResult {
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::generation_prepare,
                static_cast<std::int64_t>(reason),
                0,
                0,
                0);
            const auto end_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                handle_value(session),
                end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
                end_info ? end_info->layerCount : 0);
            const XrResult end_result = use_continuous_presenter
                ? submit_borrowed_to_presenter()
                : state->fps_overlay
                    ? state->fps_overlay->end_frame(end_info, false)
                    : state->dispatch->end_frame(session, end_info);
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                end_result,
                0,
                0,
                0);
            if (XR_SUCCEEDED(end_result) && end_info != nullptr) {
                consume_application_frame(
                    state,
                    end_info->displayTime,
                    consume_in_submission_order);
            }
            if (XR_SUCCEEDED(end_result) && !use_continuous_presenter) {
                const XrResult start_result =
                    start_requested_pipelined_presenter(nullptr);
                if (XR_FAILED(start_result)) {
                    return start_result;
                }
            }
            if (use_continuous_presenter && !pipelined_presenter_mode) {
                stop_continuous_presenter(state);
                std::scoped_lock lock(state->mutex);
                state->steamvr_throttled_wait_streak = 0;
            }
            return end_result;
        };

    bool generation_cooling_down = false;
    bool structural_quarantine_active = false;
    {
        std::scoped_lock lock(state->mutex);
        if (state->generation_resume_wall_time !=
            std::chrono::steady_clock::time_point{}) {
            if (application_end_now >= state->generation_resume_wall_time) {
                state->generation_resume_wall_time = {};
            } else {
                generation_cooling_down = true;
                structural_quarantine_active = true;
            }
        }
        if (state->generation_resume_display_time != 0) {
            if (end_info != nullptr &&
                end_info->displayTime >= state->generation_resume_display_time) {
                state->generation_resume_display_time = 0;
            } else {
                generation_cooling_down = true;
            }
        }
    }
    if (generation_cooling_down || manually_disarmed || !state->menu_enabled) {
        return bypass_generation(
            manually_disarmed
                ? GenerationPrepareReason::manual_disarmed
                : structural_quarantine_active
                    ? GenerationPrepareReason::structural_quarantine_active
                    : GenerationPrepareReason::cooldown_active);
    }

    ProjectionSnapshot current_snapshot{};
    const bool has_projection =
        capture_projection_snapshot(end_info, &current_snapshot);
    ProjectionMappingResult resource_mappings{};
    if (has_projection) {
        resource_mappings = build_projection_resource_mappings(current_snapshot);
    } else {
        resource_mappings.reason = ProjectionMappingReason::no_projection_views;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::projection_mapping,
        static_cast<std::int64_t>(resource_mappings.reason),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        (static_cast<std::uint64_t>(current_snapshot.layers.size()) << 32) |
            resource_mappings.mappings.size(),
        resource_mappings.detail);
    if (!resource_mappings.ready()) {
        clear_generation_continuity(state);
        if (resource_mappings.reason !=
            ProjectionMappingReason::no_projection_views) {
            schedule_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_mapping_failed,
                (static_cast<std::uint64_t>(resource_mappings.reason) << 56) |
                    (resource_mappings.detail & 0x00FFFFFFFFFFFFFFULL));
        }
        return bypass_generation(GenerationPrepareReason::empty_mappings);
    }
    const std::optional<XrDuration> application_display_period =
        latest_pending_application_period(
            state,
            current_snapshot.display_time,
            consume_in_submission_order);
    const bool latest_application_frame = application_display_period.has_value();
    std::optional<ProjectionSnapshot> previous_snapshot;
    {
        std::scoped_lock lock(state->mutex);
        previous_snapshot = state->previous_projection;
    }
    const bool snapshots_compatible =
        !previous_snapshot ||
        projection_snapshots_compatible(*previous_snapshot, current_snapshot);
    const bool metadata_pairable =
        latest_application_frame && previous_snapshot && snapshots_compatible;
    if (previous_snapshot && !snapshots_compatible) {
        if (!projection_resource_layout_compatible(
                *previous_snapshot,
                current_snapshot)) {
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_changed,
                static_cast<std::uint64_t>(current_snapshot.display_time));
            return bypass_generation(
                GenerationPrepareReason::structural_quarantine_active);
        }
        // Recenter/reference-space and layer-flag changes are safe to prime
        // immediately; do not turn an ordinary pose-space transition into a
        // one-second outage.
        clear_generation_continuity(state);
    }
    // Admit this frame once the presenter has taken the previous pair's
    // synthetic, leaving only its current submission outstanding.
    //
    // Waiting for the queue to empty instead spent half the available budget:
    // the application was released only after the second of two paced
    // submissions and still had to enqueue before the very next slot, so it
    // had one display period to render a frame that two periods were available
    // for. Admitting it a slot earlier is what the alternating current
    // swapchain exists to make safe -- the previous pair's queued current
    // frame names the other slot, and its synthetic has already left the
    // queue, so neither can be repointed by the release below.
    if (use_continuous_presenter) {
        const XrResult capacity_result = wait_for_presenter_capacity(state, 1);
        if (XR_FAILED(capacity_result)) {
            return capacity_result;
        }
    }
    PreparedProjectionFrame prepared = prepare_projection_frame(
        current_snapshot,
        std::span<const ProjectionResourceMapping>(
            resource_mappings.mappings.data(),
            resource_mappings.mappings.size()),
        metadata_pairable);
    // Acquire, synthesis and release all ran without the content lock, so the
    // presenter kept submitting throughout. Hold it only across the handoff.
    if (use_continuous_presenter) {
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
    }
    GenerationPrepareReason prepare_reason = prepared.reason;
    XrSwapchain failed_prepare_swapchain = prepared.failed_swapchain;
    if (prepared.kind == PreparedGenerationKind::none &&
        should_quarantine_generation_failure(prepare_reason)) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::generation_prepare,
            static_cast<std::int64_t>(prepare_reason),
            handle_value(failed_prepare_swapchain),
            static_cast<std::uint64_t>(prepared.kind),
            (metadata_pairable ? 1u : 0u) |
                (latest_application_frame ? 2u : 0u));
        schedule_generation_quarantine(
            state,
            GenerationQuarantineReason::generation_prepare_failed,
            (static_cast<std::uint64_t>(prepare_reason) << 56) |
                (handle_value(failed_prepare_swapchain) &
                 0x00FFFFFFFFFFFFFFULL));
        return bypass_generation(
            GenerationPrepareReason::structural_quarantine_active);
    }

    GeneratedFrameEndInfo first_generated{};
    GeneratedFrameEndInfo current_generated{};
    const XrFrameEndInfo* submitted_end_info = end_info;
    bool pair_ready = false;
    std::vector<ProjectionResourceDestination> current_destinations;
    std::vector<ProjectionResourceDestination> synthetic_destinations;
    current_destinations.reserve(prepared.resources.size());
    synthetic_destinations.reserve(prepared.resources.size());
    for (const PreparedProjectionResource& resource : prepared.resources) {
        current_destinations.push_back({
            resource.application_swapchain,
            resource.generation.current_handle,
        });
        synthetic_destinations.push_back({
            resource.application_swapchain,
            resource.generation.synthetic_handle,
        });
    }
    if (prepared.kind == PreparedGenerationKind::prime) {
        if (build_generated_frame_end_info(
                end_info,
                current_snapshot,
                false,
                std::span<const ProjectionResourceDestination>(
                    current_destinations.data(), current_destinations.size()),
                &first_generated)) {
            submitted_end_info = &first_generated.info;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (prepared.kind == PreparedGenerationKind::pair && previous_snapshot) {
        const bool synthetic_built = build_generated_frame_end_info(
            end_info,
            current_snapshot,
            true,
            std::span<const ProjectionResourceDestination>(
                synthetic_destinations.data(), synthetic_destinations.size()),
            &first_generated);
        const bool current_built = build_generated_frame_end_info(
            end_info,
            current_snapshot,
            false,
            std::span<const ProjectionResourceDestination>(
                current_destinations.data(), current_destinations.size()),
            &current_generated);
        if (synthetic_built && current_built) {
            submitted_end_info = &first_generated.info;
            pair_ready = true;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (!prepared.anchor_is_current) {
        clear_generation_continuity(state);
    }

    std::shared_ptr<GeneratedFrameEndInfo> presenter_first_frame;
    first_generated.synthetic = pair_ready;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_current_frame;
    if (use_continuous_presenter &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        presenter_first_frame =
            make_presenter_owned_frame(std::move(first_generated));
        if (pair_ready) {
            presenter_current_frame =
                make_presenter_owned_frame(std::move(current_generated));
        }
        if (!presenter_first_frame ||
            (pair_ready && !presenter_current_frame)) {
            presenter_first_frame.reset();
            presenter_current_frame.reset();
            submitted_end_info = end_info;
            pair_ready = false;
            prepare_reason =
                GenerationPrepareReason::presenter_unsafe_composition;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::presenter_composition_failed);
            prepared = {};
        } else {
            submitted_end_info = &presenter_first_frame->info;
        }
    }

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::generation_prepare,
        static_cast<std::int64_t>(prepare_reason),
        handle_value(failed_prepare_swapchain),
        static_cast<std::uint64_t>(prepared.kind),
        (metadata_pairable ? 1u : 0u) |
            (latest_application_frame ? 2u : 0u));

    const auto first_end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        handle_value(session),
        submitted_end_info
            ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
            : 0,
        submitted_end_info ? submitted_end_info->layerCount : 0);
    const auto first_end_started = std::chrono::steady_clock::now();
    XrResult result = XR_SUCCESS;
    if (use_continuous_presenter) {
        if (pair_ready) {
            result = enqueue_presenter_pair(
                state,
                presenter_first_frame,
                presenter_current_frame);
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            if (XR_SUCCEEDED(result) && pipelined_presenter_mode) {
                // Let a whole pair stay outstanding. The presenter paces its
                // submissions a display period apart, so any smaller bound
                // puts one of those periods on the application's critical
                // path: waiting for the queue to empty cost both periods and
                // held it to 36/s, waiting for one cost a single period and
                // held it to 34.6/s, with 11.93 ms of every 28.9 ms frame
                // spent blocked here. What should govern the application is
                // its own virtual wait, one pair per two display periods, and
                // that only gets to act once this stops pre-empting it.
                //
                // The pair enqueued here is drained by the time the next one
                // arrives, so this bound is a runaway guard rather than part
                // of the steady-state cadence.
                result = wait_for_presenter_capacity(state, 2);
            }
        } else if (presenter_first_frame) {
            auto request = enqueue_presenter_submission(
                state,
                presenter_first_frame,
                nullptr);
            if (presenter_content_lock.owns_lock()) {
                presenter_content_lock.unlock();
            }
            result = wait_for_presenter_submission(state, request);
        } else {
            result = submit_borrowed_to_presenter();
        }
    } else {
        result = state->fps_overlay
            ? state->fps_overlay->end_frame(submitted_end_info, pair_ready)
            : state->dispatch->end_frame(session, submitted_end_info);
    }
    const auto first_end_elapsed =
        std::chrono::steady_clock::now() - first_end_started;
    xrfg::bridge_flight_logger().end(
        first_end_token,
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        result,
        static_cast<std::uint64_t>(prepared.kind),
        pair_ready ? 1u : 0u,
        latest_application_frame ? 1u : 0u);
    if (XR_FAILED(result)) {
        enter_generation_quarantine(
            state,
            GenerationQuarantineReason::downstream_end_failed,
            static_cast<std::uint32_t>(result));
        return result;
    }
    if (pair_ready) {
        state->generation_steady_state_established = true;
    }
    if (use_continuous_presenter && !presenter_first_frame &&
        !pipelined_presenter_mode) {
        stop_continuous_presenter(state);
        std::scoped_lock lock(state->mutex);
        state->steamvr_throttled_wait_streak = 0;
    }

    consume_application_frame(
        state,
        current_snapshot.display_time,
        consume_in_submission_order);
    if (!use_continuous_presenter && pair_ready && application_display_period &&
        !pipelined_presenter_mode &&
        xrfg::should_enter_generation_cooldown(
            state->optical_flow_backend ==
                xrfg::D3D12OpticalFlowBackend::nvidia,
            first_end_elapsed,
            *application_display_period)) {
        {
            std::scoped_lock lock(state->mutex);
            state->generation_resume_display_time =
                generation_resume_time(current_snapshot.display_time);
        }
        clear_generation_continuity(state);
        return result;
    }
    if (prepared.anchor_is_current) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection = current_snapshot;
    } else {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
    }

    if (!use_continuous_presenter &&
        pipelined_presenter_start_requested) {
        std::shared_ptr<GeneratedFrameEndInfo> seed_frame;
        if (pair_ready) {
            seed_frame = make_presenter_owned_frame(
                std::move(current_generated));
        } else if (prepared.kind == PreparedGenerationKind::prime) {
            seed_frame = make_presenter_owned_frame(
                std::move(first_generated));
        }
        const XrResult start_result =
            start_requested_pipelined_presenter(std::move(seed_frame));
        return XR_FAILED(start_result) ? start_result : result;
    }

    if (!pair_ready) {
        return result;
    }

    if (use_continuous_presenter) {
        return result;
    }

    const InternalCycleResult current_cycle =
        submit_current_cycle(state, current_generated.info);
    if (!current_cycle.completed) {
        // The synthetic submission has already completed. A transient runtime
        // failure in the optional second cycle is recovered by the established
        // continuity reset; treating it as a structural resize would retain a
        // private image in an uncertain ownership phase for the whole timeout.
        clear_generation_continuity(state);
    } else if (steamvr_wait_requires_continuous_presenter(
                   state,
                   current_cycle)) {
        // Request the promotion; do not perform it here. submit_current_cycle
        // has already submitted this frame, so seeding a freshly started
        // presenter thread with it handed a second owner to composition layers
        // and private swapchain leases that this frame still holds. The next
        // xrEndFrame starts the presenter through the same path the pipelined
        // promotion uses, before the inline cycle runs and with a frame nothing
        // else has submitted.
        state->steamvr_presenter_start_requested = true;
    }
    return XR_FAILED(current_cycle.result) ? current_cycle.result : result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_instance_proc_addr(
    XrInstance instance,
    const char* name,
    PFN_xrVoidFunction* function) {
    return guard_c_api_boundary([&] {
        return layer_get_instance_proc_addr_impl(instance, name, function);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_api_layer_instance(
    const XrInstanceCreateInfo* create_info,
    const XrApiLayerCreateInfo* layer_info,
    XrInstance* instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_create);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_api_layer_instance_impl(create_info, layer_info, instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_create,
        result,
        instance && XR_SUCCEEDED(result) ? handle_value(*instance) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_instance(XrInstance instance) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::instance_destroy,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_instance_impl(instance);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::instance_destroy,
        result,
        handle_value(instance));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_session(
    XrInstance instance,
    const XrSessionCreateInfo* create_info,
    XrSession* session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_create,
        handle_value(instance));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_create_session_impl(instance, create_info, session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_create,
        result,
        handle_value(instance),
        session && XR_SUCCEEDED(result) ? handle_value(*session) : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_destroy,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_destroy_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_destroy,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_session(
    XrSession session,
    const XrSessionBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_begin,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_session_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_begin,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_session(XrSession session) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::session_end,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_session_impl(session);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::session_end,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_frame(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_wait_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_frame_impl(session, wait_info, frame_state);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_wait_frame,
        result,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayTime)
            : 0,
        frame_state && XR_SUCCEEDED(result)
            ? static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod)
            : 0,
        frame_state && XR_SUCCEEDED(result) ? frame_state->shouldRender : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_begin_frame(
    XrSession session,
    const XrFrameBeginInfo* begin_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_begin_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_frame_impl(session, begin_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_begin_frame,
        result,
        handle_value(session));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_swapchain(
    XrSession session,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    return guard_c_api_boundary([&] {
        return layer_create_swapchain_impl(session, create_info, swapchain);
    });
}

// A composition layer names a space, and at any pipeline depth above zero a
// submission naming one can still be queued when the application destroys it.
// The runtime then rejects that submission, presenter_failure latches, and
// every later frame call fails for the life of the session - seen as the
// session freezing on recentre, which is when MSFS rebuilds its reference
// space. Settle the presenter first so nothing queued names this handle.
//
// Upstream never needed this because the application waited for the queue to
// empty inside xrEndFrame; it does not any more.
XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_space(XrSpace space) {
    return guard_c_api_boundary([&]() -> XrResult {
        std::shared_ptr<Dispatch> dispatch;
        std::vector<std::shared_ptr<SessionState>> sessions;
        {
            std::scoped_lock lock(g_state_mutex);
            for (const auto& [handle, session] : g_sessions) {
                static_cast<void>(handle);
                if (session) {
                    sessions.push_back(session);
                    dispatch = session->dispatch;
                }
            }
        }
        if (!dispatch || dispatch->destroy_space == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        std::sort(sessions.begin(), sessions.end(), [](const auto& a, const auto& b) {
            return handle_value(a->handle) < handle_value(b->handle);
        });
        std::vector<PresenterResourceLifetimeGuard> guards;
        guards.reserve(sessions.size());
        for (const auto& session : sessions) {
            guards.emplace_back(session);
        }
        return dispatch->destroy_space(space);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_destroy_swapchain(XrSwapchain swapchain) {
    return guard_c_api_boundary([&] {
        return layer_destroy_swapchain_impl(swapchain);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    return guard_c_api_boundary([&] {
        return layer_enumerate_swapchain_images_impl(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo* acquire_info,
    std::uint32_t* index) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_acquire_swapchain_image_impl(swapchain, acquire_info, index);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        result,
        handle_value(swapchain),
        index && XR_SUCCEEDED(result) ? *index : 0);
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        handle_value(swapchain),
        wait_info ? static_cast<std::uint64_t>(wait_info->timeout) : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_swapchain_image_impl(swapchain, wait_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_release,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_release_swapchain_image_impl(swapchain, release_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_swapchain_release,
        result,
        handle_value(swapchain));
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_end_frame(
    XrSession session,
    const XrFrameEndInfo* end_info) {
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_end_frame,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_frame_impl(session, end_info);
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::application_end_frame,
        result,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    return result;
}

}  // namespace

extern "C" __declspec(dllexport) XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader_info,
    const char* layer_name,
    XrNegotiateApiLayerRequest* layer_request) {
    if (const auto delegated = optiscaler_bootstrap::negotiate(
            loader_info, layer_name, layer_request)) {
        return *delegated;
    }
    xrfg::initialize_bridge_flight_logger();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::negotiation,
        loader_info ? loader_info->minInterfaceVersion : 0,
        loader_info ? loader_info->maxInterfaceVersion : 0,
        loader_info ? loader_info->maxApiVersion : 0);
    const XrResult result = guard_c_api_boundary([&] {
        if (loader_info == nullptr || layer_request == nullptr || layer_name == nullptr ||
            std::strcmp(layer_name, kLayerName) != 0 ||
            loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
            loader_info->structVersion != XR_LOADER_INFO_STRUCT_VERSION ||
            loader_info->structSize != sizeof(XrNegotiateLoaderInfo) ||
            layer_request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
            layer_request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION ||
            layer_request->structSize != sizeof(XrNegotiateApiLayerRequest)) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        if (loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
            loader_info->minApiVersion > kLayerApiVersion ||
            loader_info->maxApiVersion < kLayerApiVersion) {
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        layer_request->layerApiVersion = kLayerApiVersion;
        layer_request->getInstanceProcAddr = layer_get_instance_proc_addr;
        layer_request->createApiLayerInstance = layer_create_api_layer_instance;
        return XR_SUCCESS;
    });
    xrfg::bridge_flight_logger().end(
        token,
        xrfg::BridgeFlightOperation::negotiation,
        result,
        layer_request ? layer_request->layerInterfaceVersion : 0,
        layer_request ? layer_request->layerApiVersion : 0);
    return result;
}
