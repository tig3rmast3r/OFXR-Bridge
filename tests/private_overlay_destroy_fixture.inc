static void optional_overlay_destroy(bool d12) {
    Fixture f(d12);f.throttled=false;f.block_end=true;
    const auto chain=f.make_chain();f.write(chain,71);f.begin_virtual();
    xr_ok(f.submit(chain,1),"accept old UI image before retirement");f.wait_entered();
    auto attempt=std::async(std::launch::async,[&]{return f.transport->try_destroy_swapchain(chain);});
    const bool independent=attempt.wait_for(40ms)==std::future_status::ready;
    // Always release the test's native End before judging failure, so the
    // negative control cannot hang fixture shutdown or leave GPU work held.
    f.unblock();check(attempt.wait_for(3s)==std::future_status::ready,"optional retirement completes after native release");
    const auto result=attempt.get();
    check(independent&&result==XR_ERROR_CALL_ORDER_INVALID,"optional UI destroy must defer instead of waiting for an old native End");
    check(f.transport->owns(chain),"busy optional retirement must not poison or remove the source");
    f.wait_count(1);xr_ok(f.transport->drain(),"old UI native consumer retires");
    XrResult retired=XR_ERROR_CALL_ORDER_INVALID;
    for(unsigned n=0;n<100 && retired==XR_ERROR_CALL_ORDER_INVALID;++n) {
        retired=f.transport->try_destroy_swapchain(chain);
        if(retired==XR_ERROR_CALL_ORDER_INVALID)std::this_thread::sleep_for(1ms);
    }
    xr_ok(retired,"idle UI source can be returned on a later nonblocking retry");
    check(!f.transport->owns(chain),"successful optional retirement removes exactly its source");
    const auto app=f.make_chain();f.write(app,82);f.block_end=true;f.end_entered=false;f.begin_virtual();
    xr_ok(f.submit(app,2),"ordinary application source remains independent");f.wait_entered();
    auto ordinary=std::async(std::launch::async,[&]{return f.transport->destroy_swapchain(app);});
    const bool waited=ordinary.wait_for(40ms)==std::future_status::timeout;
    f.unblock();check(ordinary.wait_for(3s)==std::future_status::ready,"ordinary destroy completes after its real consumer");
    xr_ok(ordinary.get(),"ordinary blocking destroy result");
    check(waited,"ordinary application destruction must retain its original blocking contract");
    std::printf("Optional UI retirement %s: blocked native End deferred, source retained, idle retry PASS\n",d12?"D12":"D11");
}
