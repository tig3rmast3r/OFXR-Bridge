static int independent_feature_settings_test() {
    xrfg_fixture::ControlProfile profile(true);
    auto controls=xrfg::autonomous::snapshot().desired;controls.independent_pacing=true;controls.advanced_mode=true;
    controls.afw_mode=3;controls.r19_camera=true;controls.nr_enabled=true;controls.lower_vram=1;controls.pacing_mode=1;
    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());session->independent_pacing=true;
    for(int method=0;method<=4;++method) {
        xrfg::autonomous::select_generation(controls,method,method==0?1:0);controls.motion_vectors=method>0 && method<4;
        check(xrfg::autonomous::request(controls),"Independent feature request");xrfg::autonomous::apply_runtime_settings();
        const auto saved=xrfg::autonomous::snapshot().desired;const auto input=xrfg::embedded::snapshot().desired;
        const auto effective=effective_session_settings(*session,input);
        check(saved.frame_generation==method && effective.frame_generation==(method>=3?0:method) &&
            effective.hybrid==(method==3) && effective.motion_vectors==controls.motion_vectors &&
            effective.rife_model==(method==4?controls.rife_model:-1),"Independent preserves the selected real generator and guide source");
        check(effective.independent_pacing && effective.pacing_mode==0 && !effective.increase_buffer &&
            !effective.lower_vram && !effective.separate_synthesis,"legacy pacing cannot leak into restored features");
        check(xrfg::embedded::afw_snapshot().desired.mode==3 && xrfg::autonomous::r19_camera_enabled.load() &&
            xrfg::neural_rendering::status().enabled,"reconstruction/camera/NR remain available");
    }
    for(int backend:{0,1,2}) {
        xrfg::autonomous::select_generation(controls,3,0);controls.hybrid_backend=backend;
        check(xrfg::autonomous::request(controls),"Hybrid saved backend");xrfg::autonomous::apply_runtime_settings();
        const auto saved=xrfg::autonomous::snapshot().desired;
        const auto effective=effective_session_settings(*session,xrfg::embedded::snapshot().desired);
        check(saved.hybrid_backend==backend && effective.backend==(backend==0?0:1),"Independent Hybrid uses the same single NVIDIA engine as Independent OFXR without destroying the saved legacy preference");
    }
    std::puts("INDEPENDENT_FEATURES methods0..4, immutable route, reconstruction/camera/NR and legacy pacing bypass PASS");return 0;
}

static int independent_feature_inputs_test(bool reconstructed=false) {
    xrfg_fixture::ControlProfile profile(true);Fixture f(true);f.throttled=false;f.unblock();
    auto state=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    state->private_runtime=f.transport;state->independent_pacing=true;state->menu_enabled=true;
    const auto handle=f.make_chain();
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=info.height=1;
    info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto chain=std::make_shared<SwapchainState>(state,info);chain->handle=handle;
    auto generation=std::make_shared<FrameGenerationSwapchainState>();chain->frame_generation=generation;
    unsigned count{};xr_ok(f.transport->enumerate_images(handle,0,&count,nullptr),"source count");
    std::vector<XrSwapchainImageD3D12KHR> xr_images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    xr_ok(f.transport->enumerate_images(handle,count,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(xr_images.data())),"source images");
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();ID3D12Resource* images[]{xr_images[0].texture};
    ok(history->initialize(f.device12.Get(),f.queue.Get(),images,D3D12_RESOURCE_STATE_RENDER_TARGET),"feature source history");
    xrfg::D3D12HistoryCaptureTicket capture;ok(history->capture(0,&capture),"feature capture");ok(history->commit(capture),"feature commit");
    chain->d3d12_history=history;chain->last_released_capture=capture;generation->afw_final_history=history;
    auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();auto eye=std::make_shared<xrfg::DlssMotionVectorFrame>();eye->serial=11;guides->eyes[0]=eye;
    chain->last_released_motion_vectors=guides;
    {std::scoped_lock lock(g_state_mutex);g_swapchains[handle]=chain;}
    XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};view.pose.orientation.w=1;view.fov={-.7f,.7f,.7f,-.7f};
    view.subImage.swapchain=handle;view.subImage.imageRect.extent={1,1};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=1;projection.views=&view;projection.space=reinterpret_cast<XrSpace>(9);
    const auto* base=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&base;end.displayTime=epoch+period;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"feature metadata");
    ProjectionResourceMapping mapping;mapping.application_swapchain=handle;mapping.views={{0,0}};
    std::shared_ptr<CompletedAfwFrame> afw;
    if(reconstructed) {
        auto controls=xrfg::autonomous::snapshot().desired;
        controls.afw_mode=3;controls.luke_osr=2;
        check(xrfg::autonomous::request(controls),"enable OSR reconstruction");
        xrfg::autonomous::apply_runtime_settings();
        state->luke_afw_enabled=true;
        state->dispatch->steamvr_runtime=true;
        state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::nvidia;
        state->dlss_motion_vectors=true; // OSR uses guides even with plain optical framegen
        check(independent_osr_optical_pair(*state),"OSR with NVIDIA optical 2X uses measured pair phase");
        state->nvidia_options.ofr=true;
        check(!independent_osr_optical_pair(*state),"AER retains its own phase policy");
        state->nvidia_options.ofr=false;state->nvidia_options.rife_model=0;
        check(!independent_osr_optical_pair(*state),"RIFE retains its own phase policy");
        state->nvidia_options.rife_model=-1;state->hybrid_requested=true;
        check(!independent_osr_optical_pair(*state),"Hybrid retains its own phase policy");
        state->hybrid_requested=false;
        afw=std::make_shared<CompletedAfwFrame>();afw->snapshot=snapshot;
        GeneratedFrameEndInfo owned;std::array<ProjectionResourceDestination,1> destinations{{{handle,handle}}};
        check(build_generated_frame_end_info(&end,snapshot,false,destinations,&owned),"completed reconstruction metadata");
        afw->submission=make_presenter_owned_frame(std::move(owned));PostAfwFrameInput input;input.application_swapchain=handle;input.generation=generation;input.capture=capture;afw->inputs.push_back(input);
        chain->last_released_capture.reset();chain->d3d12_history.reset(); // raw inputs cannot substitute for reconstruction
    }
    xr_ok(handoff_three_slot_frame(state,&end,snapshot,std::span(&mapping,1),false,afw),"Independent immutable handoff");
    chain->last_released_motion_vectors.reset();
    check(state->independent_sources.size()==1,"worker owns source");
    auto job=state->independent_sources.front();check(job->sources.size()==1 && job->sources[0]->history==history &&
        job->sources[0]->capture.serial==capture.serial && job->sources[0]->source_pin,"exact color capture survives admission");
    check(job->osr_pair_phase==reconstructed,"phase eligibility belongs to the captured source");
    if(reconstructed)check(job->post_afw_frame==afw,"completed reconstruction identity retained");
    else check(job->sources[0]->motion_vectors==guides && job->sources[0]->motion_vectors->eyes[0]->serial==11,"later release cannot replace captured guides");
    state->independent_sources.clear();job.reset();state->presenter_stop_requested=true;
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);}
    ok(history->wait_for_idle(),"source producer completed");xr_ok(f.transport->destroy_swapchain(handle),"feature cleanup");f.transport->stop();
    std::printf("INDEPENDENT_INPUTS post_reconstruction=%u exact_capture/guides/metadata pinned before worker PASS\n",reconstructed);return 0;
}
