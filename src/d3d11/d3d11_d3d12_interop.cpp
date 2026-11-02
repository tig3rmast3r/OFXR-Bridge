#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include "xrfg/owned_d11_output.hpp"

#include <windows.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

struct UniqueHandle {
    HANDLE value{};

    ~UniqueHandle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }

    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
};

[[nodiscard]] bool same_luid(const LUID& left, const LUID& right) noexcept {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

[[nodiscard]] HRESULT same_com_identity(
    IUnknown* left,
    IUnknown* right,
    bool* same) noexcept {
    if (same != nullptr) {
        *same = false;
    }
    if (left == nullptr || right == nullptr || same == nullptr) {
        return E_POINTER;
    }
    ComPtr<IUnknown> left_identity;
    HRESULT result = left->QueryInterface(
        IID_PPV_ARGS(left_identity.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IUnknown> right_identity;
    result = right->QueryInterface(
        IID_PPV_ARGS(right_identity.GetAddressOf()));
    if (SUCCEEDED(result)) {
        *same = left_identity.Get() == right_identity.Get();
    }
    return result;
}

[[nodiscard]] HRESULT d3d11_adapter_luid(
    ID3D11Device* device,
    LUID* luid) noexcept {
    if (device == nullptr || luid == nullptr) {
        return E_POINTER;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(dxgi_device.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IDXGIAdapter> adapter;
    result = dxgi_device->GetAdapter(adapter.GetAddressOf());
    if (FAILED(result)) {
        return result;
    }
    DXGI_ADAPTER_DESC description{};
    result = adapter->GetDesc(&description);
    if (SUCCEEDED(result)) {
        *luid = description.AdapterLuid;
    }
    return result;
}

enum class CopyFormatGroup {
    none,
    rgba8,
    bgra8,
};

[[nodiscard]] CopyFormatGroup copy_format_group(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return CopyFormatGroup::rgba8;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return CopyFormatGroup::bgra8;
        default:
            return CopyFormatGroup::none;
    }
}

[[nodiscard]] bool copy_compatible_format(
    DXGI_FORMAT expected,
    DXGI_FORMAT actual) noexcept {
    if (expected == actual) {
        return true;
    }
    const CopyFormatGroup expected_group = copy_format_group(expected);
    return expected_group != CopyFormatGroup::none &&
           expected_group == copy_format_group(actual);
}

[[nodiscard]] bool matching_mip_zero_shape(
    const D3D11_TEXTURE2D_DESC& expected,
    const D3D11_TEXTURE2D_DESC& actual) noexcept {
    return expected.Width == actual.Width &&
           expected.Height == actual.Height &&
           expected.ArraySize == actual.ArraySize &&
           copy_compatible_format(expected.Format, actual.Format) &&
           expected.SampleDesc.Count == actual.SampleDesc.Count &&
           expected.SampleDesc.Quality == actual.SampleDesc.Quality &&
           actual.MipLevels != 0 && actual.Usage == D3D11_USAGE_DEFAULT;
}

[[nodiscard]] HRESULT validate_images(
    ID3D11Device* expected_device,
    std::span<ID3D11Texture2D* const> images,
    const D3D11_TEXTURE2D_DESC& expected_description,
    bool require_matching_mip_count) noexcept {
    if (expected_device == nullptr || images.empty()) {
        return E_INVALIDARG;
    }
    for (ID3D11Texture2D* image : images) {
        if (image == nullptr) {
            return E_INVALIDARG;
        }
        ComPtr<ID3D11Device> actual_device;
        image->GetDevice(actual_device.GetAddressOf());
        bool same_device = false;
        const HRESULT identity_result = same_com_identity(
            actual_device.Get(), expected_device, &same_device);
        if (FAILED(identity_result) || !same_device) {
            return FAILED(identity_result) ? identity_result : E_INVALIDARG;
        }
        D3D11_TEXTURE2D_DESC description{};
        image->GetDesc(&description);
        if (!matching_mip_zero_shape(expected_description, description) ||
            (require_matching_mip_count &&
             description.MipLevels != expected_description.MipLevels)) {
            return E_INVALIDARG;
        }
    }
    return S_OK;
}

void copy_mip_zero(
    ID3D11DeviceContext* context,
    ID3D11Texture2D* destination,
    ID3D11Texture2D* source,
    UINT array_size,
    UINT source_mip_levels,
    UINT destination_mip_levels) noexcept {
    for (UINT slice = 0; slice < array_size; ++slice) {
        context->CopySubresourceRegion(
            destination,
            D3D11CalcSubresource(0, slice, destination_mip_levels),
            0,
            0,
            0,
            source,
            D3D11CalcSubresource(0, slice, source_mip_levels),
            nullptr);
    }
}

}  // namespace

HRESULT create_d3d12_device_for_d3d11(
    ID3D11Device* d3d11_device,
    ID3D12Device** d3d12_device,
    ID3D12CommandQueue** d3d12_queue) noexcept {
    if (d3d12_device != nullptr) {
        *d3d12_device = nullptr;
    }
    if (d3d12_queue != nullptr) {
        *d3d12_queue = nullptr;
    }
    if (d3d11_device == nullptr || d3d12_device == nullptr ||
        d3d12_queue == nullptr) {
        return E_POINTER;
    }
    try {
        ComPtr<IDXGIDevice> dxgi_device;
        HRESULT result = d3d11_device->QueryInterface(
            IID_PPV_ARGS(dxgi_device.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        ComPtr<IDXGIAdapter> adapter;
        result = dxgi_device->GetAdapter(adapter.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }

        ComPtr<ID3D12Device> device;
        result = D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        result = device->CreateCommandQueue(
            &queue_description,
            IID_PPV_ARGS(queue.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        *d3d12_device = device.Detach();
        *d3d12_queue = queue.Detach();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

struct D3D11D3D12SwapchainInterop::Impl {
    struct SharedImage {
        ComPtr<ID3D12Resource> d3d12;
        ComPtr<ID3D11Texture2D> d3d11;
    };

    ComPtr<ID3D11Device> d3d11_device;
    ComPtr<ID3D11DeviceContext> d3d11_context;
    ComPtr<ID3D11Device5> d3d11_device5;
    ComPtr<ID3D11DeviceContext4> d3d11_context4;
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    // Each timeline has one writer. A late signal from the other API can
    // otherwise rewind an already completed publication and strand synthesis.
    ComPtr<ID3D12Fence> d3d12_fence;
    ComPtr<ID3D11Fence> d3d11_fence;
    ComPtr<ID3D12Fence> native_fence12;
    ComPtr<ID3D11Fence> native_fence11;
    ComPtr<ID3D12CommandQueue> synthesis_queue;
    ComPtr<ID3D12Fence> synthesis_fence12;
    ComPtr<ID3D11Fence> synthesis_fence11;
    std::uint64_t last_synthesis_signal_value{};
    HANDLE fence_event{};
    std::vector<ComPtr<ID3D11Texture2D>> source_images;
    std::vector<ComPtr<ID3D11Texture2D>> current_destination_images;
    std::vector<ComPtr<ID3D11Texture2D>> synthetic_destination_images;
    std::vector<SharedImage> shared_sources;
    std::vector<SharedImage> shared_current_destinations;
    std::vector<SharedImage> shared_synthetic_destinations;
    std::vector<std::uint64_t> current_d3d11_access;
    std::vector<std::uint64_t> synthetic_d3d11_access;
    std::vector<ID3D12Resource*> source_d3d12_views;
    std::vector<ID3D12Resource*> current_d3d12_views;
    std::vector<ID3D12Resource*> synthetic_d3d12_views;
    D3D11_TEXTURE2D_DESC source_description{};
    std::uint64_t next_fence_value{1};
    std::uint64_t last_d3d11_signal_value{};
    std::uint64_t last_d3d12_signal_value{};
    std::uint64_t last_d3d12_access_value{};
    std::uint64_t last_d3d11_access_value{};
    D3D11PublicationSnapshot last_publication{};
    bool enabled{};

    ~Impl() {
        if (fence_event != nullptr) {
            CloseHandle(fence_event);
        }
    }

    [[nodiscard]] HRESULT create_shared_image(
        SharedImage* output,
        D3D11InteropInitializationStage create_stage,
        D3D11InteropInitializationStage handle_stage,
        D3D11InteropInitializationStage device1_stage,
        D3D11InteropInitializationStage open_stage,
        D3D11InteropInitializationStage* failure_stage) noexcept {
        if (output == nullptr) {
            return E_POINTER;
        }
        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = source_description.Width;
        description.Height = source_description.Height;
        description.DepthOrArraySize = static_cast<UINT16>(
            source_description.ArraySize);
        description.MipLevels = 1;
        description.Format = source_description.Format;
        description.SampleDesc = source_description.SampleDesc;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);

        HRESULT result = d3d12_device->CreateCommittedResource(
            &heap_properties,
            D3D12_HEAP_FLAG_SHARED,
            &description,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(output->d3d12.GetAddressOf()));
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = create_stage;
            }
            return result;
        }
        UniqueHandle handle;
        result = d3d12_device->CreateSharedHandle(
            output->d3d12.Get(),
            nullptr,
            GENERIC_ALL,
            nullptr,
            &handle.value);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = handle_stage;
            }
            return result;
        }
        ComPtr<ID3D11Device1> device1;
        result = d3d11_device.As(&device1);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = device1_stage;
            }
            return result;
        }
        result = device1->OpenSharedResource1(
            handle.value,
            IID_PPV_ARGS(output->d3d11.GetAddressOf()));
        if (FAILED(result) && failure_stage != nullptr) {
            *failure_stage = open_stage;
        }
        return result;
    }

    [[nodiscard]] HRESULT create_shared_images(
        std::size_t count,
        std::vector<SharedImage>* images,
        std::vector<ID3D12Resource*>* views,
        D3D11InteropInitializationStage create_stage,
        D3D11InteropInitializationStage handle_stage,
        D3D11InteropInitializationStage device1_stage,
        D3D11InteropInitializationStage open_stage,
        D3D11InteropInitializationStage* failure_stage,
        const std::vector<ComPtr<ID3D11Texture2D>>* owned_outputs=nullptr) {
        images->resize(count);
        views->reserve(count);
        for (std::size_t index=0;index<count;++index) {
            auto& image=(*images)[index];
            if(owned_outputs) {
                const auto& output=(*owned_outputs)[index];
                image.d3d12=owned_d11_output::backing_for(output.Get(),d3d12_device.Get());
                if(image.d3d12) {
                    image.d3d11=output;
                    views->push_back(image.d3d12.Get());
                    continue;
                }
            }
            const HRESULT result = create_shared_image(
                &image,
                create_stage,
                handle_stage,
                device1_stage,
                open_stage,
                failure_stage);
            if (FAILED(result)) {
                return result;
            }
            views->push_back(image.d3d12.Get());
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT allocate_fence_value(std::uint64_t* value) noexcept {
        if (value == nullptr) {
            return E_POINTER;
        }
        if (next_fence_value == 0 ||
            next_fence_value == std::numeric_limits<std::uint64_t>::max()) {
            enabled = false;
            return E_FAIL;
        }
        *value = next_fence_value;
        ++next_fence_value;
        return S_OK;
    }

    [[nodiscard]] HRESULT initialize(
        ID3D11Device* input_d3d11_device,
        ID3D11DeviceContext* input_d3d11_context,
        ID3D12Device* input_d3d12_device,
        ID3D12CommandQueue* input_d3d12_queue,
        std::span<ID3D11Texture2D* const> input_sources,
        std::span<ID3D11Texture2D* const> input_current_destinations,
        std::span<ID3D11Texture2D* const> input_synthetic_destinations,
        D3D11InteropInitializationStage* failure_stage) {
        if (failure_stage != nullptr) {
            *failure_stage = D3D11InteropInitializationStage::complete;
        }
        if (input_d3d11_device == nullptr || input_d3d11_context == nullptr ||
            input_d3d12_device == nullptr || input_d3d12_queue == nullptr ||
            input_sources.empty() || input_current_destinations.empty() ||
            input_synthetic_destinations.empty() ||
            input_d3d12_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::arguments;
            }
            return E_INVALIDARG;
        }

        ComPtr<ID3D11Device> context_device;
        input_d3d11_context->GetDevice(context_device.GetAddressOf());
        bool same_context_device = false;
        HRESULT result = same_com_identity(
            context_device.Get(), input_d3d11_device, &same_context_device);
        if (FAILED(result) || !same_context_device) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::context_device;
            }
            return FAILED(result) ? result : E_INVALIDARG;
        }
        LUID d3d11_luid{};
        result = d3d11_adapter_luid(input_d3d11_device, &d3d11_luid);
        if (FAILED(result) ||
            !same_luid(d3d11_luid, input_d3d12_device->GetAdapterLuid())) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::adapter_luid;
            }
            return FAILED(result) ? result : E_INVALIDARG;
        }

        input_sources.front()->GetDesc(&source_description);
        if (source_description.Width == 0 || source_description.Height == 0 ||
            source_description.MipLevels == 0 || source_description.ArraySize == 0 ||
            source_description.ArraySize >
                D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION ||
            source_description.SampleDesc.Count != 1 ||
            source_description.SampleDesc.Quality != 0 ||
            source_description.Usage != D3D11_USAGE_DEFAULT) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::source_description;
            }
            return E_INVALIDARG;
        }
        result = validate_images(
            input_d3d11_device,
            input_sources,
            source_description,
            true);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::source_validation;
            }
            return result;
        }
        result = validate_images(
            input_d3d11_device,
            input_current_destinations,
            source_description,
            false);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::current_validation;
            }
            return result;
        }
        result = validate_images(
            input_d3d11_device,
            input_synthetic_destinations,
            source_description,
            false);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::synthetic_validation;
            }
            return result;
        }

        d3d11_device = input_d3d11_device;
        d3d11_context = input_d3d11_context;
        d3d12_device = input_d3d12_device;
        d3d12_queue = input_d3d12_queue;
        result = d3d11_device.As(&d3d11_device5);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::device5;
            }
            return result;
        }
        result = d3d11_context.As(&d3d11_context4);
        if (FAILED(result)) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::context4;
            }
            return result;
        }

        // A D3D11 immediate context is not free-threaded, and once the layer
        // promotes a presenter thread it stops being touched by one thread only:
        // the presenter runs xrEndFrame, which makes the runtime submit swapchain
        // images through this same context, while the application thread is still
        // rendering and running publish() on it. Applications that never expected
        // a second thread do not turn protection on for themselves - UE4's D3D11
        // RHI does not - and an unprotected context corrupts under that access,
        // which surfaces as a GPU fault rather than an error the layer could fail
        // open on. Enable it for the life of the device; it is never turned back
        // off because the application may keep its own threads on the context.
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(d3d11_context.As(&multithread)) && multithread) {
            static_cast<void>(multithread->SetMultithreadProtected(TRUE));
        }

        for (ID3D11Texture2D* image : input_sources) {
            source_images.emplace_back(image);
        }
        for (ID3D11Texture2D* image : input_current_destinations) {
            current_destination_images.emplace_back(image);
        }
        for (ID3D11Texture2D* image : input_synthetic_destinations) {
            synthetic_destination_images.emplace_back(image);
        }

        result = create_shared_images(
            source_images.size(),
            &shared_sources,
            &source_d3d12_views,
            D3D11InteropInitializationStage::source_create_d3d12,
            D3D11InteropInitializationStage::source_create_handle,
            D3D11InteropInitializationStage::source_device1,
            D3D11InteropInitializationStage::source_open_d3d11,
            failure_stage);
        if (FAILED(result)) {
            return result;
        }
        result = create_shared_images(
            current_destination_images.size(),
            &shared_current_destinations,
            &current_d3d12_views,
            D3D11InteropInitializationStage::current_create_d3d12,
            D3D11InteropInitializationStage::current_create_handle,
            D3D11InteropInitializationStage::current_device1,
            D3D11InteropInitializationStage::current_open_d3d11,
            failure_stage,&current_destination_images);
        if (FAILED(result)) {
            return result;
        }
        result = create_shared_images(
            synthetic_destination_images.size(),
            &shared_synthetic_destinations,
            &synthetic_d3d12_views,
            D3D11InteropInitializationStage::synthetic_create_d3d12,
            D3D11InteropInitializationStage::synthetic_create_handle,
            D3D11InteropInitializationStage::synthetic_device1,
            D3D11InteropInitializationStage::synthetic_open_d3d11,
            failure_stage,&synthetic_destination_images);
        if (FAILED(result)) {
            return result;
        }

        auto create_timeline = [&](ComPtr<ID3D12Fence>& fence12,
                                   ComPtr<ID3D11Fence>& fence11) noexcept -> HRESULT {
            HRESULT fence_result = d3d12_device->CreateFence(
                0,
                D3D12_FENCE_FLAG_SHARED,
                IID_PPV_ARGS(fence12.GetAddressOf()));
            if (FAILED(fence_result)) {
                if (failure_stage != nullptr) {
                    *failure_stage = D3D11InteropInitializationStage::create_fence;
                }
                return fence_result;
            }
            UniqueHandle fence_handle;
            fence_result = d3d12_device->CreateSharedHandle(
                fence12.Get(),
                nullptr,
                GENERIC_ALL,
                nullptr,
                &fence_handle.value);
            if (FAILED(fence_result)) {
                if (failure_stage != nullptr) {
                    *failure_stage =
                        D3D11InteropInitializationStage::create_fence_handle;
                }
                return fence_result;
            }
            fence_result = d3d11_device5->OpenSharedFence(
                fence_handle.value,
                IID_PPV_ARGS(fence11.GetAddressOf()));
            if (FAILED(fence_result)) {
                if (failure_stage != nullptr) {
                    *failure_stage = D3D11InteropInitializationStage::open_d3d11_fence;
                }
                return fence_result;
            }
            return S_OK;
        };
        result = create_timeline(d3d12_fence, d3d11_fence);
        if (FAILED(result)) return result;
        result = create_timeline(native_fence12, native_fence11);
        if (FAILED(result)) return result;
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            if (failure_stage != nullptr) {
                *failure_stage = D3D11InteropInitializationStage::create_event;
            }
            const DWORD error = GetLastError();
            return HRESULT_FROM_WIN32(
                error == ERROR_SUCCESS ? ERROR_NOT_ENOUGH_MEMORY : error);
        }
        current_d3d11_access.resize(shared_current_destinations.size());
        synthetic_d3d11_access.resize(shared_synthetic_destinations.size());
        enabled = true;
        return S_OK;
    }

    [[nodiscard]] HRESULT prepare_capture(std::uint32_t source_index) noexcept {
        if (!enabled || source_index >= source_images.size() ||
            source_index >= shared_sources.size()) {
            return E_INVALIDARG;
        }
        HRESULT result = S_OK;
        if (last_d3d12_access_value != 0) {
            result = d3d11_context4->Wait(
                native_fence11.Get(), last_d3d12_access_value);
            if (FAILED(result)) {
                enabled = false;
                return result;
            }
        }
        copy_mip_zero(
            d3d11_context.Get(),
            shared_sources[source_index].d3d11.Get(),
            source_images[source_index].Get(),
            source_description.ArraySize,
            source_description.MipLevels,
            1);
        std::uint64_t value = 0;
        result = allocate_fence_value(&value);
        if (FAILED(result)) {
            return result;
        }
        result = d3d11_context4->Signal(d3d11_fence.Get(), value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        last_d3d11_signal_value = value;
        d3d11_context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        result = d3d12_queue->Wait(d3d12_fence.Get(), value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT finish_capture() noexcept {
        if (!enabled) {
            return E_UNEXPECTED;
        }
        std::uint64_t value = 0;
        HRESULT result = allocate_fence_value(&value);
        if (FAILED(result)) {
            return result;
        }
        result = d3d12_queue->Signal(native_fence12.Get(), value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        last_d3d12_access_value = value;
        last_d3d12_signal_value = value;
        return S_OK;
    }

    [[nodiscard]] HRESULT configure_synthesis_queue(ID3D12CommandQueue* queue) noexcept {
        if (!enabled || !queue) return E_INVALIDARG;
        if (synthesis_queue) return queue == synthesis_queue.Get() ? S_OK : E_UNEXPECTED;
        ComPtr<ID3D12Device> owner;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(owner.GetAddressOf()))) ||
            !d3d_owner::same(d3d12_device.Get(), owner.Get()) ||
            queue == d3d12_queue.Get()) return E_INVALIDARG;
        ComPtr<ID3D12Fence> fence12;
        ComPtr<ID3D11Fence> fence11;
        auto result = d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
            IID_PPV_ARGS(fence12.GetAddressOf()));
        UniqueHandle handle;
        if (SUCCEEDED(result)) result = d3d12_device->CreateSharedHandle(
            fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value);
        if (SUCCEEDED(result)) result = d3d11_device5->OpenSharedFence(
            handle.value, IID_PPV_ARGS(fence11.GetAddressOf()));
        if (FAILED(result)) return result;
        synthesis_queue = queue;
        synthesis_fence12 = std::move(fence12);
        synthesis_fence11 = std::move(fence11);
        return S_OK;
    }

    [[nodiscard]] HRESULT prepare_synthesis(bool isolated) noexcept {
        if (!enabled) {
            return E_UNEXPECTED;
        }
        if (isolated && !synthesis_queue) return E_UNEXPECTED;
        if (last_d3d11_access_value == 0) {
            return S_OK;
        }
        const HRESULT result = (isolated ? synthesis_queue : d3d12_queue)->Wait(
            d3d12_fence.Get(), last_d3d11_access_value);
        if (FAILED(result)) {
            enabled = false;
        }
        return result;
    }

    [[nodiscard]] HRESULT publish(
        std::uint32_t current_destination_index,
        std::optional<std::uint32_t> synthetic_destination_index,
        bool source_completed = false,
        std::optional<D3D11PublicationTargets> targets = {},
        bool isolated = false) noexcept {
        if (isolated && !synthesis_queue) return E_UNEXPECTED;
        const auto current_target = targets ? targets->current_index : current_destination_index;
        const auto synthetic_target = targets ? targets->synthetic_index : synthetic_destination_index;
        if (!enabled ||
            current_target >= current_destination_images.size() ||
            current_destination_index >= shared_current_destinations.size() ||
            synthetic_target.has_value() != synthetic_destination_index.has_value() ||
            (synthetic_destination_index &&
             (*synthetic_target >= synthetic_destination_images.size() ||
              *synthetic_destination_index >=
                  shared_synthetic_destinations.size()))) {
            return E_INVALIDARG;
        }
        std::uint64_t ready_value = 0;
        HRESULT result = allocate_fence_value(&ready_value);
        if (FAILED(result)) {
            return result;
        }
        result = source_completed
            ? d3d11_context4->Signal(d3d11_fence.Get(), ready_value)
            : (isolated ? synthesis_queue : d3d12_queue)->Signal(
                isolated ? synthesis_fence12.Get() : native_fence12.Get(), ready_value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        if (source_completed) last_d3d11_signal_value = ready_value;
        else if (isolated) last_synthesis_signal_value = ready_value;
        else last_d3d12_signal_value = ready_value;
        result = source_completed ? S_OK :
            d3d11_context4->Wait(isolated ? synthesis_fence11.Get() : native_fence11.Get(), ready_value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }

        // An owned output may already be the shared D12 backing's D11 view.
        // Keep every timeline transition below; only a true self-copy vanishes.
        if(current_destination_images[current_target]!=shared_current_destinations[current_destination_index].d3d11)
        copy_mip_zero(
            d3d11_context.Get(),
            current_destination_images[current_target].Get(),
            shared_current_destinations[current_destination_index].d3d11.Get(),
            source_description.ArraySize,
            1,
            [&]() noexcept {
                D3D11_TEXTURE2D_DESC description{};
                current_destination_images[current_target]->GetDesc(
                    &description);
                return description.MipLevels;
            }());
        if (synthetic_destination_index) {
            const std::uint32_t index = *synthetic_destination_index;
            if(synthetic_destination_images[*synthetic_target]!=shared_synthetic_destinations[index].d3d11)
            copy_mip_zero(
                d3d11_context.Get(),
                synthetic_destination_images[*synthetic_target].Get(),
                shared_synthetic_destinations[index].d3d11.Get(),
                source_description.ArraySize,
                1,
                [&]() noexcept {
                    D3D11_TEXTURE2D_DESC description{};
                    synthetic_destination_images[*synthetic_target]->GetDesc(&description);
                    return description.MipLevels;
                }());
        }

        std::uint64_t complete_value = 0;
        result = allocate_fence_value(&complete_value);
        if (FAILED(result)) {
            return result;
        }
        result = d3d11_context4->Signal(d3d11_fence.Get(), complete_value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        d3d11_context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        last_d3d11_access_value = complete_value;
        last_d3d11_signal_value = complete_value;
        current_d3d11_access[current_destination_index] = complete_value;
        if (synthetic_destination_index)
            synthetic_d3d11_access[*synthetic_destination_index] = complete_value;
        last_publication = {
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(d3d12_fence.Get())),
            ready_value, complete_value, 0, current_target,
            synthetic_target.value_or(UINT32_MAX)};
        return S_OK;
    }


    [[nodiscard]] HRESULT wait_for_idle() noexcept {
        if (last_d3d11_signal_value == 0 && last_d3d12_signal_value == 0 && last_synthesis_signal_value == 0) {
            return S_OK;
        }
        if (d3d12_fence == nullptr || native_fence12 == nullptr || fence_event == nullptr) {
            return E_UNEXPECTED;
        }
        auto wait_timeline = [&](ID3D12Fence* fence, std::uint64_t target) noexcept -> HRESULT {
            if (target == 0) return S_OK;
            for (;;) {
                const auto completed = fence->GetCompletedValue();
                if (completed == std::numeric_limits<std::uint64_t>::max()) {
                    enabled = false;
                    return E_FAIL;
                }
                if (completed >= target) return S_OK;
                const HRESULT result = fence->SetEventOnCompletion(target, fence_event);
                if (FAILED(result)) {
                    enabled = false;
                    return result;
                }
                if (WaitForSingleObject(fence_event, INFINITE) != WAIT_OBJECT_0) {
                    const DWORD error = GetLastError();
                    enabled = false;
                    return HRESULT_FROM_WIN32(
                        error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
                }
            }
        };
        HRESULT result = wait_timeline(d3d12_fence.Get(), last_d3d11_signal_value);
        if (SUCCEEDED(result)) result = wait_timeline(native_fence12.Get(), last_d3d12_signal_value);
        if (SUCCEEDED(result)) result = wait_timeline(synthesis_fence12.Get(), last_synthesis_signal_value);
        return result;
    }
};

D3D11D3D12SwapchainInterop::D3D11D3D12SwapchainInterop() noexcept = default;

D3D11D3D12SwapchainInterop::~D3D11D3D12SwapchainInterop() {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr && FAILED(impl_->wait_for_idle())) {
            // Preserve resources if completion cannot be proven.
            static_cast<void>(impl_.release());
        }
    } catch (...) {
    }
}

