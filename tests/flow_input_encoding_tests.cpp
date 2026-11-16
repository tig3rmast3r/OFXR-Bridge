#include "xrfg/flow_input_encoding.hpp"
#include <windows.h>
#include "pack_flow_input_shader.hpp"
#include "nvidia_pack_flow_input_shader.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;
using xrfg::D3D12OpticalFlowInputEncoding;
constexpr auto reference = xrfg::flow_encoding_for_route(false);
constexpr auto corrected = xrfg::flow_encoding_for_route(true);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, reference) == 0);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, reference) == 0);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, corrected) == 2);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, corrected) == 2);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_R8G8B8A8_UNORM, corrected) == 0);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_R16G16B16A16_FLOAT, reference) == 0);
static_assert(xrfg::flow_input_flags(DXGI_FORMAT_R16G16B16A16_FLOAT, corrected) == 2);

void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
void checked(HRESULT result,const char* message) { require(SUCCEEDED(result),message); }
D3D12_RESOURCE_BARRIER transition(ID3D12Resource* image,D3D12_RESOURCE_STATES before,
                                 D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};return b;
}
double decode(double value) { return value<=0.04045 ? value/12.92 : std::pow((value+0.055)/1.055,2.4); }
double encode(double value) { return value<=0.0031308 ? value*12.92 : 1.055*std::pow(value,1.0/2.4)-0.055; }

