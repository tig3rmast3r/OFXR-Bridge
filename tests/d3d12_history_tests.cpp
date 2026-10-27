#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d11_dlss_guides.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_origin_continuity.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "xrfg/d3d12_native_dlssg.hpp"
// Private test session; no keyboard input or shipping test control.
#include "../src/common/two_frame_capture.cpp"
#include "xrfg/d3d12_guide_composer.hpp"
#include <filesystem>
#include <fstream>
#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "../src/d3d12/d3d12_fsr_frame_generator.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#ifdef XRFG_NATIVE_DLSSG
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 7;
constexpr UINT kHeight = 3;
constexpr UINT kEyeCount = 2;
constexpr UINT kBytesPerPixel = 4;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

using StereoPattern = std::array<std::vector<std::uint8_t>, kEyeCount>;
using RgbaBytes = std::array<std::uint8_t, kBytesPerPixel>;
using ReprojectionViews =
    std::array<xrfg::D3D12ReprojectionView, kEyeCount>;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void require_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        fail(std::string(operation) + " failed with HRESULT " +
             std::to_string(static_cast<std::int32_t>(result)));
    }
}

[[nodiscard]] bool operation_succeeded(HRESULT result) noexcept {
    return SUCCEEDED(result);
}

void require_frame_start_gate(
    xrfg::D3D12FrameSynthesizer& synthesizer,
    const std::string& label) {
    const HRESULT result = synthesizer.wait_for_previous_submission(500);
    require(
        operation_succeeded(result),
        label + " failed with HRESULT " +
            std::to_string(static_cast<std::int32_t>(result)));
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
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

[[nodiscard]] bool same_description(
    const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right) noexcept {
    return left.Dimension == right.Dimension &&
           left.Alignment == right.Alignment &&
           left.Width == right.Width &&
           left.Height == right.Height &&
           left.DepthOrArraySize == right.DepthOrArraySize &&
           left.MipLevels == right.MipLevels &&
           left.Format == right.Format &&
           left.SampleDesc.Count == right.SampleDesc.Count &&
           left.SampleDesc.Quality == right.SampleDesc.Quality &&
           left.Layout == right.Layout &&
           left.Flags == right.Flags;
}

class D3D12WarpFixture final {
public:
    explicit D3D12WarpFixture(bool use_nvidia_hardware = false) {
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug_.GetAddressOf())))) {
            debug_->EnableDebugLayer();
            debug_enabled_ = true;
        }

        UINT factory_flags = debug_enabled_ ? DXGI_CREATE_FACTORY_DEBUG : 0;
        HRESULT factory_result =
            CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.GetAddressOf()));
        if (FAILED(factory_result) && factory_flags != 0) {
            factory_.Reset();
            factory_flags = 0;
            factory_result =
                CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(factory_.GetAddressOf()));
        }
        require_hresult(factory_result, "CreateDXGIFactory2");

        if (use_nvidia_hardware) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT enumeration = factory_->EnumAdapterByGpuPreference(
                    index,
                    DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(candidate.GetAddressOf()));
                if (enumeration == DXGI_ERROR_NOT_FOUND) {
                    break;
                }
                require_hresult(
                    enumeration,
                    "IDXGIFactory6::EnumAdapterByGpuPreference");
                DXGI_ADAPTER_DESC1 description{};
                require_hresult(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
                if (description.VendorId == 0x10deU &&
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                    adapter_ = candidate;
                    break;
                }
            }
            require(adapter_ != nullptr, "no NVIDIA hardware adapter found");
        } else {
            require_hresult(
                factory_->EnumWarpAdapter(IID_PPV_ARGS(adapter_.GetAddressOf())),
                "IDXGIFactory::EnumWarpAdapter");
        }
        require_hresult(
            D3D12CreateDevice(
                adapter_.Get(),
                use_nvidia_hardware
                    ? D3D_FEATURE_LEVEL_12_0
                    : D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(device_.GetAddressOf())),
            use_nvidia_hardware
                ? "D3D12CreateDevice(NVIDIA)"
                : "D3D12CreateDevice(WARP)");

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queue_description.NodeMask = 0;
        require_hresult(
            device_->CreateCommandQueue(
                &queue_description,
                IID_PPV_ARGS(queue_.GetAddressOf())),
            "ID3D12Device::CreateCommandQueue");
        require_hresult(
            device_->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(fence_.GetAddressOf())),
            "ID3D12Device::CreateFence");

        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(fence_event_ != nullptr, "CreateEventW failed");

        if (debug_enabled_) {
            (void)device_.As(&info_queue_);
            if (info_queue_ != nullptr) {
                info_queue_->ClearStoredMessages();
            }
        }
    }

    ~D3D12WarpFixture() {
        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
        }
    }

    D3D12WarpFixture(const D3D12WarpFixture&) = delete;
    D3D12WarpFixture& operator=(const D3D12WarpFixture&) = delete;

    [[nodiscard]] ID3D12Device* device() const noexcept {
        return device_.Get();
    }

    [[nodiscard]] ID3D12CommandQueue* queue() const noexcept {
        return queue_.Get();
    }

    [[nodiscard]] ID3D12Fence* completion_fence() const noexcept { return fence_.Get(); }
    [[nodiscard]] std::uint64_t last_submitted_value() const noexcept { return next_fence_value_ - 1; }

    template <typename Recorder>
    void execute_and_wait(Recorder&& recorder,bool report_submission=true) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> command_list;
        require_hresult(
            device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(allocator.GetAddressOf())),
            "ID3D12Device::CreateCommandAllocator");
        require_hresult(
            device_->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(),
                nullptr,
                IID_PPV_ARGS(command_list.GetAddressOf())),
            "ID3D12Device::CreateCommandList");

        recorder(command_list.Get());
        require_hresult(command_list->Close(), "ID3D12GraphicsCommandList::Close");
        ID3D12CommandList* command_lists[] = {command_list.Get()};
        queue_->ExecuteCommandLists(1, command_lists);
        if(report_submission)xrfg::report_dlss_motion_vector_submission(queue_.Get(),1,command_lists);

        const std::uint64_t fence_value = next_fence_value_++;
        require_hresult(queue_->Signal(fence_.Get(), fence_value), "ID3D12CommandQueue::Signal");
        wait_for_fence(fence_.Get(), fence_value);
    }

    void wait_for_fence(ID3D12Fence* fence, std::uint64_t value) {
        require(fence != nullptr, "cannot wait on a null fence");
        const std::uint64_t completed = fence->GetCompletedValue();
        require(
            completed != std::numeric_limits<std::uint64_t>::max(),
            "D3D12 fence reports device removal");
        if (completed >= value) {
            return;
        }

        require_hresult(
            fence->SetEventOnCompletion(value, fence_event_),
            "ID3D12Fence::SetEventOnCompletion");
        require(
            WaitForSingleObject(fence_event_, 10'000) == WAIT_OBJECT_0,
            "timed out waiting for WARP command completion");
        require(
            fence->GetCompletedValue() >= value,
            "D3D12 fence event fired before the requested value completed");
    }

    void require_no_debug_errors() const {
        if (!debug_enabled_ || info_queue_ == nullptr) {
            return;
        }

        const std::uint64_t message_count =
            info_queue_->GetNumStoredMessagesAllowedByRetrievalFilter();
        for (std::uint64_t index = 0; index < message_count; ++index) {
            SIZE_T message_size = 0;
            require_hresult(
                info_queue_->GetMessage(index, nullptr, &message_size),
                "ID3D12InfoQueue::GetMessage(size)");
            std::vector<std::byte> storage(message_size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            require_hresult(
                info_queue_->GetMessage(index, message, &message_size),
                "ID3D12InfoQueue::GetMessage");
            if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
                fail(std::string("D3D12 debug-layer error: ") +
                     (message->pDescription == nullptr ? "<no description>"
                                                       : message->pDescription));
            }
        }
    }

private:
    ComPtr<ID3D12Debug> debug_;
    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    HANDLE fence_event_{};
    std::uint64_t next_fence_value_{1};
    bool debug_enabled_{};
};

[[nodiscard]] D3D12_RESOURCE_DESC stereo_texture_description(
    UINT width = kWidth,
    UINT height = kHeight) noexcept {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Alignment = 0;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = static_cast<UINT16>(kEyeCount);
    description.MipLevels = 1;
    description.Format = kFormat;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return description;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_source_texture(
    D3D12WarpFixture& fixture,
    UINT width = kWidth,
    UINT height = kHeight,
    DXGI_FORMAT format = kFormat) {
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.Format = format;
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(source texture)");
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_buffer(
    D3D12WarpFixture& fixture,
    D3D12_HEAP_TYPE heap_type,
    std::uint64_t size,
    D3D12_RESOURCE_STATES initial_state);

[[nodiscard]] ComPtr<ID3D12Resource> create_single_slice_texture(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.DepthOrArraySize = 1;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(single-slice texture)");
    return texture;
}

void test_fsr_generated_array_follows_color_extent(
    D3D12WarpFixture& fixture) {
    auto color = create_single_slice_texture(fixture, 11, 10);
    auto depth = create_single_slice_texture(fixture, 6, 5);
    auto motion = create_single_slice_texture(fixture, 6, 5);
    xrfg::D3D12FsrEyeInput input{};
    input.color = color.Get();
    input.color_rect = {0, 0, 11, 10};
    input.depth = depth.Get();
    input.depth_rect = {0, 0, 6, 5};
    input.motion_vectors = motion.Get();
    input.motion_rect = {0, 0, 6, 5};

    D3D12_RESOURCE_DESC destination = color->GetDesc();
    destination.Width = 9;
    destination.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC generated{};
    require(
        xrfg::fsr_generated_array_description(
            destination, std::span<const xrfg::D3D12FsrEyeInput>(&input, 1),
            &generated),
        "FSR generated-array description rejected a valid larger colour");
    require(
        generated.Width == 11 && generated.Height == 10,
        "FSR generated array retained the smaller OpenXR destination extent");
    require(
        generated.DepthOrArraySize == destination.DepthOrArraySize &&
            generated.Format == destination.Format &&
            generated.Flags == D3D12_RESOURCE_FLAG_NONE,
        "FSR generated array changed its slice, format, or flags contract");
}

void clear_double_wide_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    UINT half_width,
    UINT height) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(double-wide RTV)");

    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = kFormat;
    rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_description.Texture2DArray.MipSlice = 0;
    rtv_description.Texture2DArray.FirstArraySlice = 0;
    rtv_description.Texture2DArray.ArraySize = 1;
    fixture.device()->CreateRenderTargetView(
        texture,
        &rtv_description,
        rtv_heap->GetCPUDescriptorHandleForHeapStart());

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        constexpr FLOAT kLeftColor[4] = {1.0F, 0.0F, 0.0F, 1.0F};
        constexpr FLOAT kRightColor[4] = {0.0F, 0.0F, 1.0F, 1.0F};
        const D3D12_RECT left_rect{
            0, 0, static_cast<LONG>(half_width), static_cast<LONG>(height)};
        const D3D12_RECT right_rect{
            static_cast<LONG>(half_width),
            0,
            static_cast<LONG>(half_width * 2U),
            static_cast<LONG>(height)};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtv_heap->GetCPUDescriptorHandleForHeapStart();
        command_list->ClearRenderTargetView(rtv, kLeftColor, 1, &left_rect);
        command_list->ClearRenderTargetView(rtv, kRightColor, 1, &right_rect);
    });
}

[[nodiscard]] std::vector<std::uint8_t> readback_single_slice(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES initial_state) {
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    require(
        description.DepthOrArraySize == 1 &&
            description.Width <= std::numeric_limits<UINT>::max(),
        "single-slice readback texture shape is unsupported");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT row_count = 0;
    UINT64 row_size = 0;
    UINT64 total_size = 0;
    fixture.device()->GetCopyableFootprints(
        &description,
        0,
        1,
        0,
        &layout,
        &row_count,
        &row_size,
        &total_size);
    require(total_size != 0, "single-slice readback layout is empty");
    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy = transition_barrier(
            texture, initial_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        const D3D12_RESOURCE_BARRIER restore = transition_barrier(
            texture, D3D12_RESOURCE_STATE_COPY_SOURCE, initial_state);
        command_list->ResourceBarrier(1, &restore);
    });

    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    require(
        row_count == height && row_size == width * kBytesPerPixel,
        "single-slice readback layout is unexpected");
    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(width) * height * kBytesPerPixel);
    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped),
        "ID3D12Resource::Map(single-slice readback)");
    const auto* bytes = static_cast<const std::byte*>(mapped);
    for (UINT y = 0; y < height; ++y) {
        std::memcpy(
            output.data() + static_cast<std::size_t>(y) * width * kBytesPerPixel,
            bytes + layout.Offset +
                static_cast<std::size_t>(y) * layout.Footprint.RowPitch,
            static_cast<std::size_t>(row_size));
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return output;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_depth_source_texture(
    D3D12WarpFixture& fixture,
    D3D12_RESOURCE_FLAGS additional_flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description = stereo_texture_description();
    description.Format = kDepthFormat;
    description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | additional_flags);

    D3D12_CLEAR_VALUE clear_value{};
    clear_value.Format = kDepthFormat;
    clear_value.DepthStencil.Depth = 1.0F;
    clear_value.DepthStencil.Stencil = 0;

    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear_value,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(depth source texture)");
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_buffer(
    D3D12WarpFixture& fixture,
    D3D12_HEAP_TYPE heap_type,
    std::uint64_t size,
    D3D12_RESOURCE_STATES initial_state) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = size;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;

    const D3D12_HEAP_PROPERTIES properties = heap_properties(heap_type);
    ComPtr<ID3D12Resource> buffer;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            initial_state,
            nullptr,
            IID_PPV_ARGS(buffer.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(buffer)");
    return buffer;
}

[[nodiscard]] StereoPattern make_pattern(std::uint8_t seed) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (UINT y = 0; y < kHeight; ++y) {
            for (UINT x = 0; x < kWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kWidth + x) * kBytesPerPixel;
                bytes[offset + 0] = static_cast<std::uint8_t>(seed + eye * 37U + x * 11U + y * 3U);
                bytes[offset + 1] = static_cast<std::uint8_t>(seed + eye * 19U + x * 5U + y * 17U);
                bytes[offset + 2] = static_cast<std::uint8_t>(seed + eye * 53U + x * 7U + y * 13U);
                bytes[offset + 3] = static_cast<std::uint8_t>(255U - eye * 7U - x - y);
            }
        }
    }
    return pattern;
}

[[nodiscard]] ReprojectionViews make_reprojection_views(float yaw_radians = 0.0F) {
    ReprojectionViews views{};
    const float half_yaw = yaw_radians * 0.5F;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].pose.orientation.y = std::sin(half_yaw);
        views[eye].pose.orientation.w = std::cos(half_yaw);
        views[eye].pose.position.x = eye == 0 ? -0.032F : 0.032F;
        views[eye].fov.angle_left = eye == 0 ? -0.7853981633974483F
                                             : -0.7504915783575616F;
        views[eye].fov.angle_right = eye == 0 ? 0.7504915783575616F
                                              : 0.7853981633974483F;
        views[eye].fov.angle_up = eye == 0 ? 0.6108652381980153F
                                           : 0.5934119456780721F;
        views[eye].fov.angle_down = eye == 0 ? -0.5934119456780721F
                                             : -0.6108652381980153F;
    }
    return views;
}

[[nodiscard]] StereoPattern make_solid_pattern(
    const std::array<RgbaBytes, kEyeCount>& colors) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
        for (std::size_t offset = 0; offset < bytes.size(); offset += kBytesPerPixel) {
            std::copy(colors[eye].begin(), colors[eye].end(), bytes.begin() + offset);
        }
    }
    return pattern;
}

[[nodiscard]] StereoPattern midpoint_pattern(
    const StereoPattern& previous,
    const StereoPattern& current) {
    StereoPattern midpoint;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(
            previous[eye].size() == current[eye].size(),
            "cannot compute a midpoint for differently sized stereo patterns");
        midpoint[eye].resize(previous[eye].size());
        for (std::size_t index = 0; index < previous[eye].size(); ++index) {
            midpoint[eye][index] = static_cast<std::uint8_t>(
                (static_cast<std::uint16_t>(previous[eye][index]) +
                 static_cast<std::uint16_t>(current[eye][index])) /
                2U);
        }
    }
    return midpoint;
}

[[nodiscard]] RgbaBytes motion_texel(UINT x, UINT y, UINT eye) noexcept {
    const float xf = static_cast<float>(x);
    const float yf = static_cast<float>(y);
    const float eye_phase = static_cast<float>(eye) * 0.7F;
    const auto channel = [](float value) noexcept {
        return static_cast<std::uint8_t>(std::lround(
            std::clamp(value, 0.0F, 255.0F)));
    };
    return {
        channel(128.0F + 82.0F * std::sin(xf * 0.061F + eye_phase) +
                31.0F * std::cos(yf * 0.093F)),
        channel(126.0F + 76.0F * std::sin((xf + yf) * 0.047F + eye_phase) +
                34.0F * std::cos((xf - yf) * 0.037F)),
        channel(124.0F + 69.0F * std::cos(xf * 0.041F - eye_phase) +
                41.0F * std::sin(yf * 0.071F)),
        255,
    };
}

[[nodiscard]] StereoPattern translated_motion_pattern(
    UINT width,
    UINT height,
    const std::array<int, kEyeCount>& translations) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(
            static_cast<std::size_t>(width) * height * kBytesPerPixel,
            0);
        for (std::size_t offset = 3; offset < bytes.size(); offset += kBytesPerPixel) {
            bytes[offset] = 255;
        }
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const int target_x = static_cast<int>(x) + translations[eye];
                if (target_x < 0 || target_x >= static_cast<int>(width)) {
                    continue;
                }
                const RgbaBytes texel = motion_texel(x, y, eye);
                const std::size_t destination =
                    (static_cast<std::size_t>(y) * width +
                     static_cast<std::size_t>(target_x)) *
                    kBytesPerPixel;
                std::copy(texel.begin(), texel.end(), bytes.begin() + destination);
            }
        }
    }
    return pattern;
}

[[nodiscard]] xrfg::Vec3 cross_product(
    const xrfg::Vec3& left,
    const xrfg::Vec3& right) noexcept {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x,
    };
}

[[nodiscard]] xrfg::Vec3 rotate_vector(
    const xrfg::Quaternion& quaternion,
    const xrfg::Vec3& input) noexcept {
    const xrfg::Vec3 axis{quaternion.x, quaternion.y, quaternion.z};
    const xrfg::Vec3 inner = cross_product(axis, input);
    const xrfg::Vec3 adjusted{
        inner.x + quaternion.w * input.x,
        inner.y + quaternion.w * input.y,
        inner.z + quaternion.w * input.z,
    };
    const xrfg::Vec3 outer = cross_product(axis, adjusted);
    return {
        input.x + 2.0F * outer.x,
        input.y + 2.0F * outer.y,
        input.z + 2.0F * outer.z,
    };
}

[[nodiscard]] std::uint8_t direction_channel(float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 255.0F)));
}

[[nodiscard]] StereoPattern ray_direction_pattern(
    UINT width,
    UINT height,
    const ReprojectionViews& views) {
    StereoPattern pattern;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        auto& bytes = pattern[eye];
        bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
        const auto& view = views[eye];
        const float left = std::tan(view.fov.angle_left);
        const float right = std::tan(view.fov.angle_right);
        const float top = std::tan(view.fov.angle_up);
        const float bottom = std::tan(view.fov.angle_down);
        const xrfg::D3D12ImageRect rect =
            view.image_rect.width == 0 && view.image_rect.height == 0
                ? xrfg::D3D12ImageRect{0, 0, width, height}
                : view.image_rect;
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
                if (x < rect.offset_x || y < rect.offset_y ||
                    x >= rect.offset_x + rect.width ||
                    y >= rect.offset_y + rect.height) {
                    bytes[offset + 0] = 0;
                    bytes[offset + 1] = 0;
                    bytes[offset + 2] = 0;
                    bytes[offset + 3] = 255;
                    continue;
                }
                const float u =
                    (static_cast<float>(x - rect.offset_x) + 0.5F) /
                    static_cast<float>(rect.width);
                const float v =
                    (static_cast<float>(y - rect.offset_y) + 0.5F) /
                    static_cast<float>(rect.height);
                const float tangent_x = left + (right - left) * u;
                const float tangent_y = top + (bottom - top) * v;
                const xrfg::Vec3 world_ray = rotate_vector(
                    view.pose.orientation,
                    {tangent_x, tangent_y, -1.0F});
                const float azimuth = std::atan2(world_ray.x, -world_ray.z);
                const float elevation = std::atan2(
                    world_ray.y,
                    std::sqrt(world_ray.x * world_ray.x +
                              world_ray.z * world_ray.z));
                const float red =
                    127.5F + 74.0F * std::sin(azimuth * 7.0F + elevation * 2.0F) +
                    31.0F * std::cos(azimuth * 3.0F - elevation * 5.0F);
                const float green =
                    127.5F + 69.0F * std::cos(azimuth * 5.0F + elevation * 4.0F) +
                    27.0F * std::sin(azimuth * 11.0F - elevation * 2.0F);
                const float blue =
                    127.5F + 71.0F * std::sin(azimuth * 9.0F - elevation * 3.0F) +
                    29.0F * std::cos(azimuth * 4.0F + elevation * 6.0F);
                bytes[offset + 0] = direction_channel(red);
                bytes[offset + 1] = direction_channel(green);
                bytes[offset + 2] = direction_channel(blue);
                bytes[offset + 3] = 255;
            }
        }
    }
    return pattern;
}

[[nodiscard]] bool target_pixel_maps_to_source(
    const xrfg::D3D12ReprojectionView& source,
    const xrfg::D3D12ReprojectionView& target,
    UINT width,
    UINT height,
    UINT x,
    UINT y, float* mapped_x=nullptr, float* mapped_y=nullptr) noexcept {
    const xrfg::D3D12ImageRect source_rect =
        source.image_rect.width == 0 && source.image_rect.height == 0
            ? xrfg::D3D12ImageRect{0, 0, width, height}
            : source.image_rect;
    const xrfg::D3D12ImageRect target_rect =
        target.image_rect.width == 0 && target.image_rect.height == 0
            ? xrfg::D3D12ImageRect{0, 0, width, height}
            : target.image_rect;
    if (x < target_rect.offset_x || y < target_rect.offset_y ||
        x >= target_rect.offset_x + target_rect.width ||
        y >= target_rect.offset_y + target_rect.height) {
        return false;
    }
    const float u =
        (static_cast<float>(x - target_rect.offset_x) + 0.5F) /
        static_cast<float>(target_rect.width);
    const float v =
        (static_cast<float>(y - target_rect.offset_y) + 0.5F) /
        static_cast<float>(target_rect.height);
    const float target_x = std::tan(target.fov.angle_left) +
                           (std::tan(target.fov.angle_right) -
                            std::tan(target.fov.angle_left)) *
                               u;
    const float target_y = std::tan(target.fov.angle_up) +
                           (std::tan(target.fov.angle_down) -
                            std::tan(target.fov.angle_up)) *
                               v;
    const xrfg::Vec3 world_ray = rotate_vector(
        target.pose.orientation,
        {target_x, target_y, -1.0F});
    const xrfg::Quaternion inverse_source{
        -source.pose.orientation.x,
        -source.pose.orientation.y,
        -source.pose.orientation.z,
        source.pose.orientation.w,
    };
    const xrfg::Vec3 source_ray = rotate_vector(inverse_source, world_ray);
    if (source_ray.z >= -1.0e-5F) {
        return false;
    }
    const float tangent_x = source_ray.x / -source_ray.z;
    const float tangent_y = source_ray.y / -source_ray.z;
    const float left = std::tan(source.fov.angle_left);
    const float right = std::tan(source.fov.angle_right);
    const float top = std::tan(source.fov.angle_up);
    const float bottom = std::tan(source.fov.angle_down);
    const float source_u = (tangent_x - left) / (right - left);
    const float source_v = (tangent_y - top) / (bottom - top);
    const float source_x = static_cast<float>(source_rect.offset_x) +
                           source_u * static_cast<float>(source_rect.width) -
                           0.5F;
    const float source_y = static_cast<float>(source_rect.offset_y) +
                           source_v * static_cast<float>(source_rect.height) -
                           0.5F;
    if (mapped_x) *mapped_x=source_x;
    if (mapped_y) *mapped_y=source_y;
    return source_x >= static_cast<float>(source_rect.offset_x) - 0.5F &&
           source_y >= static_cast<float>(source_rect.offset_y) - 0.5F &&
           source_x <= static_cast<float>(source_rect.offset_x + source_rect.width) - 0.5F &&
           source_y <= static_cast<float>(source_rect.offset_y + source_rect.height) - 0.5F;
}

[[nodiscard]] double mean_absolute_rgb_error_rect(
    const StereoPattern& actual,
    const StereoPattern& expected,
    UINT texture_width,
    UINT eye,
    const xrfg::D3D12ImageRect& rect,
    UINT margin) {
    require(
        eye < kEyeCount && margin * 2U < rect.width &&
            margin * 2U < rect.height &&
            actual[eye].size() == expected[eye].size(),
        "invalid viewport MAE input");
    double total = 0.0;
    std::size_t sample_count = 0;
    for (UINT y = rect.offset_y + margin;
         y + margin < rect.offset_y + rect.height;
         ++y) {
        for (UINT x = rect.offset_x + margin;
             x + margin < rect.offset_x + rect.width;
             ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * texture_width + x) *
                kBytesPerPixel;
            for (UINT channel = 0; channel < 3; ++channel) {
                total += std::abs(
                    static_cast<int>(actual[eye][offset + channel]) -
                    static_cast<int>(expected[eye][offset + channel]));
                ++sample_count;
            }
        }
    }
    require(sample_count != 0, "viewport MAE has no samples");
    return total / static_cast<double>(sample_count);
}

[[nodiscard]] double mean_absolute_rgb_error(
    const StereoPattern& actual,
    const StereoPattern& expected,
    UINT width,
    UINT height,
    UINT eye,
    UINT margin) {
    require(
        eye < kEyeCount && margin * 2U < width && margin * 2U < height &&
            actual[eye].size() == expected[eye].size(),
        "invalid stereo MAE input");
    double total = 0.0;
    std::size_t sample_count = 0;
    for (UINT y = margin; y + margin < height; ++y) {
        for (UINT x = margin; x + margin < width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel;
            for (UINT channel = 0; channel < 3; ++channel) {
                total += std::abs(
                    static_cast<int>(actual[eye][offset + channel]) -
                    static_cast<int>(expected[eye][offset + channel]));
                ++sample_count;
            }
        }
    }
    require(sample_count != 0, "stereo MAE has no samples");
    return total / static_cast<double>(sample_count);
}

void get_copy_layouts(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& description,
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount>* layouts,
    std::array<UINT, kEyeCount>* row_counts,
    std::array<UINT64, kEyeCount>* row_sizes,
    UINT64* total_size) {
    device->GetCopyableFootprints(
        &description,
        0,
        kEyeCount,
        0,
        layouts->data(),
        row_counts->data(),
        row_sizes->data(),
        total_size);
    require(*total_size > 0, "GetCopyableFootprints returned an empty layout");
}

void upload_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const StereoPattern& pattern) {
    require(texture != nullptr, "upload target is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_UPLOAD,
        total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped_memory = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(upload->Map(0, &no_read, &mapped_memory), "ID3D12Resource::Map(upload)");
    auto* mapped_bytes = static_cast<std::byte*>(mapped_memory);
    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == height, "unexpected upload row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(width) * kBytesPerPixel,
            "unexpected upload row size");
        require(
            pattern[eye].size() ==
                static_cast<std::size_t>(width) * height * kBytesPerPixel,
            "upload pattern dimensions do not match the texture");
        for (UINT y = 0; y < height; ++y) {
            std::byte* destination =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            const std::uint8_t* source =
                pattern[eye].data() +
                static_cast<std::size_t>(y) * width * kBytesPerPixel;
            std::memcpy(destination, source, static_cast<std::size_t>(row_sizes[eye]));
        }
    }
    upload->Unmap(0, nullptr);

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_destination = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_DEST);
        command_list->ResourceBarrier(1, &to_copy_destination);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER to_render_target = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        command_list->ResourceBarrier(1, &to_render_target);
    });
}

template <typename MotionForPixel>
[[nodiscard]] ComPtr<ID3D12Resource> create_and_upload_game_motion_field(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height,
    MotionForPixel&& motion_for_pixel) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(game motion)");

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);
    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_UPLOAD,
        total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped_memory = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(
        upload->Map(0, &no_read, &mapped_memory),
        "ID3D12Resource::Map(game motion upload)");
    auto* mapped_bytes = static_cast<std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == height, "unexpected game-motion row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(width) * sizeof(float) * 2U,
            "unexpected game-motion row size");
        for (UINT y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < width; ++x) {
                const auto motion = motion_for_pixel(eye, x, y);
                row[x * 2U] = motion[0];
                row[x * 2U + 1U] = motion[1];
            }
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            command_list->CopyTextureRegion(
                &destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER ready = transition_barrier(
            texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_list->ResourceBarrier(1, &ready);
    });
    return texture;
}

