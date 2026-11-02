#include "nr_codec.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cstdio>

using Microsoft::WRL::ComPtr;
namespace {
void demand(HRESULT result) { if(FAILED(result))throw std::runtime_error(std::format("HRESULT {:X}",unsigned(result))); }
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&barrier);
}
}
int main()try {
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;demand(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter;demand(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;demand(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages;device.As(&messages);
    D3D12_COMMAND_QUEUE_DESC qdesc{};qdesc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;demand(device->CreateCommandQueue(&qdesc,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;demand(device->CreateCommandAllocator(qdesc.Type,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;demand(device->CreateCommandList(0,qdesc.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    constexpr UINT width=8,height=8;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width=width;texture.Height=height;texture.DepthOrArraySize=1;texture.MipLevels=1;
    texture.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;texture.SampleDesc.Count=1;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES local{};local.Type=D3D12_HEAP_TYPE_DEFAULT;
    auto create=[&](D3D12_RESOURCE_STATES state){ComPtr<ID3D12Resource> r;
        demand(device->CreateCommittedResource(&local,D3D12_HEAP_FLAG_NONE,&texture,state,nullptr,IID_PPV_ARGS(&r)));return r;};
    auto source=create(D3D12_RESOURCE_STATE_COPY_DEST),encoded=create(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        original=create(D3D12_RESOURCE_STATE_UNORDERED_ACCESS),output=create(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows{};UINT64 rowbytes{},bytes{};
    device->GetCopyableFootprints(&texture,0,1,0,&footprint,&rows,&rowbytes,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;
    buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES read_heap{};read_heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> upload,readback;
    demand(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
    demand(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    const std::array<float,4> pixel{.125f,.25f,.5f,1.f};void* mapped{};D3D12_RANGE empty{};
    demand(upload->Map(0,&empty,&mapped));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(pixel),pixel.data(),sizeof(pixel));
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION input{};input.pResource=upload.Get();input.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;input.PlacedFootprint=footprint;
    D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=source.Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dest,0,0,0,&input,nullptr);
    transition(list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNr_Dx12 codec("standalone NR fixture",device.Get());if(!codec.IsInit())throw std::runtime_error("codec pipeline initialization");
    codec.begin_recording();
    DlssNrConstants params{};params.Mode=DlssNrMode_Encode;params.WhitePoint=1.f;params.Width=width;params.Height=height;
    if(!codec.Dispatch(list.Get(),params,source.Get(),nullptr,nullptr,nullptr,nullptr,encoded.Get(),original.Get()))throw std::runtime_error("encode dispatch");
    transition(list.Get(),encoded.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(),original.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    params.Mode=DlssNrMode_Resolve;params.TransferStrength=0.f;params.ColourStrength=1.f;params.MaxRatio=2.f;
    params.GuideWidth=width;params.GuideHeight=height;
    if(!codec.Dispatch(list.Get(),params,encoded.Get(),encoded.Get(),original.Get(),nullptr,nullptr,output.Get(),nullptr))throw std::runtime_error("resolve dispatch");
    transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION read{};read.pResource=readback.Get();read.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;read.PlacedFootprint=footprint;
    dest.pResource=output.Get();list->CopyTextureRegion(&read,0,0,0,&dest,nullptr);demand(list->Close());codec.end_recording();
    ID3D12CommandList* submitted[]={list.Get()};queue->ExecuteCommandLists(1,submitted);
    ComPtr<ID3D12Fence> fence;demand(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));demand(queue->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);demand(fence->SetEventOnCompletion(1,event));
    auto waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("GPU retirement");
    D3D12_RANGE entire{0,size_t(bytes)};demand(readback->Map(0,&entire,&mapped));unsigned verified{};
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto actual=static_cast<const char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(pixel);
        if(memcmp(actual,pixel.data(),sizeof(pixel)))throw std::runtime_error("zero transfer must be bit-identical");++verified;
    }
    readback->Unmap(0,&empty);
    // A second, exactly retired recording changes both source ownership and
    // codec constants. Retained descriptors from the first recording must not
    // redirect the new work or preserve the old resource graph indefinitely.
    demand(allocator->Reset());demand(list->Reset(allocator.Get(),nullptr));
    auto next_source=create(D3D12_RESOURCE_STATE_COPY_DEST);dest.pResource=next_source.Get();
    list->CopyTextureRegion(&dest,0,0,0,&input,nullptr);
    transition(list.Get(),next_source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(),encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list.Get(),original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    codec.begin_recording();params.Mode=DlssNrMode_Encode;params.WhitePoint=2.f;
    if(!codec.Dispatch(list.Get(),params,next_source.Get(),nullptr,nullptr,nullptr,nullptr,encoded.Get(),original.Get()))
        throw std::runtime_error("second encode dispatch");
    transition(list.Get(),encoded.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(),original.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    params.Mode=DlssNrMode_Resolve;
    if(!codec.Dispatch(list.Get(),params,encoded.Get(),encoded.Get(),original.Get(),nullptr,nullptr,output.Get(),nullptr))
        throw std::runtime_error("second resolve dispatch");
    transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    dest.pResource=output.Get();list->CopyTextureRegion(&read,0,0,0,&dest,nullptr);demand(list->Close());codec.end_recording();
    queue->ExecuteCommandLists(1,submitted);demand(queue->Signal(fence.Get(),2));
    event=CreateEventW(nullptr,FALSE,FALSE,nullptr);demand(fence->SetEventOnCompletion(2,event));
    waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("second GPU retirement");
    demand(readback->Map(0,&entire,&mapped));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto actual=static_cast<const char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(pixel);
        if(memcmp(actual,pixel.data(),sizeof(pixel)))throw std::runtime_error("second recording must be bit-identical");++verified;
    }
    readback->Unmap(0,&empty);
    // Independent offset/extent patterns make swapped guides, filtered motion,
    // scaled values and reading outside the ROI observable on the actual GPU.
    demand(upload->Map(0,&empty,&mapped));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const std::array<float,4> value{float(x+10*y),float(100+y)-float(x),0.f,1.f};
        memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(value),value.data(),sizeof(value));
    }
    upload->Unmap(0,nullptr);
    auto patterned=create(D3D12_RESOURCE_STATE_COPY_DEST);
    UINT64 serial=2;
    for(unsigned percent:{100u,75u,50u,25u}) {
        const UINT work=width*percent/100;
        demand(allocator->Reset());demand(list->Reset(allocator.Get(),nullptr));
        if(percent==100) {
            dest.pResource=patterned.Get();list->CopyTextureRegion(&dest,0,0,0,&input,nullptr);
            transition(list.Get(),patterned.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        auto guideDesc=texture;guideDesc.Width=guideDesc.Height=work;
        std::array<ComPtr<ID3D12Resource>,2> guides,reads;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> layouts{};
        std::array<UINT64,2> lengths{};
        for(unsigned i=0;i<2;++i) {
            guideDesc.Format=i?DXGI_FORMAT_R32G32_FLOAT:DXGI_FORMAT_R32_FLOAT;
            demand(device->CreateCommittedResource(&local,D3D12_HEAP_FLAG_NONE,&guideDesc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&guides[i])));
            device->GetCopyableFootprints(&guideDesc,0,1,0,&layouts[i],&rows,&rowbytes,&lengths[i]);
            auto rb=buffer;rb.Width=lengths[i];
            demand(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&rb,
                D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&reads[i])));
        }
        codec.begin_recording();DlssNrConstants c{};
        c.Mode=DlssNrMode_ResizePrivateGuides;c.Width=c.Height=work;
        c.DepthX=2;c.DepthY=1;c.DepthWidth=4;c.DepthHeight=6;
        c.MotionX=1;c.MotionY=3;c.MotionWidth=6;c.MotionHeight=4;
        if(!codec.Dispatch(list.Get(),c,patterned.Get(),patterned.Get(),nullptr,nullptr,nullptr,guides[0].Get(),guides[1].Get()))
            throw std::runtime_error("matched guide dispatch");
        for(unsigned i=0;i<2;++i) {
            transition(list.Get(),guides[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=reads[i].Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=layouts[i];
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=guides[i].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        demand(list->Close());codec.end_recording();queue->ExecuteCommandLists(1,submitted);
        demand(queue->Signal(fence.Get(),++serial));
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);demand(fence->SetEventOnCompletion(serial,event));
        waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("guide GPU retirement");
        for(unsigned i=0;i<2;++i) {
            D3D12_RANGE range{0,size_t(lengths[i])};demand(reads[i]->Map(0,&range,&mapped));
            for(UINT y=0;y<work;++y)for(UINT x=0;x<work;++x) {
                const UINT sx=(i?1:2)+UINT((float(x)+.5f)/float(work)*float(i?6:4));
                const UINT sy=(i?3:1)+UINT((float(y)+.5f)/float(work)*float(i?4:6));
                const auto* actual=reinterpret_cast<const float*>(static_cast<const char*>(mapped)+layouts[i].Offset+y*layouts[i].Footprint.RowPitch+x*sizeof(float)*(i?2:1));
                if(actual[0]!=float(sx+10*sy)||(i&&actual[1]!=float(100+sy)-float(sx)))throw std::runtime_error("guide ROI, point sampling or vector units changed");
                ++verified;
            }
            reads[i]->Unmap(0,&empty);
        }
    }
    unsigned errors{};
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T size{};messages->GetMessage(i,nullptr,&size);std::vector<char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());demand(messages->GetMessage(i,message,&size));
        if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            ++errors;printf("D3D12 validation: %s\n",message->pDescription);
        }
    }
    printf("NR WARP codec: %u pixels bit-identical, %u validation errors\n",verified,errors);return errors?1:0;
}catch(const std::exception& failure){printf("NR WARP codec FAIL: %s\n",failure.what());return 1;}
