#include "xrfg/luke_vulkan_scope.hpp"
#define XR_USE_GRAPHICS_API_VULKAN
#include "xrfg/vulkan_session_bridge.hpp"
#include "xrfg/d3d11_bridge.hpp"
#include "xrfg/d3d11_dlss_guides.hpp"
#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "xrfg/three_slot_buffer.hpp"
#include "xrfg/djules_triple_release.hpp"
#include "xrfg/two_output_schedule.hpp"
#include "xrfg/optical_pacing_reserve.hpp"
#include "xrfg/midpoint_schedule.hpp"
#include "xrfg/frame_capture.hpp"
#include "xrfg/diagnostic_ring_sink.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_guide_eye.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/d3d11_color_identity.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/benchmark_log.hpp"
#include "xrfg/benchmark_view_extent.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/fsr_cadence_probe.hpp"
#include "xrfg/luke_render_identity.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/luke_afw.hpp"
#include "xrfg/luke_afw_source.hpp"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include "xrfg/luke_afw_startup.hpp"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_origin_loss_capture.hpp"
#include "xrfg/luke_origin_continuity.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "xrfg/luke_world_scale.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/ngx_guide_capture.hpp"
#include "xrfg/native_ingress.hpp"
#include "xrfg/vulkan_native_ingress.hpp"
#include "autonomous_runtime.hpp"
#include "optiscaler_bootstrap.hpp"
#include "xrfg/presentation_transform.h"
#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/provider_api.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "xrfg/luke_host_config.hpp"
#include <atomic>
#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/steamvr_delivery.hpp"
#include "xrfg/private_openxr_runtime.hpp"
#include "xrfg/runtime_acceptance_rate.hpp"
#include "xrfg/private_delivery_queue.hpp"

#include <windows.h>
#include <psapi.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
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
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>


// Standalone layer: the tray publishes desired settings; this process owns GPU work.
namespace {

[[nodiscard]] std::filesystem::path current_layer_directory() noexcept;



constexpr char kLayerName[] = "XR_APILAYER_OFXRBridge";
constexpr XrVersion kLayerApiVersion = XR_MAKE_VERSION(1, 0, 0);
constexpr XrDuration kGenerationCooldownDuration = 1'000'000'000;
// Consecutive submissions one scanout apart before any phase correction is
// allowed to run. Phase means nothing until the rate is right: a grid that is
// skipping slots has no stable phase to correct towards, and correcting one
// anyway drives a feedback loop - see the comments at both correction sites.
// Eight is a quarter of a second at 90 Hz.
constexpr std::uint32_t kPhaseCorrectionGridStreak = 8;
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

// The runtime's Vulkan entry points, by the names the layer gives them. The
// enable2 variants share the signatures of the originals.
using PFN_GetVulkanExtensions = PFN_xrGetVulkanInstanceExtensionsKHR;
using PFN_CreateVulkanInstance = PFN_xrCreateVulkanInstanceKHR;
using PFN_CreateVulkanDevice = PFN_xrCreateVulkanDeviceKHR;
using PFN_GetVulkanGraphicsDevice = PFN_xrGetVulkanGraphicsDeviceKHR;
using PFN_GetVulkanGraphicsDevice2 = PFN_xrGetVulkanGraphicsDevice2KHR;
using PFN_GetVulkanGraphicsRequirements = PFN_xrGetVulkanGraphicsRequirementsKHR;

struct VulkanNegotiationDispatch {
    PFN_GetVulkanExtensions get_instance_extensions{};
    PFN_GetVulkanExtensions get_device_extensions{};
    PFN_CreateVulkanInstance create_instance{};
    PFN_CreateVulkanDevice create_device{};
    PFN_GetVulkanGraphicsDevice get_graphics_device{};
    PFN_GetVulkanGraphicsDevice2 get_graphics_device2{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements{};
    PFN_GetVulkanGraphicsRequirements get_graphics_requirements2{};
};

// The Vulkan extensions an interop with D3D12 would need, as bits; the
// numbering is the one BridgeFlightOperation::vulkan_negotiation documents.
[[nodiscard]] std::uint64_t vulkan_interop_extension_bit(
    std::string_view name) noexcept {
    constexpr std::array<std::string_view, 11> kNames{
        "VK_KHR_external_memory_win32",
        "VK_KHR_external_semaphore_win32",
        "VK_KHR_external_memory",
        "VK_KHR_external_semaphore",
        "VK_KHR_timeline_semaphore",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_win32_keyed_mutex",
        "VK_KHR_external_memory_capabilities",
        "VK_KHR_external_semaphore_capabilities",
        "VK_KHR_get_physical_device_properties2",
    };
    for (std::size_t index = 0; index < kNames.size(); ++index) {
        if (name == kNames[index]) {
            return 1ULL << index;
        }
    }
    return 0;
}

struct VulkanExtensionSummary {
    std::uint64_t count{};
    std::uint64_t bits{};
};

// The runtime's answer to xrGetVulkan*ExtensionsKHR: one space-separated
// string.
[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_string(
    std::string_view list) noexcept {
    VulkanExtensionSummary summary{};
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t end = list.find(' ', start);
        const std::string_view name = list.substr(
            start,
            end == std::string_view::npos ? list.size() - start : end - start);
        if (!name.empty()) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(name);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return summary;
}

[[nodiscard]] VulkanExtensionSummary summarize_vulkan_extension_array(
    const char* const* names,
    std::uint32_t count) noexcept {
    VulkanExtensionSummary summary{};
    if (names == nullptr) {
        return summary;
    }
    for (std::uint32_t index = 0; index < count; ++index) {
        if (names[index] != nullptr) {
            ++summary.count;
            summary.bits |= vulkan_interop_extension_bit(names[index]);
        }
    }
    return summary;
}

[[nodiscard]] std::uint64_t pack_negotiation_results(
    XrResult xr_result,
    VkResult vk_result) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(xr_result))
            << 32) |
        static_cast<std::uint32_t>(vk_result);
}

void log_vulkan_negotiation(
    std::int64_t selector,
    std::uint64_t a,
    std::uint64_t b = 0,
    std::uint64_t c = 0) noexcept {
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::vulkan_negotiation, selector, a, b, c);
}

// Whether the application's Vulkan device can take part in a D3D12 interop,
// asked of the device itself: with XR_KHR_vulkan_enable the application
// creates the device, so the list it was handed says what it was asked to
// enable, not what it did. A device returns no entry point for a command of
// an extension it did not enable.
//   11  a= commands the device returned: bit 0 vkGetMemoryWin32HandleKHR,
//       1 vkGetSemaphoreWin32HandleKHR, 2 vkImportSemaphoreWin32HandleKHR,
//       3 vkGetSemaphoreCounterValueKHR, 4 vkGetSemaphoreCounterValue
//   12  a= the same interop extension bits, for what the physical device
//       supports at all; b= how many device extensions it lists
void probe_vulkan_interop_support(
    const XrGraphicsBindingVulkanKHR& binding) noexcept {
    try {
        const HMODULE vulkan = GetModuleHandleW(L"vulkan-1.dll");
        if (vulkan == nullptr || binding.device == VK_NULL_HANDLE) {
            log_vulkan_negotiation(11, 0, 0, 1);
            return;
        }
        const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            GetProcAddress(vulkan, "vkGetDeviceProcAddr"));
        const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
        if (get_device_proc != nullptr) {
            constexpr std::array<const char*, 5> kCommands{
                "vkGetMemoryWin32HandleKHR",
                "vkGetSemaphoreWin32HandleKHR",
                "vkImportSemaphoreWin32HandleKHR",
                "vkGetSemaphoreCounterValueKHR",
                "vkGetSemaphoreCounterValue",
            };
            std::uint64_t live = 0;
            for (std::size_t index = 0; index < kCommands.size(); ++index) {
                if (get_device_proc(binding.device, kCommands[index]) != nullptr) {
                    live |= 1ULL << index;
                }
            }
            log_vulkan_negotiation(11, live);
        }
        if (get_instance_proc != nullptr && binding.instance != VK_NULL_HANDLE &&
            binding.physicalDevice != VK_NULL_HANDLE) {
            const auto enumerate =
                reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                    get_instance_proc(
                        binding.instance, "vkEnumerateDeviceExtensionProperties"));
            std::uint32_t extension_count = 0;
            if (enumerate != nullptr &&
                enumerate(binding.physicalDevice, nullptr, &extension_count,
                          nullptr) == VK_SUCCESS &&
                extension_count > 0) {
                std::vector<VkExtensionProperties> extensions(extension_count);
                if (enumerate(binding.physicalDevice, nullptr, &extension_count,
                              extensions.data()) >= VK_SUCCESS) {
                    std::uint64_t supported = 0;
                    for (std::uint32_t index = 0; index < extension_count; ++index) {
                        supported |= vulkan_interop_extension_bit(
                            extensions[index].extensionName);
                    }
                    log_vulkan_negotiation(12, supported, extension_count);
                }
            }
        }
    } catch (...) {
    }
}

struct Dispatch {
    bool vulkan_bridge{};
    VulkanNegotiationDispatch vulkan{};
    PFN_xrEnumerateSwapchainFormats enumerate_swapchain_formats{};
    // The D3D11 bridge (SessionState::d3d11_bridge): set when the ini asks
    // for it, the application enabled XR_KHR_D3D11_enable, and the layer
    // could add XR_KHR_D3D12_enable to the instance. The runtime's D3D12
    // requirements call has to be made before a D3D12 session is created.
    bool d3d11_bridge{};
    PFN_xrGetD3D12GraphicsRequirementsKHR get_d3d12_graphics_requirements{};
    XrInstance instance{XR_NULL_HANDLE};
    PFN_xrGetInstanceProcAddr get_instance_proc_addr{};
    PFN_xrConvertTimeToWin32PerformanceCounterKHR convert_time_to_qpc{};
    PFN_xrLocateViews locate_views{};
    PFN_xrCreateReferenceSpace create_reference_space{};
    PFN_xrCreateActionSpace create_action_space{};
    PFN_xrLocateSpace locate_space{};
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
    // Best-effort, like destroy_space above. The layer does not act on events
    // and never consumes one; it records session state transitions so a
    // capture can say whether the runtime stopped asking for frames, and why.
    PFN_xrPollEvent poll_event{};
    bool steamvr_runtime{};
    bool virtual_desktop_runtime{};
    bool inline_unless_pipelined{};
    XrVersion runtime_version{};
    std::string runtime_name;
    struct PanelGesture {
        PFN_xrSuggestInteractionProfileBindings suggest_bindings{};
        PFN_xrAttachSessionActionSets attach_action_sets{};
        PFN_xrSyncActions sync_actions{};
        PFN_xrCreateActionSpace create_action_space{};
        XrActionSet action_set{XR_NULL_HANDLE};
        XrAction grip_action{XR_NULL_HANDLE};
        std::array<XrPath, 2> hands{XR_NULL_PATH, XR_NULL_PATH};
        std::array<XrPath, 2> grips{XR_NULL_PATH, XR_NULL_PATH};
    } panel_gesture;
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

[[nodiscard]] std::uint64_t dlss_guide_camera_hash(
    const xrfg::DlssMotionVectorFrame& guide) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](const void* data, std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    append(guide.camera_position.data(),
           guide.camera_position.size() * sizeof(guide.camera_position[0]));
    append(guide.camera_right.data(),
           guide.camera_right.size() * sizeof(guide.camera_right[0]));
    append(guide.camera_forward.data(),
           guide.camera_forward.size() * sizeof(guide.camera_forward[0]));
    append(&guide.camera_fov_vertical, sizeof(guide.camera_fov_vertical));
    append(&guide.camera_data_valid, sizeof(guide.camera_data_valid));
    append(guide.camera_view_to_clip.data(),
           guide.camera_view_to_clip.size() *
               sizeof(guide.camera_view_to_clip[0]));
    append(&guide.camera_matrices_valid,
           sizeof(guide.camera_matrices_valid));
    return hash;
}

[[nodiscard]] std::uint64_t projection_pose_hash(
    const XrCompositionLayerProjectionView& view) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t hash = offset;
    const float fields[] = {
        view.pose.position.x, view.pose.position.y, view.pose.position.z,
        view.pose.orientation.x, view.pose.orientation.y,
        view.pose.orientation.z, view.pose.orientation.w,
        view.fov.angleLeft, view.fov.angleRight,
        view.fov.angleUp, view.fov.angleDown,
    };
    for (const float value : fields) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (bits >> shift) & 0xffU;
            hash *= prime;
        }
    }
    return hash;
}

void log_luke_projection_poses(
    const XrFrameEndInfo* frame,
    std::uint64_t serial,
    std::int64_t stage) noexcept {
    if (frame == nullptr || frame->layers == nullptr || serial == 0) return;
    if (stage==1 && xrfg::two_frame_capture_images_active()) {
        const auto rectangle=[&](unsigned layer,unsigned view,const XrSwapchainSubImage& image) {
            const auto handle=handle_value(image.swapchain);
            const auto code=static_cast<std::int64_t>((layer<<16)|view);
            xrfg::two_frame_capture_timeline("layer_image",code,handle,frame->displayTime,image.imageArrayIndex);
            xrfg::two_frame_capture_timeline("layer_rect_origin",code,handle,frame->displayTime,
                (std::uint64_t(std::uint32_t(image.imageRect.offset.x))<<32)|std::uint32_t(image.imageRect.offset.y));
            xrfg::two_frame_capture_timeline("layer_rect_extent",code,handle,frame->displayTime,
                (std::uint64_t(std::uint32_t(image.imageRect.extent.width))<<32)|std::uint32_t(image.imageRect.extent.height));
        };
        for(unsigned i=0;i<frame->layerCount && i<8;++i) {
            const auto* header=frame->layers[i];if(!header)continue;
            xrfg::two_frame_capture_timeline("layer_type",i,serial,frame->displayTime,header->type);
            xrfg::two_frame_capture_timeline("layer_flags",i,serial,frame->displayTime,header->layerFlags);
            if(header->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(header);
                for(unsigned v=0;p->views && v<p->viewCount && v<8;++v)rectangle(i,v,p->views[v].subImage);
            } else if(header->type==XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(header);rectangle(i,0,q->subImage);
                const auto bits=[](float f){std::uint32_t x{};std::memcpy(&x,&f,sizeof(x));return x;};
                xrfg::two_frame_capture_timeline("quad_size",i,handle_value(q->subImage.swapchain),frame->displayTime,
                    (std::uint64_t(bits(q->size.width))<<32)|bits(q->size.height));
                xrfg::two_frame_capture_timeline("quad_visibility",i,handle_value(q->subImage.swapchain),frame->displayTime,q->eyeVisibility);
            }
        }
    }
    for (std::uint32_t layer_index = 0;
         layer_index < frame->layerCount && layer_index < 8;
         ++layer_index) {
        const auto* header = frame->layers[layer_index];
        if (header == nullptr ||
            header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection =
            reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (projection->views == nullptr) continue;
        for (std::uint32_t view_index = 0;
             view_index < projection->viewCount && view_index < 8;
             ++view_index) {
            // result = stage*100 + layer*10 + view. Stages: 1=application,
            // 2=synthetic, 3=real. The hash covers pose and FOV only.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_pose_contract,
                stage * 100 + layer_index * 10 + view_index,
                serial,
                static_cast<std::uint64_t>(frame->displayTime),
                projection_pose_hash(projection->views[view_index]));
            xrfg::two_frame_capture_timeline(
                "pose", stage * 100 + layer_index * 10 + view_index,
                serial, static_cast<std::uint64_t>(frame->displayTime),
                projection_pose_hash(projection->views[view_index]));
        }
    }
}

void log_luke_view_bits(std::uint64_t serial, std::int64_t code,
                        const XrCompositionLayerProjectionView& view) noexcept {
    const auto pair = [](float x, float y) noexcept {
        std::uint32_t a{}, b{};
        std::memcpy(&a, &x, sizeof(a));
        std::memcpy(&b, &y, sizeof(b));
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    const float values[] = {
        view.pose.position.x, view.pose.position.y, view.pose.position.z,
        view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
        view.pose.orientation.w, view.fov.angleLeft, view.fov.angleRight,
        view.fov.angleUp, view.fov.angleDown, 0.0f};
    for (int index = 0; index < 3; ++index) {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_inline_pose_bits,
            code * 10 + index, serial,
            pair(values[index * 4], values[index * 4 + 1]),
            pair(values[index * 4 + 2], values[index * 4 + 3]));
    }
}

struct GeneratedFrameEndInfo;
void log_luke_guide_basis(const xrfg::DlssMotionVectorFrame& guide) noexcept {
    if (!xrfg::bridge_flight_logger().ram_enabled()) return;
    const auto pair = [](float x, float y) noexcept {
        std::uint32_t a{}, b{};
        std::memcpy(&a, &x, sizeof(a)); std::memcpy(&b, &y, sizeof(b));
        return (static_cast<std::uint64_t>(a) << 32) | b;
    };
    const std::array<const std::array<float, 3>*, 4> rows{
        &guide.camera_position, &guide.camera_right, &guide.camera_up, &guide.camera_forward};
    for (std::size_t row = 0; row < rows.size(); ++row)
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_render_camera,
            200 + row, guide.publication, pair((*rows[row])[0], (*rows[row])[1]),
            pair((*rows[row])[2], guide.camera_data_valid ? 1.0f : 0.0f));
}
struct ThreeSlotFrame;
struct SynthesisJob;

struct PresenterSubmission {
    xrfg::InterfaceFrameStamp interface_stamp{};
    std::uint64_t sequence{};
    std::uint64_t pair_sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> owned_frame;
    const XrFrameEndInfo* borrowed_frame{};
    std::uint64_t app_release_value{};
    XrResult result{XR_SUCCESS};
    bool completed{};
};

struct ProjectionLayerSnapshot {
    std::uint32_t layer_index{};
    XrCompositionLayerFlags layer_flags{};
    XrSpace space{XR_NULL_HANDLE};
    std::vector<XrCompositionLayerProjectionView> views;
};

// Immutable promise metadata follows the real frame through worker/queue ownership.
struct BlackBarPoseTime {
    XrTime time{};
    XrDuration applied_shift{};
    std::uint64_t epoch{};
    bool paired{};
};
struct BlackBarPromise {
    std::int32_t correction{};
    std::array<std::int8_t,64> samples{};
    unsigned count{},settle{};
    int candidate{-1};
    bool slowed{},running{};
    unsigned unpaired{};
};
struct BlackBarTiming {
    struct Locate { XrTime time{}; XrSpace space{}; XrPosef pose{}; XrDuration shift{}; };
    struct Wait { XrTime time{}; XrDuration shift{}; };
    std::mutex mutex;
    std::atomic<bool> enabled{};
    std::uint64_t epoch{1},configuration{};
    BlackBarPromise presenter,inline_promise;
    std::array<Locate,64> located{};
    std::array<Wait,64> waits{};
    unsigned located_next{},wait_next{};
    XrDuration last_shift{};
};

struct ProjectionCaptureOrder {
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::weak_ptr<xrfg::D3D12SwapchainHistory> history;
    std::uint64_t serial{}, fence_value{};
};

struct ProjectionSnapshot {
    BlackBarPoseTime blackbar_pose{};
    xrfg::InterfaceFrameStamp interface_stamp{};
    XrTime display_time{};
    XrEnvironmentBlendMode environment_blend_mode{};
    std::vector<ProjectionLayerSnapshot> layers;
    std::optional<xrfg::LukeRenderIdentity> luke_identity;
    std::uint64_t app_release_value{};
    std::vector<ProjectionCaptureOrder> capture_order;
};

// Immutable metadata belongs to the two released AFW images, not the next
// application's projection. No borrowed extension pointers are retained.
struct HeldAfwResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain output_swapchain{XR_NULL_HANDLE};
    std::uint32_t logical_eye{};
};

struct HeldAfwPair {
    ProjectionSnapshot projection;
    std::array<HeldAfwResource, 2> resources;
    std::uint64_t revision{};
    std::uint64_t production_serial{};
    std::array<xrfg::LukeNativeOriginMetadata,2> native_domains;
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

enum class FsrStereoGuideReason : std::int64_t {
    ready, non_stereo_projection, missing_guides, shared_eye_domain, recovering,
    invalid_guide_region, missing_depth,
};

struct FsrStereoAdmission {
    bool suspended{};
    unsigned recovery_frames{};
    FsrStereoGuideReason reason{FsrStereoGuideReason::ready};

    bool observe(FsrStereoGuideReason input) noexcept {
        if (input != FsrStereoGuideReason::ready) {
            suspended = true;
            recovery_frames = 0;
            reason = input;
            return false;
        }
        if (suspended && ++recovery_frames < 2) {
            reason = FsrStereoGuideReason::recovering;
            return false;
        }
        suspended = false;
        recovery_frames = 0;
        reason = FsrStereoGuideReason::ready;
        return true;
    }
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
    std::uint64_t serial{};
    std::shared_ptr<xrfg::InterfaceCycle> interface_cycle;
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

struct CompositionImageSlot;

struct SessionState {
    bool uevr_application{}; // captured once at session creation, never inferred from game name
    bool steamvr_separate_queue{true}; // immutable session preference, tray-only
    bool afw_only{}; // immutable session transport; does not disable Neural Rendering
    bool vdxr_legacy{true}; // immutable native/Legacy transport (VDXR and Pimax Auto)
    // D25 comparison policy: SteamVR's runtime gets its own binding queue.
    // Shared non-pacing support for both advanced NVIDIA routes.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> binding_queue;
    // Private-output preparation still uses binding_queue (or the app queue).
    // Only the common consumer and real runtime may use this final queue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> private_delivery_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> app_release_fence;
    Microsoft::WRL::ComPtr<ID3D12Fence> reacquire_fence;
    std::mutex binding_join_mutex;
    std::uint64_t app_release_counter{};
    std::uint64_t reacquire_counter{};
    std::uint64_t app_end_frame_release_value{};

    struct VulkanBridge { xrfg::VulkanSessionBinding binding{}; };
    std::unique_ptr<VulkanBridge> vulkan_bridge;
    xrfg::VulkanSessionBinding vulkan_binding{};

    std::mutex logged_view_rects_mutex;
    std::unordered_map<XrSwapchain, std::array<std::optional<std::array<std::uint64_t, 3>>, 4>> logged_view_rects;
    std::unique_ptr<xrfg::SteamVrDelivery> steamvr_delivery;
    std::chrono::steady_clock::time_point compositor_diagnostic_next_poll{};
    std::atomic<std::int64_t> private_clock_next_poll_ns{};
    std::uint64_t benchmark_command_revision{};
    std::uint32_t application_frames_submitted{};
    // The D3D11 bridge. The application bound D3D11; the runtime was handed
    // a D3D12 session on the layer's own device, so from here on the session
    // is a D3D12 one (graphics_binding, d3d12_device, d3d12_queue) and
    // d3d11_device stays null: no interop, no runtime-entry gate, no D3D11
    // work by the runtime on any thread. The application's device lives
    // here, for the per-swapchain shared textures it renders into
    // (D3D11BridgeSwapchain). Why: the NVIDIA D3D11 driver faults when the
    // runtime works the game's device from the presenter thread, and the
    // inline alternative is throttled by SteamVR; a runtime that only ever
    // sees D3D12 has neither problem. Capability bit 128.
    struct D3D11Bridge {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    };
    std::unique_ptr<D3D11Bridge> d3d11_bridge;
    std::atomic<bool> has_private_depth_swapchain{false};
    explicit SessionState(std::shared_ptr<Dispatch> next_dispatch)
        : dispatch(std::move(next_dispatch)), manual_control(current_layer_directory()),
          fsr_cadence(current_layer_directory()) {}

    ~SessionState() {
        if(private_runtime)private_runtime->stop();
        xrfg::unregister_d3d11_guide_owner(reinterpret_cast<std::uintptr_t>(this));
        xrfg::embedded::detach(control_id);
        if (presenter_pace_timer != nullptr) {
            CloseHandle(presenter_pace_timer);
        }
        if (optical_reserve_timer) CloseHandle(optical_reserve_timer);
    }

    SessionState(const SessionState&) = delete;
    SessionState& operator=(const SessionState&) = delete;

    std::shared_ptr<Dispatch> dispatch;
    std::shared_ptr<xrfg::PrivateOpenXrRuntime> private_runtime;
    std::shared_ptr<xrfg::RuntimeAcceptanceRate> runtime_acceptance;
    xrfg::implicit_layer::ManualArmControl manual_control;
    xrfg::FsrCadenceProbe fsr_cadence;
    FsrStereoAdmission fsr_stereo_admission; // frame_call_mutex; never persisted
    std::uint64_t control_id{xrfg::embedded::attach()};
    std::uint64_t control_revision{}; // frame_call_mutex
    bool control_reconfigure_required{};
    std::optional<bool> generation_outputs_needed; // initialized with control ownership
    std::atomic<bool> menu_enabled{true};
    std::atomic<bool> generation_steady_state_established{false};
    bool manual_stop_applied{}; // frame_call_mutex; terminal for this XrSession.
    std::atomic<std::int64_t> interface_begun_ns{};
    std::atomic<std::uint64_t> interface_serial{};
    std::array<XrSpace,2> panel_grip_spaces{};
    std::atomic<bool> panel_actions_attached{false};
    std::unique_ptr<xrfg::OpenXrFpsOverlay> fps_overlay;
    std::unique_ptr<xrfg::FrameCapture> frame_capture;
    std::uint64_t diagnostic_app_frame{};
    bool diagnostic_key_down{};
    bool diagnostic_ring_toggle_down{}, diagnostic_ring_save_down{};
    // Serializes each generated synthetic/real frame pair atomically with
    // respect to application frame calls. A successful application wait owns
    // the next admission until its matching begin has been attempted, so a
    // pipelined second wait cannot overtake that begin and deadlock the runtime.
    std::mutex frame_call_mutex;
    std::condition_variable frame_call_condition;
    // Serializes virtual waits and their matching BeginFrame admissions.
    // No application refresh timer is attached to this mutex.
    std::mutex adaptive_wait_mutex;
    // frame_call_mutex: scoped to the recognized Luke admission route. The
    // clock runs on elapsed wall time, not on how often Luke submits repeats.
    bool luke_clock_active{};
    bool luke_fresh_only{}; // pinned HFW, unchanged through AFW/OFXR switches
    bool luke_source_filter_armed{};
    std::optional<xrfg::LukeRenderIdentity> luke_startup_source;
    bool luke_output_begun{}; // physical output owner, frame_call_mutex
    XrFrameState luke_output_slot{XR_TYPE_FRAME_STATE};
    XrTime luke_prediction_origin{};
    std::chrono::steady_clock::time_point luke_prediction_wall{};
    std::optional<xrfg::LukeRenderIdentity> luke_last_source;
    bool luke_duplicate_cycle{};
    XrTime luke_clock_origin{};
    std::chrono::steady_clock::time_point luke_clock_wall_origin{};
    std::chrono::steady_clock::time_point application_wait_returned_at{};
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
    bool presenter_start_requested{};
    bool runtime_frame_waited_unbegun{}; // frame_call_mutex
    bool application_begin_needs_wait{}; // frame_call_mutex
    std::optional<XrFrameState> presenter_adopted_frame; // presenter_mutex
    bool single_threaded_d3d11{}; // D08: runtime calls cannot move to another thread.
    bool split_frame_loop{}; // D09: targeted direct-D3D11 driver workaround.
    // A proven auxiliary XR allocation ceiling selects synchronous Luke/D11
    // delivery for this SteamVR session. Never mutate either transport flag.
    std::atomic<bool> luke_aux_budget_inline{}; // reset by session lifetime
    xrfg::autonomous::SwapchainBudgetStatus swapchain_budget; // frame_call_mutex
    std::uint64_t next_swapchain_inventory_ms{};
    std::uint64_t next_snapshot_retirement_ms{};
    // Virtual auxiliary handles exist only in retained metadata. The final
    // physical EndFrame resolves them to one XR publication handle per source.
    std::mutex auxiliary_registry_mutex;
    std::unordered_map<XrSwapchain, std::weak_ptr<CompositionImageSlot>> auxiliary_registry;
    std::mutex auxiliary_publication_mutex;
    DWORD application_wait_thread_id{};
    DWORD application_end_thread_id{};
    std::atomic<DWORD> application_end_thread{};
    bool presenter_restore_after_bypass{}; // frame_call_mutex, Djules/VD only
    std::atomic<bool> djules_history_wait_enabled{};
    std::atomic<XrDuration> history_capture_wait_period{};
    XrFrameState last_inline_frame_state{XR_TYPE_FRAME_STATE};
    bool last_inline_frame_state_valid{};
    XrTime blackbar_inline_slot{};
    XrDuration blackbar_inline_shift{};
    BlackBarTiming blackbar_timing;
    std::mutex mutex;
    std::mutex gpu_mutex;
    std::deque<PendingApplicationFrame> pending_frames;
    // Timing handoffs have their own tiny atomic ownership, not the GPU worker's
    // session mutex. Profiling a swapchain hook must not introduce a worker wait.
    std::atomic<std::shared_ptr<xrfg::InterfaceCycle>> interface_waited_cycle;
    std::atomic<std::shared_ptr<xrfg::InterfaceCycle>> interface_active_cycle;
    std::uint64_t next_application_frame_serial{1}; // mutex
    std::optional<ProjectionSnapshot> previous_projection;
    XrTime generation_resume_display_time{};
    std::chrono::steady_clock::time_point generation_resume_wall_time{};
    XrDuration minimum_runtime_display_period{};
    // How long the runtime's own xrWaitFrame blocked, and how many consecutive
    // waits came back too quickly to have been pacing anything. This is what
    // the presenter's existence actually turns on; see
    // runtime_wait_lacks_pacing.
    std::chrono::steady_clock::duration last_application_wait_elapsed{};
    std::uint32_t unpaced_wait_streak{};
    // Consecutive pairs whose two frames left the layer close enough together
    // to land in one scanout window - see inline_pair_lands_in_one_scanout.
    std::uint32_t bunched_pair_streak{};
    std::uint32_t steamvr_throttled_wait_streak{};
    // Consecutive application frames the layer could not generate from. A
    // single one says nothing - the frame passes through and the next one
    // usually pairs - so the presenter is only demoted once they run together.
    std::uint32_t generation_failure_streak{};
    xrfg::D3D12OpticalFlowBackend optical_flow_backend{
        xrfg::D3D12OpticalFlowBackend::fidelity_fx};
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options{};
    // D35: bounded opt-in allocation/adapter records; never a pacing input.
    std::mutex video_memory_mutex;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> video_memory_adapter;
    bool video_memory_unavailable{};
    std::chrono::steady_clock::time_point video_memory_poll_at{};
    std::chrono::steady_clock::time_point video_memory_first_poll{};

    bool nvidia_new_route{};
    bool djules_route{};
    std::atomic_bool djules_deep_pipeline{};
    int separate_synthesis{};
    int lower_vram{};
    std::atomic_bool single_director_requested{};
    std::optional<xrfg::embedded::Settings> applied_generation_settings;
    std::atomic<xrfg::autonomous::GenerationState> generation_status{xrfg::autonomous::GenerationState::waiting};
    std::atomic_uint32_t interface_provider_error{};
    std::atomic<HRESULT> generation_initialization_result{S_OK};
    unsigned djules_phase_cooldown{}; // Presenter thread only; reset on policy epoch.
    std::atomic_bool djules_triple_requested{};
    std::atomic_uint64_t djules_configuration_epoch{};
    xrfg::DjulesTripleRelease djules_triple_release;
    bool djules_pipeline_switchable{};
    std::chrono::steady_clock::time_point djules_pipeline_poll{};
    std::optional<std::pair<bool, bool>> djules_pipeline_candidate;

    bool dlss_motion_vectors{};
    std::array<std::atomic<std::uint64_t>, 2> eye_release_publication{};
    xrfg::D3D12FrameGenerationMode frame_generation_mode{};
    bool hybrid_requested{};
    std::uint32_t native_dlss_generated_frames{};
    // Luke AFW capability is independent of the currently selected AFW mode.
    // The latter may be Off while ordinary OFXR generation remains enabled.
    bool luke_afw_enabled{};
    bool luke_shared_swapchains{}; // latched before native session creation; never live-reconfigured
    // Outlaws' initial VR texture constructor excludes the native producer.
    // Spend cold setup there, on its original SINGLETHREADED D11 owner, and
    // permanently close that window before the first application Wait/Begin.
    bool luke_outlaws_preframe{};
    bool luke_outlaws_frame_started{}; // frame_call_mutex
    bool luke_outlaws_eager_budget{}; // frame_call_mutex
    // Physical separation from ONE runtime pair, not cross-time AER poses.
    std::optional<float> luke_xr_eye_baseline; // mutex
    std::optional<bool> luke_afw_previous_selected; // frame_call_mutex
    std::optional<bool> luke_ofxr_previous_afw_input; // frame_call_mutex; actual color source
    xrfg::LukeAfwStartupRendezvous luke_afw_startup; // frame_call_mutex
    std::optional<xrfg::LukeAfwStartupKey> luke_afw_startup_drained; // frame_call_mutex
    std::optional<xrfg::LukeAfwStartupKey> luke_afw_last_delivered_startup_key; // frame_call_mutex
    bool luke_afw_odd_serial_is_left{true};
    bool luke_afw_f6_down{}; // frame_call_mutex; high-bit edge detector
    bool luke_f11_down{}; // frame_call_mutex; lightweight timing marker
    std::shared_ptr<xrfg::LukeAfwSynthesizer> luke_afw;
    // FSR keeps one immutable two-eye guide selection across D3D11 eye
    // releases. AFW records the real released eye here and resolves its
    // guide at xrEndFrame using the original color's decoded production token.
    std::mutex luke_guide_mutex;
    std::optional<std::uint32_t> luke_afw_released_eye;
    std::array<std::uint64_t, 2> luke_afw_last_guide_publication{};
    // PureDark has one temporal state across alternating real-eye calls.
    // Per-eye monotonicity alone permits a newer right-eye evaluation to be
    // followed by an older left-eye one when XR releases arrive out of order.
    std::uint64_t luke_afw_last_evaluated_publication{};
    xrfg::LukeOriginLossCapture luke_origin_loss_capture; // diagnostic only; luke_guide_mutex
    xrfg::LukeOriginContinuityBank luke_origin_continuity; // frame_call_mutex; all Luke ingress
    std::uint64_t luke_warmup_pair{},luke_warmup_started{};
    bool luke_warmup_held{},luke_warmup_probe_poisoned{};
    bool luke_origin_resync_pending{}; // frame_call_mutex
    // Last fully published AFW stereo pair. OpenXR permits re-submission of
    // the last released swapchain image on an XR frame with no game render.
    std::shared_ptr<const HeldAfwPair> luke_afw_last_pair;
    bool luke_frame_guide_resolved{};
    std::uint64_t luke_last_consumed_guide_publication{};
    std::shared_ptr<const xrfg::DlssMotionVectorSet>
        luke_frame_motion_vectors;
    xrfg::D3D12PresentationTransform presentation_transform{};
    SessionGraphicsBinding graphics_binding{SessionGraphicsBinding::none};
    std::uint64_t graphics_binding_capabilities{};
    VkDevice luke_vulkan_device{};
    ID3D12CommandQueue* luke_context_failed_queue{}; // original registry retains ownership; bounded failed initialization retry
    std::optional<bool> luke_afw_resources_needed;
    // Set once the runtime has refused a private swapchain. A runtime caps how
    // many swapchains one session may hold at all -- SteamVR hands out 16 --
    // and the application is usually still creating its own when the layer
    // reaches that ceiling, so the private swapchains already taken are what
    // makes the application's next creation fail. Hand the whole budget back
    // and pass frames through for the rest of the session: an application that
    // cannot finish creating its swapchains has no way to recover, while one
    // that merely loses generation carries on.
    std::atomic<bool> generation_budget_exhausted{false};
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_context;
    // Serialises this layer's entries into the runtime for as long as the
    // application's binding is D3D11. See RuntimeEntry below for why the gate
    // is here rather than on the device. Recursive because the frame path
    // already nests these calls inside one another on one thread.
    std::recursive_mutex runtime_entry_mutex;
    std::uint64_t runtime_entry_count{};       // runtime_entry_mutex
    std::uint64_t runtime_entry_contended{};   // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_ns{};     // runtime_entry_mutex
    std::uint64_t runtime_entry_wait_max_ns{}; // runtime_entry_mutex
    std::uint64_t runtime_entry_hold_max_ns{}; // runtime_entry_mutex
    Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device;
    std::vector<std::shared_ptr<void>> gpu_crash_monitors;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_queue;
    // Synthesis runs here rather than on the application's queue. The work
    // itself is short - the pack and composite command lists measure 249 us
    // per swapchain against MSFS 2024, and the 1610 us of optical flow runs
    // on the OFA engine rather than on any D3D12 queue - but the queue waits
    // on the OFA fence between them, and submitting that on the application's
    // queue blocks everything the application has already queued behind it,
    // which is its next frame's rendering. On a queue of its own the wait
    // holds only this. Null when the application binds D3D11, where
    // d3d12_queue is already the layer's own bridge queue.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> d3d12_synthesis_queue;
    // Luke capture/AFW stay on d3d12_queue. Only post-ingress OFXR uses this lane.
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> luke_synthesis_queue;
    std::atomic_uint32_t luke_shared_generation_count{};
    // Some SteamVR configurations throttle the inline second wait/begin/end
    // cycle to the application's half-rate interval. After that behavior is
    // measured, the dedicated presenter becomes the sole owner of downstream
    // calls while the application observes a virtual half-rate loop.
    std::mutex presenter_mutex;
    std::condition_variable presenter_condition;
    std::mutex presenter_content_mutex;
    std::deque<std::shared_ptr<PresenterSubmission>> presenter_submissions;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_last_frame;
    // A synthetic has reached xrEndFrame, but its paired current has not.
    // Protected by presenter_mutex so admission cannot discard that current
    // in the gap between the two presenter cycles.
    std::uint64_t presenter_pending_current_pair_sequence{};
    std::shared_ptr<PresenterSubmission> presenter_inflight_submission;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_inflight_frame;
    std::thread presenter_thread;
    // presenter_mutex: descriptors only, never image storage.
    xrfg::ThreeSlotBuffer<std::shared_ptr<ThreeSlotFrame>> three_slots;
    std::shared_ptr<SynthesisJob> synthesis_job;
    std::thread synthesis_thread;
    bool three_slot_director{};
    bool fsr_two_slot_presenter{}; // presenter_mutex; immutable while thread runs
    bool queued_nvidia_presenter{}; // Legacy synthesis, fresh S/B consumer; immutable while running.
    std::atomic<std::int64_t> optical_reserve_ns{};
    // presenter_mutex: a fractional preparation lead before the current frame
    // of one acknowledged synthetic pair. Not an extra XR frame or FIFO skip.
    std::chrono::steady_clock::time_point optical_reserve_current_due{};
    std::uint64_t optical_reserve_pair{}, optical_reserve_served{};
    HANDLE optical_reserve_timer{}; // application publication thread only
    bool synthesis_busy{};
    bool director_draining{};
    std::uint64_t next_source_sequence{1};
    std::uint64_t director_anchor_sequence{}; // presenter_mutex; history identity, not a retained output
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
    // The best phase this presenter has managed lately: predictedDisplayTime
    // minus the moment the frame aimed at it was actually submitted, reduced
    // modulo the display period. The two clocks have different epochs, so the
    // value is meaningless on its own and exact as a comparison - which is
    // all the phase needs.
    std::int64_t presenter_lead_reference{};
    bool presenter_lead_valid{};
    // When the previous submission actually went out, so the phase controller
    // can tell whether a lead was achieved on the grid or by overshooting it.
    std::chrono::steady_clock::time_point presenter_last_submitted_at{};
    // What the schedule expected the last interval to be. The pair is biased,
    // so "on grid" is not always one period - see the comment at the advance.
    std::chrono::nanoseconds presenter_expected_interval{};
    // Consecutive submissions that landed one scanout apart. Phase only means
    // anything once the rate is right, so the correction waits for a run of
    // them - see the comment at the controller.
    std::uint32_t presenter_on_grid_streak{};
    // The adaptive pair bias and the two smoothed costs it is derived from.
    // The synthetic's downstream xrEndFrame costs more than the real frame's
    // exactly when the runtime is blocking on pixels that are not finished,
    // so the difference is the block and the bias buys it out. The real
    // frame is the baseline rather than a constant because it absorbs
    // whatever the runtime charges per submission regardless - 0.65 to
    // 0.81 ms across every capture here, and different elsewhere.
    std::chrono::nanoseconds presenter_synthetic_call_mean{};
    std::chrono::nanoseconds presenter_real_call_mean{};
    std::chrono::nanoseconds presenter_pair_bias{};
    std::uint32_t presenter_bias_tick{};
    // The other constraint on the bias, and the one the controller was blind
    // to: the interval between the two hand-overs of a pair. Too small and
    // they share a scanout and the compositor keeps one - 11.90 ms measured
    // 0.0% sharing, 8.50 ms measured 76.4%.
    std::chrono::steady_clock::time_point presenter_synthetic_returned_at{};
    std::chrono::nanoseconds presenter_pair_gap_mean{};
    bool presenter_schedule_valid{};
    // The display time the runtime reported for the previous internal
    // xrWaitFrame, and how many slots the pace has been asked to give back.
    //
    // The schedule above is a steady_clock grid. The compositor scans out on
    // the display's clock, and nothing related the two: the phase between
    // them was whatever it happened to be when the presenter thread started,
    // and no path corrected it. A grid sitting just after the compositor's
    // deadline makes every submission miss the scanout it was built for and
    // land in the next, so each scanout sees either nothing new or two
    // frames - continuously, not occasionally, and latched for the life of
    // the session, which is why leaving and re-entering changed everything.
    //
    // predictedDisplayTime is the runtime naming the scanout each frame is
    // for, so consecutive waits advancing by exactly one period is the lock
    // condition, and the delta is the error. Guarded by presenter_mutex.
    XrTime presenter_last_predicted_display{};
    bool presenter_last_predicted_valid{};
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
    // The presenter's frame counter, and the count the application was last
    // released at. The application is handed a doubled period, so it has to be
    // released once per pair, and the validity flag above cannot do that: it is
    // a latch set on the presenter's first wait and cleared only on start and
    // stop, so waiting on it returned in microseconds on every frame after the
    // first. An application slow enough to be the limit never noticed, because
    // its own rendering paced it. One fast enough to keep up ran at a frame per
    // display period against a layer that can consume one per pair, and the
    // surplus frame lost the history ring's capture slot and was rendered and
    // thrown away -- one application frame in six on UEVR, at a steady beat,
    // never two in a row.
    std::uint64_t presenter_frame_serial{};
    std::uint64_t application_served_serial{};
    bool presenter_stop_requested{};
    bool presenter_active{};
    XrSession handle{XR_NULL_HANDLE};
};

// R20/R21 promise fixes, gated by the owner's existing live camera checkbox.
// OFF has no history work; each toggle/configuration epoch discards stale feedback.
bool blackbar_gate(SessionState& state) {
    auto& t=state.blackbar_timing;
    const bool enabled=xrfg::autonomous::r19_camera_enabled.load(std::memory_order_acquire);
    if (!enabled && !t.enabled.load(std::memory_order_acquire)) return false;
    std::scoped_lock lock(t.mutex);
    const auto configuration=state.djules_configuration_epoch.load();
    if (enabled!=t.enabled.load() || configuration!=t.configuration) {
        ++t.epoch;t.configuration=configuration;t.presenter={};t.inline_promise={};
        t.located={};t.waits={};t.located_next=t.wait_next=0;t.last_shift=0;
        t.enabled.store(enabled,std::memory_order_release);
    }
    return enabled;
}

XrDuration blackbar_promise_shift(SessionState& state,XrTime base,XrDuration period,bool presenter) {
    if (!blackbar_gate(state)) return 0;
    auto& t=state.blackbar_timing;std::scoped_lock lock(t.mutex);
    const auto& promise=presenter?t.presenter:t.inline_promise;
    const XrDuration shift=state.menu_enabled && !state.manual_control.stop_requested() &&
        promise.running && period>0 && (promise.correction==0 ||
        period<=std::numeric_limits<XrDuration>::max()/promise.correction) ? period*promise.correction : 0;
    t.last_shift=shift;
    if (base>0 && base<=std::numeric_limits<XrTime>::max()-shift) {
        t.waits[t.wait_next++%t.waits.size()]={base+shift,shift};
        t.wait_next%=static_cast<unsigned>(t.waits.size());
    }
    return shift;
}

void blackbar_record_locate(SessionState& state,const XrViewLocateInfo& info,const XrPosef& pose) {
    if (!blackbar_gate(state)) return;
    auto& t=state.blackbar_timing;std::scoped_lock lock(t.mutex);
    auto shift=t.last_shift;
    for (unsigned back=1;back<=t.waits.size();++back) {
        const auto& w=t.waits[(t.wait_next+t.waits.size()-back)%t.waits.size()];
        if(w.time==info.displayTime) {shift=w.shift;break;}
    }
    t.located[t.located_next++%t.located.size()]={info.displayTime,info.space,pose,shift};
    t.located_next%=static_cast<unsigned>(t.located.size());
}

BlackBarPoseTime blackbar_pose_time(SessionState& state,const XrFrameEndInfo& info) {
    if (!blackbar_gate(state)) return {};
    auto& t=state.blackbar_timing;std::scoped_lock lock(t.mutex);
    BlackBarPoseTime result{info.displayTime,t.last_shift,t.epoch};
    for (unsigned back=1;back<=t.waits.size();++back) {
        const auto& w=t.waits[(t.wait_next+t.waits.size()-back)%t.waits.size()];
        if(w.time==info.displayTime) {result.applied_shift=w.shift;break;}
    }
    for(std::uint32_t i=0;info.layers && i<info.layerCount;++i) {
        const auto* h=info.layers[i];
        if(!h || h->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto& p=*reinterpret_cast<const XrCompositionLayerProjection*>(h);
        if(!p.views || !p.viewCount) break;
        for(unsigned back=1;back<=t.located.size();++back) {
            const auto& e=t.located[(t.located_next+t.located.size()-back)%t.located.size()];
            if(e.time>0 && e.space==p.space && std::memcmp(&e.pose,&p.views[0].pose,sizeof(XrPosef))==0)
                return {e.time,e.shift,t.epoch};
        }
        break;
    }
    return result;
}

// Upstream R20/R21 two-window median controller. Input is the uncorrected
// lateness using the shift stored with THIS pose, rather than a newer wait.
void blackbar_observe(SessionState& state,const BlackBarPoseTime& pose,XrTime shown,
    XrDuration period,XrDuration runtime_period,bool presenter) {
    if(!blackbar_gate(state) || period<=0 || pose.time<=0 || shown<=0) return;
    auto& t=state.blackbar_timing;int previous=-1,target=-1;unsigned agreeing=0;
    {
        std::scoped_lock lock(t.mutex);
        if(pose.epoch!=t.epoch) return;
        auto& p=presenter?t.presenter:t.inline_promise;
        if(!pose.paired) {if(++p.unpaired>=3)p.running=false;return;}
        p.running=true;p.unpaired=0;
        const bool slowed=runtime_period>period+period/2;
        if(slowed!=p.slowed) {p.slowed=slowed;p.count=0;p.settle=8;p.candidate=-1;}
        if(slowed) return;
        if(p.settle) {--p.settle;return;}
        const double periods=(static_cast<double>(shown)-static_cast<double>(pose.time)+
            static_cast<double>(pose.applied_shift))/static_cast<double>(period);
        const int rounded=static_cast<int>(std::clamp(periods<0?periods-.5:periods+.5,-8.,8.));
        p.samples[p.count++]=static_cast<std::int8_t>(rounded);
        if(p.count<p.samples.size())return;
        p.count=0;std::array<unsigned,17> counts{};
        for(auto sample:p.samples)++counts[sample+8];
        const auto cost=[&](int value) {unsigned total=0;for(int i=-8;i<=8;++i)total+=counts[i+8]*std::abs(i-value);return total;};
        auto best=cost(p.correction);target=p.correction;
        for(int i=0;i<=6;++i)if(cost(i)<best){target=i;best=cost(i);}
        if(target==p.correction || (cost(p.correction)-best)*4<p.samples.size()){p.candidate=-1;return;}
        if(p.candidate!=target){p.candidate=target;return;}
        previous=p.correction;p.correction=target;p.candidate=-1;p.settle=8;agreeing=counts[target+8];
    }
    // Never log while holding the promise or presenter mutex.
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
        presenter?821:820,target,previous,agreeing);
}

// A forwarded Begin opened this physical slot. Prediction was advanced for
// the game's real pose; runtime handover, including bypass, still uses its slot.
const XrFrameEndInfo* blackbar_inline_submission(SessionState& state,
    const XrFrameEndInfo* source,XrFrameEndInfo& storage) {
    if(source && blackbar_gate(state) && state.blackbar_inline_shift>0 && state.blackbar_inline_slot>0) {
        storage=*source;storage.displayTime=state.blackbar_inline_slot;return &storage;
    }
    return source;
}
void blackbar_unpaired(SessionState& state,bool presenter,bool cooldown=false) {
    if(!blackbar_gate(state))return;
    auto& t=state.blackbar_timing;std::scoped_lock lock(t.mutex);
    auto& p=presenter?t.presenter:t.inline_promise;
    if(cooldown || ++p.unpaired>=3)p.running=false;
}

// Inside xrEndFrame, and inside each swapchain image call, the runtime drives
// the application's single D3D11 immediate context. ID3D11Multithread makes
// each of the runtime's own D3D11 calls atomic and no more, so the sequence
// one of those OpenXR calls issues interleaves with the sequence another
// issues on the other thread, and the driver's dependency tracking walks a
// chain that has moved. That is what kills nvwgf2umx when a presenter thread
// runs beside the application thread on a D3D11 session.
//
// Holding the layer's own device section could not cover it and recorded no
// contention at all: that guards the layer's D3D11 work, while the work that
// collides is the runtime's, inside the runtime's own calls. The gate has to
// wrap our entry into the runtime, because that is the only place from which
// the runtime's sequence is reachable.
//
// One OpenXR call's worth of D3D11 work is the unit that has to be atomic, so
// this wraps single calls and never a longer span. xrWaitFrame stays outside
// it deliberately: it blocks for most of a display period and issues no D3D11
// work, and holding it here would hand the application thread exactly the
// stall the presenter exists to remove.
thread_local int runtime_entry_depth = 0;

struct RuntimeEntry {
    // The private endpoint owns its handle/image locks and native consumer.
    // Its EndFrame can wait for bounded FIFO credit without doing runtime D11
    // work. Holding the old native-entry gate there stalls unrelated producer
    // releases until a physical display slot retires.
    explicit RuntimeEntry(SessionState* session) noexcept
        : session_(session != nullptr && session->d3d11_device != nullptr &&
                       !session->private_runtime
                       ? session
                       : nullptr) {
        if (session_ == nullptr) {
            return;
        }
        outermost_ = runtime_entry_depth == 0;
        const auto before = std::chrono::steady_clock::now();
        session_->runtime_entry_mutex.lock();
        entered_ = std::chrono::steady_clock::now();
        ++runtime_entry_depth;
        if (!outermost_) {
            return;
        }
        const auto waited = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                entered_ - before)
                .count());
        ++session_->runtime_entry_count;
        session_->runtime_entry_wait_ns += waited;
        if (waited > session_->runtime_entry_wait_max_ns) {
            session_->runtime_entry_wait_max_ns = waited;
        }
        // A lock handed over uncontended still costs a few hundred
        // nanoseconds, so only a wait long enough to be a real hand-off
        // counts as one thread having found the other inside the runtime.
        if (waited >= 20'000) {
            ++session_->runtime_entry_contended;
        }
    }

    ~RuntimeEntry() {
        if (session_ == nullptr) {
            return;
        }
        --runtime_entry_depth;
        if (outermost_) {
            const auto held = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - entered_)
                    .count());
            if (held > session_->runtime_entry_hold_max_ns) {
                session_->runtime_entry_hold_max_ns = held;
            }
            // Reported in windows rather than per entry: one record per frame
            // would bury the log, and the question this answers - whether the
            // two threads ever meet here - is a rate, not an event.
            // Eight application frames' worth, so a short capture still
            // reports and a long one costs a handful of records a second.
            if (session_->runtime_entry_count >= 64) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::runtime_entry_section,
                    static_cast<std::int64_t>(session_->runtime_entry_contended),
                    session_->runtime_entry_wait_ns /
                        session_->runtime_entry_count,
                    session_->runtime_entry_wait_max_ns,
                    session_->runtime_entry_hold_max_ns);
                session_->runtime_entry_count = 0;
                session_->runtime_entry_contended = 0;
                session_->runtime_entry_wait_ns = 0;
                session_->runtime_entry_wait_max_ns = 0;
                session_->runtime_entry_hold_max_ns = 0;
            }
        }
        session_->runtime_entry_mutex.unlock();
    }

    RuntimeEntry(const RuntimeEntry&) = delete;
    RuntimeEntry& operator=(const RuntimeEntry&) = delete;

private:
    SessionState* session_{};
    bool outermost_{};
    std::chrono::steady_clock::time_point entered_{};
};

[[nodiscard]] ID3D12CommandQueue* runtime_queue(const SessionState& state) noexcept {
    return state.binding_queue ? state.binding_queue.Get()
                               : state.d3d12_queue.Get();
}

// The application has released one of its images: mark where its queue
// stands, for the join before a downstream xrEndFrame that names it. See
// SessionState::binding_queue. Nothing waits here.
void mark_application_release(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->app_release_counter + 1;
        if (SUCCEEDED(state->d3d12_queue->Signal(
                state->app_release_fence.Get(), value))) {
            state->app_release_counter = value;
        }
    } catch (...) {
    }
}

// The runtime has made one of the application's images safe to write, on the
// binding queue. Carry that to the queue the application will write it on.
void carry_reacquire_to_application(SessionState* state) noexcept {
    // The private endpoint returns only images with no readers. Their GPU copy
    // is already retired, so joining the app to the REAL binding queue would
    // reintroduce the compositor dependency this endpoint removes.
    if(state && state->private_runtime)return;
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence ||
        !state->d3d12_queue) {
        return;
    }
    try {
        std::scoped_lock lock(state->binding_join_mutex);
        const std::uint64_t value = state->reacquire_counter + 1;
        if (SUCCEEDED(state->binding_queue->Signal(
                state->reacquire_fence.Get(), value))) {
            state->reacquire_counter = value;
            static_cast<void>(state->d3d12_queue->Wait(
                state->reacquire_fence.Get(), value));
        }
    } catch (...) {
    }
}

// Before a downstream xrEndFrame: the binding queue waits until the
// application's queue has run past the release of every image of the
// application's that the frame names. `value` is app_release_counter as it
// stood at the application's own xrEndFrame for that frame. A repeat waits
// the same value again, which costs nothing.
void join_runtime_queue_to_application(
    SessionState* state,
    std::uint64_t value) noexcept {
    if (state == nullptr || !state->binding_queue || !state->app_release_fence ||
        value == 0) {
        return;
    }
    static_cast<void>(
        state->binding_queue->Wait(state->app_release_fence.Get(), value));
}

// Lets the binding queue finish what it holds before the session it serves
// goes away. Bounded: a wait it holds for the application's queue could
// otherwise keep this here for as long as the application's queue is stuck.
void drain_binding_queue(SessionState* state) noexcept {
    if (state == nullptr || !state->binding_queue || !state->reacquire_fence) {
        return;
    }
    try {
        std::uint64_t value = 0;
        {
            std::scoped_lock lock(state->binding_join_mutex);
            value = state->reacquire_counter + 1;
            if (FAILED(state->binding_queue->Signal(
                    state->reacquire_fence.Get(), value))) {
                return;
            }
            state->reacquire_counter = value;
        }
        if (state->reacquire_fence->GetCompletedValue() >= value) {
            return;
        }
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr) {
            return;
        }
        if (SUCCEEDED(state->reacquire_fence->SetEventOnCompletion(value, event))) {
            WaitForSingleObject(event, 2000);
        }
        CloseHandle(event);
    } catch (...) {
    }
}


// Djules V430/V431: correct a whole-scanout disturbance once, then use the
// bounded quarter-step until the measured phase returns to the target band.
struct DjulesPhaseAcquisition {
    unsigned cooldown{};
    std::chrono::nanoseconds step(std::optional<std::chrono::nanoseconds> left,
        std::chrono::nanoseconds period) noexcept {
        if (!left || period <= std::chrono::nanoseconds::zero()) return {};
        const auto error = *left - std::chrono::nanoseconds(3'000'000);
        const bool far_reading = error > period / 2;
        if (!far_reading) cooldown = 0;
        if (far_reading && cooldown == 0) {
            cooldown = 8;
            return (std::min)(error, period * 2);
        }
        if (cooldown) --cooldown;
        constexpr auto dead_band = std::chrono::nanoseconds(250'000);
        if (error >= -dead_band && error <= dead_band) return {};
        return std::clamp(error / 4, -period / 2, period / 2);
    }
};

// Non-SteamVR/non-VD runtimes pace through xrWaitFrame. Impose only a
// half-period floor after the actual previous EndFrame return, not a second
// absolute clock. VD deliberately keeps the steady grid without a release offset.
[[nodiscard]] std::chrono::steady_clock::time_point djules_handover_floor(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point last_returned,
    std::chrono::nanoseconds period) noexcept {
    if (last_returned == std::chrono::steady_clock::time_point{} ||
        period <= std::chrono::nanoseconds::zero()) return now;
    return (std::max)(now, last_returned + period / 2);
}

// D26's absolute display grid: loop cost never compounds into the period.
// After a slow handover, skip whole slots to retain a quarter-period lead,
// rather than submitting a catch-up burst (final upstream V427 policy).
class DjulesAbsoluteSchedule {
public:
    using Clock = std::chrono::steady_clock;
    Clock::time_point claim(Clock::time_point now, std::chrono::nanoseconds period) noexcept {
        if (!valid_ || period != period_) next_ = now;
        period_ = period;
        valid_ = period > std::chrono::nanoseconds::zero();
        return next_;
    }
    void submitted(Clock::time_point slot, std::chrono::nanoseconds period,
        Clock::time_point returned, std::chrono::nanoseconds phase_step = {}) noexcept {
        valid_ = period > std::chrono::nanoseconds::zero();
        period_ = period;
        if (!valid_) { next_ = returned; return; }
        next_ = slot + period + phase_step;
        const auto earliest = returned + period / 4;
        if (next_ < earliest) {
            const auto behind = std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - next_).count();
            const auto slots = (behind + period.count() - 1) / period.count();
            next_ += period * slots;
        }
    }
private:
    Clock::time_point next_{};
    std::chrono::nanoseconds period_{};
    bool valid_{};
};

// V464 changes only ordinary native-D3D12 FSR guide association.
[[nodiscard]] bool strict_fsr_release_guides(const SessionState& state) noexcept {
    return state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        state.graphics_binding == SessionGraphicsBinding::d3d12 &&
        state.dlss_motion_vectors && !state.luke_afw_enabled;
}
[[nodiscard]] bool release_bound_projection_guides(const SessionState& state) noexcept {
    return strict_fsr_release_guides(state) || xrfg::use_release_bound_game_guides(
        state.frame_generation_mode, state.dlss_motion_vectors, state.luke_afw_enabled);
}

[[nodiscard]] bool comparison_delivery_runtime(const SessionState& state) noexcept {
    return !state.vdxr_legacy;
}

[[nodiscard]] bool advanced_native_outputs(const SessionState& state) noexcept {
    return state.nvidia_new_route && !state.luke_afw_enabled &&
        state.frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.graphics_binding == SessionGraphicsBinding::d3d12;
}

// Reference keeps its fixed-midpoint/linear-flow algorithm. Only ordinary
// native D12 SteamVR transport may overlap a retained pair with the next capture.
[[nodiscard]] bool steamvr_reference_capture_overlap(const SessionState& state) noexcept {
    return comparison_delivery_runtime(state) && !state.nvidia_new_route &&
        state.graphics_binding == SessionGraphicsBinding::d3d12 &&
        !state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled &&
        !state.luke_outlaws_preframe && !state.hybrid_requested && !state.nvidia_options.hybrid &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr;
}

[[nodiscard]] int effective_memory_control(const SessionState& state,int requested) noexcept {
    return state.luke_afw_enabled ? 0 : requested;
}
[[nodiscard]] bool configurable_native_outputs(const SessionState& state) noexcept {
    return state.graphics_binding == SessionGraphicsBinding::d3d12 && !state.luke_afw_enabled;
}

// Keep the user's OFXR selection untouched on disk. AFW-only is a startup
// transport choice, not another generation backend or a Neural Rendering switch.
[[nodiscard]] xrfg::embedded::Settings effective_session_settings(
    const SessionState& state, xrfg::embedded::Settings requested) noexcept {
    if(state.afw_only) {
        const int pacing=requested.pacing_mode;
        requested={};requested.enabled=false;requested.backend=1;
        requested.motion_vectors=1;requested.pacing_mode=pacing;
    }
    return requested;
}

[[nodiscard]] bool runtime_binding_queue_requested(const SessionState& state) noexcept {
    if(state.afw_only)return false;
    if (!comparison_delivery_runtime(state) || state.graphics_binding != SessionGraphicsBinding::d3d12) return false;
    if (!state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled)
        return state.steamvr_separate_queue;
    // Preserve established interop and Luke transport contracts.
    return state.nvidia_new_route || steamvr_reference_capture_overlap(state);
}

[[nodiscard]] HRESULT initialize_runtime_binding_queue(SessionState& state) noexcept {
    if (!runtime_binding_queue_requested(state)) return S_FALSE;
    if (!state.d3d12_device || !state.d3d12_queue) return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.NodeMask = state.d3d12_queue->GetDesc().NodeMask;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> release, reacquire;
    HRESULT result = state.d3d12_device->CreateCommandQueue(&description, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(result)) result = state.d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&release));
    if (SUCCEEDED(result)) result = state.d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&reacquire));
    if (FAILED(result)) return result;
    state.binding_queue = std::move(queue);
    state.app_release_fence = std::move(release);
    state.reacquire_fence = std::move(reacquire);
    state.graphics_binding_capabilities |= 16ULL;
    return S_OK;
}
// Only the existing private SteamVR endpoint may own the sole presentation clock.
// Requested state is atomic because a drained worker may already await its next job.
[[nodiscard]] bool single_director_pacing(const SessionState& state) noexcept {
    return state.menu_enabled.load(std::memory_order_relaxed) &&
        state.single_director_requested.load(std::memory_order_relaxed) &&
        state.private_runtime && comparison_delivery_runtime(state);
}

// HFW's unique-production filter is an ingress rule. It must not force the
// completed AFW packet back through inline OFXR publication when a private
// SteamVR director and an isolated synthesis lane are already available.
[[nodiscard]] bool fresh_luke_director_worker(const SessionState& state) noexcept {
    return state.luke_fresh_only && !state.afw_only && single_director_pacing(state) &&
        state.dispatch->steamvr_runtime && state.luke_synthesis_queue &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr;
}

[[nodiscard]] bool staged_native_outputs(const SessionState& state) noexcept {
    return !state.private_runtime && configurable_native_outputs(state) && state.separate_synthesis > 0;
}
[[nodiscard]] bool reduced_native_outputs(const SessionState& state) noexcept {
    return configurable_native_outputs(state) && state.lower_vram > 0;
}

// Enumeration admits source images, not interpolation outputs. All runtimes,
// bindings and providers spend output capacity only on submitted projections.
// Outlaws alone requires its existing pre-frame initialization rendezvous.
[[nodiscard]] bool projection_owned_outputs(const SessionState& state) noexcept {
    return !(state.luke_outlaws_preframe && !state.luke_outlaws_frame_started);
}
[[nodiscard]] bool direct_reduced_outputs(const SessionState& state) noexcept {
    return reduced_native_outputs(state) && !staged_native_outputs(state);
}

[[nodiscard]] bool djules_native_scheduler(const SessionState& state) noexcept {
    if (state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::native_dlss)
        return !state.luke_afw_enabled && state.graphics_binding == SessionGraphicsBinding::d3d12;
    return state.djules_route && state.nvidia_new_route && !state.luke_afw_enabled &&
        state.frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.graphics_binding == SessionGraphicsBinding::d3d12;
}

// FSR retains its own synthesis/copy/context contract. Only its native
// SteamVR two-ring presentation clock joins the measured Djules clock.
[[nodiscard]] bool fsr_steamvr_phase_clock(const SessionState& state) noexcept {
    return state.dispatch->steamvr_runtime &&
        state.graphics_binding==SessionGraphicsBinding::d3d12 &&
        state.frame_generation_mode==xrfg::D3D12FrameGenerationMode::fsr3 &&
        !state.luke_afw_enabled && !state.luke_outlaws_preframe &&
        !state.d3d11_bridge && !state.vulkan_bridge && !reduced_native_outputs(state);
}
[[nodiscard]] bool native_measured_phase_clock(const SessionState& state) noexcept {
    return djules_native_scheduler(state) || fsr_steamvr_phase_clock(state);
}

// Capacity follows the selected multiplier, even during a disabled/drained control transaction.
[[nodiscard]] bool extra_output_capacity(const SessionState& state) noexcept {
    return state.djules_triple_requested && djules_native_scheduler(state) &&
        (!direct_reduced_outputs(state) || (state.private_runtime));
}
[[nodiscard]] unsigned native_synthetic_rings(const SessionState& state) noexcept {
    // Fast 3X still needs two distinct S destinations, never alias S1/S2.
    const bool private_triple=state.private_runtime && extra_output_capacity(state);
    return reduced_native_outputs(state) && !private_triple ? 1U : 2U;
}
[[nodiscard]] unsigned native_current_rings(const SessionState& state) noexcept {
    // The framework may retain A while B is prepared. 3X must keep both even
    // with Fast selected; one B handle would alias an owned source descriptor.
    const bool private_triple=state.private_runtime && extra_output_capacity(state);
    return reduced_native_outputs(state) && !private_triple ? 1U : 2U;
}

[[nodiscard]] bool djules_triple_active(const SessionState& state) noexcept {
    if(direct_reduced_outputs(state) && !extra_output_capacity(state)) return false;
    if (state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::native_dlss)
        return state.djules_triple_requested && state.menu_enabled &&
            djules_native_scheduler(state) && state.native_dlss_generated_frames >= 2;
    return state.djules_triple_requested && state.menu_enabled && djules_native_scheduler(state);
}
[[nodiscard]] bool djules_triple_director(const SessionState& state) noexcept {
    return state.three_slot_director && djules_triple_active(state);
}
[[nodiscard]] bool djules_deep_active(const SessionState& state) noexcept {
    return !direct_reduced_outputs(state) && state.djules_deep_pipeline && djules_native_scheduler(state) && !djules_triple_director(state);
}

// D26 clock-source step only: a runtime prediction may already describe a
// throttled application cadence. SteamVR's actual HMD period owns the display
// cadence once available. Absolute phase/deadline policy follows separately.
[[nodiscard]] bool adopt_djules_display_period(SessionState& state,
    std::optional<std::chrono::nanoseconds> measured) noexcept {
    if (!native_measured_phase_clock(state) || !state.dispatch->steamvr_runtime ||
        !measured || measured->count() < 2'000'000 || measured->count() > 50'000'000) return false;
    std::scoped_lock lock(state.presenter_mutex);
    const auto prior = state.presenter_display_period;
    state.history_capture_wait_period.store(measured->count(), std::memory_order_relaxed);
    if (prior == measured->count()) return false;
    state.presenter_display_period = measured->count();
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        fsr_steamvr_phase_clock(state)?5570:26, static_cast<std::uint64_t>(prior), static_cast<std::uint64_t>(measured->count()), 1);
    return true;
}
void refresh_djules_display_period(const std::shared_ptr<SessionState>& state) noexcept {
    if (!native_measured_phase_clock(*state) || !state->dispatch->steamvr_runtime || !state->steamvr_delivery) return;
    static_cast<void>(adopt_djules_display_period(*state, state->steamvr_delivery->display_period()));
}

[[nodiscard]] bool adopt_reference_display_period(SessionState& state,
    std::optional<std::chrono::nanoseconds> measured) noexcept {
    // Reference's queued presenter submits one output per physical period.
    // SteamVR can report the throttled application's 22.22ms prediction; that
    // is not the 90Hz display clock. New's midpoint schedule remains unchanged.
    if (!state.dispatch->steamvr_runtime || state.graphics_binding != SessionGraphicsBinding::d3d12 ||
        state.nvidia_new_route || state.luke_afw_enabled || state.luke_outlaws_preframe ||
        state.hybrid_requested || state.nvidia_options.hybrid ||
        state.optical_flow_backend != xrfg::D3D12OpticalFlowBackend::nvidia ||
        state.frame_generation_mode != xrfg::D3D12FrameGenerationMode::ofxr ||
        !measured || measured->count() < 2'000'000 || measured->count() > 50'000'000) return false;
    std::scoped_lock lock(state.presenter_mutex);
    const auto prior = state.presenter_display_period;
    if (prior == measured->count()) return false;
    state.presenter_display_period = measured->count();
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        28, static_cast<std::uint64_t>(prior), static_cast<std::uint64_t>(measured->count()), 1);
    return true;
}
void refresh_reference_display_period(const std::shared_ptr<SessionState>& state) noexcept {
    if (!state->nvidia_new_route && state->dispatch->steamvr_runtime && state->steamvr_delivery)
        static_cast<void>(adopt_reference_display_period(*state, state->steamvr_delivery->display_period()));
}

// A Luke presenter restart can occur while SteamVR reports a throttled period.
// Keep its physical clock independent of the route that happened to start it.
[[nodiscard]] bool adopt_luke_display_period(SessionState& state,
    std::optional<std::chrono::nanoseconds> measured) noexcept {
    if (!state.luke_afw_enabled || !state.dispatch->steamvr_runtime ||
        state.graphics_binding != SessionGraphicsBinding::d3d11 ||
        !measured || measured->count() < 2'000'000 || measured->count() > 50'000'000) return false;
    std::scoped_lock lock(state.presenter_mutex);
    const auto prior=state.presenter_display_period;
    // OpenXR's time conversion can round the same HMD period by a few ns.
    if (prior>0 && std::abs(prior-measured->count())<=1000) return false;
    state.presenter_display_period=measured->count();
    state.history_capture_wait_period.store(measured->count(),std::memory_order_relaxed);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        29,prior,measured->count(),1);
    return true;
}
void refresh_luke_display_period(const std::shared_ptr<SessionState>& state) noexcept {
    if (state->luke_afw_enabled && state->dispatch->steamvr_runtime &&
        state->graphics_binding==SessionGraphicsBinding::d3d11 && state->steamvr_delivery)
        static_cast<void>(adopt_luke_display_period(*state,state->steamvr_delivery->display_period()));
}

[[nodiscard]] std::chrono::nanoseconds sample_djules_phase_step(
    const std::shared_ptr<SessionState>& state, bool fresh_real,
    std::uint64_t sequence) noexcept {
    if(state->private_runtime)return {};
    if (!fresh_real || !state->menu_enabled || !native_measured_phase_clock(*state) ||
        !state->dispatch->steamvr_runtime || !state->steamvr_delivery) return {};
    const auto left = state->steamvr_delivery->frame_time_remaining();
    const auto period = std::chrono::nanoseconds(state->presenter_display_period);
    DjulesPhaseAcquisition acquisition{state->djules_phase_cooldown};
    const auto step = acquisition.step(left, period);
    state->djules_phase_cooldown = acquisition.cooldown;
    if (left) xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
        fsr_steamvr_phase_clock(*state)?5571:27, sequence, static_cast<std::uint64_t>(left->count() - 3'000'000),
        static_cast<std::uint64_t>(step.count()));
    return step;
}

// Observer only: compositor records never feed this variant's pacing policy.
// Poll on the application frame thread, at most four times a second, and only
// while diagnostics are enabled. Session establishment remains the D33 gate.
void log_benchmark_configuration(SessionState& state) noexcept {
    auto& logger = xrfg::bridge_flight_logger();
    if (!logger.enabled()) return;
    const auto control = xrfg::autonomous::snapshot();
    // UI/AFW/pipeline revisions and embedded renderer revisions are independent.
    if(state.benchmark_command_revision!=control.revision) {
        state.benchmark_command_revision=control.revision;
        logger.event(xrfg::BridgeFlightOperation::benchmark_command,558,control.revision,
            xrfg::autonomous::benchmark_active(control.benchmark) ? 1 : 0,control.benchmark.run_id);
    }
    // PreferFPS/3X must not erase all subsequent benchmark labels.
    if (control.revision != xrfg::autonomous::applied_runtime_revision.load(std::memory_order_acquire) ||
        xrfg::embedded::snapshot().revision != state.control_revision) return;
    const auto& s = control.desired;
    if (djules_native_scheduler(state) &&
        (state.djules_triple_requested != s.triple_frame_gen ||
         state.djules_deep_pipeline != (s.prefer_fps || s.triple_frame_gen))) return;
    const std::uint64_t bits = (s.enabled ? 1ULL : 0ULL) | (std::uint64_t(s.backend) << 1) |
        (std::uint64_t(s.frame_generation) << 3) | (s.separate_synthesis ? 1ULL << 5 : 0ULL) |
        (s.lower_vram ? 1ULL << 6 : 0ULL) | (s.frame_generation == 3 ? 1ULL << 7 : 0ULL) |
        (state.nvidia_options.hybrid ? 1ULL << 8 : 0ULL) | (s.prefer_fps ? 1ULL << 9 : 0ULL) |
        (s.triple_frame_gen ? 1ULL << 10 : 0ULL) | (s.motion_vectors ? 1ULL << 11 : 0ULL) |
        (std::uint64_t(s.afw_mode) << 12) | (s.nr_enabled ? 1ULL << 14 : 0ULL) |
        (state.binding_queue ? 1ULL << 15 : 0ULL) | (state.luke_afw_enabled ? 1ULL << 16 : 0ULL) |
        (std::uint64_t(xrfg::standalone::process_dlssg_preset()) << 17) |
        (std::uint64_t(std::max(0,xrfg::embedded::afw_snapshot().active_shading_rate)) << 20) |
        (s.pacing_mode==1 ? 1ULL << 40 : 0ULL) | (std::uint64_t(s.pacing_mode)<<42) | (single_director_pacing(state) ? 1ULL << 41 : 0ULL) |
        (state.private_runtime ? (1ULL<<45) | (state.private_runtime->buffer_capacity()==3 ? 1ULL<<44:0ULL):0ULL) |
        (xrfg::autonomous::benchmark_active(control.benchmark) ? (1ULL<<24) | (std::uint64_t(control.benchmark.index)<<25) : 0ULL);
    logger.event(xrfg::BridgeFlightOperation::benchmark_configuration, 0, control.revision, bits,
        std::uint64_t(s.preset) | (std::uint64_t(s.scale) << 8) | (std::uint64_t(s.native_scale) << 16));
}

void log_compositor_delivery(SessionState& state) noexcept {
    // Refresh the atomic physical-period snapshot on the application observer
    // path, including F9/UI-off operation. A busy observer lock returns its
    // cache; the independent physical director never calls OpenVR or waits
    // for that lock merely to read a known headset frequency.
    if(state.private_runtime && state.steamvr_delivery) {
        const auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto next=state.private_clock_next_poll_ns.load(std::memory_order_relaxed);
        if(now>=next && state.private_clock_next_poll_ns.compare_exchange_strong(next,now+250'000'000)) {
            static_cast<void>(state.steamvr_delivery->display_period());
            static_cast<void>(state.steamvr_delivery->vsync_anchor(true));
        }
    }
    if (state.fps_overlay) {
        std::scoped_lock lock(state.presenter_mutex);
        state.fps_overlay->set_display_period(state.presenter_active ? state.presenter_display_period : state.minimum_runtime_display_period);
    }
    if (!xrfg::bridge_flight_logger().enabled() || !state.steamvr_delivery) return;
    try {
        const auto now = std::chrono::steady_clock::now();
        if (now < state.compositor_diagnostic_next_poll) return;
        state.compositor_diagnostic_next_poll = now + std::chrono::milliseconds(250);
        log_benchmark_configuration(state);
        const auto pack = [](float ms) -> std::uint64_t {
            return static_cast<std::uint64_t>(std::clamp(
                static_cast<double>(ms) * 100.0 + 32768.0, 0.0, 65535.0));
        };
        auto& logger = xrfg::bridge_flight_logger();
        // The private director's clock is refreshed above regardless of F9.
        // Diagnostics must only observe that cache: another vsync query here
        // would change the shared phase sampling when logging is enabled.
        // Cached anchors have zero query cost because this read makes no call.
        const auto observed_vsync = state.private_runtime
            ? state.steamvr_delivery->cached_vsync_anchor()
            : state.steamvr_delivery->vsync_anchor();
        if (const auto& vsync = observed_vsync) {
            logger.event(xrfg::BridgeFlightOperation::compositor_vsync, 0,
                vsync->frame_counter,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    vsync->at.time_since_epoch()).count()),
                static_cast<std::uint64_t>(vsync->cost.count()));
        }
        for (const auto& frame : state.steamvr_delivery->settled_frames()) {
            logger.event(xrfg::BridgeFlightOperation::compositor_presentation,
                frame.mispresented, frame.frame_index,
                static_cast<std::uint64_t>(frame.reprojection_flags) |
                    (static_cast<std::uint64_t>(frame.skipped) << 32),
                static_cast<std::uint64_t>(frame.presents) |
                    (static_cast<std::uint64_t>(frame.dropped) << 32));
            logger.event(xrfg::BridgeFlightOperation::compositor_timeline, 0,
                frame.frame_index,
                static_cast<std::uint64_t>(std::max(0.0, frame.system_time_seconds) * 1e6),
                pack(frame.wait_get_poses_called_ms) |
                    (pack(frame.new_frame_ready_ms) << 16) |
                    (pack(frame.compositor_render_start_ms) << 32));
            logger.event(xrfg::BridgeFlightOperation::compositor_timeline, 1,
                frame.frame_index,
                static_cast<std::uint64_t>(frame.total_render_gpu_us) |
                    (static_cast<std::uint64_t>(frame.compositor_render_gpu_us) << 32),
                static_cast<std::uint64_t>(frame.ready_vsyncs) |
                    (static_cast<std::uint64_t>(frame.vsyncs_to_first_view) << 32));
        }
    } catch (...) {} // An optional observer cannot fail an application frame.
}

// The gate covers exactly one call into the runtime, so a call is what it
// takes. Several frame submissions reach the runtime through the overlay
// rather than through the dispatch pointer, and those are the ones a live
// session actually uses, so the whole submitting expression is handed over
// rather than the dispatch call inside it.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(SessionState* session, Call&& call)
    -> decltype(call()) {
    const auto token = session && session->d3d11_device
        ? xrfg::bridge_flight_logger().begin(xrfg::BridgeFlightOperation::timing_runtime_gate)
        : xrfg::BridgeFlightToken{};
    const RuntimeEntry gate(session);
    xrfg::bridge_flight_logger().end(token,
        xrfg::BridgeFlightOperation::timing_runtime_gate, 0);
    return call();
}

// The frame paths hold the session by shared_ptr and the swapchain paths by
// raw pointer; the gate does not care which.
template <typename Call>
[[nodiscard]] auto with_runtime_entry(
    const std::shared_ptr<SessionState>& session, Call&& call)
    -> decltype(call()) {
    return with_runtime_entry(session.get(), std::forward<Call>(call));
}

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
    bool first_use_logged{};
};

// A buffered pair owns one current and one synthetic private swapchain. An
// OpenXR composition layer names a swapchain rather than an image index, and
// the runtime binds whichever image was released last when xrEndFrame runs.
// Reusing either handle while an older pair is queued would therefore repoint
// that queued output at newer content. Two complete slots preserve FIFO order
// while the application produces independently of the OpenXR consumer.
constexpr std::size_t kCurrentSlotCount = 2;
constexpr std::size_t kSyntheticSlotCount = 2;
constexpr std::size_t kAfwScratchSlot = kCurrentSlotCount;

struct CompositionCopy12 {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    std::uint64_t ready_value{};
};

struct DjulesOutputStaging {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    std::atomic<std::uint64_t> last_handover_copy{};
};

struct DirectSyntheticLease {}; // descriptor ownership only; never a private color texture
struct DirectOutputPublication;

struct FrameGenerationSwapchainState {
    bool staged_outputs{};
    std::uint32_t current_runtime_rings{2}, synthetic_runtime_rings{2};
    std::uint32_t current_runtime_images_per_ring{}, synthetic_runtime_images_per_ring{};
    std::uint32_t staging_stride{1};
    std::array<std::shared_ptr<DjulesOutputStaging>,2> current_staging;
    std::array<std::shared_ptr<DjulesOutputStaging>,4> synthetic_staging;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> current_runtime_images, synthetic_runtime_images;
    Microsoft::WRL::ComPtr<ID3D12Fence> handover_fence;
    std::array<CompositionCopy12,8> handover_copies;
    std::uint64_t handover_serial{};
    std::size_t handover_copy_slot{};
    // AFW scratch has its own handle. It cannot repoint either queued real.
    std::array<PrivateSwapchainState, kCurrentSlotCount + 1> current{};
    // Destination images are addressed by a flat index across every slot, so
    // slot s image i is s * current_images_per_slot + i.
    std::uint32_t current_images_per_slot{};
    std::size_t current_slot{};
    std::array<PrivateSwapchainState, 4> synthetic{};
    bool direct_extra_outputs{};
    std::array<std::shared_ptr<DirectSyntheticLease>,2> direct_synthetic_leases;
    std::array<std::atomic_bool,kCurrentSlotCount+1> current_publication_busy{};
    std::array<std::atomic_bool,4> synthetic_publication_busy{};
    std::uint32_t synthetic_images_per_slot{};
    std::size_t synthetic_slot{};
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
    // Luke AFW writes the final eye into the current interop mirror. Its
    // optical-flow history must be captured there, never from the AER source.
    std::shared_ptr<xrfg::D3D12SwapchainHistory> afw_final_history;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> afw_temporal_synthesizer;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> afw_source_synthesizer;
    bool isolated_luke_synthesis{};
    std::weak_ptr<SessionState> shared_queue_owner;
    ~FrameGenerationSwapchainState() {
        if (auto owner=shared_queue_owner.lock()) --owner->luke_shared_generation_count;
    }
};


struct CompositionPublicOutput {
    std::weak_ptr<SessionState> session;
    PrivateSwapchainState image;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pending_source;
    std::uint64_t serial{};
    XrSwapchain published_snapshot{};
    std::uint64_t published_ready_value{};
    bool unproven_copy{};
};
void delete_composition_public_output(CompositionPublicOutput* output) noexcept;

struct CompositionImageSlot {
    PrivateSwapchainState image;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> textures12;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    CompositionCopy12 copy12;
    std::uint64_t ready_value{};
    // For private auxiliary snapshots image.handle is a metadata token, NEVER
    // a runtime handle. The source cannot be reused before its last GPU read.
    std::shared_ptr<CompositionPublicOutput> public_output;
    std::atomic<std::uint64_t> last_publication{};
    bool copy_failed{};
};

struct CompositionCapture {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    Microsoft::WRL::ComPtr<ID3D12Resource> texture12;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    std::array<CompositionCopy12, 3> copies12;
    std::size_t next_copy12{};
    std::uint64_t fence_value{};
    bool valid{};
    // Two retained endpoint colors plus the producer's completed, not-yet-
    // admitted AFW packet. S/B share their endpoint lease. This is storage,
    // not another director slot: admission and physical cadence are unchanged.
    std::array<std::shared_ptr<CompositionImageSlot>, 3> outputs;
    std::shared_ptr<CompositionPublicOutput> public_output;
};

template<class Fence>
[[nodiscard]] HRESULT wait_composition_fence(Fence* fence, std::uint64_t value,
    DWORD timeout=INFINITE) {
    if (!fence || !value) return S_OK;
    const auto completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed >= value) return S_OK;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT result = fence->SetEventOnCompletion(value, event);
    if (SUCCEEDED(result)) {
        const auto waited=WaitForSingleObject(event, timeout);
        if(waited!=WAIT_OBJECT_0)
            result=waited==WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_BUSY) : E_FAIL;
    }
    CloseHandle(event);
    return result;
}

// These copies execute on the application/runtime queue, not the optical-flow
// queue. A bounded allocator ring can wait only for its own previous copy.
[[nodiscard]] HRESULT copy_composition12(
    ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence,
    std::uint64_t& serial, CompositionCopy12& work,
    ID3D12Resource* destination, D3D12_RESOURCE_STATES destination_state,
    ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) {
    HRESULT result = wait_composition_fence(fence, work.ready_value);
    if (FAILED(result)) return result;
    if (!work.allocator) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&work.allocator));
        if (SUCCEEDED(result)) result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            work.allocator.Get(), nullptr, IID_PPV_ARGS(&work.list));
        if (FAILED(result)) return result;
        work.list->SetName(L"OFXR Owned Composition Copy");
    } else {
        result = work.allocator->Reset();
        if (SUCCEEDED(result)) result = work.list->Reset(work.allocator.Get(), nullptr);
        if (FAILED(result)) return result;
    }
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE};
    barriers[1].Transition = {destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, destination_state, D3D12_RESOURCE_STATE_COPY_DEST};
    work.list->ResourceBarrier(2, barriers);
    work.list->CopyResource(destination, source);
    for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    work.list->ResourceBarrier(2, barriers);
    result = work.list->Close();
    if (FAILED(result)) return result;
    ID3D12CommandList* lists[] = {work.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    result = queue->Signal(fence, serial + 1);
    if (SUCCEEDED(result)) work.ready_value = ++serial;
    return result;
}

void log_completed_nvidia_gpu_timings(
    const std::shared_ptr<xrfg::D3D12FrameSynthesizer>& synthesizer) noexcept {
    if (!synthesizer || !xrfg::bridge_flight_logger().gpu_instrumentation_enabled()) {
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
        // When the GPU actually began and ended this pair's synthesis, on
        // the log's own timeline, so it can be compared directly with the
        // internal_end_frame that handed the synthetic to the runtime.
        if (timing.gpu_begin_qpc != 0 && timing.gpu_end_qpc != 0) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::synthesis_gpu_span,
                static_cast<std::int64_t>(timing.total_microseconds),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_begin_qpc))),
                static_cast<std::uint64_t>(
                    xrfg::bridge_flight_logger().microseconds_for_counter(
                        static_cast<std::int64_t>(timing.gpu_end_qpc))),
                timing.current_serial);
        }
    }
}

struct SwapchainState;

enum class VideoMemoryStage : std::int64_t {
    session = 1,
    application_swapchain = 2,
    current_ring = 3,
    synthetic_ring = 4,
    interop = 5,
    history = 6,
    synthesizer = 7,
    swapchain_destroyed = 8,
    periodic = 9,
    // The first acquire of a private swapchain: a runtime that allocates
    // on first use rather than at creation shows up here, c= the swapchain.
    private_first_use = 10,
    // The staging textures of single-swapchain rings.
    staging = 11,
};

void log_video_memory(
    SessionState& session,
    VideoMemoryStage stage,
    std::uint64_t subject,
    xrfg::autonomous::BenchmarkMemory* memory = nullptr) noexcept {
    try {
        if (!memory && !xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
        {
            std::scoped_lock lock(session.video_memory_mutex);
            if (!session.video_memory_adapter && !session.video_memory_unavailable) {
                LUID luid{};
                bool found = false;
                if (session.d3d12_device) {
                    luid = session.d3d12_device->GetAdapterLuid();
                    found = true;
                } else if (session.d3d11_device) {
                    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
                    Microsoft::WRL::ComPtr<IDXGIAdapter> device_adapter;
                    DXGI_ADAPTER_DESC description{};
                    if (SUCCEEDED(session.d3d11_device.As(&dxgi_device)) &&
                        SUCCEEDED(dxgi_device->GetAdapter(&device_adapter)) &&
                        SUCCEEDED(device_adapter->GetDesc(&description))) {
                        luid = description.AdapterLuid;
                        found = true;
                    }
                }
                // No device yet: a later record finds one.
                if (!found) {
                    return;
                }
                Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
                if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                    FAILED(factory->EnumAdapterByLuid(
                        luid, IID_PPV_ARGS(&session.video_memory_adapter)))) {
                    session.video_memory_adapter.Reset();
                    session.video_memory_unavailable = true;
                    return;
                }
            }
            adapter = session.video_memory_adapter;
        }
        if (!adapter) {
            return;
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        if (FAILED(adapter->QueryVideoMemoryInfo(
                0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
            return;
        }
        // Publish this existing read only; no UI-specific query or new poll.
        xrfg::autonomous::report_video_memory({GetTickCount64(),local.CurrentUsage,local.Budget});
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vram_usage,
            static_cast<std::int64_t>(stage),
            local.CurrentUsage,
            local.Budget,
            subject);
        if(memory) {
            memory->vram_valid=true; memory->vram_bytes=local.CurrentUsage;
            DXGI_QUERY_VIDEO_MEMORY_INFO shared{};
            if(SUCCEEDED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL,&shared))) {
                memory->shared_gpu_valid=true;memory->shared_gpu_bytes=shared.CurrentUsage;
            }
        }
    } catch (...) {
    }
}

void sample_benchmark_memory(SessionState& state) noexcept {
    const auto revision=xrfg::autonomous::take_benchmark_memory_request();
    if(!revision)return;
    xrfg::autonomous::BenchmarkMemory memory{};
    memory.revision=revision;memory.updated_ms=GetTickCount64();
    PROCESS_MEMORY_COUNTERS_EX process{};process.cb=sizeof(process);
    if(K32GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),sizeof(process))) {
        memory.ram_valid=true;memory.ram_working_set_bytes=process.WorkingSetSize;
        memory.ram_private_commit_bytes=process.PrivateUsage;
    }
    log_video_memory(state,VideoMemoryStage::periodic,0,&memory);
    xrfg::autonomous::report_benchmark_memory(memory);
}

// swapchain_image for a D3D12 image: what the runtime actually allocated
// against what was asked for. result= DXGI format, a= swapchain, b= width
// <<32 | height, c= mip levels<<48 | array size<<32 | resource flags.
void log_d3d12_image_description(XrSwapchain swapchain, ID3D12Resource* image) noexcept {
    try {
        if (image == nullptr ||
            !xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        const D3D12_RESOURCE_DESC description = image->GetDesc();
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_image,
            static_cast<std::int64_t>(description.Format),
            handle_value(swapchain),
            (static_cast<std::uint64_t>(description.Width) << 32) |
                description.Height,
            (static_cast<std::uint64_t>(description.MipLevels) << 48) |
                (static_cast<std::uint64_t>(description.DepthOrArraySize) << 32) |
                static_cast<std::uint32_t>(description.Flags));
    } catch (...) {
    }
}

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
    awaiting_projection_use = 17,
    budget_exhausted = 18,
};

void log_swapchain_eligibility(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason reason,
    std::uint64_t detail = 0,
    std::uint64_t auxiliary = 0) noexcept;

struct SwapchainState {
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    // On a bridged D3D11 session (SessionState::d3d11_bridge): the shared
    // textures the application renders into and the copies into the
    // runtime's images. enumerated_d3d12_images are then the shared
    // textures, which is what the D3D12 history captures from.
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    bool private_depth{};
    bool pooled_publication{}; // generated outputs share only this projection source's native sink
    std::atomic_bool projection_history_needed{true}; // unknown until the first actual projection
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
    // Djules 690e1ee9 / V335: waited images remain owned across session
    // begin/end, and several acquire/wait calls may precede the first release.
    std::size_t waited_count{};
    bool ownership_tracking_valid{true};
    std::optional<std::uint32_t> last_released_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> d3d12_history;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> enumerated_d3d11_images;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> enumerated_d3d12_images;
    std::optional<xrfg::D3D12HistoryCaptureTicket> last_released_capture;
    // Latched before producer backpressure, never sampled from EndFrame.
    std::optional<xrfg::LukeRenderIdentity> luke_released_identity;
    std::optional<xrfg::LukeRenderIdentity> luke_admitted_identity;
    std::uint64_t luke_admitted_capture_serial{};
    bool luke_released_duplicate{};
    std::optional<xrfg::LukeRenderIdentity> luke_captured_source;
    std::uint64_t last_diagnostic_capture{};
    std::shared_ptr<const xrfg::DlssMotionVectorSet> last_released_motion_vectors;
    // Learned from a two-view projection, for non-AFW native D3D12 guides.
    std::atomic<int> projection_eye{-1};
    xrfg::DlssReleasedEyeIdentity fsr_eye_identity{}; // protected by swapchain mutex
    // Watermark captured on entry to the release of this exact color ticket.
    // R15 and ordinary D3D12 FSR exact joins write/read this watermark.
    std::uint64_t release_publication{}; // mutex
    std::uint32_t dlss_logical_eye_count{1};
    std::optional<std::uint32_t> dlss_logical_eye_index;
    bool presentation_transform_known{};
    bool presentation_transform_active{};
    std::shared_ptr<FrameGenerationSwapchainState> frame_generation;
    // Release-time private snapshot, never a read of an already released app
    // image. Output leases are held by A and by B/S(A,B) until presentation.
    // call_mutex guards capture/pool mutations; gpu_mutex guards D3D11 calls.
    std::unique_ptr<CompositionCapture> composition_capture;
    bool composition_referenced{};
    unsigned diagnostic_layer_roles{}; // observed roles only; never drives capture
    // Generation costs three runtime swapchains, and a swapchain that never
    // reaches a projection layer never spends them: an application's UI quads
    // and its stereo views are indistinguishable at enumeration time, so the
    // decision waits until xrEndFrame names this swapchain as a projection
    // view. Guarded by call_mutex.
    bool generation_eligible_pending{};
    // Ordinary D11 bridge only; call_mutex. No borrowed extension pointers.
    std::optional<std::uint32_t> pending_bridge_release;
    std::chrono::steady_clock::time_point pending_bridge_release_at{};
    std::uint32_t enumerated_image_count{};
    // An attempt that failed for its own reasons -- synthesis, interop -- is
    // not retried every frame. Provider initialization refusals can be retired
    // by a control reconfiguration; structural and runtime budget refusals
    // retain their existing lifetime and are never reopened by menu changes.
    bool generation_declined{};
    SwapchainEligibilityReason generation_refusal{SwapchainEligibilityReason::ready};
    std::uint64_t generation_refusal_detail{};
};

using QueryLukeRossD3D12Context = int (*)(
    ID3D12Device**, ID3D12CommandQueue**) noexcept;
#ifdef XRFG_NATIVE_INGRESS_TESTS
std::filesystem::path test_luke_bootstrap_provider;
#endif
std::filesystem::path luke_bootstrap_provider() {
#ifdef XRFG_NATIVE_INGRESS_TESTS
    if(!test_luke_bootstrap_provider.empty())return test_luke_bootstrap_provider;
#endif
    return current_layer_directory()/L"PDAFWPlugin.dll";
}

// A Luke D11 session consumes native D12 productions, but must not append
// OFA/interop waits to the game's rendering queue. Its next native submission
// can depend on the XR release currently waiting for this worker's output.
HRESULT select_luke_execution_queue(bool native_luke, ID3D12Device* device,
    ID3D12CommandQueue* producer, Microsoft::WRL::ComPtr<ID3D12CommandQueue>& execution) {
    execution.Reset();
    if (!device || !producer) return E_INVALIDARG;
    if (!native_luke) { execution = producer; return S_OK; }
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.NodeMask = producer->GetDesc().NodeMask;
    const auto result = device->CreateCommandQueue(&description,
        IID_PPV_ARGS(execution.ReleaseAndGetAddressOf()));
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        result, 957, handle_value(producer), handle_value(execution.Get()));
    return result;
}

// Separate the game-side capture/AFW pipeline from OFXR, independent of the
// selected runtime/pacing. Do not reuse the native-D12 transport queue flag:
// Luke still submits D11 XR images and must retain that publication contract.
void initialize_luke_synthesis_queue(SessionState& state) noexcept {
    if (!state.luke_afw_enabled || state.afw_only ||
        state.graphics_binding != SessionGraphicsBinding::d3d11 ||
        !state.d3d12_device || !state.d3d12_queue || state.luke_synthesis_queue) return;
    // OFXR works on the D12 owner already validated by the D11 interop.
    // Do not wait for native AFW provenance (capability8): projection engines
    // created during loading would otherwise keep the shared lane forever.
    // This grants no native-guide authority and does not bootstrap AFW.
    D3D12_COMMAND_QUEUE_DESC description{};
    description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    description.NodeMask = state.d3d12_queue->GetDesc().NodeMask;
    // Same normal priority as capture/AFW: don't promote OFXR above its inputs.
    const auto result = state.d3d12_device->CreateCommandQueue(&description,
        IID_PPV_ARGS(state.luke_synthesis_queue.ReleaseAndGetAddressOf()));
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        result, 958, handle_value(state.d3d12_queue.Get()), handle_value(state.luke_synthesis_queue.Get()));
}

[[nodiscard]] bool session_uses_shared_queue(const SessionState& state) noexcept {
    if (state.afw_only) return false;
    if (state.luke_afw_enabled && state.graphics_binding == SessionGraphicsBinding::d3d11)
        return (state.graphics_binding_capabilities & 8ULL) != 0 &&
            (!state.luke_synthesis_queue || state.luke_synthesis_queue==state.d3d12_queue ||
             state.luke_shared_generation_count.load()!=0);
    return state.dispatch->steamvr_runtime &&
        ((state.graphics_binding == SessionGraphicsBinding::d3d12 &&
          (!state.d3d12_synthesis_queue || state.d3d12_synthesis_queue==state.d3d12_queue)) ||
         state.graphics_binding == SessionGraphicsBinding::d3d11);
}

[[nodiscard]] bool query_luke_ross_d3d12_context(
    ID3D12Device** device,
    ID3D12CommandQueue** queue) noexcept {
    if (device != nullptr) *device = nullptr;
    if (queue != nullptr) *queue = nullptr;
    if (device == nullptr || queue == nullptr) return false;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&query_luke_ross_d3d12_context),
            &module)) {
        return false;
    }
    const auto query = reinterpret_cast<QueryLukeRossD3D12Context>(
        GetProcAddress(module, "OFXR_GetLukeRossD3D12ContextV1"));
    if(query != nullptr && query(device, queue) != 0 && *device != nullptr && *queue != nullptr)return true;
    return xrfg::native_vulkan::query_luke_context(device,queue);
}

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
struct ObservedSpace {
    std::shared_ptr<Dispatch> dispatch;
    std::weak_ptr<SessionState> session;
};
std::unordered_map<XrSpace, ObservedSpace> g_observed_spaces; // g_state_mutex
std::atomic<std::uint64_t> g_space_sample_id{1};

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

#include "private_runtime_endpoint.inc"

// HFW has more application XR cycles than original AER source productions.
void configure_session_guide_capture(const SessionState& state) {
    // Plain Luke OFXR consumes the released XR colour. With AFW off it must
    // not depend on a successful SR capture, colour barcode or native camera.
    // Keep the observation hooks installed for late context discovery and for
    // turning AFW back on, but do not record native snapshots/markers here.
    const bool raw_luke_ofxr = state.luke_afw_enabled &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr &&
        !state.hybrid_requested && xrfg::embedded::afw_snapshot().desired.mode == 0;
    const bool outputs_needed=xrfg::embedded::snapshot().desired.enabled ||
        (state.luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode!=0);
    const bool enabled=xrfg::neural_rendering::status().enabled ||
        (outputs_needed && !raw_luke_ofxr && state.dlss_motion_vectors && (!state.luke_fresh_only ||
            (!state.afw_only && xrfg::embedded::snapshot().desired.enabled) || xrfg::embedded::afw_snapshot().desired.mode!=0));
    if(!enabled)
        xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::configure_ngx_guide_capture(state.d3d12_queue.Get(),enabled);
    if(state.vulkan_bridge)
        xrfg::native_vulkan::configure_guide_capture(state.vulkan_binding,state.d3d12_device.Get(),
            state.d3d12_queue.Get(),enabled);
}
// Physical Wait/Begin/End belong to actual outputs only. Virtual app polling
// never closes an empty physical frame or resubmits old color with newer poses.
[[nodiscard]] XrTime add_display_duration(XrTime time,XrDuration duration) noexcept;
void refresh_luke_prediction_clock(SessionState& state,const XrFrameState& slot,
    std::chrono::steady_clock::time_point now) {
    // A physical runtime slot can trail the virtual producer prediction after
    // GPU preparation. Rebasing backward freezes subsequent poses at +1ns.
    // Preserve elapsed producer time; only a newer runtime anchor can advance it.
    const auto origin=state.luke_prediction_origin ? state.luke_prediction_origin :
        state.last_inline_frame_state_valid ? state.last_inline_frame_state.predictedDisplayTime : 0;
    const auto wall=state.luke_prediction_origin ? state.luke_prediction_wall : state.application_wait_returned_at;
    const auto elapsed=wall.time_since_epoch().count() && now>=wall
        ? std::chrono::duration_cast<std::chrono::nanoseconds>(now-wall).count() : 0;
    const XrTime continuous=origin ? add_display_duration(origin,elapsed) : slot.predictedDisplayTime;
    state.luke_prediction_origin=(std::max)({slot.predictedDisplayTime,continuous,state.last_virtual_display_time});
    state.luke_prediction_wall=now;
}
void record_rolling_runtime_output(const XrFrameEndInfo* info) noexcept;
[[nodiscard]] bool continuous_presenter_active(const std::shared_ptr<SessionState>& state) noexcept;
XrResult end_frame_with_private_auxiliary(const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo* info,const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& delivery={});

XrResult observed_content_end_frame(const std::shared_ptr<SessionState>& state,const XrFrameEndInfo* info,
    const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& delivery) {
    if(!state || !state->dispatch->end_frame)return XR_ERROR_HANDLE_INVALID;
    const auto session=state->handle;
    if(!state->luke_fresh_only || continuous_presenter_active(state))
        return end_frame_with_private_auxiliary(state,info,delivery);
    if(!info || info->type!=XR_TYPE_FRAME_END_INFO)return XR_ERROR_VALIDATION_FAILURE;
    if(!state->luke_output_begun) {
        XrFrameState slot{XR_TYPE_FRAME_STATE};
        if(state->runtime_frame_waited_unbegun && state->last_inline_frame_state_valid)
            slot=state->last_inline_frame_state;
        else {
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            const auto result=state->dispatch->wait_frame(session,&wait,&slot);
            if(XR_FAILED(result))return result;
            state->runtime_frame_waited_unbegun=true;
            state->last_inline_frame_state=slot;
            state->last_inline_frame_state_valid=true;
        }
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        const auto result=state->dispatch->begin_frame(session,&begin);
        if(XR_FAILED(result))return result;
        state->runtime_frame_waited_unbegun=false;
        state->luke_output_begun=true;
        state->luke_output_slot=slot;
        refresh_luke_prediction_clock(*state,slot,std::chrono::steady_clock::now());
        state->last_inline_frame_state=slot;
        state->last_inline_frame_state_valid=true;
    }
    auto submitted=*info;
    // Pose/FOV/space/rect are the original immutable image metadata. Only the
    // actual physical presentation slot is assigned here.
    submitted.displayTime=state->luke_output_slot.predictedDisplayTime;
    if(state->luke_output_slot.shouldRender==XR_FALSE) {
        submitted.layerCount=0;
        submitted.layers=nullptr;
    }
    record_rolling_runtime_output(&submitted);
    const auto result=end_frame_with_private_auxiliary(state,&submitted,delivery);
    if(xrfg::diagnostic_ring_enabled)xrfg::diagnostic_ring_record("{\"kind\":\"runtime_result\",\"qpc\":"+
        std::to_string(xrfg::diagnostic_ring_qpc())+",\"display_time\":"+std::to_string(submitted.displayTime)+",\"result\":"+std::to_string(result)+"}");
    state->luke_output_begun=false;
    return result;
}

XrResult XRAPI_CALL content_end_frame(XrSession session,const XrFrameEndInfo* info) {
    return observed_content_end_frame(find_session(session),info,{});
}
void bind_private_delivery_metrics(const std::shared_ptr<SessionState>& state) {
    if((!state->private_runtime && !state->afw_only) || !state->fps_overlay)return;
    const std::weak_ptr<SessionState> weak=state;
    state->fps_overlay->set_private_delivery_end([weak](const XrFrameEndInfo* info,
        const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& delivery) {
        const auto owner=weak.lock();
        return owner?observed_content_end_frame(owner,info,delivery):XR_ERROR_HANDLE_INVALID;
    });
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
        if (state->session->dispatch->instance == dispatch->instance) {
            matches.push_back(state);
        }
    }
    return matches;
}

// Submit a deferred real-frame copy without waiting for its fence. Moving the
// synthesis wait to capture must not move this copy with it: the game render
// interval gives the copy time to complete before its runtime hand-off.
[[nodiscard]] HRESULT flush_session_pending_copies(
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
                generation->synthesizer->flush_current_copy(nullptr);
            if (FAILED(result)) {
                aggregate = result;
            }
        }
        return aggregate;
    } catch (...) {
        return E_FAIL;
    }
}

[[nodiscard]] HRESULT wait_for_previous_session_synthesis(
    const std::shared_ptr<SessionState>& session, bool application_wait=false) noexcept {
    // REAL VR must receive its next prediction before submitting game work.
    // Scratch/history ownership is still retired at its actual reuse points.
    if(application_wait && session && session->luke_afw_enabled &&
        session->graphics_binding==SessionGraphicsBinding::d3d11) return S_OK;
    try {
        HRESULT aggregate = S_OK;
        for (const auto& swapchain : find_swapchains(session)) {
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (!generation) continue;
            // Promotion must drain both producers of ordinary director
            // destinations. In Luke, the inline OFXR endpoint belongs to
            // afw_temporal_synthesizer, not the AER-to-AFW copy synthesizer.
            // A completed AFW input does not imply that endpoint is idle.
            for (const auto& synthesizer : {
                     generation->synthesizer, generation->afw_temporal_synthesizer,
                     generation->afw_source_synthesizer}) {
                if (!synthesizer) continue;
                const HRESULT result = synthesizer->wait_for_previous_submission(
                    kFrameStartSynthesisWaitMilliseconds);
                if (FAILED(result)) {
                    aggregate = result;
                    if (result != HRESULT_FROM_WIN32(ERROR_BUSY)) return result;
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
        if (state->dispatch->instance == dispatch->instance) {
            matches.push_back(state);
        }
    }
    return matches;
}

void three_slot_director_main(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool fsr_two_slot_session(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool queued_nvidia_session(const std::shared_ptr<SessionState>& state) noexcept;
void synthesis_worker_main(const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] XrResult wait_for_synthesis_handoff(
    const std::shared_ptr<SessionState>& state) noexcept;

// A promoted frame owner inherits an already-forwarded, unbegun runtime
// wait exactly once. Issuing another wait first would deadlock pipelined games.
XrResult wait_presenter_runtime_frame(const std::shared_ptr<SessionState>& state,
    const XrFrameWaitInfo* wait, XrFrameState* frame) {
    {
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_adopted_frame) {
            *frame = *state->presenter_adopted_frame;
            state->presenter_adopted_frame.reset();
            frame->next = nullptr;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                500, frame->predictedDisplayTime, frame->predictedDisplayPeriod, frame->shouldRender);
            return XR_SUCCESS;
        }
    }
    return state->dispatch->wait_frame(state->handle, wait, frame);
}

[[nodiscard]] XrDuration doubled_display_period(XrDuration period) noexcept;
[[nodiscard]] XrDuration djules_application_period(const SessionState& state,XrDuration period) noexcept {
    if(!djules_triple_active(state)) return doubled_display_period(period);
    if(period>std::numeric_limits<XrDuration>::max()/3) return std::numeric_limits<XrDuration>::max();
    return period>0?period*3:period;
}

// The presenter already runs on the physical display period. SteamVR's raw
// prediction period may instead include missed display slots. Doubling that
// throttled value a second time makes the game's camera horizon oscillate
// while delivery stays on the physical clock. Called with presenter_mutex.
[[nodiscard]] XrDuration application_prediction_period(
    const SessionState& state, XrDuration reported_period) noexcept {
    const bool native = state.graphics_binding == SessionGraphicsBinding::d3d12 &&
        !state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled;
    const bool luke = state.graphics_binding == SessionGraphicsBinding::d3d11 && state.luke_afw_enabled;
    if (comparison_delivery_runtime(state) && (native || luke) && !state.luke_outlaws_preframe &&
        state.presenter_display_period >= 2'000'000 &&
        state.presenter_display_period <= 50'000'000)
        return state.presenter_display_period;
    return reported_period;
}

[[nodiscard]] bool presenter_forbidden(const SessionState& state) noexcept {
    return state.afw_only || state.luke_aux_budget_inline ||
        (state.luke_fresh_only && !fresh_luke_director_worker(state)) || (state.graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state.single_threaded_d3d11 || state.split_frame_loop));
}

// V439/V441/V442 are explicitly limited to ordinary Djules optical flow.
[[nodiscard]] bool ordinary_djules_route(const SessionState& state) noexcept {
    return state.djules_route && state.nvidia_new_route && !state.luke_afw_enabled &&
        !state.hybrid_requested && !state.nvidia_options.hybrid &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr;
}
[[nodiscard]] bool presenter_holds_djules_prime(const SessionState& state) noexcept {
    return !(ordinary_djules_route(state) && comparison_delivery_runtime(state));
}
// V461's counted-release principle adapted to the native queued2X bridge.
// Keep other runtime/bridge/deep/one-ring contracts at the V553 baseline.
[[nodiscard]] bool steamvr_counted_native_release(const SessionState& state) noexcept {
    return !single_director_pacing(state) && comparison_delivery_runtime(state) &&
        state.graphics_binding==SessionGraphicsBinding::d3d12 &&
        !state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled &&
        !state.luke_outlaws_preframe && !state.three_slot_director &&
        (state.queued_nvidia_presenter || state.fsr_two_slot_presenter) &&
        !djules_deep_active(state) && !djules_triple_active(state) &&
        !reduced_native_outputs(state) &&
        (state.nvidia_new_route || state.frame_generation_mode!=xrfg::D3D12FrameGenerationMode::ofxr);
}

[[nodiscard]] bool adaptive_optical_reserve_route(const SessionState& state) noexcept {
    return state.menu_enabled && !state.manual_control.stop_requested() &&
        !state.private_runtime && state.dispatch->virtual_desktop_runtime && state.vdxr_legacy &&
        state.graphics_binding == SessionGraphicsBinding::d3d12 &&
        !state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled &&
        !state.luke_outlaws_preframe && !state.three_slot_director &&
        state.queued_nvidia_presenter && state.nvidia_new_route &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr &&
        state.optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        !state.hybrid_requested && !state.nvidia_options.hybrid &&
        !djules_deep_active(state) && !djules_triple_active(state) && !reduced_native_outputs(state);
}

// A positive learned lead advances preparation by less than one display slot.
// The existing two-ring output pool owns the preceding B and the new S/B.
// One admission per acknowledged S prevents an unbounded producer backlog.
[[nodiscard]] XrResult wait_for_optical_reserve_admission(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        for (;;) {
            if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
            if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
            if (state->presenter_submissions.empty() && !state->presenter_inflight_submission) break;
            // A stale S receipt cannot publish another pair behind a later
            // complete S/B pair. Only its single B may remain when the
            // already prepared new pair enters the presentation queue.
            if (state->outstanding_presenter_submissions <= 1 &&
                state->optical_reserve_pair > state->optical_reserve_served) {
                const auto budget = state->optical_reserve_ns.load(std::memory_order_relaxed);
                const auto due = state->optical_reserve_current_due - std::chrono::nanoseconds(budget);
                if (std::chrono::steady_clock::now() >= due) break;
                // Windows condition-variable timeouts may round past the
                // entire fractional lead. Use a separate high-resolution
                // publication timer; bounded slices recheck stop/failure and
                // release presenter_mutex while the consumer retires B.
                if (!state->optical_reserve_timer)
                    state->optical_reserve_timer = CreateWaitableTimerExW(nullptr, nullptr,
                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    due - std::chrono::steady_clock::now()).count();
                LARGE_INTEGER delay{};
                delay.QuadPart = -(std::max)(1LL, (std::min)(remaining, 1'000'000LL) / 100);
                if (state->optical_reserve_timer && SetWaitableTimer(state->optical_reserve_timer,
                        &delay, 0, nullptr, nullptr, FALSE)) {
                    lock.unlock();
                    const auto waited = WaitForSingleObject(state->optical_reserve_timer, 100);
                    lock.lock();
                    if (waited != WAIT_OBJECT_0) return XR_ERROR_RUNTIME_FAILURE;
                } else state->presenter_condition.wait_until(lock, due);
            } else state->presenter_condition.wait(lock);
        }
        state->optical_reserve_served = state->optical_reserve_pair;
        return XR_SUCCESS;
    } catch (...) { return XR_ERROR_RUNTIME_FAILURE; }
}

[[nodiscard]] std::uint32_t history_capture_wait_limit_us(const SessionState& state) noexcept {
    if (!state.djules_history_wait_enabled.load(std::memory_order_relaxed)) return 0;
    auto period=state.history_capture_wait_period.load(std::memory_order_relaxed);
    if (period<=0) period=11'111'111;
    return static_cast<std::uint32_t>(std::min<XrDuration>(period,20'000'000)/1000);
}
void release_pending_bridge_image_locked(const std::shared_ptr<SwapchainState>& state) noexcept;
void release_pending_bridge_images(const std::shared_ptr<SessionState>& state) noexcept;

[[nodiscard]] bool djules_inline_runtime(const SessionState& state) noexcept {
    return !state.private_runtime && state.djules_route && state.nvidia_new_route && !state.luke_afw_enabled &&
        state.frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        (state.vdxr_legacy || state.dispatch->inline_unless_pipelined) && !state.pipelined_presenter_mode &&
        !(state.vdxr_legacy &&
          ((ordinary_djules_route(state) && (state.presenter_restore_after_bypass || state.presenter_active)) ||
           (!state.split_frame_loop && state.application_wait_thread_id && state.application_end_thread_id &&
            state.application_wait_thread_id == state.application_end_thread_id)));
}

// frame_call_mutex protects the application thread shape. Observe the first
// EndFrame too, so a split loop cannot promote before its next application Wait.
void observe_application_frame_thread(SessionState& state, bool ending) noexcept {
    auto& id = ending ? state.application_end_thread_id : state.application_wait_thread_id;
    id = GetCurrentThreadId();
    if (ending) state.application_end_thread.store(id, std::memory_order_relaxed);
    if (!state.split_frame_loop && state.application_wait_thread_id && state.application_end_thread_id &&
        state.application_wait_thread_id != state.application_end_thread_id) {
        state.split_frame_loop = true;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            600, state.application_wait_thread_id, state.application_end_thread_id,
            static_cast<std::uint64_t>(state.graphics_binding));
    }
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame = nullptr,
    bool preserve_virtual_timeline = false) noexcept;
void stop_continuous_presenter(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool continuous_presenter_active(
    const std::shared_ptr<SessionState>& state) noexcept;
[[nodiscard]] bool three_slot_generation_session(
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
        if (generation && generation->afw_source_synthesizer)
            result = generation->afw_source_synthesizer->wait_for_idle();
        if (generation && generation->afw_temporal_synthesizer) {
            result = generation->afw_temporal_synthesizer->wait_for_idle();
        }
        if (generation && generation->synthesizer) {
            const HRESULT source_result =
                generation->synthesizer->wait_for_idle();
            if (SUCCEEDED(result)) result = source_result;
        }
        if (generation && generation->afw_final_history) {
            const HRESULT final_result =
                generation->afw_final_history->wait_for_idle();
            if (SUCCEEDED(result)) result = final_result;
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
        std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
        {
            std::scoped_lock lock(state->mutex);
            bridge = state->bridge;
        }
        if (bridge) {
            const HRESULT bridge_result = bridge->wait_for_idle();
            if (SUCCEEDED(result)) {
                result = bridge_result;
            }
        }
        std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
        { std::scoped_lock lock(state->mutex); vulkan_bridge = state->vulkan_bridge; }
        if (vulkan_bridge) {
            const HRESULT drained = vulkan_bridge->wait_for_idle();
            if (SUCCEEDED(result)) result = drained;
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
    SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch,
    PrivateSwapchainState& image,
    const xrfg::D3D11PublicationReceipt* completed_publication=nullptr,
    ID3D11Texture2D* completed_texture=nullptr) noexcept {
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
            const XrResult wait_result = with_runtime_entry(session, [&] {
                return dispatch->wait_swapchain_image(image.handle, &wait_info);
            });
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
        const XrResult release_result = with_runtime_entry(session, [&] {
            auto released=XR_ERROR_FUNCTION_UNSUPPORTED;
            if(completed_publication && completed_texture && session && session->private_runtime)
                released=session->private_runtime->release_completed_image(image.handle,&release_info,
                    image.acquired_index,completed_texture,completed_publication->fence.Get(),
                    completed_publication->copy_complete_value);
            if(released==XR_ERROR_FUNCTION_UNSUPPORTED)
                released=dispatch->release_swapchain_image(image.handle, &release_info);
            // Record inside the same runtime gate as actual xrEndFrame, so
            // cross-thread order reflects the runtime's last-released image.
            if(xrfg::bridge_flight_logger().ram_enabled())
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    released,68,handle_value(image.handle),image.acquired_index);
            return released;
        });
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

void delete_composition_public_output(CompositionPublicOutput* output) noexcept {
    try {
        if (output->unproven_copy) {
            const auto result=output->context->Signal(output->fence.Get(),output->serial+1);
            output->context->Flush();
            if (FAILED(result)) return; // quarantine until process/device teardown
            ++output->serial;
        }
        if (FAILED(wait_composition_fence(output->fence.Get(),output->serial,3000))) return;
        if (const auto state=output->session.lock()) {
            if (!release_private_image(state.get(),state->dispatch,output->image)) return;
            if (output->image.handle && state->dispatch->destroy_swapchain) {
                const auto result=with_runtime_entry(state,[&] {
                    return state->dispatch->destroy_swapchain(output->image.handle);
                });
                if (XR_FAILED(result)) return;
            }
        }
        delete output;
    } catch (...) { /* retain an unproven GPU owner */ }
}

void destroy_composition_capture(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (auto capture = std::move(state->composition_capture)) {
            static_cast<void>(wait_composition_fence(capture->fence.Get(), capture->fence_value));
            static_cast<void>(wait_composition_fence(capture->fence12.Get(), capture->fence_value));
            for (auto& slot : capture->outputs) {
                if (!slot || slot->public_output) continue; // metadata token, not XR
                static_cast<void>(release_private_image(state->session.get(), state->session->dispatch, slot->image));
                if (slot->image.handle && state->session->dispatch->destroy_swapchain) {
                    static_cast<void>(with_runtime_entry(state->session.get(), [&] {
                        return state->session->dispatch->destroy_swapchain(slot->image.handle);
                    }));
                    slot->image.handle = XR_NULL_HANDLE;
                }
            }
        }
    } catch (...) {
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
        if(generation->staged_outputs)
            static_cast<void>(wait_composition_fence(generation->handover_fence.Get(),generation->handover_serial));
        for (PrivateSwapchainState& image : generation->synthetic) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        for (PrivateSwapchainState& image : generation->current) {
            static_cast<void>(
                release_private_image(state->session.get(), dispatch, image));
        }
        if (dispatch->destroy_swapchain == nullptr) {
            return;
        }
        std::array<PrivateSwapchainState*,
                   kCurrentSlotCount + 1 + 4> owned{};
        for (std::size_t slot = 0; slot < generation->synthetic.size(); ++slot) {
            owned[slot] = &generation->synthetic[slot];
        }
        for (std::size_t slot = 0; slot < generation->current.size(); ++slot) {
            owned[generation->synthetic.size() + slot] = &generation->current[slot];
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
    CreatedPrivateSwapchain* output,
    XrResult* refusal = nullptr,
    D3D12_RESOURCE_STATES image_state=D3D12_RESOURCE_STATE_COMMON,
    bool generation_output=false) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (!state || !state->session || !state->session->dispatch || output == nullptr) {
        return false;
    }
    const auto& dispatch = state->session->dispatch;
    XrSwapchain handle = XR_NULL_HANDLE;
    // Outlaws must prepare before its first Wait, when projection roles are
    // not known yet. Five native destinations per enumerated color source can
    // exhaust SteamVR before the third source and retire the warmed eyes.
    // Keep every private GPU output independent; only its final native sink is
    // shared by compatible outputs of THIS source, through the existing FIFO.
    // Other Luke games opt into generation-output sharing at session startup;
    // auxiliary outputs keep their existing independent publication contract.
    const bool luke_publication=state->session->private_runtime && state->session->luke_afw_enabled &&
        state->session->luke_shared_swapchains && (state->session->luke_outlaws_preframe || generation_output) &&
        !create_info.next && !create_info.createFlags && create_info.arraySize==1 && create_info.faceCount==1 &&
        (create_info.usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
        !(create_info.usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    XrResult result = state->session->private_runtime
        ? state->session->private_runtime->create_swapchain(&create_info,&handle,image_state,1,
            ((generation_output && state->pooled_publication) || luke_publication)?state->handle:XR_NULL_HANDLE,
            generation_output && state->session->graphics_binding==SessionGraphicsBinding::d3d11
                ? state->session->d3d12_device.Get():nullptr)
        : dispatch->create_swapchain(state->session->handle,&create_info,&handle);
    if (XR_FAILED(result) || handle == XR_NULL_HANDLE) {
        // Which error the runtime gave separates a budget it will not exceed
        // from a create info it rejects outright, and only the first is worth
        // handing the whole session's private swapchains back for.
        if (refusal != nullptr) {
            *refusal = XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE;
        }
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
    std::array<CreatedPrivateSwapchain, 4> slots{};
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
    CreatedCurrentRing* output,
    XrResult* refusal = nullptr,
    std::size_t slot_count = kCurrentSlotCount) {
    if (refusal != nullptr) {
        *refusal = XR_SUCCESS;
    }
    if (output == nullptr) {
        return false;
    }
    if (slot_count == 0 || slot_count > output->slots.size()) return false;
    for (std::size_t slot = 0; slot < slot_count; ++slot) {
        if (!create_private_swapchain(
                state, create_info, &output->slots[slot], refusal,
                state->session->d3d12_synthesis_queue?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_RENDER_TARGET,true)) {
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
    for (std::size_t slot = 0; slot < generation->current.size(); ++slot) {
        generation->current[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->current_images_per_slot = ring.images_per_slot;
    generation->current_slot = 0;
}

void adopt_synthetic_ring(
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    CreatedCurrentRing& ring) noexcept {
    static_assert(kSyntheticSlotCount == kCurrentSlotCount);
    for (std::size_t slot = 0; slot < generation->synthetic.size(); ++slot) {
        generation->synthetic[slot] = ring.slots[slot].state;
        ring.slots[slot].state.handle = XR_NULL_HANDLE;
    }
    generation->synthetic_images_per_slot = ring.images_per_slot;
    generation->synthetic_slot = 0;
}

[[nodiscard]] std::shared_ptr<FrameGenerationSwapchainState>
create_d3d12_frame_generation_swapchains(
    const std::shared_ptr<SwapchainState>& state,
    SwapchainEligibilityReason* failure_reason,
    std::uint64_t* failure_detail) {
    CreatedCurrentRing current;
    CreatedCurrentRing synthetic;
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

        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        generation->staged_outputs = staged_native_outputs(*state->session);
        const auto output_slots = native_current_rings(*state->session);
        // Two runtime S handles serve both 2X and 3X. Direct 3X retains each
        // descriptor through physical submission before the next pair reuses it.
        generation->current_runtime_rings = output_slots;
        generation->synthetic_runtime_rings = native_synthetic_rings(*state->session);
        generation->direct_extra_outputs = !generation->staged_outputs && extra_output_capacity(*state->session);
        if(generation->direct_extra_outputs)
            for(auto& lease:generation->direct_synthetic_leases) lease=std::make_shared<DirectSyntheticLease>();
        generation->staging_stride = generation->staged_outputs && extra_output_capacity(*state->session) ? 2U : 1U;
        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |=
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrResult refusal = XR_SUCCESS;
        if (!create_current_ring(state, private_info, &current, &refusal,
                state->session->luke_afw_enabled ? kCurrentSlotCount + 1 : output_slots)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring, handle_value(state->handle));
        if (!create_current_ring(state, private_info, &synthetic, &refusal,
                generation->synthetic_runtime_rings)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_current_ring(dispatch, &current);
            destroy_current_ring(dispatch, &synthetic);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring, handle_value(state->handle));
        generation->current_runtime_images_per_ring = current.images_per_slot;
        generation->synthetic_runtime_images_per_ring = synthetic.images_per_slot;
        if (generation->staged_outputs) {
            for(auto* image:current.d3d12_resources) generation->current_runtime_images.emplace_back(image);
            for(auto* image:synthetic.d3d12_resources) generation->synthetic_runtime_images.emplace_back(image);
            auto description = current.d3d12_resources.front()->GetDesc();
            description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            HRESULT created = state->session->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&generation->handover_fence));
            current.d3d12_resources.clear(); synthetic.d3d12_resources.clear();
            auto make_stage=[&](auto& stage,auto& destinations) {
                if(FAILED(created)) return;
                stage=std::make_shared<DjulesOutputStaging>();
                created=state->session->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
                    &description,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&stage->resource));
                if(SUCCEEDED(created)) destinations.push_back(stage->resource.Get());
            };
            for(auto& stage:generation->current_staging) make_stage(stage,current.d3d12_resources);
            for(std::size_t i=0; i<kSyntheticSlotCount*generation->staging_stride; ++i)
                make_stage(generation->synthetic_staging[i],synthetic.d3d12_resources);
            if(FAILED(created)) {
                if(failure_detail) *failure_detail=static_cast<std::uint64_t>(created);
                destroy_current_ring(dispatch,&synthetic); destroy_current_ring(dispatch,&current);
                return nullptr;
            }
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
            state->session->d3d12_synthesis_queue
                ? state->session->d3d12_synthesis_queue.Get()
                : state->session->d3d12_queue.Get(),
            state->d3d12_history,
            std::span<ID3D12Resource* const>(
                current.d3d12_resources.data(),
                current.d3d12_resources.size()),
            std::span<ID3D12Resource* const>(
                synthetic.d3d12_resources.data(),
                synthetic.d3d12_resources.size()),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            // These are the layer's own private swapchains, so the layer
            // picks the state they rest in. On a private synthesis queue
            // that has to be COMMON: D3D12 requires a non-simultaneous-
            // access texture to be in COMMON at the point queue ownership
            // transfers, and these transfer twice a pair - written by the
            // synthesis queue, read by the runtime against the queue the
            // application supplied. synchronize_consumer_queue orders the
            // two; it does not transfer ownership, and a fence alone leaves
            // what the reader sees undefined. Observed as synthetic frames
            // that were generated, paced and complete on time, and still
            // did not reach the headset: The Callisto Protocol under UEVR
            // held 45 with 1257 pairs, 99.6% of submissions on grid and the
            // synthetic marker flashing a few times a second rather than
            // forty-five.
            //
            // Resting in COMMON costs nothing. Every transition in the
            // synthesis lists already starts and ends at this state, and a
            // resource in COMMON is implicitly promoted on first use, so a
            // runtime that wants it as a render target still gets one.
            (generation->staged_outputs || state->session->d3d12_synthesis_queue)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            state->session->nvidia_options,
            xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
            state->session->frame_generation_mode,
            (state->session->graphics_binding_capabilities & 8ULL) != 0, state->session->afw_only);
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
            destroy_current_ring(dispatch, &synthetic);
            destroy_current_ring(dispatch, &current);
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthesizer, handle_value(state->handle));
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        if(generation->staged_outputs) {
            generation->current_images_per_slot=1;
            generation->synthetic_images_per_slot=generation->staging_stride;
        }
        generation->synthesizer = std::move(synthesizer);
        if (failure_reason != nullptr) {
            *failure_reason = SwapchainEligibilityReason::ready;
        }
        return generation;
    } catch (...) {
        if (state && state->session && state->session->dispatch &&
            state->session->dispatch->destroy_swapchain != nullptr) {
            destroy_current_ring(state->session->dispatch, &synthetic);
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
    CreatedCurrentRing synthetic;
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
            destroy_current_ring(dispatch, &synthetic);
            destroy_current_ring(dispatch, &current);
        };

        XrSwapchainCreateInfo private_info = state->create_info;
        private_info.next = nullptr;
        private_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                   XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        // Interop synthesis and publication write only level zero. Keeping
        // the game's unused lower mips multiplies storage and prevents the
        // private output from using its existing D12 shared backing directly.
        private_info.mipCount=1;
        XrResult refusal = XR_SUCCESS;
        if (!create_current_ring(state, private_info, &current, &refusal,
                state->session->luke_afw_enabled ? kCurrentSlotCount + 1 : kCurrentSlotCount)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::current_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }
        log_video_memory(*state->session, VideoMemoryStage::current_ring, handle_value(state->handle));
        if (!create_current_ring(state, private_info, &synthetic, &refusal)) {
            if (failure_reason != nullptr) {
                *failure_reason =
                    SwapchainEligibilityReason::synthetic_private_swapchain_failed;
            }
            if (failure_detail != nullptr) {
                *failure_detail = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(refusal));
            }
            destroy_private();
            return nullptr;
        }

        log_video_memory(*state->session, VideoMemoryStage::synthetic_ring, handle_value(state->handle));
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

        log_video_memory(*state->session, VideoMemoryStage::interop, handle_value(state->handle));
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

        log_video_memory(*state->session, VideoMemoryStage::history, handle_value(state->handle));
        initialize_luke_synthesis_queue(*session);
        bool isolated_luke = session->luke_synthesis_queue != nullptr;
        if (isolated_luke && FAILED(interop->configure_synthesis_queue(session->luke_synthesis_queue.Get()))) {
            isolated_luke = false;
        }
        auto* synthesis_queue = isolated_luke ? session->luke_synthesis_queue.Get() : session->d3d12_queue.Get();
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
            synthesis_queue,
            history,
            interop->current_destination_images(),
            interop->synthetic_destination_images(),
            static_cast<DXGI_FORMAT>(state->create_info.format),
            D3D12_RESOURCE_STATE_COMMON,
            backend,
            session->nvidia_options,
            xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
            session->luke_afw_enabled
                ? xrfg::D3D12FrameGenerationMode::ofxr
                : session->frame_generation_mode,
            (session->graphics_binding_capabilities & 8ULL) != 0, session->luke_afw_enabled);
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

        log_video_memory(*state->session, VideoMemoryStage::synthesizer, handle_value(state->handle));
        auto generation = std::make_shared<FrameGenerationSwapchainState>();
        generation->synthesizer = std::move(synthesizer);
        generation->d3d11_interop = std::move(interop);
        generation->isolated_luke_synthesis = isolated_luke;
        if (session->luke_afw_enabled && !session->afw_only && !isolated_luke) {
            generation->shared_queue_owner=session;
            ++session->luke_shared_generation_count;
        }
        if (isolated_luke) {
            // Copy-only ingress helper. No OFA initialization or extra XR images.
            // Its scratch copy must precede marker readback/AFW on the capture lane.
            auto source = std::make_shared<xrfg::D3D12FrameSynthesizer>();
            gpu_result = source->initialize(session->d3d12_device.Get(), session->d3d12_queue.Get(),
                history, generation->d3d11_interop->current_destination_images(),
                generation->d3d11_interop->synthetic_destination_images(),
                static_cast<DXGI_FORMAT>(state->create_info.format), D3D12_RESOURCE_STATE_COMMON,
                backend, session->nvidia_options, false, xrfg::D3D12FrameGenerationMode::ofxr, true, true);
            if (FAILED(gpu_result)) {
                if (failure_reason) *failure_reason = SwapchainEligibilityReason::synthesis_initialize_failed;
                if (failure_detail) *failure_detail = static_cast<std::uint64_t>(gpu_result);
                destroy_private();
                return nullptr;
            }
            generation->afw_source_synthesizer = std::move(source);
        }
        adopt_current_ring(generation, current);
        adopt_synthetic_ring(generation, synthetic);
        if (session->luke_afw_enabled && !session->afw_only) {
            auto final_history =
                std::make_shared<xrfg::D3D12SwapchainHistory>();
            auto temporal =
                std::make_shared<xrfg::D3D12FrameSynthesizer>();
            const HRESULT history_result = final_history->initialize(
                session->d3d12_device.Get(), session->d3d12_queue.Get(),
                generation->d3d11_interop->current_destination_images(),
                D3D12_RESOURCE_STATE_COMMON);
            const HRESULT temporal_result = SUCCEEDED(history_result)
                ? temporal->initialize(
                    session->d3d12_device.Get(), synthesis_queue,
                    final_history,
                    generation->d3d11_interop->current_destination_images(),
                    generation->d3d11_interop->synthetic_destination_images(),
                    static_cast<DXGI_FORMAT>(state->create_info.format),
                    D3D12_RESOURCE_STATE_COMMON, backend,
                    session->nvidia_options,
                    xrfg::bridge_flight_logger().gpu_instrumentation_enabled(),
                    xrfg::D3D12FrameGenerationMode::ofxr, true, true)
                : history_result;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                temporal_result, 50,
                handle_value(state->handle), 0);
            if (SUCCEEDED(temporal_result)) {
                generation->afw_final_history = std::move(final_history);
                generation->afw_temporal_synthesizer = std::move(temporal);
            }
        }
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
            destroy_current_ring(state->session->dispatch, &synthetic);
            destroy_current_ring(state->session->dispatch, &current);
        }
        return nullptr;
    }
}

// The max-generated-frames query initializes NGX; it is not a passive hardware
// read. A cold Luke D11 bridge must let the game's SR/RR initialization happen
// first, before probing on its confirmed original D12 device.
void refresh_native_generation_capacity(SessionState& state) noexcept {
    if (!state.d3d12_device) return;
    if (state.luke_afw_enabled && state.graphics_binding==SessionGraphicsBinding::d3d11 &&
        !(state.graphics_binding_capabilities&8ULL)) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            S_FALSE,1003,handle_value(state.handle),0);
        return;
    }
    state.native_dlss_generated_frames=xrfg::native_dlssg_max_generated_frames(state.d3d12_device.Get());
}

// Optical flow needs the application's XR colors, not AFW's native guides.
// Keep native FSR/DLSS-G admission and the AFW-only startup contract unchanged.
[[nodiscard]] bool luke_color_only_ofxr(const SessionState& state) noexcept {
    return state.luke_afw_enabled && state.menu_enabled &&
        state.frame_generation_mode == xrfg::D3D12FrameGenerationMode::ofxr;
}

// Standalone has no OptiScaler State::currentCommandQueue at XR creation.
// The original SR/RR list submission supplies it later. Called under
// frame_call_mutex only between application frames, before private allocation.
void initialize_late_luke_context(const std::shared_ptr<SessionState>& session) noexcept {
    if(!session || !session->luke_afw_enabled || session->graphics_binding!=SessionGraphicsBinding::d3d11 ||
        session->application_frame_in_progress)return;
    const bool native_ready=(session->graphics_binding_capabilities&8ULL)!=0;
    const bool afw_needed=xrfg::embedded::afw_snapshot().desired.mode!=0;
    if(native_ready && (!afw_needed || session->luke_afw))return;
    // A sealed Outlaws session cannot discover a different owner or allocate
    // new XR outputs. A provider retired while OFF may resume on the already
    // verified owner and execution queue without changing those warm images.
    if(session->luke_outlaws_preframe&&session->luke_outlaws_frame_started&&!native_ready)return;
    try {
        if(!session->luke_vulkan_device)
            session->luke_vulkan_device=xrfg::native_vulkan::configure_luke_owner(session->d3d12_device.Get(),session->d3d12_queue.Get());
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if(!query_luke_ross_d3d12_context(device.GetAddressOf(),queue.GetAddressOf()))return;
        if(native_ready && (!d3d_owner::same(device.Get(),session->d3d12_device.Get()) ||
            !session->d3d12_queue))return;
        if(session->luke_context_failed_queue==queue.Get())return;
        auto chains=find_swapchains(session);
        bool active_history=false;
        for(const auto& chain:chains) {
            std::scoped_lock lock(chain->mutex);
            active_history|=bool(chain->frame_generation)||bool(chain->d3d12_history);
        }
        // Existing OFXR histories retain their owner. AFW can join that queue
        // only on the exact same native device; its guide carries the producer
        // fence. A different device cannot steal active OFXR resources.
        if(active_history && !d3d_owner::same(device.Get(),session->d3d12_device.Get()))return;
        auto execution_queue=session->d3d12_queue;
        if (!active_history && !native_ready) {
            Microsoft::WRL::ComPtr<ID3D12Device> native_device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> native_producer;
            const bool native_luke=xrfg::native_ingress::query_luke_context(
                native_device.GetAddressOf(),native_producer.GetAddressOf());
            const auto queue_result=select_luke_execution_queue(native_luke,
                device.Get(),queue.Get(),execution_queue);
            if(FAILED(queue_result)){session->luke_context_failed_queue=queue.Get();return;}
        }
        std::shared_ptr<xrfg::LukeAfwSynthesizer> afw;
        if(afw_needed) {
            afw=std::make_shared<xrfg::LukeAfwSynthesizer>();
            const auto result=afw->initialize(device.Get(),execution_queue.Get(),luke_bootstrap_provider());
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::synthesis_initialize,
                result,0xAF023245ULL,session->luke_afw_odd_serial_is_left?1ULL:0ULL,1);
            if(FAILED(result)){session->luke_context_failed_queue=queue.Get();return;}
        }
        // Never move active GPU histories across owners or queues.
        std::vector<std::unique_lock<std::mutex>> locks;
        for(const auto& chain:chains)locks.emplace_back(chain->call_mutex);
        std::scoped_lock gpu_lock(session->gpu_mutex);
        auto previous_queue=session->d3d12_queue;
        if(!active_history && !native_ready) {
            session->d3d12_queue=std::move(execution_queue);
        }
        // Active color history may retain a REAL VR device alias. The backing
        // challenge above proves the same owner; keep its queue/resources but
        // use the confirmed native allocation identity for strict AFW guides.
        if (!active_history && !native_ready && session->luke_synthesis_queue) {
            // The old device may be a verified REAL VR alias of the native
            // allocation device, while its queue still reports that alias.
            // Synthesis requires exact COM ownership, not shared backing.
            Microsoft::WRL::ComPtr<IUnknown> target_identity, queue_identity;
            Microsoft::WRL::ComPtr<ID3D12Device> queue_owner;
            const bool compatible = SUCCEEDED(device.As(&target_identity)) &&
                SUCCEEDED(session->luke_synthesis_queue->GetDevice(IID_PPV_ARGS(&queue_owner))) &&
                SUCCEEDED(queue_owner.As(&queue_identity)) && target_identity == queue_identity;
            if (!compatible) {
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    0,959,handle_value(session->luke_synthesis_queue.Get()),handle_value(device.Get()));
                session->luke_synthesis_queue.Reset();
            }
        }
        session->d3d12_device=std::move(device);
        if(auto monitor=xrfg::neural_rendering::observe_device_removal(session->d3d12_device.Get()))
            session->gpu_crash_monitors.push_back(std::move(monitor));
        session->luke_afw=std::move(afw);
        session->graphics_binding_capabilities=(session->graphics_binding_capabilities&~4ULL)|8ULL;
        initialize_luke_synthesis_queue(*session);
        refresh_native_generation_capacity(*session);
        if(previous_queue.Get()!=session->d3d12_queue.Get())xrfg::stop_ngx_guide_capture(previous_queue.Get());
        xrfg::register_d3d11_guide_owner(reinterpret_cast<std::uintptr_t>(session.get()),
            session->d3d11_device.Get(),session->d3d12_device.Get(),session->d3d12_queue.Get());
        xrfg::configure_ngx_guide_capture(session->d3d12_queue.Get(),true);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            0,951,handle_value(session->d3d12_device.Get()),handle_value(session->d3d12_queue.Get()));
    }catch(...) {}
}

// A refused private swapchain is the only failure that says anything about the
// rest of the session: whatever ceiling the runtime reached, the application's
// own creations are competing for it. Synthesis, interop and an unusable image
// are local to the one swapchain that hit them.
[[nodiscard]] bool budget_refusal(SwapchainEligibilityReason reason) noexcept {
    return reason ==
               SwapchainEligibilityReason::current_private_swapchain_failed ||
           reason ==
               SwapchainEligibilityReason::synthetic_private_swapchain_failed;
}

// Hands every private swapchain in the session back to the runtime.
//
// The caller must own the frame call mutex and, when a presenter is running,
// have drained it and taken the content lock first: a queued submission names
// these swapchains, and destroying one the runtime still has queued latches a
// presenter failure for the rest of the session.
void release_session_generation_budget(
    const std::shared_ptr<SessionState>& session) noexcept {
    try {
        if (!session) {
            return;
        }
        {
            // The retained repeat names private swapchains of its own.
            std::scoped_lock lock(session->presenter_mutex);
            session->presenter_last_frame.reset();
        }
        for (const auto& swapchain : find_swapchains(session)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            swapchain->generation_eligible_pending = false;
            swapchain->generation_declined = true;
            swapchain->generation_refusal = SwapchainEligibilityReason::budget_exhausted;
            swapchain->generation_refusal_detail = 0;
            drain_swapchain_gpu(swapchain);
            destroy_frame_generation_swapchains(swapchain);
        }
    } catch (...) {
    }
}

// Creates the generation resources a swapchain deferred at enumeration time.
// Returns false only when the runtime refused a private swapchain, which is
// the caller's signal to hand the session's whole budget back.
[[nodiscard]] bool ensure_frame_generation(
    const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (!state || !state->session) {
            return true;
        }
        std::scoped_lock call_lock(state->call_mutex);
        if(state->session->luke_afw_enabled && state->session->graphics_binding==SessionGraphicsBinding::d3d11 &&
            (state->session->graphics_binding_capabilities&8ULL)==0 &&
            !luke_color_only_ofxr(*state->session))return true;
        if (!state->generation_eligible_pending || state->generation_declined) {
            return true;
        }
        {
            std::scoped_lock lock(state->mutex);
            if (state->frame_generation) {
                state->generation_eligible_pending = false;
                return true;
            }
        }
        if(state->session->luke_outlaws_preframe&&state->session->luke_outlaws_frame_started)
            return true; // Missing startup preparation remains original real-frame bypass.
        const std::uint64_t auxiliary =
            (static_cast<std::uint64_t>(state->enumerated_image_count) << 32) |
            state->create_info.arraySize;
        SwapchainEligibilityReason reason =
            SwapchainEligibilityReason::exception;
        std::uint64_t detail = 0;
        // Enumeration ran before the application had submitted anything, so
        // nothing else was touching the queue. This runs on the frame path,
        // where a release on another thread may be capturing into history and
        // synthesizer initialisation submits work of its own.
        std::shared_ptr<FrameGenerationSwapchainState> candidate;
        {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            if (state->session->graphics_binding ==
                SessionGraphicsBinding::d3d11) {
                // Ask the runtime for the images again rather than holding a
                // reference to each of them from enumeration until whenever
                // the application first composites with this swapchain. Those
                // are the application's textures: keeping them alive here
                // outlives what the layer is entitled to hold, and an
                // application that exits without destroying its swapchains
                // then releases them during teardown, which hung the D3D11
                // call chain tests at process exit. xrEnumerateSwapchainImages
                // may be called as often as we like.
                std::vector<XrSwapchainImageD3D11KHR> enumerated(
                    state->enumerated_image_count,
                    XrSwapchainImageD3D11KHR{
                        XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, nullptr});
                std::uint32_t count = 0;
                const XrResult enumerate_result =
                    state->session->dispatch->enumerate_swapchain_images(
                        state->handle,
                        state->enumerated_image_count,
                        &count,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(
                            enumerated.data()));
                std::vector<ID3D11Texture2D*> images;
                if (XR_SUCCEEDED(enumerate_result) &&
                    count == state->enumerated_image_count) {
                    images.reserve(count);
                    for (const XrSwapchainImageD3D11KHR& image : enumerated) {
                        images.push_back(image.texture);
                    }
                }
                if (images.empty()) {
                    reason = SwapchainEligibilityReason::invalid_d3d11_image;
                    detail = static_cast<std::uint64_t>(
                        static_cast<std::int64_t>(enumerate_result));
                } else {
                    candidate = create_d3d11_frame_generation_swapchains(
                        state,
                        std::span<ID3D11Texture2D* const>(
                            images.data(), images.size()),
                        &reason,
                        &detail);
                }
            } else {
                candidate = create_d3d12_frame_generation_swapchains(
                    state, &reason, &detail);

            }
        }
        if (!candidate) {
            state->generation_declined = true;
            state->generation_refusal = reason;
            state->generation_refusal_detail = detail;
            if (reason == SwapchainEligibilityReason::synthesis_initialize_failed)
                state->session->generation_initialization_result = static_cast<HRESULT>(detail);
            log_swapchain_eligibility(state, reason, detail, auxiliary);
            return !budget_refusal(reason);
        }
        {
            std::scoped_lock lock(state->mutex);
            state->frame_generation = std::move(candidate);
        }
        state->generation_eligible_pending = false;
        state->generation_refusal = SwapchainEligibilityReason::ready;
        state->generation_refusal_detail = 0;
        log_swapchain_eligibility(
            state,
            SwapchainEligibilityReason::ready,
            state->enumerated_image_count,
            auxiliary);
        return true;
    } catch (...) {
        return true;
    }
}

// Spends the generation budget on the swapchains this frame actually submits
// as projection views. Returns false once the runtime has refused one, meaning
// the caller must release the session's budget and stop generating.
[[nodiscard]] bool ensure_projection_frame_generation(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    if (!session ||
        session->generation_budget_exhausted.load(std::memory_order_acquire)) {
        return true;
    }
    // The arming caller has drained old submissions and released its retained
    // repeat. Retire older projection output sets before admitting the new
    // set (loading/menu -> level), even without a settings revision. AER eye
    // identities can alternate, so only auxiliary-only AER sources are proven
    // unused; their temporal eye resources remain admitted.
    if (projection_owned_outputs(*session) && !mappings.empty()) {
        for (const auto& chain:find_swapchains(session)) {
            const bool projected=std::any_of(mappings.begin(),mappings.end(),[&](const auto& mapping) {
                return mapping.application_swapchain==chain->handle;
            });
            if (projected) {
                chain->projection_history_needed.store(true,std::memory_order_release);
                continue;
            }
            std::scoped_lock call_lock(chain->call_mutex);
            bool unused{},had_generation{};
            { std::scoped_lock lock(chain->mutex);
              had_generation=bool(chain->frame_generation);
              unused=!session->luke_afw_enabled || (chain->diagnostic_layer_roles&3)==2; }
            if (!unused || (!had_generation && !chain->projection_history_needed.load(std::memory_order_acquire)))continue;
            if(had_generation) {
                drain_swapchain_gpu(chain);
                destroy_frame_generation_swapchains(chain);
            }
            std::scoped_lock lock(chain->mutex);
            // A UI/loading source can accumulate full-size history without
            // ever owning a generator. Stop those copies too; preserve the
            // application's images and keep metadata for a future role change.
            if(chain->d3d12_history && FAILED(chain->d3d12_history->release_snapshots()))continue;
            chain->last_released_capture.reset();
            chain->last_released_motion_vectors.reset();
            chain->projection_history_needed.store(false,std::memory_order_release);
            if(session->graphics_binding==SessionGraphicsBinding::d3d11 &&
                !(session->luke_outlaws_preframe && session->luke_outlaws_frame_started))
                chain->d3d12_history.reset();
            if(had_generation) {
                chain->generation_eligible_pending=true;
                chain->generation_declined=false;
                chain->generation_refusal=SwapchainEligibilityReason::ready;
                chain->generation_refusal_detail=0;
                log_swapchain_eligibility(chain,SwapchainEligibilityReason::awaiting_projection_use);
            }
        }
    }
    for (const ProjectionResourceMapping& mapping : mappings) {
        const auto swapchain = find_swapchain(mapping.application_swapchain);
        // The independent private generations, not a UEVR module identity,
        // protect queued pixels. Only their final native destination is shared
        // by exact source/shape/state. Array slices and packed rectangles stay
        // in the original resource domain; other sources never join this pool.
        if(swapchain)swapchain->pooled_publication=session->private_runtime && !session->luke_afw_enabled;
        if(swapchain && session->luke_afw_enabled && swapchain->create_info.arraySize==1)
            xrfg::configure_luke_color_origin_extent(swapchain->create_info.width,swapchain->create_info.height);
        if (!swapchain || ensure_frame_generation(swapchain)) {
            continue;
        }
        session->generation_budget_exhausted.store(
            true, std::memory_order_release);
        log_swapchain_eligibility(
            swapchain,
            SwapchainEligibilityReason::budget_exhausted,
            0,
            handle_value(session->handle));
        return false;
    }
    return true;
}

void seal_outlaws_initialization(SessionState& state) noexcept {
    if(!state.luke_outlaws_preframe||state.luke_outlaws_frame_started)return;
    state.luke_outlaws_frame_started=true;
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        (state.graphics_binding_capabilities&8ULL)?S_OK:HRESULT_FROM_WIN32(ERROR_NOT_READY),
        1000,handle_value(state.handle),state.luke_outlaws_eager_budget);
}

// Whether any swapchain this frame submits as a projection view still owes the
// work ensure_projection_frame_generation would do. Cheap: two locks per
// mapping and no allocation, and it answers true at most once per swapchain
// until an explicit provider reconfiguration reopens a refused attempt.
[[nodiscard]] bool projection_frame_generation_pending(
    const std::shared_ptr<SessionState>& session,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        if (!session || session->generation_budget_exhausted.load(
                            std::memory_order_acquire)) {
            return false;
        }
        for (const ProjectionResourceMapping& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            std::scoped_lock call_lock(swapchain->call_mutex);
            if (swapchain->generation_eligible_pending &&
                !swapchain->generation_declined) {
                return true;
            }
        }
        return false;
    } catch (...) {
        return false;
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
XRAPI_ATTR XrResult XRAPI_CALL layer_suggest_interaction_profile_bindings(
    XrInstance instance,
    const XrInteractionProfileSuggestedBinding* suggested_bindings);
XRAPI_ATTR XrResult XRAPI_CALL layer_attach_session_action_sets(
    XrSession session,
    const XrSessionActionSetsAttachInfo* attach_info);
XRAPI_ATTR XrResult XRAPI_CALL layer_sync_actions(
    XrSession session,
    const XrActionsSyncInfo* sync_info);
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
XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data);
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

XRAPI_ATTR XrResult XRAPI_CALL layer_set_presentation_transform_ofxr(
    XrSession session,
    const XrfgPresentationTransformV1* transform) {
    if (transform == nullptr ||
        transform->version != XRFG_PRESENTATION_TRANSFORM_VERSION_1 ||
        transform->view_count > 2U ||
        (transform->active != 0U && transform->view_count == 0U)) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto state = find_session(session);
    if (!state) return XR_ERROR_HANDLE_INVALID;
    std::scoped_lock lock(state->frame_call_mutex);
    state->presentation_transform = {};
    state->presentation_transform.active = transform->active != 0U;
    state->presentation_transform.view_count = transform->view_count;
    for (std::uint32_t view = 0; view < transform->view_count; ++view) {
        std::copy_n(
            transform->clip_positions[view],
            state->presentation_transform.clip_positions[view].size(),
            state->presentation_transform.clip_positions[view].begin());
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_views(
    XrSession session, const XrViewLocateInfo* info, XrViewState* view_state,
    std::uint32_t capacity, std::uint32_t* count, XrView* views) {
    try {
        const auto state = find_session(session);
        if (!state || !state->dispatch->locate_views) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->locate_views(
            session, info, view_state, capacity, count, views);
        if (XR_SUCCEEDED(result) && info && views && count && *count && capacity &&
            view_state && (view_state->viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            blackbar_record_locate(*state,*info,views[0].pose);
        if (XR_SUCCEEDED(result) && info &&
            info->viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO &&
            view_state && count && *count == 2 &&
            capacity >= 2 && views &&
            (state->graphics_binding_capabilities & 8ULL) != 0 &&
            (view_state->viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0) {
            float baseline{};
            if (xrfg::luke_xr_eye_baseline(
                    {views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z},
                    {views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z},
                    baseline)) {
                std::scoped_lock lock(state->mutex);
                state->luke_xr_eye_baseline = baseline;
            }
        }
        if (info && (state->graphics_binding_capabilities & 8ULL) != 0 &&
            xrfg::bridge_flight_logger().ram_enabled()) {
            std::uint64_t serial{};
            {
                std::scoped_lock lock(state->mutex);
                serial = state->next_application_frame_serial - 1;
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_locate_context,
                result, serial, static_cast<std::uint64_t>(info->displayTime),
                handle_value(info->space));
            if (XR_SUCCEEDED(result) && count && views) {
                for (std::uint32_t index = 0; index < std::min({*count, capacity, 8u}); ++index) {
                    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    view.pose = views[index].pose;
                    view.fov = views[index].fov;
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_locate_pose,
                        index, serial, static_cast<std::uint64_t>(info->displayTime),
                        projection_pose_hash(view));
                    log_luke_view_bits(serial, 900 + index, view);
                }
            }
        }
        return result;
    } catch (...) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
}

void observe_created_space(const std::shared_ptr<SessionState>& state,
    XrSpace space, std::int64_t type, const XrPosef& offset) {
    { std::scoped_lock lock(g_state_mutex);
      g_observed_spaces[space] = {state->dispatch, state}; }
    if ((state->graphics_binding_capabilities & 8ULL) != 0 &&
        xrfg::bridge_flight_logger().ram_enabled()) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_created,
            type, handle_value(space), handle_value(state->handle), 0);
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.pose = offset;
        log_luke_view_bits(handle_value(space), 960, view);
    }
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_reference_space(XrSession session,
    const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || !state->dispatch->create_reference_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->create_reference_space(session, info, space);
        if (XR_SUCCEEDED(result) && info && space) {
            // Observation failure must not change a successful runtime call.
            try { observe_created_space(state, *space, info->referenceSpaceType,
                info->poseInReferenceSpace); } catch (...) {}
        }
        return result;
    });
}

template <typename Function>
[[nodiscard]] Function panel_gesture_next(Function Dispatch::PanelGesture::*member) {
    std::scoped_lock lock(g_state_mutex);
    if(g_instances.size()!=1)return nullptr;
    for (const auto& [handle, dispatch] : g_instances) {
        static_cast<void>(handle);
        if (dispatch && dispatch->panel_gesture.*member != nullptr) {
            return dispatch->panel_gesture.*member;
        }
    }
    return nullptr;
}

// The status panel's grip bindings, beside the application's own for every
// interaction profile it suggests (see create_panel_gesture). A profile with
// no grip - a gamepad, eye gaze - refuses them, and then the application's
// call is made again exactly as it made it, so its own bindings never depend
// on the layer's being accepted.
XRAPI_ATTR XrResult XRAPI_CALL layer_suggest_interaction_profile_bindings(
    XrInstance instance,
    const XrInteractionProfileSuggestedBinding* suggested_bindings) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->panel_gesture.suggest_bindings == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = dispatch->panel_gesture;
        // A call the runtime should refuse is passed on as it came, so the
        // layer's additions never make an invalid one valid.
        if (suggested_bindings == nullptr ||
            suggested_bindings->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING ||
            suggested_bindings->countSuggestedBindings == 0 ||
            suggested_bindings->suggestedBindings == nullptr) {
            return gesture.suggest_bindings(instance, suggested_bindings);
        }
        std::vector<XrActionSuggestedBinding> bindings(
            suggested_bindings->suggestedBindings,
            suggested_bindings->suggestedBindings + suggested_bindings->countSuggestedBindings);
        bindings.push_back({gesture.grip_action, gesture.grips[0]});
        bindings.push_back({gesture.grip_action, gesture.grips[1]});
        XrInteractionProfileSuggestedBinding merged = *suggested_bindings;
        merged.suggestedBindings = bindings.data();
        merged.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        XrResult result = gesture.suggest_bindings(instance, &merged);
        const bool added = XR_SUCCEEDED(result);
        if (!added) {
            result = gesture.suggest_bindings(instance, suggested_bindings);
        }
        return result;
    });
}

// The layer's action set beside the application's. Taken, it brings the two
// grip spaces the panel's gesture reads; refused, the application's sets are
// attached as it asked, and the session simply has no gesture.
XRAPI_ATTR XrResult XRAPI_CALL layer_attach_session_action_sets(
    XrSession session,
    const XrSessionActionSetsAttachInfo* attach_info) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->panel_gesture.attach_action_sets == nullptr) {
            const auto next = panel_gesture_next(&Dispatch::PanelGesture::attach_action_sets);
            return next ? next(session, attach_info) : XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = state->dispatch->panel_gesture;
        if (attach_info == nullptr ||
            attach_info->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO ||
            attach_info->countActionSets == 0 || attach_info->actionSets == nullptr) {
            return gesture.attach_action_sets(session, attach_info);
        }
        std::vector<XrActionSet> action_sets(
            attach_info->actionSets, attach_info->actionSets + attach_info->countActionSets);
        action_sets.push_back(gesture.action_set);
        XrSessionActionSetsAttachInfo merged = *attach_info;
        merged.actionSets = action_sets.data();
        merged.countActionSets = static_cast<std::uint32_t>(action_sets.size());
        XrResult result = gesture.attach_action_sets(session, &merged);
        if (XR_FAILED(result)) {
            result = gesture.attach_action_sets(session, attach_info);
                return result;
        }
        std::array<XrSpace, 2> grips{XR_NULL_HANDLE, XR_NULL_HANDLE};
        std::uint64_t made = 0;
        for (std::size_t hand = 0; hand < grips.size(); ++hand) {
            XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            space_info.action = gesture.grip_action;
            space_info.subactionPath = gesture.hands[hand];
            space_info.poseInActionSpace.orientation.w = 1.0F;
            if (XR_SUCCEEDED(gesture.create_action_space(session, &space_info, &grips[hand]))) {
                ++made;
            } else {
                grips[hand] = XR_NULL_HANDLE;
            }
        }
        state->panel_grip_spaces = grips;
        state->panel_actions_attached.store(true, std::memory_order_release);
        if (state->fps_overlay) {
            state->fps_overlay->set_grip_spaces(grips[0], grips[1]);
        }
        return result;
    });
}

// The layer's action set is synced with the application's, once it was
// attached. Should the runtime refuse the addition, the application's sync is
// made again as it asked.
XRAPI_ATTR XrResult XRAPI_CALL layer_sync_actions(
    XrSession session,
    const XrActionsSyncInfo* sync_info) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->panel_gesture.sync_actions == nullptr) {
            const auto next = panel_gesture_next(&Dispatch::PanelGesture::sync_actions);
            return next ? next(session, sync_info) : XR_ERROR_HANDLE_INVALID;
        }
        const auto& gesture = state->dispatch->panel_gesture;
        if (!state->panel_actions_attached.load(std::memory_order_acquire) ||
            sync_info == nullptr || sync_info->type != XR_TYPE_ACTIONS_SYNC_INFO ||
            (sync_info->countActiveActionSets != 0 && sync_info->activeActionSets == nullptr)) {
            return gesture.sync_actions(session, sync_info);
        }
        if(xrfg::autonomous::snapshot().desired.overlay_position!=5)return gesture.sync_actions(session,sync_info);
        std::vector<XrActiveActionSet> active;
        if(sync_info->countActiveActionSets)active.assign(sync_info->activeActionSets,sync_info->activeActionSets+sync_info->countActiveActionSets);
        active.push_back({gesture.action_set, XR_NULL_PATH});
        XrActionsSyncInfo merged = *sync_info;
        merged.activeActionSets = active.data();
        merged.countActiveActionSets = static_cast<std::uint32_t>(active.size());
        const XrResult result = gesture.sync_actions(session, &merged);
        return XR_FAILED(result) ? gesture.sync_actions(session, sync_info) : result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_action_space(XrSession session,
    const XrActionSpaceCreateInfo* info, XrSpace* space) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || !state->dispatch->create_action_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = state->dispatch->create_action_space(session, info, space);
        if (XR_SUCCEEDED(result) && info && space) {
            try { observe_created_space(state, *space, -1, info->poseInActionSpace); } catch (...) {}
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_locate_space(XrSpace space, XrSpace base,
    XrTime time, XrSpaceLocation* location) {
    return guard_c_api_boundary([&]() -> XrResult {
        ObservedSpace owner;
        {
            std::scoped_lock lock(g_state_mutex);
            if (const auto it = g_observed_spaces.find(space); it != g_observed_spaces.end())
                owner = it->second;
            // Preserve passthrough for unobserved/invalid handles in a sole
            // runtime, without guessing an instance in multi-instance use.
            else if (g_instances.size() == 1) owner.dispatch = g_instances.begin()->second;
        }
        if (!owner.dispatch || !owner.dispatch->locate_space) return XR_ERROR_HANDLE_INVALID;
        const auto result = owner.dispatch->locate_space(space, base, time, location);
        const auto state = owner.session.lock();
        if (state && (state->graphics_binding_capabilities & 8ULL) != 0 &&
            xrfg::bridge_flight_logger().ram_enabled()) {
            const auto id = g_space_sample_id.fetch_add(1, std::memory_order_relaxed);
            std::uint64_t latest_wait{};
            { std::scoped_lock lock(state->mutex);
              latest_wait = state->next_application_frame_serial - 1; }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_locate,
                result, id, handle_value(space), handle_value(base));
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_space_time,
                XR_SUCCEEDED(result) && location ? location->locationFlags : 0,
                id, static_cast<std::uint64_t>(time), latest_wait);
            if (XR_SUCCEEDED(result) && location) {
                XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose = location->pose;
                log_luke_view_bits(id, 950, view);
            }
        }
        return result;
    });
}

// Djules 3096ddcf (V334), adapted to the embedded DLL: only loader discovery
// is needed here; do not bring in standalone's OptiScaler delegation/tray.
[[nodiscard]] FARPROC loader_export(HMODULE module, const char* name) noexcept {
    __try {
        return GetProcAddress(module, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

[[nodiscard]] bool enumerate_loader_modules(
    HMODULE* modules, DWORD capacity, DWORD* bytes) noexcept {
    __try {
        return EnumProcessModules(GetCurrentProcess(), modules, capacity, bytes) != FALSE;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

[[nodiscard]] PFN_xrGetInstanceProcAddr find_loader_get_instance_proc_addr() {
    const auto loader_entry = [](HMODULE module) noexcept -> PFN_xrGetInstanceProcAddr {
        if (module == nullptr ||
            loader_export(module, "xrNegotiateLoaderApiLayerInterface") != nullptr ||
            loader_export(module, "xrNegotiateLoaderRuntimeInterface") != nullptr ||
            loader_export(module, "xrEnumerateApiLayerProperties") == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<PFN_xrGetInstanceProcAddr>(
            loader_export(module, "xrGetInstanceProcAddr"));
    };
    if (const auto entry = loader_entry(GetModuleHandleW(L"openxr_loader.dll"))) {
        return entry;
    }
    std::vector<HMODULE> modules(256);
    DWORD bytes = 0;
    if (!enumerate_loader_modules(
            modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
        return nullptr;
    }
    if (bytes > modules.size() * sizeof(HMODULE)) {
        modules.resize(bytes / sizeof(HMODULE));
        if (!enumerate_loader_modules(
                modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes)) {
            return nullptr;
        }
    }
    const std::size_t count = std::min(modules.size(), static_cast<std::size_t>(bytes / sizeof(HMODULE)));
    for (std::size_t index = 0; index < count; ++index) {
        if (const auto entry = loader_entry(modules[index])) {
            return entry;
        }
    }
    return nullptr;
}

// Before an instance exists there is no next-layer dispatch. Upper layers
// (e.g. Cheeky Foveated DLSS) still need the loader's global extension query.
// Never expose instance functions or mistake this embedded DLL for a loader.
XrResult resolve_global_function(const char* name, PFN_xrVoidFunction* function) {
    if (std::strcmp(name, "xrEnumerateInstanceExtensionProperties") != 0 &&
        std::strcmp(name, "xrEnumerateApiLayerProperties") != 0) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const PFN_xrGetInstanceProcAddr loader = find_loader_get_instance_proc_addr();
    if (loader == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = loader(XR_NULL_HANDLE, name, function);
    if (XR_FAILED(result) || *function == nullptr) {
        *function = nullptr;
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    return XR_SUCCESS;
}

[[nodiscard]] std::string augment_vulkan_extension_list(
    std::string_view list,
    bool device_list,
    std::uint64_t* appended_bits) {
    std::string augmented(list);
    const VulkanExtensionSummary present =
        summarize_vulkan_extension_string(list);
    const auto append = [&](std::string_view name) {
        const std::uint64_t bit = vulkan_interop_extension_bit(name);
        if ((present.bits & bit) != 0) {
            return;
        }
        if (!augmented.empty()) {
            augmented.push_back(' ');
        }
        augmented.append(name);
        *appended_bits |= bit;
    };
    if (device_list) {
        append("VK_KHR_external_semaphore_win32");
    } else {
        append("VK_KHR_external_semaphore_capabilities");
    }
    return augmented;
}

XrResult get_vulkan_extensions_recorded(
    PFN_GetVulkanExtensions next,
    std::int64_t selector,
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (count == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    // Read the runtime's whole list whatever the application asked for, so
    // the size query and the fill call both answer for the augmented list.
    std::uint32_t runtime_count = 0;
    XrResult result = next(instance, system_id, 0, &runtime_count, nullptr);
    if (XR_FAILED(result) || runtime_count == 0) {
        return next(instance, system_id, capacity, count, buffer);
    }
    std::string runtime_list(runtime_count, '\0');
    result = next(
        instance, system_id, runtime_count, &runtime_count, runtime_list.data());
    if (XR_FAILED(result)) {
        return next(instance, system_id, capacity, count, buffer);
    }
    runtime_list.resize(runtime_count > 0 ? runtime_count - 1 : 0);
    std::uint64_t appended_bits = 0;
    const bool enabled = GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
        (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
    const std::string augmented = enabled
        ? augment_vulkan_extension_list(runtime_list, selector == 3, &appended_bits)
        : runtime_list;
    *count = static_cast<std::uint32_t>(augmented.size() + 1);
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (buffer == nullptr || capacity < *count) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, augmented.c_str(), augmented.size() + 1);
    // Only the call that fills the buffer is recorded; the size query before
    // it would record the same list twice.
    const VulkanExtensionSummary summary =
        summarize_vulkan_extension_string(runtime_list);
    log_vulkan_negotiation(
        selector, summary.count, summary.bits, runtime_list.size());
    log_vulkan_negotiation(10, static_cast<std::uint64_t>(selector), appended_bits);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_instance_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_instance_extensions : nullptr,
            2, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_device_extensions(
    XrInstance instance,
    XrSystemId system_id,
    std::uint32_t capacity,
    std::uint32_t* count,
    char* buffer) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_extensions_recorded(
            dispatch ? dispatch->vulkan.get_device_extensions : nullptr,
            3, instance, system_id, capacity, count, buffer);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_instance(
    XrInstance instance,
    const XrVulkanInstanceCreateInfoKHR* create_info,
    VkInstance* vulkan_instance,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_instance == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_instance(
            instance, create_info, vulkan_instance, vulkan_result);
        VulkanExtensionSummary summary{};
        if (create_info != nullptr && create_info->vulkanCreateInfo != nullptr) {
            summary = summarize_vulkan_extension_array(
                create_info->vulkanCreateInfo->ppEnabledExtensionNames,
                create_info->vulkanCreateInfo->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            4, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_create_vulkan_device(
    XrInstance instance,
    const XrVulkanDeviceCreateInfoKHR* create_info,
    VkDevice* vulkan_device,
    VkResult* vulkan_result) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.create_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->vulkan.create_device(
            instance, create_info, vulkan_device, vulkan_result);
        const VkDeviceCreateInfo* device_info =
            create_info != nullptr ? create_info->vulkanCreateInfo : nullptr;
        VulkanExtensionSummary summary{};
        if (device_info != nullptr) {
            summary = summarize_vulkan_extension_array(
                device_info->ppEnabledExtensionNames,
                device_info->enabledExtensionCount);
        }
        log_vulkan_negotiation(
            5, summary.count, summary.bits,
            pack_negotiation_results(
                result, vulkan_result != nullptr ? *vulkan_result : VK_SUCCESS));
        if (device_info != nullptr && device_info->pQueueCreateInfos != nullptr) {
            for (std::uint32_t index = 0;
                 index < device_info->queueCreateInfoCount;
                 ++index) {
                const auto& queue = device_info->pQueueCreateInfos[index];
                log_vulkan_negotiation(
                    6, queue.queueFamilyIndex, queue.queueCount);
            }
        }
        return result;
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device(
    XrInstance instance,
    XrSystemId system_id,
    VkInstance vulkan_instance,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 1);
        return dispatch->vulkan.get_graphics_device(
            instance, system_id, vulkan_instance, physical_device);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_device2(
    XrInstance instance,
    const XrVulkanGraphicsDeviceGetInfoKHR* get_info,
    VkPhysicalDevice* physical_device) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->vulkan.get_graphics_device2 == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        log_vulkan_negotiation(7, 2);
        return dispatch->vulkan.get_graphics_device2(
            instance, get_info, physical_device);
    });
}

XrResult get_vulkan_requirements_recorded(
    PFN_GetVulkanGraphicsRequirements next,
    std::uint64_t which,
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    if (next == nullptr) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrResult result = next(instance, system_id, requirements);
    if (XR_SUCCEEDED(result) && requirements != nullptr) {
        log_vulkan_negotiation(
            8, which, requirements->minApiVersionSupported,
            requirements->maxApiVersionSupported);
    }
    return result;
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements : nullptr,
            1, instance, system_id, requirements);
    });
}

XRAPI_ATTR XrResult XRAPI_CALL layer_get_vulkan_graphics_requirements2(
    XrInstance instance,
    XrSystemId system_id,
    XrGraphicsRequirementsVulkanKHR* requirements) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        return get_vulkan_requirements_recorded(
            dispatch ? dispatch->vulkan.get_graphics_requirements2 : nullptr,
            2, instance, system_id, requirements);
    });
}


// A bridged Vulkan session's runtime is D3D12 and lists DXGI formats; the
// application asked a Vulkan session and reads VkFormats. Each DXGI format
// with a Vulkan equivalent is shown once, in the runtime's order.
XRAPI_ATTR XrResult XRAPI_CALL layer_enumerate_swapchain_formats(
    XrSession session,
    std::uint32_t format_capacity_input,
    std::uint32_t* format_count_output,
    std::int64_t* formats) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto state = find_session(session);
        if (!state || state->dispatch->enumerate_swapchain_formats == nullptr) {
            return XR_ERROR_HANDLE_INVALID;
        }
        if (!state->vulkan_bridge) {
            return state->dispatch->enumerate_swapchain_formats(
                session, format_capacity_input, format_count_output, formats);
        }
        if (format_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::uint32_t runtime_count = 0;
        XrResult result = state->dispatch->enumerate_swapchain_formats(
            session, 0, &runtime_count, nullptr);
        if (XR_FAILED(result)) {
            return result;
        }
        std::vector<std::int64_t> runtime_formats(runtime_count);
        if (runtime_count > 0) {
            result = state->dispatch->enumerate_swapchain_formats(
                session, runtime_count, &runtime_count, runtime_formats.data());
            if (XR_FAILED(result)) {
                return result;
            }
            runtime_formats.resize(runtime_count);
        }
        std::vector<std::int64_t> translated;
        for (const std::int64_t format : runtime_formats) {
            const VkFormat vulkan_format =
                xrfg::vulkan_format_for_dxgi(static_cast<DXGI_FORMAT>(format));
            if (vulkan_format == VK_FORMAT_UNDEFINED ||
                std::find(translated.begin(), translated.end(),
                          static_cast<std::int64_t>(vulkan_format)) != translated.end()) {
                continue;
            }
            translated.push_back(static_cast<std::int64_t>(vulkan_format));
        }
        *format_count_output = static_cast<std::uint32_t>(translated.size());
        if (format_capacity_input == 0) {
            return XR_SUCCESS;
        }
        if (formats == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        if (format_capacity_input < translated.size()) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        std::copy(translated.begin(), translated.end(), formats);
        return XR_SUCCESS;
    });
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

    if (instance == XR_NULL_HANDLE) {
        return resolve_global_function(name, function);
    }
    const auto dispatch = find_dispatch(instance);
    if (!dispatch) {
        return XR_ERROR_HANDLE_INVALID;
    }

    if (std::strcmp(
            name, XRFG_SET_PRESENTATION_TRANSFORM_FUNCTION_NAME) == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(
            layer_set_presentation_transform_ofxr);
        return XR_SUCCESS;
    }

    if (dispatch->panel_gesture.action_set != XR_NULL_HANDLE) {
        const auto& gesture = dispatch->panel_gesture;
        if (std::strcmp(name, "xrSuggestInteractionProfileBindings") == 0) {
            return expose_intercept(dispatch, gesture.suggest_bindings,
                layer_suggest_interaction_profile_bindings, function);
        }
        if (std::strcmp(name, "xrAttachSessionActionSets") == 0) {
            return expose_intercept(dispatch, gesture.attach_action_sets,
                layer_attach_session_action_sets, function);
        }
        if (std::strcmp(name, "xrSyncActions") == 0) {
            return expose_intercept(dispatch, gesture.sync_actions, layer_sync_actions, function);
        }
    }

    if (std::strcmp(name, "xrEnumerateSwapchainFormats") == 0) {
        return expose_intercept(dispatch, dispatch->enumerate_swapchain_formats,
            layer_enumerate_swapchain_formats, function);
    }
    if (std::strcmp(name, "xrGetVulkanInstanceExtensionsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_instance_extensions, layer_get_vulkan_instance_extensions, function);
    }
    if (std::strcmp(name, "xrGetVulkanDeviceExtensionsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_device_extensions, layer_get_vulkan_device_extensions, function);
    }
    if (std::strcmp(name, "xrCreateVulkanInstanceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.create_instance, layer_create_vulkan_instance, function);
    }
    if (std::strcmp(name, "xrCreateVulkanDeviceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.create_device, layer_create_vulkan_device, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsDeviceKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_device, layer_get_vulkan_graphics_device, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsDevice2KHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_device2, layer_get_vulkan_graphics_device2, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsRequirementsKHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_requirements, layer_get_vulkan_graphics_requirements, function);
    }
    if (std::strcmp(name, "xrGetVulkanGraphicsRequirements2KHR") == 0) {
        return expose_intercept(dispatch, dispatch->vulkan.get_graphics_requirements2, layer_get_vulkan_graphics_requirements2, function);
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
    if (std::strcmp(name, "xrLocateViews") == 0) {
        return expose_intercept(dispatch, dispatch->locate_views, layer_locate_views, function);
    }
    if (std::strcmp(name, "xrCreateReferenceSpace") == 0) {
        return expose_intercept(dispatch, dispatch->create_reference_space, layer_create_reference_space, function);
    }
    if (std::strcmp(name, "xrCreateActionSpace") == 0) {
        return expose_intercept(dispatch, dispatch->create_action_space, layer_create_action_space, function);
    }
    if (std::strcmp(name, "xrLocateSpace") == 0) {
        return expose_intercept(dispatch, dispatch->locate_space, layer_locate_space, function);
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
    if (std::strcmp(name, "xrPollEvent") == 0) {
        return expose_intercept(
            dispatch, dispatch->poll_event, layer_poll_event, function);
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

// view_configuration, once per view at session creation.
void log_view_configuration(
    const Dispatch& dispatch, XrInstance instance, XrSystemId system_id) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled() || system_id == XR_NULL_SYSTEM_ID) {
            return;
        }
        PFN_xrEnumerateViewConfigurationViews enumerate = nullptr;
        if (!load_function(
                dispatch.get_instance_proc_addr, instance,
                "xrEnumerateViewConfigurationViews", enumerate)) {
            return;
        }
        std::array<XrViewConfigurationView, 4> views{};
        for (auto& view : views) {
            view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        }
        std::uint32_t count = 0;
        if (XR_FAILED(enumerate(
                instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                static_cast<std::uint32_t>(views.size()), &count, views.data()))) {
            return;
        }
        for (std::uint32_t index = 0; index < std::min<std::uint32_t>(count, 4); ++index) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::view_configuration,
                index,
                (static_cast<std::uint64_t>(views[index].recommendedImageRectWidth) << 32) |
                    views[index].recommendedImageRectHeight,
                (static_cast<std::uint64_t>(views[index].maxImageRectWidth) << 32) |
                    views[index].maxImageRectHeight,
                views[index].recommendedSwapchainSampleCount);
        }
    } catch (...) {
    }
}


void create_panel_gesture(
    Dispatch& dispatch,
    PFN_xrGetInstanceProcAddr next,
    XrInstance instance) noexcept {
    try {
        auto& gesture = dispatch.panel_gesture;
        PFN_xrStringToPath string_to_path = nullptr;
        PFN_xrCreateActionSet create_action_set = nullptr;
        PFN_xrDestroyActionSet destroy_action_set = nullptr;
        PFN_xrCreateAction create_action = nullptr;
        if (!load_function(next, instance, "xrStringToPath", string_to_path) ||
            !load_function(next, instance, "xrCreateActionSet", create_action_set) ||
            !load_function(next, instance, "xrDestroyActionSet", destroy_action_set) ||
            !load_function(next, instance, "xrCreateAction", create_action) ||
            !load_function(next, instance, "xrSuggestInteractionProfileBindings",
                           gesture.suggest_bindings) ||
            !load_function(next, instance, "xrAttachSessionActionSets",
                           gesture.attach_action_sets) ||
            !load_function(next, instance, "xrSyncActions", gesture.sync_actions) ||
            !load_function(next, instance, "xrCreateActionSpace", gesture.create_action_space)) {
            return;
        }
        constexpr std::array<const char*, 2> kHands{"/user/hand/left", "/user/hand/right"};
        constexpr std::array<const char*, 2> kGrips{
            "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"};
        for (std::size_t hand = 0; hand < kHands.size(); ++hand) {
            if (XR_FAILED(string_to_path(instance, kHands[hand], &gesture.hands[hand])) ||
                XR_FAILED(string_to_path(instance, kGrips[hand], &gesture.grips[hand]))) {
                return;
            }
        }
        XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(set_info.actionSetName, "ofxr_status_panel");
        strcpy_s(set_info.localizedActionSetName, "OFXR Bridge status panel");
        // The lowest priority: where the application binds the same grip,
        // its own action sets are never the ones that give way.
        set_info.priority = 0;
        XrActionSet action_set = XR_NULL_HANDLE;
        XrResult result = create_action_set(instance, &set_info, &action_set);
        if (XR_SUCCEEDED(result)) {
            XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
            strcpy_s(action_info.actionName, "grip_pose");
            strcpy_s(action_info.localizedActionName,
                     "Controller pose, for the OFXR Bridge status panel");
            action_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            action_info.countSubactionPaths = static_cast<std::uint32_t>(gesture.hands.size());
            action_info.subactionPaths = gesture.hands.data();
            XrAction action = XR_NULL_HANDLE;
            result = create_action(action_set, &action_info, &action);
            if (XR_SUCCEEDED(result)) {
                gesture.action_set = action_set;
                gesture.grip_action = action;
            } else {
                destroy_action_set(action_set);
            }
        }
    } catch (...) {
    }
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
    // The SteamVR director needs clock conversion even with diagnostics off.
    // Runtime identity is known only after instance creation, so negotiate this
    // advertised capability here for any Windows runtime. Other runtimes do not
    // acquire the SteamVR director or its renewal policy. Never guess XrTime's
    // epoch or call an extension which was not enabled.
    XrInstanceCreateInfo diagnostic_create = *create_info;
    std::vector<const char*> diagnostic_extensions;
    bool bridge_enabled_clock = false;
    {
        bool requested = false;
        for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index)
            requested |= std::strcmp(create_info->enabledExtensionNames[index],
                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
        PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
        if (!requested && load_function(next_get_instance_proc_addr, XR_NULL_HANDLE,
                "xrEnumerateInstanceExtensionProperties", enumerate)) {
            std::uint32_t count{};
            if (XR_SUCCEEDED(enumerate(nullptr, 0, &count, nullptr))) {
                std::vector<XrExtensionProperties> extensions(
                    count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                if (XR_SUCCEEDED(enumerate(nullptr, count, &count, extensions.data()))) {
                    for (const auto& extension : extensions) {
                        if (std::strcmp(extension.extensionName,
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0) {
                            for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index)
                                diagnostic_extensions.push_back(create_info->enabledExtensionNames[index]);
                            diagnostic_extensions.push_back(
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
                            diagnostic_create.enabledExtensionCount =
                                static_cast<std::uint32_t>(diagnostic_extensions.size());
                            diagnostic_create.enabledExtensionNames = diagnostic_extensions.data();
                            bridge_enabled_clock = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    // The D3D11 bridge needs XR_KHR_D3D12_enable on the instance, which the
    // application did not ask for. Added here, only when the ini asks for
    // the bridge, the application enabled D3D11 and the runtime lists D3D12;
    // anything short of that leaves the instance as the application made it.
    XrInstanceCreateInfo bridged_create_info{};
    std::vector<const char*> bridged_extensions;
    const XrInstanceCreateInfo* forwarded_create_info = bridge_enabled_clock ? &diagnostic_create : create_info;
    const XrInstanceCreateInfo* const negotiated_info = forwarded_create_info;
    bool bridge_extension_added = false;
    bool bridge_requested_by_ini = false;
    bool vulkan_bridge_requested_by_ini = false;
    bool asked_vulkan = false;
    bool asked_d3d11 = false;
    bool asked_d3d12 = false;
    try {
        for (std::uint32_t index = 0; index < create_info->enabledExtensionCount; ++index) {
            const char* const extension = create_info->enabledExtensionNames[index];
            if (extension == nullptr) continue;
            if (std::string_view(extension) == "XR_KHR_vulkan_enable" ||
                std::string_view(extension) == "XR_KHR_vulkan_enable2") asked_vulkan = true;
            if (std::string_view(extension) == "XR_KHR_D3D11_enable") asked_d3d11 = true;
            if (std::string_view(extension) == "XR_KHR_D3D12_enable") asked_d3d12 = true;
        }
        // A game that enabled XR_KHR_D3D12_enable itself - Assetto Corsa
        // and SkyrimVR enable every graphics extension the runtime lists -
        // needs nothing added; the bridge then decides at xrCreateSession
        // on the binding it sees.
        bridge_requested_by_ini =
            GetPrivateProfileIntW(L"ofxr", L"d3d11_bridge", 1, (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
        vulkan_bridge_requested_by_ini = GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
            (current_layer_directory() / L"ofxr_bridge.ini").c_str()) == 1;
        if (!asked_d3d12 && ((asked_d3d11 && bridge_requested_by_ini) ||
                            (asked_vulkan && vulkan_bridge_requested_by_ini))) {
            PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
            bool runtime_has_d3d12 = false;
            if (XR_SUCCEEDED(next_get_instance_proc_addr(
                    XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                    reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_extensions))) &&
                enumerate_extensions != nullptr) {
                std::uint32_t count = 0;
                if (XR_SUCCEEDED(enumerate_extensions(nullptr, 0, &count, nullptr)) && count > 0) {
                    std::vector<XrExtensionProperties> properties(
                        count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
                    if (XR_SUCCEEDED(enumerate_extensions(
                            nullptr, count, &count, properties.data()))) {
                        for (std::uint32_t index = 0; index < count; ++index) {
                            if (std::string_view(properties[index].extensionName) ==
                                "XR_KHR_D3D12_enable") {
                                runtime_has_d3d12 = true;
                            }
                        }
                    }
                }
            }
            if (runtime_has_d3d12) {
                bridged_extensions.assign(
                    negotiated_info->enabledExtensionNames,
                    negotiated_info->enabledExtensionNames + negotiated_info->enabledExtensionCount);
                bridged_extensions.push_back("XR_KHR_D3D12_enable");
                bridged_create_info = *negotiated_info;
                bridged_create_info.enabledExtensionNames = bridged_extensions.data();
                bridged_create_info.enabledExtensionCount =
                    static_cast<std::uint32_t>(bridged_extensions.size());
                forwarded_create_info = &bridged_create_info;
                bridge_extension_added = true;
            }
        }
    } catch (...) {
        forwarded_create_info = negotiated_info;
        bridge_extension_added = false;
    }

    if(!xrfg::autonomous::initialize_game_configuration())return XR_ERROR_INITIALIZATION_FAILED;
    xrfg::autonomous::apply_runtime_settings();
    xrfg::prepare_ngx_feature_discovery(current_layer_directory().c_str());
    XrInstance created_instance = XR_NULL_HANDLE;
    const XrResult result = next_create_api_layer_instance(
        forwarded_create_info,
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
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrCreateReferenceSpace", dispatch->create_reference_space)), true) &&
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrCreateActionSpace", dispatch->create_action_space)), true) &&
        (static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
             "xrLocateSpace", dispatch->locate_space)), true) &&
        load_function(next_get_instance_proc_addr, created_instance, "xrDestroySwapchain", dispatch->destroy_swapchain) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, it just never has a space settled before destroy.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrDestroySpace",
             dispatch->destroy_space)),
         true) &&
        // Best effort, deliberately not a load requirement: a runtime without
        // it keeps working, the recorder just never sees a state transition.
        (static_cast<void>(load_function(
             next_get_instance_proc_addr,
             created_instance,
             "xrPollEvent",
             dispatch->poll_event)),
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

        // The bridge decision, on every instance: a the graphics extensions
        // the application enabled (4 D3D11, 8 D3D12), b whether the ini asks
        // for the bridge, c whether the extension was added. result 0 with
        // c=1 is the bridge armed; -1 is an added extension whose
        // requirements call the runtime then failed to hand out.
        const bool d3d12_on_instance = bridge_extension_added || asked_d3d12;
        if (bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance) {
            dispatch->d3d11_bridge = load_function(
                next_get_instance_proc_addr, created_instance,
                "xrGetD3D12GraphicsRequirementsKHR",
                dispatch->get_d3d12_graphics_requirements);
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            bridge_requested_by_ini && asked_d3d11 && d3d12_on_instance &&
                    !dispatch->d3d11_bridge
                ? -1
                : 0,
            (asked_d3d11 ? 4ULL : 0ULL) | (asked_d3d12 ? 8ULL : 0ULL),
            bridge_requested_by_ini ? 1 : 0,
            (bridge_extension_added ? 1ULL : 0ULL) |
                (dispatch->d3d11_bridge ? 2ULL : 0ULL));

    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrEnumerateSwapchainFormats", dispatch->enumerate_swapchain_formats));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanInstanceExtensionsKHR", dispatch->vulkan.get_instance_extensions));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanDeviceExtensionsKHR", dispatch->vulkan.get_device_extensions));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrCreateVulkanInstanceKHR", dispatch->vulkan.create_instance));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrCreateVulkanDeviceKHR", dispatch->vulkan.create_device));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsDeviceKHR", dispatch->vulkan.get_graphics_device));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsDevice2KHR", dispatch->vulkan.get_graphics_device2));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsRequirementsKHR", dispatch->vulkan.get_graphics_requirements));
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrGetVulkanGraphicsRequirements2KHR", dispatch->vulkan.get_graphics_requirements2));
    if (vulkan_bridge_requested_by_ini && asked_vulkan && d3d12_on_instance) {
        dispatch->vulkan_bridge = dispatch->get_d3d12_graphics_requirements != nullptr ||
            load_function(next_get_instance_proc_addr, created_instance,
                "xrGetD3D12GraphicsRequirementsKHR", dispatch->get_d3d12_graphics_requirements);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::vulkan_bridge,
            dispatch->vulkan_bridge ? 0 : -1, asked_vulkan, bridge_extension_added, 0);
    }

    dispatch->instance = created_instance;
    bool application_enabled_clock_conversion = false;
    for (std::uint32_t index = 0;
         index < create_info->enabledExtensionCount; ++index) {
        const char* extension = create_info->enabledExtensionNames[index];
        if (extension != nullptr && std::strcmp(
                extension,
                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0) {
            application_enabled_clock_conversion = true;
            break;
        }
    }
    static_cast<void>(load_function(next_get_instance_proc_addr, created_instance,
        "xrLocateViews", dispatch->locate_views));
    if (application_enabled_clock_conversion || bridge_enabled_clock) {
        static_cast<void>(load_function(
            next_get_instance_proc_addr,
            created_instance,
            "xrConvertTimeToWin32PerformanceCounterKHR",
            dispatch->convert_time_to_qpc));
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_clock_capability,
        dispatch->convert_time_to_qpc ? 1 : 0,
        application_enabled_clock_conversion ? 1u : 0u,
        dispatch->convert_time_to_qpc ? 1u : 0u,
        bridge_enabled_clock ? 1u : 0u);

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
            dispatch->virtual_desktop_runtime = dispatch->runtime_name.find("VirtualDesktopXR") != std::string::npos;
            dispatch->inline_unless_pipelined = dispatch->virtual_desktop_runtime ||
                dispatch->runtime_name.find("Pimax OpenXR") != std::string::npos;
            dispatch->steamvr_runtime =
                std::string_view(dispatch->runtime_name).find("SteamVR") !=
                std::string_view::npos;
        }
    }
    xrfg::set_ngx_capture_steamvr_runtime(dispatch->steamvr_runtime);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::runtime_identity,
        dispatch->steamvr_runtime ? 1 : 0,
        dispatch->runtime_version,
        dispatch->runtime_name.size(),
        runtime_name_hash(dispatch->runtime_name));

    create_panel_gesture(*dispatch,next_get_instance_proc_addr,created_instance);
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
        if (session_state->frame_capture) session_state->frame_capture->shutdown();
        session_state->fps_overlay.reset();
    }
    for (const auto& swapchain_state : find_swapchains(dispatch)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_composition_capture(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    bool last_instance = false;
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
        last_instance = g_instances.empty();
        std::erase_if(g_observed_spaces, [&](const auto& entry) {
            return entry.second.dispatch && entry.second.dispatch->instance == instance;
        });
    }
    if (last_instance) xrfg::SteamVrDelivery::release_process_connection();
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

    // RealVR may have arrived after NGX discovery or the previous XR session.
    // Resolve that boundary before selecting any session-specific Luke path.
    xrfg::native_ingress::refresh_luke_detection();
    auto state = std::make_shared<SessionState>(dispatch);
    state->steamvr_separate_queue = GetPrivateProfileIntW(L"tray", L"steamvr_separate_queue", 1,
        xrfg::autonomous::global_configuration_path().c_str()) != 0;
    state->luke_afw_enabled=xrfg::native_ingress::luke_detected();
    // The transport preference is global; physical runtime identity stays intact.
    wchar_t transport[32]{};
    GetPrivateProfileStringW(L"tray",L"pacing",L"auto",transport,32,
        xrfg::autonomous::global_configuration_path().c_str());
    // Preserve native Pimax OpenXR's queue/frame-loop contract in Auto, like
    // VDXR. Upstream V371 retains its application queue; V545/V464fix2 uses
    // the presenter for a single-thread loop and keeps split loops inline.
    // An explicit comparison-transport request still takes the private path.
    state->vdxr_legacy=_wcsicmp(transport,L"force_vdxr")==0 ||
        (_wcsicmp(transport,L"force_steamvr")!=0 && (dispatch->virtual_desktop_runtime ||
            dispatch->runtime_name.find("Pimax OpenXR")!=std::string::npos));
    const auto game_preferences=xrfg::autonomous::snapshot().desired;
    state->afw_only=state->luke_afw_enabled && game_preferences.afw_only;
    state->luke_shared_swapchains=game_preferences.luke_shared_swapchains;
    auto initial_control = xrfg::embedded::snapshot();
    initial_control.desired=effective_session_settings(*state,initial_control.desired);
    state->optical_flow_backend = initial_control.desired.backend == 0
        ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
    state->nvidia_new_route = initial_control.desired.backend >= 2;
    state->djules_route = initial_control.desired.backend == 3;
    state->djules_pipeline_switchable=true;
    state->djules_triple_requested=game_preferences.triple_frame_gen;
    state->djules_deep_pipeline=state->djules_triple_requested || game_preferences.prefer_fps;
    xrfg::embedded::register_djules_pipeline(state->control_id, state->djules_deep_pipeline, state->djules_triple_requested);
    state->nvidia_options = {
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(initial_control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(initial_control.desired.scale),
        initial_control.desired.backward,
        state->nvidia_new_route, xrfg::flow_encoding_for_route(state->nvidia_new_route), state->nvidia_new_route, initial_control.desired.native_scale};
    state->menu_enabled = initial_control.desired.enabled;
    state->frame_generation_mode = static_cast<xrfg::D3D12FrameGenerationMode>(initial_control.desired.frame_generation);
    state->luke_afw_enabled = xrfg::native_ingress::luke_detected();
    state->hybrid_requested=initial_control.desired.hybrid;
    if(state->hybrid_requested &&
        !(state->luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode != 0) &&
        xrfg::dlss_motion_vector_publications()!=0) {
        state->nvidia_options.hybrid=true;
        state->nvidia_options.input_scale=xrfg::D3D12NvidiaInputScale::quarter;
        state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    }
    state->dlss_motion_vectors = initial_control.desired.motion_vectors == 1 ||
        state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::ofxr;
    const auto experimental_ini = current_layer_directory() / L"ofxr_bridge.ini";
    state->luke_afw_enabled = xrfg::native_ingress::luke_detected();
    state->luke_fresh_only=state->luke_afw_enabled && xrfg::luke_hfw_producer_supported();
    state->luke_afw_odd_serial_is_left = xrfg::luke_host_odd_serial_is_left(
        xrfg::luke_host_game_directory(), experimental_ini);
    if (state->luke_afw_enabled) state->dlss_motion_vectors = true;
    state->djules_history_wait_enabled.store(ordinary_djules_route(*state), std::memory_order_relaxed);
    state->control_revision = initial_control.revision;
    state->generation_outputs_needed=state->menu_enabled ||
        (state->luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode!=0);
    state->separate_synthesis = effective_memory_control(*state,initial_control.desired.separate_synthesis);
    state->lower_vram = effective_memory_control(*state,initial_control.desired.lower_vram);
    state->single_director_requested = (initial_control.desired.pacing_mode==1);
    state->applied_generation_settings = initial_control.desired;
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
                    if ((state->d3d11_device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0) {
                        state->single_threaded_d3d11 = true;
                        state->graphics_binding_capabilities |= 32ULL;
                    }
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
                if (FAILED(xrfg::d3d_native::canonical_binding(
                        binding->device, binding->queue,
                        state->d3d12_device, state->d3d12_queue))) {
                    return XR_ERROR_GRAPHICS_DEVICE_INVALID;
                }
                break;
            }
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                const auto* binding = reinterpret_cast<const XrGraphicsBindingVulkanKHR*>(next);
                state->vulkan_binding = {binding->instance, binding->physicalDevice,
                    binding->device, binding->queueFamilyIndex, binding->queueIndex};
                probe_vulkan_interop_support(*binding);
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

    state->luke_outlaws_preframe=state->luke_afw_enabled&&
        state->graphics_binding==SessionGraphicsBinding::d3d11&&
        _wcsicmp(xrfg::implicit_layer::current_executable_name().c_str(),L"Outlaws.exe")==0;
    XrSessionCreateInfo substituted_info{};
    const XrSessionCreateInfo* forwarded_info = create_info;
    // The D3D11 bridge: see SessionState::d3d11_bridge. From here the
    // session is a D3D12 one for every path below, and the runtime gets a
    // D3D12 binding on the layer's device. Anything that fails leaves the
    // D3D11 binding as the application made it.
    XrGraphicsBindingD3D12KHR bridge_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    const void* d3d11_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
                d3d11_binding_next = next->next;
            }
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Device> luke_producer_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> luke_producer_queue;
    const bool luke_producer = state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        query_luke_ross_d3d12_context(luke_producer_device.GetAddressOf(), luke_producer_queue.GetAddressOf());
    if (!luke_producer && !state->luke_afw_enabled && dispatch->d3d11_bridge &&
        dispatch->get_d3d12_graphics_requirements != nullptr &&
        state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        create_info != nullptr && state->d3d11_device && state->d3d11_context) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_d3d11(
                state->d3d11_device.Get(),
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::D3D11Bridge>();
            bridge->device = std::move(state->d3d11_device);
            bridge->context = std::move(state->d3d11_context);
            state->d3d11_device.Reset();
            state->d3d11_context.Reset();
            state->d3d11_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->single_threaded_d3d11 = false;
            state->graphics_binding_capabilities |= 128ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            // Whatever the application chained after its binding stays
            // chained, after ours.
            bridge_binding.next = d3d11_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                1,
                0,
                0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }

    // The Vulkan bridge: see SessionState::vulkan_bridge. The same
    // substitution as the D3D11 bridge's, on a D3D12 device of the layer's
    // for the adapter the Vulkan physical device reports. Anything that
    // fails leaves the Vulkan binding as the application made it, on its native Vulkan runtime; no obsolete mirror fallback.
    const void* vulkan_binding_next = nullptr;
    if (create_info != nullptr) {
        for (auto* next = static_cast<const XrBaseInStructure*>(create_info->next);
             next != nullptr; next = next->next) {
            if (next->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
                vulkan_binding_next = next->next;
            }
        }
    }
    // Bit per unmet precondition, logged when a Vulkan session stays native:
    // 1 runtime/instance bridge, 2 ini switch, 4 the OFXR queue layer is
    // loaded but does not serialize this device, 8 no D3D12 requirements
    // entry point, 16 no create info or device. Without any queue layer (Star
    // Citizen admits no implicit Vulkan layer) the bridge runs in
    // synchronized-calls mode; see xrfg::vulkan_queue_access.
    std::uint64_t vulkan_bridge_unmet = 0;
    auto vulkan_access = xrfg::VulkanQueueAccess::refused;
    if (state->graphics_binding == SessionGraphicsBinding::vulkan) {
        if (!dispatch->vulkan_bridge) vulkan_bridge_unmet |= 1;
        if (GetPrivateProfileIntW(L"ofxr", L"vulkan_bridge", 1,
                (current_layer_directory() / L"ofxr_bridge.ini").c_str()) != 1)
            vulkan_bridge_unmet |= 2;
        vulkan_access = xrfg::vulkan_queue_access(
            xrfg::vulkan_queue_serialized(state->vulkan_binding), xrfg::vulkan_queue_layer_loaded());
        if (vulkan_access == xrfg::VulkanQueueAccess::refused) vulkan_bridge_unmet |= 4;
        if (dispatch->get_d3d12_graphics_requirements == nullptr) vulkan_bridge_unmet |= 8;
        if (create_info == nullptr || state->vulkan_binding.device == VK_NULL_HANDLE)
            vulkan_bridge_unmet |= 16;
        if (vulkan_bridge_unmet)
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge, 5, vulkan_bridge_unmet, 0,
                static_cast<std::uint64_t>(state->graphics_binding));
    }
    if (state->graphics_binding == SessionGraphicsBinding::vulkan && !vulkan_bridge_unmet) {
        XrGraphicsRequirementsD3D12KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        const XrResult requirements_result =
            dispatch->get_d3d12_graphics_requirements(
                instance, create_info->systemId, &requirements);
        Microsoft::WRL::ComPtr<ID3D12Device> bridge_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> bridge_queue;
        HRESULT device_result = E_FAIL;
        if (XR_SUCCEEDED(requirements_result)) {
            device_result = xrfg::create_d3d12_device_for_vulkan(
                state->vulkan_binding,
                bridge_device.ReleaseAndGetAddressOf(),
                bridge_queue.ReleaseAndGetAddressOf());
        }
        if (XR_SUCCEEDED(requirements_result) && SUCCEEDED(device_result) &&
            bridge_device && bridge_queue) {
            auto bridge = std::make_unique<SessionState::VulkanBridge>();
            bridge->binding = state->vulkan_binding;
            state->vulkan_bridge = std::move(bridge);
            state->d3d12_device = std::move(bridge_device);
            state->d3d12_queue = std::move(bridge_queue);
            state->graphics_binding = SessionGraphicsBinding::d3d12;
            state->graphics_binding_capabilities |= 256ULL;
            bridge_binding.device = state->d3d12_device.Get();
            bridge_binding.queue = state->d3d12_queue.Get();
            bridge_binding.next = vulkan_binding_next;
            substituted_info = *create_info;
            substituted_info.next = &bridge_binding;
            forwarded_info = &substituted_info;
            // b=1: synchronized-calls mode (no queue layer in the process).
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                1,
                0,
                vulkan_access == xrfg::VulkanQueueAccess::synchronized_calls ? 1 : 0,
                static_cast<std::uint64_t>(requirements.adapterLuid.LowPart));
        } else {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(requirements_result)
                    ? static_cast<std::int64_t>(requirements_result)
                    : static_cast<std::int64_t>(device_result),
                0,
                XR_FAILED(requirements_result) ? 1 : 2,
                0);
        }
    }

    XrGraphicsBindingD3D12KHR substituted_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    HRESULT binding_queue_result = S_FALSE;
    if (runtime_binding_queue_requested(*state) &&
        state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        create_info != nullptr && state->d3d12_device && state->d3d12_queue) {
        const auto* first = state->d3d11_bridge || state->vulkan_bridge
            ? reinterpret_cast<const XrBaseInStructure*>(&bridge_binding)
            : static_cast<const XrBaseInStructure*>(create_info->next);
        if (first != nullptr && first->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            binding_queue_result = initialize_runtime_binding_queue(*state);
            if (binding_queue_result == S_OK) {
                substituted_binding =
                    *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(first);
                substituted_binding.queue = state->binding_queue.Get();
                substituted_info = *create_info;
                substituted_info.next = &substituted_binding;
                forwarded_info = &substituted_info;
            }
        }
    }


    // Bind SteamVR to a queue which no producer can contaminate with future
    // app/synthesis waits. The old optional binding queue now prepares PRIVATE
    // outputs; it cannot also be the final consumer's graphics binding.
    XrGraphicsBindingD3D12KHR delivery_binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    if(!state->afw_only && comparison_delivery_runtime(*state) && state->graphics_binding==SessionGraphicsBinding::d3d12 &&
       forwarded_info && forwarded_info->next) {
        const auto* binding=static_cast<const XrBaseInStructure*>(forwarded_info->next);
        if(binding->type==XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            delivery_binding=*reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(binding);
            auto description=delivery_binding.queue->GetDesc();
            D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY priority{description.Type,D3D12_COMMAND_QUEUE_PRIORITY_HIGH,FALSE};
            const bool supported=SUCCEEDED(state->d3d12_device->CheckFeatureSupport(
                D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&priority,sizeof(priority))) && priority.PriorityForTypeIsSupported;
            const auto made=xrfg::create_private_delivery_queue(description,supported,
                [&](const D3D12_COMMAND_QUEUE_DESC& requested) {
                    return state->d3d12_device->CreateCommandQueue(&requested,
                        IID_PPV_ARGS(state->private_delivery_queue.ReleaseAndGetAddressOf()));
                });
            if(FAILED(made))return XR_ERROR_INITIALIZATION_FAILED;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::steamvr_queue_configuration,
                586,static_cast<std::uint64_t>(description.Priority),
                static_cast<std::uint64_t>(state->private_delivery_queue->GetDesc().Priority),supported?1:0);
            state->private_delivery_queue->SetName(L"OFXR real OpenXR delivery only");
            delivery_binding.queue=state->private_delivery_queue.Get();
            // forwarded_info may already refer to substituted_info.
            XrSessionCreateInfo with_delivery=*forwarded_info;
            with_delivery.next=&delivery_binding;substituted_info=with_delivery;
            forwarded_info=&substituted_info;
        }
    }
    XrSession created_session = XR_NULL_HANDLE;
    const XrResult result = dispatch->create_session(instance, forwarded_info, &created_session);
    if (XR_FAILED(result)) {
        return result;
    }
    state->handle = created_session;
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::steamvr_queue_configuration,
        binding_queue_result, handle_value(created_session),
        (state->steamvr_separate_queue ? 1ULL : 0ULL) | (state->binding_queue ? 2ULL : 0ULL) |
            (state->private_delivery_queue ? 4ULL : 0ULL),
        static_cast<std::uint64_t>(initial_control.desired.backend) |
        (static_cast<std::uint64_t>(state->frame_generation_mode) << 8) |
        (state->hybrid_requested ? 1ULL << 16 : 0ULL) | (state->menu_enabled ? 1ULL << 17 : 0ULL));
    if(state->luke_fresh_only)
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
            9,handle_value(created_session),0,0);

    state->steamvr_delivery = std::make_unique<xrfg::SteamVrDelivery>(dispatch->steamvr_runtime);
    const auto private_result=install_private_runtime(state);
    if(FAILED(private_result)){dispatch->destroy_session(created_session);return XR_ERROR_INITIALIZATION_FAILED;}
    log_video_memory(*state, VideoMemoryStage::session, 0);
    log_view_configuration(*dispatch, instance, create_info ? create_info->systemId : XR_NULL_SYSTEM_ID);

    // Optional instrumentation cannot fail an otherwise valid session.
    try {
        state->fps_overlay = std::make_unique<xrfg::OpenXrFpsOverlay>(
            instance, created_session, create_info ? create_info->systemId : 0,
            state->dispatch->get_instance_proc_addr, content_end_frame,
            state->d3d12_device.Get(), runtime_queue(*state),
            state->d3d11_device.Get(), current_layer_directory() / L"ofxr_bridge.ini", true, nullptr, nullptr,
            [](void* user, XrTime time, std::int64_t& ns) {
                auto& d = *static_cast<Dispatch*>(user);
                LARGE_INTEGER qpc{};
                if (!d.convert_time_to_qpc || XR_FAILED(d.convert_time_to_qpc(d.instance, time, &qpc))) return false;
                ns = xrfg::interface_qpc_ns(qpc.QuadPart); return ns > 0;
            }, dispatch.get());
        bind_private_delivery_metrics(state);
        state->fps_overlay->set_delivery_source(state->steamvr_delivery.get());
        if(state->dispatch->virtual_desktop_runtime ||
            state->dispatch->runtime_name.find("Pimax OpenXR")!=std::string::npos) {
            const auto rate=state->runtime_acceptance;
            state->fps_overlay->set_runtime_acceptance_source(rate?std::function<std::optional<float>(std::int64_t)>([rate](auto now){return rate->rate(now);}):std::function<std::optional<float>(std::int64_t)>{},!rate);
        }
        state->fps_overlay->set_unique_content_only(state->luke_fresh_only);
        state->fps_overlay->set_luke_scene_measurement(xrfg::native_ingress::luke_detected());
    } catch (...) {}
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 &&
        (state->graphics_binding_capabilities & 3ULL) == 3ULL) {
        Microsoft::WRL::ComPtr<ID3D12Device> luke_ross_device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> luke_ross_queue;
        if (query_luke_ross_d3d12_context(
                luke_ross_device.GetAddressOf(),
                luke_ross_queue.GetAddressOf())) {
            Microsoft::WRL::ComPtr<ID3D12Device> native_device;
            Microsoft::WRL::ComPtr<ID3D12CommandQueue> native_producer, execution;
            const bool native_luke = xrfg::native_ingress::query_luke_context(
                native_device.GetAddressOf(), native_producer.GetAddressOf());
            const auto queue_result = select_luke_execution_queue(native_luke,
                luke_ross_device.Get(), luke_ross_queue.Get(), execution);
            state->d3d12_device = std::move(luke_ross_device);
            state->d3d12_queue = std::move(execution);
            if(SUCCEEDED(queue_result) && state->d3d12_queue)
                state->graphics_binding_capabilities |= 8ULL;
            if (state->luke_afw_enabled && SUCCEEDED(queue_result) &&
                xrfg::embedded::afw_snapshot().desired.mode!=0) {
                auto afw = std::make_shared<xrfg::LukeAfwSynthesizer>();
                const HRESULT afw_result = afw->initialize(
                    state->d3d12_device.Get(), state->d3d12_queue.Get(),
                    luke_bootstrap_provider());
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::synthesis_initialize,
                    afw_result, 0xAF023245ULL,
                    state->luke_afw_odd_serial_is_left ? 1ULL : 0ULL, 0);
                if (SUCCEEDED(afw_result)) state->luke_afw = std::move(afw);
            }
        } else {
            const HRESULT bridge_device_result =
                xrfg::create_d3d12_device_for_d3d11(
                    state->d3d11_device.Get(),
                    state->d3d12_device.ReleaseAndGetAddressOf(),
                    state->d3d12_queue.ReleaseAndGetAddressOf());
            if (state->luke_afw_enabled)
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    bridge_device_result,962,handle_value(created_session),0);
            if (SUCCEEDED(bridge_device_result)) {
                state->graphics_binding_capabilities |= 4ULL;
            } else {
                state->d3d12_device.Reset();
                state->d3d12_queue.Reset();
            }
        }
    }
    if(auto monitor=xrfg::neural_rendering::observe_device_removal(state->d3d12_device.Get()))
        state->gpu_crash_monitors.push_back(std::move(monitor));
    auto* application_d3d11_device = state->d3d11_bridge
        ? state->d3d11_bridge->device.Get() : state->d3d11_device.Get();
    if (application_d3d11_device && state->d3d12_device && state->d3d12_queue) {
        xrfg::register_d3d11_guide_owner(reinterpret_cast<std::uintptr_t>(state.get()),
            application_d3d11_device, state->d3d12_device.Get(), state->d3d12_queue.Get());
    }
    if (state->graphics_binding == SessionGraphicsBinding::d3d11 && state->d3d11_device && state->d3d11_context) {
        try { state->frame_capture = std::make_unique<xrfg::FrameCapture>(state->d3d11_device.Get(),
            state->d3d11_context.Get(), current_layer_directory() / L"captures"); } catch (...) {}
    }
    // A queue of the layer's own for synthesis, so the GPU-side wait on the
    // optical flow fence does not sit in the middle of the application's
    // queue. Only for a native D3D12 application: a D3D11 one already runs
    // synthesis on the bridge queue created just above.
    if (state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        state->d3d12_device && state->d3d12_queue) {
        D3D12_COMMAND_QUEUE_DESC synthesis_queue_description{};
        synthesis_queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // Synthesis has a hard deadline the application's rendering does
        // not: it must finish inside one display period or its frame is
        // reprojected away.
        synthesis_queue_description.Priority =
            D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        synthesis_queue_description.NodeMask =
            state->d3d12_queue->GetDesc().NodeMask;
        if (FAILED(state->d3d12_device->CreateCommandQueue(
                &synthesis_queue_description,
                IID_PPV_ARGS(
                    state->d3d12_synthesis_queue.ReleaseAndGetAddressOf())))) {
            // Fail open: synthesis stays on the application's queue, which
            // is what every build before this one did.
            state->d3d12_synthesis_queue.Reset();
        } else {
            state->graphics_binding_capabilities |= 8ULL;
        }
    }
    initialize_luke_synthesis_queue(*state);
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
    if (state->vulkan_bridge)
        xrfg::native_vulkan::configure_owner(state->vulkan_binding,state->d3d12_device.Get(),state->d3d12_queue.Get());
    if(state->luke_afw_enabled && state->graphics_binding==SessionGraphicsBinding::d3d11)
        state->luke_vulkan_device=xrfg::native_vulkan::configure_luke_owner(state->d3d12_device.Get(),state->d3d12_queue.Get());
    refresh_native_generation_capacity(*state);
    configure_session_guide_capture(*state);
    *session = created_session;
    xrfg::autonomous::publish_runtime_status(state->native_dlss_generated_frames,
        xrfg::autonomous::GenerationState::waiting, S_OK, state->vulkan_binding.device != VK_NULL_HANDLE,0,nullptr,
        state->private_runtime?(state->private_runtime->director_enabled()?(single_director_pacing(*state)?1:2):0):0,state->dispatch->steamvr_runtime,state->afw_only,
        state->private_runtime && state->luke_afw_enabled && state->luke_shared_swapchains,
        false, // no title forces the INI-only sharing policy
        state->dispatch->virtual_desktop_runtime,state->vdxr_legacy,0,session_uses_shared_queue(*state),state->private_runtime?static_cast<unsigned>(state->private_runtime->buffer_capacity()):0);
    return result;
}

XrResult layer_destroy_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->destroy_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    xrfg::stop_ngx_guide_capture(state->d3d12_queue.Get());
    stop_continuous_presenter(state);
    if(state->private_runtime)state->private_runtime->stop();
    drain_binding_queue(state.get());
    state->fps_overlay.reset();
    for(auto& grip:state->panel_grip_spaces){if(grip && state->dispatch->destroy_space)state->dispatch->destroy_space(grip);grip=XR_NULL_HANDLE;}
    if (state->frame_capture) state->frame_capture->shutdown();
    for (const auto& swapchain_state : find_swapchains(state)) {
        std::scoped_lock call_lock(swapchain_state->call_mutex);
        drain_swapchain_gpu(swapchain_state);
        destroy_composition_capture(swapchain_state);
        destroy_frame_generation_swapchains(swapchain_state);
    }

    if (state->vulkan_bridge) {
        xrfg::native_vulkan::stop_owner(state->vulkan_binding.device);
        xrfg::native_vulkan::stop_guide_capture(state->d3d12_queue.Get());
    }
    if(state->luke_vulkan_device)xrfg::native_vulkan::stop_owner(state->luke_vulkan_device);
    const XrResult result = state->dispatch->destroy_session(session);
    std::size_t remaining_sessions{};
    if (XR_SUCCEEDED(result)) {
        xrfg::unregister_d3d11_guide_owner(reinterpret_cast<std::uintptr_t>(state.get()));
        std::scoped_lock lock(g_state_mutex);
        for (auto iterator = g_swapchains.begin(); iterator != g_swapchains.end();) {
            if (iterator->second->session == state) {
                iterator = g_swapchains.erase(iterator);
            } else {
                ++iterator;
            }
        }
        g_sessions.erase(session);
        remaining_sessions = g_sessions.size();
        std::erase_if(g_observed_spaces, [&](const auto& entry) {
            const auto owner = entry.second.session.lock();
            return !owner || owner->handle == session;
        });
    }
    if (XR_SUCCEEDED(result) && !remaining_sessions) {
        auto report = xrfg::autonomous::snapshot().runtime;
        report.sessions = 0;
        xrfg::autonomous::report_runtime(report);
        xrfg::autonomous::report_interface_telemetry({});
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
        state->presenter_start_requested = false;
        state->split_frame_loop = false;
        state->presenter_restore_after_bypass = false;
        state->application_end_thread.store(0);
        state->history_capture_wait_period.store(0);
        state->application_wait_thread_id = state->application_end_thread_id = 0;
        state->runtime_frame_waited_unbegun = state->application_begin_needs_wait = false;
        state->last_inline_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        state->last_inline_frame_state_valid = false;
        state->blackbar_inline_slot=0;state->blackbar_inline_shift=0;
        { auto& t=state->blackbar_timing;std::scoped_lock timing_lock(t.mutex);
          ++t.epoch;t.presenter={};t.inline_promise={};t.located={};t.waits={};
          t.last_shift=0;t.located_next=t.wait_next=0; }
        state->generation_steady_state_established = false;
        state->luke_clock_active = false;
        state->luke_duplicate_cycle = false;
        state->application_wait_returned_at = {};
    }
    state->frame_call_condition.notify_all();
    {
        std::scoped_lock presenter_lock(state->presenter_mutex);
        state->last_virtual_display_time = 0;
    }
    {
        std::scoped_lock lock(state->mutex);
        state->pending_frames.clear();
        state->interface_waited_cycle.store(nullptr);
        state->interface_active_cycle.store(nullptr);
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        state->generation_resume_wall_time = {};
        state->minimum_runtime_display_period = 0;
        state->steamvr_throttled_wait_streak = 0;
        state->unpaced_wait_streak = 0;
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
                // Neither session call releases the application's images.
                // Keep the runtime's outstanding acquire/wait mirror; reset
                // only history and generation state, not image ownership.
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                history = state->d3d12_history;
                generation = state->frame_generation;
            }
            if (generation && generation->synthesizer) {
                static_cast<void>(generation->synthesizer->wait_for_idle());
            }
            if (generation && generation->afw_source_synthesizer)
                static_cast<void>(generation->afw_source_synthesizer->wait_for_idle());
            if (generation && generation->afw_temporal_synthesizer) {
                static_cast<void>(
                    generation->afw_temporal_synthesizer->wait_for_idle());
            }
            if (generation && generation->afw_final_history) {
                static_cast<void>(generation->afw_final_history->invalidate());
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
        if(state->private_runtime)return state->private_runtime->start();
    }
    return result;
}

XrResult layer_end_session_impl(XrSession session) {
    const auto state = find_session(session);
    if (!state || state->dispatch->end_session == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    stop_continuous_presenter(state);
    if (state->fps_overlay) state->fps_overlay->set_session_focused(false);
    if(state->private_runtime){state->private_runtime->drain();state->private_runtime->stop();}
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

void update_luke_application_clock(SessionState& state, bool recognized, bool duplicate) {
    // Called with frame_call_mutex. In particular, do not sample a newer Luke
    // counter here: admission still uses the identity latched at Release.
    if (!recognized) {
        state.luke_clock_active = false;
        state.luke_duplicate_cycle = false;
        return;
    }
    if (!state.luke_clock_active) {
        if (state.last_virtual_display_time <= 0 ||
            state.application_wait_returned_at == std::chrono::steady_clock::time_point{}) return;
        state.luke_clock_origin = state.last_virtual_display_time;
        state.luke_clock_wall_origin = state.application_wait_returned_at;
        state.luke_clock_active = true;
    }
    state.luke_duplicate_cycle = duplicate;
}

XrResult layer_wait_frame_impl(
    XrSession session,
    const XrFrameWaitInfo* wait_info,
    XrFrameState* frame_state, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 :
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_session(session);
    if (!state || state->dispatch->wait_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::unique_lock adaptive_wait_lock(state->adaptive_wait_mutex);
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    state->frame_call_condition.wait(frame_call_lock, [&] {
        return !state->application_wait_pending_begin;
    });
    observe_application_frame_thread(*state, false);
    seal_outlaws_initialization(*state);
    initialize_late_luke_context(state);
    XrResult result = XR_SUCCESS;
    const bool use_continuous_presenter = continuous_presenter_active(state);
    const bool adaptive_nvidia_presenter = use_continuous_presenter &&
        three_slot_generation_session(state);
    if (!adaptive_nvidia_presenter || !state->menu_enabled || state->manual_control.stop_requested() ||
        state->last_virtual_display_time == 0)
        update_luke_application_clock(*state, false, false);
    bool use_provisional_pipelined_wait = false;
    bool forwarded_application_wait = false;
    if (!use_continuous_presenter && state->application_frame_in_progress) {
        if (!state->application_frame_has_overlapping_wait) {
            state->application_frame_has_overlapping_wait = true;
            ++state->pipelined_wait_streak;
        }
        if (!presenter_forbidden(*state) && !state->luke_afw_enabled && state->pipelined_wait_streak >= 2 &&
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
    if(state->private_runtime) {
        if(!valid_optional_frame_info(wait_info,XR_TYPE_FRAME_WAIT_INFO))return XR_ERROR_VALIDATION_FAILURE;
        const bool virtual_only=use_continuous_presenter || state->luke_fresh_only;
        result=virtual_only ? state->private_runtime->predict_frame(frame_state)
            : state->dispatch->wait_frame(session,wait_info,frame_state);
        if(XR_SUCCEEDED(result)) {
            state->last_inline_frame_state=*frame_state;state->last_inline_frame_state.next=nullptr;
            state->last_inline_frame_state_valid=true;forwarded_application_wait=!virtual_only;
            state->runtime_frame_waited_unbegun=!virtual_only;
        }
    } else if (state->luke_fresh_only && state->last_inline_frame_state_valid) {
        if(!valid_optional_frame_info(wait_info,XR_TYPE_FRAME_WAIT_INFO) || !frame_state ||
            frame_state->type!=XR_TYPE_FRAME_STATE)return XR_ERROR_VALIDATION_FAILURE;
        const auto period=std::clamp<XrDuration>(state->last_inline_frame_state.predictedDisplayPeriod,2'000'000,50'000'000);
        // Pace polling at most once per display slot; source production remains
        // independent, and this wait never requires a new runtime submission.
        const auto due=state->application_wait_returned_at+std::chrono::nanoseconds(period);
        state->frame_call_condition.wait_until(frame_call_lock,due,[]{return false;});
        const auto now=std::chrono::steady_clock::now();
        if(!state->luke_prediction_origin) {
            state->luke_prediction_origin=state->last_inline_frame_state.predictedDisplayTime;
            state->luke_prediction_wall=state->application_wait_returned_at;
        }
        const auto prediction=state->luke_prediction_origin+
            std::chrono::duration_cast<std::chrono::nanoseconds>(now-state->luke_prediction_wall).count();
        frame_state->predictedDisplayTime=(std::max)(prediction,state->last_virtual_display_time+1);
        frame_state->predictedDisplayPeriod=period;
        frame_state->shouldRender=state->last_inline_frame_state.shouldRender;
        state->last_virtual_display_time=frame_state->predictedDisplayTime;
    } else if (use_continuous_presenter) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        std::unique_lock presenter_lock(state->presenter_mutex);
        // This waits only for the initial valid prediction. D3D11's director
        // seeds that prediction at promotion. Non-Luke virtual waits neither
        // sleep nor wait for a new director tick; filtered Luke repeats use
        // the independent bounded clock below. Two display periods describe
        // the intended real/synthetic timeline, not a producer rate limiter
        // or the display period advertised to the three-slot application's AER.
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
        if (state->luke_clock_active && state->three_slot_director) {
            const auto period = std::clamp<XrDuration>(
                application_prediction_period(*state,state->presenter_frame_state.predictedDisplayPeriod),
                2'000'000, 50'000'000);
            if (state->luke_duplicate_cycle) {
                const auto token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::luke_cycle_wait, period);
                // Absolute deadline from the previous Wait, not an extra
                // period after End. No director tick, worker fence, or render
                // counter must advance to release this wait. The condition
                // wait releases presenter_mutex while asleep.
                state->presenter_condition.wait_until(presenter_lock,
                    state->application_wait_returned_at + std::chrono::nanoseconds(period), [&] {
                        return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                            !state->menu_enabled || state->manual_control.stop_requested();
                    });
                const auto wait_result = XR_FAILED(state->presenter_failure) ? state->presenter_failure
                    : state->presenter_stop_requested ? XR_ERROR_SESSION_NOT_RUNNING : XR_SUCCESS;
                xrfg::bridge_flight_logger().end(token,
                    xrfg::BridgeFlightOperation::luke_cycle_wait, wait_result);
                if (XR_FAILED(wait_result)) return wait_result;
                state->luke_duplicate_cycle = false;
            }
            // Keep the established XR epoch and prediction lead, but advance
            // it with real elapsed time even when the director has no output.
            // Unlike the non-Luke ceiling below, an idle director cannot turn
            // repeated app cycles into a nanosecond clock or stop progress.
            const auto now = std::chrono::steady_clock::now();
            const XrTime prediction = state->luke_clock_origin +
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - state->luke_clock_wall_origin).count();
            frame_state->predictedDisplayTime = std::max(prediction, state->last_virtual_display_time + 1);
            state->last_virtual_display_time = frame_state->predictedDisplayTime;
            frame_state->predictedDisplayPeriod = application_prediction_period(
                *state,state->presenter_frame_state.predictedDisplayPeriod);
            frame_state->shouldRender = state->presenter_frame_state.shouldRender;
        } else {
        const XrDuration clock_period = application_prediction_period(
            *state, state->presenter_frame_state.predictedDisplayPeriod);
        const XrDuration virtual_period =
                    state->manual_control.stop_requested() || !state->menu_enabled
                ? clock_period
                : djules_application_period(*state,
                      clock_period);
        // The application's timeline is anchored to the runtime's own
        // prediction, one virtual period ahead of the frame the presenter is
        // about to submit.
        const XrTime anchor = add_display_duration(
            state->presenter_frame_state.predictedDisplayTime,
            virtual_period);
        // A second wait inside one presenter frame has to come back later than
        // the first, so the guard below steps off the last time served. That
        // step invents time the runtime never advanced, and the ceiling is
        // what stops it becoming a clock of its own: every period handed out
        // beyond the anchor is a period the application's prediction runs
        // ahead of the runtime's, and nothing ever gives it back.
        //
        // It is not a corner case. Whenever the layer fails open - no
        // projection layers in the submission, which is what a menu or a
        // loading screen looks like - the application is paced one frame per
        // presenter frame instead of one per pair, while still being handed a
        // doubled period on every one of them. Unbounded, that drifts a full
        // second per second: a captured session reached 637 s of lead,
        // predicting poses ten minutes into the future, and never generated
        // again once it got there, because the ratchet only turns one way.
        // With the ceiling the clock simply ticks at the rate the application
        // is actually being paced at, and re-anchors as soon as the pairing
        // comes back.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = add_display_duration(
                state->last_virtual_display_time,
                virtual_period);
        }
        if (virtual_time > ceiling) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::virtual_clock_clamp,
                0,
                static_cast<std::uint64_t>(virtual_time - ceiling),
                static_cast<std::uint64_t>(ceiling),
                static_cast<std::uint64_t>(
                    state->presenter_frame_state.predictedDisplayTime));
            virtual_time = ceiling;
        }
        // The ceiling can land at or below the time already served, because
        // the anchor tracks a runtime prediction that does not advance by a
        // whole period every time it is read. Never hand the application a
        // display time that does not move: it is not something a caller has
        // to tolerate, and the runtime safeguards added in v0.2.1 read a
        // non-advancing time as a projection resource layout change and stop
        // generation for a second, which the fail-open pacing that follows
        // then reproduces on the next frame.
        //
        // A nanosecond is enough. The drift the ceiling exists to prevent
        // accumulates in whole periods - it reached 637 s of lead in a
        // captured session - so a floor measured in nanoseconds keeps the
        // sequence strictly increasing without giving the ratchet anything
        // to turn on.
        if (state->last_virtual_display_time != 0 &&
            virtual_time <= state->last_virtual_display_time) {
            virtual_time = state->last_virtual_display_time + 1;
        }
        state->last_virtual_display_time = virtual_time;
        frame_state->predictedDisplayTime = virtual_time;
        // The three-slot director owns the two-output cadence. Report the
        // runtime display period to the app, as the inline route does, rather
        // than feeding that doubled cadence into upstream half-rate policies.
        // Keep the prediction anchor/ceiling and all buffer pacing unchanged.
        frame_state->predictedDisplayPeriod = djules_triple_active(*state)?virtual_period:
            (state->three_slot_director || state->fsr_two_slot_presenter)
            ? clock_period : virtual_period;
        frame_state->shouldRender = state->presenter_frame_state.shouldRender;
        }
    } else if (use_provisional_pipelined_wait) {
        if (!valid_optional_frame_info(wait_info, XR_TYPE_FRAME_WAIT_INFO) ||
            frame_state == nullptr ||
            frame_state->type != XR_TYPE_FRAME_STATE) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        const XrDuration virtual_period = (state->manual_control.stop_requested() || !state->menu_enabled)
            ? state->last_inline_frame_state.predictedDisplayPeriod
            : djules_application_period(*state,state->last_inline_frame_state.predictedDisplayPeriod);
        const XrTime anchor = add_display_duration(
            state->last_inline_frame_state.predictedDisplayTime,
            virtual_period);
        // Same ceiling as the presenter path above, for the same reason: this
        // branch repeats for as long as the promotion takes, and each repeat
        // would otherwise push the application's timeline a period further
        // from the one the runtime is predicting on.
        const XrTime ceiling = add_display_duration(anchor, virtual_period);
        XrTime virtual_time = anchor;
        {
            std::scoped_lock presenter_lock(state->presenter_mutex);
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = add_display_duration(
                    state->last_virtual_display_time,
                    virtual_period);
            }
            if (virtual_time > ceiling) {
                virtual_time = ceiling;
            }
            // Same floor as the presenter path, for the same reason: a
            // display time that does not advance is not something a caller
            // has to tolerate.
            if (state->last_virtual_display_time != 0 &&
                virtual_time <= state->last_virtual_display_time) {
                virtual_time = state->last_virtual_display_time + 1;
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
        const auto application_wait_started = std::chrono::steady_clock::now();
        result = state->dispatch->wait_frame(session, wait_info, frame_state);
        const auto application_wait_elapsed =
            std::chrono::steady_clock::now() - application_wait_started;
        if (XR_SUCCEEDED(result) && frame_state != nullptr) {
            state->last_inline_frame_state = *frame_state;
            state->last_inline_frame_state.next = nullptr;
            state->last_inline_frame_state_valid = true;
            state->last_application_wait_elapsed = application_wait_elapsed;
            forwarded_application_wait = true;
            state->runtime_frame_waited_unbegun = true;
        }
    }
    if (XR_SUCCEEDED(result) && frame_state != nullptr) {
        if(blackbar_gate(*state)) {
            XrDuration physical_period=frame_state->predictedDisplayPeriod;
            if (use_continuous_presenter) {
                std::scoped_lock presenter_lock(state->presenter_mutex);
                physical_period=state->presenter_display_period;
            }
            const auto promise_shift=blackbar_promise_shift(*state,frame_state->predictedDisplayTime,
                physical_period,use_continuous_presenter || use_provisional_pipelined_wait);
            if(promise_shift>0) frame_state->predictedDisplayTime=
                add_display_duration(frame_state->predictedDisplayTime,promise_shift);
        }
        state->application_wait_returned_at = std::chrono::steady_clock::now();
        // Do not even enter a synthesizer mutex from the virtual D3D11 wait:
        // the worker can hold it while waiting for GPU completion. Producer
        // ownership is resolved only at the release/EndFrame handoffs.
        if (!adaptive_nvidia_presenter && !(state->luke_fresh_only &&
            (state->afw_only || !xrfg::embedded::snapshot().desired.enabled) &&
            xrfg::embedded::afw_snapshot().desired.mode==0)) {
            if (!state->d3d12_synthesis_queue)
                static_cast<void>(wait_for_previous_session_synthesis(state,true));
            else
                static_cast<void>(flush_session_pending_copies(state));
        }
        state->application_wait_pending_begin = true;
        std::scoped_lock lock(state->mutex);
        // Every runtime, not only SteamVR: the synthetic interpolation fraction
        // needs a display period wherever generation runs, and the promotion
        // that also reads this guards on the runtime itself. A runtime that
        // never reports one keeps the fixed midpoint.
        if (!use_continuous_presenter &&
            frame_state->predictedDisplayPeriod > 0 &&
            (state->minimum_runtime_display_period == 0 ||
             frame_state->predictedDisplayPeriod <
                 state->minimum_runtime_display_period)) {
            state->minimum_runtime_display_period =
                frame_state->predictedDisplayPeriod;
            state->history_capture_wait_period.store(state->minimum_runtime_display_period, std::memory_order_relaxed);
        }
        // A runtime that is pacing this application blocks the wait for most
        // of a display period. SteamVR returns in about 1.55 ms against
        // 11.11 ms, which is not pacing anything. Count the waits that came
        // back too quickly, on the same half-period line the throttle detector
        // uses in the other direction.
        if (forwarded_application_wait &&
            state->minimum_runtime_display_period > 0) {
            const auto elapsed_nanoseconds =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    state->last_application_wait_elapsed).count();
            state->unpaced_wait_streak =
                elapsed_nanoseconds * 2 < state->minimum_runtime_display_period
                    ? state->unpaced_wait_streak + 1
                    : 0;
        }
        if(forwarded_application_wait && djules_triple_active(*state) && !state->manual_control.stop_requested())
            frame_state->predictedDisplayPeriod=djules_application_period(*state,frame_state->predictedDisplayPeriod);
        const std::uint64_t frame_serial = state->next_application_frame_serial++;
        if (interface_call.started_ns()) {
            try { interface_call.cycle = std::make_shared<xrfg::InterfaceCycle>(interface_call.started_ns()); }
            catch (...) {} // Telemetry allocation cannot refuse an application frame.
        }
        state->pending_frames.push_back({
            frame_state->predictedDisplayTime,
            frame_state->predictedDisplayPeriod,
            frame_serial,
            interface_call.cycle,
        });
        state->interface_waited_cycle.store(interface_call.cycle);
        constexpr std::size_t kMaximumPendingFrameRecords = 32;
        while (state->pending_frames.size() > kMaximumPendingFrameRecords) {
            state->pending_frames.pop_front();
        }
        if (state->luke_afw_enabled) {
            // result 1: application wait ticket. a=serial, b=time, c=period.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_frame_contract,
                1, frame_serial,
                static_cast<std::uint64_t>(frame_state->predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod));
            xrfg::two_frame_capture_timeline(
                "frame", 1, frame_serial,
                static_cast<std::uint64_t>(frame_state->predictedDisplayTime),
                static_cast<std::uint64_t>(frame_state->predictedDisplayPeriod));
        }
    }
    return result;
}

XrResult layer_begin_frame_impl(
    XrSession session,
    const XrFrameBeginInfo* begin_info, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 :
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_session(session);
    if (!state || state->dispatch->begin_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    seal_outlaws_initialization(*state);
    initialize_late_luke_context(state);
    if (interface_call.started_ns()) {
        // One unbegun Wait ticket; frame-call admission prevents another Wait
        // from overtaking Begin. An overlapping next Wait cannot replace active.
        interface_call.cycle = state->interface_waited_cycle.load();
        state->interface_active_cycle.store(interface_call.cycle);
    }
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        if (state->luke_fresh_only || continuous_presenter_active(state)) {
            result = valid_optional_frame_info(
                    begin_info, XR_TYPE_FRAME_BEGIN_INFO)
                ? XR_SUCCESS
                : XR_ERROR_VALIDATION_FAILURE;
        } else if (state->pipelined_presenter_mode) {
            result = XR_ERROR_RUNTIME_FAILURE;
        } else {
            XrResult replacement_result = XR_SUCCESS;
            if (state->application_begin_needs_wait) {
                XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
                XrFrameState replacement{XR_TYPE_FRAME_STATE};
                replacement_result = state->dispatch->wait_frame(session, &wait, &replacement);
                if (XR_SUCCEEDED(replacement_result)) {
                    state->application_begin_needs_wait = false;
                    state->last_inline_frame_state = replacement;
                    state->last_inline_frame_state.next = nullptr;
                    state->last_inline_frame_state_valid = true;
                    state->runtime_frame_waited_unbegun = true;
                }
            }
            result = XR_FAILED(replacement_result) ? replacement_result : with_runtime_entry(state, [&] {
                return state->dispatch->begin_frame(session, begin_info);
            });
            if (XR_SUCCEEDED(result)) {
                state->runtime_frame_waited_unbegun = false;
                state->blackbar_inline_slot=state->last_inline_frame_state.predictedDisplayTime;
                state->blackbar_inline_shift=0;
                if(blackbar_gate(*state)) {
                    std::scoped_lock timing_lock(state->blackbar_timing.mutex);
                    state->blackbar_inline_shift=state->blackbar_timing.last_shift;
                }
            }
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
    if (XR_SUCCEEDED(result)) state->interface_begun_ns.store(
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0, std::memory_order_relaxed);
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
    // Bridged and multisampled: the runtime gets a single-sample swapchain
    // and the application a multisampled texture of its own, resolved at
    // release (D3D11BridgePath::resolve). Every path below sees the
    // single-sample create info, which is what the runtime's images are.
    XrSwapchainCreateInfo bridged_create_info{};
    const XrSwapchainCreateInfo* runtime_create_info = create_info;
    if ((state->d3d11_bridge || state->vulkan_bridge) &&
        (create_info->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))
        return XR_ERROR_FEATURE_UNSUPPORTED;
    if (state->d3d11_bridge && create_info->sampleCount > 1) {
        bridged_create_info = *create_info;
        bridged_create_info.sampleCount = 1;
        runtime_create_info = &bridged_create_info;
    }
    // Bridged Vulkan: the application's format is a VkFormat and the
    // runtime's session is D3D12, so the create info the runtime sees
    // carries the DXGI equivalent, and so does everything below, which is
    // what the runtime's images are. A format with no equivalent is refused
    // the way a runtime refuses one it does not list; so is multisampled
    // colour, which the bridge does not resolve.
    if (state->vulkan_bridge) {
        const auto vulkan_format = static_cast<VkFormat>(create_info->format);
        const bool depth =
            (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        const DXGI_FORMAT dxgi_format = depth
            ? xrfg::dxgi_depth_format_for_vulkan(vulkan_format)
            : xrfg::dxgi_format_for_vulkan(vulkan_format);
        if (dxgi_format == DXGI_FORMAT_UNKNOWN ||
            (!depth && create_info->sampleCount > 1)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                4,
                static_cast<std::uint64_t>(create_info->format),
                create_info->usageFlags,
                create_info->sampleCount);
            return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        }
        bridged_create_info = *create_info;
        bridged_create_info.format = static_cast<std::int64_t>(dxgi_format);
        // A Vulkan application need not render into the swapchain image:
        // OpenComposite copies the game's own texture in, so No Man's Sky's
        // swapchains carry only TRANSFER_DST. The runtime's image and the
        // shared texture are colour attachments whatever was asked, which
        // is what makes the swapchain a colour one to every path below.
        if (!depth) {
            bridged_create_info.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        }
        // The runtime's depth images are never written (depth_private), so
        // they are the cheapest shape that creates.
        if (depth) {
            bridged_create_info.sampleCount = 1;
        }
        runtime_create_info = &bridged_create_info;
    }
    auto swapchain_state = std::make_shared<SwapchainState>(state, *runtime_create_info);
    XrSwapchain created_swapchain = XR_NULL_HANDLE;
    XrResult result = state->dispatch->create_swapchain(
        session,
        runtime_create_info,
        &created_swapchain);
    if((result==XR_ERROR_LIMIT_REACHED||result==XR_ERROR_OUT_OF_MEMORY)&&
        state->luke_outlaws_preframe&&!state->luke_outlaws_frame_started&&state->luke_outlaws_eager_budget) {
        // Initial application creations have priority over our eager private
        // rings. Retire only those owned rings, then retry the unchanged call.
        state->generation_budget_exhausted.store(true,std::memory_order_release);
        release_session_generation_budget(state);
        state->luke_outlaws_eager_budget=false;
        created_swapchain=XR_NULL_HANDLE;
        result=state->dispatch->create_swapchain(session,runtime_create_info,&created_swapchain);
    }
    if (XR_FAILED(result)) {
        // The application losing a swapchain of its own is the shape a layer
        // that overspends the runtime's budget takes from the outside, and
        // without this record the log ends at the last creation that worked.
        // A negative result distinguishes it from the create-info records
        // below, which carry a field index there.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::swapchain_create,
            result,
            0,
            (static_cast<std::uint64_t>(create_info->width) << 32) |
                create_info->height,
            static_cast<std::uint64_t>(create_info->usageFlags));
        return result;
    }
    swapchain_state->handle = created_swapchain;
    log_video_memory(*state, VideoMemoryStage::application_swapchain, handle_value(created_swapchain));

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
    if (state->d3d11_bridge) {
        // Bridge it now, while the runtime's images can be enumerated
        // without the application seeing D3D12 ones: the application's
        // enumeration is answered from the bridge. A shape the bridge cannot
        // share fails the creation, which the application sees as a
        // swapchain it could not create.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::D3D11BridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::D3D11BridgeSwapchainDescription requested{};
            requested.requested_format =
                static_cast<DXGI_FORMAT>(create_info->format);
            requested.requested_sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->d3d11_bridge->device.Get(),
                state->d3d11_bridge->context.Get(),
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        // 2: bridged. b the image count and, in the high half, the path
        // (D3D11BridgePath); c the requested format and, in the high half,
        // the format the shared textures took from the runtime's images.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::d3d11_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::D3D11BridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->bridge = std::move(bridge);
    } else if (state->vulkan_bridge) {
        // Bridge it now, as for D3D11: the application's enumeration is
        // answered from the bridge's imported images.
        std::uint32_t runtime_count = 0;
        std::vector<XrSwapchainImageD3D12KHR> runtime_images;
        XrResult bridge_result = state->dispatch->enumerate_swapchain_images(
            created_swapchain, 0, &runtime_count, nullptr);
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            runtime_images.assign(
                runtime_count,
                XrSwapchainImageD3D12KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            bridge_result = state->dispatch->enumerate_swapchain_images(
                created_swapchain, runtime_count, &runtime_count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(runtime_images.data()));
        }
        HRESULT shared_result = E_FAIL;
        std::uint32_t failure_stage = 0;
        auto bridge = std::make_shared<xrfg::VulkanBridgeSwapchain>();
        if (XR_SUCCEEDED(bridge_result) && runtime_count > 0) {
            std::vector<ID3D12Resource*> resources;
            for (const XrSwapchainImageD3D12KHR& image : runtime_images) {
                resources.push_back(image.texture);
            }
            xrfg::VulkanBridgeSwapchainDescription requested{};
            requested.requested_format = static_cast<VkFormat>(create_info->format);
            requested.width = create_info->width;
            requested.height = create_info->height;
            requested.array_size = create_info->arraySize;
            requested.mip_levels = create_info->mipCount;
            requested.sample_count = create_info->sampleCount;
            requested.unordered_access =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) != 0;
            requested.depth_stencil =
                (create_info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
            shared_result = bridge->initialize(
                state->vulkan_bridge->binding,
                state->d3d12_device.Get(),
                state->d3d12_queue.Get(),
                resources,
                requested,
                &failure_stage);
        }
        if (XR_FAILED(bridge_result) || FAILED(shared_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::vulkan_bridge,
                XR_FAILED(bridge_result)
                    ? static_cast<std::int64_t>(bridge_result)
                    : static_cast<std::int64_t>(shared_result),
                handle_value(created_swapchain),
                failure_stage,
                static_cast<std::uint64_t>(create_info->format));
            {
                std::scoped_lock lock(g_state_mutex);
                g_swapchains.erase(created_swapchain);
            }
            state->dispatch->destroy_swapchain(created_swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::vulkan_bridge,
            2,
            handle_value(created_swapchain),
            (static_cast<std::uint64_t>(bridge->path()) << 32) | runtime_count,
            (static_cast<std::uint64_t>(bridge->shared_format()) << 32) |
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(create_info->format)));
        if (bridge->path() == xrfg::VulkanBridgePath::depth_private) {
            state->has_private_depth_swapchain.store(true, std::memory_order_release);
            swapchain_state->private_depth = true;
        }
        std::scoped_lock lock(swapchain_state->mutex);
        swapchain_state->vulkan_bridge = std::move(bridge);
    }
    *swapchain = created_swapchain;
    // V443: creation alone does not invalidate existing frame resources.
    // The lifetime guard remains; first projection use resets continuity and
    // passes through before lazy arming. Keep other structural quarantines.
    return result;
}

XrResult layer_destroy_swapchain_impl(XrSwapchain swapchain) {
    const auto state = find_swapchain(swapchain);
    if (!state || state->session->dispatch->destroy_swapchain == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    xrfg::unregister_d3d11_sr_destinations(reinterpret_cast<std::uintptr_t>(swapchain));

    PresenterResourceLifetimeGuard presenter_guard(state->session);
    const bool active_color_reconfiguration =
        state->session->generation_steady_state_established &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
        (state->create_info.usageFlags &
         XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0;
    if (active_color_reconfiguration) {
        // Drop the pair and the retained repeat - the repeat's composition
        // names this swapchain - but do not hold generation off for the
        // structural quarantine's full second.
        //
        // The wall-clock deadline was covering a race that lazy arming has
        // since removed. The lines below drain this swapchain's GPU work and
        // destroy its private swapchains synchronously, under the call lock
        // and with the presenter held by the guard above, so nothing queued
        // names anything being torn down by the time this returns. What the
        // deadline additionally prevented was the layer arming the
        // replacement swapchains the moment they were enumerated, mid
        // reconfiguration; generation resources are now taken when a
        // projection layer first names a swapchain, so the application
        // decides when the replacement is ready and the layer cannot run
        // ahead of it.
        //
        // It is not free to keep. MSFS 2024 recreates its swapchains on world
        // and settings transitions - four times in 96 s in one capture - and
        // each one cost almost exactly a second of generation: the
        // replacements were enumerated within 40 ms and armed 1.07 s later,
        // the delay being the deadline and nothing else. Every other
        // quarantine reason is unchanged.
        clear_generation_continuity(state->session);
        {
            std::scoped_lock presenter_lock(state->session->presenter_mutex);
            state->session->presenter_last_frame.reset();
        }
        state->session->generation_steady_state_established = false;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::continuity_reset,
            static_cast<std::int64_t>(
                GenerationQuarantineReason::swapchain_destroyed),
            handle_value(state->session->handle),
            handle_value(swapchain),
            0);
    }
    std::scoped_lock call_lock(state->call_mutex);
    drain_swapchain_gpu(state);
    destroy_composition_capture(state);
    destroy_frame_generation_swapchains(state);
    const XrResult result = state->session->dispatch->destroy_swapchain(swapchain);
    if (XR_SUCCEEDED(result)) {
        { std::scoped_lock lock(state->mutex); state->bridge.reset(); state->vulkan_bridge.reset(); }
        { std::scoped_lock lock(state->session->logged_view_rects_mutex); state->session->logged_view_rects.erase(swapchain); }
        std::scoped_lock lock(g_state_mutex);
        g_swapchains.erase(swapchain);
    }
    log_video_memory(*state->session, VideoMemoryStage::swapchain_destroyed, handle_value(swapchain));
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
    std::unique_lock call_lock(state->call_mutex);
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
    }
    std::vector<ID3D12Resource*> bridged_resources;
    XrResult result = XR_SUCCESS;
    if (vulkan_bridge) {
        // The application gets the bridge's imported Vulkan images, never
        // the runtime's D3D12 ones; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto vulkan_views = vulkan_bridge->vulkan_images();
        const auto count = static_cast<std::uint32_t>(vulkan_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* vulkan_images = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (vulkan_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            vulkan_images[index].image = vulkan_views[index];
        }
        const auto shared = vulkan_bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else if (bridge) {
        // The application gets the bridge's D3D11 textures, never the
        // runtime's D3D12 images; the D3D12 bookkeeping below sees the
        // shared textures, which is what history captures from.
        const auto d3d11_views = bridge->d3d11_images();
        const auto count = static_cast<std::uint32_t>(d3d11_views.size());
        if (image_count_output == nullptr) {
            return XR_ERROR_VALIDATION_FAILURE;
        }
        *image_count_output = count;
        if (image_capacity_input == 0 || images == nullptr) {
            return XR_SUCCESS;
        }
        if (image_capacity_input < count) {
            return XR_ERROR_SIZE_INSUFFICIENT;
        }
        auto* d3d11_images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for (std::uint32_t index = 0; index < count; ++index) {
            if (d3d11_images[index].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
                return XR_ERROR_VALIDATION_FAILURE;
            }
            d3d11_images[index].texture = d3d11_views[index];
        }
        const auto shared = bridge->shared_images();
        bridged_resources.assign(shared.begin(), shared.end());
    } else {
        result = state->session->dispatch->enumerate_swapchain_images(
            swapchain,
            image_capacity_input,
            image_count_output,
            images);
        if (XR_FAILED(result) || image_count_output == nullptr || images == nullptr || image_capacity_input == 0) {
            return result;
        }
    }

    try {
        // Register only the actual application-visible XR color images. The
        // native upscaler resolves its destination at that exact eye draw;
        // it never borrows a global recommendation or a different swapchain.
        if ((bridge || state->session->graphics_binding == SessionGraphicsBinding::d3d11) &&
            (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            !(state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            state->create_info.arraySize==1 && state->create_info.sampleCount==1 &&
            image_count_output && images && image_capacity_input>=*image_count_output) {
            std::vector<ID3D11Texture2D*> sr_images;
            const auto* app_images=reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
            for (std::uint32_t i=0;i<*image_count_output;++i) {
                if (app_images[i].type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR || !app_images[i].texture) {sr_images.clear();break;}
                sr_images.push_back(app_images[i].texture);
            }
            if (!sr_images.empty()) xrfg::register_d3d11_sr_destinations(
                reinterpret_cast<std::uintptr_t>(state->session.get()),reinterpret_cast<std::uintptr_t>(swapchain),
                sr_images.data(),sr_images.size());
        }
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
                destroy_composition_capture(state);
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
            // Same deferral as the D3D12 path below, and for the same reason:
            // an interop swapchain costs the session three runtime swapchains
            // and an application's UI surfaces are indistinguishable from its
            // stereo views here. The D3D12 path can rebuild its images from
            // the history ring when it arms; this one has to keep them.
            const bool static_image =
                (state->create_info.createFlags &
                 XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
            if (!has_generation && !state->generation_declined &&
                !state->session->generation_budget_exhausted.load(
                    std::memory_order_acquire)) {
                if (static_image) {
                    eligibility_reason =
                        SwapchainEligibilityReason::static_image;
                    eligibility_detail = state->create_info.createFlags;
                } else if (state->create_info.faceCount != 1) {
                    eligibility_reason =
                        SwapchainEligibilityReason::unsupported_face_count;
                    eligibility_detail = state->create_info.faceCount;
                } else {
                    state->enumerated_image_count = count;
                    state->generation_eligible_pending = true;
                    eligibility_reason =
                        SwapchainEligibilityReason::awaiting_projection_use;
                }
            }
            log_swapchain_eligibility(
                state,
                has_generation ? SwapchainEligibilityReason::ready
                               : eligibility_reason,
                eligibility_detail,
                (static_cast<std::uint64_t>(count) << 32) |
                    state->create_info.arraySize);
            if(state->session->luke_outlaws_preframe&&!state->session->luke_outlaws_frame_started&&
                state->session->luke_afw_enabled&&state->generation_eligible_pending&&
                !state->session->generation_budget_exhausted.load(std::memory_order_acquire)) {
                // The lifetime guard already owns frame_call_mutex. Only this
                // call lock must be released: both existing setup helpers take
                // their own swapchain locks. The original enum images/count and
                // its result remain the application's, including on refusal.
                call_lock.unlock();
                initialize_late_luke_context(state->session);
                if(!ensure_frame_generation(state)) {
                    state->session->generation_budget_exhausted.store(true,std::memory_order_release);
                    release_session_generation_budget(state->session);
                    state->session->luke_outlaws_eager_budget=false;
                } else {
                    std::scoped_lock lock(state->mutex);
                    state->session->luke_outlaws_eager_budget|=bool(state->frame_generation);
                }
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    (state->session->graphics_binding_capabilities&8ULL)?S_OK:HRESULT_FROM_WIN32(ERROR_NOT_READY),
                    1001,handle_value(state->handle),state->session->luke_outlaws_eager_budget);
            }
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
        if (bridge || vulkan_bridge) {
            resources = bridged_resources;
        }
        for (std::uint32_t index = 0; index < count && !bridge && !vulkan_bridge; ++index) {
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
        log_d3d12_image_description(state->handle, resources.front());
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
            destroy_composition_capture(state);
            schedule_generation_quarantine(
                state->session,
                GenerationQuarantineReason::d3d12_images_changed,
                handle_value(state->handle));
        }
        const auto release_state = required_release_state(*state);
        const bool depth_only = release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE;
        // D24 (djules V405-V406): projection synthesis never reads a depth
        // swapchain's color-history ring. Keep the reference allocation intact.
        // Captured native depth guides and runtime images remain independent.
        const bool omit_depth_history = depth_only && state->session->nvidia_new_route;
        const bool reused_history =
            !omit_depth_history && !resources_changed && history && history->initialized();
        if (!reused_history) {
            history.reset();
        }
        const bool protected_content =
            (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) != 0;
        HRESULT history_result = S_OK;
        bool history_attempted = false;
        if (!history && release_state && !omit_depth_history && !protected_content) {
            history_attempted = true;
            auto candidate = std::make_shared<xrfg::D3D12SwapchainHistory>();
            history_result = candidate->initialize(
                state->session->d3d12_device.Get(),
                state->session->d3d12_queue.Get(),
                std::span<ID3D12Resource* const>(resources.data(), resources.size()),
                *release_state);
            if (SUCCEEDED(history_result)) {
                history = std::move(candidate);
                log_video_memory(*state->session, VideoMemoryStage::history, handle_value(state->handle));
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
            // The images themselves changed, so an earlier refusal says
            // nothing about this set.
            state->generation_declined = false;
            state->generation_refusal = SwapchainEligibilityReason::ready;
            state->generation_refusal_detail = 0;
        }
        state->enumerated_image_count = count;

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
        } else if (depth_only && omit_depth_history) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        } else if (!history) {
            eligibility_reason = SwapchainEligibilityReason::history_initialize_failed;
            eligibility_detail = history_attempted
                ? static_cast<std::uint64_t>(history_result)
                : 0;
        } else if (*release_state == D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            eligibility_reason = SwapchainEligibilityReason::depth_only;
            eligibility_detail = state->create_info.usageFlags;
        }
        // Generation costs three runtime swapchains here and a runtime caps how
        // many one session may hold at all. Nothing about a colour swapchain
        // says whether the application will submit it as a projection view or
        // as a UI quad it composites once, so spending the budget now spends it
        // on both -- and the application, still creating its own swapchains,
        // is the one that finds the ceiling. Record the swapchain as a
        // candidate and let the first xrEndFrame that names it decide.
        //
        // What the create info alone settles is still settled here. A static
        // image and a face count above one can never carry generation, so
        // deferring them would put a candidacy in the log that is never
        // resolved in place of the true reason, and leave an arming attempt to
        // discover at the first frame what was knowable at creation.
        const bool static_image =
            (state->create_info.createFlags &
             XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0;
        if (history && release_state == D3D12_RESOURCE_STATE_RENDER_TARGET &&
            !has_generation && !state->generation_declined &&
            !state->session->generation_budget_exhausted.load(
                std::memory_order_acquire)) {
            if (static_image) {
                eligibility_reason = SwapchainEligibilityReason::static_image;
                eligibility_detail = state->create_info.createFlags;
            } else if (state->create_info.faceCount != 1) {
                eligibility_reason =
                    SwapchainEligibilityReason::unsupported_face_count;
                eligibility_detail = state->create_info.faceCount;
            } else {
                state->generation_eligible_pending = true;
                eligibility_reason =
                    SwapchainEligibilityReason::awaiting_projection_use;
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
    std::uint32_t* index, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 :
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_swapchain(swapchain);
    if (state && interface_call.started_ns()) {
        interface_call.cycle = state->session->interface_active_cycle.load();
    }
    if (!state || state->session->dispatch->acquire_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    release_pending_bridge_image_locked(state);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->acquire_swapchain_image(
            swapchain, acquire_info, index);
    });
    if (XR_SUCCEEDED(result) && index != nullptr) {
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid) {
            try {
                state->acquired_indices.push_back(*index);
            } catch (...) {
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->ownership_tracking_valid = false;
            }
        }
        // Synchronized-calls mode: the queue work before the application's
        // rendering belongs to this call, not to xrWaitSwapchainImage.
        if (state->vulkan_bridge && FAILED(state->vulkan_bridge->after_acquire(*index)))
            return XR_ERROR_RUNTIME_FAILURE;
    }
    return result;
}

XrResult layer_wait_swapchain_image_impl(
    XrSwapchain swapchain,
    const XrSwapchainImageWaitInfo* wait_info, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 :
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_swapchain(swapchain);
    if (state && interface_call.started_ns()) {
        interface_call.cycle = state->session->interface_active_cycle.load();
    }
    if (!state || state->session->dispatch->wait_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    std::scoped_lock call_lock(state->call_mutex);
    release_pending_bridge_image_locked(state);
    const XrResult result = with_runtime_entry(state->session.get(), [&] {
        return state->session->dispatch->wait_swapchain_image(
            swapchain, wait_info);
    });
    if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING) {
        carry_reacquire_to_application(state->session.get());
        std::scoped_lock lock(state->mutex);
        if (state->ownership_tracking_valid &&
            state->waited_count < state->acquired_indices.size()) {
            const auto image = state->acquired_indices[state->waited_count++];
            if (state->bridge && FAILED(state->bridge->before_write(image)))
                return XR_ERROR_RUNTIME_FAILURE;
            if (state->vulkan_bridge && FAILED(state->vulkan_bridge->before_write(image)))
                return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    return result;
}

// Called at the intercepted release, before forwarding ownership to OpenXR.
// Eagerly snapshot not-yet-classified color chains so a static HUD released
// once at startup is also available when EndFrame first identifies it.
[[nodiscard]] HRESULT capture_composition_image(
    const std::shared_ptr<SwapchainState>& state, std::uint32_t index) {
    if (state->frame_generation && !state->composition_referenced && state->composition_capture)
        state->composition_capture->valid = false;
    if ((state->session->graphics_binding != SessionGraphicsBinding::d3d11 &&
         state->session->graphics_binding != SessionGraphicsBinding::d3d12) ||
        (state->frame_generation && !state->composition_referenced) ||
        !(state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ||
        (state->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        (state->create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return S_FALSE;
    if (state->session->graphics_binding == SessionGraphicsBinding::d3d12) {
        if (index >= state->enumerated_d3d12_images.size()) return E_INVALIDARG;
        if (!state->composition_capture) {
            auto capture = std::make_unique<CompositionCapture>();
            auto desc = state->enumerated_d3d12_images[index]->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            HRESULT result = state->session->d3d12_device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&capture->texture12));
            if (SUCCEEDED(result)) result = state->session->d3d12_device->CreateFence(0,
                D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&capture->fence12));
            if (FAILED(result)) return result;
            state->composition_capture = std::move(capture);
        }
        auto& capture = *state->composition_capture;
        capture.valid = false;
        auto& copy = capture.copies12[capture.next_copy12++ % capture.copies12.size()];
        return copy_composition12(state->session->d3d12_device.Get(), state->session->d3d12_queue.Get(),
            capture.fence12.Get(), capture.fence_value, copy,
            capture.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
            state->enumerated_d3d12_images[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (index >= state->enumerated_d3d11_images.size()) return E_INVALIDARG;
    if (!state->composition_capture) {
        auto capture = std::make_unique<CompositionCapture>();
        Microsoft::WRL::ComPtr<ID3D11Device5> device;
        HRESULT result = state->session->d3d11_device.As(&device);
        if (SUCCEEDED(result)) result = state->session->d3d11_context.As(&capture->context);
        if (FAILED(result)) return result;
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        result = capture->context.As(&multithread);
        if (FAILED(result)) return result;
        multithread->SetMultithreadProtected(TRUE);
        D3D11_TEXTURE2D_DESC desc{};
        state->enumerated_d3d11_images[index]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
        desc.MiscFlags &= D3D11_RESOURCE_MISC_TEXTURECUBE;
        result = device->CreateTexture2D(&desc, nullptr, &capture->texture);
        if (SUCCEEDED(result)) result = device->CreateFence(0, D3D11_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&capture->fence));
        if (FAILED(result)) return result;
        state->composition_capture = std::move(capture);
    }
    auto& capture = *state->composition_capture;
    capture.valid = false;
    capture.context->CopyResource(capture.texture.Get(), state->enumerated_d3d11_images[index].Get());
    const HRESULT result = capture.context->Signal(capture.fence.Get(), capture.fence_value + 1);
    if (SUCCEEDED(result)) ++capture.fence_value;
    capture.context->Flush();
    return result;
}

[[nodiscard]] constexpr bool should_skip_luke_fsr_capture(
    bool luke_split_eye,
    xrfg::D3D12FrameGenerationMode mode,
    bool has_fresh_guides) noexcept {
    return luke_split_eye && mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        !has_fresh_guides;
}

[[nodiscard]] constexpr bool should_latch_luke_stereo_guides(
    std::uint64_t newest_publication,
    std::uint64_t previous_newest_publication) noexcept {
    return newest_publication > previous_newest_publication &&
        (previous_newest_publication == 0 ||
         newest_publication - previous_newest_publication >= 2);
}

[[nodiscard]] constexpr bool should_evaluate_luke_afw_publication(
    std::uint64_t candidate, std::uint64_t last_evaluated) noexcept {
    return candidate != 0 && candidate > last_evaluated;
}

XrResult release_swapchain_image_locked(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainImageReleaseInfo* release_info,
    std::optional<xrfg::LukeRenderIdentity> luke_identity) {
    const auto swapchain=state->handle;
    std::optional<std::uint32_t> candidate_index;
    std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
    std::shared_ptr<xrfg::D3D11BridgeSwapchain> bridge;
    std::shared_ptr<xrfg::VulkanBridgeSwapchain> vulkan_bridge;
    std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> d3d11_interop;
    {
        std::scoped_lock lock(state->mutex);
        bridge = state->bridge;
        vulkan_bridge = state->vulkan_bridge;
        if (state->ownership_tracking_valid && state->waited_count > 0 &&
            !state->acquired_indices.empty()) {
            candidate_index = state->acquired_indices.front();
            history = state->d3d12_history;
            if (state->frame_generation) {
                d3d11_interop = state->frame_generation->d3d11_interop;
            }
        }
    }

    // Bridged: the application's rendering goes to the runtime's image now,
    // whatever generation does with it afterwards. A failed copy is logged
    // and the release still goes down, so the runtime shows its last content
    // rather than the session failing.
    if (bridge && candidate_index) {
        const HRESULT copy_result = bridge->release(*candidate_index);
        if (SUCCEEDED(copy_result) && state->session->luke_afw_enabled &&
            (state->create_info.usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            !(state->create_info.usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            !(state->create_info.createFlags&XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) &&
            xrfg::two_frame_capture_claim_application(handle_value(state->handle))) {
            xrfg::TwoFrameImageInfo original{};
            original.stage="xr_application_original";
            original.swapchain=handle_value(state->handle);
            original.capture_serial=*candidate_index; // Actual acquired image index, not a guessed frame ID.
            original.eye=state->dlss_logical_eye_index?static_cast<int>(*state->dlss_logical_eye_index):-1;
            original.width=state->create_info.width;original.height=state->create_info.height;
            const auto diagnostic=bridge->diagnostic_capture_application(*candidate_index,original);
            xrfg::two_frame_capture_timeline("original_image",diagnostic,
                handle_value(state->handle),0,*candidate_index);
        }
        if (FAILED(copy_result)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_bridge,
                3,
                handle_value(state->handle),
                *candidate_index,
                static_cast<std::uint64_t>(static_cast<std::uint32_t>(copy_result)));
        }
    }

    if (vulkan_bridge && candidate_index) {
        const auto guide_cutoff=xrfg::native_vulkan::guide_capture_cutoff();
        const HRESULT copied = vulkan_bridge->release(*candidate_index);
        if (FAILED(copied)) {
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::vulkan_bridge,
                copied, handle_value(state->handle), *candidate_index, 3);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        // The D3D12 queue now waits for the game's: the DLSS guides the game
        // copied this frame are published behind that wait, before this
        // release's guide snapshot below.
        xrfg::native_vulkan::flush_guide_capture(guide_cutoff);
    }

    const bool idle_luke = state->session->luke_fresh_only &&
        (!state->session->luke_source_filter_armed ||
         ((state->session->afw_only || !xrfg::embedded::snapshot().desired.enabled) &&
          xrfg::embedded::afw_snapshot().desired.mode == 0));
    if (idle_luke) { history.reset(); d3d11_interop.reset(); }
    // Worker handoff precedes call_mutex on both APIs. The synchronous/pipelined
    // routes still need their own ownership gate, but never enter it from the director.
    // A queued Reference output owns its history leases and exact completion
    // ticket. With a separate XR queue, an unrelated next capture must not
    // wait for the whole preceding pair. History-slot/destination/work-slot
    // reuse still enforce their own fences; S/B remain bounded and ordered.
    const bool overlap_reference_capture =
        steamvr_reference_capture_overlap(*state->session) &&
        state->session->queued_nvidia_presenter && state->session->binding_queue &&
        state->session->binding_queue.Get() != state->session->d3d12_queue.Get();
    if (candidate_index && history && state->session->d3d12_synthesis_queue &&
        !state->session->three_slot_director && !overlap_reference_capture) {
        static_cast<void>(wait_for_previous_session_synthesis(state->session));
    }
    std::unique_lock<std::mutex> gpu_lock;
    const bool captures_needed=xrfg::embedded::snapshot().desired.enabled ||
        (state->session->luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode!=0);
    if (state->session->manual_control.stop_requested() || !captures_needed ||
        !state->projection_history_needed.load(std::memory_order_acquire)) {
        // Preserve the application's acquire/wait/release bookkeeping, but
        // stop recording new bridge history after the manual arm is revoked.
        history.reset();
        d3d11_interop.reset();
    }
    if (candidate_index && (history || state->session->graphics_binding == SessionGraphicsBinding::d3d11 ||
                           state->session->graphics_binding == SessionGraphicsBinding::d3d12)) {
        gpu_lock = std::unique_lock<std::mutex>(state->session->gpu_mutex);
    }

    const bool release_bound_guides = release_bound_projection_guides(*state->session);
    if (candidate_index && !bridge && state->session->luke_afw_enabled &&
        state->session->graphics_binding==SessionGraphicsBinding::d3d11 &&
        *candidate_index<state->enumerated_d3d11_images.size() &&
        (state->create_info.usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
        !(state->create_info.usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
        !(state->create_info.createFlags&XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) &&
        xrfg::two_frame_capture_claim_application(handle_value(state->handle))) {
        xrfg::TwoFrameImageInfo original{};original.stage="xr_application_original_d11";
        original.swapchain=handle_value(state->handle);original.capture_serial=*candidate_index;
        original.eye=state->dlss_logical_eye_index?static_cast<int>(*state->dlss_logical_eye_index):-1;
        original.width=state->create_info.width;original.height=state->create_info.height;
        const auto result=xrfg::two_frame_capture_record_d3d11(original,state->session->d3d12_device.Get(),
            state->session->d3d11_context.Get(),state->enumerated_d3d11_images[*candidate_index].Get());
        xrfg::two_frame_capture_timeline("original_image_d11",result,handle_value(state->handle),0,*candidate_index);
    }
    const auto release_publication = release_bound_guides && candidate_index
        ? xrfg::dlss_motion_vector_publications() : 0;
    const HRESULT composition_result = candidate_index && !idle_luke && captures_needed
        ? capture_composition_image(state, *candidate_index) : S_FALSE;

    std::optional<xrfg::D3D12HistoryCaptureTicket> pending_capture;
    std::shared_ptr<const xrfg::DlssMotionVectorSet> pending_motion_vectors;
    if (candidate_index && history) {
        const bool luke_split_eye = d3d11_interop &&
            (state->session->graphics_binding_capabilities & 8ULL) != 0 &&
            state->dlss_logical_eye_count == 2 &&
            state->dlss_logical_eye_index.has_value();
        const bool luke_private_stereo_guides = luke_split_eye &&
            state->session->frame_generation_mode ==
                xrfg::D3D12FrameGenerationMode::fsr3;
        // AFW resolves the guide at xrEndFrame against the eye actually
        // released on this application frame. A release-time pair may trail
        // the real colour by one or more NGX publications.
        if (state->session->dlss_motion_vectors &&
            !(luke_split_eye && state->session->luke_afw_enabled)) {
            std::scoped_lock lock(state->mutex);
            auto* application_d3d11_device = state->session->d3d11_bridge
                ? state->session->d3d11_bridge->device.Get() : state->session->d3d11_device.Get();
            if ((d3d11_interop || bridge) && xrfg::d3d11_guides_claimed(application_d3d11_device)) {
                // Resolve this actual eye texture. The returned one-eye set is
                // already selected, including right-eye logical index 1.
                if (bridge && *candidate_index < bridge->d3d11_images().size())
                    pending_motion_vectors = xrfg::resolve_d3d11_dlss_guides(
                        bridge->d3d11_images()[*candidate_index],
                        state->session->d3d12_queue.Get());
                else if (!bridge && *candidate_index < state->enumerated_d3d11_images.size())
                    pending_motion_vectors = xrfg::resolve_d3d11_dlss_guides(
                        state->enumerated_d3d11_images[*candidate_index].Get(),
                        state->session->d3d12_queue.Get());
                static std::atomic<unsigned> diagnostic_samples{0};
                if (diagnostic_samples.fetch_add(1,std::memory_order_relaxed)<32) {
                    const auto frame=pending_motion_vectors ? pending_motion_vectors->eyes[0] : nullptr;
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::d3d11_bridge,
                        frame?S_OK:S_FALSE,811,handle_value(state->handle),frame?frame->publication:0);
                }
            } else if (*candidate_index < state->enumerated_d3d12_images.size()) {
                const int eye = state->session->luke_afw_enabled ? -1 :
                    state->projection_eye.load(std::memory_order_relaxed);
                pending_motion_vectors = xrfg::resolve_dlss_motion_vectors(
                    state->enumerated_d3d12_images[*candidate_index].Get(),
                    state->session->d3d12_queue.Get(),
                    eye, release_publication,
                    release_bound_guides && (eye == 0 || eye == 1)
                        ? state->session->eye_release_publication[1 - eye].load(
                              std::memory_order_relaxed) : 0,
                    strict_fsr_release_guides(*state->session)
                        ? xrfg::DlssReleasedEyeIdentity{true, state->fsr_eye_identity.stream, state->fsr_eye_identity.epoch}
                        : xrfg::DlssReleasedEyeIdentity{});
            } else if (d3d11_interop) {
                std::shared_ptr<const xrfg::DlssMotionVectorSet> resolved;
                if (luke_split_eye) {
                    std::scoped_lock guide_lock(
                        state->session->luke_guide_mutex);
                    if (!state->session->luke_frame_guide_resolved) {
                        state->session->luke_frame_guide_resolved = true;
                        auto candidate = luke_private_stereo_guides
                            ? xrfg::resolve_luke_alternating_guides(
                                  state->session->d3d12_queue.Get())
                            : xrfg::resolve_dlss_motion_vectors(
                                  state->dlss_logical_eye_count,
                                  state->session->d3d12_queue.Get());
                        std::uint64_t newest_publication = 0;
                        if (candidate) {
                            for (std::uint32_t eye = 0;
                                 eye < candidate->eye_count; ++eye) {
                                if (candidate->eyes[eye]) {
                                    newest_publication = std::max(
                                        newest_publication,
                                        candidate->eyes[eye]->publication);
                                }
                            }
                        }
                        if (candidate &&
                            (luke_private_stereo_guides
                                ? should_latch_luke_stereo_guides(
                                      newest_publication,
                                      state->session->
                                          luke_last_consumed_guide_publication)
                                : newest_publication > state->session->
                                      luke_last_consumed_guide_publication)) {
                            state->session->luke_last_consumed_guide_publication =
                                newest_publication;
                            state->session->luke_frame_motion_vectors =
                                std::move(candidate);
                        }
                    }
                    resolved = state->session->luke_frame_motion_vectors;
                } else {
                    resolved = xrfg::resolve_dlss_motion_vectors(
                        state->dlss_logical_eye_count,
                        state->session->d3d12_queue.Get());
                }
                if (resolved && state->dlss_logical_eye_index) {
                    // The phase is a title-specific calibration, not a
                    // global NGX contract.
                    const bool newest_is_left = luke_private_stereo_guides &&
                        resolved->eye_count == 2 && resolved->eyes[1] &&
                        (((resolved->eyes[1]->serial & 1U) != 0) ==
                         state->session->luke_afw_odd_serial_is_left);
                    const auto eye = luke_private_stereo_guides
                        ? (*state->dlss_logical_eye_index ==
                                (newest_is_left ? 0U : 1U) ? 1U : 0U)
                        : *state->dlss_logical_eye_index;
                    if (eye < resolved->eye_count && resolved->eyes[eye]) {
                        auto selected =
                            std::make_shared<xrfg::DlssMotionVectorSet>();
                        selected->eye_count = 1;
                        selected->eyes[0] = resolved->eyes[eye];
                        pending_motion_vectors = std::move(selected);
                    }
                } else {
                    pending_motion_vectors = std::move(resolved);
                }
            }
        }
        const bool skip_capture =
            should_skip_luke_fsr_capture(
                luke_split_eye,
                state->session->frame_generation_mode,
                static_cast<bool>(pending_motion_vectors));
        if (skip_capture) {
            if (xrfg::two_frame_capture_active()) {
                xrfg::TwoFrameImageInfo missing{};
                missing.stage = "xr_final_eye_skipped_no_guide";
                missing.swapchain = handle_value(state->handle);
                missing.eye = state->dlss_logical_eye_index
                    ? static_cast<std::int32_t>(
                        *state->dlss_logical_eye_index)
                    : -1;
                missing.width = state->create_info.width;
                missing.height = state->create_info.height;
                xrfg::two_frame_capture_note(missing,
                    HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            }
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::d3d11_capture,
                S_FALSE,
                handle_value(state->handle),
                *candidate_index,
                2);
        } else {
            xrfg::D3D12HistoryCaptureTicket ticket{};
            HRESULT capture_result = S_OK;
            if (d3d11_interop) {
                const auto interop_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_capture,
                    handle_value(state->handle),
                    *candidate_index,
                    0);
                capture_result =
                    d3d11_interop->prepare_capture(*candidate_index);
                xrfg::bridge_flight_logger().end(
                    interop_token,
                    xrfg::BridgeFlightOperation::d3d11_capture,
                    capture_result,
                    handle_value(state->handle),
                    *candidate_index,
                    0);
            }
            if (SUCCEEDED(capture_result)) {
                xrfg::TwoFrameImageInfo diagnostic_image{};
                const bool capture_image = xrfg::two_frame_capture_active();
                if (capture_image) {
                    diagnostic_image.stage = "xr_final_eye";
                    diagnostic_image.swapchain = handle_value(state->handle);
                    diagnostic_image.eye = state->dlss_logical_eye_index
                        ? static_cast<std::int32_t>(
                            *state->dlss_logical_eye_index)
                        : -1;
                    diagnostic_image.width = state->create_info.width;
                    diagnostic_image.height = state->create_info.height;
                }
                xrfg::D3D12HistoryCaptureWait capture_wait{};
                capture_wait.limit_us=history_capture_wait_limit_us(*state->session);
                capture_result = history->capture(*candidate_index, &ticket,
                    capture_image ? &diagnostic_image : nullptr,
                    capture_wait.limit_us ? &capture_wait : nullptr);
                if (capture_wait.slot_busy)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::history_capture_wait,
                        capture_result,handle_value(state->handle),capture_wait.waited_us,
                        (static_cast<std::uint64_t>(capture_wait.limit_us)<<32)|*candidate_index);
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
    }

    std::uint64_t diagnostic_capture = 0;
    if (candidate_index && state->session->frame_capture &&
        ((pending_capture && d3d11_interop) || state->session->frame_capture->ring_active()) &&
        (!state->session->frame_capture->ring_active() ||
            (state->dlss_logical_eye_count==2 && state->dlss_logical_eye_index.has_value())) &&
        state->session->frame_capture->active() && *candidate_index < state->enumerated_d3d11_images.size()) {
        if(!gpu_lock.owns_lock())gpu_lock=std::unique_lock<std::mutex>(state->session->gpu_mutex);
        diagnostic_capture = state->session->frame_capture->source(handle_value(state->handle),
            state->enumerated_d3d11_images[*candidate_index].Get(), *candidate_index,
            pending_capture ? pending_capture->serial : 0, pending_capture ? pending_capture->fence_value : 0);
        if(state->session->frame_capture->ring_active())xrfg::diagnostic_ring_record("{\"kind\":\"source_receipt\",\"qpc\":"+
            std::to_string(xrfg::diagnostic_ring_qpc())+",\"capture\":"+std::to_string(diagnostic_capture)+
            ",\"swapchain\":\""+std::to_string(handle_value(state->handle))+"\",\"render\":"+
            std::to_string(luke_identity?luke_identity->render:0)+",\"object\":\""+std::to_string(luke_identity?luke_identity->object:0)+"\"}");
    }
    if (bridge && candidate_index) {
        // Every read of the shared texture this frame - the copy and the
        // capture - is queued; the next acquire of this image waits for it.
        static_cast<void>(bridge->mark_read(*candidate_index));
    }

    if (vulkan_bridge && candidate_index && FAILED(vulkan_bridge->mark_read(*candidate_index)))
        return XR_ERROR_RUNTIME_FAILURE;

    mark_application_release(state->session.get());
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    try {
        result = with_runtime_entry(state->session.get(), [&] {
            return state->session->dispatch->release_swapchain_image(
                state->handle, release_info);
        });
    } catch (...) {
        if (pending_capture && history) {
            history->discard(*pending_capture);
        }
        throw;
    }

    if (state->composition_capture && composition_result != S_FALSE)
        state->composition_capture->valid = XR_SUCCEEDED(result) && composition_result == S_OK;
    if (XR_SUCCEEDED(result)) {
        bool commit_capture = false;
        {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->waited_count > 0 &&
                !state->acquired_indices.empty()) {
                const std::uint32_t released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                --state->waited_count;
                state->last_released_index = released_index;
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->luke_released_identity = state->session->luke_fresh_only ? luke_identity : std::nullopt;
                if (state->session->luke_fresh_only) state->luke_captured_source = luke_identity;
                state->luke_released_duplicate = false;
                commit_capture = pending_capture &&
                                 pending_capture->source_index == released_index;
            } else if (state->ownership_tracking_valid) {
                state->acquired_indices.clear();
                state->waited_count = 0;
                state->last_released_index.reset();
                state->last_released_capture.reset();
                state->last_released_motion_vectors.reset();
                state->ownership_tracking_valid = false;
                state->luke_released_identity.reset();
                state->luke_released_duplicate = false;
            }
        }

        if (pending_capture && history) {
            const bool committed =
                commit_capture && SUCCEEDED(history->commit(*pending_capture));
            if (committed) {
                std::scoped_lock lock(state->mutex);
                state->last_released_capture = pending_capture;
                state->luke_released_identity = luke_identity;
                state->last_diagnostic_capture = diagnostic_capture;
                state->last_released_motion_vectors = pending_motion_vectors;
                state->release_publication = release_publication;
                const int released_eye = state->projection_eye.load(std::memory_order_relaxed);
                if (release_bound_guides && (released_eye == 0 || released_eye == 1))
                    state->session->eye_release_publication[released_eye].store(
                        release_publication, std::memory_order_relaxed);
                if (luke_identity)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_release,
                        0, handle_value(state->handle), luke_identity->render, pending_capture->serial);
                const bool luke_split_eye =
                    (state->session->graphics_binding_capabilities & 8ULL) != 0 &&
                    state->dlss_logical_eye_count == 2 &&
                    state->dlss_logical_eye_index.has_value();
                if (luke_split_eye && state->session->luke_afw_enabled) {
                    // result=logical eye, a=stage 32, b=XR colour capture
                    // serial, c=application swapchain.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_afw_stage,
                        static_cast<std::int64_t>(
                            *state->dlss_logical_eye_index),
                        32, pending_capture->serial,
                        handle_value(state->handle));
                }
                if (luke_split_eye && pending_motion_vectors &&
                    pending_motion_vectors->eye_count != 0 &&
                    pending_motion_vectors->eyes[0]) {
                    const auto& guide = *pending_motion_vectors->eyes[0];
                    if (state->session->luke_afw_enabled) log_luke_guide_basis(guide);
                    const std::int64_t logical_eye = static_cast<std::int64_t>(
                        *state->dlss_logical_eye_index);
                    // result=logical eye; a=application swapchain;
                    // b=capture serial; c=DLSS publication.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_guide_contract,
                        logical_eye,
                        handle_value(state->handle),
                        pending_capture->serial,
                        guide.publication);
                    // result=100+logical eye; a=DLSS stream; b=DLSS serial;
                    // c=hash of the camera position/right/forward/FOV data.
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_guide_contract,
                        100 + logical_eye,
                        guide.stream,
                        guide.serial,
                        dlss_guide_camera_hash(guide));
                }
                if (luke_split_eye && state->session->luke_afw_enabled) {
                    std::scoped_lock guide_lock(
                        state->session->luke_guide_mutex);
                    state->session->luke_afw_released_eye =
                        *state->dlss_logical_eye_index;
                }
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

XrResult release_swapchain_with_identity(
    const std::shared_ptr<SwapchainState>& state,
    const XrSwapchainImageReleaseInfo* release_info,
    std::optional<xrfg::LukeRenderIdentity> luke_identity) {
    if (!state || state->session->dispatch->release_swapchain_image == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    bool worker_source = false;
    bool luke_duplicate = false;
    const bool afw_producer = state->session->luke_afw_enabled &&
        xrfg::embedded::afw_snapshot().desired.mode != 0;
    {
        std::scoped_lock lock(state->mutex);
        worker_source = state->frame_generation != nullptr;
        luke_duplicate = worker_source && luke_identity &&
            state->luke_admitted_identity == luke_identity;
    }
    bool director_active = false;
    { std::scoped_lock lock(state->session->presenter_mutex);
      director_active = state->session->three_slot_director && !state->session->presenter_stop_requested;
      if (worker_source && director_active && XR_FAILED(state->session->presenter_failure))
          return state->session->presenter_failure; }
    if (!state->session->luke_fresh_only && (afw_producer || !worker_source || !director_active || !state->session->menu_enabled ||
        state->session->manual_control.stop_requested())) {
        luke_identity.reset();
        luke_duplicate = false;
    }
    if (state->session->luke_fresh_only) {
        std::scoped_lock lock(state->mutex);
        luke_duplicate = state->session->luke_source_filter_armed &&
            (!luke_identity || state->luke_captured_source == luke_identity);
    }
    if (luke_duplicate) {
        // No history/composition copy, gpu_mutex, worker or slot wait. A may
        // still be read by the worker: retain its private capture and vectors.
        std::scoped_lock call_lock(state->call_mutex);
        const XrResult result = with_runtime_entry(state->session.get(), [&] {
            return state->session->dispatch->release_swapchain_image(state->handle, release_info);
        });
        if (XR_SUCCEEDED(result)) {
            std::scoped_lock lock(state->mutex);
            if (state->ownership_tracking_valid && state->waited_count > 0 && !state->acquired_indices.empty()) {
                state->last_released_index = state->acquired_indices.front();
                state->acquired_indices.pop_front();
                --state->waited_count;
                state->luke_released_identity = luke_identity;
                state->luke_released_duplicate = true;
            } else {
                state->ownership_tracking_valid = false;
                state->acquired_indices.clear(); state->waited_count = 0;
                state->luke_released_identity.reset();
                state->luke_released_duplicate = true; // never hand retained A off as new B
            }
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_release,
            XR_SUCCEEDED(result) ? 1 : -1, handle_value(state->handle), luke_identity ? luke_identity->render : 0, 0);
        return result;
    }
    if (worker_source && !afw_producer &&
        (!state->session->luke_fresh_only || (state->session->luke_source_filter_armed && !state->session->afw_only && xrfg::embedded::snapshot().desired.enabled))) {
        const XrResult worker_ready = wait_for_synthesis_handoff(state->session);
        if (XR_FAILED(worker_ready)) return worker_ready;
    }
    std::scoped_lock call_lock(state->call_mutex);
    release_pending_bridge_image_locked(state);
    const DWORD end_thread=state->session->application_end_thread.load(std::memory_order_relaxed);
    if (!afw_producer && !state->session->luke_afw_enabled && !luke_identity &&
        (state->session->graphics_binding_capabilities & 8ULL)==0 &&
        end_thread && end_thread==GetCurrentThreadId() && release_info &&
        release_info->type==XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO && !release_info->next) {
        std::scoped_lock lock(state->mutex);
        if (state->bridge && !state->vulkan_bridge && state->ownership_tracking_valid &&
            state->waited_count>0 && !state->acquired_indices.empty()) {
            state->pending_bridge_release=state->acquired_indices.front();
            state->pending_bridge_release_at=std::chrono::steady_clock::now();
            return XR_SUCCESS;
        }
    }
    return release_swapchain_image_locked(state,release_info,luke_identity);
}

void release_pending_bridge_image_locked(const std::shared_ptr<SwapchainState>& state) noexcept {
    try {
        if (!state->pending_bridge_release) return;
        const auto index=*state->pending_bridge_release;
        state->pending_bridge_release.reset();
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now()-state->pending_bridge_release_at).count();
        const XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        const auto result=release_swapchain_image_locked(state,&release,std::nullopt);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::d3d11_bridge,
            XR_SUCCEEDED(result)?4:static_cast<std::int64_t>(result),handle_value(state->handle),
            XR_SUCCEEDED(result)?index:4,XR_SUCCEEDED(result)?static_cast<std::uint64_t>(std::max<std::int64_t>(0,elapsed)):index);
    } catch (...) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::d3d11_bridge,
            XR_ERROR_RUNTIME_FAILURE,handle_value(state->handle),4,0);
    }
}
void release_pending_bridge_images(const std::shared_ptr<SessionState>& session) noexcept {
    try { for (const auto& chain:find_swapchains(session)) {
        std::scoped_lock lock(chain->call_mutex);
        release_pending_bridge_image_locked(chain);
    }} catch (...) {}
}

XrResult layer_release_swapchain_image_impl(XrSwapchain swapchain,
    const XrSwapchainImageReleaseInfo* release_info, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 :
        xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_swapchain(swapchain);
    if (state && interface_call.started_ns()) {
        interface_call.cycle = state->session->interface_active_cycle.load();
    }
    // Snapshot before any worker wait. The reader never touches application pixels.
    return release_swapchain_with_identity(state, release_info, xrfg::query_luke_render_identity());
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

[[nodiscard]] bool acquire_and_wait_private_image(SessionState* session,
    const std::shared_ptr<Dispatch>& dispatch, PrivateSwapchainState& image) noexcept;

[[nodiscard]] bool hold_djules_synthetic(std::chrono::nanoseconds age,
    std::chrono::nanoseconds period, bool pixels_ready, bool prior_owned_frame,
    bool pipelined) noexcept {
    return prior_owned_frame && !pipelined && period>std::chrono::nanoseconds::zero() &&
        (age<period || (!pixels_ready && age<period*3));
}

struct DjulesPendingOutput {
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    std::shared_ptr<DjulesOutputStaging> staging;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    xrfg::D3D12FrameSynthesisTicket ticket;
    bool synthetic{};
    std::uint32_t runtime_slot{};
};

[[nodiscard]] bool handover_djules_outputs(SessionState* session,
    std::vector<DjulesPendingOutput>& outputs) noexcept {
    try {
        while(!outputs.empty()) {
            auto& pending=outputs.front(); auto& generation=*pending.generation;
            auto& image=pending.synthetic?generation.synthetic[pending.runtime_slot]:generation.current[pending.runtime_slot];
            auto& images=pending.synthetic?generation.synthetic_runtime_images:generation.current_runtime_images;
            // Staged B is already submitted: its own copy completion, not
            // S's earlier completion, orders the copy into the XR image.
            HRESULT synchronized=E_FAIL;
            if(session->dispatch->steamvr_runtime && session->graphics_binding==SessionGraphicsBinding::d3d12 &&
                !session->d3d11_bridge && !session->vulkan_bridge && !session->luke_afw_enabled) {
                const auto fence=pending.synthesizer->submission_fence(pending.ticket);
                const auto value=pending.synthetic && pending.ticket.synthetic_fence_value
                    ? pending.ticket.synthetic_fence_value : pending.ticket.fence_value;
                if(fence && value) synchronized=runtime_queue(*session)->Wait(fence.Get(),value);
            } else synchronized=pending.synthesizer->synchronize_consumer_queue(runtime_queue(*session),pending.ticket);
            if(FAILED(synchronized) || !acquire_and_wait_private_image(session,session->dispatch,image))
                return false;
            const auto per_ring = pending.synthetic ? generation.synthetic_runtime_images_per_ring : generation.current_runtime_images_per_ring;
            const auto runtime_index = pending.runtime_slot * per_ring + image.acquired_index;
            if(image.acquired_index>=per_ring || runtime_index>=images.size()) return false;
            const auto copy_slot=generation.handover_copy_slot++ % generation.handover_copies.size();
            const HRESULT copied=copy_composition12(session->d3d12_device.Get(),runtime_queue(*session),
                generation.handover_fence.Get(),generation.handover_serial,generation.handover_copies[copy_slot],
                images[runtime_index].Get(),D3D12_RESOURCE_STATE_COMMON,
                pending.staging->resource.Get(),D3D12_RESOURCE_STATE_COMMON);
            if(FAILED(copied)) return false;
            pending.staging->last_handover_copy.store(generation.handover_serial,std::memory_order_release);
            if(!release_private_image(session,session->dispatch,image)) return false;
            outputs.erase(outputs.begin());
        }
        return true;
    } catch(...) { return false; }
}

// One acquired direct image travels with its exact output. The normal
// consumer publishes only inside a begun XR frame. Destruction before enqueue
// retains the old safe GPU join/release for structural fallback and teardown.
struct DirectOutputPublication {
    std::weak_ptr<SessionState> session;
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    xrfg::D3D12FrameSynthesisTicket ticket;
    PrivateSwapchainState* image{};
    std::atomic_bool* busy{};
    bool synthetic{}, published{}, active{true};
    bool publish() noexcept {
        if(published) return true;
        const auto owner=session.lock();
        if(!owner || !image) return false;
        if(ticket.fence_value) {
            const auto fence=synthesizer->submission_fence(ticket);
            const auto value=synthetic && ticket.synthetic_fence_value ? ticket.synthetic_fence_value : ticket.fence_value;
            if(!fence || FAILED(runtime_queue(*owner)->Wait(fence.Get(),value))) return false;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_source,
                5530,handle_value(image->handle),value,synthetic);
        }
        if(!release_private_image(owner.get(),owner->dispatch,*image)) return false;
        published=true;return true;
    }
    void finish() noexcept {
        if(!active || !publish()) return;
        active=false;busy->store(false,std::memory_order_release);
        if(const auto owner=session.lock()) owner->presenter_condition.notify_all();
    }
    ~DirectOutputPublication(){finish();}
};

struct GeneratedFrameEndInfo {
    BlackBarPoseTime blackbar_pose{};
    xrfg::InterfaceFrameStamp interface_stamp{};
    std::vector<std::shared_ptr<xrfg::D3D12NativeFrameResult>> native_results;
    unsigned native_output{};
    struct Readiness {
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        bool require_application_release{};
        xrfg::D3D12SynthesisInputs inputs;
    };
    std::vector<Readiness> reference_readiness;
    std::vector<std::shared_ptr<DirectOutputPublication>> direct_publications;
    std::array<Readiness, 8> diagnostic_handover;
    std::vector<DjulesPendingOutput> pending_outputs;
    std::vector<std::shared_ptr<DirectSyntheticLease>> direct_synthetic_leases;
    std::uint64_t app_release_value{};
    std::uint64_t input_release_value{};
    bool synthetic{};
    bool djules_leader{};
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<ProjectionLayerCopy> projections;
    std::vector<OwnedCompositionLayer> composition_layers;
    std::vector<const XrCompositionLayerBaseHeader*> layer_pointers;
    std::vector<std::shared_ptr<CompositionImageSlot>> composition_images;
    // Synthesizers whose current copy is recorded but not yet submitted.
    // Only the synthetic half of a pair carries these: the copy must reach
    // the queue after this frame has been handed over and before the
    // current frame follows a display period later.
    std::vector<std::shared_ptr<xrfg::D3D12FrameSynthesizer>>
        pending_current_copies;
};

// Observer only. A pending application release does not establish whether S
// itself is ready, and a successful EndFrame does not establish GPU completion.
// Keep those facts distinct across the same output's XR boundary. No wait,
// signal, query allocation, queue submission or scheduler input belongs here.
void log_native_delivery_probe(const SessionState& state,
    const GeneratedFrameEndInfo* frame, std::uint64_t sequence,
    unsigned stage, XrTime display_time = 0) noexcept {
    auto& logger = xrfg::bridge_flight_logger();
    if (!logger.enabled() || !frame || !state.dispatch->steamvr_runtime ||
        state.graphics_binding != SessionGraphicsBinding::d3d12 ||
        state.luke_afw_enabled || state.luke_outlaws_preframe ||
        frame->reference_readiness.empty()) return;
    const auto identity = sequence * 2 + (frame->synthetic ? 1u : 0u);
    logger.event(xrfg::BridgeFlightOperation::presenter_source, 5490 + stage,
        identity, frame->info.displayTime, display_time);
    if (state.app_release_fence)
        logger.event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
            54900 + stage * 100 + 90,
            reinterpret_cast<std::uintptr_t>(state.app_release_fence.Get()),
            frame->app_release_value, state.app_release_fence->GetCompletedValue());
    for (std::size_t i = 0; i < frame->reference_readiness.size() && i < 8; ++i) {
        const auto& receipt = frame->reference_readiness[i];
        if (receipt.fence)
            logger.event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
                54900 + stage * 100 + i,
                reinterpret_cast<std::uintptr_t>(receipt.fence.Get()),
                receipt.value, receipt.fence->GetCompletedValue());
        for(std::size_t j=0;j<receipt.inputs.dependencies.size();++j) {
            const auto& input=receipt.inputs.dependencies[j];
            if(input.fence && input.value) logger.event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
                54900+stage*100+30+i*3+j,reinterpret_cast<std::uintptr_t>(input.fence.Get()),
                input.value,input.fence->GetCompletedValue());
        }
    }
    for (std::size_t i = 0; i < frame->composition_images.size() && i < 8; ++i) {
        const auto& image = frame->composition_images[i];
        if (image && image->fence12)
            logger.event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
                54900 + stage * 100 + 10 + i,
                reinterpret_cast<std::uintptr_t>(image->fence12.Get()),
                image->ready_value, image->fence12->GetCompletedValue());
    }
    for (std::size_t i = 0; i < frame->diagnostic_handover.size(); ++i) {
        const auto& receipt = frame->diagnostic_handover[i];
        if (receipt.fence)
            logger.event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
                54900 + stage * 100 + 20 + i,
                reinterpret_cast<std::uintptr_t>(receipt.fence.Get()),
                receipt.value, receipt.fence->GetCompletedValue());
    }
    // Record numeric positions as well as orientations: the reported movement
    // is physical head translation, which a pose hash alone cannot describe.
    if (stage == 2 && (sequence / 2) % 16 == 0) {
        for (const auto& projection : frame->projections)
            for (std::size_t eye = 0; eye < projection.views.size() && eye < 8; ++eye) {
                logger.event(xrfg::BridgeFlightOperation::luke_inline_view,
                    54900 + projection.layer_index * 10 + eye, identity,
                    handle_value(projection.layer.space),
                    handle_value(projection.views[eye].subImage.swapchain));
                log_luke_view_bits(identity, 54900 + projection.layer_index * 10 + eye,
                    projection.views[eye]);
            }
    }
}

// This describes content only. Physical slot/cadence remains unchanged.
std::span<const std::shared_ptr<xrfg::D3D12NativeFrameResult>> native_frame_results(
    const GeneratedFrameEndInfo* frame) noexcept {
    return frame ? std::span<const std::shared_ptr<xrfg::D3D12NativeFrameResult>>{frame->native_results}
        : std::span<const std::shared_ptr<xrfg::D3D12NativeFrameResult>>{};
}
void report_native_frame_content(SessionState* state, const GeneratedFrameEndInfo* frame) noexcept {
    if (!state || state->frame_generation_mode!=xrfg::D3D12FrameGenerationMode::native_dlss ||
        !frame || frame->native_results.empty()) return;
    using GS=xrfg::autonomous::GenerationState;
    auto status=GS::ready;
    std::uint32_t provider_error{};
    for(const auto& receipt:frame->native_results) {
        if(!receipt) { status=GS::invalid_guides; break; }
        if(receipt->generated(frame->native_output)) continue;
        provider_error = receipt->provider_error;
        if(receipt->resolve()==xrfg::D3D12NativeFrameResult::pending) status=GS::busy;
        else switch(receipt->skip) {
        case 1: status=(receipt->detail==5 || receipt->detail==20) ? GS::temporal_mismatch : receipt->detail==4 ? GS::recovering : GS::invalid_guides; break;
        case 2: status=GS::recovering; break;
        case 3: status=GS::multi_frame_unsupported; break;
        case 4: status=GS::unavailable; break;
        case 5: status=GS::dispatch_failed; break;
        default: status=GS::native_declined; break;
        }
        break;
    }
    state->generation_status=status;
    state->interface_provider_error=provider_error;
}

// A buffer position owns only this descriptor. The pixels stay in one
// bridge-owned swapchain image until its director submission has completed.
struct ThreeSlotFrame {
    std::chrono::steady_clock::time_point queued_at{};
    std::uint64_t source_sequence{};
    std::uint64_t previous_sequence{};
    std::shared_ptr<GeneratedFrameEndInfo> frame;
};

struct PostAfwFrameInput {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    XrSwapchain completed_swapchain{XR_NULL_HANDLE};
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    xrfg::D3D12HistoryCaptureTicket capture{};
    std::vector<xrfg::D3D12ReprojectionView> views;
    xrfg::ColorIdentityTag color_identity;
};

[[nodiscard]] xrfg::ColorIdentityTag make_color_identity(
    std::uint64_t serial, std::uint64_t capture, XrSwapchain swapchain,
    std::uint32_t image, std::uint32_t eye, std::uint32_t role,
    const xrfg::D3D12ReprojectionView& source, std::uint32_t xr_index=0) noexcept {
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose.position={source.pose.position.x,source.pose.position.y,source.pose.position.z};
    view.pose.orientation={source.pose.orientation.x,source.pose.orientation.y,
        source.pose.orientation.z,source.pose.orientation.w};
    view.fov={source.fov.angle_left,source.fov.angle_right,source.fov.angle_up,source.fov.angle_down};
    return {serial,capture,handle_value(swapchain),projection_pose_hash(view),image,eye,role,xr_index};
}

// The very same completed AFW submission is used by direct XR and OFXR.
// History tickets own the finished pixels; the frozen submission owns their
// original camera/FOV/space/rect metadata. No consumer samples newer app state.
struct CompletedAfwFrame {
    std::shared_ptr<GeneratedFrameEndInfo> submission;
    ProjectionSnapshot snapshot;
    std::vector<PostAfwFrameInput> inputs;
    std::uint64_t application_serial{};
};

struct SynthesisJob {
    xrfg::D3D12PresentationTransform presentation_transform{};
    ProjectionSnapshot snapshot;
    std::optional<ProjectionSnapshot> synthetic_camera, extra_camera;
    std::vector<ProjectionResourceMapping> mappings;
    std::shared_ptr<GeneratedFrameEndInfo> metadata;
    std::shared_ptr<ThreeSlotFrame> current;
    std::shared_ptr<const CompletedAfwFrame> post_afw_frame;
    bool pair{};
    bool triple{};
    float fraction{0.5F},extra_fraction{2.0F/3.0F};
};

[[nodiscard]] XrResult wait_for_synthesis_handoff(
    const std::shared_ptr<SessionState>& state) noexcept {
    std::unique_lock lock(state->presenter_mutex);
    if (!state->three_slot_director) return XR_SUCCESS;
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::three_slot_worker_wait, handle_value(state->handle));
    state->presenter_condition.wait(lock, [&] {
        return !state->synthesis_busy || state->presenter_stop_requested ||
               XR_FAILED(state->presenter_failure);
    });
    const XrResult result = XR_FAILED(state->presenter_failure)
        ? state->presenter_failure : XR_SUCCESS;
    lock.unlock();
    xrfg::bridge_flight_logger().end(token,
        xrfg::BridgeFlightOperation::three_slot_worker_wait, result);
    return result;
}


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

// Djules df029629 / V417: these errors reject this frame's contents, not
// the session. Drop it and return its error; preserve all fatal-error handling.
[[nodiscard]] constexpr bool frame_content_refused(XrResult result) noexcept {
    return result == XR_ERROR_LAYER_INVALID ||
           result == XR_ERROR_LAYER_LIMIT_EXCEEDED ||
           result == XR_ERROR_SWAPCHAIN_RECT_INVALID ||
           result == XR_ERROR_POSE_INVALID;
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
// inside the begun frame, so the runtime measures a frame that contains the
// work actually being done rather than an empty window. It must not run
// under presenter_content_mutex - waiting for presenter progress while
// holding that lock has deadlocked this layer twice.
void pace_presenter_submission(
    const std::shared_ptr<SessionState>& state) noexcept {
    if(state->private_runtime)return; // only the common consumer paces SteamVR
    try {
        refresh_djules_display_period(state);
        refresh_reference_display_period(state);
        refresh_luke_display_period(state);
        const auto entered = std::chrono::steady_clock::now();
        std::int64_t behind_us = 0;
        std::chrono::nanoseconds remaining{0};
        // -1 pulled earlier off the ceiling, +1 pushed later off the floor.
        int pace_band_correction = 0;
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
            {
                // The one restoring force on the grid's phase. Every other
                // path here moves the deadline later - the catch-up adds whole
                // periods, the step-over adds one - and the phase reference
                // below cannot pull it back, because it tracks whatever phase
                // the presenter is already holding and so has no opinion about
                // a grid that is self-consistent and parked at the worst place
                // in the period. Left alone the schedule ratchets to the end
                // of the period and stays there.
                //
                // Measured in Callisto Protocol on SteamVR: V155 held 10.30 ms
                // of an 11.11 ms period, dead flat from the seventh second to
                // the end of the run, with 45.0 in, 90.0 out, an 11.111 ms
                // grid and no skips or bunching at all. Every submission
                // landed as late in its period as it could, the compositor
                // kept the real frame and dropped the synthetic, and the
                // headset showed 45. The builds that worked held 1.7 to 9.6 ms
                // and moved about.
                //
                // `entered` is the moment the runtime's own xrWaitFrame
                // released this presenter, so the hold is measured against the
                // runtime's timeline rather than the layer's own, and the
                // threshold means something absolute. Three quarters leaves
                // the working range untouched and only acts on a schedule that
                // has walked to the end.
                //
                // There is a floor as well as a ceiling, and it is not
                // symmetry for its own sake. This hold *is* the window the
                // runtime measures its client across: the pace runs inside the
                // begun frame precisely so that begin-to-end spans the real
                // work rather than an empty gap (47d8590). Let the hold reach
                // zero and begin and end go back to back again, the runtime is
                // handed a frame that costs a millisecond, and it schedules
                // against that - asking for the frame late, assuming it will
                // be ready, and reprojecting when it is not.
                //
                // That regressed here. The phase correction walks the grid
                // earlier and nothing bounded it from below, so in The
                // Witcher 3 the measured window fell from 8.0 ms at 35 s to
                // 0.06 ms at 50 s, with 97.3% of frames reporting under a
                // millisecond, and recovered only when the catch-up and
                // step-over had pushed the schedule back later. Twenty-five
                // seconds of the runtime scheduling against an empty client,
                // with the layer's own view a flawless 45 in, 90 out, 11.111
                // ms grid, no skips and no bunching throughout.
                //
                // So drive the hold into a band rather than off one edge. A
                // quarter to three quarters of a period brackets the 5.7 to
                // 8.5 ms the healthy stretches of that same run held, and
                // since every other path moves the deadline later the
                // schedule settles near the top of the band, which is also
                // where the measured window is longest.
                //
                // The floor applies when the presenter is already past its
                // deadline too - `remaining` is zero there, and that is the
                // case that empties the window completely.
                //
                // Both ends wait for a run of on-grid submissions, for the
                // same reason the phase reference does: a grid that is
                // skipping slots has no stable phase to correct towards, and
                // correcting one anyway closes a loop. Measured in Atomic
                // Heart on V157, where the application hitched to 16-33/s for
                // four seconds: the starvation skips pushed the grid later
                // through the catch-up, that lifted the hold over the ceiling,
                // the pull then fired on 44-83% of frames and held the grid at
                // 10.55-10.78 ms against 11.111, and running fast tripped the
                // step-over into the next skip. It sustained itself for nine
                // seconds after the application was back at 43-45/s, throwing
                // away five to seven slots a second where two would have done,
                // and stopped only when the hold drifted back under the
                // ceiling on its own.
                //
                // The gate costs nothing in the cases the band exists for.
                // Both were clean grids: Callisto parked at the ceiling for
                // 47 s with no skips at all, and The Witcher 3 walked to the
                // floor with 0.0-0.4 skips a second.
                // The ceiling carries the bias. The pair is deliberately unevenly
                // spaced, so the hold before a synthetic is longer by exactly
                // that much; against a bare 3/4 of a period the band reads it
                // as a grid that has walked to the end and pulls against the
                // bias. Measured while the bias was being walked by hand: mean
                // hold 4.46 -> 6.88 -> 8.29 ms against an 8.33 ms ceiling, with
                // the pull rate going 2.6% -> 7.2% -> 8.5%. Offset, never
                // removed - section 14 is what happens without a ceiling at all.
                const auto band_ceiling =
                    period * 3 / 4 + state->presenter_pair_bias;
                if (state->presenter_on_grid_streak <
                    kPhaseCorrectionGridStreak) {
                    // Rate is wrong; leave the schedule alone.
                } else if (remaining > band_ceiling) {
                    state->presenter_next_submit -= period / 16;
                    pace_band_correction = -1;
                } else if (remaining < period / 4) {
                    state->presenter_next_submit += period / 16;
                    pace_band_correction = 1;
                }
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
        // result=1 marks a frame where the hold had reached the ceiling and
        // the grid was pulled back off it, result=2 one pushed up off the
        // floor, so a capture shows which edge the schedule is being held away
        // from and whether the band is working or fighting.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pace,
            pace_band_correction < 0 ? 1 : (pace_band_correction > 0 ? 2 : 0),
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
    std::uint64_t clock_anchor_tick = 0;
    LARGE_INTEGER qpc_frequency{};
    static_cast<void>(QueryPerformanceFrequency(&qpc_frequency));
    for (;;) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            if (state->presenter_stop_requested) {
                break;
            }
        }


        XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frame_state{XR_TYPE_FRAME_STATE};
        const auto wait_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_wait_frame,
            handle_value(state->handle));
        const XrResult wait_result = wait_presenter_runtime_frame(state, &wait_info, &frame_state);
        xrfg::bridge_flight_logger().end(
            wait_token,
            xrfg::BridgeFlightOperation::internal_wait_frame,
            wait_result,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
            frame_state.shouldRender);
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
            // One tick per presenter frame. The application's wait counts these
            // so that it is released once per pair rather than once per frame.
            ++state->presenter_frame_serial;
            // Lock the grid to the display the runtime is actually scanning
            // out on. One frame per period means consecutive waits advance
            // predictedDisplayTime by exactly one period; a repeat says two
            // submissions were aimed at one scanout and the grid is ahead, a
            // skip says a scanout went unfilled and it is behind. Correct by
            // a bounded fraction so a single odd prediction cannot jerk the
            // cadence, and only once the grid exists to be corrected.
            const XrDuration locked_period =
                state->presenter_display_period;
            if (!single_director_pacing(*state) && state->presenter_last_predicted_valid &&
                state->presenter_schedule_valid && locked_period > 0) {
                const XrTime previous =
                    state->presenter_last_predicted_display;
                const XrTime current = frame_state.predictedDisplayTime;
                const XrDuration advance = current > previous
                    ? static_cast<XrDuration>(current - previous)
                    : 0;
                // A predicted time that did not move, or moved absurdly,
                // is the runtime not describing a new scanout - a session
                // transition, a frame it does not want rendered, a stall.
                // It is not evidence about this grid's phase, and treating
                // a repeat as proof the grid was early is what let the
                // deadline run away into the future.
                const bool usable_signal =
                    advance > 0 && advance < locked_period * 8;
                const std::int64_t slots = usable_signal
                    ? (advance + locked_period / 2) / locked_period
                    : 1;
                if (slots != 1) {
                    const auto period_ns =
                        std::chrono::nanoseconds(locked_period);
                    // A repeated slot means the grid is early and must be
                    // pushed later; a skipped one means it is late. Step by a
                    // sixteenth of a period, which still walks out a whole slot
                    // in about a fifth of a second and cannot bunch a pair
                    // inside one scanout window on its own.
                    const auto step = period_ns / 16;
                    if (slots < 1) {
                        state->presenter_next_submit += step;
                        // Never let a correction put the deadline further
                        // out than one period. Every other path moves it
                        // later too, so without a ceiling the schedule can
                        // only walk forwards, and a presenter that keeps
                        // sleeping longer stops draining the queue the
                        // application is admitted against.
                        const auto ceiling =
                            std::chrono::steady_clock::now() + period_ns;
                        if (state->presenter_next_submit > ceiling) {
                            state->presenter_next_submit = ceiling;
                        }
                    } else {
                        state->presenter_next_submit -= step;
                    }
                }
            }
            state->presenter_last_predicted_display =
                frame_state.predictedDisplayTime;
            state->presenter_last_predicted_valid =
                frame_state.predictedDisplayTime > 0;
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
                state->history_capture_wait_period.store(state->presenter_display_period, std::memory_order_relaxed);
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
                       (!state->private_runtime && state->presenter_last_frame != nullptr) ||
                       !state->presenter_submissions.empty();
            });
        }

        XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
        const auto begin_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::internal_begin_frame,
            handle_value(state->handle));
        const XrResult begin_result = with_runtime_entry(state, [&] {
            return state->dispatch->begin_frame(state->handle, &begin_info);
        });
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

        // The pace runs here, inside the begun frame, rather than before the
        // cycle. A runtime measures an application frame between xrBeginFrame
        // and xrEndFrame; calling them back to back, as this loop did, gives
        // it a frame containing nothing - measured as 0.73 ms of CPU and
        // under a millisecond of GPU, while the real work costs 12 ms of
        // application rendering and 3.59 ms of synthesis outside the window.
        //
        // A scheduler told its client costs a millisecond has every reason to
        // ask for the frame late and to assume it will be ready. Holding the
        // frame open across the pace is what every ordinary application does
        // - begin, render, end - and lets the queue timestamps span the work
        // actually being done.
        pace_presenter_submission(state);

        std::shared_ptr<PresenterSubmission> request;
        std::shared_ptr<GeneratedFrameEndInfo> repeated_frame;
        XrResult end_result = XR_ERROR_RUNTIME_FAILURE;
        std::uint32_t submitted_layer_count = 0;
        XrTime source_display_time = 0;
        // Which half of the pair the presenter submitted, kept at this scope so
        // the flight record below can carry it.
        bool fresh_synthetic = false;
        // When the downstream xrEndFrame was entered, so the bias controller
        // below can see how long the runtime held this submission.
        auto downstream_end_started = std::chrono::steady_clock::now();
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
                state->presenter_inflight_submission = request;
                state->presenter_inflight_frame = request
                    ? request->owned_frame : repeated_frame;
            }

            const XrFrameEndInfo* source = request
                ? request->owned_frame
                    ? &request->owned_frame->info
                    : request->borrowed_frame
                : repeated_frame
                    ? &repeated_frame->info
                    : nullptr;
            source_display_time = source ? source->displayTime : 0;
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
            fresh_synthetic = request && request->owned_frame &&
                request->owned_frame->synthetic;
            if (state->fps_overlay && state->manual_control.stop_requested())
                state->fps_overlay->suspend();
            downstream_end_started = std::chrono::steady_clock::now();
            join_runtime_queue_to_application(state.get(), request
                ? request->owned_frame ? request->owned_frame->app_release_value : request->app_release_value
                : repeated_frame ? repeated_frame->app_release_value : 0);
            end_result = with_runtime_entry(state, [&] {
                auto* owned=request?request->owned_frame.get():repeated_frame.get();
                if(owned && advanced_native_outputs(*state)) {
                        for(const auto& image:owned->composition_images)
                            if(image->fence12 && FAILED(runtime_queue(*state)->Wait(image->fence12.Get(),image->ready_value)))
                                return XR_ERROR_RUNTIME_FAILURE;
                    }
                    if(owned) for(const auto& publication:owned->direct_publications)
                        if(!publication->publish()) return XR_ERROR_RUNTIME_FAILURE;
                    if(owned && !handover_djules_outputs(state.get(),owned->pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
                if(request) report_native_frame_content(state.get(),owned);
                return state->fps_overlay
                    ? state->fps_overlay->end_frame(&submitted, fresh_synthetic, request != nullptr, native_frame_results(owned),owned?owned->native_output:0, owned ? owned->interface_stamp : request ? request->interface_stamp : xrfg::InterfaceFrameStamp{})
                    : content_end_frame(state->handle, &submitted);
            });
            if(request && request->owned_frame)
                for(const auto& publication:request->owned_frame->direct_publications) publication->finish();
            // The synthetic has reached the runtime, so its current copy can
            // go to the queue now rather than ahead of it. It has a display
            // period before the current frame that reads it is submitted.
            if (request && request->owned_frame) {
                for (const auto& synthesizer :
                     request->owned_frame->pending_current_copies) {
                    if (synthesizer) {
                        static_cast<void>(
                            synthesizer->flush_current_copy(
                                state->d3d12_synthesis_queue
                                    ? runtime_queue(*state)
                                    : nullptr));
                    }
                }
            }
            // Where in the period this submission landed, measured against the
            // scanout the runtime aimed it at. The epochs differ, so only
            // comparisons between these values mean anything - which is what
            // the phase correction below uses them for.
            const std::int64_t submitted_lead =
                static_cast<std::int64_t>(frame_state.predictedDisplayTime) -
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            if (!single_director_pacing(*state)) {
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
                    // The two frames of a pair do not cost the runtime the
                    // same, so spacing them evenly gives them unequal margin.
                    // Measured in Atomic Heart across 1479 pairs, with the
                    // submission record finally carrying which half of the
                    // pair it was: the synthetic's xrEndFrame takes 2.12 ms
                    // against the current's 0.68, and the synthetic lands
                    // 2.36 ms closer to its own scanout. (V163 asserted the
                    // opposite from a capture anchored on the enqueue, which
                    // labels the previous pair's current as this pair's
                    // synthetic. It was wrong by exactly that swap.)
                    //
                    // That 1.44 ms of extra call time falls *between* the two
                    // submissions, so an even deadline spacing produces an
                    // uneven arrival spacing:
                    //
                    //   synthetic -> current   11.11 + 0.68 - 2.12 =  9.67 ms
                    //   current -> synthetic   11.11 + 2.12 - 0.68 = 12.55 ms
                    //
                    // Two arrivals 9.67 ms apart can land in one scanout
                    // window, and the compositor keeps one of them. Every
                    // metric the layer owns still reads 45 in, 90 out, two
                    // submissions per pair - which is why ten builds of pace
                    // work moved this around without fixing it. Moving the
                    // grid carries the asymmetry with it.
                    //
                    // So bias the pair, not the grid, and give the room to the
                    // frame whose call is long: an eighth of a period after
                    // the synthetic, the same back after the current. The two
                    // sum to exactly two periods, so the schedule does not
                    // drift, and both frames arrive one period apart. A repeat
                    // is not part of a pair and advances plainly.
                    // Adaptive pair bias. Shortening the interval after the
                    // synthetic lengthens the one before the *next* one, which
                    // gives that synthetic more age before its slot arrives -
                    // so the runtime blocks less, the gap to the real frame
                    // stops collapsing, and the real frame stops being
                    // discarded. See LOW_HEADROOM_PLAN.md section 16.
                    //
                    // Walked rather than computed: bias and excess are coupled,
                    // because raising the bias lowers the excess it is derived
                    // from. Predicting the result from the current distribution
                    // is the mistake section 9 forbids, so nudge and re-measure.
                    //
                    // Zero restores even spacing exactly, which is what makes
                    // this safe where there is no block to buy out - another
                    // runtime, a lower resolution, a heavier game, any refresh
                    // rate. It self-gates; the SteamVR test is belt and braces
                    // until there is a capture from each runtime rather than
                    // one smooth Quest 3 run.
                    if (request) {
                        const auto call = now - downstream_end_started;
                        auto& mean = fresh_synthetic
                            ? state->presenter_synthetic_call_mean
                            : state->presenter_real_call_mean;
                        mean = mean.count() == 0
                            ? std::chrono::duration_cast<
                                  std::chrono::nanoseconds>(call)
                            : mean + (std::chrono::duration_cast<
                                          std::chrono::nanoseconds>(call) -
                                      mean) / 16;
                        // The arrival gap, measured where it matters: between
                        // the two hand-overs, so it already carries whatever
                        // the runtime spent inside the synthetic's call.
                        if (fresh_synthetic) {
                            state->presenter_synthetic_returned_at = now;
                        } else if (state->presenter_synthetic_returned_at !=
                                   std::chrono::steady_clock::time_point{}) {
                            const auto gap = std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                now - state->presenter_synthetic_returned_at);
                            if (gap > std::chrono::nanoseconds::zero() &&
                                gap < period * 4) {
                                state->presenter_pair_gap_mean =
                                    state->presenter_pair_gap_mean.count() == 0
                                        ? gap
                                        : state->presenter_pair_gap_mean +
                                              (gap -
                                               state->presenter_pair_gap_mean) /
                                                  16;
                            }
                            state->presenter_synthetic_returned_at = {};
                        }
                    }
                    // Six times a second, not once. The block grows and shrinks
                    // with scene load - it is larger while the view is moving
                    // and smaller when it is still - so a controller that takes
                    // fifteen seconds to cover a change loses frames for the
                    // first several of them and gets them back only once the
                    // scene settles. Reported exactly that way: losing it at
                    // times, recovering after a second or two when standing
                    // still.
                    constexpr std::uint32_t kBiasUpdateFrames = 15;
                    if (++state->presenter_bias_tick >= kBiasUpdateFrames) {
                        state->presenter_bias_tick = 0;
                        // A dead band, and the rates are deliberately not
                        // symmetric. A low excess is the *success* condition -
                        // the block is suppressed because the bias is holding
                        // it down - so treating it as a reason to back off
                        // makes the controller remove its own correction and
                        // hunt. Measured doing exactly that: bias oscillating
                        // between 1.39 and 2.43 ms, with the excess swinging
                        // 0.11 to 4.24 in step with it, where the value found
                        // by hand was 2.78 and stable.
                        //
                        // So climb while the block is present, hold across the
                        // band, and decay only when the excess is clearly low,
                        // slowly enough to keep the level it found. Decay
                        // still has to exist: a genuine change - lower
                        // resolution, a heavier game, a different runtime -
                        // must bring it back to zero, and at this rate a full
                        // bias unwinds in about two minutes.
                        constexpr auto kBlockThreshold =
                            std::chrono::microseconds(500);
                        constexpr auto kSuppressedThreshold =
                            std::chrono::microseconds(200);
                        const auto excess =
                            state->presenter_synthetic_call_mean -
                            state->presenter_real_call_mean;
                        // Climb in proportion to the block, so a large one is
                        // covered in a step or two rather than walked up to.
                        // Bounded at both ends: a floor so it always makes
                        // progress, a cap so a single bad frame cannot throw
                        // the spacing. Overshoot is cheap here because the
                        // ceiling bounds it and the decay unwinds it.
                        auto climb = excess / 4;
                        const auto climb_floor = period / 128;
                        const auto climb_cap = period / 32;
                        if (climb < climb_floor) climb = climb_floor;
                        if (climb > climb_cap) climb = climb_cap;
                        const auto decay = period / 2048;
                        // period/4 was where the hand-walk stopped, not a value
                        // anything justified, and the controller saturates
                        // against it: measured pinned at 2.78 ms through a
                        // heavy stretch while the excess reached 3.53 ms, with
                        // the real frames lost for the duration.
                        //
                        // The trade is explicit. Every millisecond of bias is a
                        // millisecond less between the synthetic and the real
                        // frame, and below about 9 ms the two risk the same
                        // scanout - which is the failure this whole mechanism
                        // exists to prevent. period/3 leaves 7.4 ms and is
                        // about as far as that can go before the two
                        // constraints meet.
                        const auto ceiling = period / 4;
                        const bool steamvr = state->dispatch &&
                            state->dispatch->steamvr_runtime;
                        // Both failures are now observable, so the bias is bounded
                        // by measurement at both ends instead of by a constant.
                        // Too little and the synthetic goes over unfinished,
                        // the runtime blocks and the real frame is lost; too
                        // much and the real frame follows too soon and the
                        // synthetic is lost. Walking on the excess alone only
                        // ever saw the first, so it pushed until the second
                        // stopped it - period/6 short, period/4 close,
                        // period/3 past it, and none of those numbers survive a
                        // different headset.
                        //
                        // Order matters. While the block is present the gap is
                        // compressed *by the block*, not by the bias, so the
                        // block is dealt with first; the gap only governs once
                        // it is suppressed. Without that the controller cannot
                        // climb out of a bad state, because the state it is in
                        // makes the gap look too tight to act.
                        const auto gap_floor = period * 9 / 10;
                        const bool gap_tight =
                            state->presenter_pair_gap_mean.count() != 0 &&
                            state->presenter_pair_gap_mean < gap_floor;
                        if (steamvr && excess > kBlockThreshold) {
                            state->presenter_pair_bias =
                                state->presenter_pair_bias + climb > ceiling
                                    ? ceiling
                                    : state->presenter_pair_bias + climb;
                        } else if (gap_tight || !steamvr ||
                                   excess < kSuppressedThreshold) {
                            state->presenter_pair_bias =
                                state->presenter_pair_bias > decay
                                    ? state->presenter_pair_bias - decay
                                    : std::chrono::nanoseconds::zero();
                        }
                        xrfg::bridge_flight_logger().event(
                            xrfg::BridgeFlightOperation::presenter_transition,
                            300,
                            static_cast<std::uint64_t>(
                                state->presenter_pair_bias.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_synthetic_call_mean.count()),
                            static_cast<std::uint64_t>(
                                state->presenter_pair_gap_mean.count()));
                    }
                    const auto pair_bias = state->presenter_pair_bias;
                    const auto advance = request
                        ? (fresh_synthetic ? period - pair_bias
                                           : period + pair_bias)
                        : period;
                    state->presenter_next_submit += advance;
                    // Pull the grid back towards the best phase this
                    // presenter has managed. Everything else here corrects
                    // the grid's *rate* - a repeated scanout or a skipped
                    // one - and so only runs when the rate is wrong. Once one
                    // submission lands per scanout the rate is right at every
                    // phase, including phases that put the submission on top
                    // of the runtime's deadline, so those corrections switch
                    // off and whatever offset the last disturbance left is
                    // frozen. Captured: a hard scene walked the offset 7.7 ms
                    // later in two steps and it never came back, while the
                    // layer's own view stayed a flawless 45 in, 90 out, 100%
                    // on grid - the grid was self-consistent and simply in
                    // the wrong place.
                    //
                    // predictedDisplayTime is the runtime's own scanout time,
                    // so the interval from it back to the submission says
                    // where in the period this presenter sits. Only *where in
                    // the period* though - the lead itself is not comparable
                    // across a skipped slot. The runtime advances
                    // predictedDisplayTime by exactly one period per wait
                    // whether or not the presenter filled the slot, so missing
                    // one costs a whole period of lead while changing nothing
                    // about the phase. Measured on MSFS 2024 at a flat 45 in,
                    // 90 out with nothing missing: the controller read every
                    // skip as an 11 ms deficit, pulled the grid earlier by
                    // period/16 each frame chasing a period it cannot recover,
                    // and the walk tripped the step-over into the next skip.
                    // A 5 Hz limit cycle - submissions 10.645 ms apart against
                    // an 11.111 ms period across 3117 samples, ~4 slots a
                    // second discarded, and the pace sleep sawtoothing from
                    // 11 ms down to 2 and back, which is what the application
                    // sees as ratcheting CPU frame time.
                    //
                    // So reduce both sides modulo the period before comparing.
                    // A skipped slot then reads as no phase error at all,
                    // which is the truth.
                    //
                    // Reducing is not enough on its own, and the first attempt
                    // at it (V154) made a worse failure than the one it fixed.
                    // It took the difference the short way round the period,
                    // into (-period/2, period/2], so a loss of more than half
                    // a period came back with the wrong sign. Measured in
                    // Callisto Protocol: SteamVR's own xrEndFrame ran 3.8 to
                    // 8.0 ms on alternating frames for 80 ms, the presenter
                    // overran and recovered onto a grid point through one
                    // 19.4 ms gap, and the margin to the runtime's scanout
                    // dropped 8.3 ms in one step. The controller read that as
                    // being 2.8 ms *early*, corrected nothing, and the grid
                    // stayed 8.3 ms closer to the deadline for the remaining
                    // 26 seconds - a flawless 45 in, 90 out, 99.92% on grid,
                    // all of it landing too late to be shown.
                    //
                    // The two readings describe the same timeline and the lead
                    // cannot separate them. So do not try: take the deficit
                    // into [0, period) and always read it as being behind,
                    // because pulling earlier is the direction that gains
                    // margin and the step-over below is what stops it bunching.
                    // Two dead bands keep that from firing on noise - nothing
                    // under a millisecond, and nothing within an eighth of a
                    // period of a whole one, which is the reference sitting
                    // just under a steady phase after a decay step.
                    const std::int64_t period_ns = period.count();
                    const std::int64_t submitted_phase =
                        ((submitted_lead % period_ns) + period_ns) % period_ns;
                    // Distance from b forward to a, in [0, period).
                    const auto phase_deficit =
                        [period_ns](std::int64_t difference) -> std::int64_t {
                        return ((difference % period_ns) + period_ns) %
                            period_ns;
                    };
                    const std::int64_t kAheadBand = period_ns / 8;

                    // Whether the *rate* was right for this submission. Phase
                    // is only meaningful once it is - a grid that is skipping
                    // slots has no stable phase to correct towards - so the
                    // correction waits for a run of these.
                    const auto since_previous =
                        state->presenter_last_submitted_at ==
                            std::chrono::steady_clock::time_point{}
                        ? period
                        : (now - state->presenter_last_submitted_at);
                    // Against what the schedule asked for, not against one
                    // period: the pair is deliberately biased above, so an
                    // even period is the wrong expectation for both halves of
                    // it and would read every submission as off grid.
                    const auto expected =
                        state->presenter_expected_interval >
                            std::chrono::nanoseconds::zero()
                        ? state->presenter_expected_interval
                        : period;
                    const bool landed_on_grid =
                        since_previous > expected - period / 10 &&
                        since_previous < expected + period / 10;
                    state->presenter_on_grid_streak =
                        landed_on_grid ? state->presenter_on_grid_streak + 1 : 0;
                    state->presenter_last_submitted_at = now;
                    state->presenter_expected_interval = advance;

                    // The reference follows the best phase achieved, and bleeds
                    // down about a period every four seconds so a runtime that
                    // genuinely changes its timing is tracked rather than
                    // chased forever against a stale best.
                    //
                    // Only a submission that landed on the grid may raise the
                    // reference, and only by a small step. Without the grid
                    // test the controller feeds itself: pulling the deadline
                    // earlier lengthens the lead, the longer lead becomes the
                    // new best, and the next frame is pulled earlier again,
                    // creeping forward until the step-over shoves the deadline
                    // a whole period and starts over. Measured on a 90 Hz
                    // Pimax with the application flat at 45/s: the phase
                    // walked 6 ms earlier across a minute, the mean gap stayed
                    // a perfect 11.11 ms, and only 78% of submissions landed in
                    // their slot. Without the step limit the Callisto case
                    // above would latch its own 8.3 ms loss as the new best,
                    // because that loss is also readable as a small gain.
                    //
                    // This runs before the correction so that a submission
                    // which really is ahead sets the mark rather than being
                    // corrected towards a mark it has already passed.
                    const std::int64_t ahead_of_reference = phase_deficit(
                        submitted_phase - state->presenter_lead_reference);
                    if (!state->presenter_lead_valid ||
                        (landed_on_grid && ahead_of_reference > 0 &&
                         ahead_of_reference <= kAheadBand)) {
                        state->presenter_lead_reference = submitted_phase;
                        state->presenter_lead_valid = true;
                    } else {
                        constexpr std::int64_t kLeadReferenceDecay = 31'000;
                        state->presenter_lead_reference = phase_deficit(
                            state->presenter_lead_reference -
                            kLeadReferenceDecay);
                    }

                    // Eight consecutive on-grid submissions is a quarter of a
                    // second at 90 Hz. It is what separates the two captures
                    // above: Callisto ran clean for 26 s carrying its 8.3 ms
                    // loss, so the correction gets to run, while the MSFS
                    // capture was a deliberately hard scene skipping slots
                    // about twelve times a second, where it stays switched off
                    // and cannot ratchet.
                    if (state->presenter_on_grid_streak >=
                        kPhaseCorrectionGridStreak) {
                        const std::int64_t deficit = phase_deficit(
                            state->presenter_lead_reference - submitted_phase);
                        constexpr std::int64_t kLeadTolerance = 1'000'000;
                        if (deficit > kLeadTolerance &&
                            deficit < period_ns - kAheadBand) {
                            const auto correction =
                                std::min<std::chrono::nanoseconds>(
                                    period / 16,
                                    std::chrono::nanoseconds(deficit));
                            state->presenter_next_submit -= correction;
                        }
                    }
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
                    // The threshold has to clear the bunching without eating
                    // slots the presenter could still have filled. Stepping
                    // over costs a whole scanout - the slot shows a repeat -
                    // so the band is not free, and at half a period it fires
                    // on roughly half the deadlines left behind an overrun,
                    // which is exactly the case where content is already
                    // scarce. A quarter still clears the 3.7 ms typical
                    // xrEndFrame that produces the bunching, and keeps the
                    // deadlines between a quarter and a half of a period out
                    // - about 2.8 ms of every 11.11 - that half a period
                    // discarded.
                    //
                    // Steady state is unaffected either way: the deadline
                    // there already falls about 7 ms after the handover, so
                    // neither threshold fires. Chaining every deadline off
                    // the handover instead would add the loop's own cost to
                    // each cycle, which is what held an earlier build to
                    // 64/s.
                    //
                    // If this is too narrow it will show as submission gaps
                    // bunching under about 9 ms against the 11.11 ms period,
                    // which is the compositor discarding one of a pair.
                    const auto earliest = now + period / 4;
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
        if(XR_SUCCEEDED(end_result) && request && request->owned_frame && !fresh_synthetic &&
            submitted_layer_count && frame_state.shouldRender)
            blackbar_observe(*state,request->owned_frame->blackbar_pose,frame_state.predictedDisplayTime,
                state->presenter_display_period,frame_state.predictedDisplayPeriod,true);
        // c carries which half of the pair this was: 2 synthetic, 1 current,
        // 0 a repeat. Without it the two are indistinguishable in a capture -
        // anchoring on the enqueue does not work, because the submission that
        // follows it is the previous pair's current, not this pair's
        // synthetic. That mislabelling is what made the V162 margin
        // comparison name the wrong frame.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_submission,
            end_result,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
            request ? (fresh_synthetic ? 2u : 1u) : 0u);
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_inflight_submission.reset();
            state->presenter_inflight_frame.reset();
            if (request) {
                request->result = end_result;
                request->completed = true;
                if (state->outstanding_presenter_submissions != 0) {
                    --state->outstanding_presenter_submissions;
                }
                if (XR_SUCCEEDED(end_result) && request->owned_frame &&
                    !state->pipelined_presenter_mode) {
                    state->presenter_last_frame = request->owned_frame;
                    if (request->owned_frame->synthetic) {
                        state->presenter_pending_current_pair_sequence =
                            request->pair_sequence;
                    } else if (request->pair_sequence != 0 &&
                               request->pair_sequence ==
                                   state->presenter_pending_current_pair_sequence) {
                        state->presenter_pending_current_pair_sequence = 0;
                    }
                } else if (state->pipelined_presenter_mode) {
                    // A persistent pipelined application may destroy or replace
                    // XrSpace and non-projection swapchain handles as soon as
                    // its xrEndFrame returns. Never repeat retained application
                    // handles beyond that call boundary; let the runtime hold
                    // the last submitted image until the next owned pair.
                    state->presenter_last_frame.reset();
                    state->presenter_pending_current_pair_sequence = 0;
                }
            }
            if (frame_content_refused(end_result)) {
                state->presenter_last_frame.reset();
                state->presenter_pending_current_pair_sequence = 0;
            } else if (XR_FAILED(end_result)) {
                fail_pending_presenter_submissions_locked(*state, end_result);
            }
        }
        state->presenter_condition.notify_all();
        // Instrumentation runs after the submission's ownership and queue
        // state are settled, so an optional runtime conversion cannot delay
        // the next application admission for this pair.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_source,
            0,
            request ? request->sequence : 0,
            static_cast<std::uint64_t>(source_display_time),
            request ? request->pair_sequence : 0);
        if (state->dispatch->convert_time_to_qpc != nullptr &&
            (clock_anchor_tick++ % 64) == 0) {
            LARGE_INTEGER predicted_counter{};
            const XrResult conversion_result =
                state->dispatch->convert_time_to_qpc(
                    state->dispatch->instance,
                    frame_state.predictedDisplayTime,
                    &predicted_counter);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::presenter_clock_anchor,
                conversion_result,
                static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
                XR_SUCCEEDED(conversion_result)
                    ? static_cast<std::uint64_t>(
                          xrfg::bridge_flight_logger().microseconds_for_counter(
                              predicted_counter.QuadPart))
                    : 0,
                static_cast<std::uint64_t>(qpc_frequency.QuadPart));
        }
        if (XR_FAILED(end_result) && !frame_content_refused(end_result)) {
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

// Fresh-output consumer for the NVIDIA three-slot director, legacy NVIDIA
// S/B queue, and SteamVR FSR S/B queue. It never submits a retained real twice
// and never emits empty frames just to keep a counter moving.
void three_slot_director_main(const std::shared_ptr<SessionState>& state) noexcept {
    try {
        // Both consumers use a fixed grid rather than anchoring each deadline
        // to a late wake-up. Keep FSR's long-End recovery distinct from NVIDIA.
        xrfg::TwoOutputSchedule fsr_schedule;
        xrfg::TwoOutputSchedule director_schedule;
        xrfg::MidpointSchedule midpoint_schedule;
        DjulesAbsoluteSchedule djules_schedule;
        xrfg::OpticalPacingReserve optical_reserve;
        if(fsr_steamvr_phase_clock(*state)) state->djules_phase_cooldown=0;
        std::chrono::steady_clock::time_point djules_last_returned{};
        std::shared_ptr<GeneratedFrameEndInfo> djules_last_owned;
        auto configuration_epoch=state->djules_configuration_epoch.load();
        for (;;) {
            std::shared_ptr<ThreeSlotFrame> selected;
            std::shared_ptr<PresenterSubmission> borrowed;
            const auto ready_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_director_wait, handle_value(state->handle));
            {
                std::unique_lock lock(state->presenter_mutex);
                state->presenter_condition.wait(lock, [&] {
                    return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                        !state->presenter_submissions.empty() ||
                        (state->three_slots.releasable(state->director_draining || single_director_pacing(*state)) &&
                         state->three_slots.slot(0)->value->frame);
                });
                if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                if (state->three_slots.releasable(state->director_draining || single_director_pacing(*state)) &&
                    state->three_slots.slot(0)->value->frame) {
                    selected = state->three_slots.slot(0)->value;
                    state->presenter_inflight_frame = selected->frame;
                } else {
                    borrowed = state->presenter_submissions.front();
                    state->presenter_submissions.pop_front();
                    state->presenter_inflight_submission = borrowed;
                    state->presenter_inflight_frame = borrowed->owned_frame;
                }
            }
            const auto fail = [&](XrResult result) {
                std::scoped_lock lock(state->presenter_mutex);
                if (borrowed) { borrowed->result = result; borrowed->completed = true; }
                fail_pending_presenter_submissions_locked(*state, result);
                state->presenter_inflight_frame.reset();
                state->presenter_inflight_submission.reset();
                state->presenter_condition.notify_all();
            };
            xrfg::bridge_flight_logger().end(ready_token,
                xrfg::BridgeFlightOperation::timing_director_wait, 0,
                selected ? selected->source_sequence : 0,
                selected ? selected->previous_sequence : 0);
            // Enter the SteamVR cycle with the exact queued output ready.
            // This is a per-output receipt, not a global application/queue
            // idle wait. The producer remains free to render the next frame.
            const auto* ready_frame = selected ? selected->frame.get()
                : borrowed->owned_frame.get();
            const auto probe_sequence = selected ? selected->source_sequence : borrowed->sequence;
            log_native_delivery_probe(*state, ready_frame, probe_sequence, 0);
            bool readiness_failed = false;
            if (ready_frame) for (const auto& ready : ready_frame->reference_readiness) {
                const auto app_completed=state->app_release_fence ? state->app_release_fence->GetCompletedValue() : 0;
                const bool application_released=state->binding_queue && state->app_release_fence &&
                    ready_frame->app_release_value && app_completed!=UINT64_MAX &&
                    app_completed>=ready_frame->app_release_value;
                const bool inputs_released=state->binding_queue && state->app_release_fence &&
                    ready_frame->input_release_value && app_completed!=UINT64_MAX &&
                    app_completed>=ready_frame->input_release_value && ready.inputs.ready();
                if (ready.require_application_release && !application_released && !inputs_released) {
                    // XR progress may be what releases the input/application
                    // queue. Never park that cycle on an unproven dependency.
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::timing_synthesis_complete,
                        S_FALSE,548,ready_frame->app_release_value,
                        state->app_release_fence?state->app_release_fence->GetCompletedValue():0);
                    continue;
                }
                if (ready.require_application_release && !application_released && inputs_released)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::timing_synthesis_complete,
                        S_OK,552,ready_frame->input_release_value,ready_frame->app_release_value);
                const auto ready_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::timing_synthesis_complete, 524, ready.value);
                const auto ready_result = wait_composition_fence(ready.fence.Get(), ready.value,
                    kFrameStartSynthesisWaitMilliseconds);
                xrfg::bridge_flight_logger().end(ready_token,
                    xrfg::BridgeFlightOperation::timing_synthesis_complete, ready_result);
                if (FAILED(ready_result)) { fail(XR_ERROR_RUNTIME_FAILURE); readiness_failed = true; break; }
            }
            if (readiness_failed) break;
            // Do not also wait for auxiliary copies or application queue work.
            // Those keep their existing GPU-side dependencies and image leases.
            XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            auto token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_wait_frame, handle_value(state->handle));
            XrResult result = wait_presenter_runtime_frame(state, &wait, &prediction);
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_wait_frame,
                result, prediction.predictedDisplayTime, prediction.predictedDisplayPeriod,
                prediction.shouldRender);
            if (XR_FAILED(result)) { fail(result); break; }
            log_native_delivery_probe(*state, ready_frame, probe_sequence, 1,
                prediction.predictedDisplayTime);
            {
                std::scoped_lock lock(state->presenter_mutex);
                state->presenter_frame_state = prediction;
                state->presenter_frame_state.next = nullptr;
                state->presenter_frame_state_valid = true;
                ++state->presenter_frame_serial;
                if (prediction.predictedDisplayPeriod >= 2'000'000 &&
                    prediction.predictedDisplayPeriod <= 50'000'000 &&
                    (!state->presenter_display_period ||
                     prediction.predictedDisplayPeriod < state->presenter_display_period))
                    state->presenter_display_period = prediction.predictedDisplayPeriod;
                state->history_capture_wait_period.store(state->presenter_display_period, std::memory_order_relaxed);
            }
            refresh_djules_display_period(state);
            refresh_reference_display_period(state);
            refresh_luke_display_period(state);
            state->presenter_condition.notify_all();
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_begin_frame, handle_value(state->handle));
            result = with_runtime_entry(state, [&] {
                return state->dispatch->begin_frame(state->handle, &begin);
            });
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_begin_frame,
                result, prediction.predictedDisplayTime);
            if (XR_FAILED(result)) { fail(result); break; }
            // Only the consumer is limited to headset cadence. The game's
            // WaitFrame contains no sleep and no dependency on director ticks.
            bool synthetic = selected ? selected->frame->synthetic
                : borrowed->owned_frame && borrowed->owned_frame->synthetic;
            const auto pace_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::presenter_pace);
            std::chrono::steady_clock::time_point submission_slot{};
            if(!state->private_runtime) {
                const auto period = std::chrono::nanoseconds(state->presenter_display_period);
                const auto now = std::chrono::steady_clock::now();
                if ((djules_native_scheduler(*state) && (state->three_slot_director || state->queued_nvidia_presenter)) ||
                    fsr_steamvr_phase_clock(*state)) {
                    if (state->dispatch->steamvr_runtime || state->dispatch->virtual_desktop_runtime)
                        submission_slot = djules_schedule.claim(now, period);
                    else
                        submission_slot = djules_handover_floor(now, djules_last_returned, period);
                }
                else if (state->three_slot_director)
                    submission_slot = midpoint_schedule.claim(now, period, synthetic);
                else {
                    auto& schedule = state->fsr_two_slot_presenter ? fsr_schedule : director_schedule;
                    submission_slot = schedule.claim(now, period);
                }
                if(fsr_steamvr_phase_clock(*state))
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_clock_anchor,
                        5572,probe_sequence,std::chrono::duration_cast<std::chrono::nanoseconds>(submission_slot.time_since_epoch()).count(),period.count());
                const auto* probe=selected?selected->frame.get():borrowed->owned_frame.get();
                if(djules_triple_active(*state) && probe && probe->djules_leader) {
                    const auto deadline=submission_slot-std::chrono::milliseconds(4);
                    if(deadline>std::chrono::steady_clock::now()) std::this_thread::sleep_until(deadline);
                    bool ready=!probe->pending_outputs.empty();
                    for(const auto& output:probe->pending_outputs)
                        ready &= output.synthesizer->ticket_available(output.ticket)==S_OK;
                    for(const auto& image:probe->composition_images)
                        ready &= !image->fence12 || image->fence12->GetCompletedValue()>=image->ready_value;
                    std::int64_t delay_us=-1,ceiling_us=0;unsigned late=0;
                    { std::scoped_lock lock(state->presenter_mutex);
                      if(state->djules_triple_release.observe(ready,period)) {
                        delay_us=state->djules_triple_release.delay.count()/1000;
                        ceiling_us=state->djules_triple_release.ceiling.count()/1000;
                        late=state->djules_triple_release.last_late;
                      } }
                    if(delay_us>=0) xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::presenter_transition,801,delay_us,late,ceiling_us);
                }
                const auto remaining = submission_slot - std::chrono::steady_clock::now();
                if (remaining > std::chrono::steady_clock::duration::zero()) {
                    if (!state->presenter_pace_timer) {
                        state->presenter_pace_timer = CreateWaitableTimerExW(nullptr, nullptr,
                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
                        if (!state->presenter_pace_timer)
                            state->presenter_pace_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
                    }
                    LARGE_INTEGER due{};
                    due.QuadPart = -(std::max)(1LL,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
                    if (state->presenter_pace_timer && SetWaitableTimer(state->presenter_pace_timer,
                            &due, 0, nullptr, nullptr, FALSE))
                        static_cast<void>(WaitForSingleObject(state->presenter_pace_timer, INFINITE));
                    else
                        std::this_thread::sleep_until(submission_slot);
                }
            }
            xrfg::bridge_flight_logger().end(pace_token,
                xrfg::BridgeFlightOperation::presenter_pace, 0);
            const auto epoch=state->djules_configuration_epoch.load();
            if(epoch!=configuration_epoch) {
                optical_reserve = {};
                djules_last_owned.reset(); djules_schedule={}; djules_last_returned={};
                fsr_schedule={}; director_schedule={}; midpoint_schedule={};
                state->djules_phase_cooldown=0;
                configuration_epoch=epoch;
            }
            bool repeat_held=false;
            bool drain_requested=false;
            { std::scoped_lock lock(state->presenter_mutex); drain_requested=state->director_draining; }
            if(!state->private_runtime && selected && synthetic && djules_deep_active(*state) &&
                !drain_requested) {
                bool ready=true, output_failed=false;
                for(const auto& output:selected->frame->pending_outputs) {
                    const HRESULT available=output.synthesizer->ticket_available(output.ticket);
                    if(FAILED(available)) { fail(XR_ERROR_RUNTIME_FAILURE); output_failed=true; break; }
                    ready &= available==S_OK;
                }
                if(output_failed) break;
                repeat_held=hold_djules_synthetic(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now()-selected->queued_at),
                    std::chrono::nanoseconds(state->presenter_display_period),ready,
                    djules_last_owned!=nullptr,state->pipelined_presenter_mode);
                if(repeat_held) {
                    synthetic=false;
                    std::scoped_lock lock(state->presenter_mutex);
                    state->presenter_inflight_frame=djules_last_owned;
                }
            }
            const XrFrameEndInfo* source = repeat_held?&djules_last_owned->info : selected ? &selected->frame->info
                : borrowed->owned_frame ? &borrowed->owned_frame->info : borrowed->borrowed_frame;
            XrFrameEndInfo end = *source;
            end.next = nullptr;
            end.displayTime = prediction.predictedDisplayTime;
            if (!prediction.shouldRender) { end.layerCount = 0; end.layers = nullptr; }
            token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::internal_end_frame, handle_value(state->handle),
                end.displayTime, end.layerCount);
            std::chrono::nanoseconds phase_step{};
            {
                std::scoped_lock content_lock(state->presenter_content_mutex);
                result = with_runtime_entry(state, [&] {
                    if(selected && xrfg::bridge_flight_logger().ram_enabled()) {
                        const auto stage=synthetic ? 12 : 13;
                        log_luke_projection_poses(&end,selected->source_sequence,stage);
                        for(std::uint32_t layer=0;end.layers && layer<end.layerCount && layer<8;++layer) {
                            const auto* header=end.layers[layer];
                            if(!header || header->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(header);
                            for(std::uint32_t view=0;projection->views && view<projection->viewCount && view<8;++view)
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_inline_view,
                                    stage*100+layer*10+view,selected->source_sequence,
                                    handle_value(projection->space),handle_value(projection->views[view].subImage.swapchain));
                        }
                    }
                    auto* owned=repeat_held?djules_last_owned.get():selected?selected->frame.get():borrowed->owned_frame.get();
                    if (owned && xrfg::bridge_flight_logger().enabled()) {
                        for (std::size_t i = 0; i < owned->pending_outputs.size() &&
                            i < owned->diagnostic_handover.size(); ++i) {
                            const auto& generation = *owned->pending_outputs[i].generation;
                            owned->diagnostic_handover[i] = {
                                generation.handover_fence, generation.handover_serial + 1, false};
                        }
                    }
                    join_runtime_queue_to_application(state.get(),owned?owned->app_release_value:borrowed->app_release_value);
                    if(owned && advanced_native_outputs(*state)) {
                        for(const auto& image:owned->composition_images)
                            if(image->fence12 && FAILED(runtime_queue(*state)->Wait(image->fence12.Get(),image->ready_value)))
                                return XR_ERROR_RUNTIME_FAILURE;
                    }
                    if(owned) for(const auto& publication:owned->direct_publications)
                        if(!publication->publish()) return XR_ERROR_RUNTIME_FAILURE;
                    if(owned && !handover_djules_outputs(state.get(),owned->pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
                    log_native_delivery_probe(*state, owned, probe_sequence, 2, end.displayTime);
                    phase_step = sample_djules_phase_step(state, !synthetic && !repeat_held && owned,
                        selected ? selected->source_sequence : borrowed->pair_sequence);
                    if(!repeat_held) report_native_frame_content(state.get(),owned);
                    return state->fps_overlay ? state->fps_overlay->end_frame(&end, synthetic,!repeat_held,native_frame_results(owned),owned?owned->native_output:0, owned ? owned->interface_stamp : borrowed ? borrowed->interface_stamp : xrfg::InterfaceFrameStamp{})
                        : content_end_frame(state->handle, &end);
                });
            }
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::internal_end_frame,
                result, end.displayTime, end.layerCount, synthetic ? 1 : 0);
            log_native_delivery_probe(*state, ready_frame, probe_sequence, 3, end.displayTime);
            if(ready_frame) for(const auto& publication:ready_frame->direct_publications) publication->finish();
            if(XR_SUCCEEDED(result) && !synthetic && !repeat_held && end.layerCount) {
                const auto* owned=selected?selected->frame.get():borrowed->owned_frame.get();
                if(owned) blackbar_observe(*state,owned->blackbar_pose,end.displayTime,
                    state->presenter_display_period,prediction.predictedDisplayPeriod,true);
            }
            // Selected frames are retired below; borrowed callers receive this
            // frame's error. The director keeps its existing cadence and never
            // repeats the refused frame. Only genuine session/runtime failure
            // aborts all following requests.
            if (XR_FAILED(result) && !frame_content_refused(result)) { fail(result); break; }
            if (state->fsr_two_slot_presenter && borrowed && borrowed->owned_frame) {
                // Preserve the inline contract: B's deferred copy is submitted
                // only after S has reached XR, before B gets its own slot.
                for (const auto& synthesis : borrowed->owned_frame->pending_current_copies) {
                    const HRESULT copied = synthesis ? synthesis->flush_current_copy(
                        state->d3d12_synthesis_queue ? runtime_queue(*state) : nullptr) : S_OK;
                    if (FAILED(copied)) { fail(XR_ERROR_RUNTIME_FAILURE); return; }
                }
            }
            if (!single_director_pacing(*state)) {
                if (state->fsr_two_slot_presenter && !fsr_steamvr_phase_clock(*state))
                    fsr_schedule.submitted(submission_slot, std::chrono::nanoseconds(state->presenter_display_period),
                        std::chrono::steady_clock::now());
                else if ((djules_native_scheduler(*state) && (state->three_slot_director || state->queued_nvidia_presenter)) ||
                        fsr_steamvr_phase_clock(*state)) {
                    djules_last_returned = std::chrono::steady_clock::now();
                    djules_schedule.submitted(submission_slot,
                        std::chrono::nanoseconds(state->presenter_display_period), djules_last_returned,
                        XR_SUCCEEDED(result) ? phase_step : std::chrono::nanoseconds{});
                }
                else if (state->three_slot_director)
                    midpoint_schedule.submitted(submission_slot,
                        std::chrono::nanoseconds(state->presenter_display_period));
                else
                    // Runtime blocking already consumed wall time. Advance the
                    // intended grid, not the late actual-submit time, and let
                    // claim() reanchor after a complete missed interval.
                    director_schedule.submitted(submission_slot,
                        std::chrono::nanoseconds(state->presenter_display_period),
                        // A stalled Reference End must leave S a full exposure
                        // before B; ordinary jitter keeps the intended grid.
                        state->dispatch->steamvr_runtime && state->queued_nvidia_presenter &&
                            state->graphics_binding==SessionGraphicsBinding::d3d12 &&
                            !state->nvidia_new_route && !state->luke_afw_enabled &&
                            !state->luke_outlaws_preframe && !state->hybrid_requested && !state->nvidia_options.hybrid &&
                            state->frame_generation_mode==xrfg::D3D12FrameGenerationMode::ofxr
                            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{});
            } // The private director owns timing; no upstream schedule advances.
            if (adaptive_optical_reserve_route(*state) && XR_SUCCEEDED(result) && end.layerCount) {
                const auto returned = std::chrono::steady_clock::now();
                const auto period = std::chrono::nanoseconds(state->presenter_display_period);
                const auto ns = [](auto at) { return std::chrono::duration_cast<std::chrono::nanoseconds>(
                    at.time_since_epoch()).count(); };
                const auto budget = optical_reserve.observe(ns(returned), ns(submission_slot), period.count());
                const auto previous = state->optical_reserve_ns.exchange(budget, std::memory_order_relaxed);
                {
                    std::scoped_lock lock(state->presenter_mutex);
                    if (synthetic && borrowed) {
                        state->optical_reserve_pair = borrowed->pair_sequence;
                        state->optical_reserve_current_due = djules_native_scheduler(*state)
                            ? djules_schedule.claim(returned, period) : director_schedule.claim(returned, period);
                    }
                }
                if (budget != previous)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                        6680, budget, previous, period.count());
            }
            {
                std::scoped_lock lock(state->presenter_mutex);
                if (selected) {
                    if(!repeat_held) {
                        if(XR_SUCCEEDED(result) && djules_deep_active(*state))
                            djules_last_owned=selected->frame;
                        static_cast<void>(state->three_slots.release_front(state->director_draining || single_director_pacing(*state)));
                    }
                } else {
                    borrowed->result = result;
                    borrowed->completed = true;
                    if (state->outstanding_presenter_submissions)
                        --state->outstanding_presenter_submissions;
                }
                state->presenter_inflight_frame.reset();
                state->presenter_inflight_submission.reset();
                state->presenter_schedule_valid = state->presenter_display_period > 0;
            }
            if (state->three_slot_director)
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_present,
                repeat_held ? 2 : synthetic ? 1 : 0, selected ? selected->source_sequence : 0,
                selected ? selected->previous_sequence : 0, end.displayTime);
            if (state->frame_capture && state->frame_capture->active()) {
                try {
                state->frame_capture->record("{\"kind\":\"present\",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+
                    ",\"source_id\":"+std::to_string(selected ? selected->source_sequence : 0)+
                    ",\"previous_id\":"+std::to_string(selected ? selected->previous_sequence : 0)+
                    ",\"synthetic\":"+(synthetic ? "true" : "false")+",\"display_time\":"+std::to_string(end.displayTime)+"}");
                } catch (...) {}
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_submission,
                result, selected ? selected->source_sequence : borrowed->sequence,
                end.displayTime, synthetic ? 2 : 1);
            state->presenter_condition.notify_all();
        }
    } catch (...) {
        std::scoped_lock lock(state->presenter_mutex);
        fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
    }
    state->presenter_condition.notify_all();
}

[[nodiscard]] bool start_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> seed_frame,
    bool preserve_virtual_timeline) noexcept {
    try {
        if (presenter_forbidden(*state) || djules_inline_runtime(*state)) return false;
        std::scoped_lock lock(state->presenter_mutex);
        if (state->presenter_active) {
            return true;
        }
        state->presenter_adopted_frame.reset();
        if (state->runtime_frame_waited_unbegun && state->last_inline_frame_state_valid) {
            state->presenter_adopted_frame = state->last_inline_frame_state;
            state->presenter_adopted_frame->next = nullptr;
            state->runtime_frame_waited_unbegun = false;
        }
        state->presenter_submissions.clear();
        state->presenter_last_frame.reset();
        state->three_slot_director = three_slot_generation_session(state);
        state->fsr_two_slot_presenter = fsr_two_slot_session(state);
        state->queued_nvidia_presenter = queued_nvidia_session(state);
        state->three_slots.configure(djules_triple_director(*state)?3:2);
      state->director_anchor_sequence = 0;
        state->synthesis_job.reset();
        state->synthesis_busy = false;
        state->director_draining = false;
        state->next_source_sequence = 1;
        state->presenter_pending_current_pair_sequence = 0;
        state->presenter_inflight_submission.reset();
        state->presenter_inflight_frame.reset();
        state->presenter_frame_state = XrFrameState{XR_TYPE_FRAME_STATE};
        if (!preserve_virtual_timeline) {
            state->last_virtual_display_time = 0;
        }
        state->presenter_failure = XR_SUCCESS;
        state->next_presenter_sequence = 1;
        state->outstanding_presenter_submissions = 0;
        state->optical_reserve_ns = 0;
        state->optical_reserve_current_due = {};
        state->optical_reserve_pair = state->optical_reserve_served = 0;
        if (state->three_slot_director || state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            // Seed was already presented by the inline path. Do not repeat it.
            seed_frame.reset();
        } else if (state->pipelined_presenter_mode && seed_frame) {
            auto request = std::make_shared<PresenterSubmission>();
            request->sequence = state->next_presenter_sequence++;
            request->owned_frame = std::move(seed_frame);
            state->presenter_submissions.push_back(std::move(request));
            state->outstanding_presenter_submissions = 1;
        } else {
            state->presenter_last_frame = std::move(seed_frame);
        }
        state->presenter_frame_state_valid = false;
        if ((state->three_slot_director || state->fsr_two_slot_presenter || state->queued_nvidia_presenter) && state->last_inline_frame_state_valid) {
            // Bootstrap the virtual producer without empty/repeated runtime
            // submissions. The director waits for an eligible buffer entry.
            state->presenter_frame_state = state->last_inline_frame_state;
            state->presenter_frame_state.next = nullptr;
            state->presenter_frame_state_valid = true;
        }
        // A serial from a previous presenter would either release the first
        // wait of this one straight away or never.
        state->presenter_frame_serial = 0;
        state->application_served_serial = 0;
        // A schedule left over from a previous presenter would stall the first
        // submission of this one.
        // A display time from a previous presenter says nothing about this
        // grid, and its schedule is about to be rebuilt.
        state->presenter_last_predicted_valid = false;
        state->presenter_schedule_valid = false;
        state->presenter_display_period = 0;
        state->presenter_stop_requested = false;
        state->presenter_active = true;
        if (ordinary_djules_route(*state) && state->vdxr_legacy)
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                602,state->pipelined_presenter_mode?1:0,state->presenter_restore_after_bypass?1:0,state->split_frame_loop?1:0);
        state->presenter_restore_after_bypass=false;
        if (state->three_slot_director) {
            state->synthesis_thread = std::thread(synthesis_worker_main, state);
            state->presenter_thread = std::thread(three_slot_director_main, state);
        } else if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            state->presenter_thread = std::thread(three_slot_director_main, state);
        } else {
            state->presenter_thread = std::thread(continuous_presenter_main, state);
        }
        return true;
    } catch (...) {
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_stop_requested = true;
        }
        state->presenter_condition.notify_all();
        if (state->synthesis_thread.joinable()) state->synthesis_thread.join();
        if (state->presenter_thread.joinable()) state->presenter_thread.join();
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
        if (state->synthesis_thread.joinable()) state->synthesis_thread.join();
        if (state->presenter_thread.joinable()) {
            state->presenter_thread.join();
        }
        {
            std::scoped_lock lock(state->presenter_mutex);
            state->presenter_active = false;
            state->synthesis_busy = false;
            state->synthesis_job.reset();
            state->three_slots.clear();
            state->director_anchor_sequence = 0;
            state->three_slot_director = false;
            state->fsr_two_slot_presenter = false;
            state->queued_nvidia_presenter = false;
            state->presenter_frame_state_valid = false;
            state->presenter_last_frame.reset();
            state->presenter_pending_current_pair_sequence = 0;
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

[[nodiscard]] bool three_slot_generation_session(
    const std::shared_ptr<SessionState>& state) noexcept {
    return !presenter_forbidden(*state) && !djules_inline_runtime(*state) && state->menu_enabled && state->frame_generation_mode !=
               xrfg::D3D12FrameGenerationMode::fsr3 &&
           state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
           (state->nvidia_new_route || fresh_luke_director_worker(*state)) &&
           (!direct_reduced_outputs(*state) || extra_output_capacity(*state)) &&
           (state->graphics_binding == SessionGraphicsBinding::d3d11 ||
            state->graphics_binding == SessionGraphicsBinding::d3d12) &&
           !state->pipelined_presenter_mode;
}

[[nodiscard]] bool fsr_two_slot_session(const std::shared_ptr<SessionState>& state) noexcept {
    return !state->luke_afw_enabled && comparison_delivery_runtime(*state) && state->menu_enabled &&
        state->graphics_binding == SessionGraphicsBinding::d3d12 &&
        state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3;
}

[[nodiscard]] bool queued_nvidia_session(const std::shared_ptr<SessionState>& state) noexcept {
    return !presenter_forbidden(*state) && !djules_inline_runtime(*state) && !state->luke_afw_enabled && state->menu_enabled &&
        state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        !three_slot_generation_session(state) &&
        (state->graphics_binding == SessionGraphicsBinding::d3d11 ||
         state->graphics_binding == SessionGraphicsBinding::d3d12);
}

void delay_djules_application_release(const std::shared_ptr<SessionState>& state) {
    if(state->private_runtime)return;
    std::chrono::nanoseconds delay{};
    { std::scoped_lock lock(state->presenter_mutex);
      if(djules_triple_active(*state)) delay=state->djules_triple_release.delay; }
    if(delay>std::chrono::nanoseconds::zero()) {
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait_until(lock,std::chrono::steady_clock::now()+delay,[&] {
            return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) || !state->menu_enabled;
        });
    }
}

// Holds the application until the presenter has run a whole pair since it was
// last released. Deliberately returns nothing: it is called after the frame has
// already been handed over, so a presenter that stops or fails while this waits
// must not turn a submitted frame into an error - it just stops waiting.
void wait_for_presenter_pair(
    const std::shared_ptr<SessionState>& state, std::uint64_t frames = 2) noexcept {
    try {
        const std::uint64_t kPresenterFramesPerPair = frames;
        const auto entered = std::chrono::steady_clock::now();
        std::unique_lock lock(state->presenter_mutex);
        state->presenter_condition.wait(lock, [&] {
            return state->presenter_frame_serial >=
                       state->application_served_serial +
                           kPresenterFramesPerPair ||
                   XR_FAILED(state->presenter_failure) ||
                   state->presenter_stop_requested;
        });
        if (XR_FAILED(state->presenter_failure) ||
            state->presenter_stop_requested) {
            return;
        }
        // Diagnostic only, and the number this hold is judged by. Assigning
        // the presenter's serial rather than advancing by the pair means an
        // application that arrives late catches up in one step: the surplus
        // below is what that step discards, and while it is non-zero this
        // hold is not throttling anything - the application is gated only by
        // whatever it blocks on next, which is the frame-start synthesis
        // fence, and that one meters nothing.
        const std::uint64_t served = state->application_served_serial;
        const std::uint64_t reached = state->presenter_frame_serial;
        const std::uint64_t surplus =
            reached > served + kPresenterFramesPerPair
            ? reached - served - kPresenterFramesPerPair
            : 0;
        state->application_served_serial = reached;
        // Never record under presenter_mutex: the presenter thread takes it
        // every frame, and this path has deadlocked the layer twice.
        lock.unlock();
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::presenter_pair_release,
            static_cast<std::int64_t>(surplus),
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - entered)
                    .count()),
            reached,
            served);
    } catch (...) {
    }
}

// This admission site cannot wait for a future dequeue: that output is
// produced only AFTER this call. Count the existing XR prediction cycles;
// a genuinely empty presenter must release admission so it can be fed.
[[nodiscard]] XrResult wait_for_native_counted_admission(
    const std::shared_ptr<SessionState>& state,bool pairable) noexcept {
    try {
        const auto started=std::chrono::steady_clock::now();
        std::unique_lock lock(state->presenter_mutex);
        const auto target=state->application_served_serial+2;
        state->presenter_condition.wait(lock,[&] {
            return !pairable || state->presenter_frame_serial>=target ||
                (state->presenter_submissions.empty() && !state->presenter_inflight_submission) ||
                state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
        });
        if(XR_FAILED(state->presenter_failure)) return state->presenter_failure;
        if(state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
        const auto reached=state->presenter_frame_serial;
        state->application_served_serial=reached;
        const auto outstanding=state->outstanding_presenter_submissions;
        lock.unlock();
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_pair_release,
            556,reached,target,outstanding);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            556,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-started).count()),reached,outstanding);
        return XR_SUCCESS;
    } catch(...) {return XR_ERROR_RUNTIME_FAILURE;}
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

// Preparation can overlap one complete older pair in the existing two-ring
// pool. The fractional/S-receipt wait belongs to publishing ready outputs.
[[nodiscard]] XrResult wait_for_native_preparation_admission(
    const std::shared_ptr<SessionState>& state,bool metadata_pairable) noexcept {
    const bool counted=steamvr_counted_native_release(*state);
    const bool reserved=adaptive_optical_reserve_route(*state) && state->optical_reserve_ns.load(std::memory_order_relaxed)>0;
    if(counted) {
        const auto result=wait_for_native_counted_admission(state,metadata_pairable);
        if(XR_FAILED(result))return result;
    }
    const auto result=wait_for_presenter_capacity(state,counted || reserved ? 2U :
        (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) ? 0U : 1U);
    if(XR_SUCCEEDED(result) && !reserved && adaptive_optical_reserve_route(*state)) {
        std::scoped_lock lock(state->presenter_mutex);
        state->optical_reserve_served=state->optical_reserve_pair;
    }
    return result;
}

[[nodiscard]] XrResult wait_for_presenter_idle(
    const std::shared_ptr<SessionState>& state) noexcept {
    try {
        std::unique_lock lock(state->presenter_mutex);
        if (state->three_slot_director) {
            state->director_draining = true;
            state->presenter_condition.notify_all();
            state->presenter_condition.wait(lock, [&] {
                return (!state->synthesis_busy && state->three_slots.empty() &&
                        !state->presenter_inflight_frame) ||
                       XR_FAILED(state->presenter_failure) || state->presenter_stop_requested;
            });
            state->director_draining = false;
        }
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
    const XrFrameEndInfo* borrowed_frame, xrfg::InterfaceFrameStamp stamp = {}) {
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
        request->interface_stamp = stamp;
        request->app_release_value = state->app_end_frame_release_value;
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
    std::shared_ptr<GeneratedFrameEndInfo> current,
    std::shared_ptr<GeneratedFrameEndInfo> middle=nullptr) noexcept {
    try {
        std::scoped_lock lock(state->presenter_mutex);
        if (!state->presenter_active || state->presenter_stop_requested ||
            XR_FAILED(state->presenter_failure)) {
            return XR_FAILED(state->presenter_failure)
                ? state->presenter_failure
                : XR_ERROR_SESSION_NOT_RUNNING;
        }
        // At publication only one older real output may remain. Never append
        // a ready pair behind another complete pair, even on reserve activation.
        if(adaptive_optical_reserve_route(*state) && state->outstanding_presenter_submissions>1)
            return XR_ERROR_RUNTIME_FAILURE;
        auto first = std::make_shared<PresenterSubmission>();
        // Native counted release permits two bounded pairs in the existing
        // two-ring pool. Other paths retain one pair; slot leases gate reuse.
        if ((state->fsr_two_slot_presenter || state->queued_nvidia_presenter) &&
            state->outstanding_presenter_submissions >
                ((steamvr_counted_native_release(*state) || adaptive_optical_reserve_route(*state)) ? 2U : 0U))
            return XR_ERROR_RUNTIME_FAILURE;
        first->sequence = state->next_presenter_sequence++;
        first->pair_sequence = first->sequence;
        first->owned_frame = std::move(synthetic);
        std::shared_ptr<PresenterSubmission> middle_request;
        if(middle) {
            middle_request=std::make_shared<PresenterSubmission>();
            middle_request->sequence=state->next_presenter_sequence++;
            middle_request->pair_sequence=first->pair_sequence;
            middle_request->owned_frame=std::move(middle);
        }
        auto second = std::make_shared<PresenterSubmission>();
        second->sequence = state->next_presenter_sequence++;
        second->pair_sequence = first->pair_sequence;
        second->owned_frame = std::move(current);
        const unsigned output_count=middle_request?3:2;
        state->presenter_submissions.push_back(std::move(first));
        if(middle_request) state->presenter_submissions.push_back(std::move(middle_request));
        state->presenter_submissions.push_back(std::move(second));
        state->outstanding_presenter_submissions += output_count;
        state->presenter_condition.notify_all();
        return XR_SUCCESS;
    } catch (...) {
        return XR_ERROR_OUT_OF_MEMORY;
    }
}

[[nodiscard]] XrResult publish_native_generated_pair(
    const std::shared_ptr<SessionState>& state,
    std::shared_ptr<GeneratedFrameEndInfo> synthetic,
    std::shared_ptr<GeneratedFrameEndInfo> current,
    std::shared_ptr<GeneratedFrameEndInfo> middle=nullptr) noexcept {
    if(adaptive_optical_reserve_route(*state) && state->optical_reserve_ns.load(std::memory_order_relaxed)>0) {
        const auto result=wait_for_optical_reserve_admission(state);
        if(XR_FAILED(result))return result;
    }
    return enqueue_presenter_pair(state,std::move(synthetic),std::move(current),std::move(middle));
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

// Records, for each swapchain that holds one eye of a two-view projection
// alone, which eye it is (SwapchainState::projection_eye).
void note_projection_eyes(
    const ProjectionSnapshot& snapshot,
    const ProjectionMappingResult& mappings) noexcept {
    try {
        if (mappings.reason != ProjectionMappingReason::ready) {
            return;
        }
        for (const ProjectionResourceMapping& mapping : mappings.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) {
                continue;
            }
            int eye = -1;
            if (mapping.views.size() == 1 && swapchain->create_info.arraySize == 1) {
                const ProjectionViewReference& view = mapping.views.front();
                if (view.projection_index < snapshot.layers.size() &&
                    snapshot.layers[view.projection_index].views.size() == 2 &&
                    view.view_index < 2) {
                    eye = static_cast<int>(view.view_index);
                }
            }
            const int previous =
                swapchain->projection_eye.exchange(eye, std::memory_order_relaxed);
            if (previous != eye) {
                // Release-time capture used the previous projection identity.
                // Do not reuse its guide assignment across first discovery or
                // an eye-layout change. The next release resolves the new eye;
                // this frame keeps the existing fallback/bypass behavior.
                {
                    std::scoped_lock lock(swapchain->mutex);
                    swapchain->last_released_motion_vectors.reset();
                    swapchain->fsr_eye_identity = {};
                }
                // 750: a swapchain's eye, a the swapchain, b the eye + 1 (0
                // when it holds both or none), c the eye before + 1.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_transition,
                    750,
                    handle_value(mapping.application_swapchain),
                    static_cast<std::uint64_t>(eye + 1),
                    static_cast<std::uint64_t>(previous + 1));
            }
        }
    } catch (...) {
    }
}

// Revisit only the guide join, after both eyes' releases are known. Color
// capture and its ticket remain release-bound; no EndFrame color recapture.
// Ordinary D3D12 FSR uses strict identity; D11/AFW contracts stay separate.
void refresh_release_bound_projection_guides(
    SessionState& session, const ProjectionMappingResult& mappings) noexcept {
    if (!release_bound_projection_guides(session) ||
        mappings.reason != ProjectionMappingReason::ready) return;
    std::array<std::shared_ptr<SwapchainState>, 2> chains{};
    const auto reject = [&]() noexcept {
        for (const auto& chain : chains) {
            if (!chain) continue;
            std::scoped_lock lock(chain->mutex);
            chain->last_released_motion_vectors.reset();
        }
        for (auto& release : session.eye_release_publication)
            release.store(0, std::memory_order_relaxed);
        xrfg::report_dlss_motion_vector_status(xrfg::DlssMotionVectorStatus::invalid_input);
    };
    try {
        std::scoped_lock gpu_lock(session.gpu_mutex);
        std::array<std::uint64_t, 2> releases{};
        bool ambiguous = false;
        for (const auto& mapping : mappings.mappings) {
            const auto chain = find_swapchain(mapping.application_swapchain);
            if (!chain) continue;
            const int eye = chain->projection_eye.load(std::memory_order_relaxed);
            if (eye != 0 && eye != 1) continue;
            std::scoped_lock lock(chain->mutex);
            if (!chain->last_released_capture || !chain->last_released_index ||
                chain->last_released_capture->source_index != *chain->last_released_index ||
                *chain->last_released_index >= chain->enumerated_d3d12_images.size()) continue;
            if (chains[eye] && chains[eye] != chain) {
                // Multiple candidate colors for one eye are not a stereo join.
                chain->last_released_motion_vectors.reset();
                ambiguous = true;
                continue;
            }
            chains[eye] = chain;
            releases[eye] = chain->release_publication;
        }
        if (ambiguous) { reject(); return; }
        for (std::size_t eye = 0; eye < chains.size(); ++eye) {
            session.eye_release_publication[eye].store(releases[eye], std::memory_order_relaxed);
            if (!chains[eye]) continue;
            std::scoped_lock lock(chains[eye]->mutex);
            // No watermark means no valid release association, not a request
            // to reuse the legacy newest/odd-even matching on an R15 route.
            const auto guides = releases[eye] ? xrfg::resolve_dlss_motion_vectors(
                chains[eye]->enumerated_d3d12_images[*chains[eye]->last_released_index].Get(),
                session.d3d12_queue.Get(), static_cast<int>(eye), releases[eye], releases[1 - eye],
                strict_fsr_release_guides(session)
                    ? xrfg::DlssReleasedEyeIdentity{true, chains[eye]->fsr_eye_identity.stream,
                        chains[eye]->fsr_eye_identity.epoch} : xrfg::DlssReleasedEyeIdentity{}) : nullptr;
            chains[eye]->last_released_motion_vectors = guides;
            if (strict_fsr_release_guides(session) && releases[0] && releases[1] &&
                    guides && guides->eyes[0]) {
                const auto& guide = *guides->eyes[0];
                chains[eye]->fsr_eye_identity = {true, guide.stream, guide.epoch};
                // 752: logical eye, verified stream, reset epoch. Guide
                // publication remains result751; no additional GPU telemetry.
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    752, eye, guide.stream, guide.epoch);
            }
            // 751: eye, release watermark, selected guide publication. Only
            // normal live logging; no GPU timestamps or additional worker.
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                751, eye, releases[eye], guides && guides->eyes[0] ? guides->eyes[0]->publication : 0);
        }
    } catch (...) {
        reject();
    }
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
        const std::uint32_t logical_eye_count = static_cast<std::uint32_t>(
            std::min<std::size_t>(
                2,
                std::accumulate(
                    output.mappings.begin(), output.mappings.end(),
                    std::size_t{0},
                    [](std::size_t count,
                       const ProjectionResourceMapping& mapping) {
                        return count + mapping.views.size();
                    })));
        for (const ProjectionResourceMapping& mapping : output.mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) continue;
            std::optional<std::uint32_t> logical_eye_index;
            if (logical_eye_count == 2 && mapping.views.size() == 1) {
                const ProjectionViewReference& reference = mapping.views.front();
                std::size_t ordinal = reference.view_index;
                for (std::size_t projection = 0;
                     projection < reference.projection_index;
                     ++projection) {
                    ordinal += snapshot.layers[projection].views.size();
                }
                if (ordinal < logical_eye_count) {
                    logical_eye_index = static_cast<std::uint32_t>(ordinal);
                }
            }
            std::scoped_lock lock(swapchain->mutex);
            swapchain->dlss_logical_eye_count =
                std::max<std::uint32_t>(logical_eye_count, 1U);
            swapchain->dlss_logical_eye_index = logical_eye_index;
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
        destination.display_time = snapshot.display_time;
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
    const ProjectionSnapshot& current,
    bool consecutive_captures = false) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
        (!consecutive_captures && current.display_time <= previous.display_time) ||
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

// App predicted displayTime is not a capture identity. In MSFS it can move
// backwards while the release-bound images advance. Use actual source order
// for native SteamVR captures, independently of output scheduling. Original
// display times/poses and each generator's guide/ticket checks remain intact.
// Luke's source identity and other runtimes retain their existing contract.
[[nodiscard]] bool capture_order_timeline(
    const SessionState& state) noexcept {
    return state.dispatch && comparison_delivery_runtime(state) &&
        state.graphics_binding == SessionGraphicsBinding::d3d12 && !state.luke_afw_enabled;
}

void record_projection_capture_order(
    ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    try {
        snapshot.capture_order.clear();
        snapshot.capture_order.reserve(mappings.size());
        for (const auto& mapping : mappings) {
            const auto chain = find_swapchain(mapping.application_swapchain);
            if (!chain) { snapshot.capture_order.clear(); return; }
            std::scoped_lock lock(chain->mutex);
            if (!chain->d3d12_history || !chain->last_released_capture ||
                !chain->last_released_index ||
                chain->last_released_capture->source_index != *chain->last_released_index) {
                snapshot.capture_order.clear(); return;
            }
            const auto& capture = *chain->last_released_capture;
            snapshot.capture_order.push_back({mapping.application_swapchain,
                chain->d3d12_history, capture.serial, capture.fence_value});
        }
    } catch (...) { snapshot.capture_order.clear(); }
}

[[nodiscard]] bool consecutive_projection_captures(
    const ProjectionSnapshot& previous, const ProjectionSnapshot& current) noexcept {
    if (!previous.interface_stamp.serial ||
        current.interface_stamp.serial <= previous.interface_stamp.serial ||
        previous.capture_order.empty() ||
        previous.capture_order.size() != current.capture_order.size()) return false;
    for (std::size_t i = 0; i < current.capture_order.size(); ++i) {
        const auto& before = previous.capture_order[i];
        const auto& after = current.capture_order[i];
        const auto owner = before.history.lock();
        if (!owner || owner != after.history.lock() || before.swapchain != after.swapchain ||
            !before.serial || after.serial <= before.serial || after.serial - before.serial != 1 ||
            !before.fence_value || after.fence_value <= before.fence_value) return false;
    }
    return true;
}

// A space/flag transition only invalidates the interpolation pair and can be
// re-primed on the current application frame. A layout transition changes the
// resources or regions consumed by generation and must cross the quarantine.
//
// A display time that does not advance is deliberately not tested here. It is
// not a layout change - it names no different swapchain, sub-image, view or
// blend mode - and it is not the layer's own timeline either. MSFS 2024
// derives the time it submits from the predicted time it was given, and it
// derives it non-monotonically: with the virtual clock made strictly
// increasing and verified so over 1281 consecutive waits, 287 of the 1280
// frames that application submitted still went backwards, by whole multiples
// of the display period. A pipelined application that reorders two frames in
// flight will do this, and no clock the layer hands it prevents it.
//
// Quarantining for it is also self-sustaining: the outage puts the
// application on the fail-open path, which produces the next non-advancing
// time, which starts the next outage. Captured MSFS 2024 sessions spent 3768
// of 3895 and 1176 of 1280 frames in structural quarantine that way,
// re-entering it faster than the one-second duration expires. Clearing
// continuity and priming again remains the fallback when consecutive capture
// identities cannot prove source order. A native SteamVR capture pair can
// retain the pair when every released resource proves that order (see above).
[[nodiscard]] bool projection_resource_layout_compatible(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current) noexcept {
    if (previous.environment_blend_mode != current.environment_blend_mode ||
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
    bool synthetic_camera{};
};

// A synthetic's own camera. A synthetic is shown between the two real frames
// it is made from - after the newer one when extrapolating - and the runtime
// reprojects every submitted image from the pose it was submitted with to the
// head's pose when it is shown. Submitted with the newer frame's pose, a
// synthetic shown a display period before that frame is turned back by the
// head's motion over the period, and the edge it turns away from has no
// pixels: under a head turn, black strips that flicker at the game's frame
// rate (at 3X, 144 Hz and 120 degrees a second, 1.7 and 0.8 degrees of view
// for the two synthetics). Here each view's pose is instead the one `fraction`
// of the way from the previous frame's to the current one's - the share of
// the span the synthetic's content is placed at, so the head's pose when it is
// shown - with the current frame's field of view. The synthesizer generates
// the synthetic in that camera and it is submitted with it.
//
// A turn or a move between the two frames that no head makes within a frame
// - a recentre, a teleport - keeps the current frame's pose for every view:
// what lies between two such poses is not a camera anyone looked from.
constexpr float kMaxSyntheticCameraTurnRadians = 0.7853982F;  // 45 degrees
constexpr float kMaxSyntheticCameraMoveMeters = 0.5F;

[[nodiscard]] xrfg::Pose to_pose(const XrPosef& pose) noexcept {
    return {
        {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
        {pose.position.x, pose.position.y, pose.position.z},
    };
}

[[nodiscard]] std::optional<ProjectionSnapshot> synthetic_camera_snapshot(
    const ProjectionSnapshot& previous,
    const ProjectionSnapshot& current,
    float fraction) noexcept {
    try {
        if (previous.layers.size() != current.layers.size()) {
            return std::nullopt;
        }
        ProjectionSnapshot camera = current;
        for (std::size_t layer_index = 0; layer_index < camera.layers.size(); ++layer_index) {
            auto& views = camera.layers[layer_index].views;
            const auto& previous_views = previous.layers[layer_index].views;
            if (previous_views.size() != views.size()) {
                return std::nullopt;
            }
            for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
                const xrfg::Pose from = to_pose(previous_views[view_index].pose);
                const xrfg::Pose to = to_pose(views[view_index].pose);
                const float move_x = to.position.x - from.position.x;
                const float move_y = to.position.y - from.position.y;
                const float move_z = to.position.z - from.position.z;
                const float turn = xrfg::rotation_angle(from.orientation, to.orientation);
                const float move = std::sqrt(move_x * move_x + move_y * move_y + move_z * move_z);
                if (!(turn <= kMaxSyntheticCameraTurnRadians) ||
                    !(move <= kMaxSyntheticCameraMoveMeters)) {
                    return std::nullopt;
                }
                const xrfg::Pose shown = xrfg::pose_at_fraction(from, to, fraction);
                views[view_index].pose = {
                    {shown.orientation.x, shown.orientation.y, shown.orientation.z,
                     shown.orientation.w},
                    {shown.position.x, shown.position.y, shown.position.z},
                };
            }
        }
        return camera;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool build_generated_frame_end_info(
    const XrFrameEndInfo* source,
    const ProjectionSnapshot& current_snapshot,
    bool synthetic,
    std::span<const ProjectionResourceDestination> destinations,
    GeneratedFrameEndInfo* output,
    const ProjectionSnapshot* camera = nullptr) {
    if (source == nullptr || output == nullptr || destinations.empty() ||
        source->layerCount == 0 || source->layers == nullptr ||
        current_snapshot.layers.empty()) {
        return false;
    }

    output->blackbar_pose=current_snapshot.blackbar_pose;
    output->app_release_value = current_snapshot.app_release_value;
    output->input_release_value = current_snapshot.app_release_value;
    output->interface_stamp = current_snapshot.interface_stamp;
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
                if (camera && destination->synthetic_camera &&
                    projection_index < camera->layers.size() &&
                    view_index < camera->layers[projection_index].views.size())
                    view.pose = camera->layers[projection_index].views[view_index].pose;
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
    SessionState* session,
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
            !release_private_image(session, dispatch, image)) {
            return false;
        }

        XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        std::uint32_t acquired_index = 0;
        const auto acquire_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::private_swapchain_acquire,
            handle_value(image.handle),
            static_cast<std::uint64_t>(image.phase));
        const XrResult acquire_result = with_runtime_entry(session, [&] {
            return dispatch->acquire_swapchain_image(
                image.handle, &acquire_info, &acquired_index);
        });
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
        const XrResult wait_result = with_runtime_entry(session, [&] {
            return dispatch->wait_swapchain_image(image.handle, &wait_info);
        });
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
        if (!image.first_use_logged && session != nullptr) {
            image.first_use_logged = true;
            log_video_memory(*session, VideoMemoryStage::private_first_use, handle_value(image.handle));
        }
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
    presenter_route_changed = 23,
    synthesis_stale_guides = 24,
    fsr_stereo_unavailable = 25,
    fsr_stereo_recovering = 26,
    native_stereo_unavailable = 27,
    afw_input_unavailable = 28,
    luke_flat_scene = 29,
};

// A refused native projection/history is local to AFW. It says nothing about
// the already captured XR colours used by ordinary optical-flow OFXR. Never
// turn device removal, failed publication or GPU backpressure into this case.
[[nodiscard]] constexpr bool afw_input_unavailable(HRESULT result) noexcept {
    return result == DXGI_ERROR_UNSUPPORTED ||
        result == HRESULT_FROM_WIN32(ERROR_NOT_READY);
}
static_assert(afw_input_unavailable(DXGI_ERROR_UNSUPPORTED));
static_assert(afw_input_unavailable(HRESULT_FROM_WIN32(ERROR_NOT_READY)));
static_assert(!afw_input_unavailable(DXGI_ERROR_DEVICE_REMOVED));
static_assert(!afw_input_unavailable(DXGI_ERROR_WAS_STILL_DRAWING));

[[nodiscard]] constexpr GenerationPrepareReason classify_synthesis_failure(
    HRESULT result) noexcept {
    if (result == xrfg::kD3D12FrameSynthesisStaleGuides) {
        return GenerationPrepareReason::synthesis_stale_guides;
    }
    return result == HRESULT_FROM_WIN32(ERROR_BUSY) ||
            result == DXGI_ERROR_WAS_STILL_DRAWING
        ? GenerationPrepareReason::synthesis_busy
        : GenerationPrepareReason::synthesis_failed;
}

static_assert(classify_synthesis_failure(DXGI_ERROR_WAS_STILL_DRAWING) ==
    GenerationPrepareReason::synthesis_busy);
static_assert(classify_synthesis_failure(
                  xrfg::kD3D12FrameSynthesisStaleGuides) ==
    GenerationPrepareReason::synthesis_stale_guides);

[[nodiscard]] constexpr bool should_preserve_generation_anchor(
    HRESULT result,
    bool current_released,
    bool synthetic_released) noexcept {
    return result == xrfg::kD3D12FrameSynthesisStaleGuides &&
        current_released && synthetic_released;
}

static_assert(should_preserve_generation_anchor(
    xrfg::kD3D12FrameSynthesisStaleGuides, true, true));
static_assert(!should_preserve_generation_anchor(
    DXGI_ERROR_WAS_STILL_DRAWING, true, true));
static_assert(!should_preserve_generation_anchor(
    xrfg::kD3D12FrameSynthesisStaleGuides, false, true));

[[nodiscard]] constexpr bool should_preserve_missing_capture_anchor(
    bool request_pair,
    xrfg::D3D12FrameGenerationMode mode,
    bool luke_combined_bridge) noexcept {
    return request_pair &&
        mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        luke_combined_bridge;
}

static_assert(should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::fsr3, true));
static_assert(!should_preserve_missing_capture_anchor(
    false, xrfg::D3D12FrameGenerationMode::fsr3, true));
static_assert(!should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::ofxr, true));
static_assert(!should_preserve_missing_capture_anchor(
    true, xrfg::D3D12FrameGenerationMode::fsr3, false));

struct PreparedGeneration {
    std::shared_ptr<FrameGenerationSwapchainState> staging_generation;
    std::shared_ptr<DirectSyntheticLease> direct_synthetic_lease, direct_extra_lease;
    xrfg::D3D12FrameSynthesisTicket staging_ticket{};
    Microsoft::WRL::ComPtr<ID3D12Fence> reference_fence;
    std::uint64_t reference_synthetic_value{}, reference_current_value{};
    bool reference_requires_application_release{};
    xrfg::D3D12SynthesisInputs reference_inputs;
    std::shared_ptr<DirectOutputPublication> direct_current_publication, direct_synthetic_publication;
    std::shared_ptr<xrfg::D3D12NativeFrameResult> native_result;
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain current_handle{XR_NULL_HANDLE};
    XrSwapchain synthetic_handle{XR_NULL_HANDLE};
    XrSwapchain extra_handle{XR_NULL_HANDLE};
    // The synthesizer holding this pair's deferred current copy, so the
    // presenter can submit it once the synthetic frame has gone.
    std::shared_ptr<xrfg::D3D12FrameSynthesizer> synthesizer;
    bool anchor_is_current{};
    std::shared_ptr<FrameGenerationSwapchainState> worker_generation;
    bool late_luke_publication{};
    xrfg::D3D11PublicationReceipt worker_publication;
    Microsoft::WRL::ComPtr<ID3D12Fence> worker_fence;
    std::uint64_t worker_fence_value{};
    std::uint32_t current_destination{};
    std::uint32_t synthetic_destination{};
    std::size_t current_slot{};
    std::size_t synthetic_slot{};
    bool synthetics_in_target_camera{};
    bool preserve_previous{};
    bool publication_deferred{};
    std::uint32_t current_destination_index{
        std::numeric_limits<std::uint32_t>::max()};
};

struct PreparedProjectionResource {
    XrSwapchain application_swapchain{XR_NULL_HANDLE};
    PreparedGeneration generation;
};

struct LukeAfwStartupCandidate {
    xrfg::LukeAfwStartupKey key;
    std::uint64_t publication{};
};

struct PreparedProjectionFrame {
    PreparedGenerationKind kind{PreparedGenerationKind::none};
    GenerationPrepareReason reason{GenerationPrepareReason::exception};
    XrSwapchain failed_swapchain{XR_NULL_HANDLE};
    std::vector<PreparedProjectionResource> resources;
    bool anchor_is_current{};
    bool preserve_previous{};
    std::shared_ptr<const HeldAfwPair> held_afw_pair;
    std::optional<LukeAfwStartupCandidate> startup_candidate;
    std::uint64_t scene_serial{};
    xrfg::SceneIdentity original_scene;
};

void attach_djules_outputs(PreparedProjectionFrame& prepared,
    GeneratedFrameEndInfo& real, GeneratedFrameEndInfo* synthetic=nullptr,
    GeneratedFrameEndInfo* extra=nullptr) {
    for(auto& resource:prepared.resources) {
        auto& output=resource.generation;
        if(output.native_result) {
            real.native_results.push_back(output.native_result);
            if(synthetic) synthetic->native_results.push_back(output.native_result);
            if(extra) { extra->native_results.push_back(output.native_result); extra->native_output=1; }
        }
        if (output.reference_fence) {
            real.reference_readiness.push_back({output.reference_fence, output.reference_current_value, output.reference_requires_application_release,output.reference_inputs});
            if (synthetic) synthetic->reference_readiness.push_back({output.reference_fence, output.reference_synthetic_value, output.reference_requires_application_release,output.reference_inputs});
            if (extra) extra->reference_readiness.push_back({output.reference_fence, output.reference_synthetic_value, output.reference_requires_application_release,output.reference_inputs});
        }
        if(output.direct_current_publication) real.direct_publications.push_back(output.direct_current_publication);
        if(synthetic && output.direct_synthetic_publication) synthetic->direct_publications.push_back(output.direct_synthetic_publication);
        if(synthetic && output.direct_synthetic_lease)
            synthetic->direct_synthetic_leases.push_back(output.direct_synthetic_lease);
        if(extra && output.direct_extra_lease)
            extra->direct_synthetic_leases.push_back(output.direct_extra_lease);
        if(!output.staging_generation) continue;
        auto generation=output.staging_generation;
        real.pending_outputs.push_back({generation,generation->current_staging[output.current_slot],
            output.synthesizer,output.staging_ticket,false,
            static_cast<std::uint32_t>(output.current_slot % generation->current_runtime_rings)});
        if(synthetic) synthetic->pending_outputs.push_back({generation,generation->synthetic_staging[output.synthetic_slot*generation->staging_stride],
            output.synthesizer,output.staging_ticket,true,
            static_cast<std::uint32_t>(output.synthetic_slot % generation->synthetic_runtime_rings)});
        if(extra) extra->pending_outputs.push_back({generation,generation->synthetic_staging[output.synthetic_slot*generation->staging_stride+1],
            output.synthesizer,output.staging_ticket,true,
            static_cast<std::uint32_t>((output.synthetic_slot + (generation->synthetic_runtime_rings==4 ? 2 : 1)) % generation->synthetic_runtime_rings)});
    }
}

void log_luke_publication_boundary(
    std::uint64_t serial, std::int64_t boundary, std::size_t resource,
    XrSwapchain application, XrSwapchain current, XrSwapchain synthetic,
    const std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop>& interop) noexcept {
    if (!serial || !interop || !xrfg::bridge_flight_logger().ram_enabled()) return;
    const auto snapshot = interop->publication_snapshot();
    const auto code = boundary * 100 + static_cast<std::int64_t>(resource) * 10;
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_publication_images, code, serial,
        handle_value(application),
        (static_cast<std::uint64_t>(snapshot.current_index) << 32) | snapshot.synthetic_index);
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_publication_slots, code, serial,
        handle_value(current), handle_value(synthetic));
    const std::uint64_t values[]{snapshot.ready_value, snapshot.copy_complete_value,
                                 snapshot.completed_value};
    for (int field = 0; field < 3; ++field)
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_publication_fence, code + field,
            serial, snapshot.fence_identity, values[field]);
}

void log_luke_physical_boundary(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* frame,
    std::uint64_t serial, std::int64_t boundary, const PreparedProjectionFrame* prepared,
    XrResult result = XR_SUCCESS) noexcept {
    if (!state || !frame || !serial || (state->graphics_binding_capabilities & 8ULL) == 0 ||
        !xrfg::bridge_flight_logger().ram_enabled()) return;
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::luke_physical_submit, boundary * 100, serial,
        static_cast<std::uint64_t>(frame->displayTime),
        (static_cast<std::uint64_t>(frame->layerCount) << 32) | static_cast<std::uint32_t>(result));
    // Boundaries 4/6 are immediately before first/second physical xrEndFrame,
    // 5/7 immediately after. No additional locate or wait is performed here.
    if (boundary == 4 || boundary == 6) {
        LARGE_INTEGER counter{};
        const auto converted = state->dispatch->convert_time_to_qpc
            ? state->dispatch->convert_time_to_qpc(state->dispatch->instance, frame->displayTime, &counter)
            : XR_ERROR_FUNCTION_UNSUPPORTED;
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_inline_clock, converted, serial,
            static_cast<std::uint64_t>(frame->displayTime),
            XR_SUCCEEDED(converted) ? static_cast<std::uint64_t>(counter.QuadPart) : 0);
        log_luke_projection_poses(frame, serial, boundary);
        if (frame->layers) {
            for (std::uint32_t layer = 0; layer < std::min(frame->layerCount, 8u); ++layer) {
                const auto* header = frame->layers[layer];
                if (!header || header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
                const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if (!projection->views) continue;
                for (std::uint32_t view = 0; view < std::min(projection->viewCount, 8u); ++view) {
                    const auto code = boundary * 100 + layer * 10 + view;
                    xrfg::bridge_flight_logger().event(
                        xrfg::BridgeFlightOperation::luke_inline_view, code, serial,
                        handle_value(projection->space), handle_value(projection->views[view].subImage.swapchain));
                    log_luke_view_bits(serial, code, projection->views[view]);
                }
            }
        }
    }
    if (prepared) {
        for (std::size_t index = 0; index < prepared->resources.size(); ++index) {
            const auto& resource = prepared->resources[index];
            const auto swapchain = find_swapchain(resource.application_swapchain);
            if (!swapchain) continue;
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
            }
            if (generation)
                log_luke_publication_boundary(serial, boundary, index,
                    resource.application_swapchain, resource.generation.current_handle,
                    resource.generation.synthetic_handle, generation->d3d11_interop);
        }
    }
}

struct AfwSourceCapture {
    std::shared_ptr<FrameGenerationSwapchainState> generation;
    xrfg::D3D12HistoryCaptureTicket capture;
};

template<class Submit>
HRESULT submit_luke_worker_generation(const std::shared_ptr<SessionState>& session,
    const std::shared_ptr<FrameGenerationSwapchainState>& generation,
    const std::shared_ptr<xrfg::D3D12FrameSynthesizer>& synthesizer,
    bool worker,Submit&& submit,std::unique_lock<std::mutex>* call_lock=nullptr) {
    HRESULT result;
    {std::scoped_lock lock(session->gpu_mutex);result=submit();}
    const bool inline_fidelity = !worker &&
        session->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    const bool inline_luke = inline_fidelity&&session->luke_afw_enabled&&!session->djules_route&&
        generation->d3d11_interop&&session->frame_generation_mode==xrfg::D3D12FrameGenerationMode::ofxr;
    if((worker || inline_luke)&&
        classify_synthesis_failure(result)==GenerationPrepareReason::synthesis_busy) {
        // All worker routes can re-prime while the prior copy is still on
        // the GPU, including Legacy VDXR Djules without D3D11/AFW. Backpressure
        // on one eye is not a failed stereo transaction. Wait
        // the synthesizer's existing completion before reusing its resources;
        // keep the same capture, acquired destinations and camera packet.
        // Neither the session GPU mutex nor a swapchain call lock is held.
        if(call_lock)call_lock->unlock();
        const auto ready=synthesizer->wait_for_previous_submission(
            kFrameStartSynthesisWaitMilliseconds);
        if(call_lock)call_lock->lock();
        if(FAILED(ready))return ready;
        std::scoped_lock lock(session->gpu_mutex);result=submit();
    }
    return result;
}

[[nodiscard]] bool defer_native_direct_publication(const SessionState& state) noexcept {
    return comparison_delivery_runtime(state) && state.graphics_binding==SessionGraphicsBinding::d3d12 &&
        !state.d3d11_bridge && !state.vulkan_bridge && !state.luke_afw_enabled && !state.luke_outlaws_preframe &&
        (state.queued_nvidia_presenter || state.fsr_two_slot_presenter) &&
        (state.nvidia_new_route || state.frame_generation_mode!=xrfg::D3D12FrameGenerationMode::ofxr) &&
        !djules_deep_active(state) && !djules_triple_active(state);
}

[[nodiscard]] PreparedGeneration prepare_frame_generation(
    XrSwapchain application_swapchain,
    bool request_pair,
    std::span<const xrfg::D3D12ReprojectionView> current_source_views,
    float interpolation_fraction,
    bool worker_submission = false,
    const xrfg::D3D12PresentationTransform* captured_transform = nullptr,
    bool defer_d3d11_publication = false,
    const PostAfwFrameInput* post_afw_input = nullptr,
    const AfwSourceCapture* afw_source = nullptr,
    float extra_fraction=2.0F/3.0F,
    std::span<const xrfg::D3D12ReprojectionView> synthetic_views = {},
    std::span<const xrfg::D3D12ReprojectionView> extra_views = {}) noexcept {
    PreparedGeneration output{};
    try {
        // AFW's frozen source is transport, never a temporal OFXR prime.
        // Reject incompatible callers rather than letting them silently feed
        // this color into a backend-specific synthesis path.
        if (afw_source && (request_pair || worker_submission ||
                !defer_d3d11_publication || post_afw_input)) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        const auto state = find_swapchain(application_swapchain);
        if (!state) {
            output.reason = GenerationPrepareReason::unknown_swapchain;
            return output;
        }

        std::unique_lock call_lock(state->call_mutex);
        const auto presentation_transform = captured_transform ? *captured_transform
            : state->session->presentation_transform;
        if (!worker_submission && ((!state->presentation_transform_known && presentation_transform.active) ||
            (state->presentation_transform_known && state->presentation_transform_active != presentation_transform.active)))
            request_pair = false;
        state->presentation_transform_known = true;
        state->presentation_transform_active = presentation_transform.active;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
        std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_vectors;
        {
            std::scoped_lock lock(state->mutex);
            generation = state->frame_generation;
            if (afw_source) {
                // The producer selected its color BEFORE private-image/GPU
                // waits, alongside the immutable guide. Never reselect latest
                // release here after those waits have changed the phase.
                if (generation != afw_source->generation || post_afw_input) {
                    output.reason = GenerationPrepareReason::missing_generation;
                    return output;
                }
                capture = afw_source->capture;
            } else {
                capture = state->last_released_capture;
                motion_vectors = state->last_released_motion_vectors;
            }
        }
        const auto synthesizer = afw_source && generation && generation->afw_source_synthesizer
            ? generation->afw_source_synthesizer
            : post_afw_input && generation && generation == post_afw_input->generation
            ? generation->afw_temporal_synthesizer
            : generation ? generation->synthesizer : nullptr;
        if (post_afw_input) {
            if (generation != post_afw_input->generation || !synthesizer) {
                output.reason = GenerationPrepareReason::missing_generation;
                return output;
            }
            capture = post_afw_input->capture;
            motion_vectors.reset(); // post-AFW OFA consumes owned final color, not AER guides
        }
        if (!generation || !synthesizer) {
            output.reason = GenerationPrepareReason::missing_generation;
            return output;
        }
        if (!capture) {
            output.reason = GenerationPrepareReason::missing_capture;
            // Luke's D3D11-to-D3D12 bridge can present an application frame
            // between two actual post-ReShade captures.  That frame has
            // nothing new to synthesize, but it must not discard the last
            // valid FSR projection anchor: the following fresh capture and
            // fresh guide pair still belong to that continuity chain.  Do not
            // extend this contract to NVIDIA or non-Luke sessions, and do not
            // cache or reuse guide resources here.
            output.preserve_previous = should_preserve_missing_capture_anchor(
                request_pair,
                state->session->frame_generation_mode,
                (state->session->graphics_binding_capabilities & 8ULL) != 0
            );
            return output;
        }
        // AFW scratch is disjoint from both ordinary director output handles.
        // V577 retires the former A-only Luke delayed-publication experiment.
        const bool late_luke_publication = false;
        const std::size_t current_slot = defer_d3d11_publication
            ? kAfwScratchSlot : generation->current_slot;
        const std::size_t synthetic_slot = defer_d3d11_publication
            ? 0 : generation->synthetic_slot;
        const auto current_runtime_slot = defer_d3d11_publication ? current_slot : current_slot % generation->current_runtime_rings;
        const auto synthetic_runtime_slot = synthetic_slot % generation->synthetic_runtime_rings;
        const bool late_direct_publication=!worker_submission && !generation->staged_outputs &&
            defer_native_direct_publication(*state->session);
        PrivateSwapchainState& current_image = generation->current[current_runtime_slot];
        PrivateSwapchainState& synthetic_image = generation->synthetic[synthetic_runtime_slot];
        if (current_image.handle == XR_NULL_HANDLE ||
            synthetic_image.handle == XR_NULL_HANDLE ||
            generation->current_images_per_slot == 0 ||
            generation->synthetic_images_per_slot == 0) {
            output.reason = GenerationPrepareReason::invalid_private_swapchain;
            return output;
        }

        const bool preserve_ofxr_anchor = afw_source && luke_color_only_ofxr(*state->session);
        if (!request_pair && !preserve_ofxr_anchor) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            const HRESULT retire_result = synthesizer->retire_previous();
            if (FAILED(retire_result)) {
                output.reason = GenerationPrepareReason::retire_previous_failed;
                return output;
            }
        }

        const bool request_extra=request_pair && djules_triple_active(*state->session) &&
            (generation->staged_outputs || generation->direct_extra_outputs);
        // A private SteamVR route handoff joins the old presenter before inline
        // priming can promote its replacement. Its latched stop request belongs
        // to that retired owner, not to this new output transaction. Live owners
        // still cancel their lease waits immediately; other runtimes are unchanged.
        const auto publication_stopping=[&] {
            return state->session->presenter_stop_requested &&
                (state->session->presenter_active || !state->session->private_runtime ||
                 !comparison_delivery_runtime(*state->session));
        };
        auto& extra_image = generation->synthetic[(synthetic_slot + (generation->synthetic_runtime_rings==4 ? 2 : 1)) % generation->synthetic_runtime_rings];
        if(request_extra && !generation->staged_outputs) {
            std::unique_lock lock(state->session->presenter_mutex);
            state->session->presenter_condition.wait(lock,[&] {
                return (generation->direct_synthetic_leases[0].use_count()==1 &&
                    generation->direct_synthetic_leases[1].use_count()==1) ||
                    publication_stopping() || XR_FAILED(state->session->presenter_failure);
            });
            if(publication_stopping() || XR_FAILED(state->session->presenter_failure)) {
                output.reason=GenerationPrepareReason::synthesis_busy;return output;
            }
            output.direct_synthetic_lease=generation->direct_synthetic_leases[synthetic_runtime_slot];
            output.direct_extra_lease=generation->direct_synthetic_leases[(synthetic_runtime_slot+1)%2];
        }
        if (generation->staged_outputs) {
            std::unique_lock lock(state->session->presenter_mutex);
            state->session->presenter_condition.wait(lock,[&] {
                return (generation->current_staging[current_slot].use_count()==1 &&
                    (!request_pair || generation->synthetic_staging[synthetic_slot*generation->staging_stride].use_count()==1) &&
                    (!request_extra || generation->synthetic_staging[synthetic_slot*generation->staging_stride+1].use_count()==1)) ||
                    (state->session->presenter_active &&
                     (state->session->presenter_stop_requested || XR_FAILED(state->session->presenter_failure)));
            });
            if(state->session->presenter_active &&
               (state->session->presenter_stop_requested || XR_FAILED(state->session->presenter_failure))) {
                output.reason=GenerationPrepareReason::synthesis_busy; return output;
            }
            lock.unlock();
            auto* queue=state->session->d3d12_synthesis_queue ? state->session->d3d12_synthesis_queue.Get()
                : state->session->d3d12_queue.Get();
            for(const auto& stage:{generation->current_staging[current_slot],
                    request_pair ? generation->synthetic_staging[synthetic_slot*generation->staging_stride] : nullptr,
                    request_extra ? generation->synthetic_staging[synthetic_slot*generation->staging_stride+1] : nullptr}) {
                if(stage && FAILED(queue->Wait(generation->handover_fence.Get(),
                        stage->last_handover_copy.load(std::memory_order_acquire)))) {
                    output.reason=GenerationPrepareReason::synthesis_failed; return output;
                }
            }
        }
        if(late_direct_publication) {
            std::unique_lock lock(state->session->presenter_mutex);
            auto& current_busy=generation->current_publication_busy[current_runtime_slot];
            auto& synthetic_busy=generation->synthetic_publication_busy[synthetic_runtime_slot];
            state->session->presenter_condition.wait(lock,[&] {
                return (!current_busy.load(std::memory_order_acquire) &&
                    (!request_pair || !synthetic_busy.load(std::memory_order_acquire))) ||
                    publication_stopping() || XR_FAILED(state->session->presenter_failure);
            });
            if(publication_stopping() || XR_FAILED(state->session->presenter_failure)) {
                output.reason=GenerationPrepareReason::synthesis_busy;return output;
            }
            auto claim=[&](PrivateSwapchainState& image,std::atomic_bool& busy,bool synthetic) {
                auto publication=std::make_shared<DirectOutputPublication>();
                publication->session=state->session;publication->generation=generation;
                publication->synthesizer=synthesizer;publication->image=&image;
                publication->busy=&busy;publication->synthetic=synthetic;
                busy.store(true,std::memory_order_release);return publication;
            };
            output.direct_current_publication=claim(current_image,current_busy,false);
            if(request_pair) output.direct_synthetic_publication=claim(synthetic_image,synthetic_busy,true);
        }
        if (!generation->staged_outputs && !late_luke_publication && !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image)) {
            output.reason = GenerationPrepareReason::current_private_acquire_failed;
            if (request_pair) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(synthesizer->retire_previous());
            }
            return output;
        }

        if (request_pair && !generation->staged_outputs && !late_luke_publication &&
            !acquire_and_wait_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image)) {
            output.reason =
                GenerationPrepareReason::synthetic_private_acquire_failed;
            static_cast<void>(release_private_image(
                state->session.get(),
                state->session->dispatch,
                current_image));
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(synthesizer->retire_previous());
            return output;
        }

        // Both private images are acquired. The runtime made them safe to
        // write on the queue the application supplied, which is not the one
        // about to write them, so carry that guarantee across before any
        // synthesis is queued.
        if (request_extra && !generation->staged_outputs && !acquire_and_wait_private_image(
                state->session.get(), state->session->dispatch, extra_image)) {
            static_cast<void>(release_private_image(state->session.get(), state->session->dispatch, synthetic_image));
            static_cast<void>(release_private_image(state->session.get(), state->session->dispatch, current_image));
            { std::scoped_lock gpu_lock(state->session->gpu_mutex); static_cast<void>(synthesizer->retire_previous()); }
            output.reason = GenerationPrepareReason::synthetic_private_acquire_failed;
            return output;
        }

        if (!generation->staged_outputs && state->session->d3d12_synthesis_queue) {
            std::scoped_lock gpu_lock(state->session->gpu_mutex);
            static_cast<void>(
                synthesizer->synchronize_producer_queue(
                    runtime_queue(*state->session)));
        }

        const std::uint32_t current_destination_index =
            static_cast<std::uint32_t>(generation->staged_outputs ? current_slot : current_runtime_slot) *
                generation->current_images_per_slot +
            (generation->staged_outputs || late_luke_publication ? 0 : current_image.acquired_index);
        const std::uint32_t synthetic_destination_index =
            static_cast<std::uint32_t>(generation->staged_outputs ? synthetic_slot : synthetic_runtime_slot) *
                generation->synthetic_images_per_slot +
            (generation->staged_outputs || late_luke_publication ? 0 : synthetic_image.acquired_index);

        xrfg::D3D12FrameSynthesisTicket ticket{};
        const auto debug_marker = request_pair && state->session->fsr_cadence.marker_enabled() &&
                state->session->fps_overlay
            ? state->session->fps_overlay->marker_placement()
            : std::nullopt;
        HRESULT submit_result = E_UNEXPECTED;
        xrfg::D3D11PublicationReceipt inline_publication;
        const xrfg::BridgeFlightOperation synthesis_operation = request_pair
            ? xrfg::BridgeFlightOperation::synthesis_pair
            : xrfg::BridgeFlightOperation::synthesis_prime;
        const auto synthesis_token = xrfg::bridge_flight_logger().begin(
            synthesis_operation,
            handle_value(application_swapchain),
            capture->serial,
            (static_cast<std::uint64_t>(
                 synthetic_destination_index) << 32) |
                current_destination_index);
        if (state->session->frame_capture && state->session->frame_capture->active() && state->d3d12_history) {
            try {
            state->session->frame_capture->record("{\"kind\":\"consume\",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+
                ",\"swapchain\":\""+std::to_string(handle_value(application_swapchain))+"\",\"serial\":"+std::to_string(capture->serial)+
                ",\"history_fence_required\":"+std::to_string(capture->fence_value)+",\"history_fence_observed\":"+
                std::to_string(state->d3d12_history->diagnostic_completed_value())+",\"gpu_wait_required_by_pipeline\":true}");
            } catch (...) {}
        }
        submit_result=submit_luke_worker_generation(state->session,generation,synthesizer,worker_submission,[&] {
            log_completed_nvidia_gpu_timings(synthesizer);
            if (generation->d3d11_interop) {
                // The frozen post-AFW history ticket carries its own producer
                // fence. Only these output slots need D11 reuse protection;
                // the mutable publication cursor may already name disjoint
                // AFW scratch from the NEXT frame, particularly for eye two.
                submit_result = post_afw_input && worker_submission &&
                        generation->isolated_luke_synthesis
                    ? generation->d3d11_interop->prepare_synthesis_targets(
                        current_destination_index, request_pair
                            ? std::optional<std::uint32_t>{synthetic_destination_index}
                            : std::nullopt)
                    : generation->d3d11_interop->prepare_synthesis(
                        generation->isolated_luke_synthesis && !afw_source);
            }
            // Deferring the current copy keeps a full-resolution copy the
            // synthetic never reads off its critical path, but it only works
            // where the copy's destination is read after flush_current_copy.
            // The interop's publish below is read before it: it moves both
            // results back across to the application's D3D11 images while the
            // copy is still an unsubmitted command list, so it publishes the
            // previous pair's B as this pair's current frame. The headset then
            // runs forward to the midpoint and back a whole pair, every pair,
            // which reads as doubling that scales with motion and disappears
            // wherever the scene is still.
            // The legacy queue can hand S to the runtime while the producer
            // advances. Submit B now as well, so its queue dependency precedes
            // that next application frame rather than being inserted after S.
            const bool defer_current_copy = !generation->staged_outputs && !worker_submission &&
                generation->d3d11_interop == nullptr &&
                !state->session->queued_nvidia_presenter;
            if (!generation->d3d11_interop || SUCCEEDED(submit_result)) {
                if (afw_source) {
                    // All Luke games/modes consume the exact immutable input
                    // color here. Backend warm-up belongs only to real OFXR
                    // priming, not this per-frame AFW scratch-image copy.
                    submit_result = synthesizer->submit_copy(
                        *capture, current_source_views,
                        current_destination_index, &ticket, preserve_ofxr_anchor);
                } else if (request_pair) {
                    submit_result = synthesizer->submit_pair(
                                          *capture,
                                          current_source_views,
                                          synthetic_views.empty() ? current_source_views : synthetic_views,
                                          synthetic_destination_index,
                                          current_destination_index,
                                          &ticket,
                                          debug_marker,
                                          motion_vectors,
                                          defer_current_copy,
                                          interpolation_fraction, presentation_transform,
                                          request_extra ? std::optional<xrfg::D3D12ExtraSynthetic>{
                                              xrfg::D3D12ExtraSynthetic{generation->staged_outputs ? synthetic_destination_index+1 :
                                                  static_cast<std::uint32_t>((synthetic_runtime_slot+1)%generation->synthetic_runtime_rings) * generation->synthetic_images_per_slot + extra_image.acquired_index,
                                                  extra_fraction, extra_views}} : std::nullopt,
                                          !synthetic_views.empty());
                } else {
                    submit_result = synthesizer->submit_prime(
                                          *capture,
                                          current_source_views,
                                          current_destination_index,
                                          &ticket,
                                          motion_vectors, presentation_transform);
                }
                if (request_pair && post_afw_input) {
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        submit_result, 101, post_afw_input->color_identity.application_serial,
                        ticket.current_serial);
                }
            }
            if (SUCCEEDED(submit_result) && generation->d3d11_interop &&
                !worker_submission && !defer_d3d11_publication && !late_luke_publication) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                const HRESULT publish_result =
                    generation->d3d11_interop->publish(
                        current_destination_index,
                        request_pair
                            ? std::optional<std::uint32_t>(
                                  synthetic_destination_index)
                            : std::nullopt,
                        post_afw_input ? &inline_publication : nullptr, {},
                        generation->isolated_luke_synthesis && !afw_source);
                xrfg::bridge_flight_logger().end(
                    publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    publish_result,
                    handle_value(application_swapchain),
                    current_destination_index,
                    request_pair ? synthetic_destination_index
                                 : std::numeric_limits<std::uint32_t>::max());
                if (FAILED(publish_result)) {
                    static_cast<void>(synthesizer->retire_previous());
                    submit_result = publish_result;
                }
            }
            return submit_result;
        },&call_lock);
        xrfg::bridge_flight_logger().end(
            synthesis_token,
            synthesis_operation,
            submit_result,
            ticket.previous_serial,
            ticket.current_serial,
            ticket.fence_value);

        if (SUCCEEDED(submit_result) && late_luke_publication && !worker_submission) {
            // OFA has already been dispatched into the existing private mirror.
            // The runtime may wait here while that GPU work is progressing.
            const bool acquired = acquire_and_wait_private_image(state->session.get(), state->session->dispatch, current_image) &&
                (!request_pair || acquire_and_wait_private_image(state->session.get(), state->session->dispatch, synthetic_image));
            auto fence = synthesizer->submission_fence(ticket);
            submit_result = acquired && fence ? wait_composition_fence(fence.Get(), ticket.fence_value,
                kFrameStartSynthesisWaitMilliseconds) : E_FAIL;
            if (SUCCEEDED(submit_result)) {
                const xrfg::D3D11PublicationTargets targets{
                    static_cast<std::uint32_t>(current_runtime_slot) * generation->current_images_per_slot + current_image.acquired_index,
                    request_pair ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(synthetic_runtime_slot) *
                        generation->synthetic_images_per_slot + synthetic_image.acquired_index} : std::nullopt};
                submit_result = generation->d3d11_interop->publish_completed(current_destination_index,
                    request_pair ? std::optional<std::uint32_t>{synthetic_destination_index} : std::nullopt,
                    fence.Get(), ticket.fence_value, &inline_publication, targets);
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                submit_result, 1005, current_destination_index, current_image.acquired_index);
        }

        // A successful publish only queues D3D12->D3D11 copies. The legacy
        // post-AFW consumer must not release S/B to XR until THIS receipt is
        // complete, just as the director's worker does. Wait outside gpu_mutex
        // and the interop mutex; never follow the mutable next-frame cursor.
        if (SUCCEEDED(submit_result) && inline_publication.fence) {
            const auto publication_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_publication_complete,
                post_afw_input ? post_afw_input->color_identity.application_serial : 0,
                handle_value(application_swapchain),
                inline_publication.copy_complete_value);
            const HRESULT published = xrfg::wait_for_d3d11_publication(inline_publication);
            xrfg::bridge_flight_logger().end(publication_token,
                xrfg::BridgeFlightOperation::timing_publication_complete, published,
                post_afw_input ? post_afw_input->color_identity.application_serial : 0,
                handle_value(application_swapchain),
                inline_publication.copy_complete_value);
            if (FAILED(published)) {
                // Retain acquired images on failure, never expose unfinished
                // output to XR. Normal session retirement/teardown drains it.
                output.reason = GenerationPrepareReason::synthesis_failed;
                return output;
            }
        }

        // The runtime orders its use of these swapchain images against the
        // queue the application supplied, so when synthesis ran elsewhere
        // that queue has to wait for it before the release below hands the
        // images over. GPU-side, so it costs the application thread nothing.
        const bool deferred_runtime_join=late_direct_publication ||
            (generation->staged_outputs && comparison_delivery_runtime(*state->session) &&
             state->session->graphics_binding==SessionGraphicsBinding::d3d12 &&
             !state->session->d3d11_bridge && !state->session->vulkan_bridge && !state->session->luke_afw_enabled);
        if (!worker_submission && !deferred_runtime_join && state->session->d3d12_synthesis_queue && SUCCEEDED(submit_result)) {
            static_cast<void>(
                synthesizer->synchronize_consumer_queue(
                    runtime_queue(*state->session),
                    ticket));
        }

        if (SUCCEEDED(submit_result)) {
            output.native_result=ticket.native_result;
            output.synthetics_in_target_camera=!synthetic_views.empty() && ticket.synthetics_in_target_camera;
        }
        if (SUCCEEDED(submit_result) && !worker_submission &&
            (state->session->queued_nvidia_presenter || state->session->fsr_two_slot_presenter) &&
            comparison_delivery_runtime(*state->session) &&
            !state->session->luke_afw_enabled && !state->session->luke_outlaws_preframe &&
            state->session->graphics_binding == SessionGraphicsBinding::d3d12 &&
            !djules_deep_active(*state->session) && !djules_triple_active(*state->session)) {
            // Fresh NVIDIA/DLSS-G queued S/B outputs need ready-before-Wait
            // contract as Reference. An early XR prediction followed by GPU
            // work misses its own display slot even if EndFrame succeeds.
            // Deferred B uses its own completion; S must never wait on that
            // still-unsubmitted copy. FSR may require XR progress to release
            // its input queue; intentional deep/3X scheduling is also separate.
            output.reference_fence = synthesizer->submission_fence(ticket);
            output.reference_inputs=ticket.inputs;
            output.reference_current_value = ticket.fence_value;
            output.reference_synthetic_value = ticket.synthetic_fence_value
                ? ticket.synthetic_fence_value : ticket.fence_value;
            output.reference_requires_application_release = state->session->nvidia_new_route ||
                state->session->frame_generation_mode != xrfg::D3D12FrameGenerationMode::ofxr;
        }
        if (generation->staged_outputs && SUCCEEDED(submit_result)) {
            output.staging_generation=generation; output.staging_ticket=ticket;
            output.synthesizer=synthesizer;
            output.current_slot=current_slot; output.synthetic_slot=synthetic_slot;
        }
        if (worker_submission && SUCCEEDED(submit_result)) {
            // Submit both eyes before waiting. No unfinished synthesis fence
            // is placed on the application's D3D11 context here.
            output.worker_generation = generation;
            output.late_luke_publication = late_luke_publication;
            output.worker_fence=synthesizer->submission_fence(ticket);
            output.worker_fence_value=ticket.fence_value;
            output.current_destination = current_destination_index;
            output.synthetic_destination = synthetic_destination_index;
            output.current_slot = current_slot;
            output.synthetic_slot = synthetic_slot;
            output.current_handle = current_image.handle;
            output.synthetic_handle = synthetic_image.handle;
            output.extra_handle = request_extra ? extra_image.handle : XR_NULL_HANDLE;
            output.synthesizer = synthesizer;
            output.anchor_is_current = true;
            output.kind = request_pair ? PreparedGenerationKind::pair
                                       : PreparedGenerationKind::prime;
            output.reason = GenerationPrepareReason::ready;
            generation->current_slot = (current_slot + 1) % kCurrentSlotCount;
            if (request_pair)
                generation->synthetic_slot = (synthetic_slot + 1) % kSyntheticSlotCount;
            return output;
        }
        if(output.direct_current_publication) output.direct_current_publication->ticket=ticket;
        if(output.direct_synthetic_publication) output.direct_synthetic_publication->ticket=ticket;
        const bool direct_deferred=late_direct_publication && SUCCEEDED(submit_result);
        bool synthetic_released = true;
        if (request_pair && !generation->staged_outputs && !direct_deferred) {
            synthetic_released = release_private_image(
                state->session.get(),
                state->session->dispatch,
                synthetic_image);
        }
        if (request_extra && !generation->staged_outputs)
            synthetic_released = release_private_image(state->session.get(), state->session->dispatch, extra_image) && synthetic_released;
        const bool current_released = generation->staged_outputs || direct_deferred || (defer_d3d11_publication && SUCCEEDED(submit_result))
            ? true
            : release_private_image(state->session.get(),
                state->session->dispatch, current_image);

        if (FAILED(submit_result)) {
            // A busy bridge slot or an FSR context waiting for its previous GPU
            // users is ordinary backpressure. A repeated AFW guide is different:
            // pass this application frame through while retaining the last real
            // anchor, so the next advanced guide can still form an FSR pair.
            const bool stale_guides = request_pair &&
                should_preserve_generation_anchor(
                    submit_result, current_released, synthetic_released);
            output.reason = stale_guides
                ? GenerationPrepareReason::synthesis_stale_guides
                : classify_synthesis_failure(submit_result);
            output.preserve_previous = stale_guides;
            if (request_pair && !stale_guides) {
                std::scoped_lock gpu_lock(state->session->gpu_mutex);
                static_cast<void>(synthesizer->retire_previous());
            }
            return output;
        }

        output.anchor_is_current = true;
        output.current_handle = current_image.handle;
        output.synthetic_handle = synthetic_image.handle;
        output.extra_handle = request_extra ? extra_image.handle : XR_NULL_HANDLE;
        output.publication_deferred = defer_d3d11_publication;
        output.current_destination_index = current_destination_index;
        if (current_released && synthetic_released) {
            // Hand the next frame the other slot, so it can release its output
            // while this one is still queued behind the presenter.
            if (!defer_d3d11_publication)
                generation->current_slot =
                    (current_slot + 1) % kCurrentSlotCount;
            // Only a pair defers its current copy; a prime submits it inline.
            if (request_pair) {
                generation->synthetic_slot =
                    (synthetic_slot + 1) % kSyntheticSlotCount;
                output.synthesizer = synthesizer;
            }
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

[[nodiscard]] float synthetic_interpolation_fraction(
    const std::shared_ptr<SessionState>& state,
    const std::optional<ProjectionSnapshot>& previous_snapshot,
    const ProjectionSnapshot& current_snapshot, unsigned index=0) noexcept {
    const unsigned frames=state && djules_triple_active(*state)?3:2;
    const float kFixedMidpoint=float(index+1)/float(frames);
    if (state && state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::native_dlss)
        return kFixedMidpoint;
    // Match the validated new NVIDIA image contract. Predicted XR-time gaps
    // must not move legacy's synthesized content toward B from pair to pair.
    // Other backends retain their independently tested contracts.
    if (state && state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        !advanced_native_outputs(*state))
        return kFixedMidpoint;
    if (!state || !previous_snapshot) {
        return kFixedMidpoint;
    }
    XrDuration period = 0;
    {
        std::scoped_lock lock(state->mutex);
        period = state->minimum_runtime_display_period;
    }
    const XrTime interval =
        current_snapshot.display_time - previous_snapshot->display_time;
    // One period or less cannot hold a synthetic at all, and an interval wider
    // than four says the pairing has already lost its cadence.
    if (period <= 0 || interval <= period * (frames-1) || interval > period * frames * 2) {
        return kFixedMidpoint;
    }
    return 1.0F - static_cast<float>(period * (frames-1-index)) / static_cast<float>(interval);
}

[[nodiscard]] PreparedProjectionFrame prepare_projection_frame(
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    bool request_pair,
    float interpolation_fraction,
    bool worker_submission = false,
    const xrfg::D3D12PresentationTransform* captured_transform = nullptr,
    std::span<const PostAfwFrameInput> post_afw_inputs = {},
    float extra_fraction=2.0F/3.0F,
    const ProjectionSnapshot* synthetic_camera = nullptr,
    const ProjectionSnapshot* extra_camera = nullptr) noexcept {
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
        bool all_preserve_previous = request_pair;
        output.resources.reserve(mappings.size());
        for (const ProjectionResourceMapping& mapping : mappings) {
            const PostAfwFrameInput* post_afw_input = nullptr;
            if (!post_afw_inputs.empty()) {
                const auto found = std::find_if(post_afw_inputs.begin(), post_afw_inputs.end(),
                    [&](const auto& input) { return input.application_swapchain == mapping.application_swapchain; });
                if (found == post_afw_inputs.end()) {
                    output.reason = GenerationPrepareReason::missing_capture;
                    return output;
                }
                post_afw_input = &*found;
            }
            const auto reprojection_views = post_afw_input
                ? std::optional<std::vector<xrfg::D3D12ReprojectionView>>{post_afw_input->views}
                : build_reprojection_views(snapshot, mapping);
            if (!reprojection_views) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                return output;
            }
            const auto target = synthetic_camera ? build_reprojection_views(*synthetic_camera, mapping) : std::nullopt;
            const auto extra = extra_camera ? build_reprojection_views(*extra_camera, mapping) : std::nullopt;
            PreparedGeneration generation = prepare_frame_generation(
                mapping.application_swapchain,
                request_pair,
                std::span<const xrfg::D3D12ReprojectionView>(
                    reprojection_views->data(),
                    reprojection_views->size()),
                interpolation_fraction, worker_submission, captured_transform, false, post_afw_input, nullptr, extra_fraction,
                target ? std::span<const xrfg::D3D12ReprojectionView>{*target} : std::span<const xrfg::D3D12ReprojectionView>{},
                extra ? std::span<const xrfg::D3D12ReprojectionView>{*extra} : std::span<const xrfg::D3D12ReprojectionView>{});
            all_expected_kind =
                all_expected_kind && generation.kind == expected_kind;
            all_anchor_current =
                all_anchor_current && generation.anchor_is_current;
            all_preserve_previous =
                all_preserve_previous && generation.preserve_previous;
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
        output.preserve_previous = all_preserve_previous;
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

// Complete a stereo transaction on thread 2. No session GPU/call lock is
// held across a GPU fence wait; the producer may render its next frame.
[[nodiscard]] bool finish_worker_outputs(
    const std::shared_ptr<SessionState>& state, PreparedProjectionFrame& prepared,
    std::uint64_t source_sequence) noexcept {
    bool success = prepared.kind != PreparedGenerationKind::none;
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        if (g.late_luke_publication) continue; // acquire below while dispatched GPU work proceeds
        if(g.staging_generation && (djules_deep_active(*state)||djules_triple_active(*state))) continue;
        const auto completion_token = xrfg::bridge_flight_logger().begin(
            xrfg::BridgeFlightOperation::timing_synthesis_complete,
            source_sequence, handle_value(resource.application_swapchain));
        const HRESULT ready = g.worker_fence && g.worker_fence_value
            ? wait_composition_fence(g.worker_fence.Get(),g.worker_fence_value,
                kFrameStartSynthesisWaitMilliseconds) : E_INVALIDARG;
        xrfg::bridge_flight_logger().end(completion_token,
            xrfg::BridgeFlightOperation::timing_synthesis_complete, ready,
            source_sequence, handle_value(resource.application_swapchain));
        // On failure keep the images acquired; session teardown drains GPU
        // work before releasing them. Never return unfinished images to XR.
        if (FAILED(ready)) {
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::gpu_fence_snapshot,
                49300, handle_value(g.worker_fence.Get()), g.worker_fence_value,
                g.worker_fence ? g.worker_fence->GetCompletedValue() : 0);
            if (g.worker_generation->synthesizer)
                g.worker_generation->synthesizer->report_pending_submission();
            if (g.worker_generation->afw_temporal_synthesizer)
                g.worker_generation->afw_temporal_synthesizer->report_pending_submission();
            return false;
        }
    }
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        if (success && g.worker_generation->d3d11_interop) {
            std::optional<xrfg::D3D11PublicationTargets> targets;
            if (g.late_luke_publication) {
                auto& current = g.worker_generation->current[g.current_slot];
                auto& synthetic = g.worker_generation->synthetic[g.synthetic_slot];
                const bool acquired = acquire_and_wait_private_image(state.get(), state->dispatch, current) &&
                    (g.kind != PreparedGenerationKind::pair || acquire_and_wait_private_image(state.get(), state->dispatch, synthetic));
                const HRESULT ready = acquired && g.worker_fence ? wait_composition_fence(g.worker_fence.Get(),
                    g.worker_fence_value, kFrameStartSynthesisWaitMilliseconds) : E_FAIL;
                if (FAILED(ready)) return false; // teardown retains/drains any acquired images
                targets = xrfg::D3D11PublicationTargets{
                    static_cast<std::uint32_t>(g.current_slot * g.worker_generation->current_images_per_slot + current.acquired_index),
                    g.kind == PreparedGenerationKind::pair ? std::optional<std::uint32_t>{g.synthetic_slot *
                        g.worker_generation->synthetic_images_per_slot + synthetic.acquired_index} : std::nullopt};
            }
            const auto token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::d3d11_publish, handle_value(resource.application_swapchain));
            const HRESULT result = g.worker_generation->d3d11_interop->publish_completed(
                g.current_destination, g.kind == PreparedGenerationKind::pair
                    ? std::optional<std::uint32_t>{g.synthetic_destination} : std::nullopt,
                g.worker_fence.Get(),g.worker_fence_value,
                &g.worker_publication, targets);
            xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::d3d11_publish,
                result, g.current_destination, g.synthetic_destination);
            success = SUCCEEDED(result) && success;
        }
    }
    for (auto& resource : prepared.resources) {
        auto& g = resource.generation;
        if (!g.worker_generation) continue;
        // Wait only for this publication, outside the interop mutex. A global
        // idle wait blocks AFW capture/publish on that same bridge and can
        // also inherit the next producer's completion cursor.
        if (g.worker_generation->d3d11_interop && g.worker_publication.fence) {
            const auto publication_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_publication_complete,
                source_sequence, handle_value(resource.application_swapchain),
                g.worker_publication.copy_complete_value);
            const HRESULT published = xrfg::wait_for_d3d11_publication(g.worker_publication);
            xrfg::bridge_flight_logger().end(publication_token,
                xrfg::BridgeFlightOperation::timing_publication_complete, published,
                source_sequence, handle_value(resource.application_swapchain),
                g.worker_publication.copy_complete_value);
            success = SUCCEEDED(published) && success;
        }
        if (g.staging_generation) continue;
        const bool private_ready=state->luke_afw_enabled && state->dispatch->steamvr_runtime &&
            state->graphics_binding==SessionGraphicsBinding::d3d11 && state->private_runtime;
        auto current_proof=private_ready && g.worker_generation->d3d11_interop && g.worker_publication.fence
            ? g.worker_generation->d3d11_interop->published_image(g.worker_publication.current_destination_index,false)
            : Microsoft::WRL::ComPtr<ID3D11Texture2D>{};
        auto synthetic_proof=private_ready && g.worker_generation->d3d11_interop && g.worker_publication.fence
            ? g.worker_generation->d3d11_interop->published_image(g.worker_publication.synthetic_destination_index,true)
            : Microsoft::WRL::ComPtr<ID3D11Texture2D>{};
        if (g.kind == PreparedGenerationKind::pair)
            success = release_private_image(state.get(), state->dispatch,
                g.worker_generation->synthetic[g.synthetic_slot],&g.worker_publication,synthetic_proof.Get()) && success;
        if (g.extra_handle != XR_NULL_HANDLE)
            success = release_private_image(state.get(), state->dispatch,
                g.worker_generation->synthetic[(g.synthetic_slot+1)%g.worker_generation->synthetic_runtime_rings]) && success;
        success = release_private_image(state.get(), state->dispatch,
            g.worker_generation->current[g.current_slot],&g.worker_publication,current_proof.Get()) && success;
    }
    return success;
}

void synthesis_worker_main(const std::shared_ptr<SessionState>& state) noexcept {
    try {
        for (;;) {
            std::shared_ptr<SynthesisJob> job;
            {
                std::unique_lock lock(state->presenter_mutex);
                state->presenter_condition.wait(lock, [&] {
                    return state->synthesis_job || state->presenter_stop_requested ||
                           XR_FAILED(state->presenter_failure);
                });
                if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                job = std::move(state->synthesis_job);
            }
            const auto worker_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_run,
                job->current->source_sequence, job->current->previous_sequence);
            const auto prepare_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_prepare,
                job->current->source_sequence, job->current->previous_sequence);
            PreparedProjectionFrame prepared = prepare_projection_frame(
                job->snapshot, job->mappings, job->pair, job->fraction, true,
                &job->presentation_transform, job->post_afw_frame
                    ? std::span<const PostAfwFrameInput>{job->post_afw_frame->inputs}
                    : std::span<const PostAfwFrameInput>{},job->extra_fraction,
                job->synthetic_camera ? &*job->synthetic_camera : nullptr,
                job->extra_camera ? &*job->extra_camera : nullptr);
            state->generation_status = prepared.kind == PreparedGenerationKind::pair
                ? xrfg::autonomous::GenerationState::ready : prepared.kind == PreparedGenerationKind::prime
                ? xrfg::autonomous::GenerationState::recovering : xrfg::autonomous::GenerationState::waiting;
            xrfg::bridge_flight_logger().end(prepare_token,
                xrfg::BridgeFlightOperation::timing_worker_prepare,
                prepared.kind == PreparedGenerationKind::none ? -1 : 0,
                job->current->source_sequence, job->current->previous_sequence,
                static_cast<std::uint64_t>(prepared.kind));
            bool success = finish_worker_outputs(state, prepared, job->current->source_sequence);
            const auto auxiliary_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_aux_complete,
                job->current->source_sequence, job->current->previous_sequence);
            if (success && job->metadata &&
                !(djules_deep_active(*state)||djules_triple_active(*state))) {
                for (const auto& image : job->metadata->composition_images) {
                    success = SUCCEEDED(wait_composition_fence(image->fence.Get(), image->ready_value)) && success;
                    success = SUCCEEDED(wait_composition_fence(image->fence12.Get(), image->ready_value)) && success;
                }
            }
            std::shared_ptr<GeneratedFrameEndInfo> real;
            std::shared_ptr<GeneratedFrameEndInfo> synthetic, extra_synthetic;
            xrfg::bridge_flight_logger().end(auxiliary_token,
                xrfg::BridgeFlightOperation::timing_aux_complete, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            const auto build_token = xrfg::bridge_flight_logger().begin(
                xrfg::BridgeFlightOperation::timing_worker_build,
                job->current->source_sequence, job->current->previous_sequence);
            if (success) {
                std::vector<ProjectionResourceDestination> real_destinations, synthetic_destinations, extra_destinations;
                for (const auto& resource : prepared.resources) {
                    XrSwapchain source = resource.application_swapchain;
                    if (job->post_afw_frame) {
                        const auto& inputs = job->post_afw_frame->inputs;
                        const auto found = std::find_if(inputs.begin(), inputs.end(),
                            [&](const auto& input) { return input.application_swapchain == source; });
                        if (found == inputs.end()) { success = false; break; }
                        source = found->completed_swapchain;
                    }
                    real_destinations.push_back({source,
                                                resource.generation.current_handle});
                    synthetic_destinations.push_back({source,
                                                     resource.generation.synthetic_handle, resource.generation.synthetics_in_target_camera});
                    extra_destinations.push_back({source, resource.generation.extra_handle, resource.generation.synthetics_in_target_camera});
                }
                GeneratedFrameEndInfo real_info, synthetic_info;
                success = success && build_generated_frame_end_info(&job->metadata->info, job->snapshot,
                    false, real_destinations, &real_info);
                GeneratedFrameEndInfo pending_synthetic,pending_extra;
                attach_djules_outputs(prepared,real_info,job->pair?&pending_synthetic:nullptr,
                    job->pair&&job->triple?&pending_extra:nullptr);
                real_info.blackbar_pose.paired=job->pair;
                real_info.composition_images = job->metadata->composition_images;
                if (success) real = make_presenter_owned_frame(std::move(real_info));
                success = success && real != nullptr;
                if (success && job->pair) {
                    success = build_generated_frame_end_info(&job->metadata->info, job->snapshot,
                        true, synthetic_destinations, &synthetic_info, job->synthetic_camera ? &*job->synthetic_camera : nullptr);
                    synthetic_info.pending_outputs=std::move(pending_synthetic.pending_outputs);
                    synthetic_info.native_results=std::move(pending_synthetic.native_results);
                    synthetic_info.direct_synthetic_leases=std::move(pending_synthetic.direct_synthetic_leases);
                    synthetic_info.synthetic = true;
                    synthetic_info.composition_images = job->metadata->composition_images;
                    synthetic_info.djules_leader=job->triple;
                    if (success) synthetic = make_presenter_owned_frame(std::move(synthetic_info));
                    success = success && synthetic != nullptr;
                    if(success && job->triple) {
                        GeneratedFrameEndInfo extra_info;
                        success=build_generated_frame_end_info(&job->metadata->info,job->snapshot,true,
                            extra_destinations,&extra_info, job->extra_camera ? &*job->extra_camera : nullptr);
                        extra_info.pending_outputs=std::move(pending_extra.pending_outputs);
                        extra_info.native_results=std::move(pending_extra.native_results);
                        extra_info.direct_synthetic_leases=std::move(pending_extra.direct_synthetic_leases);
                        extra_info.native_output=1;
                        extra_info.synthetic=true;
                        extra_info.composition_images=job->metadata->composition_images;
                        if(success) extra_synthetic=make_presenter_owned_frame(std::move(extra_info));
                        success=success && extra_synthetic!=nullptr;
                    }
                }
            }
            xrfg::bridge_flight_logger().end(build_token,
                xrfg::BridgeFlightOperation::timing_worker_build, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            xrfg::bridge_flight_logger().end(worker_token,
                xrfg::BridgeFlightOperation::timing_worker_run, success ? 0 : -1,
                job->current->source_sequence, job->current->previous_sequence);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::generation_prepare,
                success ? 0 : static_cast<std::int64_t>(prepared.reason),
                handle_value(prepared.failed_swapchain), static_cast<std::uint64_t>(prepared.kind),
                job->current->source_sequence);
            if (!success) {
                std::scoped_lock lock(state->presenter_mutex);
                state->synthesis_busy = false;
                fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
                state->presenter_condition.notify_all();
                break;
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_gpu_ready,
                (djules_deep_active(*state)||djules_triple_active(*state)) ? 2 : job->pair ? 1 : 0,
                job->current->source_sequence, job->current->previous_sequence);
            {
                std::unique_lock lock(state->presenter_mutex);
                job->current->frame = std::move(real);
                if (job->pair) {
                    // With immediate real delivery there may be no retained A.
                    // Align B beside its S slots, without requiring another input.
                    if (single_director_pacing(*state) && !state->three_slots.slot(0))
                        static_cast<void>(state->three_slots.shift_empty());
                    // A may still be inside downstream EndFrame. Never replace
                    // its descriptor until the director has acknowledged it.
                    const auto slot_token = xrfg::bridge_flight_logger().begin(
                        xrfg::BridgeFlightOperation::timing_slot_wait,
                        job->current->source_sequence, job->current->previous_sequence);
                    state->presenter_condition.wait(lock, [&] {
                        return state->three_slots.can_insert_synthetic() ||
                            state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
                    });
                    if (state->presenter_stop_requested || XR_FAILED(state->presenter_failure)) break;
                    xrfg::bridge_flight_logger().end(slot_token,
                        xrfg::BridgeFlightOperation::timing_slot_wait, 0,
                        job->current->source_sequence, job->current->previous_sequence);
                    auto midpoint = std::make_shared<ThreeSlotFrame>();
                    midpoint->queued_at=job->current->queued_at;
                    midpoint->source_sequence = job->current->source_sequence;
                    midpoint->previous_sequence = job->current->previous_sequence;
                    midpoint->frame = std::move(synthetic);
                    if(job->triple) {
                        auto extra=std::make_shared<ThreeSlotFrame>();
                        extra->queued_at=job->current->queued_at; extra->source_sequence=midpoint->source_sequence;
                        extra->previous_sequence=midpoint->previous_sequence; extra->frame=std::move(extra_synthetic);
                        static_cast<void>(state->three_slots.push_synthetic_pair(std::move(midpoint),std::move(extra)));
                    } else static_cast<void>(state->three_slots.push_synthetic(std::move(midpoint)));
                    state->generation_steady_state_established = true;
                } else {
                    // Two O(1) index changes, no empty OpenXR submissions and
                    // no image copies. A is now held in logical slot 1.
                    for(std::size_t i=1;i<state->three_slots.capacity();++i)
                        static_cast<void>(state->three_slots.shift_empty());
                }
                if (single_director_pacing(*state)) {
                    // No additional ready-pair backlog. The endpoint credit is the
                    // only delivery admission; retain this batch until accepted,
                    // not until native EndFrame/display. The app may render ahead.
                    state->presenter_condition.notify_all();
                    state->presenter_condition.wait(lock,[&] {
                        return (state->three_slots.empty() && !state->presenter_inflight_frame) ||
                            state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
                    });
                }
                state->synthesis_busy = false;
            }
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::timing_worker_publish,
                0, job->current->source_sequence, job->current->previous_sequence);
            state->presenter_condition.notify_all();
        }
    } catch (...) {
        std::scoped_lock lock(state->presenter_mutex);
        state->synthesis_busy = false;
        fail_pending_presenter_submissions_locked(*state, XR_ERROR_RUNTIME_FAILURE);
    }
    state->presenter_condition.notify_all();
}

// Only texture-backed layers can be retained without additional extension-
// specific lifetime hooks. Preserve order, eye visibility, geometry and alpha.
[[nodiscard]] XrSwapchain* composition_swapchain(OwnedCompositionLayer& layer) {
    return std::visit([](auto& value) -> XrSwapchain* {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, XrCompositionLayerCubeKHR>) return &value.swapchain;
        else if constexpr (std::is_same_v<T, XrCompositionLayerQuad> ||
                           std::is_same_v<T, XrCompositionLayerCylinderKHR> ||
                           std::is_same_v<T, XrCompositionLayerEquirectKHR> ||
                           std::is_same_v<T, XrCompositionLayerEquirect2KHR>) return &value.subImage.swapchain;
        else return nullptr;
    }, layer);
}

[[nodiscard]] std::shared_ptr<CompositionImageSlot> private_composition_slot(
    const std::shared_ptr<SessionState>& state, XrSwapchain token) {
    std::scoped_lock lock(state->auxiliary_registry_mutex);
    const auto found=state->auxiliary_registry.find(token);
    return found==state->auxiliary_registry.end() ? nullptr : found->second.lock();
}

XrResult end_frame_with_private_auxiliary(const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo* info,const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& delivery) {
    const auto submit=[&](const XrFrameEndInfo* end) {
        if(delivery && state->private_runtime)return state->private_runtime->end_frame(end,delivery);
        // AFW-only measures the exact native target after the Luke source clock
        // has been replaced by the real runtime slot. Observation only: no
        // director, extra GPU work, or synthetic refresh added to this estimate.
        const auto entered=delivery?xrfg::interface_clock_ns():0;
        const auto result=state->dispatch->end_frame(state->handle,end);
        if(delivery) {
            const auto returned=xrfg::interface_clock_ns();
            LARGE_INTEGER qpc{};
            const bool target=end && state->dispatch->convert_time_to_qpc &&
                XR_SUCCEEDED(state->dispatch->convert_time_to_qpc(state->dispatch->instance,end->displayTime,&qpc));
            delivery->completed(result==XR_SUCCESS && end && end->layerCount,entered,returned,
                target?xrfg::interface_qpc_ns(qpc.QuadPart):0);
        }
        return result;
    };
    if (!state->luke_afw_enabled || !comparison_delivery_runtime(*state) ||
        state->graphics_binding!=SessionGraphicsBinding::d3d11 ||
        !info || !info->layers || !info->layerCount)
        return submit(info);
    std::vector<OwnedCompositionLayer> copies;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::vector<std::shared_ptr<CompositionImageSlot>> snapshots;
    copies.reserve(info->layerCount);layers.reserve(info->layerCount);
    for (std::uint32_t i=0;i<info->layerCount;++i) {
        const auto* header=info->layers[i];
        auto copy=copy_composition_layer_for_presenter(header);
        auto* handle=copy ? composition_swapchain(*copy) : nullptr;
        auto slot=handle ? private_composition_slot(state,*handle) : nullptr;
        if (!slot) {layers.push_back(header);continue;}
        // One physical output cannot name two different retained snapshots of
        // the same source. Repeated layers of the SAME snapshot share one copy.
        const auto prior=std::find_if(snapshots.begin(),snapshots.end(),[&](const auto& other) {
            return other->public_output==slot->public_output;
        });
        if (prior!=snapshots.end() && *prior!=slot) return XR_ERROR_LAYER_INVALID;
        if (prior==snapshots.end()) snapshots.push_back(slot);
        *handle=slot->public_output->image.handle;
        std::visit([&](auto& value){value.next=header->next;},*copy);
        copies.push_back(std::move(*copy));
        layers.push_back(composition_layer_header(copies.back()));
    }
    if (snapshots.empty()) return submit(info);
    std::scoped_lock publication_lock(state->auxiliary_publication_mutex);
    for (const auto& slot:snapshots) {
        auto& output=*slot->public_output;
        if (slot->copy_failed || output.unproven_copy) return XR_ERROR_RUNTIME_FAILURE;
        // A preceding failed/timed-out physical publication may still read its
        // source. Retire that exact receipt before replacing pending_source.
        if (FAILED(wait_composition_fence(output.fence.Get(),output.serial,3000)))
            return XR_ERROR_RUNTIME_FAILURE;
        output.pending_source.Reset();
        // S/B may share this endpoint. If no newer publication repointed the
        // XR handle, reuse its completed pixels without another GPU copy.
        if (output.image.phase==PrivateOwnershipPhase::idle &&
            output.published_snapshot==slot->image.handle && output.published_ready_value==slot->ready_value) continue;
        if (FAILED(wait_composition_fence(slot->fence.Get(),slot->ready_value,3000)))
            return XR_ERROR_RUNTIME_FAILURE;
        if (!acquire_and_wait_private_image(state.get(),state->dispatch,output.image) ||
            output.image.acquired_index>=output.textures.size()) return XR_ERROR_RUNTIME_FAILURE;
        HRESULT result;
        {
            // Physical EndFrame already owns the runtime gate. The game's
            // Release holds gpu_mutex before entering that gate: taking it
            // here would invert their order and deadlock both threads.
            // This copy touches only an immutable leased snapshot and this
            // serialized publication output, never mutable capture/history.
            // capture_composition_image requires ID3D11Multithread protection;
            // CopyResource/Signal do not modify pipeline bindings and remain
            // ordered on that shared immediate context without gpu_mutex.
            output.pending_source=slot->textures.front();
            output.context->CopyResource(output.textures[output.image.acquired_index].Get(),output.pending_source.Get());
            result=output.context->Signal(output.fence.Get(),output.serial+1);
            if (SUCCEEDED(result)) {
                ++output.serial;
                slot->last_publication.store(output.serial);
            } else {
                output.unproven_copy=true;
                slot->last_publication.store(UINT64_MAX);
            }
            output.context->Flush();
        }
        // Hold only the publication mutex across this exact copy receipt.
        // No producer GPU/call lock or next-frame fence is followed here.
        if (FAILED(result) || FAILED(wait_composition_fence(output.fence.Get(),output.serial,3000)))
            return XR_ERROR_RUNTIME_FAILURE;
        output.pending_source.Reset();
        if (!release_private_image(state.get(),state->dispatch,output.image)) return XR_ERROR_RUNTIME_FAILURE;
        output.published_snapshot=slot->image.handle;output.published_ready_value=slot->ready_value;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_publish,
            S_OK,handle_value(slot->image.handle),handle_value(output.image.handle),output.serial);
    }
    auto submitted=*info;submitted.layers=layers.data();
    return submit(&submitted);
}

// Once per second, CPU-only, and never wait for a producer/presenter lock.
// A busy owner keeps the preceding complete snapshot; partial counts are not
// reported as an unexplained resource or as proof of runtime headroom.
xrfg::autonomous::SwapchainBudgetStatus luke_swapchain_inventory(
    const std::shared_ptr<SessionState>& state,const XrFrameEndInfo* frame) {
    auto previous=state->swapchain_budget;
    previous.fallback=state->luke_aux_budget_inline.load();
    const auto now=GetTickCount64();
    if (!state->luke_afw_enabled || now<state->next_swapchain_inventory_ms) return previous;
    state->next_swapchain_inventory_ms=now+1000;
    xrfg::autonomous::SwapchainBudgetStatus out{};out.fallback=previous.fallback;
    std::vector<xrfg::PrivateOpenXrRuntime::SwapchainMapping> native_mapping;
    if(state->private_runtime) {
        auto mapping=state->private_runtime->native_swapchain_mapping();
        if(!mapping)return previous;
        native_mapping=std::move(*mapping);
    }
    bool incomplete{};
    std::vector<XrSwapchain> projections,auxiliary,seen;
    if (frame && frame->layers) for (unsigned i=0;i<frame->layerCount;++i) {
        const auto* layer=frame->layers[i];if (!layer) continue;
        if (layer->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(layer);
            for (unsigned v=0;p->views && v<p->viewCount;++v) projections.push_back(p->views[v].subImage.swapchain);
        } else {
            auto copy=copy_composition_layer_for_presenter(layer);
            if (auto* handle=copy ? composition_swapchain(*copy) : nullptr) auxiliary.push_back(*handle);
        }
    }
    const auto count=[&](XrSwapchain handle,std::uint32_t& category) {
        if(!handle)return;
        if(state->private_runtime) {
            const auto found=std::find_if(native_mapping.begin(),native_mapping.end(),[&](const auto& m){return m.handle==handle;});
            if(found==native_mapping.end()){incomplete=true;return;}
            handle=found->native;
        }
        if (std::find(seen.begin(),seen.end(),handle)!=seen.end()) return;
        seen.push_back(handle);++category;++out.total;
    };
    std::vector<CompositionImageSlot*> private_images;
    const auto count_slot=[&](const std::shared_ptr<CompositionImageSlot>& slot) {
        if (!slot) return;
        if (slot->public_output) {
            count(slot->public_output->image.handle,out.auxiliary_outputs);
            if (std::find(private_images.begin(),private_images.end(),slot.get())==private_images.end()) {
                private_images.push_back(slot.get());++out.private_snapshots;
            }
        } else count(slot->image.handle,out.auxiliary_outputs);
    };
    std::size_t detail_size{};
    for (const auto& chain:find_swapchains(state)) {
        std::unique_lock call_lock(chain->call_mutex,std::try_to_lock);
        if (!call_lock.owns_lock()) return previous;
        std::unique_lock data_lock(chain->mutex,std::try_to_lock);
        if (!data_lock.owns_lock()) return previous;
        count(chain->handle,out.application);
        if (std::find(projections.begin(),projections.end(),chain->handle)!=projections.end()) chain->diagnostic_layer_roles|=1;
        if (std::find(auxiliary.begin(),auxiliary.end(),chain->handle)!=auxiliary.end()) chain->diagnostic_layer_roles|=2;
        const bool eye=(chain->diagnostic_layer_roles&1)!=0;
        const bool aux=(chain->diagnostic_layer_roles&2)!=0;
        const bool depth=(chain->create_info.usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)!=0;
        if (eye) ++out.projection;else if(aux) ++out.auxiliary;else if(depth)++out.depth;else ++out.other;
        const bool refused=chain->generation_refusal==SwapchainEligibilityReason::budget_exhausted;
        if (refused) ++out.refused;
        if((!eye && !aux && !depth) || refused) {
            char text[192]{};
            std::snprintf(text,sizeof(text),"App %s: 0x%llX, %ux%u, array %u, format %lld%s\n",
                eye ? "eye" : aux ? "auxiliary" : depth ? "depth" : "UNCLASSIFIED",
                static_cast<unsigned long long>(handle_value(chain->handle)),chain->create_info.width,chain->create_info.height,
                chain->create_info.arraySize,static_cast<long long>(chain->create_info.format),refused ? " [generation allocation refused]" : "");
            const auto bytes=std::strlen(text);
            if (detail_size+bytes<sizeof(out.details)) {
                std::memcpy(out.details+detail_size,text,bytes);detail_size+=bytes;
            } else out.details_truncated=true;
        }
        if (chain->frame_generation) {
            for (const auto& image:chain->frame_generation->current)count(image.handle,out.current);
            for (const auto& image:chain->frame_generation->synthetic)count(image.handle,out.synthetic);
        }
        if (chain->composition_capture) {
            if (chain->composition_capture->public_output)
                count(chain->composition_capture->public_output->image.handle,out.auxiliary_outputs);
            for (const auto& slot:chain->composition_capture->outputs) count_slot(slot);
        }
    }
    std::vector<std::shared_ptr<CompositionImageSlot>> retained;
    { // Also account for snapshots retained after their producer was retired.
        std::unique_lock lock(state->auxiliary_registry_mutex,std::try_to_lock);
        if (!lock.owns_lock()) return previous;
        for (const auto& [token,weak]:state->auxiliary_registry) if (auto slot=weak.lock())retained.push_back(std::move(slot));
    }
    for (const auto& slot:retained)count_slot(slot);
    if (state->fps_overlay) {
        const auto ui=state->fps_overlay->swapchain_handles();if (!ui) return previous;
        count((*ui)[0],out.fps);count((*ui)[1],out.meter);count((*ui)[2],out.menu);
    }
    if(incomplete)return previous;
    // Count each final native endpoint once, never its independent private tokens.
    for(const auto& mapping:native_mapping)if(mapping.native &&
        std::find(seen.begin(),seen.end(),mapping.native)==seen.end()) {
        seen.push_back(mapping.native);++out.unassigned_native;++out.total;
    }
    out.updated_ms=now;state->swapchain_budget=out;
    return out;
}

// Loading/menu quad outputs otherwise occupy SteamVR's finite XR budget until
// session teardown, even after the game submits only its projection. Retire
// only unreferenced, unleased, released outputs whose own copy fence completed;
// preserve the application's handles and captured source for a later return.
void retire_unused_composition_outputs(const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo* current) noexcept {
    if (!current || current->type!=XR_TYPE_FRAME_END_INFO ||
        (current->layerCount && !current->layers) || !state->dispatch->destroy_swapchain) return;
    try {
        std::vector<XrSwapchain> referenced;
        for(unsigned i=0;i<current->layerCount;++i) {
            const auto* header=current->layers[i];if(!header)return;
            if(header->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
            auto copy=copy_composition_layer_for_presenter(header);
            auto* handle=copy ? composition_swapchain(*copy) : nullptr;
            if(!handle)return;referenced.push_back(*handle);
        }
        for(const auto& chain:find_swapchains(state)) {
            if(std::find(referenced.begin(),referenced.end(),chain->handle)!=referenced.end())continue;
            std::scoped_lock lock(chain->call_mutex);
            if(!chain->composition_capture)continue;
            for(auto& slot:chain->composition_capture->outputs) {
                if(!slot || slot.use_count()!=1 || !slot->image.handle ||
                    slot->image.phase!=PrivateOwnershipPhase::idle)continue;
                const auto completed=slot->fence12 ? slot->fence12->GetCompletedValue() :
                    slot->fence ? slot->fence->GetCompletedValue() : 0;
                if(completed==UINT64_MAX || completed<slot->ready_value)continue;
                if(slot->public_output) {
                    // This is a private snapshot token, never an XR handle.
                    // Its last publication may still be reading its color.
                    const auto published=slot->public_output->fence->GetCompletedValue();
                    if(slot->copy_failed || published==UINT64_MAX ||
                        published<slot->last_publication.load())continue;
                    slot.reset();continue;
                }
                const auto retired=slot->image.handle;
                const auto result=with_runtime_entry(state.get(),[&] {
                    return state->dispatch->destroy_swapchain(retired);
                });
                if(XR_SUCCEEDED(result)) {
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_publish,
                        S_FALSE,handle_value(chain->handle),handle_value(retired),slot->ready_value);
                    slot->image.handle=XR_NULL_HANDLE;slot.reset();
                }
            }
            auto& output=chain->composition_capture->public_output;
            if(output && output.use_count()==1 && !output->unproven_copy &&
                output->image.phase==PrivateOwnershipPhase::idle) {
                const auto completed=output->fence->GetCompletedValue();
                if(completed!=UINT64_MAX && completed>=output->serial)output.reset();
            }
        }
    } catch(...) {}
}

[[nodiscard]] bool composition_can_be_owned(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source) {
    if (!source || source->next || (source->layerCount && !source->layers)) return false;
    bool supported = true;
    for (std::uint32_t i = 0; i < source->layerCount; ++i) {
        const auto* header = source->layers[i];
        if (!header) return false;
        if (header->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        auto copy = copy_composition_layer_for_presenter(header);
        XrSwapchain* handle = copy ? composition_swapchain(*copy) : nullptr;
        std::uint64_t reason = (!handle || header->next) ? 1 : 0;
        if (!reason) {
            const auto chain = find_swapchain(*handle);
            if (!chain || chain->session != state) reason = 1;
            else {
                std::scoped_lock lock(chain->call_mutex);
                chain->composition_referenced = true;
                if (!chain->composition_capture || !chain->composition_capture->valid) reason = 2;
            }
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_layer,
            header->type, i, handle ? handle_value(*handle) : 0, reason);
        supported = !reason && supported;
    }
    return supported;
}

[[nodiscard]] XrResult own_composition_images(
    const std::shared_ptr<SessionState>& session, GeneratedFrameEndInfo& metadata,
    XrResult* creation_refusal = nullptr) {
    if (creation_refusal) *creation_refusal=XR_SUCCESS;
    std::unordered_map<XrSwapchain, std::shared_ptr<CompositionImageSlot>> leased;
    for (auto& layer : metadata.composition_layers) {
        auto* handle = composition_swapchain(layer);
        if (!handle) return XR_ERROR_LAYER_INVALID;
        if (const auto found = leased.find(*handle); found != leased.end()) {
            *handle = found->second->image.handle;
            continue;
        }
        if (auto retained=private_composition_slot(session,*handle)) {
            leased.emplace(*handle,retained);
            metadata.composition_images.push_back(std::move(retained));
            continue;
        }
        const auto chain = find_swapchain(*handle);
        if (!chain || chain->session != session) return XR_ERROR_LAYER_INVALID;
        std::scoped_lock call_lock(chain->call_mutex);
        if (!chain->composition_capture || !chain->composition_capture->valid) return XR_ERROR_LAYER_INVALID;
        // Separate an auxiliary ownership failure from projection capture
        // gaps. Otherwise a failed handoff looks like a generator stall.
        const auto failed=[&](unsigned reason,std::uint64_t detail=0) {
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_publish,
                XR_ERROR_RUNTIME_FAILURE,handle_value(*handle),reason,detail);
            return XR_ERROR_RUNTIME_FAILURE;
        };
        auto& capture = *chain->composition_capture;
        const bool private_auxiliary=session->luke_afw_enabled && comparison_delivery_runtime(*session) &&
            session->graphics_binding==SessionGraphicsBinding::d3d11 && capture.texture;
        if (private_auxiliary && !capture.public_output) {
            if (!session->d3d11_device) return failed(7);
            auto output=std::shared_ptr<CompositionPublicOutput>(new CompositionPublicOutput,
                delete_composition_public_output);
            output->session=session;output->context=capture.context;
            Microsoft::WRL::ComPtr<ID3D11Device5> device;
            HRESULT result=session->d3d11_device.As(&device);
            if (SUCCEEDED(result)) result=device->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&output->fence));
            if (FAILED(result)) return failed(7,static_cast<std::uint32_t>(result));
            auto info=chain->create_info;info.next=nullptr;
            info.createFlags &= ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
            info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            CreatedPrivateSwapchain created;XrResult refusal=XR_SUCCESS;
            const auto made=with_runtime_entry(session,[&] {
                return create_private_swapchain(chain,info,&created,&refusal);
            });
            if (!made) {
                if (creation_refusal) *creation_refusal=refusal;
                return failed(1,static_cast<std::uint64_t>(static_cast<std::int64_t>(refusal)));
            }
            output->image=created.state;
            for (auto* texture:created.d3d11_resources) output->textures.emplace_back(texture);
            capture.public_output=std::move(output);
        }
        std::shared_ptr<CompositionImageSlot> slot;
        for (auto& candidate : capture.outputs) {
            if (candidate && candidate->public_output) {
                if (candidate->copy_failed) continue;
                const auto completed=candidate->public_output->fence->GetCompletedValue();
                if (completed==UINT64_MAX || completed<candidate->last_publication.load()) continue;
            }
            if (candidate && candidate.use_count() != 1) continue;
            if (!candidate) {
                if (private_auxiliary) {
                    auto retained=std::make_shared<CompositionImageSlot>();
                    D3D11_TEXTURE2D_DESC desc{};capture.texture->GetDesc(&desc);
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                    const auto result=session->d3d11_device->CreateTexture2D(&desc,nullptr,&texture);
                    if (FAILED(result)) return failed(7,static_cast<std::uint32_t>(result));
                    retained->textures.push_back(std::move(texture));
                    retained->fence=capture.fence;retained->public_output=capture.public_output;
                    retained->image.handle=reinterpret_cast<XrSwapchain>(retained.get());
                    {
                        std::scoped_lock registry_lock(session->auxiliary_registry_mutex);
                        std::erase_if(session->auxiliary_registry,[](const auto& entry){return entry.second.expired();});
                        session->auxiliary_registry[retained->image.handle]=retained;
                    }
                    candidate=std::move(retained);
                } else {
                    // Native/other-runtime ownership keeps distinct XR handles.
                    candidate = std::make_shared<CompositionImageSlot>();
                    auto info = chain->create_info;
                    info.next = nullptr;
                    info.createFlags &= ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
                    info.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
                    CreatedPrivateSwapchain created;
                    XrResult refusal=XR_SUCCESS;
                    const XrResult created_result = with_runtime_entry(session.get(), [&] {
                        return create_private_swapchain(chain, info, &created,&refusal)
                            ? XR_SUCCESS : XR_ERROR_RUNTIME_FAILURE;
                    });
                    if (XR_FAILED(created_result)) {
                        if(creation_refusal) *creation_refusal=refusal;
                        candidate.reset(); return failed(1,
                            static_cast<std::uint64_t>(static_cast<std::int64_t>(refusal)));
                    }
                    candidate->image = created.state;
                    for (auto* texture : created.d3d11_resources) candidate->textures.emplace_back(texture);
                    for (auto* texture : created.d3d12_resources) candidate->textures12.emplace_back(texture);
                    candidate->fence = capture.fence;
                    candidate->fence12 = capture.fence12;
                }
            }
            slot = candidate;
            break;
        }
        if (!slot) return failed(2); // all auxiliary slots still leased
        if (!slot->public_output && !acquire_and_wait_private_image(session.get(), session->dispatch, slot->image))
            return failed(3,handle_value(slot->image.handle));
        HRESULT result;
        {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            if (capture.texture12) {
                if (slot->image.acquired_index >= slot->textures12.size()) return failed(4,slot->image.acquired_index);
                result = copy_composition12(session->d3d12_device.Get(), session->d3d12_queue.Get(),
                    capture.fence12.Get(), capture.fence_value, slot->copy12,
                    slot->textures12[slot->image.acquired_index].Get(), D3D12_RESOURCE_STATE_COMMON,
                    capture.texture12.Get(), D3D12_RESOURCE_STATE_COMMON);
                if (SUCCEEDED(result)) slot->ready_value = slot->copy12.ready_value;
            } else {
                if (slot->image.acquired_index >= slot->textures.size()) return failed(4,slot->image.acquired_index);
                capture.context->CopyResource(slot->textures[slot->image.acquired_index].Get(), capture.texture.Get());
                result = capture.context->Signal(capture.fence.Get(), capture.fence_value + 1);
                if (SUCCEEDED(result)) slot->ready_value = ++capture.fence_value;
                capture.context->Flush();
            }
        }
        // The worker waits for this fence, not the producer. No descriptor
        // becomes director-eligible before all projection + auxiliary pixels.
        const bool released = slot->public_output || release_private_image(session.get(), session->dispatch, slot->image);
        if (FAILED(result)) slot->copy_failed=true;
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::composition_publish,
            result, handle_value(*handle), handle_value(slot->image.handle), slot->ready_value);
        if (FAILED(result)) return failed(5,static_cast<std::uint32_t>(result));
        if (!released) return failed(6,handle_value(slot->image.handle));
        leased.emplace(*handle, slot);
        metadata.composition_images.push_back(slot);
        *handle = slot->image.handle;
    }
    return XR_SUCCESS;
}

// Match ownership by application resource and logical eye, even if mapping
// enumeration order changes. A space/layout/configuration transition cannot
// reuse images whose camera packet belongs to the preceding configuration.
[[nodiscard]] bool prepare_held_luke_afw_frame(
    const std::shared_ptr<const HeldAfwPair>& pair,
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    const std::array<XrSwapchain, 2>& handles,
    const std::array<std::uint32_t, 2>& logical_eyes,
    std::uint64_t revision, PreparedProjectionFrame& output) {
    if (!pair || pair->revision != revision || mappings.size() != 2 ||
        !projection_resource_layout_compatible(pair->projection, snapshot)) return false;
    for (std::size_t layer = 0; layer < snapshot.layers.size(); ++layer) {
        if (pair->projection.layers[layer].space != snapshot.layers[layer].space ||
            pair->projection.layers[layer].layer_flags != snapshot.layers[layer].layer_flags)
            return false;
    }
    if (logical_eyes[0] > 1 || logical_eyes[1] > 1 || logical_eyes[0] == logical_eyes[1])
        return false;
    std::vector<PreparedProjectionResource> resources;
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        const auto found = std::find_if(pair->resources.begin(), pair->resources.end(),
            [&](const auto& resource) {
                return resource.application_swapchain == mappings[index].application_swapchain &&
                    resource.output_swapchain == handles[index] && handles[index] != XR_NULL_HANDLE &&
                    resource.logical_eye == logical_eyes[index];
            });
        if (found == pair->resources.end()) return false;
        PreparedGeneration held{};
        held.kind = PreparedGenerationKind::prime;
        held.reason = GenerationPrepareReason::ready;
        held.current_handle = found->output_swapchain;
        held.anchor_is_current = true;
        resources.push_back({found->application_swapchain, held});
    }
    output.kind = PreparedGenerationKind::prime;
    output.reason = GenerationPrepareReason::ready;
    output.anchor_is_current = true;
    output.resources = std::move(resources);
    output.held_afw_pair = pair;
    output.scene_serial=pair->production_serial;
    return true;
}

[[nodiscard]] bool build_afw_terminal_submission(const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, const PreparedProjectionFrame& produced,
    GeneratedFrameEndInfo& terminal, ProjectionSnapshot& terminal_snapshot) {
    std::vector<ProjectionResourceDestination> destinations;
    for (const auto& resource : produced.resources)
        destinations.push_back({resource.application_swapchain, resource.generation.current_handle});
    if (produced.kind == PreparedGenerationKind::none ||
        !build_generated_frame_end_info(source, snapshot, false, destinations, &terminal))
        return false;
    if (produced.held_afw_pair) {
        const auto& owned = produced.held_afw_pair->projection;
        if (!projection_resource_layout_compatible(owned, snapshot)) return false;
        for (std::size_t layer = 0; layer < terminal.projections.size(); ++layer) {
            auto& copy = terminal.projections[layer];
            const auto& saved = owned.layers[layer];
            if (copy.layer.space != saved.space || copy.layer.layerFlags != saved.layer_flags)
                return false;
            for (std::size_t view = 0; view < copy.views.size(); ++view) {
                copy.views[view].pose = saved.views[view].pose;
                copy.views[view].fov = saved.views[view].fov;
            }
        }
        // displayTime names this physical presentation interval; the retained
        // render pose/FOV still describe the pixels generated on the old one.
    }
    terminal.interface_stamp.scene_owner=0x4146570000000001ULL;
    terminal.interface_stamp.scene_serial=produced.scene_serial;
    const bool captured = capture_projection_snapshot(&terminal.info, &terminal_snapshot);
    terminal_snapshot.blackbar_pose=snapshot.blackbar_pose;
    terminal_snapshot.interface_stamp = terminal.interface_stamp;
    return captured;
}

[[nodiscard]] std::shared_ptr<const CompletedAfwFrame> freeze_completed_afw_frame(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, std::vector<PostAfwFrameInput> inputs,
    std::uint64_t application_serial) {
    const auto failed = [&](std::int64_t reason, std::uint64_t detail = 0)
        -> std::shared_ptr<const CompletedAfwFrame> {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            reason, 53, application_serial, detail);
        return {};
    };
    if (inputs.size() != 2) return failed(1, inputs.size());
    // The exact-output probe must not silently strip an extension chain while
    // owning metadata. Such a packet is unsupported until that chain can be
    // deep-copied; normal AFW-only delivery remains unchanged.
    if (!source || source->next || (source->layerCount && !source->layers)) return failed(8);
    for (std::uint32_t i = 0; i < source->layerCount; ++i) {
        const auto* header = source->layers[i];
        if (!header || header->next) return failed(8, i);
        if (header->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(header);
        if (!projection->views) return failed(8, i);
        for (std::uint32_t v = 0; v < projection->viewCount; ++v)
            if (projection->views[v].next) return failed(8, (static_cast<std::uint64_t>(i) << 32) | v);
    }
    std::vector<ProjectionResourceDestination> destinations;
    for (const auto& input : inputs) {
        if (!input.generation || !input.capture.fence_value || input.views.empty() ||
            input.completed_swapchain == XR_NULL_HANDLE) return failed(2, input.capture.fence_value);
        // Source is already the ONE direct-XR terminal submission. Never
        // reconstruct it from the caller's original AER projection here.
        const bool names_terminal_image = std::any_of(snapshot.layers.begin(), snapshot.layers.end(),
            [&](const auto& layer) { return std::any_of(layer.views.begin(), layer.views.end(),
                [&](const auto& view) { return view.subImage.swapchain == input.completed_swapchain; }); });
        if (!names_terminal_image) return failed(4, handle_value(input.completed_swapchain));
        destinations.push_back({input.completed_swapchain, input.completed_swapchain});
    }
    // Classify auxiliary chains BEFORE requiring their retained release image.
    // Enumeration may provision generation on a color chain later used as a
    // quad. Its release capture is skipped until composition_referenced is set.
    // Checking only after a successful AFW freeze creates a permanent cold-start
    // loop: freeze needs the capture, but no future release knows to produce it.
    // This preflight is common to inline and director consumers; AFW-only never
    // enters this OFXR ownership path.
    if (!composition_can_be_owned(state, source)) return failed(3);
    GeneratedFrameEndInfo submission;
    if (!build_generated_frame_end_info(source, snapshot, false, destinations, &submission)) return failed(4);
    auto packet = std::make_shared<CompletedAfwFrame>();
    packet->submission = make_presenter_owned_frame(std::move(submission));
    if (!packet->submission) return failed(5);
    if (!capture_projection_snapshot(&packet->submission->info, &packet->snapshot)) return failed(6);
    packet->snapshot.blackbar_pose=snapshot.blackbar_pose;
    packet->snapshot.interface_stamp = packet->submission->interface_stamp;
    // Async consumers must own auxiliary colors. A proven SteamVR/D11 XR
    // ceiling instead keeps both physical submissions inside the application's
    // EndFrame, before it can release a new quad image. The session cannot
    // promote again; never send these borrowed handles to a worker/director.
    const bool inline_auxiliary=state->luke_aux_budget_inline && !continuous_presenter_active(state);
    if (!inline_auxiliary) {
        XrResult refusal=XR_SUCCESS;
        const XrResult owned=own_composition_images(state,*packet->submission,&refusal);
        if (XR_FAILED(owned)) {
            if (refusal==XR_ERROR_LIMIT_REACHED && state->dispatch->steamvr_runtime &&
                state->graphics_binding==SessionGraphicsBinding::d3d11 && state->luke_afw_enabled &&
                !state->luke_aux_budget_inline) {
                state->luke_aux_budget_inline=true;
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    610,handle_value(state->handle),static_cast<std::uint64_t>(static_cast<std::int64_t>(refusal)),
                    continuous_presenter_active(state)?1:0);
                // Rebuild from original metadata: a multi-quad attempt can
                // have leased/replaced earlier layers before the refusal.
                packet.reset();
                if (!continuous_presenter_active(state))
                    return freeze_completed_afw_frame(state,source,snapshot,std::move(inputs),application_serial);
            }
            return failed(7,static_cast<std::uint64_t>(owned));
        }
    }
    packet->inputs = std::move(inputs);
    packet->application_serial = application_serial;
    log_luke_projection_poses(&packet->submission->info, application_serial, 10);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
        0, 52, application_serial, packet->inputs.front().capture.serial);
    return packet;
}

[[nodiscard]] XrResult handoff_three_slot_frame(
    const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* source,
    const ProjectionSnapshot& snapshot, std::span<const ProjectionResourceMapping> mappings,
    bool metadata_pairable,
    std::shared_ptr<const CompletedAfwFrame> post_afw_frame = {}) {
    if (!metadata_pairable) {
        const XrResult drain = wait_for_presenter_idle(state);
        if (XR_FAILED(drain)) return drain;
        std::scoped_lock lock(state->presenter_mutex);state->director_anchor_sequence = 0;
    }
    if (post_afw_frame && state->luke_aux_budget_inline)
        return XR_ERROR_LAYER_INVALID; // synchronous-only packet cannot escape its EndFrame
    // Copy metadata now; auxiliary image ownership is acquired after capacity
    // admission, before making this frame visible to either consumer.
    std::vector<ProjectionResourceDestination> identities;
    for (const auto& mapping : mappings)
        identities.push_back({mapping.application_swapchain, mapping.application_swapchain});
    GeneratedFrameEndInfo metadata;
    if (!post_afw_frame && !build_generated_frame_end_info(source, snapshot, false, identities, &metadata))
        return XR_ERROR_LAYER_INVALID;
    auto job = std::make_shared<SynthesisJob>();
    // AFW color is already the final direct-XR output. Do not apply the
    // game's presentation transform a second time in the OFXR consumer.
    job->presentation_transform = post_afw_frame
        ? xrfg::D3D12PresentationTransform{} : state->presentation_transform;
    job->metadata = post_afw_frame ? post_afw_frame->submission
        : make_presenter_owned_frame(std::move(metadata));
    if (!job->metadata) return XR_ERROR_LAYER_INVALID;
    job->snapshot = post_afw_frame ? post_afw_frame->snapshot : snapshot;
    std::optional<ProjectionSnapshot> previous;
    { std::scoped_lock lock(state->mutex); previous=state->previous_projection; }
    job->fraction=synthetic_interpolation_fraction(state,previous,job->snapshot);
    job->extra_fraction=synthetic_interpolation_fraction(state,previous,job->snapshot,1);
    if (previous && xrfg::autonomous::r19_camera_enabled.load(std::memory_order_acquire)) {
        job->synthetic_camera=synthetic_camera_snapshot(*previous,job->snapshot,job->fraction);
        job->extra_camera=synthetic_camera_snapshot(*previous,job->snapshot,job->extra_fraction);
    }
    job->mappings.assign(mappings.begin(), mappings.end());
    job->post_afw_frame = std::move(post_afw_frame);
    job->current = std::make_shared<ThreeSlotFrame>();
    job->current->queued_at=std::chrono::steady_clock::now();
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::three_slot_buffer_wait, handle_value(state->handle));
    {
        std::unique_lock lock(state->presenter_mutex);
        // The only output-capacity gate: the previous synthetic has gone and
        // only the retained real remains. No input is silently discarded.
        state->presenter_condition.wait(lock, [&] {
            return (!state->synthesis_busy && state->three_slots.can_accept_real()) ||
                   state->presenter_stop_requested || XR_FAILED(state->presenter_failure);
        });
        if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
        if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
        lock.unlock();
        // Capacity admission above also proves the prior worker is finished.
        // Preflight ALL resources before submitting either eye. Extra Releases
        // between EndFrames may advance only one history by more than one:
        // drain the old real once and prime the whole current stereo frame.
        // Keep submit_pair's strict ticket/lease checks and genuine error path.
        if (metadata_pairable) {
            bool capture_gap = false;
            for (const auto& mapping : mappings) {
                const auto chain = find_swapchain(mapping.application_swapchain);
                if (!chain) continue; // normal preparation reports invalid state
                std::shared_ptr<FrameGenerationSwapchainState> generation;
                std::optional<xrfg::D3D12HistoryCaptureTicket> capture;
                {
                    std::scoped_lock source_lock(chain->mutex);
                    generation = chain->frame_generation;
                    capture = chain->last_released_capture;
                }
                capture_gap |= (!chain->presentation_transform_known && job->presentation_transform.active) ||
                    (chain->presentation_transform_known && chain->presentation_transform_active != job->presentation_transform.active);
                if (!job->post_afw_frame) {
                    capture_gap |= generation && generation->synthesizer && capture &&
                        generation->synthesizer->has_capture_gap(*capture);
                } else {
                    const auto& inputs = job->post_afw_frame->inputs;
                    const auto found = std::find_if(inputs.begin(), inputs.end(),
                        [&](const auto& input) { return input.application_swapchain == mapping.application_swapchain; });
                    capture_gap |= found == inputs.end() || !generation ||
                        !generation->afw_temporal_synthesizer ||
                        generation->afw_temporal_synthesizer->has_capture_gap(found->capture);
                }
                if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
                    generation && generation->synthesizer &&
                    !generation->synthesizer->can_pair_fsr(chain->last_released_motion_vectors)) {
                    capture_gap = true;
                    xrfg::report_fsr_frame_generation_status(xrfg::FsrFrameGenerationStatus::waiting_for_guides);
                }
            }
            if (capture_gap) {
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::continuity_reset, 2, handle_value(state->handle));
                const XrResult drain = wait_for_presenter_idle(state);
                if (XR_FAILED(drain)) return drain;
                { std::scoped_lock anchor_lock(state->presenter_mutex);state->director_anchor_sequence = 0; }
                // An empty director buffer makes job->pair false below;
                // prepare_frame_generation retires/prime-copies BOTH eyes.
            }
        }
        const XrResult composition = job->post_afw_frame ? XR_SUCCESS
            : own_composition_images(state, *job->metadata);
        if (XR_FAILED(composition)) return composition;
        lock.lock();
        if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
        if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING;
        job->pair = single_director_pacing(*state)
            ? metadata_pairable && state->director_anchor_sequence != 0
            : !state->three_slots.empty();
        job->triple=djules_triple_director(*state);
        job->current->source_sequence = state->next_source_sequence++;
        job->current->previous_sequence = job->pair
            ? single_director_pacing(*state) ? state->director_anchor_sequence
                : state->three_slots.slot(0)->value->source_sequence : 0;
        state->director_anchor_sequence = job->current->source_sequence;
        // One atomic handoff starts generation AND makes A eligible for the
        // director. Worker GPU latency does not delay the release of A.
        static_cast<void>(state->three_slots.push_real(job->current));
        state->synthesis_busy = true;
        state->synthesis_job = job;
    }
    xrfg::bridge_flight_logger().end(token, xrfg::BridgeFlightOperation::three_slot_buffer_wait,
        XR_SUCCESS, job->current->source_sequence, job->current->previous_sequence);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::three_slot_handoff,
        job->pair ? 1 : 0, job->current->source_sequence, job->current->previous_sequence,
        job->post_afw_frame ? job->post_afw_frame->application_serial : 0);
    if (state->frame_capture && state->frame_capture->active()) { try {
        state->frame_capture->record("{\"kind\":\"handoff\",\"app_frame\":"+std::to_string(state->diagnostic_app_frame)+
            ",\"qpc\":"+std::to_string(xrfg::FrameCapture::qpc())+",\"source_id\":"+std::to_string(job->current->source_sequence)+
            ",\"previous_id\":"+std::to_string(job->current->previous_sequence)+"}");
    } catch (...) {} }
    state->presenter_condition.notify_all();
    return XR_SUCCESS;
}


struct LukeOriginHistoryProbe {
    std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
    xrfg::D3D12HistoryCaptureTicket capture;
};
// Only a native reset uses this preflight. Pin the two exact released captures
// before reading CRCs, without acquiring or changing any XR destination image.
[[nodiscard]] HRESULT read_luke_capture_origins(ID3D12CommandQueue* queue,
    const std::array<LukeOriginHistoryProbe,2>& inputs,
    std::array<std::uint32_t,2>& tokens,std::array<HRESULT,2>& results,
    bool& poisoned) noexcept {
    std::array<xrfg::D3D12HistoryConsumerLease,2> leases{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> resources;
    std::array<Microsoft::WRL::ComPtr<ID3D12Fence>,2> producers;
    unsigned acquired{};
    const auto cancel=[&] {for(unsigned i=0;i<acquired;++i)inputs[i].history->cancel_consumer(leases[i]);};
    if(!queue)return E_POINTER;
    for(unsigned i=0;i<2;++i) {
        if(!inputs[i].history) {cancel();return E_POINTER;}
        const auto hr=inputs[i].history->acquire_consumer(inputs[i].capture,&leases[i],
            resources[i].GetAddressOf(),producers[i].GetAddressOf());
        if(FAILED(hr)) {cancel();return hr;}
        ++acquired;
        const auto wait=queue->Wait(producers[i].Get(),inputs[i].capture.fence_value);
        if(FAILED(wait)) {cancel();return wait;}
    }
    xrfg::LukeColorOriginReadReceipt receipt;
    const auto hr=xrfg::read_luke_color_origin_pair(queue,{resources[0].Get(),resources[1].Get()},
        tokens,results,D3D12_RESOURCE_STATE_COMMON,250,nullptr,&receipt);
    if(receipt.fence&&receipt.value) {
        for(unsigned i=0;i<2;++i)if(FAILED(inputs[i].history->retire_consumer(leases[i],receipt.fence.Get(),receipt.value)))poisoned=true;
    } else if(!receipt.submitted)cancel();
    else poisoned=true; // Submitted without a fence: quarantine leases until session teardown.
    return poisoned?E_FAIL:hr;
}

// V23189 deliberately owns the whole two-eye transaction. Both private
// current images are populated first, AFW replaces only the peer, and only then
// are the D3D11 OpenXR images published/released.  This prevents the first eye
// from escaping before the cross-swapchain AFW write has been queued.
[[nodiscard]] PreparedProjectionFrame prepare_luke_afw_frame(
    const std::shared_ptr<SessionState>& session,
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings,
    std::optional<std::uint32_t> released_eye,
    std::uint64_t frame_serial,
    bool capture_temporal_input,
    std::vector<PostAfwFrameInput>& completed_inputs,
    const XrFrameEndInfo* composition=nullptr) noexcept {
    PreparedProjectionFrame output{};
    struct Entry {
        std::shared_ptr<SwapchainState> swapchain;
        std::shared_ptr<FrameGenerationSwapchainState> generation;
        std::vector<xrfg::D3D12ReprojectionView> views;
        PreparedGeneration prepared;
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> guide;
        xrfg::D3D12HistoryCaptureTicket afw_final_capture{};
        xrfg::D3D11PublicationReceipt publication;
        std::optional<AfwSourceCapture> source_capture;
        std::uint32_t logical_eye{std::numeric_limits<std::uint32_t>::max()};
    };
    std::array<Entry, 2> entries{};
    static const bool gta=[] {wchar_t p[MAX_PATH]{};GetModuleFileNameW(nullptr,p,MAX_PATH);
        const auto* n=wcsrchr(p,L'\\');return !_wcsicmp(n?n+1:p,L"GTA5_Enhanced.exe");}();
    const auto flat_quad=[&]() -> bool {
        if(!gta||!composition||!composition->layers)return false;
        bool found{};
        for(unsigned i=0;i<composition->layerCount;++i) {
            const auto* base=composition->layers[i];if(!base||base->type!=XR_TYPE_COMPOSITION_LAYER_QUAD)continue;
            const auto& q=*reinterpret_cast<const XrCompositionLayerQuad*>(base);
            if(q.eyeVisibility!=XR_EYE_VISIBILITY_BOTH||q.subImage.imageArrayIndex||q.subImage.imageRect.offset.x||q.subImage.imageRect.offset.y)continue;
            auto chain=find_swapchain(q.subImage.swapchain);if(!chain||chain->session!=session)continue;
            std::scoped_lock lock(chain->mutex);
            if(chain->create_info.width!=std::uint32_t(q.subImage.imageRect.extent.width)||
                chain->create_info.height!=std::uint32_t(q.subImage.imageRect.extent.height)||
                chain->create_info.width<1024)continue;
            // This is a passthrough classification, not an origin/guide or
            // FPS receipt. A game effect may erase the quad marker as well.
            found=true;
        }
        return found;
    };
    // Diagnostic-only stages 28/29: missing-guide subreason and the age of
    // the selected eye's NGX publication relative to the newest publication.
    const auto report_missing = [](std::int64_t reason,
        std::uint64_t detail) noexcept {
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_afw_stage,
            reason, 28, detail, 0);
    };
    const auto release_deferred = [&]() noexcept {
        for (Entry& entry : entries) {
            if (!entry.prepared.publication_deferred || !entry.generation)
                continue;
            static_cast<void>(release_private_image(
                session.get(), session->dispatch, entry.generation->current[kAfwScratchSlot]));
            entry.prepared.publication_deferred = false;
        }
    };
    try {
        if(gta&&session) {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_origin_loss_capture.set_logging(xrfg::bridge_flight_logger().enabled());
        }
        if (!session || !session->luke_afw_enabled || !session->luke_afw ||
            (session->graphics_binding_capabilities & 8ULL) == 0 ||
            mappings.size() != 2) {
            output.reason = flat_quad()?GenerationPrepareReason::luke_flat_scene:GenerationPrepareReason::missing_generation;
            return output;
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const ProjectionResourceMapping& mapping = mappings[index];
            if (mapping.views.size() != 1) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
            Entry& entry = entries[index];
            entry.swapchain = find_swapchain(mapping.application_swapchain);
            const auto views = build_reprojection_views(snapshot, mapping);
            if (!entry.swapchain || !views || views->size() != 1) {
                output.reason = GenerationPrepareReason::reprojection_views_failed;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
            entry.views = *views;
            {
                std::scoped_lock lock(entry.swapchain->mutex);
                entry.generation = entry.swapchain->frame_generation;
                if (entry.swapchain->last_released_capture)
                    entry.source_capture = AfwSourceCapture{
                        entry.generation, *entry.swapchain->last_released_capture};
                if (entry.swapchain->dlss_logical_eye_index)
                    entry.logical_eye = *entry.swapchain->dlss_logical_eye_index;
            }
            if (!entry.generation || !entry.generation->d3d11_interop ||
                entry.logical_eye > 1) {
                report_missing(1, index);
                output.reason = GenerationPrepareReason::missing_capture;
                output.failed_swapchain = mapping.application_swapchain;
                release_deferred();
                return output;
            }
        }

        const auto afw_status = xrfg::embedded::afw_snapshot();
        const auto hold_last_pair = [&]() -> bool {
            std::shared_ptr<const HeldAfwPair> pair;
            {
                std::scoped_lock guide_lock(session->luke_guide_mutex);
                pair = session->luke_afw_last_pair;
            }
            if (!prepare_held_luke_afw_frame(pair, snapshot, mappings,
                    {entries[0].generation->current[kAfwScratchSlot].handle,
                     entries[1].generation->current[kAfwScratchSlot].handle},
                    {entries[0].logical_eye, entries[1].logical_eye},
                    afw_status.revision, output)) return false;
            // No new AFW color was produced. Keep both image ownership and
            // camera metadata, while leaving the post-AFW temporal anchor intact.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                1, 33, afw_status.revision, pair->production_serial);
            return true;
        };
        if(gta&&!released_eye) {
            if(flat_quad()) {
                output.reason=GenerationPrepareReason::luke_flat_scene;
                release_deferred();return output;
            }
        }
        if (!released_eye) {
            if (hold_last_pair()) return output;
        }

        if (!released_eye || *released_eye > 1 ||
            entries[0].logical_eye == entries[1].logical_eye) {
            report_missing(2,
                (static_cast<std::uint64_t>(released_eye.value_or(255)) << 32) |
                (static_cast<std::uint64_t>(entries[0].logical_eye) << 16) |
                entries[1].logical_eye);
            output.reason = GenerationPrepareReason::missing_capture;
            release_deferred();
            return output;
        }
        // A cut resets native N-1/N-2 history, independently of barcode loss.
        // Probe only while this accepted stream has a newer native epoch.
        // A positive CRC naming native serial1..3 keeps the already published
        // stereo pair; never synthesize from old-epoch depth or motion vectors.
        if(!session->luke_warmup_probe_poisoned) {
            std::shared_ptr<const HeldAfwPair> pair;
            {std::scoped_lock lock(session->luke_guide_mutex);pair=session->luke_afw_last_pair;}
            const bool reset=pair&&(xrfg::luke_native_stream_reset(pair->native_domains[0].stream,pair->native_domains[0].epoch,session->d3d12_device.Get()) ||
                ((pair->native_domains[1].stream!=pair->native_domains[0].stream || pair->native_domains[1].epoch!=pair->native_domains[0].epoch)&&
                 xrfg::luke_native_stream_reset(pair->native_domains[1].stream,pair->native_domains[1].epoch,session->d3d12_device.Get())));
            if(reset) {
                const auto now=GetTickCount64();
                if(session->luke_warmup_pair!=pair->production_serial) {
                    session->luke_warmup_pair=pair->production_serial;session->luke_warmup_started=0;
                }
                if(!session->luke_warmup_started ||
                    (now>=session->luke_warmup_started&&now-session->luke_warmup_started<=250)) {
                    std::array<LukeOriginHistoryProbe,2> inputs;
                    for(unsigned i=0;i<2;++i) {
                        std::scoped_lock lock(entries[i].swapchain->mutex);
                        inputs[i].history=entries[i].swapchain->d3d12_history;
                        if(entries[i].source_capture)inputs[i].capture=entries[i].source_capture->capture;
                    }
                    std::array<std::uint32_t,2> tokens{};std::array<HRESULT,2> results{};
                    const auto probe=read_luke_capture_origins(session->d3d12_queue.Get(),inputs,tokens,results,
                        session->luke_warmup_probe_poisoned);
                    if(SUCCEEDED(probe))for(unsigned i=0;i<2;++i)if(entries[i].logical_eye==*released_eye&&
                        SUCCEEDED(results[i])&&SUCCEEDED(results[1-i])&&tokens[1-i]&&
                        std::uint64_t(tokens[1-i])+1==tokens[i]) {
                        const auto origin=xrfg::luke_native_origin_metadata(tokens[i],session->d3d12_device.Get());
                        const bool new_epoch=origin&&std::any_of(pair->native_domains.begin(),pair->native_domains.end(),[&](const auto& domain) {
                            return origin->stream==domain.stream&&domain.epoch&&origin->epoch>domain.epoch;
                        });
                        if(new_epoch&&hold_last_pair()) {
                            if(!session->luke_warmup_started)session->luke_warmup_started=now;
                            session->luke_afw->prepare_history_reset();
                            const bool renderer_ready=session->luke_afw->history_reset_ready();
                            bool native_warming=origin->serial<=3;
                            if(auto guide=xrfg::resolve_luke_guide_token(tokens[i],session->d3d12_queue.Get(),session->d3d12_device.Get())) {
                                xrfg::LukeNativeMotionConstants constants{};unsigned failure{};
                                const bool ready=xrfg::luke_native_motion_constants(*guide,0,0,constants,failure);
                                native_warming=!ready&&(failure==1||failure==3);
                            }
                            if(native_warming||!renderer_ready) {
                                session->luke_warmup_held=true;
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                                    origin->serial,1220,origin->publication,origin->epoch);
                                return output;
                            }
                            output={}; // a genuinely ready new scene proceeds through normal admission
                        }
                    }
                }
            }
        }
        std::optional<float> xr_eye_baseline;
        {std::scoped_lock lock(session->mutex);xr_eye_baseline=session->luke_xr_eye_baseline;}
        if(!xr_eye_baseline) {report_missing(7,0);output.reason=GenerationPrepareReason::missing_capture;return output;}
        std::uint64_t last_evaluated{};
        {std::scoped_lock lock(session->luke_guide_mutex);
            last_evaluated=session->luke_afw_last_evaluated_publication;
            session->luke_afw_last_pair.reset();}
        std::array<std::uint64_t,2> publications{};
        std::array<std::uint32_t,2> origin_tokens{};
        // Each color is frozen by its own history ticket. Copy into disjoint
        // AFW scratch, then resolve only the token actually present in it.
        for(std::size_t index=0;index<entries.size();++index) {
            auto& entry=entries[index];
            if(!entry.source_capture) {output.reason=GenerationPrepareReason::missing_capture;release_deferred();return output;}
            entry.prepared=prepare_frame_generation(mappings[index].application_swapchain,false,
                std::span<const xrfg::D3D12ReprojectionView>(entry.views.data(),entry.views.size()),
                0.5f,false,nullptr,true,nullptr,&*entry.source_capture);
            if(entry.prepared.kind!=PreparedGenerationKind::prime||!entry.prepared.publication_deferred) {
                output.reason=entry.prepared.reason;output.failed_swapchain=mappings[index].application_swapchain;
                release_deferred();return output;
            }
        }
        std::array<ID3D12Resource*,2> origin_colors{};
        std::array<HRESULT,2> origin_results{};
        std::array<bool,2> origin_nonblack{};
        for(std::size_t index=0;index<entries.size();++index) {
            auto& entry=entries[index];const auto images=entry.generation->d3d11_interop->current_destination_images();
            if(entry.prepared.current_destination_index>=images.size()) {
                output.reason=GenerationPrepareReason::invalid_private_swapchain;release_deferred();return output;
            }
            origin_colors[index]=images[entry.prepared.current_destination_index];
        }
        const auto read_origins = [&] {
            // Both colors are already frozen in disjoint scratch on this queue.
            // Submit their small marker copies together: one GPU/CPU rendezvous,
            // independent tokens/results and the same producer ordering. No new
            // frame of history or delayed eye selection is introduced.
            xrfg::read_luke_color_origin_pair(session->d3d12_queue.Get(),
                origin_colors,origin_tokens,origin_results,D3D12_RESOURCE_STATE_COMMON,250,
                (gta||xrfg::bridge_flight_logger().enabled())?&origin_nonblack:nullptr);
        };
        if (entries[0].generation->isolated_luke_synthesis && entries[1].generation->isolated_luke_synthesis) {
            // The reader owns its allocator/readback lock. Its CPU decode must
            // not exclude OFXR submission on the other lane. Scratch stays on
            // the capture queue and is never an OFXR destination.
            read_origins();
        } else {
            std::scoped_lock lock(session->gpu_mutex);
            read_origins();
        }
        output.original_scene=xrfg::luke_original_scene_identity(
            {SUCCEEDED(origin_results[0])?origin_tokens[0]:0,
             SUCCEEDED(origin_results[1])?origin_tokens[1]:0},
            {entries[0].logical_eye,entries[1].logical_eye});
        for(std::size_t index=0;index<entries.size();++index) {
            auto& entry=entries[index];const HRESULT result=origin_results[index];
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                result,989+entry.logical_eye,frame_serial,origin_tokens[index]);
            if(FAILED(result)||!origin_tokens[index])continue;
            entry.guide=xrfg::resolve_luke_guide_token(origin_tokens[index],
                session->d3d12_queue.Get(),session->d3d12_device.Get());
            if(entry.guide&&entry.guide->camera_data_valid&&entry.guide->previous_opposite_guide&&
                entry.guide->previous_opposite_guide->camera_data_valid)
                publications[index]=entry.guide->publication;
        }
        bool sequence_continued=false;
        bool force_afw_reset=false;
        std::optional<xrfg::LukeOriginSequencePrediction> sequence_prediction;
        const auto sequence_now=GetTickCount64();
        std::array<std::uint64_t,2> sequence_captures{},sequence_contexts{};
        for(const auto& entry:entries)if(entry.source_capture&&entry.logical_eye<2) {
            sequence_captures[entry.logical_eye]=entry.source_capture->capture.serial;
            sequence_contexts[entry.logical_eye]=reinterpret_cast<std::uintptr_t>(entry.generation.get());
        }
        const auto sequence_stamp=[&](const xrfg::DlssMotionVectorFrame& guide,unsigned eye) {
            xrfg::LukeOriginSequenceFrame f;f.stream=guide.stream;f.epoch=guide.epoch;
            f.publication=guide.publication;f.native=guide.native_production_sequence?guide.native_production_sequence:(guide.camera_frame_token_valid?guide.camera_frame_token:0);
            f.owner=guide.gta_headlock?guide.gta_headlock->owner:reinterpret_cast<std::uintptr_t>(session->d3d12_device.Get());f.token=guide.color_origin_token;f.eye=eye;
            f.captures=sequence_captures;f.contexts=sequence_contexts;
            f.shape={guide.output_width,guide.output_height,guide.motion_width,guide.motion_height};return f;
        };
        if(released_eye&&*released_eye<2) {
            for(const auto& entry:entries)if(entry.logical_eye==*released_eye&&entry.guide&&
                origin_tokens[&entry-entries.data()]==entry.guide->color_origin_token) {
                const auto index=std::size_t(&entry-entries.data());
                const auto other=1-index;
                const bool complete=origin_tokens[other]&&
                    std::uint64_t(origin_tokens[other])+1==origin_tokens[index];
                force_afw_reset=session->luke_origin_continuity.confirm(
                    sequence_stamp(*entry.guide,entry.logical_eye),complete,sequence_now)||session->luke_origin_resync_pending;
                if(force_afw_reset) {
                    std::scoped_lock guide_lock(session->luke_guide_mutex);
                    session->luke_afw_last_evaluated_publication=0;
                    session->luke_afw_last_guide_publication={};session->luke_afw_last_pair.reset();last_evaluated=0;
                    session->luke_origin_resync_pending=false;
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        1,1206,entry.guide->publication,origin_tokens[index]);
                }
            }
        }
        auto selected=xrfg::select_luke_afw_color_source(publications,last_evaluated);
        if(!selected&&released_eye&&*released_eye<2) {
            const auto missing=HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            bool ambiguous=false;std::optional<std::size_t> current_index;
            for(std::size_t i=0;i<entries.size();++i) {
                if(entries[i].logical_eye==*released_eye)current_index=i;
                ambiguous|=origin_results[i]==HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            if(ambiguous)session->luke_origin_resync_pending|=session->luke_origin_continuity.invalidate();
            if(!ambiguous&&current_index&&origin_results[*current_index]==missing&&!origin_tokens[*current_index]) {
                unsigned sequence_refusal{};
                sequence_prediction=session->luke_origin_continuity.predict(sequence_now,*released_eye,
                    sequence_captures,sequence_contexts,&sequence_refusal);
                if(!sequence_prediction) {
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        sequence_refusal,1208,frame_serial,last_evaluated);
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        session->luke_origin_continuity.confirmations(*released_eye),1209,frame_serial,
                        session->luke_origin_continuity.age(*released_eye,sequence_now));
                }
                if(sequence_prediction) {
                    bool compatible=true;
                    for(std::size_t i=0;i<entries.size();++i)if(origin_tokens[i]) {
                        const auto token=sequence_prediction->frame.token-(entries[i].logical_eye!=*released_eye?1U:0U);
                        compatible&=origin_tokens[i]==token;
                    }
                    if(!compatible) {
                        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                            7,1208,frame_serial,sequence_prediction->frame.token);
                        session->luke_origin_resync_pending|=session->luke_origin_continuity.invalidate();
                    }
                    else {
                        auto guide=xrfg::resolve_luke_guide_token(sequence_prediction->frame.token,
                            session->d3d12_queue.Get(),session->d3d12_device.Get());
                        if(!guide)xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                            5,1208,frame_serial,sequence_prediction->frame.token);
                        if(guide) {
                            if(!xrfg::LukeOriginContinuity::matches(sequence_prediction->frame,
                                sequence_stamp(*guide,*released_eye))) {
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                                    6,1208,frame_serial,sequence_prediction->frame.token);
                                session->luke_origin_resync_pending|=session->luke_origin_continuity.invalidate();
                            }
                            else if(guide->camera_data_valid&&guide->previous_opposite_guide&&
                                guide->previous_opposite_guide->camera_data_valid&&guide->publication>last_evaluated) {
                                const auto source=*current_index,peer=1-source;
                                entries[source].guide=guide;
                                entries[peer].guide=guide->previous_opposite_guide;
                                selected=source;sequence_continued=true;
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                                    *released_eye,1204,guide->publication,sequence_prediction->frame.token);
                                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                                    *released_eye,1205,frame_serial,sequence_prediction->age_ms);
                                xrfg::two_frame_capture_timeline("origin_continuity",*released_eye,
                                    guide->publication,sequence_prediction->age_ms,sequence_prediction->frame.token);
                            }
                        }
                    }
                }
            }
        }
        if (xrfg::bridge_flight_logger().enabled()) {
            static const bool gta=[] {
                wchar_t path[MAX_PATH]{};GetModuleFileNameW(nullptr,path,MAX_PATH);
                const auto* name=wcsrchr(path,L'\\');
                return !_wcsicmp(name?name+1:path,L"GTA5_Enhanced.exe");
            }();
            static const bool automatic_capture=GetPrivateProfileIntW(L"diagnostics",L"gta_auto_capture",1,
                (current_layer_directory()/L"ofxr_bridge.ini").c_str())!=0;
            if (gta && automatic_capture) {
                const bool has_token=origin_tokens[0]||origin_tokens[1];
                const bool fresh_stereo=released_eye&&*released_eye<2&&
                    entries[0].source_capture&&entries[1].source_capture;
                // A black compact read ROI does not prove a black full image.
                // Freeze the whole original colour to distinguish a letterbox
                // from an actually empty projection on the next loss witness.
                const bool diagnostic_scene=has_token||origin_nonblack[0]||origin_nonblack[1]||fresh_stereo;
                // Capture failed AFW source admission AFTER the bounded bonus.
                // A rescued missing CRC is not the unavailable state the owner
                // sees. F9 can also arm this while failure is already present.
                const bool afw_ready=selected.has_value();
                const auto now=GetTickCount64();
                bool witness{};unsigned missing_count{};
                {std::scoped_lock guide_lock(session->luke_guide_mutex);
                    auto& loss=session->luke_origin_loss_capture;
                    if(loss.observe(afw_ready,diagnostic_scene,now)&&xrfg::two_frame_capture_request_origin_loss()) {
                        loss.captured(now);witness=true;missing_count=loss.missing;
                    }
                }
                if (witness) {
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                        0,1190,frame_serial,missing_count);
                    xrfg::two_frame_capture_timeline("afw_unavailable_after_bonus",5,frame_serial,
                        composition?composition->displayTime:0,session->luke_origin_continuity.age(released_eye.value_or(2),now));
                    log_luke_projection_poses(composition,frame_serial,12);
                    // Freeze the actual submitted quad in THIS failing EndFrame,
                    // not the next release, so a pause/menu cannot be confused
                    // with a later scripted scene from neighbouring frames.
                    if(composition&&composition->layers)for(unsigned layer=0;layer<composition->layerCount;++layer) {
                        const auto* base=composition->layers[layer];
                        if(!base||base->type!=XR_TYPE_COMPOSITION_LAYER_QUAD)continue;
                        const auto& quad=*reinterpret_cast<const XrCompositionLayerQuad*>(base);
                        auto chain=find_swapchain(quad.subImage.swapchain);
                        if(!chain||chain->session!=session||
                            (chain->create_info.createFlags&XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT))continue;
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;std::uint32_t index{};
                        {std::scoped_lock chain_lock(chain->mutex);
                            if(!chain->last_released_index||*chain->last_released_index>=chain->enumerated_d3d11_images.size())continue;
                            index=*chain->last_released_index;texture=chain->enumerated_d3d11_images[index];}
                        if(!texture||!xrfg::two_frame_capture_claim_application(handle_value(chain->handle)))continue;
                        xrfg::TwoFrameImageInfo image{};image.stage="na_application_quad";
                        image.swapchain=handle_value(chain->handle);image.capture_serial=frame_serial;
                        image.display_time=composition->displayTime;image.slice=quad.subImage.imageArrayIndex;
                        image.width=chain->create_info.width;image.height=chain->create_info.height;
                        const auto captured=xrfg::two_frame_capture_record_d3d11(image,session->d3d12_device.Get(),
                            session->d3d11_context.Get(),texture.Get());
                        xrfg::two_frame_capture_timeline("na_application_quad_result",captured,frame_serial,
                            composition->displayTime,(std::uint64_t(layer)<<32)|index);
                    }
                    // These disjoint mirrors are the exact frozen colors just
                    // read above. Copy NOW, before release/reuse: starting on
                    // the next xrEndFrame can miss a one-frame kill effect.
                    for (std::size_t i=0;i<entries.size();++i) {
                        xrfg::two_frame_capture_timeline("origin_loss_crc",origin_results[i],frame_serial,0,
                            (std::uint64_t(entries[i].logical_eye)<<32)|origin_tokens[i]);
                        xrfg::TwoFrameImageInfo image{};
                        image.stage="luke_origin_loss_color";
                        image.capture_serial=frame_serial;
                        image.eye=static_cast<std::int32_t>(entries[i].logical_eye);
                        const auto desc=origin_colors[i]->GetDesc();
                        image.width=static_cast<std::uint32_t>(desc.Width);
                        image.height=desc.Height;
                        const auto captured=xrfg::two_frame_capture_record_queue(image,session->d3d12_device.Get(),session->d3d12_queue.Get(),
                            origin_colors[i],D3D12_RESOURCE_STATE_COMMON);
                        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                            captured,1191+i,frame_serial,origin_tokens[i]);
                    }
                }
            }
        }
        if(gta) {
            // Only classify after the bounded continuation attempt. The
            // nonblack flags describe the marker ROI, NOT the whole projection:
            // a late letterbox can blacken that ROI while native stereo continues.
            // Genuine no-new-stereo quads still take the earlier bypass above.
            if(!selected&&flat_quad()&&!origin_tokens[0]&&!origin_tokens[1]&&
                !origin_nonblack[0]&&!origin_nonblack[1]&&
                origin_results[0]==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)&&
                origin_results[1]==HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
                output.reason=GenerationPrepareReason::luke_flat_scene;
                release_deferred();return output;
            }
        }
        if(!selected) {
            output.reason=GenerationPrepareReason::missing_capture;release_deferred();return output;
        }
        const std::size_t source_index=*selected,destination_index=1-source_index;
        Entry& source=entries[source_index];Entry& destination=entries[destination_index];
        const bool continuation_gap=sequence_continued&&last_evaluated&&source.guide->publication!=last_evaluated+1;
        if(force_afw_reset||continuation_gap) {
            if(continuation_gap)xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                1,1207,source.guide->publication,last_evaluated);
            auto reset=std::make_shared<xrfg::DlssMotionVectorFrame>(*source.guide);
            reset->reset=true;source.guide=std::move(reset);
        }
        const auto origin_token=sequence_continued?source.guide->color_origin_token:origin_tokens[source_index];
        const auto source_images=source.generation->d3d11_interop->current_destination_images();
        const auto destination_images=destination.generation->d3d11_interop->current_destination_images();
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
            source.logical_eye,991,source.guide->publication,last_evaluated);
        const auto ingress = [&](std::uint64_t stage, std::uint64_t value) {
            const auto flags = static_cast<std::int64_t>(source.logical_eye |
                (session->menu_enabled ? 0x100 : 0) |
                (session->nvidia_new_route ? 0x200 : 0) |
                (static_cast<unsigned>(afw_status.desired.mode) << 12));
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                flags, stage, frame_serial, value);
        };
        ingress(71, source.guide->publication);
        ingress(72, source.source_capture->capture.serial);
        ingress(73, source.guide->stream);
        ingress(74, source.guide->epoch);
        ingress(75, source.guide->camera_frame_token_valid
            ? source.guide->camera_frame_token : UINT64_MAX);
        ingress(76, source.guide->previous_opposite_guide
            ? source.guide->previous_opposite_guide->publication : 0);
        ingress(77, source.guide->producer_fence_value);
        ingress(78, make_color_identity(frame_serial, source.source_capture->capture.serial,
            source.swapchain->handle, 0, source.logical_eye, 3, source.views.front()).pose_hash);
        ingress(90, source.guide->publication);
        ingress(79, make_color_identity(frame_serial, 0,
            destination.swapchain->handle, 0, destination.logical_eye, 3, destination.views.front()).pose_hash);
        if (!should_evaluate_luke_afw_publication(
                source.guide->publication, last_evaluated)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                static_cast<std::int64_t>(source.guide->publication),
                34, last_evaluated, source.guide->publication);
            release_deferred();
            output.reason = GenerationPrepareReason::synthesis_stale_guides;
            return output;
        }
        // Never evaluate one original color production twice for this eye.
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            auto& last = session->luke_afw_last_guide_publication[source.logical_eye];
            if (source.guide->publication <= last) {
                report_missing(5,
                    (static_cast<std::uint64_t>(source.logical_eye) << 32) | last);
                output.reason = GenerationPrepareReason::missing_capture;
                release_deferred();
                return output;
            }
            last = source.guide->publication;
        }

        const bool native_peer_camera = source.guide->gta_headlock &&
            source.guide->gta_headlock->active() && destination.guide &&
            source.guide->previous_opposite_guide &&
            source.guide->previous_opposite_guide->publication == destination.guide->publication &&
            source.guide->previous_opposite_guide->stream == destination.guide->stream &&
            source.guide->previous_opposite_guide->epoch == destination.guide->epoch &&
            source.guide->previous_opposite_guide->camera_frame_token == destination.guide->camera_frame_token;
        if(source.guide->gta_headlock)xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_afw_stage,source.guide->gta_headlock->mode,1199,
            source.guide->publication,native_peer_camera);
        HRESULT afw_result = E_UNEXPECTED;
        bool afw_evaluated = false;
        const auto& afw_settings = afw_status.desired;
        {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            afw_result = session->luke_afw->synthesize(
                source_images[source.prepared.current_destination_index],
                destination_images[destination.prepared.current_destination_index],
                *source.guide, source.views.front(), destination.views.front(),
                source.logical_eye, *xr_eye_baseline, afw_settings,native_peer_camera);
            afw_evaluated = SUCCEEDED(afw_result);
        }
        if (afw_evaluated) {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            for (Entry& entry : entries) {
                const auto publish_token = xrfg::bridge_flight_logger().begin(
                    xrfg::BridgeFlightOperation::d3d11_publish,
                    handle_value(entry.swapchain->handle), frame_serial, source.guide->publication);
                afw_result = entry.generation->d3d11_interop->publish(
                    entry.prepared.current_destination_index,
                    std::nullopt, &entry.publication);
                if(SUCCEEDED(afw_result) && session->frame_capture && session->frame_capture->ring_active()) {
                    const auto image=entry.generation->d3d11_interop->published_current_image(entry.publication.current_destination_index);
                    const auto output_handle=entry.generation->current[kAfwScratchSlot].handle;
                    const auto capture=session->frame_capture->source(handle_value(output_handle),image.Get(),
                        entry.publication.current_destination_index,frame_serial,entry.publication.copy_complete_value);
                    xrfg::diagnostic_ring_record("{\"kind\":\"afw_output\",\"qpc\":"+std::to_string(xrfg::diagnostic_ring_qpc())+
                        ",\"capture\":"+std::to_string(capture)+",\"serial\":"+std::to_string(frame_serial)+",\"publication\":"+
                        std::to_string(source.guide->publication)+",\"eye\":"+std::to_string(entry.logical_eye)+",\"swapchain\":\""+
                        std::to_string(handle_value(output_handle))+"\"}");
                }
                xrfg::bridge_flight_logger().end(publish_token,
                    xrfg::BridgeFlightOperation::d3d11_publish, afw_result,
                    handle_value(entry.swapchain->handle), frame_serial, source.guide->publication);
                log_luke_publication_boundary(frame_serial, 1,
                    static_cast<std::size_t>(&entry - entries.data()), entry.swapchain->handle,
                    entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                    entry.generation->d3d11_interop);
                if (FAILED(afw_result)) break;
            }
        }
        if (afw_evaluated && SUCCEEDED(afw_result) &&
            xrfg::two_frame_capture_images_active()) {
            std::scoped_lock gpu_lock(session->gpu_mutex);
            for (const Entry& entry : entries) {
                xrfg::TwoFrameImageInfo terminal{};
                terminal.stage = "afw_xr_terminal_eye";
                terminal.stream = source.guide->stream;
                terminal.capture_serial = source.guide->publication;
                terminal.swapchain = handle_value(entry.generation->current[kAfwScratchSlot].handle);
                terminal.display_time = snapshot.display_time;
                terminal.eye = static_cast<std::int32_t>(entry.logical_eye);
                terminal.width = entry.swapchain->create_info.width;
                terminal.height = entry.swapchain->create_info.height;
                const HRESULT diagnostic = entry.generation->d3d11_interop->
                    diagnostic_capture_published_current(entry.publication, terminal);
                // Diagnostic failures are receipts, never AFW admission failures.
                xrfg::two_frame_capture_timeline("afw_terminal_result", diagnostic,
                    source.guide->publication, snapshot.display_time, entry.logical_eye);
                xrfg::two_frame_capture_timeline("afw_terminal_index", entry.logical_eye,
                    source.guide->publication, snapshot.display_time,
                    entry.publication.current_destination_index);
                xrfg::two_frame_capture_timeline("afw_terminal_fence", entry.logical_eye,
                    source.guide->publication, snapshot.display_time,
                    entry.publication.copy_complete_value);
            }
        }
        if (afw_evaluated && SUCCEEDED(afw_result) && capture_temporal_input) {
            // This is the terminal D3D11 XR output, not the intermediate
            // D3D12 AFW render target. Both eyes have been published. Snapshot
            // the exact acquired images before either XR release; the receipt
            // binds image index and completion independently of mutable cursors.
            std::scoped_lock gpu_lock(session->gpu_mutex);
            for (Entry& entry : entries) {
                if (!entry.generation->afw_final_history) { afw_result = E_UNEXPECTED; break; }
                afw_result = entry.generation->d3d11_interop->capture_published_current(entry.publication);
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->afw_final_history->capture(
                        entry.publication.current_destination_index, &entry.afw_final_capture);
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->d3d11_interop->finish_capture();
                if (SUCCEEDED(afw_result))
                    afw_result = entry.generation->afw_final_history->commit(entry.afw_final_capture);
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    afw_result, 54, frame_serial,
                    (static_cast<std::uint64_t>(entry.publication.current_destination_index) << 32) |
                    entry.logical_eye);
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                    entry.logical_eye, 55, frame_serial, entry.publication.copy_complete_value);
                if (FAILED(afw_result)) break;
                completed_inputs.push_back({entry.swapchain->handle,
                    entry.generation->current[kAfwScratchSlot].handle, entry.generation,
                    entry.afw_final_capture, entry.views,
                    make_color_identity(frame_serial,entry.afw_final_capture.serial,
                        entry.generation->current[kAfwScratchSlot].handle,
                        entry.publication.current_destination_index,entry.logical_eye,0,entry.views.front(),
                        entry.generation->current[kAfwScratchSlot].acquired_index)});
            }
            afw_evaluated = SUCCEEDED(afw_result);
            if (!afw_evaluated) completed_inputs.clear();
        }
        if (afw_evaluated) {
            if(sequence_continued)session->luke_origin_continuity.used(*released_eye);
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_afw_last_evaluated_publication =
                source.guide->publication;
        }
        float motion_scale_x = source.guide->scale_x *
            static_cast<float>(source.guide->output_width) /
            static_cast<float>(source.guide->motion_width);
        float motion_scale_y = source.guide->scale_y *
            static_cast<float>(source.guide->output_height) /
            static_cast<float>(source.guide->motion_height);
        xrfg::embedded::report_afw_evaluation(
            source.guide->publication, source.logical_eye,
            source.guide->motion_width, source.guide->motion_height,
            source.guide->output_width, source.guide->output_height,
            motion_scale_x, motion_scale_y, afw_result);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::synthesis_pair,
            afw_result, 0xAF023291ULL, source.guide->publication,
            (static_cast<std::uint64_t>(source.logical_eye) << 32) |
                destination.logical_eye);
        bool released = true;
        for (Entry& entry : entries) {
            const auto index = static_cast<std::size_t>(&entry - entries.data());
            log_luke_publication_boundary(frame_serial, 2, index, entry.swapchain->handle,
                entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                entry.generation->d3d11_interop);
            released = release_private_image(
                session.get(), session->dispatch, entry.generation->current[kAfwScratchSlot]) && released;
            entry.prepared.publication_deferred = false;
            log_luke_publication_boundary(frame_serial, 3, index, entry.swapchain->handle,
                entry.generation->current[kAfwScratchSlot].handle, entry.generation->synthetic[0].handle,
                entry.generation->d3d11_interop);
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_input_guide,
            source.logical_eye, frame_serial, source.guide->publication, source.guide->epoch);
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_input_camera,
            source.logical_eye, frame_serial, source.guide->publication, dlss_guide_camera_hash(*source.guide));
        if (FAILED(afw_result) || !released) {
            output.reason = released && !afw_evaluated && afw_input_unavailable(afw_result)
                ? GenerationPrepareReason::afw_input_unavailable : FAILED(afw_result)
                ? GenerationPrepareReason::synthesis_failed
                : GenerationPrepareReason::private_release_failed;
            return output;
        }
        auto pair = std::make_shared<HeldAfwPair>();
        pair->projection = snapshot;
        pair->revision = afw_status.revision;
        pair->production_serial = frame_serial;
        pair->native_domains[source.logical_eye]={source.guide->stream,source.guide->epoch,source.guide->publication,source.guide->serial};
        if(source.guide->previous_opposite_guide) {
            const auto& peer=*source.guide->previous_opposite_guide;
            pair->native_domains[1-source.logical_eye]={peer.stream,peer.epoch,peer.publication,peer.serial};
        }
        for (std::size_t index = 0; index < entries.size(); ++index)
            pair->resources[index] = {mappings[index].application_swapchain,
                entries[index].generation->current[kAfwScratchSlot].handle,
                entries[index].logical_eye};
        {
            std::scoped_lock guide_lock(session->luke_guide_mutex);
            session->luke_afw_last_pair = std::move(pair);
        }
        xrfg::two_frame_capture_mark_afw_evaluation();
        xrfg::report_dlss_motion_vector_use();
        output.kind = PreparedGenerationKind::prime;
        output.reason = GenerationPrepareReason::ready;
        output.anchor_is_current = true;
        output.startup_candidate = LukeAfwStartupCandidate{
            {source.guide->stream, source.guide->epoch},
            source.guide->publication};
        output.scene_serial=frame_serial;
        output.resources.reserve(entries.size());
        for (std::size_t index = 0; index < entries.size(); ++index) {
            output.resources.push_back({
                mappings[index].application_swapchain,
                entries[index].prepared});
        }
        return output;
    } catch (...) {
        release_deferred();
        output.reason = GenerationPrepareReason::exception;
        return output;
    }
}

[[nodiscard]] std::optional<PendingApplicationFrame> latest_pending_application_frame(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order,
    bool match_latest_by_submission) noexcept {
    try {
        std::scoped_lock lock(state->mutex);
        if (consume_in_submission_order) {
            return state->pending_frames.empty()
                ? std::nullopt
                : std::optional<PendingApplicationFrame>{
                      state->pending_frames.front()};
        }
        const auto frame = std::find_if(
            state->pending_frames.begin(),
            state->pending_frames.end(),
            [display_time](const PendingApplicationFrame& candidate) {
                return candidate.display_time == display_time;
            });
        if (frame != state->pending_frames.end()) {
            return *frame;
        }
        if (state->pending_frames.size() == 1) return state->pending_frames.front();
        // Native Stereo Fix may rewrite displayTime. A begun frame with no
        // overlapping wait identifies the newest pending record by call order.
        return match_latest_by_submission && !state->pending_frames.empty()
            ? std::optional<PendingApplicationFrame>{
                  state->pending_frames.back()}
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<XrDuration> latest_pending_application_period(
    const std::shared_ptr<SessionState>& state,
    XrTime display_time,
    bool consume_in_submission_order,
    bool match_latest_by_submission) noexcept {
    const auto frame = latest_pending_application_frame(state, display_time,
        consume_in_submission_order, match_latest_by_submission);
    return frame ? std::optional<XrDuration>{frame->display_period} : std::nullopt;
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
    if (continuous_presenter_active(state))
        static_cast<void>(wait_for_presenter_idle(state));
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::continuity_reset,
        0,
        state ? handle_value(state->handle) : 0);
    try {
        {
            std::scoped_lock lock(state->mutex);
            state->previous_projection.reset();
        }
        { std::scoped_lock lock(state->presenter_mutex); state->director_anchor_sequence = 0; }
        for (const auto& swapchain : find_swapchains(state)) {
            std::scoped_lock call_lock(swapchain->call_mutex);
            std::shared_ptr<FrameGenerationSwapchainState> generation;
            {
                std::scoped_lock lock(swapchain->mutex);
                generation = swapchain->frame_generation;
                swapchain->luke_admitted_identity.reset();
                swapchain->luke_admitted_capture_serial = 0;
            }
            if (!generation || !generation->synthesizer) {
                continue;
            }
            std::scoped_lock gpu_lock(state->gpu_mutex);
            static_cast<void>(generation->synthesizer->retire_previous());
            if (generation->afw_source_synthesizer)
                static_cast<void>(generation->afw_source_synthesizer->retire_previous());
            if (generation->afw_temporal_synthesizer) {
                static_cast<void>(
                    generation->afw_temporal_synthesizer->retire_previous());
            }
        }
    } catch (...) {
    }
}

// The raw XR and post-AFW colors have different temporal anchors. Reset once
// on an actual source transition, never on every missing-guide frame.
[[nodiscard]] bool select_luke_ofxr_input(
    const std::shared_ptr<SessionState>& state, bool afw_input) noexcept {
    if (!luke_color_only_ofxr(*state)) {
        state->luke_ofxr_previous_afw_input.reset();
        return false;
    }
    const bool changed = state->luke_ofxr_previous_afw_input &&
        *state->luke_ofxr_previous_afw_input != afw_input;
    state->luke_ofxr_previous_afw_input = afw_input;
    if (changed) {
        clear_generation_continuity(state);
        if (!afw_input) {
            std::scoped_lock lock(state->luke_guide_mutex);
            state->luke_afw_last_pair.reset();
        }
    }
    return changed;
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
    XrTime display_time{};
    bool completed{};
};

[[nodiscard]] InternalCycleResult submit_current_cycle(
    const std::shared_ptr<SessionState>& state,
    const XrFrameEndInfo& current_end_info,
    std::uint64_t frame_serial = 0,
    const PreparedProjectionFrame* prepared = nullptr,
    std::optional<XrFrameState> adopted_frame = std::nullopt,
    GeneratedFrameEndInfo* owned_output=nullptr, bool synthetic_output=false, xrfg::InterfaceFrameStamp stamp={}) {
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
    const XrResult wait_result = adopted_frame
        ? (frame_state = *adopted_frame, frame_state.next = nullptr, XR_SUCCESS)
        : state->dispatch->wait_frame(state->handle, &wait_info, &frame_state);
    output.wait_elapsed = std::chrono::steady_clock::now() - wait_started;
    output.predicted_display_period = frame_state.predictedDisplayPeriod;
    output.display_time = frame_state.predictedDisplayTime;
    xrfg::bridge_flight_logger().end(
        wait_token,
        xrfg::BridgeFlightOperation::internal_wait_frame,
        wait_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime),
        static_cast<std::uint64_t>(frame_state.predictedDisplayPeriod),
        frame_state.shouldRender);
    if (state->luke_afw_enabled && frame_serial != 0) {
        // result 4: requested real time versus physical runtime slot.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            4, frame_serial,
            static_cast<std::uint64_t>(current_end_info.displayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
        xrfg::two_frame_capture_timeline(
            "frame", 4, frame_serial,
            static_cast<std::uint64_t>(current_end_info.displayTime),
            static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    }
    if (XR_FAILED(wait_result)) {
        return output;
    }

    XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const auto begin_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::internal_begin_frame,
        handle_value(state->handle));
    const XrResult begin_result = with_runtime_entry(state, [&] {
        return state->dispatch->begin_frame(state->handle, &begin_info);
    });
    xrfg::bridge_flight_logger().end(
        begin_token,
        xrfg::BridgeFlightOperation::internal_begin_frame,
        begin_result,
        static_cast<std::uint64_t>(frame_state.predictedDisplayTime));
    if (XR_FAILED(begin_result)) {
        output.result = begin_result;
        return output;
    }

    if (state->luke_fresh_only && !continuous_presenter_active(state)) {
        state->luke_output_begun = true;
        state->luke_output_slot = frame_state;
        state->runtime_frame_waited_unbegun = false;
        refresh_luke_prediction_clock(*state,frame_state,std::chrono::steady_clock::now());
        state->last_inline_frame_state = frame_state;
        state->last_inline_frame_state_valid = true;
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
    join_runtime_queue_to_application(state.get(), state->app_end_frame_release_value);
    const XrResult end_result = with_runtime_entry(state, [&] {
        log_luke_physical_boundary(state, &submitted, frame_serial, 6, prepared);
        GeneratedFrameEndInfo pending;
        if(prepared && !owned_output) {
            // Only this real's exact staging; no extra current copy or latest lookup.
            for(const auto& resource:prepared->resources) {
                const auto& output=resource.generation;
                if(output.staging_generation) pending.pending_outputs.push_back({output.staging_generation,
                    output.staging_generation->current_staging[output.current_slot],output.synthesizer,output.staging_ticket,false});
            }
        }
        if(!handover_djules_outputs(state.get(),owned_output?owned_output->pending_outputs:pending.pending_outputs))
            return XR_ERROR_RUNTIME_FAILURE;
        report_native_frame_content(state.get(),owned_output);
        return state->fps_overlay
            ? state->fps_overlay->end_frame(&submitted, synthetic_output,true,native_frame_results(owned_output),owned_output?owned_output->native_output:0, owned_output ? owned_output->interface_stamp : stamp)
            : content_end_frame(state->handle, &submitted);
    });
    log_luke_physical_boundary(state, &submitted, frame_serial, 7, prepared, end_result);
    xrfg::bridge_flight_logger().end(
        end_token,
        xrfg::BridgeFlightOperation::internal_end_frame,
        end_result,
        static_cast<std::uint64_t>(submitted.displayTime),
        submitted.layerCount,
        frame_state.shouldRender);
    if (state->luke_afw_enabled && frame_serial != 0) {
        // result 5: real output submitted (or runtime rejection).
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            5, frame_serial,
            static_cast<std::uint64_t>(submitted.displayTime),
            (static_cast<std::uint64_t>(submitted.layerCount) << 32) |
                static_cast<std::uint32_t>(end_result));
        xrfg::two_frame_capture_timeline(
            "frame", 5, frame_serial,
            static_cast<std::uint64_t>(submitted.displayTime),
            (static_cast<std::uint64_t>(submitted.layerCount) << 32) |
                static_cast<std::uint32_t>(end_result));
    }
    if (XR_FAILED(end_result)) {
        output.result = end_result;
        return output;
    }

    if(owned_output && !synthetic_output && !owned_output->synthetic && submitted.layerCount &&
        blackbar_gate(*state)) {
        XrDuration period{};
        {std::scoped_lock lock(state->mutex);period=state->minimum_runtime_display_period;}
        blackbar_observe(*state,owned_output->blackbar_pose,submitted.displayTime,
            period,frame_state.predictedDisplayPeriod,false);
    }
    output.completed = true;
    return output;
}


// The presenter exists because some runtimes do not pace xrWaitFrame. The
// layer submits two frames per application frame and they must land one
// display period apart; a runtime that returns the wait immediately leaves
// both in the same scanout window and the compositor discards one, so the
// headset shows half rate while the GPU does all of the synthesis work.
//
// Inferring that from the application's threading - whether its second wait
// lands inside the previous frame, twice in a row - reads a property of the
// application under load rather than of the runtime. It moves with scene
// cost, and because the promotion latches the first time the race resolves,
// a session can run un-paced indefinitely and then engage the moment the
// view happens to get cheap. Measuring the runtime does not have that
// problem: how long its wait blocks is the same whether the headset is
// pointed at the floor or at a city.
//
// Scoped to SteamVR, like the throttle detector it sits beside. The two are
// the same runtime quirk seen from opposite ends - one throttles the inline
// second cycle, the other does not pace the application at all - and VDXR,
// which blocks a full period and gets this right for free, is deliberately
// left alone rather than being told apart by a measurement that a merely
// late application can also produce.
[[nodiscard]] bool runtime_wait_lacks_pacing(
    const std::shared_ptr<SessionState>& state) noexcept {
    // Enough waits to rule out an application that was simply late for one.
    constexpr std::uint32_t kUnpacedWaitPromotionStreak = 3;
    if (!state->dispatch->steamvr_runtime) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    return state->minimum_runtime_display_period > 0 &&
           state->unpaced_wait_streak >= kUnpacedWaitPromotionStreak;
}
// The inline path hands the synthetic to the runtime and then, immediately
// after, the real frame. Nothing in the layer decides how far apart those two
// land: the gap is whatever the runtime imposes when it blocks the second
// wait. When that gap is much shorter than a display period both frames
// arrive inside one scanout window, the compositor keeps the later one - the
// real frame - and drops the synthetic. The next window has nothing new and
// repeats. Half the layer's work is discarded, the overlay still counts it,
// and the application is separately halved because each of its frames now
// costs two runtime cycles. A game that held the full rate without the layer
// drops to half with it.
//
// The presenter fixes this by construction: it sleeps to its own
// scanout-locked grid between hand-overs, so the two frames are a period
// apart whatever the runtime does with a wait.
//
// Measured here rather than inferred. The two existing detectors ask whether
// the situation looks like one that needs a presenter - how many threads the
// application uses, how long a wait took - and on Atomic Heart both answer
// no while the output is plainly wrong. Captured on that title across two
// runtimes and two refresh rates, the gap sits at 0.42, 0.45 and 0.54 of a
// period against about 1.0 when the spacing is right, so three quarters
// separates them with margin either way.
//
// A long streak because promoting is a large switch - it hands every
// downstream frame call to another thread, and an over-eager version of the
// SteamVR detector once hung the GPU four milliseconds after firing. Thirty
// pairs is a quarter of a second at 120 Hz and cannot be reached by a hitch,
// while the real thing holds for thousands of consecutive frames.
[[nodiscard]] bool inline_pair_lands_in_one_scanout(
    const std::shared_ptr<SessionState>& state,
    std::chrono::steady_clock::duration gap) noexcept {
    if (djules_inline_runtime(*state)) {
        std::scoped_lock lock(state->mutex);
        state->bunched_pair_streak = 0;
        return false;
    }
    constexpr std::uint32_t kBunchedPairPromotionStreak = 30;
    constexpr std::int64_t kBunchedNumerator = 3;
    constexpr std::int64_t kBunchedDenominator = 4;
    const auto gap_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(gap).count();
    std::scoped_lock lock(state->mutex);
    const XrDuration period = state->minimum_runtime_display_period;
    if (period <= 0 || gap_nanoseconds < 0) {
        state->bunched_pair_streak = 0;
        return false;
    }
    const bool bunched =
        gap_nanoseconds * kBunchedDenominator < period * kBunchedNumerator;
    state->bunched_pair_streak =
        bunched ? state->bunched_pair_streak + 1 : 0;
    if (!bunched ||
        state->bunched_pair_streak < kBunchedPairPromotionStreak) {
        return false;
    }
    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::presenter_transition,
        static_cast<std::int64_t>(state->bunched_pair_streak),
        static_cast<std::uint64_t>(gap_nanoseconds),
        static_cast<std::uint64_t>(period),
        200);
    return true;
}

[[nodiscard]] bool steamvr_wait_requires_continuous_presenter(
    const std::shared_ptr<SessionState>& state,
    const InternalCycleResult& cycle) noexcept {
    // FSR owns an inline two-eye pair whose guides and history are tied to the
    // application's submission timeline. Moving it to the asynchronous
    // director corrupts that ownership. A genuine overlapping application
    // xrWaitFrame can still request the established pipelined path, but a
    // runtime timing heuristic must never do so.
    if (state->frame_generation_mode ==
        xrfg::D3D12FrameGenerationMode::fsr3) {
        std::scoped_lock lock(state->mutex);
        state->steamvr_throttled_wait_streak = 0;
        return false;
    }
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
    bool consume_in_submission_order,
    bool match_latest_by_submission) {
    std::scoped_lock lock(state->mutex);
    if (consume_in_submission_order) {
        if (!state->pending_frames.empty()) {
            state->pending_frames.pop_front();
        }
        return;
    }
    if (match_latest_by_submission) {
        state->pending_frames.clear();
        return;
    }
    const auto frame = std::find_if(
        state->pending_frames.begin(),
        state->pending_frames.end(),
        [display_time](const PendingApplicationFrame& candidate) {
            return candidate.display_time == display_time;
        });
    if (frame == state->pending_frames.end()) {
        // Unmatched own-time End cannot identify an orphan. Retain the known
        // next virtual ticket when Begin is still outstanding; shed older waits.
        if (state->application_wait_pending_begin && !state->pending_frames.empty()) {
            const auto next = state->pending_frames.back();
            state->pending_frames.clear(); state->pending_frames.push_back(next);
        } else state->pending_frames.clear();
        return;
    }
    state->pending_frames.erase(state->pending_frames.begin(), std::next(frame));
}

// Native depth history follows the selected comparison route even when the
// application does not enumerate its images again after a menu switch.
// Caller holds call_mutex, mutex and the session GPU transaction lock.
HRESULT reconcile_depth_history_for_route(SwapchainState& chain) {
    if (chain.session->graphics_binding != SessionGraphicsBinding::d3d12 ||
        required_release_state(chain) != D3D12_RESOURCE_STATE_DEPTH_WRITE) return S_OK;
    if (chain.session->nvidia_new_route) {
        if (chain.d3d12_history) {
            const auto result = chain.d3d12_history->wait_for_idle();
            if (FAILED(result)) return result;
            chain.last_released_capture.reset();
            chain.d3d12_history.reset();
        }
        return S_OK;
    }
    if (chain.d3d12_history || chain.enumerated_d3d12_images.empty() ||
        (chain.create_info.createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) return S_OK;
    std::vector<ID3D12Resource*> resources;
    for (const auto& image : chain.enumerated_d3d12_images) resources.push_back(image.Get());
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    const auto result = history->initialize(chain.session->d3d12_device.Get(),
        chain.session->d3d12_queue.Get(), resources, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    if (SUCCEEDED(result)) chain.d3d12_history = std::move(history);
    return result;
}

// The caller owns frame-call admission and has drained the exact old descriptors.
// Source history/captures stay intact. Retire all changed XR output shapes
// before allocating any replacements, so a transaction cannot double its budget.
HRESULT reconcile_multiplier_outputs(const std::shared_ptr<SessionState>& state) {
    if(!configurable_native_outputs(*state)) return S_OK;
    const bool extra=extra_output_capacity(*state);
    std::vector<std::shared_ptr<SwapchainState>> replace;
    for(const auto& chain:find_swapchains(state)) {
        std::unique_lock call_lock(chain->call_mutex);
        std::unique_lock lock(chain->mutex);
        auto generation=chain->frame_generation;
        if(!generation) continue;
        const auto ready=generation->synthesizer->wait_for_idle();
        if(FAILED(ready)) return ready;
        if((generation->staged_outputs && generation->staging_stride!=(extra?2U:1U)) ||
            generation->synthetic_runtime_rings!=native_synthetic_rings(*state) ||
            generation->current_runtime_rings!=native_current_rings(*state) ||
            state->frame_generation_mode==xrfg::D3D12FrameGenerationMode::native_dlss) {
            lock.unlock();
            generation.reset();
            drain_swapchain_gpu(chain);
            destroy_frame_generation_swapchains(chain);
            replace.push_back(chain);
        } else if(!generation->staged_outputs) {
            generation->direct_extra_outputs=extra;
            for(auto& lease:generation->direct_synthetic_leases)
                lease=extra?std::make_shared<DirectSyntheticLease>():nullptr;
        }
    }
    for(const auto& chain:replace) {
        std::unique_lock call_lock(chain->call_mutex);
        SwapchainEligibilityReason reason{};std::uint64_t detail{};
        auto generation=create_d3d12_frame_generation_swapchains(chain,&reason,&detail);
        std::scoped_lock lock(chain->mutex);
        chain->frame_generation=std::move(generation);
        chain->generation_eligible_pending=false;
        chain->generation_declined=!chain->frame_generation;
        chain->generation_refusal=reason;chain->generation_refusal_detail=detail;
        if(!chain->frame_generation) {
            if(budget_refusal(reason)) state->generation_budget_exhausted=true;
            return E_FAIL;
        }
    }
    return S_OK;
}

// Frame-call ownership is held. Capacity is reconciled only after old output drain.
[[nodiscard]] XrResult switch_djules_pipeline(const std::shared_ptr<SessionState>& state,bool target,bool prefer_fps) {
    if(!djules_native_scheduler(*state)) return XR_SUCCESS;
    prefer_fps = target || prefer_fps;
    if(target==state->djules_triple_requested && prefer_fps==state->djules_deep_pipeline) return XR_SUCCESS;
    if(continuous_presenter_active(state)) {
        const auto result=wait_for_presenter_idle(state);
        if(XR_FAILED(result)) return result;
    }
    const bool multiplier_changed=target!=state->djules_triple_requested;
    clear_generation_continuity(state);
    {
        std::scoped_lock content(state->presenter_content_mutex);
        std::scoped_lock lock(state->presenter_mutex);
        state->djules_triple_requested=target;
        state->djules_deep_pipeline=prefer_fps;
        ++state->djules_configuration_epoch;
        state->djules_triple_release={};
        state->three_slots.configure(djules_triple_director(*state)?3:2);
      state->director_anchor_sequence = 0;
        state->application_served_serial=state->presenter_frame_serial;
        state->presenter_last_frame.reset();
    }
    if(multiplier_changed) {
        const auto capacity=reconcile_multiplier_outputs(state);
        if(FAILED(capacity)) {
            state->generation_initialization_result=capacity;
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    if(state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
        700,target?3:2,state->djules_deep_pipeline && !target,2);
    xrfg::embedded::register_djules_pipeline(state->control_id, prefer_fps, target);
    return XR_SUCCESS;
}

[[nodiscard]] XrResult poll_djules_pipeline(const std::shared_ptr<SessionState>& state) {
    if(!state->djules_pipeline_switchable || !djules_native_scheduler(*state)) return XR_SUCCESS;
    const auto now=std::chrono::steady_clock::now();
    if(now<state->djules_pipeline_poll) return XR_SUCCESS;
    state->djules_pipeline_poll=now+std::chrono::milliseconds(250);
    const auto desired=xrfg::embedded::djules_pipeline_snapshot();
    const auto target=std::pair{desired.triple_frame_gen,desired.prefer_fps};
    // One owned INI and two matching observations for both live policy controls.
    XrResult result=XR_SUCCESS;
    if(state->djules_pipeline_candidate && *state->djules_pipeline_candidate==target)
        result=switch_djules_pipeline(state,target.first,target.second);
    state->djules_pipeline_candidate=target;
    return result;
}

// Called with frame ownership and presenter content excluded. No GUI thread
// touches GPU objects; no new captures may enter during this transaction.
HRESULT apply_embedded_control(const std::shared_ptr<SessionState>& state,
                               bool force_startup_rendezvous = false,
                               const XrFrameEndInfo* current = nullptr) {
    auto control = xrfg::embedded::snapshot();
    control.desired=effective_session_settings(*state,control.desired);
    // A concurrent GUI enable cancels the Off-only startup request; ordinary
    // control application will take that revision on the next safe entry.
    if (force_startup_rendezvous && control.desired.enabled) return E_ABORT;
    // Published AER guides are not guides for the reconstructed peer eye.
    const bool hybrid_ready=control.desired.hybrid &&
        !(state->luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode != 0) &&
        xrfg::dlss_motion_vector_publications()!=0;
    const bool afw_needed=state->luke_afw_enabled && xrfg::embedded::afw_snapshot().desired.mode!=0;
    const bool outputs_needed=control.desired.enabled || afw_needed;
    const bool afw_use_changed=state->luke_afw_resources_needed &&
        afw_needed!=*state->luke_afw_resources_needed;
    if(!state->luke_afw_resources_needed)state->luke_afw_resources_needed=afw_needed;
    const bool output_use_changed=state->generation_outputs_needed &&
        outputs_needed!=*state->generation_outputs_needed;
    if (!state->generation_outputs_needed) state->generation_outputs_needed=outputs_needed;
    if (state->control_revision == control.revision && !force_startup_rendezvous && !output_use_changed && !afw_use_changed &&
        hybrid_ready==state->nvidia_options.hybrid) return S_OK;
    const bool explicit_revision = state->control_revision != control.revision;
    if (!force_startup_rendezvous && !state->control_reconfigure_required && !output_use_changed && !afw_use_changed &&
        hybrid_ready == state->nvidia_options.hybrid && state->applied_generation_settings) {
        auto prior=*state->applied_generation_settings;
        const bool pacing_changed=prior.pacing_mode!=control.desired.pacing_mode;
        const bool buffer_changed=prior.increase_buffer!=control.desired.increase_buffer;
        prior.pacing_mode=control.desired.pacing_mode;
        prior.increase_buffer=control.desired.increase_buffer;
        if ((pacing_changed || buffer_changed) && prior==control.desired) {
            // A presentation-policy toggle is not a generator reset. In
            // particular do not re-prime an existing FidelityFX packed input
            // as though it were a newly allocated UAV. Keep its history/state.
            if (state->private_runtime &&
                continuous_presenter_active(state) && XR_FAILED(wait_for_presenter_idle(state)))
                return E_FAIL;
            if(state->private_runtime) {
                const auto policy=state->private_runtime->set_director_enabled(state->menu_enabled && control.desired.pacing_mode!=0);
                if(XR_FAILED(policy))return E_FAIL;
                const auto buffer=state->private_runtime->set_buffer_capacity(control.desired.increase_buffer?3:2);
                if(XR_FAILED(buffer))return E_FAIL;
                state->single_director_requested=control.desired.pacing_mode==1;
            }
            state->applied_generation_settings=control.desired;
            state->control_revision=control.revision;
            if(pacing_changed)++state->djules_configuration_epoch;
            if (state->fps_overlay) state->fps_overlay->reset_metrics();
            xrfg::embedded::applied(state->control_id,control.revision,state->menu_enabled,S_OK);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::embedded_configuration,
                611,control.revision,control.desired.pacing_mode,single_director_pacing(*state));
            return S_OK;
        }
    }
    const bool previously_enabled = state->menu_enabled;
    if (continuous_presenter_active(state))
        static_cast<void>(wait_for_presenter_idle(state));
    state->menu_enabled = false;
    {
        std::scoped_lock lock(state->presenter_mutex);
        state->presenter_last_frame.reset();
        state->optical_reserve_ns = 0;
        state->optical_reserve_current_due = {};
        state->optical_reserve_pair = state->optical_reserve_served = 0;
    }
    if(!outputs_needed || afw_use_changed) {
        // Held AER descriptors can retain complete interop generations. Drop
        // them only after presentation drain and before retiring their images.
        clear_generation_continuity(state);
        std::scoped_lock guide_lock(state->luke_guide_mutex);
        state->luke_afw_released_eye.reset();
        state->luke_afw_last_pair.reset();
        state->luke_frame_motion_vectors.reset();
    }
    auto swapchains = find_swapchains(state);
    std::vector<std::unique_lock<std::mutex>> capture_locks;
    for (const auto& chain : swapchains) capture_locks.emplace_back(chain->call_mutex);
    std::scoped_lock gpu_lock(state->gpu_mutex);
    HRESULT result = S_OK;
    if(!afw_needed) {
        // The isolated provider owns eye buffers outside per-chain generation.
        // Its destructor waits its exact submissions and retires allocations.
        state->luke_afw.reset();
        state->luke_context_failed_queue=nullptr;
    }
    auto backend = control.desired.backend == 0
        ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
    const bool nvidia_new_route = control.desired.backend >= 2;
    const bool djules_route = control.desired.backend == 3;
    const auto generation_mode = static_cast<xrfg::D3D12FrameGenerationMode>(control.desired.frame_generation);
    xrfg::D3D12NvidiaOpticalFlowOptions options{
        static_cast<xrfg::D3D12NvidiaPerformancePreset>(control.desired.preset),
        static_cast<xrfg::D3D12NvidiaInputScale>(control.desired.scale), control.desired.backward,
        nvidia_new_route, xrfg::flow_encoding_for_route(nvidia_new_route), nvidia_new_route, control.desired.native_scale};
    if(hybrid_ready) {
        options.hybrid=true; options.input_scale=xrfg::D3D12NvidiaInputScale::quarter;
        backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    }
    const int effective_separation = effective_memory_control(*state,control.desired.separate_synthesis);
    const int effective_reduction = effective_memory_control(*state,control.desired.lower_vram);
    const bool changed = options.hybrid!=state->nvidia_options.hybrid ||
        control.desired.hybrid!=state->hybrid_requested || force_startup_rendezvous || state->control_reconfigure_required || backend != state->optical_flow_backend ||
        options.preset != state->nvidia_options.preset ||
        options.input_scale != state->nvidia_options.input_scale ||
        options.bidirectional != state->nvidia_options.bidirectional ||
        options.native_scale != state->nvidia_options.native_scale ||
        generation_mode != state->frame_generation_mode ||
        nvidia_new_route != state->nvidia_new_route || djules_route != state->djules_route ||
        state->separate_synthesis != effective_separation || state->lower_vram != effective_reduction;
    // UI/depth enumeration is not permission to allocate generator outputs.
    // This applies equally to optical flow, Hybrid, FSR and DLSS-G, on every
    // runtime. Keep only the separately required Outlaws startup exception.
    const bool projected_budget_control = projection_owned_outputs(*state);
    const bool retry_budget = projected_budget_control && (explicit_revision || output_use_changed) &&
        (changed || (output_use_changed && outputs_needed) || (!previously_enabled && control.desired.enabled));
    if (retry_budget)
        state->generation_budget_exhausted.store(false, std::memory_order_release);
    // Enumeration is excluded by frame_call_mutex, so newly created contexts
    // also see this tuple. A partial failure remains bypass, never mixed flow.
    state->optical_flow_backend = backend;
    state->nvidia_options = options;
    state->frame_generation_mode = generation_mode;
    state->hybrid_requested=control.desired.hybrid;
    state->nvidia_new_route = nvidia_new_route;
    state->djules_route = djules_route;
    state->djules_history_wait_enabled.store(ordinary_djules_route(*state), std::memory_order_relaxed);
    if (!ordinary_djules_route(*state)) state->presenter_restore_after_bypass=false;
    state->separate_synthesis = effective_separation;
    state->lower_vram = effective_reduction;
    state->dlss_motion_vectors = control.desired.motion_vectors == 1 ||
        generation_mode != xrfg::D3D12FrameGenerationMode::ofxr ||
        state->luke_afw_enabled;
    if (changed) state->generation_initialization_result = S_OK;
    std::vector<XrSwapchain> projected_sources, auxiliary_sources;
    bool submitted_sources_known = current && current->type == XR_TYPE_FRAME_END_INFO &&
        (!current->layerCount || current->layers);
    if (submitted_sources_known) for (unsigned i=0; i<current->layerCount; ++i) {
        const auto* header=current->layers[i];
        if (!header) { submitted_sources_known=false; break; }
        if (header->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(header);
            if (projection->viewCount && !projection->views) { submitted_sources_known=false; break; }
            for (unsigned v=0; v<projection->viewCount; ++v)
                projected_sources.push_back(projection->views[v].subImage.swapchain);
        } else {
            auto copy=copy_composition_layer_for_presenter(header);
            if (auto* handle=copy ? composition_swapchain(*copy) : nullptr)
                auxiliary_sources.push_back(*handle);
        }
    }
    for (const auto& chain : swapchains) {
        std::unique_lock lock(chain->mutex);
        const bool projected=std::find(projected_sources.begin(),projected_sources.end(),chain->handle)!=projected_sources.end();
        const bool auxiliary=std::find(auxiliary_sources.begin(),auxiliary_sources.end(),chain->handle)!=auxiliary_sources.end();
        if (projected) chain->diagnostic_layer_roles|=1;
        if (auxiliary) chain->diagnostic_layer_roles|=2;
        // A drained control revision is the safe retirement boundary. Ordinary
        // stereo keeps only its submitted eye sources. AER may alternate source
        // handles, so retain its observed/unknown eye histories; only proven
        // auxiliary-only sources are redundant there. Off without AFW needs no
        // interpolation outputs at all. Application images/captures survive.
        const bool auxiliary_only=(chain->diagnostic_layer_roles&3)==2;
        const bool unused_projection=submitted_sources_known && !projected &&
            (!state->luke_afw_enabled || auxiliary_only);
        // Outlaws can create these on its original SINGLETHREADED owner only
        // before the first frame. Off pauses their use; it must not retire the
        // warmed set that the sealed initialization window cannot recreate.
        const bool sealed_outlaws_output=state->luke_outlaws_preframe && state->luke_outlaws_frame_started;
        if ((explicit_revision || output_use_changed) && projected_budget_control && chain->frame_generation &&
            !sealed_outlaws_output && (unused_projection || !outputs_needed)) {
            auto generation=chain->frame_generation;
            result=generation->synthesizer ? generation->synthesizer->wait_for_idle() : S_OK;
            if (SUCCEEDED(result) && generation->afw_source_synthesizer)
                result=generation->afw_source_synthesizer->wait_for_idle();
            if (SUCCEEDED(result) && generation->afw_temporal_synthesizer)
                result=generation->afw_temporal_synthesizer->wait_for_idle();
            if (SUCCEEDED(result) && generation->d3d11_interop)
                result=generation->d3d11_interop->wait_for_idle();
            if (FAILED(result)) break;
            generation.reset();lock.unlock();
            destroy_frame_generation_swapchains(chain);
            lock.lock();
            chain->generation_eligible_pending=true;
            chain->generation_declined=false;
            chain->generation_refusal=SwapchainEligibilityReason::ready;
            chain->generation_refusal_detail=0;
        }
        const bool valid_projection_source =
            (chain->d3d12_history || !chain->enumerated_d3d11_images.empty()) &&
            chain->enumerated_image_count &&
            (chain->create_info.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            !(chain->create_info.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
            !(chain->create_info.createFlags & (XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT |
                                               XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT)) &&
            chain->create_info.faceCount == 1;
        if (retry_budget && valid_projection_source && chain->generation_declined &&
            (budget_refusal(chain->generation_refusal) ||
             chain->generation_refusal == SwapchainEligibilityReason::budget_exhausted)) {
            // One explicit route/enable revision reopens a refused transaction.
            // The current projection still decides which eyes may allocate;
            // another refusal keeps the existing real-frame/session budget gate.
            chain->generation_declined = false;
            chain->generation_eligible_pending = true;
            chain->generation_refusal = SwapchainEligibilityReason::ready;
            chain->generation_refusal_detail = 0;
        }
        if (changed && valid_projection_source && chain->generation_declined &&
            chain->generation_refusal == SwapchainEligibilityReason::synthesis_initialize_failed &&
            !state->generation_budget_exhausted.load(std::memory_order_acquire)) {
            // This refusal belongs to the previous provider attempt, not to
            // the application's images. Retry once for this control revision,
            // preserving source history, captures and all non-provider gates.
            chain->generation_declined = false;
            chain->generation_eligible_pending = true;
            chain->generation_refusal = SwapchainEligibilityReason::ready;
            chain->generation_refusal_detail = 0;
        }
        const bool admitted_shape_change = changed && chain->frame_generation &&
            state->graphics_binding == SessionGraphicsBinding::d3d12 &&
            (chain->frame_generation->staged_outputs != staged_native_outputs(*state) ||
             chain->frame_generation->current_runtime_rings != native_current_rings(*state) ||
             chain->frame_generation->synthetic_runtime_rings != native_synthetic_rings(*state) ||
             chain->frame_generation->direct_extra_outputs != (!staged_native_outputs(*state) && extra_output_capacity(*state)) ||
             chain->frame_generation->staging_stride != (staged_native_outputs(*state) && extra_output_capacity(*state) ? 2U : 1U));
        if (admitted_shape_change) {
            result = chain->frame_generation->synthesizer
                ? chain->frame_generation->synthesizer->wait_for_idle() : S_OK;
            if (FAILED(result)) break;
            lock.unlock();
            // Replace the old private output shape; the already-admitted source
            // history and this frame's exact released capture remain owned.
            destroy_frame_generation_swapchains(chain);
            lock.lock();
            // A paused shape change retires the admitted outputs without
            // creating replacements below. Retain their admission so a later
            // enable can rebuild them even when the route tuple is unchanged.
            // Refusal and session-budget gates still govern that later attempt.
            chain->generation_eligible_pending = true;
        }
        if (changed) {
            result = reconcile_depth_history_for_route(*chain);
            if (FAILED(result)) break;
        }
        // djules75 a99413d: control revisions must respect the same eligibility,
        // previous-refusal and session-budget gates as deferred generation.
        const bool eligible = (chain->generation_eligible_pending || admitted_shape_change) &&
            !chain->generation_declined &&
            (admitted_shape_change || !state->generation_budget_exhausted.load(std::memory_order_acquire));
        if (!chain->frame_generation &&
            !projected_budget_control &&
            !(state->luke_outlaws_preframe && state->luke_outlaws_frame_started) &&
            (!state->luke_afw_enabled || (state->graphics_binding_capabilities&8ULL)!=0 ||
                luke_color_only_ofxr(*state)) &&
            (control.desired.enabled || state->luke_afw_enabled) && eligible) {
            std::vector<ID3D11Texture2D*> sources;
            for (const auto& image : chain->enumerated_d3d11_images) sources.push_back(image.Get());
            lock.unlock();
            SwapchainEligibilityReason reason{};
            std::uint64_t detail{};
            auto candidate = state->graphics_binding == SessionGraphicsBinding::d3d11
                ? create_d3d11_frame_generation_swapchains(chain, sources, &reason, &detail)
                : create_d3d12_frame_generation_swapchains(chain, &reason, &detail);
            lock.lock();
            const std::uint64_t auxiliary =
                (static_cast<std::uint64_t>(chain->enumerated_image_count) << 32) |
                chain->create_info.arraySize;
            if (candidate) {
                chain->frame_generation = std::move(candidate);
                chain->generation_eligible_pending = false;
                chain->generation_refusal = SwapchainEligibilityReason::ready;
                chain->generation_refusal_detail = 0;
                lock.unlock();
                log_swapchain_eligibility(chain, SwapchainEligibilityReason::ready,
                    chain->enumerated_image_count, auxiliary);
                lock.lock();
            } else {
                chain->generation_declined = true;
                chain->generation_refusal = reason;
                chain->generation_refusal_detail = detail;
                if (reason == SwapchainEligibilityReason::synthesis_initialize_failed)
                    result = static_cast<HRESULT>(detail);
                lock.unlock();
                log_swapchain_eligibility(chain, reason, detail, auxiliary);
                lock.lock();
            }
        }
        if (chain->frame_generation && chain->frame_generation->synthesizer) {
            if (chain->frame_generation->afw_source_synthesizer) {
                result = chain->frame_generation->afw_source_synthesizer->wait_for_idle();
                if (SUCCEEDED(result))
                    result = chain->frame_generation->afw_source_synthesizer->retire_previous();
                if (FAILED(result)) break;
            }
            auto& synthesis = chain->frame_generation->synthesizer;
            result = synthesis->wait_for_idle();
            if(SUCCEEDED(result) && !control.desired.enabled)
                result=synthesis->suspend_flow_resources();
            if (SUCCEEDED(result) && changed) {
                result = synthesis->reconfigure(backend, options,
                    state->luke_afw_enabled
                        ? xrfg::D3D12FrameGenerationMode::ofxr
                        : generation_mode);
            }
        }
        if (SUCCEEDED(result) && chain->frame_generation &&
            chain->frame_generation->afw_temporal_synthesizer) {
            auto& temporal = chain->frame_generation->afw_temporal_synthesizer;
            result = temporal->wait_for_idle();
            if(SUCCEEDED(result) && (!control.desired.enabled || !afw_needed))
                result=temporal->suspend_flow_resources();
            if (SUCCEEDED(result) && changed) {
                result = temporal->reconfigure(
                    backend, options, xrfg::D3D12FrameGenerationMode::ofxr);
            }
        }
        if (SUCCEEDED(result) && chain->frame_generation &&
            chain->frame_generation->afw_final_history) {
            result = !control.desired.enabled || !afw_needed
                ? chain->frame_generation->afw_final_history->release_snapshots()
                : chain->frame_generation->afw_final_history->invalidate();
        }
        if (SUCCEEDED(result) && chain->d3d12_history) result = chain->d3d12_history->wait_for_idle();
        if (SUCCEEDED(result) && chain->frame_generation && chain->frame_generation->d3d11_interop)
            result = chain->frame_generation->d3d11_interop->wait_for_idle();
        if(SUCCEEDED(result) && !outputs_needed) {
            chain->last_released_capture.reset();
            chain->last_released_motion_vectors.reset();
            if(chain->d3d12_history) {
                result=chain->d3d12_history->release_snapshots();
                // D11 history owns private interop source mirrors; keeping its
                // input bindings would keep those allocations alive while OFF.
                if(SUCCEEDED(result) && state->graphics_binding==SessionGraphicsBinding::d3d11 && !sealed_outlaws_output)
                    chain->d3d12_history.reset();
            }
            if(SUCCEEDED(result) && !sealed_outlaws_output)destroy_composition_capture(chain);
        }
        if(FAILED(result) && chain->frame_generation && !sealed_outlaws_output &&
            ((chain->frame_generation->synthesizer && !chain->frame_generation->synthesizer->initialized()) ||
             (chain->frame_generation->afw_temporal_synthesizer && !chain->frame_generation->afw_temporal_synthesizer->initialized()))) {
            // A failed replacement has already retired the old method. Reopen
            // normal projection admission for the next explicit revision.
            lock.unlock();
            destroy_frame_generation_swapchains(chain);
            lock.lock();
            chain->generation_eligible_pending=true;
            chain->generation_declined=false;
        }
        // This runs in EndFrame, AFTER the application's Release. The history
        // and its current capture survive backend reconfiguration; only the
        // previous synthesis anchor is retired above. Keep this frame's owned
        // capture and matching guides so the drained director can prime the
        // new route. Erasing them here hands a capture-less job to the worker
        // and turns an ordinary menu switch into a fatal presenter failure.
        if (FAILED(result)) break;
    }
    // Native ingress sees the desired value before this drained boundary.
    // Retirement below uses each original producer submission receipt; an
    // OFXR queue-idle fence cannot retire an unsubmitted game command list.
    if (changed || output_use_changed || afw_use_changed || !state->dlss_motion_vectors) {
        std::scoped_lock guide_lock(state->luke_guide_mutex);
        state->luke_afw_released_eye.reset();
        state->luke_afw_last_guide_publication = {};
        state->luke_afw_last_evaluated_publication = 0;
        state->luke_afw_last_pair.reset();
        state->luke_frame_guide_resolved = false;
        state->luke_last_consumed_guide_publication = 0;
        state->luke_frame_motion_vectors.reset();
    }
    {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
        state->generation_resume_display_time = 0;
        if (SUCCEEDED(result)) {
            state->optical_flow_backend = backend;
            state->nvidia_options = options;
        }
    }
    configure_session_guide_capture(*state);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    // OFXR OFF is always the ordered native-runtime bypass, including AFW-only.
    // Keep the selected preference for re-enable, but no director deadlines,
    // held phase or sole-director framework policy may remain effective OFF.
    state->menu_enabled = SUCCEEDED(result) && control.desired.enabled;
    if(state->private_runtime) {
        const auto buffer=state->private_runtime->set_buffer_capacity(control.desired.increase_buffer?3:2);
        if(XR_FAILED(buffer)) {result=E_FAIL;state->menu_enabled=false;}
        const auto policy=state->private_runtime->set_director_enabled(
            state->menu_enabled && control.desired.pacing_mode!=0);
        if(XR_FAILED(policy)) {result=E_FAIL;state->menu_enabled=false;}
        state->single_director_requested=control.desired.pacing_mode==1;
    }
    state->control_revision = control.revision;
    state->applied_generation_settings = control.desired;
    state->generation_outputs_needed=outputs_needed;
    state->luke_afw_resources_needed=afw_needed;
    state->control_reconfigure_required = FAILED(result);
    if (FAILED(result)) state->generation_initialization_result = result;
    ++state->djules_configuration_epoch;
    { std::scoped_lock lock(state->presenter_mutex);
      state->three_slots.configure(djules_triple_director(*state)?3:2);
      state->director_anchor_sequence = 0;
      state->djules_triple_release={}; }

    if (state->fps_overlay) state->fps_overlay->reset_metrics();
    xrfg::embedded::applied(state->control_id, control.revision, state->menu_enabled, result);
    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::embedded_configuration,
        result, control.revision, optical_flow_configuration_code(backend, options),
        (state->menu_enabled ? 1ULL : 0ULL) | (state->nvidia_new_route ? 2ULL : 0ULL) |
        (staged_native_outputs(*state) ? 4ULL : 0ULL) | (reduced_native_outputs(*state) ? 8ULL : 0ULL));
    return result;
}

// Correlate source-release fingerprints with submitted eye metadata, even
// when a submission retains an eye without releasing a new source image.
void record_rolling_runtime_output(const XrFrameEndInfo* info) noexcept {
    if(!xrfg::diagnostic_ring_enabled || !info || !info->layers)return;
    try {
        for(uint32_t l=0;l<info->layerCount;++l) {
            const auto* base=info->layers[l];
            if(!base || base->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION)continue;
            const auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(base);
            for(uint32_t eye=0;p->views && eye<p->viewCount;++eye) {
                const auto& v=p->views[eye];std::ostringstream row;row<<std::setprecision(9);
                row<<"{\"kind\":\"runtime_view\",\"qpc\":"<<xrfg::diagnostic_ring_qpc()<<",\"display_time\":"<<info->displayTime
                   <<",\"layer\":"<<l<<",\"eye\":"<<eye<<",\"space\":\""<<handle_value(p->space)
                   <<"\",\"swapchain\":\""<<handle_value(v.subImage.swapchain)<<"\",\"slice\":"<<v.subImage.imageArrayIndex
                   <<",\"position\":["<<v.pose.position.x<<','<<v.pose.position.y<<','<<v.pose.position.z
                   <<"],\"orientation\":["<<v.pose.orientation.x<<','<<v.pose.orientation.y<<','<<v.pose.orientation.z<<','<<v.pose.orientation.w
                   <<"],\"fov\":["<<v.fov.angleLeft<<','<<v.fov.angleRight<<','<<v.fov.angleUp<<','<<v.fov.angleDown
                   <<"],\"rect\":["<<v.subImage.imageRect.offset.x<<','<<v.subImage.imageRect.offset.y<<','
                   <<v.subImage.imageRect.extent.width<<','<<v.subImage.imageRect.extent.height<<"]}";
                xrfg::diagnostic_ring_record(row.str());
            }
        }
    }catch(...){}
}

void capture_application_metadata(const std::shared_ptr<SessionState>& state, const XrFrameEndInfo* info) noexcept {
    if (!state->frame_capture) return;
    try {
        const bool down=(GetAsyncKeyState(VK_CONTROL)&0x8000) &&
            (GetAsyncKeyState(VK_SHIFT)&0x8000) && (GetAsyncKeyState(VK_F10)&0x8000);
        const bool diagnostics=xrfg::bridge_flight_logger().enabled() || xrfg::bridge_flight_logger().ram_enabled();
        const bool trigger=down&&!state->diagnostic_key_down && diagnostics;
        state->diagnostic_key_down=down;
        // No GPU mutex, queries, thread, copies or disk work outside a capture.
        if (!trigger && !state->frame_capture->busy()) return;
        std::scoped_lock gpu_lock(state->gpu_mutex);
        if(trigger) static_cast<void>(state->frame_capture->start(true));
        state->frame_capture->tick(diagnostics);
        ++state->diagnostic_app_frame;
        if (!state->frame_capture->active() || !info || !info->layers) return;
        for (std::uint32_t l=0;l<info->layerCount;++l) {
            if (!info->layers[l] || info->layers[l]->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[l]);
            for (std::uint32_t eye=0;eye<projection->viewCount;++eye) {
                const auto& v=projection->views[eye];
                const auto chain=find_swapchain(v.subImage.swapchain);
                std::uint64_t serial=0,capture=0;
                if(chain) { std::scoped_lock lock(chain->mutex);
                    if(chain->last_released_capture) serial=chain->last_released_capture->serial;
                    capture=chain->last_diagnostic_capture; }
                std::ostringstream row;row<<std::setprecision(9);
                row<<"{\"kind\":\"view\",\"app_frame\":"<<state->diagnostic_app_frame<<",\"qpc\":"<<xrfg::FrameCapture::qpc()
                   <<",\"display_time\":"<<info->displayTime<<",\"layer\":"<<l<<",\"eye\":"<<eye
                   <<",\"space\":\""<<handle_value(projection->space)<<"\",\"swapchain\":\""<<handle_value(v.subImage.swapchain)
                   <<"\",\"serial\":"<<serial<<",\"capture\":"<<capture<<",\"slice\":"<<v.subImage.imageArrayIndex
                   <<",\"rect\":["<<v.subImage.imageRect.offset.x<<','<<v.subImage.imageRect.offset.y<<','
                   <<v.subImage.imageRect.extent.width<<','<<v.subImage.imageRect.extent.height<<"],\"position\":["
                   <<v.pose.position.x<<','<<v.pose.position.y<<','<<v.pose.position.z<<"],\"orientation\":["
                   <<v.pose.orientation.x<<','<<v.pose.orientation.y<<','<<v.pose.orientation.z<<','<<v.pose.orientation.w
                   <<"],\"fov\":["<<v.fov.angleLeft<<','<<v.fov.angleRight<<','<<v.fov.angleUp<<','<<v.fov.angleDown
                   <<"],\"upstream_engine_id\":null}";
                state->frame_capture->record(row.str());
            }
        }
    } catch (...) {}
}

struct LukePairDecision {
    enum class Action { normal, accept, drop } action{Action::normal};
    std::optional<xrfg::LukeRenderIdentity> identity;
    bool reset{};
};

LukePairDecision luke_pair_decision(std::span<const ProjectionResourceMapping> mappings) {
    LukePairDecision decision;
    bool any_identity = false, complete = true, skipped = false, all_admitted = true;
    bool captures_unchanged = true;
    for (const auto& mapping : mappings) {
        auto chain = find_swapchain(mapping.application_swapchain);
        if (!chain) { complete = false; continue; }
        std::scoped_lock lock(chain->mutex);
        skipped |= chain->luke_released_duplicate;
        const auto identity = chain->luke_released_identity;
        any_identity |= identity.has_value();
        if (!identity) {
            decision.reset |= chain->luke_admitted_identity.has_value();
            complete = false; continue;
        }
        if (!decision.identity) decision.identity = identity;
        else if (decision.identity != identity) complete = false;
        if (!chain->last_released_capture) complete = false;
        all_admitted &= chain->luke_admitted_identity == identity;
        captures_unchanged &= chain->last_released_capture &&
            chain->last_released_capture->serial == chain->luke_admitted_capture_serial;
        if (chain->luke_admitted_identity &&
            (identity->object != chain->luke_admitted_identity->object ||
             identity->render < chain->luke_admitted_identity->render)) decision.reset = true;
    }
    if (!any_identity && !skipped) return decision;
    if (!complete || skipped || all_admitted) {
        decision.action = LukePairDecision::Action::drop;
        decision.reset |= !complete || !all_admitted || !captures_unchanged;
    } else decision.action = LukePairDecision::Action::accept;
    return decision;
}

void commit_luke_pair(std::span<const ProjectionResourceMapping> mappings,
    std::optional<xrfg::LukeRenderIdentity> identity) {
    for (const auto& mapping : mappings) {
        if (auto chain = find_swapchain(mapping.application_swapchain)) {
            std::scoped_lock lock(chain->mutex);
            chain->luke_admitted_identity = identity;
            chain->luke_admitted_capture_serial = identity && chain->last_released_capture
                ? chain->last_released_capture->serial : 0;
        }
    }
}

// projection_view_rect, for every view whose rectangle or array index
// differs from the last record for its swapchain.
void log_projection_view_rects(
    SessionState& state, const ProjectionSnapshot& snapshot) noexcept {
    try {
        if (!xrfg::bridge_flight_logger().enabled()) {
            return;
        }
        std::scoped_lock lock(state.logged_view_rects_mutex);
        for (const auto& layer : snapshot.layers) {
            for (std::size_t view_index = 0; view_index < layer.views.size(); ++view_index) {
                const XrSwapchainSubImage& sub_image = layer.views[view_index].subImage;
                const std::array<std::uint64_t, 3> rect{
                    static_cast<std::uint64_t>(sub_image.imageArrayIndex),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.offset.x)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.offset.y),
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sub_image.imageRect.extent.width)) << 32) |
                        static_cast<std::uint32_t>(sub_image.imageRect.extent.height)};
                if (view_index >= 4) continue;
                if (!state.logged_view_rects.contains(sub_image.swapchain) && state.logged_view_rects.size() >= 64) continue;
                auto& last = state.logged_view_rects[sub_image.swapchain][view_index];
                if (last && *last == rect) {
                    continue;
                }
                last = rect;
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::projection_view_rect,
                    static_cast<std::int64_t>((rect[0] << 8) | (view_index & 0xFFU)),
                    handle_value(sub_image.swapchain),
                    rect[1],
                    rect[2]);
            }
        }
    } catch (...) {
    }
}


void log_video_memory_periodically(SessionState& state) noexcept {
    if (!xrfg::bridge_flight_logger().enabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < state.video_memory_poll_at) {
        return;
    }
    // Every second for the first minute: what a runtime or the game does
    // right after arming lands inside the first few seconds.
    if (state.video_memory_first_poll == std::chrono::steady_clock::time_point{}) {
        state.video_memory_first_poll = now;
    }
    state.video_memory_poll_at = now +
        (now - state.video_memory_first_poll < std::chrono::seconds(60)
             ? std::chrono::seconds(1)
             : std::chrono::seconds(5));
    log_video_memory(state, VideoMemoryStage::periodic, 0);
}

// A bridged depth swapchain on D3D11BridgePath::depth_private has runtime
// images nothing ever writes, so depth information naming it must not reach
// the runtime. The application's end-frame data is read-only, so the
// projection layers whose views carry such a node are copied with the node
// unlinked; every other layer pointer is forwarded as it came. The storage
// lives for the xrEndFrame call, which is as long as anything downstream
// reads the application's data: an owned frame copies what it keeps, a
// borrowed one is waited for before this call returns.
struct PrivateDepthStrip {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::deque<ProjectionLayerCopy> projections;
};

[[nodiscard]] bool names_private_depth_swapchain(const void* next) noexcept {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    if (header == nullptr ||
        header->type != XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
        return false;
    }
    const auto* depth = reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(header);
    const auto swapchain_state = find_swapchain(depth->subImage.swapchain);
    if (!swapchain_state) {
        return false;
    }
    std::scoped_lock lock(swapchain_state->mutex);
    return swapchain_state->private_depth;
}

// Returns the end info to submit: the application's own unless a projection
// view's chain starts with depth information naming a private depth
// swapchain, in which case a copy without those nodes. Only a leading run
// of such nodes is unlinked: the chain's other structures are of types this
// layer does not know the size of, so a node behind one of them stays.
[[nodiscard]] const XrFrameEndInfo* strip_private_depth(
    const SessionState& state,
    const XrFrameEndInfo* end_info,
    PrivateDepthStrip& storage) {
    if (end_info == nullptr || end_info->layers == nullptr ||
        !state.has_private_depth_swapchain.load(std::memory_order_acquire)) {
        return end_info;
    }
    bool any = false;
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount && !any; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        for (std::uint32_t view_index = 0;
             projection->views != nullptr && view_index < projection->viewCount; ++view_index) {
            if (names_private_depth_swapchain(projection->views[view_index].next)) {
                any = true;
                break;
            }
        }
    }
    if (!any) {
        return end_info;
    }
    storage.info = *end_info;
    storage.layers.assign(end_info->layers, end_info->layers + end_info->layerCount);
    for (std::uint32_t layer_index = 0; layer_index < end_info->layerCount; ++layer_index) {
        const auto* layer = end_info->layers[layer_index];
        if (layer == nullptr || layer->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(layer);
        if (projection->views == nullptr) {
            continue;
        }
        bool affected = false;
        for (std::uint32_t view_index = 0; view_index < projection->viewCount; ++view_index) {
            affected = affected ||
                names_private_depth_swapchain(projection->views[view_index].next);
        }
        if (!affected) {
            continue;
        }
        ProjectionLayerCopy& copy = storage.projections.emplace_back();
        copy.layer_index = layer_index;
        copy.layer = *projection;
        copy.views.assign(projection->views, projection->views + projection->viewCount);
        for (XrCompositionLayerProjectionView& view : copy.views) {
            while (names_private_depth_swapchain(view.next)) {
                view.next = static_cast<const XrBaseInStructure*>(view.next)->next;
            }
        }
        copy.layer.views = copy.views.data();
        storage.layers[layer_index] =
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&copy.layer);
    }
    storage.info.layers = storage.layers.data();
    return &storage.info;
}


// Validate the exact immutable guides retained with the released XR colours.
// Two views may share a packed/array stereo resource, but may not consume the
// same mono motion domain twice. No cross-eye or colour-only synthesis is used
// when the pair cannot be proved; the existing borrowed-frame owner presents it.
[[nodiscard]] FsrStereoGuideReason classify_fsr_stereo_guides(
    const ProjectionSnapshot& snapshot,
    std::span<const ProjectionResourceMapping> mappings) noexcept {
    if (snapshot.layers.empty()) return FsrStereoGuideReason::non_stereo_projection;
    for (std::size_t projection = 0; projection < snapshot.layers.size(); ++projection) {
        const auto& views = snapshot.layers[projection].views;
        if (views.size() != 2) return FsrStereoGuideReason::non_stereo_projection;
        std::array<std::shared_ptr<const xrfg::DlssMotionVectorFrame>, 2> eyes;
        for (const auto& mapping : mappings) {
            const auto swapchain = find_swapchain(mapping.application_swapchain);
            if (!swapchain) return FsrStereoGuideReason::missing_guides;
            std::scoped_lock lock(swapchain->mutex);
            const auto& guides = swapchain->last_released_motion_vectors;
            if (!guides) return FsrStereoGuideReason::missing_guides;
            for (std::size_t local = 0; local < mapping.views.size(); ++local) {
                const auto& reference = mapping.views[local];
                if (reference.projection_index != projection) continue;
                if (reference.view_index >= eyes.size()) return FsrStereoGuideReason::non_stereo_projection;
                // Same selection as record_fsr_pair: double-wide uses local
                // viewport order, array/split resources use their slice index.
                const auto index = swapchain->create_info.arraySize == 1 &&
                        guides->eye_count == mapping.views.size()
                    ? static_cast<std::uint32_t>(local)
                    : views[reference.view_index].subImage.imageArrayIndex;
                if (index >= guides->eye_count || index >= guides->eyes.size())
                    return FsrStereoGuideReason::missing_guides;
                eyes[reference.view_index] = guides->eyes[index];
            }
        }
        for (const auto& eye : eyes) {
            if (!eye || !eye->publication || !eye->stream || !eye->serial ||
                !eye->motion_vectors || !eye->motion_width || !eye->motion_height)
                return FsrStereoGuideReason::missing_guides;
            if (!eye->depth || !eye->depth_width || !eye->depth_height)
                return FsrStereoGuideReason::missing_depth;
            const auto valid_region = [](ID3D12Resource* resource, std::uint32_t x,
                std::uint32_t y, std::uint32_t width, std::uint32_t height,
                std::uint32_t slice) {
                const auto desc = resource->GetDesc();
                return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    slice < desc.DepthOrArraySize &&
                    static_cast<std::uint64_t>(x) + width <= desc.Width &&
                    static_cast<std::uint64_t>(y) + height <= desc.Height;
            };
            if (!valid_region(eye->motion_vectors.Get(), eye->motion_x, eye->motion_y,
                    eye->motion_width, eye->motion_height, eye->motion_slice) ||
                !valid_region(eye->depth.Get(), eye->depth_x, eye->depth_y,
                    eye->depth_width, eye->depth_height, 0))
                return FsrStereoGuideReason::invalid_guide_region;
        }
        const auto& a = *eyes[0];
        const auto& b = *eyes[1];
        const bool same_publication = a.stream == b.stream && a.epoch == b.epoch &&
            a.serial == b.serial && a.publication == b.publication;
        const bool overlapping_motion = a.motion_vectors.Get() == b.motion_vectors.Get() &&
            a.motion_slice == b.motion_slice &&
            static_cast<std::uint64_t>(a.motion_x) < static_cast<std::uint64_t>(b.motion_x) + b.motion_width &&
            static_cast<std::uint64_t>(b.motion_x) < static_cast<std::uint64_t>(a.motion_x) + a.motion_width &&
            static_cast<std::uint64_t>(a.motion_y) < static_cast<std::uint64_t>(b.motion_y) + b.motion_height &&
            static_cast<std::uint64_t>(b.motion_y) < static_cast<std::uint64_t>(a.motion_y) + a.motion_height;
        if (same_publication && overlapping_motion)
            return FsrStereoGuideReason::shared_eye_domain;
    }
    return FsrStereoGuideReason::ready;
}

XrResult layer_end_frame_impl(
    XrSession session,
    const XrFrameEndInfo* end_info, xrfg::InterfaceCallTiming* api_timing = nullptr) {
    const auto interface_entered_ns = api_timing ? api_timing->started_ns()
        : xrfg::interface_measurement_requested() ? xrfg::interface_clock_ns() : 0;
    xrfg::InterfaceCallTiming local_timing(api_timing ? 0 : interface_entered_ns, true);
    auto& interface_call = api_timing ? *api_timing : local_timing;
    const auto state = find_session(session);
    if (!state || state->dispatch->end_frame == nullptr) {
        return XR_ERROR_HANDLE_INVALID;
    }

    xrfg::InterfaceFrameStamp interface_stamp{
        state->interface_serial.fetch_add(1, std::memory_order_relaxed) + 1,
        interface_entered_ns,
        state->interface_begun_ns.exchange(0, std::memory_order_relaxed)};
    if(xrfg::native_ingress::luke_detected())if(const auto source=xrfg::query_luke_render_identity()) {
        interface_stamp.scene_owner=source->object;
        interface_stamp.scene_serial=source->render+1;
    }
    xrfg::autonomous::poll_diagnostic_logging_key();
    if(state->luke_fresh_only && state->frame_capture) {
        const bool save_down=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
        const bool toggle_down=save_down && (GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
        const bool diagnostics=xrfg::bridge_flight_logger().enabled() || xrfg::bridge_flight_logger().ram_enabled();
        const bool toggle=diagnostics && toggle_down && !state->diagnostic_ring_toggle_down;
        const bool save=save_down && !state->diagnostic_ring_save_down && !toggle_down;
        state->diagnostic_ring_toggle_down=toggle_down;state->diagnostic_ring_save_down=save_down;
        if (!diagnostics && state->frame_capture->ring_active()) state->frame_capture->stop_ring();
        if(toggle || state->frame_capture->busy()) {
            std::scoped_lock lock(state->gpu_mutex);
            if(toggle) {
                if(state->frame_capture->ring_active())state->frame_capture->stop_ring();
                else static_cast<void>(state->frame_capture->start_ring());
            } else if(save && state->frame_capture->ring_active())static_cast<void>(state->frame_capture->save_ring());
            state->frame_capture->tick(diagnostics);
        }
    }
    xrfg::autonomous::apply_runtime_settings();
    struct RuntimeReportScope {
        const std::shared_ptr<SessionState>& state;
        const XrFrameEndInfo* frame;
        xrfg::autonomous::SwapchainBudgetStatus budget{};
        ~RuntimeReportScope() {
            sample_benchmark_memory(*state);
            budget.fallback=state->luke_aux_budget_inline.load();
            xrfg::autonomous::publish_runtime_status(state->native_dlss_generated_frames,
            state->generation_status.load(std::memory_order_relaxed),
            state->generation_initialization_result.load(std::memory_order_relaxed),
            state->vulkan_binding.device != VK_NULL_HANDLE,
            state->interface_provider_error.load(std::memory_order_relaxed),&budget,
            state->private_runtime?(state->private_runtime->director_enabled()?(single_director_pacing(*state)?1:2):0):0,state->dispatch->steamvr_runtime,state->afw_only,
        state->private_runtime && state->luke_afw_enabled && state->luke_shared_swapchains,
        false, // no title forces the INI-only sharing policy
        state->dispatch->virtual_desktop_runtime,state->vdxr_legacy,xrfg::benchmark_eye_long_side(frame),session_uses_shared_queue(*state),state->private_runtime?static_cast<unsigned>(state->private_runtime->buffer_capacity()):0); }
    } runtime_report_scope{state,end_info};
    configure_session_guide_capture(*state);
    const std::uint32_t diagnostic_capture_frame =
        xrfg::two_frame_capture_begin_end_frame();
    xrfg::bridge_flight_logger().refresh_settings();
    struct CaptureFrameScope {
        std::uint32_t frame;
        ~CaptureFrameScope() {
            xrfg::two_frame_capture_finish_end_frame(frame);
        }
    } diagnostic_capture_scope{diagnostic_capture_frame};

    std::optional<std::uint32_t> luke_afw_released_eye;
    if ((state->graphics_binding_capabilities & 8ULL) != 0) {
        std::scoped_lock guide_lock(state->luke_guide_mutex);
        luke_afw_released_eye = state->luke_afw_released_eye;
        state->luke_afw_released_eye.reset();
        state->luke_frame_guide_resolved = false;
        state->luke_frame_motion_vectors.reset();
    }

    // The inline second cycle releases this lock before the next app wait.
    std::unique_lock frame_call_lock(state->frame_call_mutex);
    if(const auto now=GetTickCount64();now>=state->next_snapshot_retirement_ms) {
        state->next_snapshot_retirement_ms=now+100;
        xrfg::retire_disabled_dlss_motion_vector_resources();
    }
    runtime_report_scope.budget=luke_swapchain_inventory(state,end_info);
    observe_application_frame_thread(*state, true);
    release_pending_bridge_images(state);
    if (presenter_forbidden(*state)) {
        state->pipelined_presenter_start_requested = state->presenter_start_requested = false;
        state->pipelined_presenter_mode = false;
    }
    mark_application_release(state.get());
    { std::scoped_lock lock(state->binding_join_mutex);
      state->app_end_frame_release_value = state->app_release_counter; }
    PrivateDepthStrip private_depth_storage;
    end_info = strip_private_depth(*state, end_info, private_depth_storage);
    if (state->steamvr_delivery && end_info && end_info->layerCount &&
        state->application_frames_submitted < 100 &&
        ++state->application_frames_submitted == 100)
        state->steamvr_delivery->mark_established();
    log_compositor_delivery(*state);
    const auto application_end_now = std::chrono::steady_clock::now();
    const bool frame_had_overlapping_wait =
        state->application_frame_has_overlapping_wait;
    const bool application_frame_in_progress =
        state->application_frame_in_progress;
    const bool pipelined_presenter_mode = state->pipelined_presenter_mode;
    // Both promotions hand the presenter its first frame at the top of an
    // xrEndFrame, before the inline second cycle runs, so the handoff never
    // overlaps a frame this thread has already submitted.
    const bool pipelined_presenter_start_requested =
        state->pipelined_presenter_start_requested ||
        state->presenter_start_requested;
    const bool consume_in_submission_order =
        pipelined_presenter_mode || frame_had_overlapping_wait;
    const bool match_latest_by_submission =
        application_frame_in_progress && !consume_in_submission_order;
    if (interface_entered_ns) {
        const auto interface_frame = latest_pending_application_frame(state,
            end_info ? end_info->displayTime : 0, consume_in_submission_order, match_latest_by_submission);
        if (interface_frame && interface_frame->interface_cycle == state->interface_active_cycle.load()) {
            interface_stamp.cycle = interface_call.cycle = interface_frame->interface_cycle;
        }
        state->interface_active_cycle.store(nullptr);
    }
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

    const auto afw_control = xrfg::embedded::afw_snapshot();
    bool afw_selected = state->luke_afw_enabled &&
        afw_control.desired.mode != 0;
    const bool ofxr_was_enabled = state->menu_enabled;
    const auto desired_control = xrfg::embedded::snapshot();
    auto startup_request = state->luke_afw_startup.pending();
    if (!afw_selected || desired_control.desired.enabled) {
        state->luke_afw_startup.observe_failure();
        startup_request.reset();
    } else if (startup_request && !luke_afw_released_eye) {
        // The drained control lifecycle clears the held pair. Execute it
        // only alongside an already-committed fresh release so this cycle
        // can repopulate both eyes instead of falling back to raw AER.
        // Qualification stays armed; a held cycle consumes no attempt.
        startup_request.reset();
    }
    if (startup_request && xrfg::bridge_flight_logger().ram_enabled())
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            S_OK, 126,
            startup_request->stream, startup_request->epoch);
    const HRESULT control_result = apply_embedded_control(state, startup_request.has_value(), end_info);
    if (startup_request) {
        state->luke_afw_startup.complete(*startup_request, SUCCEEDED(control_result));
        if (xrfg::bridge_flight_logger().ram_enabled())
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_frame_contract,
                control_result, 127, startup_request->stream, startup_request->epoch);
    }
    if (ofxr_was_enabled && !state->menu_enabled && SUCCEEDED(control_result) &&
        afw_selected)
        state->luke_afw_startup_drained = state->luke_afw_last_delivered_startup_key;
    else if (state->menu_enabled || !afw_selected)
        state->luke_afw_startup_drained.reset();

    const auto pipeline_result=poll_djules_pipeline(state);
    if(XR_FAILED(pipeline_result)) return pipeline_result;

    // Only the synchronous AFW->XR Off path supplies startup observations.
    // The guard covers validation/bypass/failed-prepare early returns without
    // treating an async director handoff as a physically completed frame.
    struct StartupObservationScope {
        SessionState& state;
        std::unique_lock<std::mutex>& ownership;
        bool eligible;
        bool physical_success{};
        bool held{};
        std::optional<LukeAfwStartupCandidate> candidate;
        ~StartupObservationScope() {
            if (!eligible || !ownership.owns_lock()) return;
            if (!physical_success) {
                state.luke_afw_startup.observe_failure();
            } else if (candidate) {
                state.luke_afw_last_delivered_startup_key = candidate->key;
                if (state.luke_afw_startup_drained == candidate->key) {
                    state.luke_afw_startup.satisfy(candidate->key);
                    state.luke_afw_startup_drained.reset();
                } else {
                    state.luke_afw_startup_drained.reset();
                    state.luke_afw_startup.observe_fresh(candidate->key, candidate->publication);
                }
            } else if (!held) {
                state.luke_afw_startup.observe_failure();
            }
        }
    } startup_observation{*state, frame_call_lock, afw_selected && !state->menu_enabled};
    if (state->luke_afw_previous_selected &&
        *state->luke_afw_previous_selected != afw_selected) {
        // The two generators have distinct temporal anchors. A menu switch
        // must not pair either generator with the other one's last frame.
        clear_generation_continuity(state);
        {
            std::scoped_lock guide_lock(state->luke_guide_mutex);
            state->luke_afw_last_pair.reset();
            state->luke_afw_last_evaluated_publication = 0;
        }
        for (const auto& chain : find_swapchains(state)) {
            std::shared_ptr<xrfg::D3D12SwapchainHistory> final_history;
            {
                std::scoped_lock chain_lock(chain->mutex);
                if (chain->frame_generation)
                    final_history = chain->frame_generation->afw_final_history;
            }
            if (final_history) {
                std::scoped_lock gpu_lock(state->gpu_mutex);
                static_cast<void>(final_history->invalidate());
            }
        }
    }
    state->luke_afw_previous_selected = afw_selected;
    if(xrfg::diagnostic_ring_enabled)capture_application_metadata(state,end_info);
    if (state->luke_fresh_only && end_info && end_info->type == XR_TYPE_FRAME_END_INFO) {
        bool projection = false;
        std::optional<xrfg::LukeRenderIdentity> source;
        for (uint32_t i=0; end_info->layers && i<end_info->layerCount; ++i) {
            const auto* base=end_info->layers[i];
            if (!base || base->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
            projection=true;
            const auto* layer=reinterpret_cast<const XrCompositionLayerProjection*>(base);
            for(uint32_t eye=0; layer->views && eye<layer->viewCount; ++eye) {
                const auto chain=find_swapchain(layer->views[eye].subImage.swapchain);
                if(!chain)continue;
                std::scoped_lock lock(chain->mutex);
                const auto receipt=chain->luke_released_identity;
                if(receipt && (!source || (receipt->object==source->object && receipt->render>source->render))) source=receipt;
            }
        }
        // Menus/loading do not publish the selected AER source counter. Pass
        // their actual application images through until the pinned producer
        // proves that it is advancing. Keep the same physical cycle owner.
        if(projection && !state->luke_source_filter_armed && source) {
            const auto previous=state->luke_startup_source;
            if(previous && previous->object==source->object && source->render>previous->render) {
                state->luke_source_filter_armed=true;
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                    14, handle_value(session), source->render, 0);
            }
            state->luke_startup_source=source;
        }
        if(projection && !state->luke_source_filter_armed) {
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                15, handle_value(session), source ? source->render : 0,
                (afw_selected ? 1 : 0) | (state->menu_enabled ? 2 : 0));
        }
        if(projection && state->luke_source_filter_armed) {
            const bool duplicate=xrfg::luke_hfw_source_is_duplicate(
                source,state->luke_last_source,afw_selected,luke_afw_released_eye);
            if(xrfg::diagnostic_ring_enabled)xrfg::diagnostic_ring_record("{\"kind\":\"source_admission\",\"qpc\":"+
                std::to_string(xrfg::diagnostic_ring_qpc())+",\"render\":"+std::to_string(source?source->render:0)+
                ",\"display_time\":"+std::to_string(end_info->displayTime)+",\"dropped\":"+(duplicate?"true":"false")+"}");
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                duplicate ? (source ? 11 : 12) : 10, handle_value(session), source ? source->render : 0,
                (afw_selected ? 1 : 0) | (state->menu_enabled ? 2 : 0));
            if(duplicate) {
                consume_application_frame(state,end_info->displayTime,
                    consume_in_submission_order,match_latest_by_submission);
                return XR_SUCCESS;
            }
            if(state->luke_last_source && (source->object!=state->luke_last_source->object || source->render<state->luke_last_source->render))
                clear_generation_continuity(state);
            state->luke_last_source=source;
        }
    }
    if(!xrfg::diagnostic_ring_enabled)capture_application_metadata(state,end_info);
    const bool manually_disarmed = state->manual_control.stop_requested();
    const bool presenter_route_mismatch = use_continuous_presenter &&
        (three_slot_generation_session(state) != state->three_slot_director ||
         fsr_two_slot_session(state) != state->fsr_two_slot_presenter ||
         queued_nvidia_session(state) != state->queued_nvidia_presenter);
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
        state->presenter_restore_after_bypass=false;
        if (state->fps_overlay) state->fps_overlay->suspend();
    }
    if (!manually_disarmed) {
        retire_unused_composition_outputs(state,end_info);
        if (state->fps_overlay) {
            std::scoped_lock gpu_lock(state->gpu_mutex);
            state->fps_overlay->application_frame(end_info);
        }
    }

    const auto submit_borrowed_to_presenter = [&]() -> XrResult {
        auto request = enqueue_presenter_submission(state, nullptr, end_info, interface_stamp);
        if (presenter_content_lock.owns_lock()) {
            presenter_content_lock.unlock();
        }
        return wait_for_presenter_submission(state, request);
    };
    if (presenter_forbidden(*state) && use_continuous_presenter) {
        // A loop may change threads after promotion. Finish this virtual app
        // frame through its existing physical owner before draining that owner;
        // a direct downstream EndFrame here would have no matching BeginFrame.
        const auto result = submit_borrowed_to_presenter();
        if (XR_SUCCEEDED(result) && end_info)
            consume_application_frame(state, end_info->displayTime,
                consume_in_submission_order, match_latest_by_submission);
        stop_continuous_presenter(state);
        clear_generation_continuity(state);
        return result;
    }
    const auto start_requested_pipelined_presenter =
        [&](std::shared_ptr<GeneratedFrameEndInfo> seed_frame) -> XrResult {
            // An auxiliary allocation refusal may have selected inline mode
            // after this EndFrame captured its earlier promotion request.
            if (presenter_forbidden(*state)) {
                state->pipelined_presenter_start_requested=false;
                state->presenter_start_requested=false;
                state->presenter_restore_after_bypass=false;
                return XR_SUCCESS;
            }
            if (!pipelined_presenter_start_requested) {
                return XR_SUCCESS;
            }
            if (three_slot_generation_session(state)) {
                // One ownership rendezvous at promotion, not a steady-state
                // producer gate. The director cannot inherit an unfinished
                // inline prime/deferred endpoint: its first worker submission
                // would otherwise flush that copy and immediately see BUSY.
                // No GPU/presenter/swapchain call lock is held during this wait.
                if (FAILED(wait_for_previous_session_synthesis(state))) {
                    return XR_ERROR_RUNTIME_FAILURE;
                }
            }
            if (!start_continuous_presenter(
                    state,
                    std::move(seed_frame),
                    true)) {
                return XR_ERROR_RUNTIME_FAILURE;
            }
            state->pipelined_presenter_start_requested = false;
            state->presenter_start_requested = false;
            state->presenter_restore_after_bypass=false;
            return wait_for_presenter_idle(state);
        };
    const auto bypass_generation =
        [&](GenerationPrepareReason reason, bool keep_generation_presenter = false) -> XrResult {
            using GS = xrfg::autonomous::GenerationState;
            if (reason == GenerationPrepareReason::synthesis_busy)
                state->generation_status = GS::busy;
            else if (reason == GenerationPrepareReason::missing_capture)
                state->generation_status = GS::waiting;
            else if (reason == GenerationPrepareReason::manual_disarmed)
                state->generation_status = GS::paused;
            else if (reason != GenerationPrepareReason::fsr_stereo_unavailable &&
                reason != GenerationPrepareReason::fsr_stereo_recovering &&
                reason != GenerationPrepareReason::native_stereo_unavailable)
                state->generation_status = GS::waiting;
            if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3)
                state->fsr_cadence.bypass(static_cast<std::int64_t>(reason));
            update_luke_application_clock(*state, false, false);
            // A borrowed frame advances source capture serials without advancing
            // synthesis history. Retire the old anchor BEFORE draining/taking
            // the content lock, so the next valid frame primes instead of
            // pairing across a missing serial and entering quarantine.
            bool has_anchor;
            { std::scoped_lock lock(state->mutex); has_anchor = state->previous_projection.has_value(); }
            if (has_anchor) clear_generation_continuity(state);
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
            XrFrameEndInfo physical_slot{};
            const auto* physical_end=use_continuous_presenter?end_info:
                blackbar_inline_submission(*state,end_info,physical_slot);
            const XrResult end_result = use_continuous_presenter
                ? submit_borrowed_to_presenter()
                : with_runtime_entry(state, [&] {
                      join_runtime_queue_to_application(state.get(), state->app_end_frame_release_value);
                      return state->fps_overlay
                          ? state->fps_overlay->end_frame(physical_end, false, true, {}, 0, interface_stamp)
                          : content_end_frame(session, physical_end);
                  });
            xrfg::bridge_flight_logger().end(
                end_token,
                xrfg::BridgeFlightOperation::downstream_first_end_frame,
                end_result,
                0,
                0,
                0);
            if(XR_SUCCEEDED(end_result)) blackbar_unpaired(*state,use_continuous_presenter);
            if (end_info != nullptr) {
                consume_application_frame(
                    state,
                    end_info->displayTime,
                    consume_in_submission_order,
                    match_latest_by_submission);
            }
            if (XR_SUCCEEDED(end_result) && !use_continuous_presenter) {
                const XrResult start_result =
                    start_requested_pipelined_presenter(nullptr);
                if (XR_FAILED(start_result)) {
                    return start_result;
                }
            }
            if (use_continuous_presenter &&
                ((!pipelined_presenter_mode && !keep_generation_presenter) || presenter_route_mismatch ||
                 presenter_forbidden(*state))) {
                state->presenter_restore_after_bypass =
                    ordinary_djules_route(*state) && state->vdxr_legacy &&
                    reason != GenerationPrepareReason::manual_disarmed && !state->manual_stop_applied;
                if (ordinary_djules_route(*state) && state->vdxr_legacy)
                    xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                        603,static_cast<std::uint64_t>(reason),state->presenter_restore_after_bypass?1:0,state->split_frame_loop?1:0);
                stop_continuous_presenter(state);
                state->pipelined_presenter_start_requested = false;
                state->presenter_start_requested = false;
                if (pipelined_presenter_mode && !presenter_forbidden(*state)) {
                    // The application's outstanding Wait/Begin tickets are virtual.
                    // Finish the old physical cycle above, join its owner, and
                    // start the selected owner before returning this boundary.
                    // Even Off keeps a borrowed-frame presenter for a pipelined
                    // app: those tickets cannot become unmatched runtime Begins.
                    if (!start_continuous_presenter(state, nullptr, true))
                        return XR_ERROR_RUNTIME_FAILURE;
                } else {
                    state->pipelined_presenter_mode = false;
                    if (state->application_wait_pending_begin &&
                        !state->runtime_frame_waited_unbegun)
                        state->application_begin_needs_wait = true;
                }
                std::scoped_lock lock(state->mutex);
                state->steamvr_throttled_wait_streak = 0;
                // Both promotion routes start over, or the demotion would be
                // undone by the next frame that measures the same runtime.
                state->unpaced_wait_streak = 0;
            }
            return end_result;
        };

    if(state->luke_fresh_only && !state->luke_source_filter_armed)
        return bypass_generation(GenerationPrepareReason::missing_capture);

    // Menu backend changes can happen after a presenter already exists. End
    // this virtual application frame through its current owner before stopping
    // it; the next promotion selects the appropriate backend's director. Never
    // hand a synthesis job to an old presenter with no worker thread.
    if (use_continuous_presenter) {
        const bool desired_director = three_slot_generation_session(state);
        bool route_changed = false;
        {
            std::scoped_lock lock(state->presenter_mutex);
            route_changed = desired_director != state->three_slot_director ||
                fsr_two_slot_session(state) != state->fsr_two_slot_presenter ||
                queued_nvidia_session(state) != state->queued_nvidia_presenter;
        }
        if (route_changed)
            return bypass_generation(GenerationPrepareReason::presenter_route_changed);
    }
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
    if (manually_disarmed ||
        ((generation_cooling_down || !state->menu_enabled) &&
          !afw_selected)) {
        return bypass_generation(manually_disarmed
                ? GenerationPrepareReason::manual_disarmed
                : structural_quarantine_active
                    ? GenerationPrepareReason::structural_quarantine_active
                    : GenerationPrepareReason::cooldown_active);
    }

    ProjectionSnapshot current_snapshot{};
    const bool has_projection =
        capture_projection_snapshot(end_info, &current_snapshot);
    current_snapshot.app_release_value = state->app_end_frame_release_value;
    current_snapshot.interface_stamp = interface_stamp;
    current_snapshot.blackbar_pose=blackbar_pose_time(*state,*end_info);
    ProjectionMappingResult resource_mappings{};
    if (has_projection) {
        resource_mappings = build_projection_resource_mappings(current_snapshot);
        log_projection_view_rects(*state, current_snapshot);
        if (!state->luke_afw_enabled) note_projection_eyes(current_snapshot, resource_mappings);
        refresh_release_bound_projection_guides(*state, resource_mappings);
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
    // The first frame that names a swapchain as a projection view is where the
    // deferred generation resources are taken - but taking them here, between
    // the application's xrEndFrame and the submission that follows it, puts
    // the cost of creating them on the frame's own deadline. Synthesizer
    // initialization measured 80 ms and 46 ms in one captured session, which
    // is seven display periods: the submission left 92 ms after the
    // application entered xrEndFrame, the runtime rejected its display time
    // with XR_ERROR_TIME_INVALID, and the failure quarantined generation for a
    // second. Generation then engaged several seconds into the session instead
    // of immediately.
    //
    // So the frame that triggers arming passes through, and the arming happens
    // after its submission has gone out. It costs one frame at world load
    // rather than a rejected submission and a second of outage, and the next
    // frame generates.
    const std::span<const ProjectionResourceMapping> projection_mappings(
        resource_mappings.mappings.data(),
        resource_mappings.mappings.size());
    if(state->luke_outlaws_preframe&&resource_mappings.ready()) {
        // Eager histories no longer pass through lazy arming. Color identity
        // still uses the actual submitted projection, never enum order/shape.
        for(const auto& mapping:projection_mappings) {
            const auto chain=find_swapchain(mapping.application_swapchain);
            if(chain&&chain->create_info.arraySize==1)
                xrfg::configure_luke_color_origin_extent(chain->create_info.width,chain->create_info.height);
        }
    }
    const bool generation_arming_pending =
        resource_mappings.ready() &&
        projection_frame_generation_pending(state, projection_mappings);
    const bool stereo_projection = !current_snapshot.layers.empty() &&
        std::all_of(current_snapshot.layers.begin(), current_snapshot.layers.end(),
            [](const auto& layer) { return layer.views.size() == 2; });
    const auto report_guide_reason = [&](FsrStereoGuideReason reason) {
        using GS = xrfg::autonomous::GenerationState;
        state->generation_status = reason == FsrStereoGuideReason::missing_guides ? GS::missing_motion :
            reason == FsrStereoGuideReason::missing_depth ? GS::missing_depth :
            reason == FsrStereoGuideReason::recovering ? GS::recovering :
            reason == FsrStereoGuideReason::invalid_guide_region ? GS::invalid_guides : GS::invalid_stereo;
    };
    // Native FG has the same exact stereo guide requirement as FSR. Refuse
    // missing guides before private image acquisition, priming or worker handoff.
    // Continue submitting the whole real frame through its existing XR owner.
    if (!afw_selected && state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::native_dlss &&
        (!stereo_projection || resource_mappings.ready())) {
        const auto reason = stereo_projection ? classify_fsr_stereo_guides(current_snapshot, projection_mappings)
            : FsrStereoGuideReason::non_stereo_projection;
        if (reason != FsrStereoGuideReason::ready) {
            report_guide_reason(reason);
            return bypass_generation(GenerationPrepareReason::native_stereo_unavailable, true);
        }
    }
    if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3 &&
        (!stereo_projection || (resource_mappings.ready() && !generation_arming_pending))) {
        auto& admission = state->fsr_stereo_admission;
        const auto previous_reason = admission.reason;
        const bool admitted = admission.observe(stereo_projection
            ? classify_fsr_stereo_guides(current_snapshot, projection_mappings)
            : FsrStereoGuideReason::non_stereo_projection);
        if (admission.reason != previous_reason)
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::fsr_recovery,
                static_cast<std::int64_t>(admission.reason), 300,
                static_cast<std::uint64_t>(current_snapshot.display_time), admission.suspended);
        if (!admitted) {
            report_guide_reason(admission.reason);
            const bool recovering = admission.reason == FsrStereoGuideReason::recovering;
            xrfg::report_fsr_frame_generation_status(recovering
                ? xrfg::FsrFrameGenerationStatus::stereo_recovering
                : xrfg::FsrFrameGenerationStatus::stereo_unavailable);
            return bypass_generation(recovering
                ? GenerationPrepareReason::fsr_stereo_recovering
                : GenerationPrepareReason::fsr_stereo_unavailable);
        }
        if (previous_reason != FsrStereoGuideReason::ready)
            xrfg::report_fsr_frame_generation_status(xrfg::FsrFrameGenerationStatus::waiting_for_guides);
    } else if (state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3) {
        state->fsr_stereo_admission = {};
    }
    if (!resource_mappings.ready() || generation_arming_pending) {
        clear_generation_continuity(state);
        if (!resource_mappings.ready() &&
            resource_mappings.reason !=
                ProjectionMappingReason::no_projection_views) {
            schedule_generation_quarantine(
                state,
                GenerationQuarantineReason::projection_mapping_failed,
                (static_cast<std::uint64_t>(resource_mappings.reason) << 56) |
                    (resource_mappings.detail & 0x00FFFFFFFFFFFFFFULL));
        }
        const XrResult passthrough_result =
            bypass_generation(GenerationPrepareReason::empty_mappings);
        if (!generation_arming_pending) {
            return passthrough_result;
        }
        // The application's frame is already downstream; nothing this costs is
        // on its deadline any more.
        if (continuous_presenter_active(state)) {
            const auto idle=wait_for_presenter_idle(state);
            if (XR_FAILED(idle)) return idle;
            presenter_content_lock=std::unique_lock<std::mutex>(state->presenter_content_mutex);
        }
        { std::scoped_lock lock(state->presenter_mutex);
          state->presenter_last_frame.reset(); }
        if (!ensure_projection_frame_generation(state, projection_mappings)) {
            // A refusal means the runtime has no swapchains left for the
            // application either. A queued submission names a private
            // swapchain, so the presenter must be idle and its content lock
            // held before one is destroyed.
            const XrResult exclusive_result = enter_presenter_exclusive();
            if (XR_FAILED(exclusive_result)) {
                return exclusive_result;
            }
            release_session_generation_budget(state);
        }
        return passthrough_result;
    }
    const std::optional<PendingApplicationFrame> application_frame =
        latest_pending_application_frame(
            state,
            current_snapshot.display_time,
            consume_in_submission_order,
            match_latest_by_submission);
    const std::optional<XrDuration> application_display_period =
        application_frame
            ? std::optional<XrDuration>{application_frame->display_period}
            : std::nullopt;
    const bool latest_application_frame = application_frame.has_value();
    const std::uint64_t frame_serial = application_frame
        ? application_frame->serial : 0;
    if (state->luke_afw_enabled) {
        // result 2: application end matched to the wait ticket. c packs the
        // F10 capture index and whether the matching ticket was found.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            2, frame_serial,
            static_cast<std::uint64_t>(current_snapshot.display_time),
            (static_cast<std::uint64_t>(diagnostic_capture_frame) << 32) |
                (latest_application_frame ? 1ULL : 0ULL));
        xrfg::two_frame_capture_timeline(
            "frame", 2, frame_serial,
            static_cast<std::uint64_t>(current_snapshot.display_time),
            (static_cast<std::uint64_t>(diagnostic_capture_frame) << 32) |
                (latest_application_frame ? 1ULL : 0ULL));
        log_luke_projection_poses(end_info, frame_serial, 1);
        const bool f11_down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11_down && !state->luke_f11_down) {
            // result 9 marks the observed state. No F10 GPU capture, fence
            // wait, pose change, or runtime-slot change occurs here.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_frame_contract,
                9, frame_serial,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                0);
        }
        state->luke_f11_down = f11_down;
    }
    const bool source_order_timeline = capture_order_timeline(*state);
    if (source_order_timeline)
        record_projection_capture_order(current_snapshot, projection_mappings);
    std::optional<ProjectionSnapshot> previous_snapshot;
    {
        std::scoped_lock lock(state->mutex);
        previous_snapshot = state->previous_projection;
    }
    const bool consecutive_captures = source_order_timeline && previous_snapshot &&
        consecutive_projection_captures(*previous_snapshot, current_snapshot);
    const bool snapshots_compatible =
        !previous_snapshot ||
        projection_snapshots_compatible(*previous_snapshot, current_snapshot, consecutive_captures);
    if (previous_snapshot && snapshots_compatible && consecutive_captures &&
        current_snapshot.display_time <= previous_snapshot->display_time) {
        // 754: backwards/equal application prediction accepted by capture
        // identity. Preserve original poses/times and output-slot ownership.
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            754, static_cast<std::uint64_t>(previous_snapshot->display_time),
            static_cast<std::uint64_t>(current_snapshot.display_time),
            current_snapshot.interface_stamp.serial);
    }
    bool metadata_pairable =
        latest_application_frame && previous_snapshot && snapshots_compatible;
    const bool use_three_slot = use_continuous_presenter && three_slot_generation_session(state);
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
    if (state->luke_afw_enabled) {
        // A bare gameplay key must not silently tint AFW's generated frames.
        // The explicit debug chord updates the same profile shown by the UI.
        const bool f6_down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
            (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6_down && !state->luke_afw_f6_down) {
            const auto base = xrfg::autonomous::snapshot().desired;
            auto edited = base; edited.afw_debug = !base.afw_debug;
            const bool accepted = xrfg::autonomous::request_changes(base, edited);
            if (accepted) xrfg::autonomous::apply_runtime_settings();
            const auto debug = xrfg::embedded::afw_snapshot().desired.debug;
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_afw_stage,
                accepted ? (debug ? 1 : 0) : E_FAIL,
                24, debug ? 1 : 0, 0);
        }
        state->luke_afw_f6_down = f6_down;
    }
    // AFW Off does not override OFXR's independent menu switch. With both
    // switches Off, the early bypass above presents Luke's original AER frame.
    if (state->luke_afw_enabled) {
        const std::uint64_t flags =
            (afw_selected ? 1ULL : 0ULL) |
            (state->menu_enabled ? 2ULL : 0ULL) |
            (state->menu_enabled && !generation_cooling_down
                ? 4ULL : 0ULL) |
            (generation_cooling_down ? 8ULL : 0ULL) |
            (static_cast<std::uint64_t>(afw_control.desired.mode) << 8) |
            (static_cast<std::uint64_t>(state->frame_generation_mode) << 16);
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            6, frame_serial, flags, afw_control.revision);
    }
    PreparedProjectionFrame afw_produced;
    GeneratedFrameEndInfo afw_terminal_submission;
    ProjectionSnapshot afw_terminal_snapshot;
    std::shared_ptr<const CompletedAfwFrame> completed_afw;
    std::vector<PostAfwFrameInput> inputs;
    if (afw_selected) {
        // ONE producer, before selecting the consumer. In particular no OFXR
        // worker/director capacity wait precedes AFW production.
        const bool capture_temporal_input = state->menu_enabled &&
            !generation_cooling_down &&
            state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3;
        afw_produced = prepare_luke_afw_frame(state, current_snapshot,
            projection_mappings, luke_afw_released_eye, frame_serial,
            capture_temporal_input, inputs,end_info);
        if (state->fps_overlay)
            state->fps_overlay->set_afw_active(afw_produced.kind != PreparedGenerationKind::none);
        xrfg::embedded::report_afw_prepare(static_cast<std::int64_t>(afw_produced.reason));
        if(state->luke_warmup_held&&afw_produced.kind!=PreparedGenerationKind::none&&!afw_produced.held_afw_pair) {
            // A new scene must prime OFXR; it cannot interpolate with the old
            // pair that was held while native camera history was rebuilding.
            clear_generation_continuity(state);metadata_pairable=false;
            state->luke_warmup_held=false;state->luke_warmup_started=0;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,0,1221,frame_serial,0);
        }
        if (afw_produced.kind == PreparedGenerationKind::none) {
            // These are the untouched application colors, not AFW output.
            // Keep an already proven CPU identity; otherwise use BOTH exact
            // CRCs read from this stereo pair solely for real-frame metrics.
            if (afw_produced.original_scene &&
                (!interface_stamp.scene_owner || !interface_stamp.scene_serial)) {
                interface_stamp.scene_owner=afw_produced.original_scene.owner;
                interface_stamp.scene_serial=afw_produced.original_scene.serial;
                current_snapshot.interface_stamp=interface_stamp;
            }
            const bool missing_afw_input =
                afw_produced.reason == GenerationPrepareReason::missing_generation ||
                afw_produced.reason == GenerationPrepareReason::missing_capture ||
                afw_produced.reason == GenerationPrepareReason::afw_input_unavailable ||
                afw_produced.reason == GenerationPrepareReason::synthesis_stale_guides;
            if (!luke_color_only_ofxr(*state) || !missing_afw_input || generation_cooling_down)
                return bypass_generation(afw_produced.reason);
            // AFW released its scratch acquisitions before returning. Continue
            // through the existing raw-XR OFXR path; never invent native guides.
            afw_selected = false;
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_afw_stage,
                static_cast<std::int64_t>(afw_produced.reason), 1004, frame_serial, 0);
        }
    }
    if (select_luke_ofxr_input(state, afw_selected)) {
        previous_snapshot.reset();
        metadata_pairable = false;
    }
    if (afw_selected) {
        if (state->luke_fresh_only && afw_produced.held_afw_pair) {
            startup_observation.held = true;
            consume_application_frame(state,end_info->displayTime,
                consume_in_submission_order,match_latest_by_submission);
            return XR_SUCCESS;
        }
        // Build the validated AFW->XR output once, before selecting its sink.
        // Direct delivery and OFXR must consume this very same projection,
        // rather than independently remapping the original AER submission.
        if (!build_afw_terminal_submission(end_info, current_snapshot, afw_produced,
                afw_terminal_submission, afw_terminal_snapshot))
            return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        if (xrfg::two_frame_capture_images_active())
            log_luke_projection_poses(&afw_terminal_submission.info, frame_serial, 11);
        startup_observation.candidate = afw_produced.startup_candidate;
        startup_observation.held = afw_produced.held_afw_pair != nullptr;
        if (!inputs.empty()) {
            completed_afw = freeze_completed_afw_frame(state, &afw_terminal_submission.info,
                afw_terminal_snapshot, std::move(inputs), frame_serial);
            if (!completed_afw)
                return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        }
    }
    if (use_three_slot) {
        if (!afw_selected) {
            // Release-time capture is deliberately nonblocking. A busy history
            // ring leaves no ticket for that eye; admit the whole stereo frame
            // through the existing real-frame path before creating a worker job.
            // That path drains/retire the old temporal anchor and preserves the
            // runtime frame-call owner. Never synthesize from the other eye alone
            // or let transient capture backpressure become a presenter failure.
            bool missing_capture = false;
            for (const auto& mapping : projection_mappings) {
                const auto chain = find_swapchain(mapping.application_swapchain);
                if (!chain) continue;
                std::scoped_lock lock(chain->mutex);
                missing_capture |= !chain->last_released_capture.has_value();
            }
            if (missing_capture)
                return bypass_generation(GenerationPrepareReason::missing_capture, true);
        }
        if (!completed_afw && !composition_can_be_owned(state, end_info))
            return bypass_generation(GenerationPrepareReason::presenter_unsafe_composition);
        // In AFW the peer is reconstructed, not a second AER source release.
        // The original Luke pair filter cannot classify that completed pair.
        // Raw AER can release different render identities for the two eyes.
        // The optical consumer owns the exact XR captures, not a native SR
        // pair. Preserve the separately pinned HFW source-filter contract.
        const bool raw_luke_optical = !state->luke_fresh_only && luke_color_only_ofxr(*state);
        const auto luke = afw_selected || raw_luke_optical ? LukePairDecision{} : luke_pair_decision(projection_mappings);
        update_luke_application_clock(*state, afw_selected || raw_luke_optical || luke.identity.has_value() || luke.action == LukePairDecision::Action::drop,
            luke.action == LukePairDecision::Action::drop);
        if (luke.reset) {
            clear_generation_continuity(state);
            metadata_pairable = false;
        }
        if (luke.action == LukePairDecision::Action::drop && latest_application_frame) {
            { std::scoped_lock lock(state->presenter_mutex);
              if (XR_FAILED(state->presenter_failure)) return state->presenter_failure;
              if (state->presenter_stop_requested) return XR_ERROR_SESSION_NOT_RUNNING; }
            // This is a virtual app cycle; the director alone owns runtime
            // Wait/Begin/End. No borrowed repeat and no new synthesis job.
            consume_application_frame(state, current_snapshot.display_time,
                                      consume_in_submission_order,
                                      match_latest_by_submission);
            xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                luke.reset ? 2 : 1, luke.identity ? luke.identity->render : ~0ULL,
                luke.identity ? luke.identity->object : 0, current_snapshot.display_time);
            return XR_SUCCESS;
        }
        if (luke.action == LukePairDecision::Action::drop)
            return bypass_generation(GenerationPrepareReason::empty_mappings);
        current_snapshot.luke_identity = luke.identity;
        if (afw_selected) {
            if (!completed_afw) {
                // Held pair: no fresh real/synthetic job and no borrowed source.
                update_luke_application_clock(*state, true, false);
                consume_application_frame(state, current_snapshot.display_time,
                    consume_in_submission_order, match_latest_by_submission);
                // Keep the existing duplicate-cycle bound, but put its wait
                // at the end of production, never in the next game's WaitFrame.
                std::unique_lock lock(state->presenter_mutex);
                const auto period = std::clamp<XrDuration>(
                    state->presenter_frame_state.predictedDisplayPeriod, 2'000'000, 50'000'000);
                state->presenter_condition.wait_until(lock,
                    state->application_wait_returned_at + std::chrono::nanoseconds(period), [&] {
                        return state->presenter_stop_requested || XR_FAILED(state->presenter_failure) ||
                            !state->menu_enabled || state->manual_control.stop_requested();
                    });
                return XR_FAILED(state->presenter_failure) ? state->presenter_failure
                    : state->presenter_stop_requested ? XR_ERROR_SESSION_NOT_RUNNING : XR_SUCCESS;
            }
        }
        const XrResult handoff = handoff_three_slot_frame(
            state, end_info, current_snapshot, projection_mappings, metadata_pairable,
            completed_afw);
        if (XR_SUCCEEDED(handoff)) {
            commit_luke_pair(projection_mappings, luke.identity);
            if (luke.identity)
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::luke_filter,
                    luke.reset ? 3 : 0, luke.identity->render, luke.identity->object,
                    current_snapshot.display_time);
            consume_application_frame(state, current_snapshot.display_time,
                                      consume_in_submission_order,
                                      match_latest_by_submission);
            std::scoped_lock lock(state->mutex);
            state->previous_projection = current_snapshot;
            state->generation_failure_streak = 0;
        }
        if(XR_SUCCEEDED(handoff)) { frame_call_lock.unlock(); delay_djules_application_release(state); }
        return handoff;
    }
    if (use_continuous_presenter && !state->luke_afw_enabled) {
        const auto admission=wait_for_native_preparation_admission(state,metadata_pairable);
        if(XR_FAILED(admission))return admission;
    }
    // For the remaining inline backends the synthetic is displayed one display period before the current frame,
    // so it should show the scene as it was one period before the current
    // capture - which is halfway between the two captures only when they are
    // two display periods apart, that is when the application is running at
    // exactly half the display rate. Away from that the fixed midpoint places
    // the synthetic at the wrong instant, and because the error tracks the
    // application's frame interval it changes every frame rather than being a
    // constant offset nobody would see. Alternating early and late is what
    // reads as judder.
    const float interpolation_fraction = synthetic_interpolation_fraction(
        state, previous_snapshot, current_snapshot);
    const auto mapping_span = std::span<const ProjectionResourceMapping>(
        resource_mappings.mappings.data(),
        resource_mappings.mappings.size());
    const auto extra_fraction=synthetic_interpolation_fraction(state,previous_snapshot,current_snapshot,1);
    const bool r19_camera=xrfg::autonomous::r19_camera_enabled.load(std::memory_order_acquire);
    const auto synthetic_camera=previous_snapshot && r19_camera ? synthetic_camera_snapshot(*previous_snapshot,current_snapshot,interpolation_fraction) : std::nullopt;
    const auto extra_camera=previous_snapshot && r19_camera ? synthetic_camera_snapshot(*previous_snapshot,current_snapshot,extra_fraction) : std::nullopt;
    const xrfg::D3D12PresentationTransform final_color_transform{};
    PreparedProjectionFrame prepared = afw_selected
        ? completed_afw && state->menu_enabled && !generation_cooling_down &&
            state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3
            ? prepare_projection_frame(current_snapshot, mapping_span,
                metadata_pairable, interpolation_fraction, false,
                &final_color_transform, completed_afw->inputs, extra_fraction,
                synthetic_camera ? &*synthetic_camera : nullptr, extra_camera ? &*extra_camera : nullptr)
            : std::move(afw_produced)
        : prepare_projection_frame(
              current_snapshot, mapping_span, metadata_pairable, interpolation_fraction, false, nullptr, {},
              extra_fraction, synthetic_camera ? &*synthetic_camera : nullptr, extra_camera ? &*extra_camera : nullptr);
    state->generation_status = prepared.kind == PreparedGenerationKind::pair
        ? xrfg::autonomous::GenerationState::ready : prepared.kind == PreparedGenerationKind::prime
        ? xrfg::autonomous::GenerationState::recovering : xrfg::autonomous::GenerationState::waiting;
    if (state->fps_overlay) {
        state->fps_overlay->set_afw_active(
            afw_selected && prepared.kind != PreparedGenerationKind::none);
    }

    if ((state->graphics_binding_capabilities & 8ULL) != 0) {
        for (const ProjectionResourceMapping& mapping :
             resource_mappings.mappings) {
            if (mapping.views.empty()) continue;
            const ProjectionViewReference& reference = mapping.views.front();
            if (reference.projection_index >= current_snapshot.layers.size() ||
                reference.view_index >= current_snapshot.layers[
                    reference.projection_index].views.size()) {
                continue;
            }
            const auto& view = current_snapshot.layers[
                reference.projection_index].views[reference.view_index];
            const auto swapchain_state = find_swapchain(
                mapping.application_swapchain);
            std::int64_t logical_eye = -1;
            std::uint64_t capture_serial = 0;
            if (swapchain_state) {
                std::scoped_lock lock(swapchain_state->mutex);
                if (swapchain_state->dlss_logical_eye_index) {
                    logical_eye = static_cast<std::int64_t>(
                        *swapchain_state->dlss_logical_eye_index);
                }
                if (swapchain_state->last_released_capture) {
                    capture_serial =
                        swapchain_state->last_released_capture->serial;
                }
            }
            if (diagnostic_capture_frame != 0 && capture_serial != 0) {
                xrfg::two_frame_capture_annotate_projection(
                    diagnostic_capture_frame,
                    handle_value(mapping.application_swapchain), capture_serial,
                    static_cast<std::uint64_t>(current_snapshot.display_time),
                    static_cast<std::int32_t>(reference.view_index),
                    view.subImage.imageArrayIndex,
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.offset.x)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.offset.y)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.extent.width)),
                    static_cast<std::uint32_t>(
                        std::max(0, view.subImage.imageRect.extent.height)));
            }
            // result=logical eye; a=display time; b=application swapchain;
            // c=(projection index, view index, array slice, capture serial).
            const std::uint64_t packed_view =
                ((static_cast<std::uint64_t>(reference.projection_index) &
                  0xffULL) << 56) |
                ((static_cast<std::uint64_t>(reference.view_index) &
                  0xffULL) << 48) |
                ((static_cast<std::uint64_t>(view.subImage.imageArrayIndex) &
                  0xffULL) << 40) |
                (capture_serial & 0xffffffffffULL);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_projection_contract,
                logical_eye,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                handle_value(mapping.application_swapchain),
                packed_view);
        }
    }
    if (use_continuous_presenter && !state->luke_afw_enabled) {
        presenter_content_lock =
            std::unique_lock<std::mutex>(state->presenter_content_mutex);
    }
    GenerationPrepareReason prepare_reason = prepared.reason;
    XrSwapchain failed_prepare_swapchain = prepared.failed_swapchain;
    if (!afw_selected &&
        prepared.kind == PreparedGenerationKind::none &&
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
    GeneratedFrameEndInfo current_generated{},extra_generated{};
    const XrFrameEndInfo* produced_end_info = completed_afw
        ? &completed_afw->submission->info : afw_selected ? &afw_terminal_submission.info : end_info;
    const ProjectionSnapshot& produced_snapshot = completed_afw
        ? completed_afw->snapshot : afw_selected ? afw_terminal_snapshot : current_snapshot;
    const XrFrameEndInfo* submitted_end_info = end_info;
    bool pair_ready = false;
    std::vector<ProjectionResourceDestination> current_destinations;
    std::vector<ProjectionResourceDestination> synthetic_destinations;
    current_destinations.reserve(prepared.resources.size());
    synthetic_destinations.reserve(prepared.resources.size());
    for (const PreparedProjectionResource& resource : prepared.resources) {
        XrSwapchain produced_swapchain = resource.application_swapchain;
        if (completed_afw) {
            const auto& inputs = completed_afw->inputs;
            const auto input = std::find_if(inputs.begin(), inputs.end(),
                [&](const auto& value) { return value.application_swapchain == produced_swapchain; });
            if (input == inputs.end()) return XR_ERROR_LAYER_INVALID;
            produced_swapchain = input->completed_swapchain;
        }
        current_destinations.push_back({
            produced_swapchain,
            resource.generation.current_handle,
        });
        synthetic_destinations.push_back({
            produced_swapchain,
            resource.generation.synthetic_handle,
            resource.generation.synthetics_in_target_camera,
        });
    }
    if (afw_selected && !completed_afw) {
        // OFXR off: deliver the terminal output unchanged. This exact object
        // is also the source of freeze_completed_afw_frame when OFXR is on.
        submitted_end_info = &afw_terminal_submission.info;
    } else if (prepared.kind == PreparedGenerationKind::prime) {
        if (build_generated_frame_end_info(
                produced_end_info,
                produced_snapshot,
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
            produced_end_info,
            produced_snapshot,
            true,
            std::span<const ProjectionResourceDestination>(
                synthetic_destinations.data(), synthetic_destinations.size()),
            &first_generated, synthetic_camera ? &*synthetic_camera : nullptr);
        const bool current_built = build_generated_frame_end_info(
            produced_end_info,
            produced_snapshot,
            false,
            std::span<const ProjectionResourceDestination>(
                current_destinations.data(), current_destinations.size()),
            &current_generated);
        if (synthetic_built && current_built) {
            // The synthetic frame owns the deferred copies: they must reach
            // the queue after it has been handed to the runtime.
            if (!state->queued_nvidia_presenter)
            for (const PreparedProjectionResource& resource :
                 prepared.resources) {
                if (resource.generation.synthesizer && !resource.generation.staging_generation) {
                    first_generated.pending_current_copies.push_back(
                        resource.generation.synthesizer);
                }
            }
            submitted_end_info = &first_generated.info;
            current_generated.blackbar_pose.paired=true;
            pair_ready = true;
        } else {
            prepare_reason = GenerationPrepareReason::generated_end_info_failed;
            enter_generation_quarantine(
                state,
                GenerationQuarantineReason::generated_end_info_failed);
            prepared = {};
        }
    } else if (!prepared.anchor_is_current && !prepared.preserve_previous) {
        clear_generation_continuity(state);
    }

    bool extra_ready=pair_ready && djules_triple_active(*state);
    if(extra_ready) {
        auto extra_destinations = synthetic_destinations;
        for (std::size_t i=0; i<prepared.resources.size(); ++i)
            extra_destinations[i].destination_swapchain = prepared.resources[i].generation.extra_handle;
        extra_ready=build_generated_frame_end_info(produced_end_info,produced_snapshot,true,
            extra_destinations,&extra_generated, extra_camera ? &*extra_camera : nullptr);
        if(!extra_ready) {
            enter_generation_quarantine(state,GenerationQuarantineReason::generated_end_info_failed);
            return bypass_generation(GenerationPrepareReason::generated_end_info_failed);
        }
        extra_generated.synthetic=true;
    }
    if(prepared.kind==PreparedGenerationKind::prime) attach_djules_outputs(prepared,first_generated);
    else if(pair_ready) attach_djules_outputs(prepared,current_generated,&first_generated,extra_ready?&extra_generated:nullptr);
    if (completed_afw) {
        first_generated.composition_images = completed_afw->submission->composition_images;
        current_generated.composition_images = completed_afw->submission->composition_images;
    }

    if (state->luke_afw_enabled && frame_serial != 0) {
        if (pair_ready) {
            log_luke_projection_poses(&first_generated.info, frame_serial, 2);
            log_luke_projection_poses(&current_generated.info, frame_serial, 3);
        } else if (prepared.kind == PreparedGenerationKind::prime) {
            log_luke_projection_poses(&first_generated.info, frame_serial, 3);
        }
    }

    if ((state->graphics_binding_capabilities & 8ULL) != 0 &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        for (const ProjectionResourceDestination& destination :
             current_destinations) {
            // result=0 means the real current frame.
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::luke_output_contract,
                0,
                static_cast<std::uint64_t>(current_snapshot.display_time),
                handle_value(destination.application_swapchain),
                handle_value(destination.destination_swapchain));
        }
        if (pair_ready) {
            for (const ProjectionResourceDestination& destination :
                 synthetic_destinations) {
                // result=1 means the generated midpoint frame.
                xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::luke_output_contract,
                    1,
                    static_cast<std::uint64_t>(current_snapshot.display_time),
                    handle_value(destination.application_swapchain),
                    handle_value(destination.destination_swapchain));
            }
        }
    }

    std::shared_ptr<GeneratedFrameEndInfo> presenter_first_frame;
    mark_application_release(state.get());
    std::uint64_t generated_release_value = 0;
    { std::scoped_lock lock(state->binding_join_mutex);
      generated_release_value = state->app_release_counter; }
    first_generated.app_release_value = generated_release_value;
    current_generated.app_release_value = generated_release_value;
    extra_generated.app_release_value = generated_release_value;
    first_generated.synthetic = pair_ready;
    first_generated.djules_leader=extra_ready;
    std::shared_ptr<GeneratedFrameEndInfo> presenter_current_frame,presenter_extra_frame;
    if (use_continuous_presenter &&
        (prepared.kind == PreparedGenerationKind::prime || pair_ready)) {
        presenter_first_frame =
            make_presenter_owned_frame(std::move(first_generated));
        if (pair_ready) {
            presenter_current_frame =
                make_presenter_owned_frame(std::move(current_generated));
            if(extra_ready) presenter_extra_frame=make_presenter_owned_frame(std::move(extra_generated));
        }
        if (!presenter_first_frame ||
            (pair_ready && !presenter_current_frame) || (extra_ready && !presenter_extra_frame)) {
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
            if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
                // Auxiliary HUD/quad pixels need the same ownership guarantee
                // as projection outputs while the application renders ahead.
                XrResult owned = own_composition_images(state, *presenter_first_frame);
                if (XR_SUCCEEDED(owned) && presenter_current_frame) {
                    // HUD is identical on S/B: one owned snapshot shared by
                    // both entries, not two full-resolution copies per pair.
                    for(auto& owned:{presenter_current_frame,presenter_extra_frame}) {
                        if(!owned) continue;
                        auto& current=*owned;
                        current.composition_layers = presenter_first_frame->composition_layers;
                        current.composition_images = presenter_first_frame->composition_images;
                        std::size_t auxiliary = 0;
                        for (std::size_t index = 0; index < current.layer_pointers.size(); ++index) {
                            const bool projection = std::any_of(current.projections.begin(), current.projections.end(),
                                [index](const auto& layer) { return layer.layer_index == index; });
                            if (!projection)
                                current.layer_pointers[index] = composition_layer_header(current.composition_layers.at(auxiliary++));
                        }
                    }
                }
                if (XR_FAILED(owned)) {
                    presenter_first_frame.reset();
                    presenter_current_frame.reset();
                    return bypass_generation(GenerationPrepareReason::presenter_unsafe_composition);
                }
            }
        }
    }

    xrfg::bridge_flight_logger().event(
        xrfg::BridgeFlightOperation::generation_prepare,
        static_cast<std::int64_t>(prepare_reason),
        handle_value(failed_prepare_swapchain),
        static_cast<std::uint64_t>(prepared.kind),
            (metadata_pairable ? 1u : 0u) |
            (latest_application_frame ? 2u : 0u));
    if (afw_selected) {
        xrfg::embedded::report_afw_prepare(
            static_cast<std::int64_t>(prepare_reason));
        // result 3: which physical slot gets the first submission.
        xrfg::bridge_flight_logger().event(
            xrfg::BridgeFlightOperation::luke_frame_contract,
            3, frame_serial,
            submitted_end_info
                ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
                : 0,
            static_cast<std::uint64_t>(prepared.kind) |
                (pair_ready ? 1ULL << 8 : 0) |
                (submitted_end_info
                    ? static_cast<std::uint64_t>(submitted_end_info->layerCount)
                          << 16
                    : 0));
        xrfg::two_frame_capture_timeline(
            "frame", 3, frame_serial,
            submitted_end_info
                ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
                : 0,
            static_cast<std::uint64_t>(prepared.kind) |
                (pair_ready ? 1ULL << 8 : 0) |
                (submitted_end_info
                    ? static_cast<std::uint64_t>(submitted_end_info->layerCount)
                          << 16
                    : 0));
    }

    const auto first_end_token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::downstream_first_end_frame,
        handle_value(session),
        submitted_end_info
            ? static_cast<std::uint64_t>(submitted_end_info->displayTime)
            : 0,
        submitted_end_info ? submitted_end_info->layerCount : 0);
    const auto first_end_started = std::chrono::steady_clock::now();
    if (pair_ready && state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3)
        state->fsr_cadence.generated(state->fps_overlay &&
            state->fsr_cadence.marker_enabled());
    XrResult result = XR_SUCCESS;
    if (use_continuous_presenter) {
        if (pair_ready) {
            // The consumer must be able to retire the preceding frame while
            // ready outputs wait for their bounded publication admission.
            if(adaptive_optical_reserve_route(*state) && presenter_content_lock.owns_lock())
                presenter_content_lock.unlock();
            result = publish_native_generated_pair(
                state,
                presenter_first_frame,
                presenter_current_frame,presenter_extra_frame);
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
                if (!state->fsr_two_slot_presenter && !state->queued_nvidia_presenter)
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
        XrFrameEndInfo physical_slot{};
        const auto* physical_end=blackbar_inline_submission(*state,submitted_end_info,physical_slot);
        join_runtime_queue_to_application(state.get(), generated_release_value);
        result = with_runtime_entry(state, [&] {
            log_luke_physical_boundary(state, submitted_end_info, frame_serial, 4, &prepared);
            if(submitted_end_info==&first_generated.info &&
               !handover_djules_outputs(state.get(),first_generated.pending_outputs)) return XR_ERROR_RUNTIME_FAILURE;
            report_native_frame_content(state.get(),&first_generated);
            return state->fps_overlay
                ? state->fps_overlay->end_frame(physical_end, pair_ready,true,native_frame_results(&first_generated),first_generated.native_output, produced_snapshot.interface_stamp)
                : content_end_frame(session, physical_end);
        });
        log_luke_physical_boundary(state, submitted_end_info, frame_serial, 5, &prepared, result);
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
        consume_application_frame(state, current_snapshot.display_time,
            consume_in_submission_order, match_latest_by_submission);
        // A rejected display time says nothing about the generation
        // resources. No swapchain changed shape, no private image is in an
        // uncertain ownership phase - the runtime was handed a time it
        // considers past, which is the application's own: MSFS 2024 submits a
        // display time older than the previous frame's after a hitch, 3 times
        // in 2350 frames in one capture. Clear continuity so the next frame
        // primes, and leave the quarantine for failures that really do mean
        // the resources are no longer safe to use. Quarantining cost a full
        // second of generation for each of those three frames.
        if (result == XR_ERROR_TIME_INVALID) {
            clear_generation_continuity(state);
            return result;
        }
        enter_generation_quarantine(
            state,
            GenerationQuarantineReason::downstream_end_failed,
            static_cast<std::uint32_t>(result));
        return result;
    }
    // A ready single AFW output has now actually completed runtime EndFrame,
    // including the waited presenter single-submission case. Held outputs do
    // not arm or interrupt the fresh-publication sequence.
    startup_observation.physical_success = !pair_ready &&
        prepared.kind == PreparedGenerationKind::prime;
    if (pair_ready) {
        state->generation_steady_state_established = true;
    } else if(!use_continuous_presenter && end_info && XR_SUCCEEDED(result) && blackbar_gate(*state)) {
        XrDuration period{};
        {std::scoped_lock lock(state->mutex);period=state->minimum_runtime_display_period;}
        blackbar_observe(*state,current_snapshot.blackbar_pose,state->blackbar_inline_slot,
            period,state->last_inline_frame_state.predictedDisplayPeriod,false);
    }
    if (use_continuous_presenter && !pipelined_presenter_mode) {
        // A frame the layer could not generate from is already handled: it was
        // passed through unchanged above. Demoting the presenter for it buys
        // nothing and costs a great deal, because the promotion that follows
        // is automatic - three throttled waits later the presenter is back.
        //
        // Cyberpunk 2077 through the Luke Ross mod fails one frame in fourteen
        // on the D3D11 interop path, and every one of those failures was
        // isolated: 175 failures in 59 s, 380 in 66 s, never two in a row. So
        // the presenter was stopped and restarted three times a second, 524
        // transitions in one session, each teardown submitting a frame with
        // nothing in it - black until the shutdown frame repeated the last
        // composition, and a stale pose under a newer image afterwards.
        // Neither artifact is worth having, and both exist only because the
        // presenter is being torn down mid-stride.
        //
        // Demote on a run of failures instead, which is what "generation is
        // not working here" actually looks like, using the same count of three
        // the promotion uses in the other direction.
        constexpr std::uint32_t kGenerationFailureDemotionStreak = 3;
        bool demote = false;
        {
            std::scoped_lock lock(state->mutex);
            if (presenter_first_frame) {
                state->generation_failure_streak = 0;
            } else if (++state->generation_failure_streak >=
                       kGenerationFailureDemotionStreak) {
                demote = true;
            }
        }
        if (demote) {
            // Not under state->mutex: stopping the presenter joins its thread.
            if (ordinary_djules_route(*state) && state->vdxr_legacy)
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    604,static_cast<std::uint64_t>(prepare_reason),0,state->split_frame_loop?1:0);
            state->presenter_restore_after_bypass=false;
            stop_continuous_presenter(state);
            std::scoped_lock lock(state->mutex);
            state->steamvr_throttled_wait_streak = 0;
            state->unpaced_wait_streak = 0;
            state->generation_failure_streak = 0;
        }
    }

    consume_application_frame(
        state,
        current_snapshot.display_time,
        consume_in_submission_order,
        match_latest_by_submission);
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
            blackbar_unpaired(*state,false,true);
        }
        clear_generation_continuity(state);
        return result;
    }
    if (prepared.anchor_is_current) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection = current_snapshot;
    } else if (!prepared.preserve_previous) {
        std::scoped_lock lock(state->mutex);
        state->previous_projection.reset();
    }

    const bool promote_three_slot_after_inline = !use_continuous_presenter &&
        pipelined_presenter_start_requested &&
        (three_slot_generation_session(state) || fsr_two_slot_session(state) || queued_nvidia_session(state));
    if (!use_continuous_presenter && pipelined_presenter_start_requested &&
        !promote_three_slot_after_inline) {
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

    const bool hold_djules_prime = use_continuous_presenter && state->djules_route &&
        state->optical_flow_backend == xrfg::D3D12OpticalFlowBackend::nvidia &&
        state->frame_generation_mode != xrfg::D3D12FrameGenerationMode::fsr3 &&
        !state->luke_afw_enabled && prepared.kind == PreparedGenerationKind::prime &&
        !state->fsr_two_slot_presenter && !state->queued_nvidia_presenter && presenter_holds_djules_prime(*state);
    if (!pair_ready && !hold_djules_prime) {
        if (promote_three_slot_after_inline) {
            const XrResult start_result = start_requested_pipelined_presenter(nullptr);
            return XR_FAILED(start_result) ? start_result : result;
        }
        if (!use_continuous_presenter && (fsr_two_slot_session(state) || queued_nvidia_session(state)) &&
            prepared.kind == PreparedGenerationKind::prime) {
            // Prime is already presented: bootstrap prediction from that XR
            // cycle, not a repeated seed or an empty runtime submission.
            if (!start_continuous_presenter(state, nullptr, true))
                return XR_ERROR_RUNTIME_FAILURE;
        }
        return result;
    }

    if (use_continuous_presenter) {
        if (state->fsr_two_slot_presenter || state->queued_nvidia_presenter) {
            // S/B are queued now. Allow the application to render ahead;
            // admission at its next EndFrame prevents output reuse/backlog.
            if (presenter_content_lock.owns_lock()) presenter_content_lock.unlock();
            frame_call_lock.unlock();
            return single_director_pacing(*state) ? wait_for_presenter_idle(state) : result;
        }
        // The once-per-pair hold. The layer submits one pair per application
        // frame and a pair costs two presenter frames, so without this an
        // application that renders faster than half the display rate produces
        // frames the pairing has no room for - they lose the history ring's
        // capture slot and are rendered and thrown away.
        //
        // It sits here rather than in the virtual wait for two reasons. The
        // frame is already handed over, so the presenter has composition to
        // work with and its count keeps advancing - gating the wait instead
        // deadlocked on the first frame, before anything had been enqueued.
        // And the application is released at the top of its next period rather
        // than the end of this one, so it renders straight away and synthesis
        // is queued with most of a period still in front of it.
        //
        // The frame lock goes first: an application whose wait runs on another
        // thread must not be shut out of xrWaitFrame while this waits.
        frame_call_lock.unlock();
        if (single_director_pacing(*state)) {
            const auto accepted = wait_for_presenter_idle(state);
            if (XR_FAILED(accepted)) return accepted;
        } else wait_for_presenter_pair(state, pair_ready ? (djules_triple_active(*state) ? 3U : 2U) : 1U);
        if (pair_ready) delay_djules_application_release(state);
        return result;
    }

    // The synthetic has gone downstream; submit its current copy before the
    // cycle that hands the runtime the frame which reads it.
    for (const auto& synthesizer : first_generated.pending_current_copies) {
        if (synthesizer) {
            static_cast<void>(synthesizer->flush_current_copy(
                state->d3d12_synthesis_queue
                    ? runtime_queue(*state)
                    : nullptr));
        }
    }
    std::optional<XrFrameState> adopted_frame;
    if (state->runtime_frame_waited_unbegun && state->last_inline_frame_state_valid) {
        adopted_frame = state->last_inline_frame_state;
        state->runtime_frame_waited_unbegun = false;
        state->application_begin_needs_wait = true;
    }
    if(extra_ready) {
        const auto middle_cycle=submit_current_cycle(state,extra_generated.info,frame_serial,&prepared,
            adopted_frame,&extra_generated,true);
        if(!middle_cycle.completed || XR_FAILED(middle_cycle.result)) {
            clear_generation_continuity(state); return middle_cycle.result;
        }
        adopted_frame.reset();
    }
    const InternalCycleResult current_cycle =
        submit_current_cycle(state, current_generated.info, frame_serial, &prepared, adopted_frame,&current_generated);
    // How far apart the runtime actually received the two frames of this
    // pair: from the synthetic's hand-over completing to the real frame's.
    const auto inline_pair_gap = std::chrono::steady_clock::now() -
        (first_end_started + first_end_elapsed);
    if (state->frame_generation_mode == xrfg::D3D12FrameGenerationMode::fsr3) {
        const auto ns = [](auto value) { return static_cast<std::uint64_t>((std::max)(
            std::int64_t{0}, std::chrono::duration_cast<std::chrono::nanoseconds>(value).count())); };
        state->fsr_cadence.pair(current_cycle.completed, ns(inline_pair_gap),
            static_cast<std::uint64_t>((std::max)(XrDuration{0}, current_cycle.predicted_display_period)),
            ns(current_cycle.wait_elapsed), current_cycle.display_time - first_generated.info.displayTime);
    }
    if (!current_cycle.completed) {
        // The synthetic submission has already completed. A transient runtime
        // failure in the optional second cycle is recovered by the established
        // continuity reset; treating it as a structural resize would retain a
        // private image in an uncertain ownership phase for the whole timeout.
        clear_generation_continuity(state);
    } else if ((fsr_two_slot_session(state) || queued_nvidia_session(state)) && !use_continuous_presenter) {
        if (!start_continuous_presenter(state, nullptr, true))
            return XR_ERROR_RUNTIME_FAILURE;
    } else if (promote_three_slot_after_inline) {
        // Finish the final inline S(A,B),B exactly once before giving XR
        // cycle ownership to the director. B is now presented, so no seed is
        // discarded before presentation or repeated by the new scheduler.
        const XrResult start_result = start_requested_pipelined_presenter(nullptr);
        return XR_FAILED(start_result) ? start_result : result;
    } else if (!presenter_forbidden(*state) && state->frame_generation_mode !=
                   xrfg::D3D12FrameGenerationMode::fsr3 &&
               (three_slot_generation_session(state) ||
                steamvr_wait_requires_continuous_presenter(
                    state,
                    current_cycle) ||
                runtime_wait_lacks_pacing(state) ||
                inline_pair_lands_in_one_scanout(state, inline_pair_gap))) {
        // Request the promotion; do not perform it here. submit_current_cycle
        // has already submitted this frame, so seeding a freshly started
        // presenter thread with it handed a second owner to composition layers
        // and private swapchain leases that this frame still holds. The next
        // xrEndFrame finishes its inline pair before starting a three-slot
        // director; other presenters take their still-unpresented seed through
        // their existing handoff above.
        state->presenter_start_requested = true;
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_wait_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_frame_impl(session, wait_info, frame_state, &interface_call);
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_begin_frame,
        handle_value(session));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_begin_frame_impl(session, begin_info, &interface_call);
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
            if (const auto owner = g_observed_spaces.find(space); owner != g_observed_spaces.end())
                dispatch = owner->second.dispatch;
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
            if(session->private_runtime)session->private_runtime->drain();
        }
        const auto result = dispatch->destroy_space(space);
        if (XR_SUCCEEDED(result)) {
            std::scoped_lock lock(g_state_mutex);
            g_observed_spaces.erase(space);
        }
        return result;
    });
}

// Pure observation. The event is forwarded exactly as the runtime produced it
// and is never consumed, reordered or synthesized: the application sees the
// same queue it would without the layer.
//
// It exists because a session that stops being displayed is invisible from
// everywhere else in this layer. When a runtime drops a session out of the
// visible state it reports shouldRender=false and stops blocking xrWaitFrame,
// the application stops submitting projection layers, and generation fails
// open - which in a capture is indistinguishable from the layer breaking. A
// captured MSFS 2024 session did exactly that 104 s in and never recovered,
// and nothing in the log could say whether the runtime or the layer started
// it. The state transition is the answer, and the layer only sees it here.
XRAPI_ATTR XrResult XRAPI_CALL layer_poll_event(
    XrInstance instance,
    XrEventDataBuffer* event_data) {
    return guard_c_api_boundary([&]() -> XrResult {
        const auto dispatch = find_dispatch(instance);
        if (!dispatch || dispatch->poll_event == nullptr) {
            return XR_ERROR_FUNCTION_UNSUPPORTED;
        }
        const XrResult result = dispatch->poll_event(instance, event_data);
        // XR_EVENT_UNAVAILABLE is the common answer and carries no buffer, so
        // this costs one comparison on the overwhelming majority of calls.
        if (result == XR_SUCCESS && event_data != nullptr &&
            event_data->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* state_changed =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(
                    event_data);
            const auto session = find_session(state_changed->session);
            if (session && session->fps_overlay)
                session->fps_overlay->set_session_focused(state_changed->state == XR_SESSION_STATE_FOCUSED);
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::session_state,
                static_cast<std::int64_t>(state_changed->state),
                handle_value(state_changed->session),
                static_cast<std::uint64_t>(state_changed->time),
                0);
        }
        return result;
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_acquire,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_acquire_swapchain_image_impl(swapchain, acquire_info, index, &interface_call);
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_wait,
        handle_value(swapchain),
        wait_info ? static_cast<std::uint64_t>(wait_info->timeout) : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_wait_swapchain_image_impl(swapchain, wait_info, &interface_call);
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_swapchain_release,
        handle_value(swapchain));
    const XrResult result = guard_c_api_boundary([&] {
        return layer_release_swapchain_image_impl(swapchain, release_info, &interface_call);
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
    xrfg::InterfaceCallTiming interface_call(xrfg::interface_measurement_requested()
        ? xrfg::interface_clock_ns() : 0, true);
    const auto token = xrfg::bridge_flight_logger().begin(
        xrfg::BridgeFlightOperation::application_end_frame,
        handle_value(session),
        end_info ? static_cast<std::uint64_t>(end_info->displayTime) : 0,
        end_info ? end_info->layerCount : 0);
    const XrResult result = guard_c_api_boundary([&] {
        return layer_end_frame_impl(session, end_info, &interface_call);
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

        // A refusal here is the one way to keep the layer out of a process
        // entirely: the loader drops an implicit layer whose negotiation
        // fails and carries on without it, so an excluded process has no
        // hook, no history and no private swapchain from this layer.
        const std::wstring executable =
            xrfg::implicit_layer::current_executable_name();
        if (xrfg::implicit_layer::executable_is_excluded(
                executable,
                xrfg::implicit_layer::read_excluded_processes(
                    current_layer_directory()))) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::process_excluded,
                0,
                executable.size(),
                0,
                0);
            return XR_ERROR_INITIALIZATION_FAILED;
        }

        if (xrfg::optiscaler_bootstrap::negotiate(loader_info, layer_request)) {
            xrfg::bridge_flight_logger().event(
                xrfg::BridgeFlightOperation::negotiation, 5670, 1, 0, 0);
            return XR_SUCCESS;
        }

        static_cast<void>(xrfg::standalone::process_dlssg_preset());
        layer_request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        layer_request->layerApiVersion = kLayerApiVersion;
        layer_request->getInstanceProcAddr = layer_get_instance_proc_addr;
        layer_request->createApiLayerInstance = layer_create_api_layer_instance;
        xrfg::embedded::set_layer_active(true);
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
