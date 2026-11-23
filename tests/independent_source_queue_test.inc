// The shipping separate-queue topology: application/OSR writes on G,
// release-ready markers on B, and native XR copies on D. A later EndFrame
// join on B cannot repair a ready marker that was already signalled on B.
static int independent_source_queue_test() {
    xrfg_fixture::ControlProfile profile(true);
    auto desired=xrfg::embedded::snapshot().desired;desired.enabled=false;
    check(xrfg_fixture::request_control(desired),"source-queue OSR-only profile");
    independent_fixture_mode=true;independent_fixture_binding_queue_handoff=true;
    Fixture f(true);f.throttled=false;f.osr_content_timestamps=true;f.unblock();
    ComPtr<ID3D12CommandQueue> game_queue;
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ok(f.device12->CreateCommandQueue(&q,IID_PPV_ARGS(&game_queue)),"separate application/OSR queue G");
    const auto handle=f.make_chain();
    for(unsigned i=0;i<3;++i)f.write(handle,77); // Every reusable source starts with known old pixels.
    auto dispatch=std::make_shared<Dispatch>();
    dispatch->release_swapchain_image=+[](XrSwapchain h,const XrSwapchainImageReleaseInfo* i)->XrResult {
        return current->transport->release_image(h,i);
    };
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->graphics_binding=SessionGraphicsBinding::d3d12;state->independent_pacing=true;
    state->d3d12_device=f.device12;state->d3d12_queue=game_queue;
    state->binding_queue=f.queue;state->private_delivery_queue=f.delivery_queue;
    state->private_runtime=f.transport;
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&state->app_release_fence)),"application release fence");
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.width=create.height=create.arraySize=create.mipCount=create.faceCount=create.sampleCount=1;
    create.format=DXGI_FORMAT_R8G8B8A8_UNORM;
    auto chain=std::make_shared<SwapchainState>(state,create);chain->handle=handle;
    std::array<XrSwapchainImageD3D12KHR,3> images;
    for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    unsigned count{},index{};
    xr_ok(f.transport->enumerate_images(handle,3,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"source-queue images");
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
    xr_ok(f.transport->acquire_image(handle,&acquire,&index),"source-queue acquire");
    xr_ok(f.transport->wait_image(handle,&wait),"source-queue wait");
    for(const auto& image:images)chain->enumerated_d3d12_images.emplace_back(image.texture);
    chain->acquired_indices.push_back(index);chain->waited_count=1;chain->ownership_tracking_valid=true;
    {std::scoped_lock lock(g_state_mutex);g_sessions[state->handle]=state;g_swapchains[handle]=chain;}
    ComPtr<ID3D12Fence> gate;
    ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"hold OSR source rendering");
    ok(game_queue->Wait(gate.Get(),1),"gate application GPU work, leaving B and D runnable");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    ok(f.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"source-queue allocator");
    ok(f.device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"source-queue commands");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> rtv;ok(f.device12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"source-queue RTV");
    f.device12->CreateRenderTargetView(images[index].texture,nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    const float fresh[4]{200/255.F,0,0,1};
    commands->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),fresh,0,nullptr);
    ok(commands->Close(),"source-queue close");ID3D12CommandList* submitted[]={commands.Get()};
    game_queue->ExecuteCommandLists(1,submitted);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const auto entered=ns();
    xr_ok(layer_release_swapchain_image_impl(handle,&release),"ordinary source release through shipping layer");
    const auto release_cpu=ns()-entered;
    // Exactly the existing OSR-only EndFrame ordering, after the image release.
    join_runtime_queue_to_application(state.get(),state->app_release_counter);
    f.begin_virtual();
    const auto content_time=epoch+ns()+500'000'000;
    f.transport->set_next_end_content_time(content_time);
    xr_ok(f.submit(handle,123),"submit new pose while its corresponding source writes are gated");
    std::this_thread::sleep_for(60ms);
    const bool delivered_early=f.real_ends.load()!=0;
    ok(gate->Signal(1),"release source GPU gate before checking failure");
    f.wait_count(1);xr_ok(f.transport->drain(),"source-queue recovery drain");
    f.transport->stop();
    if(delivered_early || f.pixels!=std::vector<unsigned>{200})std::fprintf(stderr,
        "SOURCE_QUEUE_RACE delivered_before_source=%u pixel=%u expected=200 pose=%.1f\n",
        delivered_early?1u:0u,f.pixels.empty()?0:f.pixels.front(),f.positions.empty()?0:f.positions.front());
    check(release_cpu<100'000'000,"source readiness ordering must be GPU-side, without a CPU completion wait");
    check(!delivered_early,"Independent must not publish a new pose while application/OSR source pixels are unfinished");
    check(f.pixels==std::vector<unsigned>{200} && f.positions==std::vector<float>{123} &&
        f.targets==std::vector<XrTime>{content_time},"source pixels, pose and r3 content timestamp reach native XR together");
    {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(handle);g_sessions.erase(state->handle);}
    xr_ok(f.transport->destroy_swapchain(handle),"source-queue cleanup");
    independent_fixture_mode=false;independent_fixture_binding_queue_handoff=false;
    std::puts("Independent source queues: delayed application/OSR GPU writes precede release-ready marker and native copy PASS");
    return 0;
}