[[nodiscard]] ComPtr<ID3D12Resource> create_and_upload_game_motion(
    D3D12WarpFixture& fixture,
    UINT width,
    UINT height,
    const std::array<float, kEyeCount>& horizontal_motion) {
    D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
    description.Format = DXGI_FORMAT_R32G32_FLOAT;
    description.Flags = D3D12_RESOURCE_FLAG_NONE;
    const D3D12_HEAP_PROPERTIES properties =
        heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> texture;
    require_hresult(
        fixture.device()->CreateCommittedResource(
            &properties,
            D3D12_HEAP_FLAG_NONE,
            &description,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
        "ID3D12Device::CreateCommittedResource(game motion)");

    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);
    ComPtr<ID3D12Resource> upload = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_UPLOAD,
        total_size,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped_memory = nullptr;
    const D3D12_RANGE no_read{0, 0};
    require_hresult(
        upload->Map(0, &no_read, &mapped_memory),
        "ID3D12Resource::Map(game motion upload)");
    auto* mapped_bytes = static_cast<std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == height, "unexpected game-motion row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(width) * sizeof(float) * 2U,
            "unexpected game-motion row size");
        for (UINT y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<float*>(
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch);
            for (UINT x = 0; x < width; ++x) {
                row[x * 2U] = horizontal_motion[eye];
                row[x * 2U + 1U] = 0.0F;
            }
        }
    }
    upload->Unmap(0, nullptr);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[eye];
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = eye;
            command_list->CopyTextureRegion(
                &destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER ready = transition_barrier(
            texture.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_list->ResourceBarrier(1, &ready);
    });
    return texture;
}

[[nodiscard]] StereoPattern readback_pattern(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES initial_state) {
    require(texture != nullptr, "readback source is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    require(
        description.Width <= std::numeric_limits<UINT>::max() &&
            description.DepthOrArraySize == kEyeCount,
        "readback texture shape is unsupported");
    const UINT width = static_cast<UINT>(description.Width);
    const UINT height = description.Height;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    StereoPattern result;
    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(pattern readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(
            row_counts[eye] == height &&
                row_sizes[eye] == static_cast<UINT64>(width) * kBytesPerPixel,
            "unexpected pattern readback layout");
        result[eye].resize(
            static_cast<std::size_t>(width) * height * kBytesPerPixel);
        for (UINT y = 0; y < height; ++y) {
            const std::byte* source =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            std::uint8_t* destination =
                result[eye].data() +
                static_cast<std::size_t>(y) * width * kBytesPerPixel;
            std::memcpy(destination, source, static_cast<std::size_t>(row_sizes[eye]));
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
    return result;
}

void require_readback_matches(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const StereoPattern& expected,
    const char* label,
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
    require(texture != nullptr, std::string(label) + " resource is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(expected[eye].size()==description.Width*description.Height*kBytesPerPixel,
            std::string(label)+" expected pattern extent differs from actual texture");
        for (UINT y = 0; y < description.Height; ++y) {
            const std::byte* actual =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            const std::uint8_t* wanted =
                expected[eye].data() +
                static_cast<std::size_t>(y) * description.Width * kBytesPerPixel;
            if (std::memcmp(actual, wanted, static_cast<std::size_t>(row_sizes[eye])) != 0) {
                std::size_t first_difference = 0;
                while (first_difference < row_sizes[eye] &&
                       actual[first_difference] ==
                           static_cast<std::byte>(wanted[first_difference])) {
                    ++first_difference;
                }
                const unsigned actual_value = first_difference < row_sizes[eye]
                    ? std::to_integer<unsigned>(actual[first_difference])
                    : 0U;
                const unsigned wanted_value = first_difference < row_sizes[eye]
                    ? wanted[first_difference]
                    : 0U;
                readback->Unmap(0, nullptr);
                fail(std::string(label) + " pixel mismatch at eye " +
                     std::to_string(eye) + ", row " + std::to_string(y) +
                     ", byte " + std::to_string(first_difference) +
                     ": actual=" + std::to_string(actual_value) +
                     " expected=" + std::to_string(wanted_value));
            }
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
}

void require_source_is_render_target(
    D3D12WarpFixture& fixture,
    ID3D12Resource* source) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap_description.NumDescriptors = 1;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(rtv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(RTV)");

    D3D12_RENDER_TARGET_VIEW_DESC rtv_description{};
    rtv_description.Format = kFormat;
    rtv_description.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_description.Texture2DArray.MipSlice = 0;
    rtv_description.Texture2DArray.FirstArraySlice = 0;
    rtv_description.Texture2DArray.ArraySize = kEyeCount;
    rtv_description.Texture2DArray.PlaneSlice = 0;
    fixture.device()->CreateRenderTargetView(
        source,
        &rtv_description,
        rtv_heap->GetCPUDescriptorHandleForHeapStart());

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        constexpr FLOAT clear_color[4] = {0.125F, 0.25F, 0.5F, 1.0F};
        command_list->ClearRenderTargetView(
            rtv_heap->GetCPUDescriptorHandleForHeapStart(),
            clear_color,
            0,
            nullptr);
    });
}

void clear_depth_slices(
    D3D12WarpFixture& fixture,
    ID3D12Resource* source,
    const std::array<float, kEyeCount>& depths) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_description{};
    heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap_description.NumDescriptors = kEyeCount;
    heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    require_hresult(
        fixture.device()->CreateDescriptorHeap(
            &heap_description,
            IID_PPV_ARGS(dsv_heap.GetAddressOf())),
        "ID3D12Device::CreateDescriptorHeap(DSV)");

    const UINT descriptor_size =
        fixture.device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    const D3D12_CPU_DESCRIPTOR_HANDLE first_handle =
        dsv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_description{};
        dsv_description.Format = kDepthFormat;
        dsv_description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv_description.Flags = D3D12_DSV_FLAG_NONE;
        dsv_description.Texture2DArray.MipSlice = 0;
        dsv_description.Texture2DArray.FirstArraySlice = eye;
        dsv_description.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = first_handle;
        handle.ptr += static_cast<SIZE_T>(eye) * descriptor_size;
        fixture.device()->CreateDepthStencilView(source, &dsv_description, handle);
    }

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = first_handle;
            handle.ptr += static_cast<SIZE_T>(eye) * descriptor_size;
            command_list->ClearDepthStencilView(
                handle,
                D3D12_CLEAR_FLAG_DEPTH,
                depths[eye],
                0,
                0,
                nullptr);
        }
    });
}

void require_depth_readback_matches(
    D3D12WarpFixture& fixture,
    ID3D12Resource* texture,
    const std::array<float, kEyeCount>& expected,
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON) {
    require(texture != nullptr, "depth history resource is null");
    const D3D12_RESOURCE_DESC description = texture->GetDesc();
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kEyeCount> layouts{};
    std::array<UINT, kEyeCount> row_counts{};
    std::array<UINT64, kEyeCount> row_sizes{};
    UINT64 total_size = 0;
    get_copy_layouts(
        fixture.device(),
        description,
        &layouts,
        &row_counts,
        &row_sizes,
        &total_size);

    ComPtr<ID3D12Resource> readback = create_buffer(
        fixture,
        D3D12_HEAP_TYPE_READBACK,
        total_size,
        D3D12_RESOURCE_STATE_COPY_DEST);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        const D3D12_RESOURCE_BARRIER to_copy_source = transition_barrier(
            texture,
            initial_state,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        command_list->ResourceBarrier(1, &to_copy_source);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = texture;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = eye;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layouts[eye];
            command_list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        const D3D12_RESOURCE_BARRIER restore_state = transition_barrier(
            texture,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            initial_state);
        command_list->ResourceBarrier(1, &restore_state);
    });

    void* mapped_memory = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total_size)};
    require_hresult(
        readback->Map(0, &read_range, &mapped_memory),
        "ID3D12Resource::Map(depth readback)");
    const auto* mapped_bytes = static_cast<const std::byte*>(mapped_memory);
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        require(row_counts[eye] == kHeight, "unexpected depth readback row count");
        require(
            row_sizes[eye] == static_cast<UINT64>(kWidth) * sizeof(float),
            "unexpected depth readback row size");
        for (UINT y = 0; y < kHeight; ++y) {
            const std::byte* row =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            for (UINT x = 0; x < kWidth; ++x) {
                float actual = 0.0F;
                std::memcpy(&actual, row + static_cast<std::size_t>(x) * sizeof(float), sizeof(actual));
                if (std::fabs(actual - expected[eye]) > 1.0e-6F) {
                    readback->Unmap(0, nullptr);
                    fail(
                        "depth pixel mismatch at eye " + std::to_string(eye) +
                        ", x " + std::to_string(x) + ", y " + std::to_string(y));
                }
            }
        }
    }
    const D3D12_RANGE no_write{0, 0};
    readback->Unmap(0, &no_write);
}

struct CaptureResources {
    ComPtr<ID3D12Resource> resource;
    ComPtr<ID3D12Fence> fence;
    xrfg::D3D12HistoryConsumerLease lease{};
};

[[nodiscard]] CaptureResources acquire_capture_for_inspection(
    xrfg::D3D12SwapchainHistory& history,
    const xrfg::D3D12HistoryCaptureTicket& ticket) {
    ID3D12Resource* raw_resource = nullptr;
    ID3D12Fence* raw_fence = nullptr;
    xrfg::D3D12HistoryConsumerLease lease{};
    require(
        operation_succeeded(history.acquire_consumer(
            ticket,
            &lease,
            &raw_resource,
            &raw_fence)),
        "acquire_consumer rejected a current ticket");
    require(raw_resource != nullptr, "acquire_consumer returned a null texture");
    require(raw_fence != nullptr, "acquire_consumer returned a null fence");

    CaptureResources resources;
    resources.resource.Attach(raw_resource);
    resources.fence.Attach(raw_fence);
    resources.lease = lease;
    return resources;
}

void retire_completed_inspection(
    D3D12WarpFixture& fixture,
    xrfg::D3D12SwapchainHistory& history,
    const CaptureResources& resources) {
    ComPtr<ID3D12Fence> completed_fence;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(completed_fence.GetAddressOf())),
        "ID3D12Device::CreateFence(completed consumer)");
    require_hresult(
        completed_fence->Signal(1),
        "ID3D12Fence::Signal(completed consumer)");
    require(
        operation_succeeded(history.retire_consumer(resources.lease, completed_fence.Get(), 1)),
        "completed consumer lease retirement failed");
}

void require_private_resource_shape(
    ID3D12Resource* source,
    ID3D12Resource* history_resource) {
    require(source != nullptr && history_resource != nullptr, "cannot compare null resources");
    require(source != history_resource, "history aliases an application swapchain image");
    require(
        same_description(source->GetDesc(), history_resource->GetDesc()),
        "private history texture description does not match the source");

    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    require_hresult(
        history_resource->GetHeapProperties(&properties, &flags),
        "ID3D12Resource::GetHeapProperties(history)");
    require(properties.Type == D3D12_HEAP_TYPE_DEFAULT, "history texture is not on a DEFAULT heap");
    constexpr D3D12_HEAP_FLAGS kExternallyVisibleHeapFlags =
        D3D12_HEAP_FLAG_SHARED |
        D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
        D3D12_HEAP_FLAG_HARDWARE_PROTECTED;
    require(
        (flags & kExternallyVisibleHeapFlags) == D3D12_HEAP_FLAG_NONE,
        "history texture is shared, cross-adapter, or hardware-protected");
}

// WARP returns a per-adapter device singleton, so an actual second canonical
// WARP device is not a portable fixture. This UNUSED metadata resource forwards
// its real descriptor/COM lifetime but deliberately cannot expose a device.
// It is never copied, submitted, or consumed as AFW color (snapshot_output=false).
class UnusedColorMetadataProxy final : public ID3D12Resource {
public:
    explicit UnusedColorMetadataProxy(ID3D12Resource* resource) : resource_(resource) {}
    LONG device_queries() noexcept { return InterlockedCompareExchange(&device_queries_, 0, 0); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
            iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) || iid == __uuidof(ID3D12Resource)) {
            *object = static_cast<ID3D12Resource*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&references_);
        if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return resource_->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return resource_->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        return resource_->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return resource_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void** device) override {
        InterlockedIncrement(&device_queries_);
        if (!device) return E_POINTER;
        *device = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* range, void** data) override {
        return resource_->Map(subresource, range, data);
    }
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* range) override { resource_->Unmap(subresource, range); }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return resource_->GetDesc(); }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return resource_->GetGPUVirtualAddress(); }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT subresource, const D3D12_BOX* box, const void* data,
        UINT row_pitch, UINT depth_pitch) override {
        return resource_->WriteToSubresource(subresource, box, data, row_pitch, depth_pitch);
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* data, UINT row_pitch, UINT depth_pitch,
        UINT subresource, const D3D12_BOX* box) override {
        return resource_->ReadFromSubresource(data, row_pitch, depth_pitch, subresource, box);
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override {
        return resource_->GetHeapProperties(properties, flags);
    }
private:
    ComPtr<ID3D12Resource> resource_;
    volatile LONG references_{1}, device_queries_{};
};

#include "d3d12_reported_device_alias.hpp"

class QueueWithReportedDeviceAlias final : public ID3D12CommandQueue {
public:
    QueueWithReportedDeviceAlias(ID3D12CommandQueue* native, ID3D12Device* reported,
        bool exposes_native = false)
        : native_(native), reported_(reported), exposes_native_(exposes_native) {}
    LONG device_queries() noexcept { return InterlockedCompareExchange(&device_queries_, 0, 0); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (exposes_native_ && iid == xrfg::d3d_native::streamline_native_guid)
            return native_->QueryInterface(__uuidof(ID3D12CommandQueue), object);
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) &&
            iid != __uuidof(ID3D12DeviceChild) && iid != __uuidof(ID3D12Pageable) &&
            iid != __uuidof(ID3D12CommandQueue)) return E_NOINTERFACE;
        *object = static_cast<ID3D12CommandQueue*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&references_); if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return native_->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return native_->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override { return native_->SetPrivateDataInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return native_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** device) override {
        InterlockedIncrement(&device_queries_); return reported_->QueryInterface(iid, device);
    }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* c,
        const D3D12_TILE_REGION_SIZE* s, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f,
        const UINT* o, const UINT* t, D3D12_TILE_MAPPING_FLAGS flags) override { native_->UpdateTileMappings(r, n, c, s, h, nr, f, o, t, flags); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d, const D3D12_TILED_RESOURCE_COORDINATE* dc,
        ID3D12Resource* s, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* z,
        D3D12_TILE_MAPPING_FLAGS f) override { native_->CopyTileMappings(d, dc, s, sc, z, f); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList* const* l) override { native_->ExecuteCommandLists(n, l); }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void* d, UINT n) override { native_->SetMarker(m, d, n); }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void* d, UINT n) override { native_->BeginEvent(m, d, n); }
    void STDMETHODCALLTYPE EndEvent() override { native_->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* f, UINT64 v) override { return native_->Signal(f, v); }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f, UINT64 v) override { return native_->Wait(f, v); }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f) override { return native_->GetTimestampFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g, UINT64* c) override { return native_->GetClockCalibration(g, c); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return native_->GetDesc(); }
private:
    ComPtr<ID3D12CommandQueue> native_;
    ComPtr<ID3D12Device> reported_;
    bool exposes_native_{};
    volatile LONG references_{1}, device_queries_{};
};

void test_public_video_allocation_owner(D3D12WarpFixture& fixture) {
    ComPtr<ID3D12Device> hardware;
    require_hresult(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&hardware)),"public video hardware device");
    ComPtr<ID3D12VideoDevice2> video;
    require_hresult(hardware.As(&video),"public native video interface available");
    ComPtr<ReportedDeviceAlias> proxy;
    proxy.Attach(new ReportedDeviceAlias(hardware.Get(),false,true));
    IUnknown* legacy{};
    require(xrfg::d3d_native::get_borrowed(proxy.Get(),&legacy)&&legacy==proxy.Get(),
        "REAL VR unsupported-proxy counterfactual lost");
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> old_queue;
    require_hresult(proxy->CreateCommandQueue(&desc,IID_PPV_ARGS(&old_queue)),"legacy proxy queue creation");
    require(proxy->intercepted_queues==1,"legacy queue did not enter proxy");
    ComPtr<ID3D12Device> owner;
    require_hresult(xrfg::d3d_native::allocation_owner(proxy.Get(),owner),"public video exact allocation owner");
    require(owner.Get()==hardware.Get()&&d3d_owner::same(proxy.Get(),owner.Get()),"unverified backing admitted");
    for(unsigned repeat=0;repeat<32;++repeat) {
        ComPtr<ID3D12Device> retained;
        require_hresult(xrfg::d3d_native::allocation_owner(proxy.Get(),retained),"cached verified owner");
        require(retained.Get()==hardware.Get(),"cached allocation changed owner");
    }
    require(proxy->public_video_queries==1,"video interface queried repeatedly per frame");
    ComPtr<ReportedDeviceAlias> wrong_backing;
    wrong_backing.Attach(new ReportedDeviceAlias(fixture.device(),false,true,hardware.Get()));
    ComPtr<ID3D12Device> rejected=hardware;
    require(xrfg::d3d_native::allocation_owner(wrong_backing.Get(),rejected)==E_INVALIDARG&&!rejected,
        "public video interface on a different backing device was accepted");
    ComPtr<ID3D12CommandQueue> execution;
    require_hresult(owner->CreateCommandQueue(&desc,IID_PPV_ARGS(&execution)),"unproxied private execution");
    require(proxy->intercepted_queues==1,"private creation still entered wrapper");
    ComPtr<ID3D12Device> execution_owner;
    require_hresult(execution->GetDevice(IID_PPV_ARGS(&execution_owner)),"private queue device");
    require(execution_owner.Get()==hardware.Get(),"private execution changed allocation owner");
    ComPtr<ID3D12Fence> fence;
    require_hresult(owner->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"native owner fence");
    require_hresult(execution->Signal(fence.Get(),1),"unproxied GPU work");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"native owner event");
    const auto signaled=fence->SetEventOnCompletion(1,event);
    const auto waited=SUCCEEDED(signaled)?WaitForSingleObject(event,5000):WAIT_FAILED;CloseHandle(event);
    require(SUCCEEDED(signaled)&&waited==WAIT_OBJECT_0,"unproxied execution did not complete");
    require_hresult(xrfg::d3d_native::allocation_owner(owner.Get(),owner),"same-input owner normalization");
    require(owner.Get()==hardware.Get(),"normalization changed known native identity");
    std::cout<<"Public video device: old proxy intercepts queue, exact native owner bypasses it; real GPU fence completes PASS\n";
}

void test_protected_guide_snapshots(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    auto color = create_source_texture(fixture);
    auto motion = create_source_texture(fixture);
    auto depth = create_source_texture(fixture);
    std::vector<std::shared_ptr<const xrfg::DlssMotionVectorFrame>> held;
    std::shared_ptr<const xrfg::DlssMotionVectorFrame> published;
    std::uint64_t production_sequence{};
    const auto publish = [&](unsigned n, bool reset = false,bool submit_receipt=true) {
        upload_pattern(fixture, motion.Get(), make_pattern(static_cast<std::uint8_t>(n * 13)));
        upload_pattern(fixture, depth.Get(), make_pattern(static_cast<std::uint8_t>(n * 17)));
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            xrfg::DlssMotionVectorPublication p;
            p.stream = 90028; p.output = color.Get(); p.motion_vectors = motion.Get();
            p.depth = depth.Get(); p.producer_queue = fixture.queue(); p.producer_command_list = list;
            p.output_width = p.motion_width = p.depth_width = kWidth;
            p.output_height = p.motion_height = p.depth_height = kHeight;
            p.resource_state = p.depth_resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.protect_snapshot_lifetime = true;
            p.reset = reset;
            p.camera_frame_token_valid = true; p.camera_frame_token = n * 7;
            p.native_production_sequence = ++production_sequence;
            p.raw_camera_motion_included = n % 2;
            for (unsigned i=0;i<4;++i) p.raw_clip_to_previous_clip[i*5]=p.raw_previous_clip_to_clip[i*5]=1;
            p.raw_clip_to_previous_clip[12]=float(n)*.25F;
            p.raw_previous_clip_to_clip[12]=-float(n)*.25F;
            published = xrfg::publish_dlss_motion_vectors(p);
        },submit_receipt);
        const auto resolved = xrfg::resolve_dlss_motion_vectors(1, fixture.queue());
        require(resolved && resolved->eye_count == 1 && resolved->eyes[0], "protected guide resolution");
        return resolved->eyes[0];
    };
    constexpr unsigned snapshot_capacity = 16;
    for (unsigned n = 1; n <= snapshot_capacity; ++n) {
        // Preserve the explicit cross-queue ownership counterfactual below:
        // actual GPU copies finish, but n3/n4 have no admitted receipt yet.
        auto frame = publish(n,false,n!=3 && n!=4);
        require(published && published->publication == frame->publication &&
            published->color_origin_token != 0 &&
            published->color_origin_token == frame->color_origin_token &&
            published->serial == n && frame->motion_retention && frame->depth_retention &&
            frame->camera_frame_token_valid && frame->camera_frame_token == n * 7 &&
            frame->native_production_sequence == n,
            "protected guide lost provenance/lifetime");
        for (const auto& old : held) require(old->color_origin_token < frame->color_origin_token &&
            old->motion_vectors.Get() != frame->motion_vectors.Get() &&
            old->depth.Get() != frame->depth.Get(), "publisher overwrote a retained snapshot");
        held.push_back(std::move(frame));
    }
    const auto first_publication = held[0]->publication;
    const auto last_origin_token = held.back()->color_origin_token;
    {
        const auto selected = xrfg::resolve_luke_guide_token(
            held[3]->color_origin_token, fixture.queue(), fixture.device());
        require(selected && selected->publication == held[3]->publication &&
            selected->color_origin_token == held[3]->color_origin_token &&
            selected->serial == 4 && selected->previous_serial == 2 &&
            selected->previous_opposite_guide &&
            selected->previous_opposite_guide->publication == held[2]->publication &&
            selected->camera_frame_token == 28 &&
            selected->previous_opposite_guide->camera_frame_token == 21 &&
            selected->native_production_sequence == 4 &&
            selected->previous_opposite_guide->native_production_sequence == 3,
            "color origin did not resolve its exact retained guide and N-1 camera");
        require(selected->previous_same_native_camera &&
            selected->previous_same_native_camera->publication == held[1]->publication &&
            selected->previous_same_native_camera->serial == 2 &&
            selected->previous_same_native_camera->token == 14 &&
            selected->previous_same_native_camera->native_production_sequence == 2 &&
            selected->previous_same_native_camera->valid ==
                xrfg::dlss_native_camera_history(*held[1]).valid,
            "color origin borrowed or synthesized the N-2 native camera");
        require(!selected->previous_same_guide,
            "evicted native N-2 was substituted with a newer camera");
        require(selected->raw_camera_motion_included == 0 &&
            selected->raw_clip_to_previous_clip[12] == 1.0F &&
            selected->raw_previous_clip_to_clip[12] == -1.0F &&
            held.back()->raw_clip_to_previous_clip[12] == 4.0F,
            "older color borrowed newer producer-declared motion history");
        require_readback_matches(fixture, selected->motion_vectors.Get(), make_pattern(52),
            "exact older origin MV", selected->resource_state);
        require_readback_matches(fixture, selected->depth.Get(), make_pattern(68),
            "exact older origin depth", selected->depth_resource_state);
        require(!xrfg::resolve_luke_guide_token(0, fixture.queue(), fixture.device()) &&
            !xrfg::resolve_luke_guide_token(held.back()->color_origin_token + 1, fixture.queue(), fixture.device()) &&
            !xrfg::resolve_luke_guide_token(held[0]->color_origin_token, fixture.queue(), fixture.device()) &&
            !xrfg::resolve_luke_guide_token(held[1]->color_origin_token, fixture.queue(), fixture.device()) &&
            !xrfg::resolve_luke_guide_token(held[2]->color_origin_token, fixture.queue(), fixture.device()),
            "unknown, evicted, or missing N-1 origin fell back to the stream head");

        // Ghost F11: exact colors at age 5/6 were rejected by the old six-entry
        // history despite a valid barcode. Check both current and N-1 GPU data,
        // not merely successful lookup or substitution with the latest guide.
        for (unsigned age : {5U, 6U}) {
            const auto index = snapshot_capacity - 1 - age;
            const auto delayed = xrfg::resolve_luke_guide_token(
                held[index]->color_origin_token, fixture.queue(), fixture.device());
            require(delayed && delayed->publication == held[index]->publication &&
                delayed->color_origin_token == held[index]->color_origin_token &&
                delayed->previous_opposite_guide &&
                delayed->previous_opposite_guide->publication == held[index - 1]->publication &&
                delayed->camera_frame_token == (index + 1) * 7 &&
                delayed->previous_opposite_guide->camera_frame_token == index * 7,
                "age 5/6 exact origin lost its own N-1 guide/camera");
            // No AFW evaluations occurred between these token lookups. Depth
            // still needs native N-2, not the earlier selected color history.
            require(delayed->previous_same_guide &&
                delayed->previous_same_guide->publication == held[index - 2]->publication &&
                delayed->previous_same_guide->camera_frame_token == (index - 1) * 7 &&
                !delayed->previous_same_guide->previous_same_guide &&
                !delayed->previous_same_guide->previous_opposite_guide,
                "skipped AFW lookups changed native N-2 or retained a recursive chain");
            require_readback_matches(fixture, delayed->previous_same_guide->motion_vectors.Get(),
                make_pattern(static_cast<std::uint8_t>((index - 1) * 13)),
                "delayed native N-2 MV", delayed->previous_same_guide->resource_state);
            require_readback_matches(fixture, delayed->motion_vectors.Get(),
                make_pattern(static_cast<std::uint8_t>((index + 1) * 13)),
                "delayed exact MV", delayed->resource_state);
            require_readback_matches(fixture, delayed->depth.Get(),
                make_pattern(static_cast<std::uint8_t>((index + 1) * 17)),
                "delayed exact depth", delayed->depth_resource_state);
            require_readback_matches(fixture, delayed->previous_opposite_guide->motion_vectors.Get(),
                make_pattern(static_cast<std::uint8_t>(index * 13)),
                "delayed N-1 MV", delayed->previous_opposite_guide->resource_state);
            require_readback_matches(fixture, delayed->previous_opposite_guide->depth.Get(),
                make_pattern(static_cast<std::uint8_t>(index * 17)),
                "delayed N-1 depth", delayed->previous_opposite_guide->depth_resource_state);
        }

        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> consumer_queue;
        require_hresult(fixture.device()->CreateCommandQueue(&queue_description,
            IID_PPV_ARGS(&consumer_queue)), "origin consumer queue");
        require(!xrfg::resolve_luke_guide_token(held[3]->color_origin_token, consumer_queue.Get(), fixture.device()),
            "unsubmitted origin was admitted on another queue");
        ID3D12CommandList* submitted[] = {
            held[2]->producer_command_list.Get(), held[3]->producer_command_list.Get()};
        xrfg::report_dlss_motion_vector_submission(fixture.queue(), 2, submitted);
        const auto stamped = xrfg::resolve_luke_guide_token(
            held[3]->color_origin_token, consumer_queue.Get(), fixture.device());
        require(stamped && stamped->producer_fence && stamped->producer_fence_value != 0 &&
            stamped->color_origin_token == held[3]->color_origin_token &&
            stamped->previous_opposite_guide->color_origin_token == held[2]->color_origin_token &&
            stamped->previous_opposite_guide->producer_fence_value != 0 &&
            held[3]->producer_fence_value == 0 && selected->producer_fence_value == 0,
            "submission did not stamp older retained guides immutably");
    }
    const auto snapshot_state = held[0]->resource_state;
    require_readback_matches(fixture, held[0]->motion_vectors.Get(), make_pattern(13), "old MV after sixteen publications", snapshot_state);
    require_readback_matches(fixture, held[0]->depth.Get(), make_pattern(17), "old depth after sixteen publications", snapshot_state);
    Microsoft::WRL::ComPtr<ID3D12Fence> reader_fence;
    require_hresult(fixture.device()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&reader_fence)), "protected snapshot reader fence");
    held[0]->motion_retention->begin_gpu_read();
    held[0]->depth_retention->begin_gpu_read();
    require_hresult(held[0]->motion_retention->retire_gpu_read(reader_fence.Get(), 1), "retire MV reader");
    require_hresult(held[0]->depth_retention->retire_gpu_read(reader_fence.Get(), 1), "retire depth reader");
    const auto first_motion = held[0]->motion_vectors;
    const auto first_depth = held[0]->depth;
    held[0].reset(); // no CPU frame owner; unfinished GPU read MUST still pin it
    const auto failures = xrfg::dlss_motion_vector_statistics().snapshot_failures;
    require(publish(17)->publication == first_publication + 15 && !published &&
        xrfg::dlss_motion_vector_statistics().snapshot_failures == failures + 1,
        "full pool reused a GPU-in-flight snapshot");
    require_readback_matches(fixture, first_motion.Get(), make_pattern(13), "busy MV remains unchanged", snapshot_state);
    require_readback_matches(fixture, first_depth.Get(), make_pattern(17), "busy depth remains unchanged", snapshot_state);
    require_hresult(reader_fence->Signal(1), "complete protected snapshot read");
    const auto recycled = publish(17);
    require(published && recycled->publication == first_publication + 16 &&
        recycled->color_origin_token > last_origin_token + 1 &&
        recycled->native_production_sequence == 18 &&
        held.back()->native_production_sequence == 16 &&
        recycled->motion_vectors.Get() == first_motion.Get() &&
        recycled->depth.Get() == first_depth.Get(), "completed unowned snapshot not recycled");
    require_readback_matches(fixture, recycled->motion_vectors.Get(), make_pattern(221), "recycled MV", snapshot_state);
    held.clear();
    const auto reset = publish(10, true);
    const auto reset_metadata=xrfg::luke_native_origin_metadata(reset->color_origin_token,fixture.device());
    require(reset_metadata&&reset_metadata->epoch==reset->epoch&&reset_metadata->serial==1&&
        xrfg::luke_native_stream_reset(reset->stream,recycled->epoch,fixture.device()),
        "ordinary non-GTA native reset preflight remained title-restricted");
    require(reset->publication == recycled->publication + 1 && reset->serial == 1 &&
        reset->color_origin_token > recycled->color_origin_token &&
        !xrfg::resolve_luke_guide_token(recycled->color_origin_token, fixture.queue(), fixture.device()) &&
        !xrfg::resolve_luke_guide_token(reset->color_origin_token, fixture.queue(), fixture.device()),
        "reset retained an old origin or inferred its missing opposite guide");
    const auto after_reset = publish(11);
    const auto reset_join = xrfg::resolve_luke_guide_token(after_reset->color_origin_token, fixture.queue(), fixture.device());
    require(reset_join && reset_join->epoch == reset->epoch &&
        reset_join->previous_opposite_guide->publication == reset->publication &&
        !reset_join->previous_same_guide,
        "reset epoch did not join its exact consecutive origin");
    xrfg::invalidate_dlss_motion_vector_stream_history(90028);
    require(!xrfg::resolve_luke_guide_token(after_reset->color_origin_token,fixture.queue(),fixture.device()),
        "failed native SR left its old temporal epoch resolvable");
    require_readback_matches(fixture,reset_join->motion_vectors.Get(),make_pattern(143),
        "native failure preserved retained in-flight MV",reset_join->resource_state);
    require_readback_matches(fixture,reset_join->depth.Get(),make_pattern(187),
        "native failure preserved retained in-flight depth",reset_join->depth_resource_state);
    const auto after_native_failure=publish(12);
    require(after_native_failure->epoch==after_reset->epoch+1 && after_native_failure->serial==1 &&
        after_native_failure->native_production_sequence==production_sequence &&
        !xrfg::resolve_luke_guide_token(after_native_failure->color_origin_token,fixture.queue(),fixture.device()),
        "native SR failure did not restart exact temporal history");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    const auto restarted = publish(12);
    require(restarted->publication > after_reset->publication && restarted->serial == 1 &&
        restarted->color_origin_token > after_reset->color_origin_token &&
        !xrfg::resolve_luke_guide_token(after_reset->color_origin_token, fixture.queue(), fixture.device()),
        "tracking restart reused a color-carried origin identity");
    xrfg::retire_dlss_motion_vector_stream(90028);
    const auto replaced_stream = publish(12);
    require(replaced_stream->serial == 1 &&
        replaced_stream->color_origin_token > restarted->color_origin_token &&
        !xrfg::resolve_luke_guide_token(restarted->color_origin_token, fixture.queue(), fixture.device()),
        "retired stream recreated a previous process token");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    {
        xrfg::configure_dlss_motion_vector_tracking(true);
        ComPtr<UnusedColorMetadataProxy> unused_color;
        unused_color.Attach(new UnusedColorMetadataProxy(color.Get()));
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> exact_frame;
        for (unsigned n = 1; n <= 2; ++n) {
            fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
                xrfg::DlssMotionVectorPublication p;
                p.stream = 90033; p.output = unused_color.Get(); p.snapshot_output = false;
                p.motion_vectors = motion.Get(); p.depth = depth.Get();
                p.producer_queue = fixture.queue(); p.producer_command_list = list;
                p.verified_producer_device = fixture.device();
                p.output_width = p.motion_width = p.depth_width = kWidth;
                p.output_height = p.motion_height = p.depth_height = kHeight;
                p.resource_state = p.depth_resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
                p.protect_snapshot_lifetime = true;
                exact_frame = xrfg::publish_dlss_motion_vectors(p);
            },false); // the proxy-owner negative test supplies its receipt explicitly
            require(exact_frame != nullptr, "unused original metadata blocked protected publication");
        }
        require(unused_color->device_queries() == 0, "publisher queried an unsnapshotted unused color device");
        const auto joined = xrfg::resolve_luke_guide_token(exact_frame->color_origin_token, fixture.queue(), fixture.device());
        require(joined && joined->publication == exact_frame->publication &&
            joined->previous_opposite_guide && joined->motion_retention && joined->depth_retention,
            "exact AFW origin rejected unused original color device metadata");
        require(unused_color->device_queries() == 0,
            "exact AFW origin consulted original color instead of owned MV/depth");
        require_readback_matches(fixture, joined->motion_vectors.Get(), make_pattern(156),
            "owned MV with unusable original device metadata", joined->resource_state);
        require_readback_matches(fixture, joined->depth.Get(), make_pattern(204),
            "owned depth with unusable original device metadata", joined->depth_resource_state);

        ComPtr<ReportedDeviceAlias> device_alias;
        device_alias.Attach(new ReportedDeviceAlias(fixture.device()));
        ComPtr<QueueWithReportedDeviceAlias> queue_alias;
        queue_alias.Attach(new QueueWithReportedDeviceAlias(fixture.queue(), device_alias.Get()));
        ComPtr<ID3D12Device> reported;
        require_hresult(queue_alias->GetDevice(IID_PPV_ARGS(&reported)), "proxy queue reported device");
        ComPtr<IUnknown> native_identity, reported_identity;
        require_hresult(fixture.device()->QueryInterface(IID_PPV_ARGS(&native_identity)), "native device identity");
        require_hresult(reported.As(&reported_identity), "proxy device identity");
        const auto native_luid = fixture.device()->GetAdapterLuid();
        const auto reported_luid = reported->GetAdapterLuid();
        require(native_identity.Get() != reported_identity.Get() &&
            native_luid.HighPart == reported_luid.HighPart && native_luid.LowPart == reported_luid.LowPart,
            "reported-device regression requires distinct identity on the SAME adapter");
        require(!xrfg::resolve_luke_guide_token(exact_frame->color_origin_token, queue_alias.Get(), fixture.device()),
            "native consumer alias bypassed the required crossqueue submission fence");
        ID3D12CommandList* submitted[] = {joined->previous_opposite_guide->producer_command_list.Get(),
            joined->producer_command_list.Get()};
        xrfg::report_dlss_motion_vector_submission(fixture.queue(), 2, submitted);
        const auto queries_before = queue_alias->device_queries();
        const auto alias_join = xrfg::resolve_luke_guide_token(
            exact_frame->color_origin_token, queue_alias.Get(), fixture.device());
        require(alias_join && alias_join->producer_fence &&
            alias_join->publication == exact_frame->publication && alias_join->previous_opposite_guide &&
            alias_join->motion_snapshot_device.Get() == fixture.device() &&
            alias_join->depth_snapshot_device.Get() == fixture.device(),
            "reported proxy identity rejected the explicit native snapshot owner");
        require(!xrfg::resolve_luke_guide_token(exact_frame->color_origin_token, queue_alias.Get(), device_alias.Get()) &&
            !xrfg::resolve_luke_guide_token(exact_frame->color_origin_token, queue_alias.Get(), nullptr),
            "different SAME-adapter device identity or missing explicit owner admitted snapshots");
        require(queue_alias->device_queries() == queries_before,
            "exact-origin resolver queried the unreliable reported queue device");
        require_readback_matches(fixture, alias_join->motion_vectors.Get(), make_pattern(156),
            "native owner through reported-device alias", alias_join->resource_state);

        // Optional real different-adapter consumer: no additional WARP singleton
        // or fake device. Existing same-device crossqueue/fence tests above remain.
        ComPtr<IDXGIFactory6> consumer_factory;
        require_hresult(CreateDXGIFactory1(IID_PPV_ARGS(&consumer_factory)), "foreign consumer factory");
        ComPtr<ID3D12Device> foreign_device;
        for (UINT index = 0; !foreign_device; ++index) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT enumeration = consumer_factory->EnumAdapters1(index, &adapter);
            if (enumeration == DXGI_ERROR_NOT_FOUND) break;
            require_hresult(enumeration, "foreign consumer adapter");
            DXGI_ADAPTER_DESC1 description{};
            require_hresult(adapter->GetDesc1(&description), "foreign consumer adapter description");
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            static_cast<void>(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(&foreign_device)));
        }
        if (foreign_device) {
            ComPtr<IUnknown> foreign_identity, producer_identity;
            require_hresult(foreign_device.As(&foreign_identity), "foreign consumer identity");
            require_hresult(fixture.device()->QueryInterface(IID_PPV_ARGS(&producer_identity)), "native producer identity");
            require(foreign_identity.Get() != producer_identity.Get(), "negative consumer device is not distinct");
            D3D12_COMMAND_QUEUE_DESC description{}; description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            ComPtr<ID3D12CommandQueue> foreign_queue;
            require_hresult(foreign_device->CreateCommandQueue(&description, IID_PPV_ARGS(&foreign_queue)), "foreign consumer queue");
            require(!xrfg::resolve_luke_guide_token(exact_frame->color_origin_token, foreign_queue.Get(), foreign_device.Get()),
                "foreign consumer device admitted native MV/depth snapshots");
        } else {
            std::cout << "Protected guide foreign-device negative: skipped (WARP-only host)\n";
        }
        require(unused_color->device_queries() == 0, "foreign consumer caused irrelevant original color device query");
        xrfg::configure_dlss_motion_vector_tracking(false);
        xrfg::retire_disabled_dlss_motion_vector_resources();
    }
    std::cout << "Protected guides: bounded exact origins, immutable submission, reset rejection, CPU/GPU pins, unused-output independence\n";
}

