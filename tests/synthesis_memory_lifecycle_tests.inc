// Real WARP allocation/lifetime oracle. This observes backing bytes at the
// existing D3D12 factories; it never estimates VRAM from a count of COM refs.
struct SynthesisMemoryTotals {
    std::atomic_uint64_t live{},peak{},created{},maximum{UINT64_MAX};
    std::atomic_bool refused{};
};

class SynthesisMemoryDevice final : public ReportedDeviceAlias {
    static constexpr GUID marker_id{0x2b298d05,0x249a,0x4d39,{0xb9,0x0e,0x72,0x4c,0xa2,0xeb,0x5f,0x40}};
    struct Lifetime final : IUnknown {
        std::atomic<ULONG> references{1};
        std::shared_ptr<SynthesisMemoryTotals> totals;
        UINT64 bytes{};
        Lifetime(std::shared_ptr<SynthesisMemoryTotals> value,UINT64 size):totals(std::move(value)),bytes(size) {
            const auto live=totals->live.fetch_add(bytes)+bytes;
            auto peak=totals->peak.load();
            while(peak<live && !totals->peak.compare_exchange_weak(peak,live)) {}
            ++totals->created;
        }
        ~Lifetime(){totals->live-=bytes;}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
            if(!out)return E_POINTER;*out=nullptr;
            if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
            *out=static_cast<IUnknown*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
        ULONG STDMETHODCALLTYPE Release() override {const auto left=--references;if(!left)delete this;return left;}
    };
    std::atomic<ULONG> references_{1};
public:
    std::shared_ptr<SynthesisMemoryTotals> totals=std::make_shared<SynthesisMemoryTotals>();
    explicit SynthesisMemoryDevice(ID3D12Device* native):ReportedDeviceAlias(native){}
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {const auto left=--references_;if(!left)delete this;return left;}
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* heap,D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC* desc,D3D12_RESOURCE_STATES state,const D3D12_CLEAR_VALUE* clear,
        REFIID iid,void** output) override {
        const bool counted=heap && desc && heap->Type==D3D12_HEAP_TYPE_DEFAULT &&
            desc->Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        const UINT64 bytes=counted?GetResourceAllocationInfo(0,1,desc).SizeInBytes:0;
        if(counted && (bytes>totals->maximum || totals->live>totals->maximum-bytes)) {
            if(output)*output=nullptr;totals->refused=true;return E_OUTOFMEMORY;
        }
        const auto result=ReportedDeviceAlias::CreateCommittedResource(heap,flags,desc,state,clear,iid,output);
        if(FAILED(result) || !counted || !output || !*output)return result;
        ComPtr<ID3D12Object> resource;
        require_hresult(reinterpret_cast<IUnknown*>(*output)->QueryInterface(IID_PPV_ARGS(&resource)),"memory resource identity");
        auto* lifetime=new Lifetime(totals,bytes);
        const auto observed=resource->SetPrivateDataInterface(marker_id,lifetime);
        lifetime->Release();
        require_hresult(observed,"memory lifetime witness");
        return result;
    }
};

