// Actual D3D12 lists, blocked queues and resource destruction. The test reports
// only submissions it has really executed, matching the shipping ingress hook.
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/d3d11_dlss_guides.hpp"
#include <Windows.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <cstdlib>
#include <new>

#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
// Deterministic failure of receipt bookkeeping, without adding test switches
// to production code. No graphics/API allocation is intercepted across DLLs.
static std::atomic_bool fail_next_snapshot_test_allocation{};
void* operator new(std::size_t bytes) {
    if(fail_next_snapshot_test_allocation.exchange(false,std::memory_order_acq_rel))throw std::bad_alloc();
    if(auto* memory=std::malloc(bytes?bytes:1))return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void operator delete(void* memory) noexcept{std::free(memory);}
void operator delete[](void* memory) noexcept{::operator delete(memory);}
void operator delete(void* memory,std::size_t) noexcept{::operator delete(memory);}
void operator delete[](void* memory,std::size_t) noexcept{::operator delete(memory);}
#endif

namespace dlss_snapshot_lifetime_fixture {
using Microsoft::WRL::ComPtr;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void ok(HRESULT value,const char* message){if(FAILED(value)){std::fprintf(stderr,"HRESULT=%08x\n",unsigned(value));throw std::runtime_error(message);}}
constexpr GUID lifetime_id{0x8a19ca43,0xc1a3,0x4d57,{0x84,0x0a,0xc4,0x54,0x57,0x40,0x86,0xa7}};
struct Counts {std::atomic_uint live{};std::atomic_uint64_t bytes{};};
struct Lifetime final : IUnknown {
    std::atomic<ULONG> references{1};std::shared_ptr<Counts> counts;UINT64 bytes{};
    Lifetime(std::shared_ptr<Counts> value,UINT64 size):counts(std::move(value)),bytes(size){++counts->live;counts->bytes+=bytes;}
    ~Lifetime(){--counts->live;counts->bytes-=bytes;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;if(id!=__uuidof(IUnknown))return E_NOINTERFACE;
        *output=static_cast<IUnknown*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references;}
    ULONG STDMETHODCALLTYPE Release() override{const auto remaining=--references;if(!remaining)delete this;return remaining;}
};
struct Recording {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::shared_ptr<const xrfg::DlssMotionVectorFrame> frame;
};
class SignalFailureQueue final : public ID3D12CommandQueue {
    ComPtr<ID3D12CommandQueue> actual_;std::atomic<ULONG> references_{1};
public:
    unsigned failed_signals{};
    explicit SignalFailureQueue(ID3D12CommandQueue* queue):actual_(queue){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(ID3D12Object) && id!=__uuidof(ID3D12DeviceChild) &&
            id!=__uuidof(ID3D12Pageable) && id!=__uuidof(ID3D12CommandQueue))return E_NOINTERFACE;
        *output=static_cast<ID3D12CommandQueue*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto remaining=--references_;if(!remaining)delete this;return remaining;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g,UINT* s,void* d) override{return actual_->GetPrivateData(g,s,d);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g,UINT s,const void* d) override{return actual_->SetPrivateData(g,s,d);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,const IUnknown* d) override{return actual_->SetPrivateDataInterface(g,d);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n) override{return actual_->SetName(n);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id,void** output) override{return actual_->GetDevice(id,output);}
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r,UINT n,const D3D12_TILED_RESOURCE_COORDINATE* c,
        const D3D12_TILE_REGION_SIZE* s,ID3D12Heap* h,UINT nr,const D3D12_TILE_RANGE_FLAGS* f,
        const UINT* o,const UINT* t,D3D12_TILE_MAPPING_FLAGS flags) override{actual_->UpdateTileMappings(r,n,c,s,h,nr,f,o,t,flags);}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d,const D3D12_TILED_RESOURCE_COORDINATE* dc,
        ID3D12Resource* s,const D3D12_TILED_RESOURCE_COORDINATE* sc,const D3D12_TILE_REGION_SIZE* z,
        D3D12_TILE_MAPPING_FLAGS f) override{actual_->CopyTileMappings(d,dc,s,sc,z,f);}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n,ID3D12CommandList* const* lists) override{actual_->ExecuteCommandLists(n,lists);}
    void STDMETHODCALLTYPE SetMarker(UINT m,const void* d,UINT n) override{actual_->SetMarker(m,d,n);}
    void STDMETHODCALLTYPE BeginEvent(UINT m,const void* d,UINT n) override{actual_->BeginEvent(m,d,n);}
    void STDMETHODCALLTYPE EndEvent() override{actual_->EndEvent();}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*,UINT64) override{++failed_signals;return E_FAIL;}
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f,UINT64 v) override{return actual_->Wait(f,v);}
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f) override{return actual_->GetTimestampFrequency(f);}
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g,UINT64* c) override{return actual_->GetClockCalibration(g,c);}
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override{return actual_->GetDesc();}
};
struct Fixture {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> progress;UINT64 serial{};HANDLE event{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    std::array<ComPtr<ID3D12Resource>,3> inputs;
    explicit Fixture(bool warp) {
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIAdapter> adapter;
        if(warp){ComPtr<IDXGIFactory4> factory;ok(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");ok(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");}
        ok(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"real D12 device");
        queue=make_queue();ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress)),"progress fence");
        for(auto& input:inputs) {
            D3D12_RESOURCE_DESC image{};image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            image.Width=image.Height=64;image.DepthOrArraySize=image.MipLevels=image.SampleDesc.Count=1;
            image.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            ok(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&image,
                D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&input)),"actual DLSS source");
        }
    }
    ~Fixture(){CloseHandle(event);}
    ComPtr<ID3D12CommandQueue> make_queue() {
        D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> result;ok(device->CreateCommandQueue(&description,IID_PPV_ARGS(&result)),"actual queue");return result;
    }
    ComPtr<ID3D12Fence> make_fence(){ComPtr<ID3D12Fence> result;ok(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&result)),"actual fence");return result;}
    Recording make_recording() {
        Recording recording;ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&recording.allocator)),"allocator");
        ok(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,recording.allocator.Get(),nullptr,
            IID_PPV_ARGS(&recording.list)),"actual command list");return recording;
    }
    void observe(ID3D12Resource* resource,const std::shared_ptr<Counts>& counts) {
        require(resource!=nullptr,"snapshot missing");const auto description=resource->GetDesc();
        const auto size=device->GetResourceAllocationInfo(0,1,&description).SizeInBytes;
        auto* observed=new Lifetime(counts,size);const auto result=resource->SetPrivateDataInterface(lifetime_id,observed);observed->Release();
        ok(result,"resource-local destruction observer");
    }
    Recording publish(std::uint64_t stream,const std::shared_ptr<Counts>& counts,bool protected_lifetime=true,
        ID3D12CommandQueue* producer=nullptr,bool close_list=true) {
        auto recording=make_recording();xrfg::DlssMotionVectorPublication publication;
        publication.stream=stream;publication.output=inputs[0].Get();publication.motion_vectors=inputs[1].Get();publication.depth=inputs[2].Get();
        publication.producer_queue=producer?producer:queue.Get();publication.producer_command_list=recording.list.Get();
        publication.output_width=publication.output_height=publication.motion_width=publication.motion_height=64;
        publication.depth_width=publication.depth_height=64;publication.scale_x=publication.scale_y=1;
        publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
        publication.frame_time_delta_ms=16.6f;publication.camera_near=.1f;publication.camera_far=100;
        publication.snapshot_output=true;publication.direct_output_receipt=protected_lifetime;
        recording.frame=xrfg::publish_dlss_motion_vectors(publication);require(bool(recording.frame),"real snapshot publication");
        observe(recording.frame->output.Get(),counts);observe(recording.frame->motion_vectors.Get(),counts);observe(recording.frame->depth.Get(),counts);
        if(close_list)ok(recording.list->Close(),"published original list close");return recording;
    }
    void submit(Recording& recording,ID3D12CommandQueue* target=nullptr) {
        if(!target)target=queue.Get();ID3D12CommandList* lists[]{recording.list.Get()};
        target->ExecuteCommandLists(1,lists);xrfg::report_dlss_motion_vector_submission(target,1,lists);
    }
    void wait(ID3D12CommandQueue* target=nullptr) {
        if(!target)target=queue.Get();const auto value=++serial;
        ok(target->Signal(progress.Get(),value),"completion signal");ok(progress->SetEventOnCompletion(value,event),"completion event");
        require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0 && progress->GetCompletedValue()!=UINT64_MAX,"actual GPU completion");
    }
    void check_debug() {
        ComPtr<ID3D12InfoQueue> messages;
        if(FAILED(device.As(&messages)))return;
        for(UINT64 index=0;index<messages->GetNumStoredMessagesAllowedByRetrievalFilter();++index) {
            SIZE_T bytes{};messages->GetMessage(index,nullptr,&bytes);std::vector<char> memory(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(memory.data());messages->GetMessage(index,message,&bytes);
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"D3D12: %s\n",message->pDescription);require(false,"snapshot GPU resource lifetime/state error");}
        }
    }
    ComPtr<ID3D12CommandAllocator> reset(Recording& recording,bool report=true) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ok(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"fresh Reset allocator");
        ok(recording.list->Reset(allocator.Get(),nullptr),"actual original Reset");
        if(report)xrfg::report_dlss_motion_vector_reset(recording.list.Get());
        return allocator; // the old recording allocator may still be in flight
    }
};
struct ReleaseGate {
    Fixture& fixture;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> gate;
    ~ReleaseGate(){
        if(gate)gate->Signal(1);
        // A failed assertion must not strand the test process on its own gate.
        try{fixture.wait(queue.Get());}catch(...){}
    }
};
void begin_tracking(){xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();xrfg::configure_dlss_motion_vector_tracking(true);}
void disable(){xrfg::configure_dlss_motion_vector_tracking(false);xrfg::retire_disabled_dlss_motion_vector_resources();}

