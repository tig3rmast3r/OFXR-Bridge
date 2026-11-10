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
    std::cout<<"PASS: immutable projection preserved; packed/split/array scenes; both layer orders; real/generated output; mutable refusal retained\n";
}