int main(int argc,char** argv) {
    try {
        const bool reference_only=argc>1 && std::string_view(argv[1])=="reference-only";
        std::uint64_t reference_digest=14695981039346656037ULL;
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> device;
        checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        checked(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP");
        checked(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
        D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;
        checked(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        ComPtr<ID3D12Fence> fence;std::uint64_t serial=0;
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        const auto submit=[&](auto record) {
            ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
            checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
            checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
                IID_PPV_ARGS(&list)),"list");
            record(list.Get());checked(list->Close(),"close");
            ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);
            checked(queue->Signal(fence.Get(),++serial),"signal");
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"event");
            const HRESULT armed=fence->SetEventOnCompletion(serial,event);
            const DWORD result=SUCCEEDED(armed) ? WaitForSingleObject(event,5000) : WAIT_FAILED;
            CloseHandle(event);require(result==WAIT_OBJECT_0,"GPU timeout");
        };
        const auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
            d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES h{};h.Type=type;ComPtr<ID3D12Resource> result;
            checked(device->CreateCommittedResource(&h,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,
                IID_PPV_ARGS(&result)),"buffer");return result;
        };
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=2;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;
        d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> source;
        checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&source)),"source");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
        device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
        auto upload=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const std::array<unsigned char,8> pixels{16,32,64,128,64,64,64,128};
        void* mapped{};checked(upload->Map(0,nullptr,&mapped),"upload map");
        std::memcpy(static_cast<unsigned char*>(mapped)+footprint.Offset,pixels.data(),pixels.size());upload->Unmap(0,nullptr);
        submit([&](ID3D12GraphicsCommandList* list) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
            dst.pResource=source.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            auto b=transition(source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1,&b);
        });
        d.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;ComPtr<ID3D12Resource> source_bgra;
        checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&source_bgra)),"BGRA source");
        submit([&](ID3D12GraphicsCommandList* list) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=upload.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprint;
            src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            dst.pResource=source_bgra.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            auto b=transition(source_bgra.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1,&b);
        });
        d.Format=DXGI_FORMAT_R16G16B16A16_TYPELESS;
        ComPtr<ID3D12Resource> source_fp16;
        checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&source_fp16)),"FP16 source");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp16_footprint{};
        UINT64 fp16_bytes=0;
        device->GetCopyableFootprints(&d,0,1,0,&fp16_footprint,nullptr,nullptr,&fp16_bytes);
        auto fp16_upload=buffer(fp16_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        // Half-precision encodings of the two linear RGBA pixels below.
        const std::array<std::uint16_t,8> fp16_pixels{0x2c00,0x3000,0x3400,0x3800,0x3400,0x3400,0x3400,0x3800};
        const std::array<double,8> fp16_values{0.0625,0.125,0.25,0.5,0.25,0.25,0.25,0.5};
        checked(fp16_upload->Map(0,nullptr,&mapped),"FP16 upload map");
        std::memcpy(static_cast<unsigned char*>(mapped)+fp16_footprint.Offset,
            fp16_pixels.data(),sizeof(fp16_pixels));
        fp16_upload->Unmap(0,nullptr);
        submit([&](ID3D12GraphicsCommandList* list) {
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};
            src.pResource=fp16_upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint=fp16_footprint;src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
            dst.pResource=source_fp16.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            auto b=transition(source_fp16.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1,&b);
        });
        std::array<ComPtr<ID3D12Resource>,3> outputs;
        d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for(auto& output:outputs) checked(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output)),"output");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.NumDescriptors=5;hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;ComPtr<ID3D12DescriptorHeap> descriptors;
        checked(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&descriptors)),"descriptors");
        const auto step=device->GetDescriptorHandleIncrementSize(hd.Type);
        const auto cpu=[&](unsigned i) { auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=i*step;return h; };
        const auto gpu=[&](unsigned i) { auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=i*step;return h; };
        for(unsigned i=0;i<3;++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=d.Format;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            device->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&u,cpu(i+2));
        }
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        for(unsigned i=0;i<2;++i) {
            parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable={1,&ranges[i]};
        }
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants={0,0,62};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=parameters;
        ComPtr<ID3DBlob> serialized,error;ComPtr<ID3D12RootSignature> signature;
        checked(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&error),"serialize root");
        checked(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),
            IID_PPV_ARGS(&signature)),"root signature");
        std::array<ComPtr<ID3D12PipelineState>,2> pipelines;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=signature.Get();
        ps.CS={g_xrfg_pack_flow_input_shader,sizeof(g_xrfg_pack_flow_input_shader)};
        checked(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipelines[0])),"FFX pack pipeline");
        ps.CS={g_xrfg_nvidia_pack_flow_input_shader,sizeof(g_xrfg_nvidia_pack_flow_input_shader)};
        checked(device->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipelines[1])),"NVIDIA pack pipeline");
        auto readback=buffer(bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        unsigned cases=0;
        for(unsigned family:{0U,1U,2U}) for(bool nvidia:{false,true}) for(bool srgb:{false,true})
        for(bool new_route:{false,true}) for(bool reduced:{false,true}) {
            const bool bgra=family==1;
            const bool fp16=family==2;
            if((reference_only && new_route) || (fp16 && srgb)) continue;
            const auto format=fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : bgra
                ? (srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM)
                : (srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM);
            D3D12_SHADER_RESOURCE_VIEW_DESC v{};v.Format=format;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2DArray.MipLevels=1;
            v.Texture2DArray.ArraySize=1;
            auto* input=fp16 ? source_fp16.Get() : bgra ? source_bgra.Get() : source.Get();
            device->CreateShaderResourceView(input,&v,cpu(0));device->CreateShaderResourceView(input,&v,cpu(1));
            std::array<UINT,62> constants{};
            constants[0]=2;constants[1]=1;constants[2]=1;constants[3]=reduced?1:2;
            constants[4]=1;constants[5]=1;constants[61]=0x800000U | 1U |
                xrfg::flow_input_flags(format,xrfg::flow_encoding_for_route(new_route));
            const unsigned output_index=nvidia?1:0;
            submit([&](ID3D12GraphicsCommandList* list) {
                ID3D12DescriptorHeap* heaps[]{descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
                list->SetComputeRootSignature(signature.Get());list->SetPipelineState(pipelines[nvidia?1:0].Get());
                list->SetComputeRootDescriptorTable(0,gpu(0));list->SetComputeRootDescriptorTable(1,gpu(2));
                list->SetComputeRoot32BitConstants(2,62,constants.data(),0);list->Dispatch(1,1,1);
                auto b=transition(outputs[output_index].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->ResourceBarrier(1,&b);
                D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=outputs[output_index].Get();
                src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=readback.Get();
                dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
                dst.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
                b=transition(outputs[output_index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                list->ResourceBarrier(1,&b);
            });
            D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
            checked(readback->Map(0,&range,&mapped),"readback map");
            const auto* actual=static_cast<const unsigned char*>(mapped)+footprint.Offset;
            if(!new_route && !fp16) for(unsigned i=0;i<(reduced?4U:8U);++i) {
                reference_digest^=actual[i];reference_digest*=1099511628211ULL;
            }
            bool correct=true;
            for(unsigned channel=0;channel<4;++channel) {
                auto component=[&](unsigned pixel) {
                    if(fp16) return fp16_values[pixel*4+channel];
                    const unsigned input_channel=bgra && channel<3 ? 2-channel : channel;
                    const double c=pixels[pixel*4+input_channel]/255.0;
                    return srgb && channel<3 ? decode(c) : c;
                };
                double expected=reduced ? (component(0)+component(1))*0.5 : component(0);
                if((srgb || fp16) && new_route && channel<3) expected=encode(expected);
                correct &= std::abs(static_cast<int>(actual[channel])-static_cast<int>(std::lround(expected*255)))<=1;
            }
            D3D12_RANGE empty{};readback->Unmap(0,&empty);
            if(!correct) std::cerr<<"pack case format="<<format<<" nvidia="<<nvidia<<" new="<<new_route<<" reduced="<<reduced<<'\n';
            require(correct,"optical-flow pack output mismatch");++cases;
        }
        std::cout<<"PASS "<<cases<<" actual shader cases: RGBA/BGRA/FP16, both pack pipelines, reference/new, full/reduced, alpha\n";
        std::cout<<"REFERENCE_PACK_FNV64 "<<std::hex<<reference_digest<<'\n';
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
