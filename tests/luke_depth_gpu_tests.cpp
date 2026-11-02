#include "xrfg/luke_depth_format.hpp"
#include "xrfg/luke_recording_scope.hpp"
#include "luke_depth_convert_shader.hpp"
#include "luke_depth_calibrate_shader.hpp"
#include "luke_compose_motion_shader.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "xrfg/luke_afw_continuity.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using Microsoft::WRL::ComPtr;
void check(HRESULT h) { if (FAILED(h)) throw std::runtime_error("HRESULT " + std::to_string(h)); }
void expect(bool b) { if (!b) throw std::runtime_error("depth contract failed"); }
D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; return x;
}
int main() { try {
    expect(xrfg::luke_afw_effective_mode(3,785,783,0,0,783)==1);
    expect(xrfg::luke_afw_effective_mode(3,804,802,1,1,802)==1);
    expect(xrfg::luke_afw_effective_mode(3,815,813,0,0,813)==1);
    expect(xrfg::luke_afw_effective_mode(3,1253,1251,0,0,1251)==1);
    expect(xrfg::luke_afw_effective_mode(3,1268,1266,1,1,1266)==1);
    expect(xrfg::luke_afw_effective_mode(3,1279,1277,0,0,1277)==1);
    expect(xrfg::luke_afw_effective_mode(3,1301,1299,0,0,1299)==1);
    expect(xrfg::luke_afw_effective_mode(3,786,785,1,0,784)==3);
    expect(xrfg::luke_afw_effective_mode(3,10,9,0,0,8)==1);
    expect(xrfg::luke_afw_effective_mode(2,10,8,0,1,8)==1);
    expect(xrfg::luke_afw_effective_mode(1,10,8,0,0,8)==1);
    // V633 Horizon F8: source 1839 was missed. Re-prime 1840/eye0,
    // then 1841/eye1 still has only its old 1837 color, not required 1839.
    expect(xrfg::luke_afw_effective_mode(3,1840,1838,0,0,1838)==1);
    expect(xrfg::luke_afw_effective_mode(3,1841,1840,1,0,1837)==1);
    expect(xrfg::luke_afw_effective_mode(3,1842,1841,0,1,1840)==3);
    expect(xrfg::luke_afw_effective_mode(2,1841,1840,1,0,1837)==1);
    expect(xrfg::luke_afw_effective_mode(3,2,1,1,0,0)==1);
    expect(xrfg::luke_afw_effective_mode(3,3,2,0,1,1)==3);
    expect(xrfg::luke_afw_previous_opposite(1841,1840,1,0));
    expect(xrfg::luke_afw_effective_mode(3,1841,1840,1,0,1837,true)==3);
    expect(xrfg::luke_afw_effective_mode(3,1840,1838,0,0,1838,true)==1);
    ComPtr<ID3D12Debug> debug; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
    int aborted=0;
    for(int i=0;i<2;++i) { check(allocator->Reset()); check(list->Reset(allocator.Get(),nullptr));
        xrfg::LukeRecordingScope scope(list.Get(),[&](HRESULT h) noexcept { if(SUCCEEDED(h)) ++aborted; });
    }
    expect(aborted==2); // Repeated pre-submit failure cannot leave the list open.
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr);UINT64 value=0;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;
        d.DepthOrArraySize=d.MipLevels=1;d.Format=f;d.SampleDesc.Count=1;d.Flags=flags;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
    for(UINT i=0;i<2;++i) { ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[i].NumDescriptors=i?2:3; }
    std::array<D3D12_ROOT_PARAMETER,3> params{};
    for(UINT i=0;i<2;++i) { params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i].DescriptorTable={1,&ranges[i]}; }
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[2].Constants.Num32BitValues=52;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params.data();
    ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={g_xrfg_luke_depth_convert_shader,sizeof(g_xrfg_luke_depth_convert_shader)};
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    pd.CS={g_xrfg_luke_depth_calibrate_shader,sizeof(g_xrfg_luke_depth_calibrate_shader)};
    ComPtr<ID3D12PipelineState> calibration_pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&calibration_pipeline)));
    for(auto format:{DXGI_FORMAT_D24_UNORM_S8_UINT,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,DXGI_FORMAT_D32_FLOAT,DXGI_FORMAT_D16_UNORM}) {
      const auto typed=xrfg::luke_depth_format(format);expect(typed.resource!=DXGI_FORMAT_UNKNOWN);
      for(UINT dilate=0;dilate<3;++dilate) {
        auto source=texture(8,4,format,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE);
        auto staging=texture(8,4,typed.resource,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto output=texture(3,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto remap=texture(1,2,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;hd.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> dsv;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&dsv)));
        device->CreateDepthStencilView(source.Get(),nullptr,dsv->GetCPUDescriptorHandleForHeapStart());
        hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        auto cpu=views->GetCPUDescriptorHandleForHeapStart();const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=typed.srv;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(staging.Get(),&srv,cpu);cpu.ptr+=stride;
        auto null_srv=srv;null_srv.Format=DXGI_FORMAT_R32G32_FLOAT;
        device->CreateShaderResourceView(nullptr,&null_srv,cpu);cpu.ptr+=stride;
        device->CreateShaderResourceView(nullptr,&null_srv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        cpu.ptr+=stride;uav.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(remap.Get(),nullptr,&uav,cpu);
        const auto od=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;device->GetCopyableFootprints(&od,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto readheap=heap;readheap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&readheap,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,0.25F,0,0,nullptr);
        D3D12_RECT rect{3,1,6,3};list->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,0.75F,0,1,&rect);
        auto b=barrier(source.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=staging.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        b=barrier(staging.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=3ULL*stride;list->SetComputeRootDescriptorTable(1,gpu);
        UINT constants[52]{1,dilate,3,1};constants[15]=1;list->SetComputeRoot32BitConstants(2,52,constants,0);
        list->SetPipelineState(calibration_pipeline.Get());list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER calibration_barrier{};calibration_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        calibration_barrier.UAV.pResource=remap.Get();list->ResourceBarrier(1,&calibration_barrier);
        list->SetPipelineState(pipeline.Get());list->Dispatch(1,1,1);
        b=barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        from.pResource=output.Get();to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* mapped=nullptr;check(readback->Map(0,nullptr,&mapped));
        for(UINT y=0;y<2;++y)for(UINT x=0;x<3;++x) { float v;std::memcpy(&v,static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4,4);expect(std::abs(v-0.75F)<0.0001F); }
        readback->Unmap(0,nullptr);
      }
    }
    // Fresh calibration must independently fit both native camera motion and
    // the full 2-D three-camera difference. Object-only velocity is NOT a seed.
    // A coherent occluded prior cannot change current-camera-consistent world.
    for(float sign:{-1.0F,1.0F}) for(float velocity:{0.0F,0.02F}) for(float bias:{0.0F,0.2F,0.205F}) for(float prior_camera_step:{0.004F,-0.002F}) for(float crop_delta:{0.0F,0.01F}) for(bool occluded_prior:{false,true}) {
        if(occluded_prior && (bias!=0.0F || velocity!=0.0F)) continue;
        constexpr UINT width=64,height=16;
        const float true_depth=bias?0.1F:0.01F, camera_step=0.004F;
        std::array<ComPtr<ID3D12Resource>,3> inputs{
            texture(width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST),
            texture(width,height,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST),
            texture(width,height,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST)};
        auto output=texture(width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto remap=texture(1,2,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::array<ComPtr<ID3D12Resource>,3> uploads;
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        for(UINT slot=0;slot<3;++slot) {
            const auto desc=inputs[slot]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
            device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
            bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            auto uh=heap;uh.Type=D3D12_HEAP_TYPE_UPLOAD;
            check(device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[slot])));
            void* mapped{};check(uploads[slot]->Map(0,nullptr,&mapped));
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
                // Prior contamination alone must not alter native-consistent
                // world. The separate native camera fit rejects moving seeds.
                const float prior_contamination=slot==2 && occluded_prior ? -sign*0.027F : 0.0F;
                const float values[]{slot ? (slot==1?sign:-sign)*0.45F*true_depth+(slot==1?camera_step-crop_delta:prior_camera_step+crop_delta)+velocity+prior_contamination : true_depth+bias,0};
                std::memcpy(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch+x*(slot?8:4),values,slot?8:4);
            }
            uploads[slot]->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
            to.pResource=inputs[slot].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            auto b=barrier(inputs[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        auto cpu=views->GetCPUDescriptorHandleForHeapStart();const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
        for(UINT slot=0;slot<3;++slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=inputs[slot]->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(inputs[slot].Get(),&srv,cpu);cpu.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        cpu.ptr+=stride;uav.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
        device->CreateUnorderedAccessView(remap.Get(),nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=3ULL*stride;list->SetComputeRootDescriptorTable(1,gpu);
        std::array<float,52> constants{};
        constants[8]=constants[10]=constants[12]=static_cast<float>(width);
        constants[9]=constants[11]=constants[13]=static_cast<float>(height);
        const UINT enabled=1;std::memcpy(&constants[14],&enabled,4);std::memcpy(&constants[15],&enabled,4);
        constants[16]=crop_delta*width;
        for(UINT offset:{20U,36U}) for(UINT diagonal:{0U,5U,10U,15U}) constants[offset+diagonal]=1;
        constants[28]=sign*0.9F;constants[32]=camera_step*2;constants[48]=(camera_step+prior_camera_step)*2;
        list->SetComputeRoot32BitConstants(2,52,constants.data(),0);
        list->SetPipelineState(calibration_pipeline.Get());list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER calibration_barrier{};calibration_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        calibration_barrier.UAV.pResource=remap.Get();list->ResourceBarrier(1,&calibration_barrier);
        list->SetPipelineState(pipeline.Get());list->Dispatch(width/8,height/8,1);
        auto b=barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        const auto desc=output->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;
        bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;auto rh=heap;rh.Type=D3D12_HEAP_TYPE_READBACK;
        ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=output.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));
        check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* data{};check(readback->Map(0,nullptr,&data));
        for(UINT y=4;y<height-4;++y)for(UINT x=10;x<width-10;++x) {
            float actual{};std::memcpy(&actual,static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*4,4);
            const float expected=velocity!=0 ? true_depth+bias : true_depth;
            expect(std::abs(actual-expected)<0.0001F);
        }
        readback->Unmap(0,nullptr);
    }
    // Persistent GPU calibration across distinct publications. The test reads
    // both the remap state and untouched native motion, not a CPU substitute.
    struct DepthFrameResult {
        std::vector<float> depth, motion;
        std::array<float,4> calibration{};
    };
    auto depth_frame=[&](UINT width,UINT height,const std::array<std::vector<float>,3>& fields,
                        const std::array<float,52>& constants,ComPtr<ID3D12Resource>& remap) {
        std::array<ComPtr<ID3D12Resource>,3> inputs{
            texture(width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST),
            texture(width,height,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST),
            texture(width,height,DXGI_FORMAT_R32G32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST)};
        auto output=texture(width,height,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if(!remap) remap=texture(1,2,DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        std::array<ComPtr<ID3D12Resource>,3> uploads;
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        for(UINT slot=0;slot<3;++slot) {
            const auto desc=inputs[slot]->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
            device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
            bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            auto uh=heap;uh.Type=D3D12_HEAP_TYPE_UPLOAD;
            check(device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[slot])));
            void* mapped{};check(uploads[slot]->Map(0,nullptr,&mapped));
            const UINT channels=slot?2:1;expect(fields[slot].size()==static_cast<size_t>(width)*height*channels);
            for(UINT y=0;y<height;++y)
                std::memcpy(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,
                            fields[slot].data()+static_cast<size_t>(y)*width*channels,width*channels*sizeof(float));
            uploads[slot]->Unmap(0,nullptr);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
            to.pResource=inputs[slot].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            auto b=barrier(inputs[slot].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=views->GetCPUDescriptorHandleForHeapStart();
        for(UINT slot=0;slot<3;++slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=inputs[slot]->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(inputs[slot].Get(),&srv,cpu);cpu.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);cpu.ptr+=stride;
        uav.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;device->CreateUnorderedAccessView(remap.Get(),nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=3ULL*stride;list->SetComputeRootDescriptorTable(1,gpu);
        list->SetComputeRoot32BitConstants(2,52,constants.data(),0);list->SetPipelineState(calibration_pipeline.Get());list->Dispatch(1,1,1);
        D3D12_RESOURCE_BARRIER calibration_barrier{};calibration_barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        calibration_barrier.UAV.pResource=remap.Get();list->ResourceBarrier(1,&calibration_barrier);
        list->SetPipelineState(pipeline.Get());list->Dispatch((width+7)/8,(height+7)/8,1);
        std::array<ID3D12Resource*,3> results{output.Get(),remap.Get(),inputs[1].Get()};
        std::array<ComPtr<ID3D12Resource>,3> readbacks;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,3> footprints{};
        for(UINT slot=0;slot<3;++slot) {
            const auto before=slot==2?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            auto b=barrier(results[slot],before,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
            const auto desc=results[slot]->GetDesc();UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&footprints[slot],nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
            bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            auto rh=heap;rh.Type=D3D12_HEAP_TYPE_READBACK;
            check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readbacks[slot])));
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=results[slot];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readbacks[slot].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprints[slot];list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            if(slot==1) { b=barrier(remap.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);list->ResourceBarrier(1,&b); }
        }
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));
        check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        DepthFrameResult result;result.depth.resize(static_cast<size_t>(width)*height);result.motion.resize(static_cast<size_t>(width)*height*2);
        for(UINT slot=0;slot<3;++slot) {
            void* mapped{};check(readbacks[slot]->Map(0,nullptr,&mapped));const auto& fp=footprints[slot];
            if(slot==1) std::memcpy(result.calibration.data(),static_cast<char*>(mapped)+fp.Offset,4*sizeof(float));
            else {
                const UINT channels=slot==2?2:1;auto& target=slot==2?result.motion:result.depth;
                for(UINT y=0;y<height;++y)
                    std::memcpy(target.data()+static_cast<size_t>(y)*width*channels,
                                static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,width*channels*sizeof(float));
            }
            readbacks[slot]->Unmap(0,nullptr);
        }
        expect(result.motion==fields[1]); // Calibration never mutates model MVs.
        return result;
    };
    for(float sign:{-1.0F,1.0F}) {
        constexpr UINT width=64,height=64;
        constexpr float bias=.205F,physical=.1F,camera_step=.004F,prior_camera_step=-.002F;
        ComPtr<ID3D12Resource> remap;
        std::array<float,52> constants{};
        constants[8]=constants[10]=constants[12]=width;constants[9]=constants[11]=constants[13]=height;
        const UINT enabled=1,disabled=0;std::memcpy(&constants[14],&enabled,4);std::memcpy(&constants[15],&enabled,4);
        for(UINT offset:{20U,36U})for(UINT diagonal:{0U,5U,10U,15U})constants[offset+diagonal]=1;
        constants[28]=sign*.9F;constants[32]=camera_step*2;constants[48]=(camera_step+prior_camera_step)*2;
        auto fields=[&](float true_depth,float velocity,float camera_y,float prior_y_error) {
            std::array<std::vector<float>,3> data;
            data[0].resize(width*height,true_depth+bias);data[1].resize(width*height*2);data[2].resize(width*height*2);
            for(UINT pixel=0;pixel<width*height;++pixel) {
                data[1][2*pixel]=sign*.45F*true_depth+camera_step+velocity;
                data[1][2*pixel+1]=camera_y;
                data[2][2*pixel]=-sign*.45F*true_depth+prior_camera_step+velocity;
                data[2][2*pixel+1]=prior_y_error;
            }
            return data;
        };
        auto expect_uniform=[](const std::vector<float>& depths,float expected) {
            for(float depth:depths)expect(std::abs(depth-expected)<.0001F);
        };
        auto data=fields(physical,0,0,0);
        auto result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[3]>=32);expect(std::abs(result.calibration[0]-bias)<.0001F);
        expect_uniform(result.depth,physical);const float learned_bias=result.calibration[0];
        std::memcpy(&constants[15],&disabled,4);
        // Head rotation's current camera is exact, but orthogonal contamination
        // in the prior defeats the full-2D fit. Hold measured depth, no Y slack.
        constexpr float camera_y=.04F;
        constants[33]=-2*camera_y;constants[49]=-2*camera_y;
        data=fields(physical,0,camera_y,.05F);
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[0]==learned_bias);expect_uniform(result.depth,physical);
        // Genuine object movement must survive; it cannot rewrite calibration.
        data=fields(physical,.02F,camera_y,.05F);
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[0]==learned_bias);expect_uniform(result.depth,physical);
        // A newly observed layer depth expands measured coverage only through
        // an independent current native-camera fit, still with the bad prior.
        data=fields(.2F,0,camera_y,.05F);
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[0]==learned_bias);expect(result.calibration[2]>=.405F-.0001F);
        expect_uniform(result.depth,.2F);
        data=fields(physical,0,camera_y,.05F);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
            const UINT pixel=y*width+x;
            if(x<width/4) {
                // Same stored depth as the biased layer, but native-consistent
                // world pixels must remain untouched by the learned offset.
                data[1][2*pixel]=sign*.45F*(physical+bias)+camera_step;
                data[2][2*pixel]=-sign*.45F*(physical+bias)+prior_camera_step;
            } else if(x>=3*width/4) data[0][pixel]=.705F; // Unsupported domain; no extrapolation.
        }
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[0]==learned_bias);expect(result.calibration[2]<.5F);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
            const float expected=x<width/4?physical+bias:x>=3*width/4?.705F:physical;
            expect(std::abs(result.depth[y*width+x]-expected)<.0001F);
        }
        // Epoch/projection reset physically discards a previously valid model.
        // With incompatible prior Y there is no matching fresh calibration.
        std::memcpy(&constants[15],&enabled,4);data=fields(physical,0,camera_y,.05F);
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[3]<32);expect_uniform(result.depth,physical+bias);
        // No model may be learned merely because object velocity cancels in
        // current-minus-prior: the independent current camera fit must pass.
        constants[33]=constants[49]=0;data=fields(physical,.02F,0,0);
        result=depth_frame(width,height,data,constants,remap);
        expect(result.calibration[3]<32);expect_uniform(result.depth,physical+bias);
    }
    // Execute the restored production composition for float2/float4 in both
    // precisions, poison ZW, and test stereo removal plus temporal camera step.
    const auto half = [](float v) -> std::uint16_t {
        std::uint32_t bits{}; std::memcpy(&bits,&v,4);
        const unsigned sign=(bits>>16)&0x8000,mantissa=bits&0x7fffff;
        const int exponent=int((bits>>23)&255)-127+15;
        if(exponent>=31)return static_cast<std::uint16_t>(sign|0x7c00|(mantissa?0x200:0));
        if(exponent<=0) {
            if(exponent<-10)return static_cast<std::uint16_t>(sign);
            return static_cast<std::uint16_t>(sign|((mantissa|0x800000)>>(14-exponent)));
        }
        return static_cast<std::uint16_t>(sign|(exponent<<10)|(mantissa>>13));
    };
    const auto unhalf = [](std::uint16_t v) {
        const unsigned e=(v>>10)&31, m=v&1023;
        const float result=e?std::ldexp(1.0F+m/1024.0F,static_cast<int>(e)-15):std::ldexp(m/1024.0F,-14);
        return (v&0x8000)?-result:result;
    };