void test_luke_multi_handle_origins(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    xrfg::configure_luke_color_origin_extent(512,32);
    auto color=create_single_slice_texture(fixture,512,32);
    auto motion=create_source_texture(fixture),depth=create_source_texture(fixture);
    struct FrozenOrigin {
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
        ComPtr<ID3D12Resource> color;
    };
    const auto publish=[&](std::uint64_t stream,unsigned n,unsigned seed,bool reset=false,bool submit_receipt=true) {
        upload_pattern(fixture,motion.Get(),make_pattern(static_cast<std::uint8_t>(seed+n)));
        upload_pattern(fixture,depth.Get(),make_pattern(static_cast<std::uint8_t>(seed+n+50)));
        FrozenOrigin recorded{};recorded.color=create_single_slice_texture(fixture,512,32);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            xrfg::DlssMotionVectorPublication p;
            p.stream=stream;p.output=color.Get();p.motion_vectors=motion.Get();p.depth=depth.Get();
            p.producer_queue=fixture.queue();p.producer_command_list=list;
            p.output_width=512;p.output_height=32;
            p.motion_width=p.depth_width=kWidth;p.motion_height=p.depth_height=kHeight;
            p.resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.protect_snapshot_lifetime=true;p.reset=reset;
            p.camera_frame_token_valid=p.camera_data_valid=p.camera_matrices_valid=true;
            p.camera_frame_token=n*7;p.native_production_sequence=n;
            recorded.frame=xrfg::publish_dlss_motion_vectors(p);
            require(recorded.frame!=nullptr,"multi-handle owned native publication failed");
            require_hresult(xrfg::mark_luke_color_origin(list,color.Get(),recorded.frame->color_origin_token,
                D3D12_RESOURCE_STATE_RENDER_TARGET,fixture.device()),"multi-handle production color marker");
            const D3D12_RESOURCE_BARRIER before[]{
                transition_barrier(color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(recorded.color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(2,before);list->CopyResource(recorded.color.Get(),color.Get());
            const D3D12_RESOURCE_BARRIER after[]{
                transition_barrier(color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET),
                transition_barrier(recorded.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET)};
            list->ResourceBarrier(2,after);
        },submit_receipt);
        return recorded;
    };
    const auto read=[&](const FrozenOrigin& source) {
        std::uint32_t token{};
        require_hresult(xrfg::read_luke_color_origin(fixture.queue(),source.color.Get(),&token,
            D3D12_RESOURCE_STATE_RENDER_TARGET,5000),"multi-handle frozen color CRC read");
        require(token==source.frame->color_origin_token,"multi-handle frozen color lost its exact origin");
        return token;
    };
    auto a1=publish(90499,1,10);
    // The exact A2/A3 originals execute now, but their ownership receipt is
    // deliberately withheld for the cross-queue negative/positive pair below.
    auto a2=publish(90499,2,10,false,false),a3=publish(90499,3,10,false,false);
    const auto a_token=read(a3);
    require(xrfg::resolve_luke_guide_token(a_token,fixture.queue(),fixture.device())!=nullptr,
        "single-handle baseline did not resolve exact color");
    auto b1=publish(90500,1,90);
    // A second legitimate native handle has no opposite history yet. It must
    // neither borrow A's camera nor veto A's independently complete history.
    require(!xrfg::resolve_luke_guide_token(read(b1),fixture.queue(),fixture.device()),
        "new native handle borrowed another stream's opposite camera");
    auto old_a=xrfg::resolve_luke_guide_token(a_token,fixture.queue(),fixture.device());
    require(old_a && old_a->stream==90499 && old_a->publication==a3.frame->publication &&
        old_a->previous_opposite_guide->publication==a2.frame->publication &&
        old_a->previous_same_native_camera && old_a->previous_same_native_camera->publication==a1.frame->publication,
        "unrelated native handle invalidated exact retained color owner");
    auto b2=publish(90500,2,90),b3=publish(90500,3,90);
    auto current_b=xrfg::resolve_luke_guide_token(read(b3),fixture.queue(),fixture.device());
    require(current_b && current_b->stream==90500 && current_b->publication==b3.frame->publication &&
        current_b->previous_opposite_guide->publication==b2.frame->publication &&
        current_b->previous_same_native_camera && current_b->previous_same_native_camera->publication==b1.frame->publication,
        "current color did not retain its own native handle's N-1/N-2 history");
    old_a=xrfg::resolve_luke_guide_token(read(a3),fixture.queue(),fixture.device());
    require(old_a && old_a->publication==a3.frame->publication &&
        old_a->native_production_sequence==3 && current_b->native_production_sequence==3,
        "older native color borrowed newer stream identity");
    require_readback_matches(fixture,old_a->motion_vectors.Get(),make_pattern(13),"multi-handle old exact MV",old_a->resource_state);
    require_readback_matches(fixture,old_a->depth.Get(),make_pattern(63),"multi-handle old exact depth",old_a->depth_resource_state);
    require_readback_matches(fixture,current_b->motion_vectors.Get(),make_pattern(93),"multi-handle current exact MV",current_b->resource_state);
    require_readback_matches(fixture,current_b->previous_opposite_guide->motion_vectors.Get(),make_pattern(92),
        "multi-handle exact own N-1 MV",current_b->previous_opposite_guide->resource_state);

    ComPtr<ReportedDeviceAlias> wrong_owner;wrong_owner.Attach(new ReportedDeviceAlias(fixture.device()));
    require(!xrfg::resolve_luke_guide_token(a_token,fixture.queue(),wrong_owner.Get()),
        "multi-handle token bypassed exact canonical device identity");
    D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> consumer;require_hresult(fixture.device()->CreateCommandQueue(&description,
        IID_PPV_ARGS(&consumer)),"multi-handle separate consumer queue");
    require(!xrfg::resolve_luke_guide_token(a_token,consumer.Get(),fixture.device()),
        "multi-handle token bypassed native submission receipt");
    ID3D12CommandList* submitted[]{a2.frame->producer_command_list.Get(),a3.frame->producer_command_list.Get()};
    xrfg::report_dlss_motion_vector_submission(fixture.queue(),2,submitted);
    auto cross_queue=xrfg::resolve_luke_guide_token(a_token,consumer.Get(),fixture.device());
    require(cross_queue && cross_queue->producer_fence_value && cross_queue->previous_opposite_guide->producer_fence_value &&
        cross_queue->publication==a3.frame->publication,"multi-handle crossqueue did not preserve exact native fence ownership");
    require(!xrfg::resolve_luke_guide_token(0,fixture.queue(),fixture.device()) &&
        !xrfg::resolve_luke_guide_token(UINT32_MAX,fixture.queue(),fixture.device()),
        "multi-handle unknown color fell back to an arbitrary stream head");
    // Deliberately corrupt this fixture's registry claim: a process-unique
    // token cannot have two native owners. Preserve the actual GPU colors and
    // restore the fixture immediately after exercising the fail-closed guard.
    // Submission stamps an immutable registry copy. Corrupt that ACTUAL owner
    // temporarily, not the earlier CPU publication object returned by publish.
    const auto native_owner=xrfg::resolve_dlss_guide_publication(90500,b3.frame->publication,fixture.queue());
    require(native_owner && native_owner->color_origin_token==b3.frame->color_origin_token,
        "duplicate-origin counterfactual did not identify the actual submitted registry owner");
    auto forged_owner=std::const_pointer_cast<xrfg::DlssMotionVectorFrame>(native_owner);
    const auto b_token=forged_owner->color_origin_token;
    forged_owner->color_origin_token=a_token;
    require(!xrfg::resolve_luke_guide_token(read(a3),fixture.queue(),fixture.device()),
        "duplicate color-origin owners were resolved by stream iteration order");
    forged_owner->color_origin_token=b_token;forged_owner.reset();
    require(xrfg::resolve_luke_guide_token(read(b3),fixture.queue(),fixture.device())!=nullptr,
        "restored exact native origin could not resolve after ambiguity rejection");
    auto a4=publish(90499,4,10);
    require(!xrfg::resolve_luke_guide_token(read(a4),fixture.queue(),fixture.device()),
        "multi-handle join crossed a nonconsecutive native publication interval");

    // The barcode still decodes after its owner's epoch reset or retirement;
    // its now-invalid immutable identity must never attach to another owner.
    xrfg::invalidate_dlss_motion_vector_stream_history(90500);
    require(!xrfg::resolve_luke_guide_token(read(b3),fixture.queue(),fixture.device()) &&
        xrfg::resolve_luke_guide_token(read(a3),fixture.queue(),fixture.device())!=nullptr,
        "native failure reset leaked ownership across independent streams");
    xrfg::retire_dlss_motion_vector_stream(90500);
    require(!xrfg::resolve_luke_guide_token(read(b3),fixture.queue(),fixture.device()),
        "retired native handle's color remained resolvable");
    old_a.reset();cross_queue.reset();a1.frame.reset();a2.frame.reset();a4.frame.reset();
    for(unsigned n=5;n<=24;++n)static_cast<void>(publish(90499,n,10));
    require(!xrfg::resolve_luke_guide_token(read(a3),fixture.queue(),fixture.device()),
        "evicted native color fell back to a newer retained guide");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_luke_color_origin_extent(0,0);
    std::cout<<"Multi-handle native color: actual CRC transport, exact own N-1/N-2, old/current pixels, crossqueue/device/interval/reset/retirement/eviction guards PASS\n";
}

void test_gta_origin_continuity(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    auto color=create_source_texture(fixture),motion=create_source_texture(fixture),depth=create_source_texture(fixture);
    xrfg::LukeOriginContinuity continuity;unsigned continued=0;
    std::vector<std::shared_ptr<const xrfg::DlssMotionVectorFrame>> pins;
    for(unsigned n=1;n<=12;++n) {
        upload_pattern(fixture,motion.Get(),make_pattern(static_cast<std::uint8_t>(n*3)));
        upload_pattern(fixture,depth.Get(),make_pattern(static_cast<std::uint8_t>(n*5)));
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){xrfg::DlssMotionVectorPublication p;
            p.stream=91500;p.output=color.Get();p.motion_vectors=motion.Get();p.depth=depth.Get();
            p.producer_queue=fixture.queue();p.producer_command_list=list;p.protect_snapshot_lifetime=true;p.raw_motion_native_span=1;
            p.output_width=p.motion_width=p.depth_width=kWidth;p.output_height=p.motion_height=p.depth_height=kHeight;
            p.resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.camera_frame_token_valid=p.camera_data_valid=p.camera_matrices_valid=true;p.camera_frame_token=1000+n;
            p.camera_right={1,0,0};p.camera_up={0,1,0};p.camera_forward={0,0,-1};p.camera_position={float(n)*.2F,0,0};
            p.camera_view_to_clip[0]=p.camera_view_to_clip[5]=1;p.camera_view_to_clip[11]=-1;p.camera_view_to_clip[14]=.1F;
            p.camera_clip_to_view[0]=p.camera_clip_to_view[5]=1;p.camera_clip_to_view[11]=10;p.camera_clip_to_view[14]=-1;
            p.frame_time_delta_ms=16;p.reset=n==8;frame=xrfg::publish_dlss_motion_vectors(p);});
        require(frame!=nullptr,"continuity original native publication missing");pins.push_back(frame);
        if(n<3)continue;
        const unsigned step=n-3;xrfg::LukeOriginSequenceFrame stamp;stamp.stream=frame->stream;stamp.epoch=frame->epoch;
        stamp.publication=frame->publication;stamp.native=frame->camera_frame_token;stamp.token=frame->color_origin_token;
        stamp.eye=step&1;stamp.owner=99;stamp.contexts={10,20};stamp.captures={50+step/2,50+(step+1)/2};stamp.shape={kWidth,kHeight,kWidth,kHeight};
        if(n<=4){require(xrfg::resolve_luke_guide_token(stamp.token,fixture.queue(),fixture.device())!=nullptr,"CRC anchor did not resolve its own native history");continuity.confirm(stamp,true,n*11);continue;}
        const auto prediction=continuity.predict(n*11,stamp.eye,stamp.captures,stamp.contexts);
        require(prediction&&xrfg::LukeOriginContinuity::matches(prediction->frame,stamp),"continued key differs from original produced key");
        const auto guide=xrfg::resolve_luke_guide_token(prediction->frame.token,fixture.queue(),fixture.device());
        if(n==8){require(!guide,"continuity crossed a reset into old camera/vector history");continue;}
        require(guide&&guide->publication==stamp.publication&&guide->camera_frame_token==stamp.native&&guide->previous_opposite_guide&&
            guide->previous_opposite_guide->epoch==guide->epoch&&guide->previous_opposite_guide->publication+1==guide->publication,
            "continued token did not resolve exact fresh native guides");
        require_readback_matches(fixture,guide->motion_vectors.Get(),make_pattern(static_cast<std::uint8_t>(n*3)),"continued native MV is fresh",guide->resource_state);
        require_readback_matches(fixture,guide->previous_opposite_guide->depth.Get(),make_pattern(static_cast<std::uint8_t>((n-1)*5)),"continued opposite depth is exact",guide->previous_opposite_guide->depth_resource_state);
        continuity.used();++continued;
        if(n==12)require(!continuity.confirm(stamp,true,n*11),"matching CRC recovery reset exact continued GPU history");
    }
    require(continued==7,"continuity GPU fixture did not cover the expected new native frames");
    require(!xrfg::resolve_luke_guide_token(UINT32_MAX,fixture.queue(),fixture.device()),"unknown sequence key fell back to latest guide");
    xrfg::configure_dlss_motion_vector_tracking(false);pins.clear();xrfg::retire_disabled_dlss_motion_vector_resources();
    std::cout<<"GTA continuation GPU: actual new MV/depth/fence keys, reset refusal, own-epoch recovery and matching CRC return PASS\n";
}

void test_luke_interleaved_origins(D3D12WarpFixture& fixture, unsigned native_span=1, bool reset_transition=false) {
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    auto color=create_source_texture(fixture),motion=create_source_texture(fixture),depth=create_source_texture(fixture);
    std::vector<std::shared_ptr<const xrfg::DlssMotionVectorFrame>> produced;
    const auto publish=[&](unsigned n, unsigned token=0, bool reset=false) {
        upload_pattern(fixture,motion.Get(),make_pattern(static_cast<std::uint8_t>(n*3)));
        upload_pattern(fixture,depth.Get(),make_pattern(static_cast<std::uint8_t>(n*5)));
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            xrfg::DlssMotionVectorPublication p;
            p.raw_motion_native_span=native_span;
            p.stream=91000+(n&1);p.output=color.Get();p.motion_vectors=motion.Get();p.depth=depth.Get();
            p.producer_queue=fixture.queue();p.producer_command_list=list;
            p.output_width=p.motion_width=p.depth_width=kWidth;
            p.output_height=p.motion_height=p.depth_height=kHeight;
            p.resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.protect_snapshot_lifetime=true;p.reset=reset;
            p.camera_frame_token_valid=p.camera_data_valid=p.camera_matrices_valid=true;
            p.camera_frame_token=token?token:n+100;
            p.camera_right={1,0,0};p.camera_up={0,1,0};p.camera_forward={0,0,-1};
            p.camera_position={n*.2F+((n&1)?-.5F:.5F)*6.4F,0,0};
            p.camera_view_to_clip[0]=p.camera_view_to_clip[5]=1;p.camera_view_to_clip[11]=-1;p.camera_view_to_clip[14]=.1F;
            p.camera_clip_to_view[0]=p.camera_clip_to_view[5]=1;p.camera_clip_to_view[11]=10;p.camera_clip_to_view[14]=-1;
            p.frame_time_delta_ms=16;
            frame=xrfg::publish_dlss_motion_vectors(p);
        });
        require(frame!=nullptr,"interleaved publication");
        return frame;
    };
    for(unsigned n=1;n<=24;++n) {
        auto frame=publish(n);produced.push_back(frame);
        if(n<4)continue;
        const auto joined=xrfg::resolve_luke_guide_token(frame->color_origin_token,fixture.queue(),fixture.device());
        require(joined && joined->luke_interleaved_history && joined->stream==frame->stream &&
            joined->serial==frame->serial && joined->previous_opposite_guide && joined->previous_same_guide &&
            joined->previous_opposite_guide->stream!=joined->stream &&
            joined->previous_same_guide->stream==joined->stream &&
            joined->previous_opposite_guide->camera_frame_token==n+99 &&
            joined->previous_same_native_camera && joined->previous_same_native_camera->token==n+98 &&
            joined->previous_serial==joined->previous_same_guide->serial,
            "interleaved exact A/B/A camera history was rejected or relabelled");
        require_readback_matches(fixture,joined->motion_vectors.Get(),make_pattern(static_cast<std::uint8_t>(n*3)),
            "interleaved current MV",joined->resource_state);
        require_readback_matches(fixture,joined->previous_opposite_guide->depth.Get(),make_pattern(static_cast<std::uint8_t>((n-1)*5)),
            "interleaved exact N-1 depth",joined->previous_opposite_guide->depth_resource_state);
        if(n>=22)require(std::abs(joined->calibrated_game_eye_baseline-6.4F)<.0001F,
            "interleaved production calibration did not cancel camera translation");
    }
    if(reset_transition) {
        const auto token23=produced[22]->color_origin_token,token24=produced[23]->color_origin_token;
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> last;
        {
        const auto old23=produced[22],old24=produced[23];
        const auto reset25=publish(25,0,true);
        require(reset25->epoch==old23->epoch+1 && reset25->serial==1,
            "GTA fixture did not reproduce one native eye reset before delayed XR consumption");
        require(xrfg::luke_native_stream_reset(old23->stream,old23->epoch,fixture.device()) &&
            !xrfg::luke_native_stream_reset(old23->stream,reset25->epoch,fixture.device()) &&
            !xrfg::luke_native_stream_reset(old23->stream,old23->epoch,nullptr),
            "reset control preflight lost its exact accepted owner/epoch");
        const auto metadata=xrfg::luke_native_origin_metadata(reset25->color_origin_token,fixture.device());
        require(metadata&&metadata->stream==reset25->stream&&metadata->epoch==reset25->epoch&&
            metadata->publication==reset25->publication&&metadata->serial==1&&
            !xrfg::luke_native_origin_metadata(0,fixture.device())&&
            !xrfg::luke_native_origin_metadata(reset25->color_origin_token,nullptr),
            "reset metadata guessed a key or admitted a foreign consumer");
        const auto delayed24=xrfg::resolve_luke_guide_token(token24,fixture.queue(),fixture.device());
        require(delayed24 && delayed24->publication==old24->publication && delayed24->previous_opposite_guide &&
            delayed24->previous_opposite_guide->publication==old23->publication && delayed24->previous_same_guide &&
            delayed24->previous_same_guide->publication==produced[21]->publication,
            "GTA native reset discarded the exact in-flight old color N-1/N-2");
        const auto delayed23=xrfg::resolve_luke_guide_token(token23,fixture.queue(),fixture.device());
        require(delayed23 && delayed23->epoch==old23->epoch && delayed23->serial==old23->serial &&
            delayed23->previous_opposite_guide->publication==produced[21]->publication,
            "GTA old color was relabelled with the newest owner epoch");
        require_readback_matches(fixture,delayed24->motion_vectors.Get(),make_pattern(72),
            "GTA reset retained exact old MV",delayed24->resource_state);
        require_readback_matches(fixture,delayed24->previous_opposite_guide->depth.Get(),make_pattern(115),
            "GTA reset retained exact old N-1 depth",delayed24->previous_opposite_guide->depth_resource_state);
        require(!xrfg::resolve_luke_guide_token(reset25->color_origin_token,fixture.queue(),fixture.device()),
            "GTA new reset epoch borrowed the old opposite history");
        const auto next26=publish(26),next27=publish(27);
        require(!xrfg::resolve_luke_guide_token(next26->color_origin_token,fixture.queue(),fixture.device()) &&
            !xrfg::resolve_luke_guide_token(next27->color_origin_token,fixture.queue(),fixture.device()),
            "GTA incomplete/reset A/B/A interval was admitted");
        last=publish(28);
        auto recovered=xrfg::resolve_luke_guide_token(last->color_origin_token,fixture.queue(),fixture.device());
        require(recovered && recovered->publication==last->publication && recovered->previous_opposite_guide &&
            recovered->previous_opposite_guide->publication==next27->publication &&
            recovered->previous_same_guide->publication==next26->publication,
            "GTA new epoch failed to recover its exact new A/B/A history");
        }
        // Release test pins; only real registry ownership may limit reuse now.
        // No unbounded history: old keys expire naturally under the same ring limit.
        produced.clear();
        for(unsigned n=29;n<=56;++n)last=publish(n);
        require(!xrfg::resolve_luke_guide_token(token24,fixture.queue(),fixture.device()) &&
            !xrfg::resolve_luke_guide_token(token23,fixture.queue(),fixture.device()),
            "GTA reset history escaped the bounded exact-color cache");
        const auto recovered=xrfg::resolve_luke_guide_token(last->color_origin_token,fixture.queue(),fixture.device());
        require(recovered!=nullptr,"GTA bounded history blocked ongoing producer slots");
        xrfg::invalidate_dlss_motion_vector_stream_history(last->stream);
        require(!xrfg::resolve_luke_guide_token(last->color_origin_token,fixture.queue(),fixture.device()),
            "GTA explicit failed-native invalidation retained a stale color");
        xrfg::configure_dlss_motion_vector_tracking(false);
        xrfg::retire_disabled_dlss_motion_vector_resources();
        std::cout<<"GTA reset: delayed exact GPU guides, old epoch identity, strict new prime, bounded eviction and explicit invalidation PASS\n";
        return;
    }
    auto bad=publish(25,999);
    require(!xrfg::resolve_luke_guide_token(bad->color_origin_token,fixture.queue(),fixture.device()),
        "interleaved token discontinuity accepted");
    const auto good_token=produced.back()->color_origin_token;
    xrfg::invalidate_dlss_motion_vector_stream_history(produced[22]->stream);
    require(!xrfg::resolve_luke_guide_token(good_token,fixture.queue(),fixture.device()),
        "interleaved retired opposite epoch remained usable");
    xrfg::configure_dlss_motion_vector_tracking(false);
    produced.clear();xrfg::retire_disabled_dlss_motion_vector_resources();
    std::cout<<"Interleaved A/B/A: original identities, exact GPU pixels, calibration, missing token and opposite epoch rejection PASS\n";
}

