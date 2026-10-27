#include "xrfg/ngx_guide_capture.hpp"
#include "xrfg/upscaler_observation.hpp"
#ifdef XRFG_NATIVE_DLSSG
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/third_party/uevr_api.h"
#include "xrfg/native_ingress.hpp"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/native_ingress/DeadSpaceCameraSource.h"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_native_motion_interval.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "xrfg/neural_rendering.hpp"
#include "xrfg/d3d12_native_binding.hpp"
#include "xrfg/vulkan_native_ingress.hpp"
#include "xrfg/ngx_feature_metadata.hpp"
#include "xrfg/native_ingress/LukeNativeSceneCamera.h"
#include "xrfg/native_export_address.hpp"
#include "xrfg/luke_dispatch_module.hpp"
#include "xrfg/ngx_feature_paths.hpp"
#include "xrfg/ngx_evaluation_boundary.hpp"
#include <windows.h>
#include <wrl/client.h>
#include <nvsdk_ngx_params.h>
#include <safetyhook.hpp>
#include <d3d12sdklayers.h>
#include <array>
#include <bit>
#include <algorithm>
#include <climits>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <share.h>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef XRFG_NATIVE_INGRESS_TESTS
#include <stdexcept>
#include <iostream>
#include <fstream>
#include <dxgi1_6.h>
#include "xrfg/native_ingress/streamline1/sl1.h"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "xrfg/d3d11_dlss_guides.hpp"
#endif

namespace xrfg { namespace {
using Microsoft::WRL::ComPtr;
using Create = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Release = NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
using Evaluate = NVSDK_NGX_Result(*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*, void*);
using Feature = NgxFeatureMetadata;
struct Module {
    SafetyHookInline create_hook, evaluate_hook, evaluate_c_hook, release_hook;
    Create create{}; Evaluate evaluate{}, evaluate_c{}; Release release{};
    HMODULE retained{};
    bool typed_implementation{};
};
struct OutlawsPublicLifetime {
    std::uint32_t identity{};
    ComPtr<IUnknown> device;
    Feature guide_contract{};
    bool guide_contract_known{};
    std::uint64_t failure_epoch{};
    unsigned active_calls{};
};
struct Capture {
    std::mutex mutex, log_mutex;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<Module,5> modules;
    std::unordered_map<const NVSDK_NGX_Handle*,Feature> features;
    std::unordered_map<const NVSDK_NGX_Handle*,std::uint64_t> native_production_sequences;
    std::unordered_map<const NVSDK_NGX_Handle*,std::uint64_t> guide_streams;
    std::unordered_map<std::uint64_t,std::uint64_t> public_production_sequences;
    std::unordered_map<const NVSDK_NGX_Handle*,std::shared_ptr<OutlawsPublicLifetime>> outlaws_public_lifetimes;
    std::atomic_bool active{false}, started{false}, camera_ready{false};
    std::atomic_uint64_t calls{0};
    FILE* log{};
    ComPtr<ID3D12InfoQueue1> debug_queue;
    DWORD debug_cookie{};
    float camera_near{}, camera_far{};
    bool depth_inverted{}, depth_infinite{}, camera_valid{}, camera_from_xr{};
};
// Hooks retain their trampolines and pin this module for process lifetime.
// No CRT static destructor tries to unhook under the Windows loader lock.
Capture& state() { static auto* s=new Capture; return *s; }
constexpr std::uint64_t kKcdPublicStreamNamespace=std::uint64_t{1}<<63;
bool public_guide_stream(std::uint64_t stream) noexcept {
    return (stream&kKcdPublicStreamNamespace)!=0;
}
void log(const char* message, UINT64 a=0, UINT64 b=0, UINT64 c=0, UINT64 d=0) {
    if(!bridge_flight_logger().enabled() && !bridge_flight_logger().ram_enabled())return;
    auto& s=state(); std::scoped_lock lock(s.log_mutex);
    if(!s.log) {
        wchar_t dir[MAX_PATH]{}; GetModuleFileNameW(
            reinterpret_cast<HMODULE>([] { HMODULE m{}; GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&state),&m); return m; }()),dir,MAX_PATH);
        auto* slash=wcsrchr(dir,L'\\'); if(!slash)return; *slash=0;
        wchar_t path[MAX_PATH]{}; swprintf_s(path,L"%s\\ofxr-ngx-capture-pid%lu.log",dir,GetCurrentProcessId());
        s.log=_wfsopen(path,L"wb",_SH_DENYWR);
    }
    if(s.log) { fprintf(s.log,"%llu %s a=%llu b=%llu c=%llu d=%llu\n",
        GetTickCount64(),message,a,b,c,d); fflush(s.log); }
}
template<class T> T get(const NVSDK_NGX_Parameter* p,const char* name,T fallback={}) {
    T value=fallback;
    if(!p||NVSDK_NGX_FAILED(p->Get(name,&value)))return fallback;
    return value;
}
#ifdef XRFG_NATIVE_INGRESS_TESTS
bool test_kcd_production_identity{};
bool test_kcd_scene_camera{};
bool test_outlaws_production_identity{};
bool test_crimson_native_motion{};
bool test_ghost_native_motion{};
bool test_horizon_native_motion{};
bool test_dead_space_production_identity{};
bool test_gta_enhanced_production_identity{};
#endif
bool dead_space_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"Dead Space.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected || test_dead_space_production_identity;
#else
    return selected;
#endif
}
bool gta_enhanced_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"GTA5_Enhanced.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected||test_gta_enhanced_production_identity;
#else
    return selected;
#endif
}
bool cached_luke_title() {return dead_space_process()||gta_enhanced_process();}
void title_capture_log(const char* message,UINT64 a=0,UINT64 b=0,UINT64 c=0,UINT64 d=0) {
    if(gta_enhanced_process()&&strncmp(message,"dead_space_",11)==0) {
        char label[MAX_PATH+40]{};snprintf(label,sizeof(label),"gta_enhanced_%s",message+11);
        log(label,a,b,c,d);
    } else log(message,a,b,c,d);
}
std::atomic_uint64_t dead_space_capture_report_at{};
bool sample_dead_space_capture() noexcept {
    if(!bridge_flight_logger().enabled() || !cached_luke_title())return false;
    const auto now=GetTickCount64();auto previous=dead_space_capture_report_at.load(std::memory_order_relaxed);
    return now-previous>=1000 && dead_space_capture_report_at.compare_exchange_strong(previous,now,std::memory_order_relaxed);
}
TwoFrameGuideContract dead_space_rejected_camera_contract(
    const native_ingress::streamline_camera::Camera& camera,const DlssMotionVectorPublication& guides) {
    TwoFrameGuideContract out{};out.stage="dead_space_rejected_camera";
    out.stream=guides.stream;out.serial=camera.frame;out.native_camera_token=camera.frame;
    out.publication=0;out.mode=-static_cast<int>(camera.rejection); // diagnostic mask, not an AFW mode
    out.camera_position=camera.position;out.camera_right=camera.right;
    out.camera_up=camera.up;out.camera_forward=camera.forward;
    out.view_to_clip=out.camera_view_to_clip=camera.projection;
    out.clip_to_view=out.camera_clip_to_view=camera.inverse;
    out.raw_clip_to_previous_clip=camera.previous;out.raw_previous_clip_to_clip=camera.to_current;
    out.camera_near=camera.near_z;out.camera_far=camera.far_z;out.camera_fov_vertical=camera.fov;
    out.raw_camera_motion_included=camera.camera_motion;out.depth_inverted=camera.inverted;
    out.motion_width=guides.motion_width;out.motion_height=guides.motion_height;
    out.depth_width=guides.depth_width;out.depth_height=guides.depth_height;
    out.output_width=guides.output_width;out.output_height=guides.output_height;
    return out;
}
// One sampled capture attempt per second, including silent pre-publication
// exits. Only reads existing receipts; never supplies a queue or camera.
void report_dead_space_capture(unsigned reason,std::uint64_t call,UINT64 detail=0) {
    if(dead_space_process()) {
        const auto source=dead_space_camera_source::statistics();
        title_capture_log("dead_space_native_source",source.status,source.failure,source.observed,source.corrected);
        title_capture_log("dead_space_native_source_rejections",source.matched,source.rejected);
    }
    title_capture_log("dead_space_capture_gate",reason,call,detail,state().active.load());
    const auto context=native_ingress::luke_context_diagnostics();
    title_capture_log("dead_space_queue_state",context.admitted|(context.execute_hook?2U:0U)|(context.create_queue_hook?4U:0U),
        (UINT64(context.pending_lists)<<32)|context.registered_queues,context.submissions,context.execute_entry);
    title_capture_log("dead_space_queue_owner",context.device,context.queue);
    static std::atomic_uintptr_t reported_execute{};
    if(context.execute_entry && reported_execute.exchange(context.execute_entry)!=context.execute_entry) {
        HMODULE module{};char path[MAX_PATH]{};
        if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(context.execute_entry),&module) && GetModuleFileNameA(module,path,MAX_PATH)) {
            const auto* basename=strrchr(path,'\\');basename=basename?basename+1:path;
            char label[MAX_PATH+40]{};snprintf(label,sizeof(label),"dead_space_execute_module:%s",basename);
            title_capture_log(label,context.execute_entry-reinterpret_cast<std::uintptr_t>(module));
        }
    }
    const auto camera=native_ingress::streamline_camera::current(true);
    title_capture_log("dead_space_camera_receipt",native_ingress::streamline_camera::hooked(),
        camera.observed|(camera.valid?2U:0U)|(camera.ambiguous?4U:0U),camera.rejection,
        (UINT64(camera.frame)<<32)|camera.viewport);
    const auto bits=[](float x){return std::bit_cast<std::uint32_t>(x);};
    const auto vector=[&](const char* label,const auto& v){title_capture_log(label,bits(v[0]),bits(v[1]),bits(v[2]));};
    vector("dead_space_camera_right",camera.right);vector("dead_space_camera_up",camera.up);
    vector("dead_space_camera_forward",camera.forward);vector("dead_space_camera_position",camera.position);
    title_capture_log("dead_space_camera_planes_fov",bits(camera.near_z),bits(camera.far_z),bits(camera.fov),camera.camera_motion+1);
    for(unsigned row=0;row<4;++row) {
        char label[48]{};snprintf(label,sizeof(label),"dead_space_camera_projection_%u",row);
        title_capture_log(label,bits(camera.projection[row*4]),bits(camera.projection[row*4+1]),bits(camera.projection[row*4+2]),bits(camera.projection[row*4+3]));
        snprintf(label,sizeof(label),"dead_space_camera_inverse_%u",row);
        title_capture_log(label,bits(camera.inverse[row*4]),bits(camera.inverse[row*4+1]),bits(camera.inverse[row*4+2]),bits(camera.inverse[row*4+3]));
    }
    const auto guides=dlss_motion_vector_statistics();
    title_capture_log("dead_space_publication_state",static_cast<unsigned>(guides.status),guides.published,guides.matched,guides.used);
}
bool kingdom_come_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"KingdomCome.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected || test_kcd_production_identity;
#else
    return selected;
#endif
}
bool kcd_scene_camera_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"KingdomCome.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected || test_kcd_scene_camera;
#else
    return selected;
#endif
}
bool outlaws_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"Outlaws.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected || test_outlaws_production_identity;
#else
    return selected;
#endif
}
bool crimson_desert_process() {
    static const bool selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return !_wcsicmp(basename,L"CrimsonDesert.exe");
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    return selected || test_crimson_native_motion;
#else
    return selected;
#endif
}
std::atomic_bool capture_steamvr_runtime{};
bool same_eye_native_motion() noexcept {
    static const auto selected=[] {
        wchar_t process[MAX_PATH]{};GetModuleFileNameW(nullptr,process,MAX_PATH);
        const auto* basename=wcsrchr(process,L'\\');basename=basename?basename+1:process;
        return luke_same_eye_native_motion_title(basename);
    }();
#ifdef XRFG_NATIVE_INGRESS_TESTS
    const bool same_eye=selected||test_ghost_native_motion||test_horizon_native_motion||
        test_gta_enhanced_production_identity||test_dead_space_production_identity;
#else
    const bool same_eye=selected;
#endif
    // Quest/VDXR captures establish the same N-2 endpoint on these titles:
    // Tsushima V629 .17-.20px; Horizon V632 .011-.014px (N-1 1.88-3.03px).
    // The native motion field interval does not depend on the XR runtime.
    // Spider-Man V664 F8: N-2 median .0009/.0022px versus N-1 2.30/1.28px.
    // GTA Enhanced V679: N-2/native view rows .011-.020px; N-1 5.8-11.2px.
    // Dead Space V680 F8: native N-2 matches same-eye image flow at .30/.32px;
    // interpreting it as N-1 introduces an erroneous stereo interval.
    return same_eye;
}
bool outlaws_public_process() {
    return outlaws_process()&&native_ingress::luke_detected();
}
class KcdCameraInvocationScope;
template<class T> ComPtr<T> native(T* object);
bool same(IUnknown*,IUnknown*);
std::shared_ptr<const DlssMotionVectorFrame> capture(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,std::uint64_t,std::uint64_t,KcdCameraInvocationScope*,std::uint32_t,bool*,const Feature*,
    ID3D12Resource* = nullptr,unsigned = 0);
