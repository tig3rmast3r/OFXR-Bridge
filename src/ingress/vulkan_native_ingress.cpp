#include "xrfg/vulkan_native_ingress.hpp"
#include "xrfg/upscaler_observation.hpp"
#include "xrfg/vulkan_command_stream.hpp"
#include "xrfg/native_ingress.hpp"
#include "xrfg/neural_rendering.hpp"
#include "xrfg/embedded_control.hpp"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_vulkan_scope.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/ngx_feature_metadata.hpp"
#include "xrfg/ngx_evaluation_boundary.hpp"
#include "xrfg/native_export_address.hpp"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/two_frame_capture.hpp"
#include "vulkan_device_request.hpp"
#include "vulkan_d24_decode_shader.hpp"
#include <nvsdk_ngx_vk.h>
#include <safetyhook.hpp>
#include <wrl/client.h>
#include <windows.h>
#include <array>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <utility>
#include <bit>
#include <cstddef>
#include <functional>

namespace xrfg::native_vulkan {
namespace {
using Microsoft::WRL::ComPtr;
struct Device;
struct D24Decoder {
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
};
enum Role{color,motion,depth,output,role_count};
#if defined(XRFG_NATIVE_INGRESS_TESTS)
std::uint64_t tested_native_output_copyback_pixels{};
#endif
struct Surface {
    ComPtr<ID3D12Resource> buffer,texture;
    ComPtr<ID3D12Resource> packed_depth;
    ComPtr<ID3D12DescriptorHeap> depth_views;
    std::shared_ptr<D24Decoder> depth_decoder;
    UINT depth_view_stride{};
    VkBuffer imported{};VkDeviceMemory memory{};
    NVSDK_NGX_ImageViewInfo_VK source{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    bool native_used{};
};
struct Slot {
    std::array<Surface,role_count> surfaces;
    // F8-only witness; never substituted for the production depth guide.
    Surface diagnostic_late_depth;
    bool diagnostic_late_depth_before{};
    Surface diagnostic_native_color;
    bool diagnostic_native_color_before{};
    ComPtr<ID3D12Fence> fence,d12_done;VkSemaphore timeline{};
    std::uint64_t value{},final_value{},d12_value{};
    unsigned pending_callbacks{},submitted_phases{},recording_leases{};
    std::array<bool,role_count> native_before{};
    std::array<ComPtr<ID3D12CommandAllocator>,2> allocators;
    std::array<ComPtr<ID3D12GraphicsCommandList>,2> lists;
    bool complete()const {
        if(pending_callbacks||recording_leases)return false;if(!final_value)return true;
        const auto native_completed=fence->GetCompletedValue(),d12_completed=d12_done->GetCompletedValue();
        return native_completed!=UINT64_MAX&&native_completed>=final_value&&
            d12_completed!=UINT64_MAX&&d12_completed>=d12_value;
    }
};
struct Device {
    VulkanSessionBinding binding{};
    PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
    std::shared_ptr<D24Decoder> depth_decoder;
    VkPhysicalDeviceMemoryProperties memory{};
    std::recursive_mutex mutex;
    std::vector<std::shared_ptr<Slot>> slots;
    bool native_alive{true};
    bool owner_active{true};
    bool production_submitted{};
};
struct RecordingLease {
    std::shared_ptr<Device> device;std::shared_ptr<Slot> slot;
    std::shared_ptr<RecordingReceipt> original_receipt;
    std::array<bool,role_count> original_before{};
    bool diagnostic_before{};
    bool native_color_before{};
    ~RecordingLease(){std::scoped_lock lock(device->mutex);
        if(original_receipt&&original_receipt->disposition==RecordingDisposition::abandoned&&!slot->submitted_phases) {
            for(unsigned r=0;r<role_count;++r)slot->surfaces[r].native_used=original_before[r];
            slot->diagnostic_late_depth.native_used=diagnostic_before;
            slot->diagnostic_native_color.native_used=native_color_before;
        }
        if(slot->recording_leases)--slot->recording_leases;}
};
bool retain_transport_recording(const std::shared_ptr<Device>& d,const std::shared_ptr<Slot>& s,VkCommandBuffer original,
    std::shared_ptr<RecordingReceipt> receipt={}) {
    auto lease=std::make_shared<RecordingLease>();lease->device=d;lease->slot=s;lease->original_receipt=std::move(receipt);
    lease->original_before=s->native_before;
    lease->diagnostic_before=s->diagnostic_late_depth_before;++s->recording_leases;
    lease->native_color_before=s->diagnostic_native_color_before;
    return retain_recording(original,std::move(lease));
}
struct Feature:NgxFeatureMetadata {VkDevice device{};};
struct PublicFeatureLifetime {
    std::uint32_t identity{};
    VkDevice device{};
    NVSDK_NGX_Feature type{};
    bool type_known{};
    std::uint64_t sequence{},failure_epoch{};
    unsigned active_calls{};
};
constexpr std::uint64_t kVulkanPublicStreamNamespace=std::uint64_t{1}<<62;
class EvaluationDeviceScope {
    inline static thread_local EvaluationDeviceScope* current_{};
    EvaluationDeviceScope* parent_{};
    Feature feature_{};VkCommandBuffer recording_{};bool public_dispatch_{};
public:
    EvaluationDeviceScope(const Feature& feature,VkCommandBuffer recording,bool public_dispatch) noexcept
        :parent_(current_),feature_(feature),recording_(recording),public_dispatch_(public_dispatch) {
        if(public_dispatch_&&(!feature_.creation_observed||feature_.creation_source_identity!=feature_.identity)) {
            Feature inherited;
            if(inherit(native(recording),feature_.type,inherited))feature_=inherited;
        }
        current_=this;
    }
    ~EvaluationDeviceScope(){current_=parent_;}
    static const EvaluationDeviceScope* exact(VkCommandBuffer command,NVSDK_NGX_Feature type) noexcept {
        return current_&&command&&current_->recording_&&current_->feature_.type==type&&
            native(current_->recording_)==command&&device_for(current_->recording_)==current_->feature_.device?
            current_:nullptr;
    }
    static bool inherit(VkCommandBuffer command,NVSDK_NGX_Feature type,Feature& feature) noexcept {
        const auto scope=exact(command,type);
        if(!scope||!scope->public_dispatch_||!scope->feature_.creation_observed||
            (type!=NVSDK_NGX_Feature_SuperSampling&&type!=NVSDK_NGX_Feature_RayReconstruction))return false;
        feature=scope->feature_;feature.identity=0;return true;
    }
    VkDevice device()const noexcept{return feature_.device;}
    VkCommandBuffer recording()const noexcept{return recording_;}
};
using Create=NVSDK_NGX_Result(*)(VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Create1=NVSDK_NGX_Result(*)(VkDevice,VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate=NVSDK_NGX_Result(*)(VkCommandBuffer,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,void*);
using Release=NVSDK_NGX_Result(*)(NVSDK_NGX_Handle*);
struct Module {SafetyHookInline c,c1,e,r;Create create{};Create1 create1{};Evaluate evaluate{};Release release{};HMODULE retained{};bool typed_implementation{};};
struct Hooks {
    std::mutex mutex;
    std::array<Module,4> modules;
    std::unordered_map<const NVSDK_NGX_Handle*,Feature> features;
    // A marked public evaluation may predate creation interception. Its logical
    // lifetime is deliberately separate from immutable NGX creation metadata.
    std::unordered_map<const NVSDK_NGX_Handle*,std::shared_ptr<PublicFeatureLifetime>> public_lifetimes;
    std::unordered_map<VkDevice,std::shared_ptr<Device>> devices;
    std::unordered_map<VkDevice,bool> interop_devices;
    std::unordered_map<void*,VkInstance> instances;
    std::unordered_map<VkDevice,VulkanSessionBinding> observed_devices;
    struct DeviceImageDispatch {
        PFN_vkGetDeviceProcAddr resolver{};
        bool creation{},destruction{};
    };
    std::unordered_map<VkDevice,DeviceImageDispatch> device_image_dispatch;
    std::vector<std::shared_ptr<Device>> retired;
    // Observed native allocations carry actual transfer usage. Unknown images
    // reject capture; metadata cannot invent Vulkan allocation capabilities.
    struct ImageAllocation {
        VkDevice device{};VkImageUsageFlags usage{};VkImageCreateFlags flags{};
        VkImageType type{};VkExtent3D extent{};uint32_t mips{},layers{};
        VkSampleCountFlagBits samples{};VkFormat format{};
        std::uint64_t creation{};const void* creator{};
    };
    std::unordered_map<VkImage,ImageAllocation> image_usage;
    std::uint64_t image_creations{};
    // Device dispatchers can be cached before a replacement GDPA is called.
    // Keep each actual entry's original trampoline, rather than forwarding all
    // ICD/layer entry points through the loader's one generic trampoline.
    static constexpr unsigned image_entry_capacity=8;
    struct ImageEntry {SafetyHookInline hook;void* address{};PFN_vkVoidFunction original{};};
    std::array<ImageEntry,image_entry_capacity> image_create_entries,image_destroy_entries;
    void* loader_create_image{};void* loader_destroy_image{};
    SafetyHookInline luke_gdpa_hook;PFN_vkGetDeviceProcAddr luke_gdpa{};
    SafetyHookInline gdpa_hook,gipa_hook,create_image_hook,destroy_image_hook;
    SafetyHookInline create_device_hook,destroy_device_hook;
    PFN_vkGetDeviceProcAddr gdpa{};PFN_vkGetInstanceProcAddr gipa{};
    PFN_vkCreateImage create_image{};PFN_vkDestroyImage destroy_image{};
    PFN_vkCreateDevice create_device{};PFN_vkDestroyDevice destroy_device{};
};
Hooks& hooks(){static auto* h=new Hooks;return *h;}
struct OriginalPublicCapture;
std::shared_ptr<OriginalPublicCapture> prepare_original_public_capture(
    VkDevice,VkCommandBuffer,const NVSDK_NGX_Parameter*);
class PublicFeatureInvocationScope {
    inline static thread_local PublicFeatureInvocationScope* current_{};
    PublicFeatureInvocationScope* parent_{};
    const NVSDK_NGX_Handle* handle_{};
    VkCommandBuffer recording_{};
    VkDevice device_{};
    std::shared_ptr<PublicFeatureLifetime> lifetime_;
    std::uint64_t failure_epoch_{};
    std::uint64_t production_sequence_{};
    const NVSDK_NGX_Handle* native_handle_{};std::uint32_t native_identity_{};
    std::shared_ptr<OriginalPublicCapture> original_capture_;
    std::function<bool()> final_capture_;
    unsigned claims_{};
    bool active_{},rejected_{},completed_{},leased_{},preparation_attempted_{},capture_consumed_{},native_succeeded_{};
    bool live_locked()const noexcept {
        const auto found=hooks().public_lifetimes.find(handle_);
        return lifetime_&&found!=hooks().public_lifetimes.end()&&found->second==lifetime_&&
            lifetime_->failure_epoch==failure_epoch_&&lifetime_->active_calls==1;
    }
    void reject()noexcept {
        rejected_=true;
        if(lifetime_)retire_dlss_motion_vector_stream(kVulkanPublicStreamNamespace|lifetime_->identity);
    }
public:
    PublicFeatureInvocationScope(unsigned slot,const NVSDK_NGX_Handle* handle,VkCommandBuffer recording)noexcept {
        if(slot!=3)return;
        active_=true;parent_=current_;current_=this;
        handle_=handle;recording_=recording;device_=device_for(recording);
        if(parent_){parent_->reject();rejected_=true;return;}
        if(!handle||!recording||!native(recording)||!device_){rejected_=true;return;}
        try {
            std::scoped_lock lock(hooks().mutex);
            auto found=hooks().public_lifetimes.find(handle);
            if(found==hooks().public_lifetimes.end()) {
                if(hooks().public_lifetimes.size()>=128){rejected_=true;return;}
                auto lifetime=std::make_shared<PublicFeatureLifetime>();
                lifetime->identity=native_ingress::allocate_handle_key();lifetime->device=device_;
                if(!lifetime->identity){rejected_=true;return;}
                if(const auto created=hooks().features.find(handle);created!=hooks().features.end()&&created->second.creation_observed) {
                    if(created->second.device!=device_){rejected_=true;return;}
                    lifetime->type=created->second.type;lifetime->type_known=true;
                }
                found=hooks().public_lifetimes.emplace(handle,std::move(lifetime)).first;
            }
            lifetime_=found->second;
            failure_epoch_=lifetime_->failure_epoch;
            ++lifetime_->active_calls;leased_=true;
            if(lifetime_->device!=device_||lifetime_->active_calls!=1) {
                ++lifetime_->failure_epoch;reject();
            }
        }catch(...){rejected_=true;}
    }
    ~PublicFeatureInvocationScope() {
        if(!active_)return;
        if(!completed_)reject();
        if(leased_)try {std::scoped_lock lock(hooks().mutex);if(lifetime_->active_calls)--lifetime_->active_calls;}catch(...){}
        current_=parent_;
    }
    PublicFeatureInvocationScope(const PublicFeatureInvocationScope&)=delete;
    void prepare(const NVSDK_NGX_Parameter* p)noexcept {
        if(!active_)return;preparation_attempted_=true;
        if(rejected_)return;
        try{original_capture_=prepare_original_public_capture(device_,recording_,p);}catch(...){reject();}
    }
    static bool active()noexcept{return current_!=nullptr;}
    static VkCommandBuffer exact_recording(VkCommandBuffer command,VkDevice device)noexcept {
        const auto* owner=current_;
        return owner&&!owner->rejected_&&command&&owner->recording_&&
            native(owner->recording_)==command&&(!device||device==owner->device_)?owner->recording_:VkCommandBuffer{};
    }
    // Only the actual typed SR/RR body claims/counts production. A rejected
    // authoritative public call must not fall back to a raw-eye handle stream.
    static bool claim(NVSDK_NGX_Feature type,VkCommandBuffer recording,VkDevice device,
        std::uint64_t& stream,std::uint64_t& sequence)noexcept {
        stream=sequence=0;auto* owner=current_;if(!owner)return false;
        if(owner->rejected_||!recording||native(owner->recording_)!=native(recording)||
            !device||device!=owner->device_||
            (type!=NVSDK_NGX_Feature_SuperSampling&&type!=NVSDK_NGX_Feature_RayReconstruction)||owner->claims_++) {
            owner->reject();return true;
        }
        try {
            std::scoped_lock lock(hooks().mutex);
            if(!owner->live_locked()||(owner->lifetime_->type_known&&owner->lifetime_->type!=type)||
                owner->lifetime_->sequence==UINT64_MAX){owner->reject();return true;}
            owner->lifetime_->type=type;owner->lifetime_->type_known=true;
            stream=kVulkanPublicStreamNamespace|owner->lifetime_->identity;
            sequence=++owner->lifetime_->sequence;
            owner->production_sequence_=sequence;
            if(sequence<=8||sequence%300==0)
                bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,type,1032,stream,sequence);
        }catch(...){stream=sequence=0;owner->reject();}
        return true;
    }
    static void native_completed(NVSDK_NGX_Result result,bool eligible=true)noexcept {
        if(current_){if(NVSDK_NGX_FAILED(result)||!eligible)current_->reject();
            else current_->native_succeeded_=true;}
    }
    // The marked public invocation owns this attempt, including an absent or
    // failed original snapshot. Typed modified guides cannot replace it.
    static bool consume_original(std::shared_ptr<OriginalPublicCapture>& capture)noexcept {
        auto* owner=current_;if(!owner)return false;capture.reset();
        if(owner->rejected_||!owner->preparation_attempted_||owner->claims_!=1||owner->capture_consumed_){owner->reject();return true;}
        owner->capture_consumed_=true;capture=owner->original_capture_;return true;
    }
    static void final_capture(std::function<bool()> completion,const NVSDK_NGX_Handle* handle,std::uint32_t identity) {
        if(current_){current_->final_capture_=std::move(completion);current_->native_handle_=handle;current_->native_identity_=identity;}
    }
    static bool mark_original(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
        const std::shared_ptr<const DlssMotionVectorFrame>& frame,ID3D12Device* device,LukeColorOriginRect rectangle,
        LukeColorOriginRect* written_rectangle=nullptr)noexcept;
    static VkResult copy_original(VkCommandBuffer,const std::shared_ptr<Device>&,const std::shared_ptr<Slot>&,
        const std::shared_ptr<const DlssMotionVectorFrame>&,uint32_t,const LukeColorOriginRect* rectangle=nullptr)noexcept;
    static bool refuse_child()noexcept {
        if(!current_)return false;current_->reject();return true;
    }
    NVSDK_NGX_Result complete(NVSDK_NGX_Result result)noexcept {
        if(!active_)return result;completed_=true;
        bool live{};try{std::scoped_lock lock(hooks().mutex);live=live_locked();}catch(...){}
        if(NVSDK_NGX_FAILED(result)||rejected_||claims_!=1||!native_succeeded_||!live||
            (original_capture_&&!final_capture_))reject();
        else if(final_capture_)try{if(!final_capture_())reject();}catch(...){reject();}
        return result;
    }
};
void report_image_coverage(VkDevice device) noexcept;
bool ingress_reason(unsigned reason,std::uint64_t detail=0) noexcept {
    static std::array<std::atomic<ULONGLONG>,16> last{};
    if(reason>=last.size())return false;
    const auto now=GetTickCount64()+1;auto before=last[reason].load();
    if(before && now-before<1000)return false;
    if(!last[reason].compare_exchange_strong(before,now))return false;
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,reason,954,detail,0);return true;
}
template<class T>T get(const NVSDK_NGX_Parameter* p,const char* name,T fallback={}) {
    T value=fallback;return p&&NVSDK_NGX_SUCCEED(p->Get(name,&value))?value:fallback;
}
template<class T>T api(const Device& d,const char* name){return reinterpret_cast<T>(unhooked_command(d.gdpa(d.binding.device,name)));}
void destroy_surface(Device& d,Surface& s) {
    if(s.imported)if(auto fn=api<PFN_vkDestroyBuffer>(d,"vkDestroyBuffer"))fn(d.binding.device,s.imported,nullptr);
    if(s.memory)if(auto fn=api<PFN_vkFreeMemory>(d,"vkFreeMemory"))fn(d.binding.device,s.memory,nullptr);
    s={};
}
void collect_retired() {
    std::vector<std::shared_ptr<Device>> retired;
    {std::scoped_lock lock(hooks().mutex);retired=hooks().retired;}
    for(const auto& d:retired) {
        std::unique_lock lock(d->mutex,std::try_to_lock);if(!lock.owns_lock())continue;
        if(!std::all_of(d->slots.begin(),d->slots.end(),[](const auto& s){return s->complete();}))continue;
        for(auto& s:d->slots) {
            if(d->native_alive) {
                for(auto& surface:s->surfaces)destroy_surface(*d,surface);
                destroy_surface(*d,s->diagnostic_late_depth);
                destroy_surface(*d,s->diagnostic_native_color);
                if(s->timeline)if(auto fn=api<PFN_vkDestroySemaphore>(*d,"vkDestroySemaphore"))fn(d->binding.device,s->timeline,nullptr);
            }
            s->timeline={};
        }
        d->slots.clear();native_ingress::stop_queue(d->queue.Get());
        {std::scoped_lock registry_lock(hooks().mutex);std::erase(hooks().retired,d);}
    }
}
struct ImageReceipt {
    const void* borrowed_address{};
    NVSDK_NGX_Result parameter_result{NVSDK_NGX_Result_Success};
    NVSDK_NGX_Resource_VK borrowed{},resolved{};
    Hooks::ImageAllocation allocation{};
    uint32_t rejection{};
};
ImageReceipt image_receipt(const NVSDK_NGX_Resource_VK* borrowed,VkDevice device) {
    ImageReceipt receipt;receipt.borrowed_address=borrowed;
    if(!borrowed){receipt.rejection=1;return receipt;}
    // NGX returns a borrowed CPU descriptor. Freeze its value before invoking
    // native evaluation; neither SDK mutation nor another call may retarget it.
    receipt.borrowed=receipt.resolved=*borrowed;
    if(receipt.borrowed.Type!=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW){receipt.rejection=2;return receipt;}
    auto& image=receipt.resolved.Resource.ImageViewInfo;const auto& range=image.SubresourceRange;
    receipt.rejection=(!image.Image?4U:0U)|(!image.ImageView?8U:0U)|
        (image.Format==VK_FORMAT_UNDEFINED?16U:0U)|(!image.Width||!image.Height?32U:0U);
    {std::scoped_lock lock(hooks().mutex);const auto found=hooks().image_usage.find(image.Image);
        if(found!=hooks().image_usage.end())receipt.allocation=found->second;
    }
    const auto& allocation=receipt.allocation;
    if(!allocation.device){receipt.rejection|=64;return receipt;}
    receipt.rejection|=(allocation.device!=device?128U:0U)|
        ((allocation.flags&VK_IMAGE_CREATE_PROTECTED_BIT)?256U:0U)|
        (allocation.type!=VK_IMAGE_TYPE_2D||allocation.extent.depth!=1?512U:0U)|
        (allocation.samples!=VK_SAMPLE_COUNT_1_BIT?1024U:0U);
    const auto effective=[](uint32_t base,uint32_t count,uint32_t total,uint32_t remaining) {
        return base>=total?0U:count==remaining?total-base:count<=total-base?count:0U;
    };
    const auto levels=effective(range.baseMipLevel,range.levelCount,allocation.mips,VK_REMAINING_MIP_LEVELS);
    const auto layers=effective(range.baseArrayLayer,range.layerCount,allocation.layers,VK_REMAINING_ARRAY_LAYERS);
    receipt.rejection|=(!levels?2048U:0U)|(!layers?4096U:0U)|
        (levels&&levels!=1?8192U:0U)|(layers&&layers!=1?16384U:0U);
    const auto mip_extent=[](uint32_t extent,uint32_t mip){return mip>=32?1U:std::max(1U,extent>>mip);};
    if(levels&&(image.Width>mip_extent(allocation.extent.width,range.baseMipLevel)||
        image.Height>mip_extent(allocation.extent.height,range.baseMipLevel)))receipt.rejection|=32768;
    // A differing mutable view format needs its own observed view contract;
    // this transport does not infer one from equal byte counts.
    if(image.Format!=allocation.format)receipt.rejection|=65536;
    if(!range.aspectMask)receipt.rejection|=131072;
    if(!receipt.rejection) {
        image.SubresourceRange.levelCount=levels;image.SubresourceRange.layerCount=layers;
    }
    return receipt;
}
void report_image_receipt(unsigned role,const ImageReceipt& receipt,VkDevice owner,VkCommandBuffer command) noexcept {
    if(!ingress_reason(4,(std::uint64_t(role)<<32)|receipt.rejection))return;
    report_image_coverage(owner);
    const auto& image=receipt.borrowed.Resource.ImageViewInfo;
    const auto& range=image.SubresourceRange;const auto& allocation=receipt.allocation;
    const auto pair=[](uint32_t high,uint32_t low){return (std::uint64_t(high)<<32)|low;};
    unsigned char read_write{};std::memcpy(&read_write,&receipt.borrowed.ReadWrite,sizeof(read_write));
    auto& flight=bridge_flight_logger();
    flight.event(BridgeFlightOperation::luke_afw_stage,role,964,reinterpret_cast<std::uint64_t>(receipt.borrowed_address),
        pair(receipt.borrowed.Type,read_write));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,965,reinterpret_cast<std::uint64_t>(image.Image),reinterpret_cast<std::uint64_t>(image.ImageView));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,966,pair(range.baseMipLevel,range.levelCount),pair(range.baseArrayLayer,range.layerCount));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,967,pair(image.Width,image.Height),pair(image.Format,range.aspectMask));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,968,reinterpret_cast<std::uint64_t>(allocation.device),pair(allocation.usage,allocation.flags));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,969,pair(allocation.extent.width,allocation.extent.height),pair(allocation.mips,allocation.layers));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,970,pair(allocation.type,allocation.samples),pair(allocation.extent.depth,allocation.format));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,971,reinterpret_cast<std::uint64_t>(owner),reinterpret_cast<std::uint64_t>(command));
    flight.event(BridgeFlightOperation::luke_afw_stage,role,972,receipt.parameter_result,
        pair(sizeof(NVSDK_NGX_Resource_VK),offsetof(NVSDK_NGX_Resource_VK,Type)));
}
DXGI_FORMAT format(VkFormat f,Role role) {
    if(role==motion){if(f==VK_FORMAT_R16G16_SFLOAT)return DXGI_FORMAT_R16G16_FLOAT;
        if(f==VK_FORMAT_R16G16_SNORM)return DXGI_FORMAT_R16G16_SNORM;
        if(f==VK_FORMAT_R16G16B16A16_SFLOAT)return DXGI_FORMAT_R16G16B16A16_FLOAT;
        if(f==VK_FORMAT_R32G32_SFLOAT)return DXGI_FORMAT_R32G32_FLOAT;return DXGI_FORMAT_UNKNOWN;}
    if(role==depth){if(f==VK_FORMAT_D32_SFLOAT||f==VK_FORMAT_D32_SFLOAT_S8_UINT||f==VK_FORMAT_R32_SFLOAT)return DXGI_FORMAT_R32_FLOAT;
        if(f==VK_FORMAT_D24_UNORM_S8_UINT)return DXGI_FORMAT_R32_FLOAT;
        if(f==VK_FORMAT_D16_UNORM||f==VK_FORMAT_R16_UNORM)return DXGI_FORMAT_R16_UNORM;return DXGI_FORMAT_UNKNOWN;}
    return dxgi_format_for_vulkan(f);
}
unsigned pixel_bytes(DXGI_FORMAT f) {
    switch(f){case DXGI_FORMAT_R16_UNORM:return 2;case DXGI_FORMAT_R16G16_FLOAT:case DXGI_FORMAT_R16G16_SNORM:case DXGI_FORMAT_R32_FLOAT:case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:return 4;
    case DXGI_FORMAT_R32G32_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:return 16;default:return 0;}
}
void d12_barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    if(from==to)return;D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
}
HRESULT initialize_d24_decoder(Device& d,Surface& s,D3D12_RESOURCE_DESC texture) {
    if(!d.depth_decoder) {
        auto decoder=std::make_shared<D24Decoder>();
        std::array<D3D12_DESCRIPTOR_RANGE,2> ranges{};
        std::array<D3D12_ROOT_PARAMETER,3> parameters{};
        for(unsigned i=0;i<2;++i) {
            ranges[i].RangeType=i?D3D12_DESCRIPTOR_RANGE_TYPE_UAV:D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[i].NumDescriptors=1;
            parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable={1,&ranges[i]};
        }
        parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.Num32BitValues=2;
        D3D12_ROOT_SIGNATURE_DESC description{};description.NumParameters=3;description.pParameters=parameters.data();
        ComPtr<ID3DBlob> blob,errors;
        HRESULT result=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors);
        if(SUCCEEDED(result))result=d.owner->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&decoder->root));
        if(FAILED(result))return result;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=decoder->root.Get();
        pipeline.CS={g_xrfg_vulkan_d24_decode_shader,sizeof(g_xrfg_vulkan_d24_decode_shader)};
        result=d.owner->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&decoder->pipeline));
        if(FAILED(result))return result;d.depth_decoder=std::move(decoder);
    }
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    texture.Format=DXGI_FORMAT_R32_UINT;texture.Flags=D3D12_RESOURCE_FLAG_NONE;
    HRESULT result=d.owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.packed_depth));
    if(FAILED(result))return result;
    D3D12_DESCRIPTOR_HEAP_DESC views{};views.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;views.NumDescriptors=2;views.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    result=d.owner->CreateDescriptorHeap(&views,IID_PPV_ARGS(&s.depth_views));if(FAILED(result))return result;
    auto cpu=s.depth_views->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R32_UINT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    d.owner->CreateShaderResourceView(s.packed_depth.Get(),&srv,cpu);
    cpu.ptr+=d.owner->GetDescriptorHandleIncrementSize(views.Type);
    s.depth_view_stride=d.owner->GetDescriptorHandleIncrementSize(views.Type);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R32_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    d.owner->CreateUnorderedAccessView(s.texture.Get(),nullptr,&uav,cpu);
    s.depth_decoder=d.depth_decoder;return S_OK;
}
HRESULT surface(Device& d,Surface& target,const NVSDK_NGX_ImageViewInfo_VK& source,Role role) {
    Surface s;
    struct Cleanup{Device& device;Surface& surface;bool kept{};~Cleanup(){if(!kept)destroy_surface(device,surface);}} cleanup{d,s};
    const auto f=format(source.Format,role);const auto bpp=pixel_bytes(f);
    const bool d24=role==depth&&source.Format==VK_FORMAT_D24_UNORM_S8_UINT;
    const auto failed=[&](unsigned stage,HRESULT result,VkResult native_result=VK_SUCCESS) {
        if(ingress_reason(7,role)) {
            auto& flight=bridge_flight_logger();
            flight.event(BridgeFlightOperation::luke_afw_stage,result,1130,
                (std::uint64_t(role)<<32)|stage,(std::uint64_t(std::uint32_t(source.Format))<<32)|std::uint32_t(f));
            flight.event(BridgeFlightOperation::luke_afw_stage,native_result,1131,
                (std::uint64_t(source.Width)<<32)|source.Height,bpp);
        }
        return result;
    };
    if(!bpp)return failed(1,DXGI_ERROR_UNSUPPORTED);
    auto create_buffer=api<PFN_vkCreateBuffer>(d,"vkCreateBuffer");auto requirements=api<PFN_vkGetBufferMemoryRequirements>(d,"vkGetBufferMemoryRequirements");
    auto import_properties=api<PFN_vkGetMemoryWin32HandlePropertiesKHR>(d,"vkGetMemoryWin32HandlePropertiesKHR");
    auto allocate=api<PFN_vkAllocateMemory>(d,"vkAllocateMemory");auto bind=api<PFN_vkBindBufferMemory>(d,"vkBindBufferMemory");
    if(!create_buffer||!requirements||!import_properties||!allocate||!bind)return failed(2,E_NOINTERFACE);
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=source.Width;
    texture.Height=source.Height;texture.DepthOrArraySize=texture.MipLevels=1;texture.Format=f;texture.SampleDesc.Count=1;
    if(role==color||role==output||d24)texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    HRESULT result=d.owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.texture));
    if(FAILED(result))return failed(3,result);
    if(d24) {
        result=initialize_d24_decoder(d,s,texture);if(FAILED(result))return failed(3,result);
        texture=s.packed_depth->GetDesc();
    }
    UINT64 bytes{};d.owner->GetCopyableFootprints(&texture,0,1,0,&s.footprint,nullptr,nullptr,&bytes);
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};buffer.pNext=&external;buffer.size=bytes;
    buffer.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto created=create_buffer(d.binding.device,&buffer,nullptr,&s.imported);
    if(created!=VK_SUCCESS)return failed(4,DXGI_ERROR_UNSUPPORTED,created);
    VkMemoryRequirements req{};requirements(d.binding.device,s.imported,&req);
    D3D12_RESOURCE_DESC staging{};staging.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    // Vulkan specifies that memory-requirement size is meaningless for a
    // D3D12_RESOURCE import. The committed resource owns its allocation size.
    staging.Width=(bytes+65535)&~UINT64(65535);
    staging.Height=staging.DepthOrArraySize=staging.MipLevels=1;staging.SampleDesc.Count=1;staging.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    result=d.owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&staging,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&s.buffer));
    HANDLE handle{};if(SUCCEEDED(result))result=d.owner->CreateSharedHandle(s.buffer.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
    if(FAILED(result))return failed(5,result);
    VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    const auto imported_properties=import_properties(d.binding.device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,handle,&properties);
    if(imported_properties!=VK_SUCCESS){CloseHandle(handle);return failed(6,E_INVALIDARG,imported_properties);}
    const auto types=req.memoryTypeBits&properties.memoryTypeBits;unsigned index{};
    for(;index<d.memory.memoryTypeCount;++index)if(types&(1u<<index))break;
    if(index==d.memory.memoryTypeCount){CloseHandle(handle);return failed(7,E_INVALIDARG);}
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.buffer=s.imported;
    VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imported.pNext=&dedicated;imported.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;imported.handle=handle;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.pNext=&imported;
    allocation.allocationSize=staging.Width;allocation.memoryTypeIndex=index;
    const auto allocated=allocate(d.binding.device,&allocation,nullptr,&s.memory);CloseHandle(handle);
    if(allocated!=VK_SUCCESS)return failed(8,DXGI_ERROR_UNSUPPORTED,allocated);
    const auto bound=bind(d.binding.device,s.imported,s.memory,0);
    if(bound!=VK_SUCCESS)return failed(9,DXGI_ERROR_UNSUPPORTED,bound);
    s.source=source;destroy_surface(d,target);target=std::move(s);cleanup.kept=true;return S_OK;
}
HRESULT initialize_slot(Device& d,Slot& slot) {
    struct Cleanup{Device& device;Slot& slot;bool kept{};~Cleanup(){
        if(!kept&&slot.timeline){if(auto fn=api<PFN_vkDestroySemaphore>(device,"vkDestroySemaphore"))fn(device.binding.device,slot.timeline,nullptr);slot.timeline={};}
    }} cleanup{d,slot};
    HRESULT result=d.owner->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&slot.fence));
    if(SUCCEEDED(result))result=d.owner->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&slot.d12_done));
    HANDLE handle{};if(SUCCEEDED(result))result=d.owner->CreateSharedHandle(slot.fence.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
    if(FAILED(result))return result;
    auto create=api<PFN_vkCreateSemaphore>(d,"vkCreateSemaphore");auto import=api<PFN_vkImportSemaphoreWin32HandleKHR>(d,"vkImportSemaphoreWin32HandleKHR");
    VkSemaphoreTypeCreateInfo timeline{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};timeline.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&timeline;
    if(!create||!import||create(d.binding.device,&info,nullptr,&slot.timeline)!=VK_SUCCESS){CloseHandle(handle);return E_NOINTERFACE;}
    VkImportSemaphoreWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    imported.semaphore=slot.timeline;imported.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;imported.handle=handle;
    const auto imported_result=import(d.binding.device,&imported);CloseHandle(handle);
    if(imported_result!=VK_SUCCESS)return DXGI_ERROR_UNSUPPORTED;
    for(unsigned n=0;n<2;++n) {
        result=d.owner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&slot.allocators[n]));
        if(SUCCEEDED(result))result=d.owner->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,slot.allocators[n].Get(),nullptr,IID_PPV_ARGS(&slot.lists[n]));
        if(SUCCEEDED(result))result=slot.lists[n]->Close();if(FAILED(result))return result;
    }cleanup.kept=true;return S_OK;
}
std::shared_ptr<Slot> slot(Device& d) {
    // Native copyback, not only the D3D12 work, must retire before reuse.
    // Completed slots with matching resources may be reset; in-flight slots
    // remain process-pinned even when an abandoned app buffer never submits.
    for(const auto& s:d.slots)if(s->complete())return s;
    if(d.slots.size()>=14)return {};
    auto s=std::make_shared<Slot>();if(FAILED(initialize_slot(d,*s)))return {};
    d.slots.push_back(s);return s;
}
VkImageAspectFlags transport_aspects(VkFormat format,VkImageAspectFlags declared,Role role,bool copy_plane) {
    if(role!=depth)return declared;
    // NGX can describe a genuine depth image with a default COLOR range.
    // Transfers address the image's format-defined plane, not that default.
    switch(format) {
    case VK_FORMAT_D16_UNORM:case VK_FORMAT_D32_SFLOAT:return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return copy_plane?VK_IMAGE_ASPECT_DEPTH_BIT:
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    default:return declared;
    }
}
bool copy_native(Device& d,Surface& s,VkCommandBuffer cmd,bool to_native,Role role,uint32_t native_family,
    const LukeColorOriginRect* rectangle=nullptr) {
    if(!s.imported)return false;
    if(rectangle&&(!to_native||role!=output||!rectangle->width||!rectangle->height||
        std::uint64_t(rectangle->x)+rectangle->width>s.source.Width||
        std::uint64_t(rectangle->y)+rectangle->height>s.source.Height))return false;
    auto barrier=api<PFN_vkCmdPipelineBarrier>(d,"vkCmdPipelineBarrier");
    auto from_image=api<PFN_vkCmdCopyImageToBuffer>(d,"vkCmdCopyImageToBuffer");
    auto to_image=api<PFN_vkCmdCopyBufferToImage>(d,"vkCmdCopyBufferToImage");if(!barrier||!from_image||!to_image)return false;
    VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};buffer.buffer=s.imported;buffer.size=VK_WHOLE_SIZE;
    buffer.srcQueueFamilyIndex=s.native_used?VK_QUEUE_FAMILY_EXTERNAL:VK_QUEUE_FAMILY_IGNORED;
    buffer.dstQueueFamilyIndex=s.native_used?native_family:VK_QUEUE_FAMILY_IGNORED;
    buffer.dstAccessMask=to_native?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&buffer,0,nullptr);
    const auto layout=role==output?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.image=s.source.Image;
    image.subresourceRange=s.source.SubresourceRange;
    image.subresourceRange.aspectMask=transport_aspects(s.source.Format,image.subresourceRange.aspectMask,role,false);
    image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    image.oldLayout=layout;image.newLayout=to_native?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    image.srcAccessMask=role==output?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
    image.dstAccessMask=to_native?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_TRANSFER_READ_BIT;
    barrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
    VkBufferImageCopy region{};region.bufferOffset=s.footprint.Offset;
    region.bufferRowLength=s.footprint.Footprint.RowPitch/pixel_bytes(s.footprint.Footprint.Format);
    region.imageSubresource={s.source.SubresourceRange.aspectMask,s.source.SubresourceRange.baseMipLevel,s.source.SubresourceRange.baseArrayLayer,1};
    // Packed depth/stencil transports only the actual depth plane.
    region.imageSubresource.aspectMask=transport_aspects(s.source.Format,region.imageSubresource.aspectMask,role,true);
    region.imageExtent={s.source.Width,s.source.Height,1};
    if(rectangle) {
        region.bufferOffset+=std::uint64_t(rectangle->y)*s.footprint.Footprint.RowPitch+
            std::uint64_t(rectangle->x)*pixel_bytes(s.footprint.Footprint.Format);
        region.imageOffset={int32_t(rectangle->x),int32_t(rectangle->y),0};
        region.imageExtent={rectangle->width,rectangle->height,1};
    }
