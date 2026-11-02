#include "nr_codec.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

using Microsoft::WRL::ComPtr;
namespace {
using Pixel=std::array<float,4>;
constexpr Pixel sentinel{.125f,.25f,.75f,.625f};
void demand(HRESULT result) {
    if(FAILED(result))throw std::runtime_error(std::format("HRESULT {:X}",unsigned(result)));
}
void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};
    list->ResourceBarrier(1,&barrier);
}
Pixel pattern(UINT x,UINT y,UINT seed) {
    const float nan=std::bit_cast<float>(0x7fc12345u),inf=std::numeric_limits<float>::infinity();
    constexpr std::array<Pixel,9> values{{
        {-2.f,.125f,2.f,0.f}, {0.f,0.f,0.f,0.f}, {0.f,0.f,0.f,0.f},
        {1.f,0.f,.5f,0.f}, {1.0001f,-.0001f,.375f,0.f}, {-4.f,0.f,.625f,0.f},
        {0.f,.75f,0.f,0.f}, {.31f,.7f,.89f,0.f}, {-0.f,.9f,1.01f,0.f}
    }};
    const auto index=(x+3*y+seed)%values.size();auto result=values[index];
    if(index==1)result={nan,inf,-inf,0.f};
    if(index==2)result={-inf,nan,inf,0.f};
    if(index==5)result[1]=nan;
    if(index==6){result[0]=inf;result[2]=-inf;}
    // Include negative zero and alpha above 1: the clamp only bounds RGB.
    result[3]=(x+y)%4==0?-0.f:float((x+3*y+seed)%29)/16.f;
    return result;
}
struct Fixture {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> messages;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    UINT64 serial{};
    explicit Fixture(bool nvidia) {
        ComPtr<IDXGIFactory4> factory;demand(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> chosen;
        if(nvidia) {
            for(UINT i=0;;++i) {
                ComPtr<IDXGIAdapter1> candidate;
                if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 desc{};demand(candidate->GetDesc1(&desc));
                if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
                    chosen=std::move(candidate);break;
                }
            }
            require(bool(chosen),"requested NVIDIA adapter unavailable");
        } else demand(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)));
        demand(D3D12CreateDevice(chosen.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
        device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        demand(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        demand(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)));
        demand(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
        demand(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    ComPtr<ID3D12Resource> resource(const D3D12_RESOURCE_DESC& desc,D3D12_HEAP_TYPE type,
        D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> result;
        demand(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&result)));
        return result;
    }
    void submit() {
        demand(list->Close());ID3D12CommandList* commands[]{list.Get()};
        queue->ExecuteCommandLists(1,commands);demand(queue->Signal(fence.Get(),++serial));
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        require(event!=nullptr,"retirement event creation");
        const auto armed=fence->SetEventOnCompletion(serial,event);
        const auto waited=SUCCEEDED(armed)?WaitForSingleObject(event,10000):WAIT_FAILED;
        CloseHandle(event);demand(armed);require(waited==WAIT_OBJECT_0,"exact GPU retirement timed out");
    }
    void reset() {demand(allocator->Reset());demand(list->Reset(allocator.Get(),nullptr));}
};
struct Case {
    UINT width{},height{},work_width{},work_height{},seed{};
    ComPtr<ID3D12Resource> source,target,input_upload,target_upload,readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
};
Case make_case(Fixture& f,UINT width,UINT height,UINT work_width,UINT work_height,UINT seed) {
    Case c;c.width=width;c.height=height;c.work_width=work_width;c.work_height=work_height;c.seed=seed;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=width;texture.Height=height;texture.DepthOrArraySize=1;texture.MipLevels=1;
    texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;texture.SampleDesc.Count=1;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    c.source=f.resource(texture,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    c.target=f.resource(texture,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    UINT rows{};UINT64 row_bytes{};
    f.device->GetCopyableFootprints(&texture,0,1,0,&c.footprint,&rows,&row_bytes,&c.bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=c.bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    c.input_upload=f.resource(buffer,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    c.target_upload=f.resource(buffer,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    c.readback=f.resource(buffer,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RANGE empty{};void* mapped{};
    for(unsigned i=0;i<2;++i) {
        auto* upload=i?c.target_upload.Get():c.input_upload.Get();
        demand(upload->Map(0,&empty,&mapped));
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
            const auto value=i?sentinel:pattern(x,y,seed);
            memcpy(static_cast<char*>(mapped)+c.footprint.Offset+y*c.footprint.Footprint.RowPitch+x*sizeof(Pixel),value.data(),sizeof(value));
        }
        upload->Unmap(0,nullptr);
        D3D12_TEXTURE_COPY_LOCATION input{};input.pResource=upload;
        input.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;input.PlacedFootprint=c.footprint;
        D3D12_TEXTURE_COPY_LOCATION output{};output.pResource=i?c.target.Get():c.source.Get();
        output.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f.list->CopyTextureRegion(&output,0,0,0,&input,nullptr);
    }
    transition(f.list.Get(),c.source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(f.list.Get(),c.target.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return c;
}
void dispatch(Fixture& f,DlssNr_Dx12& codec,const Case& c) {
    DlssNrConstants p{};p.Mode=DlssNrMode_ClampEncoded;p.Width=c.work_width;p.Height=c.work_height;
    require(codec.Dispatch(f.list.Get(),p,c.source.Get(),nullptr,nullptr,nullptr,nullptr,c.target.Get(),nullptr),"encoded clamp dispatch");
}
void readback(Fixture& f,const Case& c) {
    transition(f.list.Get(),c.target.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION input{};input.pResource=c.target.Get();input.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION output{};output.pResource=c.readback.Get();
    output.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;output.PlacedFootprint=c.footprint;
    f.list->CopyTextureRegion(&output,0,0,0,&input,nullptr);
}
unsigned verify(const Case& c) {
    void* mapped{};D3D12_RANGE range{0,size_t(c.bytes)},empty{};demand(c.readback->Map(0,&range,&mapped));
    unsigned verified{};
    for(UINT y=0;y<c.height;++y)for(UINT x=0;x<c.width;++x) {
        Pixel actual{};memcpy(actual.data(),static_cast<const char*>(mapped)+c.footprint.Offset+y*c.footprint.Footprint.RowPitch+x*sizeof(Pixel),sizeof(actual));
        auto expected=sentinel;
        if(x<c.work_width && y<c.work_height) {
            expected=pattern(x,y,c.seed);
            for(unsigned i=0;i<3;++i)expected[i]=std::isfinite(expected[i])?std::clamp(expected[i],0.f,1.f):.5f;
        }
        for(unsigned i=0;i<3;++i)require(actual[i]==expected[i],"clamp RGB, odd-dispatch guard or immutable descriptor mismatch");
        require(std::bit_cast<UINT>(actual[3])==std::bit_cast<UINT>(expected[3]),"clamp changed alpha bits");
        ++verified;
    }
    c.readback->Unmap(0,&empty);return verified;
}
}
int main(int argc,char** argv)try {
    const bool nvidia=argc>1 && std::string_view(argv[1])=="--nvidia";
    ComPtr<ID3D12Debug> debug;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Fixture f(nvidia);DlssNr_Dx12 codec("NR encoded clamp fixture",f.device.Get());
    require(codec.IsInit(),"accepted codec initialization");
    auto first=make_case(f,17,11,13,9,0),second=make_case(f,19,13,17,11,4);
    DlssNrConstants invalid{};invalid.Mode=DlssNrMode_ClampEncoded;invalid.Width=18;invalid.Height=11;
    require(!codec.Dispatch(f.list.Get(),invalid,first.source.Get(),nullptr,nullptr,nullptr,nullptr,first.target.Get(),nullptr),"oversized clamp extent accepted");
    require(codec.IsInit(),"invalid optional clamp poisoned accepted codec");
    codec.begin_recording();dispatch(f,codec,first);dispatch(f,codec,second);
    readback(f,first);readback(f,second);codec.end_recording();f.submit();
    unsigned verified=verify(first)+verify(second);
    // Only after the exact first submission fence may a new recording retire
    // its unused immutable bindings and release the preceding resource graph.
    f.reset();first={};second={};
    auto third=make_case(f,23,15,21,13,7);
    codec.begin_recording();dispatch(f,codec,third);readback(f,third);codec.end_recording();f.submit();
    verified+=verify(third);
    unsigned errors{};
    if(f.messages)for(UINT64 i=0;i<f.messages->GetNumStoredMessages();++i) {
        SIZE_T size{};demand(f.messages->GetMessage(i,nullptr,&size));std::vector<char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());demand(f.messages->GetMessage(i,message,&size));
        if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            ++errors;std::printf("D3D12 validation: %s\n",message->pDescription);
        }
    }
    std::printf("NR encoded clamp %s: %u pixels, finite RGB + untouched alpha + guards + retired bindings, %u validation errors\n",
        nvidia?"NVIDIA":"WARP",verified,errors);
    return errors?1:0;
}catch(const std::exception& error){std::printf("NR encoded clamp FAIL: %s\n",error.what());return 1;}