// REAL VR's public handle can predate interception and own two physical-eye
// RR handles. Its exact synchronous invocation proves production ownership,
// independently of any creation dimensions or flags. A late lifetime is never
// inserted into feature metadata or used to classify a generic NGX feature.
class OutlawsPublicInvocationScope {
    inline static thread_local OutlawsPublicInvocationScope* current_{};
    OutlawsPublicInvocationScope* parent_{};
    const NVSDK_NGX_Handle* handle_{};
    const void* list_{};
    const void* device_{};
    std::shared_ptr<OutlawsPublicLifetime> lifetime_;
    ComPtr<ID3D12GraphicsCommandList> public_command_;
    ComPtr<ID3D12Resource> public_output_;
    const NVSDK_NGX_Parameter* public_parameters_{};
    Feature prepared_contract_{},native_contract_{};
    std::uint64_t predicted_sequence_{};
    std::uint64_t failure_epoch_{};
    bool leased_{};
    bool prepared_{},contract_known_{},snapshot_attempted_{};
    std::shared_ptr<const DlssMotionVectorFrame> public_frame_;
    std::shared_ptr<const DlssMotionVectorFrame> native_frame_;
    unsigned claims_{};
    bool active_{},rejected_{},completed_{},native_completed_{},native_success_{},native_hdr_{};
    void reject() noexcept {
        if(rejected_)return;
        rejected_=true;
        if(lifetime_) {
            const auto stream=kKcdPublicStreamNamespace|lifetime_->identity;
            // A failed enclosing call can leave the typed guide copy recorded
            // on its original list. Pin those destinations until layer idle.
            if(prepared_||native_frame_)retire_dlss_motion_vector_stream(stream);
            else invalidate_dlss_motion_vector_stream_history(stream);
        }
    }
    bool live_locked() const noexcept {
        const auto found=state().outlaws_public_lifetimes.find(handle_);
        return lifetime_&&found!=state().outlaws_public_lifetimes.end()&&found->second==lifetime_&&
            lifetime_->failure_epoch==failure_epoch_&&lifetime_->active_calls==1;
    }
    HRESULT mark_public_color() {
        // This is authorized only by this exact successful public invocation
        // and its unique typed RR child. Ordinary marker completion still
        // requires color == frame.output; no global resource alias is added.
        if(!native_frame_||!public_command_||!public_output_)return E_INVALIDARG;
        const auto& frame=*native_frame_;
        ComPtr<IUnknown> owner,list;
        if(!frame.verified_producer_device||FAILED(frame.verified_producer_device.As(&owner))||
            owner.Get()!=device_||FAILED(public_command_.As(&list))||list.Get()!=list_||
            frame.stream!=(kKcdPublicStreamNamespace|lifetime_->identity)||
            !frame.native_production_sequence||!frame.color_origin_token)return E_INVALIDARG;
        const auto desc=public_output_->GetDesc();
        if(!frame.output_width||!frame.output_height||frame.output_slice||
            std::uint64_t(frame.output_x)+frame.output_width>desc.Width||
            std::uint64_t(frame.output_y)+frame.output_height>desc.Height)return E_INVALIDARG;
        const auto view=native_hdr_&&desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS?DXGI_FORMAT_R16G16B16A16_FLOAT:
            native_hdr_&&desc.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_UNKNOWN;
        return mark_luke_color_origin(public_command_.Get(),public_output_.Get(),frame.color_origin_token,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,frame.verified_producer_device.Get(),
            {frame.output_x,frame.output_y,frame.output_width,frame.output_height},view);
    }
public:
    OutlawsPublicInvocationScope(unsigned slot,const NVSDK_NGX_Handle* handle,
        const void* list,IUnknown* device,ID3D12GraphicsCommandList* command,
        const NVSDK_NGX_Parameter* parameters) noexcept {
        if(slot!=3||!outlaws_public_process())return;
        active_=true;parent_=current_;current_=this;
        handle_=handle;list_=list;device_=device;public_parameters_=parameters;
        if(parent_) {parent_->reject();rejected_=true;return;}
        if(!handle||!list||!device||!command) {rejected_=true;return;}
        try {
            IUnknown* borrowed{};
            if(!d3d_native::get_borrowed(get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_Output),&borrowed)||
                FAILED(borrowed->QueryInterface(IID_PPV_ARGS(public_output_.GetAddressOf())))) {reject();return;}
            public_command_=command;
            ComPtr<ID3D12Device> reported,canonical;
            ComPtr<IUnknown> output_owner,command_identity;
            if(FAILED(public_output_->GetDevice(IID_PPV_ARGS(reported.GetAddressOf())))||
                FAILED(d3d_native::allocation_owner(reported.Get(),canonical))||
                FAILED(canonical.As(&output_owner))||output_owner.Get()!=device||
                FAILED(public_command_.As(&command_identity))||command_identity.Get()!=list) {reject();return;}
            std::scoped_lock lock(state().mutex);
            const auto found=state().outlaws_public_lifetimes.find(handle);
            if(found!=state().outlaws_public_lifetimes.end()) {
                lifetime_=found->second;
                ++lifetime_->active_calls;leased_=true;failure_epoch_=lifetime_->failure_epoch;
                if(lifetime_->active_calls>1) {
                    ++lifetime_->failure_epoch;rejected_=true;
                    // The other thread may still be recording a partial copy.
                    // Retire its whole pool before any provisional reuse.
                    retire_dlss_motion_vector_stream(kKcdPublicStreamNamespace|lifetime_->identity);
                } else if(lifetime_->device.Get()!=device)reject();
                return;
            }
            constexpr std::size_t capacity=128;
            if(state().outlaws_public_lifetimes.size()>=capacity) {rejected_=true;return;}
            auto lifetime=std::make_shared<OutlawsPublicLifetime>();
            lifetime->identity=native_ingress::allocate_handle_key();
            if(!lifetime->identity) {rejected_=true;return;}
            lifetime->device=device;
            const auto creator=state().features.find(handle);
            if(creator!=state().features.end()&&creator->second.creation_observed&&
                creator->second.creation_source_identity==creator->second.identity&&
                creator->second.type==NVSDK_NGX_Feature_RayReconstruction&&creator->second.flags_known) {
                lifetime->guide_contract=creator->second;lifetime->guide_contract_known=true;
            }
            state().outlaws_public_lifetimes.emplace(handle,lifetime);
            lifetime_=std::move(lifetime);lifetime_->active_calls=1;leased_=true;
        }catch(...) {rejected_=true;}
    }
    ~OutlawsPublicInvocationScope() {
        if(!active_)return;
        if(!completed_)reject();
        if(leased_) {
            std::scoped_lock lock(state().mutex);
            if(lifetime_->active_calls)--lifetime_->active_calls;
        }
        current_=parent_;
    }
    OutlawsPublicInvocationScope(const OutlawsPublicInvocationScope&)=delete;
    void prepare_public_frame() noexcept {
        if(!active_||parent_||rejected_||prepared_||!lifetime_)return;
        prepared_=true;
        try {
            {
                std::scoped_lock lock(state().mutex);
                if(!live_locked()) {reject();return;}
                const auto stream=kKcdPublicStreamNamespace|lifetime_->identity;
                const auto found=state().public_production_sequences.find(stream);
                const auto sequence=found==state().public_production_sequences.end()?0:found->second;
                if(sequence==UINT64_MAX) {reject();return;}
                predicted_sequence_=sequence+1;
                contract_known_=lifetime_->guide_contract_known;
                prepared_contract_=lifetime_->guide_contract;
            }
            // Late public creation is not reconstructed. The first genuine
            // typed RR child supplies a receipt for the next scoped call;
            // that cold production still counts as a real history gap.
            if(!contract_known_)return;
            // The exact typed receipt supplies feature type/flags only here.
            // Prior native creation dimensions cannot select the original
            // caller's window; use its explicit subrect or actual allocations.
            auto provisional=prepared_contract_;
            provisional.width=provisional.height=provisional.out_width=provisional.out_height=0;
            provisional.creation_observed=false;provisional.creation_source_identity=0;
            public_frame_=capture(public_command_.Get(),handle_,public_parameters_,predicted_sequence_,
                kKcdPublicStreamNamespace|lifetime_->identity,nullptr,0,&snapshot_attempted_,&provisional);
            if(!public_frame_&&snapshot_attempted_)reject();
            {
                std::scoped_lock lock(state().mutex);
                if(!live_locked())reject();
            }
        }catch(...) {reject();}
    }
    static bool consume_public_frame(const Feature& contract,const NVSDK_NGX_Parameter* parameters,
        std::uint64_t sequence,const void* list,const void* device,
        std::shared_ptr<const DlssMotionVectorFrame>& frame) noexcept {
        auto* owner=current_;if(!owner)return false;
        if(owner->rejected_||!owner->prepared_||owner->claims_!=1||owner->native_completed_||
            owner->list_!=list||owner->device_!=device||!sequence||sequence!=owner->predicted_sequence_||
            contract.type!=NVSDK_NGX_Feature_RayReconstruction||!contract.flags_known) {
            owner->reject();return true;
        }
        owner->native_contract_=contract;
        const auto discard_prepared=[&] {
            // Early GPU copies and provisional history are already recorded.
            // Keep their entire pool pinned before dropping the local frame;
            // the successful actual child can supply next-call metadata.
            retire_dlss_motion_vector_stream(kKcdPublicStreamNamespace|owner->lifetime_->identity);
            owner->public_frame_.reset();
        };
        if(owner->contract_known_&&(owner->prepared_contract_.type!=contract.type||
            owner->prepared_contract_.flags_known!=contract.flags_known||owner->prepared_contract_.flags!=contract.flags)) {
            // A changed native contract earns a new receipt after successful
            // completion, but cannot consume copies prepared with old flags.
            discard_prepared();return true;
        }
        if(owner->public_frame_) {
            auto output=native(get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_Output));
            if(!output) {owner->reject();return true;}
            const auto desc=output->GetDesc();const auto& captured=*owner->public_frame_;
            const unsigned x=get<UINT>(parameters,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
            const unsigned y=get<UINT>(parameters,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
            const auto width=std::min(ngx_display_dimension(contract.out_width,parameters,NVSDK_NGX_Parameter_OutWidth,
                UINT(desc.Width),contract.creation_observed),UINT(desc.Width-std::min<UINT64>(desc.Width,x)));
            const auto height=std::min(ngx_display_dimension(contract.out_height,parameters,NVSDK_NGX_Parameter_OutHeight,
                desc.Height,contract.creation_observed),desc.Height-std::min(desc.Height,y));
            if(captured.output_x!=x||captured.output_y!=y||captured.output_width!=width||captured.output_height!=height) {
                discard_prepared();return true;
            }
        }
        frame=owner->public_frame_;return true;
    }
    // An active public call must refuse mismatched or multiple native children,
    // rather than quietly reverting to an unrelated raw-handle publication.
    static bool claim(NVSDK_NGX_Feature type,const void* list,const void* device,
        std::uint32_t& identity) noexcept {
        identity=0;
        auto* owner=current_;if(!owner)return false;
        if(owner->rejected_||type!=NVSDK_NGX_Feature_RayReconstruction||
            owner->list_!=list||owner->device_!=device||owner->claims_++) {
            owner->reject();return true;
        }
        try {
            std::scoped_lock lock(state().mutex);
            if(!owner->live_locked()) {owner->reject();return true;}
            identity=owner->lifetime_->identity;
            state().guide_streams[owner->handle_]=kKcdPublicStreamNamespace|identity;
        }catch(...) {identity=0;owner->reject();}
        return true;
    }
    static void complete_native(NVSDK_NGX_Feature type,const void* list,const void* device,
        const std::shared_ptr<const DlssMotionVectorFrame>& frame,bool hdr,
        NVSDK_NGX_Result result,bool can_complete) noexcept {
        auto* owner=current_;if(!owner)return;
        if(owner->rejected_||owner->native_completed_||owner->claims_!=1||
            type!=NVSDK_NGX_Feature_RayReconstruction||owner->list_!=list||owner->device_!=device) {
            owner->reject();return;
        }
        owner->native_completed_=true;owner->native_frame_=frame;owner->native_hdr_=hdr;
        if(NVSDK_NGX_FAILED(result)||!can_complete) {owner->reject();return;}
        owner->native_success_=true;
    }
    NVSDK_NGX_Result complete(NVSDK_NGX_Result result) noexcept {
        if(!active_)return result;
        completed_=true;
        try {
            // Serialize final lifetime validation through the short marker
            // recording with observed public release/address reuse.
            std::scoped_lock lock(state().mutex);
            if(NVSDK_NGX_FAILED(result)||rejected_||claims_!=1||!native_success_||!live_locked())reject();
            else {
                lifetime_->guide_contract=native_contract_;
                lifetime_->guide_contract_known=native_contract_.flags_known&&
                    native_contract_.type==NVSDK_NGX_Feature_RayReconstruction;
                if(native_frame_&&embedded::afw_snapshot().desired.mode!=0) {
                const auto stream=kKcdPublicStreamNamespace|lifetime_->identity;
                const auto sequence=state().public_production_sequences.find(stream);
                if(sequence==state().public_production_sequences.end()||
                    sequence->second!=native_frame_->native_production_sequence||
                    mark_public_color()!=S_OK)reject();
                }
            }
        }catch(...) {reject();}
        return result;
    }
};
class KcdCameraInvocationScope;
std::shared_ptr<const DlssMotionVectorFrame> capture(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter*,std::uint64_t,std::uint64_t,KcdCameraInvocationScope*,std::uint32_t,bool*,const Feature* = nullptr,
    ID3D12Resource*,unsigned);
HRESULT complete_color(ID3D12GraphicsCommandList*,const NVSDK_NGX_Parameter*,
    const std::shared_ptr<const DlssMotionVectorFrame>&,bool);
HRESULT complete_color_resource(ID3D12GraphicsCommandList*,ID3D12Resource*,
    const std::shared_ptr<const DlssMotionVectorFrame>&,bool);