void test_history_memory_lifecycle(D3D12WarpFixture& fixture) {
    ComPtr<SynthesisMemoryDevice> device;device.Attach(new SynthesisMemoryDevice(fixture.device()));
    ComPtr<QueueWithReportedDeviceAlias> queue;
    queue.Attach(new QueueWithReportedDeviceAlias(fixture.queue(),device.Get()));
    auto source=create_source_texture(fixture,256,192);
    const auto pattern=fast_control_pattern(256,192,23,1);
    upload_pattern(fixture,source.Get(),pattern);
    ID3D12Resource* sources[]{source.Get()};
    xrfg::D3D12SwapchainHistory history;
    require_hresult(history.initialize(device.Get(),queue.Get(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"memory history initialize");
    require(history.initialized() && device->totals->created==0 && device->totals->live==0,
        "unused history reserved full-resolution snapshots at enumeration");
    const auto desc=source->GetDesc();
    const auto one=fixture.device()->GetResourceAllocationInfo(0,1,&desc).SizeInBytes;
    device->totals->maximum=0;
    xrfg::D3D12HistoryCaptureTicket refused{};
    require(history.capture(0,&refused)==E_OUTOFMEMORY && refused.serial==0 &&
        history.initialized() && device->totals->live==0,"lazy allocation failure poisoned source admission");
    device->totals->maximum=UINT64_MAX;
    xrfg::D3D12HistoryCaptureTicket previous{};
    for(unsigned cycle=0;cycle<4;++cycle) {
        xrfg::D3D12HistoryCaptureTicket captured{};
        require_hresult(history.capture(0,&captured),"lazy memory capture");
        require_hresult(history.commit(captured),"lazy memory commit");
        require(captured.serial>previous.serial && captured.fence_value>previous.fence_value,
            "history serial/fence reused after releasing snapshot backing");
        require(device->totals->live==one,"first capture allocated more than its one slot");
        xrfg::D3D12HistoryConsumerLease lease{};ComPtr<ID3D12Resource> color;ComPtr<ID3D12Fence> producer;
        require_hresult(history.acquire_consumer(captured,&lease,&color,&producer),"memory consumer");
        require(history.release_snapshots()==HRESULT_FROM_WIN32(ERROR_BUSY) && device->totals->live==one,
            "snapshot release freed an active CPU lease");
        require_hresult(producer->SetEventOnCompletion(captured.fence_value,nullptr),"memory producer completion");
        require_readback_matches(fixture,color.Get(),pattern,"lazy history exact stereo pixels",D3D12_RESOURCE_STATE_COMMON);
        ComPtr<ID3D12Fence> consumer;
        require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&consumer)),"memory consumer fence");
        require_hresult(history.retire_consumer(lease,consumer.Get(),1),"memory consumer retirement");
        color.Reset();producer.Reset();
        std::promise<void> entered;auto began=entered.get_future();
        auto releasing=std::async(std::launch::async,[&]{entered.set_value();return history.release_snapshots();});
        began.wait();
        const auto waiting=releasing.wait_for(std::chrono::milliseconds(30));
        const auto retained=device->totals->live.load();
        require_hresult(consumer->Signal(1),"memory consumer completion");
        require(waiting==std::future_status::timeout && retained==one,
            "snapshot release did not retain backing until the exact consumer fence");
        require_hresult(releasing.get(),"release drained snapshots");
        require(history.initialized() && device->totals->live==0,"disabled history retained texture backing");
        ComPtr<ID3D12Resource> stale;ComPtr<ID3D12Fence> stale_producer;xrfg::D3D12HistoryConsumerLease stale_lease{};
        require(FAILED(history.acquire_consumer(captured,&stale_lease,&stale,&stale_producer)),
            "released snapshot ticket still resolves");
        previous=captured;
    }
    for(unsigned slot=0;slot<xrfg::D3D12SwapchainHistory::kSlotCount;++slot) {
        xrfg::D3D12HistoryCaptureTicket captured{};
        require_hresult(history.capture(0,&captured),"warm memory ring capture");
        require_hresult(history.commit(captured),"warm memory ring commit");
        require(device->totals->live==(slot+1)*one,"history did not bound allocation to its actually captured slots");
    }
    require_hresult(history.release_snapshots(),"release fully allocated ring");
    require(device->totals->live==0 && device->totals->peak==3*one,"warm history release leaked/overallocated its three slots");
    std::cout<<"HISTORY_MEMORY first_slot_bytes="<<one<<" peak_bytes="<<device->totals->peak.load()
        <<" cycles=4 unused=0 disabled=0 exact_stereo=PASS producer_consumer_fences=PASS\n";
}

