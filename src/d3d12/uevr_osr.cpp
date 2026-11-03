#include "xrfg/uevr_osr.hpp"

#include "osr_full_warp_pass.hpp"
#include "uevr_osr_input_shader.hpp"
#include "uevr_osr_guide_shader.hpp"
#include "xrfg/luke_depth_format.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/osr_full_warp.hpp"

#include <algorithm>
#include <cstring>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;

DXGI_FORMAT linear_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
    }
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

struct BarrierList {
    std::array<D3D12_RESOURCE_BARRIER, 8> items{};
    UINT count{};
    void add(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
        if (resource && before != after && count < items.size()) items[count++] = transition(resource, before, after);
    }
    void flush(ID3D12GraphicsCommandList* list) noexcept {
        if (count) list->ResourceBarrier(count, items.data());
        count = 0;
    }
};

HRESULT create_texture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
    bool uav, ComPtr<ID3D12Resource>& out) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    out.Reset();
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(out.GetAddressOf()));
}

void copy_whole(ID3D12GraphicsCommandList* list, ID3D12Resource* destination, ID3D12Resource* source,
    UINT width, UINT height) noexcept {
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = destination; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box{0, 0, 0, width, height, 1};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
}

void store(float* destination, const UevrMatrix& m) noexcept { std::memcpy(destination, m.data(), sizeof(float) * 16); }
UevrMatrix load(const std::array<float, 16>& m) noexcept { return m; }
}  // namespace