void unsubmitted_and_blocked_writer(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41001,counts);
    require(counts->live==3,"three actual owned snapshots");recording.frame.reset();disable();
    require(counts->live==3,"disable destroyed snapshots referenced by an unsubmitted original list");
    auto gate=fixture.make_fence();ok(fixture.queue->Wait(gate.Get(),1),"hold original producer GPU work");
    fixture.submit(recording);ReleaseGate release{fixture,fixture.queue,gate};xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==3,"retired producer receipt did not protect blocked GPU copies");
    ok(gate->Signal(1),"release original production");fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts->live && !counts->bytes,"completed disabled snapshots did not retire");
    std::puts("Snapshots: unsubmitted and blocked original producer remain alive; actual Execute+completion retires all bytes PASS");
}
void retired_stream_and_reenable(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41002,counts);
    recording.frame.reset();xrfg::retire_dlss_motion_vector_stream(41002);
    require(counts->live==3,"retiring a native feature destroyed its recorded copies");
    disable();xrfg::configure_dlss_motion_vector_tracking(true);
    fixture.submit(recording);fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts->live,"retired stream ignored actual Execute after re-enable");disable();
    std::puts("Snapshots: retired feature still observes original Execute across rapid OFF/ON; zero retained bytes PASS");
}
void protected_consumer(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41003,counts);
    fixture.submit(original);fixture.wait();
    auto motion=original.frame->motion_retention,depth=original.frame->depth_retention;
    require(motion && depth,"native protected publication lacks consumer receipts");
    disable();require(counts->live==3,"CPU-pinned frame lost its snapshot allocations");
    auto consumer=fixture.make_queue();auto gate=fixture.make_fence(),complete=fixture.make_fence();
    auto read=fixture.make_recording();std::vector<ComPtr<ID3D12Resource>> destinations;
    for(auto* source:{original.frame->motion_vectors.Get(),original.frame->depth.Get()}) {
        const auto description=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        fixture.device->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> destination;ok(fixture.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,
            &buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&destination)),"actual consumer readback");
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={source,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,original.frame->resource_state,D3D12_RESOURCE_STATE_COPY_SOURCE};
        read.list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=destination.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        read.list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);read.list->ResourceBarrier(1,&barrier);
        destinations.push_back(std::move(destination));
    }
    ok(read.list->Close(),"consumer close");ok(consumer->Wait(gate.Get(),1),"hold actual consumer");
    motion->begin_gpu_read();depth->begin_gpu_read();ID3D12CommandList* lists[]{read.list.Get()};consumer->ExecuteCommandLists(1,lists);
    ok(consumer->Signal(complete.Get(),1),"actual consumer completion");
    ReleaseGate release{fixture,consumer,gate};
    ok(motion->retire_gpu_read(complete.Get(),1),"motion consumer receipt");ok(depth->retire_gpu_read(complete.Get(),1),"depth consumer receipt");
    original.frame.reset();motion.reset();depth.reset();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==2,"OFF did not preserve actual blocked motion/depth reads or retained unused output");
    ok(gate->Signal(1),"release consumer");fixture.wait(consumer.Get());xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts->live,"completed consumer kept private snapshot bytes");
    std::puts("Snapshots: CPU pins and actual separate-queue GPU reads protect exact textures; final fence frees every byte PASS");
}
void independent_writer_queues(Fixture& fixture) {
    begin_tracking();auto counts_a=std::make_shared<Counts>(),counts_b=std::make_shared<Counts>();
    auto second=fixture.make_queue();auto gate=fixture.make_fence();ok(fixture.queue->Wait(gate.Get(),1),"hold first writer");
    auto first=fixture.publish(41004,counts_a);fixture.submit(first);first.frame.reset();
    ReleaseGate release{fixture,fixture.queue,gate};
    auto later=fixture.publish(41004,counts_b,false,second.Get());fixture.submit(later,second.Get());fixture.wait(second.Get());later.frame.reset();
    disable();require(counts_a->live==3 && !counts_b->live,"another writer queue's higher fence value retired blocked original copies");
    ok(gate->Signal(1),"release first writer");fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts_a->live,"first writer allocations survived exact completion");
    std::puts("Snapshots: independent writer queues cannot advance each other's completion receipts PASS");
}
void repeated_toggles(Fixture& fixture) {
    for(unsigned cycle=0;cycle<8;++cycle) {
        begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41005,counts,cycle%2==0);
        fixture.submit(original);fixture.wait();original.frame.reset();disable();
        require(!counts->live && !counts->bytes,"repeated ON/OFF retained completed snapshot allocations");
    }
    std::puts("Snapshots: protected and ordinary guide OFF/ON x8 return to zero owned texture bytes PASS");
}
void successful_reset_cancels_unsubmitted(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41101,counts);
    recording.frame.reset();disable();require(counts->live==3,"unsubmitted original copies were not pinned");
    auto allocator=fixture.reset(recording);
    xrfg::retire_disabled_dlss_motion_vector_resources();require(!counts->live,"successful Reset did not cancel abandoned original copies");
    ok(recording.list->Close(),"reset original close");
    std::puts("Snapshots: successful original Reset cancels never-executed OFF copies, zero bytes without Execute PASS");
}
void reset_does_not_cancel_executed_writer(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41102,counts);
    auto gate=fixture.make_fence();ok(fixture.queue->Wait(gate.Get(),1),"hold executed writer before Reset");
    fixture.submit(recording);ReleaseGate release{fixture,fixture.queue,gate};recording.frame.reset();disable();
    auto allocator=fixture.reset(recording);
    xrfg::retire_disabled_dlss_motion_vector_resources();require(counts->live==3,"Reset canceled already-executed GPU work");
    ok(gate->Signal(1),"release executed writer after Reset");fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts->live,"actual writer fence did not release Reset list's previous GPU allocations");
    ok(recording.list->Close(),"reset in-flight list close");
    std::puts("Snapshots: Reset preserves already-executed blocked GPU copies until exact writer completion PASS");
}
void failed_reset_preserves_recordings(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto recording=fixture.publish(41103,counts,true,nullptr,false);
    recording.frame.reset();disable();fixture.check_debug();
    ComPtr<ID3D12InfoQueue> messages;fixture.device.As(&messages);
    D3D12_MESSAGE_SEVERITY expected[]{D3D12_MESSAGE_SEVERITY_ERROR,D3D12_MESSAGE_SEVERITY_CORRUPTION};
    D3D12_INFO_QUEUE_FILTER filter{};filter.DenyList.NumSeverities=2;filter.DenyList.pSeverityList=expected;
    if(messages)ok(messages->PushStorageFilter(&filter),"expected failed-Reset validation scope");
    // Reset of an OPEN list is a documented failure. The original recorded
    // copies still exist; only a successful Reset may report cancellation.
    const auto result=recording.list->Reset(recording.allocator.Get(),nullptr);
    if(messages)messages->PopStorageFilter();
    if(SUCCEEDED(result))xrfg::report_dlss_motion_vector_reset(recording.list.Get());
    require(FAILED(result),"open command list Reset unexpectedly succeeded");
    xrfg::retire_disabled_dlss_motion_vector_resources();require(counts->live==3,"failed Reset released original recorded snapshots");
    ok(recording.list->Close(),"failed Reset preserves open-list close");auto allocator=fixture.reset(recording);
    require(!counts->live,"later successful Reset did not retire abandoned copies");ok(recording.list->Close(),"later reset close");
    std::puts("Snapshots: original failed Reset retains recordings; later successful Reset alone releases them PASS");
}
void canceled_history_recovers(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto valid=fixture.publish(41104,counts);
    fixture.submit(valid);fixture.wait();const auto valid_publication=valid.frame->publication;
    require(bool(xrfg::resolve_dlss_guide_publication(41104,valid_publication,fixture.queue.Get())),"submitted original guide unavailable");
    auto valid_allocator=fixture.reset(valid);
    require(bool(xrfg::resolve_dlss_guide_publication(41104,valid_publication,fixture.queue.Get())),"Reset invalidated a guide whose copy actually executed");
    ok(valid.list->Close(),"already-executed reset close");valid.frame.reset();
    auto abandoned=fixture.publish(41104,counts);const auto canceled_epoch=abandoned.frame->epoch;
    const auto canceled_publication=abandoned.frame->publication;auto allocator=fixture.reset(abandoned);
    require(!xrfg::resolve_dlss_guide_publication(41104,canceled_publication,fixture.queue.Get()) &&
        !xrfg::resolve_dlss_motion_vectors(fixture.inputs[0].Get(),fixture.queue.Get()),
        "canceled active guide cache remained resolvable");
    abandoned.frame.reset();ok(abandoned.list->Close(),"canceled active list close");
    auto next=fixture.publish(41104,counts);
    require(next.frame->epoch>canceled_epoch && next.frame->serial==1 && next.frame->previous_serial==0,
        "first valid guide after cancellation did not prime a fresh epoch");
    fixture.submit(next);fixture.wait();
    require(bool(xrfg::resolve_dlss_guide_publication(41104,next.frame->publication,fixture.queue.Get())),"valid guide did not recover after canceled history");
    next.frame.reset();disable();require(!counts->live,"canceled/recovered stream retained completed snapshot bytes");
    std::puts("Snapshots: canceled active guides invalidate, executed guides survive Reset, next production primes fresh and recovers PASS");
}
void unknown_writer_is_not_reset_cancellation(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();ComPtr<SignalFailureQueue> queue;
    queue.Attach(new SignalFailureQueue(fixture.queue.Get()));auto original=fixture.publish(41105,counts,true,queue.Get());
    fixture.submit(original,queue.Get());fixture.wait();
    require(queue->failed_signals==1,"failed completion receipt fixture did not execute original Signal failure");
    original.frame.reset();disable();auto allocator=fixture.reset(original);
    xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==3,"Reset reclassified executed Signal-failed work as never submitted");
    ok(original.list->Close(),"quarantined reset list close");
    // No manufactured success fence or whole-queue-idle bypass clears this
    // failed proof. The isolated test process owns the quarantine until exit.
    std::puts("Snapshots: actual Execute with failed Signal remains quarantined after successful Reset (3 expected retained textures) PASS");
}
void execute_attempt_protects_reentrant_reset(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41106,counts);
    const auto publication=original.frame->publication;original.frame.reset();
    auto gate=fixture.make_fence();ok(fixture.queue->Wait(gate.Get(),1),"hold potentially reentrant original Execute");
    ID3D12CommandList* lists[]{original.list.Get()};
    xrfg::begin_dlss_motion_vector_submission(1,lists);
    require(!xrfg::resolve_dlss_guide_publication(41106,publication,fixture.queue.Get()),"pre-Execute quarantine granted a GPU completion receipt");
    fixture.queue->ExecuteCommandLists(1,lists);
    ReleaseGate release{fixture,fixture.queue,gate};
    // Model a wrapped original Execute which resets the list internally after
    // its native submission, before returning to the post-Execute reporter.
    auto allocator=fixture.reset(original);
    require(counts->live==3,"reentrant Reset canceled a started original Execute attempt");
    disable();xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,lists);
    xrfg::retire_disabled_dlss_motion_vector_resources();require(counts->live==3,"post-Execute receipt released blocked original copies");
    ok(gate->Signal(1),"release original attempted submission");fixture.wait();xrfg::retire_disabled_dlss_motion_vector_resources();
    require(!counts->live,"reentrant Reset prevented actual writer-fence retirement");ok(original.list->Close(),"reentrant Reset close");
    std::puts("Snapshots: pre-Execute attempt blocks reentrant Reset cancellation; only original post-Execute completion frees bytes PASS");
}
void abandoned_attempt_rejects_later_receipt(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41107,counts);
    ID3D12CommandList* lists[]{original.list.Get()};