void test_synthesis_method_memory(D3D12WarpFixture& fixture) {
    ComPtr<SynthesisMemoryDevice> device;device.Attach(new SynthesisMemoryDevice(fixture.device()));
    ComPtr<QueueWithReportedDeviceAlias> queue;
    queue.Attach(new QueueWithReportedDeviceAlias(fixture.queue(),device.Get()));
    auto source=create_source_texture(fixture,256,192);
    auto current=create_source_texture(fixture,256,192),synthetic=create_source_texture(fixture,256,192);
    const auto pattern=fast_control_pattern(256,192,7,1);
    upload_pattern(fixture,source.Get(),pattern);
    ID3D12Resource* sources[]{source.Get()},*current_images[]{current.Get()},*synthetic_images[]{synthetic.Get()};
    auto history=std::make_shared<xrfg::D3D12SwapchainHistory>();
    require_hresult(history->initialize(fixture.device(),fixture.queue(),sources,D3D12_RESOURCE_STATE_RENDER_TARGET),"method memory history");
    require(device->totals->live==0,"method test requires cold history");
    for(auto backend:{xrfg::D3D12OpticalFlowBackend::fidelity_fx,xrfg::D3D12OpticalFlowBackend::nvidia}) {
        xrfg::D3D12FrameSynthesizer fsr;
        device->totals->maximum=0;device->totals->refused=false;
        require_hresult(fsr.initialize(device.Get(),queue.Get(),history,current_images,synthetic_images,kFormat,
            D3D12_RESOURCE_STATE_RENDER_TARGET,backend,{},false,xrfg::D3D12FrameGenerationMode::fsr3),"FSR without unused optical provider");
        require(device->totals->live==0 && !device->totals->refused,
            "FSR allocated an unused ordinary optical engine before any FSR dispatch");
        auto guides=std::make_shared<xrfg::DlssMotionVectorSet>();guides->eye_count=2;
        for(unsigned eye=0;eye<2;++eye) {
            auto frame=std::make_shared<xrfg::DlssMotionVectorFrame>();
            frame->stream=eye+1;frame->epoch=frame->serial=1;guides->eyes[eye]=std::move(frame);
        }
        xrfg::D3D12HistoryCaptureTicket captured{};xrfg::D3D12FrameSynthesisTicket prime{};
        require_hresult(history->capture(0,&captured),"FSR memory capture");
        require_hresult(history->commit(captured),"FSR memory commit");
        require_hresult(fsr.submit_prime(captured,make_reprojection_views(),0,&prime,guides),"FSR primes without unused optical warmup");
        require_frame_start_gate(fsr,"FSR memory prime completion");
        require_readback_matches(fixture,current.Get(),pattern,"FSR cold prime exact stereo",D3D12_RESOURCE_STATE_RENDER_TARGET);
        require(device->totals->live==0 && !device->totals->refused,"FSR prime allocated unused optical backing");
    }
    require_hresult(history->release_snapshots(),"FSR memory history release");
    device->totals->maximum=UINT64_MAX;
    xrfg::D3D12FrameSynthesizer flow;
    require_hresult(flow.initialize(device.Get(),queue.Get(),history,current_images,synthetic_images,kFormat,
        D3D12_RESOURCE_STATE_RENDER_TARGET,xrfg::D3D12OpticalFlowBackend::fidelity_fx,{},false),"active Fidelity memory");
    const auto active=device->totals->live.load();
    require(active>0,"active Fidelity must own its actual optical textures");
    // Give the replacement exactly one live engine's committed-texture budget.
    // Building it beside the old provider fails this real allocation boundary.
    device->totals->maximum=active;device->totals->refused=false;
    for(unsigned cycle=0;cycle<3;++cycle) {
        require_hresult(flow.reconfigure(xrfg::D3D12OpticalFlowBackend::fidelity_fx,{}),"method switch without double allocation peak");
        require(!device->totals->refused && device->totals->live<=active,"method switch doubled the working set");
    }
    const auto views=make_reprojection_views();
    for(unsigned cycle=0;cycle<2;++cycle) {
        ComPtr<ID3D12Fence> blocked;
        require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked)),"suspend producer gate");
        struct SignalOnExit {
            ComPtr<ID3D12Fence> fence;
            ~SignalOnExit(){if(fence)static_cast<void>(fence->Signal(1));}
        } cleanup{blocked};
        require_hresult(fixture.queue()->Wait(blocked.Get(),1),"suspend blocks actual producer work");
        xrfg::D3D12HistoryCaptureTicket captured{};
        require_hresult(history->capture(0,&captured),"warm optical capture");
        require_hresult(history->commit(captured),"warm optical capture commit");
        xrfg::D3D12FrameSynthesisTicket warming{};
        const auto submitted=cycle
            ?flow.submit_pair(captured,views,views,0,0,&warming)
            :flow.submit_prime(captured,views,0,&warming);
        require_hresult(submitted,"actual warmed optical submission");
        const auto publication=flow.submission_fence(warming);
        std::promise<void> entered;auto began=entered.get_future();
        auto suspending=std::async(std::launch::async,[&]{entered.set_value();return flow.suspend_flow_resources();});
        began.wait();
        const bool waiting=suspending.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout;
        const auto retained=device->totals->live.load();
        require_hresult(blocked->Signal(1),"suspend gate cleanup");cleanup.fence.Reset();
        const auto suspended=suspending.get();
        require(waiting && retained>0,"suspension freed active optical textures before their actual submission fence");
        require_hresult(suspended,"suspend warmed optical resources");
        require(flow.initialized() && device->totals->live==0 &&
            flow.submission_fence(warming)==publication && flow.ticket_available(warming)==S_OK,
            "suspension retained optical backing, broke copy transport or invalidated completed publication receipts");
        require_hresult(flow.suspend_flow_resources(),"idempotent optical suspension");
        require(device->totals->live==0,"cold optical suspension recreated method textures");
        xrfg::D3D12HistoryCaptureTicket copied{};
        require_hresult(history->capture(0,&copied),"cold transport capture");
        require_hresult(history->commit(copied),"cold transport capture commit");
        xrfg::D3D12FrameSynthesisTicket transport{};
        require_hresult(flow.submit_copy(copied,views,0,&transport),"copy transport survives optical suspension");
        require_frame_start_gate(flow,"cold transport completion");
        require(device->totals->live==0 && transport.fence_value>warming.fence_value,
            "copy transport allocated optical resources or restarted its publication timeline");
        require_readback_matches(fixture,current.Get(),pattern,"suspended exact stereo current",D3D12_RESOURCE_STATE_RENDER_TARGET);
        xrfg::D3D12FrameSynthesisTicket resumed{};
        require_hresult(flow.submit_prime(copied,views,0,&resumed),"actual OFXR prime lazily resumes optical resources");
        require_frame_start_gate(flow,"resumed optical completion");
        require(device->totals->live>0 && device->totals->live<=active && !device->totals->refused &&
            flow.submission_fence(transport)==publication && flow.ticket_available(transport)==S_OK &&
            resumed.fence_value>transport.fence_value,
            "real optical resume did not restore one bounded working set with its original publication fence");
        require_readback_matches(fixture,current.Get(),pattern,"resumed exact stereo current",D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    require_hresult(flow.reconfigure(xrfg::D3D12OpticalFlowBackend::fidelity_fx,{},xrfg::D3D12FrameGenerationMode::fsr3),"release Fidelity on FSR switch");
    require(device->totals->live==0,"switch to cold FSR retained old Fidelity texture backing");
    device->totals->maximum=0;device->totals->refused=false;
    ComPtr<ID3D12Fence> gate;
    require_hresult(fixture.device()->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)),"failed replacement gate");
    require_hresult(fixture.queue()->Wait(gate.Get(),1),"unrelated queued work gate");
    auto replacing=std::async(std::launch::async,[&]{return flow.reconfigure(xrfg::D3D12OpticalFlowBackend::fidelity_fx,{});});
    const bool returned=replacing.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
    require_hresult(gate->Signal(1),"failed replacement queue cleanup");
    const auto refused=replacing.get();
    require(returned && FAILED(refused) &&
        device->totals->refused && !flow.initialized() && device->totals->live==0,
        "failed method replacement drained unrelated queued work or retained/advertised a previous provider");
    std::cout<<"SYNTHESIS_MEMORY Fidelity_active_bytes="<<active<<" FSR_unused_optical_bytes=0 switch_cycles=3 suspend_resume_cycles=2 failed_switch=empty PASS\n";
}
