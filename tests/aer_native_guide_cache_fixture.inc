#include "xrfg/luke_origin_capture_policy.hpp"

int aer_native_guide_cache_test() {
    xrfg::embedded::Settings generation;
    xrfg::embedded::AfwSettings reconstruction;
    reconstruction.mode=0;generation.enabled=false;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"all-off producer records no protected colour markers");
    generation.enabled=true;generation.ofr=true;
    for(int method:{0,1,2})for(bool hybrid:{false,true}) {
        generation.frame_generation=method;generation.hybrid=hybrid;
        check(xrfg::luke_origin_capture_required(generation,reconstruction)==(method==1 || method==2 || hybrid),
            "native AER methods produce CRC origins with AFW explicitly off");
    }
    reconstruction.mode=3;generation.frame_generation=0;generation.hybrid=false;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"raw OFXR AER does not mark because of bypassed AFW preference");
    generation.rife_model=3;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"RIFE AER does not mark because of bypassed AFW preference");
    generation.enabled=false;
    check(xrfg::luke_origin_capture_required(generation,reconstruction),"turning AER generation off restores requested reconstruction origins");
    reconstruction.mode=0;
    check(!xrfg::luke_origin_capture_required(generation,reconstruction),"saved AER preference does not mark while every effect is off");
    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    auto source=std::make_shared<SwapchainState>(session,info);
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    xrfg::D3D12HistoryCaptureTicket capture{11,23,1,2};
    AerNativeGuideCache::Eye eye;
    check(eye.observe(source,history,capture),"new release owns a new marker association");
    eye.attempted=true;eye.token=73;eye.guides=std::make_shared<xrfg::DlssMotionVectorSet>();
    const auto retained=eye.guides;
    check(!eye.observe(source,history,capture) && eye.attempted && eye.token==73 && eye.guides==retained,
        "held release keeps its exact token/owned guide without another marker read");
    auto fresh=capture;++fresh.serial;++fresh.fence_value;
    check(eye.observe(source,history,fresh) && !eye.attempted && !eye.token && !eye.guides,
        "fresh capture invalidates old colour/guide association");
    eye.token=74;eye.attempted=true;
    auto replaced_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(eye.observe(source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "history-resource epoch change invalidates token even with identical serial");
    eye.attempted=true;eye.token=75;
    auto replaced_source=std::make_shared<SwapchainState>(session,info);
    check(eye.observe(replaced_source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "replaced source domain cannot inherit a token association");
    eye.attempted=true;eye.token=76;++fresh.fence_value;
    check(eye.observe(replaced_source,replaced_history,fresh) && !eye.attempted && !eye.token,
        "changed producer receipt invalidates cached colour identity");

    xrfg::DlssMotionVectorFrame current;
    current.stream=4;current.epoch=3;current.serial=21;current.previous_serial=19;
    check(!aer_native_same_eye_history(current),"missing same-eye history cannot be guessed from serial parity");
    auto previous=std::make_shared<xrfg::DlssMotionVectorFrame>();
    previous->stream=4;previous->epoch=3;previous->serial=19;current.previous_same_guide=previous;
    check(!aer_native_same_eye_history(current),"same-eye key alone cannot prove raw vector span");
    current.raw_motion_native_span_confirmed=true;current.raw_motion_native_span=1;
    check(!aer_native_same_eye_history(current),"adjacent-eye N1 vectors cannot enter raw same-eye native FG");
    current.raw_motion_native_span=2;
    check(aer_native_same_eye_history(current),"exact producer-owned same-eye history retained");
    current.raw_motion_native_span=1;current.publication=21;previous->publication=19;
    auto middle=std::make_shared<xrfg::DlssMotionVectorFrame>();
    middle->stream=4;middle->epoch=3;middle->serial=middle->publication=20;
    middle->raw_motion_native_span=1;middle->raw_motion_native_span_confirmed=true;
    current.previous_opposite_guide=middle;
    check(aer_native_same_eye_history(current),"confirmed N1 traverses exact middle publication before same-eye FG");
    ++middle->publication;
    check(!aer_native_same_eye_history(current),"N1 composition rejects a missing middle publication");
    --middle->publication;middle->epoch=4;
    check(!aer_native_same_eye_history(current),"N1 composition cannot cross a producer reset");
    middle->epoch=3;middle->raw_motion_native_span_confirmed=false;
    check(!aer_native_same_eye_history(current),"unobserved N1 span cannot authorize forced composition");
    current.previous_opposite_guide.reset();current.raw_motion_native_span=2;
    previous->stream=5;
    check(!aer_native_same_eye_history(current),"opposite/other-stream history cannot authorize native AER");
    previous->stream=4;previous->epoch=4;
    check(!aer_native_same_eye_history(current),"old/new epoch history cannot authorize native AER");
    previous->epoch=3;previous->serial=18;
    check(!aer_native_same_eye_history(current),"missing same-eye generation cannot be bridged by a guessed older receipt");

    session->nvidia_options.ofr=true;session->luke_afw_enabled=true;session->menu_enabled=true;
    session->nvidia_options.rife_model=-1;session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
    check(aer_native_guides_requested(*session),"native AER consumer requests exact guide ingress");
    ProjectionSnapshot snapshot;
    check(refresh_aer_native_guides(session,snapshot,{})==S_FALSE && !session->aer_native_guide_cache,
        "unsupported projection refuses before marker allocation or source scanning");
    session->nvidia_options.rife_model=3;
    check(!aer_native_guides_requested(*session) && refresh_aer_native_guides(session,snapshot,{})==S_OK &&
        !session->aer_native_guide_cache,"RIFE never activates native marker/guide ingress");
    session->nvidia_options.rife_model=-1;session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
    check(!aer_native_guides_requested(*session),"plain optical flow never activates native marker ingress");
    session->hybrid_requested=true;check(aer_native_guides_requested(*session),"Hybrid requests native guides even before its first publication");
    session->menu_enabled=false;
    check(!aer_native_guides_requested(*session),"all-off path performs no native AER marker work");
    std::puts("AER native source cache, held reuse, exact same-eye history and route gating PASS");
    return 0;
}


#include "d3d12_reported_device_alias.hpp"
class AerOriginQueueRefusal final:public ReportedDeviceAlias {
public:
    using ReportedDeviceAlias::ReportedDeviceAlias;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC*,REFIID,void** out) override {
        if(out)*out=nullptr;return E_OUTOFMEMORY;
    }
};

