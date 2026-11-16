void independent_release_tail_test() {
    independent_fixture_mode=true;independent_fixture_buffer=0;
    Fixture f(true,true);f.throttled=false;f.unblock();
    auto chain=f.make_chain();f.write(chain,77);
    // The image is released and complete; only the NEXT rendering is held.
    ok(f.queue->Wait(f.gate.Get(),1),"gate later unrelated game GPU work");
    f.begin_virtual();xr_ok(f.submit(chain,1),"accept already released image");
    const auto deadline=std::chrono::steady_clock::now()+50ms;
    while(!f.source_ready_events && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    const bool ready_before_later_work=f.source_ready_events!=0;
    ok(f.gate->Signal(1),"release unrelated game queue");
    f.wait_count(1);xr_ok(f.transport->drain(),"release tail drain");
    check(ready_before_later_work,"released image inherited a readiness marker behind the game's NEXT GPU work");
    check(f.pixels==std::vector<unsigned>{77},"release-scoped readiness changed exact output pixels");
    xr_ok(f.transport->destroy_swapchain(chain),"release tail cleanup");
    f.transport->stop();independent_fixture_mode=false;
    std::puts("Independent release-scoped GPU readiness ignores later game queue tail PASS");
}
void independent_completed_tail_test(bool enabled) {
    independent_fixture_mode=enabled;independent_fixture_buffer=0;
    Fixture f(true,true);f.throttled=false;f.unblock();auto chain=f.make_chain();
    ComPtr<ID3D12Fence> proof;
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&proof)),"exact synthesis completion proof");
    f.release_callback=[&](XrSwapchain handle,unsigned index) {
        std::array<XrSwapchainImageD3D12KHR,3> images;
        for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};unsigned count{};
        xr_ok(f.transport->enumerate_images(handle,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"owned completed D12 textures");
        // write() CPU-observed this output's commands already. Signal the
        // retained producer proof, then hold only later application work.
        ok(f.delivery_queue->Signal(proof.Get(),1),"signal completed output proof");f.gpu_wait(f.delivery_queue.Get());
        ok(f.queue->Wait(f.gate.Get(),1),"hold game before completed-output release");
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if(enabled) {
            check(f.transport->release_completed_image12(handle,&release,index,images[index].texture,proof.Get(),2)==XR_ERROR_CALL_ORDER_INVALID,"pending D12 proof accepted");
            check(f.transport->release_completed_image12(handle,&release,(index+1)%count,images[index].texture,proof.Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"wrong D12 proof index accepted");
            check(f.transport->release_completed_image12(handle,&release,index,images[(index+1)%count].texture,proof.Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"foreign D12 texture proof accepted");
        }
        auto result=f.transport->release_completed_image12(handle,&release,index,images[index].texture,proof.Get(),1);
        if(!enabled) {
            check(result==XR_ERROR_FUNCTION_UNSUPPORTED,"legacy D12 unexpectedly changed release policy");
            result=f.transport->release_image(handle,&release);
        }
        return result;
    };
    f.write(chain,99);f.begin_virtual();xr_ok(f.submit(chain,1),"completed D12 output handed off");
    const auto deadline=std::chrono::steady_clock::now()+50ms;
    while(!f.source_ready_events && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    const bool ready_without_game=f.source_ready_events!=0;
    ok(f.gate->Signal(1),"unblock future game GPU work");
    f.wait_count(1);xr_ok(f.transport->drain(),"completed-proof output drain");
    check(ready_without_game==enabled,"completed-output release inherited unrelated game GPU work or changed legacy control");
    check(f.pixels==std::vector<unsigned>{99},"exact D12 proof changed output pixels");
    xr_ok(f.transport->destroy_swapchain(chain),"completed-proof cleanup");
    f.transport->stop();independent_fixture_mode=false;
    std::puts(enabled?"Independent completed D12 proof ignores blocked game queue PASS":"Legacy D12 readiness unchanged, blocked-game control PASS");
}
