// Exercise the actual ingress with deterministic GPU stand-ins for SR and NR.
// No model runtime or game installation is needed.
#include "../src/ingress/d3d11_native_ingress.cpp"
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <any>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fixture {
bool nr_enabled{},nr_before{true},fail_sr{},protected_output{},nested_output{},luke_output{},direct_render{};
bool refuse_post{};
int switch_after_sr{-1};
ID3D12Fence* post_gate{};
unsigned raw_calls{},pre_calls{},post_calls{},checks{},output_returns{};
xrfg::embedded::Snapshot settings;
ID3D11RenderTargetView *motion_target{},*depth_target{},*output_target{};
ID3D11Resource *tracked_output{},*tracked_color{};
xrfg::native_ingress::Registry::Copy original_copy{};
xrfg::D3D11GuideTransport* transport{};

void STDMETHODCALLTYPE count_copy(ID3D11DeviceContext* context,ID3D11Resource* dst,ID3D11Resource* src) {
    if(dst==tracked_output && src!=tracked_color)++output_returns;
    original_copy(context,dst,src);
}

void require(HRESULT result,const char* message) {
    if (FAILED(result)) throw std::runtime_error(message);
}
void expect(bool value,const char* message) {
    ++checks; if (!value) throw std::runtime_error(message);
}
void model(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* parameters,const char* name) {
    ID3D12Resource* output{};
    expect(NVSDK_NGX_SUCCEED(parameters->Get(name,&output)) && output,"NR target");
    ComPtr<ID3D12Device> device;require(list->GetDevice(IID_PPV_ARGS(device.GetAddressOf())),"NR device");
    D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> heap;
    require(device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(heap.GetAddressOf())),"NR RTV heap");
    const auto target=heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(output,nullptr,target);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={output,0,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET};
    if (!std::strcmp(name,NVSDK_NGX_Parameter_Color)) barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1,&barrier);
    const float value[]{.5F,.5F,.5F,1.F};list->ClearRenderTargetView(target,value,0,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
}
struct Parameters final:NVSDK_NGX_Parameter {
    std::unordered_map<std::string,std::any> values;
    template<class T> NVSDK_NGX_Result get(const char* name,T* value) const {
        const auto found=values.find(name);if (found==values.end()) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        const auto* typed=std::any_cast<T>(&found->second);if (!typed) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        *value=*typed;return NVSDK_NGX_Result_Success;
    }
#define FIELD(T) void Set(const char* name,T value) override {values[name]=value;} \
    NVSDK_NGX_Result Get(const char* name,T* value) const override {return get(name,value);}
    FIELD(unsigned long long) FIELD(float) FIELD(double) FIELD(unsigned int) FIELD(int)
    FIELD(ID3D11Resource*) FIELD(ID3D12Resource*) FIELD(void*)
#undef FIELD
    void Reset() override {values.clear();}
};
NVSDK_NGX_Result evaluate_sr(ID3D11DeviceContext* context,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* parameters,void*) {
    ++raw_calls;
    expect(transport && !transport->list(),"original pre-SR stage closes before every SR evaluation");
    xrfg::native_ingress::nested_evaluation=nested_output;
    // Games may reuse their guide textures during SR. Deferred fencing must
    // not defer the actual D11 copies until after these writes.
    const float recycled[]{.875F,.875F,.875F,1.F};
    context->ClearRenderTargetView(motion_target,recycled);context->ClearRenderTargetView(depth_target,recycled);
    if (fail_sr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if(switch_after_sr>=0) {nr_enabled=switch_after_sr!=0;nr_before=switch_after_sr==1;}
    ID3D11Resource *color{},*output{};
    parameters->Get(NVSDK_NGX_Parameter_Color,&color);parameters->Get(NVSDK_NGX_Parameter_Output,&output);
    if(direct_render) {const float value[]{.25F,.25F,.25F,1.F};context->ClearRenderTargetView(output_target,value);}
    else context->CopyResource(output,color);
    return NVSDK_NGX_Result_Success;
}
struct Image {ComPtr<ID3D11Texture2D> texture;ComPtr<ID3D11RenderTargetView> target;};
Image image(ID3D11Device* device,DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=128;desc.ArraySize=desc.MipLevels=1;
    desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    Image result;require(device->CreateTexture2D(&desc,nullptr,result.texture.GetAddressOf()),"game image");
    require(device->CreateRenderTargetView(result.texture.Get(),nullptr,result.target.GetAddressOf()),"game RTV");return result;
}
void fill(ID3D11DeviceContext* context,const Image& image,float value) {
    const float pixel[]{value,value,value,1.F};context->ClearRenderTargetView(image.target.Get(),pixel);
}
float read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);desc.BindFlags=desc.MiscFlags=0;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;require(device->CreateTexture2D(&desc,nullptr,staging.GetAddressOf()),"D11 readback texture");
    context->CopyResource(staging.Get(),source);D3D11_MAPPED_SUBRESOURCE mapped{};
    require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"D11 readback");
    const float value=DirectX::PackedVector::XMConvertHalfToFloat(*static_cast<const std::uint16_t*>(mapped.pData));
    context->Unmap(staging.Get(),0);return value;
}
float read_guide(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* source,bool half,
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON) {
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    require(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(buffer.GetAddressOf())),"D12 readback buffer");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    require(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(allocator.GetAddressOf())),"readback allocator");
    require(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(list.GetAddressOf())),"readback list");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={source,0,state,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=source;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=buffer.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={source->GetDesc().Format,1,1,1,256};const D3D12_BOX box{0,0,0,1,1,1};
    list->CopyTextureRegion(&to,0,0,0,&from,&box);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    require(list->Close(),"readback close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;require(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"readback fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);expect(event!=nullptr,"readback event");
    const HRESULT signal=queue->Signal(fence.Get(),1),completion=fence->SetEventOnCompletion(1,event);
    const DWORD waited=SUCCEEDED(signal)&&SUCCEEDED(completion)?WaitForSingleObject(event,5000):WAIT_FAILED;CloseHandle(event);
    expect(waited==WAIT_OBJECT_0,"readback completion");void* mapped{};require(buffer->Map(0,nullptr,&mapped),"readback map");
    const float value=half?DirectX::PackedVector::XMConvertHalfToFloat(*static_cast<const std::uint16_t*>(mapped)):*static_cast<const float*>(mapped);
    buffer->Unmap(0,nullptr);return value;
}
}

