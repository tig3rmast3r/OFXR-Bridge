#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>

#include "osr_clear_shader.hpp"
#include "osr_splat_shader.hpp"
#include "osr_fill_shader.hpp"

namespace xrfg {
// Root constants of src/d3d12/osr_hole_fill.hlsl, in declaration order.
struct OsrConstants {
    float source_to_destination[16]{};
    std::uint32_t depth_width{}, depth_height{}, color_width{}, color_height{};
    float fill_fraction{};
    std::uint32_t flags{};
    std::uint32_t min_run{3};
    std::uint32_t max_run{96};
    std::uint32_t background_margin{}; // depth texels beside the run on the background side
    std::uint32_t occluder_margin{};   // depth texels beside the run on the occluder side
};
static_assert(sizeof(OsrConstants) == 26 * sizeof(std::uint32_t));

// OSR (OFXR Stereo Reconstruction) stereo hole fill, GPU side. Private
// resources only: no XR swapchain, wait or readback. Descriptors are rewritten
// per recording, after the AFW submission fence retired the previous list, in
// two blocks so the coverage and fill dispatches of one list never share one.
class OsrHoleFill {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear_, splat_, fill_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> coverage_, output_;
    D3D12_RESOURCE_STATES coverage_state_{D3D12_RESOURCE_STATE_COMMON};
    UINT increment_{};
    UINT depth_width_{}, depth_height_{}, color_width_{}, color_height_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    static constexpr UINT kDescriptorsPerBlock = 5; // t0 depth, t1 color, t2 coverage, u0 coverage, u1 output

    HRESULT create_pipelines(ID3D12Device* device) {
        if (root_) return S_OK;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {2, ranges};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, sizeof(OsrConstants) / 4};
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 2; signature.pParameters = params;
        Microsoft::WRL::ComPtr<ID3DBlob> blob, error;
        HRESULT result = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
        if (FAILED(result)) return result;
        result = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_));
        if (FAILED(result)) return result;
        const auto pipeline = [&](const unsigned char* code, std::size_t size,
            Microsoft::WRL::ComPtr<ID3D12PipelineState>& out) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};
            ps.pRootSignature = root_.Get(); ps.CS = {code, size};
            return device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&out));
        };
        result = pipeline(g_xrfg_osr_clear_shader, sizeof(g_xrfg_osr_clear_shader), clear_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_splat_shader, sizeof(g_xrfg_osr_splat_shader), splat_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_fill_shader, sizeof(g_xrfg_osr_fill_shader), fill_);
        if (FAILED(result)) return result;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * kDescriptorsPerBlock;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
        if (FAILED(result)) return result;
        increment_ = device->GetDescriptorHandleIncrementSize(hd.Type);
        return S_OK;
    }

    static HRESULT create_texture(ID3D12Device* device, UINT width, UINT height,
        DXGI_FORMAT format, Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Format = format;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out.Reset();
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out));
    }

    D3D12_GPU_DESCRIPTOR_HANDLE bind(ID3D12Device* device, UINT block,
        ID3D12Resource* depth, ID3D12Resource* color) {
        auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(block) * kDescriptorsPerBlock * increment_;
        auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(block) * kDescriptorsPerBlock * increment_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(depth, &srv, cpu); cpu.ptr += increment_;
        srv.Format = format_;
        device->CreateShaderResourceView(color, &srv, cpu); cpu.ptr += increment_; // null view when absent
        srv.Format = DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(coverage_.Get(), &srv, cpu); cpu.ptr += increment_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R32_UINT;
        device->CreateUnorderedAccessView(coverage_.Get(), nullptr, &uav, cpu); cpu.ptr += increment_;
        uav.Format = format_;
        device->CreateUnorderedAccessView(output_.Get(), nullptr, &uav, cpu);
        return gpu;
    }

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        return b;
    }

