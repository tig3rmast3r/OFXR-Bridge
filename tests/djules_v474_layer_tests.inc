// Actual production routing and lifecycle functions, not a parallel model.
void djules_v474_policy_lifecycle_test() {
    auto s=session();s->presenter_active=false;s->three_slot_director=false;
    s->graphics_binding=SessionGraphicsBinding::d3d12;
    s->application_wait_thread_id=1;s->application_end_thread_id=1;
    s->dispatch->steamvr_runtime=true;
    s->vdxr_legacy=false;
    for (int backend:{0,1,2,3}) {
        s->djules_route=backend==3;s->nvidia_new_route=backend>=2;
        s->optical_flow_backend=backend?xrfg::D3D12OpticalFlowBackend::nvidia:xrfg::D3D12OpticalFlowBackend::fidelity_fx;
        check(presenter_holds_djules_prime(*s)==(backend!=3),"V439 only Djules SteamVR prime skips hold");
        s->djules_history_wait_enabled=ordinary_djules_route(*s);
        check(history_capture_wait_limit_us(*s)==(backend==3?11111u:0u),"V441 no wait on other routes");
    }
    s->djules_route=s->nvidia_new_route=true;
    for(auto mode:{xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
        s->frame_generation_mode=mode;
        check(!ordinary_djules_route(*s) && presenter_holds_djules_prime(*s),"native/FSR excluded");
    }
    s->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
    s->hybrid_requested=true;check(!ordinary_djules_route(*s),"Hybrid excluded");s->hybrid_requested=false;
    s->luke_afw_enabled=true;check(!ordinary_djules_route(*s),"Luke excluded");s->luke_afw_enabled=false;
    s->djules_history_wait_enabled=true;s->history_capture_wait_period=13'888'888;
    check(history_capture_wait_limit_us(*s)==13888,"observed period, not fixed 11ms");
    s->history_capture_wait_period=33'333'333;
    check(history_capture_wait_limit_us(*s)==20000,"per capture 20ms ceiling");
    s->dispatch->steamvr_runtime=false;s->vdxr_legacy=true;
    check(presenter_holds_djules_prime(*s),"other runtimes retain prime hold");
    s->dispatch->inline_unless_pipelined=s->dispatch->virtual_desktop_runtime=true;
    s->split_frame_loop=true;s->application_wait_thread_id=1;s->application_end_thread_id=2;
    check(djules_inline_runtime(*s),"fresh VD split loop remains inline");
    s->presenter_restore_after_bypass=true;
    check(!djules_inline_runtime(*s),"V442 restores only previous Djules VD presenter past latch");
    s->dispatch->virtual_desktop_runtime=false;s->vdxr_legacy=false;
    check(djules_inline_runtime(*s),"V442 does not extend to other runtimes");
    s->dispatch->virtual_desktop_runtime=true;s->vdxr_legacy=true;
    s->graphics_binding=SessionGraphicsBinding::d3d11;
    check(presenter_forbidden(*s),"hard D11 split-driver prohibition retained");
    s->graphics_binding=SessionGraphicsBinding::d3d12;
    s->presenter_restore_after_bypass=false;s->presenter_active=true;
    check(!djules_inline_runtime(*s),"running VD presenter retains route after handoff");
    s->presenter_active=false;
    check(djules_inline_runtime(*s),"demotion/disarm cannot restart from old restore flag");

    // V443: an actual create while steady, including shared non-Djules routes.
    s->dispatch->create_swapchain=+[](XrSession,const XrSwapchainCreateInfo*,XrSwapchain* out)->XrResult {
        *out=reinterpret_cast<XrSwapchain>(0x474001);return XR_SUCCESS;
    };
    s->dispatch->destroy_swapchain=+[](XrSwapchain)->XrResult{return XR_SUCCESS;};
    {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
    struct Remove {XrSession h;~Remove(){std::scoped_lock lock(g_state_mutex);g_sessions.erase(h);}} cleanup{s->handle};
    s->graphics_binding=SessionGraphicsBinding::none;
    for(auto mode:{xrfg::D3D12FrameGenerationMode::ofxr,xrfg::D3D12FrameGenerationMode::fsr3,xrfg::D3D12FrameGenerationMode::native_dlss}) {
        s->frame_generation_mode=mode;s->generation_steady_state_established=true;
        s->presenter_active=true;s->presenter_failure=XR_SUCCESS;s->presenter_stop_requested=false;
        s->presenter_last_frame=frame(1)->frame;
        s->generation_resume_wall_time={};
        // The actual resource guard must wait for outstanding owned work.
        s->outstanding_presenter_submissions=1;
        std::atomic<bool> finished{};
        std::thread finish([&] {std::this_thread::sleep_for(std::chrono::milliseconds(3));
            {std::scoped_lock lock(s->presenter_mutex);finished=true;s->outstanding_presenter_submissions=0;}
            s->presenter_condition.notify_all();});
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=160;ci.height=272;
        ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        XrSwapchain chain{};const auto result=layer_create_swapchain_impl(s->handle,&ci,&chain);finish.join();
        check(XR_SUCCEEDED(result) && finished,"actual create drains outstanding presenter ownership");
        check(!s->presenter_last_frame,"retained repeat invalidated by lifetime guard");
        check(s->generation_resume_wall_time==std::chrono::steady_clock::time_point{},"no creation-only timer on any shared route");
        s->presenter_active=false;check(XR_SUCCEEDED(layer_destroy_swapchain_impl(chain)),"actual overlay destroy");
        schedule_generation_quarantine(s,GenerationQuarantineReason::projection_changed);
        const auto deadline=s->generation_resume_wall_time;
        check(deadline>std::chrono::steady_clock::now(),"projection-change quarantine remains");
        check(XR_SUCCEEDED(layer_create_swapchain_impl(s->handle,&ci,&chain)),"create during existing quarantine");
        check(s->generation_resume_wall_time==deadline,"create neither clears nor extends other quarantine");
        check(XR_SUCCEEDED(layer_destroy_swapchain_impl(chain)),"second overlay destroy");
    }
    std::cout<<"V474 scoped route policy and shared lifecycle PASS\n";
}

unsigned v474_runtime_releases{};
XrResult v474_release_result=XR_SUCCESS;
void djules_v474_bridge_test() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&context)),"D11 bridge fixture device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;
    check(SUCCEEDED(d11.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)),"same adapter");
    ComPtr<ID3D12Device> d12;check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12))),"D12 bridge fixture device");
    D3D12_COMMAND_QUEUE_DESC qd{};ComPtr<ID3D12CommandQueue> queue;
    check(SUCCEEDED(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue))),"bridge queue");
    auto s=session();s->presenter_active=false;s->three_slot_director=false;
    s->graphics_binding=SessionGraphicsBinding::d3d12;s->d3d12_device=d12;s->d3d12_queue=queue;
    s->menu_enabled=true;s->dlss_motion_vectors=false;s->luke_afw_enabled=false;s->djules_history_wait_enabled=false;
    s->dispatch->release_swapchain_image=+[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult {
        ++v474_runtime_releases;return v474_release_result;
    };
    s->dispatch->acquire_swapchain_image=+[](XrSwapchain,const XrSwapchainImageAcquireInfo*,std::uint32_t* index)->XrResult {*index=0;return XR_SUCCESS;};
    s->dispatch->wait_swapchain_image=+[](XrSwapchain,const XrSwapchainImageWaitInfo*)->XrResult{return XR_SUCCESS;};
    s->dispatch->destroy_swapchain=+[](XrSwapchain)->XrResult{return XR_SUCCESS;};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=8;ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=8;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> runtime;
    check(SUCCEEDED(d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&runtime))),"runtime texture");
    auto chain=std::make_shared<SwapchainState>(s,ci);chain->handle=reinterpret_cast<XrSwapchain>(0x474002);
    chain->bridge=std::make_shared<xrfg::D3D11BridgeSwapchain>();
    ID3D12Resource* images[]{runtime.Get()};xrfg::D3D11BridgeSwapchainDescription request{};
    request.requested_format=DXGI_FORMAT_R8G8B8A8_UNORM;request.requested_sample_count=1;std::uint32_t stage{};
    check(SUCCEEDED(chain->bridge->initialize(d11.Get(),context.Get(),d12.Get(),queue.Get(),images,request,&stage)),"real D11/D12 bridge");
    chain->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    check(SUCCEEDED(chain->d3d12_history->initialize(d12.Get(),queue.Get(),images,D3D12_RESOURCE_STATE_RENDER_TARGET)),"history initialized");
    ComPtr<ID3D11RenderTargetView> rtv;check(SUCCEEDED(d11->CreateRenderTargetView(chain->bridge->d3d11_images()[0],nullptr,&rtv)),"source RTV");
    const auto paint=[&](float red){const float color[]{red,0,0,1};context->ClearRenderTargetView(rtv.Get(),color);};
    const auto waited=[&]{chain->acquired_indices={0};chain->waited_count=1;chain->ownership_tracking_valid=true;};
    {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
    struct Remove {XrSwapchain h;~Remove(){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(h);}} cleanup{chain->handle};
    observe_application_frame_thread(*s,true);v474_runtime_releases=0;
    waited();paint(.25F);XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"application deferred release");
    check(v474_runtime_releases==0 && chain->pending_bridge_release && !chain->last_released_capture,"copy and capture deferred together");
    paint(.5F);release_pending_bridge_images(s);
    check(v474_runtime_releases==1 && !chain->pending_bridge_release && chain->last_released_capture,"EndFrame flush commits capture and release");
    check(SUCCEEDED(chain->bridge->wait_for_idle()) && SUCCEEDED(chain->d3d12_history->wait_for_idle()),"bridge/history GPU retired");
    const auto red_byte=[&](ID3D12Resource* texture,D3D12_RESOURCE_STATES state) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        const auto td=texture->GetDesc();d12->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;bd.Height=bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES rh{};rh.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> buffer;
        check(SUCCEEDED(d12->CreateCommittedResource(&rh,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer))),"readback buffer");
        ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;
        check(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc))) && SUCCEEDED(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list))),"readback list");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={texture,0,state,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        check(SUCCEEDED(list->Close()),"close readback");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        ComPtr<ID3D12Fence> fence;check(SUCCEEDED(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))) && SUCCEEDED(queue->Signal(fence.Get(),1)),"readback fence");
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event && SUCCEEDED(fence->SetEventOnCompletion(1,event)),"readback event");
        const auto wait=WaitForSingleObject(event,10000);CloseHandle(event);check(wait==WAIT_OBJECT_0,"readback completion");
        void* mapped{};D3D12_RANGE range{0,1};check(SUCCEEDED(buffer->Map(0,&range,&mapped)),"map readback");const auto red=*static_cast<unsigned char*>(mapped);buffer->Unmap(0,nullptr);return red;
    };
    const auto observed_runtime=red_byte(runtime.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    std::cerr<<"bridge runtime red="<<unsigned(observed_runtime)<<" path="<<unsigned(chain->bridge->path())<<"\n";
    check(observed_runtime>=127 && observed_runtime<=128,"runtime displays late rendering, not release-time 64");
    ID3D12Resource* captured{};ID3D12Fence* captured_fence{};xrfg::D3D12HistoryConsumerLease lease{};
    check(SUCCEEDED(chain->d3d12_history->acquire_consumer(*chain->last_released_capture,&lease,&captured,&captured_fence)),"inspect actual committed history");
    ComPtr<ID3D12Resource> owned;owned.Attach(captured);ComPtr<ID3D12Fence> owned_fence;owned_fence.Attach(captured_fence);
    check(red_byte(owned.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)==observed_runtime,"history agrees with runtime late rendering");chain->d3d12_history->cancel_consumer(lease);
    waited();check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"next deferred release");
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};std::uint32_t index{};
    check(XR_SUCCEEDED(layer_acquire_swapchain_image_impl(chain->handle,&acquire,&index)) && v474_runtime_releases==2,"reacquire flushes pending release first");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    check(XR_SUCCEEDED(layer_wait_swapchain_image_impl(chain->handle,&wait)),"wait after reacquire");
    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"third pending release");
    check(XR_SUCCEEDED(layer_wait_swapchain_image_impl(chain->handle,&wait)) && v474_runtime_releases==3,"wait flushes pending first");
    waited();s->application_end_thread=0;
    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)) && v474_runtime_releases==4,"unknown EndFrame thread remains immediate");
    waited();s->application_end_thread=GetCurrentThreadId();s->luke_afw_enabled=true;
    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)) && v474_runtime_releases==5,"Luke bridge release remains immediate");s->luke_afw_enabled=false;
    waited();std::thread other([&]{check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"cross-thread release");});other.join();
    check(v474_runtime_releases==6 && !chain->pending_bridge_release,"cross-thread bridge release remains immediate");
    const auto last_successful_capture=chain->last_released_capture->serial;
    waited();check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"pending failed release");
    v474_release_result=XR_ERROR_RUNTIME_FAILURE;release_pending_bridge_images(s);v474_release_result=XR_SUCCESS;
    check(v474_runtime_releases==7 && !chain->pending_bridge_release && chain->last_released_capture &&
        chain->last_released_capture->serial==last_successful_capture,"failed downstream release does not commit a new history ticket");
    waited();check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"pending before destruction");
    check(XR_SUCCEEDED(layer_destroy_swapchain_impl(chain->handle)) && v474_runtime_releases==7,"destroy drops deferred release, no phantom copy");
    std::cout<<"V474 actual D11 bridge late runtime/history GPU pixels and ordering PASS\n";
}

