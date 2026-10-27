#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
#include "xrfg/d3d12_guide_composer.hpp"
#include "xrfg/fsr_frame_generation.hpp"
#include "xrfg/gpu_flight_diagnostics.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "d3d12_fsr_frame_generator.hpp"
#include "xrfg/d3d12_ram_timing.hpp"
#include "xrfg/bridge_flight_logger.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <nvOpticalFlowD3D12.h>

#include "fullscreen_vertex_shader.hpp"
#include "fsr_cinema_pixel_shader.hpp"
#include "fsr_cinema_vertex_shader.hpp"
#include "cinema_flatten_vertex_shader.hpp"
#include "cinema_flatten_pixel_shader.hpp"
#include "hybrid_synthesize_midpoint_pixel_shader.hpp"
#include "hybrid_pack_flow_input_shader.hpp"
#include "nvidia_bidirectional_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_fast_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader.hpp"
#include "nvidia_pack_flow_input_shader.hpp"
#include "nvidia_pack_luma_input_shader.hpp"
#include "nvidia_synthesize_midpoint_pixel_shader.hpp"
#include "pack_flow_input_shader.hpp"
#include "reproject_generated_pixel_shader.hpp"
#include "reproject_late_hud_pixel_shader.hpp"
#include "game_motion_synthesize_midpoint_pixel_shader.hpp"
#include "synthesize_midpoint_pixel_shader.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;
using GetWitcherHudlessColor = int (*)(
    std::int64_t, ID3D12Resource**) noexcept;

GetWitcherHudlessColor witcher_hudless_color_export() noexcept {
    static std::atomic<GetWitcherHudlessColor> cached{};
    if (auto* const found = cached.load(std::memory_order_acquire))
        return found;
    std::array<wchar_t, 32768> executable_path{};
    const DWORD count = GetModuleFileNameW(
        nullptr, executable_path.data(),
        static_cast<DWORD>(executable_path.size()));
    if (count == 0 || count >= executable_path.size()) return nullptr;
    wchar_t* const slash = std::wcsrchr(
        executable_path.data(), L'\\');
    if (slash == nullptr) return nullptr;
    std::wstring module_path(executable_path.data(), slash + 1);
    module_path += L"dxgi.dll";
    const HMODULE module = GetModuleHandleW(module_path.c_str());
    if (module == nullptr) return nullptr;
    auto* const function = reinterpret_cast<GetWitcherHudlessColor>(
        GetProcAddress(module, "W3VR_GetHudlessXrColorV1"));
    if (function != nullptr)
        cached.store(function, std::memory_order_release);
    return function;
}

// Optional admission record; ordinary gameplay performs no file I/O here.
void log_post_hud_admission(bool ready, std::int64_t display_time,
                            DXGI_FORMAT scene_format,
                            DXGI_FORMAT final_format) noexcept {
    if (!bridge_flight_logger().enabled()) return;
    static std::atomic<std::uint64_t> pair_count{};
    const auto pair = pair_count.fetch_add(1, std::memory_order_relaxed);
    if (pair % 120U != 0U) return;
    bridge_flight_logger().event(BridgeFlightOperation::fsr_hudless_pair,
        ready ? 120 : -120, static_cast<std::uint64_t>(display_time),
        static_cast<std::uint64_t>(scene_format), static_cast<std::uint64_t>(final_format));
}
void dred_marker(ID3D12GraphicsCommandList* command_list, const char* label) noexcept {
    if (command_list != nullptr && label != nullptr &&
        bridge_flight_logger().gpu_instrumentation_enabled()) {
        command_list->SetMarker(1U, label, static_cast<UINT>(std::strlen(label)));
    }
}

constexpr std::uint32_t kWorkSlotCount = D3D12SwapchainHistory::kSlotCount;
constexpr UINT kFidelityFxFlowBlockSize = 8;
constexpr UINT kNvidiaFlowBlockSize = 4;
constexpr UINT kEyeGapPixels = 64;
constexpr UINT kMinimumFlowDimension = 64;
constexpr UINT kMaxReprojectionViews = 2;

struct NvidiaInputScaleRatio {
    UINT numerator;
    UINT denominator;
};

[[nodiscard]] constexpr NvidiaInputScaleRatio nvidia_input_scale_ratio(
    D3D12NvidiaInputScale scale) noexcept {
    switch (scale) {
    case D3D12NvidiaInputScale::three_quarter:
        return {3U, 4U};
    case D3D12NvidiaInputScale::half:
        return {1U, 2U};
    case D3D12NvidiaInputScale::quarter:
        return {1U, 4U};
    case D3D12NvidiaInputScale::full:
    default:
        return {1U, 1U};
    }
}
constexpr UINT kSrvDescriptorCount = 8;
constexpr UINT kUavDescriptorCount = 3;
constexpr UINT kDescriptorBlockSize =
    kSrvDescriptorCount + kUavDescriptorCount;
constexpr UINT kDescriptorCount =
    kDescriptorBlockSize * kMaxReprojectionViews;
constexpr UINT kNvidiaTimestampCount = 5;
constexpr UINT kNvidiaTimestampPackBegin = 0;
constexpr UINT kNvidiaTimestampPackEnd = 1;
constexpr UINT kNvidiaTimestampEye0End = 2;
constexpr UINT kNvidiaTimestampCompositionBegin = 3;
constexpr UINT kNvidiaTimestampCompositionEnd = 4;
// Two more, written by both backends, bracketing all of a pair's GPU work.
// The NVIDIA stage marks above stay as they are; these exist so the
// FidelityFX path is measurable too, and so the span can be reported as
// absolute times rather than only as a duration.
constexpr UINT kSpanBegin = 5;
constexpr UINT kSpanEnd = 6;
constexpr UINT kTimestampCount = 7;
constexpr float kCameraTolerance = 1.0e-5F;
// Bit 4 of synthesis_flags: the synthetic is generated in a camera of its own
// rather than B's. The rotation from it into B's rides in the other view's
// mapping (apply_synthetic_camera).
constexpr UINT kSyntheticCameraFlag = 16U;
// A target camera turned from B's by less than this - two microradians, a
// few thousandths of a pixel at any eye resolution - is B's own: what a pose
// copied from B comes to after the rotation between them is formed in float.
constexpr float kPositionToleranceMeters = 1.0e-4F;
constexpr float kSyntheticCameraMinimumTurn = 1.0e-6F;
constexpr D3D12_RESOURCE_STATES kShaderReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

[[nodiscard]] constexpr std::uint64_t pack_u32_pair(
    std::uint32_t low,
    std::uint32_t high) noexcept {
    return static_cast<std::uint64_t>(low) |
           (static_cast<std::uint64_t>(high) << 32U);
}

[[nodiscard]] std::uint64_t pack_float_pair(float low, float high) noexcept {
    return pack_u32_pair(
        std::bit_cast<std::uint32_t>(low),
        std::bit_cast<std::uint32_t>(high));
}

[[nodiscard]] constexpr std::uint64_t pack_rect_16(
    const D3D12ImageRect& rect) noexcept {
    return (static_cast<std::uint64_t>(rect.offset_x) & 0xffffULL) |
           ((static_cast<std::uint64_t>(rect.offset_y) & 0xffffULL) << 16U) |
           ((static_cast<std::uint64_t>(rect.width) & 0xffffULL) << 32U) |
           ((static_cast<std::uint64_t>(rect.height) & 0xffffULL) << 48U);
}

[[nodiscard]] constexpr D3D12ImageRect centered_aspect_rect(
    D3D12ImageRect source,
    const D3D12ImageRect& target) noexcept {
    if (source.width == 0 || source.height == 0 ||
        target.width == 0 || target.height == 0) {
        return source;
    }
    const std::uint64_t source_width = source.width;
    const std::uint64_t source_height = source.height;
    const std::uint64_t target_width = target.width;
    const std::uint64_t target_height = target.height;
    if (source_width * target_height > source_height * target_width) {
        const std::uint64_t width = std::clamp<std::uint64_t>(
            (source_height * target_width + target_height / 2U) /
                target_height,
            1U, source_width);
        source.offset_x += static_cast<std::uint32_t>(
            (source_width - width) / 2U);
        source.width = static_cast<std::uint32_t>(width);
    } else if (source_width * target_height <
               source_height * target_width) {
        const std::uint64_t height = std::clamp<std::uint64_t>(
            (source_width * target_height + target_width / 2U) /
                target_width,
            1U, source_height);
        source.offset_y += static_cast<std::uint32_t>(
            (source_height - height) / 2U);
        source.height = static_cast<std::uint32_t>(height);
    }
    return source;
}

constexpr auto kGhostSquareGuideRect = centered_aspect_rect(
    {0U, 0U, 1672U, 1672U}, {0U, 0U, 3063U, 3344U});
static_assert(kGhostSquareGuideRect.offset_x == 70U);
static_assert(kGhostSquareGuideRect.offset_y == 0U);
static_assert(kGhostSquareGuideRect.width == 1532U);
static_assert(kGhostSquareGuideRect.height == 1672U);

[[nodiscard]] bool same_identity(
    IUnknown* expected,
    IUnknown* actual) noexcept {
    if (expected == nullptr || actual == nullptr) {
        return false;
    }
    ComPtr<IUnknown> expected_identity;
    ComPtr<IUnknown> actual_identity;
    return SUCCEEDED(expected->QueryInterface(IID_PPV_ARGS(expected_identity.GetAddressOf()))) &&
           SUCCEEDED(actual->QueryInterface(IID_PPV_ARGS(actual_identity.GetAddressOf()))) &&
           expected_identity.Get() == actual_identity.Get();
}

[[nodiscard]] bool same_device(
    ID3D12Device* expected,
    ID3D12DeviceChild* object) noexcept {
    if (expected == nullptr || object == nullptr) {
        return false;
    }

    ComPtr<ID3D12Device> actual;
    return SUCCEEDED(object->GetDevice(IID_PPV_ARGS(actual.GetAddressOf()))) &&
           same_identity(expected, actual.Get());
}

[[nodiscard]] bool same_adapter(
    ID3D12Device* expected,
    ID3D12DeviceChild* object) noexcept {
    if (expected == nullptr || object == nullptr) {
        return false;
    }
    ComPtr<ID3D12Device> actual;
    if (FAILED(object->GetDevice(IID_PPV_ARGS(actual.GetAddressOf())))) {
        return false;
    }
    const LUID expected_luid = expected->GetAdapterLuid();
    const LUID actual_luid = actual->GetAdapterLuid();
    return expected_luid.HighPart == actual_luid.HighPart &&
           expected_luid.LowPart == actual_luid.LowPart;
}

[[nodiscard]] bool copy_compatible(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension &&
           left.Width == right.Width &&
           left.Height == right.Height &&
           left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout;
}

[[nodiscard]] bool compatible_view_format(
    DXGI_FORMAT resource_format,
    DXGI_FORMAT view_format) noexcept {
    switch (view_format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return resource_format == DXGI_FORMAT_R8G8B8A8_TYPELESS ||
                   resource_format == view_format;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return resource_format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                   resource_format == view_format;
        default:
            return false;
    }
}

[[nodiscard]] DXGI_FORMAT linear_color_view_format(
    DXGI_FORMAT resource_format,
    DXGI_FORMAT fallback) noexcept {
    switch (resource_format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return fallback;
    }
}

[[nodiscard]] DXGI_FORMAT motion_vector_view_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R32G32_FLOAT:
        return format;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

[[nodiscard]] D3D12_RESOURCE_BARRIER uav_barrier(
    ID3D12Resource* resource) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    return barrier;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES default_heap_properties(
    UINT node_mask) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = node_mask == 0 ? 1U : node_mask;
    properties.VisibleNodeMask = properties.CreationNodeMask;
    return properties;
}

[[nodiscard]] D3D12_RESOURCE_DESC texture2d_description(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = flags;
    return description;
}

[[nodiscard]] HRESULT ffx_result(FfxErrorCode result) noexcept {
    return result == FFX_OK ? S_OK : E_FAIL;
}

[[nodiscard]] HRESULT nvidia_result(NV_OF_STATUS result) noexcept {
    return result == NV_OF_SUCCESS
               ? S_OK
               : MAKE_HRESULT(
                     SEVERITY_ERROR,
                     FACILITY_ITF,
                     static_cast<unsigned>(result) & 0xffffU);
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE offset_cpu_handle(
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    UINT index,
    UINT increment) noexcept {
    handle.ptr += static_cast<SIZE_T>(index) * increment;
    return handle;
}

[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE offset_gpu_handle(
    D3D12_GPU_DESCRIPTOR_HANDLE handle,
    UINT index,
    UINT increment) noexcept {
    handle.ptr += static_cast<UINT64>(index) * increment;
    return handle;
}

struct CameraMapping {
    std::array<float, 4> target_to_source_rotation{};
    std::array<float, 4> source_tangents{};
    std::array<float, 4> target_tangents{};
    std::array<float, 4> source_rect{};
    std::array<float, 4> target_rect{};
};

struct SynthesisParameters {
    UINT width{};
    UINT height{};
    UINT array_size{};
    UINT packed_width{};
    UINT packed_height{};
    UINT packed_eye_stride{};
    UINT flow_width{};
    UINT flow_height{};
    UINT flow_block_size{};
    UINT view_index{};
    UINT use_game_motion{};
    UINT game_motion_padding{};
    std::array<float, 4> game_motion_rect{};
    std::array<float, 2> game_motion_scale{};
    std::array<float, 2> game_motion_jitter_delta{};
    std::array<CameraMapping, kMaxReprojectionViews> previous_mappings{};
    UINT slice{};
    // Bit 0 is the repeated-capture flag; bit 1 asks the pack shaders to
    // sRGB-encode the flow input; bits 2 and 3 describe extrapolation's
    // depth; bit 4 says the synthetic has a camera of its own
    // (kSyntheticCameraFlag); bits 8-23 carry the synthetic's position
    // between the two captures in 1/65535ths. They share a slot because the
    // root signature is full - see the assert below.
    UINT synthesis_flags{};
};

struct FsrCinemaPanelParameters {
    std::array<float, 16> clip_positions{};
    std::array<float, 4> source_rect{};
    UINT source_width{};
    UINT source_height{};
    UINT source_slice{};
    UINT padding{};
};

constexpr UINT kFsrCinemaPanelConstantCount =
    static_cast<UINT>(sizeof(FsrCinemaPanelParameters) / sizeof(UINT));
static_assert(sizeof(FsrCinemaPanelParameters) == sizeof(UINT) * 24U);

// Packs the synthetic's position between the two captures into the spare
// bits of synthesis_flags. Zero is reserved for "not set", which the shader
// decodes as the old fixed midpoint, so a half that rounds to zero is nudged
// rather than silently meaning something else.
[[nodiscard]] constexpr UINT packed_synthesis_fraction(float fraction) noexcept {
    const float clamped = fraction < 0.0F ? 0.0F : (fraction > 1.0F ? 1.0F : fraction);
    UINT packed = static_cast<UINT>(clamped * 65535.0F + 0.5F);
    if (packed == 0U) {
        packed = 1U;
    }
    return packed << 8U;
}

constexpr UINT kSynthesisConstantCount =
    static_cast<UINT>(sizeof(SynthesisParameters) / sizeof(UINT));
static_assert(sizeof(CameraMapping) == sizeof(float) * 20U);
static_assert(sizeof(SynthesisParameters) == sizeof(UINT) * 62U);
static_assert(kSynthesisConstantCount + 2U <= 64U);

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] float quaternion_length_squared(const Quaternion& value) noexcept {
    return value.x * value.x + value.y * value.y +
           value.z * value.z + value.w * value.w;
}

[[nodiscard]] bool valid_view(const D3D12ReprojectionView& view) noexcept {
    constexpr float kHalfPi = 1.57079632679489661923F;
    const Quaternion& orientation = view.pose.orientation;
    const Vec3& position = view.pose.position;
    const D3D12FieldOfView& fov = view.fov;
    const float length_squared = quaternion_length_squared(orientation);
    return finite(orientation.x) && finite(orientation.y) &&
           finite(orientation.z) && finite(orientation.w) &&
           finite(position.x) && finite(position.y) && finite(position.z) &&
           finite(fov.angle_left) && finite(fov.angle_right) &&
           finite(fov.angle_up) && finite(fov.angle_down) &&
           length_squared > 1.0e-12F && finite(length_squared) &&
           fov.angle_left < fov.angle_right &&
           // OpenXR encodes a vertically flipped image by reversing Up/Down.
           // Preserve that signed mapping; only an empty/out-of-range FOV is invalid.
           fov.angle_down != fov.angle_up &&
           fov.angle_left > -kHalfPi && fov.angle_right < kHalfPi &&
           std::min(fov.angle_down, fov.angle_up) > -kHalfPi &&
           std::max(fov.angle_down, fov.angle_up) < kHalfPi &&
           ((view.image_rect.width == 0 && view.image_rect.height == 0) ||
            (view.image_rect.width != 0 && view.image_rect.height != 0));
}

[[nodiscard]] bool rect_within_resource(
    const D3D12ImageRect& rect,
    UINT width,
    UINT height) noexcept {
    if (rect.width == 0 && rect.height == 0) {
        return true;
    }
    return rect.width != 0 && rect.height != 0 &&
           rect.offset_x < width && rect.offset_y < height &&
           static_cast<std::uint64_t>(rect.offset_x) + rect.width <= width &&
           static_cast<std::uint64_t>(rect.offset_y) + rect.height <= height;
}

[[nodiscard]] UINT resolved_array_slice(
    const D3D12ReprojectionView& view,
    std::size_t view_index,
    std::size_t view_count,
    UINT array_size) noexcept {
    if (view.array_slice != std::numeric_limits<std::uint32_t>::max()) {
        return view.array_slice < array_size
            ? static_cast<UINT>(view.array_slice)
            : std::numeric_limits<UINT>::max();
    }
    return view_count == array_size && view_index < array_size
        ? static_cast<UINT>(view_index)
        : std::numeric_limits<UINT>::max();
}

[[nodiscard]] UINT resolved_fsr_guide_eye(
    const D3D12ReprojectionView& view,
    std::size_t view_index,
    std::size_t view_count,
    UINT array_size,
    UINT guide_eye_count) noexcept {
    // Native double-wide stereo uses one physical resource slice and a DLSS
    // guide stream for each viewport. Array stereo still uses slice indices.
    if (array_size == 1 && guide_eye_count == view_count &&
        view_index < guide_eye_count) {
        return static_cast<UINT>(view_index);
    }
    const UINT slice = resolved_array_slice(
        view, view_index, view_count, array_size);
    return slice < guide_eye_count
        ? slice
        : std::numeric_limits<UINT>::max();
}

[[nodiscard]] bool rectangles_overlap(
    const D3D12ImageRect& left,
    const D3D12ImageRect& right) noexcept {
    const std::uint64_t left_right =
        static_cast<std::uint64_t>(left.offset_x) + left.width;
    const std::uint64_t left_bottom =
        static_cast<std::uint64_t>(left.offset_y) + left.height;
    const std::uint64_t right_right =
        static_cast<std::uint64_t>(right.offset_x) + right.width;
    const std::uint64_t right_bottom =
        static_cast<std::uint64_t>(right.offset_y) + right.height;
    return left.offset_x < right_right && right.offset_x < left_right &&
           left.offset_y < right_bottom && right.offset_y < left_bottom;
}

[[nodiscard]] bool valid_view_layout(
    std::span<const D3D12ReprojectionView> views,
    UINT width,
    UINT height,
    UINT array_size) noexcept {
    if (views.empty() || views.size() > kMaxReprojectionViews ||
        array_size == 0 || array_size > kMaxReprojectionViews) {
        return false;
    }
    std::array<bool, kMaxReprojectionViews> covered_slices{};
    for (std::size_t view_index = 0; view_index < views.size(); ++view_index) {
        const D3D12ReprojectionView& view = views[view_index];
        const UINT slice = resolved_array_slice(
            view,
            view_index,
            views.size(),
            array_size);
        if (slice == std::numeric_limits<UINT>::max() ||
            !valid_view(view) ||
            !rect_within_resource(view.image_rect, width, height)) {
            return false;
        }
        covered_slices[slice] = true;
        for (std::size_t preceding = 0; preceding < view_index; ++preceding) {
            if (resolved_array_slice(
                    views[preceding],
                    preceding,
                    views.size(),
                    array_size) != slice) {
                continue;
            }
            if (view.image_rect.width == 0 || view.image_rect.height == 0 ||
                views[preceding].image_rect.width == 0 ||
                views[preceding].image_rect.height == 0 ||
                rectangles_overlap(view.image_rect, views[preceding].image_rect)) {
                return false;
            }
        }
    }
    return std::all_of(
        covered_slices.begin(),
        covered_slices.begin() + array_size,
        [](bool covered) { return covered; });
}

[[nodiscard]] bool same_view_layout(
    std::span<const D3D12ReprojectionView> left,
    std::span<const D3D12ReprojectionView> right,
    UINT array_size) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t view_index = 0; view_index < left.size(); ++view_index) {
        if (resolved_array_slice(
                left[view_index], view_index, left.size(), array_size) !=
                resolved_array_slice(
                    right[view_index], view_index, right.size(), array_size) ||
            left[view_index].image_rect.offset_x !=
                right[view_index].image_rect.offset_x ||
            left[view_index].image_rect.offset_y !=
                right[view_index].image_rect.offset_y ||
            left[view_index].image_rect.width !=
                right[view_index].image_rect.width ||
            left[view_index].image_rect.height !=
                right[view_index].image_rect.height) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] D3D12ImageRect resolved_rect(
    const D3D12ImageRect& rect,
    UINT width,
    UINT height) noexcept {
    return rect.width == 0 && rect.height == 0
        ? D3D12ImageRect{0, 0, width, height}
        : rect;
}

[[nodiscard]] bool views_cover_resource(
    std::span<const D3D12ReprojectionView> views,
    UINT width, UINT height, UINT array_size) noexcept {
    if (!valid_view_layout(views, width, height, array_size)) return false;
    std::array<std::uint64_t, kMaxReprojectionViews> area{};
    for (std::size_t i = 0; i < views.size(); ++i) {
        const auto r = resolved_rect(views[i].image_rect, width, height);
        area[resolved_array_slice(views[i], i, views.size(), array_size)] +=
            std::uint64_t(r.width) * r.height;
    }
    // The layout validation establishes bounded, non-overlapping rectangles.
    // Equal area therefore establishes coverage of every physical slice.
    return std::all_of(area.begin(), area.begin() + array_size,
        [&](std::uint64_t value) { return value == std::uint64_t(width) * height; });
}

void set_viewport_and_scissor(
    ID3D12GraphicsCommandList* command_list,
    const D3D12ImageRect& image_rect,
    UINT width,
    UINT height) noexcept {
    const D3D12ImageRect rect = resolved_rect(image_rect, width, height);
    const D3D12_VIEWPORT viewport{
        static_cast<float>(rect.offset_x),
        static_cast<float>(rect.offset_y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height),
        0.0F,
        1.0F,
    };
    const D3D12_RECT scissor{
        static_cast<LONG>(rect.offset_x),
        static_cast<LONG>(rect.offset_y),
        static_cast<LONG>(rect.offset_x + rect.width),
        static_cast<LONG>(rect.offset_y + rect.height),
    };
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
}

void clear_synthetic_marker(ID3D12GraphicsCommandList* commands,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv, const D3D12ReprojectionView& view,
    UINT width, UINT height, const std::optional<OverlayPlacement>& marker,
    const float* colour = nullptr, float x_shift = 0.0F) noexcept {
    if (!marker) return; // No extra GPU work with diagnostics off.
    const auto& box = *marker;
    if (!finite(box.x) || !finite(box.y) || !finite(box.z) ||
        !finite(box.width) || !finite(box.height) || box.z >= -1.0e-5F ||
        box.width <= 0 || box.height <= 0) return;
    const auto rect = resolved_rect(view.image_rect, width, height);
    const float left = std::tan(view.fov.angle_left);
    const float right = std::tan(view.fov.angle_right);
    const float up = std::tan(view.fov.angle_up);
    const float down = std::tan(view.fov.angle_down);
    const float depth = -box.z;
    const float shifted = box.x + x_shift * box.width;
    const float x0 = ((shifted - box.width * 0.5F) / depth - left) / (right - left);
    const float x1 = ((shifted + box.width * 0.5F) / depth - left) / (right - left);
    const float y0 = ((box.y - box.height * 0.5F) / depth - up) / (down - up);
    const float y1 = ((box.y + box.height * 0.5F) / depth - up) / (down - up);
    if (!finite(x0) || !finite(x1) || !finite(y0) || !finite(y1)) return;
    const auto low = [](float a, float b, UINT offset, UINT extent) {
        return static_cast<LONG>(offset + std::floor(std::clamp(std::min(a, b), 0.0F, 1.0F) * extent));
    };
    const auto high = [](float a, float b, UINT offset, UINT extent) {
        return static_cast<LONG>(offset + std::ceil(std::clamp(std::max(a, b), 0.0F, 1.0F) * extent));
    };
    const D3D12_RECT mark{low(x0, x1, rect.offset_x, rect.width), low(y0, y1, rect.offset_y, rect.height),
                          high(x0, x1, rect.offset_x, rect.width), high(y0, y1, rect.offset_y, rect.height)};
    if (mark.left >= mark.right || mark.top >= mark.bottom) return;
    // Only the bridge-owned S render target is touched, AFTER interpolation.
    // It then follows the same fence, D3D11 publication and XR release as S.
    constexpr float purple[4]{0.75F, 0.0F, 1.0F, 1.0F};
    commands->ClearRenderTargetView(rtv, colour ? colour : purple, 1, &mark);
}



// A synthetic's target camera may be turned from B's, but it projects as B
// does: the same field of view into the same rectangle. That keeps the turn
// from the target into B a pure rotation, which the composition evaluates per
// pixel, and leaves the target where the runtime is told B's image lies.
[[nodiscard]] bool same_position(
    const Vec3& left,
    const Vec3& right) noexcept {
    const float delta_x = left.x - right.x;
    const float delta_y = left.y - right.y;
    const float delta_z = left.z - right.z;
    return delta_x * delta_x + delta_y * delta_y + delta_z * delta_z <=
           kPositionToleranceMeters * kPositionToleranceMeters;
}

[[nodiscard]] float quaternion_dot(
    const Quaternion& left,
    const Quaternion& right) noexcept {
    return left.x * right.x + left.y * right.y +
           left.z * right.z + left.w * right.w;
}

[[nodiscard]] bool same_orientation(
    Quaternion left,
    Quaternion right) noexcept {
    left = normalize(left);
    right = normalize(right);
    return std::abs(std::abs(quaternion_dot(left, right)) - 1.0F) <=
           kCameraTolerance;
}

[[nodiscard]] bool same_fov(
    const D3D12FieldOfView& left,
    const D3D12FieldOfView& right) noexcept {
    return std::abs(left.angle_left - right.angle_left) <= kCameraTolerance &&
           std::abs(left.angle_right - right.angle_right) <= kCameraTolerance &&
           std::abs(left.angle_up - right.angle_up) <= kCameraTolerance &&
           std::abs(left.angle_down - right.angle_down) <= kCameraTolerance;
}

[[nodiscard]] bool same_camera(
    const D3D12ReprojectionView& left,
    const D3D12ReprojectionView& right) noexcept {
    return same_position(left.pose.position, right.pose.position) &&
           same_orientation(left.pose.orientation, right.pose.orientation) &&
           same_fov(left.fov, right.fov) &&
           left.image_rect.offset_x == right.image_rect.offset_x &&
           left.image_rect.offset_y == right.image_rect.offset_y &&
           left.image_rect.width == right.image_rect.width &&
           left.image_rect.height == right.image_rect.height;
}

[[nodiscard]] bool same_projection(
    const D3D12ReprojectionView& left,
    const D3D12ReprojectionView& right) noexcept {
    return same_fov(left.fov, right.fov) &&
           left.image_rect.offset_x == right.image_rect.offset_x &&
           left.image_rect.offset_y == right.image_rect.offset_y &&
           left.image_rect.width == right.image_rect.width &&
           left.image_rect.height == right.image_rect.height;
}

[[nodiscard]] Quaternion multiply(
    const Quaternion& left,
    const Quaternion& right) noexcept {
    return {
        left.w * right.x + left.x * right.w +
            left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z +
            left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y -
            left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x -
            left.y * right.y - left.z * right.z,
    };
}

[[nodiscard]] std::array<float, 4> fov_tangents(
    const D3D12FieldOfView& fov) noexcept {
    return {
        std::tan(fov.angle_left),
        std::tan(fov.angle_right),
        std::tan(fov.angle_up),
        std::tan(fov.angle_down),
    };
}

[[nodiscard]] CameraMapping make_camera_mapping(
    const D3D12ReprojectionView& source,
    const D3D12ReprojectionView& target,
    UINT source_width,
    UINT source_height,
    UINT target_width,
    UINT target_height) noexcept {
    const Quaternion source_orientation = normalize(source.pose.orientation);
    const Quaternion target_orientation = normalize(target.pose.orientation);
    const Quaternion inverse_source{
        -source_orientation.x,
        -source_orientation.y,
        -source_orientation.z,
        source_orientation.w,
    };
    const Quaternion target_to_source =
        normalize(multiply(inverse_source, target_orientation));
    const D3D12ImageRect source_rect =
        resolved_rect(source.image_rect, source_width, source_height);
    const D3D12ImageRect target_rect =
        resolved_rect(target.image_rect, target_width, target_height);
    return {
        {
            target_to_source.x,
            target_to_source.y,
            target_to_source.z,
            target_to_source.w,
        },
        fov_tangents(source.fov),
        fov_tangents(target.fov),
        {
            static_cast<float>(source_rect.offset_x),
            static_cast<float>(source_rect.offset_y),
            static_cast<float>(source_rect.width),
            static_cast<float>(source_rect.height),
        },
        {
            static_cast<float>(target_rect.offset_x),
            static_cast<float>(target_rect.offset_y),
            static_cast<float>(target_rect.width),
            static_cast<float>(target_rect.height),
        },
    };
}

[[nodiscard]] CameraMapping make_camera_mapping(
    const D3D12ReprojectionView& source,
    const D3D12ReprojectionView& target,
    UINT width,
    UINT height) noexcept {
    return make_camera_mapping(
        source, target, width, height, width, height);
}