void test_luke_production_camera_units(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    auto color = create_source_texture(fixture);
    auto motion = create_source_texture(fixture);
    auto depth = create_source_texture(fixture);
    std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
    const auto publish = [&](unsigned n, float baseline, bool reset = false) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            xrfg::DlssMotionVectorPublication p;
            p.stream = 90059; p.output = color.Get(); p.motion_vectors = motion.Get();
            p.depth = depth.Get(); p.producer_queue = fixture.queue(); p.producer_command_list = list;
            p.output_width = p.motion_width = p.depth_width = kWidth;
            p.output_height = p.motion_height = p.depth_height = kHeight;
            p.resource_state = p.depth_resource_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            p.protect_snapshot_lifetime = true; p.reset = reset;
            p.camera_data_valid = true; p.camera_right = {1, 0, 0};
            p.camera_position = {n * .2F + ((n & 1) ? -.5F : .5F) * baseline, 0, 0};
            p.frame_time_delta_ms = 16;
            frame = xrfg::publish_dlss_motion_vectors(p);
        });
        require(frame != nullptr, "camera-unit protected production failed");
        return frame;
    };
    std::shared_ptr<const xrfg::DlssMotionVectorFrame> earlier;
    for (unsigned n = 1; n <= 12; ++n) {
        auto produced = publish(n, 6.4F);
        if (n < 11) require(produced->calibrated_game_eye_baseline == 0,
            "camera calibration admitted before nine independent ABA productions");
        else require(std::abs(produced->calibrated_game_eye_baseline - 6.4F) < .0001F,
            "producer calibration did not cancel linear translation");
        if (n == 9) {
            earlier = produced;
            for (unsigned query = 0; query != 12; ++query) {
                const auto joined = xrfg::resolve_luke_guide_token(
                    earlier->color_origin_token, fixture.queue(), fixture.device());
                require(joined && joined->calibrated_game_eye_baseline == 0,
                    "repeated consumers supplied false calibration confidence");
            }
        }
    }
    {
        const auto old = xrfg::resolve_luke_guide_token(
            earlier->color_origin_token, fixture.queue(), fixture.device());
        const auto newest = xrfg::resolve_luke_guide_token(
            frame->color_origin_token, fixture.queue(), fixture.device());
        require(old && old->calibrated_game_eye_baseline == 0 &&
            earlier->calibrated_game_eye_baseline == 0 && newest &&
            std::abs(newest->calibrated_game_eye_baseline - 6.4F) < .0001F,
            "old color identity borrowed newer camera calibration");
    }
    earlier.reset();
    publish(13, 12.8F, true);
    for (unsigned n = 14; n <= 24; ++n) {
        const auto produced = publish(n, 12.8F);
        if (n < 24) require(produced->calibrated_game_eye_baseline == 0,
            "producer epoch reset retained stale unit calibration");
        else require(std::abs(produced->calibrated_game_eye_baseline - 12.8F) < .0001F,
            "new producer epoch could not recalibrate its units");
    }
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    std::cout << "Camera units: production-order ABA, skipped/repeated/older consumers, immutable barcode join, epoch reset PASS\n";
}

void test_stereo_capture_ring(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "D3D12SwapchainHistory::initialize failed");
    require(history.initialized(), "history did not report initialized state");
    require(xrfg::D3D12SwapchainHistory::kSlotCount == 3, "history ring is not three slots");

    const StereoPattern pattern_a = make_pattern(11);
    const StereoPattern pattern_b = make_pattern(73);
    const StereoPattern pattern_c = make_pattern(149);

    upload_pattern(fixture, sources[2].Get(), pattern_a);
    xrfg::D3D12HistoryCaptureTicket ticket_a{};
    require(
        operation_succeeded(history.capture(2, &ticket_a)),
        "capture A failed");
    require(
        ticket_a.serial == 1 && ticket_a.slot == 0 && ticket_a.source_index == 2 &&
            ticket_a.fence_value != 0,
        "capture A ticket is incorrect");
    require(operation_succeeded(history.commit(ticket_a)), "commit A failed");
    CaptureResources capture_a = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a.fence.Get(), ticket_a.fence_value);
    require_private_resource_shape(sources[2].Get(), capture_a.resource.Get());
    require_readback_matches(fixture, capture_a.resource.Get(), pattern_a, "capture A");
    retire_completed_inspection(fixture, history, capture_a);

    upload_pattern(fixture, sources[0].Get(), pattern_b);
    xrfg::D3D12HistoryCaptureTicket ticket_b{};
    require(
        operation_succeeded(history.capture(0, &ticket_b)),
        "capture B failed");
    require(
        ticket_b.serial == 2 && ticket_b.slot == 1 && ticket_b.source_index == 0 &&
            ticket_b.fence_value > ticket_a.fence_value,
        "capture B ticket is incorrect");
    require(operation_succeeded(history.commit(ticket_b)), "commit B failed");
    CaptureResources capture_b = acquire_capture_for_inspection(history, ticket_b);
    fixture.wait_for_fence(capture_b.fence.Get(), ticket_b.fence_value);
    require(capture_a.resource.Get() != capture_b.resource.Get(), "A and B use the same history slot");
    require_private_resource_shape(sources[0].Get(), capture_b.resource.Get());
    require_readback_matches(fixture, capture_b.resource.Get(), pattern_b, "capture B");
    retire_completed_inspection(fixture, history, capture_b);
    CaptureResources capture_a_after_b = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a_after_b.fence.Get(), ticket_a.fence_value);
    require_readback_matches(
        fixture,
        capture_a_after_b.resource.Get(),
        pattern_a,
        "capture A after B");
    retire_completed_inspection(fixture, history, capture_a_after_b);

    const StereoPattern discarded_pattern = make_pattern(103);
    upload_pattern(fixture, sources[1].Get(), discarded_pattern);
    xrfg::D3D12HistoryCaptureTicket discarded_ticket{};
    require(
        operation_succeeded(history.capture(1, &discarded_ticket)),
        "discard candidate capture failed");
    require(
        discarded_ticket.serial == 3 && discarded_ticket.slot == 2 &&
            discarded_ticket.source_index == 1 &&
            discarded_ticket.fence_value > ticket_b.fence_value,
        "discard candidate ticket is incorrect");
    history.discard(discarded_ticket);

    xrfg::D3D12HistoryConsumerLease discarded_lease{};
    ID3D12Resource* discarded_resource = nullptr;
    ID3D12Fence* discarded_fence = nullptr;
    require(
        !operation_succeeded(history.acquire_consumer(
            discarded_ticket,
            &discarded_lease,
            &discarded_resource,
            &discarded_fence)),
        "discarded ticket remained valid");
    require(
        discarded_resource == nullptr && discarded_fence == nullptr,
        "discarded ticket returned COM objects");
    require(
        operation_succeeded(history.wait_for_idle()),
        "discarded capture did not drain");

    upload_pattern(fixture, sources[2].Get(), pattern_c);
    xrfg::D3D12HistoryCaptureTicket ticket_c{};
    require(
        operation_succeeded(history.capture(2, &ticket_c)),
        "capture C failed");
    require(
        ticket_c.serial == 3 && ticket_c.slot == 2 && ticket_c.source_index == 2 &&
            ticket_c.fence_value > discarded_ticket.fence_value,
        "capture C ticket is incorrect");
    require(
        ticket_c.serial == discarded_ticket.serial && ticket_c.slot == discarded_ticket.slot,
        "capture after discard did not reuse the same serial and slot");
    require(operation_succeeded(history.commit(ticket_c)), "commit C failed");
    CaptureResources capture_c = acquire_capture_for_inspection(history, ticket_c);
    fixture.wait_for_fence(capture_c.fence.Get(), ticket_c.fence_value);
    require(
        capture_c.resource.Get() != capture_a.resource.Get() &&
            capture_c.resource.Get() != capture_b.resource.Get(),
        "capture C did not use the independent third history slot");
    require_readback_matches(fixture, capture_c.resource.Get(), pattern_c, "capture C");
    retire_completed_inspection(fixture, history, capture_c);
    CaptureResources capture_b_after_c = acquire_capture_for_inspection(history, ticket_b);
    fixture.wait_for_fence(capture_b_after_c.fence.Get(), ticket_b.fence_value);
    require_readback_matches(
        fixture,
        capture_b_after_c.resource.Get(),
        pattern_b,
        "capture B after C");
    retire_completed_inspection(fixture, history, capture_b_after_c);
    CaptureResources capture_a_after_c = acquire_capture_for_inspection(history, ticket_a);
    fixture.wait_for_fence(capture_a_after_c.fence.Get(), ticket_a.fence_value);
    require_readback_matches(
        fixture,
        capture_a_after_c.resource.Get(),
        pattern_a,
        "capture A retained across the third slot");
    retire_completed_inspection(fixture, history, capture_a_after_c);

    const HRESULT reinitialize_result = history.initialize(
        fixture.device(),
        fixture.queue(),
        std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(FAILED(reinitialize_result), "second initialize unexpectedly succeeded");
    CaptureResources capture_b_after_reinitialize =
        acquire_capture_for_inspection(history, ticket_b);
    CaptureResources capture_c_after_reinitialize =
        acquire_capture_for_inspection(history, ticket_c);
    require(
        capture_b_after_reinitialize.resource.Get() == capture_b.resource.Get() &&
            capture_c_after_reinitialize.resource.Get() == capture_c.resource.Get(),
        "failed reinitialize invalidated or replaced committed captures");
    history.cancel_consumer(capture_b_after_reinitialize.lease);
    history.cancel_consumer(capture_c_after_reinitialize.lease);

    require(operation_succeeded(history.invalidate()), "history invalidate failed");
    for (const auto& invalidated_ticket : {ticket_b, ticket_c}) {
        xrfg::D3D12HistoryConsumerLease invalidated_lease{};
        ID3D12Resource* invalidated_resource = nullptr;
        ID3D12Fence* invalidated_fence = nullptr;
        require(
            !operation_succeeded(history.acquire_consumer(
                invalidated_ticket,
                &invalidated_lease,
                &invalidated_resource,
                &invalidated_fence)),
            "invalidate left an old committed ticket resolvable");
        require(
            invalidated_resource == nullptr && invalidated_fence == nullptr,
            "invalidated ticket returned COM objects");
    }

    const StereoPattern pattern_d = make_pattern(181);
    upload_pattern(fixture, sources[1].Get(), pattern_d);
    xrfg::D3D12HistoryCaptureTicket ticket_d{};
    require(
        operation_succeeded(history.capture(1, &ticket_d)),
        "capture after invalidate failed");
    require(
        ticket_d.serial > ticket_c.serial && ticket_d.serial == 4 &&
            ticket_d.slot == 0 && ticket_d.source_index == 1 &&
            ticket_d.fence_value > ticket_c.fence_value,
        "capture after invalidate reset serial/fence identity");
    require(
        operation_succeeded(history.commit(ticket_d)),
        "capture after invalidate commit failed");
    CaptureResources capture_d = acquire_capture_for_inspection(history, ticket_d);
    fixture.wait_for_fence(capture_d.fence.Get(), ticket_d.fence_value);
    require_readback_matches(fixture, capture_d.resource.Get(), pattern_d, "capture D after invalidate");
    retire_completed_inspection(fixture, history, capture_d);

    xrfg::D3D12HistoryConsumerLease stale_lease{};
    ID3D12Resource* stale_resource = nullptr;
    ID3D12Fence* stale_fence = nullptr;
    require(
        !operation_succeeded(history.acquire_consumer(
            ticket_a,
            &stale_lease,
            &stale_resource,
            &stale_fence)),
        "overwritten capture A ticket remained valid");
    require(
        stale_resource == nullptr && stale_fence == nullptr,
        "rejected stale ticket returned COM objects");

    require_source_is_render_target(fixture, sources[1].Get());
    require(operation_succeeded(history.wait_for_idle()), "history wait_for_idle failed");
}

void test_capture_is_async_and_consumer_fence_blocks_reuse(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };
    const StereoPattern pattern = make_pattern(211);
    upload_pattern(fixture, sources[0].Get(), pattern);

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "gate history initialization failed");

    ComPtr<ID3D12Fence> gate;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(gate.GetAddressOf())),
        "ID3D12Device::CreateFence(gate)");
    require_hresult(fixture.queue()->Wait(gate.Get(), 1), "ID3D12CommandQueue::Wait(gate)");

    struct CaptureOutcome {
        HRESULT result{E_FAIL};
        xrfg::D3D12HistoryCaptureTicket ticket{};
    };
    std::promise<void> capture_entered;
    std::future<void> entered = capture_entered.get_future();
    std::future<CaptureOutcome> capture = std::async(
        std::launch::async,
        [&history, promise = std::move(capture_entered)]() mutable {
            promise.set_value();
            CaptureOutcome outcome{};
            outcome.result = history.capture(0, &outcome.ticket);
            return outcome;
        });
    require(
        entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "capture worker did not start");

    const bool returned_while_gpu_blocked =
        capture.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
    if (!returned_while_gpu_blocked) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(gate cleanup)");
        fail("capture blocked the CPU on its producer fence");
    }

    const CaptureOutcome outcome = capture.get();
    require(operation_succeeded(outcome.result), "asynchronous gated capture failed");
    require(operation_succeeded(history.commit(outcome.ticket)), "gated capture commit failed");

    xrfg::D3D12HistoryConsumerLease lease{};
    ID3D12Resource* raw_resource = nullptr;
    ID3D12Fence* raw_producer_fence = nullptr;
    require(
        operation_succeeded(history.acquire_consumer(
            outcome.ticket,
            &lease,
            &raw_resource,
            &raw_producer_fence)),
        "failed to acquire the asynchronous history consumer");
    ComPtr<ID3D12Resource> leased_resource;
    ComPtr<ID3D12Fence> producer_fence;
    leased_resource.Attach(raw_resource);
    producer_fence.Attach(raw_producer_fence);
    require(
        producer_fence->GetCompletedValue() < outcome.ticket.fence_value,
        "capture producer fence completed before the gated assertion");

    ComPtr<ID3D12Fence> consumer_completion;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(consumer_completion.GetAddressOf())),
        "ID3D12Device::CreateFence(consumer completion)");
    require(
        operation_succeeded(history.retire_consumer(lease, consumer_completion.Get(), 1)),
        "consumer lease retirement failed");

    xrfg::D3D12HistoryCaptureTicket ticket_b{};
    require(
        operation_succeeded(history.capture(1, &ticket_b)) &&
            operation_succeeded(history.commit(ticket_b)),
        "the independent second ring slot did not accept an asynchronous capture");

    xrfg::D3D12HistoryCaptureTicket ticket_c{};
    require(
        operation_succeeded(history.capture(2, &ticket_c)) &&
            operation_succeeded(history.commit(ticket_c)),
        "the independent third ring slot did not accept an asynchronous capture");
    require(
        ticket_c.serial == 3 && ticket_c.slot == 2,
        "third asynchronous capture did not preserve rolling chronology");

    xrfg::D3D12HistoryCaptureTicket blocked_ticket{};
    require(
        history.capture(0, &blocked_ticket) == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            blocked_ticket.serial == 0,
        "history overwrote a slot protected by an incomplete consumer fence");

    require_hresult(gate->Signal(1), "ID3D12Fence::Signal(gate)");
    fixture.wait_for_fence(producer_fence.Get(), ticket_c.fence_value);
    require(
        history.capture(0, &blocked_ticket) == HRESULT_FROM_WIN32(ERROR_BUSY),
        "producer completion bypassed the independent consumer fence");

    require_hresult(
        consumer_completion->Signal(1),
        "ID3D12Fence::Signal(consumer completion)");
    xrfg::D3D12HistoryCaptureTicket ticket_d{};
    require(
        operation_succeeded(history.capture(0, &ticket_d)) &&
            operation_succeeded(history.commit(ticket_d)),
        "history slot did not become reusable after consumer completion");
    require(
        ticket_d.slot == outcome.ticket.slot && ticket_d.serial == 4,
        "history did not preserve chronological ring identity after lease retirement");
    require(operation_succeeded(history.wait_for_idle()), "asynchronous history drain failed");
    require_source_is_render_target(fixture, sources[0].Get());
}

void test_depth_capture_path(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_depth_source_texture(fixture),
        create_depth_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
    };
    constexpr std::array<float, kEyeCount> kCapturedDepths{0.25F, 0.75F};
    clear_depth_slices(fixture, sources[1].Get(), kCapturedDepths);

    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_DEPTH_WRITE)),
        "depth history initialization failed");

    xrfg::D3D12HistoryCaptureTicket ticket{};
    require(
        operation_succeeded(history.capture(1, &ticket)),
        "depth capture failed");
    require(
        ticket.serial == 1 && ticket.slot == 0 && ticket.source_index == 1 &&
            ticket.fence_value != 0,
        "depth capture ticket is incorrect");
    require(operation_succeeded(history.commit(ticket)), "depth capture commit failed");

    CaptureResources capture = acquire_capture_for_inspection(history, ticket);
    fixture.wait_for_fence(capture.fence.Get(), ticket.fence_value);
    require_private_resource_shape(sources[1].Get(), capture.resource.Get());
    require_depth_readback_matches(fixture, capture.resource.Get(), kCapturedDepths);
    retire_completed_inspection(fixture, history, capture);

    // No transition is recorded here: the debug layer validates that capture restored
    // the OpenXR-required DEPTH_WRITE state before returning.
    constexpr std::array<float, kEyeCount> kPostCaptureDepths{0.5F, 0.625F};
    clear_depth_slices(fixture, sources[1].Get(), kPostCaptureDepths);
    require(
        operation_succeeded(history.wait_for_idle()),
        "depth history wait_for_idle failed");
}

void test_depth_private_history_allows_shader_resource_views(D3D12WarpFixture& fixture) {
    ComPtr<ID3D12Resource> source = create_depth_source_texture(
        fixture,
        D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    std::array<ID3D12Resource*, 1> source_pointers{source.Get()};
    require(
        (source->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0,
        "DENY_SHADER_RESOURCE source fixture is missing its flag");

    constexpr std::array<float, kEyeCount> kDepths{0.375F, 0.875F};
    clear_depth_slices(fixture, source.Get(), kDepths);
    xrfg::D3D12SwapchainHistory history;
    require(
        operation_succeeded(history.initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_DEPTH_WRITE)),
        "DENY_SHADER_RESOURCE history initialization failed");

    xrfg::D3D12HistoryCaptureTicket ticket{};
    require(
        operation_succeeded(history.capture(0, &ticket)),
        "DENY_SHADER_RESOURCE capture failed");
    require(
        operation_succeeded(history.commit(ticket)),
        "DENY_SHADER_RESOURCE capture commit failed");
    CaptureResources capture = acquire_capture_for_inspection(history, ticket);
    fixture.wait_for_fence(capture.fence.Get(), ticket.fence_value);
    require(
        (capture.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0,
        "private history retained DENY_SHADER_RESOURCE");
    require(
        (capture.resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0,
        "private history lost depth-stencil compatibility");
    require_depth_readback_matches(fixture, capture.resource.Get(), kDepths);
    retire_completed_inspection(fixture, history, capture);
    constexpr std::array<float, kEyeCount> kPostCaptureDepths{0.125F, 0.625F};
    clear_depth_slices(fixture, source.Get(), kPostCaptureDepths);
    require(
        operation_succeeded(history.wait_for_idle()),
        "shader-readable history wait_for_idle failed");
}

void test_copy_only_afw_transport(D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx, bool deferred=false) {
    std::array<ComPtr<ID3D12Resource>,3> sources{
        create_source_texture(fixture), create_source_texture(fixture), create_source_texture(fixture)};
    std::array<ComPtr<ID3D12Resource>,3> destinations{
        create_source_texture(fixture), create_source_texture(fixture), create_source_texture(fixture)};
    std::array<ComPtr<ID3D12Resource>,3> synthetic{
        create_source_texture(fixture), create_source_texture(fixture), create_source_texture(fixture)};
    std::array<ID3D12Resource*,3> source_ptrs{sources[0].Get(),sources[1].Get(),sources[2].Get()};
    std::array<ID3D12Resource*,3> destination_ptrs{destinations[0].Get(),destinations[1].Get(),destinations[2].Get()};
    std::array<ID3D12Resource*,3> synthetic_ptrs{synthetic[0].Get(),synthetic[1].Get(),synthetic[2].Get()};
    const auto pattern_a=make_pattern(17), pattern_b=make_pattern(71), pattern_c=make_pattern(119);
    const auto sentinel=make_pattern(203);
    upload_pattern(fixture,sources[0].Get(),pattern_a);
    upload_pattern(fixture,sources[1].Get(),pattern_b);
    upload_pattern(fixture,sources[2].Get(),pattern_c);
    for(UINT i=0;i<3;++i) {
        upload_pattern(fixture,destinations[i].Get(),sentinel);
        upload_pattern(fixture,synthetic[i].Get(),sentinel);
    }
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),source_ptrs,
        D3D12_RESOURCE_STATE_RENDER_TARGET),"copy-only history initialize");
    xrfg::D3D12FrameSynthesizer synthesizer;
    // FidelityFX is deliberately selected: AFW transport must not dispatch its
    // generic prime packing/warmups, nor change any synthetic output pixels.
    require_hresult(synthesizer.initialize(fixture.device(),fixture.queue(),history,
        destination_ptrs,synthetic_ptrs,kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,
        backend,{},false,xrfg::D3D12FrameGenerationMode::ofxr,false,deferred),"copy-only synthesizer initialize");
    const auto views=make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{},capture_b{},capture_c{};
    require_hresult(history->capture(0,&capture_a),"copy-only capture A");
    require_hresult(history->commit(capture_a),"copy-only commit A");
    xrfg::D3D12FrameSynthesisTicket invalid{};
    require(FAILED(synthesizer.submit_copy(capture_a,
        std::span<const xrfg::D3D12ReprojectionView>(views.data(),1),0,&invalid)) &&
        invalid.current_serial==0 && invalid.fence_value==0,
        "copy-only accepted incomplete view metadata or consumed a ticket");
    require(FAILED(synthesizer.submit_copy(capture_a,views,3,&invalid)) &&
        invalid.current_serial==0 && invalid.fence_value==0,
        "copy-only accepted an invalid destination index");

    ComPtr<ID3D12Fence> gate;
    require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(gate.GetAddressOf())),"copy-only gate create");
    require_hresult(fixture.queue()->Wait(gate.Get(),1),"copy-only blocked queue");
    struct CopyOutcome { HRESULT result{E_FAIL}; xrfg::D3D12FrameSynthesisTicket ticket{}; };
    auto future=std::async(std::launch::async,[&] {
        CopyOutcome outcome;
        outcome.result=synthesizer.submit_copy(capture_a,views,2,&outcome.ticket);
        return outcome;
    });
    if(future.wait_for(std::chrono::milliseconds(500))!=std::future_status::ready) {
        require_hresult(gate->Signal(1),"copy-only timeout cleanup");
        (void)future.get();
        fail("copy-only submit waited on GPU completion");
    }
    const auto copied_a=future.get();
    // Observe everything while the queue is blocked, then release before any
    // assertion can unwind GPU-owned resources with an unsignaled test gate.
    const HRESULT pending_copy=synthesizer.wait_for_previous_submission(0);
    xrfg::D3D12HistoryConsumerLease blocked_lease{};
    ID3D12Resource* blocked_resource=nullptr;ID3D12Fence* blocked_fence=nullptr;
    const HRESULT pending_history=history->acquire_consumer(capture_a,&blocked_lease,
        &blocked_resource,&blocked_fence);
    if(SUCCEEDED(pending_history)) {
        history->cancel_consumer(blocked_lease);
        if(blocked_resource)blocked_resource->Release();
        if(blocked_fence)blocked_fence->Release();
    }
    const HRESULT capture_b_result=history->capture(1,&capture_b);
    const HRESULT commit_b_result=SUCCEEDED(capture_b_result)?history->commit(capture_b):capture_b_result;
    const HRESULT capture_c_result=history->capture(2,&capture_c);
    const HRESULT commit_c_result=SUCCEEDED(capture_c_result)?history->commit(capture_c):capture_c_result;
    xrfg::D3D12HistoryCaptureTicket blocked_capture{};
    const HRESULT pending_reuse=history->capture(0,&blocked_capture);
    require_hresult(gate->Signal(1),"copy-only release queue gate");
    require_hresult(copied_a.result,"copy-only asynchronous submit A");
    require(copied_a.ticket.previous_serial==0 && copied_a.ticket.current_serial==capture_a.serial &&
        copied_a.ticket.fence_value!=0 && copied_a.ticket.synthetic_fence_value==copied_a.ticket.fence_value &&
        copied_a.ticket.current_destination_index==2 &&
        copied_a.ticket.synthetic_destination_index==std::numeric_limits<std::uint32_t>::max(),
        "copy-only submission did not preserve exact ticket/fence/destination identity");
    require(FAILED(pending_copy),"copy-only completion fence passed the blocked queue");
    require(pending_history==HRESULT_FROM_WIN32(ERROR_BUSY),
        "copy-only retired its history lease without protecting pending GPU access");
    require_hresult(capture_b_result,"copy-only concurrent capture B");
    require_hresult(commit_b_result,"copy-only concurrent commit B");
    require_hresult(capture_c_result,"copy-only concurrent capture C");
    require_hresult(commit_c_result,"copy-only concurrent commit C");
    require(pending_reuse==HRESULT_FROM_WIN32(ERROR_BUSY) && blocked_capture.serial==0,
        "copy-only pending consumer allowed ring-slot overwrite");
    require_frame_start_gate(synthesizer,"copy-only completion A");
    require_readback_matches(fixture,destinations[2].Get(),pattern_a,
        "copy-only exact A",D3D12_RESOURCE_STATE_RENDER_TARGET);
    for(UINT i=0;i<3;++i)require_readback_matches(fixture,synthetic[i].Get(),sentinel,
        "copy-only synthetic untouched",D3D12_RESOURCE_STATE_RENDER_TARGET);
    // Completion, not a retained temporal anchor, owns this copy lease.
    CaptureResources inspected_a=acquire_capture_for_inspection(*history,capture_a);
    require_readback_matches(fixture,inspected_a.resource.Get(),pattern_a,"copy-only retained history A");
    retire_completed_inspection(fixture,*history,inspected_a);
    xrfg::D3D12FrameSynthesisTicket implicit_pair{};
    require(FAILED(synthesizer.submit_pair(capture_b,views,views,0,1,&implicit_pair)) &&
        implicit_pair.current_serial==0 && implicit_pair.fence_value==0,
        "copy-only transport secretly primed a temporal synthesis anchor");

    xrfg::D3D12FrameSynthesisTicket copied_b{};
    require_hresult(synthesizer.submit_copy(capture_b,views,1,&copied_b),"copy-only submit B");
    require(copied_b.previous_serial==0 && copied_b.current_serial==capture_b.serial &&
        copied_b.fence_value>copied_a.ticket.fence_value,
        "copy-only B did not advance its independent completion fence");
    require_frame_start_gate(synthesizer,"copy-only completion B");
    require_readback_matches(fixture,destinations[1].Get(),pattern_b,
        "copy-only exact B",D3D12_RESOURCE_STATE_RENDER_TARGET);
    require_readback_matches(fixture,destinations[2].Get(),pattern_a,
        "copy-only previous destination unchanged",D3D12_RESOURCE_STATE_RENDER_TARGET);
    for(UINT i=0;i<3;++i)require_readback_matches(fixture,synthetic[i].Get(),sentinel,
        "copy-only second synthetic untouched",D3D12_RESOURCE_STATE_RENDER_TARGET);
    require_source_is_render_target(fixture,sources[0].Get());
    require_source_is_render_target(fixture,sources[1].Get());

    // Returning to genuine OFXR must explicitly execute its original prime;
    // AFW transport neither substitutes it nor leaves a false warm anchor.
    if(deferred && backend==xrfg::D3D12OpticalFlowBackend::nvidia) {
        // WARP can copy, but cannot initialize hardware OFA. Its failure must
        // return without draining unrelated work blocked on a future gate.
        require_hresult(fixture.queue()->Wait(gate.Get(),2),"unavailable OFA gate");
        auto unavailable=std::async(std::launch::async,[&] {
            xrfg::D3D12FrameSynthesisTicket rejected{};
            return synthesizer.submit_prime(capture_c,views,0,&rejected);
        });
        const bool returned=unavailable.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
        require_hresult(gate->Signal(2),"unavailable OFA cleanup gate");
        const auto unsupported=unavailable.get();
        require(returned&&FAILED(unsupported),"unavailable OFA drained a gated queue or falsely generated");
        require_hresult(synthesizer.reconfigure(xrfg::D3D12OpticalFlowBackend::fidelity_fx,{}),
            "deferred NVIDIA transport can switch before actual flow initialization on WARP");
        require_hresult(synthesizer.submit_copy(capture_c,views,1,&copied_b),"new route deferred copy");
        require_frame_start_gate(synthesizer,"new route deferred copy completion");
    }
    const auto preserved_fence=synthesizer.submission_fence(copied_b);
    xrfg::D3D12FrameSynthesisTicket genuine_prime{};
    require_hresult(synthesizer.submit_prime(capture_c,views,0,&genuine_prime),
        "copy-only to genuine temporal prime");
    require(genuine_prime.previous_serial==0 && genuine_prime.current_serial==capture_c.serial &&
        genuine_prime.fence_value>copied_b.fence_value,
        "genuine prime ticket was altered by prior AFW copies");
    require(preserved_fence && synthesizer.submission_fence(copied_b).Get()==preserved_fence.Get() &&
        synthesizer.ticket_available(copied_b)==S_OK,"lazy upgrade invalidated a completed publication fence/ticket");
    require_frame_start_gate(synthesizer,"genuine prime completion after copies");
    require_readback_matches(fixture,destinations[0].Get(),pattern_c,
        "genuine prime current C",D3D12_RESOURCE_STATE_RENDER_TARGET);
    require_hresult(synthesizer.retire_previous(),"genuine prime retirement after copies");
    require_hresult(synthesizer.wait_for_idle(),"copy-only synthesizer drain");
    require_hresult(history->invalidate(),"copy-only leases released before invalidate");
}