HRESULT D3D11D3D12SwapchainInterop::initialize(
    ID3D11Device* d3d11_device,
    ID3D11DeviceContext* d3d11_context,
    ID3D12Device* d3d12_device,
    ID3D12CommandQueue* d3d12_queue,
    std::span<ID3D11Texture2D* const> source_images,
    std::span<ID3D11Texture2D* const> current_destination_images,
    std::span<ID3D11Texture2D* const> synthetic_destination_images,
    D3D11InteropInitializationStage* failure_stage) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr) {
            return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        }
        auto candidate = std::make_unique<Impl>();
        const HRESULT result = candidate->initialize(
            d3d11_device,
            d3d11_context,
            d3d12_device,
            d3d12_queue,
            source_images,
            current_destination_images,
            synthetic_destination_images,
            failure_stage);
        if (FAILED(result)) {
            return result;
        }
        impl_ = std::move(candidate);
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

std::span<ID3D12Resource* const>
D3D11D3D12SwapchainInterop::source_images() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ == nullptr
        ? std::span<ID3D12Resource* const>{}
        : std::span<ID3D12Resource* const>(impl_->source_d3d12_views);
}

std::span<ID3D12Resource* const>
D3D11D3D12SwapchainInterop::current_destination_images() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ == nullptr
        ? std::span<ID3D12Resource* const>{}
        : std::span<ID3D12Resource* const>(impl_->current_d3d12_views);
}

