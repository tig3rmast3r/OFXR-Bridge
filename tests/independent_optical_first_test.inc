static int independent_optical_first_test() {
    xrfg_fixture::ControlProfile profile;
    independent_fixture_mode=true;
    for(const char* runtime:{"Pimax OpenXR","SteamVR/OpenXR","VirtualDesktopXR","Other OpenXR"})for(unsigned factor:{2U,3U}) {
        Fixture f(true);f.throttled=false;f.unblock();
        auto dispatch=std::make_shared<Dispatch>();dispatch->runtime_name=runtime;
        dispatch->wait_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageWaitInfo* i){return current->transport->wait_image(h,i);};
        dispatch->release_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageReleaseInfo* i){return current->transport->release_image(h,i);};
        auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
        state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->independent_pacing=state->menu_enabled=true;state->nvidia_options.rife_multiplier=factor;
        state->nvidia_options.rife_model=-1;state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        auto job=std::make_shared<SynthesisJob>();job->pair=true;job->triple=factor==3;job->fraction=1.f/factor;
        job->current=std::make_shared<ThreeSlotFrame>();job->current->source_sequence=50;job->current->previous_sequence=49;
        job->current->queued_at=std::chrono::steady_clock::now();job->independent_period_ns=period;
        job->independent_first_due_ns=xrfg::interface_clock_ns()+period;
        std::array<XrCompositionLayerProjectionView,2> views{};
        PreparedProjectionFrame prepared;prepared.kind=PreparedGenerationKind::pair;
        std::array<ComPtr<ID3D12Fence>,2> eye_gate,ready;
        std::array<ComPtr<ID3D12CommandQueue>,2> queues;
        ComPtr<ID3D12Fence> real_gate;
        ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&real_gate)),"real-only completion gate");
        const auto acquired=[&](PrivateSwapchainState& image,std::vector<ComPtr<ID3D12Resource>>& images,unsigned color) {
            image.handle=f.make_chain();
            f.release_callback=[&](XrSwapchain,unsigned index){image.acquired_index=index;image.phase=PrivateOwnershipPhase::waited;return XR_SUCCESS;};
            f.write(image.handle,color);f.release_callback={};
            std::array<XrSwapchainImageD3D12KHR,3> raw{};for(auto& r:raw)r.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            unsigned count{};xr_ok(f.transport->enumerate_images(image.handle,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(raw.data())),"first output images");
            for(unsigned i=0;i<count;++i)images.emplace_back(raw[i].texture);
        };
        for(unsigned eye=0;eye<2;++eye) {
            const auto source=reinterpret_cast<XrSwapchain>(UINT_PTR(0x1000+eye));
            auto generation=std::make_shared<FrameGenerationSwapchainState>();
            acquired(generation->synthetic[0],generation->synthetic_runtime_images,100+eye);
            if(factor==3)acquired(generation->synthetic[1],generation->synthetic_runtime_images,150+eye);
            acquired(generation->current[0],generation->current_runtime_images,200+eye);
            generation->synthetic_runtime_images_per_ring=generation->current_runtime_images_per_ring=3;
            PreparedGeneration g;g.kind=PreparedGenerationKind::pair;g.worker_generation=generation;g.deferred_synthetic_reuse=factor==3;
            g.direct_synthetic_lease=std::make_shared<DirectSyntheticLease>();
            g.independent_synthetic_lease=std::make_shared<unsigned>(eye);
            g.synthetic_handle=generation->synthetic[0].handle;if(factor==3)g.extra_handle=generation->synthetic[1].handle;g.current_handle=generation->current[0].handle;
            g.synthetics_in_target_camera=true;
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&eye_gate[eye])),"eye readiness gate");
            ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&ready[eye])),"same eye S/B fence");
            D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            ok(f.device12->CreateCommandQueue(&q,IID_PPV_ARGS(&queues[eye])),"eye readiness lane");
            g.worker_fence=ready[eye];g.worker_synthetic_fence_value=1;g.worker_fence_value=2;
            prepared.resources.push_back({source,std::move(g),false});job->mappings.push_back({source,{{0,eye}}});
            auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
            v.pose.position.x=10.f+eye;v.fov={-.7f,.7f,.7f,-.7f};v.subImage.swapchain=source;v.subImage.imageRect.extent={1,1};
        }
        f.gpu_wait();
        for(unsigned eye=0;eye<2;++eye) {
            ok(queues[eye]->Wait(eye_gate[eye].Get(),1),"S waits for its own eye");
            ok(queues[eye]->Signal(ready[eye].Get(),1),"queue exact synthetic readiness");
            ok(queues[eye]->Wait(real_gate.Get(),1),"hold only following real completion");
            ok(queues[eye]->Signal(ready[eye].Get(),2),"real completion already queued");
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);
        projection.viewCount=2;projection.views=views.data();const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount=1;end.layers=&layer;
        check(capture_projection_snapshot(&end,&job->snapshot),"first stereo metadata snapshot");
        std::array<ProjectionResourceDestination,2> ids{{{views[0].subImage.swapchain,views[0].subImage.swapchain},{views[1].subImage.swapchain,views[1].subImage.swapchain}}};
        GeneratedFrameEndInfo metadata;
        check(build_generated_frame_end_info(&end,job->snapshot,false,ids,&metadata),"own first metadata");
        job->metadata=make_presenter_owned_frame(std::move(metadata));
        job->synthetic_camera=job->snapshot;
        for(unsigned eye=0;eye<2;++eye)job->synthetic_camera->layers[0].views[eye].pose.position.x=5.f+eye;
        bool delayed{};
        // Auxiliary transactions, mismatched multipliers and interop keep their old path.
        job->metadata->composition_images.push_back(std::make_shared<CompositionImageSlot>());
        check(publish_independent_optical_first(state,prepared,job,delayed)==S_FALSE,"auxiliary image readiness cannot be bypassed");
        job->metadata->composition_images.clear();
        for(unsigned multiplier:{2U,3U,4U}) {if(multiplier==factor)continue;state->nvidia_options.rife_multiplier=multiplier;
            check(publish_independent_optical_first(state,prepared,job,delayed)==S_FALSE,"mismatched multiplier must not publish an old job");}
        state->nvidia_options.rife_multiplier=factor;state->graphics_binding=SessionGraphicsBinding::d3d11;
        check(publish_independent_optical_first(state,prepared,job,delayed)==S_FALSE,"D11 keeps its publication contract");state->graphics_binding=SessionGraphicsBinding::d3d12;
        auto publish=std::async(std::launch::async,[&]{return publish_independent_optical_first(state,prepared,job,delayed);});
        ok(eye_gate[0]->Signal(1),"only left S completes");
        ok(wait_composition_fence(ready[0].Get(),1,1000),"left S really finished");
        check(publish.wait_for(20ms)==std::future_status::timeout,"one eye must never publish a stereo frame");
        check(!prepared.resources[0].generation.synthetic_released,"left image remains acquired until right completes");
        ok(eye_gate[1]->Signal(1),"right S completes");
        const bool early=publish.wait_for(1000ms)==std::future_status::ready;
        if(!early)real_gate->Signal(1);
        const auto published=publish.get();
        check(early && published==S_OK && !delayed && ready[0]->GetCompletedValue()==1 && ready[1]->GetCompletedValue()==1,
            "coherent S must publish while both real copies are still pending");
        check(job->first_published && state->independent_outputs.size()==1,"exactly one early S endpoint");
        const auto& first=*state->independent_outputs.front()->frame;
        check(first.direct_synthetic_leases.size()==2 && first.independent_output_leases.size()==2,
            "early endpoint must retain both eyes' descriptor and publication ownership");
        check(first.independent_due_ns==job->independent_first_due_ns,"early S preserves the frozen expiry slot");
        for(unsigned eye=0;eye<2;++eye) {
            const auto& g=prepared.resources[eye].generation;
            check(first.projections[0].views[eye].pose.position.x==5.f+eye &&
                first.projections[0].views[eye].subImage.swapchain==g.synthetic_handle,
                "early S uses its own exact eye image and synthetic camera");
            check(g.synthetic_released && (factor!=3 || g.worker_generation->synthetic[1].phase==PrivateOwnershipPhase::waited) && g.worker_generation->current[0].phase==PrivateOwnershipPhase::waited,
                "early S cannot release an unfinished real image");
        }
        check(publish_independent_optical_first(state,prepared,job,delayed)==S_FALSE,"S cannot publish twice");
        auto full=std::async(std::launch::async,[&]{return finish_worker_outputs(state,prepared,50);});
        const bool real_held=full.wait_for(20ms)==std::future_status::timeout;
        ok(real_gate->Signal(1),"both queued real copies retire");
        check(full.get() && real_held,"full completion still owns real output lifetime");
        for(auto& r:prepared.resources)check(r.generation.worker_generation->current[0].phase==PrivateOwnershipPhase::idle,
            "real release follows its own full completion");
        std::array<std::weak_ptr<DirectSyntheticLease>,2> descriptor_owners;
        for(unsigned eye=0;eye<2;++eye) {
            auto& g=prepared.resources[eye].generation;descriptor_owners[eye]=first.direct_synthetic_leases[eye];
            check(factor!=3 || (!g.direct_synthetic_lease && !g.independent_synthetic_lease),"S1 ownership must transfer out of a deferred batch");
            g.direct_synthetic_lease.reset();g.independent_synthetic_lease.reset();
            check(!descriptor_owners[eye].expired(),"retiring the batch must not retire the queued first frame's descriptor");
        }
        // A completed but late midpoint must retain the existing real-only
        // recovery decision, even though its smaller fence is now ready.
        job->first_published=false;
        for(auto& r:prepared.resources)r.generation.synthetic_released=false;
        delayed=true;
        check(publish_independent_optical_first(state,prepared,job,delayed)==S_FALSE &&
            !job->first_published && state->independent_outputs.size()==1,
            "late S must not bypass real-only recovery or publish twice");
        for(auto& r:prepared.resources)r.generation.synthetic_released=true;
        state->independent_outputs.clear();
        for(const auto& owner:descriptor_owners)check(owner.expired(),"first-frame descriptor must retire after its last consumer");
        for(auto& r:prepared.resources) {xr_ok(f.transport->destroy_swapchain(r.generation.synthetic_handle),"S cleanup");xr_ok(f.transport->destroy_swapchain(r.generation.current_handle),"B cleanup");if(factor==3)xr_ok(f.transport->destroy_swapchain(r.generation.extra_handle),"S2 cleanup");}
        std::printf("OPTICAL_FIRST %s %uX coherent two-eye S before pending B; exact pose/deadline, aux/multiplier/interop guards PASS\n",runtime,factor);
    }
    // Retry/destructor paths must not repeat a late acquire after failure.
    for(unsigned mode=0;mode<3;++mode) {
        unsigned calls{};
        {
            QueuedOpticalTail tail;
            tail.engine=std::make_shared<xrfg::D3D12FrameSynthesizer>();
            tail.submitted=mode!=2;
            tail.before_flush=[&]() -> HRESULT {++calls;if(mode==1)throw std::runtime_error("late acquire");return E_ABORT;};
            check(FAILED(tail.flush()) && FAILED(tail.flush()),"failed acquire must remain a failed transaction");
        }
        check(calls==(mode==2?0U:1U),"failure, exception and unsubmitted teardown must cancel once");
    }
    // Exercise the production S2 wait/acquire boundary under shutdown, an XR
    // acquire refusal and an XR wait failure, without submitting any writes.
    for(bool stop:{false,true}) {
        auto owner=std::make_shared<SessionState>(std::make_shared<Dispatch>());
        auto bank=std::make_shared<FrameGenerationSwapchainState>();
        bank->direct_synthetic_leases[1]=std::make_shared<DirectSyntheticLease>();
        auto previous=bank->direct_synthetic_leases[1];
        std::shared_ptr<DirectSyntheticLease> retained;
        auto pending=std::async(std::launch::async,[&]{return acquire_optical_tail_image(owner,bank,1,retained);});
        check(pending.wait_for(20ms)==std::future_status::timeout,"S2 must retain its previous reader's descriptor");
        {std::scoped_lock lock(owner->presenter_mutex);if(stop)owner->presenter_stop_requested=true;else owner->presenter_failure=XR_ERROR_RUNTIME_FAILURE;}
        owner->presenter_condition.notify_all();
        const bool interrupted=pending.wait_for(1000ms)==std::future_status::ready;
        if(!interrupted){previous.reset();owner->presenter_condition.notify_all();}
        check(pending.get()==E_ABORT && interrupted && !retained,"stop/failure must interrupt S2 before private acquisition");
    }
    for(unsigned mode=0;mode<3;++mode) {
        static unsigned acquire_calls{},wait_calls{},release_calls{},failure_mode{};
        acquire_calls=wait_calls=release_calls=0;failure_mode=mode;
        auto dispatch=std::make_shared<Dispatch>();
        dispatch->acquire_swapchain_image=+[](XrSwapchain,const XrSwapchainImageAcquireInfo*,std::uint32_t* index){++acquire_calls;*index=0;return failure_mode==0?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;};
        dispatch->wait_swapchain_image=+[](XrSwapchain,const XrSwapchainImageWaitInfo*){++wait_calls;return failure_mode==1?XR_ERROR_RUNTIME_FAILURE:XR_SUCCESS;};
        dispatch->release_swapchain_image=+[](XrSwapchain,const XrSwapchainImageReleaseInfo*){++release_calls;return XR_SUCCESS;};
        auto owner=std::make_shared<SessionState>(dispatch);
        auto bank=std::make_shared<FrameGenerationSwapchainState>();
        bank->direct_synthetic_leases[1]=std::make_shared<DirectSyntheticLease>();
        auto& image=bank->synthetic[1];image.handle=reinterpret_cast<XrSwapchain>(9999);image.first_use_logged=true;
        std::shared_ptr<DirectSyntheticLease> retained;
        const auto result=acquire_optical_tail_image(owner,bank,1,retained);
        check((mode==2?SUCCEEDED(result):FAILED(result)) && retained && acquire_calls==1 && wait_calls==(mode==0?0U:1U) && release_calls==0,
            "S2 failure must not flush writes or release an image that has not completed");
        check(image.phase==(mode==0?PrivateOwnershipPhase::idle:mode==1?PrivateOwnershipPhase::acquired:PrivateOwnershipPhase::waited),
            "partial S2 acquisition must retain its exact teardown ownership state");
    }
    independent_fixture_mode=false;return 0;
}
