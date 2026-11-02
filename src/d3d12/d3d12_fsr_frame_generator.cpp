#include "d3d12_fsr_frame_generator.hpp"

#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/fsr_frame_generation.hpp"

#include <windows.h>
#include <wrl/client.h>

#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMaximumEyes = 2;
constexpr D3D12_RESOURCE_STATES kComputeRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES kShaderRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

// Native Stereo stores both eye viewports in one physical XR texture slice.
// Their FSR temporal contexts remain separate even though the copy targets
// share that slice. Array stereo instead owns one context per physical slice.
[[nodiscard]] bool shared_stereo_slice(
    std::span<const D3D12FsrEyeInput> inputs) noexcept {
    if (inputs.size() != 2 ||
        inputs[0].output_slice != inputs[1].output_slice) return false;
    const auto& left = inputs[0].color_rect;
    const auto& right = inputs[1].color_rect;
    return static_cast<std::uint64_t>(left.offset_x) + left.width <= right.offset_x ||
           static_cast<std::uint64_t>(right.offset_x) + right.width <= left.offset_x ||
           static_cast<std::uint64_t>(left.offset_y) + left.height <= right.offset_y ||
           static_cast<std::uint64_t>(right.offset_y) + right.height <= left.offset_y;
}

DXGI_FORMAT linear_color_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return format;
    }
}

D3D12_RESOURCE_BARRIER transition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER value{};
    value.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    value.Transition.pResource = resource;
    value.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    value.Transition.StateBefore = before;
    value.Transition.StateAfter = after;
    return value;
}

D3D12_HEAP_PROPERTIES default_heap() noexcept {
    D3D12_HEAP_PROPERTIES value{};
    value.Type = D3D12_HEAP_TYPE_DEFAULT;
    value.CreationNodeMask = 1;
    value.VisibleNodeMask = 1;
    return value;
}

D3D12_RESOURCE_DESC mono_texture(
    UINT width,
    UINT height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
    D3D12_RESOURCE_DESC value{};
    value.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    value.Width = width;
    value.Height = height;
    value.DepthOrArraySize = 1;
    value.MipLevels = 1;
    value.Format = format;
    value.SampleDesc.Count = 1;
    value.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    value.Flags = flags;
    return value;
}

bool same_shape(const D3D12_RESOURCE_DESC& left,
                const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension && left.Width == right.Width &&
           left.Height == right.Height &&
           left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels && left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout && left.Flags == right.Flags;
}

HRESULT create_texture(ID3D12Device* device,
                       const D3D12_RESOURCE_DESC& description,
                       D3D12_RESOURCE_STATES state,
                       ComPtr<ID3D12Resource>* resource) noexcept {
    if (!device || !resource) return E_POINTER;
    const auto heap = default_heap();
    return device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, state, nullptr,
        IID_PPV_ARGS(resource->ReleaseAndGetAddressOf()));
}

void copy_region(ID3D12GraphicsCommandList* commands,
                 ID3D12Resource* destination,
                 UINT destination_x,
                 UINT destination_y,
                 UINT destination_slice,
                 ID3D12Resource* source,
                 UINT source_slice,
                 const D3D12ImageRect& source_rect) noexcept {
    const D3D12_RESOURCE_DESC destination_description = destination->GetDesc();
    const D3D12_RESOURCE_DESC source_description = source->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = destination;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = destination_slice * destination_description.MipLevels;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = source_slice * source_description.MipLevels;
    const D3D12_BOX box{
        source_rect.offset_x,
        source_rect.offset_y,
        0,
        source_rect.offset_x + source_rect.width,
        source_rect.offset_y + source_rect.height,
        1,
    };
    commands->CopyTextureRegion(
        &to, destination_x, destination_y, 0, &from, &box);
}

std::filesystem::path module_directory() noexcept {
    HMODULE module{};
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_directory), &module)) {
        return {};
    }
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(
        module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    return std::filesystem::path(path.data()).parent_path();
}

Vec3 rotate(Quaternion q, Vec3 v) noexcept {
    q = normalize(q);
    const Vec3 cross_a{
        q.y * v.z - q.z * v.y,
        q.z * v.x - q.x * v.z,
        q.x * v.y - q.y * v.x,
    };
    const Vec3 sum{
        cross_a.x + q.w * v.x,
        cross_a.y + q.w * v.y,
        cross_a.z + q.w * v.z,
    };
    const Vec3 cross_b{
        q.y * sum.z - q.z * sum.y,
        q.z * sum.x - q.x * sum.z,
        q.x * sum.y - q.y * sum.x,
    };
    return {v.x + 2.0F * cross_b.x,
            v.y + 2.0F * cross_b.y,
            v.z + 2.0F * cross_b.z};
}

}  // namespace