void test_rolling_frame_synthesizer(D3D12WarpFixture& fixture) {
    std::array<ComPtr<ID3D12Resource>, 3> sources{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 3> current_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ComPtr<ID3D12Resource>, 3> synthetic_destinations{
        create_source_texture(fixture),
        create_source_texture(fixture),
        create_source_texture(fixture),
    };
    std::array<ID3D12Resource*, 3> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
        sources[2].Get(),
    };
    std::array<ID3D12Resource*, 3> current_destination_pointers{
        current_destinations[0].Get(),
        current_destinations[1].Get(),
        current_destinations[2].Get(),
    };
    std::array<ID3D12Resource*, 3> synthetic_destination_pointers{
        synthetic_destinations[0].Get(),
        synthetic_destinations[1].Get(),
        synthetic_destinations[2].Get(),
    };

    const StereoPattern pattern_a = make_solid_pattern({
        RgbaBytes{16, 40, 72, 255},
        RgbaBytes{32, 56, 88, 255},
    });
    const StereoPattern pattern_b = make_solid_pattern({
        RgbaBytes{80, 104, 136, 255},
        RgbaBytes{96, 120, 152, 255},
    });
    const StereoPattern pattern_c = make_solid_pattern({
        RgbaBytes{160, 184, 208, 255},
        RgbaBytes{176, 200, 224, 255},
    });
    const StereoPattern pattern_d = make_solid_pattern({
        RgbaBytes{24, 112, 200, 255},
        RgbaBytes{48, 136, 224, 255},
    });
    upload_pattern(fixture, sources[0].Get(), pattern_a);
    upload_pattern(fixture, sources[1].Get(), pattern_b);
    upload_pattern(fixture, sources[2].Get(), pattern_c);

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "rolling synthesizer history initialization failed");

    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_destination_pointers.data(),
                current_destination_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
             synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "rolling frame synthesizer initialization failed");
    require(synthesizer.initialized(), "rolling frame synthesizer did not report initialized");
    const ReprojectionViews static_views = make_reprojection_views();

    xrfg::D3D12FrameSynthesizer invalid_synthesizer;
    require(
        FAILED(invalid_synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            {},
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
                synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "synthesizer accepted an empty current-destination set");

    ComPtr<ID3D12Fence> gate;
    require_hresult(
        fixture.device()->CreateFence(
            0,
            D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(gate.GetAddressOf())),
        "ID3D12Device::CreateFence(synthesis gate)");
    require_hresult(
        fixture.queue()->Wait(gate.Get(), 1),
        "ID3D12CommandQueue::Wait(synthesis gate)");

    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "prime capture A failed");
    require(
        capture_a.serial == 1 && capture_a.slot == 0 && capture_a.source_index == 0,
        "prime capture A chronology is incorrect");

    xrfg::D3D12FrameSynthesisTicket invalid_prime{};
    require(
        FAILED(synthesizer.submit_prime(
            capture_a,
            std::span<const xrfg::D3D12ReprojectionView>(static_views.data(), 1),
            0,
            &invalid_prime)) &&
            invalid_prime.current_serial == 0 && invalid_prime.fence_value == 0,
        "prime accepted incomplete per-eye camera metadata");
    require(
        FAILED(synthesizer.submit_prime(
            capture_a,
            static_views,
            static_cast<std::uint32_t>(current_destinations.size()),
            &invalid_prime)) &&
            invalid_prime.current_serial == 0 && invalid_prime.fence_value == 0,
        "out-of-range prime submission was not rejected atomically");

    struct SynthesisOutcome {
        HRESULT result{E_FAIL};
        xrfg::D3D12FrameSynthesisTicket ticket{};
    };
    std::future<SynthesisOutcome> prime_future = std::async(
        std::launch::async,
        [&synthesizer, &capture_a, &static_views] {
            SynthesisOutcome outcome{};
            outcome.result =
                synthesizer.submit_prime(capture_a, static_views, 2, &outcome.ticket);
            return outcome;
        });
    if (prime_future.wait_for(std::chrono::milliseconds(500)) !=
        std::future_status::ready) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(prime timeout cleanup)");
        (void)prime_future.get();
        fail("prime submission blocked the CPU on GPU completion");
    }
    const SynthesisOutcome prime = prime_future.get();
    require(operation_succeeded(prime.result), "asynchronous prime submission failed");
    require(
        prime.ticket.previous_serial == 0 &&
            prime.ticket.current_serial == capture_a.serial &&
            prime.ticket.fence_value != 0 &&
            prime.ticket.synthetic_destination_index ==
                std::numeric_limits<std::uint32_t>::max() &&
            prime.ticket.current_destination_index == 2,
        "prime synthesis ticket is incorrect");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "pair capture B failed while prime work was queued");
    require(
        capture_b.serial == 2 && capture_b.slot == 1 && capture_b.source_index == 1 &&
            capture_b.fence_value > capture_a.fence_value,
        "capture B chronology is incorrect");

    xrfg::D3D12FrameSynthesisTicket invalid_pair{};
    const ReprojectionViews mismatched_target_views = make_reprojection_views(0.05F);
    require(
        FAILED(synthesizer.submit_pair(
            capture_b,
            static_views,
            mismatched_target_views,
            0,
            0,
            &invalid_pair)) &&
            invalid_pair.current_serial == 0 && invalid_pair.fence_value == 0,
        "synthesizer accepted a target camera different from render pose B");
    require(
        FAILED(synthesizer.submit_pair(
            capture_b,
            static_views,
            static_views,
            static_cast<std::uint32_t>(synthetic_destinations.size()),
            0,
            &invalid_pair)) &&
            invalid_pair.current_serial == 0 && invalid_pair.fence_value == 0,
        "out-of-range pair submission was not rejected atomically");

    // Release the deliberately blocked queue, then model V090's frame-start
    // gate before advancing the temporal pair. Submission itself remains
    // nonblocking and cannot accumulate another complete transaction.
    require_hresult(gate->Signal(1), "ID3D12Fence::Signal(synthesis gate)");
    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(500)),
        "frame-start synthesis gate did not settle the asynchronous prime");

    std::future<SynthesisOutcome> pair_future = std::async(
        std::launch::async,
        [&synthesizer, &capture_b, &static_views] {
            SynthesisOutcome outcome{};
            outcome.result = synthesizer.submit_pair(
                capture_b,
                static_views,
                static_views,
                2,
                1,
                &outcome.ticket);
            return outcome;
        });
    if (pair_future.wait_for(std::chrono::milliseconds(500)) !=
        std::future_status::ready) {
        require_hresult(gate->Signal(1), "ID3D12Fence::Signal(pair timeout cleanup)");
        (void)pair_future.get();
        fail("pair submission blocked the CPU on GPU completion");
    }
    const SynthesisOutcome pair_ab = pair_future.get();
    require(operation_succeeded(pair_ab.result), "asynchronous A/B pair submission failed");
    require(
        pair_ab.ticket.previous_serial == capture_a.serial &&
            pair_ab.ticket.current_serial == capture_b.serial &&
            pair_ab.ticket.fence_value > prime.ticket.fence_value &&
            pair_ab.ticket.work_slot != prime.ticket.work_slot &&
            pair_ab.ticket.synthetic_destination_index == 2 &&
            pair_ab.ticket.current_destination_index == 1,
        "A/B synthesis ticket is incorrect");

    require(
        operation_succeeded(synthesizer.wait_for_previous_submission(500)),
        "frame-start synthesis gate did not settle the A/B pair");
    xrfg::D3D12FrameSynthesisTicket repeated_pair{};
    const ReprojectionViews repeated_views = make_reprojection_views(0.025F);
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b,
            repeated_views,
            repeated_views,
            0,
            0,
            &repeated_pair)) &&
            repeated_pair.previous_serial == capture_b.serial &&
            repeated_pair.current_serial == capture_b.serial &&
            repeated_pair.fence_value > pair_ab.ticket.fence_value &&
            repeated_pair.synthetic_destination_index == 0 &&
            repeated_pair.current_destination_index == 0,
        "rolling synthesizer did not reuse an unchanged eye capture");

    xrfg::D3D12HistoryCaptureTicket capture_c{};
    require(
        operation_succeeded(history->capture(2, &capture_c)) &&
            operation_succeeded(history->commit(capture_c)),
        "third rolling slot did not accept C while A/B work was in flight");
    require(
        capture_c.serial == 3 && capture_c.slot == 2 && capture_c.source_index == 2 &&
            capture_c.fence_value > capture_b.fence_value,
        "capture C did not preserve A0/B1/C2 rolling chronology");

    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});

    require_readback_matches(
        fixture,
        current_destinations[2].Get(),
        pattern_a,
        "prime current A",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require_readback_matches(
        fixture,
        current_destinations[1].Get(),
        pattern_b,
        "pair current B",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern synthetic_ab = readback_pattern(
        fixture,
        synthetic_destinations[2].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(
        synthetic_ab != pattern_a && synthetic_ab != pattern_b,
        "A/B synthetic output did not remain temporally distinct");
    require_source_is_render_target(fixture, sources[0].Get());
    require_source_is_render_target(fixture, sources[1].Get());
    require_readback_matches(
        fixture,
        current_destinations[0].Get(),
        pattern_b,
        "repeated current B",
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    xrfg::D3D12FrameSynthesisTicket pair_bc{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_c,
            static_views,
            static_views,
            0,
            0,
            &pair_bc)),
        "B/C rolling pair submission failed");
    require(
        pair_bc.previous_serial == capture_b.serial &&
            pair_bc.current_serial == capture_c.serial &&
            pair_bc.fence_value > pair_ab.ticket.fence_value &&
            pair_bc.synthetic_destination_index == 0 &&
            pair_bc.current_destination_index == 0,
        "B/C synthesis ticket is incorrect");

    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    require_readback_matches(
        fixture,
        current_destinations[0].Get(),
        pattern_c,
        "pair current C",
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern synthetic_bc = readback_pattern(
        fixture,
        synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(
        synthetic_bc != pattern_b && synthetic_bc != pattern_c,
        "B/C synthetic output did not remain temporally distinct");
    require_source_is_render_target(fixture, sources[2].Get());

    upload_pattern(fixture, sources[0].Get(), pattern_d);
    xrfg::D3D12HistoryCaptureTicket capture_d{};
    require(
        operation_succeeded(history->capture(0, &capture_d)) &&
            operation_succeeded(history->commit(capture_d)),
        "capture D did not reuse slot zero after synthesis completion");
    require(
        capture_d.serial == 4 && capture_d.slot == 0 && capture_d.source_index == 0 &&
            capture_d.fence_value > capture_c.fence_value,
        "capture D did not preserve A0/B1/C2/D0 rolling chronology");

    xrfg::D3D12FrameSynthesisTicket repeated_c{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_c,
            static_views,
            static_views,
            1,
            1,
            &repeated_c)) &&
            repeated_c.previous_serial == capture_c.serial &&
            repeated_c.current_serial == capture_c.serial &&
            repeated_c.fence_value > pair_bc.fence_value,
        "rolling synthesizer did not retain a repeated capture after an advancing pair");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    require_readback_matches(
        fixture,
        current_destinations[1].Get(),
        pattern_c,
        "repeated current C",
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    require(
        operation_succeeded(synthesizer.retire_previous()),
        "rolling previous retirement failed");
    require(
        operation_succeeded(synthesizer.retire_previous()),
        "rolling previous retirement was not idempotent");
    require(operation_succeeded(synthesizer.wait_for_idle()), "synthesizer drain failed");

    CaptureResources history_b = acquire_capture_for_inspection(*history, capture_b);
    fixture.wait_for_fence(history_b.fence.Get(), capture_b.fence_value);
    require_readback_matches(
        fixture,
        history_b.resource.Get(),
        pattern_b,
        "history B after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_b);

    CaptureResources history_c = acquire_capture_for_inspection(*history, capture_c);
    fixture.wait_for_fence(history_c.fence.Get(), capture_c.fence_value);
    require_readback_matches(
        fixture,
        history_c.resource.Get(),
        pattern_c,
        "history C after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_c);
    CaptureResources history_d = acquire_capture_for_inspection(*history, capture_d);
    fixture.wait_for_fence(history_d.fence.Get(), capture_d.fence_value);
    require_readback_matches(
        fixture,
        history_d.resource.Get(),
        pattern_d,
        "history D after rolling synthesis");
    retire_completed_inspection(fixture, *history, history_d);
    require(operation_succeeded(history->wait_for_idle()), "synthesis leases did not retire");

    xrfg::D3D12HistoryConsumerLease stale_lease{};
    ID3D12Resource* stale_resource = nullptr;
    ID3D12Fence* stale_fence = nullptr;
    require(
        FAILED(history->acquire_consumer(
            capture_a,
            &stale_lease,
            &stale_resource,
            &stale_fence)) &&
            stale_resource == nullptr && stale_fence == nullptr,
        "overwritten capture A remained consumable after serial 3");

    require(operation_succeeded(history->invalidate()), "synthesis history invalidate failed");
}

void test_stereo_motion_synthesis_beats_same_pixel_blend(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    bool validate_repeated_capture = false,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kMotionWidth = 1024;
    constexpr UINT kMotionHeight = 512;
    constexpr UINT kEvaluationMargin = 32;

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, kMotionWidth, kMotionHeight),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(),
        sources[1].Get(),
    };
    std::array<ID3D12Resource*, 2> current_destination_pointers{
        current_destinations[0].Get(),
        current_destinations[1].Get(),
    };
    std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
        synthetic_destinations[0].Get(),
    };

    const StereoPattern previous = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {0, 0});
    const StereoPattern current = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {16, -16});
    const StereoPattern expected_midpoint = translated_motion_pattern(
        kMotionWidth,
        kMotionHeight,
        {8, -8});
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "motion synthesis history initialization failed");

    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_destination_pointers.data(),
                current_destination_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_destination_pointers.data(),
                synthetic_destination_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            nvidia_options,
            backend == xrfg::D3D12OpticalFlowBackend::nvidia)),
        "motion frame synthesizer initialization failed");
    const ReprojectionViews static_views = make_reprojection_views();

    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "motion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(
            synthesizer.submit_prime(capture_a, static_views, 0, &prime)),
        "motion prime submission failed");
    require_frame_start_gate(synthesizer, "embedded test frame-start gate");
    if(backend==xrfg::D3D12OpticalFlowBackend::nvidia) {
        xrfg::D3D12NvidiaGpuTiming prime_timing{};
        require(synthesizer.consume_nvidia_gpu_timing(&prime_timing)==S_FALSE,
            "transport/prime exposed an unresolved GPU generation interval");
    }


    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "motion capture B failed");
    require(
        capture_a.serial == 1 && capture_a.slot == 0 &&
            capture_b.serial == 2 && capture_b.slot == 1,
        "motion pair history chronology is incorrect");
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b,
            static_views,
            static_views,
            0,
            1,
            &pair)),
        "motion A/B pair submission failed");
    require(
        pair.previous_serial == capture_a.serial &&
            pair.current_serial == capture_b.serial,
        "motion pair synthesis ticket is incorrect");
    const StereoPattern actual_current = readback_pattern(
        fixture,
        current_destinations[1].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    const StereoPattern actual_synthetic = readback_pattern(
        fixture,
        synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(actual_current == current, "motion current output is not a bit-exact copy of B");
    require(
        actual_synthetic != previous && actual_synthetic != current,
        "motion synthetic output duplicates one of its inputs");
    require(
        actual_synthetic != same_pixel_blend,
        "motion synthetic output ignored the optical-flow field");

    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double synthetic_error = mean_absolute_rgb_error(
            actual_synthetic,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double previous_duplicate_error = mean_absolute_rgb_error(
            previous,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        const double current_duplicate_error = mean_absolute_rgb_error(
            current,
            expected_midpoint,
            kMotionWidth,
            kMotionHeight,
            eye,
            kEvaluationMargin);
        std::cout << "motion eye=" << eye
                  << " synthetic_mae=" << synthetic_error
                  << " blend_mae=" << blend_error
                  << " previous_mae=" << previous_duplicate_error
                  << " current_mae=" << current_duplicate_error << '\n';
        require(
            synthetic_error <= blend_error * 1.10 &&
                synthetic_error < previous_duplicate_error &&
                synthetic_error < current_duplicate_error,
            "motion midpoint is less stable than blend/duplicates for eye " +
                std::to_string(eye) +
                ": synthetic=" + std::to_string(synthetic_error) +
                " blend=" + std::to_string(blend_error));
    }

    require_source_is_render_target(fixture, sources[0].Get());
    require_source_is_render_target(fixture, sources[1].Get());
    if (validate_repeated_capture) {
        const ReprojectionViews repeated_views = static_views;
        xrfg::D3D12FrameSynthesisTicket repeated{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture_b,
                repeated_views,
                repeated_views,
                0,
                0,
                &repeated)) &&
                repeated.previous_serial == capture_b.serial &&
                repeated.current_serial == capture_b.serial &&
                repeated.fence_value > pair.fence_value,
            "motion synthesizer did not accept an unchanged capture");
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            "repeated motion synthesis drain failed");
        fixture.require_no_debug_errors();
        const StereoPattern repeated_current = readback_pattern(
            fixture,
            current_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(
            repeated_current == current,
            "repeated motion current output is not a bit-exact copy of B");
        const StereoPattern repeated_synthetic = readback_pattern(
            fixture,
            synthetic_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const double repeated_error = mean_absolute_rgb_error(
                repeated_synthetic,
                current,
                kMotionWidth,
                kMotionHeight,
                eye,
                kEvaluationMargin);
            require(
                repeated_error < 0.5,
                "repeated capture pose synthesis is unstable for eye " +
                    std::to_string(eye) + ": error=" +
                    std::to_string(repeated_error));
        }
    } else {
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            "motion synthesis drain failed");
    }
    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
        xrfg::D3D12NvidiaGpuTiming timing{};
        require(
            synthesizer.consume_nvidia_gpu_timing(&timing) == S_OK &&
                timing.previous_serial == capture_a.serial &&
                timing.current_serial == capture_b.serial &&
                timing.eye_count == kEyeCount &&
                timing.total_microseconds >=
                    timing.pack_microseconds +
                        timing.composition_microseconds,
            "NVIDIA GPU timing did not resolve the measured stereo pair");
        std::cout << "nvidia gpu pack_us=" << timing.pack_microseconds
                  << " eye0_us=" << timing.eye0_microseconds
                  << " eye1_us=" << timing.eye1_microseconds
                  << " composition_us=" << timing.composition_microseconds
                  << " total_us=" << timing.total_microseconds << '\n';
    }
    require(operation_succeeded(history->invalidate()), "motion history invalidate failed");
}

void test_dlss_motion_vector_per_eye_swapchains(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::configure_dlss_motion_vector_tracking(true);
    constexpr UINT width = 64;
    constexpr UINT height = 32;
    const auto create_texture = [&](UINT texture_width, UINT texture_height) {
        D3D12_RESOURCE_DESC description =
            stereo_texture_description(texture_width, texture_height);
        description.DepthOrArraySize = 1;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        require_hresult(fixture.device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
            "create per-eye DLSS texture");
        return texture;
    };
    const auto left_output = create_texture(width, height);
    const auto right_output = create_texture(width, height);
    const auto motion = create_texture(width, height / 2);
    const auto left_xr = create_texture(width, height);
    const auto right_xr = create_texture(width, height);
    const auto publish = [&](std::uint64_t stream, ID3D12Resource* eye_output, UINT motion_x = 0) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
            xrfg::publish_dlss_motion_vectors({
                stream, eye_output, motion.Get(), fixture.queue(), 0, 0, width, height,
                motion_x, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
                D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        });
    };

    // Right-eye first discovery: input offsets override first-seen order.
    // Autonomous retains full-resource snapshots and their native rectangles.
    for (int frame = 0; frame < 2; ++frame) {
        publish(622, right_output.Get(), width / 2);
        publish(611, left_output.Get());
    }
    const auto left = xrfg::resolve_dlss_motion_vectors(left_xr.Get(), fixture.queue(), 0);
    const auto right = xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue(), 1);
    require(left && right && left->eye_count == 1 && right->eye_count == 1 &&
            left->eyes[0]->stream == 611 && right->eyes[0]->stream == 622 &&
            left->eyes[0]->serial == 2 && right->eyes[0]->serial == 2,
        "per-eye swapchains did not each take their own eye's DLSS evaluation");
    require(right->eyes[0]->motion_x == width / 2 &&
            right->eyes[0]->source_motion_x == width / 2 &&
            right->eyes[0]->motion_vectors->GetDesc().Width == width,
        "eye ordering changed Autonomous's full snapshot/native rectangle contract");
    // The right eye evaluated first this frame: the eyes stay put.
    publish(622, right_output.Get(), width / 2);
    publish(611, left_output.Get());
    const auto left_again = xrfg::resolve_dlss_motion_vectors(left_xr.Get(), fixture.queue(), 0);
    const auto right_again = xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue(), 1);
    require(left_again && right_again && left_again->eyes[0]->stream == 611 &&
            right_again->eyes[0]->stream == 622 && left_again->eyes[0]->serial == 3 &&
            right_again->eyes[0]->serial == 3,
        "per-eye swapchains swapped eyes with this frame's evaluation order");
    // Without the eye, the newest as before.
    const auto unknown = xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue());
    require(unknown && unknown->eyes[0]->stream == 611,
        "an image of unknown eye no longer took the newest evaluation");
    // An evaluation written straight into the image is that image's.
    publish(611, left_xr.Get());
    publish(622, right_output.Get(), width / 2);
    const auto direct = xrfg::resolve_dlss_motion_vectors(left_xr.Get(), fixture.queue(), 1);
    require(direct && direct->eyes[0]->stream == 611 && direct->eyes[0]->serial == 4,
        "an evaluation whose output is the image did not go to that image");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_dlss_motion_vector_stream(611);
    xrfg::retire_dlss_motion_vector_stream(622);
    xrfg::configure_dlss_motion_vector_tracking(true);

    // One DLSS feature for both eyes, evaluated left then right: each eye
    // sees every other serial.
    publish(633, left_output.Get());
    require(!xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue(), 1),
        "first left evaluation was incorrectly borrowed by the right eye");
    const auto first_left = xrfg::resolve_dlss_motion_vectors(left_xr.Get(), fixture.queue(), 0);
    require(first_left && first_left->eyes[0]->serial == 1,
        "the left eye did not take the first evaluation of one shared stream");
    publish(633, right_output.Get(), width / 2);
    publish(633, left_output.Get());
    const auto half = xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue(), 1);
    require(!half, "the right eye took the previous frame's evaluation of a shared stream");
    publish(633, right_output.Get(), width / 2);
    const auto shared_left = xrfg::resolve_dlss_motion_vectors(left_xr.Get(), fixture.queue(), 0);
    const auto shared_right = xrfg::resolve_dlss_motion_vectors(right_xr.Get(), fixture.queue(), 1);
    require(shared_left && shared_right && shared_left->eyes[0]->serial == 3 &&
            shared_right->eyes[0]->serial == 4 &&
            shared_left->eyes[0]->previous_serial == 1 &&
            shared_right->eyes[0]->previous_serial == 2,
        "one shared DLSS stream did not give each eye its own evaluation");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_dlss_motion_vector_stream(633);
}

