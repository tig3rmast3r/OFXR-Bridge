void release_readiness_test(bool enabled) {
    Fixture f(false,true,{},true,true,enabled,enabled?2:3);f.throttled=false;f.unblock();
    auto chain=f.make_chain(),overlay=f.make_chain();
    ComPtr<IDXGIDevice> dxgi;ok(f.device11.As(&dxgi),"proof DXGI device");
    ComPtr<IDXGIAdapter> adapter;ok(dxgi->GetAdapter(&adapter),"proof same adapter");
    ComPtr<ID3D12Device> device12;ok(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)),"proof device");
    ComPtr<ID3D12Fence> proof,gate;ComPtr<ID3D11Device5> device11;ComPtr<ID3D11DeviceContext4> context11;
    ok(f.device11.As(&device11),"proof D11 owner");ok(f.context.As(&context11),"proof D11 context");
    auto shared=[&](ComPtr<ID3D12Fence>& fence) {
        ok(device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence)),"shared timeline");
        HANDLE h{};ok(device12->CreateSharedHandle(fence.Get(),nullptr,GENERIC_ALL,nullptr,&h),"shared handle");
        ComPtr<ID3D11Fence> r;const auto hr=device11->OpenSharedFence(h,IID_PPV_ARGS(&r));CloseHandle(h);ok(hr,"D11 timeline");return r;
    };
    auto proof11=shared(proof),gate11=shared(gate);
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    unsigned index{},count{};xr_ok(f.transport->acquire_image(chain,&acquire,&index),"owned proof image");
    xr_ok(f.transport->wait_image(chain,&wait),"owned proof wait");
    std::array<XrSwapchainImageD3D11KHR,3> images;for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
    xr_ok(f.transport->enumerate_images(chain,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"exact proof textures");
    ComPtr<ID3D11RenderTargetView> rtv;ok(f.device11->CreateRenderTargetView(images[index].texture,nullptr,&rtv),"proof RTV");
    float color[]={77/255.F,0,0,1};f.context->ClearRenderTargetView(rtv.Get(),color);
    ok(context11->Signal(proof11.Get(),1),"actual completed image signal");context11->Flush();
    HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(bool(done),"proof event");
    ok(proof->SetEventOnCompletion(1,done),"proof completion event");const auto finished=WaitForSingleObject(done,3000);CloseHandle(done);
    check(finished==WAIT_OBJECT_0 && proof->GetCompletedValue()>=1,"actual image proof completed");
    if(enabled) {
        check(f.transport->release_completed_image(chain,&release,index,images[index].texture,proof.Get(),2)==XR_ERROR_CALL_ORDER_INVALID,"pending proof accepted");
        check(f.transport->release_completed_image(chain,&release,(index+1)%3,images[index].texture,proof.Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"wrong index accepted");
        check(f.transport->release_completed_image(chain,&release,index,images[(index+1)%3].texture,proof.Get(),1)==XR_ERROR_CALL_ORDER_INVALID,"foreign texture accepted");
    }
    auto released=f.transport->release_completed_image(chain,&release,index,images[index].texture,proof.Get(),1);
    if(!enabled){check(released==XR_ERROR_FUNCTION_UNSUPPORTED,"control unexpectedly opts in");released=f.transport->release_image(chain,&release);}
    xr_ok(released,"exact completed release");
    ok(context11->Wait(gate11.Get(),1),"future-work GPU gate");context11->Flush();
    f.begin_virtual();const auto submitted=f.submit(chain,1);
    const auto deadline=std::chrono::steady_clock::now()+35ms;
    while(!f.source_ready_events && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    const bool ready_before_gate=f.source_ready_events!=0;const auto pending_before_gate=f.pending_source_events.load();
    gate->Signal(1);xr_ok(submitted,"owned output acceptance");f.wait_count(1);xr_ok(f.transport->drain(),"completed proof drain");
    check(ready_before_gate==enabled,"exact proof inherited future context work, or old control skipped it");
    if(enabled)check(!pending_before_gate,"completed proof still pending");
    check(f.pixels==std::vector<unsigned>{77},"completed proof changed source pixels");
    // A pending auxiliary write has no projection completion proof.
    const auto events=f.source_ready_events.load();
    ok(context11->Wait(gate11.Get(),2),"pending auxiliary gate");context11->Flush();
    f.write(overlay,86);f.begin_virtual();const auto mixed=f.submit(chain,2,overlay);
    std::this_thread::sleep_for(15ms);const bool auxiliary_waited=f.source_ready_events==events;
    gate->Signal(2);xr_ok(mixed,"mixed layers accepted");f.wait_count(2);xr_ok(f.transport->drain(),"auxiliary drain");
    check(auxiliary_waited && f.overlay_pixels.back()==86 && f.pixels.back()==77,"pending auxiliary bypassed or pixels changed");
    // A later rewritten generation must not inherit the earlier proof.
    const auto before_rewrite=f.source_ready_events.load();
    ok(context11->Wait(gate11.Get(),3),"pending rewrite gate");context11->Flush();
    f.write(chain,99);f.begin_virtual();const auto rewritten=f.submit(chain,3);
    std::this_thread::sleep_for(15ms);const bool rewrite_waited=f.source_ready_events==before_rewrite;
    gate->Signal(3);xr_ok(rewritten,"rewritten source accepted");f.wait_count(3);xr_ok(f.transport->drain(),"rewrite drain");
    check(rewrite_waited && f.pixels.back()==99,"new source reused old proof");
    check(!f.work_margins.empty() && std::all_of(f.work_margins.begin(),f.work_margins.end(),[](auto margin){return margin==0;}),
        "completed packet inherited a past GPU-service eligibility floor");
    check(f.positions==std::vector<float>({1,2,3}),"readiness path reordered native packets");
    for(unsigned i=1;i<f.handoffs.size();++i)check(f.handoffs[i]-f.handoffs[i-1]>=period-100'000,"readiness path shortened physical cadence");
    xr_ok(f.transport->destroy_swapchain(chain),"proof cleanup");xr_ok(f.transport->destroy_swapchain(overlay),"overlay cleanup");
    f.transport->stop();check(f.real_begins==f.real_ends+f.empty_ends+f.discarded_clocks,"native protocol unbalanced");
    std::puts(enabled?"D11 exact release readiness, pending overlay/rewrite, pixels/order and no held service floor PASS":"D11 original End-marker future-work control PASS");
}
