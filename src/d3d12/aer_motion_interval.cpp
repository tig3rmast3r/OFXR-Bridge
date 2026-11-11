#include "xrfg/aer_motion_interval.hpp"
#include "aer_motion_interval_shader.hpp"
#include <algorithm>
#include <mutex>
#include <vector>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
struct Parameters {
    std::array<float,4> current{},middle{},previous{};
    float size[2]{},scale[2]{},jitter[2]{};UINT enabled{},slice{};
};
static_assert(sizeof(Parameters)==80);
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return v;
}
DXGI_FORMAT readable(DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R8G8B8A8_TYPELESS)return DXGI_FORMAT_R8G8B8A8_UNORM;
    if(f==DXGI_FORMAT_B8G8R8A8_TYPELESS)return DXGI_FORMAT_B8G8R8A8_UNORM;
    if(f==DXGI_FORMAT_R16G16_TYPELESS)return DXGI_FORMAT_R16G16_FLOAT;
    if(f==DXGI_FORMAT_R32G32_TYPELESS)return DXGI_FORMAT_R32G32_FLOAT;
    return f;
}
}
struct AerMotionInterval::Impl {
    struct History {ComPtr<ID3D12Resource> color;std::uint32_t token{};
        std::array<float,4> rect{};Result result;};
    std::array<History,2> history;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> heap;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Resource> decision,readback;
    ComPtr<ID3D12Fence> fence;std::uint64_t value{};bool copy_state{},pending{};
    HANDLE event{};unsigned pending_eye{};std::vector<ComPtr<ID3D12Resource>> active;
    std::shared_ptr<const DlssMotionVectorFrame> active_guide;
    ~Impl(){if(event)CloseHandle(event);}
    HRESULT finish() {
        if(!pending)return S_OK;
        if(fence->GetCompletedValue()==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(fence->GetCompletedValue()<value) {
            auto hr=fence->SetEventOnCompletion(value,event);if(FAILED(hr))return hr;
            if(WaitForSingleObject(event,32)!=WAIT_OBJECT_0)return DXGI_ERROR_WAS_STILL_DRAWING;
        }
        void* data{};D3D12_RANGE range{0,12};auto hr=readback->Map(0,&range,&data);if(FAILED(hr))return hr;
        const auto* words=static_cast<const UINT*>(data);auto& r=history[pending_eye].result;
        r.interval=words[0];r.one=words[1];r.two=words[2];D3D12_RANGE empty{};readback->Unmap(0,&empty);
        pending=false;active.clear();active_guide.reset();return S_OK;
    }
    HRESULT initialize(ID3D12Device* d,ID3D12CommandQueue* q) {
        device=d;queue=q;
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,4,0,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0}};
        D3D12_ROOT_PARAMETER args[3]{};
        for(unsigned i=0;i<2;++i){args[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;args[i].DescriptorTable={1,&ranges[i]};}
        args[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;args[2].Constants={0,0,sizeof(Parameters)/4};
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC desc{3,args,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};ComPtr<ID3DBlob> blob,error;
        auto hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr))return hr;
        hr=d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));if(FAILED(hr))return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root.Get();
        ps.CS={g_xrfg_aer_motion_interval_shader,sizeof(g_xrfg_aer_motion_interval_shader)};
        hr=d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline));if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,5,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        hr=d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap));if(FAILED(hr))return hr;
        hr=d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));if(FAILED(hr))return hr;
        hr=d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list));if(FAILED(hr))return hr;
        hr=list->Close();if(FAILED(hr))return hr;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=12;
        buffer.Height=1;buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        hr=d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&decision));if(FAILED(hr))return hr;
        buffer.Flags=D3D12_RESOURCE_FLAG_NONE;hp.Type=D3D12_HEAP_TYPE_READBACK;
        hr=d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback));if(FAILED(hr))return hr;
        hr=d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));if(FAILED(hr))return hr;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);return event?S_OK:HRESULT_FROM_WIN32(GetLastError());
    }
};
AerMotionInterval::AerMotionInterval()=default;
AerMotionInterval::~AerMotionInterval() {
    // A timed-out submission still owns its allocator, descriptors and inputs.
    // Pin it on teardown rather than releasing resources under the GPU.
    if(impl_&&impl_->pending&&impl_->fence->GetCompletedValue()<impl_->value) {
        static std::mutex mutex;static auto* retained=new std::vector<std::shared_ptr<Impl>>;
        std::scoped_lock lock(mutex);retained->push_back(impl_);
    }
}
HRESULT AerMotionInterval::observe(ID3D12Device* d,ID3D12CommandQueue* q,unsigned eye,ID3D12Resource* color,
    const DlssMotionVectorFrame& original,const DlssMotionVectorFrame& placed,Result& output,
    ID3D12Fence** submitted,std::uint64_t* submitted_value) noexcept {
    output={};if(submitted)*submitted=nullptr;if(submitted_value)*submitted_value=0;
    try {
        if(!d||!q||eye>1||!color||!original.motion_vectors||!original.color_origin_token)return E_INVALIDARG;
        if(!impl_){auto p=std::make_shared<Impl>();auto hr=p->initialize(d,q);if(FAILED(hr))return hr;impl_=std::move(p);}
        auto& p=*impl_;if(p.device.Get()!=d||p.queue.Get()!=q)return E_INVALIDARG;
        auto hr=p.finish();if(FAILED(hr))return hr;
        auto& own=p.history[eye];auto& peer=p.history[eye^1];
        if(own.token==original.color_origin_token){output=own.result;return S_OK;}
        const auto cd=color->GetDesc();
        if(cd.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||cd.DepthOrArraySize!=1||cd.SampleDesc.Count!=1)return E_INVALIDARG;
        Parameters params{};const auto rect=guide_motion_rect(placed);
        params.size[0]=float(original.motion_width);params.size[1]=float(original.motion_height);
        if(!params.size[0]||!params.size[1])return E_INVALIDARG;
        params.current={rect[0]/params.size[0],rect[1]/params.size[1],rect[2]/params.size[0],rect[3]/params.size[1]};
        params.middle=peer.rect;params.previous=own.rect;
        params.enabled=own.color&&peer.color&&original.previous_same_guide&&original.previous_opposite_guide&&
            own.token==original.previous_same_guide->color_origin_token&&peer.token==original.previous_opposite_guide->color_origin_token;
        params.scale[0]=original.scale_x/params.size[0];params.scale[1]=original.scale_y/params.size[1];
        if(original.jittered){params.jitter[0]=(original.jitter_x-original.previous_jitter_x)/params.size[0];
            params.jitter[1]=(original.jitter_y-original.previous_jitter_y)/params.size[1];}
        params.slice=original.motion_slice;
        if(own.color){const auto old=own.color->GetDesc();if(old.Width!=cd.Width||old.Height!=cd.Height||old.Format!=cd.Format){own={};params.enabled=0;}}
        if(!own.color){auto copy=cd;copy.Flags=D3D12_RESOURCE_FLAG_NONE;copy.MipLevels=1;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            hr=d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&copy,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&own.color));if(FAILED(hr))return hr;}
        hr=p.allocator->Reset();if(FAILED(hr))return hr;hr=p.list->Reset(p.allocator.Get(),p.pipeline.Get());if(FAILED(hr))return hr;
        p.active={color,original.motion_vectors.Get(),own.color?own.color.Get():color,peer.color?peer.color.Get():color};
        p.active_guide=std::make_shared<DlssMotionVectorFrame>(original);
        const auto increment=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=p.heap->GetCPUDescriptorHandleForHeapStart();
        const std::array<ID3D12Resource*,4> inputs{color,peer.color?peer.color.Get():color,own.color?own.color.Get():color,original.motion_vectors.Get()};
        for(unsigned i=0;i<4;++i){D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=readable(inputs[i]->GetDesc().Format);
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension=i==3?D3D12_SRV_DIMENSION_TEXTURE2DARRAY:D3D12_SRV_DIMENSION_TEXTURE2D;
            if(i==3){srv.Texture2DArray.MipLevels=1;srv.Texture2DArray.ArraySize=inputs[i]->GetDesc().DepthOrArraySize;}
            else srv.Texture2D.MipLevels=1;d->CreateShaderResourceView(inputs[i],&srv,cpu);cpu.ptr+=increment;}
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_TYPELESS;uav.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements=3;uav.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;d->CreateUnorderedAccessView(p.decision.Get(),nullptr,&uav,cpu);
        auto b=barrier(color,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);p.list->ResourceBarrier(1,&b);
        if(p.copy_state){auto v=barrier(p.decision.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);p.list->ResourceBarrier(1,&v);}
        ID3D12DescriptorHeap* heaps[]{p.heap.Get()};p.list->SetDescriptorHeaps(1,heaps);p.list->SetComputeRootSignature(p.root.Get());
        auto gpu=p.heap->GetGPUDescriptorHandleForHeapStart();p.list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=4*increment;p.list->SetComputeRootDescriptorTable(1,gpu);
        p.list->SetComputeRoot32BitConstants(2,sizeof(params)/4,&params,0);p.list->Dispatch(1,1,1);
        auto v=barrier(p.decision.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);p.list->ResourceBarrier(1,&v);
        p.list->CopyBufferRegion(p.readback.Get(),0,p.decision.Get(),0,12);
        std::array<D3D12_RESOURCE_BARRIER,2> copies{barrier(color,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE),
            barrier(own.color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST)};
        p.list->ResourceBarrier(2,copies.data());
        D3D12_TEXTURE_COPY_LOCATION destination{},source{};
        destination.pResource=own.color.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.pResource=color;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        p.list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        copies={barrier(color,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON),barrier(own.color.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        p.list->ResourceBarrier(2,copies.data());hr=p.list->Close();if(FAILED(hr))return hr;
        ID3D12CommandList* lists[]{p.list.Get()};q->ExecuteCommandLists(1,lists);++p.value;p.pending=true;p.pending_eye=eye;p.copy_state=true;
        if(submitted)p.fence.CopyTo(submitted);if(submitted_value)*submitted_value=p.value;
        hr=q->Signal(p.fence.Get(),p.value);if(FAILED(hr))return hr;
        const auto middle_interval=params.enabled?peer.result.interval:0;
        own.token=original.color_origin_token;own.rect=params.current;own.result={0,middle_interval,0,0};p.pending=true;p.pending_eye=eye;
        hr=p.finish();if(FAILED(hr))return hr;output=own.result;return S_OK;
    }catch(...){return E_FAIL;}
}
}
