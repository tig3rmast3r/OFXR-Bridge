#include "xrfg/private_openxr_runtime.hpp"
#include "xrfg/private_refresh_wait_policy.hpp"
#include "xrfg/interface_metrics.hpp"
#include "xrfg/private_delivery_queue.hpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/owned_d11_output.hpp"
#include <wrl/client.h>
#include <dxgi1_4.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono_literals;
namespace {
void check(bool condition,const char* message){if(!condition){std::fprintf(stderr,"FAIL: %s\n",message);std::fflush(stderr);std::_Exit(1);}}
void ok(HRESULT result,const char* message){check(SUCCEEDED(result),message);}
void xr_ok(XrResult result,const char* message){if(XR_FAILED(result))std::fprintf(stderr,"XR result=%d: %s\n",result,message);check(XR_SUCCEEDED(result),message);}
std::int64_t ns(){LARGE_INTEGER q{},f{};QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);return q.QuadPart/f.QuadPart*1'000'000'000LL+q.QuadPart%f.QuadPart*1'000'000'000LL/f.QuadPart;}
std::int64_t period=11'111'111;
constexpr std::int64_t epoch=1'000'000'000'000LL;
bool native_contract_fixture_mode{},native_adaptive_wait_fixture_mode{};
bool native_director_only_fixture_mode{};
struct Chain {
    std::vector<ComPtr<ID3D12Resource>> d12;
    std::vector<ComPtr<ID3D11Texture2D>> d11;
    unsigned acquired{},released{};bool owned{},waited{};
    bool static_image{};unsigned acquisitions{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_RENDER_TARGET};
};
struct Fixture;
Fixture* current;
struct Fixture {
    bool native12;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandQueue> delivery_queue;
    ComPtr<ID3D12Fence> fence,gate;
    std::uint64_t serial{};
    HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    HANDLE preparation_timer{CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS)}; // high resolution
    HANDLE frame_wait_timer{CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS)};
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context;
    std::unordered_map<XrSwapchain,Chain> chains;
    std::uintptr_t next_handle{100};
    std::recursive_mutex gpu;
    std::mutex mutex,protocol;
    std::condition_variable changed;
    std::atomic_bool block_end{true},end_entered{},fail_end{};
    std::atomic_uint real_acquires{},real_waits{},real_begins{},real_ends{};
    std::atomic_uint refuse_end_call{},refuse_end_count{1};
    std::atomic<std::int64_t> native_wait_tick_ns{}; // layer fixture's throttling runtime
    bool cycle_waited{},cycle_begun{};
    unsigned successful_waits{};
    std::atomic_bool throttled{true};
    bool real_common{},reuse_current_real_image{};
    std::atomic_int slow_image_wait_ms{};
    std::atomic_int slow_frame_wait_ms{};
    std::atomic_uint fail_wait_call{},fail_begin_call{};
    std::function<int(unsigned)> frame_wait_us;
    std::atomic<DWORD> preparation_thread{},delivery_thread{},wait_thread{};
    std::atomic_uint empty_ends{};
    std::atomic<std::uint64_t> wait_input_ordinal{},begun_input_ordinal{};
    std::atomic_uint discarded_clocks{};
    std::atomic_bool stale_next_clock{};
    // Waits [stall_from,stall_from+stall_count) block stall_us and return an
    // already expired cycle, as SteamVR with VRto3D did on a 165Hz 3D monitor.
    std::atomic_uint stall_from{},stall_count{},stalled_waits{},stalled_traces{};
    std::atomic_int stall_us{};
    std::atomic_bool allow_expired_end{};
    std::atomic_bool native_should_render{true};
    std::atomic<std::int64_t> prediction_horizon_ns{};
    std::unordered_map<std::uint64_t,unsigned> window_renewals;
    std::unordered_map<std::uint64_t,std::int64_t> copy_ready_times,native_wait_entries;
    std::vector<std::int64_t> native_windows;
    unsigned expected_views{1};
    unsigned native_image_count{2};
    std::function<XrResult(XrSwapchain,unsigned)> release_callback;
    std::function<void(unsigned)> begin_callback;
    std::function<void()> after_head_ready;
    std::function<void(std::uint64_t)> preparation_callback;
    std::function<void(std::uint64_t)> prepared_callback;
    std::function<void(XrSwapchain,unsigned)> image_wait_callback;
    std::function<void(const XrFrameEndInfo*)> inspect_layers;
    std::function<D3D12_RESOURCE_STATES()> create_state;
    std::vector<unsigned> pixels,overlay_pixels,right_pixels;
    std::vector<std::array<XrSwapchain,2>> stereo_handles;
    std::size_t native_chain_limit{1000};
    std::vector<float> positions;
    std::vector<std::array<float,2>> panel_positions;
    std::vector<std::int64_t> handoffs,submit_receipts;
    std::vector<bool> director_policies;
    std::int64_t simulated_submit_ns{};
    std::function<int(unsigned)> end_work_us;
    std::vector<std::int64_t> buffer_reserves,buffer_refills;
    std::function<void(int,std::uint64_t,std::int64_t)> diagnostic_work;
    std::atomic_uint copy_clock_receipts{};
    std::vector<XrTime> targets;
    XrTime raw_prediction{},begun_prediction{};
    unsigned advanced_predictions{};
    std::int64_t wait_return{},begun_wait_return{},prediction_age_limit{};
    std::vector<std::int64_t> prediction_ages;
    std::vector<std::int64_t> refill_margins;
    std::vector<std::int64_t> native_margins;
    std::vector<std::int64_t> work_margins;
    std::atomic_uint source_ready_events{},pending_source_events{};
    std::int64_t physical_origin{};
    std::atomic_bool physical_clock_enabled{true};
    std::shared_ptr<xrfg::PrivateOpenXrRuntime> transport;
    explicit Fixture(bool d12,bool split=true,std::function<int(unsigned)> variable_wait={},bool physical_clock=false,bool diagnostics=true,bool release_readiness=false,std::size_t capacity=3,bool adaptive_buffer=false):native12(d12),frame_wait_us(std::move(variable_wait)) {
        current=this;
        if(d12) {
            ok(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)),"actual D3D12 device");
            D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ok(device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"queue");
            if(split) {
                D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY priority{desc.Type,D3D12_COMMAND_QUEUE_PRIORITY_HIGH,FALSE};
                const bool high=SUCCEEDED(device12->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY,&priority,sizeof(priority))) && priority.PriorityForTypeIsSupported;
                ok(xrfg::create_private_delivery_queue(desc,high,[&](const D3D12_COMMAND_QUEUE_DESC& requested){return device12->CreateCommandQueue(&requested,IID_PPV_ARGS(delivery_queue.ReleaseAndGetAddressOf()));}),"independent priority delivery queue");
            }
            ok(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"test fence");
            ok(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"GPU gate");
        } else {
            D3D_FEATURE_LEVEL level{};
            ok(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device11,&level,&context),"actual D3D11 device");
        }
        xrfg::PrivateOpenXrRuntime::Options options;
        options.real={reinterpret_cast<XrInstance>(1),reinterpret_cast<XrSession>(2),wait,begin,end,create,destroy,enumerate,acquire,wait_image,release,to_qpc};
        options.device12=device12.Get();options.queue12=delivery_queue?delivery_queue.Get():queue.Get();options.producer_queue12=queue.Get();options.device11=device11.Get();options.context11=context.Get();
        options.d11_release_readiness=release_readiness;
        options.physical_period=[]{return period;};options.capacity=capacity;
        options.adaptive_buffer=adaptive_buffer;
        options.native_frame_contract=native_contract_fixture_mode;
        options.native_adaptive_wait_lookahead=native_adaptive_wait_fixture_mode;
        options.native_director_only=native_director_only_fixture_mode;
        if(physical_clock) {
            physical_origin=ns();
            options.physical_clock=[this]() -> std::optional<xrfg::PrivatePhysicalClock> {
                if(!physical_clock_enabled)return {};
                const auto n=std::max(std::int64_t{},(ns()-physical_origin)/period);
                return xrfg::PrivatePhysicalClock{physical_origin+n*period,period,1000+static_cast<std::uint64_t>(n)};
            };
        }
        options.trace=[this](int stage,std::uint64_t seq,std::int64_t at,std::int64_t margin){if(stage==5)copy_ready_times[seq]=ns();if(stage==7)native_wait_entries[seq]=at;if(stage==10){if(margin)++pending_source_events;++source_ready_events;}if(stage==19)work_margins.push_back(margin);if(stage==11)refill_margins.push_back(margin);if(stage==17)native_margins.push_back(margin);if(stage==18)wait_input_ordinal=seq;if(stage==25)++window_renewals[seq];if(stage==45)++stalled_traces;if(stage==4) {preparation_thread=GetCurrentThreadId();if(after_head_ready)after_head_ready();if(preparation_callback)preparation_callback(seq);}if(stage==5 && prepared_callback)prepared_callback(seq);};
        const auto original_trace=options.trace;
        options.trace=[this,original_trace](int stage,std::uint64_t seq,std::int64_t at,std::int64_t margin) {
            original_trace(stage,seq,at,margin);
            if(diagnostic_work)diagnostic_work(stage,seq,at);
            if(stage==32){check(at>0 && margin>=at,"GPU copy timestamps calibrated and ordered");++copy_clock_receipts;}
            if(stage==29)buffer_reserves.push_back(at);
            if(stage==30)buffer_refills.push_back(margin);
        };
        if(!diagnostics)options.trace_enabled=[]{return false;};
        transport=std::make_shared<xrfg::PrivateOpenXrRuntime>(std::move(options));
        ok(transport->initialize(),"initialize actual private endpoint");xr_ok(transport->start(),"start endpoint");
    }
    ~Fixture(){block_end=false;changed.notify_all();if(gate)gate->Signal(1);if(transport)transport->stop();transport.reset();CloseHandle(event);if(preparation_timer)CloseHandle(preparation_timer);if(frame_wait_timer)CloseHandle(frame_wait_timer);current=nullptr;}
    void gpu_wait(ID3D12CommandQueue* target=nullptr) {
        if(!target)target=queue.Get();const auto expected=++serial;
        ok(target->Signal(fence.Get(),expected),"test signal");
        while(fence->GetCompletedValue()<expected) {
            ok(fence->SetEventOnCompletion(expected,event),"test event");
            check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"test GPU completion event");
        }
        check(fence->GetCompletedValue()!=UINT64_MAX,"test GPU device remains valid");
    }
    template<class Record> void commands(Record record,ID3D12CommandQueue* target=nullptr) {
        // Guide publication can arrive without the fixture's outer GPU lock.
        // One shared proof fence/event must serialize both producer/native queues.
        std::scoped_lock lock(gpu);
        if(!target)target=queue.Get();
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        ok(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"test allocator");
        ok(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"test list");
        record(list.Get());ok(list->Close(),"test close");ID3D12CommandList* lists[]={list.Get()};target->ExecuteCommandLists(1,lists);gpu_wait(target);
    }
    unsigned read_pixel(Chain& chain,unsigned x=0,unsigned y=0,unsigned slice=0) {
        unsigned color{};
        if(native12) {
            auto source=chain.d12[chain.released];D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
            ok(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
            commands([&](ID3D12GraphicsCommandList* list){
                D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,chain.state,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);
                D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=slice;
                D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};D3D12_BOX box{x,y,0,x+1,y+1,1};list->CopyTextureRegion(&to,0,0,0,&from,&box);
                std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
            },delivery_queue.Get());
            void* data{};ok(readback->Map(0,nullptr,&data),"map readback");color=*static_cast<unsigned char*>(data);readback->Unmap(0,nullptr);
        } else {
            auto source=chain.d11[chain.released];D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> readback;
            ok(device11->CreateTexture2D(&desc,nullptr,&readback),"D11 readback");context->CopyResource(readback.Get(),source.Get());D3D11_MAPPED_SUBRESOURCE mapped{};ok(context->Map(readback.Get(),slice,D3D11_MAP_READ,0,&mapped),"D11 map");color=*(static_cast<unsigned char*>(mapped.pData)+y*mapped.RowPitch+x*4);context->Unmap(readback.Get(),slice);
        }
        return color;
    }
    static XrResult XRAPI_CALL wait(XrSession,const XrFrameWaitInfo*,XrFrameState* output) {
        auto& f=*current;f.wait_thread=GetCurrentThreadId();const auto n=++f.real_waits;
        if(f.fail_wait_call==n)return XR_ERROR_RUNTIME_FAILURE;
        const bool stalled=f.stall_count && n>=f.stall_from && n<f.stall_from+f.stall_count;
        if(stalled)++f.stalled_waits;
        const auto delay=stalled?f.stall_us.load():f.frame_wait_us?f.frame_wait_us(n):f.slow_frame_wait_ms.load()*1000;
        if(delay){LARGE_INTEGER due{};due.QuadPart=-delay*10LL;check(f.frame_wait_timer && SetWaitableTimer(f.frame_wait_timer,&due,0,nullptr,nullptr,FALSE),"precise runtime delay");check(WaitForSingleObject(f.frame_wait_timer,1000)==WAIT_OBJECT_0,"runtime delay completed");}
        std::scoped_lock protocol(f.protocol);
        check(!f.cycle_waited && f.real_begins==f.successful_waits,"one real Wait, after prior Begin; it may overlap prior End");++f.successful_waits;f.cycle_waited=true;
        // Vary the runtime horizon while retaining the monotonic prediction
        // required by OpenXR. XR time deliberately has another epoch than QPC.
        if(n>1 && (stalled || f.stale_next_clock.exchange(false)))f.raw_prediction+=period;
        else f.raw_prediction=std::max(f.raw_prediction+1,epoch+ns()+(f.prediction_horizon_ns?f.prediction_horizon_ns.load():((!f.throttled || n%2)?period:period*3)));
        output->predictedDisplayTime=f.raw_prediction;output->predictedDisplayPeriod=(!f.throttled || n%2)?period:period*2;output->shouldRender=f.native_should_render?XR_TRUE:XR_FALSE;f.wait_return=ns();return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL begin(XrSession,const XrFrameBeginInfo*) {
        auto& f=*current;bool discarded{};unsigned ordinal{};
        {
            std::scoped_lock protocol(f.protocol);
            check(f.cycle_waited,"real Begin follows exactly one successful Wait");
            if(f.fail_begin_call==f.real_begins+1) {
                f.fail_begin_call=0;return XR_ERROR_RUNTIME_FAILURE;
            }
            discarded=f.cycle_begun;if(discarded)++f.discarded_clocks;
            f.cycle_waited=false;f.cycle_begun=true;f.begun_prediction=f.raw_prediction;
            f.begun_wait_return=f.wait_return;
            f.begun_input_ordinal=std::max(f.begun_input_ordinal.load(),f.wait_input_ordinal.load());
            ordinal=++f.real_begins;
        }
        if(f.begin_callback)f.begin_callback(ordinal);
        return discarded?XR_FRAME_DISCARDED:XR_SUCCESS;
    }
    static XrResult XRAPI_CALL end(XrSession,const XrFrameEndInfo* info) {
        // Timestamp native entry before fixture validation/bookkeeping, so its
        // work cannot masquerade as an early subsequent runtime handoff.
        const auto start=ns();
        auto& f=*current;
        const auto end_work=f.simulated_submit_ns+(f.end_work_us?std::int64_t(f.end_work_us(f.real_ends.load()))*1000:0);
        if(info->layerCount && end_work) {
            // Model work inside native End before SteamVR receives both eyes.
            while(ns()<start+end_work)YieldProcessor();
            f.submit_receipts.push_back(ns());
        }
        XrTime prediction{};std::int64_t predicted_at{};
        {std::scoped_lock protocol(f.protocol);check(f.cycle_begun,"real End follows real Begin even if next Wait completed");f.cycle_begun=false;prediction=f.begun_prediction;predicted_at=f.begun_wait_return;}
        if(!info->layerCount){++f.empty_ends;return XR_SUCCESS;}
        f.native_windows.push_back(prediction-epoch-start);
        if(prediction<=epoch+start)std::fprintf(stderr,
            "Expired native: D12=%d director=%d content=%u prediction_lead=%.6fms age=%.6fms waits=%u begins=%u discards=%u image_wait=%dms\n",
            f.native12,f.transport?f.transport->director_enabled():false,f.real_ends.load(),
            (prediction-epoch-start)/1e6,(start-predicted_at)/1e6,f.real_waits.load(),f.real_begins.load(),
            f.discarded_clocks.load(),f.slow_image_wait_ms.load());
        check(f.allow_expired_end || prediction>epoch+start,"content requires a current native cycle, not a timestamp advanced on an expired cycle");
        if(f.prediction_age_limit && f.real_ends>=2) {
            f.prediction_ages.push_back(start-predicted_at);
            if(start-predicted_at>f.prediction_age_limit)std::fprintf(stderr,"freshness age%.3fms, limit%.3fms\n",(start-predicted_at)/1e6,f.prediction_age_limit/1e6);
            check(start-predicted_at<=f.prediction_age_limit,"ready output must use a fresh runtime cycle rather than age its prediction through the refresh wait");
        }
        f.delivery_thread=GetCurrentThreadId();
        if(info->layerCount){std::unique_lock lock(f.mutex);f.end_entered=true;f.changed.notify_all();f.changed.wait(lock,[&]{return !f.block_end.load();});}
        check(info->displayTime>=prediction && (info->displayTime-prediction)%period==0,"End uses its own runtime prediction, advancing whole physical periods only if expired");
        if(info->displayTime>prediction)++f.advanced_predictions;
        unsigned color{},overlay_color{};bool has_overlay{};float position{};
        if(info->layerCount) {
            auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[0]);
            check(p->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION && p->viewCount==f.expected_views,"owned projection metadata");
            auto& chain=f.chains.at(p->views[0].subImage.swapchain);position=p->views[0].pose.position.x;
            std::scoped_lock lock(f.gpu);
            const auto& left=p->views[0].subImage;
            color=f.read_pixel(chain,left.imageRect.offset.x,left.imageRect.offset.y,left.imageArrayIndex);
            if(p->viewCount==2) {
                const auto& right=p->views[1].subImage;
                if(left.swapchain!=right.swapchain)check(left.imageArrayIndex==0 && right.imageArrayIndex==0,"separate eyes retain array domain");
                f.right_pixels.push_back(f.read_pixel(f.chains.at(right.swapchain),right.imageRect.offset.x,right.imageRect.offset.y,right.imageArrayIndex));
                f.stereo_handles.push_back({p->views[0].subImage.swapchain,p->views[1].subImage.swapchain});
            }
            if(info->layerCount>1) {
                auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[1]);
                check(quad->type==XR_TYPE_COMPOSITION_LAYER_QUAD,"owned overlay metadata");
                overlay_color=f.read_pixel(f.chains.at(quad->subImage.swapchain));has_overlay=true;
            }
        }
        if(f.inspect_layers)f.inspect_layers(info);
        std::array<float,2> panels{};unsigned panel_count{};
        for(unsigned i=0;i<info->layerCount;++i)if(info->layers[i]&&info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_QUAD) {
            if(panel_count<2)panels[panel_count]=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i])->pose.position.x;
            ++panel_count;
        }
        if(panel_count==2){std::scoped_lock lock(f.mutex);f.panel_positions.push_back(panels);}
        {std::scoped_lock lock(f.mutex);f.pixels.push_back(color);if(has_overlay)f.overlay_pixels.push_back(overlay_color);f.positions.push_back(position);f.handoffs.push_back(start);f.director_policies.push_back(f.transport?f.transport->director_enabled():true);f.targets.push_back(info->displayTime);++f.real_ends;}
        f.changed.notify_all();
        if(f.refuse_end_call && f.real_ends>=f.refuse_end_call && f.real_ends<f.refuse_end_call+f.refuse_end_count)
            return XR_ERROR_SWAPCHAIN_RECT_INVALID;
        return f.fail_end?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;
    }
    static XrResult XRAPI_CALL create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* output) {
        auto& f=*current;if(f.chains.size()>=f.native_chain_limit)return XR_ERROR_LIMIT_REACHED;
        *output=reinterpret_cast<XrSwapchain>(f.next_handle++);auto& chain=f.chains[*output];
        chain.static_image=(info->createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)!=0;
        chain.state=f.create_state?f.create_state():f.real_common?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_RENDER_TARGET;
        for(unsigned i=0;i<f.native_image_count;++i)if(f.native12) {
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=info->width;desc.Height=info->height;desc.DepthOrArraySize=static_cast<UINT16>(info->arraySize);desc.MipLevels=1;desc.Format=static_cast<DXGI_FORMAT>(info->format);desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> texture;
            ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,chain.state,nullptr,IID_PPV_ARGS(&texture)),"fake runtime real D12 image");chain.d12.push_back(std::move(texture));
        } else {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=info->width;desc.Height=info->height;desc.ArraySize=info->arraySize;desc.MipLevels=1;desc.Format=static_cast<DXGI_FORMAT>(info->format);desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;ComPtr<ID3D11Texture2D> texture;ok(f.device11->CreateTexture2D(&desc,nullptr,&texture),"fake runtime real D11 image");chain.d11.push_back(std::move(texture));
        }
        return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL destroy(XrSwapchain handle){current->chains.erase(handle);return XR_SUCCESS;}
    static XrResult XRAPI_CALL enumerate(XrSwapchain handle,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* images) {
        auto& f=*current;auto& chain=f.chains.at(handle);*count=static_cast<unsigned>(f.native12?chain.d12.size():chain.d11.size());if(!capacity)return XR_SUCCESS;if(capacity<*count)return XR_ERROR_SIZE_INSUFFICIENT;
        for(unsigned i=0;i<*count;++i)if(f.native12)reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[i].texture=chain.d12[i].Get();else reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[i].texture=chain.d11[i].Get();return XR_SUCCESS;
    }
    static XrResult XRAPI_CALL acquire(XrSwapchain h,const XrSwapchainImageAcquireInfo*,std::uint32_t* index){auto& f=*current;auto& c=f.chains.at(h);check(!c.owned,"physical image ownership");check(!c.static_image || !c.acquisitions,"static physical image can be acquired only once");++c.acquisitions;c.acquired=f.reuse_current_real_image?c.released:(c.released+1)%static_cast<unsigned>(f.native12?c.d12.size():c.d11.size());c.owned=true;c.waited=false;*index=c.acquired;++f.real_acquires;return XR_SUCCESS;}
    static XrResult XRAPI_CALL wait_image(XrSwapchain h,const XrSwapchainImageWaitInfo*){auto& f=*current;auto& c=f.chains.at(h);check(c.owned,"physical wait after acquire");if(const auto delay=f.slow_image_wait_ms.load()){LARGE_INTEGER due{};due.QuadPart=-delay*10'000LL;check(f.preparation_timer && SetWaitableTimer(f.preparation_timer,&due,0,nullptr,nullptr,FALSE),"precise preparation delay");check(WaitForSingleObject(f.preparation_timer,1000)==WAIT_OBJECT_0,"preparation delay completed");}c.waited=true;if(f.image_wait_callback)f.image_wait_callback(h,c.acquired);return XR_SUCCESS;}
    static XrResult XRAPI_CALL release(XrSwapchain h,const XrSwapchainImageReleaseInfo*){auto& c=current->chains.at(h);check(c.owned && c.waited,"physical release after wait");c.released=c.acquired;c.owned=false;return XR_SUCCESS;}
    static XrResult XRAPI_CALL to_qpc(XrInstance,XrTime t,LARGE_INTEGER* q){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);const auto n=t-epoch;q->QuadPart=n/1'000'000'000LL*f.QuadPart+n%1'000'000'000LL*f.QuadPart/1'000'000'000LL;return XR_SUCCESS;}
    XrSwapchain make_chain(bool static_image=false,unsigned private_images=3) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.createFlags=static_image?XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT:0;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.width=info.height=info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        XrSwapchain handle{};xr_ok(transport->create_swapchain(&info,&handle,D3D12_RESOURCE_STATE_RENDER_TARGET,private_images),"private swapchain creation");return handle;
    }
    void write(XrSwapchain handle,unsigned value) {
        unsigned index{};XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait_info.timeout=XR_INFINITE_DURATION;
        xr_ok(transport->acquire_image(handle,&acquire_info,&index),"private acquire");xr_ok(transport->wait_image(handle,&wait_info),"private wait");
        {std::scoped_lock lock(gpu);float color[]={value/255.0F,0,0,1};unsigned count{};
        if(native12) {
            std::array<XrSwapchainImageD3D12KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};xr_ok(transport->enumerate_images(handle,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"enumerate private D12");
            for(auto& [native,c]:chains)for(auto& real:c.d12)check(images[index].texture!=real.Get(),"producer never writes real XR image");
            ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;ok(device12->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)),"RTV heap");device12->CreateRenderTargetView(images[index].texture,nullptr,heap->GetCPUDescriptorHandleForHeapStart());
            commands([&](ID3D12GraphicsCommandList* list){list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),color,0,nullptr);});
        } else {
            std::array<XrSwapchainImageD3D11KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};xr_ok(transport->enumerate_images(handle,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"enumerate private D11");for(auto& [native,c]:chains)for(auto& real:c.d11)check(images[index].texture!=real.Get(),"D11 producer private");ComPtr<ID3D11RenderTargetView> rtv;ok(device11->CreateRenderTargetView(images[index].texture,nullptr,&rtv),"D11 RTV");context->ClearRenderTargetView(rtv.Get(),color);
        }
        }
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr_ok(release_callback?release_callback(handle,index):transport->release_image(handle,&release_info),"private release");
    }
    void begin_virtual(){XrFrameState state{XR_TYPE_FRAME_STATE};xr_ok(transport->wait_frame(nullptr,&state),"virtual wait");check(state.predictedDisplayPeriod==period,"physical period despite throttled runtime prediction");xr_ok(transport->begin_frame(nullptr),"virtual begin");}
    XrResult submit(XrSwapchain handle,float identity,XrSwapchain overlay=XR_NULL_HANDLE,
        const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& delivery={}) {
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;view.pose.position.x=identity;view.subImage.swapchain=handle;view.subImage.imageRect.extent={1,1};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.space=reinterpret_cast<XrSpace>(9);quad.pose.orientation.w=1;quad.size={1,1};quad.subImage.swapchain=overlay;quad.subImage.imageRect.extent={1,1};
        std::array<const XrCompositionLayerBaseHeader*,2> layers={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=epoch+ns();info.layerCount=overlay?2:1;info.layers=layers.data();info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        auto result=transport->end_frame(&info,delivery);view.pose.position.x=-999;view.subImage.swapchain=XR_NULL_HANDLE;return result;
    }
    void unblock(){block_end=false;changed.notify_all();}
    void wait_entered(){std::unique_lock lock(mutex);check(changed.wait_for(lock,3s,[&]{return end_entered.load();}),"consumer reached blocked real EndFrame");}
    void wait_count(unsigned count){std::unique_lock lock(mutex);check(changed.wait_for(lock,4s,[&]{return real_ends>=count;}),"consumer delivered all accepted outputs");}
};
void fixture_gpu_completion_proof() {
    Fixture f(true);f.unblock();ok(f.queue->Wait(f.gate.Get(),1),"hold fixture GPU commands");
    check(SetEvent(f.event)!=0,"inject an already-signaled completion event");
    auto waiting=std::async(std::launch::async,[&]{f.commands([](ID3D12GraphicsCommandList*){});});
    check(waiting.wait_for(20ms)==std::future_status::timeout,"a stale event cannot retire the fixture command allocator before its actual fence");
    ok(f.gate->Signal(1),"release held fixture GPU work");waiting.get();
    std::puts("Fixture GPU allocator retirement requires actual fence completion PASS");
}
void live_framework_bypass(bool d12) {
    Fixture f(d12);f.throttled=false;f.unblock();auto chain=f.make_chain();
    const auto original=f.transport;
    unsigned identity{};
    auto batch=[&](unsigned count) {
        for(unsigned i=0;i<count;++i) {
            ++identity;f.write(chain,identity*8);f.begin_virtual();
            xr_ok(f.submit(chain,float(identity)),"live bypass ordered packet accepted");
        }
        f.wait_count(identity);xr_ok(f.transport->drain(),"policy batch fully delivered");
    };
    xr_ok(f.transport->set_director_enabled(false),"enter framework bypass on running session");
    check(!f.transport->director_enabled(),"bypass is effective immediately after drain");
    batch(8);
    auto shortest=period;
    for(unsigned i=1;i<8;++i)shortest=std::min(shortest,f.handoffs[i]-f.handoffs[i-1]);
    check(shortest<period/2,"unpaced fake runtime receives bypass frames without a director refresh floor");
    xr_ok(f.transport->set_director_enabled(true),"reenable director without recreating transport");
    batch(4);
    for(unsigned i=9;i<12;++i)
        check(f.handoffs[i]-f.handoffs[i-1]>=period,"director cadence restored after bypass");
    xr_ok(f.transport->set_director_enabled(false),"return to framework without carrying director delay");
    batch(8);
    f.transport->stop();xr_ok(f.transport->start(),"restart the same bypass endpoint");
    check(!f.transport->director_enabled(),"bypass policy survives endpoint restart");batch(2);
    check(f.transport==original && f.chains.size()==1,"live policy changes preserve handles, binding and allocation count");
    for(unsigned i=0;i<identity;++i)check(f.positions[i]==float(i+1) && f.pixels[i]==(i+1)*8,"all modes preserve exact FIFO scene and GPU pixel identity");
    xr_ok(f.transport->destroy_swapchain(chain),"retire live bypass chain");
    std::puts(d12?"D12 Framework bypass, restored cadence and live roundtrip PASS":"D11 Framework bypass, restored cadence and live roundtrip PASS");
}
void bypass_policy_drain() {
    Fixture f(true);auto chain=f.make_chain();
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"old director frame accepted");f.wait_entered();
    auto changing=std::async(std::launch::async,[&]{return f.transport->set_director_enabled(false);});
    check(changing.wait_for(20ms)==std::future_status::timeout && f.transport->director_enabled(),"live change waits for old native End and retains its policy");
    f.unblock();xr_ok(changing.get(),"bypass enters after old frame completes");
    check(!f.transport->director_enabled() && f.positions==std::vector<float>{1},"no old output discarded during transition");
    f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"first bypass packet after transition");f.wait_count(2);xr_ok(f.transport->drain(),"bypass transition drain");
    check(f.pixels==std::vector<unsigned>({40,80}),"transition preserves real target ownership and pixels");
    xr_ok(f.transport->destroy_swapchain(chain),"destroy transition chain");
}
void ownership(bool d12) {
    Fixture f(d12);auto chain=f.make_chain();
    for(unsigned n=1;n<=3;++n){f.write(chain,n*40);f.begin_virtual();xr_ok(f.submit(chain,static_cast<float>(n)),"End returns success without real delivery");if(n==1)f.wait_entered();}
    check(f.real_ends==0,"three accepted while real EndFrame remains blocked");
    f.begin_virtual();auto blocked=std::async(std::launch::async,[&]{return f.submit(chain,4);});
    check(blocked.wait_for(70ms)==std::future_status::timeout,"full private FIFO backpressures EndFrame");
    f.unblock();check(blocked.wait_for(3s)==std::future_status::ready,"slot retirement wakes producer");xr_ok(blocked.get(),"fourth accepted");f.wait_count(4);xr_ok(f.transport->drain(),"drain");
    const std::vector<unsigned> expected{40,80,120,120};check(f.pixels==expected,"FIFO pixels survive producer advance without overwrite");check(f.positions==std::vector<float>({1,2,3,4}),"projection metadata copied before producer stack mutation");
    for(unsigned i=1;i<4;++i){check(f.handoffs[i]-f.handoffs[i-1]>=period,"strict one physical refresh between real handoffs including long End");check(f.targets[i]>f.targets[i-1],"monotonic runtime target across variable horizon");}
    check(f.discarded_clocks>0,"long held End must renew its expired overlapped native clock without losing content");
    check(f.real_acquires>=3 && f.real_acquires<=4,"private immutable repeat may share the prepared real image; no producer acquisition");xr_ok(f.transport->destroy_swapchain(chain),"destroy drained image");
    std::printf("%s private End handoff, bounded FIFO/backpressure, immutable pixels/metadata, physical cadence and XR/QPC target PASS\n",d12?"D3D12":"D3D11");
}
void copied_source_reuse(bool d12) {
    Fixture f(d12);auto chain=f.make_chain(false,1);
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"single-source first packet accepted");f.wait_entered();
    std::promise<void> prepared;auto second_prepared=prepared.get_future();
    f.prepared_callback=[&](std::uint64_t sequence){if(sequence==2)prepared.set_value();};
    // Native End remains blocked. Only completed source-copy reads may end now;
    // the FIFO credit, real image and chain must remain protected until End.
    auto writer=std::async(std::launch::async,[&]{f.write(chain,80);});
    check(writer.wait_for(250ms)==std::future_status::ready,"completed copy frees single producer image before blocked native End");writer.get();
    check(f.real_ends==0,"source reuse must not fake native completion");
    f.begin_virtual();xr_ok(f.submit(chain,2),"rewritten single source accepted");
    check(second_prepared.wait_for(3s)==std::future_status::ready,"rewritten generation copied while older native End blocked");
    check(f.real_acquires==2 && f.chains.size()==1,"rewritten source needs a new real image, not an extra swapchain or stale prepared target");
    auto destroyed=std::async(std::launch::async,[&]{return f.transport->destroy_swapchain(chain);});
    check(destroyed.wait_for(50ms)==std::future_status::timeout,"source copy completion does not permit destroying in-flight real chain");
    f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"single-source packets drain");
    check(destroyed.wait_for(3s)==std::future_status::ready,"last native consumer permits chain destruction");xr_ok(destroyed.get(),"destroy protected chain");
    check(f.pixels==std::vector<unsigned>({40,80}),"source rewrite preserves old copied pixels and publishes new generation");
    check(f.positions==std::vector<float>({1,2}),"source reuse keeps per-packet metadata");
    std::printf("%s completed-copy source reuse, generation isolation and native chain lifetime PASS\n",d12?"D3D12":"D3D11");
}
void luke_acquire_ahead(bool d12) {
    Fixture f(d12);auto chain=f.make_chain();
    std::promise<void> entered,release;auto seen=entered.get_future();auto gate=release.get_future();
    f.preparation_callback=[&](std::uint64_t sequence){if(sequence==1){entered.set_value();gate.wait();}};
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"Luke first released frame");
    check(seen.wait_for(3s)==std::future_status::ready,"hold first source read before copy");
    f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"Luke second released frame");
    f.write(chain,120); // index2 is released; indices0/1 still have source readers.
    auto next=std::async(std::launch::async,[&]{unsigned index{};xr_ok(f.transport->acquire_image(chain,nullptr,&index),"Luke acquire next before End");return index;});
    const auto pressure=next.wait_for(50ms);
    release.set_value();f.wait_entered();
    check(next.wait_for(3s)==std::future_status::ready,"completed older copy makes next producer target available");const auto acquired=next.get();
    f.begin_virtual();xr_ok(f.submit(chain,3),"Luke release/acquire-next/End must retain the released image under copy pressure");
    check(pressure==std::future_status::timeout && acquired!=2,"next target must wait for an older image, not reacquire the latest release");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    xr_ok(f.transport->wait_image(chain,&wait),"wait next producer image");xr_ok(f.transport->release_image(chain,nullptr),"release next producer image after current End");
    f.unblock();f.wait_count(3);xr_ok(f.transport->drain(),"Luke acquire-ahead drain");
    check(f.pixels==std::vector<unsigned>({40,80,120}),"acquire-ahead must preserve every original released image");
    check(f.positions==std::vector<float>({1,2,3}) && f.chains.size()==1,"Luke startup keeps metadata and unchanged swapchain budget");
    xr_ok(f.transport->destroy_swapchain(chain),"destroy Luke startup chain");
    std::printf("%s Luke released-image protection, next acquisition under source pressure, End success and exact FIFO pixels PASS\n",d12?"D3D12":"D3D11");
}
void acquired_capacity(bool d12) {
    Fixture f(d12);auto chain=f.make_chain();unsigned indices[3]{};
    for(auto& index:indices)xr_ok(f.transport->acquire_image(chain,nullptr,&index),"multiple outstanding acquisitions before release");
    unsigned extra{};check(f.transport->acquire_image(chain,nullptr,&extra)==XR_ERROR_CALL_ORDER_INVALID,"fully acquired chain must fail rather than wait indefinitely");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    xr_ok(f.transport->wait_image(chain,&wait),"oldest acquired wait");xr_ok(f.transport->release_image(chain,nullptr),"oldest acquired release");
    check(f.transport->acquire_image(chain,nullptr,&extra)==XR_ERROR_CALL_ORDER_INVALID,"only latest release plus caller-owned images cannot cause an indefinite wait");
    for(unsigned n=0;n<2;++n){xr_ok(f.transport->wait_image(chain,&wait),"remaining acquired wait");xr_ok(f.transport->release_image(chain,nullptr),"remaining acquired release");}
    xr_ok(f.transport->acquire_image(chain,nullptr,&extra),"releasing another image permits producer reuse");
    check(extra!=indices[2],"latest release remains reserved for End");
    xr_ok(f.transport->wait_image(chain,&wait),"last acquired wait");xr_ok(f.transport->release_image(chain,nullptr),"last acquired release");
    xr_ok(f.transport->destroy_swapchain(chain),"destroy no-frame acquisition test");
    std::printf("%s multi-acquire capacity and bounded exhaustion without a latest-release alias PASS\n",d12?"D3D12":"D3D11");
}
void stop_wakeup() {
    Fixture f(true);auto chain=f.make_chain();f.write(chain,90);
    for(int n=0;n<3;++n){f.begin_virtual();xr_ok(f.submit(chain,n),"accept held image");if(!n)f.wait_entered();}
    f.begin_virtual();auto pending=std::async(std::launch::async,[&]{return f.submit(chain,4);});check(pending.wait_for(50ms)==std::future_status::timeout,"stop test full");
    auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
    check(pending.wait_for(2s)==std::future_status::ready,"stop wakes blocked End before runtime returns");check(pending.get()==XR_ERROR_SESSION_NOT_RUNNING,"stopped producer receives real state");f.unblock();check(stopped.wait_for(3s)==std::future_status::ready,"consumer stop joins");stopped.get();
    std::puts("Stop wakes full producer, joins consumer and retires GPU reads PASS");
}
void failure_wakeup() {
    Fixture f(true);auto chain=f.make_chain();f.write(chain,90);
    for(int n=0;n<3;++n){f.begin_virtual();xr_ok(f.submit(chain,n),"failure test accept");if(!n)f.wait_entered();}
    f.begin_virtual();auto pending=std::async(std::launch::async,[&]{return f.submit(chain,4);});check(pending.wait_for(50ms)==std::future_status::timeout,"failure test full");
    f.fail_end=true;f.unblock();check(pending.wait_for(3s)==std::future_status::ready,"runtime failure wakes blocked producer");check(pending.get()==XR_ERROR_RUNTIME_FAILURE,"runtime error propagated, no fabricated success after failure");
    std::puts("Runtime failure propagation and cancellation PASS");
}
// V417 in the consumer: one frame refused for its contents (OpenComposite's
// skybox quad, Green Hell VR) must not end the session on any delivery path.
void content_refusal(bool directed,unsigned refusals) {
    Fixture f(true);f.throttled=false;f.allow_expired_end=true;f.unblock();f.refuse_end_call=3;f.refuse_end_count=refusals;auto chain=f.make_chain();
    if(!native_contract_fixture_mode || native_director_only_fixture_mode)
        xr_ok(f.transport->set_director_enabled(directed),"choose the delivery path");
    const unsigned frames=refusals+7;
    for(unsigned n=1;n<=frames;++n) {
        f.write(chain,n*8);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"frames around refused ones are accepted");
    }
    f.wait_count(frames);xr_ok(f.transport->drain(),"refused frames do not stop delivery");
    f.write(chain,8);f.begin_virtual();xr_ok(f.submit(chain,1.f),"the session runs on after the refusals");
    f.wait_count(frames+1);
    std::printf("Content refusal keeps the session: contract=%d director_only=%d directed=%d refusals=%u PASS\n",
        native_contract_fixture_mode,native_director_only_fixture_mode,directed,refusals);
}
// A refusal that never ends is a fault: it must fail the session, at the limit.
void content_refusal_limit(bool directed) {
    Fixture f(true);f.throttled=false;f.allow_expired_end=true;f.unblock();f.refuse_end_call=3;f.refuse_end_count=1000000;auto chain=f.make_chain();
    if(!native_contract_fixture_mode || native_director_only_fixture_mode)
        xr_ok(f.transport->set_director_enabled(directed),"choose the delivery path");
    f.write(chain,8);
    XrResult result=XR_SUCCESS;
    const auto limit=xrfg::kPrivateRefusedFrameLimit;
    for(unsigned n=1;n<=limit+64 && XR_SUCCEEDED(result);++n) {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        result=f.transport->wait_frame(nullptr,&state);
        if(XR_SUCCEEDED(result))result=f.transport->begin_frame(nullptr);
        if(XR_SUCCEEDED(result))result=f.submit(chain,float(n));
    }
    check(result==XR_ERROR_SWAPCHAIN_RECT_INVALID,"a permanent refusal fails the session with its own error");
    check(f.real_ends==limit+2,"the session fails on the limit's refusal, not before and not after");
    std::printf("Content refusal limit: contract=%d director_only=%d directed=%d PASS\n",
        native_contract_fixture_mode,native_director_only_fixture_mode,directed);
}
void content_refusal_paths() {
    const auto run=[](bool directed) {
        content_refusal(directed,1);content_refusal(directed,xrfg::kPrivateRefusedFrameLimit-1);content_refusal_limit(directed);
    };
    for(bool directed:{false,true})run(directed);
    native_contract_fixture_mode=true;run(false);
    native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=true;
    run(true);
    native_contract_fixture_mode=native_adaptive_wait_fixture_mode=native_director_only_fixture_mode=false;
}
void pending_gpu() {
    Fixture f(true);f.unblock();auto chain=f.make_chain();f.write(chain,77);
    ok(f.queue->Wait(f.gate.Get(),1),"independently hold producer GPU");f.begin_virtual();
    xr_ok(f.submit(chain,1),"accept while producer GPU is pending");std::this_thread::sleep_for(20ms);
    check(f.real_ends==0 && f.real_acquires==0,"no image write/content delivery before source GPU completion; runtime clock may overlap preparation");
    ok(f.gate->Signal(1),"release GPU independently of XR");f.wait_count(1);xr_ok(f.transport->drain(),"GPU pending drain");check(f.pixels==std::vector<unsigned>{77},"pending frame retains actual source");
    f.transport->stop();check(!f.cycle_waited && !f.cycle_begun,"stop closes the real frame cycle");
    xr_ok(f.transport->start(),"restart same session endpoint");f.begin_virtual();xr_ok(f.submit(chain,2),"same endpoint works after restart");f.wait_count(2);xr_ok(f.transport->drain(),"restart drain");
    std::puts("Private End returns before GPU readiness; real consumer waits exact fence; session restart PASS");
}
void preparation_overlap(bool d12) {
    Fixture f(d12);f.unblock();f.throttled=false;
    f.slow_image_wait_ms=4;
    auto chain=f.make_chain();
    // The runtime reports an available image only after4ms. That preparation
    // must fit INSIDE the refresh interval, not be added after a full interval.
    for(unsigned i=1;i<=8;++i){f.write(chain,i*25);f.begin_virtual();xr_ok(f.submit(chain,float(i)),"accept paced packet");}
    f.wait_count(8);xr_ok(f.transport->drain(),"paced FIFO drain");
    check(f.pixels==std::vector<unsigned>({25,50,75,100,125,150,175,200}),"paced delivery preserves every FIFO image");
    for(unsigned i=1;i<f.handoffs.size();++i) {
        const auto interval=f.handoffs[i]-f.handoffs[i-1];
        if(interval<period)std::fprintf(stderr,"%s overlap handoff%u interval%lldns, required%lldns\n",d12?"D3D12":"D3D11",i,static_cast<long long>(interval),static_cast<long long>(period));
        check(interval>=period,"fresh prediction keeps physical refresh minimum");
    }
    const auto average=(f.handoffs.back()-f.handoffs.front())/(f.handoffs.size()-1);
    std::fprintf(stderr,"%s overlap measured interval%.3fms\n",d12?"D3D12":"D3D11",average/1e6);
    check(average<period+3'000'000,"refresh pacing must include preparation instead of adding it to every interval");
    std::printf("%s4ms preparation inside refresh, mean interval%.3fms; raw runtime time/Begin-before-copy/FIFO PASS\n",d12?"D3D12":"D3D11",average/1e6);
}
void future_work_dependency(bool split) {
    Fixture f(true,split);f.unblock();auto chain=f.make_chain();f.write(chain,77);
    std::promise<void> queued;auto observed=queued.get_future();
    f.after_head_ready=[&]{ok(f.queue->Wait(f.gate.Get(),1),"later unrelated producer work");queued.set_value();};
    f.begin_virtual();xr_ok(f.submit(chain,1),"accept completed head before later producer dependency");
    check(observed.wait_for(3s)==std::future_status::ready,"consumer observed exact head completion");
    {std::unique_lock lock(f.mutex);f.changed.wait_for(lock,50ms,[&]{return f.real_ends.load()>0;});}
    const bool independent=f.real_ends.load()>0;
    ok(f.gate->Signal(1),"release later work for safe fixture cleanup");f.wait_count(1);xr_ok(f.transport->drain(),"future dependency drain");
    check(independent==split,"only the split queue must deliver the ready head while later producer work is blocked");
    check(f.pixels==std::vector<unsigned>{77},"independent delivery preserves the exact completed image");
    std::printf("Ready-head future dependency: split=%d independent=%d PASS\n",split,independent);
}
void cadence_precision(bool d12) {
    Fixture f(d12);f.unblock();f.throttled=false;f.slow_image_wait_ms=4;
    auto chain=f.make_chain();
    for(unsigned n=1;n<=48;++n){f.write(chain,n*4);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"precision cadence packet");}
    f.wait_count(48);xr_ok(f.transport->drain(),"precision cadence drain");
    const auto average=(f.handoffs.back()-f.handoffs.front())/(f.handoffs.size()-1);
    for(unsigned n=1;n<f.handoffs.size();++n)check(f.handoffs[n]-f.handoffs[n-1]>=period,"precision must never send before a physical interval");
    std::fprintf(stderr,"%s precision mean interval%.6fms (%.3ffps)\n",d12?"D3D12":"D3D11",average/1e6,1e9/average);
    check(average<period+250'000,"timer overshoot must not impose a common sub-refresh FPS ceiling");
    check(f.pixels.size()==48 && f.pixels.front()==4 && f.pixels.back()==192,"precision wait preserves FIFO images");
}
void preparation_independence(bool d12) {
    Fixture f(d12);f.unblock();f.throttled=false;
    f.slow_frame_wait_ms=5;f.slow_image_wait_ms=8;
    auto chain=f.make_chain();
    for(unsigned n=1;n<=24;++n){f.write(chain,n*8);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"parallel preparation input");}
    f.wait_count(24);xr_ok(f.transport->drain(),"parallel preparation drain");
    const auto average=(f.handoffs.back()-f.handoffs.front())/(f.handoffs.size()-1);
    std::fprintf(stderr,"%s independent preparation measured%.6fms\n",d12?"D3D12":"D3D11",average/1e6);
    check(f.preparation_thread && f.delivery_thread && f.preparation_thread!=f.delivery_thread,"GPU preparation must not execute on the delivery thread");
    for(unsigned n=1;n<f.handoffs.size();++n)check(f.handoffs[n]-f.handoffs[n-1]>=period,"independent preparer cannot cause a short output interval");
    check(average<period+1'000'000,"5ms runtime wait and8ms preparation must overlap, not add to13ms per frame");
    check(f.pixels.size()==24 && f.pixels.front()==8 && f.pixels.back()==192,"independent delivery retains every accepted image");
    std::printf("%s independent5ms runtime+8ms preparation, interval%.3fms PASS\n",d12?"D3D12":"D3D11",average/1e6);
}
void stalled_preparer() {
    Fixture f(true);f.unblock();auto a=f.make_chain(),b=f.make_chain(),c=f.make_chain();
    std::promise<void> entered,release;auto seen=entered.get_future();auto gate=release.get_future();
    f.preparation_callback=[&](std::uint64_t seq){if(seq==3){entered.set_value();gate.wait();}};
    for(auto h:{a,b,c}){f.write(h,unsigned(h==a?40:h==b?80:120));f.begin_virtual();xr_ok(f.submit(h,1),"independent-chain input");}
    check(seen.wait_for(3s)==std::future_status::ready,"third preparation held independently");
    f.wait_count(2);check(f.real_ends==2,"ready outputs must be delivered while later preparation is blocked");
    release.set_value();f.wait_count(3);xr_ok(f.transport->drain(),"stalled preparation drain");
    check(f.pixels==std::vector<unsigned>({40,80,120}),"late third frame must not overwrite/drop the earlier pair");
    std::puts("Delivery consumes ready pair while third preparation is blocked PASS");
}
void unchanged_overlay() {
    Fixture f(true);auto a=f.make_chain(),b=f.make_chain(),overlay=f.make_chain();
    f.write(overlay,33);f.write(a,40);f.begin_virtual();xr_ok(f.submit(a,1,overlay),"first overlay frame");f.wait_entered();
    std::promise<void> ready;auto seen=ready.get_future();f.prepared_callback=[&](std::uint64_t seq){if(seq==2)ready.set_value();};
    f.write(b,80);f.begin_virtual();xr_ok(f.submit(b,2,overlay),"second unchanged-overlay frame");
    check(seen.wait_for(3s)==std::future_status::ready,"unchanged overlay permits the next image to finish preparation before prior End returns");
    check(f.real_acquires==3,"second frame reuses the same immutable overlay contents without an additional XR chain/copy");
    f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"unchanged overlay drain");
    check(f.pixels==std::vector<unsigned>({40,80}),"shared overlay must not replace either scene image");
    std::puts("Immutable overlay sharing preserves lookahead without extra swapchains PASS");
}
void same_chain_lookahead(bool d12) {
    Fixture f(d12);auto chain=f.make_chain();
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"first same-chain input");f.wait_entered();
    const auto previous=f.chains.at(chain).released;
    std::promise<void> ready;auto seen=ready.get_future();
    f.prepared_callback=[&](std::uint64_t seq){if(seq==2)ready.set_value();};
    f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"next same-chain input");
    check(seen.wait_for(3s)==std::future_status::ready,"next real image must finish GPU preparation while prior same-chain EndFrame is blocked");
    auto& real=f.chains.at(chain);
    check(f.real_acquires==2 && real.owned && real.waited,"exactly one next real target stays acquired/waited");
    check(real.released==previous && real.acquired!=previous,"copying ahead must not release/replace the previous EndFrame image");
    check(f.chains.size()==1,"lookahead allocates no extra real swapchain");
    f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"same-chain lookahead drain");
    check(f.pixels==std::vector<unsigned>({40,80}),"same-chain lookahead preserves the previous and next scene pixels");
    check(f.handoffs[1]-f.handoffs[0]>=period,"same-chain lookahead retains strict physical cadence");
    std::printf("%s same-chain GPU lookahead with unreleased next target and exact FIFO pixels PASS\n",d12?"D3D12":"D3D11");
}
void changing_overlay_lookahead() {
    Fixture f(true);auto a=f.make_chain(),b=f.make_chain(),overlay=f.make_chain();
    f.write(overlay,20);f.write(a,40);f.begin_virtual();xr_ok(f.submit(a,1,overlay),"first changing-overlay frame");f.wait_entered();
    const auto previous=f.chains.at(overlay).released;
    std::promise<void> ready;auto seen=ready.get_future();
    f.prepared_callback=[&](std::uint64_t seq){if(seq==2)ready.set_value();};
    f.write(overlay,60);f.write(b,80);f.begin_virtual();xr_ok(f.submit(b,2,overlay),"next changing-overlay frame");
    check(seen.wait_for(3s)==std::future_status::ready,"changing shared overlay must not prevent next scene and overlay GPU preparation");
    auto& real=f.chains.at(overlay);
    check(real.owned && real.waited && real.released==previous,"new overlay stays waited/unreleased while previous frame captures old overlay");
    check(f.real_acquires==4 && f.chains.size()==3,"changing-overlay lookahead reuses existing real chains only");
    f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"changing-overlay lookahead drain");
    check(f.pixels==std::vector<unsigned>({40,80}),"changing shared overlay preserves both scene images");
    check(f.overlay_pixels==std::vector<unsigned>({20,60}),"native End captures previous overlay then next overlay, never the future released image");
    check(f.chains.at(overlay).released!=previous && !f.chains.at(overlay).owned,"new overlay is released only for its own FIFO frame");
    std::puts("Changing shared overlay preserves next-scene GPU lookahead without early release PASS");
}
void stop_prepared_target() {
    Fixture f(true);auto chain=f.make_chain();
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"stop target first input");f.wait_entered();
    std::promise<void> ready;auto seen=ready.get_future();
    f.prepared_callback=[&](std::uint64_t seq){if(seq==2)ready.set_value();};
    f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"stop target next input");
    check(seen.wait_for(3s)==std::future_status::ready,"stop test owns a GPU-complete unreleased target");
    auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
    const auto limit=std::chrono::steady_clock::now()+3s;
    for(;;) {XrFrameState prediction{XR_TYPE_FRAME_STATE};
        if(f.transport->predict_frame(&prediction)==XR_ERROR_SESSION_NOT_RUNNING)break;
        check(std::chrono::steady_clock::now()<limit,"stop must mark the endpoint stopped before waiting for native End");std::this_thread::yield();}
    f.unblock();check(stopped.wait_for(3s)==std::future_status::ready,"stop drains an unreleased prepared image");stopped.get();
    check(f.real_ends==1 && !f.chains.at(chain).owned,"stop releases canceled prepared target without presenting it");
    f.prepared_callback={};xr_ok(f.transport->start(),"restart after pending target cleanup");
    f.begin_virtual();xr_ok(f.submit(chain,3),"restart retained private input");f.wait_count(2);xr_ok(f.transport->drain(),"restart target drain");
    check(f.pixels==std::vector<unsigned>({40,80}),"restart copies retained private source after canceled real target cleanup");
    std::puts("Stop releases an unpresented waited target and preserves restart ownership PASS");
}
void reused_real_index(bool cancel) {
    Fixture f(true);f.reuse_current_real_image=true;auto chain=f.make_chain();
    f.write(chain,40);f.begin_virtual();xr_ok(f.submit(chain,1),"reused-index first input");f.wait_entered();
    std::promise<void> acquired;auto seen=acquired.get_future();
    f.image_wait_callback=[&](XrSwapchain h,unsigned){if(h==chain && f.real_acquires==2)acquired.set_value();};
    f.write(chain,80);f.begin_virtual();xr_ok(f.submit(chain,2),"reused-index next input");
    check(seen.wait_for(3s)==std::future_status::ready,"runtime returns the still-current physical index for the next target");
    {std::scoped_lock lock(f.gpu);check(f.read_pixel(f.chains.at(chain))==40,"next copy must not overwrite an older retained generation sharing the same real index");}
    if(cancel) {
        auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
        const auto limit=std::chrono::steady_clock::now()+3s;
        for(;;){XrFrameState prediction{XR_TYPE_FRAME_STATE};if(f.transport->predict_frame(&prediction)==XR_ERROR_SESSION_NOT_RUNNING)break;
            check(std::chrono::steady_clock::now()<limit,"stop wakes real-index lease wait");std::this_thread::yield();}
        f.unblock();check(stopped.wait_for(3s)==std::future_status::ready,"stop drains a real-index reuse wait");stopped.get();
        check(f.pixels==std::vector<unsigned>({40}) && !f.chains.at(chain).owned,"canceling an aliased target preserves prior pixels and releases acquired ownership");
    } else {
        f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"reused-index drain");
        check(f.pixels==std::vector<unsigned>({40,80}),"runtime index reuse waits only for the old generation then preserves both FIFO images");
    }
    std::printf("Reused physical image index protects retained pixels%s PASS\n",cancel?" and stops cleanly":"");
}
void static_image_repeat() {
    Fixture f(true);f.unblock();auto chain=f.make_chain(true);f.write(chain,70);
    for(unsigned n=1;n<=4;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"repeat immutable static source");f.wait_count(n);xr_ok(f.transport->drain(),"static repeat drain");}
    check(f.real_acquires==1,"static repeated image reuses the sole runtime acquisition after users reach zero");
    unsigned index{};check(f.transport->acquire_image(chain,nullptr,&index)==XR_ERROR_CALL_ORDER_INVALID,"private static image cannot be acquired again after release");
    f.transport->stop();xr_ok(f.transport->start(),"static retained chain restart");
    f.begin_virtual();xr_ok(f.submit(chain,5),"repeat static source after session restart");f.wait_count(5);xr_ok(f.transport->drain(),"static restart drain");
    check(f.real_acquires==1 && f.pixels==std::vector<unsigned>(5,70),"static generation survives session restart without a second physical acquire/release");
    std::puts("Static image repeats retain one physical acquisition across drained frames and restart PASS");
}
void stop_first_static_target() {
    Fixture f(true);f.unblock();auto chain=f.make_chain(true);f.write(chain,70);
    std::promise<void> entered,proceed;auto seen=entered.get_future();auto gate=proceed.get_future();
    f.image_wait_callback=[&](XrSwapchain h,unsigned){if(h==chain){entered.set_value();gate.wait();}};
    f.begin_virtual();xr_ok(f.submit(chain,1),"first static target before cancellation");
    check(seen.wait_for(3s)==std::future_status::ready,"static first target is acquired/waited before GPUcopy");
    auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
    const auto limit=std::chrono::steady_clock::now()+3s;
    for(;;){XrFrameState prediction{XR_TYPE_FRAME_STATE};if(f.transport->predict_frame(&prediction)==XR_ERROR_SESSION_NOT_RUNNING)break;
        check(std::chrono::steady_clock::now()<limit,"stop marks static target cancellation before native Wait returns");std::this_thread::yield();}
    proceed.set_value();check(stopped.wait_for(3s)==std::future_status::ready,"stop finishes the first static GPUcopy and releases its one acquired target");stopped.get();
    check(f.real_acquires==1 && !f.chains.at(chain).owned && f.real_ends==0,"canceled static target is released once without presenting");
    f.image_wait_callback={};xr_ok(f.transport->start(),"restart after canceled first static target");
    f.begin_virtual();xr_ok(f.submit(chain,2),"reuse completed static target after cancellation");f.wait_count(1);xr_ok(f.transport->drain(),"canceled static restart drain");
    check(f.real_acquires==1 && f.pixels==std::vector<unsigned>({70}),"restart presents the exact static source without forbidden reacquisition");
    std::puts("Stop after first static Wait finishes its safe GPUcopy and restarts with one acquisition PASS");
}
void overlapped_native_wait(bool cancel) {
    std::promise<void> entered,release;auto seen=entered.get_future();auto gate=release.get_future();
    Fixture f(true,true,[&](unsigned n){if(n==2)return 14'000;if(n==3){entered.set_value();gate.wait();}return 0;});
    auto chain=f.make_chain();f.write(chain,70);
    for(unsigned n=0;n<2;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"overlapped native Wait input");}
    check(seen.wait_for(3s)==std::future_status::ready,"next native Wait must start before the current End completes");
    f.wait_entered();f.unblock();f.wait_count(1);
    check(f.wait_thread && f.wait_thread!=f.delivery_thread,"native Wait must run independently of Begin/End delivery");
    check(f.pixels==std::vector<unsigned>({70}),"current frame must deliver while the next native Wait is still blocked");
    if(cancel) {
        auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
        check(stopped.wait_for(30ms)==std::future_status::timeout,"stop cannot abandon an in-flight native Wait");
        release.set_value();check(stopped.wait_for(3s)==std::future_status::ready,"stop must join and balance overlapped Wait");stopped.get();
        check(!f.cycle_waited && !f.cycle_begun && f.real_ends==1,"canceled lookahead is closed once without a phantom content frame");
        xr_ok(f.transport->start(),"restart after overlapped native Wait stop");f.begin_virtual();xr_ok(f.submit(chain,3),"restart input after joined Wait");f.wait_count(2);xr_ok(f.transport->drain(),"overlapped restart drain");
    } else {release.set_value();f.wait_count(2);xr_ok(f.transport->drain(),"overlapped Wait drain");}
    check(f.pixels==std::vector<unsigned>({70,70}),"overlapped native Wait keeps exact FIFO images");
    for(unsigned n=1;n<f.handoffs.size();++n)check(f.handoffs[n]-f.handoffs[n-1]>=period,"overlapped native Wait cannot shorten the physical interval");
    std::puts(cancel?"Overlapped native Wait stop/join/cleanup/restart PASS":"Blocked next native Wait does not block current delivery PASS");
}
void overlapped_wait_failure() {
    Fixture f(true,true,[](unsigned n){return n==2?14'000:0;});f.fail_wait_call=3;
    auto chain=f.make_chain();f.write(chain,70);
    for(unsigned n=0;n<2;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"queued native failure input");}
    f.wait_entered();f.unblock();f.wait_count(1);
    check(f.transport->drain()==XR_ERROR_RUNTIME_FAILURE,"future native Wait error must propagate after the current handoff");
    check(f.pixels==std::vector<unsigned>({70}),"future Wait failure cannot erase the current valid content frame");
    f.transport->stop();check(!f.cycle_waited && !f.cycle_begun,"failed lookahead leaves no native frame open");
    std::puts("Future native Wait failure preserves current delivery and propagates PASS");
}
void physical_clock_pipeline(bool d12) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;
    auto chain=f.make_chain();std::vector<unsigned> expected;
    f.preparation_callback=[&](std::uint64_t seq){if(seq%5==0)std::this_thread::sleep_for(5ms);};
    for(unsigned n=0;n<24;++n){const auto color=40+n*3;f.write(chain,color);expected.push_back(color);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"physical-clock irregular input");if(n%7==0)std::this_thread::sleep_for(9ms);}
    f.wait_count(24);xr_ok(f.transport->drain(),"physical-clock drain");
    check(f.pixels==expected,"physical clock retained every exact FIFO image");
    std::uint64_t previous{};
    std::int64_t largest{};
    for(auto at:f.handoffs){const auto elapsed=at-f.physical_origin+1'000'000;const auto slot=static_cast<std::uint64_t>(elapsed/period);const auto phase=elapsed%period;
        if(phase>=300'000)std::fprintf(stderr,"physical phase%lldns at slot%llu\n",static_cast<long long>(phase),slot);
        check(phase<300'000 && slot>previous,"handoff must stay on absolute headset slots after late preparation");previous=slot;largest=std::max(largest,phase);}
    for(unsigned n=1;n<f.handoffs.size();++n)check(f.handoffs[n]-f.handoffs[n-1]>=period-100'000,"physical-clock correction cannot burst outputs");
    std::printf("%s absolute physical slots,24exact FIFO pixels, late-preparation phase recovery, largest%.3fms PASS\n",d12?"D3D12":"D3D11",largest/1e6);
}
void native_end_advance(bool d12) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;f.simulated_submit_ns=350'000;
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<12;++n){const auto color=60+n;f.write(chain,color);expected.push_back(color);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"early native End input");}
    f.wait_count(12);xr_ok(f.transport->drain(),"early native End drain");
    check(f.pixels==expected && f.submit_receipts.size()==12,"early native End retains every copied source and actual runtime receipt");
    for(unsigned n=0;n<f.handoffs.size();++n) {
        const auto shifted=f.handoffs[n]-f.physical_origin+1'000'000;
        check(shifted%period<300'000,"actual native End entry must occur 1ms before headset vsync, not just earlier Wait/Begin");
        const auto physical_edge=f.physical_origin+(shifted/period)*period;
        check(f.submit_receipts[n]<=physical_edge-300'000,"native submission work must finish with real compositor headroom");
        if(n)check(f.handoffs[n]-f.handoffs[n-1]>=period-100'000,"advanced End cannot catch up with a shortened interval");
    }
    std::printf("%s actual native End1ms early,350us native submit work before vsync, exact FIFO pixels and physical cadence PASS\n",d12?"D3D12":"D3D11");
}
void late_physical_clock() {
    Fixture f(true,true,{},true);f.physical_clock_enabled=false;f.unblock();f.throttled=false;
    auto chain=f.make_chain();f.write(chain,70);
    for(unsigned n=0;n<3;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"unavailable physical clock input");f.wait_count(n+1);xr_ok(f.transport->drain(),"unavailable clock drain");}
    f.physical_clock_enabled=true;
    for(unsigned n=3;n<9;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"late physical clock input");}
    f.wait_count(9);xr_ok(f.transport->drain(),"late physical clock drain");
    check(f.pixels==std::vector<unsigned>(9,70),"late physical-clock activation must preserve every queued image");
    for(unsigned n=3;n<f.handoffs.size();++n){check((f.handoffs[n]-f.physical_origin+1'000'000)%period<300'000,"late physical clock must anchor to display phase");check(f.handoffs[n]-f.handoffs[n-1]>=period-100'000,"first anchored slot cannot burst after fallback");}
    std::puts("Late physical-clock availability transitions from fallback without a burst or lost image PASS");
}
void stale_native_clock(bool d12,bool directed=true,bool clean=false) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;
    if(!directed)xr_ok(f.transport->set_director_enabled(false),"Framework expired-cycle test");
    f.transport->set_clean_bypass(clean);
    check(f.transport->clean_bypass_enabled()==clean,"clean bypass policy applied");
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<6;++n) {
        if(n==2 || n==4) {
            // A sparse producer resumes after the next native clock would
            // have elapsed. SteamVR can still return the previous target+P.
            std::this_thread::sleep_for(65ms);f.stale_next_clock=true;
        }
        const auto color=60+n*7;f.write(chain,color);expected.push_back(color);
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"sparse input with stale native prediction");
        f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"renewed native clocks drain");f.transport->stop();
    check(f.discarded_clocks>=2,"native cycle validity remains mandatory even in clean all-off bypass");
    check(f.pixels==expected && f.real_ends==6,"renewing native clocks cannot drop, repeat or invent a content frame");
    check(!f.cycle_waited && !f.cycle_begun,"renewed cycles remain balanced on stop");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"every native Begin is ended or explicitly discarded");
    check(f.wait_thread!=f.delivery_thread,"cycle renewal preserves the independent Wait worker");
    std::puts(d12?"D12 stale native clock renewal retains all six exact FIFO images PASS":"D11 stale native clock renewal retains all six exact FIFO images PASS");
}
void clean_bypass_roundtrip(bool d12) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<9;++n) {
        if(n==0 || n==6) {
            f.transport->set_clean_bypass(true);
        }
        if(n==3)f.transport->set_clean_bypass(false);
        if(n%3==1) {std::this_thread::sleep_for(65ms);f.stale_next_clock=true;}
        if(n<3 || n>=6) {
            // Simulate timing hints left by an earlier generation transaction.
            f.transport->pair_next_end_with_previous();
            f.transport->set_next_end_content_time(1);
        }
        f.write(chain,80+n);expected.push_back(80+n);
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"clean/active/clean FIFO submission");
        f.wait_count(n+1);
        check(f.transport->clean_bypass_enabled()==(n<3 || n>=6),"live clean bypass follows all-off phases");
    }
    xr_ok(f.transport->drain(),"live clean bypass roundtrip drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==9,"Off/On/Off retains each exact source image once");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"live clean bypass retains native call ownership");
}
// pattern 0: two stalled renewals in a row stop renewal. 1: a current renewal
// between stalls resets the count. 2: stalls more than 2s apart do not latch.
void stalled_native_renewal(bool d12,bool directed,int pattern) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;f.allow_expired_end=true;
    if(!directed)xr_ok(f.transport->set_director_enabled(false),"Framework stalled-renewal test");
    // At each gap the first Wait returns an expired cycle. stalled: its
    // renewals block 60ms (> 4 refreshes) and stay expired. renewals: the
    // native Begins discarded for that packet, -1 = at least one.
    struct Gap{unsigned frame;bool stalled;int renewals;std::chrono::milliseconds pause;};
    std::vector<Gap> gaps;
    if(pattern==0)gaps={{2,true,1,65ms},{4,true,1,65ms},{6,false,0,65ms},{8,false,0,65ms}};
    else if(pattern==1)gaps={{2,true,1,65ms},{4,false,-1,65ms},{6,true,1,65ms},{8,true,1,65ms},{10,false,0,65ms}};
    else gaps={{2,true,1,65ms},{4,true,1,2100ms},{6,true,1,65ms},{8,false,0,65ms}};
    // This fixture's D11 Director slots also renew current cycles between the
    // gaps (stage 23), which resets the count by design; D12 Director covers
    // the latch on the same consumer loop.
    const bool latch_checked=d12 || !directed;
    auto chain=f.make_chain();std::vector<unsigned> expected;unsigned stalls{};
    for(unsigned n=0;n<gaps.back().frame+2;++n) {
        const Gap* gap{};
        for(const auto& g:gaps)if(g.frame==n)gap=&g;
        if(gap) {
            std::this_thread::sleep_for(gap->pause);f.stale_next_clock=true;
            if(gap->stalled){f.stall_us=60'000;f.stall_from=f.real_waits+2;f.stall_count=20;++stalls;}
        }
        const auto discarded=f.discarded_clocks.load();
        const auto color=40+n*9;f.write(chain,color);expected.push_back(color);
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"input before a stalling native renewal");
        f.wait_count(n+1);f.stall_count=0;
        if(gap) {
            const auto renewed=f.discarded_clocks-discarded;
            if(gap->renewals<0)check(renewed>=1,"an expired cycle renews while the runtime renews current cycles");
            else if(gap->renewals)check(renewed==1,"a stalled renewal is not repeated for the same packet");
            else if(latch_checked)check(renewed==0,"after two stalled renewals in a row this transport no longer renews");
        }
    }
    xr_ok(f.transport->drain(),"stalled renewal drain");f.transport->stop();
    check(f.stalled_waits==stalls,"each stalled gap costs exactly one stalled Wait");
    check(f.stalled_traces==stalls,"each stalled renewal is traced once");
    check(f.pixels==expected && f.real_ends==expected.size(),"stalled renewals cannot drop, repeat or invent a content frame");
    check(!f.cycle_waited && !f.cycle_begun,"stalled renewals remain balanced on stop");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"every native Begin is ended or explicitly discarded");
    static constexpr const char* names[]{"two in a row latch","a current renewal resets","2s apart do not latch"};
    std::printf("%s %s stalled native renewal: %s%s PASS\n",d12?"D12":"D11",directed?"Director":"Framework",names[pattern],latch_checked?"":" (latch not asserted)");
}
void native_renewal_failure() {
    Fixture f(true,true,{},true);f.unblock();f.throttled=false;
    auto chain=f.make_chain();f.write(chain,70);f.begin_virtual();xr_ok(f.submit(chain,1),"before native renewal failure");
    f.wait_count(1);xr_ok(f.transport->drain(),"first native renewal input drained");
    std::this_thread::sleep_for(65ms);f.stale_next_clock=true;f.fail_wait_call=f.real_waits+2;
    f.write(chain,90);f.begin_virtual();xr_ok(f.submit(chain,2),"input whose native renewal fails");
    check(f.transport->drain()==XR_ERROR_RUNTIME_FAILURE,"renewal Wait failure must reach the producer");
    f.transport->stop();
    check(f.pixels==std::vector<unsigned>({70}),"failed clock renewal cannot deliver the stale second frame or erase the prior frame");
    check(!f.cycle_waited && !f.cycle_begun,"failed renewal closes its previous begun native cycle");
    std::puts("Native clock renewal failure propagates and balances its prior Begin PASS");
}
void nonrenderable_native_clock() {
    Fixture f(true,true,{},true);f.unblock();f.throttled=false;
    auto chain=f.make_chain();f.write(chain,70);f.begin_virtual();xr_ok(f.submit(chain,1),"before nonrenderable native clock");
    f.wait_count(1);xr_ok(f.transport->drain(),"first visible input drained");
    std::this_thread::sleep_for(65ms);f.native_should_render=false;f.stale_next_clock=true;
    const auto discarded=f.discarded_clocks.load(),empty=f.empty_ends.load();
    f.write(chain,90);f.begin_virtual();xr_ok(f.submit(chain,2),"nonrenderable input with stale native clock");
    xr_ok(f.transport->drain(),"nonrenderable clock must close without a renewal spin");
    check(f.discarded_clocks==discarded && f.empty_ends==empty+1,"shouldRender false closes one empty native frame");
    f.native_should_render=true;f.write(chain,110);f.begin_virtual();xr_ok(f.submit(chain,3),"visible again after stale nonrenderable clock");
    f.wait_count(2);xr_ok(f.transport->drain(),"resumed visible native clock drains");f.transport->stop();
    check(f.pixels==std::vector<unsigned>({70,110}),"visibility recovery preserves subsequent content");
    std::puts("Nonrenderable expired cycle closes once and resumes visible delivery PASS");
}
void bounded_native_window(bool d12) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;
    auto chain=f.make_chain();std::vector<unsigned> expected;
    // This runtime returns now+P. A current prediction need only cover the
    // actual display edge, not another complete refresh after the handoff.
    for(unsigned n=0;n<6;++n){const auto color=50+n*9;f.write(chain,color);expected.push_back(color);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"short-but-current native window input");}
    f.wait_count(6);xr_ok(f.transport->drain(),"bounded native-window drain");f.transport->stop();
    for(auto [seq,count]:f.window_renewals)check(count==1,"preventive renewal is bounded to one attempt for each source packet");
    check(std::any_of(f.native_windows.begin(),f.native_windows.end(),[](auto window){return window>0 && window<period;}),"a runtime unable to extend its future window must still make progress");
    check(f.pixels==expected && f.real_ends==6,"preventive renewal preserves all exact FIFO images without extra content Ends");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"preventive Begin discards remain balanced on stop");
    std::puts(d12?"D12 short native-window renewal is bounded and retains exact content PASS":"D11 short native-window renewal is bounded and retains exact content PASS");
}
void quiet_native_clock(bool d12) {
    Fixture f(d12,true,{},true,false);f.unblock();f.throttled=false;
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<6;++n){
        if(n==2){f.wait_count(2);std::this_thread::sleep_for(65ms);f.stale_next_clock=true;}
        const auto color=60+n*7;f.write(chain,color);expected.push_back(color);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"logging-off native input");}
    f.wait_count(6);xr_ok(f.transport->drain(),"logging-off native drain");f.transport->stop();
    check(f.discarded_clocks>0,"native clock renewal must remain active without diagnostic callbacks");
    check(f.window_renewals.empty(),"disabled diagnostic logging must not emit preventive-window traces");
    check(f.pixels==expected && f.real_ends==6,"logging-off delivery retains all exact FIFO images");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"logging-off native clocks balance on stop");
    std::puts(d12?"D12 native clock renewal and exact FIFO delivery with logging off PASS":"D11 native clock renewal and exact FIFO delivery with logging off PASS");
}
void native_margin_scope(bool d12) {
    {
        Fixture f(d12);f.unblock();f.throttled=false;
        auto chain=f.make_chain();f.write(chain,70);f.slow_image_wait_ms=8;
        // The native cycle is already ready before each deliberately late
        // image wait/copy. This must not create a new native margin afterward.
        f.preparation_callback=[&](std::uint64_t seq){
            const auto limit=std::chrono::steady_clock::now()+3s;
            // Renewals add native Begins without consuming another input.
            // Match this packet's input ordinal, not the total Begin count.
            while(f.begun_input_ordinal<seq){check(std::chrono::steady_clock::now()<limit,"this input's native Begin overlaps late preparation");std::this_thread::yield();}
        };
        for(unsigned n=0;n<4;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"late preparation input");f.wait_count(n+1);xr_ok(f.transport->drain(),"late preparation drain");}
        check(f.native_margins.empty(),"late GPU preparation must not be charged another native1.1ms margin");
        check(f.pixels==std::vector<unsigned>(4,70),"late preparation retains all finite FIFO inputs");
        for(unsigned n=1;n<f.handoffs.size();++n)check(f.handoffs[n]-f.handoffs[n-1]>=period,"late preparation preserves physical minimum");
    }
    {
        Fixture f(d12,true,[](unsigned n){return n==2?8'000:0;});f.unblock();f.throttled=false;
        auto chain=f.make_chain();f.write(chain,80);
        f.begin_virtual();xr_ok(f.submit(chain,1),"late native input");f.wait_count(1);xr_ok(f.transport->drain(),"late native drain");
        check(!f.native_margins.empty(),"genuinely late native Begin still receives held presentation headroom");
        check(f.pixels==std::vector<unsigned>({80}),"native margin preserves the owned image");
    }
    std::printf("%s native headroom excludes late GPU preparation and retains late native margin PASS\n",d12?"D3D12":"D3D11");
}
void rising_native_wait() {
    Fixture f(true,true,[](unsigned n){return 200+std::min(n>8?n-8:0u,20u)*400;});f.unblock();f.throttled=false;
    // Reproduce the observed staircase: each call takes another0.4ms while
    // immutable pixels are already ready. The predictor must learn variation,
    // rather than missing every refresh by the same unreserved increment.
    auto chain=f.make_chain();f.write(chain,70);
    for(unsigned n=0;n<40;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"rising native Wait owned input");}
    f.wait_count(40);xr_ok(f.transport->drain(),"rising native Wait drain");
    unsigned late{};
    for(unsigned n=1;n<f.handoffs.size();++n){const auto delta=f.handoffs[n]-f.handoffs[n-1];check(delta>=period,"rising Wait retains strict physical minimum");if(n>=8 && n<=28 && delta>period+200'000)++late;}
    const auto average=(f.handoffs[28]-f.handoffs[8])/20;
    std::fprintf(stderr,"Rising native Wait: mean%.6fms, late%u/21\n",average/1e6,late);
    check(average<period+200'000 && late<=5,"rising runtime cost must not cause a persistent sub-refresh staircase with ready input");
    check(f.pixels==std::vector<unsigned>(40,70),"variable native entry retains every FIFO image");
    std::puts("Rising native Wait learns service-time variation without slowing production or shortening physical cadence PASS");
}
void fresh_runtime_window(bool d12,int wait_ms=0) {
    Fixture f(d12);f.throttled=false;auto chain=f.make_chain();f.write(chain,70);
    f.slow_frame_wait_ms=wait_ms;
    // Retaining a recent longer native Wait can leave some lead after a
    // shorter timer wake. V589 deliberately opens native entry another1ms
    // earlier, so allow exactly that increment in prediction age; the physical
    // minimum interval and all cadence/FIFO assertions remain unchanged.
    f.prediction_age_limit=wait_ms?4'000'000:2'000'000;
    for(unsigned n=0;n<12;++n) {
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"fresh-cycle immutable input");
        if(!n){f.wait_entered();f.unblock();}
    }
    f.wait_count(12);xr_ok(f.transport->drain(),"fresh runtime window drain");
    for(unsigned i=1;i<f.handoffs.size();++i){const auto delta=f.handoffs[i]-f.handoffs[i-1];if(delta<period)std::fprintf(stderr,"%s freshness interval%lldns required%lldns\n",d12?"D12":"D11",static_cast<long long>(delta),static_cast<long long>(period));check(delta>=period,"fresh runtime window preserves physical minimum");}
    check(f.pixels==std::vector<unsigned>(12,70),"fresh runtime window retains every accepted image");
    const auto oldest=*std::max_element(f.prediction_ages.begin(),f.prediction_ages.end());
    std::printf("%s fresh runtime prediction, native Wait%dms, oldest%.3fms under saturated ready FIFO PASS\n",d12?"D3D12":"D3D11",wait_ms,oldest/1e6);
}
void adaptive_source_refill(bool d12,std::size_t capacity=3) {
    Fixture f(d12,true,{},false,true,false,capacity);f.throttled=false;auto chain=f.make_chain();f.write(chain,40);
    f.begin_virtual();xr_ok(f.submit(chain,0),"adaptive first input");f.wait_entered();f.unblock();f.wait_count(1);
    std::this_thread::sleep_for(18ms); // deliberate gap, no throughput assertion
    for(unsigned n=1;n<5;++n){f.begin_virtual();xr_ok(f.submit(chain,float(n)),"adaptive irregular input");}
    f.wait_count(5);xr_ok(f.transport->drain(),"adaptive refill drain");
    check(!f.refill_margins.empty(),"late source must build automatic presentation margin");
    for(auto margin:f.refill_margins)check(margin>0 && margin<=period,"fallback refill must stay at one period regardless of two/three admission");
    check(f.pixels==std::vector<unsigned>(5,40),"irregular input must deliver all five accepted images without awaiting imaginary90fps");
    for(unsigned i=1;i<f.handoffs.size();++i)check(f.handoffs[i]-f.handoffs[i-1]>=period,"adaptive source refill must not catch up with a burst");
    std::printf("%s automatic source refill retains all finite irregular input, bounded3slots, strict cadence PASS\n",d12?"D3D12":"D3D11");
}
void native_delivery_receipts(bool d12) {
    Fixture f(d12);f.throttled=false;auto chain=f.make_chain();f.write(chain,88);
    const auto started=ns();auto cycle=std::make_shared<xrfg::InterfaceCycle>(started);
    auto generated=std::make_shared<xrfg::InterfaceDeliveryReceipt>(true);generated->synthetic_confirmed=true;
    auto real=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
    cycle->synthetic_accepted(started,period);cycle->attach_native_delivery(generated);cycle->attach_native_delivery(real);
    f.begin_virtual();xr_ok(f.submit(chain,1,XR_NULL_HANDLE,generated),"observed synthetic accepted virtually");f.wait_entered();
    f.begin_virtual();xr_ok(f.submit(chain,2,XR_NULL_HANDLE,real),"observed real accepted while native End remains held");
    const auto producer_returned=ns();cycle->producer_returned(producer_returned);
    float cpu{},length{},estimated{};
    check(!cycle->read(producer_returned,cpu,length) && !cycle->read_minimum_cycle(producer_returned,estimated),"real GPU endpoint never publishes virtual success as native timing");
    std::this_thread::sleep_for(30ms); // deliberate native delay; not a throughput assertion
    f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"native observed drain");
    check(generated->outcome.load()==1 && real->outcome.load()==1,"both native outcomes published after success");
    check(real->entered_ns>producer_returned && real->returned_ns>=real->entered_ns,"real receipt includes downstream queue wait after producer return");
    check(cycle->read(producer_returned,cpu,length) && length>=30,"measured complete cycle contains deliberate native hold");
    check(cycle->read_minimum_cycle(producer_returned,estimated) && estimated<=length+.001F && estimated>=30,"actual native End completion drives runtime cycle");
    check(f.pixels==std::vector<unsigned>({88,88}) && f.chains.size()==1,"measurement leaves delivery pixels, count and resource ownership unchanged");
    xr_ok(f.transport->destroy_swapchain(chain),"observed chain cleanup");
    std::printf("%s native receipt: virtual return before physical delivery, held-End delay %.3fms, actual runtime completed cycle %.3fms PASS\n",d12?"D3D12":"D3D11",length,estimated);
}
void pooled_publication(bool reuse=false,bool d12=true) {
    Fixture f(d12);f.expected_views=2;f.native_chain_limit=2;f.throttled=false;f.reuse_current_real_image=reuse;
    std::array<std::array<XrSwapchain,4>,2> outputs{};
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format=DXGI_FORMAT_R8G8B8A8_UNORM;info.width=info.height=info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    for(unsigned eye=0;eye<2;++eye)for(auto& handle:outputs[eye])
        xr_ok(f.transport->create_swapchain(&info,&handle,D3D12_RESOURCE_STATE_RENDER_TARGET,1,reinterpret_cast<XrSwapchain>(eye+400)),"four independent private buffers fit one real sink per eye");
    check(f.chains.size()==2,"eight generated rings consume exactly two native handles");
    auto mappings=f.transport->native_swapchain_mapping();
    for(unsigned retry=0;!mappings && retry<1000;++retry){std::this_thread::yield();mappings=f.transport->native_swapchain_mapping();}
    check(mappings && mappings->size()==8,"inventory preserves all private buffer identities");
    std::vector<XrSwapchain> native;
    for(const auto& m:*mappings) {
        check(f.chains.contains(m.native),"inventory reports an actual downstream native handle");
        if(std::find(native.begin(),native.end(),m.native)==native.end())native.push_back(m.native);
    }
    check(native.size()==2,"eight logical outputs map to two native swapchains");
    XrSwapchain incompatible{};info.width=2;
    check(f.transport->create_swapchain(&info,&incompatible,D3D12_RESOURCE_STATE_RENDER_TARGET,1,reinterpret_cast<XrSwapchain>(400))==XR_ERROR_LIMIT_REACHED,"incompatible extent never aliases existing publication sink");
    info.width=1;check(f.chains.size()==2,"failed extra sink leaves working pools intact");
    auto submit=[&](unsigned slot,unsigned identity) {
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye) {
            views[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[eye].pose.orientation.w=1;
            views[eye].pose.position.x=float(identity);views[eye].subImage.swapchain=outputs[eye][slot];views[eye].subImage.imageRect.extent={1,1};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
        auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
        f.begin_virtual();return f.transport->end_frame(&end);
    };
    for(unsigned slot=0;slot<4;++slot){f.write(outputs[0][slot],20+slot*30);f.write(outputs[1][slot],130+slot*30);}
    xr_ok(submit(0,1),"first pooled pair accepted");f.wait_entered();
    xr_ok(submit(1,2),"second pooled pair accepted despite held native End");
    xr_ok(submit(2,3),"third pooled pair accepted despite held native End");
    auto blocked=std::async(std::launch::async,[&]{return submit(3,4);});
    check(blocked.wait_for(40ms)==std::future_status::timeout,"pool retains original FIFO capacity/backpressure");
    f.unblock();check(blocked.wait_for(3s)==std::future_status::ready,"pooled FIFO retirement wakes producer");xr_ok(blocked.get(),"fourth pooled pair");
    f.wait_count(4);xr_ok(f.transport->drain(),"pooled first drain");
    // Same slot/generation numbers across virtual buffers must never reuse
    // another buffer's completed XR image; returning 0->1->0 tests that join.
    for(auto slot:{0U,1U,0U}){xr_ok(submit(slot,5+slot),"pooled return/repeat accepted");}
    f.wait_count(7);xr_ok(f.transport->drain(),"pooled return drain");
    check(f.pixels==std::vector<unsigned>({20,50,80,110,20,50,20}),"left native pixel sequence exact across private ring changes");
    check(f.right_pixels==std::vector<unsigned>({130,160,190,220,130,160,130}),"right eye never overwritten by left or following frame");
    for(auto& pair:f.stereo_handles)check(pair[0]!=pair[1] && pair==f.stereo_handles.front(),"one stable native sink per separate eye");
    for(unsigned i=1;i<f.handoffs.size();++i)check(f.handoffs[i]-f.handoffs[i-1]>=period,"pooled delivery preserves director cadence");
    auto& left=outputs[0];for(unsigned slot=0;slot<3;++slot)xr_ok(f.transport->destroy_swapchain(left[slot]),"destroy unused pooled ring");
    check(f.chains.size()==2,"shared publication survives other ring destruction");
    xr_ok(f.transport->destroy_swapchain(left[3]),"last left ring retires its native sink");check(f.chains.size()==1,"one eye publication released on final owner");
    for(auto handle:outputs[1])xr_ok(f.transport->destroy_swapchain(handle),"destroy right rings");check(f.chains.empty(),"all pooled native handles released");
    auto retired=f.transport->native_swapchain_mapping();
    for(unsigned retry=0;!retired && retry<1000;++retry){std::this_thread::yield();retired=f.transport->native_swapchain_mapping();}
    check(retired && retired->empty(),"retired publications leave no stale native inventory");
    // A fresh generation after a preset switch must not reuse a retired pool.
    XrSwapchain rebuilt{};xr_ok(f.transport->create_swapchain(&info,&rebuilt,D3D12_RESOURCE_STATE_RENDER_TARGET,1,reinterpret_cast<XrSwapchain>(400)),"rebuild retired pool");
    check(f.chains.size()==1,"preset rebuild creates one fresh sink");f.write(rebuilt,77);f.expected_views=1;
    f.begin_virtual();xr_ok(f.submit(rebuilt,9),"rebuilt pool delivery");f.wait_count(8);xr_ok(f.transport->drain(),"rebuilt pool drain");check(f.pixels.back()==77,"rebuilt source copied to new native sink");
    f.transport->stop();xr_ok(f.transport->start(),"pooled session restart");f.write(rebuilt,101);
    f.begin_virtual();xr_ok(f.submit(rebuilt,10),"retained pool after session restart");f.wait_count(9);xr_ok(f.transport->drain(),"restarted pool drain");check(f.pixels.back()==101,"restart retains correct publication ownership");
    xr_ok(f.transport->destroy_swapchain(rebuilt),"rebuilt pool cleanup");check(f.chains.empty(),"no native sink leak");
    std::puts("Pooled native publication: eight private rings/two real sinks, exact stereo FIFO pixels, backpressure, stable cadence, destruction and rebuild PASS");
}
#include "private_vram_pool_tests.inc"
#include "owned_d11_output_tests.inc"
void adaptive_buffer_test(bool directed,bool d12) {
    Fixture f(d12,true,{},directed,true,false,3,true);f.throttled=false;f.unblock();
    // This case targets near-refresh service jitter, not expired/short native
    // cycles (covered independently). The fake Wait does not block on vsync.
    f.prediction_horizon_ns=period*4;
    xr_ok(f.transport->set_director_enabled(directed),"adaptive buffer pacing selection");
    f.end_work_us=[](unsigned n){return n%2?2000:0;};
    std::unordered_map<std::uint64_t,std::int64_t> native_targets,native_refills;
    std::int64_t refilled_total{};
    if(native_director_only_fixture_mode && directed)f.diagnostic_work=[&](int stage,std::uint64_t seq,std::int64_t at){
        if(stage==30){native_refills[seq]=at-refilled_total;refilled_total=at;}
        if(stage==39)native_targets[seq]=at;
    };
    const auto chain=f.make_chain();
    const unsigned count=224;
    for(unsigned n=1;n<=count;++n) {
        f.write(chain,(n%10+1)*17);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"adaptive buffer owned input");
    }
    xr_ok(f.transport->drain(),"adaptive buffer drains exact native output");
    check(f.positions.size()==count && f.pixels.size()==count,"adaptive buffer lost or repeated a packet");
    for(unsigned n=1;n<=count;++n)
        check(f.positions[n-1]==float(n) && f.pixels[n-1]==(n%10+1)*17,"adaptive buffer changed source identity or GPU pixels");
    const auto elapsed=f.handoffs.back()-f.handoffs.front();
    const auto rate=double(count-1)*1e9/double(elapsed);
    std::fprintf(stderr,"BUFFER %s %s outputs=%u rate=%.3f learned=%zu refill=%zu\n",directed?"Director":"Framework",d12?"D12":"D11",count,rate,f.buffer_reserves.size(),f.buffer_refills.size());
    check(!f.buffer_reserves.empty() && !f.buffer_refills.empty(),"near-refresh jitter did not build actual timing reserve");
    std::int64_t filled{};for(auto v:f.buffer_refills){check(v>0 && v<=500'000,"unbounded refill step");filled+=v;}
    check(filled<=period*7/4,"adaptive buffer escaped the private fractional cap");
    for(const auto& [seq,increment]:native_refills) {
        check(seq>1 && seq<=count && increment>0,"native refill has an owned output and positive increment");
        check(native_targets.at(seq)>=f.handoffs[seq-2]+period+increment-100'000,
            "native refill did not add real time beyond the previous handoff plus one period");
        check(f.handoffs[seq-1]>=native_targets.at(seq)-100'000,
            "native handoff ignored the learned refill deadline");
    }
    check(rate>84 && rate<94,"buffer delay was charged repeatedly or output ran above refresh");
    const auto learned=f.buffer_reserves.size(),refills=f.buffer_refills.size();
    xr_ok(f.transport->set_buffer_capacity(3,false),"OFXR disabled clears timing even at unchanged capacity");
    xr_ok(f.transport->set_director_enabled(false),"disabled bypass");
    for(unsigned n=1;n<=12;++n){f.write(chain,99);f.begin_virtual();xr_ok(f.submit(chain,float(n+count)),"disabled reference");}
    xr_ok(f.transport->drain(),"disabled drain");
    check(f.buffer_reserves.size()==learned && f.buffer_refills.size()==refills,"disabled mode retained adaptive timing");
    auto shortest=period;
    for(unsigned n=count+1;n<f.handoffs.size();++n)shortest=std::min(shortest,f.handoffs[n]-f.handoffs[n-1]);
    check(shortest<period/2,"disabled mode retained the buffered Framework clock");
    xr_ok(f.transport->set_buffer_capacity(2,true),"buffer off drains and clears reserve");
    xr_ok(f.transport->set_buffer_capacity(3,true),"buffer re-enable");
    xr_ok(f.transport->destroy_swapchain(chain),"adaptive source cleanup");
    std::printf("%s %s adaptive buffer: %u exact outputs, %.2ffps, learned=%zu refill=%lldns, OFF reset PASS\n",d12?"D12":"D11",directed?"Director":"Framework",count,rate,learned,static_cast<long long>(filled));
}
void buffer_early_metadata(bool d12) {
    Fixture f(d12,true,{},false,true,false,3,true);f.throttled=false;f.unblock();f.prediction_horizon_ns=period*4;
    xr_ok(f.transport->set_director_enabled(false),"buffered Framework");
    std::unordered_map<std::uint64_t,std::int64_t> intended;
    f.diagnostic_work=[&](int stage,std::uint64_t seq,std::int64_t at){
        if(stage==31)intended[seq]=at;
        // Simulate variable optional bookkeeping. This used to execute AFTER
        // Framework's last sleep and appear directly as output jitter.
        if(stage==3 && seq%2){const auto until=ns()+800'000;while(ns()<until)YieldProcessor();}
    };
    const auto chain=f.make_chain();constexpr unsigned count=96;
    for(unsigned n=1;n<=count;++n){f.write(chain,77);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"metadata buffer input");}
    xr_ok(f.transport->drain(),"metadata drain");check(f.positions.size()==count,"metadata lost output");
    std::vector<std::int64_t> lateness;
    for(unsigned n=8;n<count;++n)lateness.push_back(f.handoffs[n]-intended.at(n+1));
    std::sort(lateness.begin(),lateness.end());const auto p90=lateness[lateness.size()*9/10];
    std::printf("BUFFER early metadata %s p90 final entry lateness=%lldns\n",d12?"D12":"D11",static_cast<long long>(p90));
    check(p90<600'000,"optional metadata remains after the final buffered wait");
    check(d12?f.copy_clock_receipts==count:f.copy_clock_receipts==0,"GPU timing receipts present only on diagnostic D12 copies");
    xr_ok(f.transport->destroy_swapchain(chain),"metadata cleanup");
}
void buffer_release_before_begin(bool directed,bool enabled,bool alias) {
    Fixture f(true,true,{},true,true,false,3,enabled);
    f.unblock();f.throttled=false;f.prediction_horizon_ns=period*4;
    f.reuse_current_real_image=alias;
    xr_ok(f.transport->set_director_enabled(directed),"early-release Buffer policy");
    const auto chain=f.make_chain();
    std::promise<void> entered,unblock,prepared;
    auto native_enter=entered.get_future();auto release=unblock.get_future().share();
    auto second=prepared.get_future();
    f.begin_callback=[&](unsigned ordinal){if(ordinal==2){entered.set_value();release.wait();}};
    f.prepared_callback=[&](std::uint64_t seq){if(seq==2)prepared.set_value();};
    f.write(chain,77);f.begin_virtual();xr_ok(f.submit(chain,1),"first immutable buffered output");
    check(native_enter.wait_for(3s)==std::future_status::ready,"native Begin entered");
    f.write(chain,88);f.begin_virtual();xr_ok(f.submit(chain,2),"next output while native Begin blocks");
    const bool early=second.wait_for(150ms)==std::future_status::ready;
    unblock.set_value();f.wait_count(2);xr_ok(f.transport->drain(),"early-release drain");
    check(early==(enabled && !alias),alias?"reused physical image was overwritten before its End":
          enabled?"next copy remained gated by the prior native Begin":"unbuffered preparation order changed");
    check(f.pixels==std::vector<unsigned>({77,88}) && f.positions==std::vector<float>({1,2}),
          "early release preserves exact FIFO pixels and source poses");
    xr_ok(f.transport->destroy_swapchain(chain),"early-release cleanup");
    std::printf("D12 %s Buffer=%d alias=%d: preparation inside blocked Begin=%d, exact FIFO PASS\n",
                directed?"Director":"Framework",enabled,alias,early);
}
void buffer_early_release_cancel(bool directed,bool fail_begin) {
    Fixture f(true,true,{},true,true,false,3,true);
    f.unblock();f.throttled=false;f.prediction_horizon_ns=period*4;
    xr_ok(f.transport->set_director_enabled(directed),"early-release cancellation policy");
    const auto chain=f.make_chain();std::promise<void> entered,unblock;
    auto seen=entered.get_future();auto released=unblock.get_future().share();
    if(fail_begin)f.fail_begin_call=2;
    else f.begin_callback=[&](unsigned n){if(n==2){entered.set_value();released.wait();}};
    f.write(chain,77);f.begin_virtual();const auto accepted=f.submit(chain,1);
    check(XR_SUCCEEDED(accepted) || accepted==XR_ERROR_RUNTIME_FAILURE,"expected asynchronous Begin failure only");
    if(fail_begin) {
        check(XR_FAILED(f.transport->drain()),"native Begin failure must propagate after early release");
        f.transport->stop();
    } else {
        check(seen.wait_for(3s)==std::future_status::ready,"cancel fixture native Begin entered");
        auto stopped=std::async(std::launch::async,[&]{f.transport->stop();});
        const bool joined_early=stopped.wait_for(10ms)==std::future_status::ready;
        unblock.set_value();check(stopped.wait_for(3s)==std::future_status::ready,"early release stop joins native caller");stopped.get();
        check(!joined_early,"stop cannot retire an image while its native call is still active");
    }
    check(f.pixels.empty(),"failed/cancelled early-release frame cannot become a content End");
    f.begin_callback={};xr_ok(f.transport->start(),"early-release failure/cancel restart");
    f.write(chain,99);f.begin_virtual();xr_ok(f.submit(chain,9),"fresh output after early-release restart");
    f.wait_count(1);xr_ok(f.transport->drain(),"early-release restart drain");
    check(f.pixels==std::vector<unsigned>{99} && f.positions==std::vector<float>{9},
          "early-release restart must not revive the cancelled image/pose");
    xr_ok(f.transport->destroy_swapchain(chain),"early-release cancel cleanup");
    std::printf("D12 %s early release: %s, balanced restart and exact fresh pixels PASS\n",
                directed?"Director":"Framework",fail_begin?"Begin failure":"stop during Begin");
}
void buffer_cold_start_logging_off(bool directed) {
    Fixture f(true,true,{},true,false,false,3,true);
    f.unblock();f.throttled=false;f.prediction_horizon_ns=period*4;
    xr_ok(f.transport->set_director_enabled(directed),"cold-start Buffer policy");
    const auto chain=f.make_chain();
    for(unsigned n=1;n<=6;++n) {
        f.write(chain,n*20);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"cold-start buffered image");
    }
    f.wait_count(6);xr_ok(f.transport->drain(),"cold-start buffered drain");
    check(f.pixels==std::vector<unsigned>({20,40,60,80,100,120}),
          "logging-off Buffer must deliver every exact image from the first frame");
    check(f.copy_clock_receipts==0 && f.source_ready_events==0,
          "cold-start progress cannot depend on diagnostic GPU queries or callbacks");
    f.transport->stop();xr_ok(f.transport->start(),"logging-off Buffer restart");
    f.write(chain,140);f.begin_virtual();xr_ok(f.submit(chain,7),"first image after restart");
    f.wait_count(7);xr_ok(f.transport->drain(),"logging-off restart drain");
    check(f.pixels.back()==140,"logging-off Buffer restart retains exact image");
    xr_ok(f.transport->destroy_swapchain(chain),"cold-start cleanup");
    std::printf("D12 %s Buffer ON before first frame, diagnostics OFF, restart PASS\n",
                directed?"Director":"Framework");
}
void native_contract_sparse_reference(bool d12) {
    Fixture f(d12,true,{},true);f.unblock();f.throttled=false;
    const auto caller=GetCurrentThreadId();const auto chain=f.make_chain();
    std::vector<unsigned> expected;
    const unsigned count=6;
    for(unsigned n=0;n<count;++n) {
        if(n==2 || n==4){std::this_thread::sleep_for(65ms);f.stale_next_clock=true;}
        const auto value=60+(n%20)*7;expected.push_back(value);f.write(chain,value);
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"native-contract sparse output");
        f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"native-contract sparse drain");f.transport->stop();
    check(f.discarded_clocks>=2,"sparse reference must renew expired native cycles");
    check(f.pixels==expected && f.real_ends==count,"reference retains every exact FIFO image");
    check((f.wait_thread==f.delivery_thread) && f.delivery_thread!=caller,
          "configured Wait ownership and serialized frame consumer stay independent of production");
    check(!f.cycle_waited && !f.cycle_begun,"reference stop balances native cycles");
    check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,
          "reference Begin has exactly one End or explicit discard");
    xr_ok(f.transport->destroy_swapchain(chain),"reference sparse cleanup");
    std::printf("Native-contract %s sparse exact pixels, cycle renewal and independent consumer PASS\n",d12?"D12":"D11");
}
void native_contract_reference_checks() {
    native_contract_fixture_mode=true;
    native_contract_sparse_reference(true);native_contract_sparse_reference(false);
    native_renewal_failure();nonrenderable_native_clock();
    copied_source_reuse(true);copied_source_reuse(false);
    buffer_early_release_cancel(true,false);buffer_early_release_cancel(true,true);
    buffer_cold_start_logging_off(true);
}
void native_refresh_policy_checks() {
    xrfg::PrivateRefreshWaitPolicy p;
    int64_t t=1'000'000'000;
    p.observe(t,period);
    for(int n=0;n<32;++n){t+=period*9/7;p.observe(t,period);check(!p.enabled(t),"70fps cannot arm90Hz lookahead");}
    const auto qualify=[&] {
        for(int n=0;n<16;++n){t+=period;p.observe(t,period);check(!p.enabled(t),"full median window alone cannot arm");}
        for(int n=0;n<90;++n){t+=period;p.observe(t,period);check(!p.enabled(t),"entry must wait at least one continuous second");}
        t+=period;p.observe(t,period);check(p.enabled(t),"one full second of qualified cadence arms lookahead");
    };
    qualify();
    for(int n=0;n<32;++n){t+=period+(n%2?500'000:-500'000);p.observe(t,period);check(p.enabled(t),"paired jitter retains active full-rate policy");}
    t+=period*9/7;p.observe(t,period);check(!p.enabled(t),"70fps disarms immediately, not after the entry second");
    qualify();
    t+=period*3;check(!p.enabled(t),"source pause disarms without another accepted image");
    for(int n=0;n<160;++n){t+=period*90/89;p.observe(t,period);check(!p.enabled(t),"89fps cannot sustain entry qualification");}
    t+=period;p.observe(t,0);check(!p.enabled(t),"unknown refresh cannot qualify");
    for(int n=0;n<160;++n){t+=period;p.observe(t,8'333'333);check(!p.enabled(t),"90fps cannot arm120Hz lookahead");}
    xrfg::PrivateRefreshWaitPolicy edge;
    const auto base=t+period*4,below=period*90/89;
    for(int n=0;n<240;++n){const auto at=base+n*below+(n%17==0?-500'000:0);edge.observe(at,period);check(!edge.enabled(at),"89fps plus observation jitter must not pulse early mode");}
    xrfg::PrivateRefreshWaitPolicy recovery;
    t+=period*1000;recovery.observe(t,period);
    for(int phase=0;phase<2;++phase){
        for(int n=0;n<80;++n){t+=period;recovery.observe(t,period);check(!recovery.enabled(t),"sub-second recovery cannot arm");}
        for(int n=0;n<16;++n){t+=period*9/7;recovery.observe(t,period);check(!recovery.enabled(t),"slow gap resets, short recoveries cannot accumulate");}
    }
    xrfg::PrivateRefreshWaitPolicy missed;
    for(int n=0;n<1000;++n){t+=n%90==0?period*2:period;missed.observe(t,period);check(!missed.enabled(t),"a90fps median cannot hide periodic missed production slots");}
    std::puts("Qualified native Wait: median/window plus1s entry, immediate mean/idle exit, no accumulated recovery and periodic-miss checks PASS");
}
void native_adaptive_transitions(bool d12) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,2);f.unblock();f.throttled=false;
    std::vector<std::pair<unsigned,bool>> policies;
    std::vector<std::pair<unsigned,std::int64_t>> accepted;
    policies.reserve(2048);accepted.reserve(2048);
    std::atomic_bool observed_first_on{},observed_off_again{},observed_second_on{};
    f.diagnostic_work=[&](int stage,uint64_t seq,int64_t value){
        if(stage==1)accepted.emplace_back(unsigned(seq),value);
        if(stage==38) {
            const bool on=value!=0;policies.emplace_back(unsigned(seq),on);
            if(seq<=210 && on)observed_first_on.store(true,std::memory_order_release);
            if(seq>=211 && seq<=230 && !on && observed_first_on.load(std::memory_order_relaxed))
                observed_off_again.store(true,std::memory_order_release);
            if(seq>=241 && on && observed_off_again.load(std::memory_order_relaxed))
                observed_second_on.store(true,std::memory_order_release);
        }
    };
    auto chain=f.make_chain();std::vector<unsigned> expected;
    HANDLE source_timer=CreateWaitableTimerExW(nullptr,nullptr,0x2,TIMER_ALL_ACCESS);
    check(source_timer!=nullptr,"high resolution source timer");
    std::int64_t source_deadline{},second_fast_deadline{};
    bool second_fast_timeout{};
    for(unsigned n=0;;++n) {
        if(n==240)second_fast_deadline=ns()+6'000'000'000;
        if(n>=240 && ns()>=second_fast_deadline){second_fast_timeout=true;break;}
        if(n>=440 && observed_off_again.load(std::memory_order_acquire) &&
            observed_second_on.load(std::memory_order_acquire))break;
        const bool fast=(n>=20 && n<220) || n>=240;
        // Model periodic source arrivals, not a fresh sleep after GPU proof
        // and admission work. Each control/cadence phase starts a new timeline;
        // bounded transport ownership still regulates a producer that is late.
        if(n==0 || n==20 || n==210 || n==220 || n==230 || n==240)source_deadline=ns();
        source_deadline+=fast?6'000'000:16'000'000;
        if(const auto remaining=source_deadline-ns();remaining>0) {
            LARGE_INTEGER due{};due.QuadPart=-(remaining+99)/100;
            check(SetWaitableTimer(source_timer,&due,0,nullptr,nullptr,FALSE) && WaitForSingleObject(source_timer,1000)==WAIT_OBJECT_0,"source cadence timer");
        }
        if(n==210)xr_ok(f.transport->set_buffer_capacity(3,false),"disable OFXR during native lookahead");
        if(n==230)xr_ok(f.transport->set_buffer_capacity(2),"shrink during native normal policy");
        const auto value=40+(n%20)*7;expected.push_back(value);f.write(chain,value);
        f.begin_virtual();xr_ok(f.submit(chain,float(n)),"adaptive transition output");
        // Production Director regulates handoffs. Keep the producer independent
        // instead of measuring a serial source+native-readback loop as90fps.
        // Source-image leases and bounded admission still prevent reuse.
        if(!native_director_only_fixture_mode)f.wait_count(n+1);
    }
    f.wait_count(static_cast<unsigned>(expected.size()));
    CloseHandle(source_timer);
    xr_ok(f.transport->drain(),"adaptive transition drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==expected.size(),"adaptive policy preserves every exact input image across transitions");
    check(!f.cycle_waited && !f.cycle_begun,"adaptive stop balances an outstanding future Wait");
    bool first_on=false,off_again=false,second_on=false;
    for(const auto& [seq,on]:policies){if(seq<=20)check(!on,"slow startup remains native normal");
        if(seq>=211 && seq<=230)check(!on,"OFXR OFF must disable early Wait even with full source cadence");
        if(on)first_on=true;if(first_on && !on)off_again=true;if(off_again && on)second_on=true;}
    std::printf("Adaptive native %s receipts: first_on=%d off_again=%d second_on=%d policies=%zu exact_ends=%u\n",
                d12?"D12":"D11",first_on,off_again,second_on,policies.size(),f.real_ends.load());
    struct Phase {const char* name;unsigned first,last;};
    const std::array phases{Phase{"slow-start",1,20},Phase{"first-fast",21,210},
        Phase{"disabled-fast",211,220},Phase{"disabled-slow",221,230},
        Phase{"reenabled-slow",231,240},Phase{"second-fast",241,static_cast<unsigned>(expected.size())}};
    for(const auto& phase:phases) {
        unsigned on_count{},off_count{},first_on_seq{},last_on_seq{};
        for(const auto& [seq,on]:policies)if(seq>=phase.first && seq<=phase.last) {
            if(on){++on_count;if(!first_on_seq)first_on_seq=seq;last_on_seq=seq;}else ++off_count;
        }
        std::printf("  phase=%s seq=%u..%u on=%u off=%u first_on=%u last_on=%u\n",
                    phase.name,phase.first,phase.last,on_count,off_count,first_on_seq,last_on_seq);
        unsigned interval_count{},windows{},bad_mean{},bad_median{},idle_gaps{};
        std::int64_t minimum{},maximum{},sum{},worst_mean{},worst_median{},eligible_since{},longest_eligible{};
        for(std::size_t i=1;i<accepted.size();++i) {
            const auto [seq,at]=accepted[i];
            if(seq<phase.first || seq>phase.last)continue;
            const auto interval=at-accepted[i-1].second;
            if(!interval_count || interval<minimum)minimum=interval;
            maximum=std::max(maximum,interval);sum+=interval;++interval_count;
            if(interval>period*2)++idle_gaps;
            if(i<16)continue;
            std::array<std::int64_t,16> samples{};std::int64_t window_sum{};bool idle{};
            for(std::size_t j=0;j<samples.size();++j) {
                samples[j]=accepted[i-j].second-accepted[i-j-1].second;
                window_sum+=samples[j];idle=idle || samples[j]>period*2;
            }
            std::sort(samples.begin(),samples.end());
            const auto median=samples[7]+(samples[8]-samples[7])/2;
            const bool mean_ok=window_sum*99<=period*16*100;
            const bool median_ok=median>0 && median*(1'000'000'000-period)<1'000'000'000*period;
            ++windows;if(!mean_ok)++bad_mean;if(!median_ok)++bad_median;
            worst_mean=std::max(worst_mean,window_sum/16);worst_median=std::max(worst_median,median);
            if(!mean_ok || !median_ok || idle)eligible_since=0;
            else {
                if(!eligible_since)eligible_since=at;
                longest_eligible=std::max(longest_eligible,at-eligible_since);
            }
        }
        std::printf("    stage1 cadence intervals=%u min/mean/max=%.6f/%.6f/%.6fms windows16=%u bad_mean99=%u bad_median=%u idle_gaps=%u worst_mean16=%.6fms worst_median16=%.6fms longest_eligible=%.6fms\n",
                    interval_count,minimum/1e6,interval_count?sum/(1e6*interval_count):0.0,maximum/1e6,
                    windows,bad_mean,bad_median,idle_gaps,worst_mean/1e6,worst_median/1e6,longest_eligible/1e6);
    }
    if(!first_on || !off_again || !second_on || second_fast_timeout) {
        std::printf("  stage38 %s policies:",d12?"D12":"D11");
        for(std::size_t i=0;i<policies.size();++i) {
            if(i%20==0)std::printf("\n   ");
            std::printf(" %u:%u",policies[i].first,unsigned(policies[i].second));
        }
        std::printf("\n");
        for(const auto& [seq,at]:accepted)
            std::printf("  stage1 QPC %s seq=%u at=%lld\n",d12?"D12":"D11",seq,static_cast<long long>(at));
    }
    std::printf("  second-fast bounded timeout=%d inputs=%zu minimum=440\n",second_fast_timeout,expected.size());
    std::fflush(stdout);
    check(expected.size()>=440,"adaptive transitions must retain at least the original 440 exact inputs");
    check(!second_fast_timeout,"second fast phase must requalify within six seconds");
    check(first_on && off_again && second_on,"adaptive transport must execute normal/early/normal/early transitions");
    std::printf("Adaptive native %s transitions, capacity changes, exact GPU pixels and balanced cycles PASS\n",d12?"D12":"D11");
}
void native_controls(bool d12) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,2,true);f.unblock();auto chain=f.make_chain();
    std::vector<unsigned> expected;
    for(unsigned n=0;n<24;++n) {
        if(n==8){xr_ok(f.transport->set_director_enabled(false),"switch to framework");xr_ok(f.transport->set_buffer_capacity(3),"grow future admission");}
        if(n==16){xr_ok(f.transport->set_buffer_capacity(2),"shrink future admission");xr_ok(f.transport->set_director_enabled(true),"switch to director");}
        const auto value=35+n*7;expected.push_back(value);f.write(chain,value);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"submit across policy/capacity changes");f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"control-change drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==24,"control changes retain every exact owned image");
    check(f.transport->buffer_capacity()==2 && f.transport->director_enabled(),"final controls match requested mode");
    check(!f.cycle_waited && !f.cycle_begun,"control changes leave no native cycle open");
    std::printf("Native lookahead %s control/capacity changes and exact pixels PASS\n",d12?"D12":"D11");
}
void native_nondraining_controls(bool adaptive=true) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=adaptive;
    Fixture f(true,true,{},false,true,false,2,true);auto chain=f.make_chain();f.write(chain,83);
    f.begin_virtual();xr_ok(f.submit(chain,1),"own frame while native End is held");f.wait_entered();
    std::promise<void> entered;auto signal=entered.get_future();
    auto update=std::async(std::launch::async,[&]{entered.set_value();
        auto r=f.transport->set_director_enabled(false);if(XR_SUCCEEDED(r))r=f.transport->set_buffer_capacity(3);return r;});
    signal.wait();const bool returned=update.wait_for(50ms)==std::future_status::ready;
    f.unblock();check(update.wait_for(2s)==std::future_status::ready,"control update eventually completes");
    xr_ok(update.get(),"future policy/admission changes");f.wait_count(1);xr_ok(f.transport->drain(),"control update drain");f.transport->stop();
    check(f.pixels==std::vector<unsigned>{83},"control update cannot discard or replace in-flight image");
    check(!f.cycle_waited && !f.cycle_begun,"control update balances native cycles");
    check(returned,"shared native transport controls must not drain accepted frames and create a producer pause");
    std::puts("Shared native transport: mode/buffer update returns with owned End held, exact pixels PASS");
}
void director_scope_and_pair_checks(bool d12) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,2,true);f.unblock();f.throttled=false;
    std::vector<unsigned> native,framework,expected;
    f.diagnostic_work=[&](int stage,uint64_t seq,int64_t) {
        if(stage==38)native.push_back(unsigned(seq));
        if(stage==28)framework.push_back(unsigned(seq));
    };
    const auto chain=f.make_chain();
    for(unsigned n=0;n<48;++n) {
        if(n==12)xr_ok(f.transport->set_director_enabled(false),"restore V819 Framework live");
        if(n==20)xr_ok(f.transport->set_buffer_capacity(3),"V819 Framework Buffer");
        if(n==28)xr_ok(f.transport->set_buffer_capacity(2),"V819 Framework normal");
        if(n==36)xr_ok(f.transport->set_director_enabled(true),"return to isolated Director");
        const auto value=35+n*3;expected.push_back(value);f.write(chain,value);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"paired output through isolated routes");f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"isolated route drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==48,"route changes retain all exact GPU images");
    check(native.size()==24 && framework.size()==24,"native Wait policy only on Director; Framework uses V819 branch");
    for(unsigned n=1;n<48;++n) {
        const bool same_director=(n<12 || n>36);
        if(same_director)check(f.handoffs[n]-f.handoffs[n-1]>=period-20'000,
            "normal Director must not burst a ready pair into one physical refresh");
    }
    check(!f.cycle_waited && !f.cycle_begun,"isolated route changes balance native cycles");
    std::printf("Director-only %s: V819 Framework normal/Buffer, no normal-mode bursts, exact images PASS\n",d12?"D12":"D11");
}
void rife_ready_clock_checks(bool d12,bool directed,std::size_t capacity) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,capacity,true);f.unblock();f.throttled=false;
    xr_ok(f.transport->set_director_enabled(directed),"RIFE selected delivery policy");
    f.transport->set_ready_clock(true);check(f.transport->ready_clock_enabled(),"RIFE ready-clock active");
    f.prediction_horizon_ns=period*3;f.prediction_age_limit=period+500'000;
    f.slow_image_wait_ms=20; // exceeds one90Hz interval, like a late RIFE copy
    const auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<8;++n){
        const auto value=35+n*7;expected.push_back(value);f.write(chain,value);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"RIFE ready-clock source retained");f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"RIFE ready-clock drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==8,"RIFE ready-clock preserves every exact GPU image");
    check(f.discarded_clocks==0,"RIFE preparation must not age and replace each native cycle");
    for(const auto& [seq,ready]:f.copy_ready_times){
        check(f.native_wait_entries.contains(seq),"every prepared image has a native cycle receipt");
        check(f.native_wait_entries.at(seq)>=ready,"RIFE native Wait started before this image copy completed");
    }
    check(f.copy_ready_times.size()==8,"all RIFE prepared images checked");
    check(!f.cycle_waited && !f.cycle_begun,"RIFE stop balances all native cycles");
    f.transport->set_ready_clock(false);check(!f.transport->ready_clock_enabled(),"ordinary OFXR clock policy restored");
    std::printf("RIFE ready clock %s %s capacity%zu:8exact outputs, zero age renewals, native Wait after copy PASS\n",
        d12?"D12":"D11",directed?"Director":"Framework",capacity);
}
void director_aged_cycle_checks(bool d12,bool corrected=true) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=corrected;
    Fixture f(d12,true,{},false,true,false,2);f.unblock();f.throttled=false;
    f.prediction_horizon_ns=period*3;f.prediction_age_limit=period+500'000;
    f.slow_image_wait_ms=20;
    const auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=0;n<8;++n) {
        const auto value=35+n*7;expected.push_back(value);f.write(chain,value);f.begin_virtual();
        xr_ok(f.submit(chain,float(n)),"aged native cycle keeps its exact image");f.wait_count(n+1);
    }
    xr_ok(f.transport->drain(),"aged native cycle drain");f.transport->stop();
    check(f.pixels==expected && f.real_ends==8,"renewing aged native cycles cannot discard accepted frames");
    check(f.discarded_clocks>=8,"long GPU preparation renews a cycle even with future display prediction");
    check(!f.cycle_waited && !f.cycle_begun,"aged-cycle recovery balances all real cycles");
    std::printf("Director-only %s: long preparation, future prediction, renewed native cycles and exact GPU images PASS\n",d12?"D12":"D11");
}
void director_normal_capacity_checks(bool d12) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,2,true);
    check(f.transport->buffer_capacity()==3,"Director Normal has independent current, prepared and producer credits");
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=1;n<=3;++n){expected.push_back(n*40);f.write(chain,n*40);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"three Normal credits accepted with the first End held");}
    f.wait_entered();
    std::atomic_bool attempted{};
    auto fourth=std::async(std::launch::async,[&]{attempted=true;f.write(chain,160);f.begin_virtual();return f.submit(chain,4);});
    while(!attempted)std::this_thread::yield();
    const auto blocked=fourth.wait_for(30ms);
    f.unblock();check(fourth.wait_for(3s)==std::future_status::ready,"retiring native delivery wakes bounded producer");xr_ok(fourth.get(),"fourth Normal delivery admitted");
    check(blocked==std::future_status::timeout,"Normal remains bounded to three owned delivery credits");
    f.wait_count(4);expected.push_back(160);
    xr_ok(f.transport->drain(),"Normal bounded delivery drain");
    check(f.pixels==expected,"Normal three credits preserve every exact GPU image in order");
    xr_ok(f.transport->set_director_enabled(false),"switch Normal to Framework");
    check(f.transport->buffer_capacity()==2,"Framework recovers its requested two-credit Normal mode");
    xr_ok(f.transport->set_buffer_capacity(3),"Framework Buffer still selects three");
    check(f.transport->buffer_capacity()==3,"Framework Buffer retains three credits");
    xr_ok(f.transport->set_buffer_capacity(2),"Framework Normal restored");
    xr_ok(f.transport->set_director_enabled(true),"return to Director Normal");
    check(f.transport->buffer_capacity()==3,"Director minimum is independent of Buffer checkbox");
    xr_ok(f.transport->set_buffer_capacity(3),"Director Buffer enables the extra reserve credit");
    check(f.transport->buffer_capacity()==4,"Director Buffer adds a fourth independent delivery credit");
    xr_ok(f.transport->set_buffer_capacity(2),"Director Normal keeps throughput when Buffer clears");
    check(f.transport->buffer_capacity()==3,"clearing Buffer must not remove a needed Director credit");
    f.transport->stop();check(!f.cycle_waited && !f.cycle_begun,"Normal controls leave all native cycles balanced");
    std::printf("Director Normal %s: three bounded credits, exact GPU images, Framework2/3 restored PASS\n",d12?"D12":"D11");
}
void director_buffer_capacity_checks(bool d12) {
    native_contract_fixture_mode=true;native_adaptive_wait_fixture_mode=true;native_director_only_fixture_mode=true;
    Fixture f(d12,true,{},false,true,false,3,true);
    check(f.transport->buffer_capacity()==4,"Director Buffer owns one more credit than Normal");
    auto chain=f.make_chain();std::vector<unsigned> expected;
    for(unsigned n=1;n<=4;++n){expected.push_back(n*40);f.write(chain,n*40);f.begin_virtual();xr_ok(f.submit(chain,float(n)),"four Buffer credits while native End held");}
    f.wait_entered();
    std::atomic_bool attempted{};
    auto fifth=std::async(std::launch::async,[&]{attempted=true;f.write(chain,200);f.begin_virtual();return f.submit(chain,5);});
    while(!attempted)std::this_thread::yield();
    const auto blocked=fifth.wait_for(30ms);f.unblock();
    check(fifth.wait_for(3s)==std::future_status::ready,"retiring one Buffer delivery wakes producer");
    xr_ok(fifth.get(),"fifth Buffer delivery admitted");
    check(blocked==std::future_status::timeout,"Buffer is bounded to four owned outputs");
    f.wait_count(5);xr_ok(f.transport->drain(),"Buffer four-credit drain");expected.push_back(200);
    check(f.pixels==expected,"four Buffer credits preserve exact independent GPU images");
    xr_ok(f.transport->set_buffer_capacity(2),"Buffer off restores Normal");
    check(f.transport->buffer_capacity()==3,"Buffer off keeps Normal three credits");
    f.transport->stop();check(!f.cycle_waited && !f.cycle_begun,"Buffer leaves native calls balanced");
    std::printf("Director Buffer %s: four bounded credits, fifth blocks, exact ordered images PASS\n",d12?"D12":"D11");
}
void queue_priority_policy() {
    D3D12_COMMAND_QUEUE_DESC original{};original.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;original.NodeMask=1;
    std::vector<int> attempts;
    auto create=[&](bool reject_high,bool reject_all) {
        attempts.clear();return xrfg::create_private_delivery_queue(original,true,[&](const D3D12_COMMAND_QUEUE_DESC& desc)->HRESULT {
            check(desc.Type==original.Type && desc.Flags==original.Flags && desc.NodeMask==original.NodeMask,"priority choice preserves binding queue descriptor");
            attempts.push_back(desc.Priority);
            return reject_all || (reject_high && desc.Priority==D3D12_COMMAND_QUEUE_PRIORITY_HIGH)?E_FAIL:S_OK;
        });
    };
    ok(create(false,false),"supported priority queue");check(attempts==std::vector<int>{100},"deadline delivery requests HIGH once");
    ok(create(true,false),"rejected priority fallback");check(attempts==std::vector<int>({100,0}),"priority rejection retries original dedicated queue");
    check(FAILED(create(true,true)) && attempts==std::vector<int>({100,0}),"both queue failures propagate rather than alias producer queue");
    attempts.clear();ok(xrfg::create_private_delivery_queue(original,false,[&](const D3D12_COMMAND_QUEUE_DESC& desc)->HRESULT{attempts.push_back(desc.Priority);return S_OK;}),"unsupported priority uses original descriptor");
    check(attempts==std::vector<int>{0},"unsupported HIGH is not requested");
    original.Priority=D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    ok(create(false,false),"retain already higher priority");check(attempts==std::vector<int>{10000},"never lower inherited priority or introduce realtime from normal");
    std::puts("Final delivery priority: supported HIGH, descriptor preservation, original dedicated fallback, failure propagation PASS");
}
}
#include "private_overlay_destroy_fixture.inc"
int main(int argc,char** argv){if(argc>1 && std::string_view(argv[1])=="clean-bypass"){stale_native_clock(true,true,true);stale_native_clock(false,true,true);clean_bypass_roundtrip(true);clean_bypass_roundtrip(false);return 0;}if(argc>1 && std::string_view(argv[1])=="ui-destroy"){optional_overlay_destroy(true);optional_overlay_destroy(false);return 0;}
if(argc>1 && std::string_view(argv[1])=="content-refusal"){content_refusal_paths();return 0;}
if(argc>1 && std::string_view(argv[1])=="stalled-renewal"){
    for(bool d12:{true,false})for(bool directed:{true,false})stalled_native_renewal(d12,directed,0);
    for(bool directed:{true,false})for(int pattern:{1,2})stalled_native_renewal(true,directed,pattern);
    return 0;
}
if(argc>1 && std::string_view(argv[1])=="stalled-renewal-native"){
    native_contract_fixture_mode=true;stalled_native_renewal(true,true,0);stalled_native_renewal(false,true,0);
    native_director_only_fixture_mode=true;stalled_native_renewal(true,true,0);stalled_native_renewal(false,true,0);
    return 0;
}
if(argc>1 && std::string_view(argv[1])=="rife-ready-clock"){
    for(bool d12:{false,true})for(bool directed:{false,true})for(std::size_t capacity:{2u,3u})rife_ready_clock_checks(d12,directed,capacity);
    return 0;
}

if(argc>1 && std::string_view(argv[1])=="director-native-buffer"){native_contract_fixture_mode=true;native_director_only_fixture_mode=true;native_adaptive_wait_fixture_mode=false;adaptive_buffer_test(true,true);adaptive_buffer_test(true,false);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-buffer-capacity"){director_buffer_capacity_checks(false);director_buffer_capacity_checks(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-normal-capacity"){director_normal_capacity_checks(false);director_normal_capacity_checks(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-scope-pairs"){director_scope_and_pair_checks(false);director_scope_and_pair_checks(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-qualified"){native_director_only_fixture_mode=true;native_refresh_policy_checks();native_adaptive_transitions(false);native_adaptive_transitions(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-aged-cycle"){director_aged_cycle_checks(false);director_aged_cycle_checks(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="director-aged-cycle-control"){director_aged_cycle_checks(false,false);return 0;}
if(argc>1 && std::string_view(argv[1])=="native-contract"){native_contract_reference_checks();return 0;}
if(argc>1 && std::string_view(argv[1])=="native-adaptive-wait"){native_refresh_policy_checks();native_adaptive_transitions(false);native_adaptive_transitions(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="native-controls"){native_controls(false);native_controls(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="native-nondraining-controls"){native_nondraining_controls(false);native_nondraining_controls(true);return 0;}
if(argc>1 && std::string_view(argv[1])=="buffer-release-early"){for(bool d:{false,true}){for(bool enabled:{false,true})for(bool alias:{false,true})buffer_release_before_begin(d,enabled,alias);buffer_early_release_cancel(d,false);buffer_early_release_cancel(d,true);}return 0;}if(argc>1 && std::string_view(argv[1])=="buffer-cold-quiet"){buffer_cold_start_logging_off(false);buffer_cold_start_logging_off(true);return 0;}if(argc>1 && std::string_view(argv[1])=="buffer-metadata"){buffer_early_metadata(true);buffer_early_metadata(false);return 0;}if(argc>1 && (std::string_view(argv[1])=="buffer-framework" || std::string_view(argv[1])=="buffer-director")){const bool d=std::string_view(argv[1])=="buffer-director";adaptive_buffer_test(d,true);adaptive_buffer_test(d,false);return 0;}if(argc>1 && std::string_view(argv[1])=="bypass-expired"){stale_native_clock(true,false);stale_native_clock(false,false);return 0;}std::setvbuf(stdout,nullptr,_IONBF,0);if(argc>1){if(std::string_view(argv[1])=="vram-hdr"){vram_hdr_publication_tests();return 0;}if(std::string_view(argv[1])=="owned-d11"){owned_d11_output_tests();return 0;}if(std::string_view(argv[1])=="vram"){vram_publication_tests();return 0;}if(std::string_view(argv[1])=="bypass"){fixture_gpu_completion_proof();live_framework_bypass(true);live_framework_bypass(false);bypass_policy_drain();}else if(std::string_view(argv[1])=="metrics"){native_delivery_receipts(true);native_delivery_receipts(false);}else if(std::string_view(argv[1])=="pool"){pooled_publication();pooled_publication(true);pooled_publication(false,false);pooled_publication(true,false);}else if(std::string_view(argv[1])=="luke"){luke_acquire_ahead(true);luke_acquire_ahead(false);acquired_capacity(true);acquired_capacity(false);}else if(std::string_view(argv[1])=="advance"){native_end_advance(true);native_end_advance(false);}else if(std::string_view(argv[1])=="source"){copied_source_reuse(true);copied_source_reuse(false);}else if(std::string_view(argv[1])=="clock"){stale_native_clock(true);stale_native_clock(false);native_renewal_failure();nonrenderable_native_clock();bounded_native_window(true);bounded_native_window(false);quiet_native_clock(true);quiet_native_clock(false);}else if(std::string_view(argv[1])=="phase"){physical_clock_pipeline(true);physical_clock_pipeline(false);late_physical_clock();}else if(std::string_view(argv[1])=="margin"){native_margin_scope(true);native_margin_scope(false);}else if(std::string_view(argv[1])=="pipeline"){overlapped_native_wait(false);overlapped_native_wait(true);overlapped_wait_failure();}else if(std::string_view(argv[1])=="fresh"){fresh_runtime_window(true);fresh_runtime_window(false);fresh_runtime_window(true,5);fresh_runtime_window(false,5);}else{cadence_precision(true);cadence_precision(false);}return 0;}stale_native_clock(true);stale_native_clock(false);native_renewal_failure();nonrenderable_native_clock();bounded_native_window(true);bounded_native_window(false);queue_priority_policy();ownership(true);ownership(false);stop_wakeup();failure_wakeup();pending_gpu();preparation_overlap(true);preparation_overlap(false);future_work_dependency(false);future_work_dependency(true);cadence_precision(true);cadence_precision(false);preparation_independence(true);preparation_independence(false);stalled_preparer();unchanged_overlay();same_chain_lookahead(true);same_chain_lookahead(false);changing_overlay_lookahead();stop_prepared_target();reused_real_index(false);reused_real_index(true);static_image_repeat();stop_first_static_target();overlapped_native_wait(false);overlapped_native_wait(true);overlapped_wait_failure();physical_clock_pipeline(true);physical_clock_pipeline(false);late_physical_clock();native_margin_scope(true);native_margin_scope(false);rising_native_wait();fresh_runtime_window(true);fresh_runtime_window(false);fresh_runtime_window(true,5);fresh_runtime_window(false,5);adaptive_source_refill(true);adaptive_source_refill(false);adaptive_source_refill(true,2);adaptive_source_refill(false,2);quiet_native_clock(true);quiet_native_clock(false);copied_source_reuse(true);copied_source_reuse(false);native_end_advance(true);native_end_advance(false);luke_acquire_ahead(true);luke_acquire_ahead(false);acquired_capacity(true);acquired_capacity(false);return 0;}
