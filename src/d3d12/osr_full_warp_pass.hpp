#pragma once
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>

#include "osr_full_clear_shader.hpp"
#include "osr_full_splat_shader.hpp"
#include "osr_full_splat_history_shader.hpp"
#include "osr_full_splat_offset_shader.hpp"
#include "osr_full_resolve_shader.hpp"
#include "osr_full_inpaint_shader.hpp"
#include "osr_full_resolve_lower_shader.hpp"
#include "osr_full_resolve_map_shader.hpp"
#include "xrfg/osr_full_warp.hpp"

namespace xrfg {
// Root constants of src/d3d12/osr_full_warp.hlsl, in declaration order. The
// three matrices mean different things per dispatch; see the shader.
struct OsrFullConstants {
    float forward[16]{};
    float backward_source[16]{};
    float backward_history[16]{};
    std::uint32_t depth_width{}, depth_height{}, color_width{}, color_height{};
    std::uint32_t flags{};
    std::uint32_t max_run{kOsrFullMaxRun};
    float depth_tolerance{kOsrFullDepthTolerance};
    float locked_depth{2.0F}; // reversed depth from which history texels are camera-attached; 2 = none
    float motion_scale[2]{}; // composed motion value -> depth texels; zero = no motion compensation
    float source_dejitter[2]{};  // depth texel -> scene position, in texels (window fraction - jitter)
    float history_dejitter[2]{};
};
static_assert(sizeof(OsrFullConstants) == 62 * sizeof(std::uint32_t));

// The camera of an eye's last real frame, in the provider's convention
// (row vectors, reversed depth), kept with that frame's colour and depth.
struct OsrFullHistoryCamera {
    std::array<float, 16> view_to_world{}, world_to_view{}, view_to_clip{}, clip_to_view{};
};

struct OsrFullHistory {
    std::uint64_t publication{}, stream{}, epoch{};
    OsrFullHistoryCamera camera{};
    // The same eye's real frame before this one: its composed same-eye motion
    // points there, and the camera's share of that motion comes from this.
    std::uint64_t previous_publication{};
    OsrFullHistoryCamera camera_previous{};
    float motion_scale[2]{}; // composed motion value -> depth texels at that frame
    float dejitter[2]{};     // window fraction - DLSS jitter of that frame, in texels
    bool has_motion{};
};

// OSR (OFXR Stereo Reconstruction), full reconstruction, GPU side. Private
// resources only: no XR swapchain, wait or readback. Descriptors are rewritten
// per recording, after the AFW submission fence retired the previous list, in
// one block per dispatch so no two dispatches of a list share one.
class OsrFullWarp {
    struct Tracked {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    };
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> clear_, splat_, splat_history_, splat_offset_, resolve_, resolve_lower_, resolve_map_, inpaint_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    Tracked stereo_, history_splat_, history_offset_, warped_, resolved_, output_, scene_state_, lower_map_;
    // One serialized AFW recording at a time, like the descriptor blocks.
    // Uniform matrices only: no new image, dispatch, wait or readback.
    Microsoft::WRL::ComPtr<ID3D12Resource> current_history_data_;
    void* current_history_mapped_{};
    bool scene_initialized_{};
    bool lower_active_{};
    std::array<Tracked, 2> history_color_{}, history_depth_{}, history_motion_{};
    std::array<OsrFullHistory, 2> history_{};
    OsrFullHistory pending_{};
    unsigned pending_eye_{2};
    UINT increment_{};
    UINT depth_width_{}, depth_height_{}, color_width_{}, color_height_{};
    DXGI_FORMAT format_{DXGI_FORMAT_UNKNOWN};
    static constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT; // kLukeAfwMotionFormat
    static constexpr UINT kSrvCount = 13, kUavCount = 8;
    static constexpr UINT kDescriptorsPerBlock = kSrvCount + kUavCount;
    enum Block : UINT { block_clear, block_splat_stereo, block_splat_history, block_splat_offset,
        block_resolve, block_inpaint, block_resolve_map, block_count };

