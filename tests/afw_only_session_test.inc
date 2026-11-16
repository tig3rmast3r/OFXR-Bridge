// The original XR runtime is mocked; session routing and native terminal
// submission below are the shipping layer. AFW provider image math has separate
// pinned-provider GPU coverage. Never opens or edits the owner's active profile.
int afw_only_session_test(std::string_view route,const char* fixture,bool independent=false) {
    xrfg_fixture::ControlProfile profile(independent);
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
    const auto ini=current_layer_directory()/L"ofxr_bridge.ini";
    struct Restore {
        std::filesystem::path path;bool existed;std::string bytes;
        explicit Restore(std::filesystem::path p):path(std::move(p)),existed(std::filesystem::exists(path)) {
            if(existed){std::ifstream file(path,std::ios::binary);bytes.assign(std::istreambuf_iterator<char>(file),{});}
        }
        ~Restore(){if(existed){std::ofstream file(path,std::ios::binary);file<<bytes;}else{std::error_code e;std::filesystem::remove(path,e);}WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());}
    } restore(ini);
    const bool shared_session=route=="luke-shared-session";
    const bool nonluke=route=="afw-only-nonluke";
    const bool vdxr=route=="afw-only-vdxr";
    HMODULE marker=nonluke?nullptr:LoadLibraryA(fixture);
    check(nonluke || marker,"load actual Luke detection marker");
    check(WritePrivateProfileStringW(L"ofxr",L"afw_only",shared_session?L"0":L"1",ini.c_str())!=0,"isolated startup flag");
    auto desired=xrfg::autonomous::snapshot().desired;
    desired.independent_pacing=independent;desired.afw_only=!shared_session;
    desired.enabled=true;desired.frame_generation=2;desired.backend=0;desired.afw_mode=3;desired.nr_enabled=true;
    desired.pacing_mode=2;desired.triple_frame_gen=true;desired.luke_shared_swapchains=shared_session;
    check(xrfg::autonomous::request(desired),"preserve requested OFXR/DLSS-G and DLSS5");xrfg::autonomous::apply_runtime_settings();
    Fixture f(true);f.transport->stop();f.transport.reset();f.throttled=false;f.unblock();f.expected_views=2;
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);
    dispatch->steamvr_runtime=!vdxr;dispatch->virtual_desktop_runtime=vdxr;
    dispatch->get_instance_proc_addr=+[](XrInstance,const char*,PFN_xrVoidFunction* out)->XrResult{*out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;};
    dispatch->create_session=+[](XrInstance,const XrSessionCreateInfo* info,XrSession* out)->XrResult {
        auto* binding=static_cast<const XrGraphicsBindingD3D12KHR*>(info->next);
        current->delivery_queue=binding->queue;*out=reinterpret_cast<XrSession>(2);return XR_SUCCESS;
    };
    dispatch->destroy_session=+[](XrSession)->XrResult{return XR_SUCCESS;};
    dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    dispatch->create_swapchain=Fixture::create;dispatch->destroy_swapchain=Fixture::destroy;dispatch->enumerate_swapchain_images=Fixture::enumerate;
    dispatch->acquire_swapchain_image=Fixture::acquire;dispatch->wait_swapchain_image=Fixture::wait_image;dispatch->release_swapchain_image=Fixture::release;
    dispatch->convert_time_to_qpc=Fixture::to_qpc;
    {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;}
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};binding.device=f.device12.Get();binding.queue=f.queue.Get();
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=&binding;info.systemId=1;XrSession handle{};
    xr_ok(layer_create_session_impl(dispatch->instance,&info,&handle),"actual session startup");auto state=find_session(handle);
    if(shared_session) {
        check(state && !state->afw_only && state->private_runtime && state->luke_shared_swapchains,"startup choice latched on actual Luke session");
        check(xrfg::autonomous::snapshot().runtime.luke_shared_swapchains,"runtime reports active choice");
        auto changed=xrfg::autonomous::snapshot().desired;changed.luke_shared_swapchains=false;
        check(xrfg::autonomous::request(changed),"save next startup choice");xrfg::autonomous::apply_runtime_settings();
        const auto transport=state->private_runtime;
        ok(apply_embedded_control(state),"apply other live controls");
        check(state->luke_shared_swapchains && state->private_runtime==transport,"live settings do not relatch or rebuild output ownership");
        f.transport=transport;xr_ok(layer_destroy_session_impl(handle),"shared Luke session cleanup");
        {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
        FreeLibrary(marker);std::puts("Luke startup-only shared publication, live choice remains pending PASS");return 0;
    }
    if(independent && !nonluke) {
        check(state && state->afw_only && state->independent_pacing && state->private_runtime && !state->menu_enabled,
            "reconstruction-only retains Independent native endpoint without frame generation");
        f.transport=state->private_runtime;f.expected_views=1;
        xr_ok(f.transport->start(),"reconstruction-only endpoint starts");
        const auto chain=f.make_chain();f.write(chain,88);f.begin_virtual();xr_ok(f.submit(chain,17),"reconstruction-only content admission");
        f.wait_count(1);xr_ok(f.transport->drain(),"reconstruction-only native copy completes");
        check(f.pixels==std::vector<unsigned>({88}) && f.positions==std::vector<float>({17}),"Independent reconstruction-only exact native pixels/pose");
        xr_ok(f.transport->destroy_swapchain(chain),"reconstruction-only cleanup");xr_ok(layer_destroy_session_impl(handle),"reconstruction-only session teardown");
        {std::scoped_lock lock(g_state_mutex);g_instances.clear();}FreeLibrary(marker);
        std::puts("INDEPENDENT_RECONSTRUCTION_ONLY native endpoint retained,FG paused,exact pixels/pose PASS");return 0;
    }
    if(nonluke) {
        check(state && !state->afw_only && state->private_runtime,"non-Luke keeps normal OFXR transport");
        f.transport=state->private_runtime;
    } else {
        check(state && state->afw_only && !state->private_runtime && state->dispatch==dispatch,"AFW only keeps real dispatch without private endpoint");
        const auto published=xrfg::autonomous::snapshot();
        check(published.runtime.pacing_mode==xrfg::autonomous::kAfwOnlyPacingMode &&
            published.runtime.generation_state==xrfg::autonomous::GenerationState::paused,
            "F10 reports effective AFW-only rather than the saved OFXR engine");
        check(!state->menu_enabled && !state->private_delivery_queue && !state->binding_queue &&
            f.delivery_queue==f.queue && !state->presenter_active,"no OFXR queue, worker or binding substitution");
        check(state->frame_generation_mode==xrfg::D3D12FrameGenerationMode::ofxr && !state->djules_route &&
            !state->nvidia_new_route && !state->hybrid_requested && presenter_forbidden(*state),"no hidden DLSS-G/FSR/hybrid/presenter path");
        for(int backend:{0,1,2,3})for(int pacing:{0,1,2}) {
            auto changed=xrfg::autonomous::snapshot().desired;
            changed.backend=backend;changed.pacing_mode=pacing;changed.enabled=true;
            check(xrfg::autonomous::request(changed),"attempt live OFXR enable");xrfg::autonomous::apply_runtime_settings();
            ok(apply_embedded_control(state),"apply AFW-only session override");
            check(!state->menu_enabled && !state->private_runtime && !continuous_presenter_active(state),"OFXR stays disconnected until restart");
            check(xrfg::autonomous::snapshot().desired.nr_enabled && xrfg::neural_rendering::status().enabled,"DLSS5 remains enabled independently");
        }
        // Both native eyes get the exact terminal GPU pixels. No director FIFO,
        // second XR cycle, or private output copy exists at this boundary.
        std::array<XrSwapchain,2> eyes{};
        for(unsigned eye=0;eye<2;++eye) {
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.format=DXGI_FORMAT_R8G8B8A8_UNORM;
            ci.width=ci.height=ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;
            xr_ok(Fixture::create(handle,&ci,&eyes[eye]),"native AFW terminal output");
            unsigned index{};XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=XR_INFINITE_DURATION;
            xr_ok(Fixture::acquire(eyes[eye],&ai,&index),"native acquire");xr_ok(Fixture::wait_image(eyes[eye],&wi),"native image wait");
            ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
            ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"terminal RTV");
            f.device12->CreateRenderTargetView(f.chains[eyes[eye]].d12[index].Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
            const float color[]{(eye?160.f:80.f)/255.f,0,0,1};
            f.commands([&](ID3D12GraphicsCommandList* list){list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),color,0,nullptr);});
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr_ok(Fixture::release(eyes[eye],&ri),"native release");
        }
        for(bool fresh_filter:{false,true}) {
            state->luke_fresh_only=fresh_filter;
            if(fresh_filter)state->last_inline_frame_state.predictedDisplayTime+=period*10;
            // Next-session flag changes cannot alter ownership mid-frame.
            auto next=xrfg::autonomous::snapshot().desired;next.afw_only=false;check(xrfg::autonomous::request(next),"save next-session OFF in game profile");
            check(state->afw_only && !state->private_runtime,"transport selection is immutable until restart");
            xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,true);
            const auto started=xrfg::interface_clock_ns();
            auto cycle=std::make_shared<xrfg::InterfaceCycle>(started);
            XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};XrFrameState fs{XR_TYPE_FRAME_STATE};
            xr_ok(layer_wait_frame_impl(handle,&wi,&fs),"app direct Wait");
            XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};xr_ok(layer_begin_frame_impl(handle,&bi),"app direct Begin");
            std::array<XrCompositionLayerProjectionView,2> views{};
            for(unsigned eye=0;eye<2;++eye) {auto& v=views[eye];v.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;v.pose.orientation.w=1;v.subImage.swapchain=eyes[eye];v.subImage.imageRect.extent={1,1};}
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=2;projection.views=views.data();
            const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=fs.predictedDisplayTime;end.layerCount=1;end.layers=&header;
            xrfg::InterfaceFrameStamp stamp{};stamp.serial=fresh_filter?2:1;stamp.entered_ns=started;stamp.cycle=cycle;
            xr_ok(state->fps_overlay->end_frame(&end,false,true,{},0,stamp),"shipping AFW terminal directly to native OpenXR");
            const auto returned=xrfg::interface_clock_ns();cycle->producer_returned(returned);
            float estimate{};check(cycle->read_minimum_cycle(returned,estimate) && std::isfinite(estimate) && estimate>0,"direct delivery retains native cycle estimate");
            bool native_required{};xrfg::InterfaceNativeTiming native;
            check(cycle->native_timing(native_required,native) && native_required,"direct AFW retains actual native End completion receipt");
            check(native.returned_ns<=returned,"native completion precedes outer wrapper bookkeeping");
            const float expected=float(native.returned_ns-started)*1e-6F;
            check(std::abs(estimate-expected)<0.01f,"AFW-only cycle uses observed native return, not a display target or added refresh");
            check(f.pixels.back()==80 && f.right_pixels.back()==160 && f.delivery_thread==GetCurrentThreadId(),"both AFW eye pixels directly delivered on app thread");
            check(f.successful_waits==f.real_begins && f.real_begins==f.real_ends && !f.empty_ends && !f.cycle_begun,"balanced native Wait/Begin/End, one output per AFW pair");
        }
        for(auto eye:eyes)xr_ok(Fixture::destroy(eye),"terminal cleanup");
    }
    xr_ok(layer_destroy_session_impl(handle),"AFW-only session cleanup");
    {std::scoped_lock lock(g_state_mutex);g_instances.clear();}
    if(marker)FreeLibrary(marker);
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::desktop,false);
    std::puts("AFW-only session: real binding, no private workers/FIFO, forced OFXR OFF, DLSS5 retained, both GPU eyes/native latency, non-Luke scope PASS");
    return 0;
}
