#include "../src/d3d12/rife_dependency_timing.hpp"
#include <windows.h>
#include <d3d12sdklayers.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <map>
#include <thread>
using Microsoft::WRL::ComPtr;
static void check(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
static void hr(HRESULT v){check(SUCCEEDED(v),"D3D12 operation failed");}
static void wait(ID3D12Fence* f,UINT64 value){HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(e!=nullptr,"event");
    hr(f->SetEventOnCompletion(value,e));const auto r=WaitForSingleObject(e,3000);CloseHandle(e);check(r==WAIT_OBJECT_0,"owned test fence timeout");}
int main(){try{
    ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> completion,progress;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion)));hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress)));
    xrfg::RifeDependencyTiming probe;check(!probe.begin(device.Get(),false,1,1) && !probe.allocated(),"F9 off allocated GPU resources");
    auto folder=std::filesystem::temp_directory_path()/ (L"ofxr-rife-dependency-"+std::to_wstring(GetCurrentProcessId()));std::filesystem::create_directories(folder);
    std::ofstream(folder/L"ofxr_bridge.ini")<<"[diagnostics]\nlogging_enabled=1\nmax_file_mb=1\nflush_each_event=0\n";
    auto& log=xrfg::bridge_flight_logger();log.initialize(folder);check(log.enabled(),"test logging not enabled");const auto file=log.log_path();
    for(unsigned held=0;held<3;++held){
        const UINT64 value=held+1;std::array<ComPtr<ID3D12Fence>,3> dep;
        for(unsigned i=0;i<3;++i)hr(device->CreateFence(i==held?0:1,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&dep[i])));
        check(probe.begin(device.Get(),true,100+held,value),"probe failed to arm");probe.marker(queue.Get(),0);
        for(unsigned i=0;i<3;++i){if(i==held)hr(queue->Signal(progress.Get(),value));hr(queue->Wait(dep[i].Get(),1));probe.marker(queue.Get(),i+1);}
        ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
        hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
        hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));
        list->EndQuery(probe.queries(),D3D12_QUERY_TYPE_TIMESTAMP,4);list->EndQuery(probe.queries(),D3D12_QUERY_TYPE_TIMESTAMP,5);hr(list->Close());
        ID3D12CommandList* raw=list.Get();queue->ExecuteCommandLists(1,&raw);probe.finish(queue.Get(),value,true);hr(queue->Signal(completion.Get(),value));
        check(!probe.begin(device.Get(),true,999,999),"probe reset while GPU was still referencing it");
        wait(progress.Get(),value);std::this_thread::sleep_for(std::chrono::milliseconds(20));hr(dep[held]->Signal(1));wait(completion.Get(),value);
        probe.poll(completion.Get(),queue.Get());check(!probe.pending(),"completed probe did not retire");
    }
    log.flush();log.shutdown();
    auto parse=[](const std::filesystem::path& path){
        std::map<unsigned,std::map<unsigned,std::uint64_t>> rows;std::ifstream input(path);std::string line;
        while(std::getline(input,line))if(line.find("op=gpu_fence_snapshot")!=std::string::npos){
            auto number=[&](const std::string& key){const auto p=line.find(key);return p==std::string::npos?0ULL:std::stoull(line.substr(p+key.size()));};
            const auto code=number("result=");if(code>=86720 && code<=86725)rows[unsigned(number(" a="))][unsigned(code-86720)]=number(" b=");
        }return rows;
    };
    for(const auto& path:{file,xrfg::benchmark_log_path(file)}){
        const auto rows=parse(path);check(rows.size()==3,"missing probe receipts in flight/sidecar");
        for(unsigned held=0;held<3;++held){const auto& marks=rows.at(100+held);check(marks.size()==6,"missing GPU boundary timestamp");
            for(unsigned i=1;i<6;++i)check(marks.at(i)>=marks.at(i-1),"GPU markers out of order");
            const auto us=marks.at(held+1)-marks.at(held);check(us>=15000,"held dependency was not measured in the correct GPU interval");
            std::cout<<"dependency="<<held<<" gpu_wait_us="<<us<<" PASS\n";
        }
    }
    ComPtr<ID3D12InfoQueue> messages;if(SUCCEEDED(device.As(&messages)))for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
        SIZE_T n{};messages->GetMessage(i,nullptr,&n);std::vector<std::byte> storage(n);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());hr(messages->GetMessage(i,m,&n));
        check(m->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && m->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,"D3D12 validation error");}
    std::cout<<"F9-off allocation, pending lifetime, A/B/acquire GPU attribution and both log outputs PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
