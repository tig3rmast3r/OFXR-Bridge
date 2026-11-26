namespace uevr_turn_stop_fixture {
static double getter_yaw;
static int getter_bytes=sizeof(UEVR_Rotatord), getter_calls;
static UEVR_UObjectHandle manager=reinterpret_cast<UEVR_UObjectHandle>(0x22);
}

static int uevr_turn_stop_test() {
    using namespace uevr_turn_stop_fixture;
    UEVR_SDKFunctions functions{}; UEVR_UObjectFunctions object{};
    UEVR_UStructFunctions structure{}; UEVR_FPropertyFunctions property{};
    UEVR_SDKData sdk{}; UEVR_PluginInitializeParam api{};
    sdk.functions=&functions; sdk.uobject=&object; sdk.ustruct=&structure; sdk.fproperty=&property; api.sdk=&sdk;
    functions.get_player_controller=+[](int)->UEVR_UObjectHandle { return reinterpret_cast<UEVR_UObjectHandle>(0x11); };
    object.get_property_data=+[](UEVR_UObjectHandle,const wchar_t* name)->void* {
        return std::wcscmp(name,L"PlayerCameraManager")==0?&manager:nullptr;
    };
    object.get_class=+[](UEVR_UObjectHandle)->UEVR_UClassHandle { return reinterpret_cast<UEVR_UClassHandle>(0x33); };
    object.process_event=+[](UEVR_UObjectHandle,UEVR_UFunctionHandle,void* parameters) {
        ++getter_calls;
        if(getter_bytes==sizeof(UEVR_Rotatord)) {const UEVR_Rotatord value{0,getter_yaw,0};std::memcpy(parameters,&value,sizeof(value));}
        else {const UEVR_Rotatorf value{0,float(getter_yaw),0};std::memcpy(parameters,&value,sizeof(value));}
    };
    structure.find_function=+[](UEVR_UStructHandle,const wchar_t* name)->UEVR_UFunctionHandle {
        return std::wcscmp(name,L"GetCameraRotation")==0?reinterpret_cast<UEVR_UFunctionHandle>(0x44):nullptr;
    };
    structure.find_property=+[](UEVR_UStructHandle,const wchar_t* name)->UEVR_FPropertyHandle {
        return std::wcscmp(name,L"ReturnValue")==0?reinterpret_cast<UEVR_FPropertyHandle>(0x55):nullptr;
    };
    structure.get_properties_size=+[](UEVR_UStructHandle)->int {return getter_bytes;};
    property.is_return_param=+[](UEVR_FPropertyHandle)->bool {return true;};
    property.get_offset=+[](UEVR_FPropertyHandle)->int {return 0;};
    for(int size:{int(sizeof(UEVR_Rotatorf)),int(sizeof(UEVR_Rotatord))}) {
        getter_bytes=size;getter_yaw=-174.25;
        const auto read=uevr_read_game_yaw(&api);
        check(read.valid && read.yaw==getter_yaw && read.camera==0x22,"reflected game-camera getter lost float/double yaw");
    }
    const auto calls=getter_calls;getter_bytes=16;
    check(!uevr_read_game_yaw(&api).valid && getter_calls==calls,"unrecognized getter layout must bypass without calling game code");
    getter_bytes=sizeof(UEVR_Rotatord);getter_yaw=std::numeric_limits<double>::quiet_NaN();
    check(!uevr_read_game_yaw(&api).valid,"non-finite game yaw must bypass");
    getter_yaw=std::numeric_limits<double>::max();
    check(!uevr_read_game_yaw(&api).valid,"out-of-range return must not enter angle wrapping");
    check(!uevr_read_game_yaw(nullptr).valid,"missing API must preserve prediction");

    // Emulate UEVR having replaced the incoming view with its held yaw:
    // the independent game-camera sample must stay bound to this one view.
    auto& feed=uevr_camera_feed();feed.queue.clear();
    UEVR_Vector3f position{};UEVR_Rotatorf rotation{0,20,0};
    uevr_pre_view_offset(nullptr,0,100,&position,&rotation,false);
    getter_yaw=22;t_uevr_game_frame={100,GetTickCount64()};uevr_sample_game_yaw(&api);
    uevr_post_view_offset(nullptr,0,100,&position,&rotation,false);
    check(feed.queue.front() && feed.queue.front()->base_rotation[1]==20 &&
        feed.queue.front()->game_yaw_valid && feed.queue.front()->game_yaw==22 &&
        feed.queue.front()->game_frame==100,"raw game yaw was replaced by UEVR's held view");
    getter_yaw=40;t_uevr_game_frame={101,GetTickCount64()};uevr_sample_game_yaw(&api);
    check(feed.queue.front()->game_yaw==22 && feed.queue.front()->game_frame==100,
        "a later viewport overwrote the frozen source's game camera");
    const auto sampled_calls=getter_calls;t_uevr_game_frame={101,0};uevr_sample_game_yaw(&api);
    check(!t_uevr_game_yaw.valid && getter_calls==sampled_calls,"stale Draw stamp queried game objects outside its frame");
    feed.queue.clear();t_uevr_game_yaw={};t_uevr_game_frame={};

    auto session=std::make_shared<SessionState>(std::make_shared<Dispatch>());
    session->menu_enabled=true;session->hybrid_requested=true;
    session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
    std::uint64_t serial{};
    const auto pair=[&](UevrAfrOsrState& afr,double base,double actual_half_step,bool valid=true,bool fresh=true,double head_delta=0.) {
        xrfg::UevrEyeCamera camera{};camera.game_yaw_valid=valid;camera.game_camera=7;
        camera.base_rotation[1]=camera.rotation[1]=base;
        camera.game_yaw=base-10;camera.game_frame=++serial;afr.eye_releases=0;double carried{};
        check(uevr_afr_stick_yaw(*session,afr,camera,true,carried,0)==0,"first frame must retain observed engine camera");
        camera.game_yaw+=actual_half_step;if(fresh)camera.game_frame=++serial;afr.eye_releases=1;
        return uevr_afr_stick_yaw(*session,afr,camera,true,carried,head_delta);
    };
    for(bool independent:{false,true})for(bool nvidia:{false,true}) {
        session->independent_pacing=independent;
        session->optical_flow_backend=nvidia?xrfg::D3D12OpticalFlowBackend::nvidia:xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        UevrAfrOsrState afr;
        pair(afr,0,2);
        for(double base:{4.,8.,12.,16.,20.})
            check(std::abs(pair(afr,base,2)-2)<1e-6,"continuous stick turn lost its halfway camera: slideshow regression");
        check(afr.game_yaw_status&4,"moving held-frame camera source never qualified");
        check(pair(afr,24,0)==0 && (afr.game_yaw_status&4),"fresh measured stop overshot the game camera");
        check(pair(afr,24,0)==0,"stationary next pair reversed a predicted overshoot");
        check(pair(afr,24,1)==1,"turn restart ignored current measured motion");
        check(pair(afr,26,-1)==-1,"current reversal must replace previous positive velocity");
        check(pair(afr,24,0,false)==-1,"missing camera must retain original smooth prediction");
        uevr_afr_source_gap(afr);
        check(afr.game_yaw_motion_checks==0 && !afr.pair_game_valid,"source gap retained camera qualification");
    }
    // A reader that only sees the held UEVR camera must never qualify: this
    // catches the rejected r1 fix and stale getter locations before shipping.
    for(bool valid:{false,true})for(bool fresh:{false,true}) {
        UevrAfrOsrState afr;pair(afr,0,0,valid,fresh);
        for(unsigned i=1;i<12;++i)
            check(pair(afr,4.*i,0,valid,fresh)==2,"held/unavailable camera suppressed steady-turn smoothing");
        check(!(afr.game_yaw_status&4),"pair-held camera incorrectly qualified as fresh motion");
    }
    UevrAfrOsrState protected_route;pair(protected_route,0,2);
    pair(protected_route,4,2);pair(protected_route,8,2);
    check(pair(protected_route,12,0,true,true,1.)==2,"measured yaw duplicated a moving head's relocation");
    session->menu_enabled=false;
    check(pair(protected_route,16,0)==2,"OSR-only policy changed");
    session->menu_enabled=true;session->hybrid_requested=false;
    check(pair(protected_route,20,0)==2,"plain optical-flow policy changed");
    session->hybrid_requested=true;
    for(auto mode:{xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
        session->frame_generation_mode=mode;
        check(pair(protected_route,protected_route.pair_yaw+4,0)==2,"other generator's camera policy changed");
    }
    session->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;session->nvidia_options.rife_model=0;
    check(pair(protected_route,protected_route.pair_yaw+4,0)==2,"RIFE camera policy changed");
    std::puts("UEVR_TURN_STOP measured per-frame yaw: smooth turns, stop/restart/reversal, stale/held/missing camera fallback, head and other-route guards; float/double getter ABI PASS");
    return 0;
}
