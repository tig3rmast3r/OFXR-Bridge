// Real D3D12/NVIDIA and the actual release/EndFrame/worker/director code.
// The runtime alone is mocked. Sources are poisoned after intercepted release.
#include "../src/layer/openxr_layer.cpp"
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <fstream>
#include "ram_timing_fixture.hpp"
#include "autonomous_layer_fixture.hpp"

namespace {
#ifdef OFXR_TEST_DJULES_ROUTE
constexpr bool kTestDjules=true;
#else
constexpr bool kTestDjules=false;
#endif
using Microsoft::WRL::ComPtr;
std::shared_ptr<SessionState> session_state;
std::mutex test_gpu, resources_mutex;
std::unordered_map<XrSwapchain, ComPtr<ID3D12Resource>> images;
std::unordered_set<XrSwapchain> synthetic_handles;
std::vector<std::pair<unsigned, bool>> submissions;
std::atomic<unsigned> waits{}, begins{}, ends{}, created{}, destroyed{};
unsigned empty_submissions{};
XrTime runtime_time = 1000000000;
bool correct = true;
bool alternating_eyes{};
bool promotion_test{};
bool fsr_test{}, fsr_missing{}, fsr_packed{};
bool fsr_foveated{};
bool fsr_queue{}, fsr_queue_slow{};
bool fsr_queue_switch{};
bool fsr_stereo_guard{};
std::mutex guard_journal_mutex;
bool guard_runtime_waited{}, guard_runtime_begun{};
bool legacy_queue{};
bool transport_test{}, transport_inline{};
bool msfs_time_test{};
XrTime msfs_previous_time{};
std::unordered_map<unsigned,unsigned> triple_synthetic_ordinals;
bool legacy_early_current{};
bool midpoint_cadence{};
bool publication_test{};
std::atomic_bool publication_frame_open{};
std::atomic<DWORD> publication_consumer_thread{};
std::atomic<unsigned> publication_releases{};
bool deep_readiness{};
bool triple_core{};
bool triple_inline{};
bool triple_switch{};
bool deep_switch{};
bool lower_vram_test{}, direct_output_test{};
bool capture_busy_test{};
unsigned private_budget_limit{}, private_budget_refusals{};
bool reference_stall_test{};
std::chrono::steady_clock::time_point reference_synthetic_returned{};
bool guide_bypass_switch{};
XrSwapchain last_synthetic_handle{};
unsigned last_synthetic_frame{};
bool fsr_queue_gpu_pending{};
ComPtr<ID3D12Fence> fsr_gpu_gate;
std::jthread fsr_gpu_watchdog;
std::atomic<bool> fsr_wait_before_gpu_complete{};
bool reference_readiness_test{};
bool shared_present_queue_test{};
bool reference_capture_overlap_test{};
std::atomic<bool> reference_wait_before_gpu_complete{};
std::vector<std::chrono::steady_clock::time_point> output_times;
std::vector<XrTime> output_display_times;
std::vector<std::string> fsr_receipt_trace;
ComPtr<ID3D12Fence> promotion_gate;
std::jthread promotion_unblock;
std::atomic<bool> promotion_gate_released{};
unsigned promotion_gate_count{};
void require(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
#include "msfs_capture_order_fixture.hpp"
void hr(HRESULT r, const char* s) { require(SUCCEEDED(r), s); }

ComPtr<ID3D12Resource> resource(unsigned slices, bool buffer = false,
    D3D12_HEAP_TYPE heap_type = D3D12_HEAP_TYPE_DEFAULT, UINT64 bytes = 0,
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON,
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = heap_type;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = buffer ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = buffer ? bytes : 256; d.Height = buffer ? 1 : 128;
    d.DepthOrArraySize = static_cast<UINT16>(buffer ? 1 : slices); d.MipLevels = 1;
    d.Format = buffer ? DXGI_FORMAT_UNKNOWN : format;
    d.SampleDesc.Count = 1;
    d.Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = buffer ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> r;
    hr(session_state->d3d12_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,initial,nullptr,
        IID_PPV_ARGS(&r)), "resource");
    return r;
}

template<class Record> void commands(Record record, ID3D12CommandQueue* execution_queue=nullptr) {
    if(!execution_queue) execution_queue=session_state->d3d12_queue.Get();
    std::scoped_lock lock(test_gpu);
    ComPtr<ID3D12CommandAllocator> allocator; ComPtr<ID3D12GraphicsCommandList> list;
    hr(session_state->d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator");
    hr(session_state->d3d12_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
    record(list.Get()); hr(list->Close(),"close");
    ID3D12CommandList* lists[]={list.Get()}; execution_queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;
    hr(session_state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    hr(execution_queue->Signal(fence.Get(),1),"signal");
    hr(wait_composition_fence(fence.Get(),1),"test GPU completion");
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; list->ResourceBarrier(1,&x);
}
void fill(ID3D12Resource* r, unsigned red, unsigned green, bool stereo = false) {
    auto desc=r->GetDesc(); std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layout(desc.DepthOrArraySize);
    UINT64 bytes=0; session_state->d3d12_device->GetCopyableFootprints(&desc,0,desc.DepthOrArraySize,0,layout.data(),nullptr,nullptr,&bytes);
    auto upload=resource(1,true,D3D12_HEAP_TYPE_UPLOAD,bytes,D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped=nullptr; hr(upload->Map(0,nullptr,&mapped),"upload map");
    for(unsigned slice=0;slice<desc.DepthOrArraySize;++slice) for(unsigned y=0;y<128;++y) {
        auto* row=reinterpret_cast<unsigned*>(static_cast<unsigned char*>(mapped)+layout[slice].Offset+y*layout[slice].Footprint.RowPitch);
        for(unsigned x=0;x<256;++x) row[x]=0xff200000u|((stereo && (fsr_packed ? x>=128 : slice!=0) ? 160 : green)<<8)|red;
    }
    upload->Unmap(0,nullptr);
    commands([&](auto* list){
        barrier(list,r,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
        for(unsigned slice=0;slice<desc.DepthOrArraySize;++slice) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=r; dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex=slice;
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=layout[slice];
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        }
        barrier(list,r,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
    });
}
unsigned pixel(ID3D12Resource* r, unsigned slice,
    D3D12_RESOURCE_STATES submitted_state = D3D12_RESOURCE_STATE_COMMON,
    unsigned sample_x = 128) {
    auto desc=r->GetDesc(); D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{}; UINT64 bytes=0;
    session_state->d3d12_device->GetCopyableFootprints(&desc,slice,1,0,&layout,nullptr,nullptr,&bytes);
    auto readback=resource(1,true,D3D12_HEAP_TYPE_READBACK,bytes,D3D12_RESOURCE_STATE_COPY_DEST);
    commands([&](auto* list){
        barrier(list,r,submitted_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=r; src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=slice;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=layout;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(list,r,D3D12_RESOURCE_STATE_COPY_SOURCE,submitted_state);
    // Read as the downstream runtime would: Reference overlap also uses a
    // distinct binding queue. Reading its output on the application queue
    // ignores that queue's readiness dependency and can observe the initial0.
    },(deep_readiness||triple_core||reference_capture_overlap_test||shared_present_queue_test)?runtime_queue(*session_state):nullptr);
    void* mapped=nullptr; hr(readback->Map(0,nullptr,&mapped),"readback");
    const unsigned value=*reinterpret_cast<unsigned*>(static_cast<unsigned char*>(mapped)+layout.Offset+64*layout.Footprint.RowPitch+sample_x*4);
    readback->Unmap(0,nullptr); return value;
}
XrResult XRAPI_CALL fake_create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* h) {
    {
        std::scoped_lock lock(resources_mutex);
        // The transport matrix bounds projection outputs independently of
        // its additional poisoned-quad ownership oracle.
        const bool budgeted=!transport_test || info->arraySize==1;
        const auto allocation_count=transport_test ? std::count_if(images.begin(),images.end(),
            [](const auto& entry){return entry.second->GetDesc().DepthOrArraySize==1;}) : images.size();
        if(budgeted && private_budget_limit && allocation_count>=private_budget_limit) {
            ++private_budget_refusals;return XR_ERROR_LIMIT_REACHED;
        }
    }
    auto r=resource(info->arraySize);
    *h=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x1000+ ++created));
    std::scoped_lock lock(resources_mutex); images[*h]=r; return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_enumerate(XrSwapchain h,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* out) {
    *count=1;
    if(capacity){std::scoped_lock lock(resources_mutex);reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)->texture=images.at(h).Get();}
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy(XrSwapchain h) {
    std::scoped_lock lock(resources_mutex); destroyed+=static_cast<unsigned>(images.erase(h)); return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_wait(XrSession,const XrFrameWaitInfo*,XrFrameState* s) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(!guard_runtime_waited && !guard_runtime_begun,"guard double physical Wait");
        guard_runtime_waited=true;
    }
    if(reference_readiness_test && fsr_gpu_gate && fsr_gpu_gate->GetCompletedValue()==0)
        reference_wait_before_gpu_complete=true;
    if(fsr_queue_gpu_pending && fsr_gpu_gate && fsr_gpu_gate->GetCompletedValue()==0) {
        fsr_wait_before_gpu_complete=true;
        hr(fsr_gpu_gate->Signal(1), "release pending application GPU work at XR Wait");
    }
    if(fsr_test && !fsr_queue) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ++waits; runtime_time+=8333333; s->predictedDisplayTime=runtime_time;
    s->predictedDisplayPeriod=8333333; s->shouldRender=XR_TRUE; return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_begin(XrSession,const XrFrameBeginInfo*) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(guard_runtime_waited && !guard_runtime_begun,"guard unmatched physical Begin");
        guard_runtime_waited=false;guard_runtime_begun=true;
    }
    publication_consumer_thread=GetCurrentThreadId();publication_frame_open=true;
    ++begins; return XR_SUCCESS;
}
ComPtr<ID3D12Resource> submitted_image(XrSwapchain handle) {
    {
        std::scoped_lock lock(resources_mutex);
        const auto found=images.find(handle);
        if(found!=images.end()) return found->second;
    }
    std::scoped_lock lock(g_state_mutex);
    const auto found=g_swapchains.find(handle);
    require(found!=g_swapchains.end() && !found->second->enumerated_d3d12_images.empty(),
        "submitted image is not known to the fake runtime");
    return found->second->enumerated_d3d12_images[0];
}
bool is_private_image(XrSwapchain handle) {
    std::scoped_lock lock(resources_mutex);
    return images.contains(handle);
}
XrResult XRAPI_CALL fake_end_impl(XrSession,const XrFrameEndInfo* end) {
    if(fsr_stereo_guard) {
        std::scoped_lock lock(guard_journal_mutex);
        require(guard_runtime_begun && !guard_runtime_waited,"guard unmatched physical End");
        guard_runtime_begun=false;
    }
    // Cadence receipts measure entry into EndFrame, before its simulated
    // compositor stall. Empty call-order probes carry no output receipt.
    if (end->layerCount) {
        output_times.push_back(std::chrono::steady_clock::now());
        output_display_times.push_back(end->displayTime);
    }
    if(fsr_queue_slow && ends.load() == 5) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    ++ends;
    if (!end->layerCount) { ++empty_submissions; return XR_SUCCESS; }
    const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
    bool synthetic=synthetic_handles.contains(projection->views[0].subImage.swapchain);
    if(transport_test && triple_core && synthetic && !session_state->separate_synthesis)
        std::this_thread::sleep_for(std::chrono::milliseconds(8)); // allow the next producer to attempt reuse
    if(guide_bypass_switch && !synthetic) for(const auto& chain:find_swapchains(session_state)) {
        std::scoped_lock lock(chain->mutex);
        if(chain->frame_generation) for(const auto& image:chain->frame_generation->synthetic)
            synthetic |= image.handle==projection->views[0].subImage.swapchain;
    }
    const unsigned frame_id=static_cast<unsigned>(projection->views[0].pose.position.y);
    if(reference_stall_test && !synthetic && frame_id==4 && reference_synthetic_returned.time_since_epoch().count())
        if(std::chrono::steady_clock::now()-reference_synthetic_returned<std::chrono::milliseconds(6)) {
            std::cerr<<"Reference real endpoint overwrote a stalled synthetic before one display exposure\n";
            require(false,"Reference stalled synthetic exposure");
        }
    if (direct_output_test && (triple_core || triple_inline) && synthetic) {
        if(last_synthetic_frame==frame_id)
            require(last_synthetic_handle!=projection->views[0].subImage.swapchain,"3X synthetic outputs alias one direct handle");
        last_synthetic_handle=projection->views[0].subImage.swapchain; last_synthetic_frame=frame_id;
    }
    if (legacy_early_current && synthetic && session_state->queued_nvidia_presenter) {
        std::shared_ptr<GeneratedFrameEndInfo> next;
        {
            std::scoped_lock lock(session_state->presenter_mutex);
            require(!session_state->presenter_submissions.empty(), "B queued alongside S");
            next=session_state->presenter_submissions.front()->owned_frame;
        }
        require(next != nullptr, "private B ownership");
        require(next->pending_current_copies.empty(), "B copy is already submitted before S EndFrame");
        const auto* b=reinterpret_cast<const XrCompositionLayerProjection*>(next->info.layers[0]);
        for (unsigned eye=0;eye<2;++eye) {
            const auto sub=b->views[eye].subImage;
            const auto image=submitted_image(sub.swapchain);
            require((pixel(image.Get(),sub.imageArrayIndex)&255)==
                static_cast<unsigned>(b->views[eye].pose.position.y),
                "fresh private B pixels available before S EndFrame returns");
        }
    }
    unsigned real_id=0;
    const auto synthetic_ordinal=transport_test && (triple_core||triple_inline) && synthetic
        ? ++triple_synthetic_ordinals[frame_id] : 0;
    for(unsigned eye=0;eye<projection->viewCount;++eye) {
        auto sub=projection->views[eye].subImage;
        ComPtr<ID3D12Resource> image=submitted_image(sub.swapchain);
        const unsigned p=pixel(image.Get(),sub.imageArrayIndex,
            is_private_image(sub.swapchain)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET,
            sub.imageRect.offset.x+sub.imageRect.extent.width/2);
        if(((p>>8)&255)!=(eye?160u:80u))
            std::cerr<<"eye mismatch frame="<<frame_id<<" synthetic="<<synthetic<<" eye="<<eye<<" pixel="<<std::hex<<p<<std::dec<<'\n';
        correct &= ((p>>8)&255)==(eye?160u:80u);
        if(synthetic_ordinal) require((p&255)==(synthetic_ordinal==1?frame_id-1:frame_id),
            "3X synthetic image belongs to another pair or interpolation slot");
        if(!synthetic) {
            if(eye==0) real_id=p&255;
            const unsigned expected=alternating_eyes && frame_id>1 && frame_id%2!=eye ? frame_id-1:frame_id;
            if((p&255)!=expected)std::cerr<<"real mismatch frame="<<frame_id<<" eye="<<eye<<" expected="<<expected<<" got="<<(p&255)<<'\n';
            correct &= (p&255)==expected;
        }
    }
    correct &= end->layerCount==(promotion_test && !session_state->three_slot_director ? 1u : 3u);
    for(unsigned i=1;i<end->layerCount;++i) {
        const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[i]);
        correct &= q->type==XR_TYPE_COMPOSITION_LAYER_QUAD && q->subImage.imageArrayIndex==i-1;
        ComPtr<ID3D12Resource> image=submitted_image(q->subImage.swapchain);
        const unsigned p=pixel(image.Get(),q->subImage.imageArrayIndex,
            is_private_image(q->subImage.swapchain)
                ? D3D12_RESOURCE_STATE_COMMON
                : D3D12_RESOURCE_STATE_RENDER_TARGET);
        const unsigned expected=i==2 ? 55 : static_cast<unsigned>(q->pose.position.y);
        // Panel optical flow may select either endpoint; changing a uniform
        // marker's intensity has no required linear-blend result. Check the
        // exact source pair instead, independently of the interpolated pose.
        // Sixteen-byte separation rejects frames outside A..B and poison.
        const bool generated_panel=fsr_foveated && synthetic && i==1;
        const unsigned lower=generated_panel?(frame_id-1)*16:expected;
        const unsigned upper=generated_panel?frame_id*16:expected;
        const bool hud_match=(p&255)>=lower && (p&255)<=upper;
        if(!hud_match || ((p>>8)&255)!=60*i)
            std::cerr<<"HUD mismatch frame="<<frame_id<<" layer="<<i<<" expected="<<expected<<" pixel="<<std::hex<<p<<std::dec<<'\n';
        correct &= hud_match && ((p>>8)&255)==60*i;
        if(fsr_foveated && i==1) {
            const float pose=16.f*(float(frame_id)-(synthetic?.5f:0.f));
            require(std::abs(q->pose.position.y-pose)<.001f,
                "FSR panel pose must independently match the real/midpoint frame");
        }
    }
    submissions.emplace_back(frame_id,synthetic);
    // Real EndFrame promotion: the synthetic has been consumed, but its
    // deferred real endpoint has not even been submitted yet. Hold that GPU
    // queue so a premature director start cannot hide behind a fast copy.
    if(promotion_test && synthetic && session_state->presenter_start_requested &&
       !session_state->three_slot_director) {
        ++promotion_gate_count;
        hr(session_state->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&promotion_gate)),"promotion fence");
        auto* endpoint_queue=session_state->d3d12_synthesis_queue.Get();
        // Staging already contains B. Its remaining operation is the handover
        // copy on the binding queue; block that actual endpoint dependency.
        if(advanced_native_outputs(*session_state)) endpoint_queue=runtime_queue(*session_state);
        hr(endpoint_queue->Wait(promotion_gate.Get(),1),"delay final endpoint dependency");
        promotion_unblock=std::jthread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            promotion_gate_released=true;
            promotion_gate->Signal(1);
        });
    }
    if(reference_stall_test && synthetic && frame_id==4) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        reference_synthetic_returned=std::chrono::steady_clock::now();
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_end(XrSession session,const XrFrameEndInfo* end) {
    struct Close {~Close(){publication_frame_open=false;}} close;
    try{return fake_end_impl(session,end);}
    catch(const std::exception& error){std::cerr<<"runtime fixture: "<<error.what()<<'\n';throw;}
}
std::shared_ptr<SwapchainState> source(unsigned id,unsigned slices,bool projection,bool static_image=false) {
    auto c=std::make_shared<SwapchainState>(session_state, XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO});
    c->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(id));
    c->create_info={XR_TYPE_SWAPCHAIN_CREATE_INFO}; c->create_info.width=256;c->create_info.height=128;
    c->create_info.arraySize=slices;c->create_info.faceCount=1;c->create_info.mipCount=1;c->create_info.sampleCount=1;
    c->create_info.format=DXGI_FORMAT_R8G8B8A8_UNORM;c->create_info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    if(static_image)c->create_info.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    c->enumerated_d3d12_images.push_back(resource(slices,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET));
    c->enumerated_image_count=1;
    if(projection) {
        c->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* sources[]={c->enumerated_d3d12_images[0].Get()};
        hr(c->d3d12_history->initialize(session_state->d3d12_device.Get(),session_state->d3d12_queue.Get(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"history");
        SwapchainEligibilityReason reason{};uint64_t detail{};
        c->frame_generation=create_d3d12_frame_generation_swapchains(c,&reason,&detail);
        require(c->frame_generation!=nullptr,"native generation");
        for(auto& image:c->frame_generation->synthetic)synthetic_handles.insert(image.handle);
    }
    {std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;}
    return c;
}
void release(const std::shared_ptr<SwapchainState>& c, bool poison = true) {
    c->acquired_indices.push_back(0);c->waited_count=1;
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    require(XR_SUCCEEDED(layer_release_swapchain_image_impl(c->handle,&info)),"source release");
    if(!fsr_test && poison) fill(c->enumerated_d3d12_images[0].Get(),239,238); // queued director must own a private capture
}
}

