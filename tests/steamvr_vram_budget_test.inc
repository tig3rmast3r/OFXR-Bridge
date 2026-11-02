// Real shipping layer allocations and control lifetime; only the downstream
// runtime is simulated. Three native images model SteamVR's allocation cost.
// The observer never holds a texture alive and does not intercept allocations.
#include "xrfg/owned_d11_output.hpp"
namespace steamvr_vram_test {
constexpr GUID observation_id{0x548b0d43,0xe4ce,0x4bc1,{0x90,0xb2,0x70,0x69,0x94,0x0e,0x7b,0x42}};
enum class Kind { native, application, generated, interop, unused_history };
struct Observation final : IUnknown {
    std::atomic<ULONG> references{1};
    std::atomic_bool alive{};
    IUnknown* identity{}; // weak, valid only while alive
    Kind kind{};
    UINT64 bytes{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining=--references;
        if(remaining==1)alive.store(false,std::memory_order_release);
        if(!remaining)delete this;return remaining;
    }
};
struct Ledger {
    ID3D12Device* device{};
    std::vector<ComPtr<Observation>> observations;
    explicit Ledger(ID3D12Device* value):device(value){}
    template<class Texture> void observe(Texture* texture,Kind kind,const D3D12_RESOURCE_DESC& description) {
        if(!texture)return;
        ComPtr<IUnknown> identity;ok(texture->QueryInterface(IID_PPV_ARGS(&identity)),"VRAM texture identity");
        for(const auto& existing:observations)
            if(existing->alive.load(std::memory_order_acquire) && existing->identity==identity.Get())return;
        ComPtr<Observation> item;item.Attach(new Observation);
        item->identity=identity.Get();item->kind=kind;
        item->bytes=device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
        check(item->bytes && item->bytes!=UINT64_MAX,"VRAM actual resource allocation footprint");
        ok(texture->SetPrivateDataInterface(observation_id,item.Get()),"non-owning VRAM lifetime observation");
        item->alive.store(true,std::memory_order_release);observations.push_back(std::move(item));
    }
    void observe(ID3D12Resource* texture,Kind kind) {if(texture)observe(texture,kind,texture->GetDesc());}
    void observe(ID3D11Texture2D* texture,Kind kind) {
        if(!texture)return;
        // An OFXR-owned D11 view and its interop D12 resource are one physical
        // allocation. Observe the proven backing identity only, without pinning
        // either texture in this ledger after the call returns.
        if(auto backing=xrfg::owned_d11_output::backing_for(texture,device)) {
            observe(backing.Get(),kind);return;
        }
        D3D11_TEXTURE2D_DESC source{};texture->GetDesc(&source);
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width=source.Width;description.Height=source.Height;
        description.DepthOrArraySize=static_cast<UINT16>(source.ArraySize);description.MipLevels=static_cast<UINT16>(source.MipLevels);
        description.Format=source.Format;description.SampleDesc=source.SampleDesc;
        if(source.BindFlags&D3D11_BIND_RENDER_TARGET)description.Flags|=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if(source.BindFlags&D3D11_BIND_UNORDERED_ACCESS)description.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        // D11 uses the real texture descriptor with the same adapter's D12
        // allocation footprint. DXGI below independently measures residency;
        // this is not represented as a D11 driver's exact heap allocation.
        observe(texture,kind,description);
    }
    UINT64 bytes(Kind kind) const {
        UINT64 total{};for(const auto& item:observations)
            if(item->kind==kind && item->alive.load(std::memory_order_acquire))total+=item->bytes;
        return total;
    }
    unsigned count(Kind kind) const {
        unsigned total{};for(const auto& item:observations)
            if(item->kind==kind && item->alive.load(std::memory_order_acquire))++total;
        return total;
    }
};
XrResult XRAPI_CALL create_three(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* output) {
    auto& fixture=*current;
    if(fixture.chains.size()>=fixture.native_chain_limit)return XR_ERROR_LIMIT_REACHED;
    *output=reinterpret_cast<XrSwapchain>(++fixture.next_handle);auto& chain=fixture.chains[*output];
    chain.state=fixture.create_state?fixture.create_state():D3D12_RESOURCE_STATE_COMMON;
    for(unsigned image=0;image<3;++image)if(fixture.native12) {
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width=info->width;description.Height=info->height;
        description.DepthOrArraySize=static_cast<UINT16>(info->arraySize);description.MipLevels=1;
        description.Format=static_cast<DXGI_FORMAT>(info->format);description.SampleDesc.Count=1;
        description.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        ok(fixture.device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,
            chain.state,nullptr,IID_PPV_ARGS(&texture)),"three-image native SteamVR texture");
        chain.d12.push_back(std::move(texture));
    } else {
        D3D11_TEXTURE2D_DESC description{};description.Width=info->width;description.Height=info->height;
        description.ArraySize=info->arraySize;description.MipLevels=1;
        description.Format=static_cast<DXGI_FORMAT>(info->format);description.SampleDesc.Count=1;
        description.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        ok(fixture.device11->CreateTexture2D(&description,nullptr,&texture),"three-image native SteamVR D11 texture");
        chain.d11.push_back(std::move(texture));
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerate_three(XrSwapchain handle,std::uint32_t capacity,
    std::uint32_t* count,XrSwapchainImageBaseHeader* images) {
    if(!count)return XR_ERROR_VALIDATION_FAILURE;*count=3;if(!capacity)return XR_SUCCESS;
    if(capacity<3)return XR_ERROR_SIZE_INSUFFICIENT;if(!images)return XR_ERROR_VALIDATION_FAILURE;
    auto& fixture=*current;auto& chain=fixture.chains.at(handle);
    for(unsigned index=0;index<3;++index)if(fixture.native12)
        reinterpret_cast<XrSwapchainImageD3D12KHR*>(images)[index].texture=chain.d12[index].Get();
    else reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[index].texture=chain.d11[index].Get();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire_three(XrSwapchain handle,const XrSwapchainImageAcquireInfo*,std::uint32_t* index) {
    auto& fixture=*current;auto& chain=fixture.chains.at(handle);
    check(!chain.owned,"native three-image acquire ownership");
    chain.acquired=(chain.released+1)%3;chain.owned=true;chain.waited=false;
    ++chain.acquisitions;++fixture.real_acquires;*index=chain.acquired;return XR_SUCCESS;
}
void observe_resources(Fixture& fixture,const std::shared_ptr<SessionState>& state,
    std::span<const XrSwapchain> applications,Ledger& ledger) {
    for(const auto& [handle,chain]:fixture.chains) {
        for(const auto& texture:chain.d12)ledger.observe(texture.Get(),Kind::native);
        for(const auto& texture:chain.d11)ledger.observe(texture.Get(),Kind::native);
    }
    auto mapping=state->private_runtime->native_swapchain_mapping();
    // The production inventory deliberately uses a try-lock. A runtime clock
    // waiter may briefly own it even after all content/GPU work has drained.
    // This retry belongs to the laboratory sampler, never the delivery path.
    const auto deadline=std::chrono::steady_clock::now()+100ms;
    while(!mapping && std::chrono::steady_clock::now()<deadline) {
        std::this_thread::sleep_for(1ms);
        mapping=state->private_runtime->native_swapchain_mapping();
    }
    check(mapping.has_value(),"drained transport supplies complete memory inventory");
    for(const auto& entry:*mapping) {
        const auto kind=std::find(applications.begin(),applications.end(),entry.handle)!=applications.end()
            ?Kind::application:Kind::generated;
        unsigned count{};xr_ok(state->private_runtime->enumerate_images(entry.handle,0,&count,nullptr),"private texture count");
        if(fixture.native12) {
            std::vector<XrSwapchainImageD3D12KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
            xr_ok(state->private_runtime->enumerate_images(entry.handle,count,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"private D12 textures");
            for(const auto& image:images)ledger.observe(image.texture,kind);
        } else {
            std::vector<XrSwapchainImageD3D11KHR> images(count,{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
            xr_ok(state->private_runtime->enumerate_images(entry.handle,count,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"private D11 textures");
            for(const auto& image:images)ledger.observe(image.texture,kind);
        }
    }
    for(const auto& chain:find_swapchains(state))if(chain->frame_generation && chain->frame_generation->d3d11_interop) {
        const auto& interop=chain->frame_generation->d3d11_interop;
        for(auto* texture:interop->source_images())ledger.observe(texture,Kind::interop);
        for(auto* texture:interop->current_destination_images())ledger.observe(texture,Kind::interop);
        for(auto* texture:interop->synthetic_destination_images())ledger.observe(texture,Kind::interop);
    }
}
UINT64 residency(IDXGIAdapter3* adapter) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    ok(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&info),"independent process VRAM query");
    return info.CurrentUsage;
}
} // namespace steamvr_vram_test

int steamvr_vram_budget_test(int argc,char** argv) {
    using namespace steamvr_vram_test;
    std::fprintf(stderr,"VRAM checkpoint: test entry\n");std::fflush(stderr);
    const std::string shape=argc>2?argv[2]:"native-split";
    const unsigned extent=argc>3?unsigned(std::stoul(argv[3])):2048;
    const bool hdr=argc>4 && std::string_view(argv[4])=="rgba16f";
    const bool luke=shape.starts_with("luke-");
    const bool split=shape.ends_with("split"),array=shape.ends_with("array"),packed=shape.ends_with("packed");
    check((split || array || packed) && (!luke || split),"VRAM shape: native split/array/packed or Luke D11 split");
    check(extent>=128 && extent<=4096,"VRAM test eye extent 128..4096");
    xrfg_fixture::ControlProfile profile;
    auto desired=xrfg::autonomous::snapshot().desired;
    desired.enabled=false;desired.afw_mode=0;desired.pacing_mode=0;
    desired.backend=0;desired.frame_generation=0;desired.motion_vectors=false;
    desired.lower_vram=0;desired.separate_synthesis=0;desired.triple_frame_gen=false;desired.prefer_fps=false;
    check(xrfg::autonomous::request(desired),"VRAM OFF startup control");xrfg::autonomous::apply_runtime_settings();
    std::fprintf(stderr,"VRAM checkpoint: controls applied, create device\n");std::fflush(stderr);
    Fixture fixture(!luke);fixture.transport->stop();fixture.transport.reset();
    std::fprintf(stderr,"VRAM checkpoint: device ready\n");std::fflush(stderr);
    fixture.real_common=true;fixture.throttled=false;fixture.unblock();
    if(luke) {
        ok(xrfg::create_d3d12_device_for_d3d11(fixture.device11.Get(),
            fixture.device12.GetAddressOf(),fixture.queue.GetAddressOf()),"VRAM D11 canonical interop owner");
        // Fixture(false) starts as a D11 runtime and has no D12 proof objects.
        // The production interop creates its own fences; our independent wait
        // helper needs a real fence on the subsequently created D12 owner too.
        ok(fixture.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fixture.fence)),"VRAM independent D11 interop proof fence");
        ok(fixture.device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&fixture.gate)),"VRAM independent D11 interop gate");
    }
    auto dispatch=std::make_shared<Dispatch>();dispatch->instance=reinterpret_cast<XrInstance>(1);dispatch->steamvr_runtime=true;
    dispatch->wait_frame=Fixture::wait;dispatch->begin_frame=Fixture::begin;dispatch->end_frame=Fixture::end;
    dispatch->create_swapchain=create_three;dispatch->destroy_swapchain=Fixture::destroy;dispatch->enumerate_swapchain_images=enumerate_three;
    dispatch->acquire_swapchain_image=acquire_three;dispatch->wait_swapchain_image=Fixture::wait_image;
    dispatch->release_swapchain_image=Fixture::release;dispatch->convert_time_to_qpc=Fixture::to_qpc;
    auto state=std::make_shared<SessionState>(dispatch);state->handle=reinterpret_cast<XrSession>(2);
    state->vdxr_legacy=false;
    state->graphics_binding=luke?SessionGraphicsBinding::d3d11:SessionGraphicsBinding::d3d12;
    state->d3d12_device=fixture.device12;state->d3d12_queue=fixture.queue;
    state->d3d11_device=fixture.device11;state->d3d11_context=fixture.context;
    state->private_delivery_queue=fixture.delivery_queue;
    state->luke_afw_enabled=luke;state->luke_shared_swapchains=true;
    state->graphics_binding_capabilities=luke?11ULL:8ULL;
    state->uevr_application=false; // ordinary SteamVR must meet the same envelope
    D3D12_COMMAND_QUEUE_DESC queue_description{};queue_description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!luke)ok(fixture.device12->CreateCommandQueue(&queue_description,
        IID_PPV_ARGS(&state->d3d12_synthesis_queue)),"VRAM isolated synthesis queue");
    {std::scoped_lock lock(g_state_mutex);g_instances[dispatch->instance]=dispatch;g_sessions[state->handle]=state;}
    std::fprintf(stderr,"VRAM checkpoint: apply startup OFF\n");std::fflush(stderr);
    ok(apply_embedded_control(state),"VRAM apply startup OFF");
    ok(install_private_runtime(state),"VRAM private SteamVR endpoint");
    check(bool(state->private_runtime),"VRAM fixture must exercise the private SteamVR endpoint");
    fixture.transport=state->private_runtime;
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter3> adapter;
    ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"VRAM adapter factory");
    ok(factory->EnumAdapterByLuid(fixture.device12->GetAdapterLuid(),IID_PPV_ARGS(&adapter)),"VRAM actual adapter");
    Ledger ledger(fixture.device12.Get());
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.width=packed?extent*2:extent;info.height=extent;info.arraySize=array?2:1;
    info.faceCount=info.mipCount=info.sampleCount=1;
    info.format=hdr?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;
    info.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    std::vector<XrSwapchain> applications(split?3:2);
    std::fprintf(stderr,"VRAM checkpoint: create application images\n");std::fflush(stderr);
    for(auto& handle:applications) {
        xr_ok(layer_create_swapchain_impl(state->handle,&info,&handle),"VRAM application chain");
        unsigned count{};
        if(luke) {
            std::array<XrSwapchainImageD3D11KHR,3> images{};for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(handle,3,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"VRAM full app D11 enumeration");
        } else {
            std::array<XrSwapchainImageD3D12KHR,3> images{};for(auto& image:images)image={XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            xr_ok(layer_enumerate_swapchain_images_impl(handle,3,&count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),"VRAM full app D12 enumeration");
        }
        check(count==3,"three private app images");
    }
    observe_resources(fixture,state,applications,ledger);
    const auto baseline_native=ledger.bytes(Kind::native),baseline_app=ledger.bytes(Kind::application);
    const auto baseline_residency=residency(adapter.Get());
    std::fprintf(stderr,"VRAM checkpoint: OFF baseline collected\n");std::fflush(stderr);
    std::printf("VRAM phase=off-baseline shape=%s eye=%u format=%s native_bytes=%llu native_textures=%u application_private_bytes=%llu process_bytes=%llu\n",
        shape.c_str(),extent,hdr?"rgba16f":"rgba8",baseline_native,ledger.count(Kind::native),baseline_app,baseline_residency);std::fflush(stdout);
    check(ledger.count(Kind::native)==applications.size()*3 && !ledger.bytes(Kind::generated) && !ledger.bytes(Kind::interop),
        "OFF/enumeration spent VRAM on unused generation destinations");
    for(const auto& chain:find_swapchains(state))check(!chain->frame_generation,"OFF allocated an unused generator context");
    std::array<XrCompositionLayerProjectionView,2> views{};
    for(unsigned eye=0;eye<2;++eye) {
        auto& view=views[eye];view.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        view.pose.orientation.w=1;view.pose.position.x=eye?.032f:-.032f;
        view.fov={-.7f,.7f,.7f,-.7f};view.subImage.swapchain=applications[split?eye:0];
        view.subImage.imageArrayIndex=array?eye:0;
        view.subImage.imageRect.offset.x=packed?int(eye*extent):0;
        view.subImage.imageRect.extent={int(extent),int(extent)};
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);
    projection.viewCount=2;projection.views=views.data();
    const auto* header=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.layerCount=1;end.layers=&header;
    end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ProjectionSnapshot snapshot{};check(capture_projection_snapshot(&end,&snapshot),"VRAM valid stereo projection");
    const auto mapping=build_projection_resource_mappings(snapshot);check(mapping.ready(),"VRAM stereo resource domains");
    const unsigned active_sources=split?2:1;
    const auto source_bytes=baseline_native/(applications.size()*3);
    const auto physical_budget=baseline_native+active_sources*3*source_bytes;
    UINT64 peak_residency=baseline_residency,peak_native=baseline_native,peak_private=baseline_app;
    std::array<xrfg::D3D12HistoryCaptureTicket,xrfg::D3D12SwapchainHistory::kSlotCount> unused_captures{};
    std::vector<std::uintptr_t> unused_application_identities;
    for(unsigned cycle=0;cycle<3;++cycle) {
        std::fprintf(stderr,"VRAM checkpoint: enable cycle %u\n",cycle);std::fflush(stderr);
        desired.enabled=true;desired.pacing_mode=cycle%2;desired.increase_buffer=cycle==2;
        check(xrfg::autonomous::request(desired),"VRAM enable route/pacing revision");xrfg::autonomous::apply_runtime_settings();
        ok(apply_embedded_control(state,false,&end),"VRAM apply enabled route");
        if(!luke)note_projection_eyes(snapshot,mapping);
        if(!luke && cycle==0) {
            // Model a loading/UI source which was released before its final
            // role became known. It never had an interpolation generator, but
            // all three real history slots can still hold full-size colors.
            // No observer keeps these private snapshots alive after this scope.
            const auto unused=find_swapchain(applications.back());
            check(unused && unused->d3d12_history && !unused->frame_generation,
                "unused source history exists independently of a generator");
            for(const auto& image:unused->enumerated_d3d12_images) {
                ComPtr<IUnknown> identity;ok(image.As(&identity),"unused application texture identity");
                unused_application_identities.push_back(reinterpret_cast<std::uintptr_t>(identity.Get()));
            }
            for(unsigned slot=0;slot<unused_captures.size();++slot) {
                auto& ticket=unused_captures[slot];
                ok(unused->d3d12_history->capture(slot,&ticket),"warm actual unused history capture");
                ok(unused->d3d12_history->commit(ticket),"commit actual unused history capture");
                xrfg::D3D12HistoryConsumerLease lease{};ComPtr<ID3D12Resource> resource;ComPtr<ID3D12Fence> writer;
                ok(unused->d3d12_history->acquire_consumer(ticket,&lease,&resource,&writer),"observe committed unused history slot");
                check(ticket.serial && ticket.fence_value && writer && resource,"unused history ticket has actual backing and writer proof");
                ledger.observe(resource.Get(),Kind::unused_history);
                unused->d3d12_history->cancel_consumer(lease);
            }
            ok(unused->d3d12_history->wait_for_idle(),"unused history writer completion");
            unused->last_released_capture=unused_captures.back();
            check(ledger.count(Kind::unused_history)==unused_captures.size() &&
                ledger.bytes(Kind::unused_history)==unused_captures.size()*source_bytes,
                "unused-source precondition must own three full-size GPU history allocations");
            std::printf("VRAM phase=unused-history-warmed shape=%s eye=%u format=%s unused_history_bytes=%llu unused_history_textures=%u\n",
                shape.c_str(),extent,hdr?"rgba16f":"rgba8",ledger.bytes(Kind::unused_history),ledger.count(Kind::unused_history));std::fflush(stdout);
        }
        check(ensure_projection_frame_generation(state,mapping.mappings),"VRAM arm actual projection outputs");
        if(!luke) {
            const auto unused=find_swapchain(applications.back());
            check(!ledger.bytes(Kind::unused_history) && !ledger.count(Kind::unused_history),
                "projection admission retained three histories of an unused source with no generator");
            check(unused && unused->d3d12_history && !unused->last_released_capture &&
                !unused->projection_history_needed.load(),"unused source did not retire capture admission/ticket while retaining its metadata");
            for(unsigned source=0;source<active_sources;++source)
                check(find_swapchain(applications[source])->projection_history_needed.load(),"actual projected source lost history admission");
            for(const auto& ticket:unused_captures) {
                xrfg::D3D12HistoryConsumerLease lease{};ComPtr<ID3D12Resource> resource;ComPtr<ID3D12Fence> writer;
                const auto resolved=unused->d3d12_history->acquire_consumer(ticket,&lease,&resource,&writer);
                if(SUCCEEDED(resolved))unused->d3d12_history->cancel_consumer(lease);
                check(FAILED(resolved) && !resource,"retired unused-source ticket still exposes GPU snapshot backing");
            }
            // HDR cases deliberately exercise renderer refusal/rollback, not
            // an active frame generator. Avoid adding a separate auxiliary
            // capture allocation to that rollback residency measurement.
            if(!hdr) {
            const auto writer_before=unused->d3d12_history->diagnostic_completed_value();
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wait.timeout=XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};unsigned index{};
            xr_ok(layer_acquire_swapchain_image_impl(unused->handle,&acquire,&index),"unused source actual XR acquire");
            xr_ok(layer_wait_swapchain_image_impl(unused->handle,&wait),"unused source actual XR wait");
            xr_ok(layer_release_swapchain_image_impl(unused->handle,&release),"unused source actual XR release while FG remains ON");
            fixture.gpu_wait();
            if(unused->last_released_capture) {
                xrfg::D3D12HistoryConsumerLease lease{};ComPtr<ID3D12Resource> resource;ComPtr<ID3D12Fence> writer;
                if(SUCCEEDED(unused->d3d12_history->acquire_consumer(*unused->last_released_capture,&lease,&resource,&writer))) {
                    ledger.observe(resource.Get(),Kind::unused_history);unused->d3d12_history->cancel_consumer(lease);
                }
            }
            check(!ledger.bytes(Kind::unused_history) && !unused->last_released_capture &&
                unused->d3d12_history->diagnostic_completed_value()==writer_before,
                "releasing an unused source while FG is ON reallocated/submitted hidden history snapshots");
            }
            check(unused->enumerated_d3d12_images.size()==unused_application_identities.size(),"unused app images changed during history retirement");
            for(unsigned image=0;image<unused_application_identities.size();++image) {
                ComPtr<IUnknown> identity;ok(unused->enumerated_d3d12_images[image].As(&identity),"retained app texture identity");
                check(reinterpret_cast<std::uintptr_t>(identity.Get())==unused_application_identities[image],
                    "history retirement replaced or destroyed the application's actual texture");
            }
        }
        for(unsigned source=0;source<active_sources;++source) {
            const auto chain=find_swapchain(applications[source]);
            if(!chain->frame_generation) {
                std::fprintf(stderr,"VRAM initialization refusal: expected_unsupported_hdr=%u source=%u reason=%lld detail=%016llx pending=%u declined=%u history=%u menu=%u capabilities=%llu session_hresult=%08x\n",
                    unsigned(hdr),source,static_cast<long long>(chain->generation_refusal),chain->generation_refusal_detail,
                    unsigned(chain->generation_eligible_pending),unsigned(chain->generation_declined),
                    unsigned(bool(chain->d3d12_history)),unsigned(state->menu_enabled.load()),
                    state->graphics_binding_capabilities,unsigned(state->generation_initialization_result.load()));std::fflush(stderr);
            }
            if(hdr) {
                // The established renderer admits UNORM/SRGB color views.
                // Test HDR's transactional memory rollback without granting a
                // format the shipping compositor does not support.
                check(!chain->frame_generation && chain->generation_declined &&
                    chain->generation_refusal==SwapchainEligibilityReason::synthesis_initialize_failed &&
                    static_cast<HRESULT>(chain->generation_refusal_detail)==E_INVALIDARG,
                    "unsupported HDR did not retain its explicit renderer refusal/real-frame bypass");
                check(!luke || !chain->d3d12_history,"failed D11 HDR setup retained interop source history");
            } else check(bool(chain->frame_generation),"VRAM active projection failed initialization");
        }
        check(!find_swapchain(applications.back())->frame_generation,"unused same-size app source allocated generation outputs");
        observe_resources(fixture,state,applications,ledger);
        peak_native=std::max(peak_native,ledger.bytes(Kind::native));
        peak_private=std::max(peak_private,ledger.bytes(Kind::application)+ledger.bytes(Kind::generated)+ledger.bytes(Kind::interop));
        peak_residency=std::max(peak_residency,residency(adapter.Get()));
        std::printf("VRAM phase=%s shape=%s eye=%u format=%s cycle=%u native_bytes=%llu native_textures=%u private_output_bytes=%llu interop_bytes=%llu process_bytes=%llu\n",
            hdr?"unsupported-format-rollback":"on",shape.c_str(),extent,hdr?"rgba16f":"rgba8",cycle,ledger.bytes(Kind::native),ledger.count(Kind::native),
            ledger.bytes(Kind::generated),ledger.bytes(Kind::interop),residency(adapter.Get()));std::fflush(stdout);
        if(hdr) {
            check(ledger.bytes(Kind::native)==baseline_native && !ledger.bytes(Kind::generated) && !ledger.bytes(Kind::interop),
                "unsupported HDR setup retained generated color/interop allocations after rollback");
            fixture.gpu_wait();
            const auto deadline=std::chrono::steady_clock::now()+2s;
            while(residency(adapter.Get())>baseline_residency+(16ULL<<20) && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(5ms);
            check(residency(adapter.Get())<=baseline_residency+(16ULL<<20),
                "unsupported HDR left more than16MiB of process VRAM after its resource transaction rolled back");
        }
        // Each independent source domain needs one physical destination per
        // native image index. Private generations may queue independently;
        // extra physical copies cannot increase that simultaneous need.
        check(ledger.bytes(Kind::native)<=physical_budget,
            "SteamVR generation exceeds one physical three-image publication sink per active source domain");
        check(ledger.bytes(Kind::application)==baseline_app,"generation replaced or multiplied app private images");
        desired.enabled=false;
        check(xrfg::autonomous::request(desired),"VRAM disable generation");xrfg::autonomous::apply_runtime_settings();
        std::fprintf(stderr,"VRAM checkpoint: apply OFF cycle %u\n",cycle);std::fflush(stderr);
        ok(apply_embedded_control(state,false,&end),"VRAM drained OFF retirement");
        std::fprintf(stderr,"VRAM checkpoint: independent GPU proof cycle %u\n",cycle);std::fflush(stderr);
        fixture.gpu_wait();if(fixture.delivery_queue)fixture.gpu_wait(fixture.delivery_queue.Get());
        observe_resources(fixture,state,applications,ledger);
        check(ledger.bytes(Kind::native)==baseline_native && !ledger.bytes(Kind::generated) && !ledger.bytes(Kind::interop) && !ledger.bytes(Kind::unused_history),
            "OFF retained generated physical/private/interop full-color allocations after drained retirement");
        for(const auto& chain:find_swapchains(state))check(!chain->frame_generation,"OFF retained an unused generator context");
    }
    fixture.transport->stop();
    for(const auto handle:applications)xr_ok(layer_destroy_swapchain_impl(handle),"VRAM application teardown");
    check(fixture.chains.empty() && !ledger.bytes(Kind::native) && !ledger.bytes(Kind::application) && !ledger.bytes(Kind::generated) && !ledger.bytes(Kind::unused_history),
        "final GPU/resource retirement retained color allocations");
    state->private_runtime.reset();fixture.transport.reset();
    {std::scoped_lock lock(g_state_mutex);g_sessions.clear();g_instances.clear();g_swapchains.clear();}
    std::printf("VRAM PASS shape=%s eye=%u format=%s coverage=%s baseline_native_bytes=%llu baseline_private_bytes=%llu native_peak_bytes=%llu private_peak_bytes=%llu process_baseline_bytes=%llu process_peak_bytes=%llu; OFF/ON x3, unused source, array/rect domains, non-owning retirement\n",
        shape.c_str(),extent,hdr?"rgba16f":"rgba8",hdr?"renderer-refusal-and-rollback":"active-allocation-budget",baseline_native,baseline_app,peak_native,peak_private,baseline_residency,peak_residency);
    return 0;
}
