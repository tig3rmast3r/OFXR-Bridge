// Protocol regression: UEVR submits an immutable dummy before its mutable scene.
// CPU-only: no device, runtime, inference provider or headset is started.
void static_projection_tests() {
    for(unsigned layout=0;layout<3;++layout) for(bool first:{false,true}) {
        std::vector<std::shared_ptr<SwapchainState>> chains;
        for(unsigned i=0;i<3;++i) {
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.width=i?6552:4;info.height=i?3004:4;info.arraySize=layout==2&&i?2:1;
            if(i==0)info.createFlags=XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
            auto c=std::make_shared<SwapchainState>(session_state,info);
            c->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x700+i));
            chains.push_back(c);
            std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;
        }
        std::array<std::array<XrCompositionLayerProjectionView,2>,2> views{};
        std::array<XrCompositionLayerProjection,2> projections{};
        for(unsigned layer=0;layer<2;++layer) {
            auto& p=projections[layer];p={XR_TYPE_COMPOSITION_LAYER_PROJECTION};p.viewCount=2;p.views=views[layer].data();
            p.space=reinterpret_cast<XrSpace>(std::uintptr_t(0x800+layer));
            for(unsigned eye=0;eye<2;++eye) {
                auto& v=views[layer][eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
                v.subImage.swapchain=chains[layer?(layout==1?eye+1:1):0]->handle;
                v.subImage.imageRect={{layer?0:int(eye*2),0},{layer?3276:2,layer?3004:4}};
                if(layer&&layout==0)v.subImage.imageRect.offset.x=eye*3276;
                if(layer&&layout==2)v.subImage.imageArrayIndex=eye;
            }
        }
        const XrCompositionLayerBaseHeader* layers[]={
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections[first?0:1]),
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections[first?1:0])};
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=2;end.layers=layers;
        ProjectionSnapshot snapshot;require(capture_projection_snapshot(&end,&snapshot),"capture mixed projection metadata");
        const auto mapped=build_projection_resource_mappings(snapshot);
        require(mapped.ready() && mapped.mappings.size()==(layout==1?2u:1u),"immutable projection incorrectly admitted as temporal source");
        std::vector<ProjectionResourceDestination> destinations;
        for(const auto& m:mapped.mappings) {
            require(m.application_swapchain!=chains[0]->handle,"static capture must never be required");
            for(const auto& ref:m.views)require(ref.projection_index==unsigned(first),"scene references changed after static filtering");
            destinations.push_back({m.application_swapchain,reinterpret_cast<XrSwapchain>(handle_value(m.application_swapchain)+0x1000)});
        }
        std::vector<std::shared_ptr<xrfg::D3D12SwapchainHistory>> histories;
        for(const auto& m:mapped.mappings) {
            auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();histories.push_back(history);
            snapshot.capture_order.push_back({m.application_swapchain,history,10,10});
        }
        const auto sources=ofr_sources(snapshot,mapped.mappings);
        require(sources[0].serial==10 && sources[1].serial==10,"AER must identify both scene eyes beside a static projection");
        const auto scene=ofr_scene_projection(snapshot,mapped.mappings);
        require(scene && *scene==unsigned(first),"AER must select the mutable scene without changing layer order");
        xrfg::ofr::SourceTracker tracker;tracker.observe(sources);
        for(unsigned frame=0;frame<12;++frame) {
            const unsigned changed=layout==1?frame%2:0;
            ++snapshot.capture_order[changed].serial;
            const auto current=ofr_sources(snapshot,mapped.mappings);
            const auto fresh=tracker.observe(current);
            require(fresh.fresh==(layout==1?(1u<<changed):3u),"AER fresh mask must use eye captures, never the static projection");
            require(!tracker.observe(current).fresh,"held polls must not advance AER cadence");
        }
        for(bool swap:{false,true}) {
            auto previous=snapshot;
            for(unsigned eye=0;eye<2;++eye) {
                previous.layers[*scene].views[eye].pose.position.x=float(eye)*.064F;
                snapshot.layers[*scene].views[eye].pose.position.x=.01F+float(eye)*.064F;
                // Unrelated static metadata must neither invalidate scene cameras
                // nor receive the AER endpoint camera correction.
                previous.layers[!*scene].views[eye].pose.position.x=-100;
                snapshot.layers[!*scene].views[eye].pose.position.x=100;
            }
            const auto camera=synthetic_camera_snapshot(previous,snapshot,.5F,scene);
            require(camera.has_value(),"static camera movement must not reject scene interpolation");
            require(camera->layers[!*scene].views[0].pose.position.x==100,"static projection camera was interpolated");
            GeneratedFrameEndInfo early,late;
            require(build_generated_frame_end_info(&end,snapshot,true,destinations,&early),"AER earlier output metadata");
            require(build_generated_frame_end_info(&end,snapshot,false,destinations,&late),"AER later output metadata");
            // Source pointers still carry original app poses; the endpoint correction
            // must use the selected previous scene, never static metadata.
            const auto static_pose=early.projections[!*scene].views[0].pose.position.x;
            const auto newer=xrfg::ofr::newer_eye(swap);
            const auto endpoint=early.projections[*scene].views[newer].pose.position.x;
            require(realign_ofr_paired_metadata(early,late,previous,swap,*scene),"paired AER metadata with extra immutable projection");
            require(early.projections[*scene].views[newer].pose.position.x==previous.layers[*scene].views[newer].pose.position.x &&
                late.projections[*scene].views[newer].pose.position.x==endpoint,"AER endpoint cameras attached to wrong layer");
            require(early.projections[!*scene].views[0].pose.position.x==static_pose &&
                late.projections[!*scene].views[0].pose.position.x==static_pose,"paired AER altered static projection metadata");
        }
        auto ambiguous=snapshot;ambiguous.layers[!*scene].views=ambiguous.layers[*scene].views;
        require(!ofr_scene_projection(ambiguous,mapped.mappings),"second mutable scene incorrectly accepted for AER");
        for(bool synthetic:{false,true}) {
            GeneratedFrameEndInfo output;
            require(build_generated_frame_end_info(&end,snapshot,synthetic,destinations,&output),"mutable scene with immutable projection must build");
            require(output.info.layerCount==2,"immutable projection dropped");
            const auto& fixed=output.projections[first?0:1];
            require(fixed.layer.space==projections[0].space,"immutable projection space changed");
            for(unsigned eye=0;eye<2;++eye)require(matching_sub_image(fixed.views[eye].subImage,views[0][eye].subImage),"immutable projection subimage changed");
            auto missing=destinations;missing.pop_back();
            require(!build_generated_frame_end_info(&end,snapshot,synthetic,missing,&output),"missing mutable output incorrectly passed through");
        }
        // Unknown/malformed static resources must still fail validation.
        auto invalid=snapshot;invalid.layers[first?0:1].views[0].subImage.imageRect.extent.width=8;
        require(!build_projection_resource_mappings(invalid).ready(),"invalid static rectangle silently accepted");
        {std::scoped_lock lock(g_state_mutex);g_swapchains.clear();}
    }
    std::cout<<"PASS: immutable projection preserved; packed/split/array scenes; both layer orders; real/generated/AER output; fresh/held/paired/swap/cameras; mutable refusal retained\n";
}
