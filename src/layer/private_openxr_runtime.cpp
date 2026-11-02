#include "xrfg/private_openxr_runtime.hpp"
#include "xrfg/private_output_distributor.hpp"
#include "xrfg/interface_metrics.hpp"
#include "xrfg/owned_d11_output.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
std::int64_t clock_ns() noexcept {
    LARGE_INTEGER q{},f{};QueryPerformanceCounter(&q);QueryPerformanceFrequency(&f);
    return q.QuadPart/f.QuadPart*1'000'000'000LL+
        q.QuadPart%f.QuadPart*1'000'000'000LL/f.QuadPart;
}
XrResult xr(HRESULT result) {return SUCCEEDED(result)?XR_SUCCESS:XR_ERROR_RUNTIME_FAILURE;}
template<class T> bool valid(const T* value,XrStructureType type) {
    return !value || (value->type==type && !value->next);
}
using Layer=std::variant<XrCompositionLayerProjection,XrCompositionLayerQuad,
    XrCompositionLayerCubeKHR,XrCompositionLayerCylinderKHR,
    XrCompositionLayerEquirectKHR,XrCompositionLayerEquirect2KHR>;
struct LayerCopy {
    Layer value;
    std::vector<XrCompositionLayerProjectionView> views;
    std::vector<XrCompositionLayerDepthInfoKHR> depths;
    std::optional<XrCompositionLayerColorScaleBiasKHR> color_scale;
    const XrCompositionLayerBaseHeader* header() {
        return std::visit([](auto& x){return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&x);},value);
    }
};
} // namespace

