#include "xrfg/d3d12_history.hpp"
#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
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

    template <typename Recorder>
    void execute_and_wait(Recorder&& recorder) {
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
    UINT height = kHeight) {
    const D3D12_HEAP_PROPERTIES properties = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC description = stereo_texture_description(width, height);
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
    UINT y) noexcept {
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
        for (UINT y = 0; y < kHeight; ++y) {
            const std::byte* actual =
                mapped_bytes + layouts[eye].Offset +
                static_cast<std::size_t>(y) * layouts[eye].Footprint.RowPitch;
            const std::uint8_t* wanted =
                expected[eye].data() +
                static_cast<std::size_t>(y) * kWidth * kBytesPerPixel;
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

    xrfg::D3D12HistoryCaptureTicket blocked_capture_d{};
    require(
        history->capture(0, &blocked_capture_d) == HRESULT_FROM_WIN32(ERROR_BUSY) &&
            blocked_capture_d.serial == 0,
        "history overwrote A at the first wrap before the A/B synthesis fence completed");

    require_hresult(gate->Signal(1), "ID3D12Fence::Signal(synthesis gate)");
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
                                const StereoPattern& input_current) {
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
                &prime)),
            std::string(label) + " prime submission failed");

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
                &pair)),
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

}  // namespace

int main() {
    try {
        D3D12WarpFixture fixture;
        test_stereo_capture_ring(fixture);
        test_capture_is_async_and_consumer_fence_blocks_reuse(fixture);
        test_depth_capture_path(fixture);
        test_depth_private_history_allows_shader_resource_views(fixture);
        test_rolling_frame_synthesizer(fixture);
        test_double_wide_single_slice_views(fixture);
        test_stereo_motion_synthesis_beats_same_pixel_blend(fixture);
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
            }
            std::array<char, 8> input_scale{};
            const DWORD input_scale_length = GetEnvironmentVariableA(
                "XRFG_TEST_NVIDIA_INPUT_SCALE",
                input_scale.data(),
                static_cast<DWORD>(input_scale.size()));
            if (input_scale_length == 2 &&
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
            test_stereo_motion_synthesis_beats_same_pixel_blend(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                true,
                nvidia_options);
            test_double_wide_single_slice_views(
                nvidia_fixture,
                xrfg::D3D12OpticalFlowBackend::nvidia,
                nvidia_options);
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
        std::cout << "D3D12 WARP asynchronous history/synthesis tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "D3D12 WARP asynchronous history/synthesis test failed: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
