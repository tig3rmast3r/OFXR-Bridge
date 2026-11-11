#include "xrfg/d3d12_guide_composer.hpp"

#include <algorithm>
#include <cmath>

#include "guide_composer_compose_shader.hpp"
#include "guide_composer_decide_shader.hpp"

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

// The guide snapshots' state, in which consumers find the composed vectors too.
constexpr D3D12_RESOURCE_STATES kReadState =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr UINT kDescriptorsPerEye = 6;

// guide_composer.hlsl's Params.
struct Params {
    UINT origin_b[2];
    UINT size_b[2];
    UINT origin_r[2];
    UINT size_r[2];
    UINT slice_b;
    UINT slice_r;
    float scale_b[2];
    float scale_r[2];
    float output[2];
    float tangents_b[4];
    float tangents_r[4];
    float minimum_offset;
    UINT force_compose;
    UINT depth_padding[2]{};
    float depth_b[4]{},depth_r[4]{},planes[4]{};
    UINT depth_flags{},reject_occlusions{},depth_slice_b{},depth_slice_r{};
};
static_assert(sizeof(Params) == 44 * 4);

DXGI_FORMAT depth_format(DXGI_FORMAT format) noexcept {
    switch(format) {
    case DXGI_FORMAT_R32_TYPELESS:case DXGI_FORMAT_D32_FLOAT:return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:case DXGI_FORMAT_D16_UNORM:return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:case DXGI_FORMAT_D24_UNORM_S8_UINT:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:return format;
    default:return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT readable(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_R16G16_TYPELESS   ? DXGI_FORMAT_R16G16_FLOAT
           : format == DXGI_FORMAT_R32G32_TYPELESS ? DXGI_FORMAT_R32G32_FLOAT
                                                   : format;
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

D3D12_RESOURCE_BARRIER uav_barrier(ID3D12Resource* resource) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}

// Tangents of a field of view: left, right, down, up.
std::array<float, 4> tangents(const D3D12FieldOfView& fov) noexcept {
    return {std::tan(fov.angle_left), std::tan(fov.angle_right), std::tan(fov.angle_down),
            std::tan(fov.angle_up)};
}

}  // namespace

HRESULT D3D12GuideComposer::initialize(ID3D12Device* device) noexcept {
    try {
        if (device == nullptr) return E_INVALIDARG;
        device_ = device;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        for (UINT i = 0; i < 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable = {1, &ranges[i]};
        }
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants = {0, 0, sizeof(Params) / 4};
        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 3;
        description.pParameters = parameters;
        ComPtr<ID3DBlob> blob, error;
        HRESULT result = D3D12SerializeRootSignature(
            &description, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(result)) return result;
        result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&root_));
        if (FAILED(result)) return result;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.CS = {g_xrfg_guide_composer_decide_shader, sizeof(g_xrfg_guide_composer_decide_shader)};
        result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&decide_));
        if (FAILED(result)) return result;
        pipeline.CS = {g_xrfg_guide_composer_compose_shader, sizeof(g_xrfg_guide_composer_compose_shader)};
        result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compose_));
        if (FAILED(result)) return result;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kDescriptorsPerEye * kDlssMotionVectorEyeCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (auto& slot : heaps_) {
            result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&slot));
            if (FAILED(result)) return result;
        }
        increment_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

