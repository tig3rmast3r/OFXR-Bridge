// Replay the observed allocation orders through the shipping factories.
// The runtime ceiling is explicit; source images and GPU work stay identical.
static int independent_panel_budget_test(unsigned order) {
    independent_fixture_mode=true;independent_fixture_binding_queue_handoff=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
        dispatch->runtime_name="SteamVR/OpenXR";
        dispatch->create_swapchain=private_runtime_create;dispatch->destroy_swapchain=private_runtime_destroy;
        dispatch->enumerate_swapchain_images=private_runtime_enumerate;
        dispatch->acquire_swapchain_image=private_runtime_acquire;dispatch->wait_swapchain_image=private_runtime_wait_image;
        dispatch->release_swapchain_image=private_runtime_release;
        auto session=std::make_shared<SessionState>(dispatch);session->handle=reinterpret_cast<XrSession>(2);
        session->independent_pacing=session->menu_enabled=true;session->vdxr_legacy=false;
        session->graphics_binding=SessionGraphicsBinding::d3d12;
        session->d3d12_device=f.device12;session->d3d12_queue=f.queue;session->private_delivery_queue=f.delivery_queue;
        session->private_runtime=f.transport;session->nvidia_options.rife_model=-1;session->nvidia_new_route=true;
        session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        session->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ok(f.device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&session->d3d12_synthesis_queue)),"panel budget synthesis queue");
        {std::scoped_lock lock(g_state_mutex);g_sessions[session->handle]=session;}
        std::array<XrSwapchain,6> sources{};
        std::array<std::shared_ptr<SwapchainState>,4> chains;
        for(unsigned i=0;i<sources.size();++i) {
            sources[i]=f.make_chain();
            if(i>=chains.size())continue;
            auto chain=std::make_shared<SwapchainState>(session,optional_surface_info());chain->handle=sources[i];
            chain->enumerated_image_count=3;chain->generation_eligible_pending=true;
            std::array<XrSwapchainImageD3D12KHR,3> images{};for(auto& image:images)image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
            unsigned count{};xr_ok(f.transport->enumerate_images(sources[i],3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"panel budget source images");
            std::array<ID3D12Resource*,3> textures{};for(unsigned j=0;j<count;++j)textures[j]=images[j].texture;
            if(order==4)for(unsigned j=0;j<count;++j)chain->enumerated_d3d12_images.emplace_back(textures[j]);
            chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
            ok(chain->d3d12_history->initialize(f.device12.Get(),f.queue.Get(),std::span(textures.data(),count),D3D12_RESOURCE_STATE_RENDER_TARGET),"panel budget source history");
            chains[i]=chain;std::scoped_lock lock(g_state_mutex);g_swapchains[sources[i]]=chain;
        }
        const auto base=f.chains.size();f.native_chain_limit=base+(order==4?17:8);
        std::array<ProjectionResourceMapping,2> eyes{};
        for(unsigned i=0;i<2;++i)eyes[i].application_swapchain=sources[i];
        const std::array<PanelSource,2> panels{{{sources[2],0},{sources[3],0}}};
        bool panels_ok{};std::size_t after_panels{};
        const auto arm_panels=[&] {
            if(order==2)for(unsigned i=2;i<4;++i)chains[i]->pooled_publication=true;
            panels_ok=arm_panel_generation(session,panels);after_panels=f.chains.size();
            if(!panels_ok)retire_panel_generation(session);
        };
        if(order==0 || order==2)arm_panels();
        std::promise<void> entered,release;auto inside=entered.get_future();auto gate=release.get_future().share();
        std::atomic_bool first{true};
        if(order==3) {
            f.write(sources[0],80);f.gpu_wait();
            f.create_callback=[&]{if(first.exchange(false)){entered.set_value();gate.wait();}};
        }
        check(ensure_projection_frame_generation(session,eyes),"launch eye factories after replayed panel allocation");
        if(order==3) {
            check(inside.wait_for(2s)==std::future_status::ready,"eye worker entered gated allocation");
            auto optional=std::async(std::launch::async,[&]{return arm_panel_generation(session,panels);});
            const bool nonblocking=optional.wait_for(50ms)==std::future_status::ready;
            if(!nonblocking)release.set_value();
            check(optional.get(),"optional panel deferral failed");
            check(nonblocking && !chains[2]->panel_source && !chains[3]->panel_source,
                "panels blocked on or allocated ahead of unfinished eyes");
            for(unsigned frame=1;frame<=3;++frame) {
                f.begin_virtual();xr_ok(f.submit(sources[0],float(frame)),"fresh real frame during eye allocation");
                xr_ok(f.transport->drain(),"real-frame delivery during eye allocation");
            }
            release.set_value();
        }
        finish_session_generation_creation(session);
        const bool eyes_ok=ensure_projection_frame_generation(session,eyes);
        const auto after_eyes=f.chains.size();
        if(order==1 || order==3 || order==4)arm_panels();
        const bool failed_eye=std::any_of(chains.begin(),chains.begin()+2,[](const auto& chain) {
            return budget_refusal(chain->generation_refusal) &&
                static_cast<std::int64_t>(chain->generation_refusal_detail)==XR_ERROR_LIMIT_REACHED;
        });
        if(order==0)check(panels_ok && !eyes_ok && failed_eye && session->generation_budget_exhausted,
            "panel-first control did not reproduce session-wide eye budget failure");
        else check(eyes_ok && !failed_eye && !session->generation_budget_exhausted &&
            chains[0]->frame_generation && chains[1]->frame_generation,
            "eye-priority/shared-panel control lost required eye outputs");
        std::printf("PANEL_BUDGET order=%u base=%zu limit=%zu after_panels=%zu after_eyes=%zu panels_ok=%d eyes_ok=%d eye_limit=%d session_latched=%d\n",
            order,base,f.native_chain_limit,after_panels,after_eyes,panels_ok,eyes_ok,failed_eye,bool(session->generation_budget_exhausted));
        if(order==4) {
            check(panels_ok && chains[2]->frame_generation && chains[3]->frame_generation,
                "both optional generators must occupy the budget before required UI publication");
            // Allow the first required quad, then refuse the second. Keep
            // this exact ceiling through recovery; do not buy more capacity.
            f.native_chain_limit=f.chains.size()+1;
            std::array<XrCompositionLayerProjectionView,2> views{};
            for(unsigned eye=0;eye<2;++eye) {
                f.write(sources[eye],80+eye*7);
                auto& view=views[eye];view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                view.pose.orientation.w=1;view.pose.position.x=eye?.032F:-.032F;
                view.fov={-.5F,.5F,.5F,-.5F};view.subImage.swapchain=sources[eye];view.subImage.imageRect.extent={1,1};
                f.write(sources[eye+2],30+eye*40);
                ok(capture_composition_image(chains[eye+2],0),"capture the required quad pixels");
                chains[eye+2]->composition_capture->valid=true;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space=reinterpret_cast<XrSpace>(3);projection.viewCount=2;projection.views=views.data();
            std::array<XrCompositionLayerQuad,2> quads{};
            std::array<const XrCompositionLayerBaseHeader*,3> layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
            for(unsigned i=0;i<2;++i) {
                auto& q=quads[i];q.type=XR_TYPE_COMPOSITION_LAYER_QUAD;q.space=projection.space;
                q.pose.orientation.w=1;q.pose.position.x=float(i+1);q.size={1,1};
                q.subImage.swapchain=sources[i+2];q.subImage.imageRect.extent={1,1};
                layers[i+1]=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&q);
            }
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=period;end.layerCount=3;end.layers=layers.data();
            ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"budget recovery original projection");
            const std::array<ProjectionResourceDestination,2> identities{{{sources[0],sources[0]},{sources[1],sources[1]}}};
            const auto packet=[&] {
                GeneratedFrameEndInfo raw;check(build_generated_frame_end_info(&end,snapshot,false,identities,&raw),"budget recovery original metadata");
                return make_presenter_owned_frame(std::move(raw));
            };
            auto original=packet();XrResult refusal=XR_SUCCESS;
            check(own_composition_images(session,*original,&refusal)==XR_ERROR_RUNTIME_FAILURE && refusal==XR_ERROR_LIMIT_REACHED,
                "old Independent path did not reproduce the required-UI native limit");
            check(original->composition_images.size()==1,"negative must own the first quad before the second is refused");
            original.reset();
            auto recovered=packet();
            xr_ok(own_composition_images_with_budget_recovery(session,&end,snapshot,identities,recovered),"recover required UI publication");
            check(session->panel_budget_refused && !session->generation_budget_exhausted &&
                chains[0]->frame_generation && chains[1]->frame_generation &&
                !chains[2]->frame_generation && !chains[3]->frame_generation,
                "required-UI recovery must retire only optional generation");
            std::atomic_bool inspected{};
            f.inspect_layers=[&](const XrFrameEndInfo* native) {
                check(native->layerCount==3,"recovery lost an original UI layer");
                for(unsigned i=0;i<2;++i) {
                    const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(native->layers[i+1]);
                    check(q->pose.position.x==float(i+1) && f.read_pixel(f.chains.at(q->subImage.swapchain))==30+i*40,
                        "recovery changed or confused the two UI images/poses");
                }
                inspected=true;
            };
            f.expected_views=2;
            f.begin_virtual();xr_ok(f.transport->end_frame(&recovered->info),"native recovered world/UI submit");
            xr_ok(f.transport->drain(),"native recovered world/UI retirement");
            check(inspected && f.pixels==std::vector<unsigned>{80} && f.right_pixels==std::vector<unsigned>{87} &&
                std::abs(f.positions.front()+.032F)<.0001F,"recovery changed world pixels or eye pose");
            recovered.reset();f.inspect_layers={};
            for(unsigned i=2;i<4;++i)destroy_composition_capture(chains[i]);
            std::puts("REQUIRED_UI_BUDGET old=-2 native=-10 partial_quad=1 world_and_both_ui_pixels_poses=PASS no_extra_capacity=PASS");
        }
        if(order==3) {
            check(f.pixels==std::vector<unsigned>({80,80,80}),"eye priority stalled or changed real-frame delivery");
            auto owner=std::move(chains[0]->frame_generation);
            chains[0]->generation_creation=std::async(std::launch::deferred,[owner] {
                SwapchainState::GenerationCreation result;result.generation=owner;return result;
            });
            session->generation_budget_exhausted=true;
            release_session_generation_budget(session);
            for(const auto& chain:chains)check(!chain->frame_generation && chain->generation_declined &&
                !chain->generation_eligible_pending && chain->generation_refusal==SwapchainEligibilityReason::budget_exhausted,
                "late async adoption overwrote the retired budget state");
            std::puts("PANEL_PRIORITY real_pixels=3 nonblocking=1 late_adoption_budget_state=PASS");
        }
        f.transport->stop();
        for(auto& chain:chains)destroy_frame_generation_swapchains(chain);
        check(f.chains.size()==base,"panel budget replay leaked native endpoints");
        {std::scoped_lock lock(g_state_mutex);for(auto h:sources)g_swapchains.erase(h);g_sessions.erase(session->handle);}
        for(auto h:sources)xr_ok(f.transport->destroy_swapchain(h),"panel budget original source cleanup");
    }
    independent_fixture_mode=false;independent_fixture_binding_queue_handoff=false;
    return 0;
}