#if defined(XRFG_NATIVE_INGRESS_TESTS)
    if(to_native&&role==output)tested_native_output_copyback_pixels+=std::uint64_t(region.imageExtent.width)*region.imageExtent.height;
#endif
    if(to_native)to_image(cmd,s.imported,s.source.Image,image.newLayout,1,&region);
    else from_image(cmd,s.source.Image,image.newLayout,s.imported,1,&region);
    image.srcAccessMask=image.dstAccessMask;image.dstAccessMask=role==output?VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
    std::swap(image.oldLayout,image.newLayout);
    buffer.srcQueueFamilyIndex=native_family;buffer.dstQueueFamilyIndex=VK_QUEUE_FAMILY_EXTERNAL;
    buffer.srcAccessMask=to_native?VK_ACCESS_TRANSFER_READ_BIT:VK_ACCESS_TRANSFER_WRITE_BIT;buffer.dstAccessMask=0;
    barrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,1,&buffer,1,&image);
    s.native_used=true;return true;
}
void copy_d12(ID3D12GraphicsCommandList* list,Surface& s,bool to_buffer,const LukeColorOriginRect* rectangle=nullptr) {
    // D24 arrives as packed unsigned words, never as IEEE floats. Decode only
    // the low 24 bits into the standard depth guide on this existing list.
    if(s.packed_depth&&!to_buffer) {
        D3D12_TEXTURE_COPY_LOCATION buffer{};buffer.pResource=s.buffer.Get();buffer.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;buffer.PlacedFootprint=s.footprint;
        D3D12_TEXTURE_COPY_LOCATION packed{};packed.pResource=s.packed_depth.Get();packed.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d12_barrier(list,s.buffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        d12_barrier(list,s.packed_depth.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyTextureRegion(&packed,0,0,0,&buffer,nullptr);
        d12_barrier(list,s.packed_depth.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d12_barrier(list,s.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap* heaps[]{s.depth_views.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(s.depth_decoder->root.Get());list->SetPipelineState(s.depth_decoder->pipeline.Get());
        auto gpu=s.depth_views->GetGPUDescriptorHandleForHeapStart();list->SetComputeRootDescriptorTable(0,gpu);
        gpu.ptr+=s.depth_view_stride;list->SetComputeRootDescriptorTable(1,gpu);
        const UINT extent[]{s.source.Width,s.source.Height};list->SetComputeRoot32BitConstants(2,2,extent,0);
        list->Dispatch((extent[0]+7)/8,(extent[1]+7)/8,1);
        d12_barrier(list,s.texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        d12_barrier(list,s.packed_depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        d12_barrier(list,s.buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);return;
    }
    D3D12_TEXTURE_COPY_LOCATION buffer{};buffer.pResource=s.buffer.Get();buffer.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;buffer.PlacedFootprint=s.footprint;
    D3D12_TEXTURE_COPY_LOCATION texture{};texture.pResource=s.texture.Get();texture.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d12_barrier(list,s.buffer.Get(),D3D12_RESOURCE_STATE_COMMON,to_buffer?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE);
    d12_barrier(list,s.texture.Get(),D3D12_RESOURCE_STATE_COMMON,to_buffer?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COPY_DEST);
    if(to_buffer&&rectangle) {
        const D3D12_BOX box{rectangle->x,rectangle->y,0,rectangle->x+rectangle->width,rectangle->y+rectangle->height,1};
        list->CopyTextureRegion(&buffer,rectangle->x,rectangle->y,0,&texture,&box);
    } else if(to_buffer)list->CopyTextureRegion(&buffer,0,0,0,&texture,nullptr);
    else list->CopyTextureRegion(&texture,0,0,0,&buffer,nullptr);
    d12_barrier(list,s.texture.Get(),to_buffer?D3D12_RESOURCE_STATE_COPY_SOURCE:D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
    d12_barrier(list,s.buffer.Get(),to_buffer?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
HRESULT sample_late_public_depth(Device& d,Slot& slot,VkCommandBuffer command,uint32_t family) {
    auto& witness=slot.diagnostic_late_depth;
    const auto& source=slot.surfaces[depth].source;
    // A diagnostic may not destroy a resource referenced by an older recording.
    // The slot's normal fences/leases retire this independent surface too.
    if(witness.texture&&(witness.source.Width!=source.Width||witness.source.Height!=source.Height||
        witness.source.Format!=source.Format))return DXGI_ERROR_UNSUPPORTED;
    if(!witness.buffer) {
        const auto result=surface(d,witness,source,depth);
        if(FAILED(result))return result;
    }
    witness.source=source;
    return copy_native(d,witness,native(command),false,depth,family)?S_OK:E_FAIL;
}
HRESULT sample_native_public_color(Device& d,Slot& slot,VkCommandBuffer command,
    const NVSDK_NGX_Parameter* parameters,uint32_t family) {
    void* borrowed{};
    if(!parameters||NVSDK_NGX_FAILED(parameters->Get(NVSDK_NGX_Parameter_Output,&borrowed)))return E_INVALIDARG;
    const auto receipt=image_receipt(static_cast<const NVSDK_NGX_Resource_VK*>(borrowed),d.binding.device);
    if(receipt.rejection||!(receipt.allocation.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return E_INVALIDARG;
    const auto& source=receipt.resolved.Resource.ImageViewInfo;auto& witness=slot.diagnostic_native_color;
    if(witness.texture&&(witness.source.Width!=source.Width||witness.source.Height!=source.Height||
        witness.source.Format!=source.Format))return DXGI_ERROR_UNSUPPORTED;
    if(!witness.buffer) {const auto result=surface(d,witness,source,output);if(FAILED(result))return result;}
    witness.source=source;
    return copy_native(d,witness,native(command),false,output,family)?S_OK:E_FAIL;
}
void record_native_witness(ID3D12Device* device,ID3D12GraphicsCommandList* list,
    Surface& surface,const char* stage,const DlssMotionVectorFrame& frame) {
    TwoFrameImageInfo info{};info.stage=stage;info.stream=frame.stream;
    info.capture_serial=frame.publication;info.width=surface.source.Width;info.height=surface.source.Height;
    two_frame_capture_record_ng_x(info,device,list,surface.texture.Get(),D3D12_RESOURCE_STATE_COMMON);
}
struct Parameters:NVSDK_NGX_Parameter {
    const NVSDK_NGX_Parameter* base{};Slot* slot{};
    Parameters(const NVSDK_NGX_Parameter* p,Slot* s):base(p),slot(s){}
#define SCALAR(T) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char* n,T* v)const override{return base->Get(n,v);}
    SCALAR(unsigned long long) SCALAR(float) SCALAR(double) SCALAR(unsigned int) SCALAR(int)
#undef SCALAR
    void Set(const char*,ID3D11Resource*)override{}void Set(const char*,ID3D12Resource*)override{}void Set(const char*,void*)override{}
    NVSDK_NGX_Result Get(const char* n,ID3D11Resource** v)const override{return base->Get(n,v);}
    ID3D12Resource* resource(const char* n)const {
        Role role;if(!strcmp(n,NVSDK_NGX_Parameter_Color))role=color;else if(!strcmp(n,NVSDK_NGX_Parameter_Output))role=output;
        else if(!strcmp(n,NVSDK_NGX_Parameter_MotionVectors))role=motion;else if(!strcmp(n,NVSDK_NGX_Parameter_Depth))role=depth;else return nullptr;
        return slot->surfaces[role].texture.Get();
    }
    NVSDK_NGX_Result Get(const char* n,ID3D12Resource** v)const override{if(auto* r=resource(n)){*v=r;return NVSDK_NGX_Result_Success;}return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char* n,void** v)const override{if(auto* r=resource(n)){*v=r;return NVSDK_NGX_Result_Success;}return base->Get(n,v);}
    void Reset()override{}
};
// Full declared scalar/resource receipt, frozen before the marked public
// wrapper. The typed child still owns actual creation flags and production.
struct OriginalParameters:NVSDK_NGX_Parameter {
    template<class T>struct Value{const char* name{};T value{};NVSDK_NGX_Result result{NVSDK_NGX_Result_FAIL_InvalidParameter};};
    std::array<Value<unsigned>,12> uints{{{NVSDK_NGX_Parameter_Width},{NVSDK_NGX_Parameter_Height},
        {NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width},{NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height},
        {NVSDK_NGX_Parameter_OutWidth},{NVSDK_NGX_Parameter_OutHeight},
        {NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X},{NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y},
        {NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X},{NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y},
        {NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X},{NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y}}};
    std::array<Value<float>,5> floats{{{NVSDK_NGX_Parameter_MV_Scale_X},{NVSDK_NGX_Parameter_MV_Scale_Y},
        {NVSDK_NGX_Parameter_Jitter_Offset_X},{NVSDK_NGX_Parameter_Jitter_Offset_Y},{NVSDK_NGX_Parameter_FrameTimeDeltaInMsec}}};
    std::array<Value<int>,2> ints{{{NVSDK_NGX_Parameter_Reset},{NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags}}};
    std::array<ImageReceipt,role_count> images;
    OriginalParameters(const NVSDK_NGX_Parameter* p,VkDevice device) {
        if(!p)return;
        for(auto& v:uints)v.result=p->Get(v.name,&v.value);
        for(auto& v:floats)v.result=p->Get(v.name,&v.value);
        for(auto& v:ints)v.result=p->Get(v.name,&v.value);
        const char* names[]{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_Output};
        for(unsigned r=0;r<role_count;++r){void* borrowed{};const auto result=p->Get(names[r],&borrowed);
            images[r]=image_receipt(NVSDK_NGX_SUCCEED(result)?static_cast<const NVSDK_NGX_Resource_VK*>(borrowed):nullptr,device);
            images[r].parameter_result=result;images[r].borrowed_address=borrowed;}
    }
    template<class T,std::size_t N>static NVSDK_NGX_Result read(const std::array<Value<T>,N>& values,const char* name,T* out) {
        for(const auto& v:values)if(!strcmp(name,v.name)){if(out&&NVSDK_NGX_SUCCEED(v.result))*out=v.value;return v.result;}
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
#define FROZEN(T,VALUES) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char* n,T* v)const override{return read(VALUES,n,v);}
    FROZEN(float,floats) FROZEN(unsigned int,uints) FROZEN(int,ints)
#undef FROZEN
#define UNUSED(T) void Set(const char*,T)override{} NVSDK_NGX_Result Get(const char*,T*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    UNUSED(unsigned long long) UNUSED(double) UNUSED(ID3D11Resource*) UNUSED(ID3D12Resource*)
#undef UNUSED
    void Set(const char*,void*)override{}
    NVSDK_NGX_Result Get(const char* name,void** out)const override {
        const char* names[]{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_Output};
        for(unsigned r=0;r<role_count;++r)if(!strcmp(name,names[r])){const auto& image=images[r];
            if(out&&NVSDK_NGX_SUCCEED(image.parameter_result))*out=const_cast<NVSDK_NGX_Resource_VK*>(&image.resolved);
            return image.parameter_result;}
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    void Reset()override{}
};
struct OriginalPublicCapture {
    std::shared_ptr<Device> device;std::shared_ptr<Slot> slot;
    std::shared_ptr<OriginalParameters> parameters;
    DlssMotionVectorPublication camera;
    VkCommandBuffer recording{};uint32_t family{UINT32_MAX};
    std::shared_ptr<RecordingReceipt> receipt;
};
struct Copyback {Device* device{};Surface* surface{};Role role{};uint32_t family{UINT32_MAX};
    std::function<VkResult(VkCommandBuffer)> guarded;};
VkResult record_copyback(VkCommandBuffer cmd,void* context) {
    auto& c=*static_cast<Copyback*>(context);if(c.guarded)return c.guarded(cmd);
    return copy_native(*c.device,*c.surface,cmd,true,c.role,c.family)?VK_SUCCESS:VK_ERROR_INITIALIZATION_FAILED;
}
HRESULT begin_d12(Slot& slot,unsigned phase) {
    auto result=slot.allocators[phase]->Reset();if(SUCCEEDED(result))result=slot.lists[phase]->Reset(slot.allocators[phase].Get(),nullptr);return result;
}
struct Submission {HRESULT result{};bool executed{};};
Submission submit_d12(Device& d,Slot& s,unsigned phase,std::uint64_t native_ready,std::uint64_t d12_ready) {
    auto result=d.queue->Wait(s.fence.Get(),native_ready);if(FAILED(result))return {result,false};
    ID3D12CommandList* lists[]{s.lists[phase].Get()};d.queue->ExecuteCommandLists(1,lists);
    two_frame_capture_submission(d.queue.Get(),1,lists);
    const auto ready=d.queue->Signal(s.fence.Get(),d12_ready);
    const auto done=d.queue->Signal(s.d12_done.Get(),++s.d12_value);
    return {FAILED(ready)?ready:done,true};
}
struct Work {
    std::shared_ptr<Device> device;std::shared_ptr<Slot> slot;
    std::vector<neural_rendering::SubmissionReceipt> nr_receipts;
    std::uint64_t stream{},prefix{},ready{};unsigned phase{};
    bool delivered{}, exact_publication{};
};
void work_submitted(VkQueue,void* opaque) noexcept {
    try {
        auto& work=*static_cast<Work*>(opaque);auto& d=*work.device;auto& s=*work.slot;
        std::scoped_lock lock(d.mutex);if(work.delivered)return;work.delivered=true;
        if(s.pending_callbacks)--s.pending_callbacks;
        const auto submission=submit_d12(d,s,work.phase,work.prefix,work.ready);
        if(submission.executed){++s.submitted_phases;neural_rendering::complete_submission(d.queue.Get(),work.nr_receipts);
            if(d.owner_active&&work.phase==1&&work.exact_publication&&SUCCEEDED(submission.result)){
                d.production_submitted=true;ingress_reason(0,reinterpret_cast<std::uint64_t>(d.binding.device));}}
        else {neural_rendering::abandon_submission(work.nr_receipts);retire_dlss_motion_vector_stream(work.stream);}
    }catch(...) {}
}
void work_abandoned(void* opaque) noexcept {
    try {
        auto& work=*static_cast<Work*>(opaque);auto& d=*work.device;auto& s=*work.slot;
        std::scoped_lock lock(d.mutex);if(work.delivered)return;work.delivered=true;
        if(s.pending_callbacks)--s.pending_callbacks;
        // No Wait/Execute was issued for this exact recording. Abandonment is
        // explicit; neither elapsed frames nor a guessed fence cancels work.
        neural_rendering::abandon_submission(work.nr_receipts);
        retire_dlss_motion_vector_stream(work.stream);
        if(!s.pending_callbacks&&!s.submitted_phases) {
            s.final_value=0;
            for(unsigned role=0;role<role_count;++role)s.surfaces[role].native_used=s.native_before[role];
            s.diagnostic_late_depth.native_used=s.diagnostic_late_depth_before;
            s.diagnostic_native_color.native_used=s.diagnostic_native_color_before;
        }
    }catch(...) {}
}
CommandSplit handoff(const std::shared_ptr<Device>& d,const std::shared_ptr<Slot>& s,
    unsigned phase,std::uint64_t stream,VkCommandBuffer original,Copyback& copy,bool exact_publication=false) {
    ID3D12CommandList* lists[]{s->lists[phase].Get()};
    const auto closed=s->lists[phase]->Close();if(FAILED(closed)) {
        neural_rendering::abandon_submission(neural_rendering::take_submission_receipts(1,lists));
        retire_dlss_motion_vector_stream(stream);return {};
    }
    auto work=std::make_shared<Work>();work->device=d;work->slot=s;work->phase=phase;work->stream=stream;
    work->exact_publication=exact_publication;
    work->prefix=++s->value;work->ready=++s->value;const auto retired=++s->value;s->final_value=retired;
    work->nr_receipts=neural_rendering::take_submission_receipts(1,lists);
    ++s->pending_callbacks;
    CommandHandoff packet{s->timeline,work->prefix,work->ready,retired};
    packet.submission_context=work;packet.submitted=&work_submitted;packet.abandoned=&work_abandoned;
    const auto result=split(original,packet,record_copyback,&copy);
    if(result.result!=VK_SUCCESS)work_abandoned(work.get());
    return result;
}
std::shared_ptr<OriginalPublicCapture> prepare_original_public_capture(
    VkDevice device,VkCommandBuffer recording,const NVSDK_NGX_Parameter* p) {
    if(!p||!recording||split_refusal(recording))return {};
    const auto family=queue_family_for(recording);if(family==UINT32_MAX)return {};
    std::shared_ptr<Device> d;
    {std::scoped_lock lock(hooks().mutex);if(const auto found=hooks().devices.find(device);found!=hooks().devices.end())d=found->second;}
    if(!d||!d->owner||!d->queue)return {};
    auto capture=std::make_shared<OriginalPublicCapture>();capture->device=d;capture->recording=recording;capture->family=family;
    capture->parameters=std::make_shared<OriginalParameters>(p,device);
    if(!native_ingress::apply_camera(capture->camera,nullptr)) {
        const auto camera=native_ingress::streamline_camera::current(true);
        const auto mask=camera.rejection|(!native_ingress::streamline_camera::hooked()?0x1000U:0U);
        if(ingress_reason(3,(std::uint64_t(camera.frame)<<32)|mask)) {
            const auto bits=[](float value){return std::bit_cast<std::uint32_t>(value);};
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,3,955,
                (std::uint64_t(bits(camera.near_z))<<32)|bits(camera.far_z),
                (std::uint64_t(bits(camera.fov))<<32)|camera.viewport);}
        return {};
    }
    for(auto r:{motion,depth,output}) {
        const auto& receipt=capture->parameters->images[r];
        if(receipt.rejection){report_image_receipt(r,receipt,device,native(recording));return {};}
        const auto needed=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|(r==output?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0);
        if((receipt.allocation.usage&needed)!=needed)return {};
    }
    std::scoped_lock lock(d->mutex);auto s=slot(*d);if(!s)return {};
    // A slot cannot change shape after any copy references it. Preserve the
    // old pool and allocate another exact slot when public images resize.
    for(auto r:{motion,depth,output}) {
        const auto& image=capture->parameters->images[r].resolved.Resource.ImageViewInfo;
        const auto& target=s->surfaces[r];
        if(target.texture&&(target.source.Width!=image.Width||target.source.Height!=image.Height||target.source.Format!=image.Format)) {
            if(d->slots.size()>=14)return {};s=std::make_shared<Slot>();if(FAILED(initialize_slot(*d,*s)))return {};d->slots.push_back(s);break;
        }
    }
    for(auto r:{motion,depth,output}) {
        const auto& image=capture->parameters->images[r].resolved.Resource.ImageViewInfo;auto& target=s->surfaces[r];
        if(!target.buffer&&FAILED(surface(*d,target,image,r)))return {};target.source=image;
    }
    s->submitted_phases=0;for(unsigned r=0;r<role_count;++r)s->native_before[r]=s->surfaces[r].native_used;
    s->diagnostic_late_depth_before=s->diagnostic_late_depth.native_used;
    s->diagnostic_native_color_before=s->diagnostic_native_color.native_used;
    // The original recording lease pins every partially recorded input, also
    // when the wrapper later has no child/fails/reuses the opaque handle.
    const auto receipt=recording_receipt(recording);
    if(!receipt||receipt->disposition!=RecordingDisposition::recording||!retain_transport_recording(d,s,recording,receipt))return {};
    capture->receipt=receipt;
    capture->slot=s;
    if(!copy_native(*d,s->surfaces[motion],native(recording),false,motion,family)||
        !copy_native(*d,s->surfaces[depth],native(recording),false,depth,family))return {};
    return capture;
}
bool PublicFeatureInvocationScope::mark_original(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
    const std::shared_ptr<const DlssMotionVectorFrame>& frame,ID3D12Device* device,LukeColorOriginRect rectangle,
    LukeColorOriginRect* written_rectangle)noexcept {
    try {
        auto* owner=current_;if(!owner||!owner->original_capture_||!frame||!resource)return false;
        if(recording_epoch(owner->original_capture_->recording)!=owner->original_capture_->receipt->epoch)return false;
        std::scoped_lock lock(hooks().mutex);
        const auto& capture=*owner->original_capture_;
        const auto native_feature=hooks().features.find(owner->native_handle_);
        if(owner->rejected_||capture.receipt->disposition!=RecordingDisposition::recording||!owner->native_succeeded_||owner->claims_!=1||!owner->capture_consumed_||
            !owner->live_locked()||owner->lifetime_->sequence!=owner->production_sequence_||
            native_feature==hooks().features.end()||native_feature->second.identity!=owner->native_identity_||
            frame->native_production_sequence!=owner->production_sequence_||
            frame->stream!=(kVulkanPublicStreamNamespace|owner->lifetime_->identity)||
            capture.device->owner.Get()!=device||capture.slot->surfaces[output].texture.Get()!=resource)return false;
        const auto& receipt=capture.parameters->images[output];
        const auto found=hooks().image_usage.find(receipt.resolved.Resource.ImageViewInfo.Image);
        if(found==hooks().image_usage.end()||found->second.creation!=receipt.allocation.creation||found->second.device!=owner->device_)return false;
        return SUCCEEDED(mark_luke_color_origin(list,resource,frame->color_origin_token,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,device,rectangle,DXGI_FORMAT_UNKNOWN,written_rectangle));
    }catch(...){return false;}
}
VkResult PublicFeatureInvocationScope::copy_original(VkCommandBuffer command,const std::shared_ptr<Device>& d,
    const std::shared_ptr<Slot>& s,const std::shared_ptr<const DlssMotionVectorFrame>& frame,uint32_t family,
    const LukeColorOriginRect* rectangle)noexcept {
    try {
        auto* owner=current_;if(!owner||!owner->original_capture_||!frame)return VK_ERROR_INITIALIZATION_FAILED;
        // This short guard covers the actual final VkImage copyback recording,
        // not only the staging-texture marker. No Wait/Submit/split is called
        // under it; original declared vkCmd entries are forwarded unchanged.
        std::scoped_lock lock(hooks().mutex);const auto& capture=*owner->original_capture_;
        const auto native_feature=hooks().features.find(owner->native_handle_);
        const auto& receipt=capture.parameters->images[output];
        const auto allocation=hooks().image_usage.find(receipt.resolved.Resource.ImageViewInfo.Image);
        if(owner->rejected_||capture.receipt->disposition!=RecordingDisposition::recording||!owner->native_succeeded_||owner->claims_!=1||!owner->capture_consumed_||
            !owner->live_locked()||owner->lifetime_->sequence!=owner->production_sequence_||
            native_feature==hooks().features.end()||native_feature->second.identity!=owner->native_identity_||
            frame->native_production_sequence!=owner->production_sequence_||
            frame->stream!=(kVulkanPublicStreamNamespace|owner->lifetime_->identity)||
            capture.device!=d||capture.slot!=s||capture.family!=family||
            allocation==hooks().image_usage.end()||allocation->second.creation!=receipt.allocation.creation||
            allocation->second.device!=owner->device_)return VK_ERROR_INITIALIZATION_FAILED;
        return copy_native(*d,s->surfaces[output],command,true,output,family,rectangle)?VK_SUCCESS:VK_ERROR_INITIALIZATION_FAILED;
    }catch(...){return VK_ERROR_INITIALIZATION_FAILED;}
}
template<unsigned Index>NVSDK_NGX_Result evaluate(VkCommandBuffer original,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p,void* callback) {
    VkCommandBuffer recording=original;
    PublicFeatureInvocationScope public_invocation(Index,handle,original);
    public_invocation.prepare(p);
    auto raw=[&]{
        const auto result=hooks().modules[Index].evaluate(native(recording),handle,p,callback);
        if constexpr(Index<2)if(hooks().modules[Index].typed_implementation)
            PublicFeatureInvocationScope::native_completed(result);
        return public_invocation.complete(result);
    };
    const auto command_identity=native(original);
    auto observed_device=device_for(original);
    if(const auto scoped=PublicFeatureInvocationScope::exact_recording(command_identity,observed_device)) {
        recording=scoped;observed_device=device_for(scoped);
    }
    std::uint64_t public_stream{},production_sequence{};bool public_production{};
    if(!p||!handle) {
        if constexpr(Index<2)if(hooks().modules[Index].typed_implementation)
            PublicFeatureInvocationScope::claim(Index==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction,
                recording,observed_device,public_stream,production_sequence);
        return raw();
    }
    Feature f{};std::shared_ptr<Device> d;bool native_available{},replaced{};
    {std::scoped_lock lock(hooks().mutex);
     if constexpr(Index<2)if(hooks().modules[Index].typed_implementation) {
        const auto type=Index==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
        const auto existing=hooks().features.find(handle);
        const bool direct_creation=existing!=hooks().features.end()&&existing->second.creation_observed&&
            existing->second.creation_source_identity&&existing->second.creation_source_identity==existing->second.identity;
        const auto scoped=EvaluationDeviceScope::exact(command_identity,type);
        const auto scoped_device=scoped?scoped->device():VkDevice{};
        if(scoped&&(!observed_device||observed_device==scoped_device))recording=scoped->recording();
        Feature recovered{};recovered.device=observed_device?observed_device:scoped_device;
        const bool owner_conflict=observed_device&&scoped_device&&observed_device!=scoped_device;
        const bool inherited=!direct_creation&&EvaluationDeviceScope::inherit(command_identity,type,recovered);
        bool recover=inherited;
        if(inherited&&!recovered.flags_known) {
            NgxFeatureMetadata flags;
            if(recover_typed_ngx_metadata(true,type,p,flags)&&flags.flags_known) {
                recovered.flags=flags.flags;recovered.flags_known=true;
            }
        }
        if(!inherited&&(existing==hooks().features.end()||!existing->second.flags_known)) {
            if(existing!=hooks().features.end()&&existing->second.creation_observed) {
                recovered=existing->second;NgxFeatureMetadata flags;
                recover=recover_typed_ngx_metadata(true,type,p,flags)&&flags.flags_known;
                if(recover){recovered.flags=flags.flags;recovered.flags_known=true;}
            } else recover=recover_typed_ngx_metadata(true,type,p,recovered);
        }
        if(recover&&recovered.device&&!owner_conflict) {
            replaced=existing!=hooks().features.end()&&inherited&&
                existing->second.creation_source_identity!=recovered.creation_source_identity;
            recovered.identity=existing==hooks().features.end()||replaced?
                native_ingress::allocate_handle_key():existing->second.identity;
            hooks().features[handle]=recovered;
        }
     }
     const auto it=hooks().features.find(handle);if(it!=hooks().features.end()) {
        f=it->second;const auto device=hooks().devices.find(f.device);if(device!=hooks().devices.end())d=device->second;
        if(f.type==NVSDK_NGX_Feature_SuperSampling||f.type==NVSDK_NGX_Feature_RayReconstruction)
            native_available=hooks().modules[f.type==NVSDK_NGX_Feature_SuperSampling?0:1].typed_implementation;
     }}
    if constexpr(Index<2)if(hooks().modules[Index].typed_implementation) {
        const auto type=Index==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
        if(f.identity&&f.type!=type)public_production=PublicFeatureInvocationScope::refuse_child();
        else public_production=PublicFeatureInvocationScope::claim(type,
            recording,f.device?f.device:observed_device,public_stream,production_sequence);
    }
    if(public_production&&!production_sequence)return raw();
    if(replaced)invalidate_dlss_motion_vector_stream_history(public_production?public_stream:reinterpret_cast<std::uintptr_t>(handle));
    if(!f.identity||(f.type!=NVSDK_NGX_Feature_SuperSampling&&f.type!=NVSDK_NGX_Feature_RayReconstruction)||
        (observed_device&&f.device!=observed_device)) {
        if(f.creation_observed){EvaluationDeviceScope block(f,{},false);return raw();}
        return raw();
    }
    if(const auto scoped=EvaluationDeviceScope::exact(command_identity,f.type)) {
        if(scoped->device()==f.device)recording=scoped->recording();
    }
    EvaluationDeviceScope owner(f,recording,!hooks().modules[Index].typed_implementation);
    NgxEvaluationBoundary boundary(Index,hooks().modules[Index].typed_implementation,native_available);
    if(!boundary.observes())return raw();
    if(PublicFeatureInvocationScope::active()&&!hooks().modules[Index].typed_implementation)return raw();
    native_ingress::observe_upscaler_inputs(get<void*>(p,NVSDK_NGX_Parameter_MotionVectors) != nullptr,
        get<void*>(p,NVSDK_NGX_Parameter_Depth) != nullptr);
    if(const auto refusal=split_refusal(recording)){ingress_reason(1,refusal);return raw();}
    const auto native_family=queue_family_for(recording);if(native_family==UINT32_MAX)return raw();
    if(native_ingress::luke_detected()&&!f.flags_known)return raw();
    std::shared_ptr<OriginalPublicCapture> original_capture;
    const bool original_scope=PublicFeatureInvocationScope::consume_original(original_capture);
    if(original_scope&&(!original_capture||original_capture->device!=d||original_capture->family!=native_family||
        native(original_capture->recording)!=native(recording)||!original_capture->receipt||
        original_capture->receipt->disposition!=RecordingDisposition::recording||
        recording_epoch(original_capture->recording)!=original_capture->receipt->epoch)) {
        if(original_capture)PublicFeatureInvocationScope::refuse_child();return raw();}
    const NVSDK_NGX_Parameter* guides=original_capture?original_capture->parameters.get():p;
    if(original_capture){int original_flags{};
        if(NVSDK_NGX_SUCCEED(guides->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&original_flags))&&original_flags!=f.flags) {
            PublicFeatureInvocationScope::refuse_child();return raw();}}
    if(!d||!d->owner||!d->queue){ingress_reason(2);return raw();}std::unique_lock lock(d->mutex);
    std::array<const char*,role_count> names{NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_Output};
    std::array<ImageReceipt,role_count> receipts{};
    std::array<const NVSDK_NGX_Resource_VK*,role_count> resources{};
    for(unsigned r=0;r<role_count;++r) {
        void* borrowed{};const auto parameter_result=(r==color?p:guides)->Get(names[r],&borrowed);
        receipts[r]=image_receipt(NVSDK_NGX_SUCCEED(parameter_result)?static_cast<const NVSDK_NGX_Resource_VK*>(borrowed):nullptr,f.device);
        receipts[r].borrowed_address=borrowed;receipts[r].parameter_result=parameter_result;
        if(receipts[r].rejection){report_image_receipt(r,receipts[r],f.device,native(recording));return raw();}
        resources[r]=&receipts[r].resolved;
    }
    const auto& output_info=resources[output]->Resource.ImageViewInfo;
    const auto& motion_info=resources[motion]->Resource.ImageViewInfo;
    const auto& depth_info=resources[depth]->Resource.ImageViewInfo;
    DlssMotionVectorPublication g=original_capture?original_capture->camera:DlssMotionVectorPublication{};
    g.stream=public_production?public_stream:reinterpret_cast<std::uintptr_t>(handle);
    g.native_production_sequence=production_sequence;g.producer_queue=d->queue.Get();g.verified_producer_device=d->owner.Get();
    const auto render_width=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
        f.creation_observed&&f.width?f.width:depth_info.Width);
    const auto render_height=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
        f.creation_observed&&f.height?f.height:depth_info.Height);
    g.output_width=ngx_display_dimension(f.out_width,guides,NVSDK_NGX_Parameter_OutWidth,output_info.Width,f.creation_observed);
    g.output_height=ngx_display_dimension(f.out_height,guides,NVSDK_NGX_Parameter_OutHeight,output_info.Height,f.creation_observed);
    g.depth_width=render_width;g.depth_height=render_height;
    const bool low=(f.flags&NVSDK_NGX_DLSS_Feature_Flags_MVLowRes)!=0;
    g.motion_width=low?render_width:g.output_width;g.motion_height=low?render_height:g.output_height;
    g.scale_x=get<float>(guides,NVSDK_NGX_Parameter_MV_Scale_X,1);g.scale_y=get<float>(guides,NVSDK_NGX_Parameter_MV_Scale_Y,1);
    g.jitter_x=get<float>(guides,NVSDK_NGX_Parameter_Jitter_Offset_X);g.jitter_y=get<float>(guides,NVSDK_NGX_Parameter_Jitter_Offset_Y);
    g.reset=get<int>(guides,NVSDK_NGX_Parameter_Reset)!=0;g.jittered=(f.flags&NVSDK_NGX_DLSS_Feature_Flags_MVJittered)!=0;
    g.depth_inverted=(f.flags&NVSDK_NGX_DLSS_Feature_Flags_DepthInverted)!=0;g.frame_time_delta_ms=get<float>(guides,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec,16.6667F);
    if(!original_capture&&!native_ingress::apply_camera(g,nullptr)&&native_ingress::luke_detected()) {
            const auto camera=native_ingress::streamline_camera::current(true);
        const auto mask=camera.rejection|(!native_ingress::streamline_camera::hooked()?0x1000U:0U);
        if(ingress_reason(3,(std::uint64_t(camera.frame)<<32)|mask)) {
            const auto bits=[](float value){return std::bit_cast<std::uint32_t>(value);};
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,3,955,
                (std::uint64_t(bits(camera.near_z))<<32)|bits(camera.far_z),
                (std::uint64_t(bits(camera.fov))<<32)|camera.viewport);
        }
        return raw();
    }
    for(unsigned r=0;r<role_count;++r) {
        const auto required=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|((r==color||r==output)?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0);
        bool supported{};{std::scoped_lock registry_lock(hooks().mutex);const auto usage=hooks().image_usage.find(resources[r]->Resource.ImageViewInfo.Image);
            supported=usage!=hooks().image_usage.end()&&usage->second.device==f.device&&
                !(usage->second.flags&VK_IMAGE_CREATE_PROTECTED_BIT)&&(usage->second.usage&required)==required;
        }
        if(!supported){ingress_reason(5,r);return raw();}
    }
    auto s=original_capture?original_capture->slot:slot(*d);if(!s){ingress_reason(6);return raw();}
    if(!original_capture){s->submitted_phases=0;
        for(unsigned role=0;role<role_count;++role)s->native_before[role]=s->surfaces[role].native_used;
        s->diagnostic_late_depth_before=s->diagnostic_late_depth.native_used;
        s->diagnostic_native_color_before=s->diagnostic_native_color.native_used;}
    for(unsigned r=0;r<role_count;++r) {
        auto& target=s->surfaces[r];const auto& image=resources[r]->Resource.ImageViewInfo;
        if(target.texture&&(target.source.Width!=image.Width||target.source.Height!=image.Height||target.source.Format!=image.Format)) {
            if(original_capture){if(r!=color){PublicFeatureInvocationScope::refuse_child();return raw();}
                destroy_surface(*d,target);continue;}
            // Reallocation only after both APIs retired. Keep abandoned/in-flight
            // resources process-pinned; their fences are the recovery boundary.
            if(d->slots.size()>=14)return raw();
            auto replacement=std::make_shared<Slot>();if(FAILED(initialize_slot(*d,*replacement)))return raw();
            d->slots.push_back(replacement);s=replacement;break;
        }
    }
    for(unsigned r=0;r<role_count;++r){auto& target=s->surfaces[r];const auto& image=resources[r]->Resource.ImageViewInfo;
        if(!target.buffer&&FAILED(surface(*d,target,image,static_cast<Role>(r))))return raw();target.source=image;}
    g.output=s->surfaces[output].texture.Get();g.motion_vectors=s->surfaces[motion].texture.Get();g.depth=s->surfaces[depth].texture.Get();
    g.resource_state=g.depth_resource_state=g.output_resource_state=D3D12_RESOURCE_STATE_COMMON;
    g.output_x=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);g.output_y=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
    g.motion_x=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);g.motion_y=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    g.depth_x=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);g.depth_y=get<UINT>(guides,NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    const auto available=[](UINT size,UINT base){return size-std::min(size,base);};
    g.output_width=std::min(g.output_width,available(output_info.Width,g.output_x));
    g.output_height=std::min(g.output_height,available(output_info.Height,g.output_y));
    g.motion_width=std::min(g.motion_width,available(motion_info.Width,g.motion_x));
    g.motion_height=std::min(g.motion_height,available(motion_info.Height,g.motion_y));
    g.depth_width=std::min(g.depth_width,available(depth_info.Width,g.depth_x));
    g.depth_height=std::min(g.depth_height,available(depth_info.Height,g.depth_y));
    const auto nr=neural_rendering::status();const bool pre_nr=nr.enabled&&nr.before_sr;
    if(((!original_capture||pre_nr)&&FAILED(begin_d12(*s,pre_nr?0:1)))||
        (!original_capture&&!retain_transport_recording(d,s,recording)))return raw();
    if(!original_capture&&(!copy_native(*d,s->surfaces[motion],native(recording),false,motion,native_family)||
        !copy_native(*d,s->surfaces[depth],native(recording),false,depth,native_family)))return raw();
    Parameters shadow(p,s.get());std::shared_ptr<const DlssMotionVectorFrame> frame;
    if(pre_nr) {
        if(!copy_native(*d,s->surfaces[color],native(recording),false,color,native_family))return raw();
        auto* list=s->lists[0].Get();for(auto role:{motion,depth,color})copy_d12(list,s->surfaces[role],false);
        g.producer_command_list=list;frame=publish_dlss_motion_vectors(g);
        for(auto role:{color,motion,depth})d12_barrier(list,s->surfaces[role].texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        neural_rendering::before_sr(f.identity,list,&shadow,d->queue.Get());
        for(auto role:{color,motion,depth})d12_barrier(list,s->surfaces[role].texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        copy_d12(list,s->surfaces[color],true);
        Copyback copy{d.get(),&s->surfaces[color],color,native_family};
        if(original_capture)lock.unlock();
        const auto split_result=handoff(d,s,0,g.stream,recording,copy);
        if(original_capture)lock.lock();
        if(split_result.result!=VK_SUCCESS)return raw();
    }
    if(original_capture)lock.unlock();
    const auto result=raw();
    if(public_production)PublicFeatureInvocationScope::native_completed(result,boundary.can_complete());
    if(NVSDK_NGX_FAILED(result)||!boundary.can_complete())return result;
    // F8 only: capture the actual typed native Output before the wrapper can
    // overwrite/copy/composite it. Keep its transport on the existing lease.
    const bool native_color_probe=original_capture&&two_frame_capture_active();
    HRESULT native_color_result=S_FALSE;
    if(native_color_probe) {
        std::scoped_lock diagnostic_lock(d->mutex);
        native_color_result=sample_native_public_color(*d,*s,recording,p,native_family);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,native_color_result,1190,
            reinterpret_cast<std::uint64_t>(s->diagnostic_native_color.source.Image),
            reinterpret_cast<std::uint64_t>(s->surfaces[output].source.Image));
    }
    auto finish=[d,s,g,frame,pre_nr,nr_enabled=nr.enabled,recording,native_family,p,original_capture,native_color_probe,native_color_result,identity=f.identity,handle]()mutable->bool {
        std::unique_lock device_lock(d->mutex,std::defer_lock);
        if(original_capture)device_lock.lock();
        if(original_capture) {
            if(!original_capture->receipt||original_capture->receipt->disposition!=RecordingDisposition::recording||
                recording_epoch(original_capture->recording)!=original_capture->receipt->epoch)return false;
            std::scoped_lock registry_lock(hooks().mutex);
            const auto native_feature=hooks().features.find(handle);
            if(native_feature==hooks().features.end()||native_feature->second.identity!=identity)return false;
            for(auto r:{motion,depth,output}){const auto& receipt=original_capture->parameters->images[r];
                const auto found=hooks().image_usage.find(receipt.resolved.Resource.ImageViewInfo.Image);
                if(found==hooks().image_usage.end()||found->second.creation!=receipt.allocation.creation||
                    found->second.device!=d->binding.device)return false;}
        }
        // F8 reads the SAME original depth image again after public completion.
        // Keep the early production guide immutable and retain the late witness
        // on the existing slot/fences. No new split, submit, callback or wait.
        const bool depth_probe=original_capture&&two_frame_capture_active();
        HRESULT depth_probe_result=S_FALSE;
        if(depth_probe) {
            depth_probe_result=sample_late_public_depth(*d,*s,recording,native_family);
            const auto& source=s->surfaces[depth].source;
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,depth_probe_result,1180,
                (std::uint64_t(std::uint32_t(source.Format))<<32)|source.SubresourceRange.aspectMask,
                (std::uint64_t(source.Width)<<32)|source.Height);
        }
        // For a public transaction the wrapper has now finished every private
        // color pass/copyback. Only this original output is copied and marked.
        if(((pre_nr||original_capture)&&FAILED(begin_d12(*s,1)))||
            !copy_native(*d,s->surfaces[output],native(recording),false,output,native_family))return false;
        auto* list=s->lists[1].Get();copy_d12(list,s->surfaces[output],false);
        if(!pre_nr){for(auto role:{motion,depth})copy_d12(list,s->surfaces[role],false);g.producer_command_list=list;frame=publish_dlss_motion_vectors(g);}
        if(depth_probe&&frame) {
            record_native_witness(d->owner.Get(),list,s->surfaces[depth],"vk_depth_before_public",*frame);
            if(SUCCEEDED(depth_probe_result)) {
                copy_d12(list,s->diagnostic_late_depth,false);
                record_native_witness(d->owner.Get(),list,s->diagnostic_late_depth,"vk_depth_after_public",*frame);
            } else {
                TwoFrameImageInfo missing{};missing.stage="vk_depth_after_public";
                missing.stream=frame->stream;missing.capture_serial=frame->publication;
                two_frame_capture_note(missing,depth_probe_result);
            }
        }
        if(native_color_probe&&frame) {
            record_native_witness(d->owner.Get(),list,s->surfaces[output],"vk_color_after_public",*frame);
            if(SUCCEEDED(native_color_result)) {
                copy_d12(list,s->diagnostic_native_color,false);
                record_native_witness(d->owner.Get(),list,s->diagnostic_native_color,"vk_color_native_before_public",*frame);
            } else {
                TwoFrameImageInfo missing{};missing.stage="vk_color_native_before_public";
                missing.stream=frame->stream;missing.capture_serial=frame->publication;
                two_frame_capture_note(missing,native_color_result);
            }
        }
        for(auto role:{motion,depth})d12_barrier(list,s->surfaces[role].texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        d12_barrier(list,s->surfaces[output].texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Parameters final_parameters(original_capture?original_capture->parameters.get():p,s.get());
        if(nr_enabled)neural_rendering::after_sr(identity,list,&final_parameters,d->queue.Get());
        bool marker=true;
        LukeColorOriginRect written_rectangle{};
        if(frame&&g.protect_snapshot_lifetime&&embedded::afw_snapshot().desired.mode!=0) {
            if(original_capture)marker=PublicFeatureInvocationScope::mark_original(list,g.output,frame,d->owner.Get(),
                {g.output_x,g.output_y,g.output_width,g.output_height},&written_rectangle);
            else mark_luke_color_origin(list,g.output,frame->color_origin_token,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,d->owner.Get(),
                {g.output_x,g.output_y,g.output_width,g.output_height});
        }
        d12_barrier(list,s->surfaces[output].texture.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        for(auto role:{motion,depth})d12_barrier(list,s->surfaces[role].texture.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        // AFW consumes final XR color, not this native HDR surface. With NR
        // inactive the only modified pixels are the exact written marker bands.
        // Keep the full incoming image/snapshots, but return only that rectangle.
        const bool marker_only=original_capture&&marker&&!nr_enabled&&written_rectangle.width&&written_rectangle.height;
        if(frame&&(frame->native_production_sequence<=8||frame->native_production_sequence%300==0))
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,marker_only?1:0,1191,
                marker_only?std::uint64_t(written_rectangle.width)*written_rectangle.height*pixel_bytes(s->surfaces[output].footprint.Footprint.Format):0,
                std::uint64_t(s->surfaces[output].source.Width)*s->surfaces[output].source.Height*pixel_bytes(s->surfaces[output].footprint.Footprint.Format));
        copy_d12(list,s->surfaces[output],true,marker_only?&written_rectangle:nullptr);
        Copyback copy{d.get(),&s->surfaces[output],output,native_family};
        if(original_capture)copy.guarded=[d,s,frame,native_family,marker_only,written_rectangle](VkCommandBuffer command){
            return PublicFeatureInvocationScope::copy_original(command,d,s,frame,native_family,marker_only?&written_rectangle:nullptr);};
        if(device_lock.owns_lock())device_lock.unlock();
        const auto transferred=handoff(d,s,1,g.stream,recording,copy,frame!=nullptr&&marker);
        if(transferred.result!=VK_SUCCESS)ingress_reason(8,static_cast<std::uint64_t>(transferred.result));
        return marker&&transferred.result==VK_SUCCESS&&(!original_capture||frame!=nullptr);
    };
    if(original_capture)PublicFeatureInvocationScope::final_capture(std::move(finish),handle,f.identity);
    else static_cast<void>(finish());
    return result;
}
Feature creation_feature(VkDevice device,NVSDK_NGX_Feature type,NVSDK_NGX_Parameter* p) {
    Feature f{};f.device=device;f.type=type;f.creation_observed=true;
    f.flags=get<int>(p,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags);f.width=get<UINT>(p,NVSDK_NGX_Parameter_Width);f.height=get<UINT>(p,NVSDK_NGX_Parameter_Height);
    f.flags_known=p&&NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,&f.flags));
    f.out_width=get<UINT>(p,NVSDK_NGX_Parameter_OutWidth);f.out_height=get<UINT>(p,NVSDK_NGX_Parameter_OutHeight);
    return f;
}
void register_feature(Feature f,NVSDK_NGX_Handle** out,NVSDK_NGX_Result result,bool public_dispatch=false) {
    if(NVSDK_NGX_FAILED(result)||!out||!*out||!f.device)return;
    f.identity=native_ingress::allocate_handle_key();f.creation_source_identity=f.identity;bool replaced{};
    std::shared_ptr<PublicFeatureLifetime> previous;
    {std::scoped_lock lock(hooks().mutex);replaced=hooks().features.contains(*out);hooks().features[*out]=f;
        if(public_dispatch)if(const auto old=hooks().public_lifetimes.find(*out);old!=hooks().public_lifetimes.end()) {
            previous=old->second;hooks().public_lifetimes.erase(old);
        }}
    if(previous)retire_dlss_motion_vector_stream(kVulkanPublicStreamNamespace|previous->identity);
    if(replaced)invalidate_dlss_motion_vector_stream_history(reinterpret_cast<std::uintptr_t>(*out));
}
template<unsigned Index>NVSDK_NGX_Result create1(VkDevice device,VkCommandBuffer cmd,NVSDK_NGX_Feature type,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    const auto f=creation_feature(device,type,p);
    const auto result=hooks().modules[Index].create1(device,native(cmd),type,p,out);if(p)register_feature(f,out,result,Index==3);return result;
}
template<unsigned Index>NVSDK_NGX_Result create(VkCommandBuffer cmd,NVSDK_NGX_Feature type,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    const auto device=device_for(cmd);
    const auto f=creation_feature(device,type,p);
    const auto result=hooks().modules[Index].create(native(cmd),type,p,out);if(p)register_feature(f,out,result,Index==3);return result;
}
template<unsigned Index>NVSDK_NGX_Result release(NVSDK_NGX_Handle* handle) {
    Feature f{};std::shared_ptr<PublicFeatureLifetime> public_lifetime;
    {std::scoped_lock lock(hooks().mutex);auto it=hooks().features.find(handle);if(it!=hooks().features.end())f=it->second;
        if constexpr(Index==3)if(const auto found=hooks().public_lifetimes.find(handle);found!=hooks().public_lifetimes.end())public_lifetime=found->second;}
    const auto result=hooks().modules[Index].release(handle);
    if(NVSDK_NGX_SUCCEED(result)) {
        bool raw_reused{};
        {std::scoped_lock lock(hooks().mutex);
            if(const auto found=hooks().features.find(handle);found!=hooks().features.end()) {
                raw_reused=found->second.identity!=f.identity;
                if(!raw_reused)hooks().features.erase(found);
            }
            if(public_lifetime)if(const auto found=hooks().public_lifetimes.find(handle);
                found!=hooks().public_lifetimes.end()&&found->second==public_lifetime)hooks().public_lifetimes.erase(found);
            if(f.identity&&!raw_reused)retire_dlss_motion_vector_stream(reinterpret_cast<std::uintptr_t>(handle));}
        if(public_lifetime)retire_dlss_motion_vector_stream(kVulkanPublicStreamNamespace|public_lifetime->identity);
        if(f.identity)neural_rendering::retire_handle(f.identity);
    }
    return result;
}
template<unsigned Index>void install(HMODULE module) {
    auto& m=hooks().modules[Index];if(!module||m.retained)return;
    auto c=GetProcAddress(module,"NVSDK_NGX_VULKAN_CreateFeature"),c1=GetProcAddress(module,"NVSDK_NGX_VULKAN_CreateFeature1"),
        e=GetProcAddress(module,"NVSDK_NGX_VULKAN_EvaluateFeature"),r=GetProcAddress(module,"NVSDK_NGX_VULKAN_ReleaseFeature");if(!e||!r||(!c&&!c1))return;
    if((Index<2&&native_ingress::luke_detected())||Index==3) {
        const auto original_e=native_export_address(module,"NVSDK_NGX_VULKAN_EvaluateFeature");
        const auto original_r=native_export_address(module,"NVSDK_NGX_VULKAN_ReleaseFeature");
        const auto original_c=native_export_address(module,"NVSDK_NGX_VULKAN_CreateFeature");
        const auto original_c1=native_export_address(module,"NVSDK_NGX_VULKAN_CreateFeature1");
        if(original_e&&original_r&&(original_c||original_c1)){e=original_e;r=original_r;c=original_c;c1=original_c1;}
        else if constexpr(Index==3)return;
    }
    HMODULE implementation{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(e),&implementation);
    m.typed_implementation=Index<2&&implementation==module;
    if(c)m.c=safetyhook::create_inline(c,reinterpret_cast<void*>(&create<Index>),SafetyHookInline::StartDisabled);
    if(c1)m.c1=safetyhook::create_inline(c1,reinterpret_cast<void*>(&create1<Index>),SafetyHookInline::StartDisabled);
    m.e=safetyhook::create_inline(e,reinterpret_cast<void*>(&evaluate<Index>),SafetyHookInline::StartDisabled);
    m.r=safetyhook::create_inline(r,reinterpret_cast<void*>(&release<Index>),SafetyHookInline::StartDisabled);
    if((c&&!m.c)||(c1&&!m.c1)||!m.e||!m.r){m.c={};m.c1={};m.e={};m.r={};return;}
    if(m.c)m.create=m.c.original<Create>();if(m.c1)m.create1=m.c1.original<Create1>();
    m.evaluate=m.e.original<Evaluate>();m.release=m.r.original<Release>();
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(e),&m.retained);
    if(m.c)(void)m.c.enable();if(m.c1)(void)m.c1.enable();(void)m.e.enable();(void)m.r.enable();
}
void install_luke_feature_dispatch()noexcept {
    try {
        if(hooks().modules[3].retained)return;
        const auto valid=[](HMODULE module) {
            if(!module)return false;
            const auto marker=GetProcAddress(module,"RealVRVersion");HMODULE owner{};
            return marker&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(marker),&owner)&&owner==module&&
                native_export_address(module,"NVSDK_NGX_VULKAN_EvaluateFeature")&&
                native_export_address(module,"NVSDK_NGX_VULKAN_ReleaseFeature")&&
                (native_export_address(module,"NVSDK_NGX_VULKAN_CreateFeature")||
                 native_export_address(module,"NVSDK_NGX_VULKAN_CreateFeature1"));
        };
        HMODULE module{};
        for(const auto* name:{L"dxgi.dll",L"RealVR64.dll"})
            if(const auto candidate=GetModuleHandleW(name);valid(candidate)){module=candidate;break;}
        if(!module) {
            using Enum=BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
            const auto enumerate=reinterpret_cast<Enum>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32EnumProcessModules"));
            HMODULE modules[1024]{};DWORD bytes{};
            if(enumerate&&enumerate(GetCurrentProcess(),modules,sizeof(modules),&bytes))
                for(unsigned index=0;index<std::min<DWORD>(bytes/sizeof(HMODULE),1024);++index)
                    if(valid(modules[index])){module=modules[index];break;}
        }
        if(!module)return;install<3>(module);
        if(hooks().modules[3].retained)bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,1,1031,
            reinterpret_cast<std::uint64_t>(module),reinterpret_cast<std::uint64_t>(hooks().modules[3].e.target()));
    }catch(...){}
}
struct ImageCallScope {
    inline static thread_local ImageCallScope* current{};
    struct Receipt {VkImage image{};Hooks::ImageAllocation allocation{};bool ambiguous{};};
    ImageCallScope* parent{current};VkDevice device{};VkImage* output{};
    const VkImageCreateInfo* forwarded{};bool retry{};std::vector<Receipt> observed;
    ImageCallScope(VkDevice owner,VkImage* result,const VkImageCreateInfo* info,bool fallback)
        :device(owner),output(result),forwarded(info),retry(fallback){current=this;}
    ~ImageCallScope(){current=parent;}
};
VkResult observe_create_image(PFN_vkCreateImage original,VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* image,const void* creator=nullptr) {
    if(!info||!image)return original(device,info,allocator,image);
    // Transfer capability is requested at native allocation creation, never
    // inferred afterward from resolution or successful NGX evaluation.
    const VkImageCreateInfo requested=*info;VkImageCreateInfo local=requested;
    const bool forwarded=ImageCallScope::current&&ImageCallScope::current->device==device&&ImageCallScope::current->output==image&&
        (ImageCallScope::current->forwarded==info||ImageCallScope::current->retry);
    if(!forwarded&&!(local.flags&VK_IMAGE_CREATE_PROTECTED_BIT)&&local.imageType==VK_IMAGE_TYPE_2D&&local.samples==VK_SAMPLE_COUNT_1_BIT&&local.mipLevels==1&&local.arrayLayers==1)
        local.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ImageCallScope::Receipt received{};
    const auto invoke=[&](const VkImageCreateInfo* used,bool retry){
        ImageCallScope scope(device,image,used,retry);received={};
        const auto result=original(device,used,allocator,image);
        if(result==VK_SUCCESS)for(const auto& receipt:scope.observed)if(receipt.image==*image) {
            if(received.image&&(received.ambiguous||receipt.ambiguous||received.allocation.device!=receipt.allocation.device))
                received.ambiguous=true;
            else received=receipt;
        }
        if(result==VK_SUCCESS&&!scope.observed.empty()&&!received.image)
            received={*image,{},true}; // A wrapper replaced the native result without a creation receipt.
        return result;
    };
    auto result=invoke(forwarded?info:&local,forwarded&&ImageCallScope::current->retry);
    if(result!=VK_SUCCESS&&!forwarded&&local.usage!=requested.usage){local=requested;result=invoke(info,true);}
    if(result==VK_SUCCESS){
        // Only a receipt from this exact synchronous original-call chain can
        // supersede the outer descriptor. A lower foreign device remains
        // foreign and will fail the unchanged owner guard at evaluation.
        {std::scoped_lock lock(hooks().mutex);
            if(!received.ambiguous&&!received.allocation.device)received={*image,{device,local.usage,local.flags,local.imageType,local.extent,
                local.mipLevels,local.arrayLayers,local.samples,local.format,++hooks().image_creations,creator}};
            if(received.ambiguous)hooks().image_usage.erase(*image);else hooks().image_usage[*image]=received.allocation;}
        if(ImageCallScope::current)ImageCallScope::current->observed.push_back(received);
    }
    return result;
}
void observe_destroy_image(PFN_vkDestroyImage original,VkDevice device,VkImage image,const VkAllocationCallbacks* allocator) {
    {std::scoped_lock lock(hooks().mutex);const auto it=hooks().image_usage.find(image);
        if(it!=hooks().image_usage.end()&&it->second.device==device)hooks().image_usage.erase(it);}
    original(device,image,allocator);
}
VkResult VKAPI_CALL create_image(VkDevice device,const VkImageCreateInfo* info,const VkAllocationCallbacks* allocator,VkImage* image) {
    return observe_create_image(hooks().create_image,device,info,allocator,image,hooks().loader_create_image);
}
void VKAPI_CALL destroy_image(VkDevice device,VkImage image,const VkAllocationCallbacks* allocator) {
    observe_destroy_image(hooks().destroy_image,device,image,allocator);
}
template<unsigned Index>VkResult VKAPI_CALL actual_create_image(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* image) {
    return observe_create_image(reinterpret_cast<PFN_vkCreateImage>(hooks().image_create_entries[Index].original),device,info,allocator,image,
        hooks().image_create_entries[Index].address);
}
template<unsigned Index>void VKAPI_CALL actual_destroy_image(VkDevice device,VkImage image,const VkAllocationCallbacks* allocator) {
    observe_destroy_image(reinterpret_cast<PFN_vkDestroyImage>(hooks().image_destroy_entries[Index].original),device,image,allocator);
}
template<std::size_t... Index>auto actual_image_creators(std::index_sequence<Index...>) {
    return std::array<PFN_vkCreateImage,sizeof...(Index)>{&actual_create_image<Index>...};
}
template<std::size_t... Index>auto actual_image_destroyers(std::index_sequence<Index...>) {
    return std::array<PFN_vkDestroyImage,sizeof...(Index)>{&actual_destroy_image<Index>...};
}
bool install_image_entry(PFN_vkVoidFunction function,bool creation) noexcept {
    try {
        if(!function)return false;
        static const auto creators=actual_image_creators(std::make_index_sequence<Hooks::image_entry_capacity>{});
        static const auto destroyers=actual_image_destroyers(std::make_index_sequence<Hooks::image_entry_capacity>{});
        auto& h=hooks();std::scoped_lock lock(h.mutex);auto* address=reinterpret_cast<void*>(function);
        if(address==h.loader_create_image||address==h.loader_destroy_image||
            function==reinterpret_cast<PFN_vkVoidFunction>(&create_image)||function==reinterpret_cast<PFN_vkVoidFunction>(&destroy_image))return true;
        for(unsigned index=0;index<Hooks::image_entry_capacity;++index) {
            if(function==reinterpret_cast<PFN_vkVoidFunction>(creators[index])||
                function==reinterpret_cast<PFN_vkVoidFunction>(destroyers[index]))return true;
        }
        for(auto* entries:{&h.image_create_entries,&h.image_destroy_entries})for(const auto& entry:*entries)
            if(entry.address==address||entry.original==function)return true;
        auto& entries=creation?h.image_create_entries:h.image_destroy_entries;
        for(unsigned index=0;index<entries.size();++index)if(!entries[index].address) {
            auto& entry=entries[index];auto* replacement=creation?reinterpret_cast<void*>(creators[index]):reinterpret_cast<void*>(destroyers[index]);
            auto hook=safetyhook::create_inline(address,replacement,SafetyHookInline::StartDisabled);if(!hook)return false;
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(address),&retained))return false;
            entry.address=address;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            if(!entry.hook.enable()){entry.hook={};entry.address=nullptr;entry.original=nullptr;return false;}
            bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,creation?1:2,973,
                reinterpret_cast<std::uint64_t>(address),reinterpret_cast<std::uint64_t>(entry.original));
            return true;
        }
    }catch(...){}return false;
}
void install_image_entries(VkDevice device,PFN_vkGetDeviceProcAddr original) noexcept {
    if(!device||!original)return;
    const bool created=install_image_entry(original(device,"vkCreateImage"),true);
    const bool destroyed=install_image_entry(original(device,"vkDestroyImage"),false);
    bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,(created?1:0)|(destroyed?2:0),974,
        reinterpret_cast<std::uint64_t>(device),0);
}
PFN_vkVoidFunction VKAPI_CALL get_luke_device(VkDevice device,const char* name) {
    const auto function=hooks().luke_gdpa(device,name);
    if(name&&(!strcmp(name,"vkCreateImage")||!strcmp(name,"vkDestroyImage")))
        static_cast<void>(install_image_entry(function,!strcmp(name,"vkCreateImage")));
    return function; // Preserve every actual public dispatcher result/other command.
}
void install_luke_image_dispatch() noexcept {
    try {
        auto& h=hooks();if(h.luke_gdpa_hook)return;
        const auto valid=[](HMODULE module) {
            if(!module||!GetProcAddress(module,"RealVRVersion"))return false;
            const auto address=GetProcAddress(module,"vkGetDeviceProcAddr");HMODULE owner{};
            return address&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address),&owner)&&owner==module;
        };
        HMODULE module{};
        for(const auto* name:{L"dxgi.dll",L"RealVR64.dll"})if(const auto candidate=GetModuleHandleW(name);valid(candidate)){module=candidate;break;}
        if(!module) {
            using Enum=BOOL(WINAPI*)(HANDLE,HMODULE*,DWORD,LPDWORD);
            const auto enumerate=reinterpret_cast<Enum>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"K32EnumProcessModules"));
            HMODULE modules[1024]{};DWORD bytes{};
            if(enumerate&&enumerate(GetCurrentProcess(),modules,sizeof(modules),&bytes))
                for(unsigned index=0;index<std::min<DWORD>(bytes/sizeof(HMODULE),1024);++index)
                    if(valid(modules[index])){module=modules[index];break;}
        }
        if(!module)return;
        const auto address=native_export_address(module,"vkGetDeviceProcAddr");if(!address)return;
        HMODULE owner{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(address),&owner)||owner!=module)return;
        auto hook=safetyhook::create_inline(address,reinterpret_cast<void*>(&get_luke_device),SafetyHookInline::StartDisabled);
        if(!hook)return;h.luke_gdpa=hook.original<PFN_vkGetDeviceProcAddr>();h.luke_gdpa_hook=std::move(hook);
        if(!h.luke_gdpa_hook.enable()){h.luke_gdpa_hook={};h.luke_gdpa=nullptr;}
    }catch(...){}
}
void report_image_coverage(VkDevice device) noexcept {
    try {
        static std::atomic_uint reports{};if(reports.fetch_add(1,std::memory_order_relaxed)>=16)return;
        auto& h=hooks();std::scoped_lock lock(h.mutex);auto& log=bridge_flight_logger();
        if(const auto downstream=h.device_image_dispatch.find(device);downstream!=h.device_image_dispatch.end())
            log.event(BridgeFlightOperation::luke_afw_stage,
                (downstream->second.creation?1:0)|(downstream->second.destruction?2:0),1030,
                reinterpret_cast<std::uint64_t>(device),reinterpret_cast<std::uint64_t>(downstream->second.resolver));
        log.event(BridgeFlightOperation::luke_afw_stage,h.luke_gdpa_hook?1:0,975,
            reinterpret_cast<std::uint64_t>(h.luke_gdpa_hook.target()),reinterpret_cast<std::uint64_t>(h.luke_gdpa));
        for(unsigned kind=0;kind<2;++kind)for(const auto& entry:kind?h.image_destroy_entries:h.image_create_entries)
            if(entry.address)log.event(BridgeFlightOperation::luke_afw_stage,kind+1,976,
                reinterpret_cast<std::uint64_t>(entry.address),reinterpret_cast<std::uint64_t>(entry.original));
        unsigned live{},samples{};
        for(const auto& [image,allocation]:h.image_usage)if(allocation.device==device) {
            ++live;
            if(samples<8&&(allocation.format==VK_FORMAT_R16G16_SFLOAT||allocation.format==VK_FORMAT_R16G16_SNORM)) {
                ++samples;
                log.event(BridgeFlightOperation::luke_afw_stage,allocation.format,978,
                    reinterpret_cast<std::uint64_t>(image),reinterpret_cast<std::uint64_t>(allocation.creator));
                log.event(BridgeFlightOperation::luke_afw_stage,allocation.usage,979,
                    (std::uint64_t(allocation.extent.width)<<32)|allocation.extent.height,
                    (std::uint64_t(allocation.mips)<<32)|allocation.layers);
                log.event(BridgeFlightOperation::luke_afw_stage,0,980,allocation.creation,reinterpret_cast<std::uint64_t>(device));
            }
        }
        log.event(BridgeFlightOperation::luke_afw_stage,live,977,reinterpret_cast<std::uint64_t>(device),h.image_creations);
    }catch(...){}
}
VkResult VKAPI_CALL create_device(VkPhysicalDevice physical,const VkDeviceCreateInfo* info,const VkAllocationCallbacks* allocator,VkDevice* device) {
    InteropDeviceRequest request;bool interop{};
    const auto module=GetModuleHandleW(L"vulkan-1.dll");
    auto enumerate=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(GetProcAddress(module,"vkEnumerateDeviceExtensionProperties"));
    auto features=reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(GetProcAddress(module,"vkGetPhysicalDeviceFeatures2"));
    if(enumerate&&features&&info) {
        uint32_t count{};if(enumerate(physical,nullptr,&count,nullptr)==VK_SUCCESS) {
            std::vector<VkExtensionProperties> supported(count);
            if(enumerate(physical,nullptr,&count,supported.data())==VK_SUCCESS) {
                VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
                VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};query.pNext=&timeline;features(physical,&query);
                interop=request.prepare(*info,supported,timeline.timelineSemaphore!=0);
            }
        }
    }
    const auto result=hooks().create_device(physical,interop?&request.info:info,allocator,device);
    if(result==VK_SUCCESS) {
        {std::scoped_lock lock(hooks().mutex);hooks().interop_devices[*device]=interop;
            const auto instance=hooks().instances.find(*reinterpret_cast<void* const*>(physical));
            if(instance!=hooks().instances.end())hooks().observed_devices[*device]={instance->second,physical,*device,0,0};}
        // Observe command pools, allocation and recording before the application
        // retrieves its device dispatcher. No existing state is guessed later.
        initialize_commands({},physical,*device,hooks().gipa,hooks().gdpa);
        install_image_entries(*device,hooks().gdpa);
        if(hooks().luke_gdpa)install_image_entries(*device,hooks().luke_gdpa);
    }
    return result;
}
void VKAPI_CALL destroy_device(VkDevice device,const VkAllocationCallbacks* allocator) {
    stop_owner(device);
    {std::scoped_lock lock(hooks().mutex);hooks().interop_devices.erase(device);
        hooks().observed_devices.erase(device);
        hooks().device_image_dispatch.erase(device);
        std::erase_if(hooks().public_lifetimes,[&](const auto& lifetime){
            if(lifetime.second->device!=device)return false;
            retire_dlss_motion_vector_stream(kVulkanPublicStreamNamespace|lifetime.second->identity);return true;
        });
        std::erase_if(hooks().image_usage,[&](const auto& image){return image.second.device==device;});}
    std::vector<std::shared_ptr<Device>> retired;{std::scoped_lock lock(hooks().mutex);retired=hooks().retired;}
    for(const auto& d:retired)if(d->binding.device==device){std::scoped_lock device_lock(d->mutex);d->native_alive=false;}
    destroy_commands(device);hooks().destroy_device(device,allocator);
}
PFN_vkVoidFunction VKAPI_CALL get_device(VkDevice device,const char* name) {
    auto fn=hooks().gdpa(device,name);
    if(!strcmp(name,"vkCreateImage")){(void)install_image_entry(fn,true);return fn;}
    if(!strcmp(name,"vkDestroyImage")){(void)install_image_entry(fn,false);return fn;}
    if(!strcmp(name,"vkDestroyDevice"))return fn?reinterpret_cast<PFN_vkVoidFunction>(&destroy_device):nullptr;
    return resolve_command(device,name,fn);
}
PFN_vkVoidFunction VKAPI_CALL get_instance(VkInstance instance,const char* name) {
    if(instance){std::scoped_lock lock(hooks().mutex);hooks().instances[*reinterpret_cast<void* const*>(instance)]=instance;}
    auto fn=hooks().gipa(instance,name);
    if(!strcmp(name,"vkGetDeviceProcAddr"))return fn?reinterpret_cast<PFN_vkVoidFunction>(&get_device):nullptr;
    if(!strcmp(name,"vkCreateDevice"))return fn?reinterpret_cast<PFN_vkVoidFunction>(&create_device):nullptr;
    if(!strcmp(name,"vkCreateImage")){(void)install_image_entry(fn,true);return fn;}
    if(!strcmp(name,"vkDestroyImage")){(void)install_image_entry(fn,false);return fn;}
    if(!strcmp(name,"vkDestroyDevice"))return fn?reinterpret_cast<PFN_vkVoidFunction>(&destroy_device):nullptr;
    return resolve_command(VK_NULL_HANDLE,name,fn);
}
}
void observe_device_dispatch(VkDevice device,PFN_vkGetDeviceProcAddr next_device_get) noexcept {
    try {
#if !defined(XRFG_NATIVE_INGRESS_TESTS)
        if(!luke_vulkan_process()||!native_ingress::luke_detected())return;
#endif
        if(!device||!next_device_get)return;
        // These addresses come from this successful loader-chain creation,
        // not a public resolver that REAL VR may redirect to its own wrapper.
        // Hooking the exact entries also covers originals cached by that
        // wrapper before it creates private motion/depth replacements.
        const bool creation=install_image_entry(next_device_get(device,"vkCreateImage"),true);
        const bool destruction=install_image_entry(next_device_get(device,"vkDestroyImage"),false);
        {std::scoped_lock lock(hooks().mutex);
            hooks().device_image_dispatch[device]={next_device_get,creation,destruction};}
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            (creation?1:0)|(destruction?2:0),1030,reinterpret_cast<std::uint64_t>(device),
            reinterpret_cast<std::uint64_t>(next_device_get));
    }catch(...){}
}
void configure_owner(const VulkanSessionBinding& binding,ID3D12Device* owner,ID3D12CommandQueue* xr_queue) noexcept {
    try {
        poll_capture();if(!binding.device||!owner||!xr_queue||!hooks().gdpa||!hooks().gipa)return;
        install_image_entries(binding.device,hooks().gdpa);
        if(hooks().luke_gdpa)install_image_entries(binding.device,hooks().luke_gdpa);
        report_image_coverage(binding.device);
        collect_retired();
        {std::scoped_lock lock(hooks().mutex);if(hooks().retired.size()>=8)return;
            if(hooks().devices.contains(binding.device))return;
            const auto capabilities=hooks().interop_devices.find(binding.device);
            if(capabilities==hooks().interop_devices.end()||!capabilities->second)return;}
        auto d=std::make_shared<Device>();d->binding=binding;d->owner=owner;d->gdpa=hooks().gdpa;d->gipa=hooks().gipa;
        ComPtr<ID3D12Device> queue_owner;ComPtr<IUnknown> a,b;owner->QueryInterface(IID_PPV_ARGS(&a));
        if(FAILED(xr_queue->GetDevice(IID_PPV_ARGS(&queue_owner))))return;queue_owner.As(&b);if(a.Get()!=b.Get())return;
        auto description=xr_queue->GetDesc();description.Priority=D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        description.Flags=D3D12_COMMAND_QUEUE_FLAG_NONE;if(FAILED(owner->CreateCommandQueue(&description,IID_PPV_ARGS(&d->queue))))return;
        const auto memory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(d->gipa(binding.instance,"vkGetPhysicalDeviceMemoryProperties"));
        if(!memory)return;memory(binding.physical_device,&d->memory);
        if(!initialize_commands(binding.instance,binding.physical_device,binding.device,d->gipa,d->gdpa))return;
        native_ingress::configure_queue(d->queue.Get());
        {std::scoped_lock lock(hooks().mutex);hooks().devices[binding.device]=std::move(d);}
    }catch(...) {}
}
void stop_owner(VkDevice device) noexcept {
    // An OpenXR session end cannot cancel the application's command recording.
    // Submission/abandonment callbacks and both exact fences own quiescence.
    std::shared_ptr<Device> retired;
    {std::scoped_lock lock(hooks().mutex);auto it=hooks().devices.find(device);if(it!=hooks().devices.end()) {
        retired=it->second;hooks().retired.push_back(retired);hooks().devices.erase(it);
    }}
    if(retired){std::scoped_lock lock(retired->mutex);retired->owner_active=false;retired->production_submitted=false;}
    collect_retired();
}
void poll_capture() noexcept {try {
#if !defined(XRFG_NATIVE_INGRESS_TESTS)
    if(!luke_vulkan_process()||!native_ingress::luke_detected())return;
#endif
    static std::mutex polling_mutex;std::scoped_lock polling_lock(polling_mutex);
    collect_retired();
    install<0>(GetModuleHandleW(L"nvngx_dlss.dll"));install<1>(GetModuleHandleW(L"nvngx_dlssd.dll"));install<2>(GetModuleHandleW(L"_nvngx.dll"));
    install_luke_feature_dispatch();
    auto& h=hooks();if(h.gdpa_hook){install_luke_image_dispatch();ingress_reason(12);return;}const auto module=GetModuleHandleW(L"vulkan-1.dll");if(!module)return;
    auto gdpa=GetProcAddress(module,"vkGetDeviceProcAddr"),gipa=GetProcAddress(module,"vkGetInstanceProcAddr"),
        ci=GetProcAddress(module,"vkCreateImage"),di=GetProcAddress(module,"vkDestroyImage"),
        cd=GetProcAddress(module,"vkCreateDevice"),dd=GetProcAddress(module,"vkDestroyDevice");if(!gdpa||!gipa||!ci||!di||!cd||!dd)return;
    h.loader_create_image=ci;h.loader_destroy_image=di;
    h.gdpa_hook=safetyhook::create_inline(gdpa,reinterpret_cast<void*>(&get_device),SafetyHookInline::StartDisabled);
    h.gipa_hook=safetyhook::create_inline(gipa,reinterpret_cast<void*>(&get_instance),SafetyHookInline::StartDisabled);
    h.create_image_hook=safetyhook::create_inline(ci,reinterpret_cast<void*>(&create_image),SafetyHookInline::StartDisabled);
    h.destroy_image_hook=safetyhook::create_inline(di,reinterpret_cast<void*>(&destroy_image),SafetyHookInline::StartDisabled);
    h.create_device_hook=safetyhook::create_inline(cd,reinterpret_cast<void*>(&create_device),SafetyHookInline::StartDisabled);
    h.destroy_device_hook=safetyhook::create_inline(dd,reinterpret_cast<void*>(&destroy_device),SafetyHookInline::StartDisabled);
    if(!h.gdpa_hook||!h.gipa_hook||!h.create_image_hook||!h.destroy_image_hook||!h.create_device_hook||!h.destroy_device_hook){
        h.gdpa_hook={};h.gipa_hook={};h.create_image_hook={};h.destroy_image_hook={};h.create_device_hook={};h.destroy_device_hook={};return;}
    h.gdpa=h.gdpa_hook.original<PFN_vkGetDeviceProcAddr>();h.gipa=h.gipa_hook.original<PFN_vkGetInstanceProcAddr>();
    h.create_image=h.create_image_hook.original<PFN_vkCreateImage>();h.destroy_image=h.destroy_image_hook.original<PFN_vkDestroyImage>();
    h.create_device=h.create_device_hook.original<PFN_vkCreateDevice>();h.destroy_device=h.destroy_device_hook.original<PFN_vkDestroyDevice>();
    (void)h.create_image_hook.enable();(void)h.destroy_image_hook.enable();(void)h.create_device_hook.enable();(void)h.destroy_device_hook.enable();
    (void)h.gdpa_hook.enable();(void)h.gipa_hook.enable();
    install_luke_image_dispatch();
    ingress_reason(13,install_command_entry_hooks(module));
}catch(...) {}}