[[nodiscard]] std::array<float,4> target_to_current_rotation(const D3D12ReprojectionView& current, const D3D12ReprojectionView& target) noexcept {
    return make_camera_mapping(current,target,1,1).target_to_source_rotation;
}
[[nodiscard]] bool camera_turned(const std::array<float,4>& rotation) noexcept {
    return std::sqrt(rotation[0]*rotation[0]+rotation[1]*rotation[1]+rotation[2]*rotation[2]) > kSyntheticCameraMinimumTurn*0.5F;
}

}  // namespace

float usable_synthesis_fraction(float fraction, bool extrapolate) noexcept {
    if (!std::isfinite(fraction)) {
        return extrapolate ? 1.0F : 0.5F;
    }
    if (extrapolate) {
        return std::clamp(fraction, 1.0F, 3.0F);
    }
    return fraction > 0.05F && fraction < 0.95F ? fraction : 0.5F;
}

struct D3D12FrameSynthesizer::Impl {
    struct Destination {
        ComPtr<ID3D12Resource> resource;
        std::uint64_t fence_value{};
    };

    struct NvidiaEyeResources {
        ComPtr<ID3D12Resource> previous_input;
        ComPtr<ID3D12Resource> current_input;
        ComPtr<ID3D12Resource> flow;
        ComPtr<ID3D12Resource> cost;
        ComPtr<ID3D12Resource> backward_flow;
        ComPtr<ID3D12Resource> backward_cost;
        NvOFGPUBufferHandle previous_input_handle{};
        NvOFGPUBufferHandle current_input_handle{};
        NvOFGPUBufferHandle flow_handle{};
        NvOFGPUBufferHandle cost_handle{};
        NvOFGPUBufferHandle backward_flow_handle{};
        NvOFGPUBufferHandle backward_cost_handle{};
    };

    struct WorkSlot {
        D3D12RamTiming<5> ram_gpu;
        UINT64 ram_source_identity{}, ram_source_serial{};
        bool ram_pair{};
        // Original snapshots read by R22 remain pinned until this slot's fence.
        std::shared_ptr<const DlssMotionVectorSet> guide_composer_inputs;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        ComPtr<ID3D12CommandAllocator> synthesis_allocator;
        ComPtr<ID3D12GraphicsCommandList> synthesis_command_list;
        // The bit-exact copy of B into the current output. Recorded apart from
        // the synthesis and submitted only once the synthetic frame has been
        // handed to the runtime, so it is not queued ahead of it.
        ComPtr<ID3D12CommandAllocator> current_copy_allocator;
        ComPtr<ID3D12GraphicsCommandList> current_copy_command_list;
        ComPtr<ID3D12CommandAllocator> timing_marker_allocator;
        ComPtr<ID3D12GraphicsCommandList> timing_marker_command_list;
        ComPtr<ID3D12DescriptorHeap> descriptor_heap;
        ComPtr<ID3D12DescriptorHeap> cinema_source_heap;
        // Keeps the Witcher scene-only XR image alive until this GPU slot's
        // recorded FSR copy has retired.
        ComPtr<ID3D12Resource> hudless_source;
        std::array<NvidiaEyeResources, kMaxReprojectionViews> nvidia_eyes;
        D3D12NvidiaGpuTiming timing_metadata{};
        D3D12NvidiaGpuTiming cached_timing{};
        std::uint64_t fence_value{};
        UINT timing_query_base{};
        bool timing_pending{};
        bool timing_cached{};
    };

    struct RollingSource {
        D3D12HistoryCaptureTicket ticket{};
        D3D12HistoryConsumerLease lease{};
        ComPtr<ID3D12Resource> resource;
        ComPtr<ID3D12Fence> producer_fence;
        std::array<D3D12ReprojectionView, kMaxReprojectionViews> views{};
        UINT view_count{};
        std::uint64_t last_use_fence_value{};
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors;
        D3D12PresentationTransform presentation_transform{};

        [[nodiscard]] bool active() const noexcept {
            return lease.lease_serial != 0;
        }

        void clear() noexcept {
            ticket = {};
            lease = {};
            resource.Reset();
            producer_fence.Reset();
            views = {};
            view_count = 0;
            last_use_fence_value = 0;
            motion_vectors.reset();
            presentation_transform = {};
        }
    };



    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<D3D12SwapchainHistory> history;
    std::vector<Destination> current_destinations;
    std::vector<Destination> synthetic_destinations;
    static_assert(kWorkSlotCount <= D3D12GuideComposer::kSlots);
    D3D12GuideComposer guide_composer;
    bool guide_composer_tried{}, guide_composer_ready{};
    std::array<WorkSlot, kWorkSlotCount> work_slots;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT current_rtv_base{};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12RootSignature> fsr_cinema_root_signature;
    ComPtr<ID3D12PipelineState> pack_pipeline;
    ComPtr<ID3D12PipelineState> graphics_pipeline;
    ComPtr<ID3D12PipelineState> game_motion_graphics_pipeline;
    ComPtr<ID3D12PipelineState> fsr_reprojection_pipeline;
    ComPtr<ID3D12PipelineState> fsr_post_hud_pipeline;
    ComPtr<ID3D12PipelineState> fsr_cinema_panel_pipeline;
    ComPtr<ID3D12PipelineState> cinema_flatten_pipeline;
    ComPtr<ID3D12Resource> cinema_flat_color;
    ComPtr<ID3D12DescriptorHeap> cinema_flat_rtv_heap;
    ComPtr<ID3D12PipelineState> hybrid_graphics_pipeline;
    ComPtr<ID3D12PipelineState> hybrid_pack_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_pack_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_luma_pack_pipeline;
    DXGI_FORMAT nvidia_input_format{DXGI_FORMAT_B8G8R8A8_UNORM};
    ComPtr<ID3D12PipelineState> nvidia_graphics_pipeline;
    ComPtr<ID3D12PipelineState> nvidia_bidirectional_graphics_pipeline;
    ComPtr<ID3D12Resource> packed_color;
    ComPtr<ID3D12Resource> optical_flow_vector;
    ComPtr<ID3D12Resource> optical_flow_scene_change;
    std::vector<std::byte> ffx_scratch;
    FfxOpticalflowContext ffx_context{};
    HMODULE nvidia_module{};
    NV_OF_D3D12_API_FUNCTION_LIST nvidia_api{};
    std::array<NvOFHandle, kMaxReprojectionViews> nvidia_contexts{};
    ComPtr<ID3D12Fence> fence;
    // Carries the runtime's acquire guarantee from the queue it knows about
    // onto the one it does not. Kept apart from `fence` so it cannot disturb
    // the submission values the rest of the pipeline tracks against that one.
    ComPtr<ID3D12Fence> acquire_fence;
    std::uint64_t next_acquire_fence_value{1};
    [[nodiscard]] D3D12SynthesisInputs submission_inputs(const RollingSource& first,
        const RollingSource* second=nullptr) const noexcept {
        D3D12SynthesisInputs result;
        result.captured=true;
        result.dependencies[0]={first.producer_fence,first.ticket.fence_value};
        if(second) result.dependencies[1]={second->producer_fence,second->ticket.fence_value};
        if(next_acquire_fence_value>1)
            result.dependencies[2]={acquire_fence,next_acquire_fence_value-1};
        return result;
    }

    ComPtr<ID3D12Fence> nvidia_fence;
    ComPtr<ID3D12QueryHeap> nvidia_timestamp_heap;
    // Maps GPU ticks onto QueryPerformanceCounter, sampled once when the
    // heap is created. Without it a timestamp is only a duration; with it
    // it is a moment that can be compared to when the frame was submitted.
    std::uint64_t calibration_gpu_ticks{};
    std::uint64_t calibration_qpc_ticks{};
    std::uint64_t qpc_frequency{};
    ComPtr<ID3D12Resource> nvidia_timestamp_readback;
    HANDLE fence_event{};
    RollingSource previous;
    RollingSource untracked_source;
    D3D12_RESOURCE_DESC image_description{};
    DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
    D3D12_RESOURCE_STATES release_state{};
    UINT descriptor_increment{};
    UINT rtv_increment{};
    UINT packed_width{};
    UINT packed_height{};
    UINT packed_eye_stride{};
    UINT flow_width{};
    UINT flow_height{};
    UINT flow_block_size{};
    std::uint64_t next_fence_value{1};
    std::uint64_t last_submitted_fence_value{};
    // Set by each submit_pair from the interval its two captures span. Half
    // is only correct when the application runs at exactly half the display
    // rate; away from that the synthetic belongs somewhere else between them.
    float synthetic_fraction{0.5F};
    std::optional<D3D12ExtraSynthetic> extra_output;
    UINT synthetic_output_count() const noexcept { return extra_output ? 2U : 1U; }
    UINT synthetic_output_destination(UINT output, UINT first) const noexcept {
        return output ? extra_output->destination_index : first;
    }
    // Each synthetic output's target camera, per view, as the rotation from
    // it into the current capture's camera; set by every submit_pair beside
    // the fractions. An output whose camera is B's in every view is drawn
    // exactly as before, without the flag.
    std::array<std::array<std::array<float, 4>, kMaxReprojectionViews>, 2>
        synthetic_camera_rotations{};
    std::array<bool, 2> synthetic_camera_turned{};
    bool synthetic_camera_requested{}, fsr_pair_in_target_camera{};

    // Sets one composition draw's synthetic camera: the flag, and the rotation
    // from the output's camera into B's in the other view's mapping, which a
    // view's composition never reads for anything else - the pack, which
    // reads both, has drawn by then. The caller restores the pair's mappings
    // before each draw. A one-view resource's other mapping is unused anyway.
    void apply_synthetic_camera(
        SynthesisParameters& parameters,
        UINT output,
        UINT view_index) const noexcept {
        const bool turned = synthetic_camera_turned[output];
        parameters.synthesis_flags =
            (parameters.synthesis_flags & ~kSyntheticCameraFlag) |
            (turned ? kSyntheticCameraFlag : 0U);
        if (turned) {
            parameters.previous_mappings[1U - std::min(view_index, 1U)]
                .target_to_source_rotation =
                synthetic_camera_rotations[output][std::min(view_index, kMaxReprojectionViews - 1U)];
        }
    }

    float synthetic_output_fraction(UINT output) const noexcept {
        return output ? extra_output->interpolation_fraction : synthetic_fraction;
    }
    // The work slot whose current copy has been recorded and closed but not
    // yet submitted, and the fence value that submission will signal. The
    // slot cannot be recycled and the history lease cannot retire until it
    // has, so every entry point that needs either flushes first.
    static constexpr std::uint32_t kNoPendingCopy =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t pending_copy_slot{kNoPendingCopy};
    std::uint64_t pending_copy_fence_value{};
    // The value the most recent submission signalled once the synthetic's
    // pixels existed, as opposed to the value the whole pair completes at.
    std::uint64_t last_synthetic_fence_value{};
    std::array<std::uint64_t, 2> last_fsr_context_fence_values{};
    std::uint64_t next_nvidia_fence_value{1};
    std::uint64_t last_nvidia_fence_value{};
    std::uint64_t fsr_eye_diagnostic_pair{};
    std::uint64_t fsr_hudless_diagnostic_pair{};
    std::uint64_t nvidia_timestamp_frequency{};
    std::uint32_t next_work_slot{};
    std::uint32_t next_timing_slot{};
    D3D12OpticalFlowBackend backend{D3D12OpticalFlowBackend::fidelity_fx};
    D3D12NvidiaOpticalFlowOptions nvidia_options{};
    D3D12FrameGenerationMode generation_mode{D3D12FrameGenerationMode::ofxr};
    std::unique_ptr<D3D12NativeDlssG> native_dlss;
    bool native_pair_generated{};
    // Full projection and anchored Cinema use different FSR display domains.
    // Keep their contexts and temporal histories physically independent: the
    // Witcher can alternate both domains within a few OpenXR frames.
    std::array<std::unique_ptr<D3D12FsrFrameGenerator>, 2>
        fsr_frame_generators;
    bool synthesis_enabled{};
    bool flow_resources_deferred{};
    bool copy_resources_retired{};
    bool requested_gpu_timing{};
    bool completion_unknown{};
    std::array<bool, 2> fsr_context_reset_pending{};
    std::uint32_t active_fsr_domain{};
    bool recording_uses_fsr_context{};
    bool fsr_stale_guide_gap{};
    bool ffx_context_created{};
    bool flow_output_consumed{};
    bool nvidia_gpu_timing_enabled{};
    bool allow_luke_guide_device_alias{};
    ~Impl() {
        // Nothing outside this object waits on the synthesis queue. Its work
        // writes into the slot command allocators and reads the history
        // slots, so releasing them with submissions still in flight is a
        // use-after-free - which the debug layer reports at destruction
        // rather than where it happened. When the synthesis runs on the
        // application's queue the application drains it; on a queue of the
        // layer's own, nobody does.
        //
        // Signal a fresh value rather than waiting on the tracked one. A
        // deferred current copy that was never flushed has already reserved
        // last_submitted_fence_value, and nothing will ever signal it, so
        // waiting there would hang instead of draining. The acquire fence is
        // used because its values are not part of that bookkeeping.
        if (!copy_resources_retired && queue != nullptr && acquire_fence != nullptr &&
            fence_event != nullptr) {
            const std::uint64_t drain = next_acquire_fence_value++;
            if (SUCCEEDED(queue->Signal(acquire_fence.Get(), drain))) {
                static_cast<void>(wait_for_fence(acquire_fence.Get(), drain));
            }
        }
        if (ffx_context_created) {
            static_cast<void>(ffxOpticalflowContextDestroy(&ffx_context));
        }
        if (nvidia_fence != nullptr && last_nvidia_fence_value != 0) {
            static_cast<void>(wait_for_fence(
                nvidia_fence.Get(),
                last_nvidia_fence_value));
        }
        if (nvidia_api.nvOFUnregisterResourceD3D12 != nullptr) {
            for (WorkSlot& slot : work_slots) {
                for (NvidiaEyeResources& eye : slot.nvidia_eyes) {
                    const std::array<NvOFGPUBufferHandle*, 6> handles{
                        &eye.backward_cost_handle,
                        &eye.backward_flow_handle,
                        &eye.cost_handle,
                        &eye.flow_handle,
                        &eye.current_input_handle,
                        &eye.previous_input_handle,
                    };
                    for (NvOFGPUBufferHandle* handle : handles) {
                        if (*handle == nullptr) {
                            continue;
                        }
                        NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 parameters{};
                        parameters.hOFGpuBuffer = *handle;
                        static_cast<void>(
                            nvidia_api.nvOFUnregisterResourceD3D12(&parameters));
                        *handle = nullptr;
                    }
                }
            }
        }
        for (WorkSlot& slot : work_slots) {
            for (NvidiaEyeResources& eye : slot.nvidia_eyes) {
                eye.backward_cost.Reset();
                eye.backward_flow.Reset();
                eye.cost.Reset();
                eye.flow.Reset();
                eye.current_input.Reset();
                eye.previous_input.Reset();
            }
        }
        if (nvidia_api.nvOFDestroy != nullptr) {
            for (NvOFHandle& context : nvidia_contexts) {
                if (context == nullptr) {
                    continue;
                }
                static_cast<void>(nvidia_api.nvOFDestroy(context));
                context = nullptr;
            }
        }
        if (nvidia_module != nullptr) {
            FreeLibrary(nvidia_module);
            nvidia_module = nullptr;
        }
        if (fence_event != nullptr) {
            CloseHandle(fence_event);
        }
    }