// A Vulkan bridge's release waits for xrEndFrame from any thread: Star Citizen
// releases at the top of its frame, before rendering, and ends from three
// threads. No GPU here; the bridge is never initialized, so its copy fails
// and the runtime is never reached.
unsigned vulkan_deferred_runtime_releases{};
void vulkan_deferred_release_test() {
    auto s=session();s->presenter_active=false;s->three_slot_director=false;
    s->graphics_binding=SessionGraphicsBinding::d3d12;s->luke_afw_enabled=false;s->dlss_motion_vectors=false;
    s->graphics_binding_capabilities|=8ULL; // its own synthesis queue, as every bridged Vulkan session
    s->dispatch->release_swapchain_image=+[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult {
        ++vulkan_deferred_runtime_releases;return XR_SUCCESS;
    };
    s->dispatch->destroy_swapchain=+[](XrSwapchain)->XrResult{return XR_SUCCESS;};
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=8;ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto chain=std::make_shared<SwapchainState>(s,ci);chain->handle=reinterpret_cast<XrSwapchain>(0x474003);
    chain->vulkan_bridge=std::make_shared<xrfg::VulkanBridgeSwapchain>();
    const auto waited=[&]{chain->acquired_indices={0};chain->waited_count=1;chain->ownership_tracking_valid=true;};
    {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
    struct Remove {XrSwapchain h;~Remove(){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(h);}} cleanup{chain->handle};
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    waited();s->application_end_thread=0;
    static_cast<void>(layer_release_swapchain_image_impl(chain->handle,&release));
    check(!chain->pending_bridge_release,"no deferral before the first xrEndFrame");
    observe_application_frame_thread(*s,true);vulkan_deferred_runtime_releases=0;
    waited();check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"Vulkan release deferred");
    check(chain->pending_bridge_release==0U && vulkan_deferred_runtime_releases==0,"Vulkan copy waits for xrEndFrame");
    release_pending_bridge_images(s);
    check(!chain->pending_bridge_release,"xrEndFrame runs the deferred Vulkan release");
    waited();std::thread other([&]{check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,&release)),"cross-thread Vulkan release");});other.join();
    check(chain->pending_bridge_release==0U && vulkan_deferred_runtime_releases==0,"Vulkan release deferred from another thread");
    check(XR_SUCCEEDED(layer_destroy_swapchain_impl(chain->handle)) && vulkan_deferred_runtime_releases==0,"destroy drops the deferred Vulkan release");
    std::cout<<"Vulkan bridge release deferred to xrEndFrame PASS\n";
}

