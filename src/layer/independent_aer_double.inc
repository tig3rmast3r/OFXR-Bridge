// Temporal generation AFTER AER reconstruction. Each source here is already a
// coherent stereo endpoint. Raw staggered eyes never enter this second history.
struct IndependentAerDouble {
    using Resource=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Slot {
        std::shared_ptr<xrfg::PrivateOpenXrRuntime> runtime;
        XrSwapchain handle{};Resource image;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> image11;
        std::atomic_bool busy{};
        ~Slot(){if(handle && runtime)static_cast<void>(runtime->destroy_swapchain(handle));}
    };
    struct Lease {
        std::shared_ptr<Slot> slot;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        std::uint64_t value{};
        bool acquired{};
        ~Lease(){
            // Only an aborted transaction still owns a virtual acquisition.
            // Preserve any submitted writes before balancing that acquisition.
            if(acquired) {
                if(fence && value)static_cast<void>(IndependentAerDouble::wait(fence.Get(),value));
                static_cast<void>(slot->runtime->release_image(slot->handle,nullptr));
            }
            slot->busy=false;
        }
    };
    struct Channel {
        XrSwapchain source{};
        Resource input,scratch;
        std::array<std::shared_ptr<Slot>,xrfg::kIndependentOutputCredits> slots;
        std::shared_ptr<xrfg::D3D11D3D12SwapchainInterop> interop;
        std::shared_ptr<xrfg::D3D12SwapchainHistory> history;
        std::shared_ptr<xrfg::D3D12FrameSynthesizer> generator;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        D3D12_RESOURCE_DESC description{};
        ~Channel(){if(generator)static_cast<void>(generator->wait_for_idle());}
    };
    std::vector<std::unique_ptr<Channel>> channels;
    std::optional<ProjectionSnapshot> previous;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    std::uint64_t ordinal{};
    std::uint64_t configuration{UINT64_MAX};
    HRESULT failure{S_OK};
    unsigned stage{}; // failure location in the optional second stage
#if defined(XRFG_SOFTWARE_OPTICAL_FIXTURE)
    std::atomic_size_t submitted_for_test{};
    std::function<void()> before_publication_for_test;
#endif

