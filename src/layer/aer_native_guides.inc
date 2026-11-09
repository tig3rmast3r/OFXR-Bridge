// Included after the release-history origin probe. Session-owned, so an idle
// or destroyed session cannot retain guides in a process-global source cache.
struct AerNativeGuideCache {
    struct Eye {
        std::weak_ptr<SwapchainState> source;
        std::weak_ptr<xrfg::D3D12SwapchainHistory> history;
        xrfg::D3D12HistoryCaptureTicket capture{};
        std::uint32_t token{};
        bool attempted{}, epoch_rejected{};
        std::shared_ptr<const xrfg::DlssMotionVectorSet> guides;

        bool observe(const std::shared_ptr<SwapchainState>& chain,
            const std::shared_ptr<xrfg::D3D12SwapchainHistory>& owner,
            const xrfg::D3D12HistoryCaptureTicket& value) noexcept {
            if(source.lock()==chain && history.lock()==owner && capture.serial==value.serial &&
                capture.fence_value==value.fence_value && capture.slot==value.slot &&
                capture.source_index==value.source_index)return false;
            source=chain;history=owner;capture=value;token=0;attempted=false;
            epoch_rejected=false;guides.reset();return true;
        }
    };
    std::array<Eye,2> eyes;
    xrfg::LukeColorOriginReadReceipt pending;
    bool poisoned{};
};

bool aer_native_guides_requested(const SessionState& state) noexcept {
    return state.menu_enabled && state.nvidia_options.ofr && state.luke_afw_enabled &&
        state.nvidia_options.rife_model<0 &&
        (state.frame_generation_mode==xrfg::D3D12FrameGenerationMode::fsr3 ||
         state.frame_generation_mode==xrfg::D3D12FrameGenerationMode::native_dlss ||
         state.hybrid_requested);
}

bool aer_native_same_eye_history(const xrfg::DlssMotionVectorFrame& frame) noexcept {
    const auto& previous=frame.previous_same_guide;
    // These are raw colour endpoints, not the post-AFW N1 composer output.
    // A same-eye history key alone does not turn adjacent-eye vectors into
    // same-eye motion. Require the producer-confirmed two-native-frame span.
    return frame.raw_motion_native_span_confirmed && frame.raw_motion_native_span==2 &&
        frame.stream && frame.epoch && frame.serial && frame.previous_serial && previous &&
        previous->stream==frame.stream && previous->epoch==frame.epoch &&
        previous->serial==frame.previous_serial && previous->serial<frame.serial;
}

// The marker reader already owns its allocator/readback and refuses reuse
// before its exact previous fence. Omit unchanged peers rather than probing a
// held image on every presentation. A timeout retains that receipt; no further
// read is admitted while its GPU copy is in flight.
HRESULT read_aer_luke_capture_origins(ID3D12CommandQueue* queue,
    const std::array<LukeOriginHistoryProbe,2>& inputs,unsigned mask,
    std::array<std::uint32_t,2>& tokens,std::array<HRESULT,2>& results,
    xrfg::LukeColorOriginReadReceipt& receipt,bool& poisoned) noexcept {
    std::array<xrfg::D3D12HistoryConsumerLease,2> leases{};
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>,2> colors;
    std::array<Microsoft::WRL::ComPtr<ID3D12Fence>,2> producers;
    unsigned acquired{};
    const auto cancel=[&] {
        for(unsigned eye=0;eye<2;++eye)if(acquired&(1U<<eye))
            inputs[eye].history->cancel_consumer(leases[eye]);
    };
    if(!queue)return E_POINTER;
    for(unsigned eye=0;eye<2;++eye)if(mask&(1U<<eye)) {
        if(!inputs[eye].history){cancel();return E_POINTER;}
        const auto hr=inputs[eye].history->acquire_consumer(inputs[eye].capture,&leases[eye],
            colors[eye].GetAddressOf(),producers[eye].GetAddressOf());
        if(FAILED(hr)){cancel();return hr;}
        acquired|=1U<<eye;
        const auto wait=queue->Wait(producers[eye].Get(),inputs[eye].capture.fence_value);
        if(FAILED(wait)){cancel();return wait;}
    }
    const auto hr=xrfg::read_luke_color_origin_pair(queue,{colors[0].Get(),colors[1].Get()},
        tokens,results,D3D12_RESOURCE_STATE_COMMON,8,nullptr,&receipt);
    if(receipt.fence && receipt.value) {
        for(unsigned eye=0;eye<2;++eye)if(acquired&(1U<<eye))
            if(FAILED(inputs[eye].history->retire_consumer(leases[eye],receipt.fence.Get(),receipt.value)))poisoned=true;
    } else if(!receipt.submitted)cancel();
    else poisoned=true; // Retain the leases until teardown, never free submitted reads.
    return poisoned?E_FAIL:hr;
}