#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
    fail_next_snapshot_test_allocation=true;
#endif
    xrfg::begin_dlss_motion_vector_submission(1,lists);
    // An original Execute exception has no reliable acceptance/completion
    // receipt. Its catch boundary must not leave an active attempt token.
    xrfg::abandon_dlss_motion_vector_submission(1,lists);
#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
    const bool no_allocation=fail_next_snapshot_test_allocation.exchange(false);
    require(no_allocation,"begin/abandon submission attempted a CPU heap allocation");
#endif
    original.frame.reset();disable();auto allocator=fixture.reset(original);
    require(counts->live==3,"Reset canceled an unknown original Execute exception");
    ok(original.list->Close(),"later empty reuse close");
    fixture.queue->ExecuteCommandLists(1,lists);
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,lists);fixture.wait();
    xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==3,"later reuse receipt cleared the abandoned original attempt quarantine");
    std::puts("Snapshots: abandoned Execute exception remains quarantined after later same-COM-list Reset/reuse (3 expected retained textures) PASS");
}
void missing_queue_seals_receipt(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41109,counts);
    ID3D12CommandList* lists[]{original.list.Get()};xrfg::begin_dlss_motion_vector_submission(1,lists);
    fixture.queue->ExecuteCommandLists(1,lists);
    xrfg::report_dlss_motion_vector_submission(nullptr,1,lists,true);fixture.wait();
    original.frame.reset();disable();auto allocator=fixture.reset(original);
    require(counts->live==3,"missing canonical queue left an executed writer Reset-cancelable");
    ok(original.list->Close(),"missing-queue later empty list close");fixture.queue->ExecuteCommandLists(1,lists);
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,lists);fixture.wait();
    xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==3,"later queue receipt consumed stale started marker from unresolved queue");
    std::puts("Snapshots: missing canonical post-Execute queue seals markers; later receipt cannot clear quarantine (3 expected textures) PASS");
}
void unidentified_input_seals_the_whole_attempt(Fixture& fixture) {
    begin_tracking();auto known_counts=std::make_shared<Counts>(),omitted_counts=std::make_shared<Counts>();
    auto known=fixture.publish(41110,known_counts),omitted=fixture.publish(41111,omitted_counts);
    known.frame.reset();omitted.frame.reset();
    // Model an original batch whose second list could not be normalized. The
    // known subset alone cannot establish which copies the full batch used.
    ID3D12CommandList* unidentified[]{nullptr};xrfg::begin_dlss_motion_vector_submission(1,unidentified);
    ID3D12CommandList* normalized_subset[]{known.list.Get()};
    xrfg::begin_dlss_motion_vector_submission(1,normalized_subset);
    fixture.queue->ExecuteCommandLists(1,normalized_subset);
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,normalized_subset,true);fixture.wait();disable();
    auto known_allocator=fixture.reset(known),omitted_allocator=fixture.reset(omitted);
    require(known_counts->live==3 && omitted_counts->live==3,"partial normalization left original batch copies Reset-cancelable");
    ok(known.list->Close(),"known quarantined Reset close");ok(omitted.list->Close(),"omitted quarantined Reset close");
    fixture.queue->ExecuteCommandLists(1,normalized_subset);
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,normalized_subset);fixture.wait();
    xrfg::retire_disabled_dlss_motion_vector_resources();
    require(known_counts->live==3 && omitted_counts->live==3,"partial later receipt cleared unidentified original-batch quarantine");
    std::puts("Snapshots: one unidentified original list quarantines the whole pending attempt; partial receipts/Reset cannot free copies (6 expected textures) PASS");
}
#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
void failed_receipt_allocation_is_quarantined(Fixture& fixture) {
    begin_tracking();auto counts=std::make_shared<Counts>();auto original=fixture.publish(41108,counts);
    ID3D12CommandList* lists[]{original.list.Get()};xrfg::begin_dlss_motion_vector_submission(1,lists);
    fixture.queue->ExecuteCommandLists(1,lists);
    fail_next_snapshot_test_allocation=true;
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,lists,true);
    const bool failure_injected=!fail_next_snapshot_test_allocation.exchange(false);
    require(failure_injected,"receipt allocation failpoint was not consumed");
    fixture.wait();original.frame.reset();disable();auto allocator=fixture.reset(original);
    require(counts->live==3,"receipt bad_alloc left original copies cancelable by Reset");
    ok(original.list->Close(),"receipt failure later empty list close");
    fixture.queue->ExecuteCommandLists(1,lists);
    xrfg::report_dlss_motion_vector_submission(fixture.queue.Get(),1,lists);fixture.wait();
    xrfg::retire_disabled_dlss_motion_vector_resources();
    require(counts->live==3,"later receipt consumed stale submission_started after bad_alloc");
    std::puts("Snapshots: deterministic post-Execute bad_alloc seals markers; later Reset/reuse cannot clear quarantine (3 expected textures) PASS");
}
#endif
} // namespace dlss_snapshot_lifetime_fixture
int main(int argc,char** argv) {
    try {
        using namespace dlss_snapshot_lifetime_fixture;
#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
        ID3D12CommandList* no_captures[]{nullptr};fail_next_snapshot_test_allocation=true;
        xrfg::begin_dlss_motion_vector_submission(1,no_captures);
        xrfg::abandon_dlss_motion_vector_submission(1,no_captures);
        const bool no_registry_allocation=fail_next_snapshot_test_allocation.exchange(false);
        require(no_registry_allocation,"empty begin/abandon lazily allocated registry storage");
#endif
        std::setvbuf(stdout,nullptr,_IONBF,0);Fixture fixture(argc>1 && std::string_view(argv[1])=="warp");
        unsubmitted_and_blocked_writer(fixture);retired_stream_and_reenable(fixture);protected_consumer(fixture);
        independent_writer_queues(fixture);repeated_toggles(fixture);
        successful_reset_cancels_unsubmitted(fixture);reset_does_not_cancel_executed_writer(fixture);
        failed_reset_preserves_recordings(fixture);canceled_history_recovers(fixture);
        execute_attempt_protects_reentrant_reset(fixture);
        abandoned_attempt_rejects_later_receipt(fixture);
        missing_queue_seals_receipt(fixture);
        unidentified_input_seals_the_whole_attempt(fixture);
#ifndef XRFG_SNAPSHOT_LIFETIME_FIXTURE_ONLY
        failed_receipt_allocation_is_quarantined(fixture);
#endif
        unknown_writer_is_not_reset_cancellation(fixture);fixture.check_debug();return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"DLSS snapshot lifetime FAIL: %s\n",error.what());return 1;}
}