    static bool native_d11(const SessionState& state) noexcept {
        return state.graphics_binding==SessionGraphicsBinding::d3d11 && !state.d3d11_bridge && !state.vulkan_bridge;
    }
    // Adopt only exact OFXR-owned, same-device shared backings. The first AER
    // stage has completed its D12 writes; no additional D11 copy is necessary.
    static Resource endpoint_image(const std::shared_ptr<SessionState>& state,XrSwapchain handle,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>* view11=nullptr) {
        unsigned count{};
        if(native_d11(*state)) {
            XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            const auto result=state->private_runtime->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image));
            if(XR_FAILED(result) || count!=1 || !image.texture) {
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    111002,handle_value(handle),std::uint32_t(result),count);
                return {};
            }
            if(view11)*view11=image.texture;
            return xrfg::owned_d11_output::backing_for(image.texture,state->d3d12_device.Get());
        }
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
        const auto result=state->private_runtime->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image));
        return XR_SUCCEEDED(result) && count==1 && image.texture?Resource(image.texture):Resource{};
    }

    static D3D12_RESOURCE_DESC description(const Resource& resource,ID3D11Texture2D* image) {
        if(resource)return resource->GetDesc();
        D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);
        D3D12_RESOURCE_DESC out{};out.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        out.Width=d.Width;out.Height=d.Height;out.DepthOrArraySize=UINT16(d.ArraySize);
        out.MipLevels=1;out.Format=d.Format;out.SampleDesc=d.SampleDesc;
        return out;
    }

    static HRESULT wait(ID3D12Fence* fence,std::uint64_t value) {
        if(!fence || !value)return E_INVALIDARG;
        auto completed=fence->GetCompletedValue();if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
        if(completed>=value)return S_OK;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
        auto hr=fence->SetEventOnCompletion(value,event);
        if(SUCCEEDED(hr) && WaitForSingleObject(event,10000)!=WAIT_OBJECT_0)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        CloseHandle(event);return hr;
    }
    static Resource resource(ID3D12Device* device,D3D12_RESOURCE_DESC desc) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Resource r;const auto hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r));
        if(FAILED(hr))throw std::runtime_error("AER4 private texture allocation failed");return r;
    }
    static bool same_extent(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b) {
        return a.Width==b.Width && a.Height==b.Height && a.DepthOrArraySize==b.DepthOrArraySize &&
            a.Format==b.Format && a.SampleDesc.Count==b.SampleDesc.Count;
    }
    static bool same_layout(const ProjectionSnapshot& a,const ProjectionSnapshot& b) {
        if(a.layers.size()!=b.layers.size())return false;
        for(std::size_t i=0;i<a.layers.size();++i) {
            const auto& x=a.layers[i];const auto& y=b.layers[i];
            if(x.space!=y.space || x.layer_flags!=y.layer_flags || x.views.size()!=y.views.size())return false;
            for(std::size_t j=0;j<x.views.size();++j) {
                const auto& u=x.views[j].subImage;const auto& v=y.views[j].subImage;
                if(u.imageArrayIndex!=v.imageArrayIndex || u.imageRect.offset.x!=v.imageRect.offset.x ||
                   u.imageRect.offset.y!=v.imageRect.offset.y || u.imageRect.extent.width!=v.imageRect.extent.width ||
                   u.imageRect.extent.height!=v.imageRect.extent.height)return false;
            }
        }
        return true;
    }
    HRESULT initialize_channel(const std::shared_ptr<SessionState>& state,Channel& c,
        const ProjectionResourceMapping& mapping,const Resource& source,ID3D11Texture2D* source11) {
        stage=11;c.source=mapping.application_swapchain;c.description=description(source,source11);
        if(c.description.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || c.description.SampleDesc.Count!=1)return E_NOTIMPL;
        auto device=state->d3d12_device.Get();
        // Copy/SRV/RTV resources, not UAVs: BGRA XR colors need no typed UAV.
        auto desc=c.description;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        c.input=resource(device,desc);c.scratch=resource(device,desc);
        auto original=find_swapchain(mapping.application_swapchain);if(!original)return E_INVALIDARG;
        auto info=original->create_info;info.next=nullptr;
        info.width=unsigned(desc.Width);info.height=desc.Height;info.arraySize=desc.DepthOrArraySize;
        // First-stage endpoints contain mip zero only, even when the game's
        // source swapchain has a mip chain. Do not inherit that source layout.
        info.mipCount=desc.MipLevels;info.sampleCount=desc.SampleDesc.Count;
        // Match the first reconstruction stage's native publication pool.
        info.usageFlags|=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        if(info.createFlags || info.faceCount!=1)return E_NOTIMPL;
        std::vector<ID3D12Resource*> destinations;
        std::vector<ID3D11Texture2D*> destinations11;bool transfer=!source;
        for(auto& entry:c.slots) {
            entry=std::make_shared<Slot>();entry->runtime=state->private_runtime;
            const auto publication_state=native_d11(*state) && !state->d3d12_synthesis_queue
                ?D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_COMMON;
            auto result=entry->runtime->create_swapchain(&info,&entry->handle,publication_state,1,c.source,
                native_d11(*state)?device:nullptr);
            if(XR_FAILED(result))return E_FAIL;
            entry->image=endpoint_image(state,entry->handle,&entry->image11);
            if(!entry->image && !entry->image11)return E_NOINTERFACE;
            transfer|=!entry->image;
            destinations.push_back(entry->image.Get());destinations11.push_back(entry->image11.Get());
        }
        if(transfer) {
            if(!native_d11(*state) || !source11)return E_NOINTERFACE;
            c.interop=std::make_shared<xrfg::D3D11D3D12SwapchainInterop>();
            ID3D11Texture2D* sources[]{source11};
            // Only the current mirror bank is used by this stereo stage.
            const auto result=c.interop->initialize(state->d3d11_device.Get(),state->d3d11_context.Get(),
                device,queue.Get(),sources,destinations11,
                std::span<ID3D11Texture2D* const>(destinations11.data(),1));
            if(FAILED(result))return result;
            const auto mirrors=c.interop->current_destination_images();
            destinations.assign(mirrors.begin(),mirrors.end());
        }
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
            111000,handle_value(c.source),transfer?1:0,c.description.Format);
        auto hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.allocator));
        if(SUCCEEDED(hr))hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.allocator.Get(),nullptr,IID_PPV_ARGS(&c.list));
        if(SUCCEEDED(hr))hr=c.list->Close();if(FAILED(hr))return hr;
        c.history=std::make_shared<xrfg::D3D12SwapchainHistory>();ID3D12Resource* input[]{c.input.Get()};
        hr=c.history->initialize(device,queue.Get(),input,D3D12_RESOURCE_STATE_COMMON);if(FAILED(hr))return hr;
        c.generator=std::make_shared<xrfg::D3D12FrameSynthesizer>();
        auto options=state->nvidia_options;options.ofr=false;options.hybrid=false;options.rife_multiplier=2;options.rife_async=options.rife_model>=0;
        ID3D12Resource* scratch[]{c.scratch.Get()};
        return c.generator->initialize(device,queue.Get(),c.history,scratch,destinations,DXGI_FORMAT(info.format),
            D3D12_RESOURCE_STATE_COMMON,state->optical_flow_backend,options,false,xrfg::D3D12FrameGenerationMode::ofxr);
    }
    HRESULT capture(Channel& c,ID3D12Resource* source,D3D12_RESOURCE_STATES source_state,
        xrfg::D3D12HistoryCaptureTicket& captured) {
        stage=41;auto hr=c.allocator->Reset();if(SUCCEEDED(hr))hr=c.list->Reset(c.allocator.Get(),nullptr);if(FAILED(hr))return hr;
        const auto transition=[](ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
            D3D12_RESOURCE_BARRIER v{};v.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};return v;
        };
        D3D12_RESOURCE_BARRIER before[]{transition(source,source_state,D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(c.input.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
        c.list->ResourceBarrier(2,before);c.list->CopyResource(c.input.Get(),source);
        D3D12_RESOURCE_BARRIER after[]{transition(source,D3D12_RESOURCE_STATE_COPY_SOURCE,source_state),
            transition(c.input.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};
        c.list->ResourceBarrier(2,after);hr=c.list->Close();if(FAILED(hr))return hr;
        ID3D12CommandList* lists[]{c.list.Get()};queue->ExecuteCommandLists(1,lists);
        xrfg::D3D12HistoryCaptureWait budget{};budget.limit_us=1000000;
        stage=42;hr=c.history->capture(0,&captured,nullptr,&budget);if(FAILED(hr))return hr;
        hr=c.history->commit(captured);if(FAILED(hr))return hr;
        // submit_prime/pair acquires and retires the immutable history lease
        // against its own GPU fence. A CPU wait here serializes the eyes.
        return S_OK;
    }
    HRESULT process(const std::shared_ptr<SessionState>& state,const std::shared_ptr<GeneratedFrameEndInfo>& endpoint,
        std::span<const ProjectionResourceMapping> mappings,std::shared_ptr<GeneratedFrameEndInfo>& midpoint) {
        midpoint.reset();
        const auto epoch=state->djules_configuration_epoch.load();
        if(configuration!=epoch) {
            channels.clear();previous.reset();failure=S_OK;configuration=epoch;
        }
        if(FAILED(failure))return failure; // retry only after a real mode/configuration change
        try {
            const auto hr=process_endpoint(state,endpoint,mappings,midpoint);
            if(FAILED(hr))failure=hr;
            return hr;
        } catch(const std::bad_alloc&) {failure=E_OUTOFMEMORY;}
          catch(...) {failure=E_FAIL;}
        return failure;
    }
    HRESULT process_endpoint(const std::shared_ptr<SessionState>& state,const std::shared_ptr<GeneratedFrameEndInfo>& endpoint,
        std::span<const ProjectionResourceMapping> mappings,std::shared_ptr<GeneratedFrameEndInfo>& midpoint) {
        midpoint.reset();
        if(!endpoint->interface_stamp.aer_mixed){channels.clear();previous.reset();return S_FALSE;}
        if(!state->d3d12_device || !state->private_runtime)return E_NOTIMPL;
        if(!queue) {
            D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
            const auto hr=state->d3d12_device->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue));if(FAILED(hr))return hr;
        }
        ProjectionSnapshot current;if(!capture_projection_snapshot(&endpoint->info,&current))return E_INVALIDARG;
        current.interface_stamp=endpoint->interface_stamp;
        stage=10;
        std::vector<Resource> images;std::vector<ProjectionResourceMapping> inputs;
        std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> images11;
        for(const auto& mapping:mappings) {
            if(mapping.views.empty())return E_INVALIDARG;
            const auto ref=mapping.views.front();
            if(ref.projection_index>=current.layers.size() || ref.view_index>=current.layers[ref.projection_index].views.size())return E_INVALIDARG;
            auto input=mapping;input.application_swapchain=current.layers[ref.projection_index].views[ref.view_index].subImage.swapchain;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> image11;
            auto image=endpoint_image(state,input.application_swapchain,&image11);
            if(!image && !image11) {
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    111001,handle_value(input.application_swapchain),handle_value(mapping.application_swapchain),0);
                return E_NOINTERFACE;
            }
            images11.push_back(std::move(image11));images.push_back(std::move(image));inputs.push_back(std::move(input));
        }
        bool reset=channels.size()!=mappings.size() || (previous && !same_layout(*previous,current));
        for(std::size_t i=0;!reset && i<channels.size();++i)
            reset=channels[i]->source!=mappings[i].application_swapchain || !same_extent(channels[i]->description,description(images[i],images11[i].Get())) ||
                (!channels[i]->interop && !images[i]);
        if(reset){channels.clear();previous.reset();}
        if(channels.empty())for(std::size_t i=0;i<mappings.size();++i) {
            auto channel=std::make_unique<Channel>();const auto hr=initialize_channel(state,*channel,mappings[i],images[i],images11[i].Get());
            if(FAILED(hr)){channels.clear();previous.reset();return hr;}channels.push_back(std::move(channel));
        }
        const auto camera=previous?synthetic_camera_snapshot(*previous,current,.5f):std::optional<ProjectionSnapshot>{};
        if(previous && !camera){channels.clear();previous.reset();return S_FALSE;}
        struct Timings {
            bool enabled=xrfg::bridge_flight_logger().enabled();
            std::uint64_t ordinal;std::int64_t entered=enabled?xrfg::interface_clock_ns():0;
            std::array<std::int64_t,6> costs{};
            ~Timings(){if(enabled){
                for(unsigned i=0;i<costs.size();++i)xrfg::bridge_flight_logger().event(
                    xrfg::BridgeFlightOperation::presenter_transition,111110+i,ordinal,costs[i],0);
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    111116,ordinal,xrfg::interface_clock_ns()-entered,0);
            }}
        } timing{xrfg::bridge_flight_logger().enabled(),ordinal+1};
        const auto timed=[&](unsigned bucket,auto&& operation) {
            const auto before=timing.enabled?xrfg::interface_clock_ns():0;
            const auto hr=operation();
            if(timing.enabled)timing.costs[bucket]+=xrfg::interface_clock_ns()-before;
            return hr;
        };
        struct Work {
            Channel* channel{};
            xrfg::D3D12HistoryCaptureTicket capture;
            xrfg::D3D12FrameSynthesisTicket ticket;
            std::shared_ptr<Slot> slot;
            std::shared_ptr<Lease> lease;
            Microsoft::WRL::ComPtr<ID3D12Fence> fence;
            std::uint64_t ready{};unsigned output{},index{};
        };
        std::vector<Work> work(channels.size());
        // Reserve the whole stereo transaction before submitting any GPU work.
        // A busy native consumer must not stall generation or game admission.
        if(previous)for(std::size_t i=0;i<channels.size();++i) {
            auto& w=work[i];auto& c=*channels[i];w.channel=&c;
            for(unsigned j=0;j<c.slots.size();++j) {
                bool idle=false;
                if(c.slots[j]->busy.compare_exchange_strong(idle,true)) {
                    w.slot=c.slots[j];w.output=j;w.lease=std::make_shared<Lease>();w.lease->slot=w.slot;
                    XrResult acquired{};
                    auto hr=timed(4,[&] {
                        stage=3;
                        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        acquired=w.slot->runtime->try_acquire_image(w.slot->handle,&acquire,&w.index);
                        if(acquired==XR_TIMEOUT_EXPIRED)return S_FALSE;
                        if(XR_FAILED(acquired))return E_FAIL;
                        w.lease->acquired=true;
                        XrSwapchainImageWaitInfo waited{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};waited.timeout=XR_INFINITE_DURATION;
                        return XR_FAILED(w.slot->runtime->wait_image(w.slot->handle,&waited))?E_FAIL:S_OK;
                    });
                    if(FAILED(hr))return hr;
                    if(acquired==XR_TIMEOUT_EXPIRED) {
                        // Descriptor ownership can retire before the native
                        // copy releases its exact image reader. Try another
                        // existing ring slot; never wait on that reader here.
                        if(timing.enabled)xrfg::bridge_flight_logger().event(
                            xrfg::BridgeFlightOperation::presenter_transition,112100,ordinal+1,i,j);
                        w.lease.reset();w.slot.reset();continue;
                    }
                    break;
                }
            }
            if(!w.slot) {
                xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,
                    111118,ordinal+1,i,c.slots.size());
                return S_FALSE; // keep the last actual history/camera anchor
            }
        }
        // Queue both history copies before either eye waits for the GPU.
        for(std::size_t i=0;i<channels.size();++i) {
            auto& w=work[i];auto& c=*channels[i];w.channel=&c;
            auto hr=timed(0,[&] {
                auto source=images[i];
                if(c.interop) {
                    stage=12;std::scoped_lock gpu_lock(state->gpu_mutex);
                    const auto result=c.interop->prepare_capture_image(0,images11[i].Get());if(FAILED(result))return result;
                    source=c.interop->source_images()[0];
                }
                const auto result=capture(c,source.Get(),native_d11(*state) || state->d3d12_synthesis_queue
                    ?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_RENDER_TARGET,w.capture);
                if(FAILED(result))return result;
                return c.interop?c.interop->finish_capture():S_OK;
            });if(FAILED(hr))return hr;
        }
        // Submit all eye generators before waiting for their completion proofs.
        for(std::size_t i=0;i<work.size();++i) {
            auto& w=work[i];auto& c=*w.channel;
            auto views=build_reprojection_views(current,inputs[i]);if(!views)return E_INVALIDARG;
            HRESULT hr;
            if(!previous) {
                stage=5;hr=timed(1,[&]{return c.generator->submit_prime(w.capture,*views,0,&w.ticket);});
            }else {
                auto targets=build_reprojection_views(*camera,inputs[i]);if(!targets)return E_INVALIDARG;
                stage=6;hr=timed(1,[&]{return c.generator->submit_pair(w.capture,*views,*targets,w.output,0,
                    &w.ticket,std::nullopt,{},false,.5f,{},std::nullopt,true);});
            }
            if(FAILED(hr))return hr;
            w.fence=c.generator->submission_fence(w.ticket);w.ready=w.ticket.fence_value;
            if(w.lease){w.lease->fence=w.fence;w.lease->value=w.ready;}
#if defined(XRFG_SOFTWARE_OPTICAL_FIXTURE)
            submitted_for_test.store(i+1,std::memory_order_release);
#endif
        }
        // All commands are now queued. Each publication names its exact fence.
        GeneratedFrameEndInfo result;std::vector<ProjectionResourceDestination> destinations;
        bool paired=previous.has_value();
        for(std::size_t i=0;i<work.size();++i) {
            auto& w=work[i];auto& c=*w.channel;
            stage=7;auto hr=timed(2,[&]{return wait(w.fence.Get(),w.ready);});if(FAILED(hr))return hr;
            if(!previous)continue;
            paired &= w.ticket.previous_serial!=0 && (!w.ticket.native_result || w.ticket.native_result->generated());
            if(c.interop) {
                xrfg::D3D11PublicationReceipt receipt;
#if defined(XRFG_SOFTWARE_OPTICAL_FIXTURE)
                if(before_publication_for_test)before_publication_for_test();
#endif
                hr=timed(3,[&] {
                    stage=13;std::scoped_lock gpu_lock(state->gpu_mutex);
                    return c.interop->publish_completed(w.output,std::nullopt,w.fence.Get(),w.ready,&receipt);
                });if(FAILED(hr))return hr;
                // The private consumer already retains and waits this exact
                // publication receipt. Free the generator while D11 copies run.
                w.fence=receipt.fence;w.ready=receipt.copy_complete_value;
            }
            w.lease->fence=w.fence;w.lease->value=w.ready;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};stage=8;
            const auto released=timed(5,[&] {
                const auto result=w.slot->image11
                    ?w.slot->runtime->release_queued_image(w.slot->handle,&release,w.index,w.slot->image11.Get(),w.fence.Get(),w.ready)
                    :w.slot->runtime->release_completed_image12(w.slot->handle,&release,w.index,w.slot->image.Get(),w.fence.Get(),w.ready);
                return XR_FAILED(result)?E_FAIL:S_OK;
            });if(FAILED(released))return released;
            w.lease->acquired=false;
            if(w.ticket.native_result)result.native_results.push_back(w.ticket.native_result);
            result.reference_readiness.push_back({w.fence,w.ready,false,w.ticket.inputs});
            destinations.push_back({inputs[i].application_swapchain,w.slot->handle,true,false});
        }
        previous=current;++ordinal;
        if(!paired)return S_FALSE;
        auto native_results=std::move(result.native_results);auto readiness=std::move(result.reference_readiness);
        if(!build_generated_frame_end_info(&endpoint->info,current,true,destinations,&result,&*camera,.5f))return E_INVALIDARG;
        result.native_results=std::move(native_results);result.reference_readiness=std::move(readiness);
        for(auto& w:work)result.independent_output_leases.push_back(std::move(w.lease));
        result.independent_output_leases.push_back(endpoint); // immutable auxiliary pixels until copy completes
        result.composition_images=endpoint->composition_images;
        result.synthetic=true;result.interface_stamp.aer_mixed=false;result.interface_stamp.aer_generated_pair=true;
        result.app_release_value=result.input_release_value=0;
        midpoint=make_presenter_owned_frame(std::move(result));
        xrfg::bridge_flight_logger().event(xrfg::BridgeFlightOperation::presenter_transition,110740,ordinal,2,mappings.size());
        return midpoint?S_OK:E_FAIL;
    }
};