// S_OK: exact guides attached (or this route does not consume them).
// S_FALSE: unavailable/unsupported; keep the real input. Failure: poisoned GPU
// transport, which the caller must propagate rather than recycling its leases.
HRESULT refresh_aer_native_guides(const std::shared_ptr<SessionState>& session,
    const ProjectionSnapshot& snapshot,std::span<const ProjectionResourceMapping> mappings) noexcept {
    if(!session || !aer_native_guides_requested(*session))return S_OK;
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    const auto clear=[&] {
        for(const auto& chain:chains)if(chain) {
            std::scoped_lock lock(chain->mutex);chain->last_released_motion_vectors.reset();
        }
    };
    try {
        // No size/parity/latest fallback for packed or ambiguous projections.
        if(mappings.size()!=2 || snapshot.layers.size()!=1 || snapshot.layers[0].views.size()!=2)return S_FALSE;
        std::array<LukeOriginHistoryProbe,2> inputs;
        for(const auto& mapping:mappings) {
            if(mapping.views.size()!=1 || mapping.views[0].projection_index!=0 || mapping.views[0].view_index>1) {
                clear();return S_FALSE;
            }
            const auto eye=unsigned(mapping.views[0].view_index);
            const auto chain=find_swapchain(mapping.application_swapchain);
            if(!chain || chain->session!=session || chains[eye]){clear();return S_FALSE;}
            chains[eye]=chain;
            const auto& view=snapshot.layers[0].views[eye];
            if(view.subImage.swapchain!=mapping.application_swapchain || view.subImage.imageArrayIndex ||
                chain->create_info.arraySize!=1){clear();return S_FALSE;}
            const auto recorded=std::find_if(snapshot.capture_order.begin(),snapshot.capture_order.end(),
                [&](const auto& capture){return capture.swapchain==mapping.application_swapchain;});
            if(recorded==snapshot.capture_order.end()){clear();return S_FALSE;}
            {
                std::scoped_lock lock(chain->mutex);
                const auto history=recorded->history.lock();
                const auto capture=chain->last_released_capture;
                if(!history || history!=chain->d3d12_history || !capture || !recorded->serial ||
                    capture->serial!=recorded->serial || capture->fence_value!=recorded->fence_value ||
                    !chain->last_released_index || capture->source_index!=*chain->last_released_index) {
                    chain->last_released_motion_vectors.reset();
                } else inputs[eye]={history,*capture};
            }
            if(!inputs[eye].history){clear();return S_FALSE;}
        }
        if(!chains[0] || !chains[1] || chains[0]==chains[1]){clear();return S_FALSE;}
        if(!session->aer_native_guide_cache)session->aer_native_guide_cache=std::make_shared<AerNativeGuideCache>();
        auto& cache=*session->aer_native_guide_cache;
        if(cache.poisoned){clear();return E_FAIL;}
        if(cache.pending.fence && cache.pending.value) {
            const auto completed=cache.pending.fence->GetCompletedValue();
            if(completed==UINT64_MAX){cache.poisoned=true;clear();return DXGI_ERROR_DEVICE_REMOVED;}
            if(completed<cache.pending.value){clear();return S_FALSE;}
            cache.pending={};
        }
        unsigned read_mask{};
        for(unsigned eye=0;eye<2;++eye) {
            auto& item=cache.eyes[eye];
            if(item.observe(chains[eye],inputs[eye].history,inputs[eye].capture)) {
                std::scoped_lock lock(chains[eye]->mutex);chains[eye]->last_released_motion_vectors.reset();
            }
            if(!item.attempted)read_mask|=1U<<eye;
        }
        if(read_mask) {
            std::array<std::uint32_t,2> tokens{};
            std::array<HRESULT,2> results{E_PENDING,E_PENDING};
            xrfg::LukeColorOriginReadReceipt receipt;
            const auto hr=read_aer_luke_capture_origins(session->d3d12_queue.Get(),inputs,read_mask,
                tokens,results,receipt,cache.poisoned);
            if(cache.poisoned){clear();return E_FAIL;}
            if(FAILED(hr)) {
                // Keep exact source leases retired by the submitted copy fence.
                // The reader API has no decode-pending operation: once that
                // fence is complete a bounded ROI retry is safe. Never retry
                // before completion or substitute a newer guide meanwhile.
                cache.pending=std::move(receipt);clear();return S_FALSE;
            }
            for(unsigned eye=0;eye<2;++eye)if(read_mask&(1U<<eye)) {
                auto& item=cache.eyes[eye];item.attempted=true;
                item.token=SUCCEEDED(results[eye])?tokens[eye]:0;
            }
        }
        bool complete=true;
        for(unsigned eye=0;eye<2;++eye) {
            auto& item=cache.eyes[eye];
            if(item.guides && item.guides->eyes[0] && xrfg::luke_native_stream_reset(
                item.guides->eyes[0]->stream,item.guides->eyes[0]->epoch,session->d3d12_device.Get())) {
                item.guides.reset();item.epoch_rejected=true;
            }
            if(!item.guides && item.token && !item.epoch_rejected) {
                const auto frame=xrfg::resolve_luke_guide_token(item.token,session->d3d12_queue.Get(),session->d3d12_device.Get());
                if(frame && frame->color_origin_token==item.token && frame->publication &&
                    frame->motion_vectors && frame->depth && aer_native_same_eye_history(*frame)) {
                    auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();
                    guides->eye_count=1;guides->resolved_eye=int(eye);guides->eyes[0]=frame;
                    item.guides=std::move(guides);
                }
            }
            std::scoped_lock lock(chains[eye]->mutex);
            const auto current=chains[eye]->last_released_capture;
            const bool same=current && current->serial==item.capture.serial &&
                current->fence_value==item.capture.fence_value && chains[eye]->d3d12_history==item.history.lock();
            chains[eye]->last_released_motion_vectors=same?item.guides:nullptr;
            complete=complete && same && item.guides!=nullptr;
        }
        return complete?S_OK:S_FALSE;
    } catch(...) {clear();return S_FALSE;}
}
