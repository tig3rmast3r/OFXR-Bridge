// Exercise actual UI allocation and migration, not a duplicate policy formula.
void atlas_policy_fixture(const std::filesystem::path& ini,const XrFrameEndInfo& frame) {
    policy_fixture=true;chain_limit=2;
    const auto pump=[&](xrfg::OpenXrFpsOverlay& overlay,const auto& done,const char* message) {
        for(unsigned n=0;n<300;++n) {
            overlay.application_frame(&frame);check(overlay.end_frame(&frame,false)==XR_SUCCESS,"policy never fails the game EndFrame");
            if(done())return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(false,message);
    };
    input={};input.focused=true;position(ini,"upper_right");
    unsigned external=11,reads{};bool available=true;
    {
        xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
            device.Get(),queue.Get(),device11.Get(),ini,true,poll);
        overlay.set_native_swapchain_count_source([&]()->std::optional<unsigned>{
            ++reads;if(!available)return std::nullopt;return external+static_cast<unsigned>(chains.size());
        });
        pump(overlay,[&]{return fps_seen==1&&chains.size()==1;},"single counter must use one compact direct surface");
        check(chains.begin()->second.height<512,"single counter does not reserve a shared atlas");
        input.open=true;
        pump(overlay,[&]{return menu_seen==1&&fps_seen==1;},"projected13 opens both direct panels");
        check(chains.size()==2,"projected13 stays separate");
        const auto low_reads=reads;external=12;
        for(unsigned n=0;n<5;++n){overlay.application_frame(&frame);overlay.end_frame(&frame,false);}
        check(reads==low_reads && chains.size()==2,"separate decision is latched until a panel closes");
        input.open=false;
        pump(overlay,[&]{return menu_seen==0&&fps_seen==1&&chains.size()==1;},"close menu returns its direct handle");
        chain_limit=1;input.open=true;available=false;
        for(unsigned n=0;n<3;++n){overlay.application_frame(&frame);check(overlay.end_frame(&frame,false)==XR_SUCCESS,"busy inventory preserves game frame");}
        check(chains.size()==1&&menu_seen==0,"unknown count cannot allocate the second UI speculatively");
        available=true;
        pump(overlay,[&]{return menu_seen==1&&fps_seen==1&&chains.size()==1;},"projected14 converts without a spare second native handle");
        check(chains.begin()->second.height==1280,"projected14 selects shared atlas");
        const auto high_reads=reads;external=0;
        for(unsigned n=0;n<5;++n){overlay.application_frame(&frame);overlay.end_frame(&frame,false);}
        check(reads==high_reads&&chains.size()==1&&chains.begin()->second.height==1280,"shared decision does not undo itself after saving a handle");
        input.open=false;
        pump(overlay,[&]{return menu_seen==0&&fps_seen==1&&chains.size()==1&&chains.begin()->second.height<512;},"closing pair releases atlas and restores compact counter");
        chain_limit=2;input.open=true;
        pump(overlay,[&]{return menu_seen==1&&fps_seen==1&&chains.size()==2;},"reopening with room re-evaluates separate surfaces");
        input.open=false;position(ini,"off");
        pump(overlay,[&]{return chains.empty();},"all off releases both direct native reservations");
    }
    check(chains.empty(),"policy session leaves no UI native handles");
    for(const bool limited:{false,true}) {
        input={};input.open=input.focused=true;position(ini,"upper_right");
        max_image_height=limited?1024:4096;
        xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
            device.Get(),queue.Get(),device11.Get(),ini,true,poll);
        if(limited)overlay.set_native_swapchain_count_source([&]()->std::optional<unsigned>{return 12u+static_cast<unsigned>(chains.size());});
        pump(overlay,[&]{return menu_seen==1&&fps_seen==1;},"ordinary or too-small runtime retains both panels");
        check(chains.size()==2,"no SteamVR count source or unsupported atlas keeps separate UI");
        input.open=false;position(ini,"off");
        pump(overlay,[&]{return chains.empty();},"fallback runtime frees its native UI handles");
    }
    max_image_height=4096;
    check(made==destroyed&&chains.empty(),"policy migrations balance all native allocations");
    std::cout<<checks<<" UI automatic threshold checks passed: projected13/14, other runtime, busy inventory, latch, reopen, compact/all-off and no extra native slot\n";
}
