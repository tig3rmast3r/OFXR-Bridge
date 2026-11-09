// Split resources model Luke's alternating native eyes. Providers retain a
// separate temporal context per eye; a held peer never enters the provider.
void test_aer_split_eye_guide_endpoints(D3D12WarpFixture& fixture,
    xrfg::D3D12FrameGenerationMode mode) {
    constexpr UINT width=256,height=128;
    const auto texture=[&](DXGI_FORMAT format,UINT value) {
        auto desc=stereo_texture_description(width,height);
        desc.DepthOrArraySize=1;desc.Format=format;
        desc.Flags=format==kFormat?D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET:D3D12_RESOURCE_FLAG_NONE;
        const auto heap=heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> result;
        require_hresult(fixture.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
            &desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&result)),"AER guide texture");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows{};UINT64 row_bytes{},bytes{};
        fixture.device()->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&row_bytes,&bytes);
        auto upload=create_buffer(fixture,D3D12_HEAP_TYPE_UPLOAD,bytes,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped{};require_hresult(upload->Map(0,nullptr,&mapped),"AER guide upload");
        std::memset(mapped,0,SIZE_T(bytes));
        for(UINT y=0;y<height;++y) {
            auto* row=static_cast<std::byte*>(mapped)+fp.Offset+SIZE_T(y)*fp.Footprint.RowPitch;
            if(format==kFormat)for(UINT x=0;x<width;++x) {
                auto* pixel=reinterpret_cast<std::uint8_t*>(row)+x*4;
                pixel[0]=std::uint8_t(value);pixel[1]=std::uint8_t(255-value);pixel[2]=17;pixel[3]=255;
            }
            if(format==DXGI_FORMAT_R32_FLOAT)for(UINT x=0;x<width;++x)reinterpret_cast<float*>(row)[x]=.5F;
        }
        upload->Unmap(0,nullptr);
        const auto state=format==kFormat?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fixture.execute_and_wait([&](ID3D12GraphicsCommandList* list) {
            D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=upload.Get();
            a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=fp;
            b.pResource=result.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&b,0,0,0,&a,nullptr);
            auto barrier=transition_barrier(result.Get(),D3D12_RESOURCE_STATE_COPY_DEST,state);
            list->ResourceBarrier(1,&barrier);
        });
        return result;
    };
    auto motion=texture(DXGI_FORMAT_R32G32_FLOAT,0),depth=texture(DXGI_FORMAT_R32_FLOAT,0);
    for(UINT eye=0;eye<2;++eye) {
        const UINT color=eye?211:43;
        auto a=texture(kFormat,color),b=texture(kFormat,color+1);
        auto current=create_single_slice_texture(fixture,width,height),synthetic=create_single_slice_texture(fixture,width,height);
        ID3D12Resource* sources[]{a.Get(),b.Get()},*currents[]{current.Get()},*synthetics[]{synthetic.Get()};
        auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        require_hresult(history->initialize(fixture.device(),fixture.queue(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"AER split history");
        xrfg::D3D12NvidiaOpticalFlowOptions options;options.ofr=true;
        xrfg::D3D12FrameSynthesizer synthesizer;
        require_hresult(synthesizer.initialize(fixture.device(),fixture.queue(),history,currents,synthetics,
            kFormat,D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::fidelity_fx,
            options,false,mode),"AER guide synthesizer");
        auto view=make_reprojection_views()[eye];view.logical_eye=eye;view.array_slice=0;
        view.image_rect={0,0,width,height};
        const auto guides=[&](UINT serial,UINT stream=0) {
            auto set=std::make_shared<xrfg::DlssMotionVectorSet>();set->eye_count=1;set->resolved_eye=int(eye);
            auto g=std::make_shared<xrfg::DlssMotionVectorFrame>();g->stream=stream?stream:910+eye;
            g->epoch=1;g->serial=serial;g->previous_serial=serial-1;g->publication=serial;
            g->producer_queue=fixture.queue();g->motion_vectors=motion;g->depth=depth;
            g->output_width=g->motion_width=g->depth_width=width;
            g->output_height=g->motion_height=g->depth_height=height;
            g->resource_state=g->depth_resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            set->eyes[0]=std::move(g);return set;
        };
        xrfg::D3D12HistoryCaptureTicket capture{};
        require_hresult(history->capture(0,&capture),"AER guide prime capture");
        require_hresult(history->commit(capture),"AER guide prime commit");
        xrfg::D3D12FrameSynthesisTicket ticket{};
        require_hresult(synthesizer.submit_prime(capture,{&view,1},0,&ticket,guides(1)),"AER guide prime");
        require_hresult(synthesizer.wait_for_previous_submission(30000),"AER guide prime ready");
        bool native_generated=false;
        for(UINT serial=2;serial<=5;++serial) {
            require(!synthesizer.can_pair_fsr(guides(serial,910+(eye^1))),"AER rejects opposite-eye stream before provider");
            require(!synthesizer.can_pair_fsr(guides(serial+1)),"AER rejects a missing same-eye interval");
            require(synthesizer.can_pair_fsr(guides(serial)),"AER accepts exact next same-eye guides");
            require_hresult(history->capture(serial%2,&capture),"AER fresh eye capture");
            require_hresult(history->commit(capture),"AER fresh eye commit");
            auto input=guides(serial);
            ComPtr<ID3D12CommandQueue> producer;
            ComPtr<ID3D12Fence> gate,receipt;
            struct ReleaseGate {ID3D12Fence* gate{};~ReleaseGate(){if(gate)gate->Signal(1);}} release;
            if(serial==2) {
                D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
                require_hresult(fixture.device()->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&producer)),"AER separate guide producer");
                require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"AER guide producer gate");
                require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&receipt)),"AER guide producer receipt");
                release.gate=gate.Get();
                require_hresult(producer->Wait(gate.Get(),1),"hold exact guide production independently of colour capture");
                require_hresult(producer->Signal(receipt.Get(),1),"queued exact guide receipt");
                auto g=std::make_shared<xrfg::DlssMotionVectorFrame>(*input->eyes[0]);
                g->producer_queue=producer;g->producer_fence=receipt;g->producer_fence_value=1;
                input->eyes[0]=std::move(g);
            }
            if(serial==3) {
                D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
                ComPtr<ID3D12CommandQueue> unproved;
                require_hresult(fixture.device()->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&unproved)),"AER unproved guide producer");
                auto missing=guides(serial);
                auto g=std::make_shared<xrfg::DlssMotionVectorFrame>(*missing->eyes[0]);
                g->producer_queue=unproved;missing->eyes[0]=std::move(g);
                xrfg::D3D12FrameSynthesisTicket refused{};
                require(synthesizer.submit_pair(capture,{&view,1},{&view,1},0,0,&refused,
                    std::nullopt,missing,false,.5F,{},std::nullopt,false,1u<<eye)==DXGI_ERROR_WAS_STILL_DRAWING,
                    "AER refuses a guide whose separate producer has no exact completion receipt");
                // The complete retry below must work on the same source:
                // missing guide readiness is not a permanent provider failure.
            }
            require_hresult(synthesizer.submit_pair(capture,{&view,1},{&view,1},0,0,&ticket,
                std::nullopt,input,false,.5F,{},std::nullopt,false,1u<<eye),"AER fresh guided pair");
            if(gate) {
                auto completion=std::async(std::launch::async,[&]{return synthesizer.wait_for_previous_submission(30000);});
                const bool blocked=completion.wait_for(std::chrono::milliseconds(40))==std::future_status::timeout;
                require_hresult(gate->Signal(1),"release independent guide production");
                require_hresult(completion.get(),"AER pair resumes after exact guide receipt");
                require(blocked,"AER synthesis consumed guides before their own producer receipt");
            } else require_hresult(synthesizer.wait_for_previous_submission(30000),"AER fresh guided pair ready");
            require(!ticket.held_capture,"fresh same-eye input was mistaken for repeated peer");
            if(ticket.native_result)native_generated|=ticket.native_result->mask.load()!=0;
            else require(ticket.aer_retimed,"FSR guided pair did not enter AER retiming");
            const auto expected=readback_single_slice(fixture,sources[serial%2],D3D12_RESOURCE_STATE_RENDER_TARGET);
            require(readback_single_slice(fixture,current.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==expected,
                "fresh guided pair changed its raw current endpoint");
            // This eye is held on the next application presentation. It has
            // no new DLSS evaluation, and must copy its own retained colour.
            require_hresult(synthesizer.submit_pair(capture,{&view,1},{&view,1},0,0,&ticket,
                std::nullopt,{},false,.5F,{},std::nullopt,false,1u<<(eye^1)),"AER guided held peer");
            require_hresult(synthesizer.wait_for_previous_submission(30000),"AER guided held peer ready");
            require(ticket.held_capture && ticket.aer_retimed,"AER guided held peer lost its real endpoint receipt");
            require(readback_single_slice(fixture,synthetic.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==expected,
                "held peer synthesized or mixed the other eye's pixels");
            require(readback_single_slice(fixture,current.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET)==expected,
                "held peer changed its raw current endpoint");
        }
        if(mode==xrfg::D3D12FrameGenerationMode::native_dlss)
            require(native_generated,"native AER guide test must observe actual generated output, not only fallback");
    }
    std::cout<<"AER split-eye guides: exact chronology, cross-eye/gap refusal, provider pairs and bit-exact held peers PASS\n";
}
