#include "luke_stale_queue_fixture.hpp"
// Match the wrapped execution device/queue pair used in-game, while resources
// continue to report the native allocation identity through GetDevice.
class Aer4ExecutionDeviceAlias final:public ReportedDeviceAlias {
public:
    using ReportedDeviceAlias::ReportedDeviceAlias;
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* desc,REFIID iid,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        ComPtr<ID3D12CommandQueue> native;
        auto hr=ReportedDeviceAlias::CreateCommandQueue(desc,__uuidof(ID3D12CommandQueue),reinterpret_cast<void**>(native.GetAddressOf()));
        if(FAILED(hr))return hr;
        auto* wrapped=new QueueWithReportedDeviceAlias(native.Get(),this);
        hr=wrapped->QueryInterface(iid,output);wrapped->Release();return hr;
    }
};
// Exercise the shipping second stage with deliberately distinct per-eye pixels
// and cameras. WARP substitutes only the downstream runtime and optical provider.
static int independent_aer4_stage_test(bool native12=true,bool bgra=false,bool unmarked=false,unsigned app_mips=1,bool retain_many=false,bool hold_gpu=false,bool hold_publication=false,bool alias_owner=false,bool hold_reader=false,unsigned continuity_case=0) {
    xrfg_fixture::ControlProfile profile;
    independent_fixture_mode=true;
    Fixture f(native12,true,{},false,true,!native12);f.throttled=false;f.unblock();
    if(!native12)ok(xrfg::create_d3d12_device_for_d3d11(f.device11.Get(),f.device12.GetAddressOf(),f.queue.GetAddressOf()),"D11 shared-output owner device");
    auto dispatch=std::make_shared<Dispatch>();auto state=std::make_shared<SessionState>(dispatch);
    state->d3d12_device=f.device12;state->d3d12_queue=f.queue;state->private_runtime=f.transport;
    if(alias_owner)state->d3d12_device.Attach(new Aer4ExecutionDeviceAlias(f.device12.Get()));
    state->graphics_binding=native12?SessionGraphicsBinding::d3d12:SessionGraphicsBinding::d3d11;
    state->d3d11_device=f.device11;state->d3d11_context=f.context;
    state->optical_flow_backend=xrfg::D3D12OpticalFlowBackend::fidelity_fx;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=64;
    ci.arraySize=ci.faceCount=ci.sampleCount=ci.mipCount=1;ci.format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    std::array<XrSwapchain,4> sources{};
    const unsigned source_count=unmarked?4:2;
    std::array<ProjectionResourceMapping,2> mappings;
    for(unsigned eye=0;eye<source_count;++eye) {
        xr_ok(f.transport->create_swapchain(&ci,&sources[eye],D3D12_RESOURCE_STATE_RENDER_TARGET,1,XR_NULL_HANDLE,native12?nullptr:state->d3d12_device.Get()),"coherent input allocation");
        auto original=ci;original.mipCount=app_mips;
        auto source=std::make_shared<SwapchainState>(state,original);source->handle=sources[eye];
        {std::scoped_lock lock(g_state_mutex);g_swapchains[sources[eye]]=source;}
        if(eye<2)mappings[eye]={sources[eye],{{0,eye}}};
        if(unmarked) {
            XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};unsigned count{};
            xr_ok(f.transport->enumerate_images(sources[eye],1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"unmarked source enumeration");
            ok(image.texture->SetPrivateDataInterface(xrfg::owned_d11_output::marker_id,nullptr),"remove direct-backing capability");
            check(!xrfg::owned_d11_output::backing_for(image.texture,f.device12.Get()),"test must use a genuinely unmarked D11 image");
        }
    }
    auto endpoint=[&](unsigned n) {
        const auto bank=unmarked?(n%2)*2:0;
        f.write(sources[bank],40+n*4);f.write(sources[bank+1],180+n*4);
        if(!native12) {
            // The real first stage already supplies completed generation
            // fences. These fixture clears instead need their own completion.
            XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};unsigned count{};
            xr_ok(f.transport->enumerate_images(sources[bank+1],1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image)),"coherent D11 input witness");
            Chain read;read.d11.emplace_back(image.texture);static_cast<void>(f.read_pixel(read));
        }
        std::array<XrCompositionLayerProjectionView,2> views;
        for(unsigned eye=0;eye<2;++eye) {
            auto& v=views[eye];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
            v.pose.position={.02f*n+(eye?.032f:-.032f),.01f*n,0};v.fov={-.7f,.7f,.7f,-.7f};
            v.subImage.swapchain=sources[bank+eye];v.subImage.imageRect.extent={64,64};
        }
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
        const auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&layer;
        end.displayTime=epoch+n*period;end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ProjectionSnapshot snapshot;check(capture_projection_snapshot(&end,&snapshot),"coherent metadata snapshot");
        std::array<ProjectionResourceDestination,2> ids{{{sources[bank],sources[bank]},{sources[bank+1],sources[bank+1]}}};
        GeneratedFrameEndInfo owned;check(build_generated_frame_end_info(&end,snapshot,false,ids,&owned),"coherent owned endpoint");
        owned.interface_stamp.mark_aer_mixed();return make_presenter_owned_frame(std::move(owned));
    };
    auto pixel=[&](const GeneratedFrameEndInfo& image,unsigned eye) {
        const auto handle=image.projections[0].views[eye].subImage.swapchain;
        Chain read;read.state=D3D12_RESOURCE_STATE_COMMON;unsigned count{};
        if(native12) {
            XrSwapchainImageD3D12KHR r{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(f.transport->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&r)),"generated D12 pair texture");read.d12.emplace_back(r.texture);
        } else {
            XrSwapchainImageD3D11KHR r{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(f.transport->enumerate_images(handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&r)),"generated D11 pair texture");read.d11.emplace_back(r.texture);
        }
        return f.read_pixel(read,32,32,0,bgra?2:0);
    };
    {
        auto stage_owner=std::make_shared<IndependentAerDouble>();auto& stage=*stage_owner;
        if(continuity_case) {
            state->independent_pacing=true;state->menu_enabled=true;state->nvidia_options.ofr=true;
            state->independent_aer_double=stage_owner;
        }
        std::shared_ptr<GeneratedFrameEndInfo> mid,held;
        std::vector<std::shared_ptr<GeneratedFrameEndInfo>> retained;
        const auto prime=stage.process(state,endpoint(1),mappings,mid);
        std::printf("AER4_STAGE prime hr=%08x stage=%u channels=%zu ordinal=%llu\n",unsigned(prime),stage.stage,stage.channels.size(),stage.ordinal);std::fflush(stdout);
        check(prime==S_FALSE && !mid,"prime cannot count a repeated frame as generation");
        check(stage.channels.size()==2,"prime must initialize both eye channels");
        for(const auto& channel:stage.channels)
            check(std::count_if(channel->slots.begin(),channel->slots.end(),[](const auto& slot){return bool(slot);})==3,
                "each eye must start with three allocated outputs");
        if(alias_owner)for(const auto& channel:stage.channels)
            check(!channel->interop,"proven alias must retain direct shared backings instead of D11 copies");
        if(continuity_case==7) {
            dispatch->destroy_swapchain=+[](XrSwapchain handle){
                return current->transport->destroy_swapchain(handle);
            };
            const auto source=find_swapchain(sources[0]);
            source->frame_generation=std::make_shared<FrameGenerationSwapchainState>();
            auto& primary=source->frame_generation->current[0].handle;
            xr_ok(f.transport->create_swapchain(&ci,&primary,D3D12_RESOURCE_STATE_COMMON,1),"primary output allocation");
            const auto primary_handle=primary;
            source->projection_history_needed=true;

            ok(stage.process(state,endpoint(2),mappings,mid),"disarm reader midpoint");
            check(bool(mid),"disarm reader output");
            const auto generated=mid->projections[0].views[0].subImage.swapchain;
            stage_owner.reset(); // the session is the only owner of the output bank
            std::promise<void> reader_entered, release_reader;
            auto entered=reader_entered.get_future();auto released=release_reader.get_future().share();
            f.expected_views=2;
            f.preparation_callback=[&](std::uint64_t){reader_entered.set_value();released.wait();};
            f.begin_virtual();
            const auto submitted=f.transport->end_frame(&mid->info);
            mid.reset();
            if(XR_FAILED(submitted) || entered.wait_for(std::chrono::seconds(2))!=std::future_status::ready) {
                release_reader.set_value();
                throw std::runtime_error("private reader did not enter before Disarm");
            }
            auto retiring=std::async(std::launch::async,[&]{return retire_disarmed_generation(state);});
            const auto reader_protected=retiring.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout;
            release_reader.set_value();
            check(retiring.get() && reader_protected,"Disarm must retire generation only after the native read");
            f.preparation_callback={};
            unsigned count{};
            check(f.transport->enumerate_images(primary_handle,0,&count,nullptr)==XR_ERROR_HANDLE_INVALID,
                "Disarm must release primary outputs");
            check(f.transport->enumerate_images(generated,0,&count,nullptr)==XR_ERROR_HANDLE_INVALID,
                "Disarm must release second-stage outputs");
            check(!source->frame_generation && !source->projection_history_needed && !state->independent_aer_double,
                "Disarm must clear temporal ownership");
            for(const auto original:sources)if(original)
                xr_ok(f.transport->enumerate_images(original,0,&count,nullptr),"original swapchain survives Disarm");
            auto raw=endpoint(3);
            f.begin_virtual();
            xr_ok(f.transport->end_frame(&raw->info),"raw stereo survives Disarm");
            xr_ok(f.transport->drain(),"retire raw frame after Disarm");
        } else if(continuity_case) {
            ok(stage.process(state,endpoint(2),mappings,mid),"initial 4X pair before gap");
            check(bool(mid),"initial pair exists");held=mid;mid.reset();
            std::array<std::shared_ptr<xrfg::D3D12FrameSynthesizer>,2> generators;
            std::array<IndependentAerDouble::Resource,2> buffers;
            for(unsigned eye=0;eye<2;++eye){generators[eye]=stage.channels[eye]->generator;buffers[eye]=stage.channels[eye]->input;}
            const auto old_left=pixel(*held,0),old_right=pixel(*held,1);
            for(unsigned cycle=0;cycle<3;++cycle) {
                const auto gap=3+cycle*3;auto missing=endpoint(gap);
                if(continuity_case==1||continuity_case>=4) {
                    ProjectionSnapshot snapshot;check(capture_projection_snapshot(&missing->info,&snapshot),"gap snapshot");
                    state->previous_projection=snapshot;
                    // This is the actual temporary native-stereo-unavailable
                    // bypass boundary observed before each live reallocation.
                    clear_bypassed_generation_continuity(state);
                    check(state->independent_aer_double==stage_owner,"temporary stereo bypass destroyed the 4X resource owner");
                } else if(continuity_case==2) {
                    missing->interface_stamp.aer_mixed=false;
                    check(stage.process(state,missing,mappings,mid)==S_FALSE&&!mid,"nonmixed input must bypass interpolation");
                } else {
                    auto changed=endpoint(gap);changed->projections[0].layer.space=reinterpret_cast<XrSpace>(10);
                    // Recenter/reference space changes temporal compatibility,
                    // but leave source handles, dimensions and formats intact.
                    check(stage.process(state,changed,mappings,mid)==S_FALSE&&!mid,"space change must re-prime instead of interpolate");
                }
                for(unsigned eye=0;eye<2;++eye)
                    check(stage.channels.size()==2&&stage.channels[eye]->generator==generators[eye]&&
                        stage.channels[eye]->input==buffers[eye],"temporal gap reallocated compatible 4X channels");
                const auto prime_n=gap+1;
                check(stage.process(state,endpoint(prime_n),mappings,mid)==S_FALSE&&!mid,"first recovered endpoint must prime, never bridge a gap");
                ok(stage.process(state,endpoint(prime_n+1),mappings,mid),"recovered adjacent 4X pair");
                check(bool(mid),"4X pair recovers after gap");
                check(std::abs(mid->projections[0].views[0].pose.position.x-(.02f*(prime_n+.5f)-.032f))<.00001f,
                    "4X paired across the stale camera rather than the new prime");
                check(pixel(*mid,0)<130&&pixel(*mid,1)>180,"recovered eyes crossed histories");
                check(pixel(*held,0)==old_left&&pixel(*held,1)==old_right,"re-prime overwrote a leased pre-gap midpoint");
                mid.reset();
            }
            held.reset();
            if(continuity_case==6) {
                auto resized=ci;resized.width=resized.height=96;
                for(unsigned eye=0;eye<2;++eye) {
                    xr_ok(f.transport->create_swapchain(&resized,&sources[2+eye],D3D12_RESOURCE_STATE_RENDER_TARGET,1),"resized AER4 source backing");
                    auto chain=std::make_shared<SwapchainState>(state,resized);chain->handle=sources[2+eye];
                    {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
                    f.write(sources[2+eye],eye?200:60);mappings[eye].application_swapchain=sources[2+eye];
                }
                auto changed=endpoint(12);
                for(unsigned eye=0;eye<2;++eye)changed->projections[0].views[eye].subImage.swapchain=sources[2+eye];
                check(stage.process(state,changed,mappings,mid)==S_FALSE&&!mid,"resized backing must start a fresh epoch");
                for(unsigned eye=0;eye<2;++eye)check(stage.channels[eye]->generator!=generators[eye]&&stage.channels[eye]->description.Width==96,
                    "resource reuse retained the wrong backing dimensions");
            }
            if(continuity_case==4)stage.failure=E_FAIL;
            if(continuity_case==5)state->menu_enabled=false;
            state->generation_continuity_reset_pending=continuity_case!=4&&continuity_case!=5;
            state->previous_projection=ProjectionSnapshot{};
            clear_bypassed_generation_continuity(state);
            check(!state->independent_aer_double,"structural/disabled/failed boundary must release the retained 4X owner");
            std::printf("AER4_RESOURCE_REUSE case=%u compatible_channels=2 soft_gaps=3 stale_pairs=0 retained_pixels=PASS structural_release=PASS\n",continuity_case);
        } else {
        if(hold_reader) {
            f.expected_views=2;
            ok(stage.process(state,endpoint(2),mappings,mid),"reader-gate midpoint");check(bool(mid),"reader-gate output exists");
            const auto original_handle=mid->projections[0].views[0].subImage.swapchain;
            std::promise<void> release_reader;auto released=release_reader.get_future().share();
            std::atomic_bool reading{};auto copied=std::make_shared<std::atomic_bool>();
            f.preparation_callback=[&](std::uint64_t){reading=true;released.wait();};
            f.prepared_callback=[copied](std::uint64_t){copied->store(true);};
            f.begin_virtual();xr_ok(f.transport->end_frame(&mid->info),"native reader retains the generated source image");
            const auto limit=GetTickCount64()+2000;while(!reading && GetTickCount64()<limit)Sleep(1);
            check(reading,"real consumer reached the protected read");
            mid.reset(); // descriptor is gone; actual native source lease is still held
            check(!stage.channels[0]->slots[0]->busy,"descriptor credit alone cannot certify native reader completion");
            auto next=endpoint(3);
            auto pending=std::async(std::launch::async,[&]{return stage.process(state,next,mappings,mid);});
            const bool independent=pending.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
            release_reader.set_value(); // always rescue old code before reporting failure
            const auto hr=pending.get();ok(hr,"next stereo transaction after native reader gate");
            const auto drained=GetTickCount64()+2000;while(!copied->load() && GetTickCount64()<drained)Sleep(1);
            check(copied->load(),"held native read completed before fixture callback cleanup");
            f.preparation_callback={};
            const bool different=mid && mid->projections[0].views[0].subImage.swapchain!=original_handle;
            std::printf("AER4_NATIVE_READER producer_free=%u different_output=%u native12=%u\n",independent,different,native12);std::fflush(stdout);
            check(independent && different,"AER4 must use another idle output instead of waiting for a native reader");
            check(pixel(*mid,0)<55 && pixel(*mid,1)>185,"alternative output preserves both eye pixels");
            held=mid;mid.reset();
        }
        for(unsigned n=hold_reader?4:2;n<=9;++n) {
            if(hold_publication && n==2) {
                auto input=endpoint(n);ComPtr<ID3D12Fence> gate;ComPtr<ID3D11Fence> gate11;
                ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&gate)),"pending publication gate");
                HANDLE shared{};ok(f.device12->CreateSharedHandle(gate.Get(),nullptr,GENERIC_ALL,nullptr,&shared),"publication gate handle");
                ComPtr<ID3D11Device5> device;ok(f.device11.As(&device),"publication gate device");
                const auto opened=device->OpenSharedFence(shared,IID_PPV_ARGS(&gate11));CloseHandle(shared);ok(opened,"publication gate view");
                ComPtr<ID3D11DeviceContext4> context;ok(f.context.As(&context),"publication gate context");
                std::atomic_bool held{};
                stage.before_publication_for_test=[&]{if(!held.exchange(true)){
                    ok(context->Wait(gate11.Get(),1),"delay D11 publication only after generation");context->Flush();}};
                auto pending=std::async(std::launch::async,[&]{return stage.process(state,input,mappings,mid);});
                const auto released=pending.wait_for(std::chrono::milliseconds(1500))==std::future_status::ready;
                if(!released)ok(gate->Signal(1),"release old blocking code before fixture failure");
                const auto result=pending.get();
                bool receipt_pending=false;
                if(released && mid)for(const auto& proof:mid->reference_readiness)
                    receipt_pending|=proof.fence->GetCompletedValue()<proof.value;
                ok(gate->Signal(1),"release queued D11 publications");
                stage.before_publication_for_test={};
                if(mid)for(const auto& proof:mid->reference_readiness)ok(IndependentAerDouble::wait(proof.fence.Get(),proof.value),"complete exact queued publication before pixel read");
                std::printf("AER4_PUBLICATION producer_free=%d pending_exact_receipt=%d\n",int(released),int(receipt_pending));
                check(held && released && receipt_pending,"producer must return the exact pending publication proof without CPU waiting");
                ok(result,"queued second-stage publication");
            }else if(hold_gpu && n==2) {
                auto input=endpoint(n);ComPtr<ID3D12Fence> gate;
                ok(f.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"second-stage delayed GPU gate");
                ok(stage.queue->Wait(gate.Get(),1),"hold both eye copies behind exact GPU dependency");
                stage.submitted_for_test.store(0);
                auto pending=std::async(std::launch::async,[&]{return stage.process(state,input,mappings,mid);});
                const auto limit=GetTickCount64()+1500;
                while(stage.submitted_for_test.load(std::memory_order_acquire)<2 && GetTickCount64()<limit)Sleep(1);
                const auto queued=stage.submitted_for_test.load(std::memory_order_acquire);
                const auto incomplete=pending.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready;
                ok(gate->Signal(1),"release software fixture GPU dependency before any failure exit");
                const auto hr=pending.get();
                std::printf("AER4_BATCH queued_before_gpu=%zu unpublished=%d\n",queued,int(incomplete));
                check(queued==2 && incomplete,"both eye generators must queue before GPU completion and publish only afterward");
                ok(hr,"delayed second-stage completion");
            }else ok(stage.process(state,endpoint(n),mappings,mid),"actual second-stage generation");
            check(bool(mid),"stereo midpoint exists after warmup");
            const auto& left=mid->projections[0].views[0];const auto& right=mid->projections[0].views[1];
            check(std::abs(left.pose.position.x-(.02f*(n-.5f)-.032f))<.00001f &&
                std::abs(right.pose.position.x-(.02f*(n-.5f)+.032f))<.00001f,
                "both generated eye poses must lie at their own midpoint without eye swap");
            check(mid->synthetic && mid->interface_stamp.aer_generated_pair && !mid->interface_stamp.aer_mixed,
                "full generated stereo must not count as a half-real AER endpoint");
            const auto actual_left=pixel(*mid,0),actual_right=pixel(*mid,1);
            check(actual_left<100 && actual_right>170,"second-stage pixels crossed left/right histories");
            if(unmarked) {
                std::printf("AER4_TRANSFER n=%u pixels=%u,%u expected=%u,%u\n",n,actual_left,actual_right,40+n*4-2,180+n*4-2);
                check(std::abs(int(actual_left)-int(40+n*4-2))<=1 &&
                    std::abs(int(actual_right)-int(180+n*4-2))<=1,
                    "interop must capture the exact changing endpoint, not the originally registered bank");
            }
            if(n==2)held=mid;
            if(retain_many)retained.push_back(mid);
            mid.reset();
        }
        const auto held_left=pixel(*held,0),held_right=pixel(*held,1);
        check(held_left<55 && held_right>170 && held_right<195,"output ring overwrote a still-owned old generated pair");
        if(retain_many) {
            check(retained.size()==8,"eight completed stereo midpoints can coexist within independent output credits");
            for(const auto& channel:stage.channels)
                check(std::all_of(channel->slots.begin(),channel->slots.end(),[](const auto& slot){return bool(slot);}),
                    "retained frames must grow both eye banks to full capacity");
            check(pixel(*retained.front(),0)<55 && pixel(*retained.back(),0)>55,"retained midpoints preserve distinct immutable contents");
            const auto before=GetTickCount64();
            check(stage.process(state,endpoint(10),mappings,mid)==S_FALSE && !mid,
                "full output credits must skip an optional midpoint without blocking the game");
            check(GetTickCount64()-before<1000,"full native output credits caused a producer wait");
            retained.clear();
            ok(stage.process(state,endpoint(11),mappings,mid),"generation resumes when output credits return");
            check(bool(mid) && std::abs(mid->projections[0].views[0].pose.position.x-(.2f-.032f))<.00001f,
                "credit recovery must preserve the last actual camera/history anchor");
            mid.reset();
            std::puts("AER4_CREDITS retained=8 saturated_skip=1 recovered=1");
        }
        for(const auto& c:stage.channels) {
            unsigned busy{};for(const auto& s:c->slots)if(s)busy+=s->busy.load();
            check(busy==1,"only the deliberately retained pair may hold an output credit");
        }
        held.reset();
        for(const auto& c:stage.channels)for(const auto& s:c->slots)
            check(!s || !s->busy,"retired generated output leaked its ring credit");
        // A real configuration change starts a fresh temporal epoch. A/B alone
        // does not advance this epoch and is covered by the live route test.
        ++state->djules_configuration_epoch;
        check(stage.process(state,endpoint(10),mappings,mid)==S_FALSE && !mid,"mode switch must not interpolate across old history");
        auto device=state->d3d12_device;state->d3d12_device.Reset();
        check(FAILED(stage.process(state,endpoint(11),mappings,mid)),"unsupported stage must fail explicitly");
        state->d3d12_device=device;const auto ordinal=stage.ordinal;
        check(FAILED(stage.process(state,endpoint(12),mappings,mid)) && stage.ordinal==ordinal,"stage failure must latch instead of reallocating every frame");
        ++state->djules_configuration_epoch;
        check(stage.process(state,endpoint(13),mappings,mid)==S_FALSE && !mid,"configuration change can retry and prime the failed stage");
        ok(stage.process(state,endpoint(14),mappings,mid),"generation recovers after deliberate retry");check(bool(mid),"recovered stage creates a genuine pair");
        mid.reset();
        }
        state->independent_aer_double.reset();
    }
    for(auto source:sources) {
        if(!source)continue;
        {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(source);}
        xr_ok(f.transport->destroy_swapchain(source),"coherent source teardown");
    }
    f.transport->stop();independent_fixture_mode=false;
    check(f.chains.empty(),"AER4 stage leaked physical publication sinks");
    if(alias_owner)std::puts("AER4_ALIAS direct_backing=1 exact_eye_pixels=1 retained_leases=1");
    std::puts("AER4_STAGE exact per-eye midpoint poses/pixels, priming, retained leases, retry epoch and teardown PASS");return 0;
}
