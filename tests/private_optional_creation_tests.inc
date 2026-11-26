// Gate an actual native create before it mutates the fixture's image map.
// Empty native cycles can then progress without racing that test-only map.
static XrSwapchainCreateInfo optional_surface_info() {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    info.width=info.height=info.arraySize=info.faceCount=info.mipCount=info.sampleCount=1;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    return info;
}
static void optional_creation_progress(bool d12,unsigned transition) {
    independent_fixture_mode=true;
    {
        Fixture f(d12);f.throttled=false;
        XrFrameState seed{XR_TYPE_FRAME_STATE};xr_ok(f.transport->predict_frame(&seed),"seed native UI progress test");
        std::promise<void> entered,release;auto entered_future=entered.get_future();
        auto gate=release.get_future().share();
        f.create_callback=[&]{entered.set_value();gate.wait();};
        const auto info=optional_surface_info();XrSwapchain handle{};
        // Explicit negative control: the original application create still
        // owns its old critical section. This must fail the progress check.
        const bool original=std::getenv("XRFG_TEST_OLD_UI_CREATE")!=nullptr;
        auto creation=std::async(std::launch::async,[&]{return original?
            f.transport->create_swapchain(&info,&handle):f.transport->create_optional_swapchain(&info,&handle);});
        check(entered_future.wait_for(3s)==std::future_status::ready,"optional native create gate entered");
        const auto before=f.real_begins.load();
        auto access=std::async(std::launch::async,[&]{
            if(transition){f.transport->stop();return transition==2?f.transport->start():XR_SUCCESS;}
            XrFrameState predicted{XR_TYPE_FRAME_STATE};return f.transport->predict_frame(&predicted);
        });
        const bool independent=access.wait_for(100ms)==std::future_status::ready;
        if(!transition && independent)
            for(unsigned n=0;n<100 && f.real_begins.load()==before;++n)std::this_thread::sleep_for(1ms);
        const bool progressed=f.real_begins.load()>before;
        release.set_value();
        check(creation.wait_for(3s)==std::future_status::ready,"optional creation cleanup completes");
        const auto created=creation.get();
        check(access.wait_for(3s)==std::future_status::ready,"optional native access cleanup completes");
        xr_ok(access.get(),"native access during optional creation");
        f.create_callback={};
        if(transition) {
            check(independent,"stop waited on unpublished UI creation");
            check(created==XR_ERROR_SESSION_NOT_RUNNING && !handle && f.chains.empty(),
                "stopped optional creation must destroy its unpublished native chain");
        } else {
            check(independent&&progressed,"optional UI creation blocked native prediction/delivery progress");
            xr_ok(created,"completed optional source publishes atomically");
            unsigned count{};xr_ok(f.transport->enumerate_images(handle,0,&count,nullptr),"optional private images available");
            check(count==3 && f.chains.size()==1,"optional construction changed existing native/private image counts");
            XrResult retired=XR_ERROR_CALL_ORDER_INVALID;
            for(unsigned n=0;n<100 && retired==XR_ERROR_CALL_ORDER_INVALID;++n) {
                retired=f.transport->try_destroy_swapchain(handle);
                if(retired==XR_ERROR_CALL_ORDER_INVALID)std::this_thread::sleep_for(1ms);
            }
            xr_ok(retired,"unreferenced optional image cleanup");
        }
    }
    independent_fixture_mode=false;
    std::printf("OPTIONAL_CREATE %s %s PASS\n",d12?"D12":"D11",
        transition==2?"stop/start cancels old unpublished owner":transition?"stop cancels unpublished owner":"native progress and original image counts");
}
static void optional_creation_learning(bool d12) {
    const auto saved=period;period=25'000'000;
    independent_fixture_mode=true;independent_fixture_auto=true;
    {
        Fixture f(d12);f.throttled=false;f.block_end=false;
        const auto chain=f.make_chain();f.write(chain,80);
        const auto old_due=ns();
        // Cover the known synchronous UI work without inventing a provider
        // or GPU pause. The original source deadline predates this create.
        f.create_callback=[]{std::this_thread::sleep_for(35ms);};
        const auto info=optional_surface_info();XrSwapchain ui{};
        const bool original=std::getenv("XRFG_TEST_OLD_UI_CREATE")!=nullptr;
        xr_ok(original?f.transport->create_swapchain(&info,&ui):f.transport->create_optional_swapchain(&info,&ui),
            "synchronous optional menu creation");
        f.create_callback={};
        const auto affected_first=ns()-period/4;
        const auto send=[&](std::int64_t due,float identity,std::int64_t offset=0) {
            f.begin_virtual();f.transport->set_next_independent_deadline(due,false,{offset,true,true});
            auto receipt=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
            xr_ok(f.submit(chain,identity,XR_NULL_HANDLE,receipt),"optional learning packet admission");
            xr_ok(f.transport->drain(),"optional learning packet retirement");return receipt;
        };
        for(unsigned n=0;n<3;++n)
            check(send(old_due,n+1)->outcome==-2,"menu pause must not rescue stale pixels/poses");
        for(unsigned n=0;n<200 && f.ignored_optional_lateness<2;++n)std::this_thread::sleep_for(1ms);
        std::printf("UI_PAUSE %s ignored=%u reserve=%.3fms\n",d12?"D12":"D11",
            f.ignored_optional_lateness.load(),f.transport->independent_load().reserve/1e6);
        check(f.ignored_optional_lateness>=2 && !f.transport->independent_load().reserve,
            "menu setup lateness inflated future expiry allowance");
        // R12 excluded the head but let later slots of that same source train
        // reserve merely because their deadlines fell after UI construction.
        std::this_thread::sleep_for(std::chrono::nanoseconds(4*period));
        for(unsigned slot=1;slot<=3;++slot)
            check(send(affected_first+slot*period,10+slot,slot*period)->outcome==-2,
                "UI-affected 2X/3X/4X tail must expire without purchasing allowance");
        for(unsigned n=0;n<200 && f.ignored_optional_lateness<5;++n)std::this_thread::sleep_for(1ms);
        check(f.ignored_optional_lateness>=5 && !f.transport->independent_load().reserve,
            "UI-affected source tail escaped the batch exclusion");
        const auto fresh=send(ns()+period*2,4);
        check(fresh->outcome==1,"fresh output must immediately recover after optional work");
        // A failed UI create must also close the exclusion; later genuine
        // lateness still earns the existing bounded reserve.
        f.native_chain_limit=f.chains.size();XrSwapchain refused=reinterpret_cast<XrSwapchain>(1);
        check(f.transport->create_optional_swapchain(&info,&refused)==XR_ERROR_LIMIT_REACHED && !refused,
            "refused optional allocation must publish no handle");
        std::this_thread::sleep_for(std::chrono::nanoseconds(2*period));
        for(unsigned n=0;n<4 && !f.transport->independent_load().reserve;++n)send(ns()-period,5+n);
        for(unsigned n=0;n<200 && !f.transport->independent_load().reserve;++n)std::this_thread::sleep_for(1ms);
        check(f.transport->independent_load().reserve>0,"optional work disabled genuine future lateness learning");
        f.transport->stop();
        xr_ok(f.transport->destroy_swapchain(ui),"optional learning UI cleanup");
        xr_ok(f.transport->destroy_swapchain(chain),"optional learning source cleanup");
    }
    independent_fixture_auto=false;independent_fixture_mode=false;period=saved;
    std::printf("OPTIONAL_LEARNING %s stale outputs expire without reserve, fresh output recovers, later genuine losses learn PASS\n",d12?"D12":"D11");
}