public:
    // Every recording ends with the coverage back in COMMON, so the tracked
    // state never depends on whether the list was executed.
    void finish(ID3D12GraphicsCommandList* list) {
        if (!coverage_ || coverage_state_ == D3D12_RESOURCE_STATE_COMMON) return;
        auto barrier = transition(coverage_.Get(), coverage_state_, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &barrier);
        coverage_state_ = D3D12_RESOURCE_STATE_COMMON;
    }
    // A recording that is closed but never executed leaves the GPU state as
    // the last executed recording ended it: COMMON.
    void discard() noexcept { coverage_state_ = D3D12_RESOURCE_STATE_COMMON; }
    ID3D12Resource* coverage() const noexcept { return coverage_.Get(); }
    D3D12_RESOURCE_STATES coverage_state() const noexcept { return coverage_state_; }
    ID3D12Resource* output() const noexcept { return output_.Get(); }

    // color_format must support typed UAV stores (the caller checks, as for the
    // provider's reduced-quality expansion).
    HRESULT prepare(ID3D12Device* device, UINT depth_width, UINT depth_height,
        UINT color_width, UINT color_height, DXGI_FORMAT color_format) {
        HRESULT result = create_pipelines(device);
        if (FAILED(result)) return result;
        if (coverage_ && output_ && depth_width == depth_width_ && depth_height == depth_height_ &&
            color_width == color_width_ && color_height == color_height_ && color_format == format_)
            return S_OK;
        // Typeless so the F8 capture, which knows R32_TYPELESS, can read it; all
        // views are R32_UINT.
        result = create_texture(device, depth_width, depth_height, DXGI_FORMAT_R32_TYPELESS, coverage_);
        if (FAILED(result)) return result;
        result = create_texture(device, color_width, color_height, color_format, output_);
        if (FAILED(result)) { coverage_.Reset(); return result; }
        coverage_state_ = D3D12_RESOURCE_STATE_COMMON;
        depth_width_ = depth_width; depth_height_ = depth_height;
        color_width_ = color_width; color_height_ = color_height; format_ = color_format;
        return S_OK;
    }

    // Clears and splats the source depth into the destination coverage. The
    // depth is returned to its incoming state; the coverage stays in UAV state.
    void record_coverage(ID3D12Device* device, ID3D12GraphicsCommandList* list,
        ID3D12Resource* depth, D3D12_RESOURCE_STATES depth_state, const OsrConstants& constants) {
        const auto table = bind(device, 0, depth, nullptr);
        const bool depth_readable = (depth_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        D3D12_RESOURCE_BARRIER before[2]{}; UINT count = 0;
        if (!depth_readable) before[count++] = transition(depth, depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (coverage_state_ != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            before[count++] = transition(coverage_.Get(), coverage_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (count) list->ResourceBarrier(count, before);
        coverage_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        ID3D12DescriptorHeap* heaps[]{heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRootDescriptorTable(0, table);
        list->SetComputeRoot32BitConstants(1, sizeof(OsrConstants) / 4, &constants, 0);
        list->SetPipelineState(clear_.Get());
        list->Dispatch((depth_width_ + 7) / 8, (depth_height_ + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; uav.UAV.pResource = coverage_.Get();
        list->ResourceBarrier(1, &uav);
        list->SetPipelineState(splat_.Get());
        list->Dispatch((depth_width_ + 7) / 8, (depth_height_ + 7) / 8, 1);
        if (!depth_readable) {
            auto restore = transition(depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, depth_state);
            list->ResourceBarrier(1, &restore);
        }
    }

    // Writes color with the band filled into output(), which is left in COMMON
    // state. color is returned to its incoming state.
    void record_fill(ID3D12Device* device, ID3D12GraphicsCommandList* list,
        ID3D12Resource* depth, ID3D12Resource* color, D3D12_RESOURCE_STATES color_state,
        const OsrConstants& constants) {
        const auto table = bind(device, 1, depth, color);
        const bool color_readable = (color_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        D3D12_RESOURCE_BARRIER before[3]{}; UINT count = 0;
        if (!color_readable) before[count++] = transition(color, color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        before[count++] = transition(coverage_.Get(), coverage_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        before[count++] = transition(output_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(count, before);
        coverage_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ID3D12DescriptorHeap* heaps[]{heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRootDescriptorTable(0, table);
        list->SetComputeRoot32BitConstants(1, sizeof(OsrConstants) / 4, &constants, 0);
        list->SetPipelineState(fill_.Get());
        list->Dispatch((color_width_ + 7) / 8, (color_height_ + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER after[2]{}; count = 0;
        if (!color_readable) after[count++] = transition(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, color_state);
        after[count++] = transition(output_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(count, after);
    }
};
}  // namespace xrfg
