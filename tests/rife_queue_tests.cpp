// Real D3D12/CUDA interop, with both completion policies in one process.
#define main xrfg_history_tests_unused_entry
#include "d3d12_history_tests.cpp"
#undef main
#include <numeric>
#include <iomanip>

namespace {
using Completion=xrfg::D3D12Rife::Completion;
struct Eye {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>,2> input;
    std::array<ComPtr<ID3D12Resource>,3> output;
    std::unique_ptr<xrfg::D3D12Rife> generator;
    std::array<xrfg::D3D12ReprojectionView,1> view;
    std::array<std::array<float,4>,1> rotation{{{0,0,0,1}}};
    std::uint64_t serial{};
    void batch(unsigned factor,Completion policy,bool fractional_batch=false) {
        // Every fraction has its own camera: a batching implementation that
        // accidentally reuses one packed pair must fail the pixel comparison.
        std::array<std::array<std::array<float,4>,1>,3> ar,br;
        for(unsigned i=0;i<factor-1;++i){
            const float angle=.015F*(i+1)/factor;
            ar[i][0]={0,std::sin(angle),0,std::cos(angle)};
            br[i][0]={0,-std::sin(angle*.7F),0,std::cos(angle*.7F)};
        }
        if(fractional_batch){
            std::array<xrfg::D3D12Rife::Output,3> targets;
            for(unsigned i=0;i<factor-1;++i)targets[i]={output[i].Get(),view,ar[i],br[i],float(i+1)/factor,{}};
            require_hresult(generator->generate_batch(input[0].Get(),input[1].Get(),view,view,{targets.data(),factor-1},D3D12_RESOURCE_STATE_RENDER_TARGET,policy),"batched fractional interop");
        }else for(unsigned i=0;i<factor-1;++i)
            require_hresult(generator->generate(input[0].Get(),input[1].Get(),output[i].Get(),
                view,view,view,ar[i],br[i],D3D12_RESOURCE_STATE_RENDER_TARGET,
                float(i+1)/factor,{},policy),"fractional interop");
        const auto value=++serial;
        require_hresult(queue->Signal(fence.Get(),value),"caller publication fence");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"receipt event");
        const auto hr=fence->SetEventOnCompletion(value,event);
        const auto waited=SUCCEEDED(hr)?WaitForSingleObject(event,30000):WAIT_FAILED;
        CloseHandle(event);require_hresult(hr,"receipt registration");
        require(waited==WAIT_OBJECT_0,"caller receipt did not retire final compose");
    }
};
void fill(D3D12WarpFixture& f,ID3D12Resource* resource,bool later,unsigned eye){
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> heap;
    require_hresult(f.device()->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)),"pattern RTV heap");
    const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();f.device()->CreateRenderTargetView(resource,nullptr,rtv);
    f.execute_and_wait([&](ID3D12GraphicsCommandList* list){
        const float background[]{.08F+eye*.1F,.2F,.3F,1};
        const float object[]{.8F,.3F+eye*.1F,.1F,1};
        list->ClearRenderTargetView(rtv,background,0,nullptr);
        const auto d=resource->GetDesc();const LONG x=static_cast<LONG>(d.Width/3)+(later?9:0);
        const D3D12_RECT rect{x,0,x+static_cast<LONG>(d.Width/3),static_cast<LONG>(d.Height)};
        list->ClearRenderTargetView(rtv,object,1,&rect);
        const auto b=transition_barrier(resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1,&b);
    });
}
int test_queue(D3D12WarpFixture& fixture){
    constexpr UINT width=4396,height=4028;
    std::array<Eye,2> eyes;
    for(unsigned i=0;i<eyes.size();++i){
        auto& e=eyes[i];D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        require_hresult(fixture.device()->CreateCommandQueue(&q,IID_PPV_ARGS(&e.queue)),"independent eye queue");
        require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&e.fence)),"eye fence");
        for(unsigned j=0;j<2;++j){e.input[j]=create_single_slice_texture(fixture,width,height);fill(fixture,e.input[j].Get(),j!=0,i);}
        for(auto& out:e.output)out=create_single_slice_texture(fixture,width,height);
        e.view[0]=make_reprojection_views()[0];e.view[0].array_slice=0;e.view[0].image_rect={0,0,width,height};
        e.generator=std::make_unique<xrfg::D3D12Rife>();
        require_hresult(e.generator->initialize(fixture.device(),e.queue.Get(),kFormat,3,.1F),"RIFE4.4 Potato");
    }
    auto batch=[&](unsigned factor,Completion policy,bool fractional_batch=false){
        const auto started=std::chrono::steady_clock::now();
        auto a=std::async(std::launch::async,[&]{eyes[0].batch(factor,policy,fractional_batch);});
        auto b=std::async(std::launch::async,[&]{eyes[1].batch(factor,policy,fractional_batch);});
        a.get();b.get();
        return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    };
    batch(3,Completion::wait);
    const auto state=xrfg::rife_execution_status();
    require(!state.error,"installed TensorRT backend reported an error");
    if(state.optimizing){std::cout<<"SKIP: prepare a440x403 single-eye RIFE4.4 engine before this compiled-only benchmark\n";return 77;}
    require(state.tensorrt,"compiled-only queue test must not benchmark fallback");
    const auto folder=std::filesystem::temp_directory_path()/ (L"ofxr-fraction-proof-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    std::ofstream(folder/L"ofxr_bridge.ini")<<"[diagnostics]\nlogging_enabled=1\nmax_file_mb=1\nflush_each_event=0\n";
    auto& logger=xrfg::bridge_flight_logger();logger.initialize(folder);require(logger.enabled(),"fractional proof logger");
    const auto proof_path=logger.log_path();batch(3,Completion::queue_ordered,true);logger.flush();logger.shutdown();
    std::ifstream proof(proof_path);unsigned batched=0;
    for(std::string line;std::getline(proof,line);)if(line.find("op=rife_backend result=5")!=std::string::npos&&line.find("c=4294967298")!=std::string::npos)++batched;
    require(batched==2,"worker capability missing or fractional batch silently fell back");
    std::cout<<"QUEUE_BATCH_PROOF compiled_batches="<<batched<<" PASS\n";
    for(unsigned factor:{2U,3U,4U}){
        batch(factor,Completion::wait);
        std::array<std::vector<std::vector<std::uint8_t>>,2> expected;
        for(unsigned eye=0;eye<2;++eye)for(unsigned i=0;i<factor-1;++i)
            expected[eye].push_back(readback_single_slice(fixture,eyes[eye].output[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
        batch(factor,Completion::queue_ordered,true);
        for(unsigned eye=0;eye<2;++eye)for(unsigned i=0;i<factor-1;++i)
            require(expected[eye][i]==readback_single_slice(fixture,eyes[eye].output[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),
                "deferred compose changed pixels or publication fence released an unfinished output");
        fixture.require_no_debug_errors();
        std::cout<<"QUEUE_PIXELS factor="<<factor<<" both_eyes=exact PASS\n";
    }
    std::array<std::vector<double>,2> times;
    for(unsigned round=0;round<32;++round){
        // ABBA to avoid comparing a cold first run with a warm second run.
        const auto mode=(round/2+round%2)%2;
        const auto value=batch(3,Completion::queue_ordered,mode!=0);
        times[mode].push_back(value);
        std::cout<<"QUEUE_SAMPLE mode="<<mode<<" ms="<<value<<'\n';
    }
    for(unsigned mode=0;mode<2;++mode){
        auto v=times[mode];std::sort(v.begin(),v.end());
        std::cout<<"QUEUE_BENCH mode="<<mode<<" n="<<v.size()<<" mean_ms="<<std::accumulate(v.begin(),v.end(),0.0)/v.size()
            <<" median_ms="<<(v[v.size()/2]+v[v.size()/2-1])*.5<<" p95_ms="<<v[static_cast<size_t>((v.size()-1)*.95)]<<'\n';
    }
    // Retain the last deferred output through destruction, without the caller's
    // publication fence. The generator's own tail receipt must drain it.
    for(auto& e:eyes){
        require_hresult(e.generator->generate(e.input[0].Get(),e.input[1].Get(),e.output[0].Get(),e.view,e.view,e.view,
            e.rotation,e.rotation,D3D12_RESOURCE_STATE_RENDER_TARGET,.5F,{},Completion::queue_ordered),"deferred destructor submission");
        e.generator.reset();
        const auto pixels=readback_single_slice(fixture,e.output[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(!pixels.empty(),"deferred destruction lost output");
    }
    fixture.require_no_debug_errors();
    std::cout<<"QUEUE_DESTRUCTION PASS\n";
    // A real worker protocol rejection after a queued compose must not free
    // resources still referenced by that earlier submission.
    auto& e=eyes[0];
    const auto expected=readback_single_slice(fixture,e.output[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    e.generator=std::make_unique<xrfg::D3D12Rife>();
    require_hresult(e.generator->initialize(fixture.device(),e.queue.Get(),kFormat,3,.1F),"fault fixture initialize");
    require_hresult(e.generator->generate(e.input[0].Get(),e.input[1].Get(),e.output[0].Get(),e.view,e.view,e.view,
        e.rotation,e.rotation,D3D12_RESOURCE_STATE_RENDER_TARGET,.5F,{},Completion::queue_ordered),"compose before rejection");
    const auto refused=e.generator->generate(e.input[0].Get(),e.input[1].Get(),e.output[1].Get(),e.view,e.view,e.view,
        e.rotation,e.rotation,D3D12_RESOURCE_STATE_RENDER_TARGET,1.F,{},Completion::queue_ordered);
    require(FAILED(refused),"invalid fraction must actually inject a worker failure");
    e.generator.reset();
    require(expected==readback_single_slice(fixture,e.output[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET),
        "worker failure invalidated the preceding deferred compose");
    fixture.require_no_debug_errors();std::cout<<"QUEUE_PARTIAL_FAILURE PASS\n";
    return 0;
}
}
int main(){try{D3D12WarpFixture fixture(true);return test_queue(fixture);}
    catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}}
