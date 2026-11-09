#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include "rdr2_camera_phase_shader.hpp"
#include "xrfg/rdr2_camera_phase.hpp"
namespace xrfg {
class Rdr2CameraPhasePass {
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> output_;
    UINT increment_{},width_{},height_{};DXGI_FORMAT format_{};
public:
    ID3D12Resource* output(unsigned eye)const{return output_[eye].Get();}
    HRESULT prepare(ID3D12Device* device,UINT width,UINT height,DXGI_FORMAT format) {
        if(output_[0]&&output_[1]&&width_==width&&height_==height&&format_==format)return S_OK;
        output_={};
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2}};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={2,ranges};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[1].Constants={0,0,20};
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=2;signature.pParameters=parameters;
        Microsoft::WRL::ComPtr<ID3DBlob> blob,error;
        HRESULT hr=D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);if(FAILED(hr))return hr;
        root_.Reset();pipeline_.Reset();heap_.Reset();
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root_));if(FAILED(hr))return hr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=root_.Get();ps.CS={g_xrfg_rdr2_camera_phase_shader,sizeof(g_xrfg_rdr2_camera_phase_shader)};
        hr=device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline_));if(FAILED(hr))return hr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=6;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_));if(FAILED(hr))return hr;
        increment_=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for(auto& out:output_){hr=device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out));if(FAILED(hr))return hr;}
        width_=width;height_=height;format_=format;return S_OK;
    }
    void record(ID3D12Device* device,ID3D12GraphicsCommandList* list,ID3D12Resource* source,
        ID3D12Resource* scene,unsigned eye,const Rdr2Matrix& mapping) {
        auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(eye)*3*increment_;
        auto gpu=heap_->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(eye)*3*increment_;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format_;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(source,&srv,cpu);cpu.ptr+=increment_;srv.Format=DXGI_FORMAT_R32_UINT;
        device->CreateShaderResourceView(scene,&srv,cpu);cpu.ptr+=increment_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=format_;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output_[eye].Get(),nullptr,&uav,cpu);
        D3D12_RESOURCE_BARRIER bs[3]{};ID3D12Resource* resources[]{source,scene,output_[eye].Get()};
        for(unsigned i=0;i<3;++i){bs[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;bs[i].Transition={resources[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COMMON,i==2?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};}
        list->ResourceBarrier(3,bs);ID3D12DescriptorHeap* heaps[]{heap_.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(root_.Get());list->SetPipelineState(pipeline_.Get());list->SetComputeRootDescriptorTable(0,gpu);
        list->SetComputeRoot32BitConstants(1,16,mapping.data(),0);const UINT size[]{width_,height_,0,0};list->SetComputeRoot32BitConstants(1,4,size,16);
        list->Dispatch((width_+7)/8,(height_+7)/8,1);
        for(auto& b:bs)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(3,bs);
    }
};
}