void test_release_bound_dlss_guides(D3D12WarpFixture& fixture) {
    using Mode = xrfg::D3D12FrameGenerationMode;
    require(xrfg::use_release_bound_game_guides(Mode::ofxr, true, false) &&
            xrfg::use_release_bound_game_guides(Mode::native_dlss, true, false) &&
            !xrfg::use_release_bound_game_guides(Mode::fsr3, true, false) &&
            !xrfg::use_release_bound_game_guides(Mode::ofxr, false, false) &&
            !xrfg::use_release_bound_game_guides(Mode::ofxr, true, true),
        "R15 changed the owner's FSR/OFA/AFW scope");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    const auto texture = [&](UINT width) {
        auto description = stereo_texture_description(width, 32);
        description.DepthOrArraySize = 1;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const auto properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> resource;
        require_hresult(fixture.device()->CreateCommittedResource(&properties,
            D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&resource)), "R15 texture");
        return resource;
    };
    const auto color = texture(64), xr_color = texture(64), motion = texture(64);
    const auto spectator = texture(128);
    ComPtr<ID3D12GraphicsCommandList> recorded;
    const auto publish = [&](std::uint64_t stream, ID3D12Resource* output,
                             bool submit_receipt = true, bool reset = false) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            recorded = list;
            xrfg::DlssMotionVectorPublication p{};
            p.stream = stream; p.output = output; p.motion_vectors = motion.Get();
            p.output_width = static_cast<UINT>(output->GetDesc().Width); p.output_height = 32;
            p.motion_width = 64; p.motion_height = 32;
            p.producer_queue = fixture.queue(); p.producer_command_list = list;
            p.resource_state = D3D12_RESOURCE_STATE_COMMON; p.reset = reset;
            require(xrfg::publish_dlss_motion_vectors(p) != nullptr, "R15 publication failed");
        },false); // this helper owns the deliberate submit_receipt counterfactual
        if (submit_receipt) {
            ID3D12CommandList* lists[] = {recorded.Get()};
            xrfg::report_dlss_motion_vector_submission(fixture.queue(), 1, lists);
        }
        return xrfg::dlss_motion_vector_publications();
    };
    const auto resolve = [&](int eye, std::uint64_t release, std::uint64_t other,
                             ID3D12CommandQueue* queue = nullptr) {
        return xrfg::resolve_dlss_motion_vectors(xr_color.Get(), queue ? queue : fixture.queue(),
                                                eye, release, other);
    };
    publish(755, color.Get()); // same-size stream retained from start-up
    publish(744, spectator.Get());
    publish(733, color.Get()); // capture begins on the RIGHT eye, serial 1
    std::uint64_t left_release{}, right_release{};
    for (int frame = 0; frame < 3; ++frame) {
        left_release = publish(733, color.Get());
        right_release = publish(733, color.Get());
    }
    const auto left = resolve(0, left_release, right_release);
    const auto right = resolve(1, right_release, left_release);
    require(left && right && left->eyes[0]->stream == 733 && right->eyes[0]->stream == 733 &&
            left->eyes[0]->serial == 6 && right->eyes[0]->serial == 7 &&
            left->eyes[0]->previous_serial == 4 && right->eyes[0]->previous_serial == 5 &&
            left->eyes[0]->publication <= left_release && right->eyes[0]->publication <= right_release,
        "R15 right-first shared stream did not follow each color release");
    const auto strict_left = xrfg::resolve_dlss_motion_vectors(xr_color.Get(), fixture.queue(),
        0, left_release, right_release, {true, 733, left->eyes[0]->epoch});
    require(strict_left && !strict_left->eyes[0]->bridge &&
            strict_left->eyes[0]->motion_vectors.Get() == left->eyes[0]->motion_vectors.Get() &&
            strict_left->eyes[0]->previous_serial == left->eyes[0]->previous_serial,
        "R22 changed strict FSR guide identity, previous serial or motion snapshot");
    // The default resolver remains the V455 odd/even behavior, including its
    // refusal of this half-updated pair; this is what the FSR route still uses.
    require(!xrfg::resolve_dlss_motion_vectors(xr_color.Get(), fixture.queue(), 0),
        "R15 altered the legacy FSR resolver");
    publish(733, color.Get()); publish(733, color.Get());
    const auto both = xrfg::dlss_motion_vector_publications();
    const auto first = resolve(0, both, both), second = resolve(1, both, both);
    require(first && second && first->eyes[0]->serial == 8 && second->eyes[0]->serial == 9,
        "R15 both evaluated before releases did not use left-first evaluation order");
    require(first->eyes[0]->bridge && first->eyes[0]->bridge->serial == 7 &&
            second->eyes[0]->bridge && second->eyes[0]->bridge->serial == 8 &&
            !first->eyes[0]->bridge->bridge,
        "R22 shared stream did not retain each eye's preceding opposite-eye evaluation");
    require(!resolve(0, left_release > 10 ? left_release - 10 : 1, right_release > 10 ? right_release - 10 : 1),
        "R15 borrowed a newer guide for an evicted release");
    publish(733, color.Get(), true, true);
    const auto after_reset = xrfg::dlss_motion_vector_publications();
    require(!resolve(0, after_reset, after_reset), "R15 borrowed an opposite-eye guide across reset");
    xrfg::retire_dlss_motion_vector_stream(733);
    xrfg::retire_dlss_motion_vector_stream(744);
    xrfg::retire_dlss_motion_vector_stream(755);

    // Separate streams with identical input rectangles: first discovery is
    // right-first. Release order, rather than that discovery order, wins.
    for (int frame = 0; frame < 2; ++frame) {
        right_release = publish(722, color.Get());
        left_release = publish(711, color.Get());
    }
    const auto separate_left = resolve(0, left_release, right_release);
    const auto separate_right = resolve(1, right_release, left_release);
    require(separate_left && separate_right && separate_left->eyes[0]->stream == 711 &&
            separate_right->eyes[0]->stream == 722,
        "R15 separate streams followed discovery order rather than the releases");
    const auto legacy_left = xrfg::resolve_dlss_motion_vectors(xr_color.Get(), fixture.queue(), 0);
    require(legacy_left && legacy_left->eyes[0]->stream == 722,
        "R15 changed FSR's legacy separate-stream ordering");
    // Both releases follow both evaluations: preserve upstream stereo ordering.
    const auto equal_release = xrfg::dlss_motion_vector_publications();
    require(resolve(0, equal_release, equal_release)->eyes[0]->stream == 722 &&
            resolve(1, equal_release, equal_release)->eyes[0]->stream == 711,
        "R15 changed the known both-evaluated-before-release ordering");
    publish(766, color.Get());
    const auto ambiguous = xrfg::dlss_motion_vector_publications();
    require(!resolve(0, ambiguous, ambiguous), "R15 guessed among three active same-size streams");
    xrfg::retire_dlss_motion_vector_stream(711);
    xrfg::retire_dlss_motion_vector_stream(722);
    xrfg::retire_dlss_motion_vector_stream(766);

    // No Man's Sky: one evaluation per frame for both eyes side by side, each
    // eye in the matching half of an image the evaluation's size. Both eyes
    // take the frame's evaluation, chained to the previous frame's; without
    // both_eyes the shared-stream rule gave the left eye the previous frame's
    // and left every pair's serials unchained (DLSS-G skip 1, detail 5).
    // Each takes its half of the vectors, its output on its view's half.
    const xrfg::DlssReleasedEyeIdentity both_eyes{false, 0, 0, true, 0};
    const xrfg::DlssReleasedEyeIdentity both_eyes_right{false, 0, 0, true, 1};
    std::uint64_t both_previous{};
    for (int frame = 0; frame < 3; ++frame) {
        const auto release = publish(799, color.Get());
        const auto both_left = xrfg::resolve_dlss_motion_vectors(xr_color.Get(), fixture.queue(),
            0, release, release, both_eyes);
        const auto both_right = xrfg::resolve_dlss_motion_vectors(xr_color.Get(), fixture.queue(),
            1, release, release, both_eyes_right);
        require(both_left && both_right && both_left->eyes[0]->publication == release &&
                both_right->eyes[0]->publication == release &&
                both_left->eyes[0]->previous_serial + 1 == both_left->eyes[0]->serial &&
                (!both_previous || both_left->eyes[0]->previous_serial == both_previous),
            "a both-eyes evaluation did not go to both eyes, chained frame to frame");
        const auto& l = *both_left->eyes[0];
        const auto& r = *both_right->eyes[0];
        require(l.motion_x == 0 && l.motion_width == 32 && r.motion_x == 32 && r.motion_width == 32 &&
                l.output_x == 0 && l.output_width == 32 && r.output_x == 32 && r.output_width == 32,
            "a both-eyes evaluation did not give each eye its own half");
        both_previous = both_left->eyes[0]->serial;
    }
    const auto alternating = resolve(0, xrfg::dlss_motion_vector_publications(),
        xrfg::dlss_motion_vector_publications());
    require(alternating && alternating->eyes[0]->serial + 1 == both_previous,
        "without both_eyes one shared stream no longer gave the left eye the earlier evaluation");
    xrfg::retire_dlss_motion_vector_stream(799);

    // A CPU publication on another queue is insufficient. Only the actual
    // producer submission attaches the fence to the selected older snapshot.
    ComPtr<ID3D12CommandQueue> consumer;
    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    require_hresult(fixture.device()->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&consumer)),
        "R15 consumer queue");
    left_release = publish(777, color.Get(), false);
    require(!resolve(0, left_release, 0, consumer.Get()),
        "R15 cross-queue CPU publication bypassed submission ownership");
    ID3D12CommandList* lists[] = {recorded.Get()};
    xrfg::report_dlss_motion_vector_submission(fixture.queue(), 1, lists);
    right_release = publish(777, color.Get());
    const auto crossqueue = resolve(0, left_release, right_release, consumer.Get());
    require(crossqueue && crossqueue->eyes[0]->publication == left_release &&
            crossqueue->eyes[0]->producer_fence && crossqueue->eyes[0]->producer_fence_value,
        "R15 older released-eye guide lost its exact producer fence");
    const auto watermark = xrfg::dlss_motion_vector_publications();
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::configure_dlss_motion_vector_tracking(true);
    require(publish(788, color.Get()) > watermark, "R15 watermark reused an old publication after toggle");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    std::cout << "R15 release-bound guides: right-first shared/separate, stale/spectator, resets, ambiguity, both-eyes evaluations, cross-queue receipts; FSR/OFA/AFW legacy preserved PASS\n";
}

// A game that renders both eyes through one view (UEVR's Synchronized
// Sequential without its ghosting fix) writes each eye's vectors into the
// other eye's image: the shift of a distant point between the eyes'
// projections plus half the motion. Followed through the other eye's vectors
// they reach the eye's own previous image; vectors that already do are copied.
void test_guide_composer(D3D12WarpFixture& fixture) {
    constexpr UINT width = 256;
    constexpr UINT height = 32;
    xrfg::D3D12GuideComposer composer;
    require_hresult(composer.initialize(fixture.device()), "guide composer initialization");
    xrfg::D3D12ReprojectionView view{};
    view.fov = {-0.942477822F, 0.698131740F, 0.872664630F, -0.855211377F};
    const float left = std::tan(view.fov.angle_left);
    const float right = std::tan(view.fov.angle_right);
    // Where a distant point of this (left) eye's image is in the other's, less
    // where it is in this one: the other eye's field of view is the mirror.
    const float shift = (left + right) / (right - left) * float(width);
    const auto run = [&](float b_motion, float r_motion, bool bridged) {
        auto motion = create_and_upload_game_motion(fixture, width, height, {b_motion, r_motion});
        auto r = std::make_shared<xrfg::DlssMotionVectorFrame>();
        r->stream = 1;
        r->epoch = 1;
        r->serial = 1;
        r->motion_vectors = motion;
        r->output_width = r->motion_width = width;
        r->output_height = r->motion_height = height;
        r->motion_slice = 1;
        r->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        auto b = std::make_shared<xrfg::DlssMotionVectorFrame>(*r);
        b->serial = 2;
        b->motion_slice = 0;
        if (bridged) b->bridge = r;
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = 1;
        set->eyes[0] = b;
        std::shared_ptr<const xrfg::DlssMotionVectorSet> composed;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            composed = composer.compose(list, 0, set, std::span(&view, 1));
        });
        return std::pair<std::shared_ptr<const xrfg::DlssMotionVectorSet>,
                         std::shared_ptr<const xrfg::DlssMotionVectorSet>>{set, composed};
    };
    // The x of the composed texels in the central half of the middle row.
    const auto read = [&](const xrfg::DlssMotionVectorFrame& frame) {
        const D3D12_RESOURCE_DESC description = frame.motion_vectors->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 row_size = 0;
        UINT64 total = 0;
        fixture.device()->GetCopyableFootprints(&description, 0, 1, 0, &layout, &rows, &row_size, &total);
        ComPtr<ID3D12Resource> readback = create_buffer(fixture, D3D12_HEAP_TYPE_READBACK, total,
                                                        D3D12_RESOURCE_STATE_COPY_DEST);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            const auto to_copy = transition_barrier(frame.motion_vectors.Get(), frame.resource_state,
                                                    D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_copy);
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = frame.motion_vectors.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint = layout;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            const auto back = transition_barrier(frame.motion_vectors.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_SOURCE, frame.resource_state);
            list->ResourceBarrier(1, &back);
        });
        void* mapped = nullptr;
        require_hresult(readback->Map(0, nullptr, &mapped), "map composed vectors");
        const auto half_to_float = [](std::uint16_t h) {
            const std::uint32_t sign = (std::uint32_t(h) & 0x8000U) << 16;
            const std::uint32_t exponent = (h >> 10) & 0x1FU;
            const std::uint32_t mantissa = h & 0x3FFU;
            const std::uint32_t bits =
                exponent == 0 ? sign : sign | ((exponent + 112U) << 23) | (mantissa << 13);
            float value{};
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        std::vector<float> xs;
        const auto* row = static_cast<const std::byte*>(mapped) + layout.Offset +
                          std::size_t(height / 2) * layout.Footprint.RowPitch;
        for (UINT x = width / 4; x < 3 * width / 4; ++x) {
            std::uint16_t h{};
            std::memcpy(&h, row + std::size_t(x) * 4, sizeof(h));
            xs.push_back(half_to_float(h));
        }
        readback->Unmap(0, nullptr);
        return xs;
    };
    constexpr float motion = -10.0F;
    // Into the other eye: followed on to this eye's previous image.
    const auto [cross_set, cross] = run(shift + motion / 2, -shift + motion / 2, true);
    require(cross && cross != cross_set && cross->eyes[0] && !cross->eyes[0]->bridge &&
                cross->eyes[0]->motion_vectors.Get() != cross_set->eyes[0]->motion_vectors.Get(),
        "a bridged eye's vectors were not given a texture of their own");
    for (const float x : read(*cross->eyes[0])) {
        require(std::abs(x - motion) < 0.5F,
                "vectors into the other eye did not reach this eye's previous image");
    }
    // Into its own previous image already: copied unchanged.
    const auto [own_set, own] = run(motion, motion, true);
    for (const float x : read(*own->eyes[0])) {
        require(std::abs(x - motion) < 0.5F, "vectors into this eye's own previous image were changed");
    }
    // Without a bridge nothing is recorded.
    const auto [plain_set, plain] = run(motion, motion, false);
    require(plain == plain_set, "an eye without a bridge was composed");
    require(composer.composed() == 2, "the composer's count did not follow the bridged eyes");
}

void test_dlss_motion_vector_stereo_stream_pairing(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(true);
    constexpr UINT width = 64;
    constexpr UINT height = 32;
    const auto create_texture = [&](UINT texture_width, UINT texture_height, UINT16 slices) {
        D3D12_RESOURCE_DESC description =
            stereo_texture_description(texture_width, texture_height);
        description.DepthOrArraySize = slices;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        require_hresult(fixture.device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
            "create DLSS provenance texture");
        return texture;
    };
    const auto shared_output = create_texture(width, height, 1);
    const auto left_output = create_texture(width, height, 1);
    const auto right_output = create_texture(width, height, 1);
    const auto motion = create_texture(width / 2, height / 2, 1);
    const auto xr_output = create_texture(width, height, 2);
    const auto xr_double_wide = create_texture(width * 2, height, 1);

    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::DlssMotionVectorPublication verified_publication{
            101, shared_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list};
        verified_publication.verified_producer_device = fixture.device();
        verified_publication.camera_position = {1.0F, 2.0F, 3.0F};
        verified_publication.camera_up = {0.0F, 1.0F, 0.0F};
        verified_publication.camera_right = {1.0F, 0.0F, 0.0F};
        verified_publication.camera_forward = {0.0F, 0.0F, -1.0F};
        verified_publication.camera_fov_vertical = 1.25F;
        verified_publication.camera_data_valid = true;
        verified_publication.output_resource_state =
            D3D12_RESOURCE_STATE_COMMON;
        verified_publication.snapshot_output = true;
        xrfg::publish_dlss_motion_vectors(verified_publication);
        xrfg::publish_dlss_motion_vectors({
            202, shared_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });

    const auto resolved = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(resolved && resolved->eye_count == 2 && resolved->eyes[0] &&
            resolved->eyes[1] && resolved->eyes[0]->stream == 101 &&
            resolved->eyes[1]->stream == 202 &&
            resolved->eyes[0]->motion_vectors.Get() != motion.Get() &&
            resolved->eyes[1]->motion_vectors.Get() != motion.Get() &&
            resolved->eyes[0]->motion_vectors.Get() !=
                resolved->eyes[1]->motion_vectors.Get() &&
            resolved->eyes[0]->output.Get() != shared_output.Get() &&
            resolved->eyes[1]->output.Get() == shared_output.Get() &&
            resolved->eyes[0]->output_slice == 0 &&
            resolved->eyes[1]->output_slice == 0 &&
            resolved->eyes[0]->camera_data_valid &&
            resolved->eyes[0]->camera_position[0] == 1.0F &&
            resolved->eyes[0]->camera_position[1] == 2.0F &&
            resolved->eyes[0]->camera_position[2] == 3.0F &&
            resolved->eyes[0]->camera_forward[2] == -1.0F &&
            resolved->eyes[0]->camera_fov_vertical == 1.25F,
        "two DLSS streams were not associated with the XR stereo image");
    const auto double_wide = xrfg::resolve_dlss_motion_vectors(
        xr_double_wide.Get(), fixture.queue());
    require(double_wide && double_wide->eye_count == 2 &&
            double_wide->eyes[0] && double_wide->eyes[1] &&
            double_wide->eyes[0]->stream == 101 &&
            double_wide->eyes[1]->stream == 202 &&
            double_wide->eyes[0]->output_x == 0 && double_wide->eyes[1]->output_x == width &&
            double_wide->eyes[0]->motion_x == 0 && double_wide->eyes[1]->motion_x == 0,
        "two eye-sized DLSS streams were not associated with double-wide XR");
    const auto mono = xrfg::resolve_dlss_motion_vectors(
        left_output.Get(), fixture.queue());
    require(mono && mono->eye_count == 1 && mono->eyes[0],
        "ordinary mono output was changed by double-wide stream detection");
    require(resolved->eyes[0]->output.Get() != left_output.Get(),
        "diagnostic DLSS output was retained by alias instead of snapshot");

    // Publish in the opposite order. Eye assignment must remain tied to the
    // stable first-seen stream order, not to this frame's evaluation order.
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            202, shared_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        xrfg::publish_dlss_motion_vectors({
            101, shared_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });
    const auto reordered = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), fixture.queue());
    require(reordered && reordered->eyes[0]->stream == 101 &&
            reordered->eyes[1]->stream == 202 &&
            reordered->eyes[0]->serial == 2 &&
            reordered->eyes[1]->serial == 2 &&
            reordered->eyes[0]->output.Get() == shared_output.Get() &&
            reordered->eyes[1]->output.Get() == resolved->eyes[1]->output.Get() &&
            reordered->eyes[0]->output.Get() == reordered->eyes[1]->output.Get(),
        "DLSS stream pairing changed eye assignment with evaluation order");

    const auto tracked = xrfg::dlss_motion_vector_statistics();
    require(tracked.published == 4 && tracked.snapshot_copies == 5 &&
            tracked.snapshot_failures == 0 && tracked.matched == 4 &&
            tracked.resolve_missing_streams == 0 &&
            tracked.resolve_stale_pairs == 0,
        "DLSS stream diagnostics did not account for the stereo pair");
    xrfg::report_dlss_motion_vector_use();
    const auto used = xrfg::dlss_motion_vector_statistics();
    require(used.status == xrfg::DlssMotionVectorStatus::used && used.used == 1 &&
            used.last_used_publication == std::max(
                reordered->eyes[0]->publication, reordered->eyes[1]->publication),
        "DLSS stream diagnostics did not retain the last successful use");
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            101, shared_output.Get(), motion.Get(), fixture.queue(), 0, 0, width, height,
            0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });
    require(xrfg::dlss_motion_vector_statistics().status ==
                xrfg::DlssMotionVectorStatus::used,
        "a new DLSS publication falsely demoted a recently used vector route");

    // UEVR Native Stereo publishes one eye-sized DLSS output per stream for
    // a single double-wide XR resource. The Witcher array-stereo assertions
    // above still cover the shared-output topology.
    const auto native_left = create_texture(width, height, 1);
    const auto native_right = create_texture(width, height, 1);
    const auto native_xr = create_texture(width * 2, height, 1);
    fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
        xrfg::publish_dlss_motion_vectors({
            303, native_left.Get(), motion.Get(), fixture.queue(),
            0, 0, width, height, 0, 0, width / 2, height / 2,
            1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        xrfg::publish_dlss_motion_vectors({
            404, native_right.Get(), motion.Get(), fixture.queue(),
            0, 0, width, height, 0, 0, width / 2, height / 2,
            1.0F, 1.0F, 0.0F, 0.0F,
            D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
    });
    const auto native_guides = xrfg::resolve_dlss_motion_vectors(
        native_xr.Get(), fixture.queue());
    require(native_guides && native_guides->eye_count == 2 &&
            native_guides->eyes[0] && native_guides->eyes[1] &&
            native_guides->eyes[0]->stream == 303 &&
            native_guides->eyes[1]->stream == 404,
        "Native Stereo did not select its two eye-sized DLSS streams");
    const auto native_mono = xrfg::resolve_dlss_motion_vectors(
        native_left.Get(), fixture.queue());
    require(native_mono && native_mono->eye_count == 1,
        "ordinary mono resolution changed with Native Stereo guides");

    xrfg::configure_dlss_motion_vector_tracking(false);
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "disabled DLSS vector tracking still resolved a pair");
    xrfg::configure_dlss_motion_vector_tracking(true);
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), fixture.queue()),
        "re-enabled DLSS vector tracking exposed a stale pair");

    const auto publish_alternating_eye = [&](ID3D12Resource* eye_output) {
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* command_list) {
            xrfg::publish_dlss_motion_vectors({
                303, eye_output, motion.Get(), fixture.queue(), 0, 0, width, height,
                0, 0, width / 2, height / 2, 1.0F, 1.0F, 0.0F, 0.0F,
                D3D12_RESOURCE_STATE_COMMON, false, false, command_list});
        });
    };
    publish_alternating_eye(left_output.Get());
    require(!xrfg::resolve_dlss_motion_vectors(
                std::uint32_t{2}, fixture.queue()),
        "single DLSS stream resolved before the second eye arrived");
    publish_alternating_eye(right_output.Get());
    const auto alternating_ab = xrfg::resolve_dlss_motion_vectors(
        std::uint32_t{2}, fixture.queue());
    require(alternating_ab && alternating_ab->eyes[0] && alternating_ab->eyes[1] &&
            alternating_ab->eyes[0]->stream == 303 &&
            alternating_ab->eyes[1]->stream == 303 &&
            alternating_ab->eyes[0]->serial == 1 &&
            alternating_ab->eyes[1]->serial == 2 &&
            alternating_ab->eyes[0]->previous_serial == 0 &&
            alternating_ab->eyes[1]->previous_serial == 0,
        "single alternating DLSS stream did not form its first stereo pair");
    publish_alternating_eye(left_output.Get());
    require(!xrfg::resolve_dlss_motion_vectors(
                std::uint32_t{2}, fixture.queue()),
        "single alternating DLSS stream exposed a half-updated stereo pair");
    publish_alternating_eye(right_output.Get());
    const auto alternating_cd = xrfg::resolve_dlss_motion_vectors(
        std::uint32_t{2}, fixture.queue());
    require(alternating_cd && alternating_cd->eyes[0] && alternating_cd->eyes[1] &&
            alternating_cd->eyes[0]->serial == 3 &&
            alternating_cd->eyes[1]->serial == 4 &&
            alternating_cd->eyes[0]->previous_serial == 1 &&
            alternating_cd->eyes[1]->previous_serial == 2,
        "single alternating DLSS stream lost per-eye temporal continuity");
    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::retire_dlss_motion_vector_stream(101);
    xrfg::retire_dlss_motion_vector_stream(202);
    xrfg::retire_dlss_motion_vector_stream(303);
    xrfg::retire_dlss_motion_vector_stream(404);
}

void test_dlss_motion_vector_submission_handoff(D3D12WarpFixture& fixture) {
    xrfg::configure_dlss_motion_vector_tracking(true);
    constexpr UINT width = 64;
    constexpr UINT height = 32;
    const auto create_texture = [&](UINT texture_width, UINT texture_height) {
        D3D12_RESOURCE_DESC description =
            stereo_texture_description(texture_width, texture_height);
        description.DepthOrArraySize = 1;
        description.Flags = D3D12_RESOURCE_FLAG_NONE;
        const D3D12_HEAP_PROPERTIES properties =
            heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> texture;
        require_hresult(fixture.device()->CreateCommittedResource(
            &properties, D3D12_HEAP_FLAG_NONE, &description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(texture.GetAddressOf())),
            "create DLSS submission-handoff texture");
        return texture;
    };

    D3D12_COMMAND_QUEUE_DESC queue_description{};
    queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> producer_queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    require_hresult(fixture.device()->CreateCommandQueue(
        &queue_description, IID_PPV_ARGS(producer_queue.GetAddressOf())),
        "create distinct DLSS producer queue");
    require_hresult(fixture.device()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(allocator.GetAddressOf())),
        "create DLSS producer allocator");
    require_hresult(fixture.device()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
        IID_PPV_ARGS(command_list.GetAddressOf())),
        "create DLSS producer command list");

    const auto output = create_texture(width, height);
    const auto motion = create_texture(width / 2, height / 2);
    const auto xr_output = create_texture(width, height);
    ComPtr<ReportedDeviceAlias> binding_device;
    binding_device.Attach(new ReportedDeviceAlias(fixture.device(), true));
    ComPtr<QueueWithReportedDeviceAlias> binding_queue;
    binding_queue.Attach(new QueueWithReportedDeviceAlias(
        fixture.queue(), binding_device.Get(), true));
    ComPtr<ID3D12Device> canonical_device;
    ComPtr<ID3D12CommandQueue> canonical_queue;
    require_hresult(xrfg::d3d_native::canonical_binding(
        binding_device.Get(), binding_queue.Get(), canonical_device, canonical_queue),
        "canonicalize Streamline D3D12 XR binding");
    require(canonical_device.Get() == fixture.device() &&
        canonical_queue.Get() == fixture.queue(), "XR binding retained wrapper identities");
    xrfg::DlssMotionVectorPublication publication{
        404, output.Get(), motion.Get(), producer_queue.Get(), 0, 0,
        width, height, 0, 0, width / 2, height / 2,
        1.0F, 1.0F, 0.0F, 0.0F, D3D12_RESOURCE_STATE_COMMON,
        false, false, command_list.Get()};
    publication.verified_producer_device = fixture.device();
    xrfg::publish_dlss_motion_vectors(publication);
    require(!xrfg::resolve_dlss_motion_vectors(
                xr_output.Get(), fixture.queue()),
        "unsubmitted DLSS snapshots crossed queues without synchronization");
    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), canonical_queue.Get()),
        "canonical binding bypassed the producer submission fence");

    require_hresult(command_list->Close(), "close DLSS producer command list");
    ID3D12CommandList* submitted[] = {command_list.Get()};
    producer_queue->ExecuteCommandLists(1, submitted);
    xrfg::report_dlss_motion_vector_submission(
        producer_queue.Get(), 1, submitted);

    require(!xrfg::resolve_dlss_motion_vectors(xr_output.Get(), binding_queue.Get()),
        "wrapper mismatch counterexample no longer exercises the MSFS rejection");

    const auto resolved = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), canonical_queue.Get());
    require(resolved && resolved->eye_count == 1 && resolved->eyes[0] &&
            resolved->eyes[0]->stream == 404 &&
            resolved->eyes[0]->producer_fence &&
            resolved->eyes[0]->producer_fence_value != 0,
        "submitted DLSS snapshots did not cross queues through the fence handoff");
    const auto known_left = xrfg::resolve_dlss_motion_vectors(
        xr_output.Get(), canonical_queue.Get(), 0);
    require(known_left && known_left->eyes[0]->producer_fence_value != 0 &&
            !xrfg::resolve_dlss_motion_vectors(xr_output.Get(), canonical_queue.Get(), 1),
        "known-eye resolution lost its producer fence or borrowed a missing opposite eye");
    ComPtr<ReportedDeviceAlias> unknown_device;
    unknown_device.Attach(new ReportedDeviceAlias(fixture.device()));
    require(FAILED(xrfg::d3d_native::canonical_binding(
        unknown_device.Get(), binding_queue.Get(), canonical_device, canonical_queue)) &&
        !canonical_device && !canonical_queue,
        "same adapter admitted an unproven binding device identity");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});

    xrfg::configure_dlss_motion_vector_tracking(false);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    xrfg::retire_dlss_motion_vector_stream(404);
}

void test_dlss_motion_vector_gpu_ingress(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx, bool packed = false) {
    constexpr UINT width = 256;
    constexpr UINT height = 128;
    constexpr UINT motion_width = width / 2;
    constexpr UINT motion_height = height / 2;
    constexpr UINT margin = 16;
    const std::array<int, kEyeCount> translation{8, -8};

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, width, height),
    };
    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_destination_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
        synthetic_destinations[0].Get()};

    const StereoPattern previous = translated_motion_pattern(width, height, {0, 0});
    const StereoPattern current = translated_motion_pattern(width, height, translation);
    const StereoPattern expected = translated_motion_pattern(width, height, {4, -4});
    const StereoPattern unscaled_expected = translated_motion_pattern(width, height, {2, -2});
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    const auto pack = [&](ID3D12Resource* input) {
        auto result = create_single_slice_texture(fixture, width * 2, height);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            std::array barriers{transition_barrier(input, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(result.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(UINT(barriers.size()), barriers.data());
            for (UINT eye=0;eye<2;++eye) {
                D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=input;
                src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=eye;
                dst.pResource=result.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&dst,eye*width,0,0,&src,nullptr);
            }
            for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            list->ResourceBarrier(UINT(barriers.size()),barriers.data());
        });
        return result;
    };
    if (packed) {
        for(auto& source:sources)source=pack(source.Get());
        for(auto& image:current_destinations)image=create_single_slice_texture(fixture,width*2,height);
        for(auto& image:synthetic_destinations)image=create_single_slice_texture(fixture,width*2,height);
        for(UINT i=0;i<2;++i){source_pointers[i]=sources[i].Get();current_destination_pointers[i]=current_destinations[i].Get();}
        synthetic_destination_pointers[0]=synthetic_destinations[0].Get();
    }
    ComPtr<ID3D12Resource> game_motion = create_and_upload_game_motion(
        fixture,
        motion_width,
        motion_height,
        {static_cast<float>(translation[0]) * motion_width / width,
         static_cast<float>(translation[1]) * motion_width / width});

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(), fixture.queue(), source_pointers,
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "DLSS-motion history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(), fixture.queue(), history,
            current_destination_pointers, synthetic_destination_pointers,
            kFormat, D3D12_RESOURCE_STATE_RENDER_TARGET, backend)),
        "DLSS-motion synthesizer initialization failed");

    auto frame_a = std::make_shared<xrfg::DlssMotionVectorFrame>();
    frame_a->stream = 17;
    frame_a->epoch = 3;
    frame_a->serial = 1;
    frame_a->motion_vectors = game_motion;
    frame_a->producer_queue = fixture.queue();
    frame_a->output_width = width;
    frame_a->output_height = height;
    frame_a->motion_width = motion_width;
    frame_a->motion_height = motion_height;
    frame_a->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto frame_b = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_b->previous_serial = 1;
    frame_b->serial = 2;
    auto frame_a_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_a);
    frame_a_right->motion_slice = 1;
    auto frame_b_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
    frame_b_right->motion_slice = 1;
    if(packed)frame_a_right->output_x=frame_b_right->output_x=width;
    auto set_a = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_a->eye_count = kEyeCount;
    set_a->eyes = {frame_a, frame_a_right};
    auto set_b = std::make_shared<xrfg::DlssMotionVectorSet>();
    set_b->eye_count = kEyeCount;
    set_b->eyes = {frame_b, frame_b_right};

    ReprojectionViews views = make_reprojection_views();
    if(packed)for(UINT eye=0;eye<2;++eye){views[eye].array_slice=0;views[eye].image_rect={eye*width,0,width,height};}
    xrfg::D3D12HistoryCaptureTicket capture_a{};
    require(
        operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "DLSS-motion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(synthesizer.submit_prime(
            capture_a, views, 0, &prime, set_a)),
        "DLSS-motion prime failed");
    require_frame_start_gate(synthesizer, "embedded test frame-start gate");

    xrfg::D3D12HistoryCaptureTicket capture_b{};
    require(
        operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "DLSS-motion capture B failed");
    const auto statistics_before = xrfg::dlss_motion_vector_statistics();
    xrfg::D3D12FrameSynthesisTicket pair{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture_b, views, views, 0, 1, &pair, std::nullopt, set_b)),
        "DLSS-motion pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    const auto statistics_after = xrfg::dlss_motion_vector_statistics();
    require(
        statistics_after.used == statistics_before.used + 1 &&
            statistics_after.status == xrfg::DlssMotionVectorStatus::used,
        "DLSS-motion path was not selected by the GPU synthesizer");

    auto read_image = synthetic_destinations[0];
    if(packed) {
        read_image=create_source_texture(fixture,width,height);
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list){
            std::array barriers{transition_barrier(synthetic_destinations[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),
                transition_barrier(read_image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST)};
            list->ResourceBarrier(UINT(barriers.size()),barriers.data());
            for(UINT eye=0;eye<2;++eye){
                D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=synthetic_destinations[0].Get();
                src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=read_image.Get();
                dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=eye;
                const D3D12_BOX box{eye*width,0,0,(eye+1)*width,height,1};
                list->CopyTextureRegion(&dst,0,0,0,&src,&box);
            }
            for(auto& barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            list->ResourceBarrier(UINT(barriers.size()),barriers.data());
        });
    }
    const StereoPattern actual=readback_pattern(fixture,read_image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The normal backend invocation above proves shader sampling. The NVIDIA
    // fixture reuses this case specifically to exercise its extra work-list
    // lifetime while game motion bypasses OFA; rounding can make its confidence
    // result byte-identical to the smooth same-pixel reference.
    if (backend != xrfg::D3D12OpticalFlowBackend::nvidia) {
        require(actual != same_pixel_blend,
            "DLSS-motion texture was not consumed by the synthesis shader");
    }
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double actual_error = mean_absolute_rgb_error(
            actual, expected, width, height, eye, margin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend, expected, width, height, eye, margin);
        const double unscaled_error = mean_absolute_rgb_error(
            actual, unscaled_expected, width, height, eye, margin);
        const double previous_error = mean_absolute_rgb_error(
            previous, expected, width, height, eye, margin);
        const double current_error = mean_absolute_rgb_error(
            current, expected, width, height, eye, margin);
        std::cout << "DLSS motion eye=" << eye
                  << " synthetic_mae=" << actual_error
                  << " unscaled_mae=" << unscaled_error
                  << " blend_mae=" << blend_error << '\n';
        require(
            actual_error < unscaled_error &&
                actual_error < previous_error && actual_error < current_error,
            "DLSS-motion low-resolution vectors were not converted to output space for eye " +
                std::to_string(eye));
    }

    // Exercise enough advancing pairs to reuse a work slot. NVIDIA owns an
    // additional OFA command list per slot even though this path bypasses OFA.
    std::uint64_t guide_serial = frame_b->serial;
    for (UINT iteration = 0; iteration < 3; ++iteration) {
        const std::uint32_t source_index = iteration % 2U;
        xrfg::D3D12HistoryCaptureTicket capture{};
        require(
            operation_succeeded(history->capture(source_index, &capture)) &&
                operation_succeeded(history->commit(capture)),
            "DLSS-motion slot-reuse capture failed");
        auto guide_left = std::make_shared<xrfg::DlssMotionVectorFrame>(*frame_b);
        guide_left->previous_serial = guide_serial;
        guide_left->serial = ++guide_serial;
        auto guide_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*guide_left);
        guide_right->motion_slice = 1;
        if(packed)guide_right->output_x=width;
        auto guide = std::make_shared<xrfg::DlssMotionVectorSet>();
        guide->eye_count = kEyeCount;
        guide->eyes = {guide_left, guide_right};
        xrfg::D3D12FrameSynthesisTicket advancing{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture, views, views, 0, source_index, &advancing,
                std::nullopt, guide)),
            "DLSS-motion work-slot reuse failed");
        fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    }
    require(
        operation_succeeded(synthesizer.wait_for_idle()),
        "DLSS-motion final drain failed");
    require(
        operation_succeeded(history->invalidate()),
        "DLSS-motion history invalidate failed");
}