std::shared_ptr<const DlssMotionVectorSet> D3D12GuideComposer::compose(
    ID3D12GraphicsCommandList* list, UINT slot,
    const std::shared_ptr<const DlssMotionVectorSet>& set,
    std::span<const D3D12ReprojectionView> views,bool reject_occlusions) noexcept {
    try {
        if (!set || !list || !root_ || slot >= kSlots || views.empty()) return set;
        bool any = false;
        for (UINT eye = 0; eye < std::min(set->eye_count, kDlssMotionVectorEyeCount); ++eye) {
            const auto& frame = set->eyes[eye];
            any = any || (frame && frame->bridge && frame->bridge->motion_vectors && frame->motion_vectors);
        }
        if (!any) return set;
        auto result = std::make_shared<DlssMotionVectorSet>(*set);
        ID3D12DescriptorHeap* heaps[] = {heaps_[slot].Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        for (UINT eye = 0; eye < std::min(set->eye_count, kDlssMotionVectorEyeCount); ++eye) {
            const auto& b = set->eyes[eye];
            if (!b || !b->bridge || !b->bridge->motion_vectors || !b->motion_vectors) continue;
            const auto& r = *b->bridge;
            if (b->motion_width == 0 || b->motion_height == 0 || r.motion_width == 0 ||
                r.motion_height == 0 || b->output_width == 0 || b->output_height == 0) continue;
            auto& resources = eyes_[slot][eye];
            if (!resources.texture || resources.width != b->motion_width ||
                resources.height != b->motion_height) {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC texture{};
                texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                texture.Width = b->motion_width;
                texture.Height = b->motion_height;
                texture.DepthOrArraySize = 1;
                texture.MipLevels = 1;
                texture.Format = DXGI_FORMAT_R16G16_FLOAT;
                texture.SampleDesc.Count = 1;
                texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                resources.texture.Reset();
                if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
                        kReadState, nullptr, IID_PPV_ARGS(&resources.texture)))) {
                    return set;
                }
                resources.width = b->motion_width;
                resources.height = b->motion_height;
            }
            if(!resources.allocation || resources.allocation->resource.Get()!=resources.texture.Get())
                resources.allocation=std::make_shared<const DlssSnapshotAllocation>(resources.texture.Get(),device_.Get());
            if (!resources.decision) {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC buffer{};
                buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                buffer.Width = 256;
                buffer.Height = 1;
                buffer.DepthOrArraySize = 1;
                buffer.MipLevels = 1;
                buffer.SampleDesc.Count = 1;
                buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                        IID_PPV_ARGS(&resources.decision)))) {
                    return set;
                }
            }
            auto cpu = heaps_[slot]->GetCPUDescriptorHandleForHeapStart();
            cpu.ptr += SIZE_T(eye) * kDescriptorsPerEye * increment_;
            const auto next = [&] { auto h = cpu; cpu.ptr += increment_; return h; };
            for (const auto* frame : {b.get(), &r}) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format = readable(frame->motion_vectors->GetDesc().Format);
                view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.MipLevels = 1;
                view.Texture2DArray.ArraySize = frame->motion_vectors->GetDesc().DepthOrArraySize;
                device_->CreateShaderResourceView(frame->motion_vectors.Get(), &view, next());
            }
            const auto valid_depth=[](const DlssMotionVectorFrame& g) {
                if(!g.depth||!g.depth_width||!g.depth_height||!std::isfinite(g.camera_near)||g.camera_near<=0)return false;
                const auto d=g.depth->GetDesc();
                return d.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.SampleDesc.Count==1 &&
                    depth_format(d.Format)!=DXGI_FORMAT_UNKNOWN &&
                    std::uint64_t(g.depth_x)+g.depth_width<=d.Width && std::uint64_t(g.depth_y)+g.depth_height<=d.Height &&
                    (g.depth_infinite||(std::isfinite(g.camera_far)&&g.camera_far>g.camera_near));
            };
            const bool depth_check=reject_occlusions && valid_depth(*b) && valid_depth(r);
            for(const auto* frame:{b.get(),&r}) {
                D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=depth_check?depth_format(frame->depth->GetDesc().Format):DXGI_FORMAT_R32_FLOAT;
                view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;view.Texture2DArray.MipLevels=1;
                view.Texture2DArray.ArraySize=depth_check?frame->depth->GetDesc().DepthOrArraySize:1;
                device_->CreateShaderResourceView(depth_check?frame->depth.Get():nullptr,&view,next());
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC texture_view{};
            texture_view.Format = DXGI_FORMAT_R16G16_FLOAT;
            texture_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(resources.texture.Get(), nullptr, &texture_view, next());
            D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_view{};
            buffer_view.Format = DXGI_FORMAT_R32_TYPELESS;
            buffer_view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            buffer_view.Buffer.NumElements = 64;
            buffer_view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            device_->CreateUnorderedAccessView(resources.decision.Get(), nullptr, &buffer_view, next());

            const auto& fov = views[std::min<std::size_t>(eye, views.size() - 1)].fov;
            const auto t = tangents(fov);
            Params p{};
            p.origin_b[0] = b->motion_x; p.origin_b[1] = b->motion_y;
            p.size_b[0] = b->motion_width; p.size_b[1] = b->motion_height;
            p.origin_r[0] = r.motion_x; p.origin_r[1] = r.motion_y;
            p.size_r[0] = r.motion_width; p.size_r[1] = r.motion_height;
            p.slice_b = b->motion_slice;
            p.slice_r = r.motion_slice;
            p.scale_b[0] = b->scale_x * float(b->output_width) / float(b->motion_width);
            p.scale_b[1] = b->scale_y * float(b->output_height) / float(b->motion_height);
            p.scale_r[0] = r.scale_x * float(r.output_width) / float(r.motion_width);
            p.scale_r[1] = r.scale_y * float(r.output_height) / float(r.motion_height);
            p.output[0] = float(b->output_width);
            p.output[1] = float(b->output_height);
            // The other eye's field of view, taken as this one's mirror image.
            p.tangents_b[0] = t[0]; p.tangents_b[1] = t[1]; p.tangents_b[2] = t[2]; p.tangents_b[3] = t[3];
            p.tangents_r[0] = -t[1]; p.tangents_r[1] = -t[0]; p.tangents_r[2] = t[2]; p.tangents_r[3] = t[3];
            p.minimum_offset = 16.0F;
            p.force_compose = b->raw_motion_native_span_confirmed && b->raw_motion_native_span==1;
            p.reject_occlusions=depth_check;
            if(depth_check) {
                p.depth_b[0]=float(b->depth_x);p.depth_b[1]=float(b->depth_y);p.depth_b[2]=float(b->depth_width);p.depth_b[3]=float(b->depth_height);
                p.depth_r[0]=float(r.depth_x);p.depth_r[1]=float(r.depth_y);p.depth_r[2]=float(r.depth_width);p.depth_r[3]=float(r.depth_height);
                p.planes[0]=b->camera_near;p.planes[1]=b->camera_far;p.planes[2]=r.camera_near;p.planes[3]=r.camera_far;
                p.depth_flags=(b->depth_inverted?1u:0u)|(r.depth_inverted?2u:0u)|(b->depth_infinite?4u:0u)|(r.depth_infinite?8u:0u);
                p.depth_slice_b=b->depth->GetDesc().DepthOrArraySize==1?0:b->output_slice;
                p.depth_slice_r=r.depth->GetDesc().DepthOrArraySize==1?0:r.output_slice;
            }
            if (p.scale_b[0] == 0.0F || p.scale_b[1] == 0.0F) continue;

            auto gpu = heaps_[slot]->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(eye) * kDescriptorsPerEye * increment_;
            const auto barrier = transition(resources.texture.Get(), kReadState,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &barrier);
            list->SetComputeRootDescriptorTable(0, gpu);
            auto uavs = gpu;
            uavs.ptr += 4ULL * increment_;
            list->SetComputeRootDescriptorTable(1, uavs);
            list->SetComputeRoot32BitConstants(2, sizeof(Params) / 4, &p, 0);
            list->SetPipelineState(decide_.Get());
            list->Dispatch(1, 1, 1);
            const auto decided = uav_barrier(resources.decision.Get());
            list->ResourceBarrier(1, &decided);
            list->SetPipelineState(compose_.Get());
            std::array<D3D12_RESOURCE_BARRIER,2> depth_barriers{};UINT depth_count=0;
            if(depth_check)for(const auto* frame:{b.get(),&r}) {
                if(frame->depth_resource_state==kReadState ||
                    (frame==&r && r.depth.Get()==b->depth.Get()))continue;
                depth_barriers[depth_count++]=transition(frame->depth.Get(),frame->depth_resource_state,kReadState);
            }
            if(depth_count)list->ResourceBarrier(depth_count,depth_barriers.data());
            list->Dispatch((b->motion_width + 7) / 8, (b->motion_height + 7) / 8, 1);
            for(UINT n=0;n<depth_count;++n)std::swap(depth_barriers[n].Transition.StateBefore,depth_barriers[n].Transition.StateAfter);
            if(depth_count)list->ResourceBarrier(depth_count,depth_barriers.data());
            const std::array<D3D12_RESOURCE_BARRIER, 2> after{
                transition(resources.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kReadState),
                uav_barrier(resources.decision.Get())};
            list->ResourceBarrier(UINT(after.size()), after.data());

            auto composed = std::make_shared<DlssMotionVectorFrame>(*b);
            composed->motion_vectors = resources.texture;
            composed->motion_snapshot_device = device_;
            composed->motion_allocation = resources.allocation;
            composed->motion_x = 0;
            composed->motion_y = 0;
            composed->motion_slice = 0;
            composed->resource_state = kReadState;
            // The vectors now reach this eye's previous image, whose jitter is
            // the bridge's previous.
            composed->previous_jitter_x = r.previous_jitter_x;
            composed->previous_jitter_y = r.previous_jitter_y;
            composed->bridge.reset();
            if(p.force_compose) {
                composed->raw_motion_native_span=2;
                composed->raw_motion_native_span_confirmed=true;
            }
            result->eyes[eye] = std::move(composed);
            ++composed_;
        }
        return result;
    } catch (...) {
        return set;
    }
}

}  // namespace xrfg