    HRESULT create_pipelines(ID3D12Device* device) {
        if (root_) return S_OK;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvCount, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, kUavCount, 0, 0, kSrvCount};
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable = {2, ranges};
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants = {0, 0, sizeof(OsrFullConstants) / 4};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC signature{};
        signature.NumParameters = 2; signature.pParameters = params;
        signature.NumStaticSamplers = 1; signature.pStaticSamplers = &sampler;
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
        result = pipeline(g_xrfg_osr_full_clear_shader, sizeof(g_xrfg_osr_full_clear_shader), clear_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_full_splat_shader, sizeof(g_xrfg_osr_full_splat_shader), splat_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_full_splat_history_shader, sizeof(g_xrfg_osr_full_splat_history_shader), splat_history_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_full_splat_offset_shader, sizeof(g_xrfg_osr_full_splat_offset_shader), splat_offset_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_full_resolve_shader, sizeof(g_xrfg_osr_full_resolve_shader), resolve_);
        if (FAILED(result)) return result;
        result = pipeline(g_xrfg_osr_full_inpaint_shader, sizeof(g_xrfg_osr_full_inpaint_shader), inpaint_);
        if (FAILED(result)) return result;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = block_count * kDescriptorsPerBlock;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_));
        if (FAILED(result)) return result;
        D3D12_HEAP_PROPERTIES upload{}; upload.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC data{}; data.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        data.Width = 320; data.Height = 1; data.DepthOrArraySize = 1; data.MipLevels = 1;
        data.SampleDesc.Count = 1; data.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &data,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&current_history_data_));
        if (FAILED(result)) { root_.Reset(); return result; }
        const D3D12_RANGE no_read{0, 0};
        result = current_history_data_->Map(0, &no_read, &current_history_mapped_);
        if (FAILED(result)) { root_.Reset(); return result; }
        increment_ = device->GetDescriptorHandleIncrementSize(hd.Type);
        return S_OK;
    }

    static HRESULT create_texture(ID3D12Device* device, UINT width, UINT height,
        DXGI_FORMAT format, bool unordered, Tracked& out) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = width; rd.Height = height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Format = format;
        rd.Flags = unordered ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        out.resource.Reset();
        out.state = D3D12_RESOURCE_STATE_COMMON;
        return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out.resource));
    }

    // Barrier batch for one pass: tracked resources move and remember their
    // state; borrowed ones (the provider's depth and colour) are restored by
    // the caller with the same helper.
    struct Barriers {
        std::array<D3D12_RESOURCE_BARRIER, 16> list{};
        UINT count{};
        void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            if (!resource || before == after || count == list.size()) return;
            auto& b = list[count++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        }
        void track(Tracked& tracked, D3D12_RESOURCE_STATES after) {
            if (!tracked.resource) return;
            transition(tracked.resource.Get(), tracked.state, after);
            tracked.state = after;
        }
        void uav(ID3D12Resource* resource) {
            if (count == list.size()) return;
            auto& b = list[count++];
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; b.UAV.pResource = resource;
        }
        void flush(ID3D12GraphicsCommandList* list_) {
            if (count) list_->ResourceBarrier(count, list.data());
            count = 0;
        }
    };

    D3D12_GPU_DESCRIPTOR_HANDLE bind(ID3D12Device* device, UINT block,
        const std::array<ID3D12Resource*, kSrvCount>& srvs,
        const std::array<ID3D12Resource*, kUavCount>& uavs) {
        auto cpu = heap_->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T(block) * kDescriptorsPerBlock * increment_;
        auto gpu = heap_->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64(block) * kDescriptorsPerBlock * increment_;
        // t0 depth, t1 colour, t2 stereo splat, t3 history splat, t4 history
        // depth, t5 history colour, t6 resolved, t7 warped, t8 history motion,
        // t9 history offset; u0 splat, u1 splat 2, u2 warped, u3 resolved,
        // u4 output, u5 history offset, u6 scene state.
        const DXGI_FORMAT srv_formats[kSrvCount]{DXGI_FORMAT_R32_FLOAT, format_, DXGI_FORMAT_R32_UINT,
            DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT, format_, DXGI_FORMAT_R32_UINT, format_,
            kMotionFormat, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32_FLOAT};
        const DXGI_FORMAT uav_formats[kUavCount]{DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, format_,
            DXGI_FORMAT_R32_UINT, format_, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32G32B32A32_FLOAT};
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < kSrvCount; ++i) {
            srv.Format = srv_formats[i];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D = {}; srv.Texture2D.MipLevels = 1;
            if (i == 11) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Buffer = {}; srv.Buffer.NumElements = 20;
            }
            device->CreateShaderResourceView(srvs[i], &srv, cpu); // null view when absent
            cpu.ptr += increment_;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT i = 0; i < kUavCount; ++i) {
            uav.Format = uav_formats[i];
            device->CreateUnorderedAccessView(i == 6 ? scene_state_.resource.Get() : uavs[i], nullptr, &uav, cpu);
            cpu.ptr += increment_;
        }
        return gpu;
    }

    void dispatch(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pipeline,
        D3D12_GPU_DESCRIPTOR_HANDLE table, const OsrFullConstants& constants, UINT width, UINT height, UINT threads = 8) {
        ID3D12DescriptorHeap* heaps[]{heap_.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(root_.Get());
        list->SetComputeRootDescriptorTable(0, table);
        list->SetComputeRoot32BitConstants(1, sizeof(OsrFullConstants) / 4, &constants, 0);
        list->SetPipelineState(pipeline);
        list->Dispatch((width + threads - 1) / threads, (height + threads - 1) / threads, 1);
    }

public:
    // Inputs of one reconstruction. Borrowed resources are returned to their
    // incoming states before record() returns.
    struct Inputs {
        ID3D12Resource* source_depth{};
        D3D12_RESOURCE_STATES source_depth_state{D3D12_RESOURCE_STATE_COMMON};
        ID3D12Resource* source_color{}; // colour resolution, format_ family
        D3D12_RESOURCE_STATES source_color_state{D3D12_RESOURCE_STATE_COMMON};
        ID3D12Resource* source_motion{}; // composed same-eye motion, depth resolution, kMotionFormat
        D3D12_RESOURCE_STATES source_motion_state{D3D12_RESOURCE_STATE_COMMON};
        unsigned source_eye{};
        bool use_history{};
        bool reset_history{};
        bool use_history_motion{};
        bool use_current_history_motion{}; // contiguous native span2 only
        float current_history_data[80]{}; // shader CurrentHistoryData rows0..19
        float forward_stereo[16]{};           // source clip -> destination clip
        float forward_history[16]{};          // history clip -> destination clip
        float forward_history_previous[16]{}; // history clip -> clip of that eye's frame before it
        // Camera-attached history texels: history clip -> destination clip with
        // the camera they hang on held (kOsrFullFlagLockedFollows); unset, they
        // keep their screen place.
        bool locked_follows{};
        bool warp_keeps_attached{}; // kOsrFullFlagWarpKeepsAttached
        float forward_history_locked[16]{};
        float backward_source[16]{};
        float backward_history[16]{};
        float history_motion_scale[2]{};
        float source_dejitter[2]{};
        float history_dejitter[2]{};
        float locked_depth{2.0F}; // osr_full_depth_at(kOsrFullLockedDistanceMetres)
        bool debug{};
        bool lower_quality{};
    };

    bool lower_quality_active() const noexcept {return lower_active_;}
    ID3D12Resource* output() const noexcept { return output_.resource.Get(); }
    ID3D12Resource* stereo_depth() const noexcept { return stereo_.resource.Get(); }
    D3D12_RESOURCE_STATES stereo_depth_state() const noexcept { return stereo_.state; }
    ID3D12Resource* resolved() const noexcept { return resolved_.resource.Get(); }
    ID3D12Resource* history_depth(unsigned eye) const noexcept { return history_depth_[eye&1].resource.Get(); }
    D3D12_RESOURCE_STATES history_depth_state(unsigned eye) const noexcept { return history_depth_[eye&1].state; }
    ID3D12Resource* scene_state() const noexcept { return scene_state_.resource.Get(); }
    D3D12_RESOURCE_STATES scene_state_state() const noexcept { return scene_state_.state; }
    D3D12_RESOURCE_STATES resolved_state() const noexcept { return resolved_.state; }
    const OsrFullHistory& history(unsigned eye) const noexcept { return history_[eye & 1]; }

    // color_format must support typed UAV stores (the caller checks, as for the
    // provider's reduced-quality expansion). A geometry or format change drops
    // the history: its frames no longer match the new images.
    HRESULT prepare(ID3D12Device* device, UINT depth_width, UINT depth_height,
        UINT color_width, UINT color_height, DXGI_FORMAT color_format) {
        HRESULT result = create_pipelines(device);
        if (FAILED(result)) return result;
        if (output_.resource && depth_width == depth_width_ && depth_height == depth_height_ &&
            color_width == color_width_ && color_height == color_height_ && color_format == format_)
            return S_OK;
        lower_map_.resource.Reset(); lower_map_.state=D3D12_RESOURCE_STATE_COMMON;
        history_ = {};
        scene_initialized_ = false;
        pending_eye_ = 2;
        // Typeless so the F8 capture, which knows R32_TYPELESS, can read them;
        // the views are R32_UINT.
        result = create_texture(device, depth_width, depth_height, DXGI_FORMAT_R32_TYPELESS, true, stereo_);
        if (SUCCEEDED(result)) result = create_texture(device, 5, 1, DXGI_FORMAT_R32_TYPELESS, true, scene_state_);
        if (SUCCEEDED(result)) result = create_texture(device, depth_width, depth_height, DXGI_FORMAT_R32_TYPELESS, true, history_splat_);
        if (SUCCEEDED(result)) result = create_texture(device, depth_width, depth_height, DXGI_FORMAT_R32_TYPELESS, true, history_offset_);
        if (SUCCEEDED(result)) result = create_texture(device, color_width, color_height, color_format, true, warped_);
        if (SUCCEEDED(result)) result = create_texture(device, color_width, color_height, DXGI_FORMAT_R32_TYPELESS, true, resolved_);
        if (SUCCEEDED(result)) result = create_texture(device, color_width, color_height, color_format, true, output_);
        for (unsigned eye = 0; eye < 2 && SUCCEEDED(result); ++eye) {
            result = create_texture(device, color_width, color_height, color_format, false, history_color_[eye]);
            if (SUCCEEDED(result)) result = create_texture(device, depth_width, depth_height, DXGI_FORMAT_R32_FLOAT, false, history_depth_[eye]);
            if (SUCCEEDED(result)) result = create_texture(device, depth_width, depth_height, kMotionFormat, false, history_motion_[eye]);
        }
        if (FAILED(result)) { output_.resource.Reset(); return result; }
        depth_width_ = depth_width; depth_height_ = depth_height;
        color_width_ = color_width; color_height_ = color_height; format_ = color_format;
        return S_OK;
    }

    // Records the passes. Every private resource ends in COMMON; the borrowed
    // depth and colour return to their incoming states.
    void record(ID3D12Device* device, ID3D12GraphicsCommandList* list, const Inputs& in,ID3D12QueryHeap* timing=nullptr,UINT query_base=0) {
        const auto mark=[&](UINT index){if(timing)list->EndQuery(timing,D3D12_QUERY_TYPE_TIMESTAMP,query_base+index);};
        if(!in.lower_quality){lower_map_.resource.Reset();lower_map_.state=D3D12_RESOURCE_STATE_COMMON;}
        bool lower=in.lower_quality && in.use_history;
        if(lower && (!resolve_lower_ || !resolve_map_)){
            D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root_.Get();
            if(!resolve_lower_){ps.CS={g_xrfg_osr_full_resolve_lower_shader,sizeof(g_xrfg_osr_full_resolve_lower_shader)};
                lower=SUCCEEDED(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&resolve_lower_)));}
            if(lower && !resolve_map_){ps.CS={g_xrfg_osr_full_resolve_map_shader,sizeof(g_xrfg_osr_full_resolve_map_shader)};
                lower=SUCCEEDED(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&resolve_map_)));}
        }
        if(lower && !lower_map_.resource)lower=SUCCEEDED(create_texture(device,(color_width_+1)/2,(color_height_+1)/2,DXGI_FORMAT_R32G32B32A32_FLOAT,true,lower_map_));
        lower_active_=lower;
        mark(0);
        const unsigned source_eye = in.source_eye & 1, destination_eye = 1 - source_eye;
        OsrFullConstants constants{};
        constants.depth_width = depth_width_; constants.depth_height = depth_height_;
        constants.color_width = color_width_; constants.color_height = color_height_;
        constants.flags = osr_full_flags(in.debug, in.use_history);
        constants.flags |= (source_eye << 4) | ((!scene_initialized_ || in.reset_history) ? 8u : 0u);
        if (in.warp_keeps_attached) constants.flags |= kOsrFullFlagWarpKeepsAttached;
        std::memcpy(constants.forward, in.forward_stereo, sizeof(constants.forward));
        std::memcpy(constants.backward_source, in.backward_source, sizeof(constants.backward_source));
        std::memcpy(constants.backward_history, in.backward_history, sizeof(constants.backward_history));
        constants.source_dejitter[0] = in.source_dejitter[0]; constants.source_dejitter[1] = in.source_dejitter[1];
        constants.history_dejitter[0] = in.history_dejitter[0]; constants.history_dejitter[1] = in.history_dejitter[1];
        constants.locked_depth = in.locked_depth;
        const bool current_motion = in.use_history && in.use_current_history_motion && in.source_motion;
        if (current_motion) {
            std::memcpy(current_history_mapped_, in.current_history_data, sizeof(in.current_history_data));
        }
        const bool motion_readable = (in.source_motion_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        const bool depth_readable = (in.source_depth_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        const bool color_readable = (in.source_color_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        Tracked& history_depth = history_depth_[destination_eye];
        Tracked& history_color = history_color_[destination_eye];
        Tracked& history_motion = history_motion_[destination_eye];
        Barriers barriers;
        // 1. clear
        barriers.track(stereo_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.track(history_splat_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.track(history_offset_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.track(scene_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!depth_readable) barriers.transition(in.source_depth, in.source_depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!color_readable) barriers.transition(in.source_color, in.source_color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (in.use_history) {
            barriers.track(history_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers.track(history_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barriers.track(history_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        barriers.flush(list);
        dispatch(list, clear_.Get(), bind(device, block_clear,
            {in.source_depth, in.source_color, nullptr, nullptr,
             in.use_history ? history_depth.resource.Get() : nullptr,
             in.use_history ? history_color.resource.Get() : nullptr},
            {stereo_.resource.Get(), history_splat_.resource.Get(), nullptr, nullptr, nullptr, history_offset_.resource.Get()}),
            constants, depth_width_, depth_height_);
        barriers.uav(stereo_.resource.Get());
        barriers.uav(history_splat_.resource.Get());
        barriers.uav(history_offset_.resource.Get());
        barriers.uav(scene_state_.resource.Get());
        barriers.flush(list);
        mark(1);
        // 2. splats
        std::memcpy(constants.forward, in.forward_stereo, sizeof(constants.forward));
        dispatch(list, splat_.Get(), bind(device, block_splat_stereo, {in.source_depth}, {stereo_.resource.Get()}),
            constants, depth_width_, depth_height_);
        mark(2);
        if (in.use_history) {
            // History splat: Forward = history -> destination, BackwardHistory
            // = history -> the frame before it (camera share of the motion).
            OsrFullConstants history_constants = constants;
            std::memcpy(history_constants.forward, in.forward_history, sizeof(history_constants.forward));
            std::memcpy(history_constants.backward_history, in.forward_history_previous, sizeof(history_constants.backward_history));
            if (in.locked_follows) {
                std::memcpy(history_constants.backward_source, in.forward_history_locked, sizeof(history_constants.backward_source));
                history_constants.flags |= kOsrFullFlagLockedFollows;
            }
            if (in.use_history_motion) {
                history_constants.motion_scale[0] = in.history_motion_scale[0];
                history_constants.motion_scale[1] = in.history_motion_scale[1];
            }
            const std::array<ID3D12Resource*, kSrvCount> srvs{history_depth.resource.Get(), nullptr, nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, history_motion.resource.Get(), nullptr};
            dispatch(list, splat_history_.Get(), bind(device, block_splat_history, srvs, {history_splat_.resource.Get()}),
                history_constants, depth_width_, depth_height_);
            barriers.uav(history_splat_.resource.Get());
            barriers.flush(list);
            dispatch(list, splat_offset_.Get(), bind(device, block_splat_offset, srvs,
                {history_splat_.resource.Get(), nullptr, nullptr, nullptr, nullptr, history_offset_.resource.Get()}),
                history_constants, depth_width_, depth_height_);
        }
        mark(3);
        // 3. resolve: current native motion pulls the previous own-eye colour.
        // History splats/clear keep their original constants and motion field.
        if (current_motion) {
            constants.flags |= 256u;
            barriers.track(history_depth_[source_eye], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (!motion_readable) barriers.transition(in.source_motion, in.source_motion_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        barriers.track(stereo_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.track(history_splat_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.track(history_offset_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.track(warped_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.track(resolved_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barriers.flush(list);
        const std::array<ID3D12Resource*,kSrvCount> resolve_srvs{in.source_depth,in.source_color,stereo_.resource.Get(),history_splat_.resource.Get(),
             in.use_history?history_depth.resource.Get():nullptr,in.use_history?history_color.resource.Get():nullptr,
             nullptr,nullptr,current_motion?in.source_motion:nullptr,history_offset_.resource.Get(),lower?lower_map_.resource.Get():nullptr,
             current_motion?current_history_data_.Get():nullptr,current_motion?history_depth_[source_eye].resource.Get():nullptr};
        if(lower){
            barriers.track(lower_map_,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);barriers.flush(list);
            auto map_srvs=resolve_srvs;map_srvs[10]=nullptr;
            dispatch(list,resolve_map_.Get(),bind(device,block_resolve_map,map_srvs,
                {nullptr,nullptr,warped_.resource.Get(),resolved_.resource.Get(),nullptr,nullptr,nullptr,lower_map_.resource.Get()}),constants,(color_width_+1)/2,(color_height_+1)/2);
            barriers.uav(warped_.resource.Get());barriers.uav(resolved_.resource.Get());
            barriers.track(lower_map_,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);barriers.flush(list);
        }
        dispatch(list,lower?resolve_lower_.Get():resolve_.Get(),bind(device,block_resolve,resolve_srvs,
            {nullptr,nullptr,warped_.resource.Get(),resolved_.resource.Get()}),constants,color_width_,color_height_);
        mark(4);
        // 4. inpaint
        if (current_motion) {
            barriers.track(history_depth_[source_eye], D3D12_RESOURCE_STATE_COMMON);
            if (!motion_readable) barriers.transition(in.source_motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.source_motion_state);
        }
        barriers.track(warped_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.track(resolved_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barriers.track(output_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!depth_readable) barriers.transition(in.source_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.source_depth_state);
        if (!color_readable) barriers.transition(in.source_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.source_color_state);
        barriers.flush(list);
        dispatch(list, inpaint_.Get(), bind(device, block_inpaint,
            {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, resolved_.resource.Get(), warped_.resource.Get()},
            {nullptr, nullptr, nullptr, nullptr, output_.resource.Get()}),
            constants, color_width_, color_height_);
        barriers.track(stereo_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(history_splat_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(history_offset_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(warped_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(resolved_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(output_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(lower_map_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(scene_state_, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(history_depth, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(history_color, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(history_motion, D3D12_RESOURCE_STATE_COMMON);
        barriers.flush(list);
        mark(5);
    }

    // Keeps this frame's source eye as that eye's history: colour, depth,
    // composed motion and camera. The metadata takes effect with commit() once
    // the list executed; the frame it replaces becomes that eye's previous one.
    void record_history_update(ID3D12GraphicsCommandList* list, const Inputs& in,
        const OsrFullHistory& history) {
        const unsigned source_eye = in.source_eye & 1;
        Tracked& color = history_color_[source_eye];
        Tracked& depth = history_depth_[source_eye];
        Tracked& motion = history_motion_[source_eye];
        const bool copy_motion = in.source_motion != nullptr;
        Barriers barriers;
        barriers.track(color, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.track(depth, D3D12_RESOURCE_STATE_COPY_DEST);
        if (copy_motion) barriers.track(motion, D3D12_RESOURCE_STATE_COPY_DEST);
        barriers.transition(in.source_color, in.source_color_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.transition(in.source_depth, in.source_depth_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (copy_motion) barriers.transition(in.source_motion, in.source_motion_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barriers.flush(list);
        const auto copy = [&](ID3D12Resource* destination, ID3D12Resource* source, UINT width, UINT height) {
            D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
            dst.pResource = destination; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = source; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, width, height, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        };
        copy(color.resource.Get(), in.source_color, color_width_, color_height_);
        copy(depth.resource.Get(), in.source_depth, depth_width_, depth_height_);
        if (copy_motion) copy(motion.resource.Get(), in.source_motion, depth_width_, depth_height_);
        barriers.track(color, D3D12_RESOURCE_STATE_COMMON);
        barriers.track(depth, D3D12_RESOURCE_STATE_COMMON);
        if (copy_motion) barriers.track(motion, D3D12_RESOURCE_STATE_COMMON);
        barriers.transition(in.source_color, D3D12_RESOURCE_STATE_COPY_SOURCE, in.source_color_state);
        barriers.transition(in.source_depth, D3D12_RESOURCE_STATE_COPY_SOURCE, in.source_depth_state);
        if (copy_motion) barriers.transition(in.source_motion, D3D12_RESOURCE_STATE_COPY_SOURCE, in.source_motion_state);
        barriers.flush(list);
        pending_ = history;
        pending_.has_motion = copy_motion;
        pending_.previous_publication = history_[source_eye].publication;
        pending_.camera_previous = history_[source_eye].camera;
        pending_eye_ = source_eye;
    }

    // The recorded list executed: the copied frame is that eye's history.
    void commit() noexcept {
        if (pending_eye_ < 2) {
            history_[pending_eye_] = pending_;
            scene_initialized_ = true;
        }
        pending_eye_ = 2;
    }
    // A recording that is closed but never executed leaves the GPU state as
    // the last executed recording ended it: COMMON, with the old history.
    void discard() noexcept {
        for (Tracked* t : {&stereo_, &history_splat_, &history_offset_, &warped_, &resolved_, &output_, &scene_state_, &lower_map_,
            &history_color_[0], &history_color_[1], &history_depth_[0], &history_depth_[1],
            &history_motion_[0], &history_motion_[1]})
            t->state = D3D12_RESOURCE_STATE_COMMON;
        pending_eye_ = 2;
    }
};
}  // namespace xrfg
