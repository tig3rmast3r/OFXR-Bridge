// Native OpenXR color/depth images have binding-defined states, independent
// of the private work image's producer state. Exercise the actual copy path.
void native_image_state_case(D3D12_RESOURCE_STATES work_state,bool independent,bool handoff) {
    independent_fixture_mode=independent;
    independent_fixture_binding_queue_handoff=handoff;
    Fixture f(true);f.throttled=false;f.unblock();
    ComPtr<ID3D12InfoQueue> messages;ok(f.device12.As(&messages),"native state GPU validation queue");
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ci.width=ci.height=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.arraySize=2;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchain color{},depth{};
    xr_ok(f.transport->create_swapchain(&ci,&color,work_state,1),"private array color chain");
    ci.format=DXGI_FORMAT_D32_FLOAT;ci.usageFlags=XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    xr_ok(f.transport->create_swapchain(&ci,&depth,work_state,1),"private depth chain");
    for(bool is_depth:{false,true}) {
        const auto handle=is_depth?depth:color;
        unsigned index{},count{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        xr_ok(f.transport->acquire_image(handle,&acquire,&index),"private state acquire");
        xr_ok(f.transport->wait_image(handle,&wait),"private state wait");
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        xr_ok(f.transport->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"private state image");
        ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type=is_depth?D3D12_DESCRIPTOR_HEAP_TYPE_DSV:D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
        ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"producer view heap");
        const auto view=heap->GetCPUDescriptorHandleForHeapStart();
        if(is_depth)f.device12->CreateDepthStencilView(image.texture,nullptr,view);
        else f.device12->CreateRenderTargetView(image.texture,nullptr,view);
        f.commands([&](ID3D12GraphicsCommandList* list) {
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={image.texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,work_state,D3D12_RESOURCE_STATE_RENDER_TARGET};
            if(!is_depth && work_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)list->ResourceBarrier(1,&b);
            if(is_depth)list->ClearDepthStencilView(view,D3D12_CLEAR_FLAG_DEPTH,.25F,0,0,nullptr);
            else {const float rgba[]={.5F,0,0,1};list->ClearRenderTargetView(view,rgba,0,nullptr);}
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
            if(!is_depth && work_state!=D3D12_RESOURCE_STATE_RENDER_TARGET)list->ResourceBarrier(1,&b);
        });
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr_ok(f.transport->release_image(handle,&release),"private state release");
    }
    // Repeat the released generation to check that restored native states
    // remain valid for the runtime's reads and retained-target reuse.
    for(unsigned repeat=0;repeat<2;++repeat) {
        f.begin_virtual();
        XrCompositionLayerDepthInfoKHR di{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
        di.subImage.swapchain=depth;di.subImage.imageRect.extent={1,1};di.subImage.imageArrayIndex=1;
        di.minDepth=0;di.maxDepth=1;di.nearZ=.1F;di.farZ=100;
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.next=&di;view.pose.orientation.w=1;view.subImage.swapchain=color;
        view.subImage.imageRect.extent={1,1};view.subImage.imageArrayIndex=1;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=1;projection.views=&view;
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.layerCount=1;end.layers=&layer;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        xr_ok(f.transport->end_frame(&end),"native state submit");f.wait_count(repeat+1);
        xr_ok(f.transport->drain(),"native state drain");
    }
    f.gpu_wait(f.delivery_queue.Get());
    check(f.pixels.size()==2 && f.pixels[0]==128 && f.pixels[1]==128,"native copied color content survives repeat");
    // Read the nonzero array slice after restoring the native color state.
    for(auto& [handle,chain]:f.chains)if(chain.state==D3D12_RESOURCE_STATE_RENDER_TARGET) {
        check(f.read_pixel(chain,0,0,1)==128,"all native color array slices copied");
    } else {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=256;desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
        ok(f.device12->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,IID_PPV_ARGS(&readback)),"native depth readback");
        f.commands([&](ID3D12GraphicsCommandList* list) {
            D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition={chain.d12[chain.released].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE};
            list->ResourceBarrier(1,&b);
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=chain.d12[chain.released].Get();
            source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;source.SubresourceIndex=1;
            D3D12_TEXTURE_COPY_LOCATION target{};target.pResource=readback.Get();
            target.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint.Footprint={DXGI_FORMAT_D32_FLOAT,1,1,1,256};
            list->CopyTextureRegion(&target,0,0,0,&source,nullptr);
            std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);
        },f.delivery_queue.Get());
        void* data{};ok(readback->Map(0,nullptr,&data),"map native depth readback");
        check(*static_cast<float*>(data)==.25F,"native depth content and DEPTH_WRITE restoration");
        readback->Unmap(0,nullptr);
    }
    unsigned errors{};
    for(UINT64 i=0;i<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size{};messages->GetMessage(i,nullptr,&size);std::vector<unsigned char> storage(size);
        auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ok(messages->GetMessage(i,message,&size),"read native state GPU validation");
        if(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR || message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION) {
            ++errors;std::fprintf(stderr,"GPU_ERROR %u: %s\n",unsigned(message->ID),message->pDescription);
        }
    }
    check(!errors,"native color/depth copy and restoration obey D3D12 states");
    xr_ok(f.transport->destroy_swapchain(depth),"destroy state depth chain");
    xr_ok(f.transport->destroy_swapchain(color),"destroy state color chain");
}
void native_image_state_checks() {
    ComPtr<ID3D12Debug> debug;ok(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),"D3D12 debug layer available");
    debug->EnableDebugLayer();
    for(bool independent:{false,true})for(bool handoff:{false,true})
        for(auto state:{D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON})
            native_image_state_case(state,independent,handoff);
    independent_fixture_mode=false;independent_fixture_binding_queue_handoff=false;
    std::puts("Native color/depth states: 8 GPU cases passed");
}