#include "native_input_readiness_tests.inc"

#include "native_counted_release_tests.inc"
#include "optical_reserve_admission_tests.inc"
#include "bounded_optical_publication_tests.inc"
#include "fsr_phase_clock_tests.inc"
#include "worker_backpressure_fixture.inc"

int main(int argc,char** argv) {
    xrfg_fixture::ControlProfile autonomous_profile;
    struct RestoreRoute {
        std::optional<xrfg::embedded::Settings> previous;
        ~RestoreRoute() { if(previous) static_cast<void>(xrfg_fixture::request_control(*previous)); }
    } restore_route;
    RamTimingFixture capture(argc>1 && (std::string_view(argv[1])=="ram" || std::string_view(argv[1]).starts_with("gap-")));
    ComPtr<ID3D12Fence> blocked_synthesis, policy_gate;
    std::jthread policy_unblock;
    std::vector<std::shared_ptr<SwapchainState>> chains;
    try {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        auto dispatch=std::make_shared<Dispatch>(); session_state=std::make_shared<SessionState>(dispatch);
        auto& s=session_state;s->handle=reinterpret_cast<XrSession>(0x211);s->graphics_binding=SessionGraphicsBinding::d3d12;
        s->single_director_requested=argc>2 && std::string_view(argv[2])=="single-director";
        s->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
        s->nvidia_new_route = true; s->djules_route=kTestDjules;
        const std::string_view mode = argc>1 ? argv[1] : "";
        if(mode.starts_with("worker-busy-")) {
            dispatch->virtual_desktop_runtime=true;
            s->vdxr_legacy=true;s->djules_route=true;s->nvidia_new_route=false;
            s->separate_synthesis=mode.ends_with("fast");s->lower_vram=s->separate_synthesis;
        }
        if(mode=="benchmark-revisions") {
            const auto directory=xrfg::autonomous::configuration_path().parent_path();
            {std::ofstream ini(directory/L"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
            auto& logger=xrfg::bridge_flight_logger();logger.initialize(directory);
            auto desired=xrfg::autonomous::snapshot().desired;
            xrfg::autonomous::select_generation(desired,0,3);
            desired.prefer_fps=false;desired.triple_frame_gen=false;
            desired.separate_synthesis=0;desired.lower_vram=0;
            require(xrfg::autonomous::request(desired),"initial benchmark preferences");
            xrfg::autonomous::apply_runtime_settings();
            s->djules_route=true;s->control_revision=xrfg::embedded::snapshot().revision;
            const auto renderer_revision=s->control_revision;
            desired.prefer_fps=true;
            require(xrfg::autonomous::request(desired),"pipeline-only UI preference");
            xrfg::autonomous::apply_runtime_settings();
            const auto ui_revision=xrfg::autonomous::snapshot().revision;
            require(ui_revision!=renderer_revision && xrfg::embedded::snapshot().revision==renderer_revision,
                "independent UI/renderer revision mismatch was not reproduced");
            s->djules_deep_pipeline=true;
            log_benchmark_configuration(*s);logger.flush();
            auto contents=[&] {std::ifstream file(xrfg::benchmark_log_path(logger.log_path()));
                return std::string(std::istreambuf_iterator<char>(file),{});};
            auto text=contents();
            require(text.find("op=benchmark_configuration result=0 dur_us=0 a="+std::to_string(ui_revision)+" ")!=std::string::npos,
                "PreferFPS erased the applied configuration label");
            desired.triple_frame_gen=true;
            require(xrfg::autonomous::request(desired),"3X UI preference");
            xrfg::autonomous::apply_runtime_settings();
            const auto triple_revision=xrfg::autonomous::snapshot().revision;
            log_benchmark_configuration(*s);logger.flush();text=contents();
            const auto marker="op=benchmark_configuration result=0 dur_us=0 a="+std::to_string(triple_revision)+" ";
            require(text.find(marker)==std::string::npos,"unapplied 3X was labelled active");
            s->djules_triple_requested=true;
            log_benchmark_configuration(*s);logger.flush();text=contents();
            require(text.find(marker)!=std::string::npos,"applied 3X lost its configuration label");
            desired.separate_synthesis=1;
            desired.lower_vram=1; // Unified Fast mode; A alone is normalized off.
            require(xrfg::autonomous::request(desired),"transport UI preference");
            xrfg::autonomous::apply_runtime_settings();
            const auto transport_revision=xrfg::autonomous::snapshot().revision;
            require(transport_revision!=triple_revision,"transport test must request an actual Fast-mode change");
            const auto transport_marker="op=benchmark_configuration result=0 dur_us=0 a="+std::to_string(transport_revision)+" ";
            log_benchmark_configuration(*s);logger.flush();text=contents();
            require(text.find(transport_marker)==std::string::npos,"pending renderer transport was labelled active");
            s->control_revision=xrfg::embedded::snapshot().revision;
            log_benchmark_configuration(*s);logger.flush();text=contents();
            require(text.find(transport_marker)!=std::string::npos,"later transport change lost its label");
            logger.shutdown();session_state.reset();
            std::cout<<"Actual benchmark logger preserves independent revisions and rejects pending pipeline/transport PASS\n";
            return 0;
        }
        if((mode=="steamvr-djules-restart" || mode.starts_with("transport-ready-")) && argc>2) {
            std::filesystem::path log_root=argv[2];std::filesystem::create_directories(log_root);
            {std::ofstream ini(log_root/L"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
            xrfg::bridge_flight_logger().initialize(log_root);
        }
        if(mode=="steamvr-djules-outputs" || mode=="steamvr-djules-restart") {
            dispatch->steamvr_runtime=true;s->djules_route=true;s->separate_synthesis=1;s->lower_vram=1;
            if(mode=="steamvr-djules-restart") {
                s->presenter_active=true;
                stop_continuous_presenter(s);
                require(!s->presenter_active && s->presenter_stop_requested,"restart stop state not reproduced");
            }
        }
        if(mode=="steamvr-output-retire")dispatch->steamvr_runtime=true;
        reference_stall_test=mode=="legacy-queue-stall-exposure";
        const bool shared_readiness=mode.starts_with("transport-ready-");
        const bool shared_progress=mode.starts_with("transport-progress-");
        shared_present_queue_test=shared_readiness || shared_progress || mode.starts_with("transport-publication-");
        publication_test=mode.starts_with("transport-publication-");
        reference_readiness_test=mode=="legacy-queue-readiness" || shared_readiness || publication_test;
        reference_capture_overlap_test=mode=="legacy-queue-capture-overlap";
        capture_busy_test=mode=="capture-busy";
        guide_bypass_switch=mode=="guide-bypass-switch";
        lower_vram_test=mode.ends_with("lower-vram"); direct_output_test=mode.ends_with("direct-output");
        if(lower_vram_test) { s->separate_synthesis=1; s->lower_vram=1; }
        if(direct_output_test) s->lower_vram=0;
        if(mode=="legacy-queue-lower-vram" || mode=="legacy-queue-direct-output") s->nvidia_new_route=false;
        if(mode=="fidelityfx-lower-vram") { s->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx; s->nvidia_new_route=false; s->djules_route=false; }
        deep_readiness=mode.starts_with("deep-readiness");
        deep_switch=mode.starts_with("deep-switch");
        deep_readiness=deep_readiness || deep_switch;
        // Owned-repeat readiness belongs to deferred staged outputs. Preserve
        // that regression explicitly with Lower VRAM on and exercise the new
        // direct-output default in separate fixtures with the same GPU gate.
        if(deep_readiness && !direct_output_test) { s->separate_synthesis=1; s->lower_vram=1; }
        s->djules_deep_pipeline=deep_readiness;
        triple_switch=mode.starts_with("triple-switch");
        triple_core=triple_switch || (argc>1 && std::string_view(argv[1]).starts_with("triple-core"));
        triple_inline=argc>1 && std::string_view(argv[1]).starts_with("triple-inline");
        if(deep_readiness || triple_core || triple_inline) s->djules_route=true;
        s->djules_triple_requested=triple_core||triple_inline;
        if(triple_inline) { dispatch->virtual_desktop_runtime=false; dispatch->inline_unless_pipelined=true; }
        fsr_test=argc>1 && std::string_view(argv[1]).starts_with("fsr-");
        fsr_foveated=mode=="fsr-foveated" || mode=="fsr-queue-foveated";
        legacy_queue=argc>1 && std::string_view(argv[1]).starts_with("legacy-queue");
        legacy_early_current=argc>1 && std::string_view(argv[1])=="legacy-queue-early-b";
        midpoint_cadence=argc>1 && std::string_view(argv[1])=="midpoint-cadence";
        if(legacy_queue) {
            s->nvidia_new_route=false;
            s->minimum_runtime_display_period=8333333;
            ProjectionSnapshot previous, current;
            previous.display_time=1000000000;
            for(int periods=1;periods<=5;++periods) {
                current.display_time=previous.display_time+periods*8333333;
                require(synthetic_interpolation_fraction(s,previous,current)==0.5F,
                    "legacy NVIDIA fraction must stay at the validated midpoint");
            }
            current.display_time=previous.display_time+3*8333333;
            s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
            require(synthetic_interpolation_fraction(s,previous,current)>0.66F,
                "FSR fraction contract changed during legacy-only correction");
            s->frame_generation_mode={};
            dispatch->steamvr_runtime=std::string_view(argv[1]) != "legacy-queue-vdxr";
        }
        msfs_time_test=mode.starts_with("msfs-time-") || mode.starts_with("msfs-control-");
        transport_test=mode.starts_with("transport-") || mode.starts_with("transport3-") || msfs_time_test;
        if(transport_test) {
            const bool reference=mode.find("-ref-")!=std::string_view::npos;
            s->nvidia_new_route=!reference;
            s->djules_route=mode.find("-djules-")!=std::string_view::npos || mode.find("-djules3-")!=std::string_view::npos;
            s->separate_synthesis=mode[mode.size()-2]=='1';
            s->lower_vram=mode.back()=='1';
            dispatch->steamvr_runtime=mode.find("-steam-")!=std::string_view::npos;
            dispatch->virtual_desktop_runtime=!dispatch->steamvr_runtime;
            if(s->single_director_requested && dispatch->virtual_desktop_runtime)
                require(!single_director_pacing(*s),"VDXR transport must retain its existing internal timing policy");
            dispatch->inline_unless_pipelined=dispatch->virtual_desktop_runtime;
            transport_inline=s->djules_route && dispatch->virtual_desktop_runtime;
            // VDXR's split-thread shape retains its existing synchronous owner.
            if(transport_inline) {s->application_wait_thread_id=1;s->application_end_thread_id=2;s->split_frame_loop=true;}
            if(mode.starts_with("transport3-") || mode.find("-djules3-")!=std::string_view::npos) {
                s->djules_triple_requested=true;
                triple_core=!transport_inline;triple_inline=transport_inline;
            }
            if(dispatch->steamvr_runtime) private_budget_limit=8; // actual log ceiling: eight outputs fit, twelve refuse
            legacy_queue=reference || (s->lower_vram && !s->separate_synthesis && !transport_inline);
            if(mode.find("-fidelity-")!=std::string_view::npos) {
                s->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
                s->nvidia_new_route=false;
            }
        }
        const bool fsr_clock_probe=mode.starts_with("transport-clock-fsr-");
        std::filesystem::path fsr_clock_log_directory;
        if(fsr_clock_probe) {
            fsr_test=true;fsr_queue=true;legacy_queue=false;s->nvidia_new_route=false;
            fsr_clock_log_directory=std::filesystem::temp_directory_path()/
                ("ofxr-fsr-clock-"+std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(fsr_clock_log_directory);
            {std::ofstream ini(fsr_clock_log_directory/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
            xrfg::bridge_flight_logger().initialize(fsr_clock_log_directory);
        }
        if(shared_readiness || shared_progress || publication_test) {
            // MSFS's split loop uses the fresh queued consumer, not the
            // three-slot worker. Keep that actual session shape in the probe.
            legacy_queue=true;
            if(mode.find("-fsr-")!=std::string_view::npos) {
                fsr_test=true;fsr_queue=true;legacy_queue=false;s->nvidia_new_route=false;
            }
        }
        fsr_queue=fsr_queue || (argc>1 && std::string_view(argv[1]).starts_with("fsr-queue"));
        fsr_queue_slow=argc>1 && std::string_view(argv[1]) == "fsr-queue-slow";
        fsr_queue_slow=fsr_queue_slow || (argc>1 && std::string_view(argv[1]) == "legacy-queue-slow");
        fsr_queue_switch=argc>1 && std::string_view(argv[1]) == "fsr-queue-switch";
        fsr_queue_gpu_pending=shared_progress || (argc>1 && std::string_view(argv[1]) == "fsr-queue-gpu-pending");
        fsr_stereo_guard=argc>1 && std::string_view(argv[1]).ends_with("stereo-guard");
        fsr_missing=argc>1 && std::string_view(argv[1])=="fsr-inline-wait";
        if(msfs_time_test && mode.find("-fsr-")!=std::string_view::npos) {
            fsr_test=true; fsr_queue=true; s->nvidia_new_route=false;
        }
        if(fsr_test) {
            s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
            dispatch->steamvr_runtime=fsr_queue;
            // This fixture explicitly requests native SteamVR queue ownership.
            // The SessionState default is immutable Legacy, not a live-runtime probe.
            if(fsr_queue)s->vdxr_legacy=false;
        }
        // These publication probes request the ordinary SteamVR transport.
        // SessionState's Legacy default is not a substitute for session routing.
        if(publication_test && dispatch->steamvr_runtime)s->vdxr_legacy=false;
        s->control_revision=xrfg::embedded::snapshot().revision;
        if(mode=="msfs-order-guards") { test_msfs_capture_order(*s); session_state.reset(); return 0; }
        hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&s->d3d12_device)),"device");
        if(mode=="bounded-publication-scope") {optical_reserve_scope_test();session_state.reset();return 0;}
        if(mode=="bounded-publication-preparation" || mode=="bounded-publication-old-control") {bounded_optical_preparation_test(mode.ends_with("old-control"));session_state.reset();return 0;}
        if(mode=="bounded-publication-publication") {optical_reserve_activation_test();session_state.reset();return 0;}
        if(mode=="bounded-publication-consumer") {optical_reserve_consumer_test();session_state.reset();return 0;}
        if(mode=="bounded-publication-admission") {optical_reserve_admission_test();session_state.reset();return 0;}
        if(mode=="optical-reserve-scope") {optical_reserve_scope_test();session_state.reset();return 0;}
        if(mode=="optical-reserve-admission") {optical_reserve_admission_test();session_state.reset();return 0;}
        if(mode=="optical-reserve-activation") {optical_reserve_activation_test();session_state.reset();return 0;}
        if(mode=="optical-reserve-consumer") {optical_reserve_consumer_test();session_state.reset();return 0;}
        if(mode=="optical-reserve-consumer-old-control") {optical_reserve_consumer_test(true);session_state.reset();return 0;}
        if(mode=="optical-reserve-old-control") {optical_reserve_admission_test(true);session_state.reset();return 0;}
        if(mode=="native-counted-scope") {native_counted_scope_test();session_state.reset();return 0;}
        if(mode=="native-counted-starved") {native_counted_starvation_test();session_state.reset();return 0;}
        if(mode=="native-counted-progress") {native_counted_progress_test();session_state.reset();return 0;}
        if(mode=="native-counted-old-control") {native_counted_progress_test(true);session_state.reset();return 0;}
        if(mode=="fsr-phase-scope") {fsr_phase_scope_clock_test();session_state.reset();return 0;}

        if(mode.starts_with("input-")) { native_input_readiness_test(mode);session_state.reset();return 0; }
        if(mode=="delivery-observer") {
            const auto directory=std::filesystem::temp_directory_path()/
                ("ofxr-delivery-observer-"+std::to_string(GetCurrentProcessId()));
            std::filesystem::create_directories(directory);
            {std::ofstream ini(directory/"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
            auto& logger=xrfg::bridge_flight_logger();logger.initialize(directory);
            dispatch->steamvr_runtime=true;
            ComPtr<ID3D12Fence> output;
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&output)),"observer output fence");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->app_release_fence)),"observer app fence");
            GeneratedFrameEndInfo frame;frame.synthetic=true;frame.info.displayTime=123;
            frame.app_release_value=7;frame.reference_readiness.push_back({output,3,true});
            frame.diagnostic_handover[0]={output,3,false};
            ProjectionLayerCopy projection;projection.views.resize(2);
            projection.views[0].pose.position={.125F,-.25F,1.25F};
            projection.views[0].pose.orientation.w=1;
            frame.projections.push_back(std::move(projection));
            const auto observed_at=std::chrono::steady_clock::now();
            log_native_delivery_probe(*s,&frame,32,0);
            require(std::chrono::steady_clock::now()-observed_at<std::chrono::milliseconds(100),
                "observer waited for pending GPU work");
            require(output->GetCompletedValue()==0 && s->app_release_fence->GetCompletedValue()==0,
                "observer advanced a pending GPU fence");
            hr(output->Signal(3),"complete output independently of app tail marker");
            log_native_delivery_probe(*s,&frame,32,1,456);
            hr(s->app_release_fence->Signal(7),"complete app marker");
            log_native_delivery_probe(*s,&frame,32,2,456);
            log_native_delivery_probe(*s,&frame,32,3,456);
            require(waits==0 && begins==0 && ends==0,"observer entered XR");
            require(output->GetCompletedValue()==3 && s->app_release_fence->GetCompletedValue()==7,
                "observer changed a completed GPU fence");
            logger.flush();
            std::ifstream file(logger.log_path());
            const std::string text{std::istreambuf_iterator<char>(file),{}};
            for(unsigned stage=0;stage<4;++stage) {
                require(text.find("op=presenter_source result="+std::to_string(5490+stage)+
                    " dur_us=0 a=65 b=123 c="+std::to_string(stage?456:0))!=std::string::npos,
                    "observer lost output identity or source/physical time");
                require(text.find("op=gpu_fence_snapshot result="+std::to_string(54900+stage*100)+
                    " dur_us=0 a="+std::to_string(reinterpret_cast<std::uintptr_t>(output.Get()))+
                    " b=3 c="+std::to_string(stage?3:0))!=std::string::npos,
                    "observer confused output readiness with app release");
            }
            require(text.find("op=gpu_fence_snapshot result=55090 dur_us=0 a="+
                std::to_string(reinterpret_cast<std::uintptr_t>(s->app_release_fence.Get()))+" b=7 c=0")!=std::string::npos,
                "observer lost independently pending app marker");
            require(text.find("op=luke_inline_pose_bits result=549000 dur_us=0 a=65 ")!=std::string::npos,
                "observer lost numeric head translation");
            require(text.find("op=luke_inline_pose_bits result=549000 dur_us=0 a=65 b=4467570833547591680 c=4584664420663164928")!=std::string::npos,
                "observer changed physical position components");
            require(text.find("op=gpu_fence_snapshot result=55120 dur_us=0 a="+
                std::to_string(reinterpret_cast<std::uintptr_t>(output.Get()))+" b=3 c=3")!=std::string::npos,
                "observer lost staged handover completion");
            const auto count=text.size();
            s->luke_afw_enabled=true;log_native_delivery_probe(*s,&frame,32,2,456);
            s->luke_afw_enabled=false;dispatch->steamvr_runtime=false;
            log_native_delivery_probe(*s,&frame,32,2,456);logger.flush();
            std::ifstream excluded(logger.log_path());
            require(std::string(std::istreambuf_iterator<char>(excluded),{}).size()==count,
                "native SteamVR observer entered an excluded transport");
            logger.shutdown();session_state.reset();
            std::cout<<"Nonblocking native delivery observer distinguishes output/app fences, four boundaries and numeric pose PASS\n";
            return 0;
        }
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->d3d12_queue)),"application queue");
        hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->d3d12_synthesis_queue)),"synthesis queue");
        if (mode == "runtime-queue-policy") {
            dispatch->steamvr_runtime = true;
            for (bool separate : {false, true}) for (int backend : {0, 1, 2, 3})
                for (int generator : {0, 1, 2}) for (bool hybrid : {false, true}) for (bool enabled : {false, true}) {
                    s->steamvr_separate_queue = separate;
                    s->menu_enabled = enabled;
                    s->nvidia_new_route = backend >= 2;
                    s->djules_route = backend == 3;
                    s->optical_flow_backend = backend == 0 ? xrfg::D3D12OpticalFlowBackend::fidelity_fx : xrfg::D3D12OpticalFlowBackend::nvidia;
                    s->frame_generation_mode = static_cast<xrfg::D3D12FrameGenerationMode>(generator);
                    s->hybrid_requested = hybrid; s->nvidia_options.hybrid = hybrid;
                    require(runtime_binding_queue_requested(*s) == separate, "initial generator changed explicit queue choice");
                }
            s->nvidia_new_route = false; s->djules_route = false;
            s->optical_flow_backend = xrfg::D3D12OpticalFlowBackend::fidelity_fx;
            s->frame_generation_mode = xrfg::D3D12FrameGenerationMode::fsr3;
            s->hybrid_requested = false; s->nvidia_options.hybrid = false;
            for (bool separate : {false, true}) {
                s->steamvr_separate_queue = separate;
                require(initialize_runtime_binding_queue(*s) == (separate ? S_OK : S_FALSE), "actual queue initialization disagrees with choice");
                require((runtime_queue(*s) != s->d3d12_queue.Get()) == separate, "actual runtime queue identity incorrect");
                ComPtr<ID3D12Fence> gate, runtime_done, application_done;
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)), "queue gate");
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&runtime_done)), "runtime marker");
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&application_done)), "application marker");
                hr(runtime_queue(*s)->Wait(gate.Get(), 1), "hold runtime work");
                hr(runtime_queue(*s)->Signal(runtime_done.Get(), 1), "runtime progress");
                hr(s->d3d12_queue->Signal(application_done.Get(), 1), "application progress");
                const HRESULT progress = wait_composition_fence(application_done.Get(), 1, 30);
                // Always release the GPU gate before an assertion can unwind.
                hr(gate->Signal(1), "release runtime gate");
                hr(wait_composition_fence(runtime_done.Get(), 1), "runtime drain");
                hr(wait_composition_fence(application_done.Get(), 1), "application drain");
                require(separate ? SUCCEEDED(progress) : progress == HRESULT_FROM_WIN32(ERROR_BUSY), "runtime dependency isolation differs from selected mode");
            }
            dispatch->steamvr_runtime = false; dispatch->virtual_desktop_runtime = true;
            require(!runtime_binding_queue_requested(*s), "VDXR gained a SteamVR override");
            dispatch->virtual_desktop_runtime = false; dispatch->steamvr_runtime = true;
            s->luke_afw_enabled = true; s->steamvr_separate_queue = false; s->nvidia_new_route = true;
            require(runtime_binding_queue_requested(*s), "Luke established queue contract changed");
            std::cout << "Explicit native SteamVR queue policy independent of generator; real GPU shared/separate dependency checks PASS\n";
            session_state.reset(); // Retire the GPU device before process-static layer globals.
            return 0;
        }
        if(reference_capture_overlap_test || shared_readiness || shared_progress || publication_test) {
            hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->binding_queue)),"Reference XR binding queue");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->app_release_fence)),"Reference release fence");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->reacquire_fence)),"Reference reacquire fence");
        }
        if(deep_readiness||triple_core) {
            dispatch->steamvr_runtime=true;
            hr(s->d3d12_device->CreateCommandQueue(&q,IID_PPV_ARGS(&s->binding_queue)),"DJ runtime binding queue");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->app_release_fence)),"DJ app release fence");
            hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&s->reacquire_fence)),"DJ reacquire fence");
        }
        dispatch->create_swapchain=fake_create;dispatch->enumerate_swapchain_images=fake_enumerate;dispatch->destroy_swapchain=fake_destroy;
        dispatch->acquire_swapchain_image=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,uint32_t* index)->XrResult{*index=0;return XR_SUCCESS;};
        dispatch->wait_swapchain_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*)->XrResult{return XR_SUCCESS;};
        dispatch->release_swapchain_image=[](XrSwapchain handle,const XrSwapchainImageReleaseInfo*)->XrResult{
            if(publication_test && defer_native_direct_publication(*session_state)) {
                bool projection=false;
                for(const auto& chain:find_swapchains(session_state)) if(chain->frame_generation) {
                    for(const auto& image:chain->frame_generation->current) projection|=image.handle==handle;
                    for(const auto& image:chain->frame_generation->synthetic) projection|=image.handle==handle;
                }
                if(projection) {
                    if(!publication_frame_open || publication_consumer_thread!=GetCurrentThreadId()) {
                        std::cerr<<"projection image released outside the consumer's begun XR frame\n";
                        return XR_ERROR_RUNTIME_FAILURE;
                    }
                    ++publication_releases;
                }
            }
            return XR_SUCCESS;
        };
        dispatch->wait_frame=fake_wait;dispatch->begin_frame=fake_begin;dispatch->end_frame=fake_end;
        {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
        promotion_test=argc>1 && std::string_view(argv[1]).starts_with("promotion");
        const bool gap_test=argc>1 && std::string_view(argv[1]).starts_with("gap-");
        fsr_packed=mode=="fsr-packed" || mode=="fsr-queue-packed";
        const bool array=fsr_test || (argc>1 && (std::string_view(argv[1])=="array" || std::string_view(argv[1])=="promotion-array" || std::string_view(argv[1])=="gap-array"));
        alternating_eyes=argc>1 && std::string_view(argv[1])=="repeat";
        auto left=source(0x201,array && !fsr_packed?2:1,true);chains.push_back(left);
        auto right=array?left:source(0x202,1,true);if(!array)chains.push_back(right);
        auto dynamic=source(0x203,2,false);auto fixed=source(0x204,2,false,true);
        chains.push_back(dynamic);chains.push_back(fixed);
        if(mode.starts_with("worker-busy-")) {
            worker_backpressure_probe(s,left,right,mode.find("timeout")!=std::string_view::npos);
            for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
            require(created==destroyed,"worker backpressure leaked XR resources");
            {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
            chains.clear();left.reset();right.reset();dynamic.reset();fixed.reset();session_state.reset();
            return 0;
        }
        if(mode=="resource-cleanup") {
            restore_route.previous=xrfg::embedded::snapshot().desired;
            // Three full-size UI histories reproduce UEVR's enumerated targets.
            // Start with owned outputs too, simulating a previous admission.
            std::vector<std::shared_ptr<SwapchainState>> unused;
            for(unsigned id:{0x205u,0x206u,0x207u}) {
                auto chain=source(id,1,true);chains.push_back(chain);unused.push_back(chain);
                chain->diagnostic_layer_roles=2;
            }
            fill(left->enumerated_d3d12_images[0].Get(),1,80);release(left);
            fill(right->enumerated_d3d12_images[0].Get(),1,160);release(right);
            const auto history=left->d3d12_history;
            const auto serial=left->last_released_capture->serial;
            XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            views[0].subImage.swapchain=left->handle;views[1].subImage.swapchain=right->handle;
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.views=views;projection.viewCount=2;
            const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.layerCount=1;frame.layers=&layer;
            std::array<ProjectionResourceMapping,2> projected{};
            projected[0].application_swapchain=left->handle;projected[1].application_swapchain=right->handle;
            auto desired=*restore_route.previous;desired.enabled=true;desired.backend=2;
            desired.frame_generation=0;desired.hybrid=false;desired.separate_synthesis=0;desired.lower_vram=0;
            require(ensure_projection_frame_generation(s,projected),"new projection admission cleanup");
            require(images.size()==8,"old projection/UI outputs survive new projection admission");
            for(bool steam:{true,false}) {
                dispatch->steamvr_runtime=steam;dispatch->virtual_desktop_runtime=!steam;
                for(unsigned generator:{0u,1u,2u}) {
                    desired.frame_generation=generator;
                    desired.preset=desired.preset==0?1:0;
                    require(xrfg_fixture::request_control(desired),"cleanup route request");
                    hr(apply_embedded_control(s,false,&frame),"cleanup applied route");
                    for(const auto& chain:unused)
                        require(!chain->frame_generation && chain->generation_eligible_pending,
                            "unused UI outputs retained or eagerly recreated");
                    require(images.size()<=8,"route switch spent nonprojected budget");
                    require(ensure_projection_frame_generation(s,projected) && left->frame_generation && right->frame_generation,
                        "projected eyes unavailable after cleanup");
                    require(images.size()==8 && !private_budget_refusals,"normal allocation silently reduced or exceeded");
                    require(left->d3d12_history==history && left->last_released_capture->serial==serial,
                        "cleanup discarded application history/capture");
                }
            }
            // AER can alternate eye source identities: a formerly projected
            // source remains useful even when absent from this one frame.
            desired.frame_generation=0;desired.backend=1;
            require(xrfg_fixture::request_control(desired),"cleanup optical return");
            hr(apply_embedded_control(s,false,&frame),"cleanup optical control");
            for(unsigned i=0;i<2;++i) {
                unused[i]->generation_eligible_pending=true;
                require(ensure_frame_generation(unused[i]) && unused[i]->frame_generation,"AER extra fixture");
            }
            unused[0]->diagnostic_layer_roles=1;unused[1]->diagnostic_layer_roles=2;
            s->luke_afw_enabled=true;
            desired.preset=desired.preset==0?1:0;
            require(xrfg_fixture::request_control(desired),"AER cleanup request");
            hr(apply_embedded_control(s,false,&frame),"AER cleanup control");
            require(unused[0]->frame_generation && !unused[1]->frame_generation,
                "AER eye history retired or proven auxiliary output retained");
            s->luke_outlaws_preframe=true;s->luke_outlaws_frame_started=false;
            require(!projection_owned_outputs(*s),"Outlaws startup admission contract removed");
            s->luke_outlaws_preframe=false;
            desired.enabled=false;
            require(xrfg_fixture::request_control(desired),"cleanup Off request");
            hr(apply_embedded_control(s,false,&frame),"cleanup Off");
            require(!images.empty(),"OFXR Off destroyed outputs still required by AFW");
            const auto prior_afw=xrfg::embedded::afw_snapshot().desired;
            auto afw_off=prior_afw;afw_off.mode=0;
            require(xrfg::embedded::request_afw(afw_off),"AFW-only Off request");
            hr(apply_embedded_control(s,false,&frame),"AFW Off without an OFXR revision");
            require(images.empty(),"disabled generation retains output swapchains");
            require(xrfg::embedded::request_afw(prior_afw),"AFW fixture restore");
            s->luke_afw_enabled=false;
            // A real refusal must rearm only on an explicit changed revision,
            // for vector providers too, preserving the existing budget gate.
            dispatch->steamvr_runtime=true;dispatch->virtual_desktop_runtime=false;
            desired.enabled=true;desired.frame_generation=1;desired.separate_synthesis=0;desired.lower_vram=0;
            require(xrfg_fixture::request_control(desired),"vector refusal request");
            hr(apply_embedded_control(s,false,&frame),"vector refusal control");
            private_budget_limit=3;
            require(!ensure_projection_frame_generation(s,projected),"vector refusal fixture");
            release_session_generation_budget(s);
            const auto refusals=private_budget_refusals, allocations=created.load();
            hr(apply_embedded_control(s,false,&frame),"same vector refusal revision");
            require(private_budget_refusals==refusals && created==allocations,"unchanged refusal retried");
            private_budget_limit=4;desired.separate_synthesis=1;desired.lower_vram=1;
            require(xrfg_fixture::request_control(desired),"vector reduced recovery request");
            hr(apply_embedded_control(s,false,&frame),"vector reduced recovery control");
            require(!s->generation_budget_exhausted && ensure_projection_frame_generation(s,projected) &&
                left->frame_generation && right->frame_generation && images.size()==4,
                "vector budget refusal remains latched after explicit reduction");
            private_budget_limit=0;desired.frame_generation=0;desired.backend=2;
            desired.separate_synthesis=0;desired.lower_vram=0;
            require(xrfg_fixture::request_control(desired),"cleanup oracle route");
            hr(apply_embedded_control(s,false,&frame),"cleanup oracle control");
            require(ensure_projection_frame_generation(s,projected),"cleanup oracle eyes");
            for(const auto& chain:{left,right})for(const auto& image:chain->frame_generation->synthetic)
                if(image.handle)synthetic_handles.insert(image.handle);
            std::cout<<"All-runtime/provider projected admission, drained unused output cleanup, AER history preservation, Off retirement and vector budget recovery PASS\n";
        }
        if(mode=="composition-failures") {
            const auto directory=xrfg::autonomous::configuration_path().parent_path();
            {std::ofstream ini(directory/L"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
            auto& logger=xrfg::bridge_flight_logger();logger.initialize(directory);
            fill(dynamic->enumerated_d3d12_images[0].Get(),1,60);release(dynamic,false);
            const auto packet=[&] {
                GeneratedFrameEndInfo info;
                XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
                quad.subImage.swapchain=dynamic->handle;quad.subImage.imageRect.extent={256,128};
                info.composition_layers.emplace_back(quad);
                return info;
            };
            // A runtime allocation refusal and retained color exhaustion must
            // yield distinguishable production-flight records, not just -2.
            private_budget_limit=static_cast<unsigned>(images.size());
            auto refused=packet();
            require(own_composition_images(s,refused)==XR_ERROR_RUNTIME_FAILURE,"auxiliary allocation refusal");
            private_budget_limit=0;
            std::array<GeneratedFrameEndInfo,3> held;
            for(auto& owned:held) { owned=packet();require(XR_SUCCEEDED(own_composition_images(s,owned)),"own retained auxiliary color"); }
            auto exhausted=packet();
            require(own_composition_images(s,exhausted)==XR_ERROR_RUNTIME_FAILURE,"auxiliary retained-slot refusal");
            logger.flush();
            std::ifstream stream(logger.log_path());const std::string contents{std::istreambuf_iterator<char>(stream),{}};
            const auto prefix="op=composition_publish result=-2 dur_us=0 a="+std::to_string(handle_value(dynamic->handle));
            require(contents.find(prefix+" b=1 c="+std::to_string(static_cast<std::uint64_t>(static_cast<std::int64_t>(XR_ERROR_LIMIT_REACHED))))!=std::string::npos,
                "runtime auxiliary refusal code missing from flight record");
            require(contents.find(prefix+" b=2 c=0")!=std::string::npos,"retained auxiliary exhaustion mislabeled");
            held={};
            auto recovered=packet();require(XR_SUCCEEDED(own_composition_images(s,recovered)),"released auxiliary slot failed recovery");
            recovered={};
            for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
            require(created==destroyed,"auxiliary diagnostic leaked runtime handles");
            {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
            chains.clear();left.reset();right.reset();dynamic.reset();fixed.reset();session_state.reset();
            std::cout<<"Actual auxiliary allocation refusal, retained-slot refusal and recovery diagnostics PASS\n";
            return 0;
        }
        if(transport_test) {
            const unsigned current=s->lower_vram?1:2;
            const unsigned synthetic=s->lower_vram?1:2;
            require(images.size()==(array?1:2)*(current+synthetic),"transport flags did not select exact XR budget");
            for(const auto& chain:{left,right}) {
                const auto g=chain->frame_generation;
                require(g && g->staged_outputs==bool(s->separate_synthesis),"separation still coupled to reduction");
                require(g->current_runtime_rings==current && g->synthetic_runtime_rings==synthetic,"incorrect transport ring counts");
                if(g->staged_outputs) require(std::count_if(g->current_staging.begin(),g->current_staging.end(),[](const auto& image){return bool(image);})==2 &&
                    std::count_if(g->synthetic_staging.begin(),g->synthetic_staging.end(),[](const auto& image){return bool(image);})==(s->djules_triple_requested?4:2),"2X reserved unused 3X synthesis textures");
            }
        }
        if(mode=="steamvr-djules-outputs")
            require(left->frame_generation->staged_outputs && right->frame_generation->staged_outputs &&
                images.size()==4,"Djules reduced transport did not halve four XR rings to two per eye");
        if(mode=="steamvr-new-bounded") {
            dispatch->steamvr_runtime=true;
            s->djules_route=false;
            for(const auto& chain:{left,right}) {
                fill(chain->enumerated_d3d12_images[0].Get(),1,chain==left?80:160);
                release(chain);
                drain_swapchain_gpu(chain);destroy_frame_generation_swapchains(chain);
                chain->generation_eligible_pending=true;
            }
            const auto capture=right->last_released_capture->serial;
            private_budget_limit=6;
            std::array<ProjectionResourceMapping,2> mapped{};
            mapped[0].application_swapchain=left->handle;mapped[1].application_swapchain=right->handle;
            require(!ensure_projection_frame_generation(s,mapped),"full allocation silently reduced its XR budget");
            require(private_budget_refusals==1 && right->last_released_capture->serial==capture,
                "full allocation refusal changed capture or retried unboundedly");
            restore_route.previous=xrfg::embedded::snapshot().desired;
            auto desired=*restore_route.previous;
            desired.enabled=true;desired.backend=2;desired.frame_generation=0;
            desired.separate_synthesis=1;desired.lower_vram=1;
            require(xrfg_fixture::request_control(desired),"explicit bounded output request");
            hr(apply_embedded_control(s),"explicit bounded output control");
            require(ensure_projection_frame_generation(s,mapped),"explicit reduced budget did not recover");
            require(left->frame_generation && right->frame_generation && images.size()==4 &&
                left->frame_generation->staged_outputs && right->frame_generation->staged_outputs,
                "explicit reduction lost owned output capacity");
            require(right->last_released_capture->serial==capture,"explicit reduction changed capture");
            private_budget_limit=0; // auxiliary ownership oracle is independent of the budget
            for(const auto& chain:{left,right})for(const auto& image:chain->frame_generation->synthetic)
                if(image.handle)synthetic_handles.insert(image.handle);
        }
        if(mode=="steamvr-budget-recovery") {
            restore_route.previous=xrfg::embedded::snapshot().desired;
            dispatch->steamvr_runtime=true;
            std::array<ProjectionResourceMapping,2> projected{};
            projected[0].application_swapchain=left->handle;
            projected[1].application_swapchain=right->handle;
            // REAL VR can enumerate extra eye targets that are absent from the
            // current submitted projection. Their histories do not admit outputs.
            for(unsigned id:{0x205u,0x206u}) {
                auto unused=source(id,1,true);chains.push_back(unused);
                destroy_frame_generation_swapchains(unused);
                unused->generation_eligible_pending=true;
            }
            fill(left->enumerated_d3d12_images[0].Get(),1,80);release(left);
            fill(right->enumerated_d3d12_images[0].Get(),1,160);release(right);
            const auto history=left->d3d12_history;
            const auto serial=left->last_released_capture->serial;
            auto desired=*restore_route.previous;
            desired.enabled=true;desired.backend=1;desired.frame_generation=0;desired.separate_synthesis=0;desired.lower_vram=0;
            require(xrfg_fixture::request_control(desired),"budget reference request");
            // The owner run admits the eight New/Reference private outputs,
            // but rejects the twelve direct Djules outputs, even in 2X.
            private_budget_limit=8;
            hr(apply_embedded_control(s),"budget reference control");
            s->presenter_display_period=22'222'222;
            require(adopt_reference_display_period(*s,std::chrono::nanoseconds(11'111'111)) &&
                s->presenter_display_period==11'111'111,"reference used throttled app clock as display clock");
            require(!adopt_reference_display_period(*s,std::nullopt) &&
                !adopt_reference_display_period(*s,std::chrono::nanoseconds(1)),"invalid display clock admitted");
            require(!chains[4]->frame_generation && !chains[5]->frame_generation,
                "control allocated outputs for unsubmitted eye targets");
            for(unsigned route:{3u,1u,2u}) {
                desired.backend=route;desired.separate_synthesis=route==3;desired.lower_vram=route==3;
                require(xrfg_fixture::request_control(desired),"budget route request");
                hr(apply_embedded_control(s),"budget route control");
                require(ensure_projection_frame_generation(s,projected),"bounded projected output creation");
                require(left->frame_generation && right->frame_generation,"budget lost one eye");
                require(private_budget_refusals==0,"route replacement exceeded final output budget");
                require(left->frame_generation->staged_outputs==(route==3) &&
                    right->frame_generation->staged_outputs==(route==3),"SteamVR staged output scope changed");
                require(!adopt_reference_display_period(*s,std::chrono::nanoseconds(8'333'333)) || route==1,
                    "reference display clock changed New/Djules");
                require(left->d3d12_history==history && left->last_released_capture->serial==serial,
                    "output replacement changed source history/capture");
            }
            // The supplied log has a per-eye synthetic-ring refusal without
            // the session-wide gate: reproduce that exact local latched state.
            drain_swapchain_gpu(left);destroy_frame_generation_swapchains(left);
            private_budget_limit=7;
            SwapchainEligibilityReason local_reason{};std::uint64_t local_detail{};
            auto refused=create_d3d12_frame_generation_swapchains(left,&local_reason,&local_detail);
            require(!refused && local_reason==SwapchainEligibilityReason::synthetic_private_swapchain_failed &&
                local_detail==static_cast<std::uint64_t>(static_cast<std::int64_t>(XR_ERROR_LIMIT_REACHED)),
                "logged local synthetic-ring refusal not reproduced");
            left->generation_declined=true;left->generation_eligible_pending=false;
            left->generation_refusal=local_reason;left->generation_refusal_detail=local_detail;
            require(!s->generation_budget_exhausted,"local refusal unexpectedly became session-wide");
            hr(apply_embedded_control(s),"unchanged local refused revision");
            require(left->generation_declined,"local refusal retried without a new revision");
            private_budget_limit=8;desired.backend=3;desired.separate_synthesis=1;desired.lower_vram=1;
            require(xrfg_fixture::request_control(desired),"logged local refusal route change");
            hr(apply_embedded_control(s),"logged local refusal recovery");
            require(ensure_projection_frame_generation(s,projected) && left->frame_generation && right->frame_generation,
                "logged local refusal stayed latched after route change");
            // Genuine refusal still stops generation and retires every owned
            // output. Neither another EndFrame nor the same revision retries it.
            for(const auto& chain:{left,right}) {
                drain_swapchain_gpu(chain);destroy_frame_generation_swapchains(chain);
                chain->generation_eligible_pending=true;
            }
            private_budget_limit=3;
            require(!ensure_projection_frame_generation(s,projected) && s->generation_budget_exhausted,
                "runtime budget refusal was hidden");
            release_session_generation_budget(s);
            const unsigned refusals=private_budget_refusals, allocations=created;
            hr(apply_embedded_control(s),"same refused revision");
            require(ensure_projection_frame_generation(s,projected) && s->generation_budget_exhausted &&
                private_budget_refusals==refusals && created==allocations,"refused budget retried each frame");
            // Explicit new route is a bounded new transaction, after retirement.
            private_budget_limit=8;desired.backend=1;desired.separate_synthesis=0;desired.lower_vram=0;
            require(xrfg_fixture::request_control(desired),"refused budget new route");
            hr(apply_embedded_control(s),"refused budget control recovery");
            require(!s->generation_budget_exhausted,"new route permanently retained exhausted budget");
            require(ensure_projection_frame_generation(s,projected) && left->frame_generation && right->frame_generation,
                "explicit route failed to recover both projected eyes");
            require(!fixed->generation_eligible_pending && fixed->generation_declined,
                "budget retry reopened protected/static eligibility");
            require(!chains[4]->frame_generation && !chains[5]->frame_generation,
                "budget recovery allocated unused eye targets");
            // Exercise enable recovery too, without changing the provider tuple.
            for(const auto& chain:{left,right}) {
                drain_swapchain_gpu(chain);destroy_frame_generation_swapchains(chain);
                chain->generation_eligible_pending=true;
            }
            private_budget_limit=3;
            require(!ensure_projection_frame_generation(s,projected),"enable refusal setup");
            release_session_generation_budget(s);
            desired.enabled=false;require(xrfg_fixture::request_control(desired),"budget disable request");
            hr(apply_embedded_control(s),"budget disable");
            require(s->generation_budget_exhausted,"disable unexpectedly rearmed output budget");
            private_budget_limit=8;desired.enabled=true;
            require(xrfg_fixture::request_control(desired),"budget enable request");
            hr(apply_embedded_control(s),"budget explicit enable recovery");
            require(!s->generation_budget_exhausted && ensure_projection_frame_generation(s,projected) &&
                left->frame_generation && right->frame_generation,"enable did not recover both eyes");
            // Return to new for the unchanged eight-real/seven-generated GPU
            // pixel/ownership oracle below. No fresh captures are invented.
            desired.backend=2;require(xrfg_fixture::request_control(desired),"budget final new route");
            hr(apply_embedded_control(s),"budget final route");
            require(ensure_projection_frame_generation(s,projected),"budget final outputs");
            require(images.size()==8,"final New output budget changed");
            // The bounded phase above isolates projection-output capacity.
            // The existing oracle below allocates additional retained quad
            // outputs; those are a different budget consumer, not eye rings.
            private_budget_limit=0;
            for(const auto& chain:{left,right})for(const auto& image:chain->frame_generation->synthetic)
                synthetic_handles.insert(image.handle);
            std::cout<<"SteamVR budget: only projected eyes, old shapes retired before new, strict limit refusal, no per-frame retry, explicit route/enable recovery PASS\n";
        }
        if(mode=="guide-bypass-switch") {
            restore_route.previous=xrfg::embedded::snapshot().desired;
            for(unsigned n=1;n<=11;++n) {
                auto desired=*restore_route.previous;
                desired.enabled=true;desired.backend=1;desired.frame_generation=n<=8 ? (n%2?2:1) : 0;
                desired.motion_vectors=1;
                desired.lower_vram=0;
                require(xrfg_fixture::request_control(desired),"hot native/FSR switch without guides");
                ComPtr<ID3D12Resource> native_motion;
                if (n == 3 || n == 4) {
                    // R15 and strict FSR no longer trust guide injection after
                    // release without a producer publication. Exercise missing
                    // depth on BOTH routes with real publications before color.
                    hr(apply_embedded_control(s), "prepare missing-depth producer");
                    native_motion = resource(1, false, D3D12_HEAP_TYPE_DEFAULT, 0,
                        D3D12_RESOURCE_STATE_COMMON, DXGI_FORMAT_R16G16_FLOAT);
                }
                const auto publish_native_motion = [&](const auto& chain) {
                    if (n != 3 && n != 4) return;
                    commands([&](ID3D12GraphicsCommandList* list) {
                        xrfg::DlssMotionVectorPublication publication{};
                        publication.stream = handle_value(chain->handle);
                        publication.output = chain->enumerated_d3d12_images[0].Get();
                        publication.motion_vectors = native_motion.Get();
                        publication.producer_queue = s->d3d12_queue.Get();
                        publication.producer_command_list = list;
                        publication.output_width = publication.motion_width = 256;
                        publication.output_height = publication.motion_height = 128;
                        publication.resource_state = D3D12_RESOURCE_STATE_COMMON;
                        require(xrfg::publish_dlss_motion_vectors(publication) != nullptr,
                            "missing-depth producer publication");
                    });
                };
                publish_native_motion(left);
                fill(left->enumerated_d3d12_images[0].Get(),n,80);release(left,false);
                publish_native_motion(right);
                fill(right->enumerated_d3d12_images[0].Get(),n,160);release(right,false);
                fill(dynamic->enumerated_d3d12_images[0].Get(),n,60);release(dynamic,false);
                if(n==1){fill(fixed->enumerated_d3d12_images[0].Get(),55,120);release(fixed,false);}
                XrFrameState prediction{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&prediction)),"guide bypass wait");
                require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)),"guide bypass begin");
                std::array<XrCompositionLayerProjectionView,2> views{};
                for(unsigned eye=0;eye<2;++eye) {
                    auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                    v.pose.orientation.w=1;v.pose.position.y=float(n);v.fov={-.7F,.7F,.7F,-.7F};
                    v.subImage.swapchain=eye?right->handle:left->handle;v.subImage.imageRect.extent={256,128};
                }
                XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
                projection.space=reinterpret_cast<XrSpace>(0x100);projection.viewCount=2;projection.views=views.data();
                std::array<XrCompositionLayerQuad,2> quads{};
                for(unsigned i=0;i<2;++i) {
                    auto& q=quads[i];q={XR_TYPE_COMPOSITION_LAYER_QUAD};q.pose.orientation.w=1;q.pose.position.y=float(n);
                    q.subImage.swapchain=i?fixed->handle:dynamic->handle;q.subImage.imageArrayIndex=i;
                    q.subImage.imageRect.extent={256,128};
                }
                const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[0]),reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[1])};
                XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;
                end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=3;end.layers=layers;
                const auto started=std::chrono::steady_clock::now();
                require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end)),"missing guide switch must pass real frame");
                require(std::chrono::steady_clock::now()-started<std::chrono::seconds(2),"guide bypass blocked application");
                const auto runtime=xrfg::autonomous::snapshot().runtime;
                if(n<=8) {
                    require(runtime.generation_mode==desired.frame_generation && runtime.generation_state==
                        ((n==3 || n==4) ? xrfg::autonomous::GenerationState::missing_depth :
                            xrfg::autonomous::GenerationState::missing_motion),
                        "missing guides must publish the current route and precise bypass reason");
                    require(submissions.size()==n && submissions.back()==std::pair<unsigned,bool>{n,false},
                        "guide bypass lost/reordered/duplicated real stereo frame");
                }
                if (n == 3 || n == 4) for (const auto& chain : {left, right}) {
                    require(chain->release_publication && chain->last_released_motion_vectors &&
                            chain->last_released_motion_vectors->eyes[0]->publication <= chain->release_publication,
                        "missing-depth guide was not bound to the actual color release");
                    xrfg::retire_dlss_motion_vector_stream(handle_value(chain->handle));
                }
                if (n == 3 || n == 4) {
                    // The remaining cases intentionally have no observed
                    // upscaler inputs. Clear this new producer fixture's
                    // counters so they retain the original missing-motion
                    // assertion rather than an observed-output-mismatch.
                    xrfg::configure_dlss_motion_vector_tracking(false);
                    xrfg::configure_dlss_motion_vector_tracking(true);
                }
            }
            require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"fallback director drain");
            xrfg::autonomous::publish_runtime_status(s->native_dlss_generated_frames,s->generation_status.load());
            const auto fallback=xrfg::autonomous::snapshot().runtime;
            std::cout << "guide switch fallback: mode=" << fallback.generation_mode
                << " motion=" << fallback.motion_source << " state=" << static_cast<unsigned>(fallback.generation_state) << '\n';
            require(fallback.generation_mode==0 && fallback.motion_source==1 &&
                fallback.generation_state==xrfg::autonomous::GenerationState::missing_motion,
                "OFXR must report the actual optical-flow fallback when guides are missing");
            stop_continuous_presenter(s);
            for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
            require(correct && created==destroyed && submissions.size()==13 && waits==13 && begins==13 && ends==13,
                "guide bypass XR ownership/pixel/resource accounting");
            ComPtr<ID3D12InfoQueue> info;
            if(SUCCEEDED(s->d3d12_device.As(&info))) for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
                SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> bytes(size);
                auto* message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,message,&size);
                require(message->Severity!=D3D12_MESSAGE_SEVERITY_ERROR && message->Severity!=D3D12_MESSAGE_SEVERITY_CORRUPTION,
                    "guide bypass D3D12 debug error");
            }
            {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
            chains.clear();left.reset();right.reset();dynamic.reset();fixed.reset();session_state.reset();
            std::cout<<"Eight hot native DLSS-G/FSR switches without motion/depth: real stereo bypass; OFXR fallback recovers synthesis; precise diagnostics; no GPU errors\n";
            return 0;
        }
        if(mode=="provider-recovery") {
            wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
            auto& logger=xrfg::bridge_flight_logger();
            logger.initialize(std::filesystem::path(executable).parent_path());
            restore_route.previous=xrfg::embedded::snapshot().desired;
            std::array<ProjectionResourceMapping,2> projected{};
            projected[0].application_swapchain=left->handle;projected[1].application_swapchain=right->handle;
            for(unsigned attempt=0;attempt<2;++attempt) {
                // Shipping intentionally does not retain diagnostics while F9
                // is Off. Enable it before the failure we want to observe.
                auto diagnostic=xrfg::autonomous::snapshot().desired;diagnostic.diagnostics=true;
                require(xrfg::autonomous::request(diagnostic),"enable provider diagnostic");
                xrfg::autonomous::apply_runtime_settings();
                for(unsigned poll=0;poll<3;++poll) {
                    logger.refresh_settings();std::this_thread::sleep_for(std::chrono::milliseconds(270));
                }
                auto desired=*restore_route.previous;
                desired.enabled=true;desired.backend=2;desired.frame_generation=2;
                desired.motion_vectors=1;desired.lower_vram=1;desired.separate_synthesis=1;
                require(xrfg_fixture::request_control(desired),"request missing native provider");
                hr(apply_embedded_control(s),"missing provider control defers output admission");
                require(ensure_projection_frame_generation(s,projected),"provider failure mislabeled runtime swapchain budget");
                require(s->generation_initialization_result==DXGI_ERROR_UNSUPPORTED &&
                    !left->frame_generation && !right->frame_generation,"missing native provider must refuse projected initialization");
                require(left->generation_declined || right->generation_declined,"provider refusal not retained");
                xrfg::autonomous::publish_runtime_status(0,xrfg::autonomous::GenerationState::waiting,
                    s->generation_initialization_result.load());
                auto report=xrfg::autonomous::snapshot().runtime;
                require(report.generation_state==xrfg::autonomous::GenerationState::unavailable &&
                    std::string_view(report.message).find("0x887A0004")!=std::string_view::npos,
                    "native provider refusal lacks precise bypass diagnostics");
                const auto revision=xrfg::embedded::snapshot().revision;
                auto profile=xrfg::autonomous::snapshot().desired;
                profile.diagnostics=attempt==0;
                require(xrfg::autonomous::request(profile),"logging switch after provider refusal");
                xrfg::autonomous::apply_runtime_settings();
                require(xrfg::embedded::snapshot().revision==revision,"logging mutated generation settings");
                for(unsigned poll=0;poll<3;++poll) {
                    logger.refresh_settings();std::this_thread::sleep_for(std::chrono::milliseconds(270));
                }
                require(logger.enabled()==profile.diagnostics,"actual logger did not follow the live switch");
                if(profile.diagnostics) {
                    logger.flush();std::ifstream log(logger.log_path());
                    const std::string contents{std::istreambuf_iterator<char>(log),{}};
                    require(contents.find("op=swapchain_eligibility result=13")!=std::string::npos &&
                        contents.find("op=native_dlssg_provider")!=std::string::npos,
                        "enabled logging lost the actual native provider refusal");
                }
                desired.frame_generation=0;desired.motion_vectors=0;desired.lower_vram=0;desired.separate_synthesis=0;
                require(xrfg_fixture::request_control(desired),"return to optical flow after native refusal");
                hr(apply_embedded_control(s),"apply recovery after native refusal");
                require(ensure_projection_frame_generation(s,projected),"projected optical-flow recovery");
                for(const auto& chain:{left,right}) {
                    require(chain->frame_generation!=nullptr,"provider refusal poisoned optical-flow recovery");
                    require(!chain->generation_declined,"recovered generator remains declined");
                    for(const auto& image:chain->frame_generation->synthetic)synthetic_handles.insert(image.handle);
                }
                require(s->menu_enabled && !s->control_reconfigure_required,"recovered route must enable");
                require(SUCCEEDED(s->generation_initialization_result.load()),"recovered route retains an old error");
            }
            for(const auto reason:{SwapchainEligibilityReason::protected_content,
                SwapchainEligibilityReason::static_image,SwapchainEligibilityReason::current_private_swapchain_failed,
                SwapchainEligibilityReason::budget_exhausted,SwapchainEligibilityReason::synthesis_initialize_failed}) {
                const bool session_budget=reason==SwapchainEligibilityReason::synthesis_initialize_failed;
                dynamic->generation_declined=true;dynamic->generation_eligible_pending=true;
                dynamic->generation_refusal=reason;s->generation_budget_exhausted=session_budget;
                auto desired=xrfg::embedded::snapshot().desired;desired.backend=desired.backend==2?1:2;
                require(xrfg_fixture::request_control(desired),"switch with a permanent refusal");
                hr(apply_embedded_control(s),"apply with a permanent refusal");
                require(dynamic->generation_declined && !dynamic->frame_generation,
                    "provider recovery reopened a structural or budget refusal");
            }
            s->generation_budget_exhausted=false;
            dynamic->generation_declined=false;dynamic->generation_eligible_pending=false;
            dynamic->generation_refusal=SwapchainEligibilityReason::ready;
            auto recovered=xrfg::embedded::snapshot().desired;recovered.backend=2;
            require(xrfg_fixture::request_control(recovered),"restore new OFXR after refusal guard checks");
            hr(apply_embedded_control(s),"restore new OFXR for real frame accounting");
            std::cout<<"Missing native provider: two real initialization refusals, logging-only revisions, optical-flow resource recovery\n";
        }
        if(mode=="route-shapes" || mode=="paused-route-shapes") {
            restore_route.previous=xrfg::embedded::snapshot().desired;
            fill(left->enumerated_d3d12_images[0].Get(),42,80);
            release(left);
            const auto history=left->d3d12_history;
            const auto capture=left->last_released_capture;
            require(capture.has_value(),"owned capture before route change");
            std::array<ProjectionResourceMapping,2> projected{};
            projected[0].application_swapchain=left->handle;projected[1].application_swapchain=right->handle;
            // Shipping UI exposes Normal and the combined Lower VRAM control.
            for(const int lower:{0,1}) for(const int separate:{lower}) for(const int route:{1,3,2,1,3,2}) {
                auto desired=*restore_route.previous;
                desired.enabled=true;desired.backend=route;desired.frame_generation=0;desired.separate_synthesis=separate;desired.lower_vram=lower;
                if(mode=="paused-route-shapes") {
                    auto paused=xrfg::embedded::snapshot().desired;
                    paused.enabled=false;
                    require(xrfg_fixture::request_control(paused),"pause before GPU route change");
                    hr(apply_embedded_control(s),"apply pause before GPU route change");
                    paused=desired;paused.enabled=false;
                    require(xrfg_fixture::request_control(paused),"change GPU route while paused");
                    hr(apply_embedded_control(s),"apply GPU route while paused");
                    require(!s->menu_enabled,"paused route unexpectedly enabled generation");
                    for(const auto& chain:{left,right}) {
                        require(chain->frame_generation || chain->generation_eligible_pending,
                            "paused output replacement lost generation admission");
                        require(!chain->generation_declined,"paused route recorded a provider refusal");
                    }
                }
                require(xrfg_fixture::request_control(desired),"GPU route request");
                hr(apply_embedded_control(s),"apply GPU route request");
                require(ensure_projection_frame_generation(s,projected),"projected GPU route admission");
                require(!s->control_reconfigure_required && s->menu_enabled,"GPU route reconfiguration");
                require(left->d3d12_history==history && left->last_released_capture.has_value(),"route replaced source ownership");
                for(const auto& chain:{left,right}) {
                    const auto generation=chain->frame_generation;
                    require(generation && generation->staged_outputs==(separate!=0),"stale private output shape");
                    require(!generation->direct_extra_outputs,"stale direct 3X capacity");
                    require(generation->current_runtime_rings==(lower?1U:2U) &&
                        generation->synthetic_runtime_rings==(lower?1U:2U),"stale independent runtime ring allocation");
                    require(generation->staging_stride==1U,"stale staged 3X capacity");
                    for(const auto& image:generation->synthetic)synthetic_handles.insert(image.handle);
                }
            }
            auto final=*restore_route.previous;final.enabled=true;final.backend=2;final.frame_generation=0;
            final.separate_synthesis=0;final.lower_vram=0;
            require(xrfg_fixture::request_control(final),"restore full direct output after matrix switches");
            hr(apply_embedded_control(s),"restore full direct output after matrix switches");
            require(ensure_projection_frame_generation(s,projected),"final projected shape admission");
            for(const auto& chain:{left,right}) for(const auto& image:chain->frame_generation->synthetic)
                if(image.handle) synthetic_handles.insert(image.handle);
            // The frame path below must deliver eight real and seven generated
            // frames after the last resume, with pixel and XR-cycle accounting.
            // It must not inherit a stale temporal anchor from these switches.
            if(mode=="paused-route-shapes")
                require(!s->previous_projection,"resume retained old projection history");
        }
        for(const auto& chain:{left,right}) {
            if(lower_vram_test || direct_output_test)
                require(chain->frame_generation && chain->frame_generation->staged_outputs==lower_vram_test,
                    "explicit output storage was ignored");
        }
        ComPtr<ID3D12Resource> depth, motion;
        if(fsr_test && !fsr_missing) {
            depth=resource(1,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET,DXGI_FORMAT_R32_FLOAT);
            motion=resource(1,false,D3D12_HEAP_TYPE_DEFAULT,0,D3D12_RESOURCE_STATE_RENDER_TARGET,DXGI_FORMAT_R16G16_FLOAT);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=2;
            ComPtr<ID3D12DescriptorHeap> heap;hr(s->d3d12_device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"guide heap");
            auto d=heap->GetCPUDescriptorHandleForHeapStart();auto m=d;
            m.ptr+=s->d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            s->d3d12_device->CreateRenderTargetView(depth.Get(),nullptr,d);s->d3d12_device->CreateRenderTargetView(motion.Get(),nullptr,m);
            commands([&](auto* list){const float z[4]{.5F,.5F,.5F,1},v[4]{};
                list->ClearRenderTargetView(d,z,0,nullptr);list->ClearRenderTargetView(m,v,0,nullptr);
                barrier(list,depth.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                barrier(list,motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);});
        }
        s->last_inline_frame_state={XR_TYPE_FRAME_STATE};s->last_inline_frame_state.predictedDisplayPeriod=8333333;
        s->last_inline_frame_state.predictedDisplayTime=runtime_time;s->last_inline_frame_state.shouldRender=XR_TRUE;s->last_inline_frame_state_valid=true;
        if(fsr_test) require(!three_slot_generation_session(s),
            "FSR was admitted to the three-slot director");
        if(fsr_foveated) {
            // These fixtures count projection-guide startup, not the one-off
            // discovery of the independent panels added after V811. Arm the
            // actual panel resources before the first counted release, so the
            // strict eight-real/seven-synthetic and queue-owner oracles remain.
            // source() skips intercepted enumeration for auxiliary images;
            // supply its ordinary mutable-image history/admission here. The
            // static panel keeps its existing immutable-image passthrough.
            dynamic->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
            ID3D12Resource* panel_images[]={dynamic->enumerated_d3d12_images[0].Get()};
            hr(dynamic->d3d12_history->initialize(s->d3d12_device.Get(),s->d3d12_queue.Get(),
                panel_images,D3D12_RESOURCE_STATE_RENDER_TARGET),"FSR fixture panel history");
            dynamic->generation_eligible_pending=true;
            const std::array<PanelSource,2> panels{{{dynamic->handle,0},{fixed->handle,1}}};
            arm_panel_generation(s,panels);
            require(!panel_generation_pending(panels),"FSR fixture panel arming");
            require(dynamic->panel_source && dynamic->frame_generation && dynamic->d3d12_history &&
                fixed->panel_source && !fixed->frame_generation,"FSR fixture panel resources");
            require(waits==0 && begins==0 && ends==0 && submissions.empty(),
                "FSR fixture panel arming opened a runtime frame");
        }
        if(mode!="steamvr-djules-restart" && !promotion_test && !fsr_test && !legacy_queue && !triple_inline && !transport_inline && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia) {
            require(start_continuous_presenter(s,nullptr,false),"promotion");
            if(s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia)
                require(s->three_slot_director && s->synthesis_thread.joinable(),"native did not enter three lanes");
            else require(!s->three_slot_director && !s->synthesis_thread.joinable(),"FidelityFX entered NVIDIA director");
        }
        const unsigned real_count=fsr_stereo_guard?13:8;
        std::array<xrfg::D3D12HistoryCaptureTicket,4> held_left{}, held_right{};
        std::optional<XrFrameState> guard_next;
        if(fsr_stereo_guard && fsr_queue) {
            s->pipelined_presenter_mode=true;
            require(start_continuous_presenter(s,nullptr,true),"guard initial virtual owner");
        }
        for(unsigned n=1;n<=real_count;++n) {
            if(triple_switch && (n==5 || n==7)) {
                require(XR_SUCCEEDED(switch_djules_pipeline(s,n==7,false)),"live GPU multiplier switch");
                for(const auto& chain:{left,right}) {
                    const auto g=chain->frame_generation;
                    require(g && g->synthetic_runtime_rings==(s->lower_vram?1U:2U),"live 3X change reserved extra XR handles");
                    if(g->staged_outputs) require(g->staging_stride==(n==7?2U:1U),"live 2X did not free extra owned textures");
                    for(const auto& image:g->synthetic) if(image.handle) synthetic_handles.insert(image.handle);
                }
            }
            if(deep_switch && (n==5 || n==7)) {
                if(n==5) require(policy_gate->GetCompletedValue()==0,"policy fixture did not retain pending GPU work");
                require(XR_SUCCEEDED(switch_djules_pipeline(s,false,n==7)),"live GPU deep preference switch");
                require(s->djules_deep_pipeline==(n==7),"live preference not applied");
                if(n==5) require(policy_gate->GetCompletedValue()==1,"policy crossed unfinished GPU work");
            }
            require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"worker rendezvous");
            const bool busy_frame=capture_busy_test && (n==4 || n==6);
            xrfg::D3D12HistoryConsumerLease left_lease{}, right_lease{};
            if(busy_frame) {
                ComPtr<ID3D12Resource> leased_image;
                ComPtr<ID3D12Fence> leased_fence;
                hr(left->d3d12_history->acquire_consumer(held_left[n==4?1:2],&left_lease,
                    leased_image.ReleaseAndGetAddressOf(),leased_fence.ReleaseAndGetAddressOf()),"hold next left history slot");
                if(n==6) hr(right->d3d12_history->acquire_consumer(held_right[3],&right_lease,
                    leased_image.ReleaseAndGetAddressOf(),leased_fence.ReleaseAndGetAddressOf()),"hold next right history slot");
            }
            if(midpoint_cadence && n>2)
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            // RE Village: releases need not be one-to-one with EndFrames.
            // Exercise left-only, right-only, then both-eye double captures.
            if(gap_test && (n==4 || n==6 || n==7)) {
                auto extra=n==6 ? right : left;
                fill(extra->enumerated_d3d12_images[0].Get(),230,200,array);release(extra);
                if(n==7 && !array) {fill(right->enumerated_d3d12_images[0].Get(),230,200);release(right);}
            }
            if(!alternating_eyes || n==1 || n%2==0){fill(left->enumerated_d3d12_images[0].Get(),n,80,array);release(left,!busy_frame);}
            if(!array && (!alternating_eyes || n==1 || n%2==1)){fill(right->enumerated_d3d12_images[0].Get(),n,160);release(right,!busy_frame);}
            if(reference_capture_overlap_test && n==4) {
                require(fsr_gpu_gate->GetCompletedValue()==0,"next Reference capture globally waited for preceding pair");
                require(left->last_released_capture && right->last_released_capture,
                    "overlapping Reference capture missing");
                hr(fsr_gpu_gate->Signal(1),"release preceding pair after next capture");
            }
            if(capture_busy_test && n<=3) {
                held_left[n]=*left->last_released_capture; held_right[n]=*right->last_released_capture;
            }
            if(busy_frame) {
                require(!left->last_released_capture,"busy left capture was not refused");
                require(n==6 ? !right->last_released_capture : right->last_released_capture.has_value(),
                    "one-eye / two-eye capture refusal shape");
                left->d3d12_history->cancel_consumer(left_lease);
                if(n==6) right->d3d12_history->cancel_consumer(right_lease);
            }
            if(fsr_test && !fsr_missing) {
                auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
                for(unsigned eye=0;eye<2;++eye) {
                    auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=eye+1;g->epoch=1;g->serial=n;g->previous_serial=n-1;g->publication=2*n+eye;
                    g->producer_queue=s->d3d12_queue;g->output=left->enumerated_d3d12_images[0];g->output_slice=fsr_packed?0:eye;
                    g->motion_vectors=motion;g->depth=depth;g->output_width=g->depth_width=g->motion_width=256;
                    g->output_height=g->depth_height=g->motion_height=128;g->frame_time_delta_ms=16.6667F;
                    if(fsr_foveated) {
                        g->motion_width=g->depth_width=64;
                        g->motion_height=g->depth_height=32;
                        g->placed=true;g->placed_motion_rect={0,0,64,32};
                    }
                    if(fsr_packed) {
                        g->output_width=g->depth_width=g->motion_width=128;
                        g->output_x=g->depth_x=g->motion_x=eye*128;
                    }
                    guides->eyes[eye]=std::move(g);
                }
                if(fsr_stereo_guard && (n==3 || n==4)) guides->eyes[1]=guides->eyes[0];
                if(fsr_stereo_guard && n==6) {
                    auto missing=std::make_shared<xrfg::DlssMotionVectorFrame>(*guides->eyes[1]);
                    missing->motion_vectors.Reset();guides->eyes[1]=std::move(missing);
                }
                std::scoped_lock lock(left->mutex);left->last_released_motion_vectors=std::move(guides);
            }
            // Legacy's first prime is inline with borrowed auxiliary images;
            // poisoning those before its synchronous End violates that input
            // contract. Once queued, poison the dynamic HUD to test ownership.
            // Inline submissions borrow the application's released auxiliary
            // image until synchronous End; only asynchronous consumers require
            // an independently retained release capture.
            fill(dynamic->enumerated_d3d12_images[0].Get(),fsr_foveated?n*16:n,60);release(dynamic,mode!="steamvr-djules-restart" && !busy_frame && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia && !triple_inline && !transport_inline && (!legacy_queue || n>1));
            if(fsr_foveated) require(dynamic->last_released_capture.has_value(),"FSR fixture panel release capture");
            if(n==1){fill(fixed->enumerated_d3d12_images[0].Get(),55,120);release(fixed,mode!="steamvr-djules-restart" && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::nvidia && !triple_inline && !transport_inline && !legacy_queue);}
            if(busy_frame) fill(fixed->enumerated_d3d12_images[0].Get(),55,120);
            XrFrameState prediction{XR_TYPE_FRAME_STATE};
            if(guard_next) {prediction=*guard_next;guard_next.reset();}
            else require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&prediction)),"producer wait");
            // Fidelity's inline/pipelined promotion has its own advertised
            // period. The MSFS regression checks source continuity/output
            // pixels, not NVIDIA's route-specific virtual Wait contract.
            if(!fsr_stereo_guard && !(msfs_time_test && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::fidelity_fx))
                require(prediction.predictedDisplayPeriod==(triple_switch && (n==5 || n==6)?8333333 : (triple_core||triple_inline)?24999999 : legacy_queue && n>1 ? 16666666 : 8333333),"advertised period");
            require(prediction.predictedDisplayPeriod>0,"nonpositive advertised period");
            require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)),"producer begin");
            if((shared_readiness || shared_progress || publication_test) && n==1)s->pipelined_presenter_mode=true;
            if((reference_readiness_test || reference_capture_overlap_test) && n==3) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"Reference readiness drain");
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fsr_gpu_gate)),"Reference GPU gate");
                hr(s->d3d12_synthesis_queue->Wait(fsr_gpu_gate.Get(),1),"hold Reference synthesis output");
                fsr_gpu_watchdog=std::jthread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    fsr_gpu_gate->Signal(1);
                });
            }
            if(fsr_queue_gpu_pending && n==3) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "pending GPU test drain");
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fsr_gpu_gate)), "FSR pending GPU gate");
                hr(s->d3d12_queue->Wait(fsr_gpu_gate.Get(),1), "hold application GPU work");
                fsr_gpu_watchdog=std::jthread([] {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    fsr_gpu_gate->Signal(1); // Unblock a regressed CPU wait instead of hanging the test.
                });
            }
            std::array<XrCompositionLayerProjectionView,2> views;
            for(unsigned eye=0;eye<2;++eye) {
                auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;v.pose.position.y=float(n);
                v.pose.position.x=eye?.032F:-.032F;v.fov={-.7F,.7F,.7F,-.7F};
                v.subImage.swapchain=eye?right->handle:left->handle;v.subImage.imageArrayIndex=array?eye:0;v.subImage.imageRect.extent={256,128};
                if(fsr_packed) {
                    v.subImage.imageArrayIndex=0;
                    v.subImage.imageRect.offset.x=eye*128;
                    v.subImage.imageRect.extent.width=128;
                }
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(0x100);
            projection.viewCount=2;projection.views=views.data();
            if(fsr_stereo_guard && n==10) projection.viewCount=1;
            std::array<XrCompositionLayerQuad,2> quads;
            for(unsigned i=0;i<2;++i){auto& v=quads[i];v={XR_TYPE_COMPOSITION_LAYER_QUAD};v.pose.orientation.w=1;v.pose.position.y=float(fsr_foveated && i==0?n*16:n);
                v.subImage.swapchain=i?fixed->handle:dynamic->handle;v.subImage.imageArrayIndex=i;v.subImage.imageRect.extent={256,128};}
            const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection),
                reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[0]),reinterpret_cast<XrCompositionLayerBaseHeader*>(&quads[1])};
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            // The new Fidelity fixture isolates projection continuity through
            // its inline->pipelined promotion. Retained quad ownership remains
            // covered by the existing NVIDIA/FSR transport fixtures.
            const bool msfs_fidelity=msfs_time_test && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::fidelity_fx;
            end.layerCount=(promotion_test && !s->three_slot_director) || msfs_fidelity ? 1 : 3;end.layers=layers;
            if(msfs_time_test && !mode.starts_with("msfs-control-") && n%3==0)
                end.displayTime-=4*8333333; // fresh captures, backward app prediction as in MSFS
            if(msfs_time_test && !mode.starts_with("msfs-control-") && n>1 && n%3==1) end.displayTime=msfs_previous_time;
            msfs_previous_time=end.displayTime;

            if(fsr_stereo_guard && n==1) {
                ProjectionSnapshot audit;
                require(capture_projection_snapshot(&end,&audit),"guard audit projection");
                const auto mapped=build_projection_resource_mappings(audit);
                require(mapped.ready(),"guard audit mappings");
                const auto original=left->last_released_motion_vectors;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected independent stereo guides");
                auto shared=std::make_shared<xrfg::DlssMotionVectorSet>(*original);
                shared->eyes[1]=shared->eyes[0];
                left->last_released_motion_vectors=shared;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::shared_eye_domain,
                    "guard admitted duplicated mono guides");
                auto a=std::make_shared<xrfg::DlssMotionVectorFrame>(*shared->eyes[0]);
                auto b=std::make_shared<xrfg::DlssMotionVectorFrame>(*a);
                a->motion_width=b->motion_width=128;b->motion_x=128;
                shared->eyes={a,b};
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected non-overlapping packed stereo motion");
                b->motion_x=64;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::shared_eye_domain,
                    "guard admitted overlapping shared motion domain");
                b->motion_x=0;b->motion_width=256;b->motion_slice=1;
                a->motion_vectors=b->motion_vectors=resource(2,false,D3D12_HEAP_TYPE_DEFAULT,
                    0,D3D12_RESOURCE_STATE_COMMON,DXGI_FORMAT_R16G16_FLOAT);
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::ready,
                    "guard rejected distinct array stereo motion slices");
                b->motion_slice=2;
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::invalid_guide_region,
                    "guard admitted out-of-bounds motion slice");
                b->motion_slice=1;
                b->depth.Reset();
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::missing_depth,
                    "guard admitted missing depth");
                audit.layers[0].views.resize(1);
                require(classify_fsr_stereo_guides(audit,mapped.mappings)==FsrStereoGuideReason::non_stereo_projection,
                    "guard admitted mono projection");
                left->last_released_motion_vectors=original;
            }
            const unsigned independence_frame=(mode=="steamvr-djules-restart" || fsr_test || legacy_queue || triple_inline || transport_inline || s->optical_flow_backend!=xrfg::D3D12OpticalFlowBackend::nvidia) ? 0 : promotion_test ? 5 : 2;
            if(deep_switch && n==4) {
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&policy_gate)),"policy GPU gate");
                hr(s->d3d12_synthesis_queue->Wait(policy_gate.Get(),1),"hold policy GPU work");
                policy_unblock=std::jthread([gate=policy_gate] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    gate->Signal(1);
                });
            }
            if(n==independence_frame){hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked_synthesis)),"test gate");
                hr(s->d3d12_synthesis_queue->Wait(blocked_synthesis.Get(),1),"block synthesis");}
            if(fsr_stereo_guard && fsr_queue && n<real_count) {
                XrFrameState next{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&next)),"guard outstanding virtual Wait");
                guard_next=next;
            }
            const auto fsr_used_before=xrfg::fsr_frame_generation_statistics().used;
            const auto ended=layer_end_frame_impl(s->handle,&end);
            if(XR_FAILED(ended))std::cerr<<"actual EndFrame result="<<ended<<" frame="<<n<<'\n';
            require(XR_SUCCEEDED(ended),"actual EndFrame");
            if(msfs_time_test && n>=3) {
                require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"MSFS synthesis completion");
                if(s->generation_status!=xrfg::autonomous::GenerationState::ready) {
                    std::cerr<<"MSFS frame="<<n<<" status="<<static_cast<unsigned>(s->generation_status.load())
                        <<" pending="<<s->pending_frames.size()<<" captures="
                        <<(s->previous_projection?s->previous_projection->capture_order.size():0)<<'\n';
                }
                require(s->generation_status==xrfg::autonomous::GenerationState::ready,
                    "fresh ordered captures re-primed only because MSFS displayTime went backward");
            }

            if(reference_capture_overlap_test && n==3)
                require(fsr_gpu_gate->GetCompletedValue()==0,"overlap Reference producer blocked on output");
            if(publication_test && n==3) {
                ComPtr<ID3D12Fence> marker;
                hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&marker)),"publication binding marker");
                hr(runtime_queue(*s)->Signal(marker.Get(),1),"marker before consumer publication");
                const auto independent=wait_composition_fence(marker.Get(),1,100);
                hr(fsr_gpu_gate->Signal(1),"release publication output");
                require(SUCCEEDED(independent),"unpublished generation blocked the XR binding queue before Begin");
            }
            if(reference_readiness_test && !publication_test && n==3) {
                require(fsr_gpu_gate->GetCompletedValue()==0,"Reference GPU wait blocked producer End");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                require(!reference_wait_before_gpu_complete,"Reference opened XR cycle before exact GPU output was ready");
                hr(fsr_gpu_gate->Signal(1),"release Reference output after producer returned");
            }
            if(mode=="steamvr-djules-restart" && n==1)
                require(s->generation_status==xrfg::autonomous::GenerationState::recovering &&
                    submissions.size()==1 && submissions[0]==std::pair<unsigned,bool>{1,false},
                    "stopped SteamVR presenter prevented staged bootstrap prime");
            if (fsr_test && !fsr_queue) fsr_receipt_trace.push_back(
                "frame=" + std::to_string(n) + " capture=" + std::to_string(left->last_released_capture.has_value()) +
                " generation=" + std::to_string(static_cast<unsigned>(s->generation_status.load())) +
                " outputs=" + std::to_string(submissions.size()));
            if(busy_frame) {
                require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"capture refusal failed worker rendezvous");
                require(XR_SUCCEEDED(s->presenter_failure),"capture backpressure killed presenter");
                require(!s->previous_projection,"missing capture retained stereo temporal anchor");
                require(submissions.back()==std::pair<unsigned,bool>{n,false},"busy frame not delivered as real");
            }
            if(fsr_stereo_guard) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"guard boundary drain");
                const bool generated=n==2 || n==9 || n==13;
                require(xrfg::fsr_frame_generation_statistics().used==fsr_used_before+(generated?1:0),
                    "guard dispatched with invalid inputs or failed to resume");
                const bool paused=(n>=3 && n<=7) || n==10 || n==11;
                require(s->fsr_stereo_admission.suspended==paused,"guard suspension/recovery state");
                if(paused) {
                    require(!s->previous_projection,"guard retained pre-menu projection anchor");
                    const auto status=xrfg::fsr_frame_generation_statistics().status;
                    require(status==xrfg::FsrFrameGenerationStatus::stereo_unavailable ||
                        status==xrfg::FsrFrameGenerationStatus::stereo_recovering,"guard status lost");
                }
            }
            if(legacy_queue) {
                require(continuous_presenter_active(s) && s->queued_nvidia_presenter &&
                    !s->three_slot_director && !s->synthesis_thread.joinable(),
                    "legacy NVIDIA did not use fresh-output consumer");
                {
                    std::scoped_lock lock(s->presenter_mutex);
                    require(s->outstanding_presenter_submissions<=(steamvr_counted_native_release(*s)?4U:2U), "legacy backlog exceeds bounded pairs");
                }
                if(n==4) {
                    require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "legacy starvation drain");
                    const auto count=ends.load(), wait_count=waits.load();
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    require(ends==count && waits==wait_count,
                        "legacy consumer repeated or opened XR cycles while starved");
                }
            }
            if(fsr_test) {
                require(!s->presenter_start_requested,
                    "FSR was queued for automatic presenter promotion");
                require(!s->three_slot_director && !s->synthesis_thread.joinable(), "FSR used NVIDIA worker");
                require(continuous_presenter_active(s) == (fsr_queue && !fsr_missing), "FSR runtime route");
                if(fsr_queue && !fsr_missing) {
                    std::scoped_lock lock(s->presenter_mutex);
                    require(s->outstanding_presenter_submissions <= (steamvr_counted_native_release(*s)?4U:2U), "FSR output backlog exceeds bounded pairs");
                }
            }
            if(fsr_queue_switch && n==8) {
                require(XR_SUCCEEDED(wait_for_presenter_idle(s)), "FSR switch drain");
                s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
                s->pipelined_presenter_mode=true; // Also test real-app overlapping waits.
                XrFrameState switch_prediction{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&switch_prediction)), "switch Wait");
                require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)), "switch Begin");
                end.displayTime=switch_prediction.predictedDisplayTime;
                require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end)), "switch End");
                require(continuous_presenter_active(s) && !s->fsr_two_slot_presenter && s->queued_nvidia_presenter,
                    "selected NVIDIA owner did not replace the FSR consumer");
                // The former fixture stopped after End, before the failing Begin.
                XrFrameState following{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,nullptr,&following)), "following Wait");
                require(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,nullptr)), "following Begin");
                end.displayTime=following.predictedDisplayTime;
                // Empty composition checks the live frame call chain without
                // adding another GPU-generated output to this pixel fixture.
                auto empty=end; empty.layerCount=0; empty.layers=nullptr;
                require(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&empty)), "following End");
            }
            if(gap_test && (n==4 || n==6 || n==7)) {
                require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"gap prime completed");
                require(!left->frame_generation->synthesizer->has_capture_gap(*left->last_released_capture) &&
                    !right->frame_generation->synthesizer->has_capture_gap(*right->last_released_capture),"both histories not re-anchored");
                require(s->three_slot_director && XR_SUCCEEDED(s->presenter_failure),"gap stopped director");
            }
            if(promotion_test && n==3) {
                require(s->three_slot_director && s->synthesis_thread.joinable(),"EndFrame did not promote");
                require(promotion_gate_count==1,"deferred-copy promotion not exercised");
                require(promotion_gate_released,"director started before delayed endpoint completed");
                require(submissions.size()==5 && submissions.back()==std::pair<unsigned,bool>{3,false},"promotion skipped current endpoint");
            }
            if(n==independence_frame) {
                // A blocked private GPU queue must not enqueue its wait on the
                // application's GPU queue. A marker on that queue still completes.
                ComPtr<ID3D12Fence> marker;hr(s->d3d12_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&marker)),"marker");
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                hr(s->d3d12_queue->Signal(marker.Get(),1),"independent marker");
                HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                hr(marker->SetEventOnCompletion(1,event),"marker event");
                const DWORD status=WaitForSingleObject(event,2000);CloseHandle(event);
                hr(blocked_synthesis->Signal(1),"unblock synthesis");
                require(status==WAIT_OBJECT_0,"application GPU queue blocked by synthesis");
            }
        }
        require(XR_SUCCEEDED(wait_for_synthesis_handoff(s)),"final worker");
        if(!fsr_queue_switch) require(XR_SUCCEEDED(wait_for_presenter_idle(s)),"director drain");
        stop_continuous_presenter(s);
        unsigned repeats=0;
        if(deep_readiness) {
            std::vector<std::pair<unsigned,bool>> fresh;
            for(const auto& receipt:submissions) {
                if(!fresh.empty() && receipt==fresh.back()) {
                    require(!receipt.second,"deep repeat reused synthetic rather than owned prior real"); ++repeats;
                } else fresh.push_back(receipt);
            }
            require(direct_output_test ? repeats==0 : repeats>0,
                "deep GPU readiness must respect the selected output storage");
            require(submissions.size()==fresh.size()+repeats,"deep physical output accounting");
            submissions=std::move(fresh);
        }
        const unsigned expected_count=capture_busy_test?11:fsr_stereo_guard?16:deep_switch?13:triple_switch?17:(triple_core||triple_inline)?22:fsr_queue_switch ? 16 : fsr_missing ? 8 : gap_test ? 12 : (promotion_test || mode=="steamvr-djules-restart") ? 14 : 15;
        const unsigned expected_cycles = expected_count + repeats + empty_submissions;
        if (submissions.size()!=expected_count || waits!=expected_cycles || begins!=expected_cycles || ends!=expected_cycles) {
            const auto fsr=xrfg::fsr_frame_generation_statistics();
            std::cerr << "cycle receipt actual=" << submissions.size() << " expected=" << expected_count
                      << " waits/begins/ends=" << waits << '/' << begins << '/' << ends
                      << " FSR attempts/used/failures=" << fsr.attempts << '/' << fsr.used << '/' << fsr.failures
                      << " status=" << static_cast<unsigned>(fsr.status) << '\n';
            for (const auto& receipt : fsr_receipt_trace) std::cerr << receipt << '\n';
        }
        require(submissions.size()==expected_count && waits==expected_cycles && begins==expected_cycles && ends==expected_cycles,"unique output/cycle count");
        const bool msfs_fidelity=msfs_time_test && s->optical_flow_backend==xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        require(empty_submissions == (fsr_queue_switch || msfs_fidelity ? 1U : 0U), "unexpected empty physical submission");
        if(capture_busy_test) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n==2 || n==3 || n==8) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"capture refusal lost/reordered stereo real frames or failed to re-prime");
        } else if(fsr_stereo_guard) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=real_count;++n) {
                if(n==2 || n==9 || n==13) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"guard lost/repeated/reordered real or synthetic outputs");
            require(!guard_runtime_waited && !guard_runtime_begun,"guard left an open physical cycle");
        } else if(fsr_missing) {
            for(unsigned i=0;i<8;++i) require(submissions[i]==std::pair<unsigned,bool>{i+1,false},"FSR missing guide repeated/lost real");
        } else if(gap_test) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=4 && n!=6 && n!=7) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"gap must skip only synthetic, keep every real, resume pairing");
        } else if(promotion_test || mode=="steamvr-djules-restart") {
            const std::array<unsigned,8> real_positions={0,2,4,5,7,9,11,13};
            unsigned next_real=0;
            for(unsigned i=0;i<submissions.size();++i) {
                if(next_real<real_positions.size() && i==real_positions[next_real]) {
                    require(submissions[i]==std::pair<unsigned,bool>{next_real+1,false},"promotion real endpoint order");
                    ++next_real;
                } else require(submissions[i].second,"promotion synthetic order");
            }
        } else if(deep_switch) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=5 && n!=7) expected.emplace_back(n,true);
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"deep toggle lost or reordered owned output");
        } else if(triple_switch) {
            std::vector<std::pair<unsigned,bool>> expected;
            for(unsigned n=1;n<=8;++n) {
                if(n>1 && n!=5 && n!=7) {
                    expected.emplace_back(n,true);
                    if(n!=6) expected.emplace_back(n,true);
                }
                expected.emplace_back(n,false);
            }
            require(submissions==expected,"live shape lost, repeated or reordered an owned output");
        } else if(triple_core||triple_inline) {
            for(unsigned i=0;i<7;++i) {
                require(submissions[3*i]==std::pair<unsigned,bool>{i+1,false},"3X real order");
                require(submissions[3*i+1]==std::pair<unsigned,bool>{i+2,true} &&
                    submissions[3*i+2]==std::pair<unsigned,bool>{i+2,true},"3X independent synthetic deliveries");
            }
        } else for(unsigned i=0;i<7;++i){require(submissions[2*i]==std::pair<unsigned,bool>{i+1,false},"real order");require(submissions[2*i+1].second,"synthetic order");}
        require(submissions.back()==std::pair<unsigned,bool>{real_count,false},"last real");require(correct,"stale/cross-eye/auxiliary pixels");
        if(midpoint_cadence) {
            require(output_times.size()==submissions.size(), "midpoint runtime receipts");
            for(unsigned i=1;i<submissions.size();++i)
                if(submissions[i-1].second && !submissions[i].second)
#ifdef OFXR_TEST_DJULES_ROUTE
                    // This fixture's native runtime is neither SteamVR nor VD:
                    // final D26 uses its wait plus a half-period floor (8.33/2).
                    require(output_times[i]-output_times[i-1]>=std::chrono::milliseconds(4),
                        "djules non-SteamVR handover violated the half-period floor");
#else
                    require(output_times[i]-output_times[i-1]>=std::chrono::milliseconds(6),
                        "real endpoint stole a delayed midpoint interval");
#endif
        }
        if(fsr_queue_gpu_pending) require(fsr_wait_before_gpu_complete,
            "FSR consumer CPU-waited before entering XR with pending GPU work");
        if(fsr_queue || legacy_queue) {
            require(waits == expected_cycles && output_times.size() == expected_count, "FSR repeated or lost output");
            require(output_display_times.size()==expected_count, "missing runtime output slot receipt");
            for(unsigned i=1;i<output_times.size();++i) {
                require(output_display_times[i]-output_display_times[i-1]==8333333,
                    "output skipped or reused a physical runtime slot");
                // FSR reanchors after a long End. Reference NVIDIA deliberately
                // keeps its grid through normal wake/End lateness; the fake
                // runtime's synchronous pixel readbacks must not impose an
                // unrelated minimum wall-time gap. Its exact lateness/starvation
                // contract is checked deterministically by TwoOutputSchedule.
                if(fsr_queue)
                    require(output_times[i]-output_times[i-1] >= std::chrono::milliseconds(6), "FSR bunched output slots");
            }
        }
        if(fsr_clock_probe) {
            auto& logger=xrfg::bridge_flight_logger();logger.flush();
            std::ifstream in(logger.log_path());const std::string trace{std::istreambuf_iterator<char>(in),{}};
            require(trace.find("op=presenter_clock_anchor result=5572 ")!=std::string::npos,
                "actual FSR consumer bypassed measured phase clock");
        }
        if(fsr_test) {const auto stats=xrfg::fsr_frame_generation_statistics();
            require(fsr_missing ? stats.attempts==0 : stats.used==(fsr_stereo_guard?3:7) && stats.failures==0,"FSR dispatch count/failed");}
        if(publication_test) require(publication_releases>0,"no consumer publication was exercised");
        if(reference_readiness_test)
            require(!reference_wait_before_gpu_complete,"Reference opened XR cycle before exact GPU output was ready");
        if(mode=="steamvr-output-retire" || mode=="resource-cleanup") {
            require(dynamic->composition_capture && fixed->composition_capture,"quad budget fixture missing");
            const auto live_handles=images.size();
            auto held=dynamic->composition_capture->outputs[0];
            require(held && held->image.handle,"quad lease fixture missing");
            const auto held_handle=held->image.handle;
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            const auto* only=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo current{XR_TYPE_FRAME_END_INFO};current.layerCount=1;current.layers=&only;
            retire_unused_composition_outputs(s,&current);
            require(images.size()<live_handles && images.contains(held_handle),"leased quad retired or idle quad budget retained");
            held.reset();retire_unused_composition_outputs(s,&current);
            for(const auto& chain:{dynamic,fixed}) {
                require(chain->composition_capture->valid,"quad source capture lost after output retirement");
                for(const auto& slot:chain->composition_capture->outputs)require(!slot,"idle quad XR output retained");
            }
            GeneratedFrameEndInfo returned;
            returned.composition_layers.push_back(XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD});
            auto& quad=std::get<XrCompositionLayerQuad>(returned.composition_layers[0]);
            quad.subImage.swapchain=dynamic->handle;
            require(XR_SUCCEEDED(own_composition_images(s,returned)) && !returned.composition_images.empty(),
                "returning quad could not use its preserved source capture");
            returned={};retire_unused_composition_outputs(s,&current);
            for(bool steam:{true,false}) for(auto generator:{xrfg::D3D12FrameGenerationMode::ofxr,
                xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
                s->dispatch->steamvr_runtime=steam;s->frame_generation_mode=generator;
                GeneratedFrameEndInfo packet;
                auto quad=XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.subImage.swapchain=dynamic->handle;
                packet.composition_layers.push_back(quad);
                require(XR_SUCCEEDED(own_composition_images(s,packet)),"all-route auxiliary recreation");
                const auto before=images.size();
                retire_unused_composition_outputs(s,&current);
                require(images.size()==before,"cleanup destroyed a queued auxiliary output");
                packet={};
                for(const auto& slot:dynamic->composition_capture->outputs)if(slot)
                    hr(wait_composition_fence(slot->fence12.Get(),slot->ready_value),"auxiliary output copy completion");
                retire_unused_composition_outputs(s,&current);
                require(images.size()<before,"unused auxiliary output retained outside optical SteamVR");
            }
        }
        for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        require(created==destroyed,"private XR swapchain leak");
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(s->d3d12_device.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);
            std::vector<unsigned char> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
            if(m->Severity==D3D12_MESSAGE_SEVERITY_ERROR || m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION){std::cerr<<m->pDescription<<'\n';correct=false;}}
        require(correct,"D3D12 debug errors");
        if(promotion_unblock.joinable())promotion_unblock.join();
        promotion_gate.Reset();
        if(fsr_gpu_watchdog.joinable())fsr_gpu_watchdog.join();
        fsr_gpu_gate.Reset();
        std::cout<<"Native D3D12 "<<(array?"array":"split")<<": 8 real/"<<(expected_count-8)<<" synthetic; FSR="<<fsr_test<<"; delayed promotion="<<promotion_test<<"; private poisoned-source eyes + static/dynamic quads; independent GPU queue; no debug errors\n";
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
        chains.clear();left.reset();right.reset();dynamic.reset();fixed.reset();
        // Release test-owned session/device before process-static layer globals.
        // Keeping it global past their teardown invalidates driver lifetimes.
        session_state.reset();
        capture.verify(false, true);
        return 0;
    } catch(const std::exception& e) {
        std::filesystem::path trace; std::wstring error;
        xrfg::save_ram_timing(GetCurrentProcessId(),std::filesystem::current_path(),&trace,&error);
        if(promotion_gate)promotion_gate->Signal(1);
        if(promotion_unblock.joinable())promotion_unblock.join();
        if(blocked_synthesis)blocked_synthesis->Signal(1);
        if(fsr_gpu_gate)fsr_gpu_gate->Signal(1);
        if(fsr_gpu_watchdog.joinable())fsr_gpu_watchdog.join();
        if(session_state)stop_continuous_presenter(session_state);
        for(auto& c:chains){drain_swapchain_gpu(c);destroy_frame_generation_swapchains(c);destroy_composition_capture(c);}
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();g_sessions.clear();}
        chains.clear();images.clear();promotion_gate.Reset();blocked_synthesis.Reset();fsr_gpu_gate.Reset();session_state.reset();
        std::cerr<<e.what()<<'\n';return 1;
    }
}