int aer_native_guide_join_test() {
    Fixture f(true);f.transport->stop();f.transport.reset();
    constexpr UINT width=800,height=128;
    const auto texture=[&](DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;
        ComPtr<ID3D12Resource> out;
        ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)),"join texture");return out;
    };
    auto motion=texture(DXGI_FORMAT_R32G32_FLOAT),depth=texture(DXGI_FORMAT_R32_FLOAT);
    std::array<ComPtr<ID3D12Resource>,2> colors{texture(DXGI_FORMAT_R8G8B8A8_UNORM),texture(DXGI_FORMAT_R8G8B8A8_UNORM)};
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;
    state->menu_enabled=state->luke_afw_enabled=state->dlss_motion_vectors=true;
    state->nvidia_options.ofr=true;state->nvidia_options.rife_model=-1;
    state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::fsr3;
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    std::array<ProjectionResourceMapping,2> mappings;
    ProjectionSnapshot snapshot;ProjectionLayerSnapshot projection;
    for(unsigned eye=0;eye<2;++eye) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.width=width;info.height=height;info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;
        auto& chain=chains[eye];chain=std::make_shared<SwapchainState>(state,info);
        chain->handle=reinterpret_cast<XrSwapchain>(0x9290+eye);
        chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* source[]{colors[eye].Get()};
        ok(chain->d3d12_history->initialize(f.device12.Get(),f.queue.Get(),source,D3D12_RESOURCE_STATE_COMMON),"join history");
        chain->last_released_index=0;
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.subImage.swapchain=chain->handle;view.subImage.imageRect.extent={width,height};
        projection.views.push_back(view);
        mappings[eye].application_swapchain=chain->handle;mappings[eye].views.push_back({0,eye});
        std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;
    }
    struct Cleanup {
        std::array<std::shared_ptr<SwapchainState>,2>& chains;
        ~Cleanup(){xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();
            std::scoped_lock lock(g_state_mutex);for(const auto& chain:chains)g_swapchains.erase(chain->handle);}
    } cleanup{chains};
    XrSwapchainCreateInfo immutable{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    immutable.width=immutable.height=4;immutable.arraySize=1;immutable.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    auto fixed=std::make_shared<SwapchainState>(state,immutable);fixed->handle=reinterpret_cast<XrSwapchain>(0x9292);
    {std::scoped_lock lock(g_state_mutex);g_swapchains[fixed->handle]=fixed;}
    struct StaticCleanup {XrSwapchain handle;~StaticCleanup(){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);}} static_cleanup{fixed->handle};
    auto fixed_projection=projection;
    for(unsigned eye=0;eye<2;++eye)fixed_projection.views[eye].subImage={fixed->handle,{{int(eye*2),0},{2,4}},0};
    xrfg::configure_luke_color_origin_extent(width,height);
    for(unsigned placement=0;placement<3;++placement) {
      snapshot.layers={projection};
      if(placement==1)snapshot.layers.insert(snapshot.layers.begin(),fixed_projection);
      if(placement==2)snapshot.layers.push_back(fixed_projection);
      for(std::size_t i=0;i<snapshot.layers.size();++i)snapshot.layers[i].layer_index=i;
      for(auto& mapping:mappings)mapping.views[0].projection_index=placement==1?1:0;
      for(unsigned span:{2U,1U}) {
        xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();
        xrfg::configure_dlss_motion_vector_tracking(true);state->aer_native_guide_cache.reset();
        std::array<std::uint32_t,2> expected{};
        for(unsigned n=1;n<=8;++n) {
            const unsigned eye=n%2;
            ComPtr<ID3D12GraphicsCommandList> recorded;
            f.commands([&](ID3D12GraphicsCommandList* list) {
                recorded=list;xrfg::DlssMotionVectorPublication p;
                p.stream=92900+span;p.output=colors[eye].Get();p.motion_vectors=motion.Get();p.depth=depth.Get();
                p.producer_queue=f.queue.Get();p.producer_command_list=list;
                p.output_width=p.motion_width=p.depth_width=width;p.output_height=p.motion_height=p.depth_height=height;
                p.resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
                p.protect_snapshot_lifetime=true;p.raw_motion_native_span=span;p.raw_motion_native_span_confirmed=true;
                auto frame=xrfg::publish_dlss_motion_vectors(p);check(frame && frame->color_origin_token,"join protected native publication");
                expected[eye]=frame->color_origin_token;
                ok(xrfg::mark_luke_color_origin(list,colors[eye].Get(),expected[eye],D3D12_RESOURCE_STATE_COMMON,
                    f.device12.Get()),"join exact colour CRC");
            });
            ID3D12CommandList* lists[]{recorded.Get()};xrfg::report_dlss_motion_vector_submission(f.queue.Get(),1,lists);
            xrfg::D3D12HistoryCaptureTicket capture;
            ok(chains[eye]->d3d12_history->capture(0,&capture),"join release-bound capture");
            ok(chains[eye]->d3d12_history->commit(capture),"join commit capture");
            chains[eye]->last_released_capture=capture;
            if(n<4)continue;
            record_projection_capture_order(snapshot,mappings);f.gpu_wait();
            // Exact captured inputs and guide receipts are already complete.
            // A newer unrelated ingress wait must not block the marker reader.
            ComPtr<ID3D12Fence> future;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&future)),"join future ingress gate");
            struct Unblock {ComPtr<ID3D12Fence> fence;~Unblock(){fence->Signal(1);}} unblock{future};
            ok(f.queue->Wait(future.Get(),1),"hold unrelated future ingress");
            if(n==4) {
                ComPtr<ID3D12Device> refused;refused.Attach(new AerOriginQueueRefusal(f.device12.Get()));
                state->d3d12_device=refused;
                check(refresh_aer_native_guides(state,snapshot,mappings)==S_FALSE,
                    "healthy-device optional queue OOM must keep real XR delivery instead of losing the session");
                check(state->aer_native_guide_cache && !state->aer_native_guide_cache->poisoned &&
                    state->aer_native_guide_cache->queue_retry_after>GetTickCount64(),"queue allocation refusal must back off without poisoning captures");
                state->d3d12_device=f.device12;
                state->aer_native_guide_cache->queue_retry_after=0;
            }
            check(refresh_aer_native_guides(state,snapshot,mappings)==S_OK,"actual native AER colour-token join did not attach both eyes");
            check(state->aer_native_guide_cache->read_queue.Get()!=f.queue.Get(),"join reused the stalled ingress queue");
            for(unsigned e=0;e<2;++e) {
                auto guides=chains[e]->last_released_motion_vectors;
                check(guides && guides->resolved_eye==int(e) && guides->eye_count==1 && guides->eyes[0] &&
                    guides->eyes[0]->color_origin_token==expected[e],"join mixed, replaced or guessed an eye's native publication");
                check(bool(guides->eyes[0]->bridge)==(span==1),"N1 join lost its exact composer input or N2 was recomposed");
            }
            check(refresh_aer_native_guides(state,snapshot,mappings)==S_OK,"held exact guides were not reused");
            check(classify_fsr_stereo_guides(snapshot,mappings)==FsrStereoGuideReason::ready,"static projection must not request native scene guides");
        }
    }
    }
    std::puts("Native AER GPU ingress: CRC, protected histories, both eyes, N1/N2, static first/last, guide admission and stalled ingress isolation PASS");
    return 0;
}
