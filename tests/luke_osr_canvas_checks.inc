namespace quality {
void osr_canvas_checks(ID3D12Device* device,ID3D12CommandQueue* queue,bool unequal_height,unsigned first_eye) {
    constexpr UINT guide_size=64,output_size=128;
    const auto texture=[&](UINT width,UINT height,DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;
        ComPtr<ID3D12Resource> out;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&out)),"canvas texture");
        return out;
    };
    std::array colors{texture(65,64,DXGI_FORMAT_R8G8B8A8_TYPELESS),
        texture(63,unequal_height?63:64,DXGI_FORMAT_R8G8B8A8_TYPELESS)};
    auto depth=texture(guide_size,guide_size,DXGI_FORMAT_R32_FLOAT);
    auto motion=texture(guide_size,guide_size,DXGI_FORMAT_R16G16_FLOAT);
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"canvas allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"canvas list");
    std::vector<ComPtr<ID3D12Resource>> uploads;
    const std::array targets{colors[0].Get(),colors[1].Get(),depth.Get(),motion.Get()};
    for(unsigned kind=0;kind<targets.size();++kind) {
        const auto d=targets[kind]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&d,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=bytes;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"canvas upload");
        void* data{};check(upload->Map(0,nullptr,&data),"canvas map");std::memset(data,0,static_cast<SIZE_T>(bytes));
        for(UINT y=0;y<d.Height;++y)for(UINT x=0;x<d.Width;++x) {
            auto* pixel=static_cast<unsigned char*>(data)+footprint.Offset+y*footprint.Footprint.RowPitch+x*4;
            if(kind<2) {const unsigned char rgba[]{80,120,160,255};std::memcpy(pixel,rgba,4);}
            else if(kind==2) {const float z=.1F/3.F;std::memcpy(pixel,&z,4);}
        }
        upload->Unmap(0,nullptr);
        auto b=transition(targets[kind],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1,&b);
        D3D12_TEXTURE_COPY_LOCATION source{},destination{};
        source.pResource=upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;
        destination.pResource=targets[kind];destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
        uploads.push_back(upload);
    }
    check(list->Close(),"canvas upload close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"canvas fence");
    const auto wait=[&](UINT64 value) {
        check(queue->Signal(fence.Get(),value),"canvas signal");
        check(fence->SetEventOnCompletion(value,nullptr),"canvas wait");
    };
    wait(1);
    xrfg::LukeAfwSynthesizer synth;check(synth.initialize(device,queue,{}),"canvas native initialization");
    xrfg::bridge_flight_logger().flush();
    const auto log_offset=std::filesystem::file_size(xrfg::bridge_flight_logger().log_path());
    std::vector<std::shared_ptr<DlssMotionVectorFrame>> frames;
    for(unsigned n=1;n<=10;++n) {
        const unsigned eye=((n-1)%2)^first_eye;
        const auto current=camera(n,scenes[0]);
        auto frame=std::make_shared<DlssMotionVectorFrame>();auto& guide=*frame;
        guide.stream=guide.epoch=1;guide.serial=guide.publication=n;
        guide.camera_data_valid=guide.camera_matrices_valid=true;
        guide.camera_position=current.packet.position;guide.camera_right=current.packet.right;
        guide.camera_up=current.packet.up;guide.camera_forward=current.packet.forward;
        guide.camera_view_to_clip=current.packet.projection;guide.camera_clip_to_view=current.packet.inverse_projection;
        guide.camera_near=current.packet.near_plane;guide.camera_far=current.packet.far_plane;
        guide.depth_inverted=guide.depth_infinite=true;
        guide.output_width=guide.output_height=output_size;
        guide.depth_width=guide.depth_height=guide.motion_width=guide.motion_height=guide_size;
        guide.scale_x=guide.scale_y=1;guide.jitter_x=.125F;guide.jitter_y=-.25F;
        guide.calibrated_game_eye_baseline=static_cast<float>(baseline);
        guide.depth=depth;guide.motion_vectors=motion;
        guide.motion_retention=std::make_shared<xrfg::DlssSnapshotRetention>();
        guide.depth_retention=std::make_shared<xrfg::DlssSnapshotRetention>();
        guide.producer_queue=queue;
        guide.verified_producer_device=guide.motion_snapshot_device=guide.depth_snapshot_device=device;
        if(n>=2)guide.previous_opposite_guide=frames[n-2];
        if(n>=3)guide.previous_same_guide=frames[n-3];
        frames.push_back(frame);if(n<3)continue;
        xrfg::D3D12ReprojectionView source{},destination{};
        source.pose.orientation.w=destination.pose.orientation.w=1;
        source.pose.position.x=eye?float(baseline*.5):float(-baseline*.5);
        destination.pose.position.x=-source.pose.position.x;
        const std::array horizontal{std::atan(65.F/output_size),std::atan(63.F/output_size)};
        const std::array vertical{std::atan(64.F/output_size),
            std::atan((unequal_height?63.F:64.F)/output_size)};
        source.fov={-horizontal[eye],horizontal[eye],vertical[eye],-vertical[eye]};
        destination.fov={-horizontal[1-eye],horizontal[1-eye],vertical[1-eye],-vertical[1-eye]};
        xrfg::embedded::AfwSettings settings{};settings.mode=3;
        settings.osr=static_cast<int>(xrfg::OsrReconstruction::full);
        std::cerr<<"Native padded-canvas publication="<<n<<" eye="<<eye<<'\n';
        check(synth.synthesize(colors[eye].Get(),colors[1-eye].Get(),guide,source,
            destination,eye,static_cast<float>(baseline),settings),"native padded-canvas synthesis");
        wait(n+1);
    }
    xrfg::bridge_flight_logger().flush();
    std::ifstream flight(xrfg::bridge_flight_logger().log_path());require(flight.good(),"canvas flight log");
    flight.seekg(static_cast<std::streamoff>(log_offset));
    unsigned histories{},adaptations{};
    for(std::string line;std::getline(flight,line);) {
        if(line.find("op=luke_afw_stage ")==std::string::npos)continue;
        if(line.find(" a=1340 ")!=std::string::npos)++adaptations;
        if(line.find(" a=1310 ")==std::string::npos)continue;
        std::map<std::string,std::string> fields;std::istringstream record(line);
        for(std::string field;record>>field;) {
            const auto separator=field.find('=');
            if(separator!=std::string::npos)fields.emplace(field.substr(0,separator),field.substr(separator+1));
        }
        const auto publication=std::stoull(fields.at("b"));
        if(publication>=5) {
            require((std::stoul(fields.at("result"))&1)!=0&&std::stoull(fields.at("c"))==publication-1,
                "padded-canvas OSR lost primed image history");
            ++histories;
        }
    }
    require(adaptations==8&&histories==6,"missing canvas or history observations");
    std::cout<<"Eight padded-canvas submissions; six primed histories; unequal_height="
        <<unequal_height<<" first_eye="<<first_eye<<" PASS\n";
}
}