// Native Stereo creates a new source after the AFR allocation failure. A live
// Hybrid -> OFXR revision must be able to arm that newly enumerated source.
static int independent_budget_rearm_probe(bool legacy=false) {
    xrfg_fixture::ControlProfile profile(true);
    auto controls=xrfg::autonomous::snapshot().desired;
    xrfg::autonomous::select_generation(controls,3,0);
    controls.hybrid_backend=0;controls.enabled=true;controls.ofr_enabled=false;
    controls.afw_mode=0;controls.nr_enabled=false;
    check(xrfg::autonomous::request(controls),"isolated initial Hybrid settings");
    xrfg::autonomous::apply_runtime_settings();
    independent_fixture_mode=true;independent_fixture_binding_queue_handoff=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
        dispatch->create_swapchain=private_runtime_create;dispatch->destroy_swapchain=private_runtime_destroy;
        dispatch->enumerate_swapchain_images=private_runtime_enumerate;
        dispatch->acquire_swapchain_image=private_runtime_acquire;dispatch->wait_swapchain_image=private_runtime_wait_image;
        dispatch->release_swapchain_image=private_runtime_release;
        auto session=std::make_shared<SessionState>(dispatch);session->handle=reinterpret_cast<XrSession>(2);
        session->independent_pacing=session->menu_enabled=true;session->vdxr_legacy=false;
        session->graphics_binding=SessionGraphicsBinding::d3d12;
        session->d3d12_device=f.device12;session->d3d12_queue=f.queue;session->private_delivery_queue=f.delivery_queue;
        session->private_runtime=f.transport;
        {std::scoped_lock lock(g_state_mutex);g_sessions[session->handle]=session;}
        ok(apply_embedded_control(session),"initial Hybrid application");
        session->generation_budget_exhausted=true;
        const auto source=f.make_chain();
        auto chain=std::make_shared<SwapchainState>(session,optional_surface_info());chain->handle=source;
        {std::scoped_lock lock(g_state_mutex);g_swapchains[source]=chain;}
        std::array<XrSwapchainImageD3D12KHR,3> images{};for(auto& image:images)image.type=XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        unsigned count{};xr_ok(layer_enumerate_swapchain_images_impl(source,3,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"new source enumeration during budget bypass");
        check(chain->d3d12_history && !chain->generation_eligible_pending && chain->generation_declined &&
            chain->generation_refusal==SwapchainEligibilityReason::budget_exhausted,
            "new source lost the recoverable budget refusal");
        if(legacy) {
            chain->generation_declined=false;
            chain->generation_refusal=SwapchainEligibilityReason::ready;
        }
        xrfg::autonomous::select_generation(controls,0,0);controls.enabled=true;
        check(xrfg::autonomous::request(controls),"isolated switch to optical only");
        xrfg::autonomous::apply_runtime_settings();
        ok(apply_embedded_control(session),"optical-only recovery revision");
        ProjectionResourceMapping mapping{};mapping.application_swapchain=source;
        const bool pending=projection_frame_generation_pending(session,std::span(&mapping,1));
        check(!session->generation_budget_exhausted && pending==!legacy && !chain->frame_generation,
            "budget-skipped source did not follow its exact recorded refusal state");
        XrResult handoff=XR_SUCCESS;
        if(!legacy) {
            check(ensure_projection_frame_generation(session,std::span(&mapping,1)),"launch properly marked replacement source");
            finish_session_generation_creation(session);
            check(ensure_projection_frame_generation(session,std::span(&mapping,1)) && chain->frame_generation,
                "budget-marking control failed to recreate the real generator");
        } else {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};unsigned index{};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xr_ok(layer_acquire_swapchain_image_impl(source,&acquire,&index),"replacement source acquire");
            xr_ok(layer_wait_swapchain_image_impl(source,&wait),"replacement source wait");
            xr_ok(layer_release_swapchain_image_impl(source,&release),"replacement source release and real capture");
            check(chain->last_released_capture.has_value(),"real capture missing from recovery replay");
            XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            view.pose.orientation.w=1;view.fov={-.5F,.5F,.5F,-.5F};view.subImage.swapchain=source;
            view.subImage.imageRect.extent={1,1};
            XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.viewCount=1;layer.views=&view;
            layer.space=reinterpret_cast<XrSpace>(3);
            const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=period;end.layerCount=1;end.layers=&header;
            ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"replacement projection metadata");
            auto mappings=build_projection_resource_mappings(snapshot);
            check(mappings.ready(),"replacement projection resource mapping");
            handoff=handoff_three_slot_frame(session,&end,snapshot,mappings.mappings,false);
            check(handoff==XR_ERROR_RUNTIME_FAILURE,"unarmed replacement did not reproduce the fatal submission result");
        }
        std::printf("BUDGET_REARM budget=%d history=%d enumerated=%u pending=%d declined=%d generator=%d refusal=%lld\n",
            bool(session->generation_budget_exhausted),bool(chain->d3d12_history),chain->enumerated_image_count,
            pending,chain->generation_declined,bool(chain->frame_generation),(long long)chain->generation_refusal);
        if(!legacy)std::puts("BUDGET_REARM control=1 generator_recreated=1 handoff_not_exercised");
        else std::printf("BUDGET_REARM control=0 actual_handoff=%d\n",handoff);
        f.transport->stop();
        destroy_frame_generation_swapchains(chain);
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(source);g_sessions.erase(session->handle);}
        xr_ok(f.transport->destroy_swapchain(source),"rearm source cleanup");
    }
    independent_fixture_mode=false;independent_fixture_binding_queue_handoff=false;
    return 0;
}