std::span<ID3D12Resource* const>
D3D11D3D12SwapchainInterop::synthetic_destination_images() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ == nullptr
        ? std::span<ID3D12Resource* const>{}
        : std::span<ID3D12Resource* const>(impl_->synthetic_d3d12_views);
}

HRESULT D3D11D3D12SwapchainInterop::prepare_capture(
    std::uint32_t source_index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? E_UNEXPECTED
                                : impl_->prepare_capture(source_index);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11D3D12SwapchainInterop::finish_capture() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? E_UNEXPECTED : impl_->finish_capture();
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11D3D12SwapchainInterop::configure_synthesis_queue(ID3D12CommandQueue* queue) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->configure_synthesis_queue(queue) : E_UNEXPECTED;
    } catch (...) { return E_FAIL; }
}

HRESULT D3D11D3D12SwapchainInterop::prepare_synthesis(bool isolated) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? E_UNEXPECTED : impl_->prepare_synthesis(isolated);
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11D3D12SwapchainInterop::prepare_synthesis_targets(
    std::uint32_t current_index,
    std::optional<std::uint32_t> synthetic_index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_ || !impl_->enabled || !impl_->synthesis_queue ||
            current_index >= impl_->current_d3d11_access.size() ||
            (synthetic_index && *synthetic_index >= impl_->synthetic_d3d11_access.size()))
            return E_INVALIDARG;
        const auto required = std::max(impl_->current_d3d11_access[current_index],
            synthetic_index ? impl_->synthetic_d3d11_access[*synthetic_index] : 0);
        const auto result = required ? impl_->synthesis_queue->Wait(
            impl_->d3d12_fence.Get(), required) : S_OK;
        if (FAILED(result)) impl_->enabled = false;
        return result;
    } catch (...) { return E_FAIL; }
}