// Hold only the next UI release marker, after its pixels have been drawn.
// Eye images and the native delivery queue remain ready throughout the test.
static void optional_ui_ready_image(bool previous,bool optional) {
    independent_fixture_mode=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        const auto scene=f.make_chain();f.write(scene,80);
        const auto info=optional_surface_info();XrSwapchain ui{};
        xr_ok(optional?f.transport->create_optional_swapchain(&info,&ui):
            f.transport->create_swapchain(&info,&ui),"create readiness UI source");
        if(previous) {
            f.write(ui,40);f.gpu_wait();
            f.begin_virtual();xr_ok(f.submit(scene,1,ui),"seed completed UI");
            xr_ok(f.transport->drain(),"completed UI seed retires");
        }
        f.release_callback=[&](XrSwapchain h,unsigned) {
            ok(f.queue->Wait(f.gate.Get(),1),"gate newer UI release marker");
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            return f.transport->release_image(h,&release);
        };
        f.write(ui,120);f.release_callback={};
        // Acquisition must preserve the old completed image as well as the
        // latest pending release, using the same existing three UI images.
        unsigned spare{};XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
        xr_ok(f.transport->try_acquire_image(ui,&acquire,&spare),"third existing UI image remains writable");
        xr_ok(f.transport->wait_image(ui,&wait),"wait spare UI image");
        const auto before=f.real_ends.load();
        f.begin_virtual();xr_ok(f.submit(scene,2,ui),"fresh scene with pending menu");
        auto drained=std::async(std::launch::async,[&]{return f.transport->drain();});
        const bool progressed=drained.wait_for(150ms)==std::future_status::ready;
        ok(f.gate->Signal(1),"release newer UI gate before checking results");
        xr_ok(drained.get(),"UI gate cleanup");
        check(progressed==optional,"optional menu readiness blocked fresh scene, or application quad lost its readiness contract");
        check(f.real_ends==before+1 && f.positions.back()==2 && f.pixels.back()==80,
            "UI readiness changed fresh scene pixels or pose");
        if(optional)check(previous?f.overlay_pixels==std::vector<unsigned>({40,40}):f.overlay_pixels.empty(),
            "pending UI must retain the exact completed image or omit its unfinished first image");
        else check(f.overlay_pixels.back()==120,"ordinary application quad must wait for its current release");
        // Finish the acquired spare without rewriting its pixels. It remains
        // acquired until after promotion, so it cannot supersede the new draw.
        f.gpu_wait();
        f.begin_virtual();xr_ok(f.submit(scene,3,ui),"promote completed newer menu");
        xr_ok(f.transport->drain(),"new UI image delivered");
        check(f.overlay_pixels.back()==120 && f.positions.back()==3,"completed menu was not promoted on a fresh scene");
        unsigned count{};xr_ok(f.transport->enumerate_images(ui,0,&count,nullptr),"existing UI count");
        check(count==3 && f.chains.size()==2,"readiness recovery allocated additional images/chains");
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr_ok(f.transport->release_image(ui,&release),"retire spare UI acquisition");
        f.transport->stop();
        xr_ok(f.transport->destroy_swapchain(ui),"UI readiness cleanup");
        xr_ok(f.transport->destroy_swapchain(scene),"scene readiness cleanup");
    }
    independent_fixture_mode=false;
    std::printf("UI_READY previous=%d optional=%d fresh pixels/poses, completed UI, fixed image counts PASS\n",previous,optional);
}

