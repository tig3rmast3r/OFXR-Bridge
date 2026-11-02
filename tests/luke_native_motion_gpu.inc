// Production GPU shader and admission, checked against independent 3D points.
    {
    std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
    std::array<D3D12_ROOT_PARAMETER,3> params{};
    for(UINT i=0;i<2;++i) {
        ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[i].NumDescriptors=i?1:4;
        params[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i].DescriptorTable={1,&ranges[i]};
    }
    params[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.Num32BitValues=60;
    D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=3;rd.pParameters=params.data();
    ComPtr<ID3DBlob> blob;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={g_xrfg_luke_compose_motion_shader,sizeof(g_xrfg_luke_compose_motion_shader)};
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    unsigned scenarios=0;
    for(unsigned native_span:{1u,2u})
    for(bool left_handed:{false,true})
    for(auto format:{DXGI_FORMAT_R16G16_FLOAT,DXGI_FORMAT_R32G32_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT})
    for(unsigned scenario=0;scenario<20;++scenario) {
        if(scenario>=17 && native_span!=1)continue;
        auto current=texture(10,8,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto depth=texture(3,2,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto middle_motion=texture(10,8,format,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto middle_depth=texture(10,8,DXGI_FORMAT_R32_FLOAT,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        auto output=texture(3,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto copied=texture(3,2,xrfg::kLukeAfwMotionFormat,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
        xrfg::DlssMotionVectorFrame g{};auto middle=std::make_shared<xrfg::DlssMotionVectorFrame>();
        auto init=[&](xrfg::DlssMotionVectorFrame& f,unsigned serial) {
            f.stream=10;f.epoch=3;f.serial=serial;f.publication=serial+10;f.camera_frame_token=serial+20;
            f.raw_motion_native_span=native_span;
            f.camera_frame_token_valid=f.camera_data_valid=f.camera_matrices_valid=true;
            f.motion_width=f.depth_width=8;f.motion_height=f.depth_height=6;f.output_width=16;f.output_height=12;
            f.camera_right={1,0,0};f.camera_up={0,1,0};f.camera_forward={0,0,-1};
            f.camera_view_to_clip[0]=f.camera_view_to_clip[5]=1;f.camera_view_to_clip[11]=-1;f.camera_view_to_clip[14]=.1F;
            f.camera_clip_to_view[0]=f.camera_clip_to_view[5]=1;f.camera_clip_to_view[11]=10;f.camera_clip_to_view[14]=-1;
            if(left_handed) {f.camera_view_to_clip[11]=1;f.camera_clip_to_view[14]=1;}
        };
        init(g,5);init(*middle,4);xrfg::DlssMotionVectorFrame old{};init(old,3);
        if(scenario>=17)old.camera_position[0]=.04F;
        g.previous_serial=3;g.motion_x=g.motion_y=1;g.scale_x=g.scale_y=4;
        if(scenario==10){g.scale_x=8;g.scale_y=2;}
        middle->camera_position[0]=scenario==1?.05F:-.05F;
        const float mid_yaw=(scenario==2 || scenario==3 || scenario==13 || scenario==14 || scenario==16)?.015F:scenario==8?3.141592653589793F:0;
        const float old_yaw=(scenario==2 || scenario==3 || scenario==13 || scenario==14 || scenario==16)?.03F:0;
        auto yaw=[](xrfg::DlssMotionVectorFrame& f,float angle) {
            f.camera_right={std::cos(angle),0,-std::sin(angle)};
            f.camera_forward={-std::sin(angle),0,-std::cos(angle)};
        };yaw(*middle,mid_yaw);yaw(old,old_yaw);
        const bool native_view_rows=scenario>=14&&scenario<17;
        const bool doom_pitch=scenario>=11&&scenario<14;
        const float mid_pitch=(doom_pitch||native_view_rows)?(scenario==12 || scenario==15?-.02F:scenario==14?0:.02F):0;
        const float old_pitch=2*mid_pitch;
        if(doom_pitch||native_view_rows) {
            // Independent right/down/physical-forward producer with real pitch.
            const auto packet=[](xrfg::DlssMotionVectorFrame& f,float y,float p) {
                f.camera_right={std::cos(y),0,-std::sin(y)};
                f.camera_up={-std::sin(y)*std::sin(p),-std::cos(p),-std::cos(y)*std::sin(p)};
                f.camera_forward={-std::sin(y)*std::cos(p),std::sin(p),-std::cos(y)*std::cos(p)};
            };
            packet(g,0,0);packet(*middle,mid_yaw,mid_pitch);packet(old,old_yaw,old_pitch);
            if(native_view_rows)for(auto* camera:{&g,middle.get(),&old}) {
                // Independent right/up/view-Z rows, matching Streamline's
                // declared inverse view. Keep both projection handednesses.
                for(float& value:camera->camera_up)value=-value;
                for(float& value:camera->camera_forward)value*=left_handed?1.F:-1.F;
            }
        }
        if(scenario==4 || scenario==19) {
            middle->jitter_x=.25F;middle->jitter_y=.125F;old.jitter_x=-.25F;old.jitter_y=-.125F;
            for(auto* f:{middle.get(),&old}) {
                f->camera_view_to_clip[8]=-2*f->jitter_x/8;f->camera_view_to_clip[9]=2*f->jitter_y/6;
                if(left_handed) {f->camera_view_to_clip[8]=-f->camera_view_to_clip[8];f->camera_view_to_clip[9]=-f->camera_view_to_clip[9];}
                f->camera_clip_to_view[12]=f->camera_view_to_clip[8]*(left_handed?-1.F:1.F);f->camera_clip_to_view[13]=f->camera_view_to_clip[9]*(left_handed?-1.F:1.F);
            }
        }
        g.motion_vectors=current;g.motion_snapshot_device=device;
        g.motion_retention=std::make_shared<xrfg::DlssSnapshotRetention>();g.depth_retention=std::make_shared<xrfg::DlssSnapshotRetention>();
        g.resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        g.previous_opposite_guide=middle;g.previous_same_native_camera=xrfg::dlss_native_camera_history(old);
        if(scenario>=17) {
            g.compose_native_object_history=middle->compose_native_object_history=true;
            middle->motion_vectors=middle_motion;middle->depth=middle_depth;
            middle->motion_x=middle->motion_y=middle->depth_x=middle->depth_y=1;
            middle->scale_x=g.scale_x;middle->scale_y=g.scale_y;
            middle->motion_retention=g.motion_retention;middle->depth_retention=g.depth_retention;
            middle->motion_snapshot_device=middle->depth_snapshot_device=device;
        }
        xrfg::LukeNativeMotionConstants constants{};unsigned failure{};
        const auto axes=native_view_rows?xrfg::LukeProducerCameraAxes::native_view_rows:
            doom_pitch?xrfg::LukeProducerCameraAxes::right_down_forward:xrfg::LukeProducerCameraAxes::view;
        expect(xrfg::luke_native_motion_constants(g,2,2,constants,failure,axes) && failure==0);
        {
            const auto single_stream_constants=constants;
            // Same three physical cameras, two native feature histories:
            // A2 / B2 / A3. Feature identities must remain distinct while
            // depth/subtraction produces the identical corrected field.
            g.luke_interleaved_history=true;g.serial=3;g.previous_serial=2;
            middle->stream=11;middle->epoch=5;middle->serial=2;
            old.serial=2;g.previous_same_native_camera=xrfg::dlss_native_camera_history(old);
            expect(xrfg::luke_native_motion_constants(g,2,2,constants,failure,axes) && failure==0);
            expect(!std::memcmp(&constants,&single_stream_constants,sizeof(constants)));
        }
        const auto d=[&](unsigned x,unsigned y) {
            if(scenario>=17)return .05F;
            if(scenario==5)return 0.F;
            if(scenario==6 && x==2 && y==2)return NAN;
            if(scenario==9)return -1.F;
            return .125F+.0625F*(x%4);
        };
        // Independent Euclidean camera projection, rather than the production matrices.
        const auto camera=[&](const xrfg::DlssMotionVectorFrame& target,float angle,unsigned x,unsigned y,float z) {
            const double nx=2*(x+.5)/8-1,ny=1-2*(y+.5)/6;
            const double px=nx-target.camera_position[0]*10*z;
            const double py=ny-target.camera_position[1]*10*z;
            const double pz=-1-target.camera_position[2]*10*z;
            const double vx=std::cos(angle)*px-std::sin(angle)*pz;
            const double yaw_z=std::sin(angle)*px+std::cos(angle)*pz;
            const float pitch=&target==middle.get()?mid_pitch:old_pitch;
            const double vy=std::cos(pitch)*py+std::sin(pitch)*yaw_z;
            const double vz=-std::sin(pitch)*py+std::cos(pitch)*yaw_z;
            const float projection_sign=left_handed?1.F:-1.F;
            return std::array<float,2>{float(((vx/-vz+projection_sign*target.camera_view_to_clip[8]-nx)*4-target.jitter_x)/g.scale_x),
                float(((vy/-vz+projection_sign*target.camera_view_to_clip[9]-ny)*-3-target.jitter_y)/g.scale_y)};
        };
        const std::array<float,2> object=(scenario==3 || scenario==13 || scenario==16 || scenario>=17)?std::array<float,2>{.125F,-.0625F}:std::array<float,2>{};
        const auto c=[&](unsigned x,unsigned y) {
            if(scenario==7 && x==2 && y==2)return std::array<float,2>{NAN,0};
            auto v=native_span==2?camera(old,old_yaw,x,y,d(x,y)):camera(*middle,mid_yaw,x,y,d(x,y));
            v[0]+=object[0];v[1]+=object[1];return v;
        };
        check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        std::array<ComPtr<ID3D12Resource>,4> uploads;
        for(unsigned slot=0;slot<4;++slot) {
            auto* target=slot==0?current.Get():slot==1?depth.Get():slot==2?middle_motion.Get():middle_depth.Get();const auto desc=target->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
            D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            auto uh=heap;uh.Type=D3D12_HEAP_TYPE_UPLOAD;check(device->CreateCommittedResource(&uh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&uploads[slot])));
            void* data{};check(uploads[slot]->Map(0,nullptr,&data));std::memset(data,0,static_cast<size_t>(bytes));
            const auto f=desc.Format;const bool wide=f==DXGI_FORMAT_R32G32_FLOAT || f==DXGI_FORMAT_R32G32B32A32_FLOAT;
            const bool four=f==DXGI_FORMAT_R16G16B16A16_FLOAT || f==DXGI_FORMAT_R32G32B32A32_FLOAT;
            const unsigned pixel_bytes=(slot&1)?4:(wide?4:2)*(four?4:2);
            for(unsigned y=0;y<desc.Height;++y)for(unsigned x=0;x<desc.Width;++x) {
                auto* dst=static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*pixel_bytes;
                if(slot&1) {const float z=slot==3&&scenario==18?0.F:d(x+2,y+2);std::memcpy(dst,&z,4);continue;}
                auto v=(x>=1 && x<9 && y>=1 && y<7)?c(x-1,y-1):std::array<float,2>{97,-81};
                if(slot==2 && scenario>=17) {
                    const auto a=camera(old,old_yaw,x,y,.05F),b=camera(*middle,mid_yaw,x,y,.05F);
                    v={a[0]-b[0]+.25F,a[1]-b[1]-.125F};
                }
                if(wide) {const float values[]{v[0],v[1],123,-91};std::memcpy(dst,values,pixel_bytes);}
                else {const std::uint16_t values[]{std::isfinite(v[0])?half(v[0]):std::uint16_t(0x7e00),std::isfinite(v[1])?half(v[1]):std::uint16_t(0x7e00),half(123),half(-91)};std::memcpy(dst,values,pixel_bytes);}
            }
            uploads[slot]->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=uploads[slot].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;to.pResource=target;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);auto b=barrier(target,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);list->ResourceBarrier(1,&b);
        }
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=5;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ComPtr<ID3D12DescriptorHeap> views;check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
        const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);auto cpu=views->GetCPUDescriptorHandleForHeapStart();
        for(auto* field:{current.Get(),depth.Get(),middle_motion.Get(),middle_depth.Get()}) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=field->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            device->CreateShaderResourceView(field,&srv,cpu);cpu.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=xrfg::kLukeAfwMotionFormat;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(output.Get(),nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]{views.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pipeline.Get());
        auto gpu=views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);gpu.ptr+=4*stride;list->SetComputeRootDescriptorTable(1,gpu);
        list->SetComputeRoot32BitConstants(2,60,&constants,0);list->Dispatch(1,1,1);
        auto b=barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);list->CopyResource(copied.Get(),output.Get());b=barrier(copied.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);
        const auto desc=copied->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto rh=heap;rh.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;check(device->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=copied.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value));check(fence->SetEventOnCompletion(value,event));expect(WaitForSingleObject(event,10000)==WAIT_OBJECT_0);
        void* data{};check(readback->Map(0,nullptr,&data));
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<3;++x) {
            std::uint16_t values[2];std::memcpy(values,static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch+x*4,4);
            const bool invalid=scenario==8 || scenario==9 || ((scenario==6 || scenario==7) && x==0 && y==0);
            auto expected=camera(old,old_yaw,x+2,y+2,d(x+2,y+2));
            for(unsigned axis=0;axis<2;++axis) {
                const float wanted=invalid?65504.F:expected[axis]+object[axis]+((scenario==17||scenario==19)?(axis?-.125F:.25F):0.F);
                if(!(std::abs(unhalf(values[axis])-wanted)<.001F)) {
                    std::cerr<<"scenario "<<scenario<<" format "<<format<<" pixel "<<x<<','<<y<<" axis "<<axis<<" actual "<<unhalf(values[axis])<<" expected "<<wanted<<'\n';expect(false);
                }
            }
        }
        readback->Unmap(0,nullptr);++scenarios;
        {
            g.luke_interleaved_history=false;g.serial=5;g.previous_serial=3;
            middle->stream=10;middle->epoch=3;middle->serial=4;
            old.serial=3;g.previous_same_native_camera=xrfg::dlss_native_camera_history(old);
        }
        // Guard failures never borrow newer cameras or fall back to uncorrected vectors.
        auto admission=[&](unsigned wanted) {xrfg::LukeNativeMotionConstants ignored{};unsigned why{};const bool ok=xrfg::luke_native_motion_constants(g,2,2,ignored,why,axes);expect(why==wanted && ok==(wanted==0));};
        const auto saved=g.previous_same_native_camera;g.previous_same_native_camera.reset();admission(1);g.previous_same_native_camera=saved;
        ++middle->epoch;admission(2);--middle->epoch;
        ++g.previous_serial;admission(2);--g.previous_serial;
        ++g.previous_same_native_camera->publication;admission(2);--g.previous_same_native_camera->publication;
        middle->reset=true;admission(3);middle->reset=false;
        g.jittered=true;admission(3);g.jittered=false;
        g.raw_motion_native_span=9;admission(3);g.raw_motion_native_span=native_span;
        middle->raw_motion_native_span=native_span==1?2:1;admission(3);middle->raw_motion_native_span=native_span;
        g.previous_same_native_camera->raw_motion_native_span=native_span==1?2:1;admission(3);
        g.previous_same_native_camera->raw_motion_native_span=native_span;
        const float w=g.camera_view_to_clip[11];g.camera_view_to_clip[11]=0;admission(3);g.camera_view_to_clip[11]=w;
        const float saved_jitter=middle->jitter_x;middle->jitter_x=NAN;admission(3);middle->jitter_x=saved_jitter;
        --middle->camera_frame_token;admission(4);++middle->camera_frame_token;
        // KCD camera-read attempts can have gaps from extra failed probes.
        // Exact observed native SR productions define temporal adjacency;
        // arbitrary success compaction must never admit a missed SR frame.
        const auto current_packet=g.camera_frame_token,middle_packet=middle->camera_frame_token;
        const auto older_packet=g.previous_same_native_camera->token;
        g.camera_frame_token=901;middle->camera_frame_token=407;g.previous_same_native_camera->token=103;
        g.native_production_sequence=105;middle->native_production_sequence=104;
        g.previous_same_native_camera->native_production_sequence=103;admission(0);
        ++g.native_production_sequence;admission(4);--g.native_production_sequence;
        --middle->native_production_sequence;admission(4);++middle->native_production_sequence;
        g.previous_same_native_camera->native_production_sequence=0;admission(4);
        g.previous_same_native_camera->native_production_sequence=103;
        middle->camera_frame_token=0;admission(3);middle->camera_frame_token=407;
        g.native_production_sequence=1;admission(4);g.native_production_sequence=105;
        g.native_production_sequence=middle->native_production_sequence=0;
        g.previous_same_native_camera->native_production_sequence=0;admission(4);
        g.camera_frame_token=current_packet;middle->camera_frame_token=middle_packet;
        g.previous_same_native_camera->token=older_packet;admission(0);
        ++middle->motion_width;admission(5);--middle->motion_width;
        const float scale=g.scale_x;g.scale_x=0;admission(6);g.scale_x=scale;
        const auto retention=g.depth_retention;g.depth_retention.reset();admission(7);g.depth_retention=retention;
        g.motion_x=3;admission(8);g.motion_x=1;
        const auto right=middle->camera_right;middle->camera_right={};admission(9);middle->camera_right=right;
        // N-1 has no GPU guide resources/retention: only exact scalar camera is needed.
        if(scenario<17)expect(!middle->motion_vectors && !middle->motion_retention);admission(0);
        if(scenario>=17) {middle->compose_native_object_history=false;admission(10);middle->compose_native_object_history=true;admission(0);}
    }
    std::cout<<"Depth/camera motion correction: "<<scenarios<<" WARP scenarios plus admission guards passed\n";
    }