namespace xrfg::neural_rendering {
Status status() {return {fixture::nr_enabled,fixture::nr_enabled,fixture::nr_before,"fixture",0};}
void before_sr(unsigned int,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* parameters,ID3D12CommandQueue*) {
    if (!fixture::nr_enabled || !fixture::nr_before) return;
    ++fixture::pre_calls;fixture::model(list,parameters,NVSDK_NGX_Parameter_Color);
}
void after_sr(unsigned int,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* parameters,ID3D12CommandQueue* queue) {
    if(fixture::post_gate)fixture::require(queue->Wait(fixture::post_gate,1),"post-SR gate");
    if (!fixture::nr_enabled || fixture::nr_before || fixture::refuse_post) return;
    ++fixture::post_calls;fixture::model(list,parameters,NVSDK_NGX_Parameter_Output);
}
bool after_sr_needs_copyback(unsigned int handle,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* parameters,ID3D12CommandQueue* queue) {
    after_sr(handle,list,parameters,queue);
    return fixture::nr_enabled && !fixture::nr_before;
}
void retire_handle(unsigned int) {}
}
namespace xrfg::embedded {
Snapshot snapshot() {return fixture::settings;}
AfwSnapshot afw_snapshot() {return {};}
}
namespace xrfg {bool luke_hfw_producer_supported() noexcept {return false;}}
namespace xrfg::native_ingress {
bool luke_detected() noexcept {return fixture::luke_output;}
bool apply_camera(DlssMotionVectorPublication& guides,ID3D12GraphicsCommandList*) noexcept {
    guides.protect_snapshot_lifetime=fixture::protected_output;return true;
}
void configure_queue(ID3D12CommandQueue*) noexcept {}
void capture_log(const char*,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t) noexcept {}
std::uint32_t allocate_handle_key() noexcept {static std::uint32_t next{};return ++next;}

void run(ID3D11Device* d11,ID3D11DeviceContext* context,ID3D12Device* d12,ID3D12CommandQueue* queue) {
    using namespace fixture;
    auto color=image(d11,DXGI_FORMAT_R16G16B16A16_FLOAT),output=image(d11,DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto motion=image(d11,DXGI_FORMAT_R16G16_FLOAT),depth_image=image(d11,DXGI_FORMAT_R32_FLOAT);
    {
        D3D11GuideTransport abandoned;require(abandoned.initialize(context,d12,queue),"abandoned transport");
        fill(context,motion,.125F);require(abandoned.begin(),"abandoned recording");
        require(abandoned.copy(D3D11GuideTransport::motion,motion.texture.Get()),"abandoned copy");
        require(abandoned.drain(),"unsubmitted D11 copies retire");
        expect(std::abs(read_guide(d12,queue,abandoned.resource(D3D11GuideTransport::motion),true)-.125F)<1e-6F,"drain fences pending copies without executing an abandoned list");
    }
    motion_target=motion.target.Get();depth_target=depth_image.target.Get();output_target=output.target.Get();
    tracked_output=output.texture.Get();tracked_color=color.texture.Get();
    observe_copies(context);original_copy=registry().original_copy;registry().original_copy=count_copy;
    register_d3d11_guide_owner(1,d11,d12,queue);configure_dlss_motion_vector_tracking(true);
    registry().modules[2].eval=evaluate_sr;settings.desired.enabled=true;settings.desired.motion_vectors=1;
    fixture::Parameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(color.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(output.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(motion.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Depth,static_cast<ID3D11Resource*>(depth_image.texture.Get()));
    for (unsigned phase=0;phase<6;++phase) {
        nr_enabled=phase==1||phase==2;nr_before=phase==1;
        protected_output=phase==3;nested_output=phase==4;luke_output=phase==5;
        auto* handle=reinterpret_cast<NVSDK_NGX_Handle*>(std::uintptr_t(phase+1));auto feature=std::make_shared<Feature>();
        feature->type=NVSDK_NGX_Feature_SuperSampling;feature->identity=phase+1;
        feature->width=feature->height=feature->out_width=feature->out_height=128;
        feature->flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        feature->flags_known=true;
        ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> owner_queue;
        require(acquire_d3d11_guide_owner(d11,owner.GetAddressOf(),owner_queue.GetAddressOf(),&feature->owner),"transport owner");
        feature->transport=std::make_unique<D3D11GuideTransport>();
        require(feature->transport->initialize(context,d12,queue),"transport");transport=feature->transport.get();
        registry().features[handle]=feature;
        // Reuse each transport across success, original SR failure and recovery.
        for (unsigned iteration=0;iteration<3;++iteration) {
            fail_sr=iteration==1;fill(context,color,.25F);fill(context,motion,.125F);fill(context,depth_image,.75F);
            const auto calls=raw_calls,returned=output_returns;const auto result=evaluate<2>(context,handle,&parameters,nullptr);
            expect(raw_calls==calls+1,"original SR called exactly once");
            expect(NVSDK_NGX_FAILED(result)==fail_sr,"original SR result preserved");
            expect(output_returns-returned==(!fail_sr&&(phase>=2)?1U:0U),"copyback only for post-SR NR or protected/nested/Luke output");
            require(transport->drain(),"evaluation retirement");expect(!transport->list(),"batch closed on success and failure");
            expect(std::abs(read_guide(d12,queue,transport->resource(D3D11GuideTransport::motion),true)-.125F)<1e-6F,"motion captured before SR recycling");
            expect(std::abs(read_guide(d12,queue,transport->resource(D3D11GuideTransport::depth),false)-.75F)<1e-6F,"depth captured before SR recycling");
            if (fail_sr) {expect(!get_d3d11_guide_receipt(output.texture.Get()).publication,"failed SR clears old output receipt");continue;}
            expect(std::abs(read(d11,context,output.texture.Get())-(nr_enabled?.5F:.25F))<1e-6F,"SR and NR output ordering");
            const auto receipt=get_d3d11_guide_receipt(output.texture.Get());
            expect(receipt.stream==reinterpret_cast<std::uintptr_t>(handle) && receipt.publication,"exact output receipt");
        }
        retire_dlss_motion_vector_stream(reinterpret_cast<std::uintptr_t>(handle));registry().features.erase(handle);
    }
    expect(pre_calls==3 && post_calls==2,"pre/post NR work remains in its selected phase");
    // Draw-like SR does not visit CopyResource's metadata hook. A failed or
    // capacity-rejected publication must still invalidate the old receipt.
    {
        nr_enabled=false;nr_before=false;protected_output=nested_output=luke_output=false;
        fail_sr=false;direct_render=true;
        auto* handle=reinterpret_cast<NVSDK_NGX_Handle*>(99);auto feature=std::make_shared<Feature>();
        feature->type=NVSDK_NGX_Feature_SuperSampling;feature->identity=99;
        feature->width=feature->height=feature->out_width=feature->out_height=128;
        feature->flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> owner_queue;
        require(acquire_d3d11_guide_owner(d11,owner.GetAddressOf(),owner_queue.GetAddressOf(),&feature->owner),"pressure owner");
        feature->transport=std::make_unique<D3D11GuideTransport>();require(feature->transport->initialize(context,d12,queue),"pressure transport");
        transport=feature->transport.get();registry().features[handle]=feature;
        std::vector<std::shared_ptr<const DlssMotionVectorFrame>> held;
        for(unsigned frame=0;frame<8;++frame) {
            fill(context,motion,.125F);fill(context,depth_image,.75F);
            const auto returned=output_returns;
            expect(NVSDK_NGX_SUCCEED(evaluate<2>(context,handle,&parameters,nullptr)),"SR survives publication pressure");
            require(transport->drain(),"pressure retirement");const auto receipt=get_d3d11_guide_receipt(output.texture.Get());
            if(frame<6) {
                expect(receipt.publication!=0,"direct publication available before capacity");
                held.push_back(resolve_dlss_guide_publication(receipt.stream,receipt.publication,queue));expect(held.back()!=nullptr,"hold exact publication");
            } else expect(receipt.publication==0,"rejected publication cannot reuse the previous receipt");
            expect(output_returns==returned,"no unchanged output return under publication pressure");
            expect(std::abs(read(d11,context,output.texture.Get())-.25F)<1e-6F,"native shader-produced pixels survive");
        }
        fail_sr=true;expect(NVSDK_NGX_FAILED(evaluate<2>(context,handle,&parameters,nullptr)),"shader-style SR failure");
        expect(!get_d3d11_guide_receipt(output.texture.Get()).publication,"shader-style failure has no stale receipt");
        held.clear();fail_sr=false;direct_render=false;

        // Alternate two native handles without an artificial drain/readback
        // between evaluations. Only the ingress's own reuse barriers remain.
        auto* other_handle=reinterpret_cast<NVSDK_NGX_Handle*>(100);
        auto other=std::make_shared<Feature>();other->type=feature->type;other->identity=100;
        other->width=other->height=other->out_width=other->out_height=128;other->flags=feature->flags;
        other->owner=feature->owner;
        other->transport=std::make_unique<D3D11GuideTransport>();
        require(other->transport->initialize(context,d12,queue),"second eye transport");registry().features[other_handle]=other;
        struct Retained {std::shared_ptr<const DlssMotionVectorFrame> frame;float motion,depth;};
        std::vector<Retained> retained;unsigned burst_rejections{};
        for(unsigned frame=0;frame<96;++frame)for(unsigned eye=0;eye<2;++eye) {
            const unsigned mode=(frame+eye)%3;
            nr_enabled=mode!=0;nr_before=mode==1;refuse_post=frame%7==0;
            switch_after_sr=(frame+eye)%4==0?static_cast<int>((mode+1)%3):-1;
            const unsigned selected=switch_after_sr<0?mode:static_cast<unsigned>(switch_after_sr);
            protected_output=frame%11==0;nested_output=frame%13==0;luke_output=frame%17==0;
            fail_sr=frame%19==7;
            const float mv=.125F+static_cast<float>(frame%4)*.125F,dz=.5F+static_cast<float>(eye)*.25F;
            fill(context,color,.25F);fill(context,motion,mv);fill(context,depth_image,dz);
            transport=eye?other->transport.get():feature->transport.get();
            const auto calls=raw_calls,returned=output_returns;
            auto* current=eye?other_handle:handle;
            const auto result=evaluate<2>(context,current,&parameters,nullptr);
            expect(raw_calls==calls+1,"burst calls native SR once");
            expect(NVSDK_NGX_FAILED(result)==fail_sr,"burst preserves native failure");
            const bool needs_return=!fail_sr&&(selected==2||protected_output||nested_output||luke_output);
            expect(output_returns-returned==(needs_return?1U:0U),"configuration switch and refused NR retain required return");
            const auto receipt=get_d3d11_guide_receipt(output.texture.Get());
            if(fail_sr)expect(!receipt.publication,"burst failure clears receipt");
            else {
                if(!receipt.publication) {
                    ++burst_rejections;
                    expect(((eye?other:feature)->noted>>32)==9,"only an actually refused publication may omit the burst receipt");
                } else expect(receipt.stream==reinterpret_cast<std::uintptr_t>(current),"burst receipt belongs to current eye");
                if(receipt.publication&&frame%24==1&&eye==0) {
                    auto published=resolve_dlss_guide_publication(receipt.stream,receipt.publication,queue);
                    expect(published!=nullptr,"retain exact burst guide");retained.push_back({published,mv,dz});
                }
            }
        }
        require(feature->transport->drain(),"first eye burst retirement");
        require(other->transport->drain(),"second eye burst retirement");
        expect(retained.size()>=2,"burst retains multiple older snapshots");
        std::cout<<"Burst publications refused under retained-snapshot pressure="<<burst_rejections<<'\n';
        for(const auto& saved:retained) {
            expect(std::abs(read_guide(d12,queue,saved.frame->motion_vectors.Get(),true,saved.frame->resource_state)-saved.motion)<1e-6F,"retained motion survives later game and eye reuse");
            expect(std::abs(read_guide(d12,queue,saved.frame->depth.Get(),false,saved.frame->depth_resource_state)-saved.depth)<1e-6F,"retained depth survives later game and eye reuse");
        }
        retained.clear();retire_dlss_motion_vector_stream(100);registry().features.erase(other_handle);

        // The final D11 return used to serialize the game behind the D12
        // snapshot. With unchanged output, the game may proceed; a distinct
        // D12 consumer must still wait for the publication's producer fence.
        nr_enabled=false;nr_before=false;refuse_post=false;switch_after_sr=-1;
        protected_output=nested_output=luke_output=fail_sr=false;transport=feature->transport.get();
        ComPtr<ID3D12Fence> gate;require(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(gate.GetAddressOf())),"publication gate");
        struct ReleaseGate {ID3D12Fence* fence;~ReleaseGate(){fixture::post_gate=nullptr;fence->Signal(1);}} release_gate{gate.Get()};
        post_gate=gate.Get();fill(context,color,.25F);fill(context,motion,.125F);fill(context,depth_image,.75F);
        expect(NVSDK_NGX_SUCCEED(evaluate<2>(context,handle,&parameters,nullptr)),"gated publication succeeds");
        ComPtr<ID3D11Device5> d11_5;ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Fence> game_progress;
        require(d11->QueryInterface(IID_PPV_ARGS(d11_5.GetAddressOf())),"game fence device");
        require(context->QueryInterface(IID_PPV_ARGS(context4.GetAddressOf())),"game fence context");
        require(d11_5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(game_progress.GetAddressOf())),"game progress fence");
        const auto receipt=get_d3d11_guide_receipt(output.texture.Get());
        fill(context,output,.875F);fill(context,motion,.875F);fill(context,depth_image,.875F);
        require(context4->Signal(game_progress.Get(),1),"signal game progress");context->Flush();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!game_progress->GetCompletedValue()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
        expect(game_progress->GetCompletedValue()==1&&gate->GetCompletedValue()==0,"game progresses while snapshot execution remains gated");
        D3D12_COMMAND_QUEUE_DESC consumer_desc{};ComPtr<ID3D12CommandQueue> consumer;
        require(d12->CreateCommandQueue(&consumer_desc,IID_PPV_ARGS(consumer.GetAddressOf())),"distinct consumer queue");
        auto published=resolve_dlss_guide_publication(receipt.stream,receipt.publication,consumer.Get());
        expect(published&&published->producer_fence,"gated consumer resolves producer fence");
        ComPtr<ID3D12Fence> consumed;require(d12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(consumed.GetAddressOf())),"consumer progress fence");
        require(consumer->Signal(consumed.Get(),1),"consumer completion signal");
        std::atomic<bool> entered{},finished{};HRESULT next_begin=E_PENDING;
        std::jthread reuse([&]{entered=true;next_begin=transport->begin();finished=true;});
        while(!entered.load())std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const bool reuse_blocked=!finished.load(),consumer_blocked=consumed->GetCompletedValue()==0;
        const auto released=gate->Signal(1);reuse.join();post_gate=nullptr;
        require(released,"release producer");
        expect(reuse_blocked,"transport cannot reset its allocator or reuse shared storage before retirement");
        expect(consumer_blocked,"consumer cannot overtake pending producer");
        require(next_begin,"next recording begins after retirement");require(transport->submit(),"close next recording");
        require(transport->drain(),"gated producer retirement");
        expect(std::abs(read_guide(d12,consumer.Get(),published->motion_vectors.Get(),true,published->resource_state)-.125F)<1e-6F,"gated snapshot retains captured motion");
        expect(std::abs(read_guide(d12,consumer.Get(),published->depth.Get(),false,published->depth_resource_state)-.75F)<1e-6F,"gated snapshot retains captured depth");
        expect(consumed->GetCompletedValue()==1,"consumer progresses after producer release");
        expect(std::abs(read(d11,context,output.texture.Get())-.875F)<1e-6F,"game's later output survives pending snapshot");
        published.reset();
        D3D11_TEXTURE2D_DESC bad_desc{};depth_image.texture->GetDesc(&bad_desc);bad_desc.MipLevels=2;
        ComPtr<ID3D11Texture2D> bad_depth;
        require(d11->CreateTexture2D(&bad_desc,nullptr,bad_depth.GetAddressOf()),"invalid ingress depth");
        for(ID3D11Resource* z:{static_cast<ID3D11Resource*>(nullptr),static_cast<ID3D11Resource*>(bad_depth.Get()),static_cast<ID3D11Resource*>(depth_image.texture.Get())}) {
            parameters.Set(NVSDK_NGX_Parameter_Depth,z);fill(context,color,.25F);fill(context,motion,.125F);fill(context,depth_image,.75F);
            const auto calls=raw_calls;
            expect(NVSDK_NGX_SUCCEED(evaluate<2>(context,handle,&parameters,nullptr)),"optional depth failure and recovery keep SR result");
            expect(raw_calls==calls+1,"optional depth fallback calls original SR once");require(transport->drain(),"optional depth retirement");
            const auto current=get_d3d11_guide_receipt(output.texture.Get());
            auto frame=resolve_dlss_guide_publication(current.stream,current.publication,queue);
            expect(frame&&bool(frame->depth)==(z==depth_image.texture.Get()),"optional failure never publishes stale depth");
        }
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(nullptr));
        expect(NVSDK_NGX_SUCCEED(evaluate<2>(context,handle,&parameters,nullptr)),"missing motion falls back to SR");
        expect(!get_d3d11_guide_receipt(output.texture.Get()).publication,"motion fallback clears old receipt");
        parameters.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(motion.texture.Get()));
        direct_render=true;parameters.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(nullptr));
        expect(NVSDK_NGX_SUCCEED(evaluate<2>(context,handle,&parameters,nullptr)),"SR without input color remains available");
        require(transport->drain(),"no color retirement");
        expect(std::abs(read(d11,context,output.texture.Get())-.25F)<1e-6F,"no-color SR keeps native output");
        direct_render=false;parameters.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(color.texture.Get()));
        retire_dlss_motion_vector_stream(99);registry().features.erase(handle);
    }
    registry().original_copy=original_copy;
    transport=nullptr;unregister_d3d11_guide_owner(1);
}
}

