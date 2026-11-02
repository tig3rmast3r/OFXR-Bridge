// Real original D12 Execute hook, COM ownership, and production layer bootstrap.
void luke_standalone_context_test(bool active_alias_history=false, bool no_helper_context=false,
    bool stale_synthesis=false,bool resource_lifecycle=false) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> c11;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d11,nullptr,&c11)),"D11 device");
    ComPtr<ID3D12Device> d12;ComPtr<ID3D12CommandQueue> private_queue,original_queue;
    check(SUCCEEDED(xrfg::create_d3d12_device_for_d3d11(d11.Get(),&d12,&private_queue)),"private bridge");
    D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(SUCCEEDED(d12->CreateCommandQueue(&desc,IID_PPV_ARGS(&original_queue))),"pre-existing game queue");
    if (!no_helper_context) xrfg::native_ingress::configure_queue(private_queue.Get());
    ComPtr<ID3D12Device> queried;ComPtr<ID3D12CommandQueue> q;
    check(!query_luke_ross_d3d12_context(&queried,&q),"ordinary host rejected");
    auto marker=LoadLibraryW((current_layer_directory()/L"luke-runtime-fixture.dll").c_str());
    // Model the production session boundary after loading the fixture DLL;
    // luke_detected() is intentionally a cached read, not a loader poll.
    xrfg::native_ingress::refresh_luke_detection();
    check(marker && xrfg::native_ingress::luke_detected(),"Luke fixture detected");
    check(!query_luke_ross_d3d12_context(&queried,&q),"queue creation is not original production");
    auto state=session();state->presenter_active=false;state->three_slot_director=false;
    if(resource_lifecycle){state->dispatch->steamvr_runtime=true;state->vdxr_legacy=false;}
    state->luke_afw_enabled=true;state->graphics_binding_capabilities=no_helper_context?3:7;
    state->d3d11_device=d11;state->d3d11_context=c11;
    if (!no_helper_context) {state->d3d12_device=d12;state->d3d12_queue=private_queue;}
    initialize_luke_synthesis_queue(*state);
    ComPtr<ReportedDeviceAlias> startup_alias;
    if(stale_synthesis) {
        startup_alias.Attach(new ReportedDeviceAlias(d12.Get(),false,true));
        state->d3d12_device=startup_alias;
        ComPtr<QueueWithReportedDeviceAlias> startup_queue;
        startup_queue.Attach(new QueueWithReportedDeviceAlias(
            state->luke_synthesis_queue.Get(),startup_alias.Get()));
        state->luke_synthesis_queue=startup_queue;
        check(d3d_owner::same(startup_alias.Get(),d12.Get()),
            "regression requires verified alias backing, not a foreign adapter");
    }
    auto early_synthesis=state->luke_synthesis_queue;
    check(bool(early_synthesis)==!no_helper_context && !state->luke_afw &&
        !(state->graphics_binding_capabilities&8),"early OFXR lane must not confer native AFW authority");
    initialize_late_luke_context(state);
    check(!state->luke_afw && state->d3d12_queue.Get()==(no_helper_context?nullptr:private_queue.Get()),"cold bootstrap stays bypass");
    auto pending=std::make_shared<SwapchainState>(state,XrSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO});
    pending->generation_eligible_pending=true;
    state->menu_enabled=false; // AFW-only still waits for the exact native producer.
    check(ensure_frame_generation(pending) && pending->generation_eligible_pending &&
        !pending->generation_declined && !pending->frame_generation,
        "cold native handoff preserves projection eligibility without unrelated GPU allocations");
    state->menu_enabled=true;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))),"allocator");
    check(SUCCEEDED(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"list");
    check(SUCCEEDED(list->Close()),"unrelated close");
    ID3D12CommandList* unrelated[]{list.Get()};original_queue->ExecuteCommandLists(1,unrelated);
    ComPtr<ID3D12Fence> fence;check(SUCCEEDED(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"fence");
    check(SUCCEEDED(original_queue->Signal(fence.Get(),1)),"fence signal");
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(fence->GetCompletedValue()<1 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    check(fence->GetCompletedValue()>=1,"unrelated list retired");
    check(!query_luke_ross_d3d12_context(&queried,&q),"unrelated execution is not SR/RR evidence");
    check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr)),"list reset");
    xrfg::native_ingress::observe_luke_production(list.Get());
    check(!query_luke_ross_d3d12_context(&queried,&q),"recording alone is not submission");
    check(SUCCEEDED(list->Close()),"close");ID3D12CommandList* lists[]{list.Get()};
    original_queue->ExecuteCommandLists(1,lists);
    check(query_luke_ross_d3d12_context(&queried,&q),"actual queue context export resolves");
    check(q==original_queue && queried==d12 && q!=private_queue,"original exact native owner, not bridge queue");
    ComPtr<ReportedDeviceAlias> alias;
    ComPtr<ID3D12Resource> color;
    struct RegisteredHistory {
        XrSwapchain handle{};
        ~RegisteredHistory(){if(handle){std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);}}
    } registered;
    if(active_alias_history) {
        alias.Attach(new ReportedDeviceAlias(d12.Get(),false,true));
        state->d3d12_device=alias;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=texture.Height=16;texture.DepthOrArraySize=texture.MipLevels=1;
        texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;
        check(SUCCEEDED(d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&color))),"active alias history source");
        pending->d3d12_history=std::make_shared<xrfg::D3D12SwapchainHistory>();
        ID3D12Resource* sources[]{color.Get()};
        check(SUCCEEDED(pending->d3d12_history->initialize(d12.Get(),private_queue.Get(),sources,
            D3D12_RESOURCE_STATE_COMMON)),"active color history before native context");
        pending->handle=reinterpret_cast<XrSwapchain>(0x540);
        {std::scoped_lock lock(g_state_mutex);g_swapchains[pending->handle]=pending;}
        registered.handle=pending->handle;
        check(d3d_owner::same(alias.Get(),d12.Get()),"alias does not share verified backing");
    }
    state->application_frame_in_progress=true;initialize_late_luke_context(state);
    check(!state->luke_afw && state->d3d12_queue.Get()==(no_helper_context?nullptr:private_queue.Get()),"in-flight frame defers binding");
    state->application_frame_in_progress=false;initialize_late_luke_context(state);
    check(state->luke_afw && (state->graphics_binding_capabilities&8) &&
        !(state->graphics_binding_capabilities&4) && state->d3d12_queue &&
        state->d3d12_queue!=original_queue &&
        (active_alias_history ? state->d3d12_queue==private_queue : state->d3d12_queue!=private_queue),
        "late AFW must retain active history queue, or select a private queue before history");
    check(state->luke_synthesis_queue && state->luke_synthesis_queue!=state->d3d12_queue &&
        state->luke_synthesis_queue!=original_queue && !session_uses_shared_queue(*state),
        "late Luke OFXR must have a separate lane without moving producer/AFW ownership");
    if(early_synthesis)check(stale_synthesis ? state->luke_synthesis_queue!=early_synthesis :
        state->luke_synthesis_queue==early_synthesis,
        "late native handoff must retire a stale queue while preserving a compatible lane");
    ComPtr<ID3D12Device> synthesis_owner;
    check(SUCCEEDED(state->luke_synthesis_queue->GetDevice(IID_PPV_ARGS(&synthesis_owner))) &&
        synthesis_owner==state->d3d12_device,
        "late synthesis queue must report the exact allocation device");
    if(active_alias_history) {
        xrfg::configure_dlss_motion_vector_tracking(true);
        std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
        for(unsigned n=0;n<2;++n) {
            ComPtr<ID3D12CommandAllocator> capture_allocator;ComPtr<ID3D12GraphicsCommandList> capture_list;
            check(SUCCEEDED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&capture_allocator))) &&
                SUCCEEDED(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,capture_allocator.Get(),nullptr,
                    IID_PPV_ARGS(&capture_list))),"alias guide command list");
            xrfg::DlssMotionVectorPublication publication;
            publication.stream=0x540;publication.output=publication.motion_vectors=color.Get();
            publication.output_width=publication.output_height=publication.motion_width=publication.motion_height=16;
            publication.producer_queue=original_queue.Get();publication.producer_command_list=capture_list.Get();
            publication.verified_producer_device=d12.Get();publication.resource_state=D3D12_RESOURCE_STATE_COMMON;
            publication.protect_snapshot_lifetime=true;publication.camera_data_valid=true;
            frame=xrfg::publish_dlss_motion_vectors(publication);check(frame!=nullptr,"native guide snapshot");
            check(SUCCEEDED(capture_list->Close()),"alias guide close");
            ID3D12CommandList* capture_lists[]{capture_list.Get()};original_queue->ExecuteCommandLists(1,capture_lists);
            xrfg::report_dlss_motion_vector_submission(original_queue.Get(),1,capture_lists);
            check(SUCCEEDED(original_queue->Signal(fence.Get(),n+2)) &&
                SUCCEEDED(wait_composition_fence(fence.Get(),n+2,3000)),"native guide snapshots retired");
        }
        const auto canonical=xrfg::resolve_luke_guide_token(frame->color_origin_token,private_queue.Get(),d12.Get());
        const auto stale=xrfg::resolve_luke_guide_token(frame->color_origin_token,private_queue.Get(),alias.Get());
        const auto actual=xrfg::resolve_luke_guide_token(frame->color_origin_token,state->d3d12_queue.Get(),state->d3d12_device.Get());
        check(canonical && !stale && actual && state->d3d12_device==d12 && pending->d3d12_history->initialized(),
            "active color-history alias prevented exact native AFW guide admission");
        check(SUCCEEDED(private_queue->Signal(fence.Get(),4)) && SUCCEEDED(wait_composition_fence(fence.Get(),4,3000)),
            "consumer joins retired");
        xrfg::retire_dlss_motion_vector_stream(0x540);
        std::cout<<"Late native alias: original wrong alias refuses guides; actual session admits exact native pair, active history and queue retained PASS\n";
    } else check(state->d3d12_device==d12,"late AFW canonical device");
    queried.Reset();q.Reset();
    check(query_luke_ross_d3d12_context(&queried,&q)&&q==original_queue&&queried==d12,
        "private execution queue replaced original SR/RR provenance");
    auto execution_queue=state->d3d12_queue;
    {
        auto initialized=state->luke_afw;initialize_late_luke_context(state);
        check(state->luke_afw==initialized&&state->d3d12_queue==execution_queue,"no repeat initialization");
    }
    if(resource_lifecycle) {
        check(active_alias_history && pending->d3d12_history && state->luke_afw,
            "provider lifecycle requires actual active color history and initialized AFW");
        const auto retained_synthesis=state->luke_synthesis_queue;
        auto* const retained_history=pending->d3d12_history.get();
        xrfg::D3D12HistoryCaptureTicket capture{};
        check(SUCCEEDED(pending->d3d12_history->capture(0,&capture)) &&
            SUCCEEDED(pending->d3d12_history->commit(capture)),"active provider-lifecycle color capture");
        const auto snapshot=[&]() {
            xrfg::D3D12HistoryConsumerLease lease{};ComPtr<ID3D12Resource> resource;
            ComPtr<ID3D12Fence> production;
            check(SUCCEEDED(pending->d3d12_history->acquire_consumer(capture,&lease,&resource,&production)),
                "retained color snapshot resolves through original history");
            pending->d3d12_history->cancel_consumer(lease);
            check(SUCCEEDED(wait_composition_fence(production.Get(),capture.fence_value,3000)),
                "provider-lifecycle color writer retires");
            return resource;
        };
        auto retained_snapshot=snapshot();
        const auto request=[&](bool ofxr,int afw) {
            auto settings=xrfg::autonomous::snapshot().desired;
            settings.enabled=ofxr;settings.afw_mode=afw;
            check(xrfg::autonomous::request(settings),"canonical provider-lifecycle desired settings");
            xrfg::autonomous::apply_runtime_settings();
            check(xrfg::embedded::snapshot().desired.enabled==ofxr &&
                xrfg::embedded::afw_snapshot().desired.mode==afw,"canonical AFW/OFXR publication");
            check(SUCCEEDED(apply_embedded_control(state)),"apply independent AFW/OFXR resource lifecycle");
        };
        const auto native_context_preserved=[&]() {
            ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> producer;
            return state->d3d12_device==d12 && state->d3d12_queue==execution_queue &&
                state->luke_synthesis_queue==retained_synthesis && (state->graphics_binding_capabilities&8) &&
                query_luke_ross_d3d12_context(&owner,&producer) && owner==d12 && producer==original_queue;
        };
        std::weak_ptr<xrfg::LukeAfwSynthesizer> first=state->luke_afw;
        request(true,0);
        check(!state->luke_afw && first.expired(),"AFW OFF retained its unused provider while OFXR remained ON");
        check(state->menu_enabled && native_context_preserved() && pending->d3d12_history.get()==retained_history &&
            snapshot()==retained_snapshot,"AFW OFF disturbed active OFXR owner, queues or color history backing");
        request(true,3);initialize_late_luke_context(state);
        check(state->luke_afw && state->luke_afw->initialized() && native_context_preserved(),
            "AFW re-enable failed to recover retained capability8 using the exact execution queue");
        check(pending->d3d12_history.get()==retained_history && snapshot()==retained_snapshot,
            "AFW re-enable replaced active OFXR history backing");
        auto* const second=state->luke_afw.get();
        initialize_late_luke_context(state);
        check(state->luke_afw.get()==second && native_context_preserved(),"active AFW repeated provider initialization");
        std::weak_ptr<xrfg::LukeAfwSynthesizer> second_lifetime=state->luke_afw;
        request(false,3);initialize_late_luke_context(state);
        check(!state->menu_enabled && state->luke_afw.get()==second && !second_lifetime.expired() && native_context_preserved(),
            "OFXR OFF retired a still-selected AFW provider");
        request(true,3);initialize_late_luke_context(state);
        check(state->menu_enabled && state->luke_afw.get()==second && native_context_preserved(),
            "OFXR re-enable replaced the active AFW provider");
        retained_snapshot.Reset();
        request(false,0);
        check(!state->menu_enabled && !state->luke_afw && second_lifetime.expired() && native_context_preserved(),
            "both OFF retained an AFW provider or lost original native producer authority");
        initialize_late_luke_context(state);
        check(!state->luke_afw,"OFF context polling reallocated an unused AFW provider");
        request(true,3);initialize_late_luke_context(state);
        check(state->luke_afw && native_context_preserved(),"AFW ON after both OFF failed or substituted the game queue");
        auto* const third=state->luke_afw.get();initialize_late_luke_context(state);
        check(state->luke_afw.get()==third,"second AFW activation repeatedly initialized its provider");
        std::weak_ptr<xrfg::LukeAfwSynthesizer> final_lifetime=state->luke_afw;
        request(false,0);
        check(!state->luke_afw && final_lifetime.expired(),"final AFW resource retirement retained its provider");
        std::cout<<"Luke provider lifecycle: AFW OFF with OFXR ON frees provider; history/owner/queues retained; capability8 re-enable twice; both OFF cold PASS\n";
    }
    xrfg::stop_ngx_guide_capture(original_queue.Get());
    queried.Reset();q.Reset();
    check(!query_luke_ross_d3d12_context(&queried,&q),"retired producer does not survive reset");
    check(!xrfg::native_ingress::query_luke_context(nullptr,&q) && !q,"invalid output rejected");
    FreeLibrary(marker);
    std::cout<<"Luke standalone: exact submitted producer retained, separate execution queue, delayed bootstrap, real AFW provider, retirement\n";
}