    [[nodiscard]] HRESULT create_root_signature_and_pipelines(UINT node_mask) noexcept {
        std::array<D3D12_DESCRIPTOR_RANGE, 2> ranges{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = kSrvDescriptorCount;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = kUavDescriptorCount;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        std::array<D3D12_ROOT_PARAMETER, 3> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &ranges[1];
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 0;
        parameters[2].Constants.RegisterSpace = 0;
        parameters[2].Constants.Num32BitValues = kSynthesisConstantCount;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC root_description{};
        root_description.NumParameters = static_cast<UINT>(parameters.size());
        root_description.pParameters = parameters.data();
        root_description.NumStaticSamplers = 0;
        root_description.pStaticSamplers = nullptr;
        root_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> error;
        HRESULT result = D3D12SerializeRootSignature(
            &root_description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            error.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateRootSignature(
            node_mask,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(root_signature.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        D3D12_DESCRIPTOR_RANGE cinema_range{};
        cinema_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        cinema_range.NumDescriptors = 1;
        cinema_range.BaseShaderRegister = 0;
        cinema_range.OffsetInDescriptorsFromTableStart = 0;
        std::array<D3D12_ROOT_PARAMETER, 2> cinema_parameters{};
        cinema_parameters[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        cinema_parameters[0].DescriptorTable = {1, &cinema_range};
        cinema_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        cinema_parameters[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        cinema_parameters[1].Constants.ShaderRegister = 0;
        cinema_parameters[1].Constants.Num32BitValues =
            kFsrCinemaPanelConstantCount;
        cinema_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC cinema_sampler{};
        cinema_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        cinema_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        cinema_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        cinema_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        cinema_sampler.MaxAnisotropy = 1;
        cinema_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        cinema_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        cinema_sampler.MinLOD = 0.0F;
        cinema_sampler.MaxLOD = D3D12_FLOAT32_MAX;
        cinema_sampler.ShaderRegister = 0;
        cinema_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC cinema_root_description{};
        cinema_root_description.NumParameters =
            static_cast<UINT>(cinema_parameters.size());
        cinema_root_description.pParameters = cinema_parameters.data();
        cinema_root_description.NumStaticSamplers = 1;
        cinema_root_description.pStaticSamplers = &cinema_sampler;
        cinema_root_description.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        serialized.Reset();
        error.Reset();
        result = D3D12SerializeRootSignature(
            &cinema_root_description,
            D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(),
            error.GetAddressOf());
        if (FAILED(result)) return result;
        result = device->CreateRootSignature(
            node_mask,
            serialized->GetBufferPointer(),
            serialized->GetBufferSize(),
            IID_PPV_ARGS(fsr_cinema_root_signature.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_description{};
        compute_description.pRootSignature = root_signature.Get();
        compute_description.CS = {
            g_xrfg_pack_flow_input_shader,
            sizeof(g_xrfg_pack_flow_input_shader),
        };
        compute_description.NodeMask = node_mask;
        result = device->CreateComputePipelineState(
            &compute_description,
            IID_PPV_ARGS(pack_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        compute_description.CS = {
            g_xrfg_nvidia_pack_flow_input_shader,
            sizeof(g_xrfg_nvidia_pack_flow_input_shader),
        };
        result = device->CreateComputePipelineState(
            &compute_description,
            IID_PPV_ARGS(nvidia_pack_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        compute_description.CS = {g_xrfg_nvidia_pack_luma_input_shader,
            sizeof(g_xrfg_nvidia_pack_luma_input_shader)};
        result = device->CreateComputePipelineState(&compute_description,
            IID_PPV_ARGS(nvidia_luma_pack_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics_description{};
        graphics_description.pRootSignature = root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fullscreen_vertex_shader,
            sizeof(g_xrfg_fullscreen_vertex_shader),
        };
        graphics_description.PS = {
            g_xrfg_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_synthesize_midpoint_pixel_shader),
        };
        graphics_description.BlendState.AlphaToCoverageEnable = FALSE;
        graphics_description.BlendState.IndependentBlendEnable = FALSE;
        D3D12_RENDER_TARGET_BLEND_DESC& target_blend =
            graphics_description.BlendState.RenderTarget[0];
        target_blend.BlendEnable = FALSE;
        target_blend.LogicOpEnable = FALSE;
        target_blend.SrcBlend = D3D12_BLEND_ONE;
        target_blend.DestBlend = D3D12_BLEND_ZERO;
        target_blend.BlendOp = D3D12_BLEND_OP_ADD;
        target_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        target_blend.DestBlendAlpha = D3D12_BLEND_ZERO;
        target_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target_blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        target_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        graphics_description.SampleMask = std::numeric_limits<UINT>::max();
        graphics_description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        graphics_description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        graphics_description.RasterizerState.FrontCounterClockwise = FALSE;
        graphics_description.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        graphics_description.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        graphics_description.RasterizerState.SlopeScaledDepthBias =
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        graphics_description.RasterizerState.DepthClipEnable = TRUE;
        graphics_description.RasterizerState.MultisampleEnable = FALSE;
        graphics_description.RasterizerState.AntialiasedLineEnable = FALSE;
        graphics_description.RasterizerState.ForcedSampleCount = 0;
        graphics_description.RasterizerState.ConservativeRaster =
            D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        graphics_description.DepthStencilState.DepthEnable = FALSE;
        graphics_description.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        graphics_description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        graphics_description.DepthStencilState.StencilEnable = FALSE;
        graphics_description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        graphics_description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        graphics_description.InputLayout = {nullptr, 0};
        graphics_description.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        graphics_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        graphics_description.NumRenderTargets = 1;
        graphics_description.RTVFormats[0] = view_format;
        graphics_description.DSVFormat = DXGI_FORMAT_UNKNOWN;
        graphics_description.SampleDesc.Count = 1;
        graphics_description.SampleDesc.Quality = 0;
        graphics_description.NodeMask = node_mask;
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        graphics_description.PS = {
            g_xrfg_game_motion_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_game_motion_synthesize_midpoint_pixel_shader)};
        result = device->CreateGraphicsPipelineState(&graphics_description,
            IID_PPV_ARGS(game_motion_graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        graphics_description.PS = {
            g_xrfg_reproject_generated_pixel_shader,
            sizeof(g_xrfg_reproject_generated_pixel_shader),
        };
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(fsr_reprojection_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        graphics_description.PS = {
            g_xrfg_reproject_late_hud_pixel_shader,
            sizeof(g_xrfg_reproject_late_hud_pixel_shader),
        };
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(fsr_post_hud_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;

        graphics_description.pRootSignature = fsr_cinema_root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fsr_cinema_vertex_shader,
            sizeof(g_xrfg_fsr_cinema_vertex_shader),
        };
        graphics_description.PS = {
            g_xrfg_fsr_cinema_pixel_shader,
            sizeof(g_xrfg_fsr_cinema_pixel_shader),
        };
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(fsr_cinema_panel_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        // The pullback works on display-encoded UNORM values; no raw DLSS
        // float colour or tone-map assumptions enter this pipeline.
        graphics_description.pRootSignature = fsr_cinema_root_signature.Get();
        graphics_description.VS = {g_xrfg_cinema_flatten_vertex_shader,
                                  sizeof(g_xrfg_cinema_flatten_vertex_shader)};
        graphics_description.PS = {g_xrfg_cinema_flatten_pixel_shader,
                                  sizeof(g_xrfg_cinema_flatten_pixel_shader)};
        graphics_description.RTVFormats[0] = linear_color_view_format(view_format, view_format);
        result = device->CreateGraphicsPipelineState(&graphics_description,
            IID_PPV_ARGS(cinema_flatten_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        graphics_description.RTVFormats[0] = view_format;
        graphics_description.pRootSignature = root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fullscreen_vertex_shader,
            sizeof(g_xrfg_fullscreen_vertex_shader),
        };

        graphics_description.pRootSignature = fsr_cinema_root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fsr_cinema_vertex_shader,
            sizeof(g_xrfg_fsr_cinema_vertex_shader),
        };
        graphics_description.PS = {
            g_xrfg_fsr_cinema_pixel_shader,
            sizeof(g_xrfg_fsr_cinema_pixel_shader),
        };
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(fsr_cinema_panel_pipeline.GetAddressOf()));
        if (FAILED(result)) return result;
        graphics_description.pRootSignature = root_signature.Get();
        graphics_description.VS = {
            g_xrfg_fullscreen_vertex_shader,
            sizeof(g_xrfg_fullscreen_vertex_shader),
        };

        if (nvidia_options.hybrid) {
            auto hybrid_pack_description = compute_description;
            hybrid_pack_description.CS = {
                g_xrfg_hybrid_pack_flow_input_shader,
                sizeof(g_xrfg_hybrid_pack_flow_input_shader)};
            result = device->CreateComputePipelineState(&hybrid_pack_description,
                IID_PPV_ARGS(hybrid_pack_pipeline.GetAddressOf()));
            if (FAILED(result)) return result;
            graphics_description.PS = {
                g_xrfg_hybrid_synthesize_midpoint_pixel_shader,
                sizeof(g_xrfg_hybrid_synthesize_midpoint_pixel_shader)};
            result = device->CreateGraphicsPipelineState(&graphics_description,
                IID_PPV_ARGS(hybrid_graphics_pipeline.GetAddressOf()));
            if (FAILED(result)) return result;
        }
        graphics_description.PS = {
            g_xrfg_nvidia_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_nvidia_synthesize_midpoint_pixel_shader),
        };
        if (nvidia_options.preset == D3D12NvidiaPerformancePreset::fast) {
            graphics_description.PS = {
                g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader,
                sizeof(g_xrfg_nvidia_fast_synthesize_midpoint_pixel_shader),
            };
        }
        result = device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(nvidia_graphics_pipeline.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        graphics_description.PS = {
            g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader,
            sizeof(g_xrfg_nvidia_bidirectional_synthesize_midpoint_pixel_shader),
        };
        if (nvidia_options.preset == D3D12NvidiaPerformancePreset::fast) {
            graphics_description.PS = {
                g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader,
                sizeof(g_xrfg_nvidia_fast_bidirectional_synthesize_midpoint_pixel_shader),
            };
        }
        return device->CreateGraphicsPipelineState(
            &graphics_description,
            IID_PPV_ARGS(
                nvidia_bidirectional_graphics_pipeline.GetAddressOf()));
    }

    [[nodiscard]] HRESULT create_fidelityfx_resources(UINT node_mask) {
        const D3D12_HEAP_PROPERTIES heap_properties =
            default_heap_properties(node_mask);

        const D3D12_RESOURCE_DESC packed_description = texture2d_description(
            packed_width,
            packed_height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        HRESULT result = device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &packed_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(packed_color.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        const size_t scratch_size =
            ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT);
        if (scratch_size == 0) {
            return E_FAIL;
        }
        ffx_scratch.resize(scratch_size);

        FfxOpticalflowContextDescription context_description{};
        result = ffx_result(ffxGetInterfaceDX12(
            &context_description.backendInterface,
            ffxGetDeviceDX12(device.Get()),
            ffx_scratch.data(),
            ffx_scratch.size(),
            FFX_OPTICALFLOW_CONTEXT_COUNT));
        if (FAILED(result)) {
            return result;
        }
        context_description.flags = 0;
        context_description.resolution = {packed_width, packed_height};
        result = ffx_result(ffxOpticalflowContextCreate(
            &ffx_context,
            &context_description));
        if (FAILED(result)) {
            return result;
        }
        ffx_context_created = true;

        FfxOpticalflowSharedResourceDescriptions shared{};
        result = ffx_result(ffxOpticalflowGetSharedResourceDescriptions(
            &ffx_context,
            &shared));
        if (FAILED(result)) {
            return result;
        }
        flow_width = shared.opticalFlowVector.resourceDescription.width;
        flow_height = shared.opticalFlowVector.resourceDescription.height;
        if (flow_width == 0 || flow_height == 0 ||
            shared.opticalFlowVector.resourceDescription.depth != 1 ||
            shared.opticalFlowVector.resourceDescription.mipCount != 1 ||
            shared.opticalFlowSCD.resourceDescription.width != 3 ||
            shared.opticalFlowSCD.resourceDescription.height != 1) {
            return E_FAIL;
        }

        const D3D12_RESOURCE_DESC flow_description = texture2d_description(
            flow_width,
            flow_height,
            ffxGetDX12FormatFromSurfaceFormat(
                shared.opticalFlowVector.resourceDescription.format),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        result = device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &flow_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(optical_flow_vector.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        const D3D12_RESOURCE_DESC scene_change_description =
            texture2d_description(
                shared.opticalFlowSCD.resourceDescription.width,
                shared.opticalFlowSCD.resourceDescription.height,
                ffxGetDX12FormatFromSurfaceFormat(
                    shared.opticalFlowSCD.resourceDescription.format),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_NONE,
            &scene_change_description,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            IID_PPV_ARGS(optical_flow_scene_change.GetAddressOf()));
    }

    [[nodiscard]] bool nvidia_surface_format_supported(
        NvOFHandle context,
        NV_OF_BUFFER_USAGE usage,
        DXGI_FORMAT required_format) const noexcept {
        std::uint32_t count = 0;
        if (nvidia_api.nvOFGetSurfaceFormatCountD3D12(
                context,
                usage,
                NV_OF_MODE_OPTICALFLOW,
                &count) != NV_OF_SUCCESS ||
            count == 0) {
            return false;
        }
        try {
            std::vector<DXGI_FORMAT> formats(count);
            if (nvidia_api.nvOFGetSurfaceFormatD3D12(
                    context,
                    usage,
                    NV_OF_MODE_OPTICALFLOW,
                    formats.data()) != NV_OF_SUCCESS) {
                return false;
            }
            return std::find(formats.begin(), formats.end(), required_format) !=
                   formats.end();
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool nvidia_capability_contains(
        NvOFHandle context,
        NV_OF_CAPS capability,
        std::uint32_t required_value) const noexcept {
        std::uint32_t count = 0;
        if (nvidia_api.nvOFGetCaps(
                context,
                capability,
                nullptr,
                &count) != NV_OF_SUCCESS ||
            count == 0) {
            return false;
        }
        try {
            std::vector<std::uint32_t> values(count);
            if (nvidia_api.nvOFGetCaps(
                    context,
                    capability,
                    values.data(),
                    &count) != NV_OF_SUCCESS) {
                return false;
            }
            return std::find(values.begin(), values.begin() + count, required_value) !=
                   values.begin() + count;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool nvidia_dimension_supported(
        NvOFHandle context,
        NV_OF_CAPS minimum_capability,
        NV_OF_CAPS maximum_capability,
        std::uint32_t value) const noexcept {
        std::uint32_t minimum_count = 0;
        std::uint32_t maximum_count = 0;
        if (nvidia_api.nvOFGetCaps(
                context,
                minimum_capability,
                nullptr,
                &minimum_count) != NV_OF_SUCCESS ||
            nvidia_api.nvOFGetCaps(
                context,
                maximum_capability,
                nullptr,
                &maximum_count) != NV_OF_SUCCESS ||
            minimum_count != 1 || maximum_count != 1) {
            return false;
        }
        std::uint32_t minimum = 0;
        std::uint32_t maximum = 0;
        return nvidia_api.nvOFGetCaps(
                   context,
                   minimum_capability,
                   &minimum,
                   &minimum_count) == NV_OF_SUCCESS &&
               nvidia_api.nvOFGetCaps(
                   context,
                   maximum_capability,
                   &maximum,
                   &maximum_count) == NV_OF_SUCCESS &&
               value >= minimum && value <= maximum;
    }

    [[nodiscard]] HRESULT register_nvidia_resource(
        NvOFHandle context,
        ID3D12Resource* resource,
        NvOFGPUBufferHandle* output_handle) noexcept {
        if (resource == nullptr || output_handle == nullptr ||
            *output_handle != nullptr || fence == nullptr ||
            nvidia_fence == nullptr || next_nvidia_fence_value == 0 ||
            next_nvidia_fence_value ==
                std::numeric_limits<std::uint64_t>::max()) {
            return E_INVALIDARG;
        }
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 parameters{};
        parameters.resource = resource;
        parameters.inputFencePoint.fence = fence.Get();
        parameters.inputFencePoint.value = 0;
        parameters.hOFGpuBuffer = output_handle;
        parameters.outputFencePoint.fence = nvidia_fence.Get();
        parameters.outputFencePoint.value = next_nvidia_fence_value;
        const HRESULT result = nvidia_result(
            nvidia_api.nvOFRegisterResourceD3D12(
                context,
                &parameters));
        if (FAILED(result)) {
            return result;
        }
        last_nvidia_fence_value = next_nvidia_fence_value;
        ++next_nvidia_fence_value;
        return S_OK;
    }

    [[nodiscard]] HRESULT create_nvidia_resources(UINT node_mask) noexcept {
        nvidia_module = LoadLibraryExW(
            L"nvofapi64.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (nvidia_module == nullptr) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        using CreateInstanceFunction = NV_OF_STATUS(NVOFAPI*)(
            std::uint32_t,
            NV_OF_D3D12_API_FUNCTION_LIST*);
        const auto create_instance = reinterpret_cast<CreateInstanceFunction>(
            GetProcAddress(nvidia_module, "NvOFAPICreateInstanceD3D12"));
        if (create_instance == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        HRESULT result = nvidia_result(
            create_instance(NV_OF_API_VERSION, &nvidia_api));
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(nvidia_fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        flow_width =
            (packed_width + kNvidiaFlowBlockSize - 1U) /
            kNvidiaFlowBlockSize;
        flow_height =
            (packed_height + kNvidiaFlowBlockSize - 1U) /
            kNvidiaFlowBlockSize;
        const D3D12_HEAP_PROPERTIES heap_properties =
            default_heap_properties(node_mask);
        D3D12_RESOURCE_DESC input_description = texture2d_description(
            packed_width,
            packed_height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC flow_description = texture2d_description(
            flow_width,
            flow_height,
            DXGI_FORMAT_R16G16_SINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const D3D12_RESOURCE_DESC cost_description = texture2d_description(
            flow_width,
            flow_height,
            DXGI_FORMAT_R8_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        NV_OF_INIT_PARAMS initialization{};
        initialization.width = packed_width;
        initialization.height = packed_height;
        initialization.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        initialization.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        initialization.mode = NV_OF_MODE_OPTICALFLOW;
        switch (nvidia_options.preset) {
        case D3D12NvidiaPerformancePreset::fast:
            initialization.perfLevel = NV_OF_PERF_LEVEL_FAST;
            break;
        case D3D12NvidiaPerformancePreset::slow:
            initialization.perfLevel = NV_OF_PERF_LEVEL_SLOW;
            break;
        case D3D12NvidiaPerformancePreset::medium:
        default:
            initialization.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
            break;
        }
        initialization.enableExternalHints = NV_OF_FALSE;
        initialization.enableOutputCost = NV_OF_TRUE;
        initialization.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
        initialization.enableRoi = NV_OF_FALSE;
        initialization.predDirection = nvidia_options.bidirectional
            ? NV_OF_PRED_DIRECTION_BOTH
            : NV_OF_PRED_DIRECTION_FORWARD;
        initialization.enableGlobalFlow = NV_OF_FALSE;
        initialization.inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvOFHandle& context = nvidia_contexts[eye_index];
            result = nvidia_result(nvidia_api.nvCreateOpticalFlowD3D12(
                device.Get(),
                &context));
            if (FAILED(result) || context == nullptr) {
                return FAILED(result) ? result : E_FAIL;
            }
            if (eye_index == 0) {
                nvidia_input_format = nvidia_options.grayscale_input &&
                    nvidia_surface_format_supported(context, NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_R8_UNORM)
                    ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
                initialization.inputBufferFormat = nvidia_input_format == DXGI_FORMAT_R8_UNORM
                    ? NV_OF_BUFFER_FORMAT_GRAYSCALE8 : NV_OF_BUFFER_FORMAT_ABGR8;
                input_description.Format = nvidia_input_format;
            }
            if (!nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_INPUT,
                    nvidia_input_format) ||
                !nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_OUTPUT,
                    DXGI_FORMAT_R16G16_SINT) ||
                !nvidia_surface_format_supported(
                    context,
                    NV_OF_BUFFER_USAGE_COST,
                    DXGI_FORMAT_R8_UINT) ||
                !nvidia_capability_contains(
                    context,
                    NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
                    kNvidiaFlowBlockSize) ||
                !nvidia_dimension_supported(
                    context,
                    NV_OF_CAPS_WIDTH_MIN,
                    NV_OF_CAPS_WIDTH_MAX,
                    packed_width) ||
                !nvidia_dimension_supported(
                    context,
                    NV_OF_CAPS_HEIGHT_MIN,
                    NV_OF_CAPS_HEIGHT_MAX,
                    packed_height)) {
                return E_NOTIMPL;
            }
            result = nvidia_result(
                nvidia_api.nvOFInit(context, &initialization));
            if (FAILED(result)) {
                return result;
            }

            // Each command-list work slot owns a complete OFA surface set.
            // A later application frame can therefore be queued while the
            // previous slot is still executing without overwriting registered
            // input, flow or cost resources.  The per-eye contexts remain
            // independent and fence points preserve submission order.
            for (WorkSlot& slot : work_slots) {
                NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
                const std::array<std::pair<const D3D12_RESOURCE_DESC*,
                                           ComPtr<ID3D12Resource>*>, 4>
                    resources{{
                        {&input_description, &eye.previous_input},
                        {&input_description, &eye.current_input},
                        {&flow_description, &eye.flow},
                        {&cost_description, &eye.cost},
                    }};
                for (const auto& [description, resource] : resources) {
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(resource->GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                }

                if (nvidia_options.bidirectional) {
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        &flow_description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(eye.backward_flow.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    result = device->CreateCommittedResource(
                        &heap_properties,
                        D3D12_HEAP_FLAG_NONE,
                        &cost_description,
                        D3D12_RESOURCE_STATE_COMMON,
                        nullptr,
                        IID_PPV_ARGS(eye.backward_cost.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                }

                const std::array<
                    std::pair<ID3D12Resource*, NvOFGPUBufferHandle*>, 4>
                    registrations{{
                        {eye.previous_input.Get(), &eye.previous_input_handle},
                        {eye.current_input.Get(), &eye.current_input_handle},
                        {eye.flow.Get(), &eye.flow_handle},
                        {eye.cost.Get(), &eye.cost_handle},
                    }};
                for (const auto& [resource, handle] : registrations) {
                    result = register_nvidia_resource(context, resource, handle);
                    if (FAILED(result)) {
                        return result;
                    }
                }
                if (nvidia_options.bidirectional) {
                    result = register_nvidia_resource(
                        context,
                        eye.backward_flow.Get(),
                        &eye.backward_flow_handle);
                    if (FAILED(result)) {
                        return result;
                    }
                    result = register_nvidia_resource(
                        context,
                        eye.backward_cost.Get(),
                        &eye.backward_cost_handle);
                    if (FAILED(result)) {
                        return result;
                    }
                }
            }
        }
        return S_OK;
    }

    // A GPU timestamp as a QueryPerformanceCounter value, using the
    // calibration sampled at creation. Both clocks run at a fixed rate, so
    // one simultaneous sample plus the two frequencies is enough.
    [[nodiscard]] std::uint64_t timestamp_qpc(
        std::uint64_t gpu_ticks) const noexcept {
        if (calibration_gpu_ticks == 0 || qpc_frequency == 0 ||
            nvidia_timestamp_frequency == 0) {
            return 0;
        }
        const double gpu_delta =
            static_cast<double>(gpu_ticks) -
            static_cast<double>(calibration_gpu_ticks);
        const double qpc_delta = gpu_delta *
            static_cast<double>(qpc_frequency) /
            static_cast<double>(nvidia_timestamp_frequency);
        return static_cast<std::uint64_t>(
            static_cast<double>(calibration_qpc_ticks) + qpc_delta);
    }

    [[nodiscard]] HRESULT create_nvidia_timing_resources(
        UINT node_mask) noexcept {
        if (!nvidia_gpu_timing_enabled) {
            return S_OK;
        }
        HRESULT result = queue->GetTimestampFrequency(
            &nvidia_timestamp_frequency);
        if (FAILED(result) || nvidia_timestamp_frequency == 0) {
            return FAILED(result) ? result : E_FAIL;
        }

        D3D12_QUERY_HEAP_DESC query_description{};
        query_description.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_description.Count =
            kTimestampCount * static_cast<UINT>(work_slots.size());
        query_description.NodeMask = node_mask;
        LARGE_INTEGER qpc_hz{};
        if (QueryPerformanceFrequency(&qpc_hz) && qpc_hz.QuadPart > 0) {
            qpc_frequency = static_cast<std::uint64_t>(qpc_hz.QuadPart);
        }
        if (FAILED(queue->GetClockCalibration(
                &calibration_gpu_ticks, &calibration_qpc_ticks))) {
            calibration_gpu_ticks = 0;
            calibration_qpc_ticks = 0;
        }
        result = device->CreateQueryHeap(
            &query_description,
            IID_PPV_ARGS(nvidia_timestamp_heap.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        D3D12_HEAP_PROPERTIES readback_heap{};
        readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
        readback_heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        readback_heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        readback_heap.CreationNodeMask = node_mask;
        readback_heap.VisibleNodeMask = node_mask;
        D3D12_RESOURCE_DESC readback_description{};
        readback_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_description.Alignment = 0;
        readback_description.Width =
            static_cast<UINT64>(query_description.Count) * sizeof(std::uint64_t);
        readback_description.Height = 1;
        readback_description.DepthOrArraySize = 1;
        readback_description.MipLevels = 1;
        readback_description.Format = DXGI_FORMAT_UNKNOWN;
        readback_description.SampleDesc.Count = 1;
        readback_description.SampleDesc.Quality = 0;
        readback_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        readback_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        return device->CreateCommittedResource(
            &readback_heap,
            D3D12_HEAP_FLAG_NONE,
            &readback_description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(nvidia_timestamp_readback.GetAddressOf()));
    }

    [[nodiscard]] HRESULT create_work_slots(UINT node_mask) noexcept {

        descriptor_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (descriptor_increment == 0) {
            return E_FAIL;
        }

        for (WorkSlot& slot : work_slots) {
            slot.timing_query_base = static_cast<UINT>(
                &slot - work_slots.data()) * kTimestampCount;
            if (backend == D3D12OpticalFlowBackend::nvidia)
                slot.ram_gpu.initialize(device.Get(),queue.Get(),BridgeFlightOperation::gpu_synthesis);
            HRESULT result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(slot.allocator.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandList(
                node_mask,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.allocator.Get(),
                nullptr,
                IID_PPV_ARGS(slot.command_list.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            // Stable DRED names let the host identify which bridge phase was executing when
            // a GPU device removal occurred. They have no effect on command recording/order.
            slot.command_list->SetName(L"OFXR Pack and Composite");
            result = slot.command_list->Close();
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(slot.current_copy_allocator.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = device->CreateCommandList(
                node_mask,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                slot.current_copy_allocator.Get(),
                nullptr,
                IID_PPV_ARGS(slot.current_copy_command_list.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            result = slot.current_copy_command_list->Close();
            if (FAILED(result)) {
                return result;
            }
            if (backend == D3D12OpticalFlowBackend::nvidia &&
                generation_mode == D3D12FrameGenerationMode::ofxr && !flow_resources_deferred) {
                result = device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(slot.synthesis_allocator.GetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                result = device->CreateCommandList(
                    node_mask,
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    slot.synthesis_allocator.Get(),
                    nullptr,
                    IID_PPV_ARGS(slot.synthesis_command_list.GetAddressOf()));
                if (FAILED(result)) {
                    return result;
                }
                slot.synthesis_command_list->SetName(L"OFXR NVIDIA Synthesis");
                result = slot.synthesis_command_list->Close();
                if (FAILED(result)) {
                    return result;
                }
                if (nvidia_gpu_timing_enabled) {
                    result = device->CreateCommandAllocator(
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        IID_PPV_ARGS(
                            slot.timing_marker_allocator.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    result = device->CreateCommandList(
                        node_mask,
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        slot.timing_marker_allocator.Get(),
                        nullptr,
                        IID_PPV_ARGS(
                            slot.timing_marker_command_list.GetAddressOf()));
                    if (FAILED(result)) {
                        return result;
                    }
                    slot.timing_marker_command_list->SetName(L"OFXR NVIDIA Timing");
                    result = slot.timing_marker_command_list->Close();
                    if (FAILED(result)) {
                        return result;
                    }
                }
            }

            D3D12_DESCRIPTOR_HEAP_DESC descriptor_description{};
            descriptor_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            descriptor_description.NumDescriptors = kDescriptorCount;
            descriptor_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            descriptor_description.NodeMask = node_mask;
            result = device->CreateDescriptorHeap(
                &descriptor_description,
                IID_PPV_ARGS(slot.descriptor_heap.GetAddressOf()));
            if (FAILED(result)) {
                return result;
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start =
                slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();

            D3D12_SHADER_RESOURCE_VIEW_DESC srv_description{};
            srv_description.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_description.Texture2D.MostDetailedMip = 0;
            srv_description.Texture2D.MipLevels = 1;
            srv_description.Texture2D.PlaneSlice = 0;
            srv_description.Texture2D.ResourceMinLODClamp = 0.0F;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav_description{};
            uav_description.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav_description.Texture2D.MipSlice = 0;
            uav_description.Texture2D.PlaneSlice = 0;
            if (backend == D3D12OpticalFlowBackend::nvidia) {
                for (UINT eye_index = 0;
                     eye_index < image_description.DepthOrArraySize;
                     ++eye_index) {
                    const UINT base = eye_index * kDescriptorBlockSize;
                    NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
                    srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                    device->CreateShaderResourceView(
                        eye.flow.Get(),
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 2,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R8_UINT;
                    device->CreateShaderResourceView(
                        eye.cost.Get(),
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 3,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                    device->CreateShaderResourceView(
                        nvidia_options.bidirectional
                            ? eye.backward_flow.Get()
                            : nullptr,
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 4,
                            descriptor_increment));
                    srv_description.Format = DXGI_FORMAT_R8_UINT;
                    device->CreateShaderResourceView(
                        nvidia_options.bidirectional
                            ? eye.backward_cost.Get()
                            : nullptr,
                        &srv_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + 5,
                            descriptor_increment));
                    uav_description.Format = nvidia_input_format;
                    device->CreateUnorderedAccessView(
                        nullptr,
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount,
                            descriptor_increment));
                    device->CreateUnorderedAccessView(
                        eye.previous_input.Get(),
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount + 1,
                            descriptor_increment));
                    device->CreateUnorderedAccessView(
                        eye.current_input.Get(),
                        nullptr,
                        &uav_description,
                        offset_cpu_handle(
                            cpu_start,
                            base + kSrvDescriptorCount + 2,
                            descriptor_increment));
                }
            } else {
                srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                device->CreateShaderResourceView(
                    optical_flow_vector.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 2, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R32_UINT;
                device->CreateShaderResourceView(
                    optical_flow_scene_change.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 3, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R16G16_SINT;
                device->CreateShaderResourceView(
                    optical_flow_vector.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 4, descriptor_increment));
                srv_description.Format = DXGI_FORMAT_R32_UINT;
                device->CreateShaderResourceView(
                    optical_flow_scene_change.Get(),
                    &srv_description,
                    offset_cpu_handle(cpu_start, 5, descriptor_increment));
                uav_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                device->CreateUnorderedAccessView(
                    packed_color.Get(),
                    nullptr,
                    &uav_description,
                    offset_cpu_handle(
                        cpu_start,
                        kSrvDescriptorCount,
                        descriptor_increment));
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC null_motion{};
            null_motion.Format = DXGI_FORMAT_R16G16_FLOAT;
            null_motion.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            null_motion.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            null_motion.Texture2DArray.MostDetailedMip = 0;
            null_motion.Texture2DArray.MipLevels = 1;
            null_motion.Texture2DArray.FirstArraySlice = 0;
            null_motion.Texture2DArray.ArraySize = 1;
            const UINT block_count = backend == D3D12OpticalFlowBackend::nvidia
                ? image_description.DepthOrArraySize
                : 1U;
            for (UINT block = 0; block < block_count; ++block) {
                device->CreateShaderResourceView(nullptr, &null_motion,
                    offset_cpu_handle(cpu_start,
                        block * kDescriptorBlockSize + 6U,
                        descriptor_increment));
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT create_rtv_heap(UINT node_mask) noexcept {
        // Synthetic destinations first, then current: the current ones exist
        // only so the diagnostic marker can be drawn on the real frame, which
        // is otherwise indistinguishable from a frame that never arrived.
        const UINT64 descriptor_count =
            static_cast<UINT64>(synthetic_destinations.size() +
                current_destinations.size()) *
            image_description.DepthOrArraySize;
        if (descriptor_count == 0 ||
            descriptor_count > std::numeric_limits<UINT>::max()) {
            return E_INVALIDARG;
        }

        D3D12_DESCRIPTOR_HEAP_DESC description{};
        description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        description.NumDescriptors = static_cast<UINT>(descriptor_count);
        description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        description.NodeMask = node_mask;
        HRESULT result = device->CreateDescriptorHeap(
            &description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        rtv_increment = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        if (rtv_increment == 0) {
            return E_FAIL;
        }

        D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
        rtv_description.Format = view_format;
        rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv_description.Texture2DArray.MipSlice = 0;
        rtv_description.Texture2DArray.ArraySize = 1;
        rtv_description.Texture2DArray.PlaneSlice = 0;
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        UINT descriptor_index = 0;
        for (const Destination& destination : synthetic_destinations) {
            for (UINT slice = 0; slice < image_description.DepthOrArraySize;
                 ++slice) {
                rtv_description.Texture2DArray.FirstArraySlice = slice;
                device->CreateRenderTargetView(
                    destination.resource.Get(),
                    &rtv_description,
                    offset_cpu_handle(start, descriptor_index, rtv_increment));
                ++descriptor_index;
            }
        }
        current_rtv_base = descriptor_index;
        for (const Destination& destination : current_destinations) {
            for (UINT slice = 0; slice < image_description.DepthOrArraySize;
                 ++slice) {
                rtv_description.Texture2DArray.FirstArraySlice = slice;
                device->CreateRenderTargetView(
                    destination.resource.Get(),
                    &rtv_description,
                    offset_cpu_handle(start, descriptor_index, rtv_increment));
                ++descriptor_index;
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* input_device,
        ID3D12CommandQueue* input_queue,
        std::shared_ptr<D3D12SwapchainHistory> input_history,
        std::span<ID3D12Resource* const> input_current_images,
        std::span<ID3D12Resource* const> input_synthetic_images,
        DXGI_FORMAT input_view_format,
        D3D12_RESOURCE_STATES input_release_state,
        D3D12OpticalFlowBackend input_backend,
        D3D12NvidiaOpticalFlowOptions input_nvidia_options,
        bool enable_nvidia_gpu_timing,
        D3D12FrameGenerationMode input_generation_mode,
        bool input_allow_luke_guide_device_alias,
        bool defer_flow_resources = false) {
        // Initialization-only F9 diagnostics. Preserve every validation rule;
        // the public HRESULT alone cannot identify a failed transport contract.
        const auto invalid_argument=[&](std::uint32_t stage,std::size_t index=0,
            ID3D12Resource* resource=nullptr) -> HRESULT {
            auto& log=bridge_flight_logger();
            if(log.enabled()) {
                constexpr std::uint64_t tag=0x661D0000ULL;
                const auto emit=[&](std::uint32_t part,std::uint64_t b,std::uint64_t c) {
                    log.event(BridgeFlightOperation::synthesis_initialize,E_INVALIDARG,tag|part,b,c);
                };
                emit(stage,index,reinterpret_cast<std::uintptr_t>(resource));
                emit(100,reinterpret_cast<std::uintptr_t>(input_device),reinterpret_cast<std::uintptr_t>(input_queue));
                emit(101,(std::uint64_t(input_current_images.size())<<32)|input_synthetic_images.size(),
                    (std::uint64_t(input_release_state)<<32)|static_cast<std::uint32_t>(input_view_format));
                emit(102,static_cast<std::uint64_t>(input_backend),static_cast<std::uint64_t>(input_generation_mode));
                std::uint64_t bad=(!input_device?1ULL:0)|(!input_queue?2ULL:0)|(!input_history?4ULL:0)|
                    (input_history&&!input_history->initialized()?8ULL:0)|
                    (input_current_images.empty()?16ULL:0)|(input_synthetic_images.empty()?32ULL:0)|
                    ((input_release_state!=D3D12_RESOURCE_STATE_RENDER_TARGET && input_release_state!=D3D12_RESOURCE_STATE_COMMON)?64ULL:0);
                if(input_queue) {
                    const auto q=input_queue->GetDesc();
                    if(q.Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)bad|=128;
                    if(!same_device(input_device,input_queue))bad|=256;
                    ComPtr<ID3D12Device> owner;
                    const auto hr=input_queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));
                    emit(103,reinterpret_cast<std::uintptr_t>(owner.Get()),static_cast<std::uint32_t>(hr));
                    emit(104,(std::uint64_t(q.Type)<<32)|q.NodeMask,static_cast<std::uint32_t>(q.Priority));
                }
                emit(105,bad,(std::uint64_t(defer_flow_resources)<<1)|input_allow_luke_guide_device_alias);
                if(resource) {
                    const auto desc=resource->GetDesc();
                    emit(110,(std::uint64_t(desc.Width)<<32)|desc.Height,
                        (std::uint64_t(desc.Format)<<32)|desc.DepthOrArraySize);
                    emit(111,(std::uint64_t(desc.Flags)<<32)|desc.MipLevels,
                        (std::uint64_t(desc.SampleDesc.Count)<<32)|desc.SampleDesc.Quality);
                    emit(112,static_cast<std::uint64_t>(desc.Dimension),static_cast<std::uint64_t>(desc.Layout));
                    ComPtr<ID3D12Device> owner;
                    const auto hr=resource->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()));
                    emit(113,reinterpret_cast<std::uintptr_t>(owner.Get()),static_cast<std::uint32_t>(hr));
                }
            }
            return E_INVALIDARG;
        };
        if (input_device == nullptr || input_queue == nullptr ||
            input_history == nullptr || !input_history->initialized() ||
            input_current_images.empty() || input_synthetic_images.empty() ||
            (input_release_state != D3D12_RESOURCE_STATE_RENDER_TARGET &&
             input_release_state != D3D12_RESOURCE_STATE_COMMON) ||
            input_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT ||
            !same_device(input_device, input_queue) ||
            (input_backend != D3D12OpticalFlowBackend::fidelity_fx &&
             input_backend != D3D12OpticalFlowBackend::nvidia) ||
            (input_generation_mode != D3D12FrameGenerationMode::ofxr &&
             input_generation_mode != D3D12FrameGenerationMode::fsr3 &&
             input_generation_mode != D3D12FrameGenerationMode::native_dlss)) {
            return invalid_argument(1);
        }

        ID3D12Resource* const first = input_current_images.front();
        if (first == nullptr || !same_adapter(input_device, first)) {
            return invalid_argument(2,0,first);
        }
        const D3D12_RESOURCE_DESC description = first->GetDesc();
        if (description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            description.Width == 0 ||
            description.Width > static_cast<UINT64>(std::numeric_limits<LONG>::max()) ||
            description.Height == 0 ||
            description.Height > static_cast<UINT>(std::numeric_limits<LONG>::max()) ||
            description.DepthOrArraySize == 0 ||
            description.DepthOrArraySize > kMaxReprojectionViews ||
            description.MipLevels != 1 ||
            description.SampleDesc.Count != 1 ||
            description.SampleDesc.Quality != 0 ||
            (description.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
            !compatible_view_format(description.Format, input_view_format)) {
            return invalid_argument(3,0,first);
        }

        D3D12_FEATURE_DATA_FORMAT_SUPPORT format_support{};
        format_support.Format = input_view_format;
        HRESULT result = input_device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT,
            &format_support,
            sizeof(format_support));
        constexpr D3D12_FORMAT_SUPPORT1 kRequiredSupport =
            D3D12_FORMAT_SUPPORT1_TEXTURE2D |
            D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
            D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
        if (FAILED(result) ||
            (format_support.Support1 & kRequiredSupport) != kRequiredSupport) {
            return FAILED(result) ? result : E_NOTIMPL;
        }

        D3D12_FEATURE_DATA_FORMAT_SUPPORT packed_format_support{};
        packed_format_support.Format =
            input_backend == D3D12OpticalFlowBackend::nvidia
                ? DXGI_FORMAT_B8G8R8A8_UNORM
                : DXGI_FORMAT_R8G8B8A8_UNORM;
        result = input_device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT,
            &packed_format_support,
            sizeof(packed_format_support));
        if (FAILED(result) ||
            (packed_format_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) == 0 ||
            (packed_format_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
            return FAILED(result) ? result : E_NOTIMPL;
        }

        const auto contains_resource = [](
            const std::vector<Destination>& retained,
            ID3D12Resource* resource) noexcept {
            return std::any_of(
                retained.begin(),
                retained.end(),
                [resource](const Destination& destination) noexcept {
                    return destination.resource.Get() == resource;
                });
        };

        current_destinations.reserve(input_current_images.size());
        for (ID3D12Resource* image : input_current_images) {
            if (image == nullptr || !same_adapter(input_device, image) ||
                !copy_compatible(description, image->GetDesc()) ||
                (image->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
                !compatible_view_format(image->GetDesc().Format, input_view_format) ||
                contains_resource(current_destinations, image)) {
                return invalid_argument(4,current_destinations.size(),image);
            }
            Destination destination{};
            destination.resource = image;
            current_destinations.push_back(std::move(destination));
        }

        synthetic_destinations.reserve(input_synthetic_images.size());
        for (ID3D12Resource* image : input_synthetic_images) {
            if (image == nullptr || !same_adapter(input_device, image) ||
                !copy_compatible(description, image->GetDesc()) ||
                !compatible_view_format(image->GetDesc().Format, input_view_format) ||
                (image->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0 ||
                contains_resource(current_destinations, image) ||
                contains_resource(synthetic_destinations, image)) {
                return invalid_argument(5,synthetic_destinations.size(),image);
            }
            Destination destination{};
            destination.resource = image;
            synthetic_destinations.push_back(std::move(destination));
        }

        const UINT width = static_cast<UINT>(description.Width);
        flow_block_size =
            input_backend == D3D12OpticalFlowBackend::nvidia
                ? kNvidiaFlowBlockSize
                : kFidelityFxFlowBlockSize;
        const bool separate_nvidia_eyes =
            input_backend == D3D12OpticalFlowBackend::nvidia;
        // One input scale for whichever backend is running. The FidelityFX
        // path packs both eyes into one texture, so its stride and packed
        // height come from the scaled per-eye height, not the source.
        const NvidiaInputScaleRatio scale =
            nvidia_input_scale_ratio(input_nvidia_options.input_scale);
        const auto scaled = [&](UINT64 value) -> UINT64 {
            return (value * scale.numerator + scale.denominator - 1U) /
                scale.denominator;
        };
        const UINT64 flow_input_width = scaled(width);
        const UINT64 flow_input_height =
            scaled(description.Height);
        const UINT64 stride = separate_nvidia_eyes
            ? 0U
            : (flow_input_height + kEyeGapPixels +
               flow_block_size - 1U) /
                  flow_block_size * flow_block_size;
        const UINT64 unaligned_packed_height = separate_nvidia_eyes
            ? flow_input_height
            : stride * (description.DepthOrArraySize - 1U) +
                  flow_input_height;
        const UINT64 packed_height_value = std::max<UINT64>(
            (unaligned_packed_height + flow_block_size - 1U) /
                flow_block_size * flow_block_size,
            kMinimumFlowDimension);
        const UINT64 packed_width_value = std::max<UINT64>(
            (flow_input_width + flow_block_size - 1U) /
                flow_block_size * flow_block_size,
            kMinimumFlowDimension);
        if ((!separate_nvidia_eyes &&
             description.Height >
                 D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION - kEyeGapPixels) ||
            stride > std::numeric_limits<UINT>::max() ||
            unaligned_packed_height > std::numeric_limits<UINT>::max() ||
            packed_width_value > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            packed_height_value > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return invalid_argument(6,0,first);
        }
        packed_width = static_cast<UINT>(packed_width_value);
        packed_height = static_cast<UINT>(packed_height_value);
        packed_eye_stride = static_cast<UINT>(stride);

        device = input_device;
        queue = input_queue;
        history = std::move(input_history);
        image_description = description;
        view_format = input_view_format;
        release_state = input_release_state;
        backend = input_backend;
        nvidia_options = input_nvidia_options;
        generation_mode = input_generation_mode;
        // Timing is no longer NVIDIA only: the span marks below are written by
        // both backends, and FidelityFX is the one that had no measurement.
        allow_luke_guide_device_alias =
            input_allow_luke_guide_device_alias;
        flow_resources_deferred = defer_flow_resources && input_generation_mode == D3D12FrameGenerationMode::ofxr;
        requested_gpu_timing = enable_nvidia_gpu_timing;
        nvidia_gpu_timing_enabled = enable_nvidia_gpu_timing && !flow_resources_deferred;
        const UINT node_mask = queue->GetDesc().NodeMask;

        result = create_root_signature_and_pipelines(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        result = device->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(acquire_fence.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_NOT_ENOUGH_MEMORY : error);
        }
        // FSR owns its optical-flow contexts. Its dispatch returns before the
        // ordinary OFXR optical backend, just as native DLSS-G does. Selecting
        // it must not allocate an unused second flow engine per swapchain.
        result = flow_resources_deferred || generation_mode != D3D12FrameGenerationMode::ofxr ? S_OK
                 : backend == D3D12OpticalFlowBackend::nvidia
                     ? create_nvidia_resources(node_mask)
                     : create_fidelityfx_resources(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = create_nvidia_timing_resources(node_mask);
        if (FAILED(result)) {
            nvidia_gpu_timing_enabled = false;
            nvidia_timestamp_frequency = 0;
            nvidia_timestamp_readback.Reset();
            nvidia_timestamp_heap.Reset();
        }
        result = create_work_slots(node_mask);
        if (FAILED(result)) {
            return result;
        }
        result = create_rtv_heap(node_mask);
        if (FAILED(result)) {
            return result;
        }
        if (generation_mode == D3D12FrameGenerationMode::native_dlss) {
            native_dlss = std::make_unique<D3D12NativeDlssG>();
            result = native_dlss->initialize(device.Get(), queue.Get(), image_description, view_format, nvidia_options.native_scale);
            if (FAILED(result)) return result;
        }
        if (generation_mode == D3D12FrameGenerationMode::fsr3) {
            for (auto& generator : fsr_frame_generators) {
                generator = std::make_unique<D3D12FsrFrameGenerator>();
                result = generator->initialize(
                    device.Get(), image_description, view_format);
                if (FAILED(result)) return result;
            }
        }
        synthesis_enabled = true;
        return S_OK;
    }

    [[nodiscard]] static HRESULT fence_status(
        ID3D12Fence* input_fence,
        std::uint64_t value) noexcept {
        if (value == 0) {
            return S_OK;
        }
        if (input_fence == nullptr) {
            return E_UNEXPECTED;
        }
        const std::uint64_t completed = input_fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            return E_FAIL;
        }
        return completed >= value ? S_OK : S_FALSE;
    }

    [[nodiscard]] std::uint64_t timestamp_microseconds(
        std::uint64_t begin,
        std::uint64_t end) const noexcept {
        if (end < begin || nvidia_timestamp_frequency == 0) {
            return 0;
        }
        const long double microseconds =
            static_cast<long double>(end - begin) * 1'000'000.0L /
            static_cast<long double>(nvidia_timestamp_frequency);
        return microseconds >=
                static_cast<long double>(
                    std::numeric_limits<std::uint64_t>::max())
            ? std::numeric_limits<std::uint64_t>::max()
            : static_cast<std::uint64_t>(microseconds + 0.5L);
    }

    [[nodiscard]] HRESULT harvest_nvidia_gpu_timing(
        WorkSlot& slot) noexcept {
        if (!nvidia_gpu_timing_enabled || !slot.timing_pending) {
            return S_FALSE;
        }
        const HRESULT status = fence_status(fence.Get(), slot.fence_value);
        if (status != S_OK) {
            return status;
        }

        const SIZE_T byte_begin =
            static_cast<SIZE_T>(slot.timing_query_base) *
            sizeof(std::uint64_t);
        const SIZE_T byte_end = byte_begin +
            kTimestampCount * sizeof(std::uint64_t);
        const D3D12_RANGE read_range{byte_begin, byte_end};
        void* mapped = nullptr;
        HRESULT result = nvidia_timestamp_readback->Map(
            0, &read_range, &mapped);
        if (FAILED(result) || mapped == nullptr) {
            slot.timing_pending = false;
            return FAILED(result) ? result : E_FAIL;
        }
        const auto* timestamps = static_cast<const std::uint64_t*>(mapped) +
            slot.timing_query_base;
        std::array<std::uint64_t, kTimestampCount> values{};
        std::copy_n(timestamps, values.size(), values.begin());
        const D3D12_RANGE no_write{0, 0};
        nvidia_timestamp_readback->Unmap(0, &no_write);

        // The span marks are written by both backends; the stage marks only
        // by NVIDIA, so their ordering is checked only where they exist.
        slot.cached_timing = slot.timing_metadata;
        slot.cached_timing.gpu_begin_qpc = timestamp_qpc(values[kSpanBegin]);
        slot.cached_timing.gpu_end_qpc = timestamp_qpc(values[kSpanEnd]);
        slot.cached_timing.total_microseconds = timestamp_microseconds(
            values[kSpanBegin], values[kSpanEnd]);
        if (backend != D3D12OpticalFlowBackend::nvidia) {
            slot.timing_pending = false;
            slot.timing_cached = true;
            return S_OK;
        }
        if (values[kNvidiaTimestampPackBegin] >
                values[kNvidiaTimestampPackEnd] ||
            values[kNvidiaTimestampPackEnd] >
                values[kNvidiaTimestampEye0End] ||
            values[kNvidiaTimestampEye0End] >
                values[kNvidiaTimestampCompositionBegin] ||
            values[kNvidiaTimestampCompositionBegin] >
                values[kNvidiaTimestampCompositionEnd]) {
            slot.timing_pending = false;
            return E_FAIL;
        }

        slot.cached_timing.pack_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackBegin],
            values[kNvidiaTimestampPackEnd]);
        slot.cached_timing.eye0_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackEnd],
            values[kNvidiaTimestampEye0End]);
        slot.cached_timing.eye1_microseconds =
            slot.cached_timing.eye_count > 1
                ? timestamp_microseconds(
                      values[kNvidiaTimestampEye0End],
                      values[kNvidiaTimestampCompositionBegin])
                : 0;
        slot.cached_timing.composition_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampCompositionBegin],
            values[kNvidiaTimestampCompositionEnd]);
        slot.cached_timing.total_microseconds = timestamp_microseconds(
            values[kNvidiaTimestampPackBegin],
            values[kNvidiaTimestampCompositionEnd]);
        slot.timing_pending = false;
        slot.timing_cached = true;
        return S_OK;
    }

    [[nodiscard]] HRESULT consume_nvidia_gpu_timing(
        D3D12NvidiaGpuTiming* output_timing) noexcept {
        if (output_timing == nullptr) {
            return E_POINTER;
        }
        *output_timing = {};
        if (!nvidia_gpu_timing_enabled) {
            return S_FALSE;
        }
        for (std::uint32_t offset = 0; offset < work_slots.size(); ++offset) {
            const std::uint32_t index =
                (next_timing_slot + offset) %
                static_cast<std::uint32_t>(work_slots.size());
            WorkSlot& slot = work_slots[index];
            if (!slot.timing_cached) {
                const HRESULT result = harvest_nvidia_gpu_timing(slot);
                if (FAILED(result)) {
                    return result;
                }
            }
            if (slot.timing_cached) {
                *output_timing = slot.cached_timing;
                slot.cached_timing = {};
                slot.timing_cached = false;
                next_timing_slot = (index + 1U) %
                    static_cast<std::uint32_t>(work_slots.size());
                return S_OK;
            }
        }
        return S_FALSE;
    }

    [[nodiscard]] HRESULT wait_for_fence(
        ID3D12Fence* input_fence,
        std::uint64_t value) noexcept {
        if (fence_event == nullptr) {
            return E_UNEXPECTED;
        }
        for (;;) {
            const HRESULT status = fence_status(input_fence, value);
            if (status != S_FALSE) {
                return status;
            }
            HRESULT result = input_fence->SetEventOnCompletion(value, fence_event);
            if (FAILED(result)) {
                return result;
            }
            if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0) {
                const DWORD error = GetLastError();
                return HRESULT_FROM_WIN32(
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            }
            // A prior bounded wait may still own a registration on this
            // reusable event. Its earlier fence can therefore wake us before
            // the target above; validate and re-arm instead of reporting a
            // false synchronization failure.
        }
    }

    [[nodiscard]] HRESULT submission_available() noexcept {
        if (last_submitted_fence_value == 0) {
            return S_OK;
        }
        const HRESULT status =
            fence_status(fence.Get(), last_submitted_fence_value);
        if (status == S_FALSE) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        if (FAILED(status)) {
            synthesis_enabled = false;
        }
        return status;
    }


    [[nodiscard]] HRESULT wait_for_previous_submission(
        std::uint32_t timeout_milliseconds) noexcept {
        if (last_submitted_fence_value == 0) {
            return S_OK;
        }

        // last_submitted_fence_value is the value a deferred copy will signal,
        // so waiting for it without submitting that copy can only time out.
        static_cast<void>(flush_pending_copy());

        const HRESULT initial_status =
            fence_status(fence.Get(), last_submitted_fence_value);
        if (initial_status == S_OK) {
            return S_OK;
        }
        if (FAILED(initial_status)) {
            synthesis_enabled = false;
            return initial_status;
        }
        if (fence_event == nullptr) {
            synthesis_enabled = false;
            return E_UNEXPECTED;
        }

        auto& flight = bridge_flight_logger();
        const auto token = flight.begin(
            BridgeFlightOperation::synthesis_frame_start_wait,
            reinterpret_cast<std::uintptr_t>(fence.Get()),
            last_submitted_fence_value,
            fence->GetCompletedValue());

        HRESULT result = S_FALSE;
        const ULONGLONG deadline =
            GetTickCount64() + timeout_milliseconds;
        while (result == S_FALSE) {
            result = fence_status(fence.Get(), last_submitted_fence_value);
            if (result != S_FALSE) {
                break;
            }
            result = fence->SetEventOnCompletion(
                last_submitted_fence_value,
                fence_event);
            if (FAILED(result)) {
                break;
            }
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                result = HRESULT_FROM_WIN32(ERROR_BUSY);
                break;
            }
            const DWORD remaining = static_cast<DWORD>(deadline - now);
            const DWORD wait_result = WaitForSingleObject(fence_event, remaining);
            if (wait_result == WAIT_TIMEOUT) {
                result = HRESULT_FROM_WIN32(ERROR_BUSY);
            } else if (wait_result != WAIT_OBJECT_0) {
                const DWORD error = GetLastError();
                result = HRESULT_FROM_WIN32(
                    error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
            } else {
                // May be a stale registration left by an earlier bounded
                // wait. The loop checks the actual fence and re-arms while
                // preserving the original deadline.
                result = S_FALSE;
                if (GetTickCount64() >= deadline) {
                    result = HRESULT_FROM_WIN32(ERROR_BUSY);
                }
            }
        }

        flight.end(
            token,
            BridgeFlightOperation::synthesis_frame_start_wait,
            result,
            reinterpret_cast<std::uintptr_t>(fence.Get()),
            last_submitted_fence_value,
            fence->GetCompletedValue());
        if (FAILED(result) &&
            result != HRESULT_FROM_WIN32(ERROR_BUSY)) {
            synthesis_enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT destination_available(
        const Destination& destination) noexcept {
        const HRESULT status = fence_status(fence.Get(), destination.fence_value);
        if (status == S_FALSE) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        if (FAILED(status)) {
            synthesis_enabled = false;
        }
        return status;
    }

    [[nodiscard]] HRESULT acquire_work_slot(std::uint32_t* output_index) noexcept {
        if (output_index == nullptr) {
            return E_POINTER;
        }
        for (std::uint32_t offset = 0; offset < work_slots.size(); ++offset) {
            const std::uint32_t index =
                (next_work_slot + offset) %
                static_cast<std::uint32_t>(work_slots.size());
            const HRESULT status =
                fence_status(fence.Get(), work_slots[index].fence_value);
            if (status == S_OK) {
                const HRESULT timing_result =
                    harvest_nvidia_gpu_timing(work_slots[index]);
                if (FAILED(timing_result)) {
                    work_slots[index].timing_pending = false;
                }
                *output_index = index;
                return S_OK;
            }
            if (FAILED(status)) {
                synthesis_enabled = false;
                return status;
            }
        }
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    }

    [[nodiscard]] HRESULT acquire_source(
        const D3D12HistoryCaptureTicket& ticket,
        std::span<const D3D12ReprojectionView> source_views,
        RollingSource* output_source) noexcept {
        if (output_source == nullptr) {
            return E_POINTER;
        }
        output_source->clear();
        if (!valid_view_layout(
                source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize)) {
            return E_INVALIDARG;
        }
        ID3D12Resource* raw_resource = nullptr;
        ID3D12Fence* raw_fence = nullptr;
        HRESULT result = history->acquire_consumer(
            ticket,
            &output_source->lease,
            &raw_resource,
            &raw_fence);
        if (FAILED(result)) {
            return result;
        }
        output_source->ticket = ticket;
        output_source->resource.Attach(raw_resource);
        output_source->producer_fence.Attach(raw_fence);
        output_source->view_count = static_cast<UINT>(source_views.size());
        std::copy(
            source_views.begin(),
            source_views.end(),
            output_source->views.begin());
        if (!same_adapter(device.Get(), output_source->resource.Get()) ||
            !same_adapter(device.Get(), output_source->producer_fence.Get()) ||
            !copy_compatible(
                image_description,
                output_source->resource->GetDesc()) ||
            !compatible_view_format(
                output_source->resource->GetDesc().Format,
                view_format)) {
            history->cancel_consumer(output_source->lease);
            output_source->clear();
            return E_INVALIDARG;
        }
        return S_OK;
    }

    void create_source_views(
        WorkSlot& slot,
        ID3D12Resource* previous_resource,
        ID3D12Resource* current_resource,
        DXGI_FORMAT previous_view_format = DXGI_FORMAT_UNKNOWN,
        UINT descriptor_blocks = 0) noexcept {
        D3D12_SHADER_RESOURCE_VIEW_DESC description{};
        description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        description.Texture2DArray.MostDetailedMip = 0;
        description.Texture2DArray.MipLevels = 1;
        description.Texture2DArray.FirstArraySlice = 0;
        description.Texture2DArray.ArraySize = image_description.DepthOrArraySize;
        description.Texture2DArray.PlaneSlice = 0;
        description.Texture2DArray.ResourceMinLODClamp = 0.0F;
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT block_count = std::max<UINT>(image_description.DepthOrArraySize, descriptor_blocks);
        for (UINT block = 0; block < block_count; ++block) {
            const UINT base = block * kDescriptorBlockSize;
            if (block >= image_description.DepthOrArraySize) {
                // Shader-visible heaps cannot be copy sources. Populate valid
                // null views for the OFA-only slots this FG shader never reads.
                D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
                null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                null_view.Texture2D.MipLevels = 1;
                for (UINT index = 2; index < kSrvDescriptorCount; ++index) {
                    null_view.Format = index == 3 || index == 5
                        ? (backend == D3D12OpticalFlowBackend::nvidia ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R32_UINT)
                        : DXGI_FORMAT_R16G16_SINT;
                    device->CreateShaderResourceView(nullptr, &null_view,
                        offset_cpu_handle(start, base + index, descriptor_increment));
                }
                D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav{};
                null_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                null_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                for (UINT index = 0; index < kUavDescriptorCount; ++index)
                    device->CreateUnorderedAccessView(nullptr, nullptr, &null_uav,
                        offset_cpu_handle(start, base + kSrvDescriptorCount + index, descriptor_increment));
            }
            // Ordinary A/B are OpenXR images. Preserve the negotiated sRGB
            // decode even when their storage is typeless. Only raw FSR cinema
            // colour has a separate view contract, supplied explicitly below.
            description.Format = previous_view_format == DXGI_FORMAT_UNKNOWN
                ? view_format : previous_view_format;
            device->CreateShaderResourceView(
                previous_resource,
                &description,
                offset_cpu_handle(start, base, descriptor_increment));
            description.Format = view_format;
            device->CreateShaderResourceView(
                current_resource,
                &description,
                offset_cpu_handle(start, base + 1, descriptor_increment));
        }
    }

    [[nodiscard]] bool compatible_game_motion_resource(
        const DlssMotionVectorFrame& frame,
        ID3D12Resource* resource) const noexcept {
        if (same_device(device.Get(), resource)) return true;
        // Luke Ross' Streamline interposer can expose a different canonical
        // device identity from a resource's GetDevice. Accept that alias only
        // for the Luke bridge, only on the same adapter, and only when the
        // in-process publisher retained the exact producer device it already
        // verified while recording the snapshot copy.
        return allow_luke_guide_device_alias &&
            same_adapter(device.Get(), resource) &&
            frame.verified_producer_device &&
            same_identity(
                device.Get(), frame.verified_producer_device.Get());
    }

    [[nodiscard]] HRESULT create_game_motion_views(
        WorkSlot& slot,
        const DlssMotionVectorSet& frames, UINT expected_eyes) noexcept {
        if (frames.eye_count == 0 ||
            frames.eye_count != expected_eyes ||
            frames.eye_count > kDlssMotionVectorEyeCount) {
            return E_INVALIDARG;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT eye = 0; eye < frames.eye_count; ++eye) {
            const auto& frame = frames.eyes[eye];
            if (!frame || !frame->motion_vectors ||
                !compatible_game_motion_resource(
                    *frame, frame->motion_vectors.Get())) {
                return E_INVALIDARG;
            }
            const D3D12_RESOURCE_DESC resource = frame->motion_vectors->GetDesc();
            const DXGI_FORMAT format = motion_vector_view_format(resource.Format);
            if (format == DXGI_FORMAT_UNKNOWN ||
                resource.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                resource.MipLevels == 0 ||
                frame->motion_slice >= resource.DepthOrArraySize ||
                frame->motion_width == 0 || frame->motion_height == 0 ||
                static_cast<std::uint64_t>(frame->motion_x) +
                        frame->motion_width > resource.Width ||
                static_cast<std::uint64_t>(frame->motion_y) +
                        frame->motion_height > resource.Height) {
                return E_INVALIDARG;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC description{};
            description.Format = format;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            description.Texture2DArray.MostDetailedMip = 0;
            description.Texture2DArray.MipLevels = 1;
            description.Texture2DArray.FirstArraySlice = frame->motion_slice;
            description.Texture2DArray.ArraySize = 1;
            description.Texture2DArray.PlaneSlice = 0;
            device->CreateShaderResourceView(frame->motion_vectors.Get(), &description,
                offset_cpu_handle(start,
                    eye * kDescriptorBlockSize + 6U,
                    descriptor_increment));
        }
        return S_OK;
    }

    // NOTE: the synthetic destination is drawn into while the private
    // swapchains rest in COMMON, which is not a state D3D12 promotes to
    // RENDER_TARGET implicitly - it promotes into the copy and shader-read
    // states, not that one. Bracketing the draws with an explicit
    // COMMON -> RENDER_TARGET -> COMMON pair is correct by the spec and was
    // tried in V148: it cost about a third of the frame rate and made frame
    // times erratic, which is what a barrier through COMMON does to a large
    // render target - a full decompress, and on this path a serialisation
    // against the other queue that gives back the overlap the private queue
    // exists to win.
    //
    // So the draws are left as they are. This is undefined behaviour that
    // NVIDIA tolerates, and the whole dedicated-queue path is NVIDIA-only by
    // decision, but it is undefined behaviour and it is written down here
    // rather than discovered again. The way out is not a cheaper barrier: it
    // is to stop writing runtime-owned images from the private queue at all,
    // compositing into layer-owned intermediates that can carry
    // ALLOW_SIMULTANEOUS_ACCESS and copying across on the application's
    // queue - which is what the D3D11 interop path already does.

    [[nodiscard]] std::uint64_t game_motion_pair_failure_code(
        const RollingSource& previous_source,
        const RollingSource& current_source,
        UINT expected_eye_count) const noexcept {
        const auto& previous_vectors = previous_source.motion_vectors;
        const auto& current = current_source.motion_vectors;
        if (!previous_vectors) return 1;
        if (!current) return 2;
        if (previous_vectors->eye_count == 0) return 3;
        if (previous_vectors->eye_count != current->eye_count) return 4;
        if (current->eye_count != expected_eye_count) return 5;
        if (current->eye_count > kDlssMotionVectorEyeCount) return 6;
        for (UINT eye = 0; eye < current->eye_count; ++eye) {
            const auto& a = previous_vectors->eyes[eye];
            const auto& b = current->eyes[eye];
            if (!a) return 10 + 100 * eye;
            if (!b) return 11 + 100 * eye;
            if (b->reset) return 12 + 100 * eye;
            if (a->stream != b->stream) return 13 + 100 * eye;
            if (a->epoch != b->epoch) return 14 + 100 * eye;
            if (a->serial == 0) return 15 + 100 * eye;
            if (b->previous_serial != a->serial) return 16 + 100 * eye;
            if (b->serial <= a->serial) return 17 + 100 * eye;
            if (!b->motion_vectors) return 18 + 100 * eye;
            if (!b->producer_queue) return 19 + 100 * eye;
            if (!compatible_game_motion_resource(
                    *b, b->motion_vectors.Get()))
                return 20 + 100 * eye;
        }
        return 0;
    }

    [[nodiscard]] bool valid_game_motion_pair(
        const RollingSource& previous_source,
        const RollingSource& current_source,
        UINT expected_eye_count) const noexcept {
        return game_motion_pair_failure_code(
            previous_source, current_source, expected_eye_count) == 0;
    }

    [[nodiscard]] bool has_complete_game_motion_set(
        const RollingSource& source) const noexcept {
        const auto& frames = source.motion_vectors;
        // Packed stereo has two viewports in one texture slice. Match the
        // same eye domain used by record_fsr_pair and can_pair_fsr, so a valid
        // packed guide set can prime instead of being rejected indefinitely.
        const UINT expected_guide_eyes = std::min<UINT>(
            std::max<UINT>(image_description.DepthOrArraySize, source.view_count),
            kDlssMotionVectorEyeCount);
        if (!frames || frames->eye_count == 0 ||
            frames->eye_count != expected_guide_eyes ||
            frames->eye_count > kDlssMotionVectorEyeCount) {
            return false;
        }
        for (UINT eye = 0; eye < frames->eye_count; ++eye) {
            const auto& frame = frames->eyes[eye];
            if (!frame || frame->stream == 0 || frame->epoch == 0 ||
                frame->serial == 0) {
                return false;
            }
        }
        return true;
    }

    void synthetic_target_state(ID3D12GraphicsCommandList* list,
                                std::uint32_t index, bool rendering) {
        if (release_state == D3D12_RESOURCE_STATE_RENDER_TARGET) return;
        const auto barrier = transition_barrier(synthetic_destinations[index].resource.Get(),
            rendering ? release_state : D3D12_RESOURCE_STATE_RENDER_TARGET,
            rendering ? D3D12_RESOURCE_STATE_RENDER_TARGET : release_state);
        list->ResourceBarrier(1, &barrier);
    }

    [[nodiscard]] bool unchanged_game_motion_pair(
        const RollingSource& previous_source,
        const RollingSource& current_source,
        UINT expected_eye_count) const noexcept {
        const auto& previous_vectors = previous_source.motion_vectors;
        const auto& current = current_source.motion_vectors;
        if (!previous_vectors || !current || previous_vectors->eye_count == 0 ||
            previous_vectors->eye_count != current->eye_count ||
            current->eye_count != expected_eye_count ||
            current->eye_count > kDlssMotionVectorEyeCount) {
            return false;
        }
        for (UINT eye = 0; eye < current->eye_count; ++eye) {
            const auto& a = previous_vectors->eyes[eye];
            const auto& b = current->eyes[eye];
            if (!a || !b || a->reset || b->reset || a->stream != b->stream ||
                a->epoch != b->epoch || a->serial == 0 ||
                a->serial != b->serial ||
                a->previous_serial != b->previous_serial) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] HRESULT record_game_motion_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        const auto& guides = current_source.motion_vectors;
        if (!guides || !valid_game_motion_pair(
                previous_source, current_source,
                std::min<UINT>(std::max<UINT>(image_description.DepthOrArraySize,
                    current_source.view_count), kDlssMotionVectorEyeCount))) {
            return E_INVALIDARG;
        }
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector begin");
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        ID3D12Resource* const synthetic_destination =
            synthetic_destinations[synthetic_destination_index].resource.Get();
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            ID3D12Resource* const motion_resource = guides->eyes[eye]->motion_vectors.Get();
            if (motion_resource == previous_resource || motion_resource == current_resource ||
                motion_resource == current_destination ||
                motion_resource == synthetic_destination) {
                return E_INVALIDARG;
            }
        }
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            const UINT slice = resolved_array_slice(target_views[view_index], view_index,
                target_views.size(), image_description.DepthOrArraySize);
            if (slice >= guides->eye_count) return E_INVALIDARG;
            const UINT guide_eye = image_description.DepthOrArraySize == 1 && guides->eye_count == target_views.size()
                ? view_index : slice;
            const auto& guide = guides->eyes[guide_eye];
            const D3D12ImageRect rect = resolved_rect(
                target_views[view_index].image_rect,
                static_cast<UINT>(image_description.Width), image_description.Height);
            const std::uint64_t rect_right =
                static_cast<std::uint64_t>(rect.offset_x) + rect.width;
            const std::uint64_t rect_bottom =
                static_cast<std::uint64_t>(rect.offset_y) + rect.height;
            const std::uint64_t guide_right =
                static_cast<std::uint64_t>(guide->output_x) + guide->output_width;
            const std::uint64_t guide_bottom =
                static_cast<std::uint64_t>(guide->output_y) + guide->output_height;
            if (rect.width == 0 || rect.height == 0 ||
                rect.offset_x < guide->output_x || rect.offset_y < guide->output_y ||
                rect_right > guide_right || rect_bottom > guide_bottom) {
                return E_INVALIDARG;
            }
        }
        create_source_views(slot, previous_resource, current_resource,
            DXGI_FORMAT_UNKNOWN, guides->eye_count);
        HRESULT result = create_game_motion_views(slot, *guides, std::min<UINT>(
            std::max<UINT>(image_description.DepthOrArraySize, current_source.view_count),
            kDlssMotionVectorEyeCount));
        if (FAILED(result)) return result;
        // NVIDIA work slots also own a second command list. This route bypasses
        // OFA, but the list was reset with the slot and must still be closed
        // before the slot can be reused on a later frame.
        if (slot.synthesis_command_list) {
            result = slot.synthesis_command_list->Close();
            if (FAILED(result)) return result;
        }

        std::array<D3D12_RESOURCE_BARRIER, 4> before{};
        UINT before_count = 0;
        before[before_count++] = transition_barrier(previous_resource,
            D3D12_RESOURCE_STATE_COMMON, kShaderReadState);
        before[before_count++] = transition_barrier(current_resource,
            D3D12_RESOURCE_STATE_COMMON, kShaderReadState);
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            const auto& guide = guides->eyes[eye];
            if (guide->resource_state != kShaderReadState &&
                (eye == 0 || guide->motion_vectors.Get() !=
                                 guides->eyes[0]->motion_vectors.Get())) {
                before[before_count++] = transition_barrier(guide->motion_vectors.Get(),
                    guide->resource_state, kShaderReadState);
            }
        }
        slot.command_list->ResourceBarrier(before_count, before.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        const auto gpu_start = slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetPipelineState(game_motion_graphics_pipeline.Get());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector compose");
        slot.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.flow_block_size = 1;
        parameters.use_game_motion = 1;
        // A against B's own camera: the synthetic's, where it has one of its
        // own, is applied per draw.
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index], current_source.views[view_index],
                static_cast<UINT>(image_description.Width), image_description.Height);
        }
        const auto view_mappings = parameters.previous_mappings;

        const auto rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            const UINT output_destination = synthetic_output_destination(output, synthetic_destination_index);
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv = output_destination * image_description.DepthOrArraySize;
            synthetic_target_state(slot.command_list.Get(), output_destination, true);
            for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
                const UINT slice = resolved_array_slice(target_views[view_index], view_index,
                    target_views.size(), image_description.DepthOrArraySize);
                if (slice >= guides->eye_count) return E_INVALIDARG;
                const UINT guide_eye = image_description.DepthOrArraySize == 1 && guides->eye_count == target_views.size()
                    ? view_index : slice;
                const auto& guide = guides->eyes[guide_eye];
                const UINT descriptor_base = guide_eye * kDescriptorBlockSize;
                slot.command_list->SetGraphicsRootDescriptorTable(0,
                    offset_gpu_handle(gpu_start, descriptor_base, descriptor_increment));
                slot.command_list->SetGraphicsRootDescriptorTable(1,
                    offset_gpu_handle(gpu_start, descriptor_base + kSrvDescriptorCount,
                        descriptor_increment));
                parameters.slice = slice;
                parameters.view_index = view_index;
                // The optical-flow fields are unused by the game-motion branch.
                // Reuse them to carry the DLSS output rectangle associated with
                // the stable eye stream without growing the root constants.
                parameters.flow_width = guide->output_x;
                parameters.flow_height = guide->output_y;
                parameters.flow_block_size = guide->output_width;
                parameters.game_motion_padding = guide->output_height;
                // A guide placed by its camera (MSFS 2024's foveated
                // periphery) maps the fractional part of its vectors under
                // the image rectangle, so the scale into image pixels is the
                // camera's.
                parameters.game_motion_rect = guide_motion_rect(*guide);
                // DLSS low-resolution vectors are expressed in render-space pixel
                // units after applying MV_Scale. Synthesis and pose reprojection
                // operate in the final output rectangle's pixel space, so convert
                // both vector components into that same coordinate system.
                parameters.game_motion_scale = {
                    guide->scale_x * static_cast<float>(guide->output_width) /
                        parameters.game_motion_rect[2],
                    guide->scale_y * static_cast<float>(guide->output_height) /
                        parameters.game_motion_rect[3]};
                parameters.game_motion_jitter_delta = guide->jittered
                    ? std::array<float, 2>{
                          guide->jitter_x - guide->previous_jitter_x,
                          guide->jitter_y - guide->previous_jitter_y}
                    : std::array<float, 2>{};
                parameters.previous_mappings = view_mappings;
                apply_synthetic_camera(parameters, output, view_index);
                slot.command_list->SetGraphicsRoot32BitConstants(2,
                    kSynthesisConstantCount, &parameters, 0);
                const auto rtv = offset_cpu_handle(rtv_start, first_rtv + slice, rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(slot.command_list.Get(), target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width), image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
            synthetic_target_state(slot.command_list.Get(), output_destination, false);
        }

        std::array<D3D12_RESOURCE_BARRIER, 5> after{};
        UINT after_count = 0;
        after[after_count++] = transition_barrier(previous_resource,
            kShaderReadState, D3D12_RESOURCE_STATE_COMMON);
        after[after_count++] = transition_barrier(current_resource,
            kShaderReadState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        after[after_count++] = transition_barrier(current_destination,
            release_state, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT eye = 0; eye < guides->eye_count; ++eye) {
            const auto& guide = guides->eyes[eye];
            if (guide->resource_state != kShaderReadState &&
                (eye == 0 || guide->motion_vectors.Get() !=
                                 guides->eyes[0]->motion_vectors.Get())) {
                after[after_count++] = transition_barrier(guide->motion_vectors.Get(),
                    kShaderReadState, guide->resource_state);
            }
        }
        slot.command_list->ResourceBarrier(after_count, after.data());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector current copy");
        slot.command_list->CopyResource(current_destination, current_resource);
        const std::array<D3D12_RESOURCE_BARRIER, 2> finish{
            transition_barrier(current_resource, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(current_destination, D3D12_RESOURCE_STATE_COPY_DEST,
                release_state)};
        slot.command_list->ResourceBarrier(static_cast<UINT>(finish.size()), finish.data());
        dred_marker(slot.command_list.Get(), "OFXR DLSS-vector complete");
        return S_OK;
    }

    [[nodiscard]] HRESULT record_cinema_color(
        WorkSlot& slot, const RollingSource& source,
        std::span<const D3D12ReprojectionView> views) noexcept {
        // One shared scratch array is safe: all writes and FSR reads are on
        // the same synthesis queue, in submission order. It is never resized
        // or destroyed independently of the synthesizer's existing lifetime.
        const DXGI_FORMAT format = linear_color_view_format(view_format, view_format);
        if (!cinema_flat_color) {
            auto desc = image_description;
            desc.Format = format;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            const auto properties = default_heap_properties(0);
            HRESULT hr = device->CreateCommittedResource(&properties,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                nullptr, IID_PPV_ARGS(cinema_flat_color.GetAddressOf()));
            if (FAILED(hr)) return hr;
            cinema_flat_color->SetName(L"OFXR Cinema final-colour pullback");
        }
        if (!cinema_flat_rtv_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = image_description.DepthOrArraySize;
            const HRESULT hr = device->CreateDescriptorHeap(&heap,
                IID_PPV_ARGS(cinema_flat_rtv_heap.GetAddressOf()));
            if (FAILED(hr)) return hr;
            for (UINT slice = 0; slice < heap.NumDescriptors; ++slice) {
                D3D12_RENDER_TARGET_VIEW_DESC rtv{};
                rtv.Format = format;
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.FirstArraySlice = slice;
                rtv.Texture2DArray.ArraySize = 1;
                device->CreateRenderTargetView(cinema_flat_color.Get(), &rtv,
                    offset_cpu_handle(cinema_flat_rtv_heap->GetCPUDescriptorHandleForHeapStart(), slice, rtv_increment));
            }
        }
        if (!slot.cinema_source_heap) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 1;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            const HRESULT hr = device->CreateDescriptorHeap(&heap,
                IID_PPV_ARGS(slot.cinema_source_heap.GetAddressOf()));
            if (FAILED(hr)) return hr;
        }
        // Validate every eye before recording the first transition.
        for (UINT eye = 0; eye < views.size(); ++eye) {
            const UINT slice = resolved_array_slice(views[eye], eye, views.size(), image_description.DepthOrArraySize);
            if (slice >= source.motion_vectors->eye_count || !source.motion_vectors->eyes[slice]) return E_INVALIDARG;
            const auto& guide = *source.motion_vectors->eyes[slice];
            const D3D12ImageRect rect{guide.output_x, guide.output_y, guide.output_width, guide.output_height};
            if (rect.width == 0 || rect.height == 0 ||
                !rect_within_resource(rect, static_cast<UINT>(image_description.Width), image_description.Height)) return E_INVALIDARG;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = view_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = image_description.DepthOrArraySize;
        device->CreateShaderResourceView(source.resource.Get(), &srv,
            slot.cinema_source_heap->GetCPUDescriptorHandleForHeapStart());
        const std::array<D3D12_RESOURCE_BARRIER, 2> before{
            transition_barrier(source.resource.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            transition_barrier(cinema_flat_color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)};
        slot.command_list->ResourceBarrier(2, before.data());
        ID3D12DescriptorHeap* heaps[]{slot.cinema_source_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(fsr_cinema_root_signature.Get());
        slot.command_list->SetPipelineState(cinema_flatten_pipeline.Get());
        slot.command_list->SetGraphicsRootDescriptorTable(0, slot.cinema_source_heap->GetGPUDescriptorHandleForHeapStart());
        slot.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for (UINT eye = 0; eye < views.size(); ++eye) {
            const UINT slice = resolved_array_slice(views[eye], eye, views.size(), image_description.DepthOrArraySize);
            const auto& guide = *source.motion_vectors->eyes[slice];
            const D3D12ImageRect rect{guide.output_x, guide.output_y, guide.output_width, guide.output_height};
            const auto rtv = offset_cpu_handle(cinema_flat_rtv_heap->GetCPUDescriptorHandleForHeapStart(), slice, rtv_increment);
            const float black[]{0, 0, 0, 1};
            slot.command_list->ClearRenderTargetView(rtv, black, 0, nullptr);
            slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            set_viewport_and_scissor(slot.command_list.Get(), rect,
                static_cast<UINT>(image_description.Width), image_description.Height);
            FsrCinemaPanelParameters parameters{};
            parameters.clip_positions = source.presentation_transform.clip_positions[eye];
            parameters.source_slice = slice;
            slot.command_list->SetGraphicsRoot32BitConstants(1, kFsrCinemaPanelConstantCount, &parameters, 0);
            slot.command_list->DrawInstanced(3, 1, 0, 0);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> after{
            transition_barrier(source.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(cinema_flat_color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON)};
        slot.command_list->ResourceBarrier(2, after.data());
        return S_OK;
    }

    [[nodiscard]] HRESULT record_fsr_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        const auto& guides = current_source.motion_vectors;
        const bool cinema_projection =
            current_source.presentation_transform.active;
        const std::uint32_t fsr_domain = cinema_projection ? 1U : 0U;
        auto& fsr_frame_generator = fsr_frame_generators[fsr_domain];
        if (!fsr_frame_generator || target_views.empty() ||
            target_views.size() != current_source.view_count ||
            target_views.size() != previous_source.view_count) {
            return E_INVALIDARG;
        }
        if (!guides) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            if (has_complete_game_motion_set(previous_source)) {
                // The alternating-eye matcher deliberately returns no pair on
                // the odd publication between two complete Ghost stereo sets.
                // Pass this AFW frame through without discarding the last
                // complete source; the next even publication can pair with it.
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_recovery,
                    kD3D12FrameSynthesisStaleGuides,
                    5,
                    previous_source.motion_vectors->eyes[0]->serial,
                    current_source.ticket.serial);
                return kD3D12FrameSynthesisStaleGuides;
            }
            const auto previous_serial = previous_source.motion_vectors &&
                    previous_source.motion_vectors->eye_count != 0 &&
                    previous_source.motion_vectors->eyes[0]
                ? previous_source.motion_vectors->eyes[0]->serial
                : 0;
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_recovery,
                DXGI_ERROR_WAS_STILL_DRAWING,
                6,
                previous_serial,
                current_source.ticket.serial);
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        const UINT expected_guide_eyes = std::min<UINT>(
            std::max<UINT>(image_description.DepthOrArraySize,
                           current_source.view_count),
            kDlssMotionVectorEyeCount);
        if (unchanged_game_motion_pair(
                previous_source, current_source, expected_guide_eyes)) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_recovery,
                kD3D12FrameSynthesisStaleGuides,
                4,
                guides->eyes[0]->serial,
                current_source.ticket.serial);
            return kD3D12FrameSynthesisStaleGuides;
        }
        const std::uint64_t guide_failure =
            game_motion_pair_failure_code(
                previous_source, current_source, expected_guide_eyes);
        if (guide_failure != 0) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            const auto previous_serial = previous_source.motion_vectors &&
                    previous_source.motion_vectors->eye_count != 0 &&
                    previous_source.motion_vectors->eyes[0]
                ? previous_source.motion_vectors->eyes[0]->serial
                : 0;
            const auto current_serial = current_source.motion_vectors &&
                    current_source.motion_vectors->eye_count != 0 &&
                    current_source.motion_vectors->eyes[0]
                ? current_source.motion_vectors->eyes[0]->serial
                : 0;
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_recovery,
                DXGI_ERROR_WAS_STILL_DRAWING,
                100 + guide_failure,
                previous_serial,
                current_serial);
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        if (cinema_projection &&
            (current_source.presentation_transform.view_count !=
                 target_views.size() ||
             fsr_cinema_root_signature == nullptr ||
             fsr_cinema_panel_pipeline == nullptr)) {
            return E_INVALIDARG;
        }

        // The V1571 Witcher renderer snapshots this exact XR colour before
        // its late HUD draw. The normal VR image and snapshot share format,
        // extent, eye slices and transfer domain. Cinema requires its own
        // panel-space pullback and deliberately stays on the prior FSR path.
        slot.hudless_source.Reset();
        bool hudless_pair_ready = false;
        const auto display_time = current_source.views[0].display_time;
        if (!cinema_projection && display_time != 0) {
            if (auto* const get_hudless = witcher_hudless_color_export()) {
                ID3D12Resource* borrowed{};
                if (get_hudless(display_time, &borrowed) != 0 &&
                    borrowed != nullptr) {
                    slot.hudless_source.Attach(borrowed);
                    const auto hudless_description = borrowed->GetDesc();
                    hudless_pair_ready = same_device(device.Get(), borrowed) &&
                        hudless_description.Dimension ==
                            D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                        hudless_description.Width == image_description.Width &&
                        hudless_description.Height == image_description.Height &&
                        hudless_description.DepthOrArraySize ==
                            image_description.DepthOrArraySize &&
                        linear_color_view_format(
                            hudless_description.Format, DXGI_FORMAT_UNKNOWN) ==
                            linear_color_view_format(
                                image_description.Format, DXGI_FORMAT_UNKNOWN);
                }
            }
        }
        if (hudless_pair_ready) {
            for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
                if (current_source.views[view_index].display_time != display_time)
                    hudless_pair_ready = false;
            }
        }

        std::array<D3D12FsrEyeInput, kMaxReprojectionViews> inputs{};
        const bool witcher_cinema = cinema_projection &&
            witcher_hudless_color_export() != nullptr;
        if (witcher_cinema) {
            const HRESULT flatten_result = record_cinema_color(slot, current_source, target_views);
            if (FAILED(flatten_result)) return flatten_result;
        }
        std::array<D3D12ReprojectionView, kMaxReprojectionViews> midpoint_views{};
        const bool log_fsr_eyes = bridge_flight_logger().enabled() &&
            ((fsr_eye_diagnostic_pair++ % 120U) == 0U);
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            const UINT resource_slice = resolved_array_slice(
                target_views[view_index], view_index, target_views.size(),
                image_description.DepthOrArraySize);
            const UINT guide_eye = resolved_fsr_guide_eye(
                target_views[view_index], view_index, target_views.size(),
                image_description.DepthOrArraySize, guides->eye_count);
            if (resource_slice >= image_description.DepthOrArraySize ||
                guide_eye >= guides->eye_count || !guides->eyes[guide_eye]) {
                report_fsr_frame_generation_status(
                    FsrFrameGenerationStatus::waiting_for_guides);
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_recovery,
                    DXGI_ERROR_WAS_STILL_DRAWING,
                    201 + view_index,
                    resource_slice,
                    guides->eye_count);
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            const auto& guide = *guides->eyes[guide_eye];
            if (!guide.depth || !guide.motion_vectors ||
                !compatible_game_motion_resource(
                    guide, guide.depth.Get()) ||
                !compatible_game_motion_resource(
                    guide, guide.motion_vectors.Get())) {
                report_fsr_frame_generation_status(
                    FsrFrameGenerationStatus::waiting_for_guides);
                const std::uint64_t resource_failure = !guide.depth
                    ? 220
                    : !guide.motion_vectors
                        ? 221
                        : !compatible_game_motion_resource(
                              guide, guide.depth.Get())
                            ? 222
                            : 223;
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_recovery,
                    DXGI_ERROR_WAS_STILL_DRAWING,
                    resource_failure,
                    guide.serial,
                    view_index);
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            // FSR's colour/optical-flow/temporal input is scene only. The
            // final HUD-containing image is read solely by the later output
            // compositor, after FSR has finished its synthesis.
            ID3D12Resource* color_resource = hudless_pair_ready
                ? slot.hudless_source.Get() : current_source.resource.Get();
            UINT color_slice = resource_slice;
            D3D12ImageRect color_rect = resolved_rect(
                current_source.views[view_index].image_rect,
                static_cast<UINT>(image_description.Width),
                image_description.Height);
            if (witcher_cinema) {
                color_resource = cinema_flat_color.Get();
                color_slice = resource_slice;
                color_rect = {
                    guide.output_x, guide.output_y,
                    guide.output_width, guide.output_height};
            } else if (cinema_projection) {
                if (!guide.output ||
                    !compatible_game_motion_resource(
                        guide, guide.output.Get()) ||
                    guide.output_width == 0 || guide.output_height == 0) {
                    report_fsr_frame_generation_status(
                        FsrFrameGenerationStatus::waiting_for_guides);
                    return DXGI_ERROR_WAS_STILL_DRAWING;
                }
                const auto output_description = guide.output->GetDesc();
                if (output_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
                    guide.output_slice >= output_description.DepthOrArraySize ||
                    guide.output_x + guide.output_width > output_description.Width ||
                    guide.output_y + guide.output_height > output_description.Height) {
                    return E_INVALIDARG;
                }
                color_resource = guide.output.Get();
                color_slice = guide.output_slice;
                color_rect = {guide.output_x, guide.output_y,
                              guide.output_width, guide.output_height};
            }
            D3D12ImageRect depth_rect{
                guide.depth_x, guide.depth_y,
                guide.depth_width, guide.depth_height};
            D3D12ImageRect motion_rect{
                guide.motion_x, guide.motion_y,
                guide.motion_width, guide.motion_height};
            if (allow_luke_guide_device_alias && !cinema_projection) {
                // Luke's final AER eye is 3063x3344 while the pre-AER DLSS
                // depth and motion guides are square. FSR assumes render and
                // display rectangles describe the same aspect ratio. Crop the
                // guides around their optical center so the post-AER colour
                // and its guides share one projection domain.
                depth_rect = centered_aspect_rect(depth_rect, color_rect);
                motion_rect = centered_aspect_rect(motion_rect, color_rect);
            }
            inputs[view_index] = {
                color_resource,
                color_slice,
                resource_slice,
                color_rect,
                guide.depth.Get(),
                depth_rect,
                guide.depth_resource_state,
                guide.motion_vectors.Get(),
                motion_rect,
                guide.resource_state,
                guide.scale_x,
                guide.scale_y,
                guide.jitter_x,
                guide.jitter_y,
                guide.frame_time_delta_ms,
                guide.camera_near,
                guide.camera_far,
                guide.camera_data_valid
                    ? guide.camera_fov_vertical
                    : std::abs(current_source.views[view_index].fov.angle_up -
                               current_source.views[view_index].fov.angle_down),
                current_source.views[view_index].pose,
                {guide.camera_position[0], guide.camera_position[1],
                 guide.camera_position[2]},
                {guide.camera_up[0], guide.camera_up[1], guide.camera_up[2]},
                {guide.camera_right[0], guide.camera_right[1],
                 guide.camera_right[2]},
                {guide.camera_forward[0], guide.camera_forward[1],
                 guide.camera_forward[2]},
                guide.depth_inverted,
                guide.depth_infinite,
                guide.jittered,
                guide.reset,
                guide.camera_data_valid,
            };
            midpoint_views[view_index] = current_source.views[view_index];
            midpoint_views[view_index].image_rect = color_rect;
            midpoint_views[view_index].pose = interpolate_pose(
                previous_source.views[view_index].pose,
                current_source.views[view_index].pose, 0.5F);
            midpoint_views[view_index].fov = {
                std::lerp(previous_source.views[view_index].fov.angle_left,
                          current_source.views[view_index].fov.angle_left, 0.5F),
                std::lerp(previous_source.views[view_index].fov.angle_right,
                          current_source.views[view_index].fov.angle_right, 0.5F),
                std::lerp(previous_source.views[view_index].fov.angle_up,
                          current_source.views[view_index].fov.angle_up, 0.5F),
                std::lerp(previous_source.views[view_index].fov.angle_down,
                          current_source.views[view_index].fov.angle_down, 0.5F),
            };

            if (log_fsr_eyes) {
                const auto color_description = color_resource->GetDesc();
                const auto depth_description = guide.depth->GetDesc();
                const auto motion_description = guide.motion_vectors->GetDesc();
                const std::uint64_t slices_and_arrays =
                    (static_cast<std::uint64_t>(resource_slice) & 0xffULL) |
                    ((static_cast<std::uint64_t>(color_slice) & 0xffULL) << 8U) |
                    ((static_cast<std::uint64_t>(inputs[view_index].output_slice) & 0xffULL) << 16U) |
                    ((static_cast<std::uint64_t>(guide.motion_slice) & 0xffULL) << 24U) |
                    ((static_cast<std::uint64_t>(color_description.DepthOrArraySize) & 0xffULL) << 32U) |
                    ((static_cast<std::uint64_t>(depth_description.DepthOrArraySize) & 0xffULL) << 40U) |
                    ((static_cast<std::uint64_t>(motion_description.DepthOrArraySize) & 0xffULL) << 48U) |
                    (static_cast<std::uint64_t>(guide.reset ? 1U : 0U) << 56U);
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_binding,
                    static_cast<std::int64_t>(view_index),
                    guide.stream,
                    guide.serial,
                    slices_and_arrays);
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_resources,
                    static_cast<std::int64_t>(view_index),
                    reinterpret_cast<std::uintptr_t>(color_resource),
                    reinterpret_cast<std::uintptr_t>(guide.depth.Get()),
                    reinterpret_cast<std::uintptr_t>(
                        guide.motion_vectors.Get()));
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_rects,
                    static_cast<std::int64_t>(view_index),
                    pack_rect_16(inputs[view_index].color_rect),
                    pack_rect_16(inputs[view_index].depth_rect),
                    pack_rect_16(inputs[view_index].motion_rect));
                const std::uint32_t motion_flags =
                    (guide.jittered ? 1U : 0U) |
                    (guide.depth_inverted ? 2U : 0U) |
                    (guide.depth_infinite ? 4U : 0U) |
                    (guide.camera_data_valid ? 8U : 0U) |
                    (guide.camera_matrices_valid ? 16U : 0U);
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_motion,
                    static_cast<std::int64_t>(view_index),
                    pack_float_pair(guide.scale_x, guide.scale_y),
                    pack_float_pair(guide.jitter_x, guide.jitter_y),
                    pack_u32_pair(
                        std::bit_cast<std::uint32_t>(guide.frame_time_delta_ms),
                        motion_flags));
                const Pose& pose = inputs[view_index].camera_pose;
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_pose,
                    static_cast<std::int64_t>(view_index),
                    pack_float_pair(pose.position.x, pose.position.y),
                    pack_float_pair(pose.position.z,
                                    inputs[view_index].camera_fov_vertical),
                    pack_float_pair(guide.camera_near, guide.camera_far));
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_orientation,
                    static_cast<std::int64_t>(view_index),
                    pack_float_pair(pose.orientation.x, pose.orientation.y),
                    pack_float_pair(pose.orientation.z, pose.orientation.w),
                    guide.publication);
                const D3D12FieldOfView& fov =
                    current_source.views[view_index].fov;
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_eye_fov,
                    static_cast<std::int64_t>(view_index),
                    pack_float_pair(fov.angle_left, fov.angle_right),
                    pack_float_pair(fov.angle_up, fov.angle_down),
                    pack_u32_pair(
                        static_cast<std::uint32_t>(color_description.Format),
                        static_cast<std::uint32_t>(motion_description.Format)));
                const auto& clip = current_source.presentation_transform
                                       .clip_positions[view_index];
                for (std::uint32_t vertex = 0; vertex < 4U; ++vertex) {
                    const std::size_t offset = vertex * 4U;
                    bridge_flight_logger().event(
                        BridgeFlightOperation::fsr_eye_clip_vertex,
                        static_cast<std::int64_t>(view_index),
                        pack_float_pair(clip[offset], clip[offset + 1U]),
                        pack_float_pair(clip[offset + 2U], clip[offset + 3U]),
                        vertex);
                }
            }
        }

        if (!hudless_pair_ready) {
            slot.hudless_source.Reset();
        }
        log_post_hud_admission(
            hudless_pair_ready, display_time,
            slot.hudless_source ? slot.hudless_source->GetDesc().Format
                                : DXGI_FORMAT_UNKNOWN,
            image_description.Format);
        if (bridge_flight_logger().enabled() &&
            ((fsr_hudless_diagnostic_pair++ % 120U) == 0U)) {
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_hudless_pair,
                hudless_pair_ready ? 1 : 0,
                static_cast<std::uint64_t>(display_time),
                slot.hudless_source
                    ? static_cast<std::uint64_t>(
                          slot.hudless_source->GetDesc().Format)
                    : 0,
                static_cast<std::uint64_t>(image_description.Format));
        }

        // FidelityFX contexts own GPU resources shared by every OFXR work
        // slot. Recreating one while a different slot still references the
        // old context is unsafe even when the current slot itself is free.
        // Wait only for the last submission that actually used those FSR
        // contexts. Prime/pass-through copies use the common bridge fence too,
        // but do not reference the contexts and must not keep moving the reset
        // target forward forever.
        if (fsr_frame_generator->requires_idle_reconfigure(
                std::span<const D3D12FsrEyeInput>(
                    inputs.data(), target_views.size()))) {
            // Once a change within this domain is observed, keep it latched
            // until every older submission using this domain has retired.
            fsr_context_reset_pending[fsr_domain] = true;
        }
        if (fsr_context_reset_pending[fsr_domain]) {
            const std::uint64_t completed = fence->GetCompletedValue();
            const std::uint64_t context_fence =
                last_fsr_context_fence_values[fsr_domain];
            if (completed < context_fence) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_recovery,
                    DXGI_ERROR_WAS_STILL_DRAWING,
                    1,
                    completed,
                    context_fence);
                return DXGI_ERROR_WAS_STILL_DRAWING;
            }
            fsr_frame_generator->reset();
            fsr_context_reset_pending[fsr_domain] = false;
            last_fsr_context_fence_values[fsr_domain] = 0;
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_recovery,
                S_OK,
                2,
                completed,
                context_fence);
        }

        if (slot.synthesis_command_list) {
            const HRESULT close_result = slot.synthesis_command_list->Close();
            if (FAILED(close_result)) return close_result;
        }

        ID3D12Resource* generated = nullptr;
        HRESULT result = fsr_frame_generator->dispatch(
            slot.command_list.Get(),
            std::span<const D3D12FsrEyeInput>(inputs.data(), target_views.size()),
            &generated);
        if (FAILED(result) || !generated) return FAILED(result) ? result : E_FAIL;
        active_fsr_domain = fsr_domain;
        recording_uses_fsr_context = true;

        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, generated, current_resource,
            cinema_projection
                ? linear_color_view_format(generated->GetDesc().Format, view_format)
                : view_format);
        if (hudless_pair_ready) {
            D3D12_SHADER_RESOURCE_VIEW_DESC scene_view{};
            scene_view.Format = view_format;
            scene_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            scene_view.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            scene_view.Texture2DArray.MipLevels = 1;
            scene_view.Texture2DArray.ArraySize = image_description.DepthOrArraySize;
            const auto cpu_start =
                slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT slice = 0; slice < image_description.DepthOrArraySize; ++slice) {
                device->CreateShaderResourceView(
                    slot.hudless_source.Get(), &scene_view,
                    offset_cpu_handle(cpu_start,
                        slice * kDescriptorBlockSize + 7U,
                        descriptor_increment));
            }
        }
        const auto current_to_read = transition_barrier(
            current_resource, D3D12_RESOURCE_STATE_COMMON, kShaderReadState);
        slot.command_list->ResourceBarrier(1, &current_to_read);
        if (hudless_pair_ready) {
            const auto scene_to_read = transition_barrier(
                slot.hudless_source.Get(), D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState);
            slot.command_list->ResourceBarrier(1, &scene_to_read);
        }

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(
            cinema_projection
                ? fsr_cinema_root_signature.Get()
                : root_signature.Get());
        slot.command_list->SetPipelineState(
            cinema_projection
                ? fsr_cinema_panel_pipeline.Get()
                : hudless_pair_ready
                    ? fsr_post_hud_pipeline.Get()
                    : fsr_reprojection_pipeline.Get());
        slot.command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        const auto rtv_start = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const UINT first_rtv = synthetic_destination_index *
            image_description.DepthOrArraySize;

        synthetic_target_state(slot.command_list.Get(), synthetic_destination_index, true);
        SynthesisParameters parameters{};
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        if (synthetic_camera_requested) {
            bool reset_output=false;
            for (UINT view=0;view<current_source.view_count;++view) reset_output |= fsr_frame_generator->output_is_current(view);
            if (reset_output) {
                // SDK reset writes B: keep its real camera, never publish C metadata.
                target_views=std::span<const D3D12ReprojectionView>{current_source.views.data(),current_source.view_count};
                for (UINT view=0;view<current_source.view_count;++view) {
                    midpoint_views[view].pose=current_source.views[view].pose;
                    midpoint_views[view].fov=current_source.views[view].fov;
                }
                synthetic_camera_turned={};
                fsr_pair_in_target_camera=false;
            }
        }
        const auto generated_description = generated->GetDesc();
        parameters.packed_width =
            static_cast<UINT>(generated_description.Width);
        parameters.packed_height = generated_description.Height;
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                midpoint_views[view_index], target_views[view_index],
                parameters.packed_width, parameters.packed_height,
                parameters.width, parameters.height);
        }
        for (UINT view_index = 0; view_index < target_views.size(); ++view_index) {
            const UINT slice = resolved_array_slice(
                target_views[view_index], view_index, target_views.size(),
                image_description.DepthOrArraySize);
            const UINT descriptor_base = slice * kDescriptorBlockSize;
            slot.command_list->SetGraphicsRootDescriptorTable(
                0, offset_gpu_handle(gpu_start, descriptor_base,
                                     descriptor_increment));
            if (cinema_projection) {
                FsrCinemaPanelParameters cinema_parameters{};
                cinema_parameters.clip_positions =
                    current_source.presentation_transform
                        .clip_positions[view_index];
                if (synthetic_camera_turned[0]) {
                    const auto q=synthetic_camera_rotations[0][view_index];
                    const Quaternion inverse{-q[0],-q[1],-q[2],q[3]};
                    const auto tangents=fov_tangents(target_views[view_index].fov);
                    for (unsigned corner=0;corner<4;++corner) {
                        auto* clip=cinema_parameters.clip_positions.data()+corner*4;
                        const float w=clip[3];
                        const Quaternion ray{std::lerp(tangents[0],tangents[1],(clip[0]/w+1)*0.5F),
                            std::lerp(tangents[2],tangents[3],(1-clip[1]/w)*0.5F),-1,0};
                        const auto turned=multiply(multiply(inverse,ray),Quaternion{q[0],q[1],q[2],q[3]});
                        const float depth=-turned.z*w;
                        const std::array<float,4> result{ (2*(turned.x/-turned.z-tangents[0])/(tangents[1]-tangents[0])-1)*depth,
                            (1-2*(turned.y/-turned.z-tangents[2])/(tangents[3]-tangents[2]))*depth,
                            clip[2]/w*depth,depth};
                        std::copy(result.begin(),result.end(),clip);
                    }
                }
                cinema_parameters.source_rect = {
                    static_cast<float>(inputs[view_index].color_rect.offset_x),
                    static_cast<float>(inputs[view_index].color_rect.offset_y),
                    static_cast<float>(inputs[view_index].color_rect.width),
                    static_cast<float>(inputs[view_index].color_rect.height)};
                cinema_parameters.source_width =
                    static_cast<UINT>(generated_description.Width);
                cinema_parameters.source_height = generated_description.Height;
                cinema_parameters.source_slice = inputs[view_index].output_slice;
                cinema_parameters.padding = witcher_cinema ? 1U : 0U;
                slot.command_list->SetGraphicsRoot32BitConstants(
                    1, kFsrCinemaPanelConstantCount,
                    &cinema_parameters, 0);
            } else {
                slot.command_list->SetGraphicsRootDescriptorTable(
                    1, offset_gpu_handle(gpu_start,
                        descriptor_base + kSrvDescriptorCount,
                        descriptor_increment));
                parameters.slice = slice;
                parameters.view_index = view_index;
                slot.command_list->SetGraphicsRoot32BitConstants(
                    2, kSynthesisConstantCount, &parameters, 0);
            }
            const auto rtv = offset_cpu_handle(
                rtv_start, first_rtv + slice, rtv_increment);
            slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            if (cinema_projection) {
                const float black[4]{0.0F, 0.0F, 0.0F, 1.0F};
                slot.command_list->ClearRenderTargetView(
                    rtv, black, 0, nullptr);
                set_viewport_and_scissor(
                    slot.command_list.Get(), {},
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(6, 1, 0, 0);
            } else {
                set_viewport_and_scissor(
                    slot.command_list.Get(), target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
            }
            clear_synthetic_marker(
                slot.command_list.Get(), rtv, target_views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height, debug_marker);
        }

        synthetic_target_state(slot.command_list.Get(), synthetic_destination_index, false);
        if (hudless_pair_ready) {
            const auto scene_to_common = transition_barrier(
                slot.hudless_source.Get(), kShaderReadState,
                D3D12_RESOURCE_STATE_COMMON);
            slot.command_list->ResourceBarrier(1, &scene_to_common);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
            transition_barrier(current_resource, kShaderReadState,
                               D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(current_destination, release_state,
                               D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_copy.size()), before_copy.data());
        slot.command_list->CopyResource(current_destination, current_resource);
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition_barrier(current_resource,
                               D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(current_destination,
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(after_copy.size()), after_copy.data());
        return S_OK;
    }

    [[nodiscard]] HRESULT reset_work_slot(WorkSlot& slot) noexcept {
        // acquire_work_slot already established completion before reuse.
        slot.guide_composer_inputs.reset();
        slot.ram_gpu.harvest();
        slot.ram_pair=false;
        recording_uses_fsr_context = false;
        HRESULT result = slot.allocator->Reset();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.command_list->Reset(slot.allocator.Get(), nullptr);
        if (SUCCEEDED(result) && nvidia_gpu_timing_enabled &&
            nvidia_timestamp_heap) {
            // First mark on the list both backends record into, so the span
            // starts where the GPU starts this pair's work.
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kSpanBegin);
        }
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_allocator->Reset();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Reset(
            slot.current_copy_allocator.Get(),
            nullptr);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        if (slot.synthesis_command_list) {
            result = slot.synthesis_allocator->Reset();
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
            result = slot.synthesis_command_list->Reset(
                slot.synthesis_allocator.Get(),
                nullptr);
            if (FAILED(result)) {
                synthesis_enabled = false;
            }
        }
        return result;
    }

    [[nodiscard]] HRESULT record_prime(
        WorkSlot& slot,
        const RollingSource& source,
        ID3D12Resource* destination) noexcept {
        if (backend == D3D12OpticalFlowBackend::nvidia ||
            generation_mode != D3D12FrameGenerationMode::ofxr) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
                transition_barrier(
                    source.resource.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(
                    destination,
                    release_state,
                    D3D12_RESOURCE_STATE_COPY_DEST),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(before_copy.size()),
                before_copy.data());
            slot.command_list->CopyResource(destination, source.resource.Get());
            const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
                transition_barrier(
                    source.resource.Get(),
                    D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_COMMON),
                transition_barrier(
                    destination,
                    D3D12_RESOURCE_STATE_COPY_DEST,
                    release_state),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(after_copy.size()),
                after_copy.data());
            return slot.synthesis_command_list ? slot.synthesis_command_list->Close() : S_OK;
        }

        create_source_views(
            slot,
            source.resource.Get(),
            source.resource.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
            transition_barrier(
                source.resource.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
            transition_barrier(
                destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_copy.size()),
            before_copy.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetComputeRootDescriptorTable(0, gpu_start);
        slot.command_list->SetComputeRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(nvidia_options.hybrid ? hybrid_pack_pipeline.Get() : pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            flow_input_flags(view_format, nvidia_options.input_encoding) |
            (backend == D3D12OpticalFlowBackend::nvidia && nvidia_options.pose_compensated_input ? 4U : 0U);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        for (UINT view_index = 0; view_index < source.view_count;
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                source.views[view_index],
                source.views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        slot.command_list->SetComputeRoot32BitConstants(
            2,
            kSynthesisConstantCount,
            &parameters,
            0);
        slot.command_list->Dispatch(
            (packed_width + 7U) / 8U,
            (packed_height + 7U) / 8U,
            1);

        const D3D12_RESOURCE_BARRIER before_fidelityfx = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot.command_list->ResourceBarrier(1, &before_fidelityfx);

        // FidelityFX suppresses optical flow through frame index five. Seed
        // its temporal history with the prime frame so the first real A/B pair
        // reaches the first usable B dispatch without presenting warm-up flow.
        for (UINT seed_index = 0; seed_index < 6U; ++seed_index) {
            const HRESULT seed_result = dispatch_fidelityfx(
                slot.command_list.Get(),
                FFX_RESOURCE_STATE_UNORDERED_ACCESS);
            if (FAILED(seed_result)) {
                return seed_result;
            }
        }
        const D3D12_RESOURCE_BARRIER before_source_copy = transition_barrier(
            source.resource.Get(),
            kShaderReadState,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        slot.command_list->ResourceBarrier(1, &before_source_copy);
        slot.command_list->CopyResource(destination, source.resource.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition_barrier(
                source.resource.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(after_copy.size()),
            after_copy.data());
        return S_OK;
    }

    [[nodiscard]] HRESULT dispatch_fidelityfx(
        ID3D12GraphicsCommandList* command_list,
        FfxResourceStates output_state) noexcept {
        FfxOpticalflowDispatchDescription dispatch{};
        dispatch.commandList = ffxGetCommandListDX12(command_list);
        dispatch.color = ffxGetResourceDX12(
            packed_color.Get(),
            ffxGetResourceDescriptionDX12(packed_color.Get()),
            L"XRFG packed color",
            FFX_RESOURCE_STATE_COMPUTE_READ);
        dispatch.opticalFlowVector = ffxGetResourceDX12(
            optical_flow_vector.Get(),
            ffxGetResourceDescriptionDX12(optical_flow_vector.Get()),
            L"XRFG FidelityFX optical flow",
            output_state);
        dispatch.opticalFlowSCD = ffxGetResourceDX12(
            optical_flow_scene_change.Get(),
            ffxGetResourceDescriptionDX12(optical_flow_scene_change.Get()),
            L"XRFG FidelityFX scene change",
            output_state);
        dispatch.reset = false;
        dispatch.backbufferTransferFunction =
            FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        dispatch.minMaxLuminance = {0.0F, 1.0F};
        return ffx_result(ffxOpticalflowContextDispatch(
            &ffx_context,
            &dispatch));
    }

    [[nodiscard]] HRESULT record_nvidia_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        if (slot.synthesis_command_list == nullptr) {
            return E_UNEXPECTED;
        }
        dred_marker(slot.command_list.Get(), "OFXR NVIDIA pack begin");
        slot.ram_pair=true;
        slot.ram_source_identity=reinterpret_cast<UINT64>(current_source.producer_fence.Get());
        slot.ram_source_serial=current_source.ticket.serial;
        slot.ram_gpu.mark(slot.command_list.Get(),0);
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, previous_resource, current_resource);
        if (nvidia_gpu_timing_enabled) {
            slot.timing_metadata = {};
            slot.timing_metadata.previous_serial = previous_source.ticket.serial;
            slot.timing_metadata.current_serial = current_source.ticket.serial;
            slot.timing_metadata.eye_count = image_description.DepthOrArraySize;
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampPackBegin);
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 2U * kMaxReprojectionViews> before_pack{};
        UINT before_pack_count = 0;
        before_pack[before_pack_count++] = transition_barrier(
            previous_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        before_pack[before_pack_count++] = transition_barrier(
            current_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_pack[before_pack_count++] = transition_barrier(
                eye.previous_input.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            before_pack[before_pack_count++] = transition_barrier(
                eye.current_input.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        slot.command_list->ResourceBarrier(before_pack_count, before_pack.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetPipelineState(nvidia_input_format == DXGI_FORMAT_R8_UNORM
            ? nvidia_luma_pack_pipeline.Get() : nvidia_pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            flow_input_flags(view_format, nvidia_options.input_encoding) |
            (backend == D3D12OpticalFlowBackend::nvidia && nvidia_options.pose_compensated_input ? 4U : 0U);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        // The pack warps A into B's own camera; the synthetic's is applied
        // per composition draw.
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index],
                current_source.views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        const auto view_mappings = parameters.previous_mappings;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            parameters.slice = eye_index;
            const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                gpu_start,
                eye_index * kDescriptorBlockSize,
                descriptor_increment);
            slot.command_list->SetComputeRootDescriptorTable(0, block);
            slot.command_list->SetComputeRootDescriptorTable(
                1,
                offset_gpu_handle(
                    block,
                    kSrvDescriptorCount,
                    descriptor_increment));
            slot.command_list->SetComputeRoot32BitConstants(
                2,
                kSynthesisConstantCount,
                &parameters,
                0);
            slot.command_list->Dispatch(
                (packed_width + 7U) / 8U,
                (packed_height + 7U) / 8U,
                1);
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   2U * kMaxReprojectionViews> uav_barriers{};
        UINT uav_barrier_count = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            uav_barriers[uav_barrier_count++] =
                uav_barrier(eye.previous_input.Get());
            uav_barriers[uav_barrier_count++] =
                uav_barrier(eye.current_input.Get());
        }
        slot.command_list->ResourceBarrier(
            uav_barrier_count,
            uav_barriers.data());

        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 2U * kMaxReprojectionViews> after_pack{};
        UINT after_pack_count = 0;
        after_pack[after_pack_count++] = transition_barrier(
            previous_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        after_pack[after_pack_count++] = transition_barrier(
            current_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            after_pack[after_pack_count++] = transition_barrier(
                eye.previous_input.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
            after_pack[after_pack_count++] = transition_barrier(
                eye.current_input.Get(),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
        }
        slot.command_list->ResourceBarrier(after_pack_count, after_pack.data());
        dred_marker(slot.command_list.Get(), "OFXR NVIDIA pack complete");
        slot.ram_gpu.mark(slot.command_list.Get(),1);
        if (nvidia_gpu_timing_enabled) {
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampPackEnd);
        }

        ID3D12GraphicsCommandList* const synthesis =
            slot.synthesis_command_list.Get();
        dred_marker(synthesis, "OFXR NVIDIA composition begin");
        slot.ram_gpu.mark(synthesis,2);
        if (nvidia_gpu_timing_enabled) {
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base +
                    kNvidiaTimestampCompositionBegin);
        }
        std::array<D3D12_RESOURCE_BARRIER,
                   2U + 4U * kMaxReprojectionViews> before_synthesis{};
        UINT before_synthesis_count = 0;
        before_synthesis[before_synthesis_count++] = transition_barrier(
            previous_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        before_synthesis[before_synthesis_count++] = transition_barrier(
            current_resource,
            D3D12_RESOURCE_STATE_COMMON,
            kShaderReadState);
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_synthesis[before_synthesis_count++] = transition_barrier(
                eye.flow.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            before_synthesis[before_synthesis_count++] = transition_barrier(
                eye.cost.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (nvidia_options.bidirectional) {
                before_synthesis[before_synthesis_count++] = transition_barrier(
                    eye.backward_flow.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                before_synthesis[before_synthesis_count++] = transition_barrier(
                    eye.backward_cost.Get(),
                    D3D12_RESOURCE_STATE_COMMON,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
        }
        synthesis->ResourceBarrier(
            before_synthesis_count,
            before_synthesis.data());

        synthesis->SetDescriptorHeaps(1, heaps);
        synthesis->SetGraphicsRootSignature(root_signature.Get());
        synthesis->SetPipelineState(
            nvidia_options.bidirectional
                ? nvidia_bidirectional_graphics_pipeline.Get()
                : nvidia_graphics_pipeline.Get());
        dred_marker(synthesis, "OFXR NVIDIA composition draw");
        synthesis->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            const UINT output_destination = synthetic_output_destination(output, synthetic_destination_index);
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv = output_destination *
                image_description.DepthOrArraySize;
            synthetic_target_state(synthesis, output_destination, true);
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                parameters.previous_mappings = view_mappings;
                apply_synthetic_camera(parameters, output, view_index);
                const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                    gpu_start,
                    slice * kDescriptorBlockSize,
                    descriptor_increment);
                synthesis->SetGraphicsRootDescriptorTable(0, block);
                synthesis->SetGraphicsRootDescriptorTable(
                    1,
                    offset_gpu_handle(
                        block,
                        kSrvDescriptorCount,
                        descriptor_increment));
                synthesis->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                synthesis->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    synthesis,
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                synthesis->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(synthesis, rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
            synthetic_target_state(synthesis, output_destination, false);
        }

        std::array<D3D12_RESOURCE_BARRIER,
                   3U + 4U * kMaxReprojectionViews> before_current_copy{};
        UINT before_current_copy_count = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            before_current_copy[before_current_copy_count++] =
                transition_barrier(
                    eye.flow.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            before_current_copy[before_current_copy_count++] =
                transition_barrier(
                    eye.cost.Get(),
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COMMON);
            if (nvidia_options.bidirectional) {
                before_current_copy[before_current_copy_count++] =
                    transition_barrier(
                        eye.backward_flow.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
                before_current_copy[before_current_copy_count++] =
                    transition_barrier(
                        eye.backward_cost.Get(),
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_STATE_COMMON);
            }
        }
        before_current_copy[before_current_copy_count++] = transition_barrier(
            previous_resource,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        synthesis->ResourceBarrier(
            before_current_copy_count,
            before_current_copy.data());
        if (nvidia_gpu_timing_enabled && nvidia_timestamp_heap) {
            // Last mark of the pair's synthesis, before the current copy
            // that is submitted separately. The span therefore covers
            // exactly the work the synthetic frame is waiting on.
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kSpanEnd);
        }
        // The copy's own transitions travel with the copy, so the resources
        // are left in their normal states while it waits to be submitted.
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy_states{
            transition_barrier(
                current_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(before_copy_states.size()),
            before_copy_states.data());
        // Recorded here but submitted only after the synthetic frame has gone
        // to the runtime; see flush_current_copy.
        dred_marker(slot.current_copy_command_list.Get(), "OFXR NVIDIA current copy");
        slot.current_copy_command_list->CopyResource(
            current_destination,
            current_resource);
        // DIAGNOSTIC. The real frame is otherwise indistinguishable from one
        // that never arrived: the purple marker rides only on the synthetic,
        // so a steady purple square means either "only synthetics are shown"
        // or "both are shown and 45 Hz reads as steady". Green, offset to the
        // right of the purple one, separates those two readings in one look.
        if (debug_marker) {
            constexpr float green[4]{0.0F, 0.9F, 0.2F, 1.0F};
            const auto marker_to_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_to_rt);
            const D3D12_CPU_DESCRIPTOR_HANDLE marker_rtv_start =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT marker_first_rtv = current_rtv_base +
                current_destination_index * image_description.DepthOrArraySize;
            for (UINT marker_view = 0;
                 marker_view < static_cast<UINT>(target_views.size());
                 ++marker_view) {
                const UINT marker_slice = resolved_array_slice(
                    target_views[marker_view],
                    marker_view,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                clear_synthetic_marker(
                    slot.current_copy_command_list.Get(),
                    offset_cpu_handle(
                        marker_rtv_start,
                        marker_first_rtv + marker_slice,
                        rtv_increment),
                    target_views[marker_view],
                    static_cast<UINT>(image_description.Width),
                    image_description.Height,
                    debug_marker,
                    green,
                    1.6F);
            }
            const auto marker_from_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_from_rt);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_current_copy{
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(after_current_copy.size()),
            after_current_copy.data());
        if (nvidia_gpu_timing_enabled) {
            synthesis->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampCompositionEnd);
            synthesis->ResolveQueryData(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base,
                kTimestampCount,
                nvidia_timestamp_readback.Get(),
                static_cast<UINT64>(slot.timing_query_base) *
                    sizeof(std::uint64_t));
        }
        dred_marker(synthesis, "OFXR NVIDIA composition complete");
        slot.ram_gpu.mark(synthesis,3);
        slot.ram_gpu.mark(slot.current_copy_command_list.Get(),4);
        slot.ram_gpu.resolve(slot.current_copy_command_list.Get());
        return S_OK;
    }

    [[nodiscard]] HRESULT record_pair(
        WorkSlot& slot,
        const RollingSource& previous_source,
        const RollingSource& current_source,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker,
        bool* used_game_motion) noexcept {
        if (used_game_motion == nullptr) return E_POINTER;
        *used_game_motion = false;
        if (native_dlss) {
            native_pair_generated = false;

            const auto current_views = std::span(current_source.views.data(), current_source.view_count);
            const bool full_coverage = views_cover_resource(current_views,
                static_cast<UINT>(image_description.Width), image_description.Height,
                image_description.DepthOrArraySize);
            // Cropped outputs need B outside the submitted rectangles. Fully
            // covered outputs are written by composition and need no pre-copy.
            auto* b = current_source.resource.Get();
            const auto copy = [&](ID3D12GraphicsCommandList* list, ID3D12Resource* destination) {
                const std::array<D3D12_RESOURCE_BARRIER, 2> before{
                    transition_barrier(b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                    transition_barrier(destination, release_state, D3D12_RESOURCE_STATE_COPY_DEST)};
                list->ResourceBarrier(2, before.data());
                list->CopyResource(destination, b);
                const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                    transition_barrier(b, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
                    transition_barrier(destination, D3D12_RESOURCE_STATE_COPY_DEST, release_state)};
                list->ResourceBarrier(2, after.data());
            };
            std::array<D3D12NativeDlssG::Output, 2> outputs{};
            for (UINT output=0; output<synthetic_output_count(); ++output) {
                outputs[output].image = synthetic_destinations[
                    synthetic_output_destination(output, synthetic_destination_index)].resource.Get();
                if (synthetic_camera_turned[output]) {
                    outputs[output].camera_to_current = synthetic_camera_rotations[output];
                }
                if (!full_coverage) copy(slot.command_list.Get(), outputs[output].image);
            }
            const UINT work_slot = static_cast<UINT>(&slot - work_slots.data());
            const HRESULT generated = native_dlss->record(slot.command_list.Get(), work_slot,
                previous_source.resource.Get(), b,
                {previous_source.views.data(), previous_source.view_count},
                {current_source.views.data(), current_source.view_count},
                previous_source.motion_vectors.get(), current_source.motion_vectors.get(),
                {outputs.data(), synthetic_output_count()}, release_state);
            if (FAILED(generated)) return generated;
            native_pair_generated = generated == S_OK;
            if (!native_pair_generated) {
                // A skipped pair never writes the outputs; show the current frame.
                if (full_coverage) for (UINT output = 0; output < synthetic_output_count(); ++output)
                    copy(slot.command_list.Get(), outputs[output].image);
                switch (native_dlss->last_skip()) {
                case D3D12NativeDlssG::Skip::resizing:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::native_resizing);
                    break;
                case D3D12NativeDlssG::Skip::multi_frame_unsupported:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::multi_frame_unsupported);
                    break;
                case D3D12NativeDlssG::Skip::feature_unavailable:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::native_unavailable);
                    break;
                case D3D12NativeDlssG::Skip::evaluate_failed:
                    report_dlss_motion_vector_status(DlssMotionVectorStatus::native_evaluate_failed);
                    break;
                default: {
                    bool missing_depth = false;
                    for (const auto* guides : {previous_source.motion_vectors.get(), current_source.motion_vectors.get()}) {
                        if (!guides) continue;
                        for (UINT eye=0; eye<std::min(guides->eye_count,kDlssMotionVectorEyeCount); ++eye)
                            missing_depth |= guides->eyes[eye] && !guides->eyes[eye]->depth;
                    }
                    report_dlss_motion_vector_status(missing_depth ? DlssMotionVectorStatus::waiting_for_depth :
                        current_source.motion_vectors ? DlssMotionVectorStatus::invalid_input :
                            DlssMotionVectorStatus::waiting_for_dlss);
                    break;
                }
                }
            }
            // A skipped pair generated nothing, so it reports no time.

            // This list is submitted after the synthetic when requested by the
            // presenter. Keeping the real copy out of the synthesis span also
            // makes its timing and completion fence reflect the actual order.
            copy(slot.current_copy_command_list.Get(), current_destinations[current_destination_index].resource.Get());
            if (slot.synthesis_command_list) {
                const HRESULT closed=slot.synthesis_command_list->Close();
                if (FAILED(closed)) return closed;
            }
            *used_game_motion = true; // execute the direct list, bypass OFA
            return S_OK;
        }
        if (generation_mode == D3D12FrameGenerationMode::fsr3) {
            const HRESULT result = record_fsr_pair(
                slot, previous_source, current_source, target_views,
                synthetic_destination_index, current_destination_index,
                debug_marker);
            if (SUCCEEDED(result)) *used_game_motion = true;
            return result;
        }
        // The hybrid runs FidelityFX's flow below and composes with the
        // game's vectors as well.
        const bool hybrid = nvidia_options.hybrid &&
            backend == D3D12OpticalFlowBackend::fidelity_fx &&
            valid_game_motion_pair(previous_source, current_source, std::min<UINT>(std::max<UINT>(image_description.DepthOrArraySize, current_source.view_count), kDlssMotionVectorEyeCount)) &&
            current_source.motion_vectors &&
            current_source.motion_vectors->eye_count == target_views.size();
        if (!hybrid && valid_game_motion_pair(previous_source, current_source, std::min<UINT>(std::max<UINT>(image_description.DepthOrArraySize, current_source.view_count), kDlssMotionVectorEyeCount))) {
            const HRESULT result = record_game_motion_pair(slot, previous_source,
                current_source, target_views, synthetic_destination_index,
                current_destination_index, debug_marker);
            if (SUCCEEDED(result)) {
                *used_game_motion = true;
                return result;
            }
            report_dlss_motion_vector_status(DlssMotionVectorStatus::invalid_input);
        } else if (!hybrid && current_source.motion_vectors) {
            report_dlss_motion_vector_status(DlssMotionVectorStatus::temporal_mismatch);
        }
        if (backend == D3D12OpticalFlowBackend::nvidia || native_dlss) {
            return record_nvidia_pair(
                slot,
                previous_source,
                current_source,
                target_views,
                synthetic_destination_index,
                current_destination_index,
                debug_marker);
        }
        ID3D12Resource* const previous_resource = previous_source.resource.Get();
        ID3D12Resource* const current_resource = current_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        const DlssMotionVectorSet* const guides = hybrid ? current_source.motion_vectors.get() : nullptr;
        create_source_views(slot, previous_resource, current_resource, DXGI_FORMAT_UNKNOWN, guides ? guides->eye_count : 0);
        if (guides) {
            const HRESULT views = create_game_motion_views(slot, *guides,
                static_cast<UINT>(target_views.size()));
            if (FAILED(views)) return views;
            // FidelityFX's flow views exist in the first block only; each
            // eye's block needs them for the hybrid composition.
            D3D12_SHADER_RESOURCE_VIEW_DESC flow_view{};
            flow_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            flow_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            flow_view.Texture2D.MipLevels = 1;
            const D3D12_CPU_DESCRIPTOR_HANDLE cpu_start =
                slot.descriptor_heap->GetCPUDescriptorHandleForHeapStart();
            for (UINT block = 1; block < guides->eye_count; ++block) {
                const UINT base = block * kDescriptorBlockSize;
                flow_view.Format = DXGI_FORMAT_R16G16_SINT;
                for (const UINT offset : {2U, 4U}) {
                    device->CreateShaderResourceView(optical_flow_vector.Get(), &flow_view,
                        offset_cpu_handle(cpu_start, base + offset, descriptor_increment));
                }
                flow_view.Format = DXGI_FORMAT_R32_UINT;
                for (const UINT offset : {3U, 5U}) {
                    device->CreateShaderResourceView(optical_flow_scene_change.Get(), &flow_view,
                        offset_cpu_handle(cpu_start, base + offset, descriptor_increment));
                }
            }
        }

        const std::array<D3D12_RESOURCE_BARRIER, 2> before_synthesis{
            transition_barrier(
                previous_resource,
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COMMON,
                kShaderReadState),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_synthesis.size()),
            before_synthesis.data());

        const D3D12_RESOURCE_BARRIER before_pack = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        slot.command_list->ResourceBarrier(1, &before_pack);

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetComputeRootSignature(root_signature.Get());
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        slot.command_list->SetComputeRootDescriptorTable(0, gpu_start);
        slot.command_list->SetComputeRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(nvidia_options.hybrid ? hybrid_pack_pipeline.Get() : pack_pipeline.Get());

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction) |
            flow_input_flags(view_format, nvidia_options.input_encoding) |
            (backend == D3D12OpticalFlowBackend::nvidia && nvidia_options.pose_compensated_input ? 4U : 0U);
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        parameters.slice = 0;
        // A against B's own camera; the synthetic's is applied per draw.
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                previous_source.views[view_index],
                current_source.views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        const auto view_mappings=parameters.previous_mappings;
        slot.command_list->SetComputeRoot32BitConstants(
            2,
            kSynthesisConstantCount,
            &parameters,
            0);
        slot.command_list->Dispatch(
            (packed_width + 7U) / 8U,
            (packed_height + 7U) / 8U,
            1);
        const D3D12_RESOURCE_BARRIER before_fidelityfx = transition_barrier(
            packed_color.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        slot.command_list->ResourceBarrier(1, &before_fidelityfx);

        const FfxResourceStates first_output_state = flow_output_consumed
            ? FFX_RESOURCE_STATE_PIXEL_READ
            : FFX_RESOURCE_STATE_UNORDERED_ACCESS;
        HRESULT result = dispatch_fidelityfx(
            slot.command_list.Get(),
            first_output_state);
        if (FAILED(result)) {
            return result;
        }

        if (!flow_output_consumed) {
            const std::array<D3D12_RESOURCE_BARRIER, 2> before_pixel{
                transition_barrier(
                    optical_flow_vector.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
                transition_barrier(
                    optical_flow_scene_change.Get(),
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            };
            slot.command_list->ResourceBarrier(
                static_cast<UINT>(before_pixel.size()),
                before_pixel.data());
        }
        flow_output_consumed = true;

        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        slot.command_list->SetGraphicsRootDescriptorTable(0, gpu_start);
        slot.command_list->SetGraphicsRootDescriptorTable(
            1,
            offset_gpu_handle(
                gpu_start,
                kSrvDescriptorCount,
                descriptor_increment));
        slot.command_list->SetPipelineState(guides ? hybrid_graphics_pipeline.Get() : graphics_pipeline.Get());
        // The guides' motion is read by the hybrid composition.
        const auto motion_barriers = [&](bool to_read) {
            if (!guides) return;
            for (UINT eye = 0; eye < guides->eye_count; ++eye) {
                const auto& guide = guides->eyes[eye];
                if (guide->resource_state == kShaderReadState ||
                    (eye != 0 && guide->motion_vectors.Get() == guides->eyes[0]->motion_vectors.Get())) {
                    continue;
                }
                const auto barrier = to_read
                    ? transition_barrier(guide->motion_vectors.Get(), guide->resource_state, kShaderReadState)
                    : transition_barrier(guide->motion_vectors.Get(), kShaderReadState, guide->resource_state);
                slot.command_list->ResourceBarrier(1, &barrier);
            }
        };
        motion_barriers(true);
        slot.command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            const UINT output_destination = synthetic_output_destination(output, synthetic_destination_index);
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv = output_destination *
                image_description.DepthOrArraySize;
            synthetic_target_state(slot.command_list.Get(), output_destination, true);
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                parameters.previous_mappings = view_mappings;
                apply_synthetic_camera(parameters, output, view_index);
                if (guides) {
                    // As record_game_motion_pair, but with the output
                    // rectangle packed 16 bits a value: the flow keeps its
                    // own constants.
                    const UINT eye = (image_description.DepthOrArraySize == 1 && guides->eye_count == target_views.size() ? view_index : slice);
                    if (eye >= guides->eye_count) return E_INVALIDARG;
                    const auto& guide = guides->eyes[eye];
                    const UINT descriptor_base = eye * kDescriptorBlockSize;
                    slot.command_list->SetGraphicsRootDescriptorTable(0,
                        offset_gpu_handle(gpu_start, descriptor_base, descriptor_increment));
                    parameters.use_game_motion = guide->output_x | (guide->output_y << 16U);
                    parameters.game_motion_padding = guide->output_width | (guide->output_height << 16U);
                    parameters.game_motion_rect = guide_motion_rect(*guide);
                    parameters.game_motion_scale = {
                        guide->scale_x * static_cast<float>(guide->output_width) /
                            parameters.game_motion_rect[2],
                        guide->scale_y * static_cast<float>(guide->output_height) /
                            parameters.game_motion_rect[3]};
                    parameters.game_motion_jitter_delta = guide->jittered
                        ? std::array<float, 2>{guide->jitter_x - guide->previous_jitter_x,
                                               guide->jitter_y - guide->previous_jitter_y}
                        : std::array<float, 2>{};
                }
                slot.command_list->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    slot.command_list.Get(),
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
            synthetic_target_state(slot.command_list.Get(), output_destination, false);
        }

        motion_barriers(false);
        if (guides) *used_game_motion = true;
        const std::array<D3D12_RESOURCE_BARRIER, 1> before_current_copy{
            transition_barrier(
                previous_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COMMON),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_current_copy.size()),
            before_current_copy.data());
        if (nvidia_gpu_timing_enabled && nvidia_timestamp_heap) {
            // Last mark of the pair's synthesis, before the current copy
            // that is submitted separately. The span therefore covers
            // exactly the work the synthetic frame is waiting on.
            slot.command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kSpanEnd);
            // Only this path records a resolve of its own; the NVIDIA path
            // resolves on its synthesis list. Without one the span marks
            // never reach the readback buffer at all.
            slot.command_list->ResolveQueryData(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base,
                kTimestampCount,
                nvidia_timestamp_readback.Get(),
                static_cast<UINT64>(slot.timing_query_base) *
                    sizeof(std::uint64_t));
        }
        // The copy's own transitions travel with the copy, so the resources
        // are left in their normal states while it waits to be submitted.
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy_states{
            transition_barrier(
                current_resource,
                kShaderReadState,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(before_copy_states.size()),
            before_copy_states.data());
        // Recorded here but submitted only after the synthetic frame has gone
        // to the runtime; see flush_current_copy.
        dred_marker(slot.current_copy_command_list.Get(), "OFXR FidelityFX current copy");
        slot.current_copy_command_list->CopyResource(
            current_destination,
            current_resource);
        // DIAGNOSTIC. The real frame is otherwise indistinguishable from one
        // that never arrived: the purple marker rides only on the synthetic,
        // so a steady purple square means either "only synthetics are shown"
        // or "both are shown and 45 Hz reads as steady". Green, offset to the
        // right of the purple one, separates those two readings in one look.
        if (debug_marker) {
            constexpr float green[4]{0.0F, 0.9F, 0.2F, 1.0F};
            const auto marker_to_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_to_rt);
            const D3D12_CPU_DESCRIPTOR_HANDLE marker_rtv_start =
                rtv_heap->GetCPUDescriptorHandleForHeapStart();
            const UINT marker_first_rtv = current_rtv_base +
                current_destination_index * image_description.DepthOrArraySize;
            for (UINT marker_view = 0;
                 marker_view < static_cast<UINT>(target_views.size());
                 ++marker_view) {
                const UINT marker_slice = resolved_array_slice(
                    target_views[marker_view],
                    marker_view,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                clear_synthetic_marker(
                    slot.current_copy_command_list.Get(),
                    offset_cpu_handle(
                        marker_rtv_start,
                        marker_first_rtv + marker_slice,
                        rtv_increment),
                    target_views[marker_view],
                    static_cast<UINT>(image_description.Width),
                    image_description.Height,
                    debug_marker,
                    green,
                    1.6F);
            }
            const auto marker_from_rt = transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_COPY_DEST);
            slot.current_copy_command_list->ResourceBarrier(1, &marker_from_rt);
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_current_copy{
            transition_barrier(
                current_resource,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.current_copy_command_list->ResourceBarrier(
            static_cast<UINT>(after_current_copy.size()),
            after_current_copy.data());
        return S_OK;
    }

    [[nodiscard]] HRESULT record_repeated_pair(
        WorkSlot& slot,
        const RollingSource& retained_source,
        // The camera metadata the repeated image arrived with this time.
        std::span<const D3D12ReprojectionView> current_views,
        std::span<const D3D12ReprojectionView> target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        const std::optional<OverlayPlacement>& debug_marker) noexcept {
        ID3D12Resource* const source = retained_source.resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        create_source_views(slot, source, source);

        const std::array<D3D12_RESOURCE_BARRIER, 2> before_current_copy{
            transition_barrier(
                source,
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(
                current_destination,
                release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_current_copy.size()),
            before_current_copy.data());
        slot.command_list->CopyResource(current_destination, source);
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_synthesis{
            transition_barrier(
                source,
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                kShaderReadState),
            transition_barrier(
                current_destination,
                D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_synthesis.size()),
            before_synthesis.data());

        ID3D12DescriptorHeap* heaps[] = {slot.descriptor_heap.Get()};
        slot.command_list->SetDescriptorHeaps(1, heaps);
        slot.command_list->SetGraphicsRootSignature(root_signature.Get());
        slot.command_list->SetPipelineState(
            backend == D3D12OpticalFlowBackend::nvidia
                ? (nvidia_options.bidirectional
                       ? nvidia_bidirectional_graphics_pipeline.Get()
                       : nvidia_graphics_pipeline.Get())
                : graphics_pipeline.Get());
        slot.command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        SynthesisParameters parameters{};
        parameters.synthesis_flags = packed_synthesis_fraction(synthetic_fraction);
        if (backend == D3D12OpticalFlowBackend::nvidia && nvidia_options.pose_compensated_input)
            parameters.synthesis_flags |= 4U;
        parameters.width = static_cast<UINT>(image_description.Width);
        parameters.height = image_description.Height;
        parameters.array_size = image_description.DepthOrArraySize;
        parameters.packed_width = packed_width;
        parameters.packed_height = packed_height;
        parameters.packed_eye_stride = packed_eye_stride;
        parameters.flow_width = flow_width;
        parameters.flow_height = flow_height;
        parameters.flow_block_size = flow_block_size;
        parameters.synthesis_flags |= 1U;
        for (UINT view_index = 0;
             view_index < static_cast<UINT>(target_views.size());
             ++view_index) {
            parameters.previous_mappings[view_index] = make_camera_mapping(
                retained_source.views[view_index],
                current_views[view_index],
                static_cast<UINT>(image_description.Width),
                image_description.Height);
        }
        const auto view_mappings = parameters.previous_mappings;

        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_start =
            slot.descriptor_heap->GetGPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv_start =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for (UINT output = 0; output < synthetic_output_count(); ++output) {
            const UINT output_destination = synthetic_output_destination(output, synthetic_destination_index);
            parameters.synthesis_flags = (parameters.synthesis_flags & 0xFFU) |
                packed_synthesis_fraction(synthetic_output_fraction(output));
            const UINT first_rtv = output_destination *
                image_description.DepthOrArraySize;
            synthetic_target_state(slot.command_list.Get(), output_destination, true);
            for (UINT view_index = 0;
                 view_index < static_cast<UINT>(target_views.size());
                 ++view_index) {
                const UINT slice = resolved_array_slice(
                    target_views[view_index],
                    view_index,
                    target_views.size(),
                    image_description.DepthOrArraySize);
                parameters.slice = slice;
                parameters.view_index = view_index;
                parameters.previous_mappings = view_mappings;
                apply_synthetic_camera(parameters, output, view_index);
                const UINT descriptor_base =
                    backend == D3D12OpticalFlowBackend::nvidia
                        ? slice * kDescriptorBlockSize
                        : 0;
                const D3D12_GPU_DESCRIPTOR_HANDLE block = offset_gpu_handle(
                    gpu_start,
                    descriptor_base,
                    descriptor_increment);
                slot.command_list->SetGraphicsRootDescriptorTable(0, block);
                slot.command_list->SetGraphicsRootDescriptorTable(
                    1,
                    offset_gpu_handle(
                        block,
                        kSrvDescriptorCount,
                        descriptor_increment));
                slot.command_list->SetGraphicsRoot32BitConstants(
                    2,
                    kSynthesisConstantCount,
                    &parameters,
                    0);
                const D3D12_CPU_DESCRIPTOR_HANDLE rtv = offset_cpu_handle(
                    rtv_start,
                    first_rtv + slice,
                    rtv_increment);
                slot.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                set_viewport_and_scissor(
                    slot.command_list.Get(),
                    target_views[view_index].image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height);
                slot.command_list->DrawInstanced(3, 1, 0, 0);
                clear_synthetic_marker(slot.command_list.Get(), rtv, target_views[view_index],
                    static_cast<UINT>(image_description.Width), image_description.Height, debug_marker);
            }
            synthetic_target_state(slot.command_list.Get(), output_destination, false);
        }

        const D3D12_RESOURCE_BARRIER after_synthesis = transition_barrier(
            source,
            kShaderReadState,
            D3D12_RESOURCE_STATE_COMMON);
        slot.command_list->ResourceBarrier(1, &after_synthesis);
        return S_OK;
    }

    // Submits the recorded work. The current output's copy is closed either
    // way, but with defer_current_copy it is held back for flush_current_copy
    // to submit once the synthetic frame has reached the runtime. Two fence
    // values are reserved in that case: the first is signalled here and marks
    // the synthetic's pixels done, the second is what the copy will signal and
    // is what the slot, the destination and the history lease are tracked
    // against, so none of them come free until the copy has actually run.
    [[nodiscard]] HRESULT execute_and_signal(
        WorkSlot& slot,
        std::uint32_t slot_index,
        RollingSource* newly_acquired,
        std::uint64_t* output_fence_value,
        bool defer_current_copy) noexcept {
        if (newly_acquired == nullptr || output_fence_value == nullptr ||
            slot.current_copy_command_list == nullptr ||
            next_fence_value == 0 ||
            next_fence_value >=
                std::numeric_limits<std::uint64_t>::max() - 1U) {
            synthesis_enabled = false;
            return E_FAIL;
        }

        HRESULT result = slot.command_list->Close();
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 40, last_submitted_fence_value,
                    next_fence_value);
                // The failed list was never submitted and its allocator had
                // already passed the per-slot retirement gate. Replace the
                // poisoned recording object, latch a full FSR context reset,
                // and surface recoverable backpressure so continuity primes
                // again instead of permanently disabling frame generation.
                const HRESULT rejected_close = result;
                if (recording_uses_fsr_context) {
                    fsr_context_reset_pending[active_fsr_domain] = true;
                }
                slot.command_list.Reset();
                result = slot.allocator->Reset();
                if (SUCCEEDED(result)) {
                    result = device->CreateCommandList(
                        queue->GetDesc().NodeMask,
                        D3D12_COMMAND_LIST_TYPE_DIRECT,
                        slot.allocator.Get(),
                        nullptr,
                        IID_PPV_ARGS(slot.command_list.GetAddressOf()));
                }
                if (SUCCEEDED(result)) {
                    result = slot.command_list->Close();
                }
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_recovery,
                    result,
                    3,
                    static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(rejected_close)),
                    last_submitted_fence_value);
                if (SUCCEEDED(result)) {
                    return DXGI_ERROR_WAS_STILL_DRAWING;
                }
            }
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        ID3D12CommandList* lists[] = {slot.command_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        two_frame_capture_submission(queue.Get(), 1, lists);
        completion_unknown = true;

        std::uint64_t value = 0;
        if (defer_current_copy) {
            // Signal the synthetic first: its pixels exist now, and it goes
            // to the runtime a display period before the current frame.
            const std::uint64_t synthetic_value = next_fence_value;
            HRESULT signal_result =
                queue->Signal(fence.Get(), synthetic_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
            ++next_fence_value;
            last_synthetic_fence_value = synthetic_value;
            value = next_fence_value;
            ++next_fence_value;
            pending_copy_slot = slot_index;
            pending_copy_fence_value = value;
        } else {
            ID3D12CommandList* copy_lists[] = {
                slot.current_copy_command_list.Get()};
            queue->ExecuteCommandLists(1, copy_lists);
            value = next_fence_value;
            ++next_fence_value;
            last_synthetic_fence_value = value;
            HRESULT signal_result = queue->Signal(fence.Get(), value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
        }

        completion_unknown = false;
        slot.fence_value = value;
        // Both backends now write timestamps, so both must mark the slot as
        // having a resolve waiting to be read back.
        slot.timing_pending = nvidia_gpu_timing_enabled;
        last_submitted_fence_value = value;
        *output_fence_value = value;
        return S_OK;
    }

    // Submits a copy left pending by execute_and_signal and signals the value
    // the rest of the pipeline is already tracking against it.
    [[nodiscard]] HRESULT flush_pending_copy() noexcept {
        if (pending_copy_slot == kNoPendingCopy) {
            return S_OK;
        }
        if (pending_copy_slot >= work_slots.size() || !synthesis_enabled) {
            pending_copy_slot = kNoPendingCopy;
            return E_FAIL;
        }
        WorkSlot& slot = work_slots[pending_copy_slot];
        const std::uint64_t value = pending_copy_fence_value;
        pending_copy_slot = kNoPendingCopy;
        ID3D12CommandList* copy_lists[] = {
            slot.current_copy_command_list.Get()};
        queue->ExecuteCommandLists(1, copy_lists);
        const HRESULT result = queue->Signal(fence.Get(), value);
        if (FAILED(result)) {
            synthesis_enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT execute_nvidia_and_signal(
        WorkSlot& slot,
        std::uint32_t slot_index,
        RollingSource* newly_acquired,
        std::uint64_t* output_fence_value,
        bool defer_current_copy) noexcept {
        if (newly_acquired == nullptr || output_fence_value == nullptr ||
            slot.synthesis_command_list == nullptr ||
            slot.current_copy_command_list == nullptr ||
            next_fence_value == 0 ||
            next_fence_value >=
                std::numeric_limits<std::uint64_t>::max() - 1U ||
            next_nvidia_fence_value == 0 ||
            next_nvidia_fence_value >
                std::numeric_limits<std::uint64_t>::max() -
                    image_description.DepthOrArraySize) {
            synthesis_enabled = false;
            return E_FAIL;
        }

        HRESULT result = slot.command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.synthesis_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = slot.current_copy_command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        result = queue->Wait(nvidia_fence.Get(), last_nvidia_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }

        ID3D12CommandList* pack_lists[] = {slot.command_list.Get()};
        if (slot.ram_pair) slot.ram_gpu.submitted(slot.ram_source_identity,slot.ram_source_serial);
        queue->ExecuteCommandLists(1, pack_lists);
        completion_unknown = true;

        const std::uint64_t pack_ready_value = next_fence_value;
        result = queue->Signal(fence.Get(), pack_ready_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(*newly_acquired);
            return result;
        }
        ++next_fence_value;

        if (nvidia_gpu_timing_enabled) {
            result = slot.timing_marker_allocator->Reset();
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            result = slot.timing_marker_command_list->Reset(
                slot.timing_marker_allocator.Get(),
                nullptr);
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            slot.timing_marker_command_list->EndQuery(
                nvidia_timestamp_heap.Get(),
                D3D12_QUERY_TYPE_TIMESTAMP,
                slot.timing_query_base + kNvidiaTimestampEye0End);
            result = slot.timing_marker_command_list->Close();
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
        }

        std::uint64_t preceding_eye_value = 0;
        std::uint64_t eye0_fence_value = 0;
        for (UINT eye_index = 0;
             eye_index < image_description.DepthOrArraySize;
             ++eye_index) {
            NvidiaEyeResources& eye = slot.nvidia_eyes[eye_index];
            std::array<NV_OF_FENCE_POINT, 2> input_fence_points{};
            input_fence_points[0].fence = fence.Get();
            input_fence_points[0].value = pack_ready_value;
            UINT input_fence_count = 1;
            if (preceding_eye_value != 0) {
                input_fence_points[input_fence_count].fence =
                    nvidia_fence.Get();
                input_fence_points[input_fence_count].value =
                    preceding_eye_value;
                ++input_fence_count;
            }

            NV_OF_EXECUTE_INPUT_PARAMS_D3D12 input{};
            input.inputFrame = eye.current_input_handle;
            input.referenceFrame = eye.previous_input_handle;
            input.disableTemporalHints = NV_OF_TRUE;
            input.numFencePoints = input_fence_count;
            input.fencePoint = input_fence_points.data();

            NV_OF_FENCE_POINT output_fence_point{};
            output_fence_point.fence = nvidia_fence.Get();
            output_fence_point.value = next_nvidia_fence_value;
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 output{};
            output.outputBuffer = eye.flow_handle;
            output.outputCostBuffer = eye.cost_handle;
            if (nvidia_options.bidirectional) {
                output.bwdOutputBuffer = eye.backward_flow_handle;
                output.bwdOutputCostBuffer = eye.backward_cost_handle;
            }
            output.fencePoint = &output_fence_point;
            result = nvidia_result(nvidia_api.nvOFExecuteD3D12(
                nvidia_contexts[eye_index],
                &input,
                &output));
            if (FAILED(result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return result;
            }
            preceding_eye_value = next_nvidia_fence_value;
            if (eye_index == 0) {
                eye0_fence_value = next_nvidia_fence_value;
                if (nvidia_gpu_timing_enabled) {
                    result = queue->Wait(
                        nvidia_fence.Get(), eye0_fence_value);
                    if (FAILED(result)) {
                        synthesis_enabled = false;
                        untracked_source = std::move(*newly_acquired);
                        return result;
                    }
                    ID3D12CommandList* timing_lists[] = {
                        slot.timing_marker_command_list.Get()};
                    queue->ExecuteCommandLists(1, timing_lists);
                }
            }
            last_nvidia_fence_value = next_nvidia_fence_value;
            ++next_nvidia_fence_value;
        }
        result = queue->Wait(nvidia_fence.Get(), last_nvidia_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            untracked_source = std::move(*newly_acquired);
            return result;
        }
        ID3D12CommandList* synthesis_lists[] = {
            slot.synthesis_command_list.Get()};
        queue->ExecuteCommandLists(1, synthesis_lists);
        two_frame_capture_submission(queue.Get(), 1, synthesis_lists);

        // The synthetic's pixels exist once this signals. The current copy
        // is held back so it is not queued ahead of the synthetic frame.
        std::uint64_t completion_value = 0;
        if (defer_current_copy) {
            // Signal the synthetic first: its pixels exist now, and it goes
            // to the runtime a display period before the current frame.
            const std::uint64_t synthetic_value = next_fence_value;
            HRESULT signal_result =
                queue->Signal(fence.Get(), synthetic_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
            ++next_fence_value;
            last_synthetic_fence_value = synthetic_value;
            completion_value = next_fence_value;
            ++next_fence_value;
            pending_copy_slot = slot_index;
            pending_copy_fence_value = completion_value;
        } else {
            ID3D12CommandList* copy_lists[] = {
                slot.current_copy_command_list.Get()};
            queue->ExecuteCommandLists(1, copy_lists);
            completion_value = next_fence_value;
            ++next_fence_value;
            last_synthetic_fence_value = completion_value;
            HRESULT signal_result = queue->Signal(fence.Get(), completion_value);
            if (FAILED(signal_result)) {
                synthesis_enabled = false;
                untracked_source = std::move(*newly_acquired);
                return signal_result;
            }
        }

        completion_unknown = false;
        slot.fence_value = completion_value;
        slot.timing_pending = nvidia_gpu_timing_enabled;
        last_submitted_fence_value = completion_value;
        *output_fence_value = completion_value;
        return S_OK;
    }

    [[nodiscard]] HRESULT submit_copy(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* output_ticket,
        bool preserve_temporal_anchor) noexcept {
        if (output_ticket == nullptr) {
            return E_POINTER;
        }
        *output_ticket = {};
        if (!synthesis_enabled || (previous.active() && !preserve_temporal_anchor) ||
            (preserve_temporal_anchor && generation_mode != D3D12FrameGenerationMode::ofxr) || untracked_source.active() ||
            current.serial == 0 || current.fence_value == 0 ||
            !valid_view_layout(
                current_source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            current_destination_index >= current_destinations.size()) {
            return E_INVALIDARG;
        }
        static_cast<void>(flush_pending_copy());
        HRESULT result = submission_available();
        if (FAILED(result)) {
            return result;
        }
        result = destination_available(current_destinations[current_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        std::uint32_t work_slot_index = 0;
        result = acquire_work_slot(&work_slot_index);
        if (FAILED(result)) {
            return result;
        }

        RollingSource next{};
        result = acquire_source(current, current_source_views, &next);
        if (FAILED(result)) {
            return result;
        }
        const auto cancel_next = [this, &next]() noexcept {
            history->cancel_consumer(next.lease);
            next.clear();
        };
        ID3D12Resource* const destination =
            current_destinations[current_destination_index].resource.Get();
        if (next.resource.Get() == destination) {
            cancel_next();
            return E_INVALIDARG;
        }

        // Transport owns only the ordinary copy list. In particular it does
        // not reset/record the optical-flow pack, synthesis or deferred-copy
        // lists, and its behavior cannot depend on the selected backend.
        WorkSlot& slot = work_slots[work_slot_index];
        slot.ram_gpu.harvest();
        slot.ram_pair = false;
        slot.timing_pending = false;
        recording_uses_fsr_context = false;
        result = slot.allocator->Reset();
        if (SUCCEEDED(result)) {
            result = slot.command_list->Reset(slot.allocator.Get(), nullptr);
        }
        if (FAILED(result)) {
            synthesis_enabled = false;
            cancel_next();
            return result;
        }
        const std::array<D3D12_RESOURCE_BARRIER, 2> before_copy{
            transition_barrier(next.resource.Get(),
                D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition_barrier(destination, release_state,
                D3D12_RESOURCE_STATE_COPY_DEST),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(before_copy.size()), before_copy.data());
        slot.command_list->CopyResource(destination, next.resource.Get());
        const std::array<D3D12_RESOURCE_BARRIER, 2> after_copy{
            transition_barrier(next.resource.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
            transition_barrier(destination, D3D12_RESOURCE_STATE_COPY_DEST,
                release_state),
        };
        slot.command_list->ResourceBarrier(
            static_cast<UINT>(after_copy.size()), after_copy.data());
        result = slot.command_list->Close();
        if (FAILED(result)) {
            synthesis_enabled = false;
            cancel_next();
            return result;
        }
        if (next_fence_value == 0 ||
            next_fence_value >= std::numeric_limits<std::uint64_t>::max() - 1U) {
            synthesis_enabled = false;
            cancel_next();
            return E_FAIL;
        }
        result = queue->Wait(next.producer_fence.Get(), current.fence_value);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }

        ID3D12CommandList* lists[]{slot.command_list.Get()};
        queue->ExecuteCommandLists(1, lists);
        two_frame_capture_submission(queue.Get(), 1, lists);
        completion_unknown = true;
        const std::uint64_t fence_value = next_fence_value++;
        result = queue->Signal(fence.Get(), fence_value);
        if (FAILED(result)) {
            // GPU access was submitted: retain the lease rather than cancel
            // it when no completion fence can prove that access retired.
            synthesis_enabled = false;
            untracked_source = std::move(next);
            return result;
        }
        completion_unknown = false;
        slot.fence_value = fence_value;
        last_submitted_fence_value = fence_value;
        last_synthetic_fence_value = fence_value;
        current_destinations[current_destination_index].fence_value = fence_value;
        next_work_slot = (work_slot_index + 1U) %
            static_cast<std::uint32_t>(work_slots.size());
        result = history->retire_consumer(next.lease, fence.Get(), fence_value);
        if (FAILED(result)) {
            // Submission exists; retain ownership on retirement failure too.
            synthesis_enabled = false;
            untracked_source = std::move(next);
            return result;
        }
        output_ticket->inputs=submission_inputs(next);
        next.clear();
        fsr_stale_guide_gap = false;

        output_ticket->previous_serial = 0;
        output_ticket->current_serial = current.serial;
        output_ticket->fence_value = fence_value;
        output_ticket->synthetic_fence_value = fence_value;
        output_ticket->work_slot = work_slot_index;
        output_ticket->synthetic_destination_index =
            std::numeric_limits<std::uint32_t>::max();
        output_ticket->current_destination_index = current_destination_index;
        return S_OK;
    }

    [[nodiscard]] HRESULT submit_prime(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* output_ticket,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
        D3D12PresentationTransform presentation_transform) noexcept {
        if (output_ticket == nullptr) {
            return E_POINTER;
        }
        *output_ticket = {};
        if (!synthesis_enabled || previous.active() || untracked_source.active() ||
            current.serial == 0 || current.fence_value == 0 ||
            !valid_view_layout(
                current_source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            current_destination_index >= current_destinations.size()) {
            return E_INVALIDARG;
        }
        // Before anything is asked about outstanding work. A copy still
        // pending holds its work slot and history lease, and the value it
        // will signal is the one submission_available and
        // wait_for_previous_submission both test, so a caller that never
        // flushed would otherwise read as permanently busy rather than
        // costing a frame of latency.
        static_cast<void>(flush_pending_copy());
        HRESULT result = submission_available();
        if (FAILED(result)) {
            return result;
        }
        result =
            destination_available(current_destinations[current_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        std::uint32_t work_slot_index = 0;
        result = acquire_work_slot(&work_slot_index);
        if (FAILED(result)) {
            return result;
        }

        RollingSource next{};
        result = acquire_source(current, current_source_views, &next);
        if (FAILED(result)) {
            return result;
        }
        next.motion_vectors = std::move(motion_vectors);
        next.presentation_transform = std::move(presentation_transform);
        const auto cancel_next = [this, &next]() noexcept {
            history->cancel_consumer(next.lease);
            next.clear();
        };
        if (generation_mode != D3D12FrameGenerationMode::ofxr &&
            !has_complete_game_motion_set(next)) {
            // FSR cannot recover from a prime that has no temporal guides: the
            // first later guide set would always be compared with this empty
            // anchor, rejected, retired, and followed by another empty prime.
            // Wait for the first complete DLSS set so the retained source can
            // participate in the next real temporal pair.
            if (generation_mode == D3D12FrameGenerationMode::fsr3)
                report_fsr_frame_generation_status(FsrFrameGenerationStatus::waiting_for_guides);
            else report_dlss_motion_vector_status(DlssMotionVectorStatus::waiting_for_dlss);
            bridge_flight_logger().event(
                BridgeFlightOperation::fsr_recovery,
                DXGI_ERROR_WAS_STILL_DRAWING,
                7,
                current.serial,
                current.fence_value);
            cancel_next();
            return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        if (next.resource.Get() ==
            current_destinations[current_destination_index].resource.Get()) {
            cancel_next();
            return E_INVALIDARG;
        }

        WorkSlot& slot = work_slots[work_slot_index];
        result = reset_work_slot(slot);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }
        result = record_prime(
            slot,
            next,
            current_destinations[current_destination_index].resource.Get());
        if (FAILED(result)) {
            synthesis_enabled = false;
            cancel_next();
            return result;
        }
        result = queue->Wait(next.producer_fence.Get(), current.fence_value);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }

        std::uint64_t fence_value = 0;
        result = execute_and_signal(
            slot, work_slot_index, &next, &fence_value, false);
        if (FAILED(result)) {
            if (next.active()) {
                cancel_next();
            }
            return result;
        }
        next.last_use_fence_value = fence_value;
        // Prime records no complete OFA stage interval. Do not expose its
        // unresolved timestamp slots as a generated pair to the profiler.
        slot.timing_pending = false;
        output_ticket->inputs=submission_inputs(next);
        previous = std::move(next);
        fsr_stale_guide_gap = false;
        current_destinations[current_destination_index].fence_value = fence_value;
        next_work_slot = (work_slot_index + 1U) %
            static_cast<std::uint32_t>(work_slots.size());

        output_ticket->previous_serial = 0;
        output_ticket->current_serial = current.serial;
        output_ticket->fence_value = fence_value;
        output_ticket->synthetic_fence_value = last_synthetic_fence_value;
        output_ticket->work_slot = work_slot_index;
        output_ticket->synthetic_destination_index =
            std::numeric_limits<std::uint32_t>::max();
        output_ticket->current_destination_index = current_destination_index;
        return S_OK;
    }

    [[nodiscard]] HRESULT submit_pair(
        const D3D12HistoryCaptureTicket& current,
        std::span<const D3D12ReprojectionView> current_source_views,
        std::span<const D3D12ReprojectionView> synthetic_target_views,
        std::uint32_t synthetic_destination_index,
        std::uint32_t current_destination_index,
        D3D12FrameSynthesisTicket* output_ticket,
        const std::optional<OverlayPlacement>& debug_marker,
        std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
        bool defer_current_copy,
        float interpolation_fraction,
        D3D12PresentationTransform presentation_transform,
        const std::optional<D3D12ExtraSynthetic>& extra_synthetic,
        bool allow_synthetic_camera) noexcept {
        synthetic_fraction = usable_synthesis_fraction(interpolation_fraction, false);
        synthetic_camera_turned = {};
        synthetic_camera_requested=allow_synthetic_camera;
        fsr_pair_in_target_camera=true;
        if (output_ticket == nullptr) {
            return E_POINTER;
        }
        *output_ticket = {};
        std::span<const D3D12ReprojectionView> extra_target_views = current_source_views;
        extra_output.reset();
        if (extra_synthetic) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3 ||
                extra_synthetic->destination_index >= synthetic_destinations.size() ||
                extra_synthetic->destination_index == synthetic_destination_index ||
                !std::isfinite(extra_synthetic->interpolation_fraction) ||
                extra_synthetic->interpolation_fraction <= 0.05F ||
                extra_synthetic->interpolation_fraction >= 0.95F) return E_INVALIDARG;
            extra_output = extra_synthetic;
            if (!extra_synthetic->target_views.empty()) extra_target_views = extra_synthetic->target_views;
        }
        const bool repeated_capture =
            previous.active() && current.serial == previous.ticket.serial &&
            current.fence_value == previous.ticket.fence_value &&
            current.slot == previous.ticket.slot &&
            current.source_index == previous.ticket.source_index;
        const bool advanced_capture =
            previous.active() &&
            previous.ticket.serial != std::numeric_limits<std::uint64_t>::max() &&
            current.serial == previous.ticket.serial + 1U &&
            current.slot != previous.ticket.slot;
        const bool stale_guide_gapped_capture =
            generation_mode == D3D12FrameGenerationMode::fsr3 &&
            fsr_stale_guide_gap && previous.active() &&
            previous.ticket.serial != std::numeric_limits<std::uint64_t>::max() &&
            current.serial > previous.ticket.serial &&
            current.slot != previous.ticket.slot;
        if (!synthesis_enabled || !previous.active() || untracked_source.active() ||
            current.serial == 0 || current.fence_value == 0 ||
            previous.ticket.serial == std::numeric_limits<std::uint64_t>::max() ||
            (!repeated_capture && !advanced_capture &&
             !stale_guide_gapped_capture) ||
            previous.view_count !=
                static_cast<UINT>(current_source_views.size()) ||
            synthetic_target_views.size() != current_source_views.size() ||
            extra_target_views.size() != current_source_views.size() ||
            !valid_view_layout(
                current_source_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            !valid_view_layout(
                synthetic_target_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            !valid_view_layout(
                extra_target_views,
                static_cast<UINT>(image_description.Width),
                image_description.Height,
                image_description.DepthOrArraySize) ||
            synthetic_destination_index >= synthetic_destinations.size() ||
            current_destination_index >= current_destinations.size()) {
            return E_INVALIDARG;
        }
        const std::span<const D3D12ReprojectionView> previous_views(
            previous.views.data(),
            previous.view_count);
        if (!same_view_layout(
                previous_views,
                current_source_views,
                image_description.DepthOrArraySize) ||
            !same_view_layout(
                current_source_views,
                synthetic_target_views,
                image_description.DepthOrArraySize) ||
            !same_view_layout(
                current_source_views,
                extra_target_views,
                image_description.DepthOrArraySize)) {
            return E_INVALIDARG;
        }
        const std::array<std::span<const D3D12ReprojectionView>, 2> output_target_views{
            synthetic_target_views,
            extra_target_views,
        };
        for (std::size_t view_index = 0;
             view_index < current_source_views.size();
             ++view_index) {
            const D3D12ReprojectionView& current_view =
                current_source_views[view_index];
            if (!valid_view(current_view) ||
                !rect_within_resource(
                    current_view.image_rect,
                    static_cast<UINT>(image_description.Width),
                    image_description.Height)) {
                return E_INVALIDARG;
            }
            for (std::size_t output = 0; output < output_target_views.size(); ++output) {
                const D3D12ReprojectionView& target_view =
                    output_target_views[output][view_index];
                if (!valid_view(target_view) ||
                    !rect_within_resource(
                        target_view.image_rect,
                        static_cast<UINT>(image_description.Width),
                        image_description.Height) ||
                    !(allow_synthetic_camera ? same_projection(current_view, target_view) : same_camera(current_view, target_view))) {
                    return E_INVALIDARG;
                }
                const auto rotation = target_to_current_rotation(current_view, target_view);
                synthetic_camera_rotations[output][view_index] = rotation;
                synthetic_camera_turned[output] =
                    synthetic_camera_turned[output] || camera_turned(rotation);
            }
        }
        if (allow_luke_guide_device_alias && bridge_flight_logger().ram_enabled()) {
            const auto identity = reinterpret_cast<std::uint64_t>(previous.producer_fence.Get());
            bridge_flight_logger().event(BridgeFlightOperation::luke_temporal_pair,
                0, identity, previous.ticket.serial, current.serial);
            bridge_flight_logger().event(BridgeFlightOperation::luke_temporal_pair,
                1, identity, current.serial,
                (static_cast<std::uint64_t>(current_destination_index) << 32) | synthetic_destination_index);
            const std::span<const D3D12ReprojectionView> cameras[]{
                previous_views, current_source_views, synthetic_target_views};
            for (int phase = 0; phase < 3; ++phase)
                for (std::size_t view = 0; view < cameras[phase].size(); ++view) {
                    const auto& camera = cameras[phase][view];
                    const float values[]{camera.pose.position.x, camera.pose.position.y, camera.pose.position.z,
                        camera.pose.orientation.x, camera.pose.orientation.y, camera.pose.orientation.z,
                        camera.pose.orientation.w, camera.fov.angle_left, camera.fov.angle_right,
                        camera.fov.angle_up, camera.fov.angle_down, 0.0f};
                    for (int pair = 0; pair < 6; ++pair) {
                        std::uint32_t x{}, y{};
                        std::memcpy(&x, &values[pair * 2], sizeof(x));
                        std::memcpy(&y, &values[pair * 2 + 1], sizeof(y));
                        bridge_flight_logger().event(BridgeFlightOperation::luke_temporal_pose,
                            phase * 100 + view * 10 + pair, identity,
                            phase == 0 ? previous.ticket.serial : current.serial,
                            (static_cast<std::uint64_t>(x) << 32) | y);
                    }
                }
        }
        // NVIDIA D3D11 interop submits the current copy immediately and uses
        // per-slot command resources. A prior pair may still be executing if
        // the next slot and both destinations are free. Deferred-copy and
        // shared-resource routes flush first and retain the complete-
        // submission fence gate.
        const bool independent_nvidia_pair =
            backend == D3D12OpticalFlowBackend::nvidia && !defer_current_copy;
        if (independent_nvidia_pair && pending_copy_slot != kNoPendingCopy) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        if (!independent_nvidia_pair) {
            static_cast<void>(flush_pending_copy());
            const HRESULT previous_result = submission_available();
            if (FAILED(previous_result)) {
                return previous_result;
            }
        }
        HRESULT result =
            destination_available(current_destinations[current_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        result = destination_available(
            synthetic_destinations[synthetic_destination_index]);
        if (FAILED(result)) {
            return result;
        }
        ID3D12Resource* const extra_destination = extra_output
            ? synthetic_destinations[extra_output->destination_index].resource.Get() : nullptr;
        if (extra_output) {
            result = destination_available(synthetic_destinations[extra_output->destination_index]);
            if (FAILED(result)) return result;
        }
        std::uint32_t work_slot_index = 0;
        result = acquire_work_slot(&work_slot_index);
        if (FAILED(result)) {
            return result;
        }

        if (repeated_capture) {
            ID3D12Resource* const repeated_resource = previous.resource.Get();
            ID3D12Resource* const synthetic_destination =
                synthetic_destinations[synthetic_destination_index].resource.Get();
            ID3D12Resource* const current_destination =
                current_destinations[current_destination_index].resource.Get();
            if (repeated_resource == synthetic_destination ||
                repeated_resource == current_destination || repeated_resource == extra_destination) {
                return E_INVALIDARG;
            }

            WorkSlot& slot = work_slots[work_slot_index];
            result = reset_work_slot(slot);
            if (FAILED(result)) {
                return result;
            }
            result = record_repeated_pair(
                slot,
                previous,
                current_source_views,
                synthetic_target_views,
                synthetic_destination_index,
                current_destination_index,
                debug_marker);
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
            if (slot.synthesis_command_list != nullptr) {
                result = slot.synthesis_command_list->Close();
                if (FAILED(result)) {
                    synthesis_enabled = false;
                    return result;
                }
            }

            RollingSource no_new_source{};
            std::uint64_t fence_value = 0;
            result = execute_and_signal(
                slot, work_slot_index, &no_new_source, &fence_value, false);
            if (FAILED(result)) {
                return result;
            }

            previous.ticket = current;
            previous.view_count = static_cast<UINT>(current_source_views.size());
            std::copy(
                current_source_views.begin(),
                current_source_views.end(),
                previous.views.begin());
            previous.last_use_fence_value = fence_value;
            current_destinations[current_destination_index].fence_value = fence_value;
            synthetic_destinations[synthetic_destination_index].fence_value = fence_value;
            if (extra_output) synthetic_destinations[extra_output->destination_index].fence_value = fence_value;
            next_work_slot = (work_slot_index + 1U) %
                static_cast<std::uint32_t>(work_slots.size());

            if (native_dlss) {
                auto receipt=std::make_shared<D3D12NativeFrameResult>();
                receipt->outputs=synthetic_output_count();receipt->skip=1;receipt->detail=20;
                receipt->mask.store(0,std::memory_order_release);
                output_ticket->native_result=std::move(receipt);
                bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_record,1,0,20,0);
            }
            output_ticket->previous_serial = current.serial;
            output_ticket->current_serial = current.serial;
            output_ticket->fence_value = fence_value;
            output_ticket->work_slot = work_slot_index;
            output_ticket->synthetic_destination_index =
                synthetic_destination_index;
            output_ticket->current_destination_index = current_destination_index;
            output_ticket->synthetics_in_target_camera = true;
            return S_OK;
        }

        RollingSource next{};
        result = acquire_source(current, current_source_views, &next);
        if (FAILED(result)) {
            return result;
        }
        next.motion_vectors = std::move(motion_vectors);
        next.presentation_transform = std::move(presentation_transform);
        const auto cancel_next = [this, &next]() noexcept {
            history->cancel_consumer(next.lease);
            next.clear();
        };
        ID3D12Resource* const synthetic_destination =
            synthetic_destinations[synthetic_destination_index].resource.Get();
        ID3D12Resource* const current_destination =
            current_destinations[current_destination_index].resource.Get();
        if (previous.resource.Get() == next.resource.Get() ||
            previous.resource.Get() == synthetic_destination ||
            previous.resource.Get() == current_destination ||
            next.resource.Get() == synthetic_destination ||
            next.resource.Get() == current_destination ||
            (extra_destination && (previous.resource.Get() == extra_destination ||
                                   next.resource.Get() == extra_destination))) {
            cancel_next();
            return E_INVALIDARG;
        }

        WorkSlot& slot = work_slots[work_slot_index];
        result = reset_work_slot(slot);
        if (FAILED(result)) {
            cancel_next();
            return result;
        }
        // R22 applies to OFXR vectors, Hybrid and native DLSS-G only.
        // FSR consumes its unchanged release-bound guides; no extra pass there.
        if (generation_mode != D3D12FrameGenerationMode::fsr3 && next.motion_vectors) {
            bool bridged = false;
            for (const auto& eye : next.motion_vectors->eyes)
                bridged |= eye && eye->bridge != nullptr;
            if (bridged) {
                if (!guide_composer_tried) {
                    guide_composer_tried = true;
                    guide_composer_ready = SUCCEEDED(guide_composer.initialize(device.Get()));
                }
                if (guide_composer_ready) {
                    slot.guide_composer_inputs = next.motion_vectors;
                    next.motion_vectors = guide_composer.compose(slot.command_list.Get(),
                        static_cast<UINT>(work_slot_index), next.motion_vectors,
                        {next.views.data(), next.view_count});
                }
            }
        }
        bool used_game_motion = false;
        result = record_pair(
            slot,
            previous,
            next,
            synthetic_target_views,
            synthetic_destination_index,
            current_destination_index,
            debug_marker,
            &used_game_motion);
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3 &&
                result != DXGI_ERROR_WAS_STILL_DRAWING &&
                result != kD3D12FrameSynthesisStaleGuides) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 1, work_slot_index, current.serial);
            }
            // A DLSS guide pair can be incomplete during startup, reset, or a
            // transient eye mismatch. The layer will discard this transaction,
            // clear continuity and prime again on the next application frame.
            // Keep the synthesizer alive so the first incomplete pair cannot
            // permanently burn the FSR route.
            const bool fsr_guides_not_ready =
                generation_mode == D3D12FrameGenerationMode::fsr3 &&
                (result == DXGI_ERROR_WAS_STILL_DRAWING ||
                 result == kD3D12FrameSynthesisStaleGuides);
            if (fsr_guides_not_ready) {
                // record_fsr_pair reports this only before recording GPU work.
                // Close the freshly reset lists so the same work slot can be
                // reset and reused when the layer primes the next frame.
                HRESULT close_result = slot.command_list->Close();
                if (SUCCEEDED(close_result)) {
                    close_result = slot.current_copy_command_list->Close();
                }
                if (SUCCEEDED(close_result) &&
                    slot.synthesis_command_list != nullptr) {
                    close_result = slot.synthesis_command_list->Close();
                }
                if (FAILED(close_result)) {
                    synthesis_enabled = false;
                    cancel_next();
                    return close_result;
                }
            } else {
                synthesis_enabled = false;
            }
            cancel_next();
            if (result == kD3D12FrameSynthesisStaleGuides) {
                // This capture was intentionally passed through. Permit the
                // retained source to pair with the next capture even though
                // their history serials will no longer be consecutive.
                fsr_stale_guide_gap = true;
            }
            return result;
        }
        result = queue->Wait(
            previous.producer_fence.Get(),
            previous.ticket.fence_value);
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 2, previous.ticket.fence_value, current.serial);
            }
            cancel_next();
            return result;
        }
        result = queue->Wait(next.producer_fence.Get(), current.fence_value);
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 3, current.fence_value, current.serial);
            }
            cancel_next();
            return result;
        }

        std::uint64_t fence_value = 0;
        result = backend == D3D12OpticalFlowBackend::nvidia && !used_game_motion
                     ? execute_nvidia_and_signal(
                           slot, work_slot_index, &next, &fence_value,
                           defer_current_copy)
                     : execute_and_signal(
                           slot, work_slot_index, &next, &fence_value,
                           defer_current_copy);
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 4, work_slot_index, current.serial);
            }
            if (next.active()) {
                cancel_next();
            }
            return result;
        }
        if (generation_mode == D3D12FrameGenerationMode::fsr3) {
            last_fsr_context_fence_values[active_fsr_domain] = fence_value;
        }
        if (native_dlss) native_dlss->submitted(fence.Get(), fence_value, last_synthetic_fence_value);
        if (used_game_motion && (!native_dlss || native_pair_generated)) report_dlss_motion_vector_use();

        next.last_use_fence_value = fence_value;
        result = history->retire_consumer(
            previous.lease,
            fence.Get(),
            fence_value);
        if (FAILED(result)) {
            if (generation_mode == D3D12FrameGenerationMode::fsr3) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::fsr_submit_stage,
                    result, 5, fence_value, current.serial);
            }
            synthesis_enabled = false;
            untracked_source = std::move(next);
            return result;
        }

        output_ticket->inputs=submission_inputs(previous,&next);
        const std::uint64_t previous_serial = previous.ticket.serial;
        previous = std::move(next);
        fsr_stale_guide_gap = false;
        current_destinations[current_destination_index].fence_value = fence_value;
        synthetic_destinations[synthetic_destination_index].fence_value = fence_value;
            if (extra_output) synthetic_destinations[extra_output->destination_index].fence_value = fence_value;
        next_work_slot = (work_slot_index + 1U) %
            static_cast<std::uint32_t>(work_slots.size());

        if (native_dlss) output_ticket->native_result = native_dlss->last_result();
        output_ticket->previous_serial = previous_serial;
        output_ticket->current_serial = current.serial;
        output_ticket->fence_value = fence_value;
        output_ticket->synthetic_fence_value = last_synthetic_fence_value;
        output_ticket->work_slot = work_slot_index;
        output_ticket->synthetic_destination_index = synthetic_destination_index;
        output_ticket->current_destination_index = current_destination_index;
        // A skipped native pair copied B into its outputs.
        output_ticket->synthetics_in_target_camera = generation_mode==D3D12FrameGenerationMode::fsr3 ? fsr_pair_in_target_camera : (!native_dlss || native_pair_generated);
        return S_OK;
    }

    [[nodiscard]] HRESULT retire_previous() noexcept {
        if (completion_unknown || untracked_source.active()) {
            synthesis_enabled = false;
            return HRESULT_FROM_WIN32(ERROR_IO_INCOMPLETE);
        }
        if (!previous.active()) { fsr_stale_guide_gap = false; return S_OK; }
        if (previous.last_use_fence_value == 0) {
            // A tracked real source was never read by synthesis. It has no
            // consumer GPU fence to retire against.
            history->cancel_consumer(previous.lease);
            previous.clear();
            fsr_stale_guide_gap = false;
            return S_OK;
        }
        if (fence == nullptr) {
            synthesis_enabled = false;
            return E_UNEXPECTED;
        }
        const HRESULT result = history->retire_consumer(
            previous.lease,
            fence.Get(),
            previous.last_use_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        previous.clear();
        fsr_stale_guide_gap = false;
        return S_OK;
    }


    [[nodiscard]] HRESULT wait_for_idle() noexcept {
        // last_submitted_fence_value is the value a deferred copy will signal,
        // so waiting for it without submitting that copy would never return.
        static_cast<void>(flush_pending_copy());
        HRESULT result = retire_previous();
        if (FAILED(result)) {
            return result;
        }
        result = wait_for_fence(fence.Get(), last_submitted_fence_value);
        if (FAILED(result)) {
            synthesis_enabled = false;
            return result;
        }
        if (backend == D3D12OpticalFlowBackend::nvidia || native_dlss) {
            result = wait_for_fence(
                nvidia_fence.Get(),
                last_nvidia_fence_value);
            if (FAILED(result)) {
                synthesis_enabled = false;
                return result;
            }
        }
        result = history->wait_for_idle();
        if (FAILED(result)) {
            synthesis_enabled = false;
        }
        return result;
    }
};

D3D12FrameSynthesizer::D3D12FrameSynthesizer() noexcept = default;

D3D12FrameSynthesizer::~D3D12FrameSynthesizer() {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr && FAILED(impl_->wait_for_idle())) {
            // Keep every resource and active history lease alive when an
            // executed submission cannot be proven complete.
            static_cast<void>(impl_.release());
        }
    } catch (...) {
        // Destruction must not let synchronization failures escape.
    }
}

HRESULT D3D12FrameSynthesizer::initialize(
    ID3D12Device* device,
    ID3D12CommandQueue* queue,
    std::shared_ptr<D3D12SwapchainHistory> history,
    std::span<ID3D12Resource* const> current_destination_images,
    std::span<ID3D12Resource* const> synthetic_destination_images,
    DXGI_FORMAT view_format,
    D3D12_RESOURCE_STATES release_state,
    D3D12OpticalFlowBackend backend,
    D3D12NvidiaOpticalFlowOptions nvidia_options,
    bool enable_nvidia_gpu_timing,
    D3D12FrameGenerationMode generation_mode,
    bool allow_luke_guide_device_alias,
    bool defer_flow_resources) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        auto candidate = std::make_unique<Impl>();
        // initialize records/allocates but never submits this engine's work.
        // Failure cleanup must not drain unrelated commands on its queue.
        candidate->copy_resources_retired=true;
        const HRESULT result = candidate->initialize(
            device,
            queue,
            std::move(history),
            current_destination_images,
            synthetic_destination_images,
            view_format,
            release_state,
            backend,
            nvidia_options,
            enable_nvidia_gpu_timing,
            generation_mode,
            allow_luke_guide_device_alias, defer_flow_resources);
        if (FAILED(result)) {
            return result;
        }
        candidate->copy_resources_retired=false;
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::reconfigure(D3D12OpticalFlowBackend backend,
    D3D12NvidiaOpticalFlowOptions options,
    D3D12FrameGenerationMode generation_mode) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_) return E_UNEXPECTED;
        return replace_resources_locked(backend,options,generation_mode,
            impl_->flow_resources_deferred,false);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT D3D12FrameSynthesizer::suspend_flow_resources() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if(!impl_)return E_UNEXPECTED;
        if(impl_->generation_mode!=D3D12FrameGenerationMode::ofxr)return S_FALSE;
        if(impl_->flow_resources_deferred)return S_OK;
        return replace_resources_locked(impl_->backend,impl_->nvidia_options,
            impl_->generation_mode,true,true);
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_FAIL; }
}

HRESULT D3D12FrameSynthesizer::replace_resources_locked(D3D12OpticalFlowBackend backend,
    D3D12NvidiaOpticalFlowOptions options,D3D12FrameGenerationMode generation_mode,
    bool defer_flow,bool preserve_completed_tickets) {
        if (!impl_) return E_UNEXPECTED;
        const HRESULT idle = impl_->wait_for_idle();
        if (FAILED(idle)) return idle; // never free pending GPU work
        // Preserve transport ownership, not the old method's working set. In
        // particular FFX/NVIDIA contexts can each be large at headset sizes;
        // constructing the replacement while retaining the drained provider
        // doubles the transient VRAM cost and can make an otherwise valid
        // switch fail. No submitted GPU work survives the idle proof above.
        auto device=impl_->device;
        auto queue=impl_->queue;
        auto history=impl_->history;
        const auto view_format=impl_->view_format;
        const auto release_state=impl_->release_state;
        const auto timing=impl_->requested_gpu_timing;
        const auto allow_alias=impl_->allow_luke_guide_device_alias;
        const auto completed_fence=preserve_completed_tickets?impl_->fence:ComPtr<ID3D12Fence>{};
        const auto next_fence=impl_->next_fence_value;
        const auto submitted_fence=impl_->last_submitted_fence_value;
        const auto synthetic_fence=impl_->last_synthetic_fence_value;
        std::vector<ComPtr<ID3D12Resource>> retained;
        std::vector<ID3D12Resource*> current, synthetic;
        retained.reserve(impl_->current_destinations.size()+impl_->synthetic_destinations.size());
        for (const auto& d : impl_->current_destinations) {
            retained.push_back(d.resource);current.push_back(d.resource.Get());
        }
        for (const auto& d : impl_->synthetic_destinations) {
            retained.push_back(d.resource);synthetic.push_back(d.resource.Get());
        }
        auto candidate = std::make_unique<Impl>();
        candidate->copy_resources_retired=true;
        impl_->copy_resources_retired=true;
        impl_.reset();
        const HRESULT result = candidate->initialize(device.Get(),queue.Get(),
            std::move(history),current,synthetic,view_format,release_state,
            backend,options,timing,generation_mode,allow_alias,defer_flow);
        // A failed replacement remains explicitly uninitialized. The layer
        // reports the error and presents real frames; never retain a different
        // method's resources or advertise that old method as the new one.
        if (FAILED(result)) return result;
        if(preserve_completed_tickets) {
            candidate->fence=completed_fence;
            candidate->next_fence_value=next_fence;
            candidate->last_submitted_fence_value=submitted_fence;
            candidate->last_synthetic_fence_value=synthetic_fence;
        }
        candidate->copy_resources_retired=false;
        impl_ = std::move(candidate);
        return S_OK;
}

// Luke source transport only calls submit_copy; allocating another flow
// engine for that role wastes provider contexts and registered images per eye.
// Upgrade once when OFXR really primes, after old copy fences have retired.
HRESULT D3D12FrameSynthesizer::ensure_flow_resources_locked() {
    if(!impl_)return E_UNEXPECTED;
    if(!impl_->flow_resources_deferred)return S_OK;
    // Only this engine's completed copy resources are being replaced. The
    // incoming history capture can be queued behind a publication gate: neither
    // drain that history nor append a global-queue drain behind that gate.
    const auto available=impl_->submission_available();if(available!=S_OK)return available;
    if(impl_->completion_unknown || impl_->previous.active() || impl_->untracked_source.active())return E_UNEXPECTED;
    std::vector<ID3D12Resource*> current,synthetic;
    for(const auto& d:impl_->current_destinations)current.push_back(d.resource.Get());
    for(const auto& d:impl_->synthetic_destinations)synthetic.push_back(d.resource.Get());
    auto candidate=std::make_unique<Impl>();
    // initialize only allocates/records; no command list has been submitted.
    // An unavailable provider must not drain the caller's gated capture queue
    // while destroying this unused candidate.
    candidate->copy_resources_retired=true;
    const auto result=candidate->initialize(impl_->device.Get(),impl_->queue.Get(),impl_->history,
        current,synthetic,impl_->view_format,impl_->release_state,impl_->backend,impl_->nvidia_options,
        impl_->requested_gpu_timing,impl_->generation_mode,impl_->allow_luke_guide_device_alias,false);
    bridge_flight_logger().event(BridgeFlightOperation::synthesis_initialize,result,0xAF053700ULL,
        static_cast<std::uint64_t>(impl_->backend),impl_->image_description.DepthOrArraySize);
    if(FAILED(result))return result;
    // Retained publication receipts can still refer to completed copy tickets.
    // Keep their fence identity and monotonic values across the lazy upgrade.
    candidate->copy_resources_retired=false;
    candidate->fence=impl_->fence;
    candidate->next_fence_value=impl_->next_fence_value;
    candidate->last_submitted_fence_value=impl_->last_submitted_fence_value;
    candidate->last_synthetic_fence_value=impl_->last_synthetic_fence_value;
    impl_->copy_resources_retired=true; // own fence proved all old copies complete
    impl_=std::move(candidate);return S_OK;
}

HRESULT D3D12FrameSynthesizer::submit_copy(
    const D3D12HistoryCaptureTicket& current,
    std::span<const D3D12ReprojectionView> current_source_views,
    std::uint32_t current_destination_index,
    D3D12FrameSynthesisTicket* ticket,
    bool preserve_temporal_anchor) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (ticket != nullptr) {
                *ticket = {};
            }
            return E_UNEXPECTED;
        }
        return impl_->submit_copy(current, current_source_views,
            current_destination_index, ticket, preserve_temporal_anchor);
    } catch (...) {
        if (ticket != nullptr) {
            *ticket = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::submit_prime(
    const D3D12HistoryCaptureTicket& current,
    std::span<const D3D12ReprojectionView> current_source_views,
    std::uint32_t current_destination_index,
    D3D12FrameSynthesisTicket* ticket,
    std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
    D3D12PresentationTransform presentation_transform) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (ticket != nullptr) {
                *ticket = {};
            }
            return E_UNEXPECTED;
        }
        const auto resources=ensure_flow_resources_locked();
        if(FAILED(resources)){if(ticket)*ticket={};return resources;}
        return impl_->submit_prime(
            current,
            current_source_views,
            current_destination_index,
            ticket,
            std::move(motion_vectors),
            std::move(presentation_transform));
    } catch (...) {
        if (ticket != nullptr) {
            *ticket = {};
        }
        return E_FAIL;
    }
}

bool D3D12FrameSynthesizer::can_pair_fsr(std::shared_ptr<const DlssMotionVectorSet> current) const noexcept {
    std::scoped_lock lock(mutex_);
    if (!impl_ || !impl_->previous.active()) return false;
    Impl::RollingSource next{};
    next.motion_vectors = std::move(current);
    const UINT expected_guide_eyes = std::min<UINT>(
        std::max<UINT>(impl_->image_description.DepthOrArraySize,
                       impl_->previous.view_count),
        kDlssMotionVectorEyeCount);
    if (!impl_->valid_game_motion_pair(
            impl_->previous, next, expected_guide_eyes)) return false;
    for (UINT eye = 0; eye < next.motion_vectors->eye_count; ++eye) {
        const auto& guide = next.motion_vectors->eyes[eye];
        if (!guide->depth || !same_device(impl_->device.Get(), guide->depth.Get()) ||
            !guide->depth_width || !guide->depth_height || !guide->motion_width ||
            !guide->motion_height) return false;
    }
    return true;
}

bool D3D12FrameSynthesizer::has_capture_gap(const D3D12HistoryCaptureTicket& current) const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ && impl_->previous.active() && current.fence_value != 0 &&
        current.serial > impl_->previous.ticket.serial &&
        current.serial - impl_->previous.ticket.serial > 1;
}

HRESULT D3D12FrameSynthesizer::submit_pair(
    const D3D12HistoryCaptureTicket& current,
    std::span<const D3D12ReprojectionView> current_source_views,
    std::span<const D3D12ReprojectionView> synthetic_target_views,
    std::uint32_t synthetic_destination_index,
    std::uint32_t current_destination_index,
    D3D12FrameSynthesisTicket* ticket,
    std::optional<OverlayPlacement> debug_marker,
    std::shared_ptr<const DlssMotionVectorSet> motion_vectors,
    bool defer_current_copy,
    float interpolation_fraction,
    D3D12PresentationTransform presentation_transform,
    std::optional<D3D12ExtraSynthetic> extra_synthetic,
    bool allow_synthetic_camera) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (ticket != nullptr) {
                *ticket = {};
            }
            return E_UNEXPECTED;
        }
        // A deferred copy engine has no temporal anchor: only a real prime
        // may upgrade it. Invalid pair calls must not allocate a flow engine.
        if(impl_->flow_resources_deferred){if(ticket)*ticket={};return E_INVALIDARG;}
        return impl_->submit_pair(
            current,
            current_source_views,
            synthetic_target_views,
            synthetic_destination_index,
            current_destination_index,
            ticket,
            debug_marker,
            std::move(motion_vectors),
            defer_current_copy,
            interpolation_fraction,
            std::move(presentation_transform), extra_synthetic, allow_synthetic_camera);
    } catch (...) {
        if (ticket != nullptr) {
            *ticket = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::flush_current_copy(
    ID3D12CommandQueue* consumer_queue) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            return S_OK;
        }
        const HRESULT result = impl_->flush_pending_copy();
        // Now that the copy is on the queue, the value it will signal is a
        // value something is actually working towards, so a consumer can
        // wait on it without parking.
        if (SUCCEEDED(result) && consumer_queue != nullptr &&
            consumer_queue != impl_->queue.Get() &&
            impl_->fence != nullptr &&
            impl_->last_submitted_fence_value != 0) {
            static_cast<void>(consumer_queue->Wait(
                impl_->fence.Get(),
                impl_->last_submitted_fence_value));
        }
        return result;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::retire_previous() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? S_OK : impl_->retire_previous();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::wait_for_idle() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? S_OK : impl_->wait_for_idle();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::wait_for_previous_submission(
    std::uint32_t timeout_milliseconds) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr
            ? S_OK
            : impl_->wait_for_previous_submission(timeout_milliseconds);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::ticket_available(const D3D12FrameSynthesisTicket& ticket) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        const auto value=ticket.synthetic_fence_value?ticket.synthetic_fence_value:ticket.fence_value;
        if(!impl_ || !impl_->fence || !value || value>impl_->last_submitted_fence_value) return E_INVALIDARG;
        const auto completed=impl_->fence->GetCompletedValue();
        if(completed==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
        return completed>=value?S_OK:S_FALSE;
    } catch(...) { return E_FAIL; }
}

Microsoft::WRL::ComPtr<ID3D12Fence> D3D12FrameSynthesizer::submission_fence(
    const D3D12FrameSynthesisTicket& ticket) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if(!impl_ || !impl_->fence || !ticket.fence_value ||
            ticket.fence_value>impl_->last_submitted_fence_value) return {};
        return impl_->fence;
    } catch(...) { return {}; }
}

HRESULT D3D12FrameSynthesizer::previous_submission_available() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            return S_OK;
        }
        const HRESULT flush_result = impl_->flush_pending_copy();
        return FAILED(flush_result)
            ? flush_result
            : impl_->submission_available();
    } catch (...) {
        return E_FAIL;
    }
}



HRESULT D3D12FrameSynthesizer::consume_nvidia_gpu_timing(
    D3D12NvidiaGpuTiming* timing) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr) {
            if (timing != nullptr) {
                *timing = {};
            }
            return E_UNEXPECTED;
        }
        return impl_->consume_nvidia_gpu_timing(timing);
    } catch (...) {
        if (timing != nullptr) {
            *timing = {};
        }
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::synchronize_producer_queue(
    ID3D12CommandQueue* queue) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || queue == nullptr) {
            return E_INVALIDARG;
        }
        if (impl_->queue == nullptr || impl_->acquire_fence == nullptr) {
            return E_UNEXPECTED;
        }
        // Same queue on both sides means the acquire already applies; the
        // signal and wait would be redundant rather than wrong.
        if (queue == impl_->queue.Get()) {
            return S_FALSE;
        }
        const std::uint64_t value = impl_->next_acquire_fence_value;
        const HRESULT signal_result =
            queue->Signal(impl_->acquire_fence.Get(), value);
        if (FAILED(signal_result)) {
            return signal_result;
        }
        ++impl_->next_acquire_fence_value;
        return impl_->queue->Wait(impl_->acquire_fence.Get(), value);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D12FrameSynthesizer::synchronize_consumer_queue(
    ID3D12CommandQueue* queue,
    const D3D12FrameSynthesisTicket& ticket) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || queue == nullptr) {
            return E_INVALIDARG;
        }
        // The synthetic's value, never the pair's. fence_value is what the
        // deferred current copy will signal, and that copy is not submitted
        // until the presenter flushes it a display period later - so waiting
        // on it here parks the application's own queue for that whole window,
        // every frame, and the game cannot start rendering until it clears.
        const std::uint64_t value = ticket.synthetic_fence_value != 0
            ? ticket.synthetic_fence_value
            : ticket.fence_value;
        if (value == 0 || impl_->fence == nullptr) {
            return E_UNEXPECTED;
        }
        // Same queue on both sides means the submission is already ordered;
        // the wait would be redundant rather than wrong.
        if (queue == impl_->queue.Get()) {
            return S_FALSE;
        }
        return queue->Wait(impl_->fence.Get(), value);
    } catch (...) {
        return E_FAIL;
    }
}

void D3D12FrameSynthesizer::report_pending_submission() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_) return;
        auto& flight = bridge_flight_logger();
        const auto snapshot = [&](int kind, ID3D12Fence* fence, std::uint64_t required) {
            if (fence) flight.event(BridgeFlightOperation::gpu_fence_snapshot, kind,
                reinterpret_cast<std::uintptr_t>(fence), required, fence->GetCompletedValue());
        };
        snapshot(49301, impl_->fence.Get(), impl_->last_submitted_fence_value);
        snapshot(49302, impl_->nvidia_fence.Get(), impl_->last_nvidia_fence_value);
        snapshot(49303, impl_->previous.producer_fence.Get(), impl_->previous.ticket.fence_value);
        snapshot(49304, impl_->acquire_fence.Get(), impl_->next_acquire_fence_value - 1);
        if (impl_->device) flight.event(BridgeFlightOperation::gpu_device_status,
            impl_->device->GetDeviceRemovedReason(), reinterpret_cast<std::uintptr_t>(impl_->queue.Get()),
            reinterpret_cast<std::uintptr_t>(impl_->device.Get()), 49300);
    } catch (...) { }
}

bool D3D12FrameSynthesizer::initialized() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ != nullptr && impl_->synthesis_enabled;
    } catch (...) {
        return false;
    }
}

}  // namespace xrfg
