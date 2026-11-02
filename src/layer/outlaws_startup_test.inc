// Included by the director test beside its existing real layer/context fixtures.
// Only the downstream runtime is a fixture; setup uses the actual layer,
// submitted native owner, SINGLETHREADED D11 device and PDAFW runtime.
struct OutlawsStartupRuntime {
    using Texture=Microsoft::WRL::ComPtr<ID3D11Texture2D>;
    struct Images {bool application{};std::array<Texture,3> textures;};
    std::shared_ptr<SessionState> state;
    std::unordered_map<XrSwapchain,Images> images;
    DWORD owner=GetCurrentThreadId();
    unsigned next{},private_attempts{},private_creates{},private_destroys{},app_creates{},wait_calls{},begin_calls{},end_calls{};
    std::size_t limit=SIZE_MAX;
    bool making_application{},producer_window=true,expect_warm{},waited{};
    XrResult app_refusal=XR_SUCCESS;
    const XrSwapchainCreateInfo* expected_info{};
    void thread() const {check(GetCurrentThreadId()==owner,"Outlaws SINGLETHREADED runtime changed owner thread");}
};
OutlawsStartupRuntime* outlaws_runtime{};
XrResult XRAPI_CALL outlaws_create(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out) {
    auto& r=*outlaws_runtime;r.thread();
    if(r.making_application) {
        ++r.app_creates;check(info==r.expected_info,"startup retry changed original app creation arguments");
        if(XR_FAILED(r.app_refusal)){const auto result=r.app_refusal;r.app_refusal=XR_SUCCESS;return result;}
    } else {++r.private_attempts;check(r.producer_window,"private cold creation reached the active consumer");++r.private_creates;}
    if(r.images.size()>=r.limit)return XR_ERROR_LIMIT_REACHED;
    OutlawsStartupRuntime::Images made;made.application=r.making_application;
    D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;
    d.MipLevels=info->mipCount;d.ArraySize=info->arraySize;d.Format=static_cast<DXGI_FORMAT>(info->format);
    d.SampleDesc.Count=info->sampleCount;d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    for(auto& image:made.textures)
        check(SUCCEEDED(r.state->d3d11_device->CreateTexture2D(&d,nullptr,image.GetAddressOf())),"startup runtime texture");
    *out=reinterpret_cast<XrSwapchain>(std::uintptr_t{0xc000}+ ++r.next);
    r.images.emplace(*out,std::move(made));return XR_SUCCESS;
}
XrResult XRAPI_CALL outlaws_enumerate(XrSwapchain handle,std::uint32_t capacity,std::uint32_t* count,
    XrSwapchainImageBaseHeader* output) {
    auto& r=*outlaws_runtime;r.thread();if(!count)return XR_ERROR_VALIDATION_FAILURE;
    *count=3;if(!capacity)return XR_SUCCESS;
    if(capacity<3)return XR_ERROR_SIZE_INSUFFICIENT;
    auto* images=reinterpret_cast<XrSwapchainImageD3D11KHR*>(output);
    for(unsigned n=0;n<3;++n)images[n].texture=r.images.at(handle).textures[n].Get();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL outlaws_destroy(XrSwapchain handle) {
    auto& r=*outlaws_runtime;r.thread();const auto found=r.images.find(handle);
    check(found!=r.images.end(),"startup retired an unowned runtime handle");
    if(!found->second.application)++r.private_destroys;
    r.images.erase(found);return XR_SUCCESS;
}
XrResult XRAPI_CALL outlaws_wait(XrSession,const XrFrameWaitInfo*,XrFrameState* frame) {
    auto& r=*outlaws_runtime;r.thread();++r.wait_calls;
    check(!r.producer_window&&r.state->luke_outlaws_frame_started,"original Wait preceded startup seal");
    check(bool(r.state->luke_afw)==r.expect_warm,"cold PDAFW initialization reached original Wait");
    r.waited=true;frame->predictedDisplayTime=2'000'000'000+ r.wait_calls*8'333'333;
    frame->predictedDisplayPeriod=8'333'333;frame->shouldRender=XR_TRUE;return XR_SUCCESS;
}
XrResult XRAPI_CALL outlaws_begin(XrSession,const XrFrameBeginInfo*) {
    auto& r=*outlaws_runtime;r.thread();++r.begin_calls;
    check(r.state->luke_outlaws_frame_started,"original Begin preceded startup seal");
    if(!r.waited)return XR_ERROR_CALL_ORDER_INVALID;
    r.waited=false;return XR_SUCCESS;
}
XrResult XRAPI_CALL outlaws_end(XrSession,const XrFrameEndInfo*) {
    auto& r=*outlaws_runtime;r.thread();++r.end_calls;return XR_SUCCESS;
}
void outlaws_startup_test(const wchar_t* marker_path,const wchar_t* provider_path) {
    using Microsoft::WRL::ComPtr;
    const auto marker=LoadLibraryW(marker_path);
    check(marker&&GetProcAddress(marker,"RealVRVersion"),"startup actual Luke marker module");
    // This fixture constructs SessionState directly. Model the real session
    // discovery boundary after loading the actual marker; luke_detected() is
    // deliberately a cached read and must not become a per-call module scan.
    xrfg::native_ingress::refresh_luke_detection();
    check(xrfg::native_ingress::luke_detected(),"startup actual Luke marker");
    test_luke_bootstrap_provider=provider_path;
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c11;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_SINGLETHREADED,nullptr,0,D3D11_SDK_VERSION,
        d11.GetAddressOf(),nullptr,c11.GetAddressOf())),"startup SINGLETHREADED hardware device");
    check((d11->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED)!=0,"startup device lost SINGLETHREADED contract");
    ComPtr<ID3D12Device> d12;ComPtr<ID3D12CommandQueue> fallback,producer;
    check(SUCCEEDED(xrfg::create_d3d12_device_for_d3d11(d11.Get(),d12.GetAddressOf(),fallback.GetAddressOf())),"startup native adapter");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(d12->CreateCommandQueue(&q,IID_PPV_ARGS(producer.GetAddressOf()))),"startup original producer");
    const auto submit_native=[&] {
        xrfg::native_ingress::configure_queue(producer.Get());
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        check(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())))&&
            SUCCEEDED(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,
                IID_PPV_ARGS(list.GetAddressOf()))),"startup original list");
        xrfg::native_ingress::observe_luke_production(list.Get());
        ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
        check(!xrfg::native_ingress::query_luke_context(owner.GetAddressOf(),queue.GetAddressOf()),"recording admitted an unsubmitted producer");
        check(SUCCEEDED(list->Close()),"startup close");ID3D12CommandList* submitted[]{list.Get()};
        producer->ExecuteCommandLists(1,submitted);
        check(xrfg::native_ingress::query_luke_context(owner.GetAddressOf(),queue.GetAddressOf())&&
            owner==d12&&queue==producer,"startup lost exact original Execute owner");
    };
    for(unsigned scenario=0;scenario<3;++scenario) {
        const bool budget=scenario==1,missing=scenario==2;
        OutlawsStartupRuntime runtime;outlaws_runtime=&runtime;runtime.state=session();auto& s=runtime.state;
        s->presenter_active=false;s->three_slot_director=false;s->single_threaded_d3d11=true;
        s->luke_afw_enabled=true;s->luke_outlaws_preframe=true;s->luke_shared_swapchains=true;s->graphics_binding_capabilities=7|32;
        s->d3d11_device=d11;s->d3d11_context=c11;s->d3d12_device=d12;s->d3d12_queue=fallback;
        s->dispatch->create_swapchain=outlaws_create;s->dispatch->enumerate_swapchain_images=outlaws_enumerate;
        s->dispatch->destroy_swapchain=outlaws_destroy;s->dispatch->wait_frame=outlaws_wait;
        s->dispatch->begin_frame=outlaws_begin;s->dispatch->end_frame=outlaws_end;
        s->dispatch->acquire_swapchain_image=[](XrSwapchain,const XrSwapchainImageAcquireInfo*,std::uint32_t* out){*out=0;return XR_SUCCESS;};
        s->dispatch->wait_swapchain_image=[](XrSwapchain,const XrSwapchainImageWaitInfo*){return XR_SUCCESS;};
        s->dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*){return XR_SUCCESS;};
        {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
        struct Cleanup {
            OutlawsStartupRuntime& runtime;ID3D12CommandQueue* producer{};bool done{};
            ~Cleanup() {
                if(done)return;
                try {
                    release_session_generation_budget(runtime.state);
                    std::vector<XrSwapchain> application;
                    for(const auto& [handle,images]:runtime.images)if(images.application)application.push_back(handle);
                    for(auto handle:application)static_cast<void>(layer_destroy_swapchain_impl(handle));
                    {std::scoped_lock lock(g_state_mutex);g_sessions.erase(runtime.state->handle);}
                    xrfg::stop_ngx_guide_capture(runtime.state->d3d12_queue.Get());
                    xrfg::native_ingress::stop_queue(producer);
                }catch(...){}
            }
        } cleanup{runtime,producer.Get()};
        if(!missing)submit_native();
        std::array<XrSwapchain,3> app{};
        std::array<XrSwapchainImageD3D11KHR,3> output;
        for(unsigned n=0;n<3;++n) {
            XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            info.width=512;info.height=n==2?264:256;info.arraySize=info.faceCount=info.sampleCount=1;
            info.mipCount=4;info.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
            if(budget&&n==2)runtime.limit=runtime.images.size();
            runtime.making_application=true;runtime.expected_info=&info;
            check(XR_SUCCEEDED(layer_create_swapchain_impl(s->handle,&info,&app[n])),"eager budget denied an original app swapchain");
            runtime.making_application=false;
            const auto before=runtime.private_creates;std::uint32_t count{};
            check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(app[n],0,&count,nullptr))&&count==3&&
                runtime.private_creates==before,"count-only enumeration performed private setup");
            for(auto& image:output)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            check(layer_enumerate_swapchain_images_impl(app[n],2,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(output.data()))==XR_ERROR_SIZE_INSUFFICIENT&&
                runtime.private_creates==before,"incomplete enum performed private setup");
            check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(app[n],3,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(output.data())))&&count==3,
                "initial original image enum");
            for(unsigned i=0;i<3;++i)check(output[i].texture==runtime.images.at(app[n]).textures[i].Get(),"eager setup replaced an original app image");
            const auto chain=find_swapchain(app[n]);
            if(!missing&&!(budget&&n==2))check(chain->frame_generation&&chain->d3d12_history&&s->luke_afw&&
                s->d3d12_queue!=producer&&s->d3d12_queue!=fallback,"full private setup did not complete in initial enum window");
        }
        if(budget)check(runtime.app_creates==4&&s->generation_budget_exhausted&&
            runtime.private_creates==runtime.private_destroys&&runtime.images.size()==3,
            "budget fallback failed to preserve all app chains or retire only private rings");
        if(missing) {
            check(!s->luke_afw && runtime.private_creates>0 &&
                find_swapchain(app[0])->frame_generation && find_swapchain(app[1])->frame_generation,
                "color-only OFXR did not arm in the existing startup window without an AFW producer");
            XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
            check(layer_begin_frame_impl(s->handle,&begin)==XR_ERROR_CALL_ORDER_INVALID&&s->luke_outlaws_frame_started,
                "first original Begin did not seal cold startup");
            submit_native(); // A late exact receipt must not reopen the consumer's setup window.
        }
        runtime.expect_warm=!missing;runtime.producer_window=false;
        const auto cold_count=runtime.private_creates;
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};XrFrameState prediction{XR_TYPE_FRAME_STATE};
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,&wait,&prediction)),"original first Wait");
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};check(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,&begin)),"original first Begin");
        for(auto& image:output)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};std::uint32_t count{};
        check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(app[0],3,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(output.data()))),"late repeated original enum");
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye) {
            views[eye]={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};views[eye].pose.orientation.w=1;
            views[eye].fov={-.7F,.7F,.7F,-.7F};views[eye].subImage.swapchain=app[eye];
            views[eye].subImage.imageRect.extent={512,256};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};layer.space=reinterpret_cast<XrSpace>(0x77);
        layer.viewCount=2;layer.views=views.data();const auto* base=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=prediction.predictedDisplayTime;
        end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&base;
        check(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end))&&runtime.end_calls==1&&
            runtime.private_creates==cold_count,"first End or late enum reopened cold private setup");
        // A new eligible chain and replacement images can become pending after
        // startup. A fresh control revision must still forward original frames
        // without the embedded-control factory reopening cold initialization.
        runtime.limit=SIZE_MAX;
        XrSwapchainCreateInfo late_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        late_info.width=512;late_info.height=256;late_info.arraySize=late_info.faceCount=late_info.sampleCount=1;
        late_info.mipCount=4;late_info.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        late_info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        XrSwapchain late{};runtime.making_application=true;runtime.expected_info=&late_info;
        check(XR_SUCCEEDED(layer_create_swapchain_impl(s->handle,&late_info,&late)),"late original app creation");
        runtime.making_application=false;
        for(auto& image:output)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(late,3,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(output.data()))),"late eligible original enum");
        auto late_chain=find_swapchain(late);
        if(!budget)check(late_chain->generation_eligible_pending,"late chain did not exercise pending eligibility");
        const auto attempts=runtime.private_attempts;
        s->control_revision=0; // Same desired tuple, observed anew on actual EndFrame.
        views[0].subImage.swapchain=late;
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,&wait,&prediction)),"late chain original Wait");
        check(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,&begin)),"late chain original Begin");
        end.displayTime=prediction.predictedDisplayTime;
        check(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end))&&runtime.end_calls==2&&
            runtime.private_attempts==attempts&&!late_chain->frame_generation&&!late_chain->generation_declined,
            "embedded control reopened cold setup for a late eligible chain");
        auto& replacement=runtime.images.at(app[0]);D3D11_TEXTURE2D_DESC replacement_info{};
        replacement.textures[0]->GetDesc(&replacement_info);
        std::array<OutlawsStartupRuntime::Texture,3> replaced;
        for(auto& texture:replaced)check(SUCCEEDED(d11->CreateTexture2D(&replacement_info,nullptr,texture.GetAddressOf())),
            "replacement original app images");
        replacement.textures=std::move(replaced);
        for(auto& image:output)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(app[0],3,&count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(output.data()))),"replacement original enum");
        const auto replacement_chain=find_swapchain(app[0]);
        const auto replacement_declined=replacement_chain->generation_declined;
        check(!replacement_chain->frame_generation&&!replacement_chain->d3d12_history,
            "replacement enum retained the retired generation or history");
        if(!budget)check(replacement_chain->generation_eligible_pending,"replacement images did not exercise pending eligibility");
        s->control_revision=0;views[0].subImage.swapchain=app[0];
        check(XR_SUCCEEDED(layer_wait_frame_impl(s->handle,&wait,&prediction)),"replacement original Wait");
        check(XR_SUCCEEDED(layer_begin_frame_impl(s->handle,&begin)),"replacement original Begin");
        end.displayTime=prediction.predictedDisplayTime;
        check(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&end))&&runtime.end_calls==3&&
            runtime.private_attempts==attempts&&!replacement_chain->frame_generation&&
            replacement_chain->generation_declined==replacement_declined,
            "embedded control reopened cold setup for replaced original images");
        runtime.making_application=true;XrSwapchainCreateInfo unrelated{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        runtime.expected_info=&unrelated;runtime.app_refusal=XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;XrSwapchain rejected{};
        const auto creates=runtime.app_creates;
        check(layer_create_swapchain_impl(s->handle,&unrelated,&rejected)==XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED&&
            runtime.app_creates==creates+1,"unrelated app refusal was retried or hidden");
        runtime.making_application=false;
        release_session_generation_budget(s);
        check(XR_SUCCEEDED(layer_destroy_swapchain_impl(late)),"late app teardown");
        for(auto handle:app)check(XR_SUCCEEDED(layer_destroy_swapchain_impl(handle)),"startup app teardown");
        check(runtime.images.empty(),"startup teardown left owned runtime images");
        {std::scoped_lock lock(g_state_mutex);g_sessions.erase(s->handle);}
        xrfg::stop_ngx_guide_capture(s->d3d12_queue.Get());xrfg::native_ingress::stop_queue(producer.Get());
        cleanup.done=true;
    }
    test_luke_bootstrap_provider.clear();FreeLibrary(marker);outlaws_runtime=nullptr;
    std::cout<<"Outlaws startup: exact native Execute, SINGLETHREADED initial enum/all three colors, app-priority budget retry, missing/late context, late eligible/replaced images and original Wait/Begin/End PASS\n";
}
