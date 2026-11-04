// Ownership regression: exactly the private acquire path measured in MSFS.
static int rife_output_banks_test(bool policy) {
    Fixture f(true);f.unblock();f.throttled=false;
    if(policy){
        auto dispatch=std::make_shared<Dispatch>();dispatch->steamvr_runtime=true;
        auto state=std::make_shared<SessionState>(dispatch);
        state->private_runtime=f.transport;state->graphics_binding=SessionGraphicsBinding::d3d12;
        state->frame_generation_mode=xrfg::D3D12FrameGenerationMode::ofxr;
        state->nvidia_options.rife_model=3;
        for(unsigned factor:{2U,3U,4U})for(bool fast:{false,true}){
            state->nvidia_options.rife_multiplier=factor;state->lower_vram=fast;
            const auto minimal=factor>2?factor-1:(fast?1U:2U);
            check(source_synthetic_rings(*state,false)==(!fast && factor>2?minimal*2:minimal),
                "Normal uses independent complete banks; Fast keeps its exact allocation");
            check(source_synthetic_rings(*state,true)==(factor>2?(factor-1)*2:minimal),"panel allocation preserved");
            dispatch->steamvr_runtime=false;
            check(source_synthetic_rings(*state,false)==minimal,"other runtime allocation must stay unchanged");
            dispatch->steamvr_runtime=true;
        }
        state->nvidia_options.rife_model=-1;state->nvidia_options.rife_multiplier=3;state->lower_vram=0;
        check(source_synthetic_rings(*state,false)==native_synthetic_rings(*state),"non-RIFE allocation retained");
        state->private_runtime.reset();
        std::puts("RIFE Normal/Fast2X/3X/4X allocation, existing panels and non-RIFE/runtime policy PASS");return 0;
    }
    auto a=f.make_chain(false,1),b=f.make_chain(false,1);
    f.write(a,70);f.write(b,140);
    std::promise<void> entered,leave;auto arrived=entered.get_future();auto gate=leave.get_future().share();
    std::atomic_bool once{},released{};
    const auto unblock_copy=[&]{if(!released.exchange(true))leave.set_value();};
    f.image_wait_callback=[&](XrSwapchain,unsigned){if(!once.exchange(true)){entered.set_value();gate.wait();}};
    HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);check(done!=nullptr,"test rescue event");
    std::thread rescue([&]{if(WaitForSingleObject(done,3000)!=WAIT_OBJECT_0)unblock_copy();});
    f.begin_virtual();xr_ok(f.submit(a,1),"first bank packet");
    const bool held=arrived.wait_for(1s)==std::future_status::ready;
    auto reused=std::async(std::launch::async,[&]{unsigned index{};XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        return std::pair{f.transport->acquire_image(a,&info,&index),index};});
    const bool old_blocked=reused.wait_for(30ms)==std::future_status::timeout;
    unsigned index{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    const auto start=std::chrono::steady_clock::now();
    const auto next=f.transport->try_acquire_image(b,&acquire,&index);
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    bool first_unchanged=false;
    if(next==XR_SUCCESS){
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        xr_ok(f.transport->wait_image(b,&wait),"next bank wait");
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr_ok(f.transport->release_image(b,&release),"next bank release");
        first_unchanged=!f.real_ends.load();
        f.begin_virtual();xr_ok(f.submit(b,2),"second bank packet while first copy held");
    }
    SetEvent(done);unblock_copy();rescue.join();CloseHandle(done);
    const auto old=reused.get();xr_ok(old.first,"old bank becomes reusable only after its copy");
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    xr_ok(f.transport->wait_image(a,&wait),"old bank retirement wait");
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};xr_ok(f.transport->release_image(a,&release),"old bank retirement release");
    check(held && old_blocked,"negative control must retain a real source-copy reader");
    check(next==XR_SUCCESS,"independent bank acquire while previous copy still held");
    check(first_unchanged,"next bank repointed the prior packet");
    f.wait_count(2);xr_ok(f.transport->drain(),"both bank packets drain");
    check(f.positions==std::vector<float>{1,2} && f.pixels==std::vector<unsigned>{70,140},"bank reuse crossed frame colour or camera identity");
    f.image_wait_callback={};
    xr_ok(f.transport->destroy_swapchain(a),"old bank cleanup");xr_ok(f.transport->destroy_swapchain(b),"new bank cleanup");
    std::printf("Held native copy: same bank blocks, independent bank acquire_ms=%.3f, exact pixels/poses PASS\n",ms);return 0;
}
