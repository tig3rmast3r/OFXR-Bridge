#include "xrfg/native_ingress.hpp"
#include "xrfg/upscaler_observation.hpp"
#include "xrfg/d3d11_dlss_guides.hpp"
#include "xrfg/d3d11_guide_transport.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "xrfg/neural_rendering.hpp"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/luke_render_identity.hpp"
#include <nvsdk_ngx.h>
#include <safetyhook.hpp>
#include <windows.h>
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <unordered_map>
#include <cstring>

namespace xrfg::native_ingress {
namespace {
using Microsoft::WRL::ComPtr;
template<class T>T get(const NVSDK_NGX_Parameter* p,const char* key,T fallback={}) {
    T value=fallback;return p&&NVSDK_NGX_SUCCEED(p->Get(key,&value))?value:fallback;
}
template<class T>ComPtr<T> native(T* input) {
    ComPtr<T> output;IUnknown* borrowed{};
    if(d3d_native::get_borrowed(input,&borrowed))borrowed->QueryInterface(IID_PPV_ARGS(&output));
    return output;
}
// BindFlags<<32 | DXGI format, for the capture log.
std::uint64_t describe(ID3D11Resource* resource) {
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};
    if(!resource||FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf()))))return 0;
    texture->GetDesc(&desc);return (std::uint64_t{desc.BindFlags}<<32)|desc.Format;
}
std::uint64_t pack(std::uint32_t high,std::uint32_t low){return (std::uint64_t{high}<<32)|low;}
// Width<<32 | height.
std::uint64_t extent(ID3D11Resource* resource) {
    ComPtr<ID3D11Texture2D> texture;D3D11_TEXTURE2D_DESC desc{};
    if(!resource||FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf()))))return 0;
    texture->GetDesc(&desc);return pack(desc.Width,desc.Height);
}
std::uint32_t bits(float value){std::uint32_t result{};std::memcpy(&result,&value,sizeof(result));return result;}
// Scalar queries preserve the exact immutable native evaluation table. Only
// transport-owned GPU objects are substituted; callers' maps are never edited.
// Depth is withheld when this evaluation's depth did not cross over.
struct Parameters final:NVSDK_NGX_Parameter {
    const NVSDK_NGX_Parameter* base{};D3D11GuideTransport* transport{};bool depth{true};
    Parameters(const NVSDK_NGX_Parameter* p,D3D11GuideTransport* t,bool has_depth):base(p),transport(t),depth(has_depth){}
#define SCALAR(T) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char* n,T* v)const override{return base->Get(n,v);}
    SCALAR(unsigned long long) SCALAR(float) SCALAR(double) SCALAR(unsigned int) SCALAR(int)
#undef SCALAR
    void Set(const char*,ID3D11Resource*)override{}void Set(const char*,ID3D12Resource*)override{}void Set(const char*,void*)override{}
    NVSDK_NGX_Result Get(const char* n,ID3D11Resource** v)const override{return base->Get(n,v);}
    ID3D12Resource* resource(const char* n)const {
        if(!strcmp(n,NVSDK_NGX_Parameter_Color))return transport->resource(D3D11GuideTransport::color);
        if(!strcmp(n,NVSDK_NGX_Parameter_Output))return transport->resource(D3D11GuideTransport::output);
        if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectors))return transport->resource(D3D11GuideTransport::motion);
        if(!strcmp(n,NVSDK_NGX_Parameter_Depth))return depth?transport->resource(D3D11GuideTransport::depth):nullptr;
        return nullptr;
    }
    NVSDK_NGX_Result Get(const char* n,ID3D12Resource** v)const override {
        if(auto* r=resource(n)){*v=r;return NVSDK_NGX_Result_Success;}return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char* n,void** v)const override {
        if(auto* r=resource(n)){*v=r;return NVSDK_NGX_Result_Success;}return base->Get(n,v);
    }
    void Reset()override{}
};
struct Feature {
    std::uint32_t identity{};NVSDK_NGX_Feature type{};int flags{};UINT width{},height{},out_width{},out_height{};bool flags_known{true};
    std::uint64_t owner{};
    // Last outcome and stream shape written to the capture log: one line per change.
    std::uint64_t noted{};std::array<std::uint64_t,5> described{};
    std::unique_ptr<D3D11GuideTransport> transport;
    std::mutex mutex;
};
using Create=NVSDK_NGX_Result(*)(ID3D11DeviceContext*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate=NVSDK_NGX_Result(*)(ID3D11DeviceContext*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,void*);
using Release=NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
struct Module {SafetyHookInline create,evaluate,release;Create make{};Evaluate eval{};Release retire{};HMODULE retained{};bool typed_implementation{};};
struct Registry {
    std::mutex mutex;std::array<Module,3> modules;
    std::unordered_map<const NVSDK_NGX_Handle*,std::shared_ptr<Feature>> features;
    SafetyHookInline copy,region;
    using Copy=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,ID3D11Resource*);
    using Region=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,ID3D11Resource*,UINT,UINT,UINT,UINT,ID3D11Resource*,UINT,const D3D11_BOX*);
    Copy original_copy{};Region original_region{};
};
Registry& registry(){static auto* r=new Registry;return *r;}
thread_local unsigned depth{},outer_module{};
thread_local bool nested_evaluation{};
void barrier(D3D11GuideTransport& t,D3D11GuideTransport::Role role,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if(!t.resource(role)||from==to)return;
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource=t.resource(role);b.Transition.StateBefore=from;b.Transition.StateAfter=to;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;t.list()->ResourceBarrier(1,&b);
}
void nr_guides(D3D11GuideTransport& t,bool enter) {
    for(auto role:{D3D11GuideTransport::color,D3D11GuideTransport::motion,D3D11GuideTransport::depth})
        barrier(t,role,enter?D3D12_RESOURCE_STATE_COMMON:D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            enter?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COMMON);
}
void STDMETHODCALLTYPE copy(ID3D11DeviceContext* ctx,ID3D11Resource* dst,ID3D11Resource* src) {
    registry().original_copy(ctx,dst,src);propagate_d3d11_guide_receipt(dst,src);
}
void STDMETHODCALLTYPE region(ID3D11DeviceContext* ctx,ID3D11Resource* dst,UINT sub,UINT x,UINT y,UINT z,
    ID3D11Resource* src,UINT source_sub,const D3D11_BOX* box) {
    registry().original_region(ctx,dst,sub,x,y,z,src,source_sub,box);
    if(!sub&&!source_sub&&!x&&!y&&!z&&!box)propagate_d3d11_guide_receipt(dst,src);
}
void observe_copies(ID3D11DeviceContext* ctx) {
    auto& r=registry();std::scoped_lock lock(r.mutex);auto** table=*reinterpret_cast<void***>(ctx);
    if(!r.copy){r.copy=safetyhook::create_inline(table[47],reinterpret_cast<void*>(&copy),SafetyHookInline::StartDisabled);
        if(r.copy){r.original_copy=r.copy.original<Registry::Copy>();(void)r.copy.enable();}}
    if(!r.region){r.region=safetyhook::create_inline(table[46],reinterpret_cast<void*>(&region),SafetyHookInline::StartDisabled);
        if(r.region){r.original_region=r.region.original<Registry::Region>();(void)r.region.enable();}}
}
template<unsigned Slot>NVSDK_NGX_Result create(ID3D11DeviceContext* context,NVSDK_NGX_Feature type,
    NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** output) {
    const auto result=registry().modules[Slot].make(context,type,p,output);
    if(NVSDK_NGX_SUCCEED(result)&&p&&output&&*output) {
        auto f=std::make_shared<Feature>();f->type=type;f->identity=allocate_handle_key();
        f->flags=get<int>(p,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);
        f->flags_known=NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&f->flags));
        f->width=get<UINT>(p,NVSDK_NGX_Parameter_Width);f->height=get<UINT>(p,NVSDK_NGX_Parameter_Height);
        f->out_width=get<UINT>(p,NVSDK_NGX_Parameter_OutWidth);f->out_height=get<UINT>(p,NVSDK_NGX_Parameter_OutHeight);
        capture_log("d3d11_feature_create",pack(Slot,static_cast<std::uint32_t>(type)),static_cast<std::uint32_t>(f->flags),
            pack(f->width,f->height),pack(f->out_width,f->out_height));
        std::scoped_lock lock(registry().mutex);registry().features[*output]=f;
    }return result;
}
template<unsigned Slot>NVSDK_NGX_Result evaluate(ID3D11DeviceContext* context,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p,void* callback) {
    auto raw=[&]{return registry().modules[Slot].eval(context,handle,p,callback);};
    if(!p||!context||!handle)return raw();
    std::shared_ptr<Feature> f;{std::scoped_lock lock(registry().mutex);
        if constexpr(Slot<2)if(registry().modules[Slot].typed_implementation&&!registry().features.contains(handle)) {
            auto recovered=std::make_shared<Feature>();recovered->type=Slot==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
            const auto flags=NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&recovered->flags));
            recovered->width=get<UINT>(p,NVSDK_NGX_Parameter_Width);recovered->height=get<UINT>(p,NVSDK_NGX_Parameter_Height);
            recovered->out_width=get<UINT>(p,NVSDK_NGX_Parameter_OutWidth);recovered->out_height=get<UINT>(p,NVSDK_NGX_Parameter_OutHeight);
            if(flags&&recovered->width&&recovered->height&&recovered->out_width&&recovered->out_height) {
                recovered->identity=allocate_handle_key();registry().features.emplace(handle,recovered);
                capture_log("d3d11_feature_recovered",pack(Slot,static_cast<std::uint32_t>(recovered->type)),static_cast<std::uint32_t>(recovered->flags),
                    pack(recovered->width,recovered->height),pack(recovered->out_width,recovered->out_height));
            }
        }
        auto it=registry().features.find(handle);
        if(it!=registry().features.end())f=it->second;}
    if(!f||(f->type!=NVSDK_NGX_Feature_SuperSampling&&f->type!=NVSDK_NGX_Feature_RayReconstruction))return raw();
    struct Scope{Scope(){++depth;}~Scope(){--depth;}} scope;
    if(depth!=1){if(Slot==outer_module)nested_evaluation=true;return raw();}outer_module=Slot;nested_evaluation=false;
    observe_upscaler_inputs(get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_MotionVectors) != nullptr,
        get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_Depth) != nullptr);
    // Warm AFW transports survive menu changes for safe GPU ownership. They
    // must not keep doing interop copies when HFW returns to both generators
    // Off. Preserve observation and the game's original DLSS evaluation.
    if (!embedded::snapshot().desired.enabled && embedded::afw_snapshot().desired.mode==0 &&
        !neural_rendering::status().enabled && luke_hfw_producer_supported()) return raw();
    if(luke_detected()&&!f->flags_known)return raw();
    auto ctx=native(context);if(!f||!ctx||ctx->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE)return raw();
    std::scoped_lock lock(f->mutex);
    // The evaluation's outcome, logged when it changes: code, HRESULT, detail.
    const auto note=[&](std::uint32_t code,HRESULT hr,std::uint64_t detail=0) {
        const auto key=pack(code,static_cast<std::uint32_t>(hr));if(f->noted==key)return;f->noted=key;
        capture_log("d3d11_evaluate",code,static_cast<std::uint32_t>(hr),detail,reinterpret_cast<std::uintptr_t>(handle));
    };
    ComPtr<ID3D11Device> game_device;ctx->GetDevice(&game_device);
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;std::uint64_t generation{};
    if(const auto hr=acquire_d3d11_guide_owner(game_device.Get(),&device,&queue,&generation);FAILED(hr)){note(1,hr);return raw();}
    observe_copies(ctx.Get());configure_queue(queue.Get());
    if(f->owner!=generation){f->transport.reset();f->owner=generation;}
    if(!f->transport){f->transport=std::make_unique<D3D11GuideTransport>();
        if(const auto hr=f->transport->initialize(ctx.Get(),device.Get(),queue.Get());FAILED(hr)){f->transport.reset();note(2,hr);return raw();}}
    auto& t=*f->transport;
    auto color=native(get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_Color));
    auto output=native(get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_Output));
    auto motion=native(get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_MotionVectors));
    auto depth_texture=native(get<ID3D11Resource*>(p,NVSDK_NGX_Parameter_Depth));
    // Depth is optional (0.2 V505): vectors alone drive Hybrid, as in Ready
    // or Not on 0.2. Luke Ross titles keep requiring it.
    if(!output||!motion||(!depth_texture&&luke_detected())) {
        note(3,S_OK,(output?1U:0U)|(motion?2U:0U)|(depth_texture?4U:0U));return raw();
    }
    if(const auto hr=t.begin();FAILED(hr)){note(4,hr);return raw();}
    const auto fail=[&]{(void)t.submit();return raw();};
    if(const auto hr=t.copy(D3D11GuideTransport::motion,motion.Get());FAILED(hr)){note(5,hr,describe(motion.Get()));return fail();}
    const HRESULT depth_result=depth_texture?t.copy(D3D11GuideTransport::depth,depth_texture.Get()):E_POINTER;
    if(FAILED(depth_result)&&luke_detected()){note(6,depth_result,describe(depth_texture.Get()));return fail();}
    if(color)if(const auto hr=t.copy(D3D11GuideTransport::color,color.Get());FAILED(hr)){note(7,hr,describe(color.Get()));return fail();}
    Parameters shadow(p,&t,SUCCEEDED(depth_result));
    DlssMotionVectorPublication g{};g.stream=reinterpret_cast<std::uintptr_t>(handle);
    g.producer_queue=queue.Get();g.verified_producer_device=device.Get();
    g.motion_vectors=t.resource(D3D11GuideTransport::motion);
    g.depth=SUCCEEDED(depth_result)?t.resource(D3D11GuideTransport::depth):nullptr;
    g.output_width=f->out_width;g.output_height=f->out_height;
    const bool low=(f->flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0;
    const auto render_width=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,f->width);
    const auto render_height=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,f->height);
    g.motion_width=low?render_width:f->out_width;g.motion_height=low?render_height:f->out_height;
    g.depth_width=render_width;g.depth_height=render_height;
    g.output_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);g.output_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
    g.motion_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);g.motion_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    g.depth_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);g.depth_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    g.scale_x=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_X,1);g.scale_y=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_Y,1);
    g.jitter_x=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_X);g.jitter_y=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_Y);
    g.reset=get<int>(p,NVSDK_NGX_Parameter_Reset)!=0;g.jittered=(f->flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
    g.depth_inverted=(f->flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0;
    g.frame_time_delta_ms=get<float>(p,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667F);
    const bool exact=apply_camera(g,nullptr);if(luke_detected()&&!exact){note(8,S_OK);return fail();}
    // The stream's shape (V520's lines), once per change of size or rectangle.
    if(const std::array<std::uint64_t,5> shape{pack(g.output_width,g.output_height),pack(g.motion_width,g.motion_height),
            pack(g.output_x,g.output_y),pack(g.motion_x,g.motion_y),pack(g.depth_x,g.depth_y)};f->described!=shape) {
        f->described=shape;
        capture_log("d3d11_stream",static_cast<std::uint32_t>(f->flags),shape[0],shape[1],pack(bits(g.scale_x),bits(g.scale_y)));
        capture_log("d3d11_stream_rects",shape[2],shape[3],shape[4],pack(g.depth_width,g.depth_height));
        capture_log("d3d11_stream_textures",extent(motion.Get()),extent(depth_texture.Get()),extent(output.Get()),
            describe(motion.Get()));
    }
    nr_guides(t,true);
    if(color)neural_rendering::before_sr(f->identity,t.list(),&shadow,queue.Get());
    nr_guides(t,false);
    if(FAILED(t.submit()))return raw();
    if(color&&FAILED(t.copy_back(D3D11GuideTransport::color,color.Get())))return raw();
    const auto result=raw();if(NVSDK_NGX_FAILED(result))return result;
    if(FAILED(t.begin()))return result;
    if(FAILED(t.copy(D3D11GuideTransport::output,output.Get()))){(void)t.submit();return result;}
    g.output=t.resource(D3D11GuideTransport::output);g.producer_command_list=t.list();
    g.resource_state=g.depth_resource_state=g.output_resource_state=D3D12_RESOURCE_STATE_COMMON;
    g.direct_output_receipt=!g.protect_snapshot_lifetime;
    const auto frame=publish_dlss_motion_vectors(g);
    // 9 refused; 10 published with depth (detail: depth's flags and format,
    // unpacked if packed); 11 published without (hr: why the depth failed).
    note(!frame?9:g.depth?10:11,depth_result,describe(depth_texture.Get()));
    nr_guides(t,true);
    barrier(t,D3D11GuideTransport::output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    neural_rendering::after_sr(f->identity,t.list(),&shadow,queue.Get());
    barrier(t,D3D11GuideTransport::output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    nr_guides(t,false);
    if(frame&&g.protect_snapshot_lifetime&&embedded::afw_snapshot().desired.mode!=0&&!nested_evaluation)
        mark_luke_color_origin(t.list(),g.output,frame->color_origin_token,D3D12_RESOURCE_STATE_COMMON,
            device.Get(),{g.output_x,g.output_y,g.output_width,g.output_height});
    if(SUCCEEDED(t.submit())&&SUCCEEDED(t.copy_back(D3D11GuideTransport::output,output.Get()))&&frame) {
        set_d3d11_guide_receipt(output.Get(),{generation,g.stream,frame->publication,g.output_width,g.output_height});
        (void)claim_d3d11_guides(game_device.Get(),generation);
    }
    return result;
}
template<unsigned Slot>NVSDK_NGX_Result release(NVSDK_NGX_Handle* handle) {
    unsigned id{};{std::scoped_lock lock(registry().mutex);auto it=registry().features.find(handle);if(it!=registry().features.end())id=it->second->identity;}const auto stream=reinterpret_cast<std::uintptr_t>(handle);
    const auto result=registry().modules[Slot].retire(handle);
    if(NVSDK_NGX_SUCCEED(result)){std::shared_ptr<Feature> feature;
        {std::scoped_lock lock(registry().mutex);auto it=registry().features.find(handle);
        if(it!=registry().features.end()){feature=it->second;registry().features.erase(it);}}
        if(feature){std::scoped_lock lock(feature->mutex);retire_dlss_motion_vector_stream(stream);neural_rendering::retire_handle(id);}}
    return result;
}
template<unsigned Slot>void install(HMODULE module) {
    auto& m=registry().modules[Slot];if(!module||m.retained)return;
    auto c=GetProcAddress(module,"NVSDK_NGX_D3D11_CreateFeature"),e=GetProcAddress(module,"NVSDK_NGX_D3D11_EvaluateFeature"),
        r=GetProcAddress(module,"NVSDK_NGX_D3D11_ReleaseFeature");if(!c||!e||!r)return;
    HMODULE implementation{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(e),&implementation);
    m.typed_implementation=Slot<2&&implementation==module;
    m.create=safetyhook::create_inline(c,reinterpret_cast<void*>(&create<Slot>),SafetyHookInline::StartDisabled);
    m.evaluate=safetyhook::create_inline(e,reinterpret_cast<void*>(&evaluate<Slot>),SafetyHookInline::StartDisabled);
    m.release=safetyhook::create_inline(r,reinterpret_cast<void*>(&release<Slot>),SafetyHookInline::StartDisabled);
    if(!m.create||!m.evaluate||!m.release){m.create={};m.evaluate={};m.release={};return;}
    m.make=m.create.original<Create>();m.eval=m.evaluate.original<Evaluate>();m.retire=m.release.original<Release>();
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(e),&m.retained);
    (void)m.create.enable();(void)m.evaluate.enable();(void)m.release.enable();
}
}
void poll_d3d11_ngx() noexcept {try {
    install<0>(GetModuleHandleW(L"nvngx_dlss.dll"));install<1>(GetModuleHandleW(L"nvngx_dlssd.dll"));
    install<2>(GetModuleHandleW(L"_nvngx.dll"));
}catch(...) {}}
}