// Keep GTA's accepted typed-native N-2 guides. Only the color marker moves
// to the end of its exact enclosing public invocation; no "latest" lookup.
class GtaFinalColorScope {
    inline static thread_local GtaFinalColorScope* current_{};
    Feature creator_{};
    const NVSDK_NGX_Handle* handle_{};
    ComPtr<ID3D12GraphicsCommandList> command_;
    ComPtr<ID3D12Resource> output_;
    const void* list_{};
    const void* device_{};
    std::shared_ptr<const DlssMotionVectorFrame> frame_;
    bool active_{},completed_{},rejected_{},hdr_{};
    unsigned claims_{},native_claims_{};
    void reject(unsigned reason=1) noexcept {
        if(!rejected_)bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            -static_cast<std::int64_t>(reason),1196,creator_.identity,
            (std::uint64_t(claims_)<<32)|(frame_?frame_->color_origin_token:0));
        rejected_=true;
        if(frame_)invalidate_dlss_motion_vector_stream_history(frame_->stream);
    }
public:
    GtaFinalColorScope(bool public_call,const Feature& feature,const NVSDK_NGX_Handle* handle,
        ID3D12GraphicsCommandList* command,const NVSDK_NGX_Parameter* p,
        const void* list,const void* device) noexcept {
        if(!gta_enhanced_process()||!native_ingress::luke_detected()||!public_call)return;
        if(current_) {
            if(current_->creator_.identity!=feature.creation_source_identity||current_->list_!=list||current_->device_!=device)
                current_->reject();
            return;
        }
        if(!feature.creation_observed||!feature.identity||feature.creation_source_identity!=feature.identity)return;
        active_=true;current_=this;creator_=feature;handle_=handle;list_=list;device_=device;
        try {
            command_=native(command);output_=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Output));
            if(!command_||!output_||!list_||!device_)reject();
        }catch(...){reject();}
    }
    ~GtaFinalColorScope() {
        if(!active_)return;
        if(!completed_)reject();
        current_=nullptr;
    }
    static void record(const Feature& feature,const void* list,const void* device,
        const std::shared_ptr<const DlssMotionVectorFrame>& frame,bool hdr,
        NVSDK_NGX_Result result,bool can_complete) noexcept {
        auto* owner=current_;if(!owner)return;
        if(owner->claims_++||owner->rejected_||!feature.creation_observed||
            feature.creation_source_identity!=owner->creator_.identity||feature.type!=owner->creator_.type||
            list!=owner->list_||device!=owner->device_||!frame||frame->raw_motion_native_span!=1||
            NVSDK_NGX_FAILED(result)||!can_complete) {
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,feature.creation_observed,1197,
                (std::uint64_t(feature.identity)<<32)|feature.creation_source_identity,
                (std::uint64_t(frame?frame->raw_motion_native_span:0)<<32)|(can_complete?1:0));
            owner->reject(4);return;
        }
        owner->frame_=frame;owner->hdr_=hdr;
    }
    static void native_complete(const Feature& feature,const void* list,const void* device,
        ID3D12GraphicsCommandList* command,const NVSDK_NGX_Parameter* parameters,
        NVSDK_NGX_Result result) noexcept {
        auto* owner=current_;if(!owner)return;
        try {
            if(owner->native_claims_++||owner->rejected_||!owner->frame_||!feature.creation_observed||
                feature.creation_source_identity!=owner->creator_.identity||feature.type!=owner->creator_.type||
                list!=owner->list_||device!=owner->device_||NVSDK_NGX_FAILED(result)||
                !get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_MotionVectors)||
                !get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_Depth)) {owner->reject(8);return;}
            auto output=native(get<ID3D12Resource*>(parameters,NVSDK_NGX_Parameter_Output));
            auto cmd=native(command);ComPtr<ID3D12Device> actual,canonical;
            if(!output||!cmd||FAILED(output->GetDevice(IID_PPV_ARGS(&actual)))||
                FAILED(d3d_native::allocation_owner(actual.Get(),canonical))||
                !d3d_owner::same(canonical.Get(),owner->frame_->verified_producer_device.Get())) {owner->reject(9);return;}
            const auto desc=output->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
                desc.Width!=owner->frame_->output_width||desc.Height!=owner->frame_->output_height||
                owner->frame_->output_x||owner->frame_->output_y) {owner->reject(10);return;}
            const auto view=owner->hdr_&&desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS?DXGI_FORMAT_R16G16B16A16_FLOAT:
                owner->hdr_&&desc.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_UNKNOWN;
            const auto marked=embedded::afw_snapshot().desired.mode==0?S_FALSE:
                mark_luke_color_origin(cmd.Get(),output.Get(),owner->frame_->color_origin_token,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,owner->frame_->verified_producer_device.Get(),
                    {0,0,owner->frame_->output_width,owner->frame_->output_height},view);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,marked,1200,
                owner->frame_->publication,owner->frame_->color_origin_token);
            if(FAILED(marked))owner->reject(11);
        }catch(...){owner->reject(12);}
    }
    NVSDK_NGX_Result complete(NVSDK_NGX_Result result) noexcept {
        if(!active_)return result;
        completed_=true;
        try {
            std::scoped_lock lock(state().mutex);
            const auto found=state().features.find(handle_);
            if(NVSDK_NGX_FAILED(result)||rejected_||claims_!=1||native_claims_!=1||!frame_||!command_||!output_||
                found==state().features.end()||found->second.identity!=creator_.identity||
                found->second.creation_source_identity!=creator_.identity) {reject(5);return result;}
            ComPtr<ID3D12Device> device,canonical;ComPtr<IUnknown> identity,list_identity;
            if(FAILED(output_->GetDevice(IID_PPV_ARGS(&device)))||
                FAILED(d3d_native::allocation_owner(device.Get(),canonical))||
                !d3d_owner::same(canonical.Get(),frame_->verified_producer_device.Get())||
                FAILED(frame_->verified_producer_device.As(&identity))||identity.Get()!=device_||
                FAILED(command_.As(&list_identity))||list_identity.Get()!=list_) {reject(6);return result;}
            const auto desc=output_->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.DepthOrArraySize!=1||desc.SampleDesc.Count!=1||
                desc.Width!=frame_->output_width||desc.Height!=frame_->output_height||frame_->output_x||frame_->output_y||
                !frame_->color_origin_token) {reject(7);return result;}
            const auto view=hdr_&&desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS?DXGI_FORMAT_R16G16B16A16_FLOAT:
                hdr_&&desc.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_UNKNOWN;
            const auto marked=embedded::afw_snapshot().desired.mode==0?S_FALSE:
                mark_luke_color_origin(command_.Get(),output_.Get(),frame_->color_origin_token,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,frame_->verified_producer_device.Get(),
                    {0,0,frame_->output_width,frame_->output_height},view);
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,marked,1196,
                frame_->publication,frame_->color_origin_token);
            if(FAILED(marked))reject();
        }catch(...){reject();}
        return result;
    }
};
// Preserve the original scene camera and final caller Output before REALVR.
// KCD guides/scalars are frozen at the unique typed DLSS entry, matching the
// selected OptiScaler D12 publisher. The actual native production owns the clock.
class KcdCameraInvocationScope {
    inline static thread_local KcdCameraInvocationScope* current_{};
    KcdCameraInvocationScope* parent_{};
    KcdCameraInvocationScope* owner_{};
    const void* list_{};
    const void* device_{};
    NVSDK_NGX_Feature type_{};
    std::uint32_t production_{};
    unsigned width_{},height_{};
    bool public_{},consumed_{};
    luke_scene_camera::Receipt receipt_{};
    Feature creator_{};
    const NVSDK_NGX_Handle* public_handle_{};
    ID3D12GraphicsCommandList* public_command_{};
    const NVSDK_NGX_Parameter* public_parameters_{};
    ComPtr<ID3D12Resource> public_output_;
    ComPtr<ID3D12Resource> public_motion_;
    std::shared_ptr<const DlssMotionVectorFrame> public_frame_;
    std::uint64_t predicted_sequence_{};
    unsigned native_claims_{};
    bool prepared_{},aborted_{},native_success_{},public_completed_{};
    bool live_locked() const noexcept {
        const auto found=state().features.find(public_handle_);
        if(found==state().features.end())return false;
        const auto& feature=found->second;
        return feature.creation_observed&&feature.identity==production_&&
            feature.creation_source_identity==production_&&feature.type==type_&&
            feature.flags_known==creator_.flags_known&&feature.flags==creator_.flags&&
            feature.width==creator_.width&&feature.height==creator_.height&&
            feature.out_width==creator_.out_width&&feature.out_height==creator_.out_height;
    }
    void abort() noexcept {
        if(aborted_)return;
        aborted_=true;
        // A provisional copy can still be recorded on an unsubmitted original
        // list. Pin its whole pool until layer idle; clearing history alone
        // would let another publication reuse those recording destinations.
        if(prepared_)retire_dlss_motion_vector_stream(kKcdPublicStreamNamespace|production_);
        else invalidate_dlss_motion_vector_stream_history(kKcdPublicStreamNamespace|production_);
    }
public:
    KcdCameraInvocationScope(const Feature& feature,const void* list,const void* device,
        std::uint32_t production,bool public_dispatch,ID3D12GraphicsCommandList* command,
        const NVSDK_NGX_Parameter* parameters,const NVSDK_NGX_Handle* handle=nullptr)
        :parent_(current_),list_(list),device_(device),type_(feature.type),
         production_(production),public_(public_dispatch) {
        if(kcd_scene_camera_process()&&native_ingress::luke_detected()) {
            // Root presence is distinct from match validity. An authoritative
            // public transaction cannot fall back to an injector's guides if
            // a child changes its list/device/type or its creator is retired.
            if(parent_&&parent_->owner_) {
                owner_=parent_->owner_;
                if(!parent_->public_||owner_->list_!=list_||owner_->device_!=device_||
                    owner_->type_!=type_||owner_->production_!=production_)owner_->abort();
            }
            if(!owner_&&public_&&feature.creation_observed&&feature.identity==production_&&
                feature.creation_source_identity==production_&&list_&&device_&&production_) {
                owner_=this;
                creator_=feature;public_handle_=handle;public_command_=command;public_parameters_=parameters;
                width_=get<UINT>(parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,feature.width);
                height_=get<UINT>(parameters,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,feature.height);
                receipt_=luke_scene_camera::take(command,width_,height_);
                static std::atomic_uint reports{};
                static std::atomic_uint64_t deadline{};
                const auto now=GetTickCount64();auto previous=deadline.load(std::memory_order_relaxed);
                if(reports.load(std::memory_order_relaxed)<32&&now>=previous&&
                    deadline.compare_exchange_strong(previous,now+1000,std::memory_order_relaxed)) {
                    const auto sample=reports.fetch_add(1,std::memory_order_relaxed)+1;
                    auto& logger=bridge_flight_logger();
                    logger.event(BridgeFlightOperation::luke_afw_stage,receipt_.valid?1:0,944,production_,receipt_.token);
                    logger.event(BridgeFlightOperation::luke_afw_stage,sample,945,
                        (std::uint64_t(receipt_.attempts)<<32)|receipt_.reads,
                        (std::uint64_t(receipt_.candidates)<<32)|receipt_.failure);
                    logger.event(BridgeFlightOperation::luke_afw_stage,receipt_.owner_mask,946,receipt_.location,receipt_.resource);
                    logger.event(BridgeFlightOperation::luke_afw_stage,(receipt_.graphics?0x100:0)|receipt_.slot,
                        947,reinterpret_cast<std::uint64_t>(command),receipt_.signature);
                    logger.event(BridgeFlightOperation::luke_afw_stage,receipt_.error,948,
                        (std::uint64_t(width_)<<32)|height_,static_cast<std::uint32_t>(receipt_.install_error));
                    for(unsigned i=0;i<receipt_.bound_addresses.size();++i)
                        logger.event(BridgeFlightOperation::luke_afw_stage,i,1020,receipt_.bound_addresses[i],receipt_.bound_slots[i]);
                    logger.event(BridgeFlightOperation::luke_afw_stage,receipt_.unknown_ranges,1021,
                        receipt_.resource_count,receipt_.resource_drops);
                    logger.event(BridgeFlightOperation::luke_afw_stage,receipt_.resource_families,1022,
                        receipt_.observed_addresses,receipt_.observed_maps);
                    logger.event(BridgeFlightOperation::luke_afw_stage,0,1023,receipt_.observed_creations,0);
                }
            }
        }
        current_=this;
    }
    ~KcdCameraInvocationScope(){
        if(owner_==this&&prepared_&&!public_completed_)abort();
        current_=parent_;
    }
    KcdCameraInvocationScope(const KcdCameraInvocationScope&)=delete;
    void prepare_public_frame() noexcept {
        if(owner_!=this||prepared_||!public_handle_)return;
        prepared_=true;
        try {
            {
                std::scoped_lock lock(state().mutex);
                if(!live_locked()) {abort();return;}
                const auto stream=kKcdPublicStreamNamespace|production_;
                const auto found=state().public_production_sequences.find(stream);
                const auto current=found==state().public_production_sequences.end()?0:found->second;
                if(current==UINT64_MAX) {abort();return;}
                predicted_sequence_=current+1;
            }
            public_output_=native(get<ID3D12Resource*>(public_parameters_,NVSDK_NGX_Parameter_Output));
            public_motion_=native(get<ID3D12Resource*>(public_parameters_,NVSDK_NGX_Parameter_MotionVectors));
            if(!public_output_) {abort();return;}
            // F8 witnesses the original GPU bits before in-place wrapper edits.
            // This predicted identity is confirmed only by the unique native claim.
            if(two_frame_capture_images_active()) {
                TwoFrameGuideContract boundary{};boundary.stage="kcd_before_public_contract";
                boundary.stream=kKcdPublicStreamNamespace|production_;boundary.serial=predicted_sequence_;
                boundary.motion_width=boundary.depth_width=width_;
                boundary.motion_height=boundary.depth_height=height_;
                boundary.output_width=creator_.out_width;boundary.output_height=creator_.out_height;
                if(creator_.flags_known&&(creator_.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)==0) {
                    boundary.motion_width=creator_.out_width;boundary.motion_height=creator_.out_height;
                }
                boundary.depth_crop_x=get<UINT>(public_parameters_,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
                boundary.depth_crop_y=get<UINT>(public_parameters_,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
                boundary.scale_x=get<float>(public_parameters_,NVSDK_NGX_Parameter_MV_Scale_X,1);
                boundary.scale_y=get<float>(public_parameters_,NVSDK_NGX_Parameter_MV_Scale_Y,1);
                boundary.jitter_x=get<float>(public_parameters_,NVSDK_NGX_Parameter_Jitter_Offset_X);
                boundary.jitter_y=get<float>(public_parameters_,NVSDK_NGX_Parameter_Jitter_Offset_Y);
                boundary.reset=get<int>(public_parameters_,NVSDK_NGX_Parameter_Reset)!=0;
                boundary.jittered=(creator_.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
                boundary.frame_time_delta_ms=get<float>(public_parameters_,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667F);
                boundary.camera_position=receipt_.camera.position;boundary.camera_view_to_clip=receipt_.camera.projection;
                two_frame_capture_guide_contract(boundary);
                auto command=native(public_command_);ComPtr<ID3D12Device> reported,device;
                if(command&&SUCCEEDED(command->GetDevice(IID_PPV_ARGS(&reported)))&&
                    SUCCEEDED(d3d_native::allocation_owner(reported.Get(),device))) {
                    for(unsigned i=0;i<2;++i) {
                        auto guide=native(get<ID3D12Resource*>(public_parameters_,i?
                            NVSDK_NGX_Parameter_Depth:NVSDK_NGX_Parameter_MotionVectors));
                        if(!guide)continue;
                        TwoFrameImageInfo info{};info.stage=i?"kcd_before_public_depth":"kcd_before_public_motion";
                        info.stream=kKcdPublicStreamNamespace|production_;info.capture_serial=predicted_sequence_;
                        info.scale_x=get<float>(public_parameters_,NVSDK_NGX_Parameter_MV_Scale_X,1);
                        info.scale_y=get<float>(public_parameters_,NVSDK_NGX_Parameter_MV_Scale_Y,1);
                        info.jitter_x=get<float>(public_parameters_,NVSDK_NGX_Parameter_Jitter_Offset_X);
                        info.jitter_y=get<float>(public_parameters_,NVSDK_NGX_Parameter_Jitter_Offset_Y);
                        two_frame_capture_record_ng_x(info,device.Get(),command.Get(),guide.Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        two_frame_capture_timeline("kcd_public_guide_resource",i,info.stream,
                            predicted_sequence_,reinterpret_cast<std::uint64_t>(guide.Get()));
                    }
                }
            }
        }catch(...) {abort();}
    }
    // The public camera/lifetime/final-color transaction remains authoritative.
    // Only its exact unique typed descendant may snapshot native input guides.
    bool consume_public_frame(std::uint64_t sequence,const void* list,const void* device,
        std::uint32_t production,std::shared_ptr<const DlssMotionVectorFrame>& frame,
        ID3D12GraphicsCommandList* command,const NVSDK_NGX_Handle* handle,
        const NVSDK_NGX_Parameter* parameters,const Feature& feature) noexcept {
        if(!owner_||!owner_->prepared_)return false;
        auto& owner=*owner_;
        if(owner.aborted_||owner.native_claims_++||owner.list_!=list||owner.device_!=device||
            owner.production_!=production||owner.type_!=type_||!sequence||sequence!=owner.predicted_sequence_) {
            owner.abort();return true;
        }
        try {
            std::scoped_lock lock(state().mutex);
            if(!owner.live_locked()) {owner.abort();return true;}
        }catch(...) {owner.abort();return true;}
        try {
            bool snapshot_attempted{};
            owner.public_frame_=capture(command,handle,parameters,sequence,kKcdPublicStreamNamespace|production,
                this,production,&snapshot_attempted,&feature,owner.public_output_.Get());
            if(!owner.public_frame_&&snapshot_attempted)owner.abort();
            frame=owner.public_frame_;
            if(two_frame_capture_images_active()) {
                two_frame_capture_timeline("kcd_native_guide_claim",frame?1:0,
                    kKcdPublicStreamNamespace|production,sequence,frame?frame->publication:0);
                for(unsigned i=0;i<2;++i) {
                    auto guide=native(get<ID3D12Resource*>(parameters,i?
                        NVSDK_NGX_Parameter_Depth:NVSDK_NGX_Parameter_MotionVectors));
                    two_frame_capture_timeline("kcd_native_guide_resource",i,kKcdPublicStreamNamespace|production,
                        sequence,reinterpret_cast<std::uint64_t>(guide.Get()));
                    if(frame&&guide) {
                        auto native_command=native(command);
                        TwoFrameImageInfo info{};info.stage=i?"kcd_at_native_depth":"kcd_at_native_motion";
                        info.stream=frame->stream;info.capture_serial=sequence;
                        info.scale_x=frame->scale_x;info.scale_y=frame->scale_y;
                        info.jitter_x=frame->jitter_x;info.jitter_y=frame->jitter_y;
                        two_frame_capture_record_ng_x(info,frame->verified_producer_device.Get(),
                            native_command.Get(),guide.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    }
                }
                two_frame_capture_timeline("kcd_motion_native_span",frame?frame->raw_motion_native_span:0,
                    kKcdPublicStreamNamespace|production,sequence,frame?frame->publication:0);
            }
        }catch(...) {owner.abort();}
        return true;
    }
    std::uint32_t raw_motion_native_span(ID3D12Resource* motion) const noexcept {
        // Owner V602 matched-camera pixel audit verifies the KCD REALVR
        // replacement is N-2 same-eye motion. An unchanged original resource
        // retains the consecutive N-1 contract; other ingress has no owner.
        return owner_&&owner_->public_motion_&&motion&&!same(owner_->public_motion_.Get(),motion)?2U:1U;
    }
    void complete_native(NVSDK_NGX_Result result,bool can_complete) noexcept {
        if(!owner_||!owner_->prepared_)return;
        if(NVSDK_NGX_FAILED(result)||!can_complete)owner_->abort();
        else if(!owner_->aborted_)owner_->native_success_=true;
    }
    void finish_public(NVSDK_NGX_Result result) noexcept {
        if(owner_!=this||!prepared_||public_completed_)return;
        public_completed_=true;
        try {
            // Marker recording has no Execute/Wait or Capture-mutex reentry.
            // Serialize its short final lifetime check with observed release
            // and creation, so an old generation cannot mark a reused output.
            std::scoped_lock lock(state().mutex);
            if(aborted_||NVSDK_NGX_FAILED(result)||native_claims_!=1||!native_success_||!live_locked()) {abort();return;}
            if(public_frame_&&embedded::afw_snapshot().desired.mode!=0) {
                const bool hdr=creator_.flags_known&&(creator_.flags&NVSDK_NGX_DLSS_Feature_Flags_IsHDR)!=0;
                // The injector may leave its private Output in the parameter map.
                // Its caller's original final target was retained before forwarding.
                const auto completed=complete_color_resource(public_command_,public_output_.Get(),public_frame_,hdr);
                if(two_frame_capture_images_active())two_frame_capture_timeline("kcd_public_guide_completion",
                    completed,kKcdPublicStreamNamespace|production_,predicted_sequence_,public_frame_->publication);
                if(completed!=S_OK)abort();
            }
        }catch(...) {abort();}
    }
    // true means this invocation owns the camera decision, including a failed
    // or already-consumed receipt. Neither case may borrow an inner/newer one.
    bool apply(DlssMotionVectorPublication& publication,const void* list,const void* device,
        std::uint32_t production,unsigned render_width,unsigned render_height,bool& valid) noexcept {
        valid=false;
        if(!owner_)return false;
        if(owner_->aborted_||list_!=list||device_!=device||production_!=production)return true;
        if(owner_->consumed_)return true;
        owner_->consumed_=true;
        if(!owner_->receipt_.valid||owner_->width_!=render_width||owner_->height_!=render_height)return true;
        const auto& c=owner_->receipt_.camera;
        publication.camera_view_to_clip=c.projection;publication.camera_clip_to_view=c.inverse_projection;
        publication.camera_position=c.position;publication.camera_right=c.right;
        publication.camera_up=c.up;publication.camera_forward=c.forward;
        publication.camera_near=c.near_plane;publication.camera_far=c.far_plane;
        publication.camera_fov_vertical=c.fov;publication.depth_inverted=true;publication.depth_infinite=c.far_plane==0;
        publication.camera_frame_token=owner_->receipt_.token;publication.camera_frame_token_valid=true;
        publication.camera_data_valid=publication.camera_matrices_valid=true;valid=true;return true;
    }
};
template<class T> ComPtr<T> native(T* object) {
    ComPtr<T> result; IUnknown* borrowed{};
    if(d3d_native::get_borrowed(object,&borrowed))borrowed->QueryInterface(IID_PPV_ARGS(&result));
    return result;
}
bool same(IUnknown* a,IUnknown* b) {
    ComPtr<IUnknown> x,y;
    return a&&b&&SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x)))&&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y)))&&x==y;
}
void update_uevr_projection() {
    // UEVR's public SDK exposes the projection actually used by the renderer.
    // This also works when the application does not attach OpenXR depth metadata.
    auto module=GetModuleHandleW(L"UEVRBackend.dll");
    if(!module)return;
    auto* api=reinterpret_cast<const UEVR_PluginInitializeParam*>(
        GetProcAddress(module,"g_plugin_initialize_param"));
    if(!api||!api->version||api->version->major!=2||!api->vr||
       !api->vr->is_runtime_ready||!api->vr->is_runtime_ready()||
       !api->vr->get_ue_projection_matrix)return;
    UEVR_Matrix4x4f eyes[2]{};
    api->vr->get_ue_projection_matrix(0,&eyes[0]);
    api->vr->get_ue_projection_matrix(1,&eyes[1]);
    for(const auto& eye:eyes) {
        for(const auto& row:eye.m)for(float value:row)if(!std::isfinite(value))return;
        // Accept only a validated reversed, infinite perspective projection.
        if(eye.m[2][2]!=0.F||eye.m[2][3]!=1.F||eye.m[3][3]!=0.F||
           eye.m[3][2]<=0.F||eye.m[0][0]<=0.F||eye.m[1][1]<=0.F)return;
    }
    if(eyes[0].m[3][2]!=eyes[1].m[3][2])return;
    auto& s=state();std::scoped_lock lock(s.mutex);
    if(s.camera_from_xr)return;
    const bool first=!s.camera_valid;
    // Keep renderer units throughout the depth/projection math. No assumed UE
    // world-to-meter constant is needed for the rotational camera alignment.
    s.camera_near=eyes[0].m[3][2];
    s.camera_far=s.camera_near*10000.F; // unused by infinite-Z unprojection
    s.depth_inverted=true;s.depth_infinite=true;s.camera_valid=true;s.camera_ready=true;
    if(first)log("uevr_projection",UINT64(s.camera_near*1000000.F),api->version->minor);
}
std::shared_ptr<const DlssMotionVectorFrame> capture(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p,std::uint64_t production_sequence,std::uint64_t guide_stream,
    KcdCameraInvocationScope* invocation_camera=nullptr,std::uint32_t public_production=0,bool* snapshot_attempted=nullptr,
    const Feature* invocation_feature,ID3D12Resource* final_caller_output,unsigned observed_motion_span) {
    if(snapshot_attempted)*snapshot_attempted=false;
    const bool diagnostic=sample_dead_space_capture();
    const auto rejected=[&](unsigned reason,UINT64 detail=0) {if(diagnostic)report_dead_space_capture(reason,state().calls.load(),detail);};
    auto& s=state(); if(!s.active.load()){rejected(1);return {};}
    const auto call=++s.calls;
    if(!list||!p){rejected(2);return {};}
    native_ingress::observe_guide_list(list);
    // The projection rarely changes: refresh it every 32 evaluations, and on
    // every one until it is known. OpenXR depth metadata takes precedence.
    if(!s.camera_ready.load()||call%32==0)update_uevr_projection();
    ComPtr<ID3D12CommandQueue> queue; Feature feature{}; bool known{};
    float camera_near{},camera_far{}; bool inverted{},infinite{},camera_valid{};
    { std::scoped_lock lock(s.mutex); queue=s.queue;
      auto it=s.features.find(handle); if(it!=s.features.end()){feature=it->second;known=true;}
      camera_near=s.camera_near;camera_far=s.camera_far;
      inverted=s.depth_inverted;infinite=s.depth_infinite;camera_valid=s.camera_valid; }
    if(invocation_feature){feature=*invocation_feature;known=true;}
    const bool luke=native_ingress::luke_detected();
    if(luke) {
        ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> producer;
        if(!native_ingress::query_luke_context(owner.GetAddressOf(),producer.GetAddressOf())){rejected(3);return {};}
        queue=std::move(producer);
    }
    if(!queue){rejected(4);return {};}
    // Only creation-observed SR/RR types may enter guide capture or NR. NGX
    // evaluation maps alone cannot distinguish SR, DLSS-G and neural rendering.
    if(!known || (feature.type!=NVSDK_NGX_Feature_SuperSampling&&
        feature.type!=NVSDK_NGX_Feature_RayReconstruction)){rejected(5,feature.type);return {};}
    auto output_owner=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Output));
    if(final_caller_output&&!output_owner){rejected(6);return {};}
    // KCD's native guides accompany the completed original public color.
    // Retain its existing output/marker identity; never stamp a private SR target.
    if(final_caller_output)output_owner=final_caller_output;
    auto* output=output_owner.Get();
    auto motion=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_MotionVectors));
    auto depth=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Depth));
    if(!output||!motion||!depth) { if(call<8)log("missing_guides",call);rejected(6,(output?1U:0U)|(motion?2U:0U)|(depth?4U:0U)); return {}; }
    auto cmd=native(list); auto nq=native(queue.Get());if(!cmd||!nq){rejected(7);return {};}
    ComPtr<ID3D12Device> md,dd,cd,qd;
    if(FAILED(motion->GetDevice(IID_PPV_ARGS(&md)))||FAILED(depth->GetDevice(IID_PPV_ARGS(&dd)))||
       FAILED(cmd->GetDevice(IID_PPV_ARGS(&cd)))||FAILED(nq->GetDevice(IID_PPV_ARGS(&qd)))){rejected(8);return {};}
    if(luke) {
        // Keep the real game resources/list/queue intact. Only the proven
        // allocation owner bypasses an injector's private-queue interception.
        // WRL operator& converts through ComPtrRef and clears the owner before
        // returning ComPtr*. Take the storage addresses without that overload.
        std::array<ComPtr<ID3D12Device>*,4> owners{
            std::addressof(md),std::addressof(dd),std::addressof(cd),std::addressof(qd)};
        for(auto* owner:owners) {
            ComPtr<ID3D12Device> canonical;
            const auto result=d3d_native::allocation_owner(owner->Get(),canonical);
            if(FAILED(result)) {
                static std::atomic_uint failures{};
                if(failures.fetch_add(1,std::memory_order_relaxed)<8)
                    log("native_allocation_owner_failed",static_cast<UINT>(result),
                        reinterpret_cast<UINT64>(owner->Get()));
                rejected(9,static_cast<UINT>(result));
                return {};
            }
            *owner=std::move(canonical);
        }
    }
    if(!d3d_owner::same(md.Get(),dd.Get())||!d3d_owner::same(md.Get(),cd.Get())||!d3d_owner::same(md.Get(),qd.Get())) {
        if(call<8)log("device_mismatch",call);rejected(10); return {};
    }
    const auto od=output->GetDesc(), mv=motion->GetDesc(), dz=depth->GetDesc();
    DlssMotionVectorPublication g{};
    if(final_caller_output&&invocation_camera)g.raw_motion_native_span=invocation_camera->raw_motion_native_span(motion.Get());
    // Crimson V561 F8: both retained native fields fit N-2 (same eye), not
    // the adjacent opposite camera advertised by Streamline's clip matrix.
    // Keep the measured field contract separate from camera receipt order.
    // V568 Ghost SteamVR F8: native motion matches same-eye N-2 at .1px,
    // opposite-eye N-1 misses by5.9px. Preserve the accepted VDXR contract.
    // Horizon SteamVR and VDXR F8s establish the same native N-2 field.
    // The advertised SL clip matrix is not the field's temporal endpoint.
    if(luke && !invocation_camera && (crimson_desert_process()||same_eye_native_motion()))
        g.raw_motion_native_span=2;
    if(observed_motion_span)g.raw_motion_native_span=observed_motion_span;
    g.compose_native_object_history=luke && gta_enhanced_process() && g.raw_motion_native_span==1;
    if(luke&&gta_enhanced_process())
        g.gta_headlock=query_luke_gta_headlock(bridge_flight_logger().enabled());
    // Scoped camera values replace apply_camera's camera read, while its Luke
    // snapshot-lifetime contract must still protect all queued GPU consumers.
    g.protect_snapshot_lifetime=luke;
    g.stream=guide_stream; g.output=output;
    g.motion_vectors=motion.Get(); g.depth=depth.Get();
    // This is the original, creation-observed NGX SR/RR MotionVectors input,
    // not an arbitrary game texture. DLSS specifies float XY. Type only our
    // snapshot of Dead Space's measured RG16_TYPELESS allocation.
    g.dlss_float_motion=luke && dead_space_process() &&
        mv.Format==DXGI_FORMAT_R16G16_TYPELESS;
    g.producer_queue=nq.Get(); g.producer_command_list=cmd.Get(); g.verified_producer_device=qd.Get();
    g.output_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
    g.output_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
    g.output_width=ngx_display_dimension(feature.out_width,p,NVSDK_NGX_Parameter_OutWidth,
        UINT(od.Width),feature.creation_observed);
    g.output_height=ngx_display_dimension(feature.out_height,p,NVSDK_NGX_Parameter_OutHeight,
        od.Height,feature.creation_observed);
    g.motion_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
    g.motion_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    const auto rw=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,feature.width);
    const auto rh=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,feature.height);
    // Without creation flags, assume render-resolution vectors: the usual
    // DLSS integration, and the one whose rectangle the render size gives.
    const bool full_mv=feature.flags_known&&(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)==0;
    g.motion_width=full_mv?g.output_width:(rw?rw:UINT(mv.Width));
    g.motion_height=full_mv?g.output_height:(rh?rh:mv.Height);
    g.depth_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
    g.depth_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    g.depth_width=rw?rw:UINT(dz.Width); g.depth_height=rh?rh:dz.Height;
    if(luke) {
        // Match OptiScaler's actual native publisher. MV_Offset is a FLOAT
        // NGX parameter, distinct from the integer MV-subrect API key.
        const auto offset=[&](const char* name) {
            const float value=get<float>(p,name);
            return std::isfinite(value) && value>0 && value<float(UINT_MAX)
                ? static_cast<UINT>(value) : 0U;
        };
        g.motion_x=offset(NVSDK_NGX_Parameter_MV_Offset_X);
        g.motion_y=offset(NVSDK_NGX_Parameter_MV_Offset_Y);
        const auto available=[](UINT64 extent,UINT base) {
            return static_cast<UINT>(extent-std::min<UINT64>(base,extent));
        };
        g.output_width=std::min(g.output_width,available(od.Width,g.output_x));
        g.output_height=std::min(g.output_height,available(od.Height,g.output_y));
        g.motion_width=std::min(g.motion_width,available(mv.Width,g.motion_x));
        g.motion_height=std::min(g.motion_height,available(mv.Height,g.motion_y));
        g.depth_width=std::min(g.depth_width,available(dz.Width,g.depth_x));
        g.depth_height=std::min(g.depth_height,available(dz.Height,g.depth_y));
        if(two_frame_capture_active()) {
            auto color=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Color));
            ComPtr<ID3D12Device> color_device;
            if(color && SUCCEEDED(color->GetDevice(IID_PPV_ARGS(&color_device))) &&
                d3d_owner::same(color_device.Get(),qd.Get())) {
                const auto desc=color->GetDesc();g.input_color=color.Get();
                g.input_color_x=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X);
                g.input_color_y=get<UINT>(p,NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
                g.input_color_width=std::min(rw?rw:UINT(mv.Width),available(desc.Width,g.input_color_x));
                g.input_color_height=std::min(rh?rh:mv.Height,available(desc.Height,g.input_color_y));
                // Parameters retain this original color through publication;
                // no color reference is added to persistent guide history.
            }
        }
    }
    g.scale_x=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_X,1);
    g.scale_y=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_Y,1);
    g.jitter_x=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_X);
    g.jitter_y=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_Y);
    g.jittered=(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
    g.reset=get<int>(p,NVSDK_NGX_Parameter_Reset)!=0;
    g.resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g.depth_resource_state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    g.frame_time_delta_ms=get<float>(p,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667F);
    if(luke && (!std::isfinite(g.frame_time_delta_ms) || g.frame_time_delta_ms<=0))
        g.frame_time_delta_ms=16.6667F;
    g.camera_near=get<float>(p,"DLSSG.CameraNear",camera_near);
    g.camera_far=get<float>(p,"DLSSG.CameraFar",camera_far);
    // Unknown flags defer to the camera's own depth convention.
    g.depth_inverted=feature.flags_known?(feature.flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0:inverted;
    // A game with no camera metadata - a native VR game that submits no OpenXR
    // depth, say - still states its depth direction in the feature's flags,
    // which its own DLSS relies on. That is all NGX's result depends on: it
    // measured bit-identical for near planes from 0.01 to 100, finite or
    // infinite, while a wrong direction quadrupled the error at moving edges.
    // So nominal planes stand in for the camera's.
    if(!camera_valid&&feature.flags_known) {
        camera_valid=true; inverted=g.depth_inverted; infinite=false;
        g.camera_near=get<float>(p,"DLSSG.CameraNear",0.1F);
        g.camera_far=get<float>(p,"DLSSG.CameraFar",1000.F);
    }
    if(!camera_valid||inverted!=g.depth_inverted) {
        // The motion alone still serves OFXR + DLSS vectors; native generation
        // finds no depth and shows the current frame.
        if(call<=8||call%300==0)log("waiting_depth_convention",call,feature.flags,camera_valid,inverted);
        g.depth=nullptr;
    }
    g.depth_infinite=infinite;
    // The exact camera is consumed BEFORE SR changes list bindings or any
    // provider mutates native motion. All snapshots are recorded now.
    if(luke&&!feature.flags_known) {
        if(call<=8||call%300==0)log("waiting_creation_flags",call,g.stream);
        rejected(11);
        return {};
    }
    bool exact_camera{},scoped_camera{};
    if(invocation_camera) {
        ComPtr<IUnknown> list_identity,device_identity;
        static_cast<void>(cmd.As(&list_identity));static_cast<void>(cd.As(&device_identity));
        scoped_camera=invocation_camera->apply(g,list_identity.Get(),device_identity.Get(),public_production,
            rw?rw:UINT(mv.Width),rh?rh:mv.Height,exact_camera);
    }
    if(!scoped_camera)exact_camera=native_ingress::apply_camera(g,cmd.Get());
    g.native_production_sequence=production_sequence;
    if(exact_camera)g.depth=depth.Get();
    if(luke && !exact_camera) {
        if(embedded::afw_snapshot().desired.mode!=0)embedded::report_afw_ingress(false,false);
        if(dead_space_process() && two_frame_capture_images_active())
            two_frame_capture_guide_contract(dead_space_rejected_camera_contract(
                native_ingress::streamline_camera::current(true),g));
        rejected(12);
        return {};
    }
    if(snapshot_attempted)*snapshot_attempted=true;
    auto frame=publish_dlss_motion_vectors(g);
    if(diagnostic)report_dead_space_capture(frame?0:13,call,g.stream);
    if(frame && !invocation_camera && two_frame_capture_images_active()) {
        // Generic Luke titles may refuse AFW before its input captures run.
        // F8 must still show the exact retained native guides, independently
        // of whether the AFW resolver admits this production.
        for(unsigned i=0;i<2;++i) {
            TwoFrameImageInfo info{};
            info.stage=i?"ngx_native_depth":"ngx_native_motion";
            info.stream=frame->stream;info.capture_serial=frame->publication;
            info.width=i?frame->depth_width:frame->motion_width;
            info.height=i?frame->depth_height:frame->motion_height;
            info.scale_x=frame->scale_x;info.scale_y=frame->scale_y;
            info.jitter_x=frame->jitter_x;info.jitter_y=frame->jitter_y;
            two_frame_capture_record_ng_x(info,qd.Get(),cmd.Get(),
                i?frame->depth.Get():frame->motion_vectors.Get(),
                i?frame->depth_resource_state:frame->resource_state);
        }
        if(g.input_color) {
            TwoFrameImageInfo info{};info.stage="ngx_input_color";
            info.stream=frame->stream;info.capture_serial=frame->publication;
            info.width=g.input_color_width;info.height=g.input_color_height;
            two_frame_capture_record_ng_x(info,qd.Get(),cmd.Get(),g.input_color,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
    }
    if(final_caller_output&&two_frame_capture_images_active()) {
        TwoFrameGuideContract boundary{};boundary.stage="kcd_native_input_contract";
        boundary.stream=g.stream;boundary.serial=production_sequence;boundary.publication=frame?frame->publication:0;
        boundary.motion_width=g.motion_width;boundary.motion_height=g.motion_height;
        boundary.depth_width=g.depth_width;boundary.depth_height=g.depth_height;
        boundary.output_width=g.output_width;boundary.output_height=g.output_height;
        boundary.motion_crop_x=g.motion_x;boundary.motion_crop_y=g.motion_y;
        boundary.depth_crop_x=g.depth_x;boundary.depth_crop_y=g.depth_y;
        boundary.scale_x=g.scale_x;boundary.scale_y=g.scale_y;
        boundary.jitter_x=g.jitter_x;boundary.jitter_y=g.jitter_y;
        boundary.reset=g.reset;boundary.jittered=g.jittered;boundary.frame_time_delta_ms=g.frame_time_delta_ms;
        boundary.raw_motion_native_span=g.raw_motion_native_span;
        boundary.camera_position=g.camera_position;boundary.camera_view_to_clip=g.camera_view_to_clip;
        two_frame_capture_guide_contract(boundary);
    }
    if(luke&&embedded::afw_snapshot().desired.mode!=0)embedded::report_afw_ingress(bool(frame),exact_camera);
    const HRESULT removed=md->GetDeviceRemovedReason();
    if(FAILED(removed))log("device_removed",static_cast<UINT>(removed),call);
    if(call<=8||call%300==0) {
        if(kingdom_come_process())log("kcd_native_guide_owner",reinterpret_cast<UINT64>(handle),
            g.stream,production_sequence,g.camera_frame_token);
        log("capture",call,g.stream,(UINT64(g.output_width)<<32)|g.output_height,feature.flags);
        log("rects",(UINT64(g.output_x)<<32)|g.output_y,(UINT64(g.motion_x)<<32)|g.motion_y,
            (UINT64(g.motion_width)<<32)|g.motion_height,(UINT64(g.depth_width)<<32)|g.depth_height);
        log("formats",mv.Format,dz.Format,od.Width,od.Height);
        log("output_format",od.Format,od.Dimension,od.DepthOrArraySize,od.SampleDesc.Count);
    }
    return frame;
}
template<UINT slot> NVSDK_NGX_Result create(ID3D12GraphicsCommandList* list,NVSDK_NGX_Feature id,
    NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    // Preserve the caller's real creation contract before an injector or the
    // native implementation mutates its parameter map. No opaque handle reads.
    Feature f{0,id,get<int>(p,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags),
            get<UINT>(p,NVSDK_NGX_Parameter_Width),get<UINT>(p,NVSDK_NGX_Parameter_Height),
            get<UINT>(p,NVSDK_NGX_Parameter_OutWidth),get<UINT>(p,NVSDK_NGX_Parameter_OutHeight)};
    f.flags_known=p&&NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&f.flags));
    f.creation_observed=true;
    const auto result=state().modules[slot].create(list,id,p,out);
    if(NVSDK_NGX_SUCCEED(result)&&out&&*out&&p) {
        f.identity=native_ingress::allocate_handle_key();
        f.creation_source_identity=f.identity;
        bool replaced{},retire_public{};std::uint64_t previous_stream{};
        std::shared_ptr<OutlawsPublicLifetime> previous_outlaws;
        {std::scoped_lock lock(state().mutex);
            if constexpr(slot==3)if(outlaws_public_process()) {
                const auto old=state().outlaws_public_lifetimes.find(*out);
                if(old!=state().outlaws_public_lifetimes.end()) {
                    previous_outlaws=old->second;state().outlaws_public_lifetimes.erase(old);
                    state().public_production_sequences.erase(kKcdPublicStreamNamespace|previous_outlaws->identity);
                }
            }
            const auto old=state().features.find(*out);replaced=old!=state().features.end();
            const auto old_stream=state().guide_streams.find(*out);
            previous_stream=old_stream==state().guide_streams.end()?reinterpret_cast<UINT64>(*out):old_stream->second;
            retire_public=replaced&&public_guide_stream(previous_stream)&&
                old->second.identity==std::uint32_t(previous_stream);
            if(retire_public)state().public_production_sequences.erase(previous_stream);
            state().features[*out]=f;
            state().native_production_sequences.erase(*out);state().guide_streams.erase(*out);}
        if(previous_outlaws)retire_dlss_motion_vector_stream(kKcdPublicStreamNamespace|previous_outlaws->identity);
        // A new feature may reuse an opaque handle address even if its old
        // release was missed. It must never inherit that generation's history.
        if(replaced&&(kingdom_come_process()||outlaws_public_process())) {
            if(retire_public)retire_dlss_motion_vector_stream(previous_stream);
            else invalidate_dlss_motion_vector_stream_history(previous_stream);
        }
        log("feature",reinterpret_cast<UINT64>(*out),f.flags,(UINT64(f.width)<<32)|f.height,
            (UINT64(f.out_width)<<32)|f.out_height);
    }
    return result;
}
HRESULT complete_color_resource(ID3D12GraphicsCommandList* list,ID3D12Resource* output,
    const std::shared_ptr<const DlssMotionVectorFrame>& frame,bool hdr) {
    if(!frame||!frame->color_origin_token||!native_ingress::luke_detected()||
       embedded::afw_snapshot().desired.mode==0)return S_FALSE;
    auto color=native(output);
    auto cmd=native(list);if(!color||!cmd||!same(color.Get(),frame->output.Get()))return E_INVALIDARG;
    ComPtr<ID3D12Device> cd,ld;
    if(FAILED(color->GetDevice(IID_PPV_ARGS(&cd)))||FAILED(cmd->GetDevice(IID_PPV_ARGS(&ld)))||
       !d3d_owner::same(cd.Get(),frame->verified_producer_device.Get())||
       !d3d_owner::same(ld.Get(),frame->verified_producer_device.Get()))return E_INVALIDARG;
    const auto desc=color->GetDesc();
    // NGX's actual HDR creation flag supplies the typed float view contract.
    // Typeless HDR storage alone never selects a numeric interpretation.
    const auto view=hdr&&desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS?DXGI_FORMAT_R16G16B16A16_FLOAT:
        hdr&&desc.Format==DXGI_FORMAT_R32G32B32A32_TYPELESS?DXGI_FORMAT_R32G32B32A32_FLOAT:DXGI_FORMAT_UNKNOWN;
    const auto result=mark_luke_color_origin(cmd.Get(),color.Get(),frame->color_origin_token,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,frame->verified_producer_device.Get(),
        {frame->output_x,frame->output_y,frame->output_width,frame->output_height},view);
    if(FAILED(result)) {
        log("origin_marker_failed",static_cast<UINT>(result),frame->publication,frame->camera_frame_token);
        static std::atomic_uint64_t last_failure{};
        const auto now=GetTickCount64();auto previous=last_failure.load();
        if(now-previous>=1000&&last_failure.compare_exchange_strong(previous,now))
            log("origin_marker_resource",desc.Format,desc.Dimension,
                (UINT64(desc.DepthOrArraySize)<<32)|desc.SampleDesc.Count,view);
    }
    if(two_frame_capture_images_active()) {
        TwoFrameImageInfo info{};info.stage="ngx_completed_color";
        info.stream=frame->stream;info.capture_serial=frame->publication;
        info.width=frame->output_width;info.height=frame->output_height;
        two_frame_capture_record_ng_x(info,frame->verified_producer_device.Get(),
            cmd.Get(),color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    return result;
}
HRESULT complete_color(ID3D12GraphicsCommandList* list,const NVSDK_NGX_Parameter* p,
    const std::shared_ptr<const DlssMotionVectorFrame>& frame,bool hdr) {
    return complete_color_resource(list,get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Output),frame,hdr);
}
template<UINT slot, bool c_entry = false> NVSDK_NGX_Result evaluate(ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,void* callback) {
    const auto original = c_entry ? state().modules[slot].evaluate_c : state().modules[slot].evaluate;
    const bool kcd_production=kingdom_come_process();
    const bool outlaws_production=outlaws_public_process();
    bool diagnostic_entry{};
    if(kingdom_come_process()||cached_luke_title()) {
        static std::array<std::atomic_uint64_t,5> last_entries{};
        auto& last_entry=last_entries[cached_luke_title()?slot:0];
        const auto now=GetTickCount64();auto previous=last_entry.load();
        if(now-previous>=1000&&last_entry.compare_exchange_strong(previous,now)) {
            diagnostic_entry=cached_luke_title()&&bridge_flight_logger().enabled();
            int flags{};
            const auto result=p?p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&flags):NVSDK_NGX_Result_Fail;
            log("native_evaluate_entry",slot,state().modules[slot].typed_implementation,h!=nullptr,
                static_cast<UINT>(result));
        }
    }
    auto native_list=native(list);
    ComPtr<IUnknown> native_list_identity;
    if(native_list)static_cast<void>(native_list.As(&native_list_identity));
    ComPtr<IUnknown> native_device_identity;
    if((kcd_production||outlaws_production||gta_enhanced_process())&&native_list) {
        ComPtr<ID3D12Device> reported,canonical;
        if(SUCCEEDED(native_list->GetDevice(IID_PPV_ARGS(reported.GetAddressOf())))&&
            SUCCEEDED(d3d_native::allocation_owner(reported.Get(),canonical)))
            static_cast<void>(canonical.As(&native_device_identity));
    }
    OutlawsPublicInvocationScope outlaws_invocation(slot,h,native_list_identity.Get(),native_device_identity.Get(),native_list.Get(),p);
    outlaws_invocation.prepare_public_frame();
    const auto forward=[&] {return outlaws_invocation.complete(original(list,h,p,callback));};
    bool sr=false,hdr=false,native_available=false;unsigned identity{};ComPtr<ID3D12CommandQueue> queue;
    bool replaced_native_generation{};std::uint64_t previous_native_stream{};
    Feature evaluation_metadata{};
    {std::scoped_lock lock(state().mutex);queue=state().queue;
     if constexpr(slot<2)if(state().modules[slot].typed_implementation&&h&&p) {
        // This trampoline belongs to the SR/RR implementation module, unlike
        // the generic core dispatcher. Its provenance identifies feature type;
        // creation dimensions may already be erased or inverted in the eval
        // map. A known public ancestor supplies its immutable contract only
        // within this exact evaluation on the same native command list.
        auto existing=state().features.find(h);
        Feature recovered{};
        const auto type=slot==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
        const bool direct_creation=existing!=state().features.end()&&existing->second.creation_observed&&
            existing->second.creation_source_identity!=0&&
            existing->second.creation_source_identity==existing->second.identity;
        const bool inherited=!direct_creation&&NgxCreationEvaluationScope::inherit_native(type,
            native_list_identity.Get(),recovered);
        const bool inherited_unchanged=inherited&&existing!=state().features.end()&&
            existing->second.creation_observed&&existing->second.type==recovered.type&&
            existing->second.creation_source_identity==recovered.creation_source_identity&&
            existing->second.flags_known&&recovered.flags_known&&existing->second.flags==recovered.flags&&
            existing->second.width==recovered.width&&existing->second.height==recovered.height&&
            existing->second.out_width==recovered.out_width&&existing->second.out_height==recovered.out_height;
        if(!inherited_unchanged&&(inherited||existing==state().features.end()||
            !existing->second.creation_observed||!existing->second.flags_known)) {
            bool recovered_metadata=inherited;
            if(inherited&&!recovered.flags_known) {
                Feature flags;
                if(recover_typed_ngx_metadata(true,type,p,flags)&&flags.flags_known) {
                    recovered.flags=flags.flags;recovered.flags_known=true;
                }
            }
            if(!inherited&&existing!=state().features.end()&&existing->second.creation_observed) {
                recovered=existing->second;
                Feature flags;
                if(recover_typed_ngx_metadata(true,type,p,flags)&&flags.flags_known) {
                    recovered.flags=flags.flags;recovered.flags_known=true;recovered_metadata=true;
                }
            } else if(!inherited&&(existing==state().features.end()||!existing->second.flags_known))
                recovered_metadata=recover_typed_ngx_metadata(true,type,p,recovered);
            if(recovered_metadata) {
                if(existing==state().features.end()) {
                    recovered.identity=native_ingress::allocate_handle_key();state().features.emplace(h,recovered);
                    log("typed_feature_recovered",slot,recovered.flags_known,
                        (UINT64(recovered.width)<<32)|recovered.height,(UINT64(recovered.out_width)<<32)|recovered.out_height);
                } else if(recovered.flags_known||inherited) {
                    replaced_native_generation=inherited&&
                        existing->second.creation_source_identity!=recovered.creation_source_identity;
                    recovered.identity=replaced_native_generation?native_ingress::allocate_handle_key():existing->second.identity;
                    existing->second=recovered;
                    if(replaced_native_generation) {
                        const auto previous=state().guide_streams.find(h);
                        previous_native_stream=previous==state().guide_streams.end()?reinterpret_cast<UINT64>(h):previous->second;
                        state().native_production_sequences.erase(h);state().guide_streams.erase(h);
                    }
                    log(inherited?"typed_creation_contract_inherited":"typed_feature_flags_recovered",
                        slot,recovered.flags,(UINT64(recovered.width)<<32)|recovered.height,
                        (UINT64(recovered.out_width)<<32)|recovered.out_height);
                }
            }
        }
     }
     auto it=state().features.find(h);sr=it!=state().features.end()&&
       (it->second.type==NVSDK_NGX_Feature_SuperSampling||it->second.type==NVSDK_NGX_Feature_RayReconstruction);
     if(it!=state().features.end()) {identity=it->second.identity;
         evaluation_metadata=it->second;
         if(sr)native_available=state().modules[it->second.type==NVSDK_NGX_Feature_SuperSampling?0:1].typed_implementation;
         hdr=it->second.flags_known&&(it->second.flags&NVSDK_NGX_DLSS_Feature_Flags_IsHDR)!=0;}}
    if(diagnostic_entry)title_capture_log("dead_space_evaluation_state",slot,sr,
        (evaluation_metadata.flags_known?1U:0U)|(evaluation_metadata.creation_observed?2U:0U),identity);
    if(replaced_native_generation)
        invalidate_dlss_motion_vector_stream_history(previous_native_stream);
    // An untyped core dispatch may reach the typed SR module, where recovery
    // is exact. Generic core parameters never classify a feature as SR.
    if(!sr) {
        // Known non-SR dispatch blocks an enclosing SR creation receipt.
        // Unknown generic forwarding keeps the caller's exact dynamic scope.
        if(evaluation_metadata.creation_observed) {
            NgxCreationEvaluationScope non_sr(evaluation_metadata,native_list_identity.Get(),false,native_device_identity.Get());
            KcdCameraInvocationScope no_camera(evaluation_metadata,native_list_identity.Get(),
                native_device_identity.Get(),0,false,native_list.Get(),p);
            return forward();
        }
        return forward();
    }
    std::uint32_t public_production{};
    if(kingdom_come_process()&&state().modules[slot].typed_implementation)
        public_production=NgxCreationEvaluationScope::public_production(evaluation_metadata.type,
            native_list_identity.Get(),native_device_identity.Get());
    NgxCreationEvaluationScope creation_scope(evaluation_metadata,native_list_identity.Get(),
        !state().modules[slot].typed_implementation,native_device_identity.Get());
    GtaFinalColorScope gta_final(!state().modules[slot].typed_implementation,evaluation_metadata,h,
        list,p,native_list_identity.Get(),native_device_identity.Get());
    if(kingdom_come_process()&&!state().modules[slot].typed_implementation)
        public_production=NgxCreationEvaluationScope::public_production(evaluation_metadata.type,
            native_list_identity.Get(),native_device_identity.Get());
    if(public_production) {
        std::scoped_lock lock(state().mutex);
        const bool live=std::any_of(state().features.begin(),state().features.end(),[&](const auto& entry) {
            const auto& creator=entry.second;
            return creator.creation_observed&&creator.identity==public_production&&
                creator.creation_source_identity==public_production&&creator.type==evaluation_metadata.type;
        });
        if(!live)public_production=0; // An inherited alias cannot revive a retired creator.
    }
    KcdCameraInvocationScope invocation_camera(evaluation_metadata,native_list_identity.Get(),
        native_device_identity.Get(),public_production,!state().modules[slot].typed_implementation,
        native_list.Get(),p,h);
    invocation_camera.prepare_public_frame();
    const auto forward_public=[&] {
        const auto result=forward();invocation_camera.finish_public(result);return gta_final.complete(result);
    };
    // V563 matched public/native F8 and color features: Crimson's public field
    // is exact N-1; REALVR modifies it into an approximate N-2 field before the
    // typed SR body. Own this known SR invocation before that modification,
    // freeze guides there, and complete its color only after the wrapper.
    // GTA's original N-1 object intervals are composed with native depth;
    // freeze them before REAL VR's history-conditioned combined field.
    // The final caller scope still stamps only after the wrapper returns.
    const bool original_public=(crimson_desert_process() || gta_enhanced_process()) && native_ingress::luke_detected() &&
        !state().modules[slot].typed_implementation;
    const bool delegate_nr=original_public && native_available;
    NgxEvaluationBoundary scope(slot,state().modules[slot].typed_implementation,
        native_available && !original_public,original_public);
    if(diagnostic_entry)title_capture_log("dead_space_observation_state",slot,scope.observes(),state().active.load(),scope.outermost());
    if(sr && scope.outermost() && !state().modules[slot].typed_implementation &&
        crimson_desert_process() && two_frame_capture_images_active() && native_ingress::luke_detected()) {
        // F8 evidence before the generic dispatcher forwards to REALVR/native
        // SR. Do not publish a guide, alter a camera or select runtime motion.
        auto motion=native(get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_MotionVectors));
        auto command=native(list);
        if(motion && command) {
            ComPtr<ID3D12Device> owner;
            if(SUCCEEDED(motion->GetDevice(IID_PPV_ARGS(&owner)))) {
                const auto desc=motion->GetDesc();
                DlssMotionVectorPublication original{};
                const bool camera=native_ingress::apply_camera(original,command.Get());
                TwoFrameImageInfo info{};info.stage="crimson_public_motion_before_dispatch";
                info.stream=reinterpret_cast<UINT64>(h);info.capture_serial=original.camera_frame_token;
                info.width=UINT(desc.Width);info.height=desc.Height;
                info.scale_x=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_X,1);
                info.scale_y=get<float>(p,NVSDK_NGX_Parameter_MV_Scale_Y,1);
                info.jitter_x=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_X);
                info.jitter_y=get<float>(p,NVSDK_NGX_Parameter_Jitter_Offset_Y);
                two_frame_capture_record_ng_x(info,owner.Get(),command.Get(),motion.Get(),
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                TwoFrameGuideContract contract{};contract.stage="crimson_public_camera_before_dispatch";
                contract.stream=info.stream;contract.native_camera_token=original.camera_frame_token;
                contract.motion_width=info.width;contract.motion_height=info.height;
                contract.scale_x=info.scale_x;contract.scale_y=info.scale_y;
                contract.jitter_x=info.jitter_x;contract.jitter_y=info.jitter_y;
                contract.camera_near=original.camera_near;contract.camera_far=original.camera_far;
                contract.camera_fov_vertical=original.camera_fov_vertical;
                contract.raw_camera_motion_included=original.raw_camera_motion_included;
                contract.depth_inverted=original.depth_inverted;contract.depth_infinite=original.depth_infinite;
                contract.camera_position=original.camera_position;contract.camera_right=original.camera_right;
                contract.camera_up=original.camera_up;contract.camera_forward=original.camera_forward;
                contract.view_to_clip=original.camera_view_to_clip;contract.clip_to_view=original.camera_clip_to_view;
                contract.raw_clip_to_previous_clip=original.raw_clip_to_previous_clip;
                contract.raw_previous_clip_to_clip=original.raw_previous_clip_to_clip;
                two_frame_capture_guide_contract(contract);
                two_frame_capture_timeline("crimson_public_camera",camera?1:0,info.stream,
                    original.camera_frame_token,slot);
            }
        }
    }

    static std::atomic_uint boundary_reports{};
    if(boundary_reports.fetch_add(1,std::memory_order_relaxed)<16)
        log("ngx_observation_boundary",slot,state().modules[slot].typed_implementation,
            native_available,scope.observes());
    if(public_production&&!state().modules[slot].typed_implementation&&
        evaluation_metadata.identity==public_production&&
        evaluation_metadata.creation_source_identity==public_production) {
        // The public lifetime owns the family even when its wrapper delegates
        // observation to a different native handle. Remember that creator
        // before return; a forwarding alias must retain its old native mapping
        // until typed generation recovery invalidates the old history.
        std::scoped_lock lock(state().mutex);
        state().guide_streams[h]=kKcdPublicStreamNamespace|public_production;
    }
    if(!scope.observes()) {
        // Only guide/color ownership moves to the public boundary. NR retains
        // its existing typed-native before/after SR execution points.
        if(sr && state().modules[slot].typed_implementation && scope.public_guide_parent()) {
            try {neural_rendering::before_sr(identity,list,p,queue.Get());}
            catch(...){log("native_pre_sr_exception");}
            const auto result=forward_public();
            if(NVSDK_NGX_SUCCEED(result))try {neural_rendering::after_sr(identity,list,p,queue.Get());}
            catch(...){log("native_completion_exception");}
            if(gta_enhanced_process())GtaFinalColorScope::native_complete(evaluation_metadata,
                native_list_identity.Get(),native_device_identity.Get(),list,p,result);
            return result;
        }
        return forward_public();
    }
    bool outlaws_family{};
    if(outlaws_production&&state().modules[slot].typed_implementation) {
        outlaws_family=OutlawsPublicInvocationScope::claim(evaluation_metadata.type,
            native_list_identity.Get(),native_device_identity.Get(),public_production);
        if(outlaws_family&&!public_production)return forward();
    }
    const bool logical_production=kcd_production||outlaws_family;
    std::uint64_t production_sequence{};
    const std::uint64_t guide_stream=logical_production&&public_production
        ? kKcdPublicStreamNamespace|public_production:reinterpret_cast<UINT64>(h);
    if(logical_production) {
        // Allocate at the unique native SR/RR owner, before any camera/guide
        // capture can fail. A generic-only known SR call owns the same logical
        // production. Nested dispatch and unrelated features do not allocate.
        std::scoped_lock lock(state().mutex);
        state().guide_streams[h]=guide_stream;
        auto& sequence=public_production?state().public_production_sequences[guide_stream]:
            state().native_production_sequences[h];
        if(sequence!=UINT64_MAX)production_sequence=++sequence;
        if(public_production)state().native_production_sequences[h]=production_sequence;
    }
    // Observe the original game list even during late XR bootstrap. The actual
    // Execute hook, not this recording call, establishes the producer queue.
    native_ingress::observe_luke_production(list);
    native_ingress::observe_upscaler_inputs(
        get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_MotionVectors) != nullptr,
        get<ID3D12Resource*>(p,NVSDK_NGX_Parameter_Depth) != nullptr);
    std::shared_ptr<const DlssMotionVectorFrame> frame;
    bool public_transaction{};
    if(sr)try {
        if(kcd_production)public_transaction=invocation_camera.consume_public_frame(production_sequence,
            native_list_identity.Get(),native_device_identity.Get(),public_production,frame,list,h,p,evaluation_metadata);
        if(outlaws_family)public_transaction=OutlawsPublicInvocationScope::consume_public_frame(evaluation_metadata,p,
            production_sequence,native_list_identity.Get(),native_device_identity.Get(),frame);
        if(!public_transaction&&(!logical_production || production_sequence))frame=capture(list,h,p,production_sequence,guide_stream,
            kcd_production?&invocation_camera:nullptr,public_production,nullptr,nullptr,nullptr,
            original_public?1U:0U);
        if(!delegate_nr)neural_rendering::before_sr(identity,list,p,queue.Get());
    }catch(...){log("native_pre_sr_exception");}
    if(original_public&&gta_enhanced_process())
        GtaFinalColorScope::record(evaluation_metadata,native_list_identity.Get(),native_device_identity.Get(),frame,hdr,
            NVSDK_NGX_Result_Success,scope.can_complete());
    const auto result=forward();
    if(sr&&NVSDK_NGX_SUCCEED(result))try {
        if(!delegate_nr)neural_rendering::after_sr(identity,list,p,queue.Get());
        // GTA's enclosing final-caller scope marks after wrapper composition.
        if(scope.can_complete()&&!public_transaction&&!outlaws_family&&
            !(original_public&&gta_enhanced_process())) {
            if(original_public&&frame)complete_color_resource(list,frame->output.Get(),frame,hdr);
            else complete_color(list,p,frame,hdr);
        }
    }catch(...){log("native_completion_exception");}
    if(outlaws_family)OutlawsPublicInvocationScope::complete_native(evaluation_metadata.type,
        native_list_identity.Get(),native_device_identity.Get(),frame,hdr,result,scope.can_complete());
    if(public_transaction)invocation_camera.complete_native(result,scope.can_complete());
    invocation_camera.finish_public(result);
    if(original_public && (NVSDK_NGX_FAILED(result)||!scope.can_complete()))
        invalidate_dlss_motion_vector_stream_history(guide_stream);
    if(logical_production) {
        if(NVSDK_NGX_FAILED(result) || !scope.can_complete() || !production_sequence)
            invalidate_dlss_motion_vector_stream_history(guide_stream);
        if(production_sequence<=8 || production_sequence%300==0 || NVSDK_NGX_FAILED(result))
            log(kcd_production?"kcd_production_identity":"outlaws_production_identity",production_sequence,frame?frame->camera_frame_token:0,
                bool(frame),static_cast<UINT>(result));
        if(public_production&&(production_sequence<=8||production_sequence%300==0))
            log(kcd_production?"kcd_public_production":"outlaws_public_production",public_production,reinterpret_cast<UINT64>(h),production_sequence,guide_stream);
    }
    return gta_final.complete(result);
}
template<UINT slot> NVSDK_NGX_Result release(NVSDK_NGX_Handle* handle) {
    unsigned id{};std::uint64_t guide_stream=reinterpret_cast<UINT64>(handle);
    std::shared_ptr<OutlawsPublicLifetime> outlaws_lifetime;
    {std::scoped_lock lock(state().mutex);auto it=state().features.find(handle);if(it!=state().features.end())id=it->second.identity;
        if constexpr(slot==3)if(outlaws_public_process()) {
            const auto lifetime=state().outlaws_public_lifetimes.find(handle);
            if(lifetime!=state().outlaws_public_lifetimes.end())outlaws_lifetime=lifetime->second;
        }
        const auto stream=state().guide_streams.find(handle);if(stream!=state().guide_streams.end())guide_stream=stream->second;}
    const auto result=state().modules[slot].release(handle);
    if(NVSDK_NGX_SUCCEED(result)&&handle) {
        bool preserve_reused_stream{};
        {std::scoped_lock lock(state().mutex);
            if(outlaws_lifetime) {
                const auto current=state().outlaws_public_lifetimes.find(handle);
                if(current!=state().outlaws_public_lifetimes.end()&&current->second==outlaws_lifetime)
                    state().outlaws_public_lifetimes.erase(current);
                const auto stream=kKcdPublicStreamNamespace|outlaws_lifetime->identity;
                state().public_production_sequences.erase(stream);
                retire_dlss_motion_vector_stream(stream);
            }
            const auto current=state().features.find(handle);
            const auto current_lifetime=state().outlaws_public_lifetimes.find(handle);
            const bool newer=(current!=state().features.end()&&current->second.identity!=id)||
                (outlaws_lifetime&&current_lifetime!=state().outlaws_public_lifetimes.end()&&
                    current_lifetime->second!=outlaws_lifetime);
            if(newer) {
                const auto stream=state().guide_streams.find(handle);
                const auto current_stream=stream==state().guide_streams.end()
                    ? reinterpret_cast<UINT64>(handle):stream->second;
                preserve_reused_stream=current_stream==guide_stream;
            } else {
                state().features.erase(handle);state().native_production_sequences.erase(handle);
                state().guide_streams.erase(handle);
            }
            // A public family's key belongs to the snapped creation, even if
            // an inner release already retired it. A new creation gets a new
            // key and must keep all of its handle metadata and production clock.
            if(public_guide_stream(guide_stream)&&id==std::uint32_t(guide_stream))
                state().public_production_sequences.erase(guide_stream);
            // Serialize retirement with a new handle generation's registration.
            // These registry operations only retain/reset ownership; they wait
            // for no GPU work and cannot retire a newly published raw stream.
            if(!preserve_reused_stream) {
                if(public_guide_stream(guide_stream)&&id!=std::uint32_t(guide_stream))
                    invalidate_dlss_motion_vector_stream_history(guide_stream);
                else retire_dlss_motion_vector_stream(guide_stream);
            }
        }
        neural_rendering::retire_handle(id);
    }
    return result;
}
void install(UINT i,HMODULE mod,void* create_fn,void* eval_fn,void* release_fn,void* eval_c_fn = nullptr) {
    auto& m=state().modules[i];if(m.retained||!mod)return;
    auto c=GetProcAddress(mod,"NVSDK_NGX_D3D12_CreateFeature");
    auto e=GetProcAddress(mod,"NVSDK_NGX_D3D12_EvaluateFeature");
    auto r=GetProcAddress(mod,"NVSDK_NGX_D3D12_ReleaseFeature");if(!c||!e||!r)return;
    auto ec=GetProcAddress(mod,"NVSDK_NGX_D3D12_EvaluateFeature_C");
    // KCD embeds its public NGX SDK dispatcher in WHGame.dll. Its exports
    // and C-callback evaluation can bypass both the NVIDIA and REAL VR bodies.
    // Resolve only declared executable exports from the matching module PE.
    if(i==4) {
        c=native_export_address(mod,"NVSDK_NGX_D3D12_CreateFeature");
        e=native_export_address(mod,"NVSDK_NGX_D3D12_EvaluateFeature");
        r=native_export_address(mod,"NVSDK_NGX_D3D12_ReleaseFeature");
        ec=native_export_address(mod,"NVSDK_NGX_D3D12_EvaluateFeature_C");
        if(!c||!e||!r)return;
    }
    // KCD's existing injector can redirect exports before the XR layer loads.
    // Hook the actual typed implementation body used by cached original calls.
    if(i<2&&native_ingress::luke_detected()&&(kingdom_come_process()||cached_luke_title())) {
        const auto original_c=native_export_address(mod,"NVSDK_NGX_D3D12_CreateFeature");
        const auto original_e=native_export_address(mod,"NVSDK_NGX_D3D12_EvaluateFeature");
        const auto original_r=native_export_address(mod,"NVSDK_NGX_D3D12_ReleaseFeature");
        log("native_export_resolution",i,original_c&&original_e&&original_r,
            e!=original_e,(UINT64(reinterpret_cast<std::uintptr_t>(e)-reinterpret_cast<std::uintptr_t>(mod))));
        if(original_c&&original_e&&original_r){c=original_c;e=original_e;r=original_r;}
    }
    HMODULE implementation{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(e),&implementation);
    m.typed_implementation=i<2&&implementation==mod;
    HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(e),&retained))return;
    m.create_hook=safetyhook::create_inline(c,create_fn,SafetyHookInline::StartDisabled);
    m.evaluate_hook=safetyhook::create_inline(e,eval_fn,SafetyHookInline::StartDisabled);
    m.release_hook=safetyhook::create_inline(r,release_fn,SafetyHookInline::StartDisabled);
    const bool separate_c = ec && ec!=e && eval_c_fn;
    if(separate_c)m.evaluate_c_hook=safetyhook::create_inline(ec,eval_c_fn,SafetyHookInline::StartDisabled);
    if(!m.create_hook||!m.evaluate_hook||!m.release_hook||(separate_c&&!m.evaluate_c_hook)){
        m.create_hook={};m.evaluate_hook={};m.evaluate_c_hook={};m.release_hook={};FreeLibrary(retained);log("hook_failed",i);return;}
    m.create=m.create_hook.original<Create>();m.evaluate=m.evaluate_hook.original<Evaluate>();
    m.evaluate_c=separate_c ? m.evaluate_c_hook.original<Evaluate>() : m.evaluate;
    m.release=m.release_hook.original<Release>();m.retained=retained;
    (void)m.create_hook.enable();(void)m.evaluate_hook.enable();(void)m.release_hook.enable();log("hook_installed",i);
    if(separate_c){(void)m.evaluate_c_hook.enable();log("hook_c_installed",i);}
}
template<UINT slot> void install_dispatch(HMODULE module) {
    install(slot,module,reinterpret_cast<void*>(&create<slot>),
        reinterpret_cast<void*>(&evaluate<slot>),reinterpret_cast<void*>(&release<slot>),
        reinterpret_cast<void*>(&evaluate<slot,true>));
}
void install_feature_hooks() {
    install_dispatch<0>(GetModuleHandleW(L"nvngx_dlss.dll"));
    install_dispatch<1>(GetModuleHandleW(L"nvngx_dlssd.dll"));
    install_dispatch<2>(GetModuleHandleW(L"_nvngx.dll"));
    if(kingdom_come_process()||outlaws_public_process()||
        (cached_luke_title()&&native_ingress::luke_detected())) {
        const auto module=luke_dispatch_module();
        install_dispatch<3>(module);
    }
    if(kingdom_come_process()) {
        install_dispatch<4>(GetModuleHandleW(L"WHGame.dll"));
    }
    native_ingress::poll_streamline();
    native_ingress::poll_d3d11_ngx();
    native_vulkan::poll_capture();
    native_vulkan::poll_guide_capture();
}
void start_hook_monitor() {
    static std::atomic_bool started{};if(started.exchange(true))return;
    HMODULE pinned{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&start_hook_monitor),&pinned))return;
    native_ingress::start_luke_detection_notifications();
    std::thread([]{for(;;){native_ingress::refresh_luke_detection(false);install_feature_hooks();Sleep(50);}}).detach();
}
// NGX looks for a feature's module only along the path list of the process's
// first initialisation, which is usually the game's own DLSS: OFXR's later
// one succeeds yet reports frame generation not found when the game ships no
// nvngx_dlssg.dll. OFXR's directory goes last on every initialisation's list,
// so a game's own copy still wins. Both core entry points take the list as
// their last argument; the SDK's static library routes every D3D12
// initialisation through one of them.
using InitExt=NVSDK_NGX_Result(*)(unsigned long long,const wchar_t*,ID3D12Device*,
    NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
using InitProject=NVSDK_NGX_Result(*)(const char*,NVSDK_NGX_EngineType,const char*,
    const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
struct Discovery {
    SafetyHookInline ext_hook, project_hook;
    InitExt ext{}; InitProject project{};
    std::wstring directory;
};
Discovery& discovery() { static auto* d=new Discovery; return *d; }
struct DiscoveryInfo {
    std::unique_ptr<NgxFeaturePaths> paths;
    const NVSDK_NGX_FeatureCommonInfo* with_directory(const NVSDK_NGX_FeatureCommonInfo* given) {
        paths=std::make_unique<NgxFeaturePaths>(given,discovery().directory);
        log("ngx_discovery_paths",paths->get()->PathListInfo.Length);
        return paths->changed()?paths->get():given;
    }
};
NVSDK_NGX_Result init_ext(unsigned long long id,const wchar_t* data,ID3D12Device* device,
    NVSDK_NGX_Version version,const NVSDK_NGX_FeatureCommonInfo* given) {
    DiscoveryInfo extended; const NVSDK_NGX_FeatureCommonInfo* info=given;
    try { info=extended.with_directory(given); } catch(...) {}
    native_ingress::install_device(device);
    install_feature_hooks();
    const auto result=discovery().ext(id,data,device,version,info);
    log("ngx_init_ext_result",static_cast<UINT>(result),id,
        info?info->PathListInfo.Length:0);
    return result;
}
NVSDK_NGX_Result init_project(const char* project,NVSDK_NGX_EngineType engine,
    const char* engine_version,const wchar_t* data,ID3D12Device* device,
    NVSDK_NGX_Version version,const NVSDK_NGX_FeatureCommonInfo* given) {
    DiscoveryInfo extended; const NVSDK_NGX_FeatureCommonInfo* info=given;
    try { info=extended.with_directory(given); } catch(...) {}
    native_ingress::install_device(device);
    install_feature_hooks();
    const auto result=discovery().project(project,engine,engine_version,data,device,version,info);
    log("ngx_init_project_result",static_cast<UINT>(result),static_cast<UINT>(engine),
        info?info->PathListInfo.Length:0);
    return result;
}
// The driver's NGX core, as the SDK finds it when nothing ships next to the
// game.
HMODULE load_ngx_core() {
    if(auto core=GetModuleHandleW(L"_nvngx.dll")) { log("ngx_core_already_loaded"); return core; }
    wchar_t directory[MAX_PATH]{}; DWORD size=sizeof(directory);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
        L"FullPath",RRF_RT_REG_SZ,nullptr,directory,&size)!=ERROR_SUCCESS)return nullptr;
    std::wstring path=directory; path+=L"\\_nvngx.dll";
    return LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
}
} // namespace
#ifdef XRFG_NATIVE_INGRESS_TESTS
#include "ngx_dispatch_test.inc"
#endif
void prepare_ngx_feature_discovery(const wchar_t* directory) noexcept {
    try {
        native_ingress::refresh_luke_detection();
        auto& d=discovery(); if(d.ext_hook||d.project_hook||!directory||!*directory)return;
        const HMODULE core=load_ngx_core(); if(!core)return;
        install_feature_hooks();start_hook_monitor();
        auto ext=GetProcAddress(core,"NVSDK_NGX_D3D12_Init_Ext");
        auto project=GetProcAddress(core,"NVSDK_NGX_D3D12_Init_ProjectID");
        if(!ext||!project)return;
        HMODULE pin{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&prepare_ngx_feature_discovery),&pin);
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(ext),&pin);
        d.directory=directory;
        d.ext_hook=safetyhook::create_inline(ext,reinterpret_cast<void*>(&init_ext),
            SafetyHookInline::StartDisabled);
        d.project_hook=safetyhook::create_inline(project,reinterpret_cast<void*>(&init_project),
            SafetyHookInline::StartDisabled);
        if(!d.ext_hook||!d.project_hook){d.ext_hook={};d.project_hook={};log("ngx_discovery_hook_failed");return;}
        d.ext=d.ext_hook.original<InitExt>(); d.project=d.project_hook.original<InitProject>();
        (void)d.ext_hook.enable(); (void)d.project_hook.enable(); log("ngx_discovery_hooked");
    } catch(...) {}
}
void configure_ngx_guide_capture(ID3D12CommandQueue* queue,bool enabled) noexcept {
    try {
        ComPtr<ID3D12Device> luke_device;ComPtr<ID3D12CommandQueue> luke_queue;
        if(queue && native_ingress::query_luke_context(luke_device.GetAddressOf(),luke_queue.GetAddressOf()))
            queue=luke_queue.Get();
        if(queue)native_ingress::configure_queue(queue);
        auto& s=state(); {std::scoped_lock lock(s.mutex);
            if(s.queue.Get()!=queue){s.camera_valid=false;s.camera_from_xr=false;s.camera_ready=false;}
            s.queue=queue;}
        if(queue&&!s.debug_queue) {
            auto nq=native(queue);ComPtr<ID3D12Device> device;
            if(SUCCEEDED(nq->GetDevice(IID_PPV_ARGS(&device)))&&
               SUCCEEDED(device.As(&s.debug_queue))) {
                s.debug_queue->RegisterMessageCallback(
                    [](D3D12_MESSAGE_CATEGORY category,D3D12_MESSAGE_SEVERITY severity,
                       D3D12_MESSAGE_ID id,LPCSTR description,void*) {
                        if(severity<=D3D12_MESSAGE_SEVERITY_WARNING&&id!=1424)log(description,id,category,severity);
                    },D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&s.debug_cookie);
                log("debug_messages_enabled");
            }
        }
        s.active=enabled&&queue;
        if(enabled&&queue)configure_dlss_motion_vector_tracking(true);
        // Observe SR/RR input availability even when optical flow needs no
        // owned guide copies. capture() still checks s.active before GPU work.
        if(!queue||s.started.exchange(true))return;
        HMODULE pin{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&configure_ngx_guide_capture),&pin);
        if(GetEnvironmentVariableW(L"XRFG_TEST_NATIVE_DLSSG_DRED",nullptr,0)) {
            auto nq=native(queue);ComPtr<ID3D12Device> device;
            if(SUCCEEDED(nq->GetDevice(IID_PPV_ARGS(&device)))) {
                std::thread([device] {
                    for(;;) {
                        const HRESULT reason=device->GetDeviceRemovedReason();
                        if(FAILED(reason)) {
                            log("device_removed_dred",static_cast<UINT>(reason));
                            ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
                            if(SUCCEEDED(device.As(&dred))) {
                                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 data{};
                                if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&data))) {
                                    for(auto* node=data.pHeadAutoBreadcrumbNode;node;node=node->pNext) {
                                        const auto done=node->pLastBreadcrumbValue?*node->pLastBreadcrumbValue:0;
                                        if(done<node->BreadcrumbCount)log(node->pCommandListDebugNameA?
                                            node->pCommandListDebugNameA:"dred_list",done,node->BreadcrumbCount,
                                            done<node->BreadcrumbCount?node->pCommandHistory[done]:0);
                                    }
                                }
                            }
                            return;
                        }
                        Sleep(1);
                    }
                }).detach();
            }
        }
        start_hook_monitor();log("enabled");
    } catch(...) {}
}
void set_ngx_capture_steamvr_runtime(bool steamvr) noexcept {
    capture_steamvr_runtime.store(steamvr,std::memory_order_relaxed);
}
void stop_ngx_guide_capture(ID3D12CommandQueue* queue) noexcept {
    native_ingress::stop_queue(queue);
    auto& s=state(); std::scoped_lock lock(s.mutex);
    if(s.queue.Get()==queue) {s.active=false;s.queue.Reset();}
}
void update_ngx_guide_depth(float near_z,float far_z,float min_depth,float max_depth) noexcept {
    if(min_depth!=0.F||max_depth!=1.F||!std::isfinite(near_z)||!std::isfinite(far_z)||
        near_z<=0||far_z<=0||near_z==far_z)return;
    auto& s=state();std::scoped_lock lock(s.mutex);
    s.depth_inverted=near_z>far_z;
    s.camera_near=std::min(near_z,far_z);
    s.camera_far=std::max(near_z,far_z);
    s.depth_infinite=s.camera_far>1e20F;
    if(s.depth_infinite)s.camera_far=1000.F;
    s.camera_valid=true;
    s.camera_from_xr=true;
    s.camera_ready=true;
}
} // namespace xrfg
#else
namespace xrfg {
void configure_ngx_guide_capture(ID3D12CommandQueue*,bool) noexcept {}
void set_ngx_capture_steamvr_runtime(bool) noexcept {}
void stop_ngx_guide_capture(ID3D12CommandQueue*) noexcept {}
void update_ngx_guide_depth(float,float,float,float) noexcept {}
void prepare_ngx_feature_discovery(const wchar_t*) noexcept {}
}
#endif