enum class VectorScene { occlusion, overlay, fade };
void test_dlss_motion_vector_occlusion_edges(D3D12WarpFixture& fixture, VectorScene kind) {
    constexpr UINT width = 256, height = 128;
    constexpr int object_left = 72, object_width = 80, object_top = 32, object_bottom = 96;
    constexpr int background_motion = 6;
    const bool overlay = kind == VectorScene::overlay, fade = kind == VectorScene::fade;
    const int object_motion = overlay ? 0 : fade ? background_motion : 20;
    const auto make_scene = [&](int background_shift, int object_shift, float brightness = 1.0F) {
        StereoPattern pattern;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto& bytes = pattern[eye];
            bytes.resize(static_cast<std::size_t>(width) * height * kBytesPerPixel);
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    const int ox = static_cast<int>(x) - object_shift;
                    const bool object = static_cast<int>(y) >= object_top &&
                        static_cast<int>(y) < object_bottom && ox >= object_left &&
                        ox < object_left + object_width;
                    const int bx = static_cast<int>(x) - background_shift + 64;
                    const RgbaBytes color = object
                        ? ((ox / 5) % 2 ? RgbaBytes{240, 210, 40, 255} : RgbaBytes{30, 60, 200, 255})
                        : RgbaBytes{static_cast<std::uint8_t>(40 + (bx * 13 + static_cast<int>(y) * 5) % 160),
                                    static_cast<std::uint8_t>(60 + ((bx / 3) % 7) * 20 + eye * 9),
                                    static_cast<std::uint8_t>(90 + (bx * 7) % 120), 255};
                    const RgbaBytes lit{static_cast<std::uint8_t>(std::lround(color[0] * brightness)),
                                        static_cast<std::uint8_t>(std::lround(color[1] * brightness)),
                                        static_cast<std::uint8_t>(std::lround(color[2] * brightness)), 255};
                    std::copy(lit.begin(), lit.end(), bytes.begin() +
                        (static_cast<std::size_t>(y) * width + x) * kBytesPerPixel);
                }
            }
        }
        return pattern;
    };
    const StereoPattern previous = make_scene(0, 0);
    const StereoPattern current = make_scene(background_motion, object_motion, fade ? 0.6F : 1.0F);
    const StereoPattern expected =
        make_scene(background_motion / 2, object_motion / 2, fade ? 0.8F : 1.0F);
    auto game_motion = create_and_upload_game_motion_field(
        fixture, width, height, [&](UINT, UINT x, UINT y) {
            const int ox = static_cast<int>(x) - object_motion;
            const bool object = static_cast<int>(y) >= object_top &&
                static_cast<int>(y) < object_bottom && ox >= object_left &&
                ox < object_left + object_width;
            return std::array<float, 2>{
                static_cast<float>(-(object && !overlay ? object_motion : background_motion)), 0.0F};
        });
    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_source_texture(fixture, width, height),
        create_source_texture(fixture, width, height)};
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_source_texture(fixture, width, height)};
    upload_pattern(fixture, sources[0].Get(), previous);
    upload_pattern(fixture, sources[1].Get(), current);
    std::array<ID3D12Resource*, 2> source_pointers{sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{synthetic_destinations[0].Get()};
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
                source_pointers, D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "occlusion history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(operation_succeeded(synthesizer.initialize(fixture.device(), fixture.queue(),
                history, current_pointers, synthetic_pointers, kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET, xrfg::D3D12OpticalFlowBackend::fidelity_fx)),
        "occlusion synthesizer initialization failed");
    const auto guides = [&](std::uint64_t serial) {
        auto set = std::make_shared<xrfg::DlssMotionVectorSet>();
        set->eye_count = kEyeCount;
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            auto f = std::make_shared<xrfg::DlssMotionVectorFrame>();
            f->stream = 311 + eye;
            f->epoch = 3;
            f->serial = serial;
            f->previous_serial = serial - 1;
            f->motion_vectors = game_motion;
            f->producer_queue = fixture.queue();
            f->output_width = f->motion_width = width;
            f->output_height = f->motion_height = height;
            f->motion_slice = f->output_slice = eye;
            f->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            set->eyes[eye] = f;
        }
        return set;
    };
    const ReprojectionViews views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket capture_a{}, capture_b{};
    require(operation_succeeded(history->capture(0, &capture_a)) &&
            operation_succeeded(history->commit(capture_a)),
        "occlusion capture A failed");
    xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
    require(operation_succeeded(synthesizer.submit_prime(capture_a, views, 0, &prime, guides(1))),
        "occlusion prime failed");
    require_frame_start_gate(synthesizer, "occlusion frame-start gate");
    require(operation_succeeded(history->capture(1, &capture_b)) &&
            operation_succeeded(history->commit(capture_b)),
        "occlusion capture B failed");
    require(operation_succeeded(synthesizer.submit_pair(capture_b, views, views, 0, 1, &pair,
                std::nullopt, guides(2))),
        "occlusion pair failed");
    fixture.execute_and_wait([](ID3D12GraphicsCommandList*) {});
    const StereoPattern actual = readback_pattern(fixture, synthetic_destinations[0].Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The bands either side of each edge of the object at the midpoint.
    const int left = object_left + object_motion / 2, right = left + object_width;
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        double total = 0;
        std::size_t count = 0;
        for (int y = object_top + 2; y < object_bottom - 2; ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                // The overlay is measured whole, the fade everywhere.
                if (fade ? x < 16 || x >= static_cast<int>(width) - 16
                    : overlay ? x < left - 2 || x >= right + 2
                              : std::abs(x - left) >= 12 && std::abs(x - right) >= 12) continue;
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + static_cast<UINT>(x)) * kBytesPerPixel;
                for (UINT c = 0; c < 3; ++c, ++count) {
                    total += std::abs(static_cast<int>(actual[eye][offset + c]) -
                                      static_cast<int>(expected[eye][offset + c]));
                }
            }
        }
        const double edge_error = total / static_cast<double>(count);
        std::cout << "DLSS " << (fade ? "fade" : overlay ? "overlay" : "occlusion") << " eye=" << eye
                  << " edge_mae=" << edge_error << '\n';
        // 0.68, 6.9 and 0.08 here. The same-pixel blend this replaced
        // measured 24.6, 31.6 and 45.7 (it does not follow the motion).
        require(edge_error <= (fade ? 1.0 : overlay ? 12.0 : 2.0),
            "DLSS vectors showed the wrong frame around a moving edge for eye " +
                std::to_string(eye) + ": " + std::to_string(edge_error));
    }
    require(operation_succeeded(synthesizer.wait_for_idle()), "occlusion final drain failed");
    require(operation_succeeded(history->invalidate()), "occlusion history invalidate failed");
}

void test_rotation_aware_synthesis_beats_uncompensated_flow(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kRotationWidth = 160;
    constexpr UINT kRotationHeight = 80;
    constexpr UINT kEvaluationMargin = 20;
    constexpr float kYawRadians = 0.2094395102393195F;

    const ReprojectionViews views_a = make_reprojection_views(0.0F);
    const ReprojectionViews views_b = make_reprojection_views(kYawRadians);
    const ReprojectionViews identity_views = make_reprojection_views(0.0F);
    const StereoPattern previous =
        ray_direction_pattern(kRotationWidth, kRotationHeight, views_a);
    const StereoPattern current =
        ray_direction_pattern(kRotationWidth, kRotationHeight, views_b);
    const StereoPattern same_pixel_blend = midpoint_pattern(previous, current);

    const auto synthesize = [&](const char* label,
                                const ReprojectionViews& source_views_a,
                                const ReprojectionViews& source_views_b,
                                const ReprojectionViews& target_views,
                                const StereoPattern& input_previous,
                                const StereoPattern& input_current,
                                std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_a = {},
                                std::shared_ptr<const xrfg::DlssMotionVectorSet> motion_b = {}, bool r19 = false) {
        std::array<ComPtr<ID3D12Resource>, 2> sources{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
            create_source_texture(fixture, kRotationWidth, kRotationHeight),
        };
        std::array<ID3D12Resource*, 2> source_pointers{
            sources[0].Get(),
            sources[1].Get(),
        };
        std::array<ID3D12Resource*, 2> current_destination_pointers{
            current_destinations[0].Get(),
            current_destinations[1].Get(),
        };
        std::array<ID3D12Resource*, 1> synthetic_destination_pointers{
            synthetic_destinations[0].Get(),
        };
        upload_pattern(fixture, sources[0].Get(), input_previous);
        upload_pattern(fixture, sources[1].Get(), input_current);

        auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
        require(
            operation_succeeded(history->initialize(
                fixture.device(),
                fixture.queue(),
                std::span<ID3D12Resource* const>(
                    source_pointers.data(),
                    source_pointers.size()),
                D3D12_RESOURCE_STATE_RENDER_TARGET)),
            std::string(label) + " history initialization failed");
        xrfg::D3D12FrameSynthesizer synthesizer;
        require(
            operation_succeeded(synthesizer.initialize(
                fixture.device(),
                fixture.queue(),
                history,
                std::span<ID3D12Resource* const>(
                    current_destination_pointers.data(),
                    current_destination_pointers.size()),
                std::span<ID3D12Resource* const>(
                    synthetic_destination_pointers.data(),
                    synthetic_destination_pointers.size()),
                kFormat,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                backend,
                nvidia_options)),
            std::string(label) + " synthesizer initialization failed");

        xrfg::D3D12HistoryCaptureTicket capture_a{};
        require(
            operation_succeeded(history->capture(0, &capture_a)) &&
                operation_succeeded(history->commit(capture_a)),
            std::string(label) + " capture A failed");
        xrfg::D3D12FrameSynthesisTicket prime{};
        require(
            operation_succeeded(synthesizer.submit_prime(
                capture_a,
                source_views_a,
                0,
                &prime,
                std::move(motion_a))),
            std::string(label) + " prime submission failed");
        require_frame_start_gate(synthesizer, "rotation frame-start gate");

        xrfg::D3D12HistoryCaptureTicket capture_b{};
        require(
            operation_succeeded(history->capture(1, &capture_b)) &&
                operation_succeeded(history->commit(capture_b)),
            std::string(label) + " capture B failed");
        xrfg::D3D12FrameSynthesisTicket pair{};
        require(
            operation_succeeded(synthesizer.submit_pair(
                capture_b,
                source_views_b,
                target_views,
                0,
                1,
                &pair,
                std::nullopt,
                std::move(motion_b), false, 0.5F, {}, std::nullopt, r19)),
            std::string(label) + " pair submission failed");
        require(
            pair.previous_serial == capture_a.serial &&
                pair.current_serial == capture_b.serial,
            std::string(label) + " pair chronology is incorrect");
        require(
            operation_succeeded(synthesizer.wait_for_idle()),
            std::string(label) + " synthesis drain failed");

        const StereoPattern actual_current = readback_pattern(
            fixture,
            current_destinations[1].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        const StereoPattern actual_synthetic = readback_pattern(
            fixture,
            synthetic_destinations[0].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(
            actual_current == input_current,
            std::string(label) + " current output is not a bit-exact copy of B");
        require_source_is_render_target(fixture, sources[0].Get());
        require_source_is_render_target(fixture, sources[1].Get());
        require(
            operation_succeeded(history->invalidate()),
            std::string(label) + " history invalidate failed");
        return actual_synthetic;
    };

    const auto validate_cropped_viewport = [&] {
        constexpr xrfg::D3D12ImageRect kCroppedRect{16, 20, 128, 40};
        ReprojectionViews cropped_views_a = make_reprojection_views(0.0F);
        ReprojectionViews cropped_views_b = make_reprojection_views(kYawRadians);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            cropped_views_a[eye].image_rect = kCroppedRect;
            cropped_views_b[eye].image_rect = kCroppedRect;
        }
        const StereoPattern cropped_previous = ray_direction_pattern(
            kRotationWidth, kRotationHeight, cropped_views_a);
        const StereoPattern cropped_current = ray_direction_pattern(
            kRotationWidth, kRotationHeight, cropped_views_b);
        const StereoPattern cropped_synthetic = synthesize(
            "cropped rotation-aware",
            cropped_views_a,
            cropped_views_b,
            cropped_views_b,
            cropped_previous,
            cropped_current);
        for (UINT eye = 0; eye < kEyeCount; ++eye) {
            const double cropped_error = mean_absolute_rgb_error_rect(
                cropped_synthetic,
                cropped_current,
                kRotationWidth,
                eye,
                kCroppedRect,
                8);
            std::cout << "cropped rotation eye=" << eye
                      << " aware_mae=" << cropped_error << '\n';
            require(
                cropped_error <= 1.0,
                "cropped viewport camera mapping is inaccurate for eye " +
                    std::to_string(eye) + ": aware=" +
                    std::to_string(cropped_error));
        }
    };

    const auto views_c=make_reprojection_views(kYawRadians*0.5F);
    const auto expected_c=ray_direction_pattern(kRotationWidth,kRotationHeight,views_c);
    const auto own_camera=synthesize("R19 own camera",views_a,views_b,views_c,previous,current,{}, {},true);
    for (UINT eye=0;eye<kEyeCount;++eye) {
        const auto error=mean_absolute_rgb_error(own_camera,expected_c,kRotationWidth,kRotationHeight,eye,kEvaluationMargin);
        std::cout << "R19 optical camera eye=" << eye << " MAE=" << error << '\n';
        require(error<2.0,"R19 generated pixels do not match submitted target camera");
    }
    if (backend == xrfg::D3D12OpticalFlowBackend::nvidia) {
        validate_cropped_viewport();
        return;
    }

    const StereoPattern rotation_aware = synthesize(
        "rotation-aware",
        views_a,
        views_b,
        views_b,
        previous,
        current);
    ComPtr<ID3D12Resource> zero_game_motion = create_and_upload_game_motion(
        fixture,
        kRotationWidth,
        kRotationHeight,
        {0.0F, 0.0F});
    auto game_frame_a = std::make_shared<xrfg::DlssMotionVectorFrame>();
    game_frame_a->stream = 31;
    game_frame_a->epoch = 1;
    game_frame_a->serial = 1;
    game_frame_a->motion_vectors = zero_game_motion;
    game_frame_a->producer_queue = fixture.queue();
    game_frame_a->output_width = kRotationWidth;
    game_frame_a->output_height = kRotationHeight;
    game_frame_a->motion_width = kRotationWidth;
    game_frame_a->motion_height = kRotationHeight;
    game_frame_a->resource_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    auto game_frame_b = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_a);
    game_frame_b->previous_serial = 1;
    game_frame_b->serial = 2;
    auto game_frame_a_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_a);
    game_frame_a_right->stream = 32;
    game_frame_a_right->motion_slice = 1;
    auto game_frame_b_right = std::make_shared<xrfg::DlssMotionVectorFrame>(*game_frame_b);
    game_frame_b_right->stream = 32;
    game_frame_b_right->motion_slice = 1;
    auto game_set_a = std::make_shared<xrfg::DlssMotionVectorSet>();
    game_set_a->eye_count = kEyeCount;
    game_set_a->eyes = {game_frame_a, game_frame_a_right};
    auto game_set_b = std::make_shared<xrfg::DlssMotionVectorSet>();
    game_set_b->eye_count = kEyeCount;
    game_set_b->eyes = {game_frame_b, game_frame_b_right};
    const StereoPattern game_pose_separated = synthesize(
        "DLSS head-pose separation",
        views_a,
        views_b,
        views_b,
        previous,
        current,
        game_set_a,
        game_set_b);
    const auto game_own_camera=synthesize("R19 game camera",views_a,views_b,views_c,previous,current,game_set_a,game_set_b,true);
    for (UINT eye=0;eye<kEyeCount;++eye)
        require(mean_absolute_rgb_error(game_own_camera,expected_c,kRotationWidth,kRotationHeight,eye,kEvaluationMargin)<2.0,
            "R19 game vector pixels do not match target camera");
    const StereoPattern uncompensated = synthesize(
        "uncompensated-flow baseline",
        identity_views,
        identity_views,
        identity_views,
        previous,
        current);

    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const double aware_error = mean_absolute_rgb_error(
            rotation_aware,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double uncompensated_error = mean_absolute_rgb_error(
            uncompensated,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double blend_error = mean_absolute_rgb_error(
            same_pixel_blend,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double previous_error = mean_absolute_rgb_error(
            previous,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        const double game_pose_error = mean_absolute_rgb_error(
            game_pose_separated,
            current,
            kRotationWidth,
            kRotationHeight,
            eye,
            kEvaluationMargin);
        double uncovered_total = 0.0;
        std::size_t uncovered_samples = 0;
        for (UINT y = 0; y < kRotationHeight; ++y) {
            for (UINT x = 0; x < kRotationWidth; ++x) {
                if (target_pixel_maps_to_source(
                        views_a[eye],
                        views_b[eye],
                        kRotationWidth,
                        kRotationHeight,
                        x,
                        y)) {
                    continue;
                }
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kRotationWidth + x) *
                    kBytesPerPixel;
                for (UINT channel = 0; channel < 3; ++channel) {
                    uncovered_total += std::abs(
                        static_cast<int>(rotation_aware[eye][offset + channel]) -
                        static_cast<int>(current[eye][offset + channel]));
                    ++uncovered_samples;
                }
            }
        }
        require(uncovered_samples != 0, "rotation fixture has no uncovered target edge");
        const double uncovered_error =
            uncovered_total / static_cast<double>(uncovered_samples);
        std::cout << "rotation eye=" << eye
                  << " aware_mae=" << aware_error
                  << " game_pose_mae=" << game_pose_error
                  << " uncompensated_flow_mae=" << uncompensated_error
                  << " blend_mae=" << blend_error
                  << " previous_mae=" << previous_error
                  << " uncovered_mae=" << uncovered_error << '\n';
        require(
            aware_error < uncompensated_error * 0.35 &&
                aware_error < blend_error * 0.35 &&
                aware_error < previous_error * 0.25 &&
                uncovered_error <= 0.5,
            "rotation-aware synthesis does not materially beat uncompensated flow for eye " +
                std::to_string(eye) + ": aware=" + std::to_string(aware_error) +
                " uncompensated=" + std::to_string(uncompensated_error) +
                " uncovered=" + std::to_string(uncovered_error));
        require(
            game_pose_error <= 1.0,
            "DLSS game vectors incorrectly included OpenXR head-pose subtraction for eye " +
                std::to_string(eye) + ": game_pose=" +
                std::to_string(game_pose_error));
    }

    validate_cropped_viewport();
}

void test_double_wide_single_slice_views(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend =
        xrfg::D3D12OpticalFlowBackend::fidelity_fx,
    xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options = {}) {
    constexpr UINT kHalfWidth = 64;
    constexpr UINT kDoubleWidth = kHalfWidth * 2U;
    constexpr UINT kDoubleHeight = 64;

    std::array<ComPtr<ID3D12Resource>, 2> sources{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 2> current_destinations{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    std::array<ComPtr<ID3D12Resource>, 1> synthetic_destinations{
        create_single_slice_texture(fixture, kDoubleWidth, kDoubleHeight),
    };
    clear_double_wide_pattern(
        fixture, sources[0].Get(), kHalfWidth, kDoubleHeight);
    clear_double_wide_pattern(
        fixture, sources[1].Get(), kHalfWidth, kDoubleHeight);

    std::array<ID3D12Resource*, 2> source_pointers{
        sources[0].Get(), sources[1].Get()};
    std::array<ID3D12Resource*, 2> current_pointers{
        current_destinations[0].Get(), current_destinations[1].Get()};
    std::array<ID3D12Resource*, 1> synthetic_pointers{
        synthetic_destinations[0].Get()};

    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(
        operation_succeeded(history->initialize(
            fixture.device(),
            fixture.queue(),
            std::span<ID3D12Resource* const>(
                source_pointers.data(), source_pointers.size()),
            D3D12_RESOURCE_STATE_RENDER_TARGET)),
        "double-wide history initialization failed");
    xrfg::D3D12FrameSynthesizer synthesizer;
    require(
        operation_succeeded(synthesizer.initialize(
            fixture.device(),
            fixture.queue(),
            history,
            std::span<ID3D12Resource* const>(
                current_pointers.data(), current_pointers.size()),
            std::span<ID3D12Resource* const>(
                synthetic_pointers.data(), synthetic_pointers.size()),
            kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            backend,
            nvidia_options)),
        "double-wide synthesizer initialization failed");

    ReprojectionViews views = make_reprojection_views();
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        views[eye].image_rect = {
            eye * kHalfWidth, 0, kHalfWidth, kDoubleHeight};
        views[eye].array_slice = 0;
    }

    xrfg::D3D12HistoryCaptureTicket capture{};
    require(
        operation_succeeded(history->capture(0, &capture)) &&
            operation_succeeded(history->commit(capture)),
        "double-wide capture failed");
    xrfg::D3D12FrameSynthesisTicket prime{};
    require(
        operation_succeeded(synthesizer.submit_prime(
            capture, views, 0, &prime)),
        "double-wide prime failed");
    require_frame_start_gate(synthesizer, "embedded test frame-start gate");
    xrfg::D3D12FrameSynthesisTicket repeated{};
    require(
        operation_succeeded(synthesizer.submit_pair(
            capture, views, views, 0, 1, &repeated)),
        "double-wide repeated pair failed");
    const auto validate_pattern = [&](ID3D12Resource* resource,
                                      const char* label,
                                      double maximum_mae) {
        const std::vector<std::uint8_t> pixels = readback_single_slice(
            fixture, resource, D3D12_RESOURCE_STATE_RENDER_TARGET);
        double total_error = 0.0;
        std::size_t sample_count = 0;
        for (UINT y = 0; y < kDoubleHeight; ++y) {
            for (UINT x = 0; x < kDoubleWidth; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * kDoubleWidth + x) *
                    kBytesPerPixel;
                const RgbaBytes expected = x < kHalfWidth
                    ? RgbaBytes{255, 0, 0, 255}
                    : RgbaBytes{0, 0, 255, 255};
                for (UINT channel = 0; channel < 3; ++channel) {
                    total_error += std::abs(
                        static_cast<int>(pixels[offset + channel]) -
                        static_cast<int>(expected[channel]));
                    ++sample_count;
                }
            }
        }
        const double mae = total_error / static_cast<double>(sample_count);
        if (maximum_mae > 0.0) {
            std::cout << label << " mae=" << mae << '\n';
        }
        require(
            mae <= maximum_mae,
            std::string(label) +
                " does not preserve both single-slice eye viewports: mae=" +
                std::to_string(mae));
    };
    validate_pattern(
        current_destinations[1].Get(), "double-wide current output", 0.0);
    validate_pattern(
        synthetic_destinations[0].Get(), "double-wide synthetic output", 0.0);

    xrfg::D3D12HistoryCaptureTicket advanced_capture{};
    require(
        operation_succeeded(history->capture(1, &advanced_capture)) &&
            operation_succeeded(history->commit(advanced_capture)),
        "double-wide advancing capture failed");
    xrfg::D3D12FrameSynthesisTicket advanced_pair{};
    const HRESULT advanced_pair_result = synthesizer.submit_pair(
        advanced_capture, views, views, 0, 0, &advanced_pair);
    require(
        operation_succeeded(advanced_pair_result),
        "double-wide advancing pair failed with HRESULT " +
            std::to_string(static_cast<std::int32_t>(advanced_pair_result)));
    require(
        operation_succeeded(synthesizer.wait_for_idle()),
        "double-wide advancing synthesis drain failed");
    validate_pattern(
        current_destinations[0].Get(),
        "double-wide advancing current output",
        0.0);
    validate_pattern(
        synthetic_destinations[0].Get(),
        "double-wide advancing synthetic output",
        backend == xrfg::D3D12OpticalFlowBackend::nvidia ? 2.0 : 0.0);
    require(
        operation_succeeded(history->invalidate()),
        "double-wide history invalidate failed");
}

#include "nvidia_fast_patterns.inc"
#include "cinema_color_contract.inc"

// Static encoded grey must not brighten only on the generated frame. A
// typeless resource is deliberate: the XR negotiated view, not GetDesc(),
// determines whether texture sampling must decode sRGB.
void test_srgb_synthesis_preserves_brightness(
    D3D12WarpFixture& fixture,
    xrfg::D3D12OpticalFlowBackend backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx) {
    constexpr UINT width = 256, height = 128;
    constexpr DXGI_FORMAT storage = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    auto a = create_source_texture(fixture, width, height, storage);
    auto b = create_source_texture(fixture, width, height, storage);
    auto real_a = create_source_texture(fixture, width, height, storage);
    auto real_b = create_source_texture(fixture, width, height, storage);
    auto synthetic = create_source_texture(fixture, width, height, storage);
    std::array<ID3D12Resource*, 2> sources{a.Get(), b.Get()};
    std::array<ID3D12Resource*, 2> real{real_a.Get(), real_b.Get()};
    std::array<ID3D12Resource*, 1> generated{synthetic.Get()};
    StereoPattern pattern = translated_motion_pattern(width, height, {0, 0});
    for (auto& eye : pattern)
        for (std::size_t i = 0; i < eye.size(); ++i)
            eye[i] = (i % 4 == 3) ? 255 : 128;
    upload_pattern(fixture, a.Get(), pattern);
    upload_pattern(fixture, b.Get(), pattern);
    auto history = std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(), fixture.queue(),
        sources, D3D12_RESOURCE_STATE_RENDER_TARGET)), "sRGB history init failed");
    xrfg::D3D12FrameSynthesizer synth;
    require(operation_succeeded(synth.initialize(fixture.device(), fixture.queue(),
        history, real, generated, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        D3D12_RESOURCE_STATE_RENDER_TARGET, backend)), "sRGB synthesis init failed");
    const auto views = make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket ca{}, cb{};
    xrfg::D3D12FrameSynthesisTicket prime{}, pair{};
    require(operation_succeeded(history->capture(0, &ca)) &&
        operation_succeeded(history->commit(ca)) &&
        operation_succeeded(synth.submit_prime(ca, views, 0, &prime)), "sRGB prime failed");
    require_frame_start_gate(synth, "sRGB frame-start gate");
    require(operation_succeeded(history->capture(1, &cb)) &&
        operation_succeeded(history->commit(cb)) &&
        operation_succeeded(synth.submit_pair(cb, views, views, 0, 1, &pair)), "sRGB pair failed");
    const auto actual = readback_pattern(fixture, synthetic.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto real_output = readback_pattern(fixture, real_b.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(real_output == pattern, "sRGB real frame changed");
    for (UINT eye = 0; eye < kEyeCount; ++eye) {
        const auto error = mean_absolute_rgb_error(actual, pattern, width, height, eye, 16);
        std::cout << "sRGB synthetic brightness error eye=" << eye << " MAE=" << error << '\n';
        require(error <= 1.0, "synthetic sRGB brightness differs from real frame");
    }
}


#ifdef OFXR_TEST_DJULES_ROUTE
void test_djules_extra_output(D3D12WarpFixture& fixture) {
    constexpr UINT width=256,height=128;
    auto a=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto b=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto ca=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto cb=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto s1=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    auto s2=create_source_texture(fixture,width,height,DXGI_FORMAT_R8G8B8A8_UNORM);
    std::array<ID3D12Resource*,2> inputs{a.Get(),b.Get()}, real{ca.Get(),cb.Get()}, outputs{s1.Get(),s2.Get()};
    // Translate the same scene: a full-screen colour change is a cut and the
    // surviving FidelityFX cut detector correctly returns current on both outputs.
    const auto pa=translated_motion_pattern(width,height,{0,0});
    const auto pb=translated_motion_pattern(width,height,{6,6});
    const auto expected_first=translated_motion_pattern(width,height,{2,2});
    const auto expected_second=translated_motion_pattern(width,height,{4,4});
    upload_pattern(fixture,a.Get(),pa); upload_pattern(fixture,b.Get(),pb);
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require(operation_succeeded(history->initialize(fixture.device(),fixture.queue(),inputs,D3D12_RESOURCE_STATE_RENDER_TARGET)),"extra history");
    xrfg::D3D12FrameSynthesizer synth;
    require(operation_succeeded(synth.initialize(fixture.device(),fixture.queue(),history,real,outputs,
        DXGI_FORMAT_R8G8B8A8_UNORM,D3D12_RESOURCE_STATE_RENDER_TARGET)),"extra initialize");
    const auto views=make_reprojection_views();
    xrfg::D3D12HistoryCaptureTicket ta{},tb{}; xrfg::D3D12FrameSynthesisTicket prime{},pair{};
    require(operation_succeeded(history->capture(0,&ta))&&operation_succeeded(history->commit(ta))&&
        operation_succeeded(synth.submit_prime(ta,views,0,&prime)),"extra prime");
    require_frame_start_gate(synth,"extra prime fence");
    require(operation_succeeded(history->capture(1,&tb))&&operation_succeeded(history->commit(tb)),"extra capture");
    require(synth.submit_pair(tb,views,views,0,1,&pair,{}, {},false,1.0F/3.0F,{},
        xrfg::D3D12ExtraSynthetic{0,2.0F/3.0F})==E_INVALIDARG,"same destination accepted");
    ComPtr<ID3D12Fence> readiness_gate;
    require(operation_succeeded(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&readiness_gate))),"ready gate");
    require(operation_succeeded(fixture.queue()->Wait(readiness_gate.Get(),1)),"ready queue gate");
    const HRESULT submitted=synth.submit_pair(tb,views,views,0,1,&pair,{}, {},false,1.0F/3.0F,{},
        xrfg::D3D12ExtraSynthetic{1,2.0F/3.0F});
    const HRESULT pending=SUCCEEDED(submitted)?synth.ticket_available(pair):submitted;
    readiness_gate->Signal(1); // release before every assertion/destructor
    require(operation_succeeded(submitted),"two-output pair");
    require(pending==S_FALSE,"unfinished owned ticket was reported ready");
    require_frame_start_gate(synth,"two-output fence");
    require(synth.ticket_available(pair)==S_OK,"completed owned ticket was not reported ready");
    require(synth.ticket_available({})==E_INVALIDARG,"empty readiness ticket accepted");
    const auto first=readback_pattern(fixture,s1.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto second=readback_pattern(fixture,s2.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    require(readback_pattern(fixture,cb.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==pb,"3X real copy changed");
    for(UINT eye=0;eye<kEyeCount;++eye) {
        const auto a1=mean_absolute_rgb_error(first,expected_first,width,height,eye,16);
        const auto a2=mean_absolute_rgb_error(first,expected_second,width,height,eye,16);
        const auto b1=mean_absolute_rgb_error(second,expected_first,width,height,eye,16);
        const auto b2=mean_absolute_rgb_error(second,expected_second,width,height,eye,16);
        std::cout << "extra fractions eye=" << eye << " errors=" << a1 << ',' << a2 << ',' << b1 << ',' << b2 << '\n';
        require(a1<a2 && b2<b1,"two outputs lost independent fractions");
    }
    fixture.require_no_debug_errors();
    std::cout << "DJ extra GPU: two fractions, one pair fence, real identity, alias refusal\n";
}
#endif

#include "native_dlssg_gpu.inc"

}  // namespace

#include "r19_upstream_camera.inc"
#include "hybrid_gpu.inc"

#include "history_v474_wait_tests.inc"
#include "synthesis_memory_lifecycle_tests.inc"

int main(int argc, char** argv) {
    const bool trace_test = std::getenv("XRFG_TEST_GPU_TRACE") != nullptr;
    const auto trace_dir = std::filesystem::temp_directory_path() /
        ("xrfg-v072-gpu-" + std::to_string(GetCurrentProcessId()));
    if (trace_test) {
        std::filesystem::create_directories(trace_dir);
        std::ofstream(trace_dir / "ofxr_bridge.ini") <<
            "[diagnostics]\nlogging_enabled=1\nmax_file_mb=32\nflush_each_event=0\n";
        xrfg::bridge_flight_logger().initialize(trace_dir);
    }
    try {
        if(argc>1 && std::string_view(argv[1])=="memory-lifecycle") {
            D3D12WarpFixture fixture;
            std::cout<<"MEMORY_TEST history begin"<<std::endl;
            test_history_memory_lifecycle(fixture);
            std::cout<<"MEMORY_TEST method begin"<<std::endl;
            test_synthesis_method_memory(fixture);
            fixture.require_no_debug_errors();
            return 0;
        }
        if(argc>1 && std::string_view(argv[1])=="init-diagnostics") {
            const auto root=std::filesystem::temp_directory_path()/("ofxr-init-guard-"+std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(root);
            SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
            std::ofstream(root/"ofxr_bridge.ini")<<"[diagnostics]\nlogging_enabled=1\n";
            auto& log=xrfg::bridge_flight_logger();log.initialize(root);
            require(log.enabled(),"initialization trace enabled only for isolated diagnostic fixture");
            D3D12WarpFixture fixture;
            auto color=create_source_texture(fixture);
            ID3D12Resource* sources[]{color.Get()};
            auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
            require_hresult(history->initialize(fixture.device(),fixture.queue(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"diagnostic history");
            std::array<ComPtr<ID3D12Resource>,5> resources;
            for(auto& resource:resources)resource=create_source_texture(fixture);
            ID3D12Resource* current[]{resources[0].Get(),resources[1].Get(),resources[2].Get()};
            ID3D12Resource* synthetic[]{resources[3].Get(),resources[4].Get()};
            const auto initialize=[&](auto device,auto cur,auto syn,DXGI_FORMAT format=kFormat) {
                xrfg::D3D12FrameSynthesizer engine;
                return engine.initialize(device,fixture.queue(),history,cur,syn,format,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::fidelity_fx,{},false,
                    xrfg::D3D12FrameGenerationMode::ofxr,false,true);
            };
            const auto cur=std::span<ID3D12Resource* const>(current);
            const auto syn=std::span<ID3D12Resource* const>(synthetic);
            require_hresult(initialize(fixture.device(),cur,syn),"independent three-current/two-synthetic destinations are valid");
            require(initialize(static_cast<ID3D12Device*>(nullptr),cur,syn)==E_INVALIDARG,"argument guard remains strict");
            current[0]=nullptr;
            require(initialize(fixture.device(),cur,syn)==E_INVALIDARG,"first resource guard remains strict");
            current[0]=resources[0].Get();
            require(initialize(fixture.device(),cur,syn,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)==E_INVALIDARG,"incompatible typed view remains rejected");
            current[1]=current[0];
            require(initialize(fixture.device(),cur,syn)==E_INVALIDARG,"duplicate current resource remains rejected");
            current[1]=resources[1].Get();synthetic[1]=current[0];
            require(initialize(fixture.device(),cur,syn)==E_INVALIDARG,"overlapping synthetic resource remains rejected");
            log.flush();std::ifstream file(log.log_path());std::string text{std::istreambuf_iterator<char>(file),{}};
            for(unsigned stage=1;stage<=5;++stage)require(text.find("a="+std::to_string(0x661D0000ULL|stage)+" ")!=std::string::npos,
                "each rejected initialization contract has a distinct recorded stage");
            file.close();log.shutdown();
            synthetic[1]=resources[4].Get();
            require(initialize(static_cast<ID3D12Device*>(nullptr),cur,syn)==E_INVALIDARG,"disabled logger does not change rejection");
            fixture.require_no_debug_errors();
            std::cout<<"Initialization-only F9 guards: arguments, first texture, view format, current/synthetic uniqueness; valid3/2 destinations PASS\n";
            return 0;
        }
        if(argc>1 && std::string_view(argv[1])=="v474-wait") {
            D3D12WarpFixture fixture;test_v474_bounded_history_wait(fixture);return 0;
        }
        if (argc>1 && std::string_view(argv[1])=="stellar-guides") {
            D3D12WarpFixture guides_fixture;
            test_dlss_motion_vector_stereo_stream_pairing(guides_fixture);
            test_dlss_motion_vector_per_eye_swapchains(guides_fixture);
            test_release_bound_dlss_guides(guides_fixture);
            test_guide_composer(guides_fixture);
            guides_fixture.require_no_debug_errors();
            std::cout << "Stellar R22 resolver/composer GPU checks passed\n";
            return 0;
        }
        if (argc>1 && std::string_view(argv[1])=="hybrid") {
            D3D12WarpFixture hybrid_fixture(true);
            for(const auto scene : {HybridVectorScene::occlusion,HybridVectorScene::overlay,HybridVectorScene::fade}) {
                test_hybrid_motion_vector_occlusion_edges(hybrid_fixture,scene,false);
                test_hybrid_motion_vector_occlusion_edges(hybrid_fixture,scene,true);
                test_hybrid_motion_vector_occlusion_edges(hybrid_fixture,scene,true,true);
            }
            hybrid_fixture.require_no_debug_errors();return 0;
        }
        if (argc>1 && std::string_view(argv[1])=="r19-camera") {
            D3D12WarpFixture camera_fixture(true);
            for (const auto path : {SyntheticCameraPath::flow,SyntheticCameraPath::vectors,SyntheticCameraPath::nvidia,SyntheticCameraPath::native,SyntheticCameraPath::hybrid}) {
                test_synthetic_camera_follows_display_pose(camera_fixture,path,false);
                if (path!=SyntheticCameraPath::native || xrfg::native_dlssg_max_generated_frames(camera_fixture.device())>=2)
                    test_synthetic_camera_follows_display_pose(camera_fixture,path,true);
            }
            test_synthetic_camera_follows_display_pose(camera_fixture,SyntheticCameraPath::native,false,67);
            test_synthetic_camera_follows_display_pose(camera_fixture,SyntheticCameraPath::fsr,false);
            camera_fixture.require_no_debug_errors();
            return 0;
        }
#ifdef XRFG_NATIVE_DLSSG
        if (argc > 1 && std::string_view(argv[1]) == "native-dlssg-capture") {
            if (!native_test_has_supported_nvidia_adapter()) return 77;
            D3D12WarpFixture fixture(true);
            auto& capture = xrfg::manager();
            capture.session = std::make_shared<xrfg::Session>();
            capture.active_frame = 1;
            const auto session = capture.session;
            StereoPattern observed;
            test_native_dlss_packed_stereo(fixture, false, kFormat, false, &observed);
            for (const char* stage : {"dlssg_aligned_previous", "dlssg_seed_motion", "dlssg_seed_depth", "dlssg_packed_current",
                "dlssg_packed_motion", "dlssg_packed_depth", "dlssg_ngx_generated", "dlssg_composed_output"}) {
                const auto found = std::find_if(session->shots.begin(), session->shots.end(),
                    [=](const auto& shot) { return std::string_view(shot->info.stage) == stage; });
                require(found != session->shots.end(), std::string("missing native F8 stage: ") + stage);
                const auto& shot = **found;
                require(SUCCEEDED(shot.status) && shot.fence && shot.fence_value,
                    "native F8 source list was not associated with GPU completion");
                const HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                require(ready != nullptr, "capture fence event");
                require_hresult(shot.fence->SetEventOnCompletion(shot.fence_value, ready), "capture readback fence");
                const DWORD waited = WaitForSingleObject(ready, 5000);
                CloseHandle(ready);
                require(waited == WAIT_OBJECT_0, "native F8 readback did not complete");
            }
            const auto composed = std::find_if(session->shots.begin(), session->shots.end(),
                [](const auto& shot) { return std::string_view(shot->info.stage) == "dlssg_composed_output"; });
            void* mapped{};
            require_hresult((*composed)->readback->Map(0, nullptr, &mapped), "capture composed map");
            for (UINT eye=0; eye<2; ++eye) for (UINT y=0; y<128; ++y) for (UINT x=0; x<256; ++x) {
                const auto* actual = static_cast<const BYTE*>(mapped) + y*(*composed)->footprint.Footprint.RowPitch + (eye*256+x)*4;
                const auto* pixel = observed[eye].data() + (y*256+x)*4;
                require(std::equal(actual, actual+4, pixel), "captured composed pixels differ from the actual rendered output");
            }
            (*composed)->readback->Unmap(0,nullptr);
            require(session->contracts.size() == 4, "native F8 stereo pose/output contracts missing");
            require(session->native_dlssg_evaluations.size()==2,"native F8 must observe reset and actual pair separately");
            const auto& seed=session->native_dlssg_evaluations.front();
            const auto& pair=session->native_dlssg_evaluations.back();
            require(seed.reset && !pair.reset && seed.index==1 && pair.index==1 && pair.count==1,
                "native F8 reset/multiplier call identity lost");
            const auto motion=std::find_if(session->shots.begin(),session->shots.end(),
                [](const auto& shot){return std::string_view(shot->info.stage)=="dlssg_packed_motion";});
            require(seed.motion_rect[2]==64 && seed.motion_rect[3]==64 &&
                pair.motion_rect[2]==(*motion)->description.Width && pair.motion_rect[3]==(*motion)->description.Height &&
                pair.motion_scale[0]==float((*motion)->description.Width) && pair.motion_scale[1]==float((*motion)->description.Height),
                "native F8 must preserve actual SDK rectangles/scales, not the ingress texture sizes");
            require(seed.color!=pair.color && seed.generated==pair.generated && pair.fov>0 && pair.object_separation>0,
                "native F8 seed precision/output linkage or actual heuristics missing");
            session->directory=std::filesystem::temp_directory_path()/ ("xrfg-native-call-capture-"+std::to_string(GetCurrentProcessId()));
            xrfg::write_session(session);
            std::ifstream parameters(session->directory/"native-dlssg-evaluations.tsv");
            std::string header,row;std::getline(parameters,header);unsigned rows{};
            require(header.find("motion_scale")!=std::string::npos && header.find("clip_to_previous")!=std::string::npos,
                "native F8 exported SDK header missing");
            while(std::getline(parameters,row)) {++rows;require(std::count(row.begin(),row.end(),'\t')==std::count(header.begin(),header.end(),'\t'),"native F8 SDK TSV columns shifted");}
            require(rows==2,"native F8 exported SDK rows missing");
            require(!xrfg::two_frame_capture_claim_dlssg_pair(17), "native F8 duplicate pair was accepted");
            capture.active_frame = 0;
            xrfg::two_frame_capture_native_dlssg_evaluation({});
            require(session->native_dlssg_evaluations.size()==2,"inactive native F8 collected another SDK call");
            capture.session.reset();
            fixture.require_no_debug_errors();
            std::cout << "Native F8 real provider: six GPU stages, exact stereo midpoint pixels, completion fences and four pose contracts passed\n";
            return 0;
        }
        if (argc > 1 && (std::string_view(argv[1]) == "native-dlssg" ||
                std::string_view(argv[1]) == "native-dlssg-late" ||
                std::string_view(argv[1]) == "native-dlssg-core-compat")) {
            if (!native_test_has_supported_nvidia_adapter()) {
                std::cout << "Native DLSS-G test requires a compatible NVIDIA D3D12 adapter\n";
                return 77;
            }
            D3D12WarpFixture fixture(true);
            const bool compat = std::string_view(argv[1]) == "native-dlssg-core-compat";
            if (compat) {
                require(argc == 3, "core compatibility fixture needs an old provider path");
                const auto old = std::filesystem::absolute(argv[2]);
                require(std::filesystem::is_regular_file(old), "old provider fixture is missing");
                const auto dir = old.parent_path(); const wchar_t* paths[]{dir.c_str()};
                const auto logs = trace_dir / L"legacy-core";
                std::filesystem::create_directories(logs);
                NVSDK_NGX_FeatureCommonInfo info{}; info.PathListInfo = {paths, 1};
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Init_with_ProjectID(
                    "2152e8d8-a3d7-4d4c-88a5-37f740f81036", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                    "OFXR-Bridge", logs.c_str(), fixture.device(), &info)), "legacy core initialization");
                NVSDK_NGX_Parameter* caps{}; int available{}, init{};
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)), "legacy core caps");
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
                NVSDK_NGX_D3D12_DestroyParameters(caps);
                require(available && static_cast<NVSDK_NGX_Result>(init) == NVSDK_NGX_Result_Success,
                    "fixture must reproduce a core that advertises working FG");
                HMODULE module{};
                require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    old.c_str(), &module), "legacy core did not load the requested game provider");
                std::cout << "Legacy core advertises FG; testing real evaluation refusal/private recovery\n";
                test_native_dlss_packed_stereo(fixture, false, kFormat, true);
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)), "game core still alive");
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &init);
                NVSDK_NGX_D3D12_DestroyParameters(caps);
                require(available && static_cast<NVSDK_NGX_Result>(init) == NVSDK_NGX_Result_Success,
                    "private recovery altered the game's core capability ownership");
                fixture.require_no_debug_errors();
                std::cout << "Legacy core refusal/private recovery and actual stereo midpoint pixels passed\n";
                return 0;
            }
            const bool late=std::string_view(argv[1])=="native-dlssg-late";
            if(late) {
                const auto logs=std::filesystem::temp_directory_path()/ ("ofxr-late-ngx-"+std::to_string(GetCurrentProcessId()));
                std::filesystem::create_directories(logs/L"empty");
                const auto empty=logs/L"empty";const wchar_t* paths[]{empty.c_str()};
                NVSDK_NGX_FeatureCommonInfo info{};info.PathListInfo={paths,1};
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_Init_with_ProjectID(
                    "2152e8d8-a3d7-4d4c-88a5-37f740f81036",NVSDK_NGX_ENGINE_TYPE_CUSTOM,"OFXR-Bridge",
                    logs.c_str(),fixture.device(),&info)),"late fixture first initialization");
                NVSDK_NGX_Parameter* caps{};int available=1,init=0;
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)),"late first caps");
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,&init);
                NVSDK_NGX_D3D12_DestroyParameters(caps);
                require(!available && static_cast<NVSDK_NGX_Result>(init)==NVSDK_NGX_Result_FAIL_FeatureNotFound,
                    "late fixture did not reproduce original FeatureNotFound");
                require(argc==3,"late fixture needs its laboratory provider source");
                wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
                std::filesystem::copy_file(argv[2],std::filesystem::path(exe).parent_path()/L"nvngx_dlssg.dll");
                std::cout<<"First core initialization reproduced FeatureNotFound; entering private late recovery\n";
            }
            if (xrfg::native_dlssg_max_generated_frames(fixture.device()) == 0) {
                require(!late,"late native provider recovery failed");
                std::cout << "Native DLSS-G unavailable on the installed adapter/driver/runtime\n";
                return 77;
            }
            test_native_dlss_packed_stereo(fixture, false);
            test_native_dlss_nvidia_prime(fixture);
            test_native_counter_bypass(fixture);
            test_native_dlss_packed_stereo(fixture, true);
            test_native_dlss_packed_stereo(fixture, false, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            test_native_dlss_packed_stereo(fixture, true, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
            test_native_dlss_reseeds_only_for_camera_motion(fixture);
            if(late) {
                NVSDK_NGX_Parameter* caps{};int available=1,init=0;
                require(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_GetCapabilityParameters(&caps)),"late core still alive");
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available,&available);
                caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult,&init);
                NVSDK_NGX_D3D12_DestroyParameters(caps);
                require(!available && static_cast<NVSDK_NGX_Result>(init)==NVSDK_NGX_Result_FAIL_FeatureNotFound,
                    "private FG changed core capability ownership");
            }
            fixture.require_no_debug_errors();
            std::cout << "Native DLSS-G packed stereo/cropped/sRGB/reseed GPU tests passed\n";
            return 0;
        }
