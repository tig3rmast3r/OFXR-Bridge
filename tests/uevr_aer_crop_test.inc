static int uevr_aer_crop_test() {
    // Actual DI2 submitted crops and full SR/motion dimensions from pid44300.
    constexpr unsigned width=4124,height=3304;
    const xrfg::DlssEyeView crops[2]{{0,0,0,0,0,171,3525,3133},{0,0,0,0,598,171,3526,3133}};
    const auto guides=[](unsigned eye) {
        auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();
        g->stream=10+eye;g->epoch=2;g->serial=7;g->previous_serial=6;g->publication=20+eye;
        g->output_width=width;g->output_height=height;
        g->motion_width=2750;g->motion_height=2203;g->depth_width=1375;g->depth_height=1102;
        g->scale_x=-1;g->scale_y=1; // preserve signed vector magnitudes
        auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=1;set->resolved_eye=int(eye);set->eyes[0]=g;
        return set;
    };
    for(unsigned eye=0;eye<2;++eye) {
        auto input=guides(eye);const auto& original=*input->eyes[0];
        check(!xrfg::place_luke_aer_guides(input,{&crops[eye],1}),"old AER geometry must reproduce the missing-camera refusal");
        const auto bound=xrfg::bind_uevr_aer_guides(input,width,height,crops[eye],eye);
        check(bound && bound->eyes[0]==input->eyes[0] && !bound->eyes[0]->placed &&
            bound->eyes[0]->output_width==width,"binding must leave full native grids intact for composition");
        const auto placed=xrfg::place_uevr_aer_guides(bound,width,height,crops[eye],eye);
        check(placed && placed->eyes[0]!=input->eyes[0],"recorded UEVR crop did not place");
        const auto& g=*placed->eyes[0];const auto m=xrfg::guide_motion_rect(g),d=xrfg::guide_depth_rect(g);
        const auto close=[](double a,double b){return std::abs(a-b)<.001;};
        check(close(m[0],double(crops[eye].x)*2750/width) && close(m[1],171.*2203/height) &&
            close(m[2],double(crops[eye].width)*2750/width) && close(m[3],3133.*2203/height),
            "motion sampling does not follow the literal colour crop");
        check(close(d[0],double(crops[eye].x)*1375/width) && close(d[1],171.*1102/height) &&
            close(d[2],double(crops[eye].width)*1375/width) && close(d[3],3133.*1102/height),
            "depth with another resolution does not cover the same colour pixels");
        check(g.stream==original.stream && g.epoch==original.epoch && g.serial==original.serial &&
            g.previous_serial==original.previous_serial && g.publication==original.publication &&
            g.scale_x==original.scale_x && g.scale_y==original.scale_y && !g.camera_matrices_valid,
            "crop fabricated a camera or changed temporal/vector identity");
        check(xrfg::place_uevr_aer_guides(placed,width,height,crops[eye],eye)==placed,
            "held input cropped twice");
        auto bad=crops[eye];bad.x=-1;
        check(!xrfg::bind_uevr_aer_guides(input,width,height,bad,eye),"negative crop accepted");
        bad=crops[eye];bad.width=width+1;
        check(!xrfg::bind_uevr_aer_guides(input,width,height,bad,eye),"out-of-image crop accepted");
        check(!xrfg::bind_uevr_aer_guides(input,width+1,height,crops[eye],eye),"different full-image domain guessed by size");
        check(!xrfg::bind_uevr_aer_guides(input,width,height,crops[eye],1-eye),"opposite eye accepted");
        auto comparison=std::make_shared<xrfg::DlssMotionVectorSet>(*input);comparison->alternate=guides(1-eye)->eyes[0];
        const auto compared=xrfg::bind_uevr_aer_guides(comparison,width,height,crops[eye],eye);
        check(compared && compared->eyes[0]==input->eyes[0] && compared->alternate==comparison->alternate,
            "release-assigned primary was refused because of its eye-order comparison stream");
        check(xrfg::place_uevr_aer_guides(compared,width,height,crops[eye],eye)->alternate==comparison->alternate,
            "consumer placement changed the comparison stream");
        comparison->resolved_eye=-1;
        check(!xrfg::bind_uevr_aer_guides(comparison,width,height,crops[eye],eye),"unresolved stream alternatives accepted");
        auto motion_only=std::make_shared<xrfg::DlssMotionVectorFrame>(original);
        motion_only->depth_width=motion_only->depth_height=0;
        auto no_depth=std::make_shared<xrfg::DlssMotionVectorSet>(*input);no_depth->eyes[0]=motion_only;
        check(bool(xrfg::bind_uevr_aer_guides(no_depth,width,height,crops[eye],eye)),
            "crop adapter imposed a new depth requirement on motion-only Hybrid");
        check(!original.placed && original.output_width==width,"native guide was modified in place");
    }
    // Shipping EndFrame metadata transaction, with committed-capture identities.
    // No GPU work is needed to decorate the already bound native guides.
    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    session->uevr_afr=make_uevr_afr_osr_state();session->graphics_binding=SessionGraphicsBinding::d3d12;
    session->menu_enabled=true;session->nvidia_options.ofr=true;session->hybrid_requested=true;
    check(uevr_aer_crop_requested(*session),"UEVR Hybrid AER adapter not selected");
    std::array<std::shared_ptr<SwapchainState>,2> chains;
    std::array<ProjectionResourceMapping,2> mappings;
    ProjectionSnapshot snapshot;snapshot.layers.resize(1);snapshot.layers[0].views.resize(2);
    struct Cleanup {std::array<std::shared_ptr<SwapchainState>,2>& chains;
        ~Cleanup(){std::scoped_lock lock(g_state_mutex);for(auto& c:chains)if(c)g_swapchains.erase(c->handle);}} cleanup{chains};
    for(unsigned eye=0;eye<2;++eye) {
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=width;info.height=height;info.arraySize=1;
        auto c=std::make_shared<SwapchainState>(session,info);c->handle=reinterpret_cast<XrSwapchain>(0xaec0+eye);
        c->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        xrfg::D3D12HistoryCaptureTicket capture{};capture.serial=eye+1;capture.fence_value=eye+11;capture.source_index=0;
        c->last_released_capture=capture;c->last_released_index=0;c->last_released_motion_vectors=guides(eye);
        chains[eye]=c;{std::scoped_lock lock(g_state_mutex);g_swapchains[c->handle]=c;}
        mappings[eye].application_swapchain=c->handle;mappings[eye].views.push_back({0,eye});
        auto& v=snapshot.layers[0].views[eye];v.subImage.swapchain=c->handle;
        v.subImage.imageRect={{crops[eye].x,crops[eye].y},{int(crops[eye].width),int(crops[eye].height)}};
    }
    record_projection_capture_order(snapshot,mappings);
    check(prepare_uevr_aer_crop_guides(*session,snapshot,mappings)==S_OK,"real metadata transaction refused recorded crops");
    const auto held=chains[0]->last_released_motion_vectors;
    check(held->uevr_aer_image_extent==std::array<std::uint32_t,2>{width,height},"transaction did not mark whole-image domain");
    check(prepare_uevr_aer_crop_guides(*session,snapshot,mappings)==S_OK && chains[0]->last_released_motion_vectors==held,
        "held source allocated a different crop binding");
    chains[1]->last_released_capture->serial++;
    check(prepare_uevr_aer_crop_guides(*session,snapshot,mappings)==S_FALSE && chains[0]->last_released_motion_vectors==held,
        "stale capture was accepted or partially published");
    chains[1]->last_released_capture->serial--;chains[1]->last_released_motion_vectors.reset();
    check(prepare_uevr_aer_crop_guides(*session,snapshot,mappings)==S_FALSE && chains[0]->last_released_motion_vectors==held,
        "missing peer changed the valid eye's metadata");
    session->nvidia_options.ofr=false;check(!uevr_aer_crop_requested(*session),"ordinary Hybrid entered AER adapter");
    session->nvidia_options.ofr=true;session->luke_afw_enabled=true;
    check(!uevr_aer_crop_requested(*session),"Luke entered UEVR adapter");
    session->luke_afw_enabled=false;session->nvidia_options.rife_model=0;
    check(!uevr_aer_crop_requested(*session),"RIFE entered native guide adapter");
    std::puts("UEVR_AER_CROP recorded DI2 windows, unchanged native grids/identity, temporal metadata, held idempotence and atomic release binding PASS");
    return 0;
}