HRESULT D3D11D3D12SwapchainInterop::capture_published_current(
    const D3D11PublicationReceipt& receipt) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_ || !impl_->enabled || !receipt.fence ||
            receipt.fence.Get() != impl_->d3d12_fence.Get() || !receipt.copy_complete_value ||
            receipt.current_destination_index >= impl_->current_destination_images.size())
            return E_INVALIDARG;
        const auto index = receipt.current_destination_index;
        const auto private_index = receipt.private_current_index == UINT32_MAX ? index : receipt.private_current_index;
        if (private_index >= impl_->shared_current_destinations.size()) return E_INVALIDARG;
        HRESULT result = impl_->d3d11_context4->Wait(impl_->d3d11_fence.Get(),
            receipt.copy_complete_value);
        if (FAILED(result)) return result;
        if(impl_->shared_current_destinations[private_index].d3d11!=impl_->current_destination_images[index])
        copy_mip_zero(impl_->d3d11_context.Get(),
            impl_->shared_current_destinations[private_index].d3d11.Get(),
            impl_->current_destination_images[index].Get(),
            impl_->source_description.ArraySize,
            [&]() noexcept { D3D11_TEXTURE2D_DESC d{};
                impl_->current_destination_images[index]->GetDesc(&d); return d.MipLevels; }(),
            [&]() noexcept { D3D11_TEXTURE2D_DESC d{};
                impl_->shared_current_destinations[private_index].d3d11->GetDesc(&d); return d.MipLevels; }());
        std::uint64_t ready{};
        result = impl_->allocate_fence_value(&ready);
        if (FAILED(result)) return result;
        result = impl_->d3d11_context4->Signal(impl_->d3d11_fence.Get(), ready);
        if (FAILED(result)) return result;
        impl_->last_d3d11_signal_value = ready;
        impl_->current_d3d11_access[private_index] = ready;
        impl_->d3d11_context4->Flush1(D3D11_CONTEXT_TYPE_ALL, nullptr);
        impl_->last_d3d11_access_value = ready;
        return impl_->d3d12_queue->Wait(impl_->d3d12_fence.Get(), ready);
    } catch (...) { return E_FAIL; }
}