#include "luke_native_motion_gpu.inc"
    std::array<D3D12_DESCRIPTOR_RANGE,2> motion_ranges{};
    std::array<D3D12_ROOT_PARAMETER,3> motion_params{};
    for(UINT i=0;i<2;++i) {
        motion_ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        motion_ranges[i].NumDescriptors=i?1:4;
        motion_params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        motion_params[i].DescriptorTable={1,&motion_ranges[i]};
    }
    motion_params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    motion_params[2].Constants.Num32BitValues=60;
    D3D12_ROOT_SIGNATURE_DESC motion_rd{};motion_rd.NumParameters=3;motion_rd.pParameters=motion_params.data();
    ComPtr<ID3DBlob> motion_blob;check(D3D12SerializeRootSignature(&motion_rd,D3D_ROOT_SIGNATURE_VERSION_1,&motion_blob,&errors));
    ComPtr<ID3D12RootSignature> motion_root;check(device->CreateRootSignature(0,motion_blob->GetBufferPointer(),motion_blob->GetBufferSize(),IID_PPV_ARGS(&motion_root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC motion_pd{};motion_pd.pRootSignature=motion_root.Get();
    motion_pd.CS={g_xrfg_luke_compose_motion_shader,sizeof(g_xrfg_luke_compose_motion_shader)};
    ComPtr<ID3D12PipelineState> motion_pipeline;check(device->CreateComputePipelineState(&motion_pd,IID_PPV_ARGS(&motion_pipeline)));
    for(auto format:{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R32G32_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT}) {
      expect(xrfg::luke_motion_srv_format(format)==format);
      for(float camera_step:{0.0F,0.25F}) {
       for(float stereo_step:{0.0F,0.5F}) {
        auto input=texture(6,4,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto motion_depth=texture(2,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto composed=texture(2,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto afw_motion=texture(2,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        std::array<ComPtr<ID3D12Resource>,2> uploads;
        for(UINT slot=0;slot<2;++slot) {
          ID3D12Resource* target=slot?motion_depth.Get():input.Get();const auto desc=target->GetDesc();
          D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
          D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;
          bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
          auto uh=heap;uh.Type=D3D12_HEAP_TYPE_UPLOAD;
          check(device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[slot])));
          void* data{};check(uploads[slot]->Map(0,nullptr,&data));std::memset(data,0,static_cast<size_t>(bytes));
          for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
            auto* row=static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch;
            if(slot) { const float depth_value=0.5F;std::memcpy(row+x*4,&depth_value,4); }
            else if(format==DXGI_FORMAT_R16G16_SNORM) {
              const std::int16_t values[]{static_cast<std::int16_t>(std::lround(x*.125F*32767.F)),
                  static_cast<std::int16_t>(std::lround(-static_cast<float>(y)*.25F*32767.F))};
              std::memcpy(row+x*4,values,4);
            }
            else if(format==DXGI_FORMAT_R32G32_FLOAT || format==DXGI_FORMAT_R32G32B32A32_FLOAT) {
              const float values[]{x*0.25F,-static_cast<float>(y)*0.5F,123.0F,-91.0F};
              const UINT pixel_bytes=format==DXGI_FORMAT_R32G32_FLOAT?8:16;
              std::memcpy(row+x*pixel_bytes,values,pixel_bytes);
            } else {
              const std::uint16_t values[]{half(x*0.25F),half(-static_cast<float>(y)*0.5F),half(123.0F),half(-91.0F)};
              const UINT pixel_bytes=format==DXGI_FORMAT_R16G16_FLOAT?4:8;
              std::memcpy(row+x*pixel_bytes,values,pixel_bytes);
            }
          }
          uploads[slot]->Unmap(0,nullptr);
          D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;
          to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
          auto b=barrier(target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=views->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=xrfg::luke_motion_srv_format(format);srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(input.Get(),&srv,cpu);cpu.ptr+=stride;srv.Format=DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(motion_depth.Get(),&srv,cpu);cpu.ptr+=stride;
        srv.Format=format;device->CreateShaderResourceView(input.Get(),&srv,cpu);cpu.ptr+=stride;
        srv.Format=DXGI_FORMAT_R32_FLOAT;device->CreateShaderResourceView(motion_depth.Get(),&srv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=xrfg::kLukeAfwMotionFormat;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(composed.Get(),nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(motion_root.Get());list->SetPipelineState(motion_pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=4*stride;list->SetComputeRootDescriptorTable(1,gpu);
        xrfg::LukeNativeMotionConstants constants{};
        constants.source_base={2,1};constants.crop={1,0};
        constants.extent={6,4};constants.scale={1,1};
        std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        const auto to_window=xrfg::native_motion_detail::guide_to_window(6,4,1,0,2,2);
        constants.opposite=xrfg::native_motion_detail::window_to_guide_transform(identity,to_window);
        auto previous=identity;previous[12]=camera_step;
        constants.same=xrfg::native_motion_detail::window_to_guide_transform(previous,to_window);
        auto stereo=identity;stereo[8]=stereo_step;
        constants.stereo=xrfg::native_motion_detail::window_to_guide_transform(stereo,to_window);
        list->SetComputeRoot32BitConstants(2,60,&constants,0);list->Dispatch(1,1,1);
        auto b=barrier(composed.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        list->CopyResource(afw_motion.Get(),composed.Get());b=barrier(afw_motion.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        const auto desc=afw_motion->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto rh=heap;rh.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=afw_motion.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* data{};check(readback->Map(0,nullptr,&data));
        for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x) {
          std::uint16_t values[2];std::memcpy(values,static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*4,4);
          const bool snorm=format==DXGI_FORMAT_R16G16_SNORM;
          expect(std::abs(unhalf(values[0])-((x+3)*(snorm?.125F:.25F)+camera_step-0.5F*stereo_step))<0.001F);
          expect(std::abs(unhalf(values[1])+((y+1)*(snorm?.25F:.5F)))<0.001F);
        }
        readback->Unmap(0,nullptr);
       }
      }
    }
    for(auto format:{DXGI_FORMAT_R16G16_TYPELESS,DXGI_FORMAT_R32G32_TYPELESS,DXGI_FORMAT_R16G16_UNORM,DXGI_FORMAT_R16G16_SINT,DXGI_FORMAT_R32_FLOAT,
                    DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R32G32B32A32_TYPELESS,DXGI_FORMAT_R16G16B16A16_UNORM,DXGI_FORMAT_R16G16B16A16_SINT})
      expect(xrfg::luke_motion_srv_format(format)==DXGI_FORMAT_UNKNOWN);
    expect(xrfg::dlss_motion_snapshot_format(DXGI_FORMAT_R16G16_TYPELESS,true)==DXGI_FORMAT_R16G16_FLOAT);
    for(auto format:{DXGI_FORMAT_R16G16_TYPELESS,DXGI_FORMAT_R32G32_TYPELESS,DXGI_FORMAT_R16G16_UNORM,
                    DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R16G16_SINT,DXGI_FORMAT_R16G16_UINT,
                    DXGI_FORMAT_R16G16B16A16_TYPELESS,DXGI_FORMAT_R16G16_FLOAT}) {
      expect(xrfg::dlss_motion_snapshot_format(format,false)==format);
      if(format!=DXGI_FORMAT_R16G16_TYPELESS)
        expect(xrfg::dlss_motion_snapshot_format(format,true)==format);
    }
    expect(xrfg::luke_depth_format(DXGI_FORMAT_R8G8B8A8_UNORM).resource==DXGI_FORMAT_UNKNOWN);
    if(info) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) { SIZE_T n=0;check(info->GetMessage(i,nullptr,&n));auto data=new char[n];auto msg=reinterpret_cast<D3D12_MESSAGE*>(data);check(info->GetMessage(i,msg,&n));bool bad=msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR; if(bad) std::cerr<<msg->pDescription<<'\n';delete[] data;expect(!bad); }
    CloseHandle(event);std::cout<<"Depth families and typed float16/float32 depth/camera correction passed on WARP\n";return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