bool fsr_generated_array_description(
    const D3D12_RESOURCE_DESC& destination_description,
    std::span<const D3D12FsrEyeInput> eyes,
    D3D12_RESOURCE_DESC* generated_description) noexcept {
    if (generated_description == nullptr || eyes.empty() ||
        eyes.size() > kMaximumEyes ||
        destination_description.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destination_description.DepthOrArraySize == 0) {
        return false;
    }
    D3D12_RESOURCE_DESC description = destination_description;
    description.Width = 1;
    description.Height = 1;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (const auto& input : eyes) {
        if (!input.color || !input.depth || !input.motion_vectors ||
            input.color_rect.width == 0 || input.color_rect.height == 0 ||
            input.depth_rect.width == 0 || input.depth_rect.height == 0 ||
            input.motion_rect.width == 0 || input.motion_rect.height == 0 ||
            input.output_slice >= description.DepthOrArraySize) {
            return false;
        }
        const std::uint64_t right =
            static_cast<std::uint64_t>(input.color_rect.offset_x) +
            input.color_rect.width;
        const std::uint64_t bottom =
            static_cast<std::uint64_t>(input.color_rect.offset_y) +
            input.color_rect.height;
        const auto color_description = input.color->GetDesc();
        if (right > color_description.Width ||
            bottom > color_description.Height ||
            input.color_slice >= color_description.DepthOrArraySize ||
            right > std::numeric_limits<UINT>::max() ||
            bottom > std::numeric_limits<UINT>::max()) {
            return false;
        }
        description.Width = std::max(description.Width, right);
        description.Height = std::max(
            description.Height, static_cast<UINT>(bottom));
    }
    *generated_description = description;
    return true;
}

struct D3D12FsrFrameGenerator::Impl {
    struct Eye {
        ffxContext context{};
        ComPtr<ID3D12Resource> color;
        ComPtr<ID3D12Resource> depth;
        ComPtr<ID3D12Resource> motion;
        ComPtr<ID3D12Resource> output;
        D3D12_RESOURCE_STATES color_state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES depth_state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES motion_state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES output_state{D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        UINT display_width{};
        UINT display_height{};
        UINT render_width{};
        UINT render_height{};
        UINT context_flags{};
        std::uint64_t frame_id{};
    };

    std::array<bool,kMaximumEyes> last_reset{};
    ComPtr<ID3D12Device> device;
    D3D12_RESOURCE_DESC array_description{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    ComPtr<ID3D12Resource> generated_array;
    D3D12_RESOURCE_STATES generated_array_state{D3D12_RESOURCE_STATE_COMMON};
    std::array<Eye, kMaximumEyes> eyes;
    HMODULE module{};
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxConfigure configure{};
    PfnFfxDispatch dispatch{};

    ~Impl() { shutdown(); }

    void destroy_eye(Eye& eye) noexcept {
        if (eye.context && destroy_context) {
            static_cast<void>(destroy_context(&eye.context, nullptr));
        }
        eye = {};
    }

    void shutdown() noexcept {
        for (auto& eye : eyes) destroy_eye(eye);
        generated_array.Reset();
        if (module) FreeLibrary(module);
        module = nullptr;
        create_context = nullptr;
        destroy_context = nullptr;
        configure = nullptr;
        dispatch = nullptr;
    }

    HRESULT load_runtime() noexcept {
        if (module && create_context && destroy_context && configure && dispatch)
            return S_OK;
        const auto directory = module_directory();
        if (directory.empty()) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        const auto isolated =
            directory / L"ofxr_amd_fidelityfx_framegeneration_dx12.dll";
        module = LoadLibraryW(isolated.c_str());
        if (!module) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::runtime_missing);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        create_context = reinterpret_cast<PfnFfxCreateContext>(
            GetProcAddress(module, "ffxCreateContext"));
        destroy_context = reinterpret_cast<PfnFfxDestroyContext>(
            GetProcAddress(module, "ffxDestroyContext"));
        configure = reinterpret_cast<PfnFfxConfigure>(
            GetProcAddress(module, "ffxConfigure"));
        dispatch = reinterpret_cast<PfnFfxDispatch>(
            GetProcAddress(module, "ffxDispatch"));
        if (!create_context || !destroy_context || !configure || !dispatch) {
            shutdown();
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::runtime_missing);
            return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        }
        return S_OK;
    }

