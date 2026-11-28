// Exercise the source-owned capture with the production Execute hook installed.
void daysgone_capture_context_test() {
    using Microsoft::WRL::ComPtr;
    const auto hr=[](HRESULT result,const char* message){check(SUCCEEDED(result),message);};
    ComPtr<ID3D11Device> game;ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&game,nullptr,&context),"capture source device");
    std::shared_ptr<xrfg::D3D11GuideCaptureOwner> lease;
    hr(xrfg::create_d3d11_guide_capture_owner(game.Get(),lease),"source capture owner");
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;std::uint64_t generation{};
    hr(xrfg::acquire_d3d11_guide_owner(game.Get(),&device,&queue,&generation),"capture context");
    xrfg::native_ingress::configure_queue(queue.Get());
    xrfg::configure_dlss_motion_vector_tracking(true);
    const auto texture=[&](DXGI_FORMAT format){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=8;desc.MipLevels=desc.ArraySize=1;
        desc.Format=format;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> value;hr(game->CreateTexture2D(&desc,nullptr,&value),"capture texture");return value;
    };
    auto depth=texture(DXGI_FORMAT_R32_FLOAT),motion=texture(DXGI_FORMAT_R16G16_FLOAT),color=texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
    xrfg::D3D11GuideTransport transport;hr(transport.initialize(context.Get(),device.Get(),queue.Get()),"capture transport");
    hr(transport.begin(),"capture begin");
    hr(transport.copy(xrfg::D3D11GuideTransport::depth,depth.Get()),"capture depth");
    hr(transport.copy(xrfg::D3D11GuideTransport::motion,motion.Get()),"capture motion");
    hr(transport.copy(xrfg::D3D11GuideTransport::output,color.Get()),"capture output");
    xrfg::DlssMotionVectorPublication p;
    p.stream=90991;p.output=transport.resource(xrfg::D3D11GuideTransport::output);
    p.motion_vectors=transport.resource(xrfg::D3D11GuideTransport::motion);p.depth=transport.resource(xrfg::D3D11GuideTransport::depth);
    p.producer_queue=queue.Get();p.producer_command_list=transport.list();p.verified_producer_device=device.Get();
    p.output_width=p.output_height=p.motion_width=p.motion_height=p.depth_width=p.depth_height=8;
    p.resource_state=p.depth_resource_state=p.output_resource_state=D3D12_RESOURCE_STATE_COMMON;
    p.camera_data_valid=p.camera_matrices_valid=p.camera_frame_token_valid=p.protect_snapshot_lifetime=true;
    p.camera_frame_token=p.source_clock_sequence=1;p.source_clock_owner=reinterpret_cast<std::uintptr_t>(game.Get());
    p.camera_near=10;p.camera_far=0;p.depth_inverted=p.depth_infinite=true;
    p.camera_right={1,0,0};p.camera_up={0,1,0};p.camera_forward={0,0,1};p.camera_fov_vertical=1.5707963f;
    p.camera_view_to_clip={1,0,0,0,0,1,0,0,0,0,0,1,0,0,10,0};
    p.camera_clip_to_view={1,0,0,0,0,1,0,0,0,0,0,.1f,0,0,1,0};
    const auto frame=xrfg::publish_dlss_motion_vectors(p);check(bool(frame),"camera guide publication");
    check(xrfg::claim_d3d11_guides(game.Get(),generation),"capture claim");
    std::uint32_t rejection{};
    check(!xrfg::admit_d3d11_luke_camera_context(game.Get(),generation,frame,&rejection)&&rejection==104,
        "pending GPU camera reports the missing submission receipt");
    hr(transport.submit(),"production-hooked submission");
    hr(transport.copy_back(xrfg::D3D11GuideTransport::output,color.Get()),"production-hooked copyback");
    check(!xrfg::admit_d3d11_luke_camera_context(game.Get(),generation+1,frame,&rejection)&&rejection==4,
        "foreign generation reports its precise admission failure");
    check(xrfg::admit_d3d11_luke_camera_context(game.Get(),generation,frame,&rejection)&&!rejection,
        "production-hooked camera admits OSR");
    check(xrfg::native_ingress::statistics().submissions>0,"production Execute hook actually observed the test");
    hr(transport.drain(),"capture retirement");xrfg::retire_dlss_motion_vector_stream(p.stream);
}

// Follow the marker through the real D12->D11 game output and cropped eye,
// then back through the eye transport. Use the dimensions from PID 60336.
void daysgone_capture_color_test() {
    using Microsoft::WRL::ComPtr;
    const auto hr=[](HRESULT result,const char* message){check(SUCCEEDED(result),message);};
    ComPtr<ID3D11Device> game;ComPtr<ID3D11DeviceContext> context;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&game,nullptr,&context),"origin D11 device");
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    hr(xrfg::create_d3d12_device_for_d3d11(game.Get(),&device,&queue),"origin bridge");
    const auto texture=[&](unsigned width){
        D3D11_TEXTURE2D_DESC d{};d.Width=width;d.Height=2688;d.MipLevels=d.ArraySize=1;
        d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> value;hr(game->CreateTexture2D(&d,nullptr,&value),"origin texture");
        ComPtr<ID3D11RenderTargetView> target;hr(game->CreateRenderTargetView(value.Get(),nullptr,&target),"origin clear target");
        const float black[4]{};context->ClearRenderTargetView(target.Get(),black);return value;
    };
    auto output=texture(3584),eye=texture(3202);
    xrfg::D3D11GuideTransport writer,reader;
    hr(writer.initialize(context.Get(),device.Get(),queue.Get()),"origin writer transport");
    hr(reader.initialize(context.Get(),device.Get(),queue.Get()),"origin reader transport");
    hr(writer.begin(),"origin writer begin");
    hr(writer.copy(xrfg::D3D11GuideTransport::output,output.Get()),"origin source copy");
    xrfg::configure_luke_color_origin_extent(3202,2688);
    constexpr std::uint32_t expected=0x13579bdf;
    hr(xrfg::mark_luke_color_origin(writer.list(),writer.resource(xrfg::D3D11GuideTransport::output),expected,
        D3D12_RESOURCE_STATE_COMMON,device.Get(),{0,0,3584,2688}),"origin marker written at actual eye extent");
    hr(writer.submit(),"origin writer submission");
    hr(writer.copy_back(xrfg::D3D11GuideTransport::output,output.Get()),"origin marker copied back to game");
    for(unsigned crop:{0U,382U}) {
        D3D11_BOX box{crop,0,0,crop+3202,2688,1};
        context->CopySubresourceRegion(eye.Get(),0,0,0,0,output.Get(),0,&box);
        hr(reader.begin(),"origin eye begin");
        hr(reader.copy(xrfg::D3D11GuideTransport::output,eye.Get()),"origin cropped eye transport");
        hr(reader.submit(),"origin eye submission");
        std::uint32_t token{};
        hr(xrfg::read_luke_color_origin(queue.Get(),reader.resource(xrfg::D3D11GuideTransport::output),&token,
            D3D12_RESOURCE_STATE_COMMON,1000),"origin decoded after real game/eye round trip");
        check(token==expected,"origin survives both crop extremes without choosing a latest guide");
    }
    hr(reader.drain(),"origin reader retirement");hr(writer.drain(),"origin writer retirement");
}
