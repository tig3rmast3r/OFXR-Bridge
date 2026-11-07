#include "osr_full_warp_pass.hpp"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <chrono>
using Microsoft::WRL::ComPtr;
void ok(HRESULT h,const char* message){if(FAILED(h))throw std::runtime_error(message);}
struct Bench {
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> alloc;
 ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;ComPtr<ID3D12QueryHeap> queries;
 ComPtr<ID3D12Resource> timing,color,depth,motion;std::vector<ComPtr<ID3D12Resource>> uploads;
 xrfg::OsrFullWarp normal,lower;xrfg::OsrFullWarp::Inputs inputs{};
 UINT w{},h{},dw{},dh{};UINT64 serial{},frequency{};HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
 Bench(const std::filesystem::path& path){
  std::ifstream c(path/"config.txt");c>>w>>h>>dw>>dh;
  if(!c||w<16||h<16)throw std::runtime_error("input configuration");
  for(float* m:{inputs.forward_stereo,inputs.backward_source,inputs.forward_history,inputs.backward_history,inputs.forward_history_previous})for(unsigned i=0;i<16;++i)c>>m[i];
  c>>inputs.source_dejitter[0]>>inputs.source_dejitter[1];
  inputs.history_dejitter[0]=inputs.source_dejitter[0];inputs.history_dejitter[1]=inputs.source_dejitter[1];
  inputs.history_motion_scale[0]=inputs.history_motion_scale[1]=1;
  if(std::getenv("OFXR_OSR_BENCH_DEBUG")){ComPtr<ID3D12Debug> debug;ok(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"debug layer");debug->EnableDebugLayer();}
  ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"GPU device");
  ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
  if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))&&SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))){adapter->GetDesc1(&desc);std::wcout<<L"GPU="<<desc.Description<<L"\n";}
  D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ok(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
  ok(queue->GetTimestampFrequency(&frequency),"timestamp frequency");ok(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&alloc)),"allocator");
  ok(device->CreateCommandList(0,q.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)),"list");ok(list->Close(),"initial list");
  ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
  D3D12_QUERY_HEAP_DESC query{};query.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;query.Count=16;
  ok(device->CreateQueryHeap(&query,IID_PPV_ARGS(&queries)),"query heap");timing=buffer(16*8,D3D12_HEAP_TYPE_READBACK);
  color=texture(w,h,DXGI_FORMAT_R8G8B8A8_UNORM);depth=texture(dw,dh,DXGI_FORMAT_R32_FLOAT);motion=texture(dw,dh,DXGI_FORMAT_R16G16_FLOAT);
  begin();upload(color.Get(),path/"color.rgba");upload(depth.Get(),path/"depth.f32");upload(motion.Get(),path/"motion.f16");submit();uploads.clear();
  inputs.source_color=color.Get();inputs.source_depth=depth.Get();inputs.source_motion=motion.Get();
  ok(normal.prepare(device.Get(),dw,dh,w,h,DXGI_FORMAT_R8G8B8A8_UNORM),"normal prepare");
  ok(lower.prepare(device.Get(),dw,dh,w,h,DXGI_FORMAT_R8G8B8A8_UNORM),"lower prepare");
 }
 ~Bench(){CloseHandle(event);}
 ComPtr<ID3D12Resource> buffer(UINT64 bytes,D3D12_HEAP_TYPE kind){
  D3D12_HEAP_PROPERTIES heap{};heap.Type=kind;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;ok(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,kind==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"buffer");return r;
 }
 ComPtr<ID3D12Resource> texture(UINT width,UINT height,DXGI_FORMAT format){
  D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;
  ComPtr<ID3D12Resource> r;ok(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)),"texture");return r;
 }
 void barrier(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&t);}
 void begin(){ok(alloc->Reset(),"allocator reset");ok(list->Reset(alloc.Get(),nullptr),"list reset");}
 void submit(){ok(list->Close(),"close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);ok(queue->Signal(fence.Get(),++serial),"signal");ok(fence->SetEventOnCompletion(serial,event),"completion");if(WaitForSingleObject(event,15000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");}
 void upload(ID3D12Resource* r,const std::filesystem::path& file){
  const auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
  auto u=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD);std::ifstream f(file,std::ios::binary);std::vector<char> data{std::istreambuf_iterator<char>(f),{}};
  if(data.size()!=d.Width*d.Height*4)throw std::runtime_error("input bytes");void* mapped{};ok(u->Map(0,nullptr,&mapped),"upload map");
  for(UINT y=0;y<d.Height;++y)std::memcpy(static_cast<char*>(mapped)+fp.Offset+y*fp.Footprint.RowPitch,data.data()+y*d.Width*4,d.Width*4);u->Unmap(0,nullptr);
  barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=r;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.pResource=u.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=fp;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
  barrier(r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);uploads.push_back(u);
 }
 void record(bool low,unsigned frame,UINT base,bool history){
  auto in=inputs;in.source_eye=frame&1;in.use_history=history;in.use_history_motion=history;in.lower_quality=low;
  // Optional paired OLD/CURRENT-history cost comparison at the SAME quality.
  // Test-only switch, never a runtime setting. Uses identity synthetic cameras.
  if(std::getenv("OFXR_OSR_BENCH_CURRENT_HISTORY")) {
   in.use_current_history_motion=history && low;
   in.lower_quality=std::getenv("OFXR_OSR_BENCH_CURRENT_LOWER")!=nullptr;
   for(unsigned m=0;m<3;++m)for(unsigned i=0;i<4;++i)in.current_history_data[m*16+i*5]=1;
   in.current_history_data[50]=in.current_history_data[54]=1;
   in.current_history_data[56]=in.current_history_data[57]=1;
  }
  auto& warp=low?lower:normal;warp.record(device.Get(),list.Get(),in,queries.Get(),base);
  xrfg::OsrFullHistory kept{};kept.publication=frame+1;kept.motion_scale[0]=kept.motion_scale[1]=1;
  kept.dejitter[0]=in.source_dejitter[0];kept.dejitter[1]=in.source_dejitter[1];
  warp.record_history_update(list.Get(),in,kept);list->EndQuery(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,base+6);
 }
 void output(bool low,const std::filesystem::path& file){
  auto* r=(low?lower:normal).output();const auto d=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
  device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);auto read=buffer(bytes,D3D12_HEAP_TYPE_READBACK);
  begin();barrier(r,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);D3D12_TEXTURE_COPY_LOCATION src{},dst{};
  src.pResource=r;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.pResource=read.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=fp;
  list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);barrier(r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);submit();
  void* data{};ok(read->Map(0,nullptr,&data),"output map");std::ofstream f(file,std::ios::binary);
  for(UINT y=0;y<h;++y)f.write(static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch,w*4);read->Unmap(0,nullptr);
 }
 void validate(){
  if(!std::getenv("OFXR_OSR_BENCH_DEBUG"))return;
  ComPtr<ID3D12InfoQueue> info;ok(device.As(&info),"info queue");unsigned errors=0;
  for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;std::cerr<<m->pDescription<<"\n";}}
  if(errors)throw std::runtime_error("D3D12 validation errors");std::cout<<"D3D12_DEBUG_ZERO_ERRORS PASS\n";
 }
 void run(const std::filesystem::path& path,unsigned repeats,bool history){
  std::ofstream csv(path/(history?"timing-history.csv":"timing-stereo.csv"));csv<<"iteration,lower,total_ms,kernels_ms,clear_ms,splat_ms,history_ms,resolve_ms,inpaint_ms,history_copy_ms\n";
  for(unsigned frame=0;frame<repeats+12;++frame){
   begin();const bool reverse=(frame&2)!=0;record(reverse,frame,0,history);record(!reverse,frame,8,history);
   list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,7,timing.Get(),0);list->ResolveQueryData(queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,8,7,timing.Get(),64);submit();normal.commit();lower.commit();
   UINT64* ticks{};ok(timing->Map(0,nullptr,reinterpret_cast<void**>(&ticks)),"timings map");
   if(frame>=12)for(unsigned block=0;block<2;++block){const unsigned base=block*8;const auto ms=[&](unsigned a,unsigned b){return double(ticks[base+b]-ticks[base+a])*1000./double(frequency);};
    csv<<frame-12<<','<<(block==0?reverse:!reverse)<<','<<ms(0,6)<<','<<ms(0,5);for(unsigned j=0;j<6;++j)csv<<','<<ms(j,j+1);csv<<'\n';}
   timing->Unmap(0,nullptr);
  }
  output(false,path/(history?"normal-history.rgba":"normal-stereo.rgba"));output(true,path/(history?"lower-history.rgba":"lower-stereo.rgba"));
 }
};
int main(int argc,char** argv){try{if(argc<2)throw std::runtime_error("input directory required");const std::filesystem::path path=argv[1];Bench bench(path);const unsigned count=argc>2?std::stoul(argv[2]):48;bench.run(path,count,false);bench.run(path,count,true);bench.validate();std::cout<<"OSR_GPU_BENCH_COMPLETE "<<bench.w<<'x'<<bench.h<<" full size colour; paired alternating order; "<<count<<" samples per mode PASS\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
