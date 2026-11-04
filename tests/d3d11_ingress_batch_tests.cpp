// Exercise the actual ingress with deterministic GPU stand-ins for SR and NR.
// No model runtime or game installation is needed.
#include "../src/ingress/d3d11_native_ingress.cpp"
#include <DirectXPackedVector.h>
#include <d3d11sdklayers.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <any>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fixture {
bool nr_enabled{},nr_before{true},fail_sr{},batch_open{};
unsigned raw_calls{},pre_calls{},post_calls{},checks{};
xrfg::embedded::Snapshot settings;
ID3D11RenderTargetView *motion_target{},*depth_target{};
xrfg::D3D11GuideTransport* transport{};

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
    expect(transport && (transport->list()!=nullptr)==batch_open,"only pre-SR NR closes the batch before SR");
    // Games may reuse their guide textures during SR. Deferred fencing must
    // not defer the actual D11 copies until after these writes.
    const float recycled[]{.875F,.875F,.875F,1.F};
    context->ClearRenderTargetView(motion_target,recycled);context->ClearRenderTargetView(depth_target,recycled);
    if (fail_sr) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    ID3D11Resource *color{},*output{};
    parameters->Get(NVSDK_NGX_Parameter_Color,&color);parameters->Get(NVSDK_NGX_Parameter_Output,&output);
    context->CopyResource(output,color);return NVSDK_NGX_Result_Success;
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
float read_guide(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12Resource* source,bool half) {
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
    barrier.Transition={source,0,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
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
void after_sr(unsigned int,ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* parameters,ID3D12CommandQueue*) {
    if (!fixture::nr_enabled || fixture::nr_before) return;
    ++fixture::post_calls;fixture::model(list,parameters,NVSDK_NGX_Parameter_Output);
}
void retire_handle(unsigned int) {}
}
namespace xrfg::embedded {
Snapshot snapshot() {return fixture::settings;}
AfwSnapshot afw_snapshot() {return {};}
}
namespace xrfg {bool luke_hfw_producer_supported() noexcept {return false;}}
namespace xrfg::native_ingress {
bool luke_detected() noexcept {return false;}
bool apply_camera(DlssMotionVectorPublication&,ID3D12GraphicsCommandList*) noexcept {return false;}
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
    motion_target=motion.target.Get();depth_target=depth_image.target.Get();
    register_d3d11_guide_owner(1,d11,d12,queue);configure_dlss_motion_vector_tracking(true);
    registry().modules[2].eval=evaluate_sr;settings.desired.enabled=true;settings.desired.motion_vectors=1;
    fixture::Parameters parameters;
    parameters.Set(NVSDK_NGX_Parameter_Color,static_cast<ID3D11Resource*>(color.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Output,static_cast<ID3D11Resource*>(output.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors,static_cast<ID3D11Resource*>(motion.texture.Get()));
    parameters.Set(NVSDK_NGX_Parameter_Depth,static_cast<ID3D11Resource*>(depth_image.texture.Get()));
    for (unsigned phase=0;phase<3;++phase) {
        nr_enabled=phase!=0;nr_before=phase==1;batch_open=phase!=1;
        auto* handle=reinterpret_cast<NVSDK_NGX_Handle*>(std::uintptr_t(phase+1));auto feature=std::make_shared<Feature>();
        feature->type=NVSDK_NGX_Feature_SuperSampling;feature->identity=phase+1;
        feature->width=feature->height=feature->out_width=feature->out_height=128;
        feature->flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> owner_queue;
        require(acquire_d3d11_guide_owner(d11,owner.GetAddressOf(),owner_queue.GetAddressOf(),&feature->owner),"transport owner");
        feature->transport=std::make_unique<D3D11GuideTransport>();
        require(feature->transport->initialize(context,d12,queue),"transport");transport=feature->transport.get();
        registry().features[handle]=feature;
        // Reuse each transport across success, original SR failure and recovery.
        for (unsigned iteration=0;iteration<3;++iteration) {
            fail_sr=iteration==1;fill(context,color,.25F);fill(context,motion,.125F);fill(context,depth_image,.75F);
            const auto calls=raw_calls;const auto result=evaluate<2>(context,handle,&parameters,nullptr);
            expect(raw_calls==calls+1,"original SR called exactly once");
            expect(NVSDK_NGX_FAILED(result)==fail_sr,"original SR result preserved");
            require(transport->drain(),"evaluation retirement");expect(!transport->list(),"batch closed on success and failure");
            expect(std::abs(read_guide(d12,queue,transport->resource(D3D11GuideTransport::motion),true)-.125F)<1e-6F,"motion captured before SR recycling");
            expect(std::abs(read_guide(d12,queue,transport->resource(D3D11GuideTransport::depth),false)-.75F)<1e-6F,"depth captured before SR recycling");
            if (fail_sr) continue;
            expect(std::abs(read(d11,context,output.texture.Get())-(phase==0?.25F:.5F))<1e-6F,"SR and NR output ordering");
            const auto receipt=get_d3d11_guide_receipt(output.texture.Get());
            expect(receipt.stream==reinterpret_cast<std::uintptr_t>(handle) && receipt.publication,"exact output receipt");
        }
        retire_dlss_motion_vector_stream(reinterpret_cast<std::uintptr_t>(handle));registry().features.erase(handle);
    }
    expect(pre_calls==3 && post_calls==2,"pre/post NR work remains in its selected phase");
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
        std::cout<<"D3D11 ingress batch checks="<<fixture::checks<<" failures=0\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