    HRESULT ensure_array(std::span<const D3D12FsrEyeInput> inputs) noexcept {
        D3D12_RESOURCE_DESC description{};
        if (!fsr_generated_array_description(
                array_description, inputs, &description)) return E_INVALIDARG;
        // Keep the generated image in the FSR input colour domain. Cinema
        // colour is the final XR image pulled back to the original guide plane;
        // it is display-encoded before FSR and decoded by the final panel draw.
        description.Format = color_format;
        // The copy target is later sampled in either the XR sRGB domain or
        // the raw cinema domain. Typeless 8-bit storage permits both legal
        // views without changing the FSR context/UAV format or the pixels.
        if (color_format == DXGI_FORMAT_R8G8B8A8_UNORM)
            description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        else if (color_format == DXGI_FORMAT_B8G8R8A8_UNORM)
            description.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (generated_array &&
            same_shape(generated_array->GetDesc(), description)) return S_OK;
        generated_array.Reset();
        generated_array_state = D3D12_RESOURCE_STATE_COMMON;
        return create_texture(device.Get(), description,
                              generated_array_state, &generated_array);
    }

    [[nodiscard]] static UINT context_flags(
        const D3D12FsrEyeInput& input) noexcept {
        UINT flags = 0;
        if (input.motion_rect.width == input.color_rect.width &&
            input.motion_rect.height == input.color_rect.height)
            flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        if (input.motion_vectors_jittered)
            flags |= FFX_FRAMEGENERATION_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
        if (input.depth_inverted)
            flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
        if (input.depth_infinite)
            flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
        return flags;
    }

    [[nodiscard]] static bool needs_recreate(
        const Eye& eye, const D3D12FsrEyeInput& input) noexcept {
        return !eye.context || eye.display_width != input.color_rect.width ||
            eye.display_height != input.color_rect.height ||
            eye.render_width != input.depth_rect.width ||
            eye.render_height != input.depth_rect.height ||
            eye.context_flags != context_flags(input);
    }

    HRESULT ensure_eye(
        Eye& eye,
        const D3D12FsrEyeInput& input,
        std::size_t eye_index) noexcept {
        const UINT display_width = input.color_rect.width;
        const UINT display_height = input.color_rect.height;
        const UINT render_width = input.depth_rect.width;
        const UINT render_height = input.depth_rect.height;
        const UINT flags = context_flags(input);
        if (!needs_recreate(eye, input)) return S_OK;
        bridge_flight_logger().event(
            BridgeFlightOperation::fsr_context_lifecycle,
            static_cast<std::int64_t>(eye_index),
            (static_cast<std::uint64_t>(display_width) << 32U) | display_height,
            (static_cast<std::uint64_t>(render_width) << 32U) | render_height,
            (static_cast<std::uint64_t>(eye.context_flags) << 32U) | flags);
        destroy_eye(eye);

        HRESULT result = create_texture(device.Get(),
            mono_texture(display_width, display_height, color_format),
            D3D12_RESOURCE_STATE_COMMON, &eye.color);
        if (FAILED(result)) return result;
        result = create_texture(device.Get(),
            mono_texture(render_width, render_height,
                         input.depth->GetDesc().Format),
            D3D12_RESOURCE_STATE_COMMON, &eye.depth);
        if (FAILED(result)) return result;
        result = create_texture(device.Get(),
            mono_texture(input.motion_rect.width, input.motion_rect.height,
                         input.motion_vectors->GetDesc().Format),
            D3D12_RESOURCE_STATE_COMMON, &eye.motion);
        if (FAILED(result)) return result;
        result = create_texture(device.Get(),
            mono_texture(display_width, display_height, color_format,
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &eye.output);
        if (FAILED(result)) return result;

        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device.Get();
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = flags;
        create.displaySize = {display_width, display_height};
        create.maxRenderSize = {render_width, render_height};
        create.backBufferFormat = ffxApiGetSurfaceFormatDX12(color_format);
        const auto code = create_context(&eye.context, &create.header, nullptr);
        if (code != FFX_API_RETURN_OK) {
            destroy_eye(eye);
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::initialization_failed);
            return E_FAIL;
        }
        eye.display_width = display_width;
        eye.display_height = display_height;
        eye.render_width = render_width;
        eye.render_height = render_height;
        eye.context_flags = flags;
        eye.color_state = D3D12_RESOURCE_STATE_COMMON;
        eye.depth_state = D3D12_RESOURCE_STATE_COMMON;
        eye.motion_state = D3D12_RESOURCE_STATE_COMMON;
        eye.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return S_OK;
    }

