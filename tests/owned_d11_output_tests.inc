struct OwnedD11Write {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12DescriptorHeap> heap;
};
OwnedD11Write queue_owned_d11_write(Fixture& f,
    std::array<ID3D12Resource*,2> images,
    std::array<std::array<unsigned,2>,2> colors) {
    OwnedD11Write work;
    ok(f.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&work.allocator)),"owned-output allocator");
    ok(f.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,work.allocator.Get(),nullptr,
        IID_PPV_ARGS(&work.list)),"owned-output command list");
    D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=4;
    ok(f.device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&work.heap)),"owned-output RTV heap");
    const auto increment=f.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(unsigned image=0;image<images.size();++image) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={images[image],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET};
        work.list->ResourceBarrier(1,&barrier);
        for(unsigned eye=0;eye<2;++eye) {
            auto handle=work.heap->GetCPUDescriptorHandleForHeapStart();handle.ptr+=(image*2+eye)*increment;
            D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.FirstArraySlice=eye;view.Texture2DArray.ArraySize=1;
            f.device12->CreateRenderTargetView(images[image],&view,handle);
            float color[]{float(colors[image][eye])/255.0F,0,0,1};
            work.list->ClearRenderTargetView(handle,color,0,nullptr);
        }
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        work.list->ResourceBarrier(1,&barrier);
    }
    ok(work.list->Close(),"owned-output close");
    ID3D12CommandList* lists[]{work.list.Get()};f.queue->ExecuteCommandLists(1,lists);
    return work;
}
std::array<unsigned,2> read_owned_d11_backing(Fixture& f,ID3D12Resource* image) {
    constexpr auto placement=D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=2*placement;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    ok(f.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&readback)),"owned-output backing readback");
    f.commands([&](ID3D12GraphicsCommandList* list) {
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={image,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1,&barrier);
        for(unsigned eye=0;eye<2;++eye) {
            D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=image;
            from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=eye;
            D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback.Get();
            to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Offset=eye*placement;
            to.PlacedFootprint.Footprint={DXGI_FORMAT_R8G8B8A8_UNORM,1,1,1,256};
            D3D12_BOX pixel{0,0,0,1,1,1};list->CopyTextureRegion(&to,0,0,0,&from,&pixel);
        }
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        list->ResourceBarrier(1,&barrier);
    });
    void* data{};ok(readback->Map(0,nullptr,&data),"map actual D12 backing pixels");
    const auto* bytes=static_cast<unsigned char*>(data);
    const std::array<unsigned,2> result{bytes[0],bytes[placement]};readback->Unmap(0,nullptr);return result;
}
void owned_d11_output_case(bool shared_output) {
    VramResourceCounters counters;
    Fixture f(false,true,{},false,true,true);f.native_image_count=3;f.expected_views=2;f.throttled=false;
    ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),&f.device12,&f.queue),"owned output's verified D12 owner");
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&f.fence)),"fixture proof fence");
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&f.gate)),"two-API blocked producer/consumer gate");
    ComPtr<ID3D12Fence> produced,reused;ComPtr<ID3D12CommandQueue> isolated;
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&produced)),"exact production proof");
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&reused)),"isolated reuse proof");
    D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ok(f.device12->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&isolated)),"isolated synthesis lane");
    HANDLE gate_handle{};ok(f.device12->CreateSharedHandle(f.gate.Get(),nullptr,GENERIC_ALL,nullptr,&gate_handle),"share gate");
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> gate11;
    ok(f.device11.As(&device5),"D11 fence device");ok(f.context.As(&context4),"D11 fence context");
    const auto opened=device5->OpenSharedFence(gate_handle,IID_PPV_ARGS(&gate11));CloseHandle(gate_handle);ok(opened,"open gate on D11");
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    info.width=512;info.height=256;info.arraySize=2;info.faceCount=info.mipCount=info.sampleCount=1;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    std::array<XrSwapchain,4> handles{};std::array<ID3D11Texture2D*,2> currents{},synthetics{};
    for(unsigned index=0;index<handles.size();++index) {
        xr_ok(f.transport->create_swapchain(&info,&handles[index],D3D12_RESOURCE_STATE_COMMON,1,
            reinterpret_cast<XrSwapchain>(400),shared_output?f.device12.Get():nullptr),"private generation output with optional D12 owner");
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};unsigned count{};
        xr_ok(f.transport->enumerate_images(handles[index],1,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"owned D11 destination enumeration");
        check(count==1,"one private work image per generation ring");
        (index<2?currents[index]:synthetics[index-2])=image.texture;
        auto backing=xrfg::owned_d11_output::backing_for(image.texture,f.device12.Get());
        check(bool(backing)==shared_output,"only opted-in private outputs carry a verified D12 backing");
        if(shared_output) {
            const auto desc=backing->GetDesc();
            const auto bytes=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
            observe_vram_resource(backing.Get(),counters,false,bytes);
        } else observe_vram_resource(image.texture,counters,false,std::uint64_t(info.width)*info.height*info.arraySize*4);
    }
    // A normal application texture and a transplanted marker cannot authorize
    // alias adoption. This catches accidental expansion to mutable app inputs.
    D3D11_TEXTURE2D_DESC source_desc{};currents[0]->GetDesc(&source_desc);
    source_desc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> source;
    ok(f.device11->CreateTexture2D(&source_desc,nullptr,&source),"independent original application input");
    check(!xrfg::owned_d11_output::backing_for(source.Get(),f.device12.Get()),"unmarked input never aliases private output memory");
    if(shared_output) {
        ComPtr<IUnknown> marker;UINT size=sizeof(IUnknown*);
        ok(currents[0]->GetPrivateData(xrfg::owned_d11_output::marker_id,&size,marker.GetAddressOf()),"private marker proof");
        ok(source->SetPrivateDataInterface(xrfg::owned_d11_output::marker_id,marker.Get()),"test marker transplant");
        check(!xrfg::owned_d11_output::backing_for(source.Get(),f.device12.Get()),"copied marker cannot authorize another texture");
        ok(source->SetPrivateDataInterface(xrfg::owned_d11_output::marker_id,nullptr),"remove transplanted marker");
    }
    auto bridge=std::make_unique<xrfg::D3D11D3D12SwapchainInterop>();ID3D11Texture2D* sources[]{source.Get()};
    ok(bridge->initialize(f.device11.Get(),f.context.Get(),f.device12.Get(),f.queue.Get(),
        sources,currents,synthetics),"actual bridge adopts only owned output aliases");
    ok(bridge->configure_synthesis_queue(isolated.Get()),"isolated lane retains shared-output ownership protocol");
    const auto payload=std::uint64_t(info.width)*info.height*info.arraySize*4;
    for(auto* image:bridge->source_images()) {
        const auto desc=image->GetDesc();observe_vram_resource(image,counters,false,
            f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes);
    }
    for(unsigned index=0;index<2;++index) {
        const std::array<ID3D12Resource*,2> mirrors{bridge->current_destination_images()[index],bridge->synthetic_destination_images()[index]};
        const std::array<ID3D11Texture2D*,2> views{currents[index],synthetics[index]};
        for(unsigned kind=0;kind<2;++kind) {
            auto backing=xrfg::owned_d11_output::backing_for(views[kind],f.device12.Get());
            check((backing.Get()==mirrors[kind])==shared_output,"interop uses the exact owned backing instead of a second allocation");
            if(!shared_output) {
                const auto desc=mirrors[kind]->GetDesc();observe_vram_resource(mirrors[kind],counters,false,
                    f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes);
            }
        }
    }
    for(auto& [handle,chain]:f.chains)for(auto& image:chain.d11)observe_vram_resource(image.Get(),counters,true,payload);
    check(counters.private_images==(shared_output?5:9) && counters.native_images==3,
        "four D11/D12 duplicate output allocations eliminated; source snapshot and native images remain separate");
    const auto acquire=[&](unsigned slot) {
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};w.timeout=XR_INFINITE_DURATION;
        unsigned index{};xr_ok(f.transport->acquire_image(handles[slot],&a,&index),"acquire output work image");
        xr_ok(f.transport->wait_image(handles[slot],&w),"wait output work image");check(index==0,"single private output index");
    };
    const auto release=[&](unsigned slot) {
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr_ok(f.transport->release_image(handles[slot],&r),"release ordered private output");
    };
    const auto submit=[&](unsigned slot,unsigned identity) {
        std::array<XrCompositionLayerProjectionView,2> views{};
        for(unsigned eye=0;eye<2;++eye) {
            auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
            v.pose.position.x=float(identity);v.subImage.swapchain=handles[slot];v.subImage.imageArrayIndex=eye;
            v.subImage.imageRect.extent={int(info.width),int(info.height)};
        }
        XrCompositionLayerProjection p{XR_TYPE_COMPOSITION_LAYER_PROJECTION};p.space=reinterpret_cast<XrSpace>(9);p.viewCount=2;p.views=views.data();
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount=1;end.layers=&layer;f.begin_virtual();xr_ok(f.transport->end_frame(&end),"submit owned stereo output");
    };
    // Producer GPU work is not complete: neither alias publication nor the
    // downstream endpoint may treat shared backing as an already-ready frame.
    acquire(0);acquire(2);ok(bridge->prepare_synthesis(),"prepare original output write lane");
    ok(f.queue->Wait(f.gate.Get(),1),"hold producer GPU writes");
    auto first=queue_owned_d11_write(f,{bridge->current_destination_images()[0],bridge->synthetic_destination_images()[0]},
        {{{20,130},{50,160}}});
    ok(f.queue->Signal(produced.Get(),1),"production completion signal");
    xrfg::D3D11PublicationReceipt receipt;
    ok(bridge->publish(0,0,&receipt),"publication orders incomplete producer work");
    ok(bridge->capture_published_current(receipt),"same-output reverse capture retains ordering without a self-copy");
    release(0);release(2);submit(0,1);
    std::this_thread::sleep_for(25ms);
    check(produced->GetCompletedValue()<1 && receipt.fence->GetCompletedValue()<receipt.copy_complete_value && !f.real_ends,
        "alias output cannot pass an incomplete native producer fence");
    ok(f.gate->Signal(1),"release producer GPU writes");f.gpu_wait();
    ok(xrfg::wait_for_d3d11_publication(receipt),"exact alias publication completed");f.wait_entered();
    submit(2,2);f.unblock();f.wait_count(2);xr_ok(f.transport->drain(),"first shared-output drain");
    check(f.pixels==std::vector<unsigned>({20,50}) && f.right_pixels==std::vector<unsigned>({130,160}),
        "D12 producer pixels reach both private D11 array slices and native eyes");
    // A completed producer alone does not authorize slot reuse while D11's
    // publication timeline is blocked. The isolated OFXR queue must join it.
    acquire(1);acquire(3);ok(bridge->prepare_synthesis(),"join previous D11 access before next source writes");
    auto second=queue_owned_d11_write(f,{bridge->current_destination_images()[1],bridge->synthetic_destination_images()[1]},
        {{{80,190},{110,220}}});
    ok(f.queue->Signal(produced.Get(),2),"second producer completion");f.gpu_wait();
    ok(context4->Wait(gate11.Get(),2),"hold D11 consumer/publication timeline");context4->Flush();
    xrfg::D3D11PublicationReceipt second_receipt;
    ok(bridge->publish_completed(1,1,produced.Get(),2,&second_receipt),"already-complete producer uses existing D11 publication timeline");
    ok(bridge->prepare_synthesis_targets(1,1),"isolated next writer waits for these exact output slots");
    ok(isolated->Signal(reused.Get(),1),"isolated reuse completion proof");
    release(1);release(3);submit(1,3);
    std::this_thread::sleep_for(25ms);
    check(reused->GetCompletedValue()<1 && second_receipt.fence->GetCompletedValue()<second_receipt.copy_complete_value && f.real_ends==2,
        "D11 consumer fence still protects shared output reuse and native admission");
    ok(f.gate->Signal(2),"release D11 consumer timeline");
    ok(xrfg::wait_for_d3d11_publication(second_receipt),"completed producer's publication retired");
    f.gpu_wait(isolated.Get());check(reused->GetCompletedValue()>=1,"isolated reuse requires actual D11 completion");
    submit(3,4);f.wait_count(4);xr_ok(f.transport->drain(),"all four owned outputs retired");
    // A remapped target is a distinct private output, even in alias mode.
    // Its copy cannot be elided, and the reverse capture must read that actual
    // target back into the selected source mirror when the indices differ.
    acquire(0);acquire(1);ok(bridge->prepare_synthesis(),"join completed publication before remap writes");
    auto remap_write=queue_owned_d11_write(f,{bridge->current_destination_images()[0],bridge->current_destination_images()[1]},
        {{{25,135},{99,88}}});f.gpu_wait();
    xrfg::D3D11PublicationReceipt remap;
    ok(bridge->publish(0,std::nullopt,&remap,xrfg::D3D11PublicationTargets{1,std::nullopt}),"remapped publication copies between distinct output aliases");
    ok(xrfg::wait_for_d3d11_publication(remap),"remapped target completed");
    {
        Chain actual;actual.d11.emplace_back(currents[1]);
        check(f.read_pixel(actual,0,0,0)==25 && f.read_pixel(actual,0,0,1)==135,
            "remap copied both eye slices into the actual acquired target");
    }
    std::vector<std::uint32_t> data(std::size_t(info.width)*info.height);
    for(unsigned eye=0;eye<2;++eye) {
        std::fill(data.begin(),data.end(),(eye?152U:42U)|0xff000000U);
        f.context->UpdateSubresource(currents[1],eye,nullptr,data.data(),info.width*4,0);
    }
    ok(bridge->capture_published_current(remap),"reverse remap captures the actual acquired D11 image");
    ok(bridge->finish_capture(),"reverse capture retains its original producer completion handshake");
    check(read_owned_d11_backing(f,bridge->current_destination_images()[0])==std::array<unsigned,2>{42,152},
        "reverse capture copies a remapped target rather than assuming shared storage");
    release(0);release(1);submit(1,5);f.wait_count(5);xr_ok(f.transport->drain(),"remapped owned output retired");
    check(f.pixels==std::vector<unsigned>({20,50,80,110,42}) &&
          f.right_pixels==std::vector<unsigned>({130,160,190,220,152}),"allocation reduction preserves all real/synthetic/remapped stereo pixels");
    ok(bridge->wait_for_idle(),"all shared-output writers and readers retired");
    first={};second={};remap_write={};bridge.reset();
    check(counters.private_images==4 && counters.native_images==3,
        "bridge shutdown releases only its own snapshots/mirrors while native/private outputs remain owned");
    for(auto handle:handles)xr_ok(f.transport->destroy_swapchain(handle),"destroy private shared output after actual GPU completion");
    check(!counters.private_images && !counters.native_images && !counters.live_bytes && f.chains.empty(),
        "shared D11 marker/backing has no COM cycle and releases every output allocation");
    std::printf("D11 outputs %s: physical backing count %u (including source snapshot and three native images), producer/consumer fences, both array eyes, remap and teardown PASS\n",
        shared_output?"shared backing":"separate mirrors",shared_output?8:12);
}
void owned_d11_output_format_contracts() {
    VramResourceCounters counters;
    Fixture f(false);
    ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),&f.device12,&f.queue),
        "format coverage uses the verified D11/D12 allocation owner");
    constexpr std::array formats{
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R16G16B16A16_FLOAT};
    for(const auto format:formats)for(const unsigned slices:{1U,2U}) {
        D3D11_TEXTURE2D_DESC requested{};
        requested.Width=128;requested.Height=64;requested.ArraySize=slices;requested.MipLevels=1;
        requested.Format=format;requested.SampleDesc.Count=1;requested.Usage=D3D11_USAGE_DEFAULT;
        requested.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> image;
        // Allocation/descriptor/lifetime proof only. The UNORM pixel and
        // fence oracle above must not be reused for sRGB or half-float data.
        ok(xrfg::owned_d11_output::create(f.device11.Get(),f.device12.Get(),requested,&image),
            "common SteamVR format creates an owned alias without mirror fallback");
        auto backing=xrfg::owned_d11_output::backing_for(image.Get(),f.device12.Get());
        check(bool(backing),"common output format retains its exact owned marker");
        D3D11_TEXTURE2D_DESC actual{};image->GetDesc(&actual);
        const auto desc=backing->GetDesc();
        check(actual.Width==requested.Width && actual.Height==requested.Height &&
              actual.ArraySize==slices && actual.MipLevels==1 && actual.Format==format &&
              actual.SampleDesc.Count==1 && !actual.SampleDesc.Quality &&
              desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
              desc.Width==requested.Width && desc.Height==requested.Height &&
              desc.DepthOrArraySize==slices && desc.MipLevels==1 && desc.Format==format &&
              desc.SampleDesc.Count==1 && !desc.SampleDesc.Quality,
            "D11 view and D12 backing preserve the exact common-format array contract");
        ComPtr<ID3D12Device> owner;
        ok(backing->GetDevice(IID_PPV_ARGS(&owner)),"query actual shared backing owner");
        check(xrfg::owned_d11_output::same_identity(owner.Get(),f.device12.Get()),
            "shared output backing belongs to the verified allocation device");
        const auto bytes=f.device12->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
        const auto payload=std::uint64_t(requested.Width)*requested.Height*slices*
            (format==DXGI_FORMAT_R16G16B16A16_FLOAT?8U:4U);
        check(bytes>=payload && bytes!=UINT64_MAX,"actual common-format allocation has valid physical size");
        observe_vram_resource(backing.Get(),counters,false,bytes);
        check(counters.private_images==1 && counters.live_bytes==bytes,
            "one physical owned allocation serves both APIs");
        auto repeated=xrfg::owned_d11_output::backing_for(image.Get(),f.device12.Get());
        check(xrfg::owned_d11_output::same_identity(backing.Get(),repeated.Get()),
            "repeated marker queries return the existing backing");
        repeated.Reset();backing.Reset();
        check(counters.private_images==1,"D11 owner marker alone pins the existing D12 allocation");
        backing=xrfg::owned_d11_output::backing_for(image.Get(),f.device12.Get());
        check(bool(backing),"shared output remains queryable without an external D12 reference");
        image.Reset();
        check(counters.private_images==1,"explicit D12 consumer reference survives D11 output release");
        backing.Reset();
        check(!counters.private_images && !counters.native_images && !counters.live_bytes,
            "common-format shared output and marker release without a COM cycle");
        std::printf("D11 owned format %u array%u: exact D11/D12 descriptor, single backing, owner marker and teardown PASS (%llu allocation bytes)\n",
            unsigned(format),slices,static_cast<unsigned long long>(bytes));
    }
}
void owned_d11_output_tests() {
    owned_d11_output_case(false);owned_d11_output_case(true);owned_d11_output_format_contracts();
}
