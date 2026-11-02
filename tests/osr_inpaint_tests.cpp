// Real OSR shaders and production pass, without OpenXR, a headset or a game.
// All waits/readbacks in this file are fixture-only.
#include "osr_full_warp_pass.hpp"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <bit>
#include <fstream>
#include <string>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void ok(HRESULT result, const char* message) { check(SUCCEEDED(result), message); }
constexpr UINT width = 128, height = 72;
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;
    ComPtr<ID3D12Resource> color, depth;
    std::vector<ComPtr<ID3D12Resource>> transfers;
    HANDLE event{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    UINT64 serial{};
    xrfg::OsrFullWarp warp;
    bool validation{};
    Fixture() {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer(); validation = true;
        }
        ok(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        device.As(&info);
        D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), "queue");
        ok(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), "allocator");
        ok(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        ok(list->Close(), "initial close");
        ok(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        color = texture(DXGI_FORMAT_R8G8B8A8_UNORM);
        depth = texture(DXGI_FORMAT_R32_FLOAT);
        ok(warp.prepare(device.Get(), width, height, width, height, DXGI_FORMAT_R8G8B8A8_UNORM), "production OSR prepare");
    }
    ~Fixture() { CloseHandle(event); }
    ComPtr<ID3D12Resource> texture(DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = width; d.Height = height; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = format; d.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> resource;
        ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(&resource)), "texture");
        return resource;
    }
    ComPtr<ID3D12Resource> buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        ok(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&resource)), "transfer buffer");
        return resource;
    }
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    }
    void upload(ID3D12Resource* resource, const void* pixels) {
        const auto desc = resource->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
        auto transfer = buffer(bytes, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{}; ok(transfer->Map(0, nullptr, &mapped), "upload map");
        for (UINT y = 0; y < height; ++y)
            std::memcpy(static_cast<char*>(mapped) + fp.Offset + y * fp.Footprint.RowPitch,
                static_cast<const char*>(pixels) + y * width * 4, width * 4);
        transfer->Unmap(0, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = resource; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = transfer.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transfers.push_back(transfer);
    }
    struct Readback { ComPtr<ID3D12Resource> buffer; D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint; };
    Readback readback(ID3D12Resource* resource) {
        const auto desc = resource->GetDesc(); Readback r{}; UINT64 bytes{};
        device->GetCopyableFootprints(&desc, 0, 1, 0, &r.footprint, nullptr, nullptr, &bytes);
        r.buffer = buffer(bytes, D3D12_HEAP_TYPE_READBACK);
        transition(resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
        dst.pResource = r.buffer.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = r.footprint;
        src.pResource = resource; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        transition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        return r;
    }

    std::vector<UINT> inpaint(const std::vector<UINT>& pixels, const std::vector<UINT>& tags) {
        ok(allocator->Reset(), "reset inpaint allocator"); ok(list->Reset(allocator.Get(), nullptr), "reset inpaint list");
        transfers.clear();
        auto mask=texture(DXGI_FORMAT_R32_UINT); upload(color.Get(),pixels.data()); upload(mask.Get(),tags.data());
        D3D12_DESCRIPTOR_RANGE ranges[2]{{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,10,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,7,0,0,0}};
        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,sizeof(xrfg::OsrFullConstants)/4};
        for(UINT i=0;i<2;++i){params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[i+1].DescriptorTable={1,&ranges[i]};}
        D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params;
        ComPtr<ID3DBlob> blob,error;ok(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"serialize inpaint root");
        ComPtr<ID3D12RootSignature> root;ok(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"inpaint root");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={g_xrfg_osr_full_inpaint_shader,sizeof(g_xrfg_osr_full_inpaint_shader)};
        ComPtr<ID3D12PipelineState> pipeline;ok(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)),"shipping inpaint shader");
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=17;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> heap;ok(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"inpaint descriptors");
        const auto stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<10;++i){D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Texture2D.MipLevels=1;d.Format=i==7?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32_UINT;
            device->CreateShaderResourceView(i==6?mask.Get():i==7?color.Get():nullptr,&d,cpu);cpu.ptr+=stride;}
        for(UINT i=0;i<7;++i){D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.Format=i==4?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R32_UINT;
            device->CreateUnorderedAccessView(i==4?warp.output():nullptr,nullptr,&d,cpu);cpu.ptr+=stride;}
        transition(color.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(mask.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(warp.output(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        xrfg::OsrFullConstants constants{};constants.depth_width=constants.color_width=width;constants.depth_height=constants.color_height=height;
        list->SetPipelineState(pipeline.Get());list->SetComputeRootSignature(root.Get());ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRoot32BitConstants(0,sizeof(constants)/4,&constants,0);auto gpu=heap->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=10*stride;list->SetComputeRootDescriptorTable(2,gpu);
        list->Dispatch((width+7)/8,(height+7)/8,1);
        transition(warp.output(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        transition(color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        transition(mask.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        const auto read=readback(warp.output());ok(list->Close(),"inpaint close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);ok(queue->Signal(fence.Get(),++serial),"inpaint signal");ok(fence->SetEventOnCompletion(serial,event),"inpaint event");check(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"inpaint completion");
        std::vector<UINT> result(width*height);void* mapped{};ok(read.buffer->Map(0,nullptr,&mapped),"inpaint map");for(UINT y=0;y<height;++y)std::memcpy(result.data()+y*width,static_cast<char*>(mapped)+read.footprint.Offset+y*read.footprint.Footprint.RowPitch,width*4);read.buffer->Unmap(0,nullptr);
        if(info)for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n{};info->GetMessage(i,nullptr,&n);std::vector<char> data(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&n);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12: %s\n",m->pDescription);check(false,"inpaint validation");}}
        if(info)info->ClearStoredMessages();return result;
    }
};
UINT word(float depth,UINT tag){return (std::bit_cast<UINT>(depth)&~3u)|tag;}
}
int main(int argc,char** argv){
    Fixture f;
    if(argc==4){
        std::vector<UINT> pixels(width*height),tags(width*height);
        std::ifstream a(argv[1],std::ios::binary),b(argv[2],std::ios::binary);a.read(reinterpret_cast<char*>(pixels.data()),pixels.size()*4);b.read(reinterpret_cast<char*>(tags.data()),tags.size()*4);check(bool(a)&&bool(b),"capture replay input sizes");
        const auto output=f.inpaint(pixels,tags);std::ofstream out(argv[3],std::ios::binary);out.write(reinterpret_cast<const char*>(output.data()),output.size()*4);check(bool(out),"capture replay output");std::puts("Captured OSR mask/color -> shipping inpaint shader PASS");return 0;
    }
    // A short purple gap beside a wide green history band. The old scan
    // crossed the green band and mirrored a distant red sign into the gap.
    for(bool background_right:{false,true})for(UINT hole_tag:{0u,3u})for(bool history_border:{false,true}){
        std::vector<UINT> pixels(width*height),tags(width*height);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
            UINT t=x; if(!background_right)t=width-1-x;
            const bool hole=t>=30 && t<=33;
            const bool history=history_border && t>=34 && t<67;
            pixels[y*width+x]=t<30?0xff2040a0u:(0xff705000u|(t+10));
            if(t>=99&&t<=106)pixels[y*width+x]=0xff1010f0u;
            tags[y*width+x]=hole?hole_tag:word(t<30?.8F:.2F,history?2u:1u);
        }
        const auto result=f.inpaint(pixels,tags);
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
            const UINT t=background_right?x:width-1-x;
            if(t>=30&&t<=33){const UINT from=67-t;const UINT xs=background_right?from:width-1-from;check(result[y*width+x]==pixels[y*width+xs],"mirror starts at purple/valid boundary on either side");}
            else check(result[y*width+x]==pixels[y*width+x],"valid stereo/history pixels remain bit-exact");
        }
    }
    std::printf("osr_inpaint_tests: PASS (8 layouts, both sides, holes/rejected history, green/stereo border; validation %s)\n",f.validation?"enabled":"unavailable");
}
