#include <windows.h>
#include "autonomous_external_fixture.hpp"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr char kLayerName[] = "XR_APILAYER_OFXRBridge";
constexpr XrDuration kFakeDisplayPeriod = 10'000'000;
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;

template <typename Handle>
[[nodiscard]] Handle fake_handle(std::uintptr_t value) {
    return reinterpret_cast<Handle>(value);
}

XrInstance g_instance = fake_handle<XrInstance>(0x101);
XrSession g_session = fake_handle<XrSession>(0x202);
XrSwapchain g_application_swapchain = fake_handle<XrSwapchain>(0x303);
XrSwapchain g_current_swapchain = fake_handle<XrSwapchain>(0x304);
XrSwapchain g_synthetic_swapchain = fake_handle<XrSwapchain>(0x305);
XrSwapchain g_application_swapchain_right = fake_handle<XrSwapchain>(0x306);
XrSwapchain g_current_swapchain_right = fake_handle<XrSwapchain>(0x307);
XrSwapchain g_synthetic_swapchain_right = fake_handle<XrSwapchain>(0x308);
// The layer alternates its current output between two private swapchains so a
// released frame cannot repoint the previous frame's queued submission, so it
// creates two per application swapchain and this runtime hands out both.
XrSwapchain g_current_swapchain_b = fake_handle<XrSwapchain>(0x309);
XrSwapchain g_current_swapchain_right_b = fake_handle<XrSwapchain>(0x30a);
XrSwapchain g_synthetic_swapchain_b = fake_handle<XrSwapchain>(0x30b);
XrSwapchain g_synthetic_swapchain_right_b = fake_handle<XrSwapchain>(0x30c);
XrSwapchain g_current_swapchain_c = fake_handle<XrSwapchain>(0x30d);
bool g_luke_scratch_mode = false;
XrSpace g_space = fake_handle<XrSpace>(0x404);
std::atomic<XrSpace> g_valid_composition_space{g_space};
std::atomic<bool> g_delay_composition_validation{false};
XrTime g_next_display_time = 100;
bool g_split_eye_mode = false;
bool g_cropped_subimage_mode = false;
bool g_double_wide_mode = false;
bool g_uevr_pipelined_display_time_mode = false;
bool g_d3d11_interop_mode = false;
bool g_inverted_vertical_fov = false;
bool g_steamvr_runtime_mode = false;
bool g_steamvr_presenter_mode = false;
// Set while the application is inside xrEndFrame. The layer runs its inline
// second wait/begin/end cycle from that call on this same thread, which is
// exactly what a throttling SteamVR configuration slows down, so this tells
// the two waits apart by what is happening rather than by counting calls.
std::atomic<bool> g_application_in_end_frame{false};
bool g_flight_simulator_mode = false;
bool g_shared_panel_mode=false;
bool g_native_time_rewrite_mode = false;
bool g_acquire_ahead_mode = false;
std::atomic<bool> g_acquire_ahead_active{false};
std::atomic<std::uint32_t> g_acquire_ahead_calls{0};
bool g_destroy_pending_swapchain = false;
bool g_destroy_pending_space = false;
std::atomic<bool> g_swapchain_destroyed{false};
std::atomic<unsigned> g_submission_after_destroy{0};
DWORD g_test_application_thread_id{};
std::atomic<bool> g_concurrent_acquire_mode{false};
std::atomic<int> g_concurrent_acquire_count{0};
std::atomic<bool> g_first_concurrent_acquire_entered{false};
bool g_throw_from_begin_frame = false;
std::atomic<bool> g_fail_next_release{false};
std::atomic<bool> g_fail_next_synthetic_release{false};
std::atomic<bool> g_fail_next_wait_frame{false};
std::atomic<bool> g_next_wait_should_not_render{false};
std::atomic<XrTime> g_delay_end_time{0};
std::atomic<bool> g_block_atomic_end{false};
std::atomic<XrTime> g_block_atomic_end_time{0};
std::atomic<bool> g_atomic_end_entered{false};
std::atomic<bool> g_allow_atomic_end_return{false};
std::atomic<bool> g_wait_begin_handoff_mode{false};
std::atomic<std::uint32_t> g_wait_begin_handoff_calls{0};
std::atomic<bool> g_second_handoff_wait_entered{false};
std::atomic<bool> g_allow_second_handoff_wait_return{false};
std::atomic<std::uint32_t> g_wait_frame_calls{0};
std::atomic<std::uint32_t> g_begin_frame_calls{0};
std::atomic<std::uint32_t> g_end_frame_calls{0};
std::atomic<std::uint32_t> g_locate_views_calls{0};
std::atomic<std::uint32_t> g_create_swapchain_calls{0};
// Opt-in live-control fixture: keep its five-handle budget, but permit a
// private handle to be allocated again after the layer actually destroys it.
bool g_recycle_control_slots{};
std::array<bool,5> g_control_slot_live{};
// Split-eye counts the two kinds of creation separately. The layer takes its
// private swapchains when a projection layer first names an application
// swapchain rather than when that swapchain's images are enumerated, so both
// eyes now exist before any private swapchain does and a single call index no
// longer identifies which swapchain is being created.
std::atomic<std::uint32_t> g_split_eye_application_creates{0};
std::atomic<std::uint32_t> g_split_eye_private_creates{0};
std::atomic<std::uint32_t> g_destroy_swapchain_calls{0};
std::atomic<std::uint32_t> g_application_release_calls{0};
std::atomic<std::uint32_t> g_current_acquire_calls{0};
std::atomic<std::uint32_t> g_current_wait_calls{0};
std::atomic<std::uint32_t> g_current_release_calls{0};
std::atomic<std::uint32_t> g_synthetic_acquire_calls{0};
std::atomic<std::uint32_t> g_synthetic_wait_calls{0};
std::atomic<std::uint32_t> g_synthetic_release_calls{0};
std::atomic<bool> g_current_create_info_valid{false};
std::atomic<bool> g_synthetic_create_info_valid{false};
std::mutex g_frame_loop_mutex;
std::deque<XrTime> g_waited_display_times;
std::optional<XrTime> g_begun_display_time;

enum class SubmittedTarget {
    none,
    original,
    current,
    synthetic,
    mixed,
};

struct EndFrameRecord {
    XrTime display_time{};
    SubmittedTarget target{SubmittedTarget::none};
    std::uint32_t layer_count{};
    XrSpace space{XR_NULL_HANDLE};
    std::array<XrPosef, 2> poses{};
    std::array<XrFovf, 2> fovs{};
    std::array<XrRect2Di, 2> image_rects{};
    bool passthrough_layer_preserved{};
    std::uint32_t passthrough_layer_index{
        std::numeric_limits<std::uint32_t>::max()};
    float quad_x{},second_quad_x{};
    std::uint32_t quad_layer_count{};
    std::uint32_t quad_layer_index{
        std::numeric_limits<std::uint32_t>::max()};
};

struct LocateViewsRecord {
    XrTime display_time{};
    XrSpace space{XR_NULL_HANDLE};
    XrViewConfigurationType view_configuration_type{};
};

std::mutex g_end_records_mutex;
std::vector<EndFrameRecord> g_end_records;
std::mutex g_locate_records_mutex;
std::vector<LocateViewsRecord> g_locate_records;
std::string g_log_path;
const XrCompositionLayerBaseHeader* g_expected_passthrough_layer = nullptr;
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D11Device> g_d3d11_device;
ComPtr<ID3D11DeviceContext> g_d3d11_context;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_application_swapchain_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_current_swapchain_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_current_swapchain_b_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_current_swapchain_c_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_synthetic_swapchain_images;
std::array<ComPtr<ID3D11Texture2D>, 3> g_d3d11_synthetic_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_application_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_application_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_current_swapchain_right_b_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_right_images;
std::array<ComPtr<ID3D12Resource>, 3> g_synthetic_swapchain_right_b_images;

[[nodiscard]] bool is_application_swapchain(XrSwapchain swapchain) {
    return swapchain == g_application_swapchain ||
           swapchain == g_application_swapchain_right;
}

[[nodiscard]] bool is_current_swapchain(XrSwapchain swapchain) {
    return swapchain == g_current_swapchain ||
           swapchain == g_current_swapchain_right ||
           swapchain == g_current_swapchain_b ||
           (g_luke_scratch_mode && swapchain == g_current_swapchain_c) ||
           swapchain == g_current_swapchain_right_b;
}

[[nodiscard]] bool is_synthetic_swapchain(XrSwapchain swapchain) {
    return swapchain == g_synthetic_swapchain ||
           swapchain == g_synthetic_swapchain_b ||
           swapchain == g_synthetic_swapchain_right ||
           swapchain == g_synthetic_swapchain_right_b;
}

[[nodiscard]] XrTime fake_camera_time(XrTime display_time) noexcept {
    if (g_steamvr_presenter_mode || g_flight_simulator_mode) {
        return (display_time / kFakeDisplayPeriod) * 100;
    }
    // Inline fake time uses tiny ticks, but the director advertises real
    // nanosecond periods (e.g. 20,004,500 on promotion). The old modulo-only
    // mapping made that a 20,004.5 sample and an impossible >200-radian FOV.
    // Bound the synthetic camera, independently of pacing/cooldown time.
    return (display_time % kGenerationCooldownDuration) % 10000;
}

[[nodiscard]] XrView fake_view_for_time(XrTime display_time, std::uint32_t index) {
    // The fake runtime advances in compact synthetic ticks, then jumps one
    // second to exercise cooldown expiry. Keep its camera values bounded
    // across that artificial timeline jump.
    const XrTime camera_time = fake_camera_time(display_time);
    const float sample = static_cast<float>(camera_time) / 1000.0F;
    const float half_yaw = sample * 0.35F;

    XrView view{XR_TYPE_VIEW};
    view.pose.orientation.y = std::sin(half_yaw);
    view.pose.orientation.w = std::cos(half_yaw);
    view.pose.position.x = sample + static_cast<float>(index) * 0.01F;
    view.pose.position.y =
        1.6F + sample * 0.02F + static_cast<float>(index) * 0.002F;
    view.pose.position.z = -sample * 0.03F;
    view.fov.angleLeft = -0.8F - sample * 0.01F - static_cast<float>(index) * 0.001F;
    view.fov.angleRight = 0.8F + sample * 0.012F + static_cast<float>(index) * 0.001F;
    view.fov.angleUp = 0.7F + sample * 0.008F;
    view.fov.angleDown = -0.7F - sample * 0.006F;
    if (g_inverted_vertical_fov) std::swap(view.fov.angleUp, view.fov.angleDown);
    return view;
}

