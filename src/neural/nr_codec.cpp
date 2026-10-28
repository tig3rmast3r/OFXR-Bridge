// Extracted NR composition from accepted OptiScaler V33034 (6c36e598).
// RenoDX/clshortfuse composition attribution and licence are in licenses/.
#include "nr_codec.hpp"
#include "DlssNr_Shader.h"
#include "NrResizeGuides.h"
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
namespace {
DXGI_FORMAT view_format(DXGI_FORMAT f) {
    switch(f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:return DXGI_FORMAT_R32_FLOAT;
    default:return f;
    }
}
bool equal_constants(const DlssNrConstants& a,const DlssNrConstants& b) {
    // Padding bytes are not shader data and must not choose a new binding.
    return a.Mode==b.Mode&&a.WhitePoint==b.WhitePoint&&a.Width==b.Width&&a.Height==b.Height&&
        a.TransferStrength==b.TransferStrength&&a.ColourStrength==b.ColourStrength&&
        a.DebugView==b.DebugView&&a.MaxRatio==b.MaxRatio&&a.Passthrough==b.Passthrough&&
        a.MvScaleX==b.MvScaleX&&a.MvScaleY==b.MvScaleY&&a.GuideWidth==b.GuideWidth&&
        a.GuideHeight==b.GuideHeight&&a.CompareMode==b.CompareMode&&a.CompareSplit==b.CompareSplit&&
        a.CompareZoom==b.CompareZoom&&a.CompareSwap==b.CompareSwap&&
        a.DepthX==b.DepthX&&a.DepthY==b.DepthY&&a.DepthWidth==b.DepthWidth&&a.DepthHeight==b.DepthHeight&&
        a.MotionX==b.MotionX&&a.MotionY==b.MotionY&&a.MotionWidth==b.MotionWidth&&a.MotionHeight==b.MotionHeight;
}
}
struct DlssNr_Dx12::DispatchBinding {
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12Resource> constants_buffer;
    std::array<ComPtr<ID3D12Resource>,5> srvs;
    std::array<ComPtr<ID3D12Resource>,2> uavs;
    DlssNrConstants constants{};
    uint64_t recording{};
    bool matches(const DlssNrConstants& value,ID3D12Resource* const(&reads)[5],
        ID3D12Resource* const(&writes)[2])const {
        if(!equal_constants(constants,value))return false;
        for(size_t i=0;i<5;++i)if(srvs[i].Get()!=reads[i])return false;
        for(size_t i=0;i<2;++i)if(uavs[i].Get()!=writes[i])return false;
        return true;
    }
};
DlssNr_Dx12::DlssNr_Dx12(std::string,ID3D12Device* device):_device(device) {
    if(!device)return;
    D3D12_DESCRIPTOR_RANGE ranges[3]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,5};
    ranges[2]={D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0,0,7};
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable={3,ranges};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root{1,&parameter,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,errors;
    if(FAILED(D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors))||
        FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            IID_PPV_ARGS(&_rootSignature))))return;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=_rootSignature.Get();
    pipeline.CS={DlssNr_cso,sizeof(DlssNr_cso)};
    _init=SUCCEEDED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&_pipelineState)));
    if(!_init)return;
    ComPtr<ID3DBlob> guides;
    if(FAILED(D3DCompile(kNrResizeGuidesShader,sizeof(kNrResizeGuidesShader)-1,"NR private guides",nullptr,nullptr,
        "main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&guides,&errors))){_init=false;return;}
    pipeline.CS={guides->GetBufferPointer(),guides->GetBufferSize()};
    _init=SUCCEEDED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&_guidePipelineState)));
}
DlssNr_Dx12::~DlssNr_Dx12()=default;
void DlssNr_Dx12::begin_recording(){std::scoped_lock lock(_bindingsMutex);++_recording;}
void DlssNr_Dx12::end_recording(){
    std::scoped_lock lock(_bindingsMutex);
    std::erase_if(_bindings,[&](const auto& binding){return binding->recording!=_recording;});
}
bool DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* list,const DlssNrConstants& constants,
    ID3D12Resource* source,ID3D12Resource* model,ID3D12Resource* original,ID3D12Resource* motion,
    ID3D12Resource* previous_edit,ID3D12Resource* target,ID3D12Resource* keep) {
    if(!_init||!list||!source||!target)return false;
    ID3D12Resource* const reads[5]={source,model?model:source,original?original:source,
        motion?motion:source,previous_edit?previous_edit:source};
    ID3D12Resource* const writes[2]={target,keep?keep:target};
    std::scoped_lock lock(_bindingsMutex);DispatchBinding* binding{};
    for(auto& candidate:_bindings)if(candidate->matches(constants,reads,writes)){binding=candidate.get();break;}
    if(!binding) {
        auto candidate=std::make_unique<DispatchBinding>();candidate->constants=constants;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=sizeof(DlssNrConstants);buffer.Height=1;buffer.DepthOrArraySize=1;
        buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        if(FAILED(_device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&candidate->constants_buffer))))return false;
        void* mapped{};D3D12_RANGE empty{};
        if(FAILED(candidate->constants_buffer->Map(0,&empty,&mapped)))return false;
        memcpy(mapped,&constants,sizeof(constants));candidate->constants_buffer->Unmap(0,nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=8;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(_device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&candidate->heap))))return false;
        const auto increment=_device->GetDescriptorHandleIncrementSize(heap.Type);
        auto slot=candidate->heap->GetCPUDescriptorHandleForHeapStart();
        for(size_t i=0;i<5;++i) {
            candidate->srvs[i]=reads[i];const auto desc=reads[i]->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||
                desc.SampleDesc.Count!=1||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=view_format(desc.Format);
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=desc.MipLevels;
            _device->CreateShaderResourceView(reads[i],&view,slot);slot.ptr+=increment;
        }
        for(size_t i=0;i<2;++i) {
            candidate->uavs[i]=writes[i];const auto desc=writes[i]->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||
                desc.SampleDesc.Count!=1||!(desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=view_format(desc.Format);
            view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            _device->CreateUnorderedAccessView(writes[i],nullptr,&view,slot);slot.ptr+=increment;
        }
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};cbv.BufferLocation=candidate->constants_buffer->GetGPUVirtualAddress();
        cbv.SizeInBytes=sizeof(DlssNrConstants);_device->CreateConstantBufferView(&cbv,slot);
        binding=candidate.get();_bindings.emplace_back(std::move(candidate));
    }
    ID3D12DescriptorHeap* heaps[]={binding->heap.Get()};list->SetDescriptorHeaps(1,heaps);
    binding->recording=_recording;
    list->SetComputeRootSignature(_rootSignature.Get());list->SetPipelineState(constants.Mode==DlssNrMode_ResizePrivateGuides?_guidePipelineState.Get():_pipelineState.Get());
    list->SetComputeRootDescriptorTable(0,binding->heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((constants.Width+7)/8,(constants.Height+7)/8,1);return true;
}