VkDevice configure_luke_owner(ID3D12Device* owner,ID3D12CommandQueue* queue) noexcept {
    if(!luke_vulkan_process()||!native_ingress::luke_detected()||!owner||!queue)return {};
    try {
        poll_capture();std::vector<VulkanSessionBinding> observed;
        {std::scoped_lock lock(hooks().mutex);for(const auto& [device,binding]:hooks().observed_devices)
            if(hooks().interop_devices[device])observed.push_back(binding);}
        VulkanSessionBinding selected{};const auto luid=owner->GetAdapterLuid();
        for(const auto& binding:observed) {
            auto properties=reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(hooks().gipa(binding.instance,"vkGetPhysicalDeviceProperties2"));
            if(!properties)continue;
            VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};info.pNext=&identity;
            properties(binding.physical_device,&info);
            if(!identity.deviceLUIDValid||std::memcmp(identity.deviceLUID,&luid,sizeof(luid)))continue;
            if(selected.device)return {}; // multiple native owners are ambiguous
            selected=binding;
        }
        if(!selected.device){ingress_reason(9,observed.size());return {};}
        configure_owner(selected,owner,queue);
        std::scoped_lock lock(hooks().mutex);
        if(!hooks().devices.contains(selected.device)){ingress_reason(10);return {};}
        ingress_reason(11,reinterpret_cast<std::uint64_t>(selected.device));return selected.device;
    }catch(...){return {};}
}
bool query_luke_context(ID3D12Device** owner,ID3D12CommandQueue** queue) noexcept {
    if(owner)*owner=nullptr;if(queue)*queue=nullptr;
    if(!owner||!queue||!luke_vulkan_process()||!native_ingress::luke_detected())return false;
    try {
        std::vector<std::shared_ptr<Device>> devices;
        {std::scoped_lock lock(hooks().mutex);for(const auto& [key,d]:hooks().devices)devices.push_back(d);}
        std::shared_ptr<Device> selected;
        for(const auto& d:devices){std::scoped_lock lock(d->mutex);
            if(d->native_alive&&d->owner_active&&d->production_submitted){if(selected)return false;selected=d;}}
        if(!selected)return false;
        std::scoped_lock lock(selected->mutex);
        if(!selected->native_alive||!selected->owner_active||!selected->production_submitted||FAILED(selected->owner.CopyTo(owner)))return false;
        if(FAILED(selected->queue.CopyTo(queue))){(*owner)->Release();*owner=nullptr;return false;}
        return true;
    }catch(...){return false;}
}
}

extern "C" __declspec(dllexport) void OFXR_ObserveVulkanDeviceDispatchV1(
    VkDevice device,PFN_vkGetDeviceProcAddr next_device_get) noexcept {
    xrfg::native_vulkan::observe_device_dispatch(device,next_device_get);
}

extern "C" __declspec(dllexport) void OFXR_StartLukeVulkanIngressV1() noexcept {
    // Called by the already armed Vulkan layer before the game creates devices.
    // Never from DllMain, and the exact executable gate keeps all other Vulkan
    // applications on the suspended path.
    if(xrfg::native_vulkan::luke_vulkan_process()) {
        // This startup can precede both NGX discovery and the first XR instance.
        xrfg::native_ingress::refresh_luke_detection(false);
        xrfg::native_vulkan::poll_capture();
    }
}

#if defined(XRFG_NATIVE_INGRESS_TESTS)
#include "native_vulkan_transport_test.inc"
#endif
