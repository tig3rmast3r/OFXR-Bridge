// Actual D11 guide transport, protected publication registry and release/capture
// metadata resolver. All source images are black 8x8 textures: no barcode fits.
void daysgone_eye_metadata_test() {
    using Microsoft::WRL::ComPtr;
    const auto hr=[](HRESULT result,const char* text){check(SUCCEEDED(result),text);};
    using xrfg::LukeRenderIdentity;
    const std::optional<LukeRenderIdentity> selected=LukeRenderIdentity{0xD600,100};
    check(xrfg::stable_real_vr_render_identity(selected,selected)==selected,
        "stable native render tags the actual evaluation");
    check(!xrfg::stable_real_vr_render_identity(selected,{})&&
        !xrfg::stable_real_vr_render_identity({},selected)&&
        !xrfg::stable_real_vr_render_identity(selected,LukeRenderIdentity{0xD600,101})&&
        !xrfg::stable_real_vr_render_identity(selected,LukeRenderIdentity{0xD601,100}),
        "missing/changed native owner or render must not be tagged");
    check(xrfg::native_ingress::daysgone_camera_process(),"fixture must execute as DaysGone.exe");
    ComPtr<ID3D11Device> game;ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
        &game,nullptr,&context),"metadata game source");
    std::shared_ptr<xrfg::D3D11GuideCaptureOwner> lease;
    hr(xrfg::create_d3d11_guide_capture_owner(game.Get(),lease),"metadata native source owner");
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;std::uint64_t generation{};
    hr(xrfg::acquire_d3d11_guide_owner(game.Get(),&device,&queue,&generation),"metadata capture context");
    xrfg::native_ingress::configure_queue(queue.Get());xrfg::configure_dlss_motion_vector_tracking(true);
    const auto texture=[&](DXGI_FORMAT format){
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=1;d.Format=format;
        d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> value;hr(game->CreateTexture2D(&d,nullptr,&value),"metadata texture");
        ComPtr<ID3D11RenderTargetView> target;hr(game->CreateRenderTargetView(value.Get(),nullptr,&target),"metadata clear target");
        const float black[4]{};context->ClearRenderTargetView(target.Get(),black);return value;
    };
    auto color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT),motion=texture(DXGI_FORMAT_R16G16_FLOAT),depth=texture(DXGI_FORMAT_R32_FLOAT);
    xrfg::D3D11GuideTransport transport;hr(transport.initialize(context.Get(),device.Get(),queue.Get()),"metadata transport");
    auto owner=session();owner->d3d12_device=device;owner->d3d12_queue=queue;
    std::array<std::shared_ptr<SwapchainState>,2> eyes;
    std::array<xrfg::D3D12HistoryCaptureTicket,2> captures{};
    std::array<std::uint64_t,2> renders{},publications{};
    xrfg::D3D12HistoryCaptureTicket delayed_capture{};std::uint64_t delayed_publication{};
    xrfg::DlssMotionVectorPublication last;
    const auto release=[&](unsigned eye,const xrfg::D3D12HistoryCaptureTicket& capture,std::uint64_t render,
        std::uint64_t source_owner=0xD600,std::uint32_t producer=OFXR_EYE_SOURCE_REAL_VR,std::uint32_t token=0){
        auto& chain=*eyes[eye];chain.last_released_capture=capture;
        const OfxrEyeFrameMetadataV1 frame{sizeof(OfxrEyeFrameMetadataV1),producer,capture.source_index,1,
            {{source_owner,render,token,eye}}};
        record_eye_frame_release(chain,capture.source_index,&frame);chain.released_eye_frame.capture_serial=capture.serial;
    };
    const auto lookup=[&](unsigned eye,const xrfg::D3D12HistoryCaptureTicket& capture){
        return resolve_eye_metadata(*eyes[eye],capture,eye,queue.Get(),device.Get(),EyeMetadataLookup::release_identity_only);
    };
    const auto publish=[&](unsigned n,bool reset=false,std::uint64_t stream=0xD60008,std::uint64_t native_owner=0xD600,
        std::uint64_t native_render=0,bool direct=true){
        hr(transport.begin(),"metadata native begin");
        hr(transport.copy(xrfg::D3D11GuideTransport::output,color.Get()),"metadata native output");
        hr(transport.copy(xrfg::D3D11GuideTransport::motion,motion.Get()),"metadata native vectors");
        hr(transport.copy(xrfg::D3D11GuideTransport::depth,depth.Get()),"metadata native depth");
        xrfg::DlssMotionVectorPublication p;p.stream=stream;p.producer_queue=queue.Get();p.producer_command_list=transport.list();
        p.verified_producer_device=device.Get();p.output=transport.resource(xrfg::D3D11GuideTransport::output);
        p.motion_vectors=transport.resource(xrfg::D3D11GuideTransport::motion);p.depth=transport.resource(xrfg::D3D11GuideTransport::depth);
        p.output_width=p.output_height=p.motion_width=p.motion_height=p.depth_width=p.depth_height=8;
        p.resource_state=p.output_resource_state=p.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
        xrfg::daysgone_camera::NativeCamera native;
        native.projection[0]=native.projection[5]=1;native.projection[11]=1;native.projection[14]=10;
        native.translated_view[0]=native.translated_view[5]=native.translated_view[10]=native.translated_view[15]=1;
        native.origin[0]=n%2?2.85f:-2.85f;
        xrfg::native_ingress::streamline_camera::Camera camera;xrfg::daysgone_camera::Rejection reason;
        check(xrfg::daysgone_camera::decode_native(native,camera,reason),"metadata native camera decode");
        camera.previous=camera.to_current=xrfg::daysgone_camera::store(DirectX::XMMatrixIdentity());
        xrfg::daysgone_camera::Packet packet;packet.frame=10000+n;packet.sequence=5000+n;packet.flags=n>1?1:0;
        xrfg::daysgone_camera::apply(p,packet,camera,reinterpret_cast<std::uintptr_t>(game.Get()),true);p.reset|=reset;
        // Exercise the real producer reader before the selected field exists.
        // The runtime object and compositor queue deliberately have different
        // identities, reproducing the owner mismatch in the r8 game run.
        namespace rb=xrfg::native_ingress::daysgone_render_binding;
        const rb::Layout tags{0x100,0x200,0,0,0x108,0x100,0x130,0x5c,0x68,0x800,0x100,0x808,0x100};
        constexpr std::uintptr_t module=0x100000,runtime=0x200000,renderer=0x300000,config=0x400000;
        std::array<std::array<unsigned char,0x1000>,4> data{};
        const auto put=[&](unsigned block,std::size_t at,const auto& value){std::memcpy(data[block].data()+at,&value,sizeof(value));};
        put(0,tags.owner,std::uint64_t{runtime});put(0,tags.config,std::uint64_t{config});
        put(1,0,std::uint64_t{module+tags.vtable});put(1,tags.selected,UINT64_MAX);
        put(1,tags.renderer,std::uint64_t{renderer});put(2,tags.compositor_owner,native_owner);
        const auto render=native_render?native_render:100+n;
        for(unsigned slot=0;slot<4;++slot)put(1,tags.ring+slot*tags.stride,UINT64_MAX);
        const auto record=tags.ring+(render&3)*tags.stride;put(1,record,render);put(1,record+tags.origin,p.camera_position);
        const auto read=[&](std::uintptr_t address,void* out,std::size_t count){
            const auto block=address/0x100000;const auto offset=address%0x100000;
            if(block<1||block>4||offset>0x1000||count>0x1000-offset)return false;
            std::memcpy(out,data[block-1].data()+offset,count);return true;
        };
        const auto observed=rb::read_identity(module,tags,p.camera_position,read);
        check(observed&&observed->object==native_owner&&observed->object!=runtime&&observed->render==render,
            "producer must use the exact camera and eye-release queue namespace before selection");
        if(const auto identity=xrfg::stable_real_vr_render_identity(observed,observed))
            p.eye_render={identity->object,identity->render,OFXR_EYE_SOURCE_REAL_VR,direct};
        auto frame=xrfg::publish_dlss_motion_vectors(p);check(bool(frame),"metadata protected guide publication");
        check(frame->compose_native_object_history&&frame->osr_native_cabin_motion&&frame->raw_motion_native_span_confirmed,
            "exact metadata publication must retain the full-OSR original N1 contract");
        check(frame->osr_depth_alignment==std::optional{std::array<float,2>{0,0}},
            "metadata publication must retain its producer's depth/colour alignment");
        check(frame->raw_motion_color_aligned,"immutable metadata guide must retain original colour correspondence");
        hr(transport.submit(),"metadata actual GPU submission");
        hr(transport.copy_back(xrfg::D3D11GuideTransport::output,color.Get()),"metadata unmarked native copyback");
        last=p;return frame;
    };
    for(unsigned n=1;n<=6;++n) {
        const auto frame=publish(n);const unsigned eye=n%2;
        if(!eyes[eye]) {
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.width=info.height=8;info.arraySize=1;
            eyes[eye]=std::make_shared<SwapchainState>(owner,info);eyes[eye]->handle=reinterpret_cast<XrSwapchain>(0xD610+eye);
            eyes[eye]->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
            ID3D12Resource* image[]{transport.resource(xrfg::D3D11GuideTransport::output)};
            hr(eyes[eye]->d3d12_history->initialize(device.Get(),queue.Get(),image,D3D12_RESOURCE_STATE_COMMON),"metadata actual eye history");
        }
        hr(eyes[eye]->d3d12_history->capture(0,&captures[eye]),"metadata frozen eye capture");
        hr(eyes[eye]->d3d12_history->commit(captures[eye]),"metadata eye capture commit");
        renders[eye]=100+n;publications[eye]=frame->publication;release(eye,captures[eye],renders[eye]);
        if(n==3){delayed_capture=captures[eye];delayed_publication=frame->publication;}
        const auto guide=lookup(eye,captures[eye]);
        if(n==1)check(!guide,"metadata does not fabricate native temporal history during priming");
        else {
            check(guide&&guide->publication==frame->publication&&guide->camera_frame_token==10000+n,
                "native render joins exact game camera despite unrelated frame counters");
            check(guide->raw_motion_color_aligned&&guide->osr_depth_alignment.has_value(),
                "resolved OSR source must retain its original colour and depth-raster contracts separately");
            check(!eyes[eye]->eye_guide_bindings[eye].confirmed&&!eyes[eye]->repeated_eye_guides[eye].token,
                "native metadata needs no barcode anchors or repeated-token cache");
            xrfg::EyeIdentitySource source{};
            auto chosen=select_luke_eye_guide(*eyes[eye],captures[eye],eye,guide,false,0,S_OK,queue.Get(),device.Get(),&source);
            check(chosen&&source==xrfg::EyeIdentitySource::metadata,"OSR selects metadata without a pixel read");
        }
        if(n>=3) {
            std::array<std::uint64_t,2> pair{};
            for(unsigned i=0;i<2;++i) {
                const auto native=lookup(i,captures[i]);check(bool(native),"metadata resolves both frozen eyes");pair[i]=native->publication;
            }
            check(bool(xrfg::select_luke_afw_color_source(pair,0)),"metadata-only pair reaches the OSR source selector");
        }
    }
    release(1,delayed_capture,103);
    auto delayed=lookup(1,delayed_capture);
    check(delayed&&delayed->publication==delayed_publication&&delayed->publication!=publications[1],
        "delayed native render selects its frozen publication instead of newer ingress");
    release(1,delayed_capture,103);check(lookup(1,delayed_capture)->publication==delayed_publication,
        "repeated exact metadata works without a marker cache");
    release(1,captures[1],renders[1]);
    const auto original=lookup(0,captures[0]);check(bool(original),"metadata final left eye");
    auto wrong=captures[0];++wrong.slot;
    check(!lookup(0,wrong)&&!resolve_eye_metadata(*eyes[0],captures[0],1,queue.Get(),device.Get()),"metadata cannot cross capture slot or eye");
    release(0,captures[0],renders[0],0xD601);check(!lookup(0,captures[0]),"foreign native owner refused");
    release(0,captures[0],renders[0],0xD600,OFXR_EYE_SOURCE_MUTAR);check(!lookup(0,captures[0]),"foreign native producer refused");
    release(0,captures[0],999999);
    eyes[0]->repeated_eye_guides[0]={0xD600,999999,original->stream,original->epoch,OFXR_EYE_SOURCE_REAL_VR,original->color_origin_token};
    check(!lookup(0,captures[0]),"missing native render cannot fall back to a cached barcode or newest guide");
    release(0,captures[0],renders[0]);check(lookup(0,captures[0])->publication==original->publication,"exact render ignores unrelated stale anchor");
    release(0,captures[0],renders[0],0xD600,OFXR_EYE_SOURCE_REAL_VR,original->color_origin_token+1);
    check(!lookup(0,captures[0]),"contradictory explicit release token is rejected");
    release(0,captures[0],renders[0]);
    publish(7,false,0xD6000A,0xD600,renders[0],false);
    check(lookup(0,captures[0])->publication==original->publication,"unqualified legacy tags do not enter the direct metadata join");
    xrfg::retire_dlss_motion_vector_stream(0xD6000A);
    publish(7,false,0xD60009,0xD600,renders[0]);
    check(!lookup(0,captures[0]),"duplicate native render across NGX streams is ambiguous");
    xrfg::retire_dlss_motion_vector_stream(0xD60009);
    check(lookup(0,captures[0])->publication==original->publication,"retiring duplicate restores the exact metadata join");
    publish(8,true);
    check(!lookup(0,captures[0]),"reset epoch cannot reuse a previous render or cached token");
    hr(transport.drain(),"metadata writer retirement");
    xrfg::retire_dlss_motion_vector_stream(0xD60008);
    std::cout<<"Days Gone metadata: unmarked D11 output, exact release/capture join and refusal cases PASS\n";
}