HRESULT D3D11D3D12SwapchainInterop::diagnostic_capture_published_current(
    const D3D11PublicationReceipt& receipt,
    const TwoFrameImageInfo& info) noexcept {
    if (!two_frame_capture_images_active()) return S_FALSE;
    try {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        Microsoft::WRL::ComPtr<ID3D12Resource> mirror;
        {
            std::scoped_lock lock(mutex_);
            const auto index = receipt.private_current_index == UINT32_MAX ? receipt.current_destination_index : receipt.private_current_index;
            if (!impl_ || index >=
                    impl_->shared_current_destinations.size()) return E_INVALIDARG;
            device = impl_->d3d12_device;
            queue = impl_->d3d12_queue;
            mirror = impl_->shared_current_destinations[index].d3d12;
        }
        // Read the actual acquired D11 image, not the intermediate D12 mirror.
        const HRESULT captured = capture_published_current(receipt);
        if (FAILED(captured)) return captured;
        const HRESULT recorded = two_frame_capture_record_queue(info,
            device.Get(), queue.Get(), mirror.Get(), D3D12_RESOURCE_STATE_COMMON);
        const HRESULT retired = finish_capture();
        return FAILED(recorded) ? recorded : retired;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11D3D12SwapchainInterop::publish(
    std::uint32_t current_destination_index,
    std::optional<std::uint32_t> synthetic_destination_index,
    D3D11PublicationReceipt* receipt,
    std::optional<D3D11PublicationTargets> targets, bool isolated) noexcept {
    if (receipt) *receipt = {};
    try {
        std::scoped_lock lock(mutex_);
        if (!impl_) return E_UNEXPECTED;
        const HRESULT result = impl_->publish(
            current_destination_index, synthetic_destination_index, false, targets, isolated);
        if (SUCCEEDED(result) && receipt) {
            receipt->fence = impl_->d3d12_fence;
            receipt->copy_complete_value = impl_->last_publication.copy_complete_value;
            receipt->current_destination_index = impl_->last_publication.current_index;
            receipt->private_current_index = current_destination_index;
            receipt->synthetic_destination_index = impl_->last_publication.synthetic_index;
        }
        return result;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT D3D11D3D12SwapchainInterop::publish_completed(
    std::uint32_t current_destination_index,
    std::optional<std::uint32_t> synthetic_destination_index,
    ID3D12Fence* producer_fence, std::uint64_t producer_value,
    D3D11PublicationReceipt* receipt,
    std::optional<D3D11PublicationTargets> targets) noexcept {
    if(receipt) *receipt={};
    if(!producer_fence || !producer_value) return E_INVALIDARG;
    const auto completed=producer_fence->GetCompletedValue();
    if(completed==UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if(completed<producer_value) return HRESULT_FROM_WIN32(ERROR_BUSY);
    try {
        std::scoped_lock lock(mutex_);
        if(!impl_) return E_UNEXPECTED;
        const auto result=impl_->publish(current_destination_index,synthetic_destination_index,true,targets);
        if(SUCCEEDED(result) && receipt) {
            receipt->fence=impl_->d3d12_fence;
            receipt->copy_complete_value=impl_->last_publication.copy_complete_value;
            receipt->current_destination_index=impl_->last_publication.current_index;
            receipt->private_current_index=current_destination_index;
            receipt->synthetic_destination_index=impl_->last_publication.synthetic_index;
        }
        return result;
    } catch(...) { return E_FAIL; }
}

HRESULT wait_for_d3d11_publication(const D3D11PublicationReceipt& receipt) noexcept {
    if (!receipt.fence || receipt.copy_complete_value == 0) return E_INVALIDARG;
    UniqueHandle completion;
    for (;;) {
        const auto completed = receipt.fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) return E_FAIL;
        if (completed >= receipt.copy_complete_value) return S_OK;
        if (!completion.value) {
            completion.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!completion.value) return HRESULT_FROM_WIN32(GetLastError());
        }
        const HRESULT result = receipt.fence->SetEventOnCompletion(
            receipt.copy_complete_value, completion.value);
        if (FAILED(result)) return result;
        if (WaitForSingleObject(completion.value, INFINITE) != WAIT_OBJECT_0) {
            const auto error = GetLastError();
            return HRESULT_FROM_WIN32(error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
        }
    }
}


HRESULT D3D11D3D12SwapchainInterop::wait_for_idle() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ == nullptr ? S_OK : impl_->wait_for_idle();
    } catch (...) {
        return E_FAIL;
    }
}

D3D11PublicationSnapshot D3D11D3D12SwapchainInterop::publication_snapshot() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ == nullptr || impl_->d3d12_fence == nullptr) return {};
        auto snapshot = impl_->last_publication;
        snapshot.completed_value = impl_->d3d12_fence->GetCompletedValue();
        return snapshot;
    } catch (...) {
        return {};
    }
}

bool D3D11D3D12SwapchainInterop::initialized() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ != nullptr && impl_->enabled;
    } catch (...) {
        return false;
    }
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11D3D12SwapchainInterop::published_current_image(std::uint32_t index) const noexcept {
    return published_image(index,false);
}
Microsoft::WRL::ComPtr<ID3D11Texture2D> D3D11D3D12SwapchainInterop::published_image(std::uint32_t index,bool synthetic) const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        if(!impl_)return {};
        const auto& images=synthetic?impl_->synthetic_destination_images:impl_->current_destination_images;
        if(index>=images.size())return {};
        return images[index];
    }catch(...){return {};}
}

}  // namespace xrfg