struct UevrOsrRenderer::Impl {
    static constexpr UINT kSlots = 3;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<ComPtr<ID3D12CommandAllocator>, kSlots> allocators;
    std::array<std::uint64_t, kSlots> slot_fence{};
    UINT next_slot{};
    // GPU timestamps: kMarks per slot, resolved into a persistently mapped
    // readback buffer and read when the slot's allocator is reused.
    static constexpr UINT kMarks = 6;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timestamp_readback;
    const std::uint64_t* timestamp_values{};
    std::uint64_t timestamp_frequency{};
    std::array<int, kSlots> slot_kind{}; // 0 none, 1 capture, 2 render
    std::array<std::uint32_t, 6> gpu_us{};
    UINT mark_base{};
    void mark(UINT index) noexcept {
        if (timestamps) list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, mark_base + index);
    }
    void resolve_marks() noexcept {
        if (timestamps) list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, mark_base, kMarks,
            timestamp_readback.Get(), static_cast<UINT64>(mark_base) * sizeof(std::uint64_t));
    }
    void harvest(UINT slot) noexcept {
        if (!timestamp_values || !timestamp_frequency || !slot_kind[slot]) return;
        const auto* v = timestamp_values + static_cast<std::size_t>(slot) * kMarks;
        const auto us = [&](UINT a, UINT b) -> std::uint32_t {
            if (v[b] < v[a]) return 0;
            return static_cast<std::uint32_t>((v[b] - v[a]) * 1'000'000ULL / timestamp_frequency);
        };
        if (slot_kind[slot] == 1) gpu_us[0] = us(0, 1);
        else for (UINT i = 1; i < 6; ++i) gpu_us[i] = us(i - 1, i);
        slot_kind[slot] = 0;
    }
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    std::uint64_t fence_value{};
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT increment{};

    std::array<ComPtr<ID3D12Resource>, 2> source_color{};
    std::array<bool, 2> source_valid{};
    DXGI_FORMAT color_format{DXGI_FORMAT_UNKNOWN};
    UINT color_width{}, color_height{};
    bool typed_store_checked{};
    bool typed_store{};

    ComPtr<ID3D12Resource> depth, motion;
    UINT grid_width{}, grid_height{};

    OsrFullWarp osr;

    // Frame generation guides (uevr_osr_guide.hlsl), one stream per eye. A
    // ring of textures per eye: a published guide is read by frame generation
    // a frame or two later and is not written again before kGuideSlots frames.
    static constexpr UINT kGuideSlots = 6;
    struct GuideConstants {
        float shown_to_previous[16];
        float shown_to_source[16];
        float source_to_history[16];
        std::uint32_t size[2];
        float motion_scale[2];
        float object_weight;
        std::uint32_t flags;
    };
    static_assert(sizeof(GuideConstants) == 54 * sizeof(std::uint32_t));
    ComPtr<ID3D12RootSignature> guide_root;
    ComPtr<ID3D12PipelineState> guide_pipeline;
    ComPtr<ID3D12DescriptorHeap> guide_heap;
    bool guide_failed{};
    std::array<std::array<ComPtr<ID3D12Resource>, kGuideSlots>, 2> guide_motion{}, guide_depth{};
    UINT guide_width{}, guide_height{};
    std::array<UINT, 2> guide_slot{};
    std::array<std::uint64_t, 2> guide_serial{};
    std::uint64_t guide_epoch{1}, guide_publication{};
    float guide_near{};
    // Each eye's camera as last shown (the guide's previous frame) and the
    // real eye's render camera as last rendered (where its vectors point).
    std::array<bool, 2> shown_valid{}, render_valid{};
    std::array<UevrMatrix, 2> shown_view_to_world{}, shown_view_to_clip{};
    std::array<UevrMatrix, 2> render_view_to_world{}, render_view_to_clip{};

    HRESULT ensure_guides(UINT width, UINT height) noexcept {
        if (guide_failed) return E_FAIL;
        HRESULT hr = S_OK;
        if (!guide_pipeline) {
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 3;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 2;
            ranges[1].OffsetInDescriptorsFromTableStart = 3;
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[0].DescriptorTable.NumDescriptorRanges = 2;
            parameters[0].DescriptorTable.pDescriptorRanges = ranges;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[1].Constants.Num32BitValues = sizeof(GuideConstants) / sizeof(std::uint32_t);
            D3D12_ROOT_SIGNATURE_DESC signature{};
            signature.NumParameters = 2;
            signature.pParameters = parameters;
            ComPtr<ID3DBlob> blob, error;
            hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
            if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(guide_root.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = guide_root.Get();
                desc.CS = {g_xrfg_uevr_osr_guide_shader, sizeof(g_xrfg_uevr_osr_guide_shader)};
                hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(guide_pipeline.GetAddressOf()));
            }
            if (SUCCEEDED(hr)) {
                D3D12_DESCRIPTOR_HEAP_DESC desc{};
                desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
                desc.NumDescriptors = 10; // two eyes, three views read and two written each
                desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
                hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(guide_heap.GetAddressOf()));
            }
            if (FAILED(hr)) { guide_failed = true; guide_pipeline.Reset(); return hr; }
        }
        if (guide_width != width || guide_height != height) {
            for (UINT eye = 0; eye < 2 && SUCCEEDED(hr); ++eye)
                for (UINT slot = 0; slot < kGuideSlots && SUCCEEDED(hr); ++slot) {
                    hr = create_texture(device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, true, guide_motion[eye][slot]);
                    if (SUCCEEDED(hr)) hr = create_texture(device.Get(), width, height, DXGI_FORMAT_R32_FLOAT, true, guide_depth[eye][slot]);
                }
            if (FAILED(hr)) { guide_width = guide_height = 0; return hr; }
            guide_width = width; guide_height = height;
            shown_valid = {}; render_valid = {};
            ++guide_epoch;
        }
        return S_OK;
    }

    // Same-eye relocation: the source eye warped to its slot pose. Its own
    // instance, history unused: the output eye index is the source eye, so
    // the source is passed as the other eye and no history is recorded.
    OsrFullWarp osr_same;
    std::array<std::uint64_t, 2> previous_publication{};
    std::array<bool, 2> history_camera_valid{};
    std::array<UevrMatrix, 2> history_engine{}; // the game camera of each eye's history frame
    std::array<bool, 2> history_engine_valid{};
    // Debug: a solid square stamped into every reconstructed eye, so the
    // headset shows at a glance which images OSR produced.
    static constexpr UINT kMarkerSize = 192;
    ComPtr<ID3D12Resource> marker;
    ComPtr<ID3D12DescriptorHeap> marker_rtv;
    DXGI_FORMAT marker_format{DXGI_FORMAT_UNKNOWN};

    ~Impl() { if (event) CloseHandle(event); }

    HRESULT ensure_marker() noexcept {
        if (marker && marker_format == color_format) return S_OK;
        marker.Reset();
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = kMarkerSize; desc.Height = kMarkerSize;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = color_format; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(marker.GetAddressOf()));
        if (SUCCEEDED(hr) && !marker_rtv) {
            D3D12_DESCRIPTOR_HEAP_DESC rtv{};
            rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtv.NumDescriptors = 1;
            hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(marker_rtv.GetAddressOf()));
        }
        if (FAILED(hr)) { marker.Reset(); return hr; }
        device->CreateRenderTargetView(marker.Get(), nullptr, marker_rtv->GetCPUDescriptorHandleForHeapStart());
        marker_format = color_format;
        return S_OK;
    }

    // Once per Impl; the renderer replaces the Impl when the device or queue changes.
    HRESULT initialize(ID3D12Device* d, ID3D12CommandQueue* q) noexcept {
        if (list) return S_OK;
        device = d; queue = q;
        HRESULT hr = S_OK;
        for (auto& allocator : allocators)
            if (SUCCEEDED(hr)) hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr,
            IID_PPV_ARGS(list.GetAddressOf()));
        if (SUCCEEDED(hr)) hr = list->Close();
        if (SUCCEEDED(hr)) hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) hr = HRESULT_FROM_WIN32(GetLastError());
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_RANGE ranges[2]{};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 2;
            ranges[1].OffsetInDescriptorsFromTableStart = 2;
            D3D12_ROOT_PARAMETER parameters[2]{};
            parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[0].DescriptorTable.NumDescriptorRanges = 2;
            parameters[0].DescriptorTable.pDescriptorRanges = ranges;
            parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[1].Constants.Num32BitValues = 8;
            D3D12_ROOT_SIGNATURE_DESC signature{};
            signature.NumParameters = 2;
            signature.pParameters = parameters;
            ComPtr<ID3DBlob> blob, error;
            hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
            if (SUCCEEDED(hr)) hr = d->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(root.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root.Get();
            desc.CS = {g_xrfg_uevr_osr_input_shader, sizeof(g_xrfg_uevr_osr_input_shader)};
            hr = d->CreateComputePipelineState(&desc, IID_PPV_ARGS(pipeline.GetAddressOf()));
        }
        if (SUCCEEDED(hr)) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = 4;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = d->CreateDescriptorHeap(&desc, IID_PPV_ARGS(heap.GetAddressOf()));
            increment = d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if (SUCCEEDED(hr) && SUCCEEDED(q->GetTimestampFrequency(&timestamp_frequency)) && timestamp_frequency) {
            // Timing is diagnostic: without it the renderer works the same.
            D3D12_QUERY_HEAP_DESC heap{};
            heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            heap.Count = kSlots * kMarks;
            D3D12_HEAP_PROPERTIES readback{};
            readback.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = sizeof(std::uint64_t) * kSlots * kMarks;
            buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            void* mapped{};
            if (FAILED(d->CreateQueryHeap(&heap, IID_PPV_ARGS(timestamps.GetAddressOf()))) ||
                FAILED(d->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(timestamp_readback.GetAddressOf()))) ||
                FAILED(timestamp_readback->Map(0, nullptr, &mapped))) {
                timestamps.Reset(); timestamp_readback.Reset();
            } else {
                timestamp_values = static_cast<const std::uint64_t*>(mapped);
            }
        }
        if (FAILED(hr)) { list.Reset(); device.Reset(); queue.Reset(); }
        return hr;
    }

    // Opens the list on a slot whose previous use has retired.
    HRESULT begin() noexcept {
        const UINT slot = next_slot;
        next_slot = (next_slot + 1) % kSlots;
        if (slot_fence[slot] && fence->GetCompletedValue() < slot_fence[slot]) {
            HRESULT hr = fence->SetEventOnCompletion(slot_fence[slot], event);
            if (FAILED(hr)) return hr;
            if (WaitForSingleObject(event, 250) != WAIT_OBJECT_0) return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        harvest(slot); // its previous use has completed (fence above)
        HRESULT hr = allocators[slot]->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocators[slot].Get(), nullptr);
        current_slot = slot;
        mark_base = slot * kMarks;
        return hr;
    }
    HRESULT submit() noexcept {
        HRESULT hr = list->Close();
        if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        ++fence_value;
        hr = queue->Signal(fence.Get(), fence_value);
        slot_fence[current_slot] = fence_value;
        return hr;
    }
    UINT current_slot{};

    HRESULT ensure_color(const D3D12_RESOURCE_DESC& desc) noexcept {
        const auto format = linear_format(desc.Format);
        if (source_color[0] && color_format == format && color_width == desc.Width && color_height == desc.Height)
            return S_OK;
        if (!typed_store_checked || color_format != format) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
            support.Format = format;
            typed_store = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
            typed_store_checked = true;
        }
        if (!typed_store || desc.Width > 16384 || desc.Height > 16384) return DXGI_ERROR_UNSUPPORTED;
        source_valid = {};
        HRESULT hr = S_OK;
        for (auto& texture : source_color)
            if (SUCCEEDED(hr)) hr = create_texture(device.Get(), static_cast<UINT>(desc.Width), desc.Height, format, false, texture);
        if (FAILED(hr)) { source_color = {}; return hr; }
        color_format = format;
        color_width = static_cast<UINT>(desc.Width);
        color_height = desc.Height;
        return S_OK;
    }
};