void djules_v474_vd_restore_test() {
    auto s=session();s->presenter_active=false;s->three_slot_director=false;
    s->graphics_binding=SessionGraphicsBinding::d3d12;
    s->dispatch->virtual_desktop_runtime=s->dispatch->inline_unless_pipelined=true;
    s->application_wait_thread_id=s->application_end_thread_id=GetCurrentThreadId();
    auto desired=xrfg::embedded::snapshot().desired;
    desired.enabled=true;desired.backend=3;desired.frame_generation=0;desired.hybrid=false;
    check(xrfg_fixture::request_control(desired),"VD request ordinary Djules");
    check(SUCCEEDED(apply_embedded_control(s)),"actual route application");
    s->luke_afw_enabled=false;
    {std::scoped_lock lock(g_state_mutex);g_sessions[s->handle]=s;}
    struct Cleanup {std::shared_ptr<SessionState> state;~Cleanup(){stop_continuous_presenter(state);
        std::scoped_lock lock(g_state_mutex);g_sessions.erase(state->handle);}} cleanup{s};
    check(start_continuous_presenter(s,nullptr,true),"VD initial actual presenter start");
    s->split_frame_loop=true;s->application_wait_thread_id=GetCurrentThreadId()+1;
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};info.displayTime=next_time;
    info.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    // The production borrowed-frame bypass actually stops/join its workers.
    check(XR_SUCCEEDED(layer_end_frame_impl(s->handle,&info)),"actual VD bypass submits borrowed frame");
    check(!continuous_presenter_active(s) && s->presenter_restore_after_bypass,"VD bypass records a previously valid presenter");
    check(!djules_inline_runtime(*s),"handoff latch permits restoration after actual bypass");
    check(start_continuous_presenter(s,nullptr,true),"actual Djules presenter restores past handoff latch");
    check(!s->presenter_restore_after_bypass && !djules_inline_runtime(*s),"start consumes restoration and keeps active route");
    stop_continuous_presenter(s);
    check(djules_inline_runtime(*s),"stopped without bypass receipt stays inline");
    s->presenter_restore_after_bypass=true;reset_frame_bookkeeping(s);
    check(!s->presenter_restore_after_bypass,"session restart clears restoration receipt");
    std::cout<<"V474 actual VD presenter bypass/stop/restore/handoff/reset PASS\n";
}
