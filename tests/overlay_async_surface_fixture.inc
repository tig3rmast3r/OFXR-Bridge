static void async_surface_fixture(const std::filesystem::path& ini,const XrFrameEndInfo& frame) {
    for(unsigned scenario=0;scenario<4;++scenario) {
        const bool menu=scenario==1 || scenario==2,cancel=scenario==2,meter=scenario==3;
        input={};input.focused=true;input.open=menu;menu_seen=fps_seen=0;
        position(ini,menu?"off":meter?"elliott_bottom":"upper_right");
        creation_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        creation_release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        creation_finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        const auto entered_event=creation_entered;
        const auto before=made;
        bool independent{};
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
                device.Get(),queue.Get(),nullptr,ini,true,poll);
            overlay.set_async_surface_creation(std::getenv("XRFG_TEST_SYNC_UI_CREATE")==nullptr);
            auto update=std::async(std::launch::async,[&]{overlay.application_frame(&frame);});
            const bool entered=WaitForSingleObject(creation_entered,3000)==WAIT_OBJECT_0;
            const bool source_free=update.wait_for(std::chrono::milliseconds(80))==std::future_status::ready;
            auto delivery=std::async(std::launch::async,[&]{return overlay.end_frame(&frame,false);});
            const bool delivery_free=delivery.wait_for(std::chrono::milliseconds(80))==std::future_status::ready;
            independent=source_free&&delivery_free;
            if(source_free && cancel) {
                input.open=false;overlay.application_frame(&frame);
            }
            SetEvent(creation_release);
            const bool completed=WaitForSingleObject(creation_finished,3000)==WAIT_OBJECT_0;
            update.get();const auto result=delivery.get();
            std::cerr<<"ASYNC_GATE scenario="<<scenario<<" source_free="<<source_free<<" delivery_free="<<delivery_free<<'\n';
            // Always open the native create gate before judging the original
            // synchronous path; negative controls must not hang destruction.
            check(entered&&completed,"native optional surface construction gate balanced");
            check(result==XR_SUCCESS,"scene continues while optional surface is unpublished");
            creation_entered=nullptr;
            for(unsigned n=0;n<200;++n) {
                overlay.application_frame(&frame);overlay.end_frame(&frame,false);
                if(cancel?chains.empty():(menu?menu_seen:fps_seen)>0)break;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if(cancel)check(chains.empty()&&menu_seen==0,"closed pending menu retires without being published");
            else check((menu?menu_seen:fps_seen)>0,"finished optional owner becomes visible");
            check(made==before+1,"background construction allocated extra native surfaces");
            input.open=false;position(ini,"off");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            for(unsigned n=0;n<100 && !chains.empty();++n) {
                overlay.application_frame(&frame);overlay.end_frame(&frame,false);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        check(independent,"UI construction blocked application End or composed delivery");
        check(chains.empty()&&made==destroyed,"background optional surface lifetime balanced");
        // creation_entered was cleared once the worker's callback returned.
        CloseHandle(entered_event);CloseHandle(creation_release);CloseHandle(creation_finished);
        creation_release=creation_finished=nullptr;
        std::cout<<"ASYNC_SURFACE "<<(menu?"menu":meter?"VR-entry meter":"VR-entry FPS")<<(cancel?" cancelled":" published")<<" source/native progress PASS\n";
    }
    input={};input.focused=input.open=true;position(ini,"off");
    creation_entered=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    creation_release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    creation_finished=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    auto overlay=std::make_unique<xrfg::OpenXrFpsOverlay>(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
        device.Get(),queue.Get(),nullptr,ini,true,poll);
    overlay->set_async_surface_creation(true);overlay->application_frame(&frame);
    const bool entered=WaitForSingleObject(creation_entered,3000)==WAIT_OBJECT_0;
    auto teardown=std::async(std::launch::async,[owner=std::move(overlay)]() mutable {owner.reset();});
    const bool retained=teardown.wait_for(std::chrono::milliseconds(40))==std::future_status::timeout;
    SetEvent(creation_release);teardown.get();
    check(entered&&retained&&chains.empty()&&made==destroyed,"teardown retains the pending builder and reclaims its exact owner");
    CloseHandle(creation_entered);CloseHandle(creation_release);CloseHandle(creation_finished);
    creation_entered=creation_release=creation_finished=nullptr;
    std::cout<<"ASYNC_SURFACE pending teardown retains owner and releases exact resources PASS\n";
}