UevrOsrRenderer::UevrOsrRenderer() noexcept = default;
UevrOsrRenderer::~UevrOsrRenderer() {
    if (impl_ && impl_->fence && impl_->fence_value && impl_->fence->GetCompletedValue() < impl_->fence_value &&
        SUCCEEDED(impl_->fence->SetEventOnCompletion(impl_->fence_value, impl_->event)))
        WaitForSingleObject(impl_->event, 1000);
}

ID3D12Resource* UevrOsrRenderer::source_image(unsigned eye) const noexcept {
    return impl_ && eye < 2 && impl_->source_valid[eye] ? impl_->source_color[eye].Get() : nullptr;
}

ID3D12Resource* UevrOsrRenderer::output() const noexcept {
    return impl_ ? impl_->osr.output() : nullptr;
}

ID3D12Resource* UevrOsrRenderer::same_output() const noexcept {
    return impl_ ? impl_->osr_same.output() : nullptr;
}

void UevrOsrRenderer::reset_history() noexcept {
    if (!impl_) return;
    impl_->previous_publication = {};
    impl_->history_camera_valid = {};
}

HRESULT UevrOsrRenderer::capture_source(ID3D12Device* device, ID3D12CommandQueue* queue,
    ID3D12Resource* image, D3D12_RESOURCE_STATES state, unsigned eye) noexcept {
    try {
        if (!device || !queue || !image || eye > 1) return E_INVALIDARG;
        if (impl_ && (impl_->device.Get() != device || impl_->queue.Get() != queue)) {
            if (impl_->fence_value && impl_->fence->GetCompletedValue() < impl_->fence_value &&
                SUCCEEDED(impl_->fence->SetEventOnCompletion(impl_->fence_value, impl_->event)))
                WaitForSingleObject(impl_->event, 1000);
            impl_.reset();
        }
        if (!impl_) impl_ = std::make_unique<Impl>();
        auto& s = *impl_;
        HRESULT hr = s.initialize(device, queue);
        if (FAILED(hr)) return hr;
        const auto desc = image->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 ||
            desc.SampleDesc.Count != 1) return DXGI_ERROR_UNSUPPORTED;
        hr = s.ensure_color(desc);
        if (FAILED(hr)) return hr;
        s.source_valid[eye] = false;
        hr = s.begin();
        if (FAILED(hr)) return hr;
        s.mark(0);
        BarrierList barriers;
        barriers.add(image, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.add(s.source_color[eye].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.flush(s.list.Get());
        copy_whole(s.list.Get(), s.source_color[eye].Get(), image, s.color_width, s.color_height);
        barriers.add(image, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
        barriers.add(s.source_color[eye].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        barriers.flush(s.list.Get());
        s.mark(1);
        s.resolve_marks();
        s.slot_kind[s.current_slot] = 1;
        hr = s.submit();
        if (SUCCEEDED(hr)) s.source_valid[eye] = true;
        return hr;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

UevrOsrResult UevrOsrRenderer::render(ID3D12Device* device, ID3D12CommandQueue* queue,
    const UevrOsrFrame& frame, ID3D12Resource* destination, D3D12_RESOURCE_STATES state,
    ID3D12Resource* same_destination, D3D12_RESOURCE_STATES same_state) noexcept {
    UevrOsrResult out{};
    const auto refuse = [&](std::uint32_t reason, HRESULT hr = DXGI_ERROR_UNSUPPORTED) {
        out.reason = reason; out.result = hr; return out;
    };
    try {
        if (!impl_ || !frame.guide || !destination || frame.source_eye > 1) return refuse(1, E_INVALIDARG);
        auto& s = *impl_;
        if (s.device.Get() != device || s.queue.Get() != queue) return refuse(2, E_INVALIDARG);
        const unsigned source_eye = frame.source_eye, peer = 1 - source_eye;
        if (!s.source_valid[source_eye]) return refuse(3, E_NOT_VALID_STATE);
        const auto& guide = *frame.guide;
        if (!guide.depth || !guide.motion_vectors) return refuse(4);
        if (!guide.depth_inverted) return refuse(5);
        // Vectors at the depth's resolution, or at the output's (DLSS without
        // MVLowRes: Hogwarts Legacy 4197x3287 over depth 2435x1907); either way
        // over the same field.
        if (!guide.depth_width || !guide.depth_height || !guide.motion_width || !guide.motion_height ||
            std::abs((double(guide.depth_width) / guide.depth_height) /
                (double(guide.motion_width) / guide.motion_height) - 1.0) > 0.02) return refuse(6);
        const auto depth_desc = guide.depth->GetDesc();
        const auto motion_desc = guide.motion_vectors->GetDesc();
        const auto depth_srv = luke_depth_format(depth_desc.Format).srv;
        const auto motion_srv = luke_motion_srv_format(motion_desc.Format);
        if (depth_srv == DXGI_FORMAT_UNKNOWN || motion_srv == DXGI_FORMAT_UNKNOWN) return refuse(7);
        if (guide.depth_x + guide.depth_width > depth_desc.Width || guide.depth_y + guide.depth_height > depth_desc.Height ||
            guide.motion_x + guide.motion_width > motion_desc.Width || guide.motion_y + guide.motion_height > motion_desc.Height)
            return refuse(8);
        const auto destination_desc = destination->GetDesc();
        if (destination_desc.Width != s.color_width || destination_desc.Height != s.color_height ||
            linear_format(destination_desc.Format) != s.color_format) return refuse(9);
        // The depth grid must cover the eye image's field: same aspect.
        const double grid_aspect = double(guide.depth_width) / guide.depth_height;
        const double color_aspect = double(s.color_width) / s.color_height;
        if (std::abs(grid_aspect / color_aspect - 1.0) > 0.02) return refuse(10);

        if (!s.depth || s.grid_width != guide.depth_width || s.grid_height != guide.depth_height) {
            HRESULT hr = create_texture(s.device.Get(), guide.depth_width, guide.depth_height, DXGI_FORMAT_R32_FLOAT, true, s.depth);
            if (SUCCEEDED(hr)) hr = create_texture(s.device.Get(), guide.depth_width, guide.depth_height, DXGI_FORMAT_R16G16_FLOAT, true, s.motion);
            if (FAILED(hr)) { s.depth.Reset(); s.motion.Reset(); return refuse(11, hr); }
            s.grid_width = guide.depth_width; s.grid_height = guide.depth_height;
            s.previous_publication = {};
        }
        HRESULT hr = s.osr.prepare(s.device.Get(), s.grid_width, s.grid_height, s.color_width, s.color_height, s.color_format);
        if (FAILED(hr)) return refuse(12, hr);
        const bool relocate = frame.relocate_same && same_destination != nullptr;
        if (relocate) {
            const auto same_desc = same_destination->GetDesc();
            if (same_desc.Width != s.color_width || same_desc.Height != s.color_height ||
                linear_format(same_desc.Format) != s.color_format) return refuse(18);
            hr = s.osr_same.prepare(s.device.Get(), s.grid_width, s.grid_height, s.color_width, s.color_height, s.color_format);
            if (FAILED(hr)) return refuse(19, hr);
        }

        if (frame.reset) { s.previous_publication = {}; s.history_camera_valid = {}; }
        const auto& history = s.osr.history(peer);
        const int effective_mode = frame.mode == 1 ? 1 : frame.mode == 2 ? 2 : 3;
        const bool history_usable = frame.cameras_valid && s.history_camera_valid[peer] && !frame.reset &&
            osr_full_history_usable(effective_mode, guide.publication, history.publication, s.previous_publication[peer]);

        UevrMatrix source_clip_to_view{}, destination_clip_to_view{};
        if (!uevr_invert(frame.source_view_to_clip, source_clip_to_view) ||
            !uevr_invert(frame.destination_view_to_clip, destination_clip_to_view)) return refuse(13, E_INVALIDARG);
        const auto source_world_to_view = uevr_rigid_inverse(frame.source_view_to_world);
        const auto destination_world_to_view = uevr_rigid_inverse(frame.destination_view_to_world);

        OsrFullWarp::Inputs in{};
        in.source_depth = s.depth.Get();
        in.source_depth_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        in.source_color = s.source_color[source_eye].Get();
        in.source_color_state = D3D12_RESOURCE_STATE_COMMON;
        in.source_motion = s.motion.Get();
        in.source_motion_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        in.source_eye = source_eye;
        in.use_history = history_usable;
        in.reset_history = frame.reset;
        in.use_history_motion = history_usable && frame.history_motion && history.has_motion &&
            history.previous_publication != 0 && history.previous_publication + 2 == history.publication;
        in.history_motion_scale[0] = history.motion_scale[0];
        in.history_motion_scale[1] = history.motion_scale[1];
        // UEVR's eye fills its whole DLSS rectangle: no window residual, only
        // this frame's sub-texel jitter (xrfg/osr_full_warp.hpp).
        in.source_dejitter[0] = -guide.jitter_x;
        in.source_dejitter[1] = -guide.jitter_y;
        in.history_dejitter[0] = history.dejitter[0];
        in.history_dejitter[1] = history.dejitter[1];
        in.locked_depth = osr_full_depth_at(frame.destination_view_to_clip.data(),
            kOsrFullLockedDistanceMetres * frame.units_per_metre);
        in.debug = frame.debug;
        in.lower_quality = frame.lower_quality;
        // The weapon hangs on the game camera, not the view: the warp, of this
        // very instant, keeps it (kOsrFullFlagWarpKeepsAttached).
        in.warp_keeps_attached = true;
        store(in.forward_stereo, uevr_multiply(uevr_multiply(uevr_multiply(source_clip_to_view, frame.source_view_to_world),
            destination_world_to_view), frame.destination_view_to_clip));
        store(in.backward_source, uevr_multiply(uevr_multiply(uevr_multiply(destination_clip_to_view, frame.destination_view_to_world),
            source_world_to_view), frame.source_view_to_clip));
        if (history_usable) {
            store(in.forward_history, uevr_multiply(uevr_multiply(uevr_multiply(load(history.camera.clip_to_view),
                load(history.camera.view_to_world)), destination_world_to_view), frame.destination_view_to_clip));
            store(in.backward_history, uevr_multiply(uevr_multiply(uevr_multiply(destination_clip_to_view,
                frame.destination_view_to_world), load(history.camera.world_to_view)), load(history.camera.view_to_clip)));
            // The weapon in this eye's history frame hangs on that frame's
            // game camera: held to it, it lands where this frame's game
            // camera puts it. Pinned to its screen place instead (Luke Ross'
            // weapons are attached to the view), it stayed where the head had
            // been and ghosted on head turns, growing with their speed.
            if (frame.engine_valid && s.history_engine_valid[peer]) {
                in.locked_follows = true;
                store(in.forward_history_locked, uevr_multiply(uevr_multiply(uevr_multiply(uevr_multiply(uevr_multiply(
                    load(history.camera.clip_to_view), load(history.camera.view_to_world)),
                    uevr_rigid_inverse(s.history_engine[peer])), frame.destination_engine_to_world),
                    destination_world_to_view), frame.destination_view_to_clip));
            }
            if (in.use_history_motion)
                store(in.forward_history_previous, uevr_multiply(uevr_multiply(uevr_multiply(load(history.camera.clip_to_view),
                    load(history.camera.view_to_world)), load(history.camera_previous.world_to_view)),
                    load(history.camera_previous.view_to_clip)));
        }

        hr = s.begin();
        if (FAILED(hr)) return refuse(14, hr);
        if (guide.producer_fence && guide.producer_fence_value &&
            guide.producer_fence->GetCompletedValue() < guide.producer_fence_value) {
            hr = queue->Wait(guide.producer_fence.Get(), guide.producer_fence_value);
            if (FAILED(hr)) { s.list->Close(); return refuse(15, hr); }
        }
        auto* list = s.list.Get();
        s.mark(0);
        // 1. The guide's depth and vectors into OSR's grids.
        auto cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = depth_srv;
        s.device->CreateShaderResourceView(guide.depth.Get(), &srv, cpu);
        cpu.ptr += s.increment;
        srv.Format = motion_srv;
        s.device->CreateShaderResourceView(guide.motion_vectors.Get(), &srv, cpu);
        cpu.ptr += s.increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        s.device->CreateUnorderedAccessView(s.depth.Get(), nullptr, &uav, cpu);
        cpu.ptr += s.increment;
        uav.Format = DXGI_FORMAT_R16G16_FLOAT;
        s.device->CreateUnorderedAccessView(s.motion.Get(), nullptr, &uav, cpu);

        const bool same_guide_resource = guide.depth.Get() == guide.motion_vectors.Get();
        BarrierList barriers;
        barriers.add(guide.depth.Get(), guide.depth_resource_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!same_guide_resource)
            barriers.add(guide.motion_vectors.Get(), guide.resource_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.add(s.depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.add(s.motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.flush(list);
        ID3D12DescriptorHeap* heaps[]{s.heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(s.root.Get());
        list->SetComputeRootDescriptorTable(0, s.heap->GetGPUDescriptorHandleForHeapStart());
        const std::uint32_t constants[8]{guide.depth_x, guide.depth_y, guide.motion_x, guide.motion_y,
            guide.depth_width, guide.depth_height, guide.motion_width, guide.motion_height};
        list->SetComputeRoot32BitConstants(1, 8, constants, 0);
        list->SetPipelineState(s.pipeline.Get());
        list->Dispatch((guide.depth_width + 7) / 8, (guide.depth_height + 7) / 8, 1);
        barriers.add(guide.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, guide.depth_resource_state);
        if (!same_guide_resource)
            barriers.add(guide.motion_vectors.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, guide.resource_state);
        barriers.add(s.depth.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.add(s.motion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.flush(list);

        s.mark(1);
        // 2. Full OSR.
        s.osr.record(s.device.Get(), list, in);
        s.mark(2);
        out.lower_quality = s.osr.lower_quality_active();

        // 3. The reconstructed eye into the destination swapchain image.
        barriers.add(destination, state, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.add(s.osr.output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.flush(list);
        copy_whole(list, destination, s.osr.output(), s.color_width, s.color_height);
        if (frame.debug && s.color_width > Impl::kMarkerSize * 2 && s.color_height > Impl::kMarkerSize * 4 &&
            SUCCEEDED(s.ensure_marker())) {
            // A magenta square a quarter down the image, centred: inside both
            // eyes' submitted rectangles.
            barriers.add(s.marker.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
            barriers.flush(list);
            const float magenta[4]{1.0F, 0.0F, 1.0F, 1.0F};
            list->ClearRenderTargetView(s.marker_rtv->GetCPUDescriptorHandleForHeapStart(), magenta, 0, nullptr);
            barriers.add(s.marker.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers.flush(list);
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = destination; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = s.marker.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, Impl::kMarkerSize, Impl::kMarkerSize, 1};
            list->CopyTextureRegion(&dst, (s.color_width - Impl::kMarkerSize) / 2, s.color_height / 4, 0, &src, &box);
            barriers.add(s.marker.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        barriers.add(destination, D3D12_RESOURCE_STATE_COPY_DEST, state);
        barriers.add(s.osr.output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barriers.flush(list);

        s.mark(3);
        // 3b. The source eye itself, relocated to the slot pose, into UEVR's
        // own image. Pure warp of this frame: no history, the same projection.
        if (relocate) {
            OsrFullWarp::Inputs same = in;
            same.source_eye = 1 - source_eye; // the instance's output index is this eye
            same.use_history = false;
            same.use_history_motion = false;
            same.reset_history = true;
            const auto same_world_to_view = uevr_rigid_inverse(frame.same_view_to_world);
            store(same.forward_stereo, uevr_multiply(uevr_multiply(uevr_multiply(source_clip_to_view, frame.source_view_to_world),
                same_world_to_view), frame.source_view_to_clip));
            store(same.backward_source, uevr_multiply(uevr_multiply(uevr_multiply(source_clip_to_view, frame.same_view_to_world),
                source_world_to_view), frame.source_view_to_clip));
            same.locked_depth = osr_full_depth_at(frame.source_view_to_clip.data(),
                kOsrFullLockedDistanceMetres * frame.units_per_metre);
            s.osr_same.record(s.device.Get(), list, same);
            barriers.add(same_destination, same_state, D3D12_RESOURCE_STATE_COPY_DEST);
            barriers.add(s.osr_same.output(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barriers.flush(list);
            copy_whole(list, same_destination, s.osr_same.output(), s.color_width, s.color_height);
            barriers.add(same_destination, D3D12_RESOURCE_STATE_COPY_DEST, same_state);
            barriers.add(s.osr_same.output(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            barriers.flush(list);
            out.same_rendered = true;
        }

        s.mark(4);
        // 4. This frame becomes the source eye's history.
        OsrFullHistory kept{};
        kept.publication = guide.publication;
        kept.stream = guide.stream;
        kept.epoch = guide.epoch;
        // Vector texels -> depth texels (1 unless the vectors are full resolution).
        kept.motion_scale[0] = guide.scale_x * float(guide.depth_width) / float(guide.motion_width);
        kept.motion_scale[1] = guide.scale_y * float(guide.depth_height) / float(guide.motion_height);
        kept.dejitter[0] = in.source_dejitter[0];
        kept.dejitter[1] = in.source_dejitter[1];
        std::copy(frame.source_view_to_world.begin(), frame.source_view_to_world.end(), kept.camera.view_to_world.begin());
        std::copy(source_world_to_view.begin(), source_world_to_view.end(), kept.camera.world_to_view.begin());
        std::copy(frame.source_view_to_clip.begin(), frame.source_view_to_clip.end(), kept.camera.view_to_clip.begin());
        std::copy(source_clip_to_view.begin(), source_clip_to_view.end(), kept.camera.clip_to_view.begin());
        s.osr.record_history_update(list, in, kept);
        // Geometry receipts (xrfg/uevr_osr.hpp): the destination eye's
        // offset from the source along the source's right axis, and its
        // distance from the camera the history frame was rendered with.
        {
            const auto& sv = frame.source_view_to_world;
            const auto& dv = frame.destination_view_to_world;
            out.baseline_right = (dv[12] - sv[12]) * sv[0] + (dv[13] - sv[13]) * sv[1] + (dv[14] - sv[14]) * sv[2];
            if (history_usable) {
                const auto& hv = history.camera.view_to_world;
                out.history_distance = std::sqrt((dv[12] - hv[12]) * (dv[12] - hv[12]) +
                    (dv[13] - hv[13]) * (dv[13] - hv[13]) + (dv[14] - hv[14]) * (dv[14] - hv[14]));
            }
        }
        // 5. Frame generation guides for both eyes as shown: the destination
        // from this OSR's splat, the source from its relocation's.
        std::array<UINT, 2> guide_slots{};
        bool guides_recorded = false;
        if (relocate && frame.cameras_valid && SUCCEEDED(s.ensure_guides(s.grid_width, s.grid_height))) {
            if (frame.reset) { s.shown_valid = {}; s.render_valid = {}; ++s.guide_epoch; }
            if (s.guide_near <= 0) s.guide_near = guide.camera_near > 0 ? guide.camera_near : 10.0F;
            const auto same_world_to_view = uevr_rigid_inverse(frame.same_view_to_world);
            ID3D12Resource* const shown_depth[2]{s.osr_same.stereo_depth(), s.osr.stereo_depth()}; // [0] source, [1] destination
            const D3D12_RESOURCE_STATES shown_state[2]{s.osr_same.stereo_depth_state(), s.osr.stereo_depth_state()};
            for (int i = 0; i < 2; ++i)
                barriers.add(shown_depth[i], shown_state[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            for (UINT e = 0; e < 2; ++e) {
                guide_slots[e] = (s.guide_slot[e] + 1) % Impl::kGuideSlots;
                barriers.add(s.guide_motion[e][guide_slots[e]].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                barriers.add(s.guide_depth[e][guide_slots[e]].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            barriers.flush(list);
            ID3D12DescriptorHeap* guide_heaps[]{s.guide_heap.Get()};
            list->SetDescriptorHeaps(1, guide_heaps);
            list->SetComputeRootSignature(s.guide_root.Get());
            list->SetPipelineState(s.guide_pipeline.Get());
            for (int i = 0; i < 2; ++i) {
                const UINT eye = i == 0 ? source_eye : peer;
                const auto& shown_v2w = i == 0 ? frame.same_view_to_world : frame.destination_view_to_world;
                const auto& shown_v2c = i == 0 ? frame.source_view_to_clip : frame.destination_view_to_clip;
                const auto& shown_c2v = i == 0 ? source_clip_to_view : destination_clip_to_view;
                Impl::GuideConstants c{};
                c.size[0] = s.grid_width; c.size[1] = s.grid_height;
                if (s.shown_valid[eye]) {
                    store(c.shown_to_previous, uevr_multiply(uevr_multiply(uevr_multiply(shown_c2v, shown_v2w),
                        uevr_rigid_inverse(s.shown_view_to_world[eye])), s.shown_view_to_clip[eye]));
                    c.flags |= 1u;
                }
                if (i == 0)
                    store(c.shown_to_source, uevr_multiply(uevr_multiply(uevr_multiply(source_clip_to_view, frame.same_view_to_world),
                        source_world_to_view), frame.source_view_to_clip));
                else
                    std::memcpy(c.shown_to_source, in.backward_source, sizeof(c.shown_to_source));
                // The vectors point at the real eye's previous render, two
                // game frames back (UEVR's Ghosting Fix): half their object
                // motion falls in one frame. Without it they point at the
                // other eye, and only the camera's share is given.
                if (frame.history_motion && s.render_valid[source_eye]) {
                    store(c.source_to_history, uevr_multiply(uevr_multiply(uevr_multiply(source_clip_to_view,
                        frame.source_view_to_world), uevr_rigid_inverse(s.render_view_to_world[source_eye])),
                        s.render_view_to_clip[source_eye]));
                    c.motion_scale[0] = guide.scale_x * float(guide.depth_width) / float(guide.motion_width);
                    c.motion_scale[1] = guide.scale_y * float(guide.depth_height) / float(guide.motion_height);
                    c.object_weight = 0.5F;
                }
                auto cpu = s.guide_heap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(i) * 5 * s.increment;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                srv.Format = DXGI_FORMAT_R32_UINT;
                s.device->CreateShaderResourceView(shown_depth[i], &srv, cpu); cpu.ptr += s.increment;
                srv.Format = DXGI_FORMAT_R32_FLOAT;
                s.device->CreateShaderResourceView(s.depth.Get(), &srv, cpu); cpu.ptr += s.increment;
                srv.Format = DXGI_FORMAT_R16G16_FLOAT;
                s.device->CreateShaderResourceView(s.motion.Get(), &srv, cpu); cpu.ptr += s.increment;
                D3D12_UNORDERED_ACCESS_VIEW_DESC guide_uav{};
                guide_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                guide_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
                s.device->CreateUnorderedAccessView(s.guide_motion[eye][guide_slots[eye]].Get(), nullptr, &guide_uav, cpu); cpu.ptr += s.increment;
                guide_uav.Format = DXGI_FORMAT_R32_FLOAT;
                s.device->CreateUnorderedAccessView(s.guide_depth[eye][guide_slots[eye]].Get(), nullptr, &guide_uav, cpu);
                auto gpu = s.guide_heap->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += static_cast<UINT64>(i) * 5 * s.increment;
                list->SetComputeRootDescriptorTable(0, gpu);
                list->SetComputeRoot32BitConstants(1, sizeof(c) / sizeof(std::uint32_t), &c, 0);
                list->Dispatch((s.grid_width + 7) / 8, (s.grid_height + 7) / 8, 1);
                s.shown_view_to_world[eye] = shown_v2w;
                s.shown_view_to_clip[eye] = shown_v2c;
            }
            for (int i = 0; i < 2; ++i)
                barriers.add(shown_depth[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, shown_state[i]);
            for (UINT e = 0; e < 2; ++e) {
                barriers.add(s.guide_motion[e][guide_slots[e]].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
                barriers.add(s.guide_depth[e][guide_slots[e]].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            }
            guides_recorded = true;
        }
        barriers.add(s.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barriers.add(s.motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        barriers.flush(list);
        s.mark(5);
        s.resolve_marks();
        s.slot_kind[s.current_slot] = 2;

        hr = s.submit();
        if (FAILED(hr)) { s.osr.discard(); s.osr_same.discard(); return refuse(16, hr); }
        s.osr.commit();
        if (guides_recorded) {
            // Published once the list is submitted: the fence value covers it.
            for (UINT eye = 0; eye < 2; ++eye) {
                auto g = std::make_shared<DlssMotionVectorFrame>();
                const bool continued = s.shown_valid[eye] && s.guide_serial[eye] != 0;
                g->stream = 0x5545565200000000ULL | (static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(impl_.get())) & 0xffffff00ULL) | (eye + 1);
                g->epoch = s.guide_epoch;
                g->previous_serial = continued ? s.guide_serial[eye] : 0;
                g->serial = ++s.guide_serial[eye];
                g->publication = ++s.guide_publication;
                g->reset = !continued;
                g->motion_vectors = s.guide_motion[eye][guide_slots[eye]];
                g->depth = s.guide_depth[eye][guide_slots[eye]];
                g->motion_width = g->depth_width = s.grid_width;
                g->motion_height = g->depth_height = s.grid_height;
                g->output_width = s.color_width; g->output_height = s.color_height;
                g->resource_state = g->depth_resource_state = D3D12_RESOURCE_STATE_COMMON;
                g->depth_inverted = true; g->depth_infinite = true;
                g->camera_near = s.guide_near > 0 ? s.guide_near : 10.0F;
                g->camera_far = g->camera_near * 1.0e6F;
                g->frame_time_delta_ms = frame.display_period_ms;
                g->producer_queue = s.queue; g->verified_producer_device = s.device;
                g->motion_snapshot_device = g->depth_snapshot_device = s.device;
                g->producer_fence = s.fence; g->producer_fence_device = s.device;
                g->producer_fence_value = s.fence_value;
                auto set = std::make_shared<DlssMotionVectorSet>();
                set->eye_count = 1;
                set->eyes[0] = std::move(g);
                out.guides[eye] = std::move(set);
                s.guide_slot[eye] = guide_slots[eye];
                s.shown_valid[eye] = true;
            }
            s.render_view_to_world[source_eye] = frame.source_view_to_world;
            s.render_view_to_clip[source_eye] = frame.source_view_to_clip;
            s.render_valid[source_eye] = true;
        }
        s.previous_publication[source_eye] = guide.publication;
        s.history_camera_valid[source_eye] = frame.cameras_valid;
        s.history_engine[source_eye] = frame.source_engine_to_world;
        s.history_engine_valid[source_eye] = frame.cameras_valid && frame.engine_valid;
        out.result = S_OK;
        out.gpu_us = s.gpu_us;
        out.history_used = history_usable;
        out.history_motion_used = in.use_history_motion;
        return out;
    } catch (...) {
        return refuse(17, E_UNEXPECTED);
    }
}
}  // namespace xrfg