    HRESULT select_color_domain(
        std::span<const D3D12FsrEyeInput> inputs) noexcept {
        DXGI_FORMAT selected = DXGI_FORMAT_UNKNOWN;
        for (const auto& input : inputs) {
            if (!input.color) return E_INVALIDARG;
            const DXGI_FORMAT candidate =
                linear_color_format(input.color->GetDesc().Format);
            if (candidate == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
            if (selected == DXGI_FORMAT_UNKNOWN) {
                selected = candidate;
            } else if (selected != candidate) {
                // One Texture2DArray cannot contain differently typed eyes.
                return E_INVALIDARG;
            }
        }
        if (selected == color_format) return S_OK;
        for (auto& eye : eyes) destroy_eye(eye);
        generated_array.Reset();
        generated_array_state = D3D12_RESOURCE_STATE_COMMON;
        color_format = selected;
        return S_OK;
    }

    HRESULT copy_inputs(ID3D12GraphicsCommandList* commands,
                        Eye& eye,
                        const D3D12FsrEyeInput& input) noexcept {
        std::array<D3D12_RESOURCE_BARRIER, 6> before{
            transition(input.color, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(eye.color.Get(), eye.color_state,
                       D3D12_RESOURCE_STATE_COPY_DEST),
            transition(input.depth, input.depth_state,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(eye.depth.Get(), eye.depth_state,
                       D3D12_RESOURCE_STATE_COPY_DEST),
            transition(input.motion_vectors, input.motion_state,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(eye.motion.Get(), eye.motion_state,
                       D3D12_RESOURCE_STATE_COPY_DEST),
        };
        commands->ResourceBarrier(static_cast<UINT>(before.size()), before.data());
        copy_region(commands, eye.color.Get(), 0, 0, 0, input.color,
                    input.color_slice, input.color_rect);
        copy_region(commands, eye.depth.Get(), 0, 0, 0, input.depth, 0,
                    input.depth_rect);
        copy_region(commands, eye.motion.Get(), 0, 0, 0,
                    input.motion_vectors, input.motion_slice, input.motion_rect);
        std::array<D3D12_RESOURCE_BARRIER, 6> after{
            transition(input.color, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_COMMON),
            transition(eye.color.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       kComputeRead),
            transition(input.depth, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       input.depth_state),
            transition(eye.depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       kComputeRead),
            transition(input.motion_vectors, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       input.motion_state),
            transition(eye.motion.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                       kComputeRead),
        };
        commands->ResourceBarrier(static_cast<UINT>(after.size()), after.data());
        eye.color_state = kComputeRead;
        eye.depth_state = kComputeRead;
        eye.motion_state = kComputeRead;
        return S_OK;
    }

    HRESULT generate_eye(ID3D12GraphicsCommandList* commands,
                         Eye& eye,
                         const D3D12FsrEyeInput& input,
                         std::size_t eye_index) noexcept {
        HRESULT result = ensure_eye(eye, input, eye_index);
        if (FAILED(result)) return result;
        result = copy_inputs(commands, eye, input);
        if (FAILED(result)) return result;

        const std::uint64_t frame_id = ++eye.frame_id;
        ffxConfigureDescFrameGeneration configure_description{};
        configure_description.header.type =
            FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        configure_description.frameGenerationEnabled = true;
        configure_description.allowAsyncWorkloads = false;
        configure_description.flags =
            FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        configure_description.generationRect = {
            0, 0, static_cast<int32_t>(eye.display_width),
            static_cast<int32_t>(eye.display_height)};
        configure_description.frameID = frame_id;
        if (configure(&eye.context, &configure_description.header) !=
            FFX_API_RETURN_OK) return E_FAIL;

        const Vec3 position = input.camera_data_valid
            ? input.camera_position
            : input.camera_pose.position;
        const Vec3 up = input.camera_data_valid
            ? input.camera_up
            : rotate(input.camera_pose.orientation, {0.0F, 1.0F, 0.0F});
        const Vec3 right = input.camera_data_valid
            ? input.camera_right
            : rotate(input.camera_pose.orientation, {1.0F, 0.0F, 0.0F});
        const Vec3 forward = input.camera_data_valid
            ? input.camera_forward
            : rotate(input.camera_pose.orientation, {0.0F, 0.0F, -1.0F});
        ffxDispatchDescFrameGenerationPrepareCameraInfo camera{};
        camera.header.type =
            FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_CAMERAINFO;
        camera.cameraPosition[0] = position.x;
        camera.cameraPosition[1] = position.y;
        camera.cameraPosition[2] = position.z;
        camera.cameraUp[0] = up.x;
        camera.cameraUp[1] = up.y;
        camera.cameraUp[2] = up.z;
        camera.cameraRight[0] = right.x;
        camera.cameraRight[1] = right.y;
        camera.cameraRight[2] = right.z;
        camera.cameraForward[0] = forward.x;
        camera.cameraForward[1] = forward.y;
        camera.cameraForward[2] = forward.z;
        ffxCreateBackendDX12Desc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device.Get();
        camera.header.pNext = &backend.header;
        ffxDispatchDescFrameGenerationPrepare prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
        prepare.header.pNext = &camera.header;
        prepare.commandList = commands;
        prepare.frameID = frame_id;
        prepare.renderSize = {eye.render_width, eye.render_height};
        prepare.jitterOffset = {input.jitter_x, input.jitter_y};
        prepare.motionVectorScale = {
            input.motion_scale_x, input.motion_scale_y};
        prepare.frameTimeDelta = std::max(0.1F, input.frame_time_delta_ms);
        prepare.cameraNear = input.camera_near;
        prepare.cameraFar = input.camera_far;
        prepare.cameraFovAngleVertical = input.camera_fov_vertical;
        prepare.viewSpaceToMetersFactor = 1.0F;
        prepare.depth = ffxApiGetResourceDX12(
            eye.depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prepare.motionVectors = ffxApiGetResourceDX12(
            eye.motion.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        if (dispatch(&eye.context, &prepare.header) != FFX_API_RETURN_OK)
            return E_FAIL;

        ffxDispatchDescFrameGeneration generation{};
        generation.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        generation.commandList = commands;
        generation.presentColor = ffxApiGetResourceDX12(
            eye.color.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        generation.outputs[0] = ffxApiGetResourceDX12(
            eye.output.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        generation.numGeneratedFrames = 1;
        generation.reset = input.reset || frame_id == 1;
        generation.backbufferTransferFunction =
            FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        generation.minMaxLuminance[0] = 0.0F;
        generation.minMaxLuminance[1] = 1.0F;
        generation.generationRect = configure_description.generationRect;
        generation.frameID = frame_id;
        if (dispatch(&eye.context, &generation.header) != FFX_API_RETURN_OK)
            return E_FAIL;
        eye.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return S_OK;
    }
};

D3D12FsrFrameGenerator::D3D12FsrFrameGenerator() noexcept = default;
D3D12FsrFrameGenerator::~D3D12FsrFrameGenerator() = default;

HRESULT D3D12FsrFrameGenerator::initialize(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& destination_description,
    DXGI_FORMAT view_format) noexcept {
    if (!device || destination_description.Dimension !=
            D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        destination_description.DepthOrArraySize == 0 ||
        destination_description.DepthOrArraySize > kMaximumEyes ||
        view_format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;
    try {
        auto implementation = std::make_unique<Impl>();
        implementation->device = device;
        implementation->array_description = destination_description;
        implementation->array_description.Flags = D3D12_RESOURCE_FLAG_NONE;
        implementation->color_format = linear_color_format(view_format);
        impl_ = std::move(implementation);
        report_fsr_frame_generation_status(
            FsrFrameGenerationStatus::waiting_for_guides);
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT D3D12FsrFrameGenerator::dispatch(
    ID3D12GraphicsCommandList* commands,
    std::span<const D3D12FsrEyeInput> inputs,
    ID3D12Resource** generated_array) noexcept {
    if (!impl_ || !commands || !generated_array || inputs.empty() ||
        inputs.size() > kMaximumEyes) return E_INVALIDARG;
    *generated_array = nullptr;
    HRESULT result = impl_->load_runtime();
    if (FAILED(result)) {
        report_fsr_frame_generation_attempt(false);
        return result;
    }
    result = impl_->select_color_domain(inputs);
    if (FAILED(result)) {
        report_fsr_frame_generation_status(
            FsrFrameGenerationStatus::waiting_for_guides);
        report_fsr_frame_generation_attempt(false);
        return result;
    }
    result = impl_->ensure_array(inputs);
    if (FAILED(result)) {
        report_fsr_frame_generation_status(
            FsrFrameGenerationStatus::initialization_failed);
        report_fsr_frame_generation_attempt(false);
        return result;
    }
    if (impl_->generated_array_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        const auto barrier = transition(impl_->generated_array.Get(),
            impl_->generated_array_state, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->ResourceBarrier(1, &barrier);
        impl_->generated_array_state = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    const bool shared_slice = shared_stereo_slice(inputs);
    for (std::size_t eye_index = 0; eye_index < inputs.size(); ++eye_index) {
        const auto& input = inputs[eye_index];
        if (!input.color || !input.depth || !input.motion_vectors ||
            input.color_rect.width == 0 || input.color_rect.height == 0 ||
            input.depth_rect.width == 0 || input.depth_rect.height == 0 ||
            input.motion_rect.width == 0 || input.motion_rect.height == 0) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            report_fsr_frame_generation_attempt(false);
            return E_INVALIDARG;
        }
        const auto generated_description = impl_->generated_array->GetDesc();
        if (static_cast<std::uint64_t>(input.color_rect.offset_x) +
                    input.color_rect.width > generated_description.Width ||
            static_cast<std::uint64_t>(input.color_rect.offset_y) +
                    input.color_rect.height > generated_description.Height ||
            input.output_slice >= generated_description.DepthOrArraySize ||
            input.output_slice >= kMaximumEyes) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            report_fsr_frame_generation_attempt(false);
            return E_INVALIDARG;
        }
        if (eye_index != 0 &&
            inputs[eye_index - 1].output_slice == input.output_slice &&
            !shared_slice) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::waiting_for_guides);
            report_fsr_frame_generation_attempt(false);
            return E_INVALIDARG;
        }

        const auto context_index = shared_slice ? eye_index : input.output_slice;
        auto& eye = impl_->eyes[context_index];
        result = impl_->generate_eye(commands, eye, input, context_index);
        if (FAILED(result)) {
            report_fsr_frame_generation_status(
                FsrFrameGenerationStatus::dispatch_failed);
            report_fsr_frame_generation_attempt(false);
            return result;
        }
        impl_->last_reset[eye_index]=input.reset || eye.frame_id==1;
        const auto before = transition(eye.output.Get(), eye.output_state,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1, &before);
        copy_region(commands, impl_->generated_array.Get(),
                    input.color_rect.offset_x, input.color_rect.offset_y,
                    input.output_slice, eye.output.Get(), 0,
                    {0, 0, input.color_rect.width, input.color_rect.height});
        const auto after = transition(eye.output.Get(),
                                      D3D12_RESOURCE_STATE_COPY_SOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->ResourceBarrier(1, &after);
        eye.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    const auto ready = transition(impl_->generated_array.Get(),
                                  D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
    commands->ResourceBarrier(1, &ready);
    impl_->generated_array_state = kShaderRead;
    *generated_array = impl_->generated_array.Get();
    report_fsr_frame_generation_attempt(true);
    return S_OK;
}

bool D3D12FsrFrameGenerator::requires_idle_reconfigure(
    std::span<const D3D12FsrEyeInput> inputs) const noexcept {
    if (!impl_ || inputs.empty() || inputs.size() > kMaximumEyes) return false;
    const bool shared_slice = shared_stereo_slice(inputs);
    D3D12_RESOURCE_DESC required_array{};
    if (!fsr_generated_array_description(
            impl_->array_description, inputs, &required_array)) {
        return false;
    }
    if (impl_->generated_array &&
        (impl_->generated_array->GetDesc().Width != required_array.Width ||
         impl_->generated_array->GetDesc().Height != required_array.Height ||
         impl_->generated_array->GetDesc().DepthOrArraySize !=
             required_array.DepthOrArraySize)) {
        return true;
    }
    for (std::size_t eye_index = 0; eye_index < inputs.size(); ++eye_index) {
        const auto context_index = shared_slice
            ? eye_index : inputs[eye_index].output_slice;
        if (context_index >= kMaximumEyes) return false;
        if (Impl::needs_recreate(impl_->eyes[context_index], inputs[eye_index]))
            return true;
    }
    return false;
}

bool D3D12FsrFrameGenerator::output_is_current(unsigned view) const noexcept {
    return !impl_ || view>=kMaximumEyes || impl_->last_reset[view];
}

void D3D12FsrFrameGenerator::reset() noexcept {
    if (!impl_) return;
    for (auto& eye : impl_->eyes) impl_->destroy_eye(eye);
    report_fsr_frame_generation_status(FsrFrameGenerationStatus::disabled);
}

}  // namespace xrfg