struct PrivateOpenXrRuntime::Impl {
    struct PreparedTarget;
    struct Image {
        ComPtr<ID3D12Resource> d12;
        ComPtr<ID3D11Texture2D> d11;
        unsigned readers{};
        std::uint64_t generation{};
        std::uint64_t ready11_value{};
        bool acquired{},waited{};
    };
    struct Chain {
        XrSwapchain handle{};
        // Internally generated outputs may share this publication owner only
        // within one original source and identical resource/state contract.
        // Application handles retain their separate native XR ownership.
        std::shared_ptr<Chain> publication;
        XrSwapchain publication_source{};
        unsigned publication_owners{};
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        std::vector<Image> images;
        std::vector<ComPtr<ID3D12Resource>> real12;
        std::vector<ComPtr<ID3D11Texture2D>> real11;
        std::deque<unsigned> acquired;
        std::optional<unsigned> released;
        unsigned cursor{};
        unsigned consumers{}; // retain the real XR chain until every native End finishes
        bool destroying{};
        std::shared_ptr<PreparedTarget> latest_target,pending_target;
        std::vector<std::weak_ptr<PreparedTarget>> retained_targets;
        D3D12_RESOURCE_STATES image_state{D3D12_RESOURCE_STATE_RENDER_TARGET};
    };
    struct Lease {std::shared_ptr<Chain> chain;unsigned image{};std::uint64_t generation{};bool source_released{};};
    struct PreparedTarget {
        std::weak_ptr<Chain> chain;
        unsigned source{},index{},users{};
        std::uint64_t source_generation{};
        bool acquired{},waited{},complete{},released{};
    };
    struct Packet {
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
        std::shared_ptr<InterfaceDeliveryReceipt> delivery;
        std::vector<LayerCopy> copies;
        std::vector<const XrCompositionLayerBaseHeader*> layers;
        std::vector<Lease> leases;
        std::vector<std::shared_ptr<PreparedTarget>> targets,copies_needed;
        std::uint64_t sequence{},ready{};
        std::int64_t accepted_at{},source_ready_at{},prepared_at{};
        std::int64_t work_ns{};
        bool director{true};
        std::uint64_t policy_epoch{};
    };
    struct WaitResult {
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        XrResult result{XR_SUCCESS};
        std::int64_t entered{},returned{};
    };
    struct WaitJob {std::int64_t target{};std::uint64_t accepted_ordinal{};};
    Options options;
    std::atomic_bool director_enabled{true};
    std::uint64_t policy_epoch{}; // under accept_mutex
    std::mutex mutex,accept_mutex,copy_mutex,stop_mutex;
    std::condition_variable changed;
    std::unordered_map<XrSwapchain,std::shared_ptr<Chain>> chains;
    std::vector<std::weak_ptr<Chain>> publication_pools;
    static Chain& destination(Chain& chain) noexcept {
        return chain.publication?*chain.publication:chain;
    }
    static bool compatible(const Chain& chain,const XrSwapchainCreateInfo& info,
                           D3D12_RESOURCE_STATES state,XrSwapchain source) noexcept {
        const auto& a=chain.info;
        return chain.publication_source==source && chain.image_state==state &&
            a.createFlags==info.createFlags && a.usageFlags==info.usageFlags &&
            a.format==info.format && a.sampleCount==info.sampleCount &&
            a.width==info.width && a.height==info.height && a.faceCount==info.faceCount &&
            a.arraySize==info.arraySize && a.mipCount==info.mipCount;
    }
    std::unique_ptr<PrivateOutputFifo<std::shared_ptr<Packet>>> fifo;
    std::thread consumer,preparer,waiter;
    std::optional<WaitJob> wait_job;
    std::optional<WaitResult> wait_result;
    bool wait_active{};
    std::uint64_t accepted_packets{},content_wait_jobs{};
    std::deque<std::shared_ptr<Packet>> prepared;
    std::atomic_bool stopping{true};
    XrResult failure{XR_SUCCESS};
    XrFrameState anchor{XR_TYPE_FRAME_STATE};
    std::int64_t anchor_at{};
    XrTime virtual_time{};
    unsigned waits{},begun{};
    bool seeded{},running{};
    std::uint64_t sequence{},ready_serial{},copy_serial{};
    std::size_t pending{};
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12CommandQueue> producer_queue12;
    ComPtr<ID3D12Fence> ready12,copy12;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11Fence> ready11,copy11;
    HANDLE fence_event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    HANDLE timer{CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)};
    HANDLE wait_timer{CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS)};
    explicit Impl(Options input):options(std::move(input)),director_enabled(options.director_enabled) {}
    ~Impl(){if(fence_event)CloseHandle(fence_event);if(timer)CloseHandle(timer);if(wait_timer)CloseHandle(wait_timer);}
    bool d12() const noexcept {return device12!=nullptr;}
    std::int64_t period(XrDuration fallback) {
        auto value=options.physical_period?options.physical_period():0;
        return value>=2'000'000 && value<=50'000'000?value:
            std::clamp<std::int64_t>(fallback,2'000'000,50'000'000);
    }
    void trace(int stage,std::uint64_t seq,std::int64_t hold=0) {
        if(tracing())options.trace(stage,seq,clock_ns(),hold);
    }
    bool tracing() const {return options.trace && (!options.trace_enabled || options.trace_enabled());}
    template<class Fence> HRESULT await(Fence* fence,std::uint64_t value,bool interruptible=true) {
        if(!value)return S_OK;
        auto completed=fence->GetCompletedValue();
        if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(completed>=value)return S_OK;
        HRESULT result=fence->SetEventOnCompletion(value,fence_event);
        if(FAILED(result))return result;
        while(fence->GetCompletedValue()<value) {
            if(interruptible && stopping)return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            if(fence->GetCompletedValue()==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
            const auto waited=WaitForSingleObject(fence_event,5);
            if(waited!=WAIT_OBJECT_0 && waited!=WAIT_TIMEOUT)return E_FAIL;
        }
        return fence->GetCompletedValue()==UINT64_MAX?DXGI_ERROR_DEVICE_REMOVED:S_OK;
    }
    bool sleep_until(std::int64_t target,std::int64_t fine_window,bool native_clock=false) {
        const auto selected_timer=native_clock?wait_timer:timer;
        for(;;) {
            if(stopping)return false;
            auto left=target-clock_ns();if(left<=0)return true;
            // Let the timer do the long wait, waking before the deadline.
            // Its positive wakeup jitter must not be added to EVERY period.
            // The final bounded window uses QPC/pause on this consumer only;
            // no producer, GPU or runtime-entry lock is held here.
            if(left<=fine_window){YieldProcessor();continue;}
            left=std::min<std::int64_t>(left-fine_window,5'000'000);
            LARGE_INTEGER due{};due.QuadPart=-std::max<std::int64_t>(1,(left+99)/100);
            if(selected_timer && SetWaitableTimer(selected_timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(selected_timer,10);
            else {std::unique_lock lock(mutex);changed.wait_for(lock,std::chrono::nanoseconds(left));}
        }
    }
    void fail(XrResult result) {
        {std::scoped_lock lock(mutex);if(XR_SUCCEEDED(failure))failure=result;}
        trace(-1,static_cast<std::uint64_t>(-result));
        stopping=true;fifo->stop();changed.notify_all();
    }
    void retire(const std::shared_ptr<Packet>& packet) {
        {std::scoped_lock lock(mutex);
         for(auto& lease:packet->leases) {
             if(!lease.source_released && lease.chain->images[lease.image].readers)
                 --lease.chain->images[lease.image].readers;
             lease.source_released=true;
             if(lease.chain->consumers)--lease.chain->consumers;
         }
         for(auto& target:packet->targets)if(target->users)--target->users;
         if(pending)--pending;}
        changed.notify_all();
    }
    XrResult lease(Packet& packet,XrSwapchain handle) {
        if(std::any_of(packet.leases.begin(),packet.leases.end(),[&](auto& x){return x.chain->handle==handle;}))return XR_SUCCESS;
        auto it=chains.find(handle);
        if(it==chains.end() || it->second->destroying)return XR_ERROR_HANDLE_INVALID;
        auto& chain=*it->second;
        if(!chain.released || chain.images[*chain.released].acquired)return XR_ERROR_CALL_ORDER_INVALID;
        packet.leases.push_back({it->second,*chain.released,chain.images[*chain.released].generation});
        if(options.d11_release_readiness && !d12())
            packet.ready=std::max(packet.ready,chain.images[*chain.released].ready11_value);
        ++chain.consumers;
        ++chain.images[*chain.released].readers;return XR_SUCCESS;
    }
    void release_copied_sources(Packet& packet) {
        // Only publish()'s completed copy fence authorizes this transition.
        // Real targets, chain consumers and FIFO credits remain owned until End.
        {std::scoped_lock lock(mutex);
         for(auto& lease:packet.leases)if(!lease.source_released) {
             if(lease.chain->images[lease.image].readers)--lease.chain->images[lease.image].readers;
             lease.source_released=true;
         }}
        changed.notify_all();
        if(tracing())trace(26,packet.sequence,static_cast<std::int64_t>(packet.leases.size()));
    }
    // Called under mutex. Every pointer submitted after EndFrame returns is
    // owned here. Unknown extension chains are refused, never borrowed.
    XrResult own(Packet& packet,const XrFrameEndInfo& source) {
        if(source.next || (source.layerCount && !source.layers))return XR_ERROR_VALIDATION_FAILURE;
        packet.info=source;packet.copies.reserve(source.layerCount);packet.layers.reserve(source.layerCount);
        auto take=[&](XrSwapchain h){return lease(packet,h);};
        for(unsigned i=0;i<source.layerCount;++i) {
            auto* header=source.layers[i];if(!header)return XR_ERROR_LAYER_INVALID;
            packet.copies.emplace_back();auto& copy=packet.copies.back();
            if(header->next){auto* next=static_cast<const XrCompositionLayerColorScaleBiasKHR*>(header->next);
                if(next->type!=XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR || next->next)return XR_ERROR_LAYER_INVALID;
                copy.color_scale=*next;}
            XrResult result=XR_SUCCESS;
            switch(header->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                auto p=*reinterpret_cast<const XrCompositionLayerProjection*>(header);
                if(!p.views || !p.viewCount)return XR_ERROR_LAYER_INVALID;
                copy.views.assign(p.views,p.views+p.viewCount);copy.depths.resize(p.viewCount);
                for(unsigned v=0;v<p.viewCount;++v) {
                    auto& view=copy.views[v];result=take(view.subImage.swapchain);if(XR_FAILED(result))return result;
                    if(view.next) {
                        auto* ext=static_cast<const XrCompositionLayerDepthInfoKHR*>(view.next);
                        if(ext->type!=XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR || ext->next)return XR_ERROR_LAYER_INVALID;
                        copy.depths[v]=*ext;view.next=&copy.depths[v];
                        result=take(ext->subImage.swapchain);if(XR_FAILED(result))return result;
                    }
                }
                p.views=copy.views.data();copy.value=p;break;
            }
#define COPY_LAYER(tag,type,handle) case tag: {auto value=*reinterpret_cast<const type*>(header);result=take(value.handle);copy.value=value;break;}
            COPY_LAYER(XR_TYPE_COMPOSITION_LAYER_QUAD,XrCompositionLayerQuad,subImage.swapchain)
            COPY_LAYER(XR_TYPE_COMPOSITION_LAYER_CUBE_KHR,XrCompositionLayerCubeKHR,swapchain)
            COPY_LAYER(XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR,XrCompositionLayerCylinderKHR,subImage.swapchain)
            COPY_LAYER(XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR,XrCompositionLayerEquirectKHR,subImage.swapchain)
            COPY_LAYER(XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR,XrCompositionLayerEquirect2KHR,subImage.swapchain)
#undef COPY_LAYER
            default:return XR_ERROR_LAYER_INVALID;
            }
            if(XR_FAILED(result))return result;
            if(copy.color_scale)std::visit([&](auto& layer){layer.next=&*copy.color_scale;},copy.value);
            packet.layers.push_back(copy.header());
        }
        packet.info.layers=packet.layers.data();return XR_SUCCESS;
    }
    // The preparer may fill the next acquired real image while EndFrame uses
    // the previously released image. Keep that next image WAITED, unreleased:
    // release selects the image seen by EndFrame and belongs to FIFO delivery.
    // OpenXR permits only one successfully waited, unreleased image per chain.
    // Repeated immutable source images share their exact prepared generation.
    bool reserve_preparation(Packet& packet) {
        std::unique_lock lock(mutex);
        for(auto& lease:packet.leases) {
            auto chain=lease.chain;
            auto& endpoint=destination(*chain);
            // A simultaneous second eye/domain must never alias the same
            // last-released XR image. Pool keys are source-specific; reject an
            // invalid internal composition rather than silently overwrite it.
            if(std::any_of(packet.targets.begin(),packet.targets.end(),[&](auto& target) {
                auto owner=target->chain.lock();return owner && &destination(*owner)==&endpoint;
            }))return false;
            auto reusable=[&]{return endpoint.latest_target && (endpoint.latest_target->users ||
                    (chain->info.createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)) &&
                    endpoint.latest_target->chain.lock()==chain &&
                    endpoint.latest_target->source==lease.image &&
                    endpoint.latest_target->source_generation==lease.generation;};
            changed.wait(lock,[&]{return stopping || !endpoint.pending_target || reusable();});
            if(stopping)return false;
            auto target=endpoint.latest_target;
            if(!reusable()) {
                target=std::make_shared<PreparedTarget>();target->chain=chain;target->source=lease.image;
                target->source_generation=lease.generation;
                endpoint.latest_target=endpoint.pending_target=target;
                std::erase_if(endpoint.retained_targets,[](auto& weak){return weak.expired();});
                endpoint.retained_targets.push_back(target);
                packet.copies_needed.push_back(target);
            }
            ++target->users;packet.targets.push_back(std::move(target));
        }
        if(std::none_of(packet.leases.begin(),packet.leases.end(),[](const auto& lease){return bool(lease.chain->publication);}))return true;
        // Metadata, rectangles, slices and poses remain the owned packet's;
        // only virtual generated handles are resolved at the native boundary.
        auto resolve=[&](XrSwapchain& handle) {
            const auto it=chains.find(handle);
            if(it!=chains.end())handle=destination(*it->second).handle;
        };
        for(auto& copy:packet.copies)std::visit([&](auto& layer) {
            using T=std::decay_t<decltype(layer)>;
            if constexpr(std::is_same_v<T,XrCompositionLayerProjection>) {
                for(auto& view:copy.views)resolve(view.subImage.swapchain);
                for(auto& depth:copy.depths)if(depth.type==XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR)
                    resolve(depth.subImage.swapchain);
            } else if constexpr(std::is_same_v<T,XrCompositionLayerCubeKHR>)resolve(layer.swapchain);
            else resolve(layer.subImage.swapchain);
        },copy.value);
        return true;
    }
    XrResult publish(Packet& packet) {
        const bool diagnostic=tracing();
        std::vector<std::shared_ptr<PreparedTarget>> targets;
        targets.reserve(packet.copies_needed.size());
        XrResult result=XR_SUCCESS;
        for(auto& target:packet.copies_needed) {
            if(stopping){result=XR_ERROR_SESSION_NOT_RUNNING;break;}
            auto chain=target->chain.lock();if(!chain){result=XR_ERROR_HANDLE_INVALID;break;}
            auto& endpoint=destination(*chain);
            unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            result=options.real.acquire(endpoint.handle,&acquire,&index);if(XR_FAILED(result))break;
            target->index=index;target->acquired=true;
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
            result=options.real.wait_image(endpoint.handle,&wait);if(XR_FAILED(result))break;
            target->waited=true;
            // A runtime may return the same physical image that an earlier
            // released generation still needs for EndFrame (including repeats).
            // Only that actual alias waits; different real indices keep their
            // preparation independent of all older presentation leases.
            {std::unique_lock lock(mutex);
             auto available=[&]{return std::none_of(endpoint.retained_targets.begin(),endpoint.retained_targets.end(),[&](auto& weak){
                 auto other=weak.lock();return other && other!=target && other->users && other->acquired && other->index==index;});};
             changed.wait(lock,[&]{return stopping || available();});
             if(!available()){result=XR_ERROR_SESSION_NOT_RUNNING;break;}}
            targets.push_back(target);
            // Finish safe, already-waited targets even when cancellation arrived
            // during native WaitImage. A static target must be copied before its
            // one permitted Release; it cannot be acquired again on restart.
            if(stopping){result=XR_ERROR_SESSION_NOT_RUNNING;break;}
        }
        if(diagnostic)trace(12,packet.sequence);
        if(!targets.empty()) {
            std::scoped_lock gpu(copy_mutex);
            const auto copy_started=clock_ns();
            HRESULT copied=S_OK;
            if(d12()) {
                copied=allocator->Reset();if(SUCCEEDED(copied))copied=list->Reset(allocator.Get(),nullptr);
                for(auto& target:targets) {
                    if(FAILED(copied))break;
                    auto owner=target->chain.lock();if(!owner){copied=E_INVALIDARG;break;}
                    auto& chain=*owner;auto& endpoint=destination(chain);auto& image=chain.images[target->source];
                    if(target->index>=endpoint.real12.size()){copied=E_INVALIDARG;break;}
                    const auto state=chain.image_state;
                    D3D12_RESOURCE_BARRIER barriers[2]{};
                    for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
                    barriers[0].Transition.pResource=image.d12.Get();barriers[0].Transition.StateBefore=state;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                    barriers[1].Transition.pResource=endpoint.real12[target->index].Get();barriers[1].Transition.StateBefore=state;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(2,barriers);list->CopyResource(endpoint.real12[target->index].Get(),image.d12.Get());
                    for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(2,barriers);
                }
                if(SUCCEEDED(copied))copied=list->Close();
                if(SUCCEEDED(copied)){ID3D12CommandList* lists[]={list.Get()};queue12->ExecuteCommandLists(1,lists);copied=queue12->Signal(copy12.Get(),copy_serial+1);if(SUCCEEDED(copied))++copy_serial;}
                if(diagnostic)trace(13,packet.sequence);
                if(SUCCEEDED(copied))copied=await(copy12.Get(),copy_serial,false);
            } else {
                for(auto& target:targets) {
                    auto owner=target->chain.lock();if(!owner){copied=E_INVALIDARG;break;}
                    auto& chain=*owner;auto& endpoint=destination(chain);
                    if(target->index>=endpoint.real11.size()){copied=E_INVALIDARG;break;}
                    context11->CopyResource(endpoint.real11[target->index].Get(),chain.images[target->source].d11.Get());
                }
                if(SUCCEEDED(copied)){copied=context11->Signal(copy11.Get(),copy_serial+1);if(SUCCEEDED(copied))++copy_serial;}
                context11->Flush();if(diagnostic)trace(13,packet.sequence);
                if(SUCCEEDED(copied))copied=await(copy11.Get(),copy_serial,false);
            }
            packet.work_ns+=clock_ns()-copy_started;
            if(diagnostic)trace(14,packet.sequence);
            if(SUCCEEDED(copied))for(auto& target:targets)target->complete=true;
            if(XR_SUCCEEDED(result))result=xr(copied);
        }
        return result;
    }
    XrResult release_prepared(Packet& packet) {
        for(auto& target:packet.targets)if(!target->released) {
            auto chain=target->chain.lock();if(!chain || !target->waited || !target->complete)return XR_ERROR_CALL_ORDER_INVALID;
            auto& endpoint=destination(*chain);
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            const auto result=options.real.release(endpoint.handle,&release);if(XR_FAILED(result))return result;
            {std::scoped_lock lock(mutex);target->released=true;
             if(endpoint.pending_target==target)endpoint.pending_target.reset();}
            changed.notify_all();
        }
        return XR_SUCCESS;
    }
    void prepare() noexcept {
        try {
            while(!stopping) {
                auto entry=fifo->pop();if(!entry)break;auto packet=std::move(*entry);
                const bool source_pending=d12()?ready12->GetCompletedValue()<packet->ready:
                    ready11->GetCompletedValue()<packet->ready;
                const auto source_wait_enter=clock_ns();
                const auto ready=d12()?await(ready12.Get(),packet->ready):await(ready11.Get(),packet->ready);
                // A source already complete when dequeued may have spent time
                // behind a real-chain lease. Do not mistake that backlog for
                // input jitter and add refill delay on every prepared image.
                packet->source_ready_at=source_pending?clock_ns():packet->accepted_at;
                if(source_pending)packet->work_ns=std::max(std::int64_t{},packet->source_ready_at-source_wait_enter);
                if(tracing())options.trace(10,packet->sequence,packet->source_ready_at,source_pending?1:0);
                if(FAILED(ready) || !reserve_preparation(*packet)) {
                    if(!stopping)fail(XR_ERROR_RUNTIME_FAILURE);
                    retire(packet);fifo->retire();break;
                }
                trace(4,packet->sequence);
                const auto result=publish(*packet);
                if(XR_FAILED(result)) {if(!stopping)fail(result);retire(packet);fifo->retire();break;}
                release_copied_sources(*packet);
                trace(5,packet->sequence);
                packet->prepared_at=clock_ns();
                {std::scoped_lock lock(mutex);prepared.push_back(std::move(packet));}
                changed.notify_all();
            }
        } catch(...) {fail(XR_ERROR_RUNTIME_FAILURE);}
    }
    XrResult real_wait(XrFrameState& prediction) {
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        auto result=options.real.wait(options.real.session,&wait,&prediction);
        if(XR_SUCCEEDED(result)) {
            std::scoped_lock lock(mutex);anchor=prediction;anchor.next=nullptr;anchor_at=clock_ns();seeded=true;changed.notify_all();
        }
        return result;
    }
    bool post_wait(std::int64_t target,bool needs_input=true) {
        {std::scoped_lock lock(mutex);
         if(stopping || wait_job || wait_active || wait_result)return false;
         wait_job=WaitJob{target,needs_input?++content_wait_jobs:0};}
        changed.notify_all();return true;
    }
    std::optional<WaitResult> take_wait() {
        std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping || wait_result.has_value();});
        if(stopping)return {};
        auto result=std::move(wait_result);wait_result.reset();return result;
    }
    void wait_loop() noexcept {
        try {for(;;) {
            WaitJob job;
            {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping ||
                (wait_job && (wait_job->accepted_ordinal==0 ||
                    (director_enabled.load()?accepted_packets>=wait_job->accepted_ordinal:!prepared.empty())));});
             if(stopping)break;job=*wait_job;wait_job.reset();wait_active=true;}
            if(!sleep_until(job.target,1'250'000,true)) {
                std::scoped_lock lock(mutex);wait_active=false;break;
            }
            WaitResult result;result.entered=clock_ns();
            try {result.result=real_wait(result.prediction);}
            catch(...) {result.result=XR_ERROR_RUNTIME_FAILURE;}
            result.returned=clock_ns();
            // Optional logging must not turn a successful native Wait into an
            // unbalanced failed clock if an observer callback itself throws.
            try {if(tracing())options.trace(18,job.accepted_ordinal,result.entered,GetCurrentThreadId());}catch(...) {}
            {std::scoped_lock lock(mutex);wait_active=false;wait_result=std::move(result);}
            changed.notify_all();
        }} catch(...) {
            WaitResult result;result.result=XR_ERROR_RUNTIME_FAILURE;
            {std::scoped_lock lock(mutex);wait_active=false;wait_result=result;wait_job.reset();}
            changed.notify_all();
        }
    }
    std::optional<std::int64_t> prediction_qpc(const XrFrameState& prediction) {
        if(options.real.to_qpc) {
            LARGE_INTEGER target{},frequency{};QueryPerformanceFrequency(&frequency);
            if(XR_SUCCEEDED(options.real.to_qpc(options.real.instance,prediction.predictedDisplayTime,&target))) {
                return target.QuadPart/frequency.QuadPart*1'000'000'000LL+
                    target.QuadPart%frequency.QuadPart*1'000'000'000LL/frequency.QuadPart;
            }
        }
        return {};
    }
    XrTime display_time(const XrFrameState& prediction,std::int64_t* display_qpc=nullptr) {
        auto result=prediction.predictedDisplayTime;
        if(auto at=prediction_qpc(prediction)) {
            const auto deficit=clock_ns()+PrivateOutputSchedule::preparation_lead-*at;
            // Cleanup/late-CPU fallback only. Content delivery renews expired
            // native cycles before posting lookahead; changing this timestamp
            // alone cannot renew a SteamVR cycle after an input gap.
            if(deficit>0) {const auto step=period(prediction.predictedDisplayPeriod);
                result+=(deficit+step-1)/step*step;}
            if(display_qpc)*display_qpc=*at+(result-prediction.predictedDisplayTime);
        }
        return result;
    }
    void run() noexcept {
        bool waited=false,begun=false;
        XrFrameState prediction{XR_TYPE_FRAME_STATE};
        try {
            PrivateOutputSchedule schedule;
            PrivatePhysicalSchedule slots;
            std::int64_t last_handoff{};
            std::uint64_t applied_policy{};
            // Request the runtime cycle near its actual handoff, not at the
            // start of an entire refresh/preparation interval. Reserve only
            // Wait/Begin/Release cost and its measured positive variation.
            // Preparation remains independent of the native entry forecast.
            PrivateRuntimeBudget runtime_budget;
            auto result=XR_SUCCESS;
            if(!post_wait(0,false))result=XR_ERROR_SESSION_NOT_RUNNING;
            auto seed=XR_SUCCEEDED(result)?take_wait():std::optional<WaitResult>{};
            if(seed){prediction=seed->prediction;result=seed->result;waited=XR_SUCCEEDED(result);}
            else result=XR_ERROR_SESSION_NOT_RUNNING;
            // Seed the producer's virtual clock, then close this empty cycle
            // immediately. Holding the seed Wait until a level finishes loading
            // would make the first content prediction arbitrarily old.
            XrFrameBeginInfo seed_begin{XR_TYPE_FRAME_BEGIN_INFO};
            if(XR_SUCCEEDED(result))result=options.real.begin(options.real.session,&seed_begin);
            if(XR_SUCCEEDED(result)) {
                waited=false;begun=true;
                XrFrameEndInfo seed_end{XR_TYPE_FRAME_END_INFO};
                seed_end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                seed_end.displayTime=prediction.predictedDisplayTime;
                result=options.real.end(options.real.session,&seed_end);begun=false;
            }
            if(XR_FAILED(result) && !stopping)fail(result);
            if(!stopping && !post_wait(0))fail(XR_ERROR_CALL_ORDER_INVALID);
            while(!stopping) {
                // Start the runtime clock as soon as owned input exists. Image
                // acquisition/copy/GPU waits run on the independent preparer,
                // concurrently with WaitFrame and the physical cadence clock.
                {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping || pending;});if(stopping)break;}
                const auto physical=period(prediction.predictedDisplayPeriod);
                schedule.period(physical);
                if(director_enabled.load() && options.physical_clock)if(auto clock=options.physical_clock())slots.clock(*clock);
                const bool diagnostic=tracing();
                auto clock=take_wait();if(!clock)break;
                prediction=clock->prediction;result=clock->result;
                auto wait_enter=clock->entered,wait_leave=clock->returned;
                const auto begin_enter=clock_ns();
                if(XR_SUCCEEDED(result)) {
                    waited=true;
                    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
                    result=options.real.begin(options.real.session,&begin);
                    if(XR_SUCCEEDED(result)){waited=false;begun=true;}
                }
                if(XR_FAILED(result)){fail(result);break;}
                auto begun_at=clock_ns();
                std::shared_ptr<Packet> packet;
                {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stopping || !prepared.empty();});
                 if(stopping)break;packet=std::move(prepared.front());prepared.pop_front();}
                const bool directed=packet->director;
                if(packet->policy_epoch!=applied_policy) {
                    // A live bypass round-trip must not revive an old phase,
                    // work floor or adaptive delay. Native/image owners stay.
                    schedule={};slots={};schedule.period(physical);
                    if(last_handoff){schedule.runtime_ready(last_handoff,0);schedule.delivered(last_handoff);}
                    if(directed && options.physical_clock)
                        if(auto clock=options.physical_clock())slots.clock(*clock);
                    applied_policy=packet->policy_epoch;
                }
                // All copies/fences are complete. Commit only this FIFO head's
                // real images; a later prepared image remains unreleased until
                // its turn, so it cannot replace this frame's scene or overlay.
                const auto release_enter=clock_ns();
                result=release_prepared(*packet);
                const auto release_leave=clock_ns();
                if(XR_FAILED(result)){fail(result);retire(packet);fifo->retire();break;}
                // Do not charge the prepared-FIFO wait to native work. Starting
                // native entry earlier changes neither producer nor End cadence.
                std::int64_t deadline{};
                if(directed) {
                    runtime_budget.observe(wait_leave-wait_enter+begun_at-begin_enter+release_leave-release_enter,physical);
                    if(diagnostic)options.trace(15,packet->sequence,release_enter,release_leave-release_enter);
                    // Only fully prepared packets reach this thread. Arrival time,
                    // not a rounded refresh slot, supplies the exact late hold.
                    // Fallback keeps its existing bounded pair refill. Buffer
                    // capacity changes admission, never the temporal policy.
                    const auto refill=slots.valid()?0:schedule.input_ready(packet->source_ready_at,2);
                    if(diagnostic && refill)options.trace(11,packet->sequence,packet->source_ready_at,refill);
                    if(!slots.valid())schedule.ready(packet->prepared_at);
                    // Reserve headroom for native Wait/Begin, not for a later GPU
                    // completion. Recharging it after every late copy adds a fresh
                    // 1.1ms pause to otherwise ready native cycles (V590 regression).
                    const auto margin=slots.valid()?0:schedule.runtime_ready(begun_at,PrivateRuntimeBudget::presentation_margin);
                    if(diagnostic && margin)options.trace(17,packet->sequence,begun_at,margin);
                    if(slots.valid()) {
                        slots.adopt_handoff(last_handoff);
                        deadline=slots.claim(packet->accepted_at,packet->prepared_at,begun_at,
                            clock_ns(),PrivateRuntimeBudget::presentation_margin);
                        if(diagnostic) {
                            options.trace(19,packet->sequence,packet->accepted_at+slots.delay(),slots.delay());
                            options.trace(20,packet->sequence,deadline,static_cast<std::int64_t>(slots.slot()));
                            options.trace(21,packet->sequence,packet->work_ns,0);
                        }
                    } else deadline=schedule.deadline();
                    // After sparse inputs SteamVR can complete Wait with an already
                    // expired native cycle. Advancing only End.displayTime does not
                    // advance that cycle: the runtime can supersede its submission.
                    // Keep the SAME owned FIFO packet and renew only the unsubmitted
                    // native clock. Begin without a prior End is the OpenXR-defined
                    // XR_FRAME_DISCARDED path; no source frame or layer is discarded.
                    // Do this before posting lookahead, so the worker remains the
                    // sole Wait caller and Begin/End stay serialized here.
                    unsigned renewed{};
                    bool renewed_window{};
                    while(!stopping && prediction.shouldRender) {
                        const auto at=prediction_qpc(prediction);
                        if(!at)break;
                        const auto target=std::max(clock_ns(),deadline);
                        bool window_renewal{};
                        if(*at>target+PrivateOutputSchedule::preparation_lead) {
                            // Cover the physical display edge targeted by this
                            // early handoff, not a whole extra refresh after it.
                            // Renewing a valid 9ms horizon at90Hz needlessly
                            // discards its Begin and can consume an entire slot.
                            // Some runtimes cannot extend that horizon on demand:
                            // accept a still-current replacement instead of spinning.
                            if(!slots.valid() ||
                                PrivatePhysicalSchedule::prediction_covers_display(*at,target) || renewed_window)break;
                            renewed_window=window_renewal=true;
                        }
                        if(diagnostic)options.trace(window_renewal?25:23,packet->sequence,*at,
                            window_renewal?target+PrivatePhysicalSchedule::handoff_lead+
                                PrivateOutputSchedule::preparation_lead:deadline);
                        if(renewed && renewed%8==0 && !sleep_until(clock_ns()+physical,1'250'000))break;
                        if(!post_wait(0,false)){result=XR_ERROR_CALL_ORDER_INVALID;break;}
                        auto fresh=take_wait();
                        if(!fresh){result=XR_ERROR_SESSION_NOT_RUNNING;break;}
                        result=fresh->result;if(XR_FAILED(result))break;
                        prediction=fresh->prediction;waited=true;
                        wait_enter=fresh->entered;wait_leave=fresh->returned;
                        const auto entered=clock_ns();
                        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
                        result=options.real.begin(options.real.session,&begin);
                        if(XR_FAILED(result))break;
                        waited=false;begun=true;begun_at=clock_ns();++renewed;
                        runtime_budget.observe(wait_leave-wait_enter+begun_at-entered,physical);
                        if(slots.valid()) {
                            if(options.physical_clock)if(auto c=options.physical_clock())slots.clock(*c);
                            deadline=slots.claim(packet->accepted_at,packet->prepared_at,begun_at,
                                clock_ns(),PrivateRuntimeBudget::presentation_margin);
                        } else {
                            schedule.runtime_ready(begun_at,PrivateRuntimeBudget::presentation_margin);
                            deadline=schedule.deadline();
                        }
                    }
                    if(stopping || XR_FAILED(result)) {
                        if(!stopping)fail(result);
                        retire(packet);fifo->retire();break;
                    }
                    if(diagnostic) {
                        options.trace(7,packet->sequence,wait_enter,0);options.trace(6,packet->sequence,wait_leave,0);
                        options.trace(16,packet->sequence,begun_at,runtime_budget.lead(physical));
                        if(slots.valid())options.trace(20,packet->sequence,deadline,static_cast<std::int64_t>(slots.slot()));
                        options.trace(24,packet->sequence,prediction_qpc(prediction).value_or(0),renewed);
                    }
                } else {
                    // Framework bypass still needs a live native XR cycle.
                    // GPU preparation may outlive the Wait already in flight.
                    // Renew only that expired Begin; retain the same FIFO image
                    // and never introduce the director's pacing/delay here.
                    unsigned renewed{};
                    while(!stopping && prediction.shouldRender) {
                        const auto at=prediction_qpc(prediction);
                        if(!at || *at>clock_ns()+PrivateOutputSchedule::preparation_lead)break;
                        if(diagnostic)options.trace(23,packet->sequence,*at,clock_ns());
                        if(renewed && renewed%8==0 && !sleep_until(clock_ns()+physical,1'250'000))break;
                        if(!post_wait(0,false)){result=XR_ERROR_CALL_ORDER_INVALID;break;}
                        auto fresh=take_wait();
                        if(!fresh){result=XR_ERROR_SESSION_NOT_RUNNING;break;}
                        result=fresh->result;if(XR_FAILED(result))break;
                        prediction=fresh->prediction;waited=true;
                        wait_enter=fresh->entered;wait_leave=fresh->returned;
                        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
                        result=options.real.begin(options.real.session,&begin);
                        if(XR_FAILED(result))break;
                        waited=false;begun=true;begun_at=clock_ns();++renewed;
                    }
                    if(stopping || XR_FAILED(result)) {
                        if(!stopping)fail(result);
                        retire(packet);fifo->retire();break;
                    }
                    if(diagnostic) {
                        options.trace(7,packet->sequence,wait_enter,0);
                        options.trace(6,packet->sequence,wait_leave,0);
                        options.trace(28,packet->sequence,begun_at,0); // no director pacing
                        options.trace(24,packet->sequence,prediction_qpc(prediction).value_or(0),renewed);
                    }
                }
                // Wait for the next cycle may overlap this frame's handoff.
                // Begin/End remain exclusively serialized on this consumer;
                // the worker needs accepted owned input before entering Wait,
                // and may overlap that input's independent GPU preparation.
                if(!post_wait(directed?deadline+physical-runtime_budget.lead(physical):0)) {
                    if(!stopping)fail(XR_ERROR_CALL_ORDER_INVALID);
                    retire(packet);fifo->retire();break;
                }
                // Only the residual time remains. A late completed copy moves
                // the phase by its exact overrun; it never causes a catch-up burst.
                // Prepare the final call 100us before its handoff edge. The
                // physical schedule places that edge 1ms before headset vsync;
                // consecutive slots still advance by the exact physical period.
                if(!stopping && (!directed || sleep_until(std::max(std::int64_t{},deadline-PrivateOutputSchedule::preparation_lead),std::min<std::int64_t>(1'250'000,physical/8)))) {
                    auto end=packet->info;
                    // Use this cycle's runtime phase. A prediction which aged
                    // through a long prior End advances whole display periods.
                    std::int64_t observed_display{};
                    if(directed)end.displayTime=display_time(prediction,packet->delivery?&observed_display:nullptr);
                    else {
                        end.displayTime=prediction.predictedDisplayTime;
                        if(packet->delivery)observed_display=prediction_qpc(prediction).value_or(0);
                    }
                    if(!prediction.shouldRender){end.layerCount=0;end.layers=nullptr;}
                    if(diagnostic)options.trace(22,packet->sequence,packet->info.layerCount,
                        std::int64_t(end.layerCount)|(std::int64_t(prediction.shouldRender)<<32));
                    // Optional logger work belongs in the early window, not
                    // after every deadline where it would accumulate drift.
                    if(options.trace)options.trace(3,packet->sequence,end.displayTime,prediction.predictedDisplayTime);
                    // Preserve V579's 2us quantization/function-call guard.
                    // A late preparation still delivers once, then holds its
                    // exact overrun; there is no catch-up or extra frame wait.
                    // Include the edge guard in this final spin window. A
                    // second tiny timer wait for that2us tail would reintroduce
                    // scheduler jitter instead of preserving the early wakeup.
                    if(directed && !sleep_until(deadline+2'000,PrivateOutputSchedule::preparation_lead+2'000)) {
                        retire(packet);fifo->retire();break;
                    }
                    const auto at_end=clock_ns();
                    result=options.real.end(options.real.session,&end);begun=false;
                    // Optional receipt only after native End: no observer lock,
                    // callback or clock query is added to the handoff edge.
                    if(packet->delivery)packet->delivery->completed(result==XR_SUCCESS && end.layerCount!=0,
                        at_end,clock_ns(),observed_display,packet->prepared_at);
                    if(options.accepted_output && result==XR_SUCCESS && end.layerCount)options.accepted_output(clock_ns());
                    last_handoff=at_end;
                    // This consumer exclusively owns the schedule. Commit its
                    // saved edge after the call too, leaving no bookkeeping
                    // between the timestamp and native entry. The completed
                    // wait already guarantees eligibility on monotonic QPC.
                    if(directed && !(slots.valid()?slots.delivered(at_end):schedule.delivered(at_end)))result=XR_ERROR_RUNTIME_FAILURE;
                    if(options.trace)options.trace(8,packet->sequence,at_end,0);
                    if(XR_SUCCEEDED(result))trace(2,packet->sequence,directed?(slots.valid()?slots.delay():schedule.delay()):0);
                }
                // Publish a failed End before freeing a producer credit; a
                // woken producer must not accept another frame in that gap.
                if(XR_FAILED(result))fail(result);
                retire(packet);fifo->retire();
                if(XR_FAILED(result))break;
            }
        } catch(...) {fail(XR_ERROR_RUNTIME_FAILURE);}
        // Provisional sessions can stop after only seeding the virtual clock.
        // Close the real cycle before xrEndSession or a later start.
        if(waited) {XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};begun=XR_SUCCEEDED(options.real.begin(options.real.session,&begin));}
        if(begun) {
            XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            empty.displayTime=display_time(prediction);
            options.real.end(options.real.session,&empty);
        }
        // The next Wait can already be in the runtime while the current End
        // is closed. It is not cancellable by OpenXR; join it, then balance a
        // completed prediction exactly once before session end/restart.
        stopping=true;changed.notify_all();
        if(waiter.joinable())waiter.join();
        std::optional<WaitResult> remaining;
        {std::scoped_lock lock(mutex);remaining=std::move(wait_result);wait_result.reset();wait_job.reset();}
        if(remaining && XR_SUCCEEDED(remaining->result)) {
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            if(XR_SUCCEEDED(options.real.begin(options.real.session,&begin))) {
                XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};empty.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                empty.displayTime=display_time(remaining->prediction);options.real.end(options.real.session,&empty);
            }
        }
    }
};