int main(int argc,char** argv) {
    try {
        ComPtr<ID3D12Debug> debug12;
        const bool validation12=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debug12.GetAddressOf())));
        if (validation12) debug12->EnableDebugLayer();
        ComPtr<IDXGIAdapter1> selected;
        if (argc==3 && std::string_view(argv[1])=="--adapter") {
            ComPtr<IDXGIFactory1> factory;fixture::require(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())),"DXGI factory");
            fixture::require(factory->EnumAdapters1(static_cast<UINT>(std::stoul(argv[2])),selected.GetAddressOf()),"requested adapter");
        }
        ComPtr<ID3D11Device> d11;ComPtr<ID3D11DeviceContext> context;
        const auto type=selected?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE;
        HRESULT creation=D3D11CreateDevice(selected.Get(),type,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,
            d11.GetAddressOf(),nullptr,context.GetAddressOf());
        const bool validation11=SUCCEEDED(creation);
        if (FAILED(creation)) creation=D3D11CreateDevice(selected.Get(),type,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            d11.GetAddressOf(),nullptr,context.GetAddressOf());
        if (FAILED(creation)) return 77;
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<ID3D12Device> d12;ComPtr<ID3D12CommandQueue> queue;
        fixture::require(d11.As(&dxgi),"DXGI device");fixture::require(dxgi->GetAdapter(adapter.GetAddressOf()),"same adapter");
        DXGI_ADAPTER_DESC adapter_desc{};adapter->GetDesc(&adapter_desc);std::wcout<<L"GPU: "<<adapter_desc.Description<<'\n';
        if (FAILED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(d12.GetAddressOf())))) return 77;
        D3D12_COMMAND_QUEUE_DESC desc{};fixture::require(d12->CreateCommandQueue(&desc,IID_PPV_ARGS(queue.GetAddressOf())),"queue");
        xrfg::native_ingress::run(d11.Get(),context.Get(),d12.Get(),queue.Get());
        ComPtr<ID3D11InfoQueue> info11;
        if (validation11 && SUCCEEDED(d11.As(&info11))) for (UINT64 i=0;i<info11->GetNumStoredMessages();++i) {
            SIZE_T size{};info11->GetMessage(i,nullptr,&size);std::vector<std::byte> storage(size);
            auto* message=reinterpret_cast<D3D11_MESSAGE*>(storage.data());info11->GetMessage(i,message,&size);
            fixture::expect(message->Severity>D3D11_MESSAGE_SEVERITY_ERROR,"D11 validation error");
        }
        ComPtr<ID3D12InfoQueue> info12;
        if (validation12 && SUCCEEDED(d12.As(&info12))) for (UINT64 i=0;i<info12->GetNumStoredMessages();++i) {
            SIZE_T size{};info12->GetMessage(i,nullptr,&size);std::vector<std::byte> storage(size);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());info12->GetMessage(i,message,&size);
            fixture::expect(message->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"D12 validation error");
        }
        std::cout<<"D3D11 output copyback checks="<<fixture::checks<<" failures=0; D11 validation="<<validation11<<" D12 validation="<<validation12<<'\n';return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
