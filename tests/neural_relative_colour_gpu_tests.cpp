#include "nr_codec.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <iterator>

using Microsoft::WRL::ComPtr;
namespace {
void demand(HRESULT result) { if(FAILED(result))throw std::runtime_error(std::format("HRESULT {:X}",unsigned(result))); }
void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&barrier);
}
}
int main(int argc,char** argv)try {
    const bool edited=argc>3&&std::string_view(argv[3])=="edit";
    const unsigned compare=argc>4?std::stoul(argv[4]):0;
    const bool swap=argc>5&&std::string_view(argv[5])=="swap";
    const bool resolveProbe=true;
    const bool outside=argc>1&&std::string_view(argv[1])=="outside";
    const float colour=argc>2?std::stof(argv[2]):1.f;
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
    auto normal=texture;texture.Width=texture.Height=1;texture.Format=DXGI_FORMAT_R32_FLOAT;
    auto exposure=create(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);texture=normal;
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
    const std::array<float,4> pixel=outside?std::array<float,4>{30.f,40.f,60.f,1.f}:std::array<float,4>{.1f,.15f,.2f,1.f};
    const std::array<float,4> patch{3.f,12.f,40.f,1.f};void* mapped{};D3D12_RANGE empty{};
    demand(upload->Map(0,&empty,&mapped));
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
        memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(pixel),(x==4&&y==4?patch:pixel).data(),sizeof(pixel));
    upload->Unmap(0,nullptr);
    D3D12_TEXTURE_COPY_LOCATION input{};input.pResource=upload.Get();input.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;input.PlacedFootprint=footprint;
    D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=source.Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dest,0,0,0,&input,nullptr);
    transition(list.Get(),source.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto model=create(D3D12_RESOURCE_STATE_COPY_DEST);ComPtr<ID3D12Resource> modelUpload;
    demand(device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&modelUpload)));
    demand(modelUpload->Map(0,&empty,&mapped));const std::array<float,4> white{1,1,1,1};
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)memcpy(static_cast<char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*sizeof(white),white.data(),sizeof(white));
    modelUpload->Unmap(0,nullptr);auto modelInput=input;modelInput.pResource=modelUpload.Get();dest.pResource=model.Get();
    list->CopyTextureRegion(&dest,0,0,0,&modelInput,nullptr);transition(list.Get(),model.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNr_Dx12 codec("standalone NR fixture",device.Get());if(!codec.IsInit())throw std::runtime_error("codec pipeline initialization");
    codec.begin_recording();
    DlssNrConstants measure{};measure.Mode=DlssNrMode_MeasureExposure;measure.Width=measure.Height=1;measure.Passthrough=1;
    if(!codec.Dispatch(list.Get(),measure,source.Get(),nullptr,nullptr,nullptr,nullptr,exposure.Get(),nullptr))throw std::runtime_error("exposure dispatch");
    transition(list.Get(),exposure.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DlssNrConstants params{};params.Mode=DlssNrMode_Encode;params.WhitePoint=1.f;params.Width=width;params.Height=height;
    if(!codec.Dispatch(list.Get(),params,source.Get(),nullptr,nullptr,nullptr,exposure.Get(),encoded.Get(),original.Get()))throw std::runtime_error("encode dispatch");
    transition(list.Get(),encoded.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    transition(list.Get(),original.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    params.Mode=DlssNrMode_Resolve;params.TransferStrength=resolveProbe?1.f:0.f;params.ColourStrength=colour;params.MaxRatio=2.f;
    params.GuideWidth=width;params.GuideHeight=height;
    params.CompareMode=compare;params.CompareZoom=1;params.CompareSplit=.5f;params.CompareSwap=swap;
    if(!codec.Dispatch(list.Get(),params,encoded.Get(),edited?model.Get():encoded.Get(),original.Get(),nullptr,exposure.Get(),output.Get(),nullptr))throw std::runtime_error("resolve dispatch");
    transition(list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION read{};read.pResource=readback.Get();read.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;read.PlacedFootprint=footprint;
    dest.pResource=output.Get();list->CopyTextureRegion(&read,0,0,0,&dest,nullptr);
    ComPtr<ID3D12Resource> encodedRead,exposureRead;
    demand(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&encodedRead)));
    read.pResource=encodedRead.Get();dest.pResource=encoded.Get();transition(list.Get(),encoded.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyTextureRegion(&read,0,0,0,&dest,nullptr);
    auto ed=exposure->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT ef{};UINT64 eb{};
    device->GetCopyableFootprints(&ed,0,1,0,&ef,nullptr,nullptr,&eb);auto smallBuffer=buffer;smallBuffer.Width=eb;
    demand(device->CreateCommittedResource(&read_heap,D3D12_HEAP_FLAG_NONE,&smallBuffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&exposureRead)));
    read.pResource=exposureRead.Get();read.PlacedFootprint=ef;dest.pResource=exposure.Get();
    transition(list.Get(),exposure.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyTextureRegion(&read,0,0,0,&dest,nullptr);
    demand(list->Close());codec.end_recording();
    ID3D12CommandList* submitted[]={list.Get()};queue->ExecuteCommandLists(1,submitted);
    ComPtr<ID3D12Fence> fence;demand(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));demand(queue->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);demand(fence->SetEventOnCompletion(1,event));
    auto waited=WaitForSingleObject(event,10000);CloseHandle(event);if(waited!=WAIT_OBJECT_0)throw std::runtime_error("GPU retirement");
    float factor{};D3D12_RANGE er{0,size_t(eb)};demand(exposureRead->Map(0,&er,&mapped));memcpy(&factor,mapped,4);exposureRead->Unmap(0,&empty);
    D3D12_RANGE entire{0,size_t(bytes)};demand(readback->Map(0,&entire,&mapped));unsigned verified{},changedPixels{},comparedPixels{};
    double maximumRelativeError=0;
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
        const auto* actual=static_cast<const char*>(mapped)+footprint.Offset+y*footprint.Footprint.RowPitch+x*16;
        {
            const auto* rgb=reinterpret_cast<const float*>(actual);
            float u=(x+.5f)/width,v=(y+.5f)/height;bool originalSide=false,separator=false,outsideImage=false;
            const float screen=u;
            if(compare==1){bool left=u<.5f;u=left?u*2:(u-.5f)*2;v=(v-.5f)*2+.5f;originalSide=swap!=left;separator=std::abs(screen-.5f)<1.f/width;outsideImage=u<0||u>1||v<0||v>1;}
            else if(compare==2){originalSide=swap!=(u<.5f);separator=std::abs(u-.5f)<1.f/width;}
            float weight=compare==1?std::max(0.f,1-std::abs(u*width-.5f-4))*std::max(0.f,1-std::abs(v*height-.5f-4)):float(x==4&&y==4);
            auto expected=pixel;for(unsigned c=0;c<3;++c)expected[c]=pixel[c]*(1-weight)+patch[c]*weight;
            if(outsideImage)expected={0,0,0,1};if(separator)expected={factor,factor,factor,1};
            bool mustMatch=!edited || originalSide || outsideImage || separator;
            double error{};for(unsigned c=0;c<3;++c){if(!std::isfinite(rgb[c])||rgb[c]<0)throw std::runtime_error("invalid relative colour");error=std::max(error,double(std::abs(rgb[c]-expected[c])/std::max(.001f,expected[c])));}
            if(mustMatch){++comparedPixels;maximumRelativeError=std::max(maximumRelativeError,error);if(error>.0001)throw std::runtime_error("neutral/original/Compare colour changed");}
            else changedPixels+=error>.001;
            if(x==4&&y==4)printf("PATCH context=%s colour=%g original=%g,%g,%g result=%g,%g,%g\n",outside?"bright":"dark",colour,patch[0],patch[1],patch[2],rgb[0],rgb[1],rgb[2]);
        }
++verified;
    }
    readback->Unmap(0,&empty);
    demand(encodedRead->Map(0,&entire,&mapped));double mean{};unsigned clipped{},zero{};
    for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){const char* p=static_cast<const char*>(mapped)+y*footprint.Footprint.RowPitch+x*16;std::array<float,3> rgb{};
        memcpy(rgb.data(),p,12);
        if(x==4&&y==4)printf("ENCODED_PATCH rgb=%g,%g,%g\n",rgb[0],rgb[1],rgb[2]);
        mean+=(rgb[0]+rgb[1]+rgb[2])/3;clipped+=std::max({rgb[0],rgb[1],rgb[2]})>=1;zero+=std::max({rgb[0],rgb[1],rgb[2]})==0;}
    encodedRead->Unmap(0,&empty);mean/=verified;
    printf("EXPOSURE factor=%g input_mean=%g pixels_at_one_percent=%g zero_percent=%g original_max_relative_error=%g\n",factor,mean,100.*clipped/verified,100.*zero/verified,maximumRelativeError);
    if(!(std::isfinite(factor)&&std::isfinite(mean)))throw std::runtime_error("invalid probe values");
    if(edited && !changedPixels)throw std::runtime_error("model edit was suppressed");
    printf("RELATIVE_COLOUR compared=%u changed=%u compare=%u swap=%u PASS\n",comparedPixels,changedPixels,compare,unsigned(swap));
    unsigned errors{};
    if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){SIZE_T size{};messages->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());messages->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;printf("%s\n",m->pDescription);}}
    printf("NR_RELATIVE_COLOUR checked=%u errors=%u PASS\n",verified,errors);return errors?1:0;
}catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
