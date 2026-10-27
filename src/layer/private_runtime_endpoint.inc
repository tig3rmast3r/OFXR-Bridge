// Included after find_session/find_dispatch. These are the layer's downstream
// transport functions, not a second public API layer. Per-session Dispatch is
// cloned: instance discovery and other runtimes retain their original table.
std::shared_ptr<SessionState> private_chain_owner(XrSwapchain handle) {
    std::scoped_lock lock(g_state_mutex);
    for(auto& [key,state]:g_sessions)
        if(state->private_runtime && state->private_runtime->owns(handle))return state;
    return {};
}
XrResult XRAPI_CALL private_runtime_wait(XrSession session,const XrFrameWaitInfo* info,XrFrameState* frame) {
    const auto state=find_session(session);if(!state)return XR_ERROR_HANDLE_INVALID;
    return state->private_runtime?state->private_runtime->wait_frame(info,frame):state->dispatch->wait_frame(session,info,frame);
}
XrResult XRAPI_CALL private_runtime_begin(XrSession session,const XrFrameBeginInfo* info) {
    const auto state=find_session(session);if(!state)return XR_ERROR_HANDLE_INVALID;
    return state->private_runtime?state->private_runtime->begin_frame(info):state->dispatch->begin_frame(session,info);
}
XrResult XRAPI_CALL private_runtime_end(XrSession session,const XrFrameEndInfo* info) {
    const auto state=find_session(session);if(!state)return XR_ERROR_HANDLE_INVALID;
    return state->private_runtime?state->private_runtime->end_frame(info):state->dispatch->end_frame(session,info);
}
XrResult XRAPI_CALL private_runtime_create(XrSession session,const XrSwapchainCreateInfo* info,XrSwapchain* handle) {
    const auto state=find_session(session);if(!state)return XR_ERROR_HANDLE_INVALID;
    return state->private_runtime?state->private_runtime->create_swapchain(info,handle):state->dispatch->create_swapchain(session,info,handle);
}
XrResult XRAPI_CALL private_runtime_destroy(XrSwapchain handle) {
    const auto state=private_chain_owner(handle);return state?state->private_runtime->destroy_swapchain(handle):XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL private_runtime_enumerate(XrSwapchain handle,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* images) {
    const auto state=private_chain_owner(handle);return state?state->private_runtime->enumerate_images(handle,capacity,count,images):XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL private_runtime_acquire(XrSwapchain handle,const XrSwapchainImageAcquireInfo* info,std::uint32_t* index) {
    const auto state=private_chain_owner(handle);return state?state->private_runtime->acquire_image(handle,info,index):XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL private_runtime_wait_image(XrSwapchain handle,const XrSwapchainImageWaitInfo* info) {
    const auto state=private_chain_owner(handle);return state?state->private_runtime->wait_image(handle,info):XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL private_runtime_release(XrSwapchain handle,const XrSwapchainImageReleaseInfo* info) {
    const auto state=private_chain_owner(handle);return state?state->private_runtime->release_image(handle,info):XR_ERROR_HANDLE_INVALID;
}
XrResult XRAPI_CALL private_runtime_get_proc(XrInstance instance,const char* name,PFN_xrVoidFunction* function) {
    const auto dispatch=find_dispatch(instance);
    if(!dispatch || !name || !function)return XR_ERROR_HANDLE_INVALID;
#define PRIVATE_PROC(n,f) if(std::strcmp(name,n)==0){*function=reinterpret_cast<PFN_xrVoidFunction>(f);return XR_SUCCESS;}
    PRIVATE_PROC("xrWaitFrame",private_runtime_wait)
    PRIVATE_PROC("xrBeginFrame",private_runtime_begin)
    PRIVATE_PROC("xrEndFrame",private_runtime_end)
    PRIVATE_PROC("xrCreateSwapchain",private_runtime_create)
    PRIVATE_PROC("xrDestroySwapchain",private_runtime_destroy)
    PRIVATE_PROC("xrEnumerateSwapchainImages",private_runtime_enumerate)
    PRIVATE_PROC("xrAcquireSwapchainImage",private_runtime_acquire)
    PRIVATE_PROC("xrWaitSwapchainImage",private_runtime_wait_image)
    PRIVATE_PROC("xrReleaseSwapchainImage",private_runtime_release)
#undef PRIVATE_PROC
    return dispatch->get_instance_proc_addr(instance,name,function);
}

HRESULT install_private_runtime(const std::shared_ptr<SessionState>& state) {
    if(state->afw_only || !comparison_delivery_runtime(*state))return S_FALSE;
    // The endpoint owns D3D images. A session still on a non-D3D binding (a
    // Vulkan session the bridge declined, OpenGL) has no device for it: pass
    // the valid runtime session through instead of failing xrCreateSession.
    if(state->graphics_binding!=SessionGraphicsBinding::d3d11 &&
        state->graphics_binding!=SessionGraphicsBinding::d3d12)return S_FALSE;
    state->uevr_application=GetModuleHandleW(L"UEVRBackend.dll")!=nullptr;
    auto original=state->dispatch;
    xrfg::PrivateOpenXrRuntime::Options options;
    options.director_enabled=state->menu_enabled && xrfg::embedded::snapshot().desired.pacing_mode!=0;
    options.capacity=xrfg::embedded::snapshot().desired.increase_buffer?3:2;
    options.real={original->instance,state->handle,original->wait_frame,original->begin_frame,original->end_frame,
        original->create_swapchain,original->destroy_swapchain,original->enumerate_swapchain_images,
        original->acquire_swapchain_image,original->wait_swapchain_image,original->release_swapchain_image,
        original->convert_time_to_qpc};
    if(state->graphics_binding==SessionGraphicsBinding::d3d12 || state->d3d11_bridge || state->vulkan_bridge) {
        options.device12=state->d3d12_device.Get();
        options.producer_queue12=runtime_queue(*state);
        options.queue12=state->private_delivery_queue?state->private_delivery_queue.Get():options.producer_queue12;
    } else {options.device11=state->d3d11_device.Get();options.context11=state->d3d11_context.Get();}
    options.d11_release_readiness=options.device11 && state->luke_afw_enabled && original->steamvr_runtime;
    // Common private admission, independent of pacing. Image storage is
    // unchanged; Legacy/AFW-only bypass installation of this endpoint.
    const std::weak_ptr<SessionState> weak=state;
    options.physical_period=[weak]() -> std::int64_t {
        if(auto s=weak.lock())if(s->steamvr_delivery)if(auto p=s->steamvr_delivery->cached_display_period())return p->count();
        return 0;
    };
    options.physical_clock=[weak]() -> std::optional<xrfg::PrivatePhysicalClock> {
        if(auto s=weak.lock())if(s->steamvr_delivery) {
            const auto anchor=s->steamvr_delivery->cached_vsync_anchor();
            const auto period=s->steamvr_delivery->cached_display_period();
            if(anchor && period)return xrfg::PrivatePhysicalClock{
                std::chrono::duration_cast<std::chrono::nanoseconds>(anchor->at.time_since_epoch()).count(),
                period->count(),anchor->frame_counter};
        }
        return {};
    };
    options.trace=[](int stage,std::uint64_t sequence,std::int64_t at,std::int64_t hold) {
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            stage<0?5719:5710+stage,sequence,at,hold);
    };
    if(original->virtual_desktop_runtime || original->runtime_name.find("Pimax OpenXR")!=std::string::npos) {
        state->runtime_acceptance=std::make_shared<xrfg::RuntimeAcceptanceRate>();
        const auto rate=state->runtime_acceptance;
        options.accepted_output=[rate](auto at){rate->accepted(at);};
    }
    options.trace_enabled=[]{return xrfg::bridge_flight_logger().enabled();};
    auto transport=std::make_shared<xrfg::PrivateOpenXrRuntime>(std::move(options));
    auto result=transport->initialize();if(FAILED(result))return result;
    auto dispatch=std::make_shared<Dispatch>(*original);
    dispatch->inline_unless_pipelined=false; // common endpoint owns the runtime frame loop
    dispatch->wait_frame=private_runtime_wait;dispatch->begin_frame=private_runtime_begin;dispatch->end_frame=private_runtime_end;
    dispatch->create_swapchain=private_runtime_create;dispatch->destroy_swapchain=private_runtime_destroy;
    dispatch->enumerate_swapchain_images=private_runtime_enumerate;
    dispatch->acquire_swapchain_image=private_runtime_acquire;dispatch->wait_swapchain_image=private_runtime_wait_image;
    dispatch->release_swapchain_image=private_runtime_release;dispatch->get_instance_proc_addr=private_runtime_get_proc;
    state->private_runtime=std::move(transport);state->dispatch=std::move(dispatch);return S_OK;
}