#endif
#ifdef OFXR_TEST_DJULES_ROUTE
        if (argc>1 && std::string_view(argv[1])=="djules-extra") {
            D3D12WarpFixture fixture; test_djules_extra_output(fixture); return 0;
        }
#endif
        if (argc > 1 && std::string_view(argv[1]) == "native-unsupported") {
            D3D12WarpFixture fixture;
            require(xrfg::native_dlssg_max_generated_frames(fixture.device())==0,
                "software adapter admitted native frame generation");
            require_hresult(fixture.device()->GetDeviceRemovedReason(),"unsupported native probe removed device");
            fixture.require_no_debug_errors();
            std::cout<<"Unsupported WARP adapter: native refusal, no GPU errors, real-frame path intact\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "packed-game-vectors") {
            const bool nvidia=argc>2 && std::string_view(argv[2])=="nvidia";
            D3D12WarpFixture fixture(nvidia);
            test_dlss_motion_vector_gpu_ingress(fixture,nvidia?xrfg::D3D12OpticalFlowBackend::nvidia:
                xrfg::D3D12OpticalFlowBackend::fidelity_fx,true);
            fixture.require_no_debug_errors();
            std::cout<<"Packed game vectors: both eyes consumed, opposite motion, output-space scaling and work-slot reuse passed\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "release-bound-guides") {
            D3D12WarpFixture fixture;
            test_release_bound_dlss_guides(fixture);
            fixture.require_no_debug_errors();
            return 0;
        }
        if(argc>1&&std::string_view(argv[1])=="gta-origin-continuity") {
            D3D12WarpFixture fixture;test_gta_origin_continuity(fixture);fixture.require_no_debug_errors();return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "gta-reset-origins") {
            D3D12WarpFixture fixture;
            test_luke_interleaved_origins(fixture,2,true);
            fixture.require_no_debug_errors();
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "protected-guides") {
            D3D12WarpFixture fixture;
            test_public_video_allocation_owner(fixture);
            test_protected_guide_snapshots(fixture);
            test_luke_multi_handle_origins(fixture);
            test_luke_interleaved_origins(fixture,1);
            test_luke_interleaved_origins(fixture,2);
            test_luke_production_camera_units(fixture);
            fixture.require_no_debug_errors();
            return 0;
        }
        if(argc>1 && std::string_view(argv[1])=="copy-deferred") {
            D3D12WarpFixture fixture;
            test_copy_only_afw_transport(fixture,xrfg::D3D12OpticalFlowBackend::fidelity_fx,true);
            test_copy_only_afw_transport(fixture,xrfg::D3D12OpticalFlowBackend::nvidia,true);
            fixture.require_no_debug_errors();
            std::cout<<"Deferred AFW copy: WARP without OFA, blocked ownership, exact stereo pixels, live fallback and monotonic upgrade fence PASS\n";return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "copy-only") {
            D3D12WarpFixture fixture;
            test_copy_only_afw_transport(fixture);
            fixture.require_no_debug_errors();
            std::cout << "D3D12 WARP copy-only AFW transport/fence/lease test passed\n";
            return 0;
        }
        if (GetEnvironmentVariableA("XRFG_TEST_NVIDIA_QUALITY", nullptr, 0)) {
            run_nvidia_fast_controls();
            return 0;
        }
        D3D12WarpFixture fixture;
        test_cinema_color_contract(fixture);
        test_srgb_synthesis_preserves_brightness(fixture);
        test_fsr_generated_array_follows_color_extent(fixture);
        test_stereo_capture_ring(fixture);
        test_capture_is_async_and_consumer_fence_blocks_reuse(fixture);
        test_depth_capture_path(fixture);
        test_depth_private_history_allows_shader_resource_views(fixture);
        test_copy_only_afw_transport(fixture);
        test_rolling_frame_synthesizer(fixture);
        test_double_wide_single_slice_views(fixture);
        test_stereo_motion_synthesis_beats_same_pixel_blend(fixture);
        test_dlss_motion_vector_stereo_stream_pairing(fixture);
        test_dlss_motion_vector_per_eye_swapchains(fixture);
        test_guide_composer(fixture);
        test_dlss_motion_vector_occlusion_edges(fixture, VectorScene::occlusion);
        test_dlss_motion_vector_occlusion_edges(fixture, VectorScene::overlay);
        test_dlss_motion_vector_occlusion_edges(fixture, VectorScene::fade);
        test_dlss_motion_vector_submission_handoff(fixture);
        test_dlss_motion_vector_gpu_ingress(fixture);
        test_rotation_aware_synthesis_beats_uncompensated_flow(fixture);
        fixture.require_no_debug_errors();
        std::array<char, 8> nvidia_test{};
        const DWORD nvidia_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_NVIDIA",
            nvidia_test.data(),
            static_cast<DWORD>(nvidia_test.size()));
        if (nvidia_test_length == 1 && nvidia_test[0] == '1') {
            xrfg::D3D12NvidiaOpticalFlowOptions nvidia_options;
            std::array<char, 16> preset{};
            const DWORD preset_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_PRESET",
                preset.data(),
                static_cast<DWORD>(preset.size()));
            if (preset_length == 4 && _stricmp(preset.data(), "slow") == 0) {
                nvidia_options.preset =
                    xrfg::D3D12NvidiaPerformancePreset::slow;
            } else if (preset_length == 4 && _stricmp(preset.data(), "fast") == 0) {
                nvidia_options.preset =
                    xrfg::D3D12NvidiaPerformancePreset::fast;
            }
            std::array<char, 8> input_scale{};
            const DWORD input_scale_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_INPUT_SCALE",
                input_scale.data(),
                static_cast<DWORD>(input_scale.size()));
            if (input_scale_length == 3 &&
                std::string_view(input_scale.data(), input_scale_length) == "100") {
                nvidia_options.input_scale = xrfg::D3D12NvidiaInputScale::full;
            } else if (input_scale_length == 2 &&
                std::string_view(input_scale.data(), input_scale_length) ==
                    "75") {
                nvidia_options.input_scale =
                    xrfg::D3D12NvidiaInputScale::three_quarter;
            } else if (input_scale_length == 2 &&
                       std::string_view(
                           input_scale.data(), input_scale_length) == "50") {
                nvidia_options.input_scale =
                    xrfg::D3D12NvidiaInputScale::half;
            }
            std::array<char, 8> bidirectional{};
            const DWORD bidirectional_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_BIDIRECTIONAL",
                bidirectional.data(),
                static_cast<DWORD>(bidirectional.size()));
            nvidia_options.bidirectional = bidirectional_length == 1 &&
                bidirectional[0] == '1';
            D3D12WarpFixture nvidia_fixture(true);
            test_srgb_synthesis_preserves_brightness(nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            test_stereo_motion_synthesis_beats_same_pixel_blend(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                true,
                nvidia_options);
            test_double_wide_single_slice_views(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
            test_rotation_aware_synthesis_beats_uncompensated_flow(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
            test_dlss_motion_vector_gpu_ingress(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            nvidia_fixture.require_no_debug_errors();
            std::cout << "D3D12 NVIDIA OFA synthesis test passed\n";
        }
        std::array<char, 8> nvidia_cropped_test{};
        const DWORD nvidia_cropped_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_NVIDIA_CROPPED",
            nvidia_cropped_test.data(),
            static_cast<DWORD>(nvidia_cropped_test.size()));
        if (nvidia_cropped_test_length == 1 &&
            nvidia_cropped_test[0] == '1') {
            D3D12WarpFixture nvidia_cropped_fixture(true);
            test_rotation_aware_synthesis_beats_uncompensated_flow(
                nvidia_cropped_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia);
            nvidia_cropped_fixture.require_no_debug_errors();
            std::cout << "D3D12 NVIDIA OFA cropped synthesis test passed\n";
        }
        std::array<char, 8> hardware_repeat_test{};
        const DWORD hardware_repeat_test_length = GetEnvironmentVariableA(
            "XRFG_TEST_HARDWARE_REPEAT",
            hardware_repeat_test.data(),
            static_cast<DWORD>(hardware_repeat_test.size()));
        if (hardware_repeat_test_length == 1 &&
            hardware_repeat_test[0] == '1') {
            D3D12WarpFixture hardware_repeat_fixture(true);
            test_stereo_motion_synthesis_beats_same_pixel_blend(
                hardware_repeat_fixture,
                xrfg::D3D12OpticalFlowBackend::fidelity_fx,
                true);
            hardware_repeat_fixture.require_no_debug_errors();
            std::cout << "D3D12 hardware repeated-capture test passed\n";
        }
        std::cout << "D3D12 WARP asynchronous history/synthesis tests passed (embedded DLSS guide contract)\n";
        if (trace_test) {
            const auto path = xrfg::bridge_flight_logger().log_path();
            std::ifstream input(path);
            const std::string contents{std::istreambuf_iterator<char>(input), {}};
            require(contents.find("op=history_capture result=0") != std::string::npos,
                "successful GPU capture breadcrumb missing");
            require(contents.find("op=history_capture result=-2147024726") != std::string::npos,
                "busy capture HRESULT breadcrumb missing");
            require(contents.find("op=gpu_fence_snapshot result=2") != std::string::npos,
                "consumer fence snapshot missing");
            if (std::getenv("XRFG_TEST_NVIDIA")) {
                require(contents.find("op=nvidia_eye_execute result=0") != std::string::npos &&
                    contents.find("op=nvidia_composite_submit result=0") != std::string::npos &&
                    contents.find("op=gpu_fence_snapshot result=6") != std::string::npos,
                    "NVIDIA submission/completion breadcrumbs missing");
            }
            xrfg::bridge_flight_logger().shutdown();
            input.close();
            std::filesystem::remove_all(trace_dir);
        }
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 WARP asynchronous history/synthesis test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