static void independent_copy_queue_learning() {
    const auto saved=period;period=50'000'000;
    independent_fixture_mode=true;independent_fixture_auto=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        const std::array<XrSwapchain,2> scenes{f.make_chain(),f.make_chain()};
        for(auto h:scenes)f.write(h,80);
        f.gpu_wait();
        ok(f.delivery_queue->Wait(f.gate.Get(),1),"gate final delivery copy queue only");
        std::array<std::shared_ptr<xrfg::InterfaceDeliveryReceipt>,2> receipts;
        for(unsigned n=0;n<2;++n) {
            f.begin_virtual();f.transport->set_next_independent_deadline(ns()+period);
            receipts[n]=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
            xr_ok(f.submit(scenes[n],float(n+1),XR_NULL_HANDLE,receipts[n]),"ready source before copy queue stall");
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(3*period));
        ok(f.gate->Signal(1),"release copy queue stall");
        xr_ok(f.transport->drain(),"expired delivery copies retire");
        std::this_thread::sleep_for(std::chrono::nanoseconds(2*period));
        check(!f.transport->independent_load().reserve,"delivery-copy queue delay trained persistent source lateness reserve");
        check(receipts[0]->outcome==-2 && receipts[1]->outcome==-2,"copy stall rescued stale scene pixels/poses");
        f.begin_virtual();f.transport->set_next_independent_deadline(ns()+period);
        auto fresh=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
        xr_ok(f.submit(scenes[0],3,XR_NULL_HANDLE,fresh),"fresh scene after startup copy backlog");
        xr_ok(f.transport->drain(),"fresh scene recovers");
        check(fresh->outcome==1 && !f.transport->independent_load().reserve,"copy backlog left a recovery tail");
        f.transport->stop();
        for(auto h:scenes)xr_ok(f.transport->destroy_swapchain(h),"copy queue source cleanup");
    }
    independent_fixture_mode=false;independent_fixture_auto=false;period=saved;
    std::puts("COPY_QUEUE no self-trained reserve, stale expiry, immediate fresh recovery PASS");
}

static void independent_initialization_learning() {
    const auto saved=period;period=25'000'000;
    independent_fixture_mode=true;independent_fixture_auto=true;
    {
        Fixture f(true);f.throttled=false;f.block_end=false;
        const auto scene=f.make_chain();f.write(scene,80);f.gpu_wait();
        const auto first_due=ns();
        // Real one-off generator initialization crosses this source deadline.
        std::this_thread::sleep_for(40ms);
        f.transport->finish_generation_initialization();
        std::this_thread::sleep_for(std::chrono::nanoseconds(4*period));
        const auto send=[&](std::int64_t due,float identity,std::int64_t offset=0) {
            f.begin_virtual();f.transport->set_next_independent_deadline(due,false,{offset,true,true});
            auto receipt=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
            xr_ok(f.submit(scene,identity,XR_NULL_HANDLE,receipt),"initialization learning packet");
            xr_ok(f.transport->drain(),"initialization learning retirement");return receipt;
        };
        for(unsigned slot=0;slot<4;++slot)
            check(send(first_due+slot*period,1+slot,slot*period)->outcome==-2,"initialization affected batch must expire");
        std::this_thread::sleep_for(std::chrono::nanoseconds(period));
        check(!f.transport->independent_load().reserve && f.ignored_optional_lateness>=4,
            "one-off initialization delay trained future 2X/3X/4X tolerance");
        check(send(ns()+2*period,5)->outcome==1,"fresh startup scene did not resume");
        for(unsigned n=0;n<4 && !f.transport->independent_load().reserve;++n)send(ns()-period,6+n);
        for(unsigned n=0;n<200 && !f.transport->independent_load().reserve;++n)std::this_thread::sleep_for(1ms);
        check(f.transport->independent_load().reserve>0,"initialization exclusion disabled later genuine source lateness learning");
        f.transport->stop();xr_ok(f.transport->destroy_swapchain(scene),"initialization source cleanup");
    }
    independent_fixture_mode=false;independent_fixture_auto=false;period=saved;
    std::puts("INITIALIZATION no stale batch tolerance, fresh recovery, later genuine learning PASS");
}
