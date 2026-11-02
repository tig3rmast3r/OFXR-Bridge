// Actual production capture/ownership/physical publication on real D3D11
// textures. The runtime fixture cycles image indices and checks pixels inside
// physical EndFrame, including old endpoints presented after newer captures.
void luke_aux_private_test(bool private_path=true, bool afw=true) {
    auto s=session();s->presenter_active=false;
    s->luke_afw_enabled=afw;s->dispatch->steamvr_runtime=private_path;
    s->dispatch->virtual_desktop_runtime=!private_path;s->vdxr_legacy=!private_path;
    // Manual sessions bypass create_session's transport selection. Set the
    // immutable route too, rather than treating a runtime-name flag as it.
    check(comparison_delivery_runtime(*s)==private_path,"auxiliary fixture transport identity");
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,
        &s->d3d11_device,nullptr,&s->d3d11_context)),"private auxiliary device");
    Microsoft::WRL::ComPtr<ID3D11Device5> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context;
    check(SUCCEEDED(s->d3d11_device.As(&device)) && SUCCEEDED(s->d3d11_context.As(&context)),"auxiliary fence APIs");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=16;
    ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.format=desc.Format;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    auto app=std::make_shared<SwapchainState>(s,ci);app->handle=reinterpret_cast<XrSwapchain>(0x820);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source,read;
    check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&source)),"auxiliary source");
    app->enumerated_d3d11_images.push_back(source);
    auto read_desc=desc;read_desc.BindFlags=0;read_desc.Usage=D3D11_USAGE_STAGING;
    read_desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    check(SUCCEEDED(device->CreateTexture2D(&read_desc,nullptr,&read)),"auxiliary readback");
    struct Runtime {
        struct Chain {
            std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>,3> images;
            unsigned cursor{},acquired{},released{};bool owned{},waited{};
        };
        std::unordered_map<XrSwapchain,Chain> chains;
        ID3D11Device5* device{};ID3D11DeviceContext4* context{};ID3D11Texture2D* read{};
        D3D11_TEXTURE2D_DESC desc{};
        unsigned creates{},destroys{},physical_ends{},expected{};
        bool valid{true};
    } runtime;
    runtime.device=device.Get();runtime.context=context.Get();runtime.read=read.Get();runtime.desc=desc;
    static Runtime* r;r=&runtime;
    s->dispatch->create_swapchain=+[](XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out)->XrResult {
        r->valid &= info->width==16 && info->height==16 &&
            (info->usageFlags&XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT)!=0;
        *out=reinterpret_cast<XrSwapchain>(std::uintptr_t(0x900+(++r->creates)));
        auto& c=r->chains[*out];
        for(auto& image:c.images) if(FAILED(r->device->CreateTexture2D(&r->desc,nullptr,&image)))return XR_ERROR_RUNTIME_FAILURE;
        return XR_SUCCESS;
    };
    s->dispatch->enumerate_swapchain_images=+[](XrSwapchain h,uint32_t capacity,uint32_t* count,XrSwapchainImageBaseHeader* images)->XrResult {
        *count=3;if(!capacity)return XR_SUCCESS;
        auto* out=reinterpret_cast<XrSwapchainImageD3D11KHR*>(images);
        for(unsigned i=0;i<3;++i)out[i].texture=r->chains.at(h).images[i].Get();
        return XR_SUCCESS;
    };
    s->dispatch->acquire_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageAcquireInfo*,uint32_t* index)->XrResult {
        auto& c=r->chains.at(h);r->valid &= !c.owned;
        c.owned=true;c.waited=false;c.acquired=(++c.cursor)%3;*index=c.acquired;return XR_SUCCESS;
    };
    s->dispatch->wait_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageWaitInfo*)->XrResult {
        auto& c=r->chains.at(h);r->valid &= c.owned;c.waited=true;return XR_SUCCESS;
    };
    s->dispatch->release_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageReleaseInfo*)->XrResult {
        auto& c=r->chains.at(h);r->valid &= c.owned && c.waited;
        c.released=c.acquired;c.owned=false;c.waited=false;return XR_SUCCESS;
    };
    s->dispatch->destroy_swapchain=+[](XrSwapchain h)->XrResult {
        r->valid &= r->chains.contains(h) && !r->chains.at(h).owned;
        r->chains.erase(h);++r->destroys;return XR_SUCCESS;
    };
    s->dispatch->end_frame=+[](XrSession,const XrFrameEndInfo* info)->XrResult {
        ++r->physical_ends;r->valid &= info->layerCount==2;
        XrSwapchain first{};
        for(unsigned i=0;i<info->layerCount;++i) {
            const auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
            r->valid &= q->type==XR_TYPE_COMPOSITION_LAYER_QUAD && q->pose.position.x==float(i+1) &&
                q->eyeVisibility==(i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT) &&
                q->subImage.imageRect.offset.x==2 && q->subImage.imageRect.extent.width==12 &&
                q->layerFlags==XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            const auto found=r->chains.find(q->subImage.swapchain);
            if(found==r->chains.end()) {r->valid=false;return XR_ERROR_HANDLE_INVALID;}
            if(!i)first=found->first;else r->valid &= first==found->first;
            const auto& c=found->second;r->valid &= !c.owned;
            r->context->CopyResource(r->read,c.images[c.released].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(FAILED(r->context->Map(r->read,0,D3D11_MAP_READ,0,&mapped)))return XR_ERROR_RUNTIME_FAILURE;
            r->valid &= *static_cast<const unsigned*>(mapped.pData)==r->expected;
            r->context->Unmap(r->read,0);
        }
        return XR_SUCCESS;
    };
    {std::scoped_lock lock(g_state_mutex);g_swapchains[app->handle]=app;g_sessions[s->handle]=s;}
    const auto cleanup=[&] {
        destroy_composition_capture(app);
        std::scoped_lock lock(g_state_mutex);g_swapchains.erase(app->handle);g_sessions.erase(s->handle);
    };
    try {
        const bool enabled=private_path && afw;
        auto capture=[&](unsigned color) {
            std::array<unsigned,256> pixels;pixels.fill(color);
            context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),64,0);
            check(SUCCEEDED(capture_composition_image(app,0)),"private auxiliary release capture");
            app->composition_capture->valid=true;
            auto packet=std::make_shared<GeneratedFrameEndInfo>();
            for(unsigned i=0;i<2;++i) {
                XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};q.subImage.swapchain=app->handle;
                q.subImage.imageRect={{2,1},{12,14}};q.pose.position.x=float(i+1);q.pose.orientation.w=1;
                q.eyeVisibility=i?XR_EYE_VISIBILITY_RIGHT:XR_EYE_VISIBILITY_LEFT;
                q.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                packet->composition_layers.emplace_back(q);
            }
            check(XR_SUCCEEDED(own_composition_images(s,*packet)),"retain auxiliary snapshot");
            check(packet->composition_images.size()==1,"duplicate layers must share snapshot");
            packet->info={XR_TYPE_FRAME_END_INFO};packet->info.displayTime=1000000000;
            packet->info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            for(auto& layer:packet->composition_layers)packet->layer_pointers.push_back(composition_layer_header(layer));
            packet->info.layers=packet->layer_pointers.data();packet->info.layerCount=2;
            return packet;
        };
        auto a=capture(0xff123456),b=capture(0xffabcdef),c=capture(0xff987654);
        check(runtime.creates==(enabled?1u:3u),"auxiliary XR handle count is not 1 vs legacy 3");
        check(a->composition_images[0]!=b->composition_images[0] && b->composition_images[0]!=c->composition_images[0],
            "retained producer snapshots aliased");
        check(a->composition_images[0]->public_output!=nullptr || !enabled,"missing private publication owner");
        const auto inventory=luke_swapchain_inventory(s,&a->info);
        if(afw) {
            // The physical frame here names private outputs, whereas inventory
            // samples real app metadata. Mark this source's observed app role.
            app->diagnostic_layer_roles=2;s->next_swapchain_inventory_ms=0;
            const auto observed=luke_swapchain_inventory(s,nullptr);
            check(observed.application==1 && observed.auxiliary==1 && observed.other==0 &&
                observed.auxiliary_outputs==(enabled?1u:3u) && observed.total==(enabled?2u:4u) &&
                observed.private_snapshots==(enabled?3u:0u),"live inventory counts private tokens as XR handles");
            auto extra=std::make_shared<SwapchainState>(s,ci);extra->handle=reinterpret_cast<XrSwapchain>(0x823);
            {std::scoped_lock lock(g_state_mutex);g_swapchains[extra->handle]=extra;}
            s->next_swapchain_inventory_ms=0;
            const auto unknown=luke_swapchain_inventory(s,nullptr);
            check(unknown.other==1 && std::string_view(unknown.details).find("0x823")!=std::string_view::npos &&
                xrfg::autonomous::swapchain_budget_warning(unknown).starts_with("Unclassified"),
                "foreign/unclassified app handle not identified");
            {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(extra->handle);}
            s->luke_aux_budget_inline=true;
            const auto fallback=luke_swapchain_inventory(s,nullptr);
            check(fallback.fallback && xrfg::autonomous::swapchain_budget_warning(fallback).starts_with("Swapchain fallback"),
                "fallback hidden by inventory throttle");
            s->luke_aux_budget_inline=false;
            std::atomic<bool> locked{},unlock{};
            std::thread owner([&]{std::scoped_lock lock(app->call_mutex);locked=true;while(!unlock)std::this_thread::yield();});
            while(!locked)std::this_thread::yield();
            s->next_swapchain_inventory_ms=0;
            const auto started=std::chrono::steady_clock::now();
            const auto busy=luke_swapchain_inventory(s,nullptr);
            const auto elapsed=std::chrono::steady_clock::now()-started;
            unlock=true;owner.join();
            check(busy.updated_ms==unknown.updated_ms && elapsed<std::chrono::milliseconds(100),
                "CPU inventory blocked producer ownership or published partial counts");
        }

        // A/B remain independent settings; none changes retained color ownership.
        std::array<std::shared_ptr<GeneratedFrameEndInfo>,5> order{a,b,a,c,c};
        const unsigned colors[]={0xff123456,0xffabcdef,0xff123456,0xff987654,0xff987654};
        for(unsigned i=0;i<5;++i) {
            s->separate_synthesis=i&1;s->lower_vram=(i>>1)&1;
            runtime.expected=colors[i];
            check(XR_SUCCEEDED(content_end_frame(s->handle,&order[i]->info)),"physical auxiliary publication");
        }
        check(runtime.valid && runtime.physical_ends==5,"old queued auxiliary pixels or geometry changed");
        if(enabled)check(a->composition_images[0]->public_output->serial==4,
            "repeated auxiliary endpoint performs a redundant GPU publication");
        // A physical publication waits for its own GPU read without owning the
        // producer GPU mutex. It must not call runtime EndFrame before that copy.
        if(enabled) {
            Microsoft::WRL::ComPtr<ID3D11Fence> gate;
            Microsoft::WRL::ComPtr<ID3D12Device> gate_device;
            Microsoft::WRL::ComPtr<ID3D12Fence> cpu_gate;
            check(SUCCEEDED(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&gate_device))),"auxiliary gate device");
            check(SUCCEEDED(gate_device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&cpu_gate))),"auxiliary CPU gate");
            HANDLE shared{};
            check(SUCCEEDED(gate_device->CreateSharedHandle(cpu_gate.Get(),nullptr,GENERIC_ALL,nullptr,&shared)),"auxiliary shared gate");
            const auto opened=device->OpenSharedFence(shared,IID_PPV_ARGS(&gate));CloseHandle(shared);
            check(SUCCEEDED(opened),"auxiliary GPU gate");
            check(SUCCEEDED(context->Wait(gate.Get(),1)),"arm auxiliary publication GPU gate");
            context->Flush();runtime.expected=colors[0];
            std::atomic<bool> done{};XrResult result=XR_ERROR_RUNTIME_FAILURE;
            std::thread consumer([&]{result=content_end_frame(s->handle,&a->info);done=true;});
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(a->composition_images[0]->last_publication.load()<5 && std::chrono::steady_clock::now()<deadline)
                std::this_thread::yield();
            bool gpu_free=false;
            while(std::chrono::steady_clock::now()<deadline && !gpu_free) {
                gpu_free=s->gpu_mutex.try_lock();
                if(gpu_free)s->gpu_mutex.unlock();else std::this_thread::yield();
            }
            const bool early=done.load();
            check(SUCCEEDED(cpu_gate->Signal(1)),"open auxiliary GPU gate");consumer.join();
            check(!early && gpu_free && XR_SUCCEEDED(result) && runtime.valid,"auxiliary copy released early or blocked producer GPU lock");
        }
        // Reproduce the live V542 cycle: game owns gpu_mutex and needs the
        // runtime gate, while physical EndFrame already owns the runtime gate.
        // The private auxiliary publisher must progress without gpu_mutex.
        // A bounded producer escape makes the old implementation FAIL rather
        // than hanging the fixture or leaving a joinable blocked thread.
        if(enabled) {
            std::atomic<bool> gpu_owned{},runtime_owned{},producer_entered{},escaped{};
            runtime.expected=colors[1];XrResult submitted=XR_ERROR_RUNTIME_FAILURE;
            std::thread producer([&] {
                std::unique_lock gpu_lock(s->gpu_mutex);gpu_owned=true;
                while(!runtime_owned)std::this_thread::yield();
                const auto limit=std::chrono::steady_clock::now()+std::chrono::seconds(1);
                while(!s->runtime_entry_mutex.try_lock()) {
                    if(std::chrono::steady_clock::now()>=limit) {escaped=true;return;}
                    std::this_thread::yield();
                }
                producer_entered=true;s->runtime_entry_mutex.unlock();
            });
            while(!gpu_owned)std::this_thread::yield();
            std::thread presenter([&] {
                submitted=with_runtime_entry(s,[&] {
                    runtime_owned=true;return content_end_frame(s->handle,&b->info);
                });
            });
            presenter.join();producer.join();
            check(!escaped && producer_entered && XR_SUCCEEDED(submitted) && runtime.valid,
                "V542 lock inversion: auxiliary publication needs GPU mutex while owning runtime gate");
        }
        // A queued snapshot owns the publication sink beyond producer teardown.
        // Capture cleanup must not destroy the single output while A/B/C exist.
        destroy_composition_capture(app);
        if(enabled) {
            check(runtime.destroys==0,"producer teardown destroyed a retained publication sink");
            runtime.expected=colors[0];
            check(XR_SUCCEEDED(content_end_frame(s->handle,&a->info)) && runtime.valid,
                "retained auxiliary pixels unavailable after producer teardown");
        }
        a.reset();b.reset();c.reset();
        order={};
        // Exercise idle auxiliary retirement on SteamVR and other runtimes,
        // including Luke's one-XR publication sink and legacy D11 outputs.
        for(bool steam:{true,false}) {
            s->dispatch->steamvr_runtime=steam;
            s->dispatch->virtual_desktop_runtime=!steam;s->vdxr_legacy=!steam;
            auto retained=capture(0xff123456);
            XrFrameEndInfo empty{XR_TYPE_FRAME_END_INFO};
            const auto before=runtime.chains.size();
            retire_unused_composition_outputs(s,&empty);
            check(runtime.chains.size()==before,"cleanup destroyed a leased auxiliary image");
            retained.reset();
            check(SUCCEEDED(wait_composition_fence(app->composition_capture->fence.Get(),
                app->composition_capture->fence_value)),"auxiliary capture completion");
            XrCompositionLayerQuad visible{XR_TYPE_COMPOSITION_LAYER_QUAD};visible.subImage.swapchain=app->handle;
            const auto* visible_layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&visible);
            auto frame=empty;frame.layerCount=1;frame.layers=&visible_layer;
            retire_unused_composition_outputs(s,&frame);
            check(runtime.chains.size()==before,"cleanup destroyed a currently submitted auxiliary output");
            for(auto generator:{xrfg::D3D12FrameGenerationMode::ofxr,
                xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
                s->frame_generation_mode=generator;
                retire_unused_composition_outputs(s,&empty);
            }
            check(runtime.chains.empty() && app->composition_capture->valid,
                "unused D11 auxiliary XR output retained or source capture discarded");
        }
    } catch(...) {cleanup();throw;}
    cleanup();
    check(runtime.valid && runtime.chains.empty() && runtime.destroys==runtime.creates,
        "auxiliary output cleanup was duplicated, leaked or used a virtual runtime handle");
    std::cout<<"Auxiliary snapshots: "<<(private_path&&afw?"3 private / 1 XR":"legacy 3 XR")
        <<", old/new/old colors, geometry, A/B, exact GPU receipt, retained teardown OK\n";
}