[[nodiscard]] XrView fake_submitted_view_for_time(
    XrTime display_time,
    std::uint32_t index) {
    XrView view = fake_view_for_time(display_time, index);
    const XrTime camera_time = fake_camera_time(display_time);
    const float sample = static_cast<float>(camera_time) / 1000.0F;
    const float submitted_half_yaw = sample * 0.35F + 0.075F;
    view.pose.orientation.y = std::sin(submitted_half_yaw);
    view.pose.orientation.w = std::cos(submitted_half_yaw);
    view.pose.position.x += 0.125F;
    view.pose.position.z += 0.05F;
    view.fov.angleLeft -= 0.005F;
    view.fov.angleRight += 0.007F;
    return view;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_instance(XrInstance) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_session(
    XrInstance,
    const XrSessionCreateInfo*,
    XrSession* session) {
    *session = g_session;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_session(XrSession) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_begin_session(XrSession, const XrSessionBeginInfo*) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_end_session(XrSession) {
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_wait_frame(
    XrSession,
    const XrFrameWaitInfo*,
    XrFrameState* frame_state) {
    if (frame_state == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const std::uint32_t wait_call =
        g_wait_frame_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    // A SteamVR configuration that paces the application: the wait blocks for
    // most of a display period, so the layer's pacing measurement finds
    // nothing to correct and this mode stays on the inline path. The internal
    // second cycle is deliberately not slowed - that is the other quirk, and
    // the steamvr-presenter mode covers it.
    if (g_steamvr_runtime_mode && !g_steamvr_presenter_mode &&
        !g_application_in_end_frame.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(7));
    }
    if (g_steamvr_presenter_mode) {
        if (GetCurrentThreadId() == g_test_application_thread_id &&
            g_application_in_end_frame.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(7));
        } else if (GetCurrentThreadId() != g_test_application_thread_id) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (g_flight_simulator_mode &&
        GetCurrentThreadId() != g_test_application_thread_id) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (g_wait_begin_handoff_mode.load(std::memory_order_acquire) &&
        g_wait_begin_handoff_calls.fetch_add(1, std::memory_order_acq_rel) == 1) {
        g_second_handoff_wait_entered.store(true, std::memory_order_release);
        const auto escape_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!g_allow_second_handoff_wait_return.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < escape_deadline) {
            std::this_thread::yield();
        }
    }
    if (g_fail_next_wait_frame.exchange(false, std::memory_order_acq_rel)) {
        if (!g_log_path.empty()) {
            std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
            stream << "[XRFG-FAKE] downstream wait frame failed\n";
        }
        return XR_ERROR_RUNTIME_FAILURE;
    }
    frame_state->predictedDisplayTime = g_next_display_time;
    frame_state->predictedDisplayPeriod = kFakeDisplayPeriod;
    frame_state->shouldRender =
        g_next_wait_should_not_render.exchange(false, std::memory_order_acq_rel)
            ? XR_FALSE
            : XR_TRUE;
    g_next_display_time += g_flight_simulator_mode
        ? kFakeDisplayPeriod * 3
        : 100;
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        g_waited_display_times.push_back(frame_state->predictedDisplayTime);
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream wait frame time="
               << frame_state->predictedDisplayTime << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_properties(
    XrInstance,
    XrInstanceProperties* properties) {
    if (properties == nullptr ||
        properties->type != XR_TYPE_INSTANCE_PROPERTIES) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    properties->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    strcpy_s(
        properties->runtimeName,
        g_steamvr_runtime_mode
            ? "SteamVR/OpenXR : XRFG fake lighthouse"
            : "XRFG fake runtime");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_begin_frame(XrSession, const XrFrameBeginInfo*) {
    g_begin_frame_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_throw_from_begin_frame) {
        throw std::runtime_error("intentional fake-runtime exception");
    }
    XrTime begun_time = 0;
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        if (g_begun_display_time || g_waited_display_times.empty()) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        begun_time = g_waited_display_times.front();
        g_waited_display_times.pop_front();
        g_begun_display_time = begun_time;
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream begin frame time=" << begun_time << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_end_frame(
    XrSession,
    const XrFrameEndInfo* end_info) {
    g_end_frame_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(g_frame_loop_mutex);
        if (!g_begun_display_time || end_info == nullptr ||
            (!g_flight_simulator_mode && !g_native_time_rewrite_mode &&
             !g_uevr_pipelined_display_time_mode &&
             end_info->displayTime != *g_begun_display_time)) {
            return XR_ERROR_CALL_ORDER_INVALID;
        }
        g_begun_display_time.reset();
    }
    EndFrameRecord record{};
    record.display_time = end_info->displayTime;
    record.layer_count = end_info->layerCount;
    bool saw_projection = false;
    std::uint32_t recorded_views = 0;
    if (end_info != nullptr && end_info->layers != nullptr) {
        for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount; ++layer_index) {
            const auto* layer = end_info->layers[layer_index];
            if (layer == g_expected_passthrough_layer) {
                record.passthrough_layer_preserved = true;
                record.passthrough_layer_index = layer_index;
            }
            if (layer != nullptr &&
                layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                if(++record.quad_layer_count==1)record.quad_x=q->pose.position.x;
                else record.second_quad_x=q->pose.position.x;
                if(record.quad_layer_count==1)record.quad_layer_index = layer_index;
            }
            if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                continue;
            }
            const auto* projection =
                reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            record.space = projection->space;
            if (projection->viewCount == 0 || projection->views == nullptr) {
                record.target = SubmittedTarget::mixed;
                continue;
            }
            for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
                saw_projection = true;
                const XrSwapchain handle = projection->views[view_index].subImage.swapchain;
                const SubmittedTarget view_target =
                    is_application_swapchain(handle)
                        ? SubmittedTarget::original
                        : is_current_swapchain(handle)
                              ? SubmittedTarget::current
                              : is_synthetic_swapchain(handle)
                                    ? SubmittedTarget::synthetic
                                    : SubmittedTarget::mixed;
                if (record.target == SubmittedTarget::none) {
                    record.target = view_target;
                } else if (record.target != view_target) {
                    record.target = SubmittedTarget::mixed;
                }
                if (recorded_views < record.poses.size()) {
                    record.poses[recorded_views] = projection->views[view_index].pose;
                    record.fovs[recorded_views] = projection->views[view_index].fov;
                    record.image_rects[recorded_views] =
                        projection->views[view_index].subImage.imageRect;
                    ++recorded_views;
                }
            }
        }
    }
    if (!saw_projection && end_info->layerCount != 0) {
        record.target = SubmittedTarget::mixed;
    }
    if (g_flight_simulator_mode &&
        g_delay_composition_validation.exchange(
            false,
            std::memory_order_acq_rel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_destroy_pending_swapchain && saw_projection &&
        g_swapchain_destroyed.load(std::memory_order_acquire)) {
        ++g_submission_after_destroy;
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_destroy_pending_space && saw_projection &&
        record.space != g_valid_composition_space.load(std::memory_order_acquire)) {
        ++g_submission_after_destroy;
        return XR_ERROR_HANDLE_INVALID;
    }
    if (g_flight_simulator_mode && saw_projection &&
        record.space !=
            g_valid_composition_space.load(std::memory_order_acquire)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    {
        std::scoped_lock lock(g_end_records_mutex);
        g_end_records.push_back(record);
    }
    if (end_info->displayTime == g_block_atomic_end_time.load(std::memory_order_acquire) &&
        g_block_atomic_end.exchange(false, std::memory_order_acq_rel)) {
        g_atomic_end_entered.store(true, std::memory_order_release);
        while (!g_allow_atomic_end_return.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    XrTime delayed_time = end_info->displayTime;
    if (g_delay_end_time.compare_exchange_strong(
            delayed_time, 0, std::memory_order_acq_rel)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!g_log_path.empty()) {
        const char* target =
            record.target == SubmittedTarget::original
                ? "original"
                : record.target == SubmittedTarget::current
                      ? "current"
                      : record.target == SubmittedTarget::synthetic
                            ? "synthetic"
                            : record.target == SubmittedTarget::none ? "none" : "mixed";
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream end frame target=" << target
               << " time=" << end_info->displayTime
               << " layers=" << end_info->layerCount
               << " pose_x=" << record.poses[0].position.x << ','
               << record.poses[1].position.x
               << '\n';
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_locate_views(
    XrSession,
    const XrViewLocateInfo* locate_info,
    XrViewState* view_state,
    std::uint32_t view_capacity_input,
    std::uint32_t* view_count_output,
    XrView* views) {
    if (locate_info == nullptr || view_state == nullptr || view_count_output == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    g_locate_views_calls.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock(g_locate_records_mutex);
        g_locate_records.push_back({
            locate_info->displayTime,
            locate_info->space,
            locate_info->viewConfigurationType,
        });
    }
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream locate views time="
               << locate_info->displayTime << " space=" << locate_info->space
               << " config=" << locate_info->viewConfigurationType << '\n';
    }
    *view_count_output = 2;
    view_state->viewStateFlags =
        XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if (view_capacity_input >= 2 && views != nullptr) {
        for (std::uint32_t index = 0; index < 2; ++index) {
            const XrView sample = fake_view_for_time(locate_info->displayTime, index);
            views[index].pose = sample.pose;
            views[index].fov = sample.fov;
        }
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_swapchain(
    XrSession,
    const XrSwapchainCreateInfo* create_info,
    XrSwapchain* swapchain) {
    std::uint32_t call =
        g_create_swapchain_calls.fetch_add(1, std::memory_order_relaxed);
    if(g_recycle_control_slots && call) {
        for(call=1;call<g_control_slot_live.size() && g_control_slot_live[call];++call) {}
        if(call==g_control_slot_live.size())return XR_ERROR_LIMIT_REACHED;
        g_control_slot_live[call]=true;
    }
    if (g_luke_scratch_mode && call == 3) {
        *swapchain = g_current_swapchain_c;
        return XR_SUCCESS;
    }
    if (g_luke_scratch_mode && call > 3) --call;
    if (g_split_eye_mode) {
        const bool private_info_valid =
            create_info != nullptr &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
            create_info->width == 4 && create_info->height == 4 &&
            create_info->arraySize == 1;
        // Per application swapchain the layer creates two current slots and
        // two synthetic slots, so each eye claims four private calls.
        const auto record_current = [&](bool first) {
            g_current_create_info_valid.store(
                (first ||
                 g_current_create_info_valid.load(std::memory_order_acquire)) &&
                    private_info_valid,
                std::memory_order_release);
        };
        const auto record_synthetic = [&](bool first) {
            g_synthetic_create_info_valid.store(
                (first ||
                 g_synthetic_create_info_valid.load(std::memory_order_acquire)) &&
                    private_info_valid,
                std::memory_order_release);
        };
        // Only the layer asks for a transfer destination, so that bit says
        // which kind of swapchain this is without depending on when it is
        // asked for. The dimension checks stay an assertion about the private
        // create info rather than a condition for routing it.
        const bool layer_owned =
            create_info != nullptr &&
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0;
        if (!layer_owned) {
            switch (g_split_eye_application_creates.fetch_add(
                1, std::memory_order_relaxed)) {
                case 0:
                    *swapchain = g_application_swapchain;
                    break;
                case 1:
                    *swapchain = g_application_swapchain_right;
                    break;
                default:
                    return XR_ERROR_LIMIT_REACHED;
            }
            return XR_SUCCESS;
        }
        // Per application swapchain the layer creates both current slots and
        // then both synthetic slots, in projection-view mapping order.
        switch (g_split_eye_private_creates.fetch_add(
            1, std::memory_order_relaxed)) {
            case 0:
                record_current(true);
                *swapchain = g_current_swapchain;
                break;
            case 1:
                record_current(false);
                *swapchain = g_current_swapchain_b;
                break;
            case 2:
                record_synthetic(true);
                *swapchain = g_synthetic_swapchain;
                break;
            case 3:
                record_synthetic(false);
                *swapchain = g_synthetic_swapchain_b;
                break;
            case 4:
                record_current(false);
                *swapchain = g_current_swapchain_right;
                break;
            case 5:
                record_current(false);
                *swapchain = g_current_swapchain_right_b;
                break;
            case 6:
                record_synthetic(false);
                *swapchain = g_synthetic_swapchain_right;
                break;
            case 7:
                record_synthetic(false);
                *swapchain = g_synthetic_swapchain_right_b;
                break;
            default:
                return XR_ERROR_LIMIT_REACHED;
        }
        return XR_SUCCESS;
    }
    if (call == 0) {
        *swapchain = g_application_swapchain;
    } else if (call == 1) {
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_current_create_info_valid.store(valid, std::memory_order_release);
        *swapchain = g_current_swapchain;
    } else if (call == 2) {
        // Second current slot: same create info as the first.
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_current_create_info_valid.store(
            g_current_create_info_valid.load(std::memory_order_acquire) && valid,
            std::memory_order_release);
        *swapchain = g_current_swapchain_b;
    } else if (call == 3) {
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_synthetic_create_info_valid.store(valid, std::memory_order_release);
        *swapchain = g_synthetic_swapchain;
    } else if (call == 4) {
        const std::uint32_t expected_width = g_double_wide_mode ? 8U : 4U;
        const std::uint32_t expected_array_size = g_double_wide_mode ? 1U : 2U;
        const bool valid = create_info != nullptr &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                           (create_info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
                           create_info->width == expected_width &&
                           create_info->height == 4 &&
                           create_info->arraySize == expected_array_size;
        g_synthetic_create_info_valid.store(
            g_synthetic_create_info_valid.load(std::memory_order_acquire) && valid,
            std::memory_order_release);
        *swapchain = g_synthetic_swapchain_b;
    } else {
        return XR_ERROR_LIMIT_REACHED;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_space(XrSpace space) {
    if (space != g_valid_composition_space.load(std::memory_order_acquire)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    g_valid_composition_space.store(XR_NULL_HANDLE, std::memory_order_release);
    return XR_SUCCESS;
}

// Delivers one session state transition and then reports the queue empty, so
// the call chain exercises both branches of the layer's pass-through: the
// event it records and the XR_EVENT_UNAVAILABLE it must forward untouched.
std::atomic<std::uint32_t> g_poll_event_calls{0};
constexpr XrSessionState kFakeSessionState = XR_SESSION_STATE_VISIBLE;
constexpr XrTime kFakeSessionStateTime = 4242;

XRAPI_ATTR XrResult XRAPI_CALL fake_poll_event(
    XrInstance,
    XrEventDataBuffer* event_data) {
    if (event_data == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (g_poll_event_calls.fetch_add(1, std::memory_order_relaxed) != 0) {
        return XR_EVENT_UNAVAILABLE;
    }
    auto* state_changed =
        reinterpret_cast<XrEventDataSessionStateChanged*>(event_data);
    *state_changed = XrEventDataSessionStateChanged{
        XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED,
        nullptr,
        g_session,
        kFakeSessionState,
        kFakeSessionStateTime};
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_destroy_swapchain(XrSwapchain handle) {
    if(g_recycle_control_slots) {
        const XrSwapchain slots[]{g_application_swapchain,g_current_swapchain,g_current_swapchain_b,
            g_synthetic_swapchain,g_synthetic_swapchain_b};
        for(unsigned i=1;i<5;++i)if(handle==slots[i]) {
            if(!g_control_slot_live[i])return XR_ERROR_HANDLE_INVALID;
            g_control_slot_live[i]=false;
        }
    }
    g_swapchain_destroyed.store(true, std::memory_order_release);
    g_destroy_swapchain_calls.fetch_add(1, std::memory_order_relaxed);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_enumerate_swapchain_images(
    XrSwapchain swapchain,
    std::uint32_t image_capacity_input,
    std::uint32_t* image_count_output,
    XrSwapchainImageBaseHeader* images) {
    if (!is_application_swapchain(swapchain) &&
        !is_current_swapchain(swapchain) &&
        !is_synthetic_swapchain(swapchain)) {
        return XR_ERROR_HANDLE_INVALID;
    }
    constexpr std::uint32_t kImageCount = 3;
    *image_count_output = kImageCount;
    if (image_capacity_input == 0 || images == nullptr) {
        return XR_SUCCESS;
    }
    if (image_capacity_input < kImageCount) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }

    if (g_d3d11_interop_mode) {
        auto* d3d11_images =
            reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        const auto* selected_images = &g_d3d11_application_swapchain_images;
        if (swapchain == g_current_swapchain) {
            selected_images = &g_d3d11_current_swapchain_images;
        } else if (swapchain == g_current_swapchain_b) {
            selected_images = &g_d3d11_current_swapchain_b_images;
        } else if (swapchain == g_current_swapchain_c) {
            selected_images = &g_d3d11_current_swapchain_c_images;
        } else if (swapchain == g_synthetic_swapchain) {
            selected_images = &g_d3d11_synthetic_swapchain_images;
        } else if (swapchain == g_synthetic_swapchain_b) {
            selected_images = &g_d3d11_synthetic_swapchain_b_images;
        }
        for (std::uint32_t index = 0; index < kImageCount; ++index) {
            if (d3d11_images[index].type !=
                XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11_images[index].texture = (*selected_images)[index].Get();
        }
        return XR_SUCCESS;
    }

    auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (std::uint32_t index = 0; index < kImageCount; ++index) {
        if (d3d12_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const auto* selected_images = &g_application_swapchain_images;
        if (swapchain == g_current_swapchain) {
            selected_images = &g_current_swapchain_images;
        } else if (swapchain == g_current_swapchain_b) {
            selected_images = &g_current_swapchain_b_images;
        } else if (swapchain == g_synthetic_swapchain) {
            selected_images = &g_synthetic_swapchain_images;
        } else if (swapchain == g_synthetic_swapchain_b) {
            selected_images = &g_synthetic_swapchain_b_images;
        } else if (swapchain == g_application_swapchain_right) {
            selected_images = &g_application_swapchain_right_images;
        } else if (swapchain == g_current_swapchain_right) {
            selected_images = &g_current_swapchain_right_images;
        } else if (swapchain == g_current_swapchain_right_b) {
            selected_images = &g_current_swapchain_right_b_images;
        } else if (swapchain == g_synthetic_swapchain_right) {
            selected_images = &g_synthetic_swapchain_right_images;
        } else if (swapchain == g_synthetic_swapchain_right_b) {
            selected_images = &g_synthetic_swapchain_right_b_images;
        }
        d3d12_images[index].texture = (*selected_images)[index].Get();
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_acquire_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageAcquireInfo*,
    std::uint32_t* index) {
    if (is_current_swapchain(swapchain)) {
        const std::uint32_t call =
            g_current_acquire_calls.fetch_add(1, std::memory_order_relaxed);
        *index = call % static_cast<std::uint32_t>(g_current_swapchain_images.size());
        return XR_SUCCESS;
    }
    if (is_synthetic_swapchain(swapchain)) {
        const std::uint32_t call =
            g_synthetic_acquire_calls.fetch_add(1, std::memory_order_relaxed);
        *index = call % static_cast<std::uint32_t>(g_synthetic_swapchain_images.size());
        return XR_SUCCESS;
    }
    if (g_concurrent_acquire_mode.load(std::memory_order_acquire)) {
        const int call = g_concurrent_acquire_count.fetch_add(1, std::memory_order_acq_rel);
        *index = static_cast<std::uint32_t>(call);
        if (call == 0) {
            g_first_concurrent_acquire_entered.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        return XR_SUCCESS;
    }
    if (g_acquire_ahead_active.load(std::memory_order_acquire)) {
        *index = g_acquire_ahead_calls.fetch_add(1, std::memory_order_relaxed) % 3U;
        return XR_SUCCESS;
    }
    *index = 2;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_wait_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo*) {
    if (is_current_swapchain(swapchain)) {
        g_current_wait_calls.fetch_add(1, std::memory_order_relaxed);
    } else if (is_synthetic_swapchain(swapchain)) {
        g_synthetic_wait_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_release_swapchain_image(
    XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo*) {
    if (is_current_swapchain(swapchain) || is_synthetic_swapchain(swapchain)) {
        const bool synthetic = is_synthetic_swapchain(swapchain);
        (synthetic ? g_synthetic_release_calls : g_current_release_calls)
            .fetch_add(1, std::memory_order_relaxed);
        if (!g_log_path.empty()) {
            std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
            stream << "[XRFG-FAKE] private release role="
                   << (synthetic ? "synthetic" : "current") << '\n';
        }
        if (synthetic &&
            g_fail_next_synthetic_release.exchange(false, std::memory_order_acq_rel)) {
            return XR_ERROR_RUNTIME_FAILURE;
        }
        return XR_SUCCESS;
    }
    g_application_release_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_log_path.empty()) {
        std::ofstream stream(g_log_path, std::ios::out | std::ios::app);
        stream << "[XRFG-FAKE] downstream release entered\n";
    }
    if (g_fail_next_release.exchange(false, std::memory_order_acq_rel)) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_proc_addr(
    XrInstance,
    const char* name,
    PFN_xrVoidFunction* function);

XRAPI_ATTR XrResult XRAPI_CALL fake_enumerate_instance_extension_properties(
    const char*, std::uint32_t capacity, std::uint32_t* count,
    XrExtensionProperties* properties) {
    constexpr const char* names[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
                                   XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
                                   XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
    *count = static_cast<std::uint32_t>(std::size(names));
    if (capacity == 0 || properties == nullptr) return XR_SUCCESS;
    if (capacity < *count) return XR_ERROR_SIZE_INSUFFICIENT;
    for (std::uint32_t index = 0; index < *count; ++index) {
        strcpy_s(properties[index].extensionName, names[index]);
        properties[index].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

extern "C" XRAPI_ATTR XrResult XRAPI_CALL fake_loader_enumerate_api_layer_properties(
    std::uint32_t, std::uint32_t* count, XrApiLayerProperties*) {
    *count = 0;
    return XR_SUCCESS;
}

// Stand in for a loader with a nonstandard module name, without negotiating
// as a layer/runtime. The production resolver must find its global exports.
extern "C" XRAPI_ATTR XrResult XRAPI_CALL fake_loader_get_instance_proc_addr(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    return fake_get_instance_proc_addr(instance, name, function);
}
#pragma comment(linker, "/export:xrGetInstanceProcAddr=fake_loader_get_instance_proc_addr")
#pragma comment(linker, "/export:xrEnumerateApiLayerProperties=fake_loader_enumerate_api_layer_properties")

XRAPI_ATTR XrResult XRAPI_CALL fake_get_instance_proc_addr(
    XrInstance,
    const char* name,
    PFN_xrVoidFunction* function) {
    *function = nullptr;
#define XRFG_FAKE_FUNCTION(openxr_name, implementation)                         \
    if (std::strcmp(name, openxr_name) == 0) {                                  \
        *function = reinterpret_cast<PFN_xrVoidFunction>(implementation);       \
        return XR_SUCCESS;                                                       \
    }
    XRFG_FAKE_FUNCTION("xrEnumerateInstanceExtensionProperties", fake_enumerate_instance_extension_properties)
    XRFG_FAKE_FUNCTION("xrEnumerateApiLayerProperties", fake_loader_enumerate_api_layer_properties)
    XRFG_FAKE_FUNCTION("xrDestroyInstance", fake_destroy_instance)
    XRFG_FAKE_FUNCTION("xrGetInstanceProperties", fake_get_instance_properties)
    XRFG_FAKE_FUNCTION("xrCreateSession", fake_create_session)
    XRFG_FAKE_FUNCTION("xrDestroySession", fake_destroy_session)
    XRFG_FAKE_FUNCTION("xrBeginSession", fake_begin_session)
    XRFG_FAKE_FUNCTION("xrEndSession", fake_end_session)
    XRFG_FAKE_FUNCTION("xrWaitFrame", fake_wait_frame)
    XRFG_FAKE_FUNCTION("xrBeginFrame", fake_begin_frame)
    XRFG_FAKE_FUNCTION("xrEndFrame", fake_end_frame)
    XRFG_FAKE_FUNCTION("xrLocateViews", fake_locate_views)
    XRFG_FAKE_FUNCTION("xrCreateSwapchain", fake_create_swapchain)
    XRFG_FAKE_FUNCTION("xrDestroySwapchain", fake_destroy_swapchain)
    XRFG_FAKE_FUNCTION("xrDestroySpace", fake_destroy_space)
    XRFG_FAKE_FUNCTION("xrPollEvent", fake_poll_event)
    XRFG_FAKE_FUNCTION("xrEnumerateSwapchainImages", fake_enumerate_swapchain_images)
    XRFG_FAKE_FUNCTION("xrAcquireSwapchainImage", fake_acquire_swapchain_image)
    XRFG_FAKE_FUNCTION("xrWaitSwapchainImage", fake_wait_swapchain_image)
    XRFG_FAKE_FUNCTION("xrReleaseSwapchainImage", fake_release_swapchain_image)
#undef XRFG_FAKE_FUNCTION
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XRAPI_ATTR XrResult XRAPI_CALL fake_create_api_layer_instance(
    const XrInstanceCreateInfo* info,
    const XrApiLayerCreateInfo*,
    XrInstance* instance) {
    unsigned clock_extensions{};
    for(std::uint32_t n=0;n<info->enabledExtensionCount;++n)
        clock_extensions+=std::strcmp(info->enabledExtensionNames[n],
            XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME)==0;
    if(clock_extensions!=1){std::cerr<<"advertised director clock must be enabled exactly once\n";return XR_ERROR_INITIALIZATION_FAILED;}
    *instance = g_instance;
    return XR_SUCCESS;
}

template <typename Function>
[[nodiscard]] Function get_layer_function(
    PFN_xrGetInstanceProcAddr get_instance_proc_addr,
    const char* name) {
    PFN_xrVoidFunction function = nullptr;
    const XrResult result = get_instance_proc_addr(g_instance, name, &function);
    if (XR_FAILED(result) || function == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<Function>(function);
}

[[nodiscard]] std::size_t count_occurrences(
    const std::string& text,
    const std::string& expected) {
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(expected, position)) != std::string::npos) {
        ++count;
        position += expected.size();
    }
    return count;
}

[[nodiscard]] bool initialize_d3d12() {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp_adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()))) ||
        FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(warp_adapter.GetAddressOf()))) ||
        FAILED(D3D12CreateDevice(
            warp_adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(g_device.GetAddressOf())))) {
        std::cerr << "failed to create the D3D12 WARP device\n";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(
            &queue_description,
            IID_PPV_ARGS(g_queue.GetAddressOf())))) {
        std::cerr << "failed to create the D3D12 command queue\n";
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_properties{};
    heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC texture_description{};
    texture_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_description.Width = g_double_wide_mode ? 8 : 4;
    texture_description.Height = 4;
    texture_description.DepthOrArraySize =
        (g_split_eye_mode || g_double_wide_mode) ? 1 : 2;
    texture_description.MipLevels = 1;
    texture_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texture_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    for (auto* images : {
             &g_application_swapchain_images,
             &g_current_swapchain_images,
             &g_current_swapchain_b_images,
             &g_synthetic_swapchain_images,
             &g_synthetic_swapchain_b_images,
             &g_application_swapchain_right_images,
             &g_current_swapchain_right_images,
             &g_current_swapchain_right_b_images,
             &g_synthetic_swapchain_right_images,
             &g_synthetic_swapchain_right_b_images}) {
        for (auto& image : *images) {
            if (FAILED(g_device->CreateCommittedResource(
                    &heap_properties,
                    D3D12_HEAP_FLAG_NONE,
                    &texture_description,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    nullptr,
                    IID_PPV_ARGS(image.GetAddressOf())))) {
                std::cerr << "failed to create a fake runtime swapchain image\n";
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool initialize_d3d11() {
    D3D_FEATURE_LEVEL feature_level{};
    if (FAILED(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            g_d3d11_device.GetAddressOf(),
            &feature_level,
            g_d3d11_context.GetAddressOf())) ||
        feature_level < D3D_FEATURE_LEVEL_11_0) {
        std::cerr << "failed to create the D3D11 hardware device\n";
        return false;
    }

    D3D11_TEXTURE2D_DESC description{};
    description.Width = g_double_wide_mode ? 8 : 4;
    description.Height = 4;
    description.MipLevels = 3;
    description.ArraySize = g_double_wide_mode ? 1 : 2;
    description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    for (auto& image : g_d3d11_application_swapchain_images) {
        if (FAILED(g_d3d11_device->CreateTexture2D(
                &description,
                nullptr,
                image.GetAddressOf()))) {
            std::cerr << "failed to create a fake D3D11 application image\n";
            return false;
        }
    }

    // Real runtimes may expose typed one-mip private images even when the
    // application swapchain uses a compatible typeless multi-mip resource.
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BindFlags = D3D11_BIND_RENDER_TARGET |
        D3D11_BIND_SHADER_RESOURCE;
    description.MiscFlags = 0;
    for (auto* images : {
             &g_d3d11_current_swapchain_images,
             &g_d3d11_current_swapchain_b_images,
             &g_d3d11_current_swapchain_c_images,
             &g_d3d11_synthetic_swapchain_images,
             &g_d3d11_synthetic_swapchain_b_images}) {
        for (auto& image : *images) {
            if (FAILED(g_d3d11_device->CreateTexture2D(
                    &description,
                    nullptr,
                    image.GetAddressOf()))) {
                std::cerr << "failed to create a fake D3D11 private image\n";
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool wait_for_queue_idle() {
    if (g_d3d11_interop_mode) {
        ComPtr<ID3D11Device5> device5;
        ComPtr<ID3D11DeviceContext4> context4;
        ComPtr<ID3D11Fence> fence;
        if (FAILED(g_d3d11_device.As(&device5)) ||
            FAILED(g_d3d11_context.As(&context4)) ||
            FAILED(device5->CreateFence(
                0,
                D3D11_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence.GetAddressOf()))) ||
            FAILED(context4->Signal(fence.Get(), 1))) {
            return false;
        }
        context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return false;
        }
        const HRESULT event_result = fence->SetEventOnCompletion(1, event);
        const DWORD wait_result = SUCCEEDED(event_result)
            ? WaitForSingleObject(event, 5'000)
            : WAIT_FAILED;
        CloseHandle(event);
        return wait_result == WAIT_OBJECT_0 &&
               fence->GetCompletedValue() >= 1;
    }
    ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()))) ||
        FAILED(g_queue->Signal(fence.Get(), 1))) {
        return false;
    }
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        return false;
    }
    const HRESULT event_result = fence->SetEventOnCompletion(1, event);
    const DWORD wait_result =
        SUCCEEDED(event_result) ? WaitForSingleObject(event, 5'000) : WAIT_FAILED;
    CloseHandle(event);
    return wait_result == WAIT_OBJECT_0 && fence->GetCompletedValue() >= 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr <<
            "usage: xrfg_layer_call_chain <layer-dll> <log-path> "
            "[split-eye|cropped-split-eye|double-wide|d3d11-interop|"
            "d3d11-double-wide|steamvr-inline|steamvr-presenter|"
            "flight-simulator|native-time-rewrite|uevr-pipelined-time|inverted-fov|"
            "d3d11-inverted-fov]\n";
        return EXIT_FAILURE;
    }
    g_cropped_subimage_mode =
        argc == 4 && std::strcmp(argv[3], "cropped-split-eye") == 0;
    const bool d3d11_double_wide_mode =
        argc == 4 && std::strcmp(argv[3], "d3d11-double-wide") == 0;
    g_destroy_pending_space =
        argc == 4 && std::strcmp(argv[3], "steamvr-destroy-space") == 0;
    g_steamvr_presenter_mode = g_destroy_pending_space ||
        (argc == 4 && std::strcmp(argv[3], "steamvr-presenter") == 0);
    g_steamvr_runtime_mode = g_steamvr_presenter_mode ||
        (argc == 4 && std::strcmp(argv[3], "steamvr-inline") == 0);
    g_destroy_pending_swapchain =
        argc == 4 && std::strcmp(argv[3], "flight-destroy-swapchain") == 0;
    g_shared_panel_mode=argc==4&&std::strcmp(argv[3],"flight-shared-panels")==0;
    g_flight_simulator_mode = g_shared_panel_mode || g_destroy_pending_swapchain ||
        (argc == 4 && std::strcmp(argv[3], "flight-simulator") == 0);
    g_native_time_rewrite_mode =
        argc == 4 && std::strcmp(argv[3], "native-time-rewrite") == 0;
    g_acquire_ahead_mode = argc == 4 && std::strcmp(argv[3], "acquire-ahead") == 0;
    g_test_application_thread_id = GetCurrentThreadId();
    g_inverted_vertical_fov = argc == 4 &&
        (std::strcmp(argv[3], "inverted-fov") == 0 ||
         std::strcmp(argv[3], "d3d11-inverted-fov") == 0);
    g_d3d11_interop_mode = argc == 4 &&
        (std::strcmp(argv[3], "d3d11-interop") == 0 ||
         std::strcmp(argv[3], "d3d11-inverted-fov") == 0 ||
         d3d11_double_wide_mode);
    g_uevr_pipelined_display_time_mode =
        argc == 4 && std::strcmp(argv[3], "uevr-pipelined-time") == 0;
    g_double_wide_mode = argc == 4 &&
        (std::strcmp(argv[3], "double-wide") == 0 ||
         d3d11_double_wide_mode || g_uevr_pipelined_display_time_mode);
    g_split_eye_mode = argc == 4 &&
        (std::strcmp(argv[3], "split-eye") == 0 || g_cropped_subimage_mode);
    if (argc == 4 && !g_split_eye_mode && !g_double_wide_mode &&
        !g_d3d11_interop_mode && !g_steamvr_runtime_mode &&
        !g_flight_simulator_mode && !g_native_time_rewrite_mode &&
        !g_uevr_pipelined_display_time_mode &&
        !g_inverted_vertical_fov && !g_acquire_ahead_mode) {
        std::cerr << "unknown test mode\n";
        return EXIT_FAILURE;
    }

    const HANDLE execution_mutex =
        CreateMutexA(nullptr, FALSE, "Local\\XRFGLayerCallChainFakeRuntimeTest");
    const DWORD mutex_wait = execution_mutex == nullptr
                                 ? WAIT_FAILED
                                 : WaitForSingleObject(execution_mutex, 30'000);
    if (execution_mutex == nullptr ||
        (mutex_wait != WAIT_OBJECT_0 && mutex_wait != WAIT_ABANDONED_0)) {
        if (execution_mutex != nullptr) {
            CloseHandle(execution_mutex);
        }
        std::cerr << "failed to serialize the fake-runtime test\n";
        return EXIT_FAILURE;
    }
    struct ExecutionMutexGuard {
        HANDLE handle{};
        ~ExecutionMutexGuard() {
            if (handle != nullptr) {
                ReleaseMutex(handle);
                CloseHandle(handle);
            }
        }
    } execution_mutex_guard{execution_mutex};
    {
        std::ofstream reset_log(argv[2], std::ios::out | std::ios::trunc);
        if (!reset_log) {
            std::cerr << "failed to truncate the fake-runtime log\n";
            return EXIT_FAILURE;
        }
    }
    g_log_path = argv[2];
    if (g_d3d11_interop_mode ? !initialize_d3d11()
                             : !initialize_d3d12()) {
        return EXIT_FAILURE;
    }

    xrfg_fixture::ExternalControlProfile control_profile(
        std::filesystem::absolute(argv[1]).parent_path() / L"ofxr_bridge.ini");
    const HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (negotiate == nullptr) {
        return EXIT_FAILURE;
    }

    XrNegotiateLoaderInfo loader_info{};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    if (XR_FAILED(negotiate(&loader_info, kLayerName, &request))) {
        return EXIT_FAILURE;
    }

    XrApiLayerNextInfo next_info{};
    next_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
    next_info.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
    next_info.structSize = sizeof(next_info);
    strcpy_s(next_info.layerName, kLayerName);
    next_info.nextGetInstanceProcAddr = fake_get_instance_proc_addr;
    next_info.nextCreateApiLayerInstance = fake_create_api_layer_instance;

    XrApiLayerCreateInfo layer_info{};
    layer_info.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
    layer_info.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
    layer_info.structSize = sizeof(layer_info);
    layer_info.nextInfo = &next_info;

    XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(instance_info.applicationInfo.applicationName, "XRFG fake runtime test");
    strcpy_s(instance_info.applicationInfo.engineName, "XRFG tests");
    instance_info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    // Probe before instance creation exactly as an upper eye-tracking layer
    // does. Verify content, both global functions, and refusal of instance APIs.
    {
        PFN_xrVoidFunction probe = nullptr;
        if (XR_FAILED(request.getInstanceProcAddr(
                XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &probe)) || !probe) {
            std::cerr << "null-instance extension query not resolved\n";
            return EXIT_FAILURE;
        }
        const auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(probe);
        std::uint32_t count = 0;
        std::array<XrExtensionProperties, 3> extensions{};
        for (auto& extension : extensions) extension.type = XR_TYPE_EXTENSION_PROPERTIES;
        if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr)) || count != extensions.size() ||
            XR_FAILED(enumerate(nullptr, static_cast<std::uint32_t>(extensions.size()), &count, extensions.data())) ||
            std::strcmp(extensions[1].extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) != 0) {
            std::cerr << "null-instance extension query lost eye tracking\n";
            return EXIT_FAILURE;
        }
        if (XR_FAILED(request.getInstanceProcAddr(
                XR_NULL_HANDLE, "xrEnumerateApiLayerProperties", &probe)) || !probe ||
            XR_FAILED(reinterpret_cast<PFN_xrEnumerateApiLayerProperties>(probe)(0, &count, nullptr)) || count != 0) {
            std::cerr << "null-instance layer query did not reach loader\n";
            return EXIT_FAILURE;
        }
        probe = reinterpret_cast<PFN_xrVoidFunction>(fake_get_instance_proc_addr);
        if (request.getInstanceProcAddr(XR_NULL_HANDLE, "xrCreateSession", &probe) !=
                XR_ERROR_FUNCTION_UNSUPPORTED || probe != nullptr) {
            std::cerr << "null-instance must not expose instance functions\n";
            return EXIT_FAILURE;
        }
    }

    XrInstance instance = XR_NULL_HANDLE;
    if (XR_FAILED(request.createApiLayerInstance(&instance_info, &layer_info, &instance)) ||
        instance != g_instance) {
        return EXIT_FAILURE;
    }

    const auto create_session = get_layer_function<PFN_xrCreateSession>(request.getInstanceProcAddr, "xrCreateSession");
    const auto destroy_session = get_layer_function<PFN_xrDestroySession>(request.getInstanceProcAddr, "xrDestroySession");
    const auto begin_session = get_layer_function<PFN_xrBeginSession>(request.getInstanceProcAddr, "xrBeginSession");
    const auto end_session = get_layer_function<PFN_xrEndSession>(request.getInstanceProcAddr, "xrEndSession");
    const auto wait_frame = get_layer_function<PFN_xrWaitFrame>(request.getInstanceProcAddr, "xrWaitFrame");
    const auto begin_frame = get_layer_function<PFN_xrBeginFrame>(request.getInstanceProcAddr, "xrBeginFrame");
    const auto end_frame = get_layer_function<PFN_xrEndFrame>(request.getInstanceProcAddr, "xrEndFrame");
    const auto locate_views = get_layer_function<PFN_xrLocateViews>(request.getInstanceProcAddr, "xrLocateViews");
    const auto create_swapchain = get_layer_function<PFN_xrCreateSwapchain>(request.getInstanceProcAddr, "xrCreateSwapchain");
    const auto destroy_swapchain = get_layer_function<PFN_xrDestroySwapchain>(request.getInstanceProcAddr, "xrDestroySwapchain");
    const auto destroy_space = get_layer_function<PFN_xrDestroySpace>(request.getInstanceProcAddr, "xrDestroySpace");
    const auto poll_event = get_layer_function<PFN_xrPollEvent>(request.getInstanceProcAddr, "xrPollEvent");
    const auto enumerate_images = get_layer_function<PFN_xrEnumerateSwapchainImages>(request.getInstanceProcAddr, "xrEnumerateSwapchainImages");
    const auto acquire_image = get_layer_function<PFN_xrAcquireSwapchainImage>(request.getInstanceProcAddr, "xrAcquireSwapchainImage");
    const auto wait_image = get_layer_function<PFN_xrWaitSwapchainImage>(request.getInstanceProcAddr, "xrWaitSwapchainImage");
    const auto release_image = get_layer_function<PFN_xrReleaseSwapchainImage>(request.getInstanceProcAddr, "xrReleaseSwapchainImage");
    const auto destroy_instance = get_layer_function<PFN_xrDestroyInstance>(request.getInstanceProcAddr, "xrDestroyInstance");

    if (!create_session || !destroy_session || !begin_session || !end_session || !wait_frame ||
        !begin_frame || !end_frame || !locate_views || !create_swapchain || !destroy_swapchain ||
        !enumerate_images || !acquire_image || !wait_image || !release_image || !destroy_instance || !poll_event) {
        return EXIT_FAILURE;
    }

    // The layer forwards events untouched. Both branches: the session state
    // change it records, and the empty-queue answer it must pass through
    // without inventing one.
    XrEventDataBuffer polled{XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult first_poll = poll_event(instance, &polled);
    const auto* polled_state =
        reinterpret_cast<const XrEventDataSessionStateChanged*>(&polled);
    XrEventDataBuffer drained{XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult second_poll = poll_event(instance, &drained);
    if (first_poll != XR_SUCCESS ||
        polled.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED ||
        polled_state->session != g_session ||
        polled_state->state != kFakeSessionState ||
        polled_state->time != kFakeSessionStateTime ||
        second_poll != XR_EVENT_UNAVAILABLE ||
        g_poll_event_calls.load(std::memory_order_relaxed) != 2) {
        return EXIT_FAILURE;
    }

    XrGraphicsBindingD3D12KHR graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    graphics_binding.device = g_device.Get();
    graphics_binding.queue = g_queue.Get();
    XrGraphicsBindingD3D11KHR d3d11_graphics_binding{
        XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    d3d11_graphics_binding.device = g_d3d11_device.Get();
    XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
    session_info.next = g_d3d11_interop_mode
        ? static_cast<const void*>(&d3d11_graphics_binding)
        : static_cast<const void*>(&graphics_binding);
    session_info.systemId = 1;
    XrSession session = XR_NULL_HANDLE;
    XrSessionBeginInfo session_begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    session_begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_FAILED(create_session(instance, &session_info, &session)) ||
        XR_FAILED(begin_session(session, &session_begin_info))) {
        return EXIT_FAILURE;
    }

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchain_info.format = static_cast<std::int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
    swapchain_info.sampleCount = 1;
    swapchain_info.width = g_double_wide_mode ? 8 : 4;
    swapchain_info.height = 4;
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize =
        (g_split_eye_mode || g_double_wide_mode) ? 1 : 2;
    swapchain_info.mipCount = g_d3d11_interop_mode ? 3 : 1;

    if (g_split_eye_mode) {
        XrSwapchain left_swapchain = XR_NULL_HANDLE;
        XrSwapchain right_swapchain = XR_NULL_HANDLE;
        std::array<XrSwapchainImageD3D12KHR, 3> enumerated_images{};
        for (auto& image : enumerated_images) {
            image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        }
        auto enumerate_application_images = [&](XrSwapchain handle) {
            std::uint32_t count = 0;
            return XR_SUCCEEDED(enumerate_images(
                       handle,
                       static_cast<std::uint32_t>(enumerated_images.size()),
                       &count,
                       reinterpret_cast<XrSwapchainImageBaseHeader*>(
                           enumerated_images.data()))) &&
                   count == enumerated_images.size();
        };
        if (XR_FAILED(create_swapchain(
                session,
                &swapchain_info,
                &left_swapchain)) ||
            left_swapchain != g_application_swapchain ||
            !enumerate_application_images(left_swapchain) ||
            XR_FAILED(create_swapchain(
                session,
                &swapchain_info,
                &right_swapchain)) ||
            right_swapchain != g_application_swapchain_right ||
            !enumerate_application_images(right_swapchain)) {
            return EXIT_FAILURE;
        }

        XrSwapchainImageAcquireInfo acquire_info{
            XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo image_wait_info{
            XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        image_wait_info.timeout = XR_INFINITE_DURATION;
        XrSwapchainImageReleaseInfo release_info{
            XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        auto capture_application_image = [&](XrSwapchain handle) {
            std::uint32_t index = 0;
            return XR_SUCCEEDED(acquire_image(handle, &acquire_info, &index)) &&
                   index == 2 &&
                   XR_SUCCEEDED(wait_image(handle, &image_wait_info)) &&
                   XR_SUCCEEDED(release_image(handle, &release_info));
        };

        std::array<XrCompositionLayerProjectionView, 2> projection_views{};
        for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
            auto& view = projection_views[index];
            view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            view.pose.orientation.w = 1.0F;
            view.subImage.swapchain =
                index == 0 ? left_swapchain : right_swapchain;
            view.subImage.imageRect.offset =
                g_cropped_subimage_mode ? XrOffset2Di{0, 1}
                                        : XrOffset2Di{0, 0};
            view.subImage.imageRect.extent =
                g_cropped_subimage_mode ? XrExtent2Di{4, 2}
                                        : XrExtent2Di{4, 4};
            view.subImage.imageArrayIndex = 0;
        }
        std::array<XrCompositionLayerProjection, 2> eye_projections{{
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION},
            {XR_TYPE_COMPOSITION_LAYER_PROJECTION},
        }};
        for (std::size_t index = 0; index < eye_projections.size(); ++index) {
            eye_projections[index].space = g_space;
            eye_projections[index].viewCount = 1;
            eye_projections[index].views = &projection_views[index];
        }
        // XRFG_TEST_SPLIT_EYE_ONE_LAYER: one projection layer holding both
        // views, each on its own swapchain, as games with a swapchain per eye
        // submit them (layer_projection_eyes.cmake).
        const bool one_layer = std::getenv("XRFG_TEST_SPLIT_EYE_ONE_LAYER") != nullptr;
        if (one_layer) {
            eye_projections[0].viewCount = 2;
            eye_projections[0].views = projection_views.data();
        }

        XrCompositionLayerQuad passthrough_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        passthrough_quad.space = g_space;
        passthrough_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        passthrough_quad.pose.orientation.w = 1.0F;
        passthrough_quad.size = {1.0F, 1.0F};
        passthrough_quad.subImage.swapchain = left_swapchain;
        passthrough_quad.subImage.imageRect.extent = {4, 4};
        const auto* passthrough_base =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &passthrough_quad);
        g_expected_passthrough_layer = passthrough_base;
        const XrCompositionLayerBaseHeader* all_layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &eye_projections[0]),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(
                &eye_projections[1]),
            passthrough_base,
        };
        const XrCompositionLayerBaseHeader* one_layer_layers[] = {
            all_layers[0],
            passthrough_base,
        };
        const XrCompositionLayerBaseHeader* const* layers =
            one_layer ? one_layer_layers : all_layers;
        const std::uint32_t layer_count = one_layer
            ? static_cast<std::uint32_t>(std::size(one_layer_layers))
            : static_cast<std::uint32_t>(std::size(all_layers));

        auto submit_frame = [&](XrTime display_time) {
            XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
            locate_info.viewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locate_info.displayTime = display_time;
            locate_info.space = g_space;
            XrViewState view_state{XR_TYPE_VIEW_STATE};
            std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
            std::uint32_t view_count = 0;
            if (XR_FAILED(locate_views(
                    session,
                    &locate_info,
                    &view_state,
                    static_cast<std::uint32_t>(views.size()),
                    &view_count,
                    views.data())) ||
                view_count != views.size()) {
                return false;
            }
            for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
                const XrView submitted =
                    fake_submitted_view_for_time(display_time, index);
                projection_views[index].pose = submitted.pose;
                projection_views[index].fov = submitted.fov;
            }
            XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
            frame_end_info.displayTime = display_time;
            frame_end_info.environmentBlendMode =
                XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frame_end_info.layerCount = layer_count;
            frame_end_info.layers = layers;
            return XR_SUCCEEDED(end_frame(session, &frame_end_info));
        };

        XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        XrFrameState frame_a{XR_TYPE_FRAME_STATE};
        XrFrameState frame_b{XR_TYPE_FRAME_STATE};
        XrFrameState frame_c{XR_TYPE_FRAME_STATE};
        const bool frame_sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_a)) &&
            frame_a.predictedDisplayTime == 100 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_a.predictedDisplayTime) &&
            wait_for_queue_idle() &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_b)) &&
            frame_b.predictedDisplayTime == 200 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_b.predictedDisplayTime) &&
            wait_for_queue_idle() &&
            // The first frame arms generation and passes through, so reaching
            // a generated pair takes one application frame longer than it did
            // when the private swapchains were taken at enumeration.
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &frame_c)) &&
            frame_c.predictedDisplayTime == 300 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_application_image(left_swapchain) &&
            capture_application_image(right_swapchain) &&
            submit_frame(frame_c.predictedDisplayTime) &&
            wait_for_queue_idle();

        g_expected_passthrough_layer = nullptr;
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(left_swapchain)) &&
            XR_SUCCEEDED(destroy_swapchain(right_swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        const auto nearly_equal = [](float actual, float expected) {
            return std::fabs(actual - expected) <= 1.0e-5F;
        };
        const auto camera_matches = [&](const EndFrameRecord& record,
                                        XrTime metadata_time) {
            for (std::uint32_t index = 0; index < record.poses.size(); ++index) {
                const XrView expected =
                    fake_submitted_view_for_time(metadata_time, index);
                const XrPosef& pose = record.poses[index];
                const XrFovf& fov = record.fovs[index];
                if (!nearly_equal(pose.orientation.y, expected.pose.orientation.y) ||
                    !nearly_equal(pose.orientation.w, expected.pose.orientation.w) ||
                    !nearly_equal(pose.position.x, expected.pose.position.x) ||
                    !nearly_equal(pose.position.y, expected.pose.position.y) ||
                    !nearly_equal(pose.position.z, expected.pose.position.z) ||
                    !nearly_equal(fov.angleLeft, expected.fov.angleLeft) ||
                    !nearly_equal(fov.angleRight, expected.fov.angleRight) ||
                    !nearly_equal(fov.angleUp, expected.fov.angleUp) ||
                    !nearly_equal(fov.angleDown, expected.fov.angleDown)) {
                    return false;
                }
            }
            return true;
        };
        const auto record_matches = [&](std::size_t index,
                                         XrTime time,
                                         SubmittedTarget target,
                                         XrTime metadata_time,
                                         std::uint32_t layer_count,
                                         bool passthrough_layer_preserved,
                                         std::uint32_t passthrough_layer_index) {
            const XrRect2Di expected_rect = g_cropped_subimage_mode
                ? XrRect2Di{{0, 1}, {4, 2}}
                : XrRect2Di{{0, 0}, {4, 4}};
            const bool rects_match = index < end_records.size() &&
                std::all_of(
                    end_records[index].image_rects.begin(),
                    end_records[index].image_rects.end(),
                    [&](const XrRect2Di& rect) {
                        return rect.offset.x == expected_rect.offset.x &&
                               rect.offset.y == expected_rect.offset.y &&
                               rect.extent.width == expected_rect.extent.width &&
                               rect.extent.height == expected_rect.extent.height;
                    });
            return index < end_records.size() && rects_match &&
                   end_records[index].display_time == time &&
                   end_records[index].target == target &&
                   end_records[index].layer_count == layer_count &&
                   end_records[index].space == g_space &&
                   end_records[index].passthrough_layer_preserved ==
                       passthrough_layer_preserved &&
                   end_records[index].passthrough_layer_index ==
                       passthrough_layer_index &&
                   camera_matches(end_records[index], metadata_time);
        };
        const bool valid =
            frame_sequence_succeeded && teardown_succeeded &&
            end_records.size() == 4 &&
            // The first frame arms and passes through, the second primes, and
            // the pair is the third.
            record_matches(0, 100, SubmittedTarget::original, 100, layer_count, true, layer_count - 1) &&
            record_matches(1, 200, SubmittedTarget::current, 200, layer_count, true, layer_count - 1) &&
            record_matches(2, 300, SubmittedTarget::synthetic, 300, layer_count, true, layer_count - 1) &&
            record_matches(3, 400, SubmittedTarget::current, 300, layer_count, true, layer_count - 1) &&
            g_wait_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_begin_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_end_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_locate_views_calls.load(std::memory_order_relaxed) == 3 &&
            // Two application swapchains, each backed by two complete private
            // pair slots: two current and two synthetic swapchains.
            g_create_swapchain_calls.load(std::memory_order_relaxed) == 10 &&
            g_destroy_swapchain_calls.load(std::memory_order_relaxed) == 10 &&
            g_application_release_calls.load(std::memory_order_relaxed) == 6 &&
            g_current_acquire_calls.load(std::memory_order_relaxed) == 4 &&
            g_current_wait_calls.load(std::memory_order_relaxed) == 4 &&
            g_current_release_calls.load(std::memory_order_relaxed) == 4 &&
            g_synthetic_acquire_calls.load(std::memory_order_relaxed) == 2 &&
            g_synthetic_wait_calls.load(std::memory_order_relaxed) == 2 &&
            g_synthetic_release_calls.load(std::memory_order_relaxed) == 2 &&
            g_current_create_info_valid.load(std::memory_order_acquire) &&
            g_synthetic_create_info_valid.load(std::memory_order_acquire) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            return EXIT_FAILURE;
        }
        std::cout << (g_cropped_subimage_mode
                          ? "OpenXR cropped split-eye fake-runtime call-chain test passed\n"
                          : "OpenXR split-eye fake-runtime call-chain test passed\n");
        return EXIT_SUCCESS;
    }

    XrSwapchain swapchain = XR_NULL_HANDLE;
    if (XR_FAILED(create_swapchain(session, &swapchain_info, &swapchain))) {
        return EXIT_FAILURE;
    }

    std::uint32_t image_count = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo image_wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    image_wait_info.timeout = XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    std::uint32_t acquired_index = 0;
    std::array<XrSwapchainImageD3D12KHR, 3> swapchain_images{};
    std::array<XrSwapchainImageD3D11KHR, 3> d3d11_swapchain_images{};
    for (auto& image : swapchain_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    }
    for (auto& image : d3d11_swapchain_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    std::array<XrSwapchainImageD3D12KHR, 2> partial_images{};
    std::array<XrSwapchainImageD3D11KHR, 2> d3d11_partial_images{};
    for (auto& image : partial_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
    }
    for (auto& image : d3d11_partial_images) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
    }
    XrSwapchainImageBaseHeader* const full_image_headers =
        g_d3d11_interop_mode
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  d3d11_swapchain_images.data())
            : reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  swapchain_images.data());
    XrSwapchainImageBaseHeader* const partial_image_headers =
        g_d3d11_interop_mode
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  d3d11_partial_images.data())
            : reinterpret_cast<XrSwapchainImageBaseHeader*>(
                  partial_images.data());
    if (XR_FAILED(enumerate_images(swapchain, 0, &image_count, nullptr)) || image_count != 3 ||
        enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(partial_images.size()),
            &image_count,
            partial_image_headers) !=
            XR_ERROR_SIZE_INSUFFICIENT ||
        XR_FAILED(enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(swapchain_images.size()),
            &image_count,
            full_image_headers)) ||
        image_count != swapchain_images.size() ||
        XR_FAILED(enumerate_images(
            swapchain,
            static_cast<std::uint32_t>(swapchain_images.size()),
            &image_count,
            full_image_headers)) ||
        XR_FAILED(acquire_image(swapchain, &acquire_info, &acquired_index)) || acquired_index != 2 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info))) {
        return EXIT_FAILURE;
    }

    g_fail_next_release.store(true, std::memory_order_release);
    if (XR_FAILED(acquire_image(swapchain, &acquire_info, &acquired_index)) || acquired_index != 2 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        release_image(swapchain, &release_info) != XR_ERROR_RUNTIME_FAILURE ||
        !wait_for_queue_idle() ||
        XR_FAILED(release_image(swapchain, &release_info))) {
        return EXIT_FAILURE;
    }

    g_concurrent_acquire_count.store(0, std::memory_order_release);
    g_first_concurrent_acquire_entered.store(false, std::memory_order_release);
    g_concurrent_acquire_mode.store(true, std::memory_order_release);
    XrResult first_acquire_result = XR_ERROR_RUNTIME_FAILURE;
    XrResult second_acquire_result = XR_ERROR_RUNTIME_FAILURE;
    std::uint32_t first_concurrent_index = 0;
    std::uint32_t second_concurrent_index = 0;
    std::thread first_acquire([&] {
        first_acquire_result = acquire_image(
            swapchain,
            &acquire_info,
            &first_concurrent_index);
    });

    const auto acquire_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_first_concurrent_acquire_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < acquire_deadline) {
        std::this_thread::yield();
    }
    if (!g_first_concurrent_acquire_entered.load(std::memory_order_acquire)) {
        first_acquire.join();
        return EXIT_FAILURE;
    }

    std::thread second_acquire([&] {
        second_acquire_result = acquire_image(
            swapchain,
            &acquire_info,
            &second_concurrent_index);
    });
    first_acquire.join();
    second_acquire.join();
    g_concurrent_acquire_mode.store(false, std::memory_order_release);

    if (XR_FAILED(first_acquire_result) || XR_FAILED(second_acquire_result) ||
        first_concurrent_index != 0 || second_concurrent_index != 1 ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info)) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_image(swapchain, &image_wait_info)) ||
        XR_FAILED(release_image(swapchain, &release_info)) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    if (g_acquire_ahead_mode) {
        // Ready or Not shape plus several waited images: observable history
        // captures must follow the actual runtime-owned indices in order.
        g_acquire_ahead_active.store(true, std::memory_order_release);
        std::uint32_t first = 3, second = 3;
        const bool ownership_sequence =
            XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &first)) && first == 0 &&
            XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &second)) && second == 1 &&
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(release_image(swapchain, &release_info)) && wait_for_queue_idle() &&
            XR_SUCCEEDED(release_image(swapchain, &release_info)) && wait_for_queue_idle() &&
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &first)) && first == 2 &&
            XR_SUCCEEDED(begin_session(session, &session_begin_info)) &&
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(release_image(swapchain, &release_info)) && wait_for_queue_idle() &&
            XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &first)) && first == 0 &&
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(begin_session(session, &session_begin_info)) &&
            XR_SUCCEEDED(release_image(swapchain, &release_info)) && wait_for_queue_idle() &&
            // An extra accepted wait must not poison later ownership tracking.
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &first)) && first == 1 &&
            XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
            XR_SUCCEEDED(release_image(swapchain, &release_info)) && wait_for_queue_idle();
        if (!ownership_sequence || XR_FAILED(destroy_swapchain(swapchain)) ||
            XR_FAILED(end_session(session)) || XR_FAILED(destroy_session(session)) ||
            XR_FAILED(destroy_instance(instance))) {
            std::cerr << "acquire-ahead ownership sequence failed\n";
            return EXIT_FAILURE;
        }
        // The embedded DLL may remain pinned after FreeLibrary. Wait for the
        // test's final record to reach its private log before process exit can
        // terminate the recorder thread; do not add a production flush/gate.
        const auto log_directory = std::filesystem::path(argv[1]).parent_path();
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool drained = false;
        do {
            for (const auto& entry : std::filesystem::directory_iterator(log_directory)) {
                if (!entry.path().filename().string().starts_with("ofxr-bridge-flight-")) continue;
                std::ifstream stream(entry.path());
                const std::string contents((std::istreambuf_iterator<char>(stream)), {});
                drained = contents.find("phase=E op=instance_destroy") != std::string::npos;
                if (drained) break;
            }
            if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } while (!drained && std::chrono::steady_clock::now() < drain_deadline);
        if (!drained) {
            std::cerr << "acquire-ahead recorder did not drain\n";
            return EXIT_FAILURE;
        }
        FreeLibrary(module);
        return EXIT_SUCCESS;
    }

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    XrFrameState frame_a{XR_TYPE_FRAME_STATE};
    XrFrameState frame_b{XR_TYPE_FRAME_STATE};
    XrFrameState frame_c{XR_TYPE_FRAME_STATE};
    XrFrameState frame_d{XR_TYPE_FRAME_STATE};
    XrFrameState frame_e{XR_TYPE_FRAME_STATE};
    XrFrameState frame_f{XR_TYPE_FRAME_STATE};
    XrFrameState frame_g{XR_TYPE_FRAME_STATE};
    XrFrameState frame_h{XR_TYPE_FRAME_STATE};
    XrFrameState frame_i{XR_TYPE_FRAME_STATE};
    XrFrameState frame_j{XR_TYPE_FRAME_STATE};
    XrFrameState handoff_frame_a{XR_TYPE_FRAME_STATE};
    XrFrameState handoff_frame_b{XR_TYPE_FRAME_STATE};
    if (!g_steamvr_presenter_mode && !g_flight_simulator_mode) {
        g_throw_from_begin_frame = true;
        const XrResult contained_exception_result =
            begin_frame(session, &frame_begin_info);
        g_throw_from_begin_frame = false;
        if (contained_exception_result != XR_ERROR_RUNTIME_FAILURE) {
            return EXIT_FAILURE;
        }
    }

    std::array<XrCompositionLayerProjectionView, 2> projection_views{};
    for (std::uint32_t index = 0; index < projection_views.size(); ++index) {
        auto& projection_view = projection_views[index];
        projection_view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projection_view.pose.orientation.w = 1.0F;
        projection_view.subImage.swapchain = swapchain;
        projection_view.subImage.imageRect.offset =
            g_double_wide_mode
                ? XrOffset2Di{static_cast<std::int32_t>(index * 4U), 0}
                : XrOffset2Di{0, 0};
        projection_view.subImage.imageRect.extent = {4, 4};
        projection_view.subImage.imageArrayIndex =
            g_double_wide_mode ? 0 : index;
    }
    XrSpace application_space = g_space;
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = application_space;
    projection.viewCount = static_cast<std::uint32_t>(projection_views.size());
    projection.views = projection_views.data();
    XrCompositionLayerQuad flight_quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    flight_quad.space = application_space;
    flight_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    flight_quad.subImage.swapchain = swapchain;
    flight_quad.subImage.imageRect.extent = {4, 4};
    flight_quad.pose.orientation.w = 1.0F;
    flight_quad.size = {1.0F, 1.0F};
    XrCompositionLayerQuad kneeboard=flight_quad;
    kneeboard.subImage.imageRect.offset={2,0};kneeboard.subImage.imageRect.extent={2,4};
    kneeboard.pose.position.x=5.f;
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&flight_quad),
    };
    const XrCompositionLayerBaseHeader* shared_panel_layers[] = {
        layers[0],layers[1],reinterpret_cast<const XrCompositionLayerBaseHeader*>(&kneeboard),
    };

    auto submit_frame = [&](XrTime display_time) {
        XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
        locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate_info.displayTime = display_time;
        locate_info.space = application_space;
        XrViewState view_state{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        std::uint32_t view_count = 0;
        if (XR_FAILED(locate_views(
                session,
                &locate_info,
                &view_state,
                static_cast<std::uint32_t>(views.size()),
                &view_count,
                views.data())) ||
            view_count != views.size()) {
            return false;
        }
        for (std::size_t index = 0; index < projection_views.size(); ++index) {
            const XrView submitted = fake_submitted_view_for_time(
                display_time,
                static_cast<std::uint32_t>(index));
            projection_views[index].pose = submitted.pose;
            projection_views[index].fov = submitted.fov;
        }

        XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
        frame_end_info.displayTime = display_time;
        frame_end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        if(g_shared_panel_mode)flight_quad.pose.position.x+=.1f;
        frame_end_info.layerCount = g_flight_simulator_mode ? (g_shared_panel_mode?3:2) : 1;
        frame_end_info.layers = g_shared_panel_mode?shared_panel_layers:layers;
        // SteamVR throttles the *inline* second cycle, not the application's
        // own wait. Mark the window so the fake runtime can tell them apart by
        // what is happening rather than by counting calls: the layer runs its
        // internal wait from this thread while the application is inside
        // xrEndFrame, and a parity rule breaks the moment the schedule shifts
        // by a frame.
        g_application_in_end_frame.store(true, std::memory_order_release);
        const bool ended = XR_SUCCEEDED(end_frame(session, &frame_end_info));
        g_application_in_end_frame.store(false, std::memory_order_release);
        return ended;
    };

    auto capture_fresh_application_image = [&] {
        return wait_for_queue_idle() &&
               XR_SUCCEEDED(acquire_image(swapchain, &acquire_info, &acquired_index)) &&
               XR_SUCCEEDED(wait_image(swapchain, &image_wait_info)) &&
               XR_SUCCEEDED(release_image(swapchain, &release_info));
    };

    if (g_uevr_pipelined_display_time_mode) {
        XrFrameState uevr_frame_a{XR_TYPE_FRAME_STATE};
        XrFrameState uevr_frame_b{XR_TYPE_FRAME_STATE};
        XrFrameState uevr_frame_c{XR_TYPE_FRAME_STATE};
        const bool frame_sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_a)) &&
            uevr_frame_a.predictedDisplayTime == 100 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_a.predictedDisplayTime - 10) &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_b)) &&
            uevr_frame_b.predictedDisplayTime == 200 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_b.predictedDisplayTime - 10) &&
            wait_for_queue_idle() &&
            // The first frame arms generation and passes through, so the pair
            // is the third application frame rather than the second.
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &uevr_frame_c)) &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(uevr_frame_c.predictedDisplayTime - 10) &&
            wait_for_queue_idle();

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        const bool valid =
            frame_sequence_succeeded && teardown_succeeded &&
            end_records.size() == 4 &&
            // The first frame arms and passes through, the second primes, and
            // the third is the pair. The internal cycle still takes the
            // runtime's own next predicted time rather than the application's.
            end_records[0].display_time == 90 &&
            end_records[0].target == SubmittedTarget::original &&
            end_records[1].display_time == 190 &&
            end_records[1].target == SubmittedTarget::current &&
            end_records[2].display_time == 290 &&
            end_records[2].target == SubmittedTarget::synthetic &&
            end_records[3].display_time == 400 &&
            end_records[3].target == SubmittedTarget::current &&
            g_wait_frame_calls.load(std::memory_order_relaxed) == 4 &&
            // Includes the harness's earlier exception-containment probe.
            g_begin_frame_calls.load(std::memory_order_relaxed) == 5 &&
            g_end_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            return EXIT_FAILURE;
        }
        std::cout <<
            "OpenXR UEVR pipelined display-time call-chain test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_native_time_rewrite_mode) {
        XrFrameState first{XR_TYPE_FRAME_STATE};
        XrFrameState second{XR_TYPE_FRAME_STATE};
        XrFrameState third{XR_TYPE_FRAME_STATE};
        constexpr XrTime kNativeFixOffset = 5'000;
        const bool sequence_succeeded =
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &first)) &&
            first.predictedDisplayTime == 100 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(first.predictedDisplayTime + kNativeFixOffset) &&
            wait_for_queue_idle() &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &second)) &&
            second.predictedDisplayTime == 200 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(second.predictedDisplayTime + kNativeFixOffset) &&
            wait_for_queue_idle() &&
            XR_SUCCEEDED(wait_frame(session, &frame_wait_info, &third)) &&
            third.predictedDisplayTime == 300 &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            capture_fresh_application_image() &&
            submit_frame(third.predictedDisplayTime + kNativeFixOffset) &&
            wait_for_queue_idle();
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);
        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        std::size_t current_frames = 0;
        std::size_t synthetic_frames = 0;
        for (const EndFrameRecord& record : end_records) {
            current_frames += record.target == SubmittedTarget::current ? 1U : 0U;
            synthetic_frames +=
                record.target == SubmittedTarget::synthetic ? 1U : 0U;
        }
        const bool valid = sequence_succeeded && teardown_succeeded &&
            current_frames == 2 && synthetic_frames == 1 &&
            g_wait_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_begin_frame_calls.load(std::memory_order_relaxed) == 5 &&
            g_end_frame_calls.load(std::memory_order_relaxed) == 4 &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "Native-time rewrite validation failed: sequence="
                      << sequence_succeeded << " teardown=" << teardown_succeeded
                      << " current=" << current_frames
                      << " synthetic=" << synthetic_frames << " waits="
                      << g_wait_frame_calls.load() << " begins="
                      << g_begin_frame_calls.load() << " ends="
                      << g_end_frame_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR Native Stereo Fix time-rewrite test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_flight_simulator_mode) {
        std::array<XrFrameState, 5> application_frames{{
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
        }};
        auto wait_next_while_beginning_current =
            [&](XrFrameState& next_frame) {
                std::atomic<bool> wait_started{false};
                XrResult wait_result = XR_ERROR_RUNTIME_FAILURE;
                std::thread wait_thread([&] {
                    wait_started.store(true, std::memory_order_release);
                    wait_result = wait_frame(
                        session,
                        &frame_wait_info,
                        &next_frame);
                });
                while (!wait_started.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const XrResult begin_result =
                    begin_frame(session, &frame_begin_info);
                wait_thread.join();
                return XR_SUCCEEDED(begin_result) && XR_SUCCEEDED(wait_result);
            };
        auto submit_flight_frame = [&](const XrFrameState& frame) {
            return capture_fresh_application_image() &&
                submit_frame(
                    frame.predictedDisplayTime + kFakeDisplayPeriod) &&
                wait_for_queue_idle();
        };

        bool sequence_succeeded =
            XR_SUCCEEDED(wait_frame(
                session,
                &frame_wait_info,
                &application_frames[0])) &&
            application_frames[0].predictedDisplayPeriod ==
                kFakeDisplayPeriod &&
            wait_next_while_beginning_current(application_frames[1]) &&
            application_frames[1].predictedDisplayPeriod ==
                kFakeDisplayPeriod &&
            submit_flight_frame(application_frames[0]) &&
            wait_next_while_beginning_current(application_frames[2]) &&
            application_frames[2].predictedDisplayPeriod ==
                kFakeDisplayPeriod * 2;
        const std::uint32_t downstream_waits_before_transition =
            g_wait_frame_calls.load(std::memory_order_acquire);
        sequence_succeeded = sequence_succeeded &&
            downstream_waits_before_transition == 2;
        g_delay_composition_validation.store(true, std::memory_order_release);
        sequence_succeeded = sequence_succeeded &&
            submit_flight_frame(application_frames[1]);
        application_space = fake_handle<XrSpace>(0x405);
        projection.space = application_space;
        flight_quad.space = application_space;
        kneeboard.space=application_space;
        g_valid_composition_space.store(
            application_space,
            std::memory_order_release);
        sequence_succeeded = sequence_succeeded &&
            wait_next_while_beginning_current(application_frames[3]) &&
            application_frames[3].predictedDisplayPeriod ==
                kFakeDisplayPeriod * 2 &&
            submit_flight_frame(application_frames[2]) &&
            // One more application frame than the pairing needs on its own:
            // the frame that arms generation passes through, so reaching the
            // same generation depth takes one frame longer.
            wait_next_while_beginning_current(application_frames[4]) &&
            submit_flight_frame(application_frames[3]) &&
            XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
            submit_flight_frame(application_frames[4]);

        const bool destroyed_before_end_session = g_destroy_pending_swapchain &&
            XR_SUCCEEDED(destroy_swapchain(swapchain));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            (destroyed_before_end_session || XR_SUCCEEDED(destroy_swapchain(swapchain))) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        constexpr std::array expected_generated_order{
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
        };
        std::size_t matched_targets = 0;
        std::size_t empty_frames = 0;
        if(g_shared_panel_mode) {
            size_t checked=0;
            for(size_t i=1;i+1<end_records.size();++i) {
                const auto& r=end_records[i];
                if(r.quad_layer_count!=2)continue;
                if(r.second_quad_x!=5.f){std::cerr<<"Shared atlas kneeboard moved: "<<r.second_quad_x;return EXIT_FAILURE;}
                if(r.target!=SubmittedTarget::synthetic)continue;
                size_t a=i,b=i;
                while(a&&end_records[a].target!=SubmittedTarget::current)--a;
                while(b+1<end_records.size()&&end_records[b].target!=SubmittedTarget::current)++b;
                if(end_records[a].target==SubmittedTarget::current&&end_records[b].target==SubmittedTarget::current&&
                    end_records[b].quad_x-end_records[a].quad_x>.05f) {
                    if(!(r.quad_x>end_records[a].quad_x&&r.quad_x<end_records[b].quad_x)) {
                        std::cerr<<"Panel pose outside source interval: "<<end_records[a].quad_x<<" / "<<r.quad_x<<" / "<<end_records[b].quad_x;return EXIT_FAILURE;
                    }
                    ++checked;
                }
            }
            if(!checked){std::cerr<<"No synthetic panel poses checked";return EXIT_FAILURE;}
            std::cout<<"Shared atlas panel positions checked: "<<checked<<'\n';
        }
        bool composition_preserved = true;
        for (const EndFrameRecord& record : end_records) {
            if (matched_targets < expected_generated_order.size() &&
                record.target == expected_generated_order[matched_targets]) {
                ++matched_targets;
            }
            if (record.layer_count == 0) {
                ++empty_frames;
            } else {
                composition_preserved = composition_preserved &&
                    record.layer_count == (g_shared_panel_mode?3:2) &&
                    record.quad_layer_count == (g_shared_panel_mode?2:1) &&
                    record.quad_layer_index == 1;
            }
        }
        const bool only_optional_teardown_empty = empty_frames == 0 ||
            (empty_frames == 1 && !end_records.empty() &&
             end_records.back().layer_count == 0);
        const std::uint32_t downstream_waits =
            g_wait_frame_calls.load(std::memory_order_relaxed);
        const bool valid = sequence_succeeded && teardown_succeeded &&
            g_submission_after_destroy.load() == 0 &&
            matched_targets == expected_generated_order.size() &&
            composition_preserved && only_optional_teardown_empty &&
            downstream_waits > downstream_waits_before_transition &&
            downstream_waits ==
                g_begin_frame_calls.load(std::memory_order_relaxed) &&
            downstream_waits ==
                g_end_frame_calls.load(std::memory_order_relaxed) &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "pipelined presenter validation failed: sequence="
                      << sequence_succeeded << " teardown="
                      << teardown_succeeded << " matched=" << matched_targets
                      << " after-destroy=" << g_submission_after_destroy.load()
                      << " waits-before=" << downstream_waits_before_transition
                      << " empty=" << empty_frames << " composition="
                      << composition_preserved
                      << " waits=" << downstream_waits << " begins="
                      << g_begin_frame_calls.load() << " ends="
                      << g_end_frame_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR pipelined continuous-presenter test passed\n";
        return EXIT_SUCCESS;
    }

    if (g_steamvr_presenter_mode) {
        // The promotion is requested when the throttled streak completes and
        // takes effect at the next xrEndFrame, so one more application frame
        // runs inline at the real rate before the virtual half-rate loop starts.
        std::array<XrFrameState, 8> application_frames{{
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
            {XR_TYPE_FRAME_STATE},
        }};
        bool frame_sequence_succeeded = true;
        for (std::size_t index = 0; index < 4; ++index) {
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(wait_frame(
                    session,
                    &frame_wait_info,
                    &application_frames[index])) &&
                application_frames[index].predictedDisplayPeriod ==
                    kFakeDisplayPeriod &&
                (index == 0 ||
                 application_frames[index].predictedDisplayTime >
                     application_frames[index - 1].predictedDisplayTime) &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                capture_fresh_application_image() &&
                submit_frame(application_frames[index].predictedDisplayTime);
        }

        // Two ordinary paired frames. The presenter is promoted earlier now
        // that the layer measures the runtime's pacing directly instead of
        // waiting for three throttled inline cycles, so these stand in for the
        // warm-up frames the old criterion needed to reach the same depth of
        // generated submissions.
        XrFrameState filler_frame{XR_TYPE_FRAME_STATE};
        for (int filler = 0; filler < 2; ++filler) {
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(wait_frame(
                    session, &frame_wait_info, &filler_frame)) &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info)) &&
                capture_fresh_application_image() &&
                submit_frame(filler_frame.predictedDisplayTime);
        }

        // frameWaitInfo and frameBeginInfo are optional in the registry, and the
        // presenter answers both calls itself rather than forwarding them, so
        // this frame passes null to keep that path accepting what a runtime
        // would. Luke Ross's mods call xrWaitFrame(session, NULL, &state).
        frame_sequence_succeeded = frame_sequence_succeeded &&
            XR_SUCCEEDED(wait_frame(
                session,
                nullptr,
                &application_frames[4])) &&
            application_frames[4].predictedDisplayPeriod ==
                kFakeDisplayPeriod * 2 &&
            application_frames[4].predictedDisplayTime >
                application_frames[3].predictedDisplayTime &&
            XR_SUCCEEDED(begin_frame(session, nullptr)) &&
            capture_fresh_application_image() &&
            submit_frame(application_frames[4].predictedDisplayTime);

        if (g_destroy_pending_space) {
            frame_sequence_succeeded = frame_sequence_succeeded && destroy_space &&
                XR_SUCCEEDED(destroy_space(application_space));
            // The presenter must not autonomously repeat a frame naming the
            // destroyed space while the application prepares its next frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
        }

        XrFrameEndInfo empty_end{XR_TYPE_FRAME_END_INFO};
        if (frame_sequence_succeeded) {
            frame_sequence_succeeded =
                XR_SUCCEEDED(wait_frame(
                    session,
                    &frame_wait_info,
                    &application_frames[5])) &&
                application_frames[5].predictedDisplayPeriod ==
                    kFakeDisplayPeriod * 2 &&
                application_frames[5].predictedDisplayTime >
                    application_frames[4].predictedDisplayTime &&
                XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
            empty_end.displayTime = application_frames[5].predictedDisplayTime;
            empty_end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            frame_sequence_succeeded = frame_sequence_succeeded &&
                XR_SUCCEEDED(end_frame(session, &empty_end));
        }

        const bool teardown_succeeded =
            XR_SUCCEEDED(end_session(session)) &&
            XR_SUCCEEDED(destroy_swapchain(swapchain)) &&
            XR_SUCCEEDED(destroy_session(session)) &&
            XR_SUCCEEDED(destroy_instance(instance));
        FreeLibrary(module);

        std::vector<EndFrameRecord> end_records;
        {
            std::scoped_lock lock(g_end_records_mutex);
            end_records = g_end_records;
        }
        constexpr std::array<SubmittedTarget, 9> expected_generated_order{
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
            SubmittedTarget::synthetic,
            SubmittedTarget::current,
        };
        std::size_t matched_targets = 0;
        for (const EndFrameRecord& record : end_records) {
            if (matched_targets < expected_generated_order.size() &&
                record.target == expected_generated_order[matched_targets]) {
                ++matched_targets;
            }
        }
        const std::uint32_t waits =
            g_wait_frame_calls.load(std::memory_order_relaxed);
        const bool valid = frame_sequence_succeeded && teardown_succeeded &&
            g_submission_after_destroy.load() == 0 &&
            matched_targets == expected_generated_order.size() &&
            waits == g_begin_frame_calls.load(std::memory_order_relaxed) &&
            waits == g_end_frame_calls.load(std::memory_order_relaxed) &&
            waits > application_frames.size() &&
            g_current_acquire_calls.load(std::memory_order_relaxed) >= 5 &&
            g_synthetic_acquire_calls.load(std::memory_order_relaxed) >= 4 &&
            g_waited_display_times.empty() && !g_begun_display_time;
        if (!valid) {
            std::cerr << "SteamVR presenter validation failed: sequence="
                      << frame_sequence_succeeded << " teardown="
                      << teardown_succeeded << " matched=" << matched_targets
                      << " after-destroy=" << g_submission_after_destroy.load()
                      << " waits=" << waits << " begins="
                      << g_begin_frame_calls.load() << " ends="
                      << g_end_frame_calls.load() << '\n';
            return EXIT_FAILURE;
        }
        std::cout << "OpenXR SteamVR continuous-presenter test passed\n";
        return EXIT_SUCCESS;
    }

    // The frame that first names a swapchain as a projection view is the frame
    // that arms generation, and it passes through unchanged: taking the
    // resources between the application's xrEndFrame and the submission that
    // follows would put their creation cost on that frame's deadline. So the
    // first generated submission lands one display period later than the naive
    // schedule, every display time after it moves with it, and the session
    // ends with one generated pair fewer and one more application frame passed
    // through in its place. This holds for every graphics API: the D3D11
    // interop, which also does not exist until that first projection use, is
    // no longer a special case.
    const XrTime arm_shift = -100;
    const std::uint32_t arm_skip = 1U;

    // A is primed while B is already waited, proving that the first generated
    // submission still performs exactly one downstream end.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_a)) ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_b)) ||
        frame_a.predictedDisplayTime != 100 || frame_b.predictedDisplayTime != 200 ||
        !submit_frame(frame_a.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_b.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // A failed synthetic release must preserve the original application
    // projection and recover its pending ownership on the following pair.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_c)) ||
        frame_c.predictedDisplayTime != 400 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }
    g_fail_next_synthetic_release.store(true, std::memory_order_release);
    if (!submit_frame(frame_c.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_d)) ||
        frame_d.predictedDisplayTime != 500 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // The internal current cycle is serialized against a concurrent
    // application wait. shouldRender=false still closes that cycle legally.
    g_next_wait_should_not_render.store(true, std::memory_order_release);
    g_block_atomic_end_time.store(600 + arm_shift, std::memory_order_release);
    g_block_atomic_end.store(true, std::memory_order_release);
    bool frame_d_submit_succeeded = false;
    std::thread frame_d_submit_thread([&] {
        frame_d_submit_succeeded = submit_frame(frame_d.predictedDisplayTime);
    });
    const auto atomic_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!g_atomic_end_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < atomic_deadline) {
        std::this_thread::yield();
    }
    if (!g_atomic_end_entered.load(std::memory_order_acquire)) {
        g_allow_atomic_end_return.store(true, std::memory_order_release);
        frame_d_submit_thread.join();
        return EXIT_FAILURE;
    }

    XrResult frame_e_wait_result = XR_ERROR_RUNTIME_FAILURE;
    const std::uint32_t waits_before_concurrent_application =
        g_wait_frame_calls.load(std::memory_order_acquire);
    std::thread frame_e_wait_thread([&] {
        frame_e_wait_result = wait_frame(session, &frame_wait_info, &frame_e);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool internal_sequence_was_atomic =
        g_wait_frame_calls.load(std::memory_order_acquire) ==
        waits_before_concurrent_application;
    g_allow_atomic_end_return.store(true, std::memory_order_release);
    frame_d_submit_thread.join();
    frame_e_wait_thread.join();
    if (!internal_sequence_was_atomic || !frame_d_submit_succeeded ||
        XR_FAILED(frame_e_wait_result) || frame_e.predictedDisplayTime != 700 + arm_shift ||
        !wait_for_queue_idle() ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // A failed internal wait occurs only after the synthetic application end.
    // It clears continuity, so the following frame must prime again.
    g_fail_next_wait_frame.store(true, std::memory_order_release);
    if (!submit_frame(frame_e.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_f)) ||
        frame_f.predictedDisplayTime != 800 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_f.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_g)) ||
        frame_g.predictedDisplayTime != 900 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image()) {
        return EXIT_FAILURE;
    }

    // FidelityFX must tolerate an occasional slow synthetic downstream end.
    // The NVIDIA-only recoverable cooldown must not suppress its matching
    // internal current-B cycle or clear otherwise valid continuity.
    const std::uint32_t waits_before_slow_pair =
        g_wait_frame_calls.load(std::memory_order_acquire);
    const std::uint32_t begins_before_slow_pair =
        g_begin_frame_calls.load(std::memory_order_acquire);
    const std::uint32_t ends_before_slow_pair =
        g_end_frame_calls.load(std::memory_order_acquire);
    g_delay_end_time.store(frame_g.predictedDisplayTime, std::memory_order_release);
    if (!submit_frame(frame_g.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        g_wait_frame_calls.load(std::memory_order_acquire) != waits_before_slow_pair + 1 ||
        g_begin_frame_calls.load(std::memory_order_acquire) != begins_before_slow_pair + 1 ||
        g_end_frame_calls.load(std::memory_order_acquire) != ends_before_slow_pair + 2) {
        return EXIT_FAILURE;
    }

    // The following FidelityFX frame remains a pair instead of becoming a
    // three-second original-frame pass-through window.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_h)) ||
        frame_h.predictedDisplayTime != 1100 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_h.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // Continue two more ordinary pairs to prove the slow end did not leave a
    // delayed cooldown or force a fresh-prime discontinuity.
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &frame_i)) ||
        frame_i.predictedDisplayTime != 1300 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_i.predictedDisplayTime) ||
        !wait_for_queue_idle() ||
        XR_FAILED(wait_frame(session, &frame_wait_info, &frame_j)) ||
        frame_j.predictedDisplayTime != 1500 + arm_shift ||
        XR_FAILED(begin_frame(session, &frame_begin_info)) ||
        !capture_fresh_application_image() ||
        !submit_frame(frame_j.predictedDisplayTime) ||
        !wait_for_queue_idle()) {
        return EXIT_FAILURE;
    }

    // AER+AFW can pipeline a second application wait from another thread just
    // before the first wait's thread calls begin. The second wait must remain
    // outside the runtime until that matching begin has been forwarded.
    g_wait_begin_handoff_calls.store(0, std::memory_order_release);
    g_second_handoff_wait_entered.store(false, std::memory_order_release);
    g_allow_second_handoff_wait_return.store(false, std::memory_order_release);
    g_wait_begin_handoff_mode.store(true, std::memory_order_release);
    if (XR_FAILED(wait_frame(session, &frame_wait_info, &handoff_frame_a)) ||
        handoff_frame_a.predictedDisplayTime != 1700 + arm_shift) {
        g_wait_begin_handoff_mode.store(false, std::memory_order_release);
        return EXIT_FAILURE;
    }

    XrResult second_handoff_wait_result = XR_ERROR_RUNTIME_FAILURE;
    std::thread second_handoff_wait([&] {
        second_handoff_wait_result =
            wait_frame(session, &frame_wait_info, &handoff_frame_b);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool second_wait_overtook_begin =
        g_second_handoff_wait_entered.load(std::memory_order_acquire);
    const auto begin_start = std::chrono::steady_clock::now();
    const XrResult first_handoff_begin_result =
        begin_frame(session, &frame_begin_info);
    const auto begin_duration = std::chrono::steady_clock::now() - begin_start;
    g_allow_second_handoff_wait_return.store(true, std::memory_order_release);
    second_handoff_wait.join();
    g_wait_begin_handoff_mode.store(false, std::memory_order_release);

    XrFrameEndInfo handoff_end_info{XR_TYPE_FRAME_END_INFO};
    handoff_end_info.displayTime = handoff_frame_a.predictedDisplayTime;
    handoff_end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    const bool handoff_valid =
        !second_wait_overtook_begin &&
        begin_duration < std::chrono::milliseconds(100) &&
        XR_SUCCEEDED(first_handoff_begin_result) &&
        XR_SUCCEEDED(second_handoff_wait_result) &&
        handoff_frame_b.predictedDisplayTime == 1800 + arm_shift &&
        XR_SUCCEEDED(end_frame(session, &handoff_end_info)) &&
        XR_SUCCEEDED(begin_frame(session, &frame_begin_info));
    handoff_end_info.displayTime = handoff_frame_b.predictedDisplayTime;
    if (!handoff_valid || XR_FAILED(end_frame(session, &handoff_end_info)) ||
        XR_FAILED(end_session(session)) ||
        XR_FAILED(destroy_swapchain(swapchain)) ||
        XR_FAILED(destroy_session(session)) ||
        XR_FAILED(destroy_instance(instance))) {
        return EXIT_FAILURE;
    }

    FreeLibrary(module);
    std::ifstream log_stream(argv[2]);
    std::ostringstream log_text;
    log_text << log_stream.rdbuf();
    const std::string log = log_text.str();
    // Which display time carries the first generated pair is not a property of
    // the layer worth pinning. The private swapchains are taken when a
    // projection layer first names an application swapchain, so the frame that
    // arms is itself a prime and the first pair is the one after it, and the
    // first synthetic in the log is not necessarily one whose internal cycle
    // completes - a failed synthetic release or a failed internal wait leaves
    // one standing alone. Walk the synthetics until one is followed by its own
    // internal current cycle with nothing in between; what the assertions below
    // check is that the cycle follows the synthetic and carries the next
    // display time. Every locate in the session is pinned separately by
    // expected_locate_times, which is what proves the internal cycle locates no
    // views of its own.
    const std::string synthetic_prefix =
        "[XRFG-FAKE] downstream end frame target=synthetic time=";
    const std::string locate_marker = "[XRFG-FAKE] downstream locate views";
    std::size_t first_synthetic_end = std::string::npos;
    std::size_t first_internal_wait = std::string::npos;
    std::size_t first_internal_begin = std::string::npos;
    std::size_t first_current_end = std::string::npos;
    for (std::size_t candidate = log.find(synthetic_prefix);
         candidate != std::string::npos;
         candidate =
             log.find(synthetic_prefix, candidate + synthetic_prefix.size())) {
        const std::size_t value_begin = candidate + synthetic_prefix.size();
        std::size_t value_end = value_begin;
        long long synthetic_time = 0;
        while (value_end < log.size() && log[value_end] >= '0' &&
               log[value_end] <= '9') {
            synthetic_time = synthetic_time * 10 + (log[value_end] - '0');
            ++value_end;
        }
        if (value_end == value_begin) {
            continue;
        }
        const std::string internal_time = std::to_string(synthetic_time + 100);
        const std::size_t current_end = log.find(
            "[XRFG-FAKE] downstream end frame target=current time=" +
                internal_time,
            candidate);
        if (current_end == std::string::npos) {
            continue;
        }
        // An application frame always locates views before it submits, so a
        // locate between the two ends means this current came from the
        // application rather than from the synthetic's internal cycle.
        if (log.find(locate_marker, candidate) < current_end) {
            continue;
        }
        first_synthetic_end = candidate;
        first_current_end = current_end;
        first_internal_wait = log.find(
            "[XRFG-FAKE] downstream wait frame time=" + internal_time,
            candidate);
        first_internal_begin = log.find(
            "[XRFG-FAKE] downstream begin frame time=" + internal_time,
            candidate);
        break;
    }

    std::vector<EndFrameRecord> end_records;
    {
        std::scoped_lock lock(g_end_records_mutex);
        end_records = g_end_records;
    }
    std::vector<LocateViewsRecord> locate_records;
    {
        std::scoped_lock lock(g_locate_records_mutex);
        locate_records = g_locate_records;
    }
    const auto nearly_equal = [](float actual, float expected) {
        return std::fabs(actual - expected) <= 1.0e-5F;
    };
    const auto pose_matches = [&](const XrPosef& actual, const XrPosef& expected) {
        return nearly_equal(actual.orientation.x, expected.orientation.x) &&
               nearly_equal(actual.orientation.y, expected.orientation.y) &&
               nearly_equal(actual.orientation.z, expected.orientation.z) &&
               nearly_equal(actual.orientation.w, expected.orientation.w) &&
               nearly_equal(actual.position.x, expected.position.x) &&
               nearly_equal(actual.position.y, expected.position.y) &&
               nearly_equal(actual.position.z, expected.position.z);
    };
    const auto fov_matches = [&](const XrFovf& actual, const XrFovf& expected) {
        return nearly_equal(actual.angleLeft, expected.angleLeft) &&
               nearly_equal(actual.angleRight, expected.angleRight) &&
               nearly_equal(actual.angleUp, expected.angleUp) &&
               nearly_equal(actual.angleDown, expected.angleDown);
    };
    const XrView located_reference = fake_view_for_time(100, 0);
    const XrView submitted_reference = fake_submitted_view_for_time(100, 0);
    const bool submitted_camera_differs_from_locate =
        !pose_matches(submitted_reference.pose, located_reference.pose) &&
        !fov_matches(submitted_reference.fov, located_reference.fov);
    const auto record_matches = [&](std::size_t index,
                                    XrTime time,
                                    SubmittedTarget target,
                                    std::uint32_t layer_count,
                                    XrTime metadata_time) {
        if (index >= end_records.size() ||
            end_records[index].display_time != time ||
            end_records[index].target != target ||
            end_records[index].layer_count != layer_count) {
            return false;
        }
        if (layer_count == 0) {
            return end_records[index].space == XR_NULL_HANDLE;
        }
        if (end_records[index].space != g_space) {
            return false;
        }
        if (g_double_wide_mode &&
            (end_records[index].image_rects[0].offset.x != 0 ||
             end_records[index].image_rects[0].extent.width != 4 ||
             end_records[index].image_rects[1].offset.x != 4 ||
             end_records[index].image_rects[1].extent.width != 4)) {
            return false;
        }
        for (std::uint32_t view_index = 0; view_index < 2; ++view_index) {
            const XrView expected = fake_submitted_view_for_time(
                metadata_time,
                view_index);
            if (!pose_matches(end_records[index].poses[view_index], expected.pose) ||
                !fov_matches(end_records[index].fovs[view_index], expected.fov)) {
                return false;
            }
        }
        return true;
    };

    struct ExpectedEnd {
        XrTime display_time;
        SubmittedTarget target;
        std::uint32_t layer_count;
        XrTime metadata_time;
    };

    // The first frame arms and passes through, the second one primes, and the
    // pair after that is the first generated one. Every submission from the
    // failed-synthetic-release frame onwards therefore carries arm_shift.
    std::vector<ExpectedEnd> expected_ends{
        {100, SubmittedTarget::original, 1, 100},
        {200, SubmittedTarget::current, 1, 200},
    };
    for (const ExpectedEnd& record : std::initializer_list<ExpectedEnd>{
             {400, SubmittedTarget::original, 1, 400},
             {500, SubmittedTarget::synthetic, 1, 500},
             {600, SubmittedTarget::none, 0, 0},
             {700, SubmittedTarget::synthetic, 1, 700},
             {800, SubmittedTarget::current, 1, 800},
             {900, SubmittedTarget::synthetic, 1, 900},
             {1000, SubmittedTarget::current, 1, 900},
             {1100, SubmittedTarget::synthetic, 1, 1100},
             {1200, SubmittedTarget::current, 1, 1100},
             {1300, SubmittedTarget::synthetic, 1, 1300},
             {1400, SubmittedTarget::current, 1, 1300},
             {1500, SubmittedTarget::synthetic, 1, 1500},
             {1600, SubmittedTarget::current, 1, 1500},
             {1700, SubmittedTarget::none, 0, 0},
             {1800, SubmittedTarget::none, 0, 0},
         }) {
        expected_ends.push_back(ExpectedEnd{
            record.display_time + arm_shift,
            record.target,
            record.layer_count,
            record.metadata_time == 0 ? 0 : record.metadata_time + arm_shift,
        });
    }

    bool frame_outputs_valid = end_records.size() == expected_ends.size();
    for (std::size_t index = 0;
         frame_outputs_valid && index < expected_ends.size();
         ++index) {
        frame_outputs_valid = record_matches(index,
                                             expected_ends[index].display_time,
                                             expected_ends[index].target,
                                             expected_ends[index].layer_count,
                                             expected_ends[index].metadata_time);
    }

    const std::array<XrTime, 10> expected_locate_times{
        100,
        200,
        400 + arm_shift,
        500 + arm_shift,
        700 + arm_shift,
        800 + arm_shift,
        900 + arm_shift,
        1100 + arm_shift,
        1300 + arm_shift,
        1500 + arm_shift,
    };
    bool locate_sequence_valid = locate_records.size() == expected_locate_times.size();
    for (std::size_t index = 0;
         locate_sequence_valid && index < expected_locate_times.size();
         ++index) {
        locate_sequence_valid =
            locate_records[index].display_time == expected_locate_times[index] &&
            locate_records[index].space == g_space &&
            locate_records[index].view_configuration_type ==
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }

    const bool valid =
        frame_outputs_valid &&
        locate_sequence_valid &&
        submitted_camera_differs_from_locate &&
        log.find("[XRFG-V") == std::string::npos &&
        first_synthetic_end != std::string::npos &&
        first_internal_wait != std::string::npos &&
        first_internal_begin != std::string::npos &&
        first_current_end != std::string::npos &&
        first_synthetic_end < first_internal_wait &&
        first_internal_wait < first_internal_begin &&
        first_internal_begin < first_current_end &&
        count_occurrences(log, "[XRFG-FAKE] downstream release entered") == 14 &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=current") == 7 - arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=synthetic") == 7 - arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=original") == 1 + arm_skip &&
        count_occurrences(log, "[XRFG-FAKE] downstream end frame target=none") == 3 &&
        count_occurrences(log, "[XRFG-FAKE] downstream locate views") == 10 &&
        g_wait_frame_calls.load(std::memory_order_relaxed) == 19 - arm_skip &&
        g_begin_frame_calls.load(std::memory_order_relaxed) == 19 - arm_skip &&
        g_end_frame_calls.load(std::memory_order_relaxed) == 18 - arm_skip &&
        g_locate_views_calls.load(std::memory_order_relaxed) == 10 &&
        // One application swapchain and two complete private pair slots.
        g_create_swapchain_calls.load(std::memory_order_relaxed) == 5 &&
        g_destroy_swapchain_calls.load(std::memory_order_relaxed) == 5 &&
        g_application_release_calls.load(std::memory_order_relaxed) == 14 &&
        g_current_acquire_calls.load(std::memory_order_relaxed) == 10 - arm_skip &&
        g_current_wait_calls.load(std::memory_order_relaxed) == 10 - arm_skip &&
        g_current_release_calls.load(std::memory_order_relaxed) == 10 - arm_skip &&
        g_synthetic_acquire_calls.load(std::memory_order_relaxed) == 8 - arm_skip &&
        g_synthetic_wait_calls.load(std::memory_order_relaxed) == 8 - arm_skip &&
        g_synthetic_release_calls.load(std::memory_order_relaxed) == 9 - arm_skip &&
        g_waited_display_times.empty() &&
        !g_begun_display_time &&
        g_current_create_info_valid.load(std::memory_order_acquire) &&
        g_synthetic_create_info_valid.load(std::memory_order_acquire);
    if (!valid) {
        std::cerr << "call-chain validation failed: outputs="
                  << frame_outputs_valid << " locate=" << locate_sequence_valid
                  << " releases=" << g_application_release_calls.load()
                  << " current=" << g_current_acquire_calls.load() << '/'
                  << g_current_wait_calls.load() << '/'
                  << g_current_release_calls.load()
                  << " synthetic=" << g_synthetic_acquire_calls.load() << '/'
                  << g_synthetic_wait_calls.load() << '/'
                  << g_synthetic_release_calls.load()
                  << " waits=" << g_wait_frame_calls.load()
                  << " begins=" << g_begin_frame_calls.load()
                  << " ends=" << g_end_frame_calls.load()
                  << " locates=" << g_locate_views_calls.load() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << (g_double_wide_mode
                      ? "OpenXR double-wide fake-runtime call-chain test passed\n"
                      : "OpenXR layer fake-runtime call-chain test passed\n");
    return EXIT_SUCCESS;
}