PrivateOpenXrRuntime::PrivateOpenXrRuntime(Options options):impl_(std::make_unique<Impl>(std::move(options))){}
PrivateOpenXrRuntime::~PrivateOpenXrRuntime(){stop();}
HRESULT PrivateOpenXrRuntime::initialize() {
    auto& s=*impl_;auto& real=s.options.real;
    if(!real.wait || !real.begin || !real.end || !real.create || !real.destroy || !real.enumerate || !real.acquire || !real.wait_image || !real.release || !s.fence_event || !s.options.capacity)return E_INVALIDARG;
    HRESULT result=S_OK;
    if(s.options.device12 && s.options.queue12) {
        s.device12=s.options.device12;s.queue12=s.options.queue12;
        s.producer_queue12=s.options.producer_queue12?s.options.producer_queue12:s.options.queue12;
        result=s.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.ready12));
        if(SUCCEEDED(result))result=s.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.copy12));
        if(SUCCEEDED(result))result=s.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&s.allocator));
        if(SUCCEEDED(result))result=s.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,s.allocator.Get(),nullptr,IID_PPV_ARGS(&s.list));
        if(SUCCEEDED(result))result=s.list->Close();
    } else if(s.options.device11 && s.options.context11) {
        result=s.options.device11->QueryInterface(IID_PPV_ARGS(&s.device11));
        if(SUCCEEDED(result))result=s.options.context11->QueryInterface(IID_PPV_ARGS(&s.context11));
        ComPtr<ID3D11Multithread> protection;
        if(SUCCEEDED(result))result=s.context11.As(&protection);
        if(SUCCEEDED(result))protection->SetMultithreadProtected(TRUE);
        if(SUCCEEDED(result))result=s.device11->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.ready11));
        if(SUCCEEDED(result))result=s.device11->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&s.copy11));
    } else result=E_INVALIDARG;
    return result;
}
XrResult PrivateOpenXrRuntime::start() {
    auto& s=*impl_;std::scoped_lock lock(s.mutex);
    if(s.running)return XR_ERROR_SESSION_RUNNING;
    s.fifo=std::make_unique<PrivateOutputFifo<std::shared_ptr<Impl::Packet>>>(s.options.capacity);
    s.prepared.clear();s.wait_job.reset();s.wait_result.reset();s.wait_active=false;s.accepted_packets=s.content_wait_jobs=0;
    s.failure=XR_SUCCESS;s.stopping=false;s.seeded=false;s.running=true;s.waits=s.begun=0;s.virtual_time=0;
    s.waiter=std::thread([&s]{s.wait_loop();});
    s.preparer=std::thread([&s]{s.prepare();});
    s.consumer=std::thread([&s]{s.run();});return XR_SUCCESS;
}
void PrivateOpenXrRuntime::stop() {
    auto& s=*impl_;
    std::scoped_lock serial_stop(s.stop_mutex);
    {std::scoped_lock lock(s.mutex);s.stopping=true;if(s.fifo)s.fifo->stop();}
    s.changed.notify_all();if(s.consumer.joinable())s.consumer.join();if(s.preparer.joinable())s.preparer.join();
    std::scoped_lock accept(s.accept_mutex);
    // An interrupted GPU wait never authorizes reuse/free. Drain submitted
    // reads before dropping the pending packet leases and command allocator.
    if(s.copy_serial) {if(s.d12())s.await(s.copy12.Get(),s.copy_serial,false);else s.await(s.copy11.Get(),s.copy_serial,false);}
    if(s.ready_serial) {if(s.d12())s.await(s.ready12.Get(),s.ready_serial,false);else s.await(s.ready11.Get(),s.ready_serial,false);}
    // A stopped ready packet may own a successfully waited real target which
    // was intentionally never released. Finish ownership after GPU reads drain,
    // before a session restart or swapchain destruction can reuse that image.
    std::vector<std::shared_ptr<Impl::Chain>> owned_chains;
    {std::scoped_lock lock(s.mutex);owned_chains.reserve(s.chains.size());
     for(auto& [h,c]:s.chains) {auto owner=c->publication?c->publication:c;
         if(std::find(owned_chains.begin(),owned_chains.end(),owner)==owned_chains.end())owned_chains.push_back(owner);}}
    for(auto& c:owned_chains)if(auto target=c->pending_target;target && target->acquired && !target->released) {
        if(!target->waited) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
            target->waited=XR_SUCCEEDED(s.options.real.wait_image(c->handle,&wait));
        }
        if(target->waited) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            target->released=XR_SUCCEEDED(s.options.real.release(c->handle,&release));
        }
    }
    {std::scoped_lock lock(s.mutex);s.fifo.reset();s.prepared.clear();s.pending=0;s.running=false;
     for(auto& [h,c]:s.chains) {c->consumers=0;for(auto& image:c->images)image.readers=0;}
     for(auto& c:owned_chains) {
         // Static runtime images can be acquired/released only once for their
         // entire lifetime, including a session stop/start with retained chains.
         if(c->latest_target && (c->info.createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) &&
             c->latest_target->complete && c->latest_target->released)c->latest_target->users=0;
         else c->latest_target.reset();
         c->pending_target.reset();c->retained_targets.clear();}}
    s.changed.notify_all();
}
XrResult PrivateOpenXrRuntime::drain() {
    auto& s=*impl_;std::unique_lock lock(s.mutex);
    s.changed.wait(lock,[&]{return s.stopping || !s.pending;});return s.failure;
}
XrResult PrivateOpenXrRuntime::set_director_enabled(bool enabled) {
    auto& s=*impl_;std::scoped_lock accept(s.accept_mutex);
    std::unique_lock lock(s.mutex);
    if(s.director_enabled.load()==enabled)return s.failure;
    s.changed.wait(lock,[&]{return s.stopping || !s.pending;});
    if(XR_FAILED(s.failure))return s.failure;
    s.director_enabled=enabled;++s.policy_epoch;
    // Lookahead has not entered native Wait without the next accepted input.
    // Do not carry a previous director deadline into the bypass's first frame.
    if(s.wait_job)s.wait_job->target=0;
    lock.unlock();s.changed.notify_all();return XR_SUCCESS;
}
bool PrivateOpenXrRuntime::director_enabled() const noexcept {
    return impl_->director_enabled.load(std::memory_order_relaxed);
}
XrResult PrivateOpenXrRuntime::set_buffer_capacity(std::size_t capacity) {
    if(capacity!=2 && capacity!=3)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock accept(s.accept_mutex);
    std::unique_lock lock(s.mutex);
    if(s.options.capacity==capacity)return s.failure;
    if(!s.running) {
        if(XR_FAILED(s.failure))return s.failure;
        s.options.capacity=capacity;++s.policy_epoch;return XR_SUCCESS;
    }
    // Let the ordinary consumer finish every accepted image. Keep the FIFO
    // object and all GPU resources; only reset future admission and learning.
    s.changed.wait(lock,[&]{return s.stopping || !s.pending;});
    if(XR_FAILED(s.failure))return s.failure;
    if(s.stopping)return XR_ERROR_SESSION_NOT_RUNNING;
    if(!s.fifo || !s.fifo->set_capacity(capacity))return XR_ERROR_RUNTIME_FAILURE;
    s.options.capacity=capacity;++s.policy_epoch;
    if(s.wait_job)s.wait_job->target=0;
    lock.unlock();s.changed.notify_all();return XR_SUCCESS;
}
std::size_t PrivateOpenXrRuntime::buffer_capacity() const noexcept {
    std::scoped_lock lock(impl_->mutex);return impl_->options.capacity;
}
XrResult PrivateOpenXrRuntime::wait_frame(const XrFrameWaitInfo* info,XrFrameState* output) {
    if(!valid(info,XR_TYPE_FRAME_WAIT_INFO) || !output || output->type!=XR_TYPE_FRAME_STATE || output->next)return XR_ERROR_VALIDATION_FAILURE;
    auto result=predict_frame(output);if(XR_FAILED(result))return result;
    std::scoped_lock lock(impl_->mutex);++impl_->waits;return result;
}
XrResult PrivateOpenXrRuntime::predict_frame(XrFrameState* output) {
    if(!output || output->type!=XR_TYPE_FRAME_STATE || output->next)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::unique_lock lock(s.mutex);
    if(!s.running)return XR_ERROR_SESSION_NOT_RUNNING;
    s.changed.wait(lock,[&]{return s.stopping || s.seeded;});
    if(XR_FAILED(s.failure))return s.failure;if(s.stopping)return XR_ERROR_SESSION_NOT_RUNNING;
    const auto interval=s.period(s.anchor.predictedDisplayPeriod);
    auto target=s.anchor.predictedDisplayTime+std::max<std::int64_t>(0,clock_ns()-s.anchor_at)+static_cast<XrDuration>(s.pending)*interval;
    target=std::max(target,s.virtual_time+1);s.virtual_time=target;
    output->predictedDisplayTime=target;output->predictedDisplayPeriod=interval;output->shouldRender=s.anchor.shouldRender;return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::begin_frame(const XrFrameBeginInfo* info) {
    if(!valid(info,XR_TYPE_FRAME_BEGIN_INFO))return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock lock(s.mutex);
    if(XR_FAILED(s.failure))return s.failure;if(!s.running || s.stopping)return XR_ERROR_SESSION_NOT_RUNNING;
    if(!s.waits)return XR_ERROR_CALL_ORDER_INVALID;--s.waits;
    const auto result=s.begun?XR_FRAME_DISCARDED:XR_SUCCESS;s.begun=1;return result;
}
XrResult PrivateOpenXrRuntime::end_frame(const XrFrameEndInfo* info,const std::shared_ptr<InterfaceDeliveryReceipt>& delivery) {
    if(!info || info->type!=XR_TYPE_FRAME_END_INFO)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock accept(s.accept_mutex);
    {std::scoped_lock lock(s.mutex);if(XR_FAILED(s.failure))return s.failure;if(!s.running || s.stopping)return XR_ERROR_SESSION_NOT_RUNNING;if(!s.begun)return XR_ERROR_CALL_ORDER_INVALID;}
    if(!s.fifo->reserve()){std::scoped_lock lock(s.mutex);return XR_FAILED(s.failure)?s.failure:XR_ERROR_SESSION_NOT_RUNNING;}
    std::shared_ptr<Impl::Packet> packet;
    struct Reservation {
        Impl& s;std::shared_ptr<Impl::Packet>& packet;bool accepted{},counted{};
        ~Reservation(){if(accepted)return;{std::scoped_lock lock(s.mutex);
            if(packet)for(auto& lease:packet->leases) {
                if(!lease.source_released)--lease.chain->images[lease.image].readers;
                --lease.chain->consumers;
            }
            if(counted)--s.pending;}s.fifo->cancel();s.changed.notify_all();}
    } reservation{s,packet};
    packet=std::make_shared<Impl::Packet>();packet->delivery=delivery;
    packet->director=s.director_enabled.load();packet->policy_epoch=s.policy_epoch;XrResult result;
    {std::scoped_lock lock(s.mutex);result=s.own(*packet,*info);
     if(XR_FAILED(result))return result;
     packet->sequence=++s.sequence;if(!s.options.d11_release_readiness || s.d12())packet->ready=s.ready_serial+1;}
    if(!s.options.d11_release_readiness || s.d12()) {
        const auto ready=s.d12()?s.producer_queue12->Signal(s.ready12.Get(),packet->ready):s.context11->Signal(s.ready11.Get(),packet->ready);
        if(!s.d12())s.context11->Flush();
        if(FAILED(ready)) {s.fail(XR_ERROR_RUNTIME_FAILURE);return XR_ERROR_RUNTIME_FAILURE;}
        s.ready_serial=packet->ready;
    }
    packet->accepted_at=clock_ns();
    {std::scoped_lock lock(s.mutex);++s.pending;reservation.counted=true;}
    if(!s.fifo->accept(packet))return XR_ERROR_SESSION_NOT_RUNNING;
    reservation.accepted=true;{std::scoped_lock lock(s.mutex);s.begun=0;++s.accepted_packets;}s.changed.notify_all();
    s.trace(1,packet->sequence);return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::create_swapchain(const XrSwapchainCreateInfo* info,XrSwapchain* output,D3D12_RESOURCE_STATES image_state,std::uint32_t image_count,XrSwapchain publication_source,ID3D12Device* d11_output_owner) {
    if(!info || !output || info->type!=XR_TYPE_SWAPCHAIN_CREATE_INFO || image_count<1 || image_count>3)return XR_ERROR_VALIDATION_FAILURE;
    const bool pooled=publication_source!=XR_NULL_HANDLE;
    // A packed stereo image or texture array is still one source domain: its
    // full resource and owned view rectangles/slices travel together. Pooling
    // never joins different sources, and compatibility includes the array size.
    // Unknown extensions, static/protected/depth/cube chains stay independent.
    if(pooled && (info->next || info->createFlags || !info->arraySize || info->faceCount!=1 ||
        !(info->usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) ||
        (info->usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)))return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::unique_lock lock(s.mutex);
    auto chain=std::make_shared<Impl::Chain>();chain->info=*info;chain->info.next=nullptr;
    chain->image_state=(info->usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)?D3D12_RESOURCE_STATE_DEPTH_WRITE:image_state;
    if(pooled) {
        std::erase_if(s.publication_pools,[](auto& weak){return weak.expired();});
        for(auto& weak:s.publication_pools)if(auto candidate=weak.lock();candidate && candidate->publication_owners &&
            Impl::compatible(*candidate,*info,chain->image_state,publication_source)){chain->publication=candidate;break;}
        if(!chain->publication) {
            chain->publication=std::make_shared<Impl::Chain>();chain->publication->info=chain->info;
            chain->publication->image_state=chain->image_state;
            chain->publication->publication_source=publication_source;
        }
    }
    auto& endpoint=Impl::destination(*chain);
    const bool created=endpoint.handle==XR_NULL_HANDLE;
    auto abandon=[&](XrResult r){if(created && endpoint.handle)s.options.real.destroy(endpoint.handle);return r;};
    if(created) {
        auto result=s.options.real.create(s.options.real.session,info,&endpoint.handle);if(XR_FAILED(result))return result;
        unsigned count{};result=s.options.real.enumerate(endpoint.handle,0,&count,nullptr);
        if(XR_FAILED(result) || !count)return abandon(XR_FAILED(result)?result:XR_ERROR_RUNTIME_FAILURE);
        if(s.d12()) {
            std::vector<XrSwapchainImageD3D12KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            result=s.options.real.enumerate(endpoint.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            if(XR_FAILED(result))return abandon(result);
            for(auto& image:images){if(!image.texture)return abandon(XR_ERROR_RUNTIME_FAILURE);endpoint.real12.emplace_back(image.texture);}
        } else {
            std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            result=s.options.real.enumerate(endpoint.handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
            if(XR_FAILED(result))return abandon(result);
            for(auto& image:images){if(!image.texture)return abandon(XR_ERROR_RUNTIME_FAILURE);endpoint.real11.emplace_back(image.texture);}
        }
    }
    const unsigned private_count=(info->createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT)?1:image_count;
    chain->images.resize(private_count);HRESULT made=S_OK;
    if(s.d12()) {
        auto desc=endpoint.real12.front()->GetDesc();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        for(auto& image:chain->images) {
            made=s.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,chain->image_state,nullptr,IID_PPV_ARGS(&image.d12));if(FAILED(made))break;
            image.d12->SetName(L"OFXR private distributor image");
        }
    } else {
        D3D11_TEXTURE2D_DESC desc{};endpoint.real11.front()->GetDesc(&desc);
        const bool share_owned_output=d11_output_owner && !info->next && !info->createFlags &&
            info->faceCount==1 && info->arraySize && info->mipCount==1 && info->sampleCount==1 &&
            (info->usageFlags&XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) &&
            !(info->usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        for(auto& image:chain->images) {
            made=share_owned_output?owned_d11_output::create(s.device11.Get(),d11_output_owner,desc,&image.d11):E_NOINTERFACE;
            if(FAILED(made))made=s.device11->CreateTexture2D(&desc,nullptr,&image.d11);
            if(FAILED(made))break;
        }
    }
    if(FAILED(made))return abandon(XR_ERROR_RUNTIME_FAILURE);
    if(pooled) {
        chain->handle=reinterpret_cast<XrSwapchain>(chain.get());
        if(s.chains.contains(chain->handle))return abandon(XR_ERROR_RUNTIME_FAILURE);
        ++endpoint.publication_owners;
        if(created)s.publication_pools.push_back(chain->publication);
    }
    s.chains.emplace(chain->handle,chain);*output=chain->handle;
    // Allocation-only F9 witness: source, virtual buffer, actual XR sink.
    // No per-frame query, additional sampler or delivery callback is installed.
    if(pooled && s.tracing())s.options.trace(27,reinterpret_cast<std::uint64_t>(publication_source),
        reinterpret_cast<std::int64_t>(chain->handle),reinterpret_cast<std::int64_t>(endpoint.handle));
    return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::destroy_swapchain(XrSwapchain handle) {
    auto& s=*impl_;std::unique_lock lock(s.mutex);auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;
    auto chain=it->second;chain->destroying=true;
    if(s.stopping){lock.unlock();stop();lock.lock();}
    s.changed.wait(lock,[&]{return !chain->consumers && std::none_of(chain->images.begin(),chain->images.end(),[](auto& i){return i.readers!=0;});});
    s.chains.erase(handle);
    auto publication=chain->publication;
    if(publication && --publication->publication_owners)return XR_SUCCESS;
    const auto native=publication?publication->handle:handle;
    lock.unlock();return s.options.real.destroy(native);
}
XrResult PrivateOpenXrRuntime::enumerate_images(XrSwapchain handle,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* output) {
    auto& s=*impl_;std::scoped_lock lock(s.mutex);auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;
    if(!count)return XR_ERROR_VALIDATION_FAILURE;*count=static_cast<unsigned>(it->second->images.size());if(!capacity)return XR_SUCCESS;
    if(capacity<*count)return XR_ERROR_SIZE_INSUFFICIENT;if(!output)return XR_ERROR_VALIDATION_FAILURE;
    for(unsigned i=0;i<*count;++i)if(s.d12()) {
        auto* image=reinterpret_cast<XrSwapchainImageD3D12KHR*>(output)+i;if(image->type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR)return XR_ERROR_VALIDATION_FAILURE;image->texture=it->second->images[i].d12.Get();
    } else {
        auto* image=reinterpret_cast<XrSwapchainImageD3D11KHR*>(output)+i;if(image->type!=XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR)return XR_ERROR_VALIDATION_FAILURE;image->texture=it->second->images[i].d11.Get();
    }
    return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::acquire_image(XrSwapchain handle,const XrSwapchainImageAcquireInfo* info,std::uint32_t* output) {
    if(!valid(info,XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || !output)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::unique_lock lock(s.mutex);auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;auto chain=it->second;
    if((chain->info.createFlags&XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && chain->released)return XR_ERROR_CALL_ORDER_INVALID;
    // End references the most recently released image, even when the app has
    // already acquired its NEXT render target. Never choose that release as
    // the next target under pressure while older images are still being read.
    // Internal single-image outputs keep their explicit release/End/copy cycle.
    const auto held_release=[&](unsigned index){return chain->images.size()>1 && chain->released==index;};
    auto available=[&]() -> std::optional<unsigned> {
        for(unsigned n=0;n<chain->images.size();++n){auto index=(chain->cursor+n)%chain->images.size();auto& image=chain->images[index];if(!held_release(index) && !image.acquired && !image.readers)return static_cast<unsigned>(index);}return {};
    };
    bool all_acquired=true;
    for(unsigned index=0;index<chain->images.size();++index)if(!held_release(index) && !chain->images[index].acquired){all_acquired=false;break;}
    if(all_acquired)return XR_ERROR_CALL_ORDER_INVALID;
    s.changed.wait(lock,[&]{return (s.running && s.stopping) || chain->destroying || available().has_value();});
    if(s.running && s.stopping)return XR_FAILED(s.failure)?s.failure:XR_ERROR_SESSION_NOT_RUNNING;if(chain->destroying)return XR_ERROR_HANDLE_INVALID;
    auto index=*available();chain->images[index].acquired=true;chain->images[index].waited=false;chain->acquired.push_back(index);chain->cursor=(index+1)%chain->images.size();*output=index;return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::wait_image(XrSwapchain handle,const XrSwapchainImageWaitInfo* info) {
    if(!info || info->type!=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO || info->next)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock lock(s.mutex);auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;
    for(auto index:it->second->acquired)if(!it->second->images[index].waited){it->second->images[index].waited=true;return XR_SUCCESS;}return XR_ERROR_CALL_ORDER_INVALID;
}
XrResult PrivateOpenXrRuntime::release_image(XrSwapchain handle,const XrSwapchainImageReleaseInfo* info) {
    if(!valid(info,XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO))return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock lock(s.mutex);auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;auto& chain=*it->second;
    if(chain.acquired.empty() || !chain.images[chain.acquired.front()].waited)return XR_ERROR_CALL_ORDER_INVALID;
    auto index=chain.acquired.front();
    if(s.options.d11_release_readiness && !s.d12()) {
        const auto value=s.ready_serial+1;
        const auto result=s.context11->Signal(s.ready11.Get(),value);s.context11->Flush();
        if(FAILED(result))return XR_ERROR_RUNTIME_FAILURE;
        s.ready_serial=value;chain.images[index].ready11_value=value;
    }
    chain.acquired.pop_front();chain.images[index].acquired=false;chain.images[index].waited=false;++chain.images[index].generation;chain.released=index;s.changed.notify_all();return XR_SUCCESS;
}
XrResult PrivateOpenXrRuntime::release_completed_image(XrSwapchain handle,
    const XrSwapchainImageReleaseInfo* info,std::uint32_t index,ID3D11Texture2D* published_image,
    ID3D12Fence* fence,std::uint64_t value) {
    if(!valid(info,XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) || !published_image || !fence || !value)return XR_ERROR_VALIDATION_FAILURE;
    auto& s=*impl_;std::scoped_lock lock(s.mutex);
    if(!s.options.d11_release_readiness || s.d12())return XR_ERROR_FUNCTION_UNSUPPORTED;
    auto it=s.chains.find(handle);if(it==s.chains.end())return XR_ERROR_HANDLE_INVALID;
    auto& chain=*it->second;
    if(chain.acquired.empty() || chain.acquired.front()!=index ||
        !chain.images[index].waited || chain.images[index].d11.Get()!=published_image)return XR_ERROR_CALL_ORDER_INVALID;
    const auto completed=fence->GetCompletedValue();
    if(completed==UINT64_MAX)return XR_ERROR_RUNTIME_FAILURE;
    if(completed<value)return XR_ERROR_CALL_ORDER_INVALID;
    // Zero is an observed-complete proof, not a pending value to be signalled.
    chain.images[index].ready11_value=0;chain.acquired.pop_front();
    chain.images[index].acquired=false;chain.images[index].waited=false;
    ++chain.images[index].generation;chain.released=index;s.changed.notify_all();return XR_SUCCESS;
}
std::optional<std::vector<PrivateOpenXrRuntime::SwapchainMapping>> PrivateOpenXrRuntime::native_swapchain_mapping() const {
    std::unique_lock lock(impl_->mutex,std::try_to_lock);
    if(!lock.owns_lock())return std::nullopt;
    std::vector<SwapchainMapping> result;result.reserve(impl_->chains.size());
    for(const auto& [handle,chain]:impl_->chains)result.push_back({handle,Impl::destination(*chain).handle});
    return result;
}
bool PrivateOpenXrRuntime::owns(XrSwapchain handle) const {std::scoped_lock lock(impl_->mutex);return impl_->chains.contains(handle);}
} // namespace xrfg
