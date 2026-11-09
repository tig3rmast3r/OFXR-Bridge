#include "xrfg/native_vulkan_transport_test.hpp"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <nvsdk_ngx_helpers_vk.h>
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/native_ingress/streamline1/sl1.h"
#include <filesystem>

namespace xrfg::native_vulkan {
namespace {
struct TransportSkip:std::runtime_error {using std::runtime_error::runtime_error;};
void require_transport(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
void require_transport(VkResult result,const char* message) {
    if(result!=VK_SUCCESS){std::ostringstream text;text<<message<<": VkResult "<<result;throw std::runtime_error(text.str());}
}
void require_transport(HRESULT result,const char* message) {
    if(FAILED(result)){std::ostringstream text;text<<message<<": HRESULT 0x"<<std::hex<<result;throw std::runtime_error(text.str());}
}
struct HostBuffer {VkBuffer buffer{};VkDeviceMemory memory{};};
struct ImageEntryProbe {
    VkImage image{reinterpret_cast<VkImage>(std::uintptr_t{0x281})};
    VkDevice last_device{},foreign{};const VkAllocationCallbacks* last_allocator{};
    VkImageCreateInfo last_info{};
    unsigned native_calls{},alternate_calls{},loader_calls{},destroy_calls{};
    bool reject_augmented{},change_extent{},independent_on_retry{},foreign_forward{},collision{},unknown_alias{};
    VkResult result{VK_SUCCESS};PFN_vkCreateImage cached{},alternate{};
} image_entry_probe;
__declspec(noinline) VkResult VKAPI_CALL image_entry_native(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* image) {
    auto& probe=image_entry_probe;++probe.native_calls;probe.last_device=device;probe.last_allocator=allocator;
    if(info)probe.last_info=*info;
    if(probe.reject_augmented&&info&&(info->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if(probe.result==VK_SUCCESS&&image)*image=probe.image;return probe.result;
}
__declspec(noinline) VkResult VKAPI_CALL image_entry_alternate(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* image) {
    auto& probe=image_entry_probe;++probe.alternate_calls;probe.last_device=device;probe.last_allocator=allocator;
    if(info)probe.last_info=*info;if(image)*image=probe.collision?probe.image:reinterpret_cast<VkImage>(std::uintptr_t{0x282});return VK_SUCCESS;
}
__declspec(noinline) void VKAPI_CALL image_entry_destroy(VkDevice device,VkImage,const VkAllocationCallbacks* allocator) {
    auto& probe=image_entry_probe;++probe.destroy_calls;probe.last_device=device;probe.last_allocator=allocator;
}
PFN_vkVoidFunction VKAPI_CALL image_entry_dispatch(VkDevice,const char* name) {
    if(!std::strcmp(name,"vkCreateImage"))return reinterpret_cast<PFN_vkVoidFunction>(&image_entry_alternate);
    if(!std::strcmp(name,"vkDestroyImage"))return reinterpret_cast<PFN_vkVoidFunction>(&image_entry_destroy);
    return nullptr;
}
VkResult VKAPI_CALL image_entry_loader(VkDevice device,const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator,VkImage* image) {
    auto& probe=image_entry_probe;++probe.loader_calls;auto copied=*info;
    if(probe.change_extent)copied.extent.width=32;
    if((probe.independent_on_retry&&!(info->usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT))||probe.collision) {
        auto independent=*info;independent.usage=VK_IMAGE_USAGE_SAMPLED_BIT;VkImage other{};
        const auto result=probe.alternate(probe.collision?probe.foreign:device,&independent,allocator,&other);
        require_transport(result==VK_SUCCESS&&image_entry_probe.last_info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "independent nested image lost transfer policy during outer fallback");
    }
    const auto result=probe.cached(probe.foreign_forward?probe.foreign:device,&copied,allocator,image);
    if(result==VK_SUCCESS&&probe.unknown_alias)*image=reinterpret_cast<VkImage>(std::uintptr_t{0x283});
    return result;
}
void verify_image_entry_contract() {
    struct Restore {
        PFN_vkCreateImage creator{hooks().create_image};PFN_vkDestroyImage destroyer{hooks().destroy_image};
        PFN_vkGetDeviceProcAddr gdpa{hooks().gdpa};
        ~Restore(){hooks().create_image=creator;hooks().destroy_image=destroyer;hooks().gdpa=gdpa;
            std::scoped_lock lock(hooks().mutex);hooks().image_usage.erase(reinterpret_cast<VkImage>(std::uintptr_t{0x281}));
            hooks().image_usage.erase(reinterpret_cast<VkImage>(std::uintptr_t{0x282}));}
    } restore;
    image_entry_probe={};auto& probe=image_entry_probe;
    const auto device=reinterpret_cast<VkDevice>(std::uintptr_t{0x181});probe.foreign=reinterpret_cast<VkDevice>(std::uintptr_t{0x182});
    VkAllocationCallbacks allocator{};VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_R16G16_SFLOAT;info.extent={16,8,1};
    info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;info.usage=VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto unchanged=info;
    // These raw addresses are cached before installing the actual inline entry.
    probe.cached=&image_entry_native;auto cached_destroy=&image_entry_destroy;
    require_transport(install_image_entry(reinterpret_cast<PFN_vkVoidFunction>(probe.cached),true)&&
        install_image_entry(reinterpret_cast<PFN_vkVoidFunction>(cached_destroy),false),"install cached actual allocation entries");
    VkImage image{};require_transport(probe.cached(device,&info,&allocator,&image),"cached actual allocation call");
    require_transport(probe.native_calls==1&&probe.last_device==device&&probe.last_allocator==&allocator&&
        (probe.last_info.usage&VK_IMAGE_USAGE_TRANSFER_SRC_BIT)&&std::memcmp(&info,&unchanged,sizeof(info))==0&&
        hooks().image_usage.at(image).device==device,"actual original arguments/count/caller descriptor or allocation receipt changed");
    cached_destroy(probe.foreign,image,&allocator);
    require_transport(probe.destroy_calls==1&&hooks().image_usage.contains(image),"foreign destroy retired another device's allocation");
    cached_destroy(device,image,&allocator);
    require_transport(probe.destroy_calls==2&&!hooks().image_usage.contains(image)&&probe.last_device==device&&probe.last_allocator==&allocator,
        "cached native destroy did not retire exact allocation or forward once");
    hooks().gdpa=&image_entry_dispatch;
    const auto later=get_device(device,"vkCreateImage");
    require_transport(later==reinterpret_cast<PFN_vkVoidFunction>(&image_entry_alternate),"later GDPA lost its exact native entry");
    probe.alternate=reinterpret_cast<PFN_vkCreateImage>(later);
    require_transport(probe.alternate(device,&info,&allocator,&image),"later actual GDPA allocation");
    require_transport(probe.alternate_calls==1&&probe.last_device==device&&probe.last_allocator==&allocator,
        "later GDPA dispatched through a different original");
    hooks().create_image=&image_entry_loader;probe.native_calls=0;probe.loader_calls=0;probe.change_extent=true;
    require_transport(create_image(device,&info,&allocator,&image),"nested loader to actual entry");
    require_transport(probe.native_calls==1&&probe.loader_calls==1&&hooks().image_usage.at(image).extent.width==32,
        "loader nesting duplicated original or replaced frozen native descriptor");
    probe.change_extent=false;probe.reject_augmented=true;probe.independent_on_retry=true;
    probe.native_calls=probe.loader_calls=probe.alternate_calls=0;
    require_transport(create_image(device,&info,&allocator,&image),"native usage fallback through nested loader");
    require_transport(probe.native_calls==2&&probe.loader_calls==2&&probe.alternate_calls==1&&
        hooks().image_usage.at(image).usage==info.usage,"nested fallback duplicated augmentation/retry or independent allocation");
    probe.reject_augmented=probe.independent_on_retry=false;probe.foreign_forward=true;
    require_transport(create_image(device,&info,&allocator,&image),"foreign native owner call");
    auto resource=NVSDK_NGX_Create_ImageView_Resource_VK(reinterpret_cast<VkImageView>(std::uintptr_t{0x381}),image,
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},info.format,16,8,false);
    require_transport((image_receipt(&resource,device).rejection&128)!=0,"outer loader invented ownership for a foreign native allocation");
    probe.foreign_forward=false;probe.collision=true;
    require_transport(create_image(device,&info,&allocator,&image),"nested colliding foreign handles");
    require_transport((image_receipt(&resource,device).rejection&64)!=0,"ambiguous child device handles accepted by last-match receipt");
    probe.collision=false;probe.unknown_alias=true;
    require_transport(create_image(device,&info,&allocator,&image),"changed public allocation output");
    require_transport(!hooks().image_usage.contains(image)&&hooks().image_usage.contains(probe.image),
        "public alias invented allocation metadata instead of retaining the actual native output");
    probe.unknown_alias=false;hooks().image_usage.erase(probe.image);
    probe.result=VK_ERROR_DEVICE_LOST;info.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    probe.native_calls=0;image={};
    require_transport(probe.cached(device,&info,&allocator,&image)==VK_ERROR_DEVICE_LOST&&probe.native_calls==1&&
        !hooks().image_usage.contains(probe.image),"actual failure/result/count invented an allocation");
    std::cout<<"Native Vulkan actual allocation entries: cached/later dispatch, exact originals, nested fallback, immutable native receipt, foreign and ambiguous owner rejection PASS\n";
}
struct MetadataParameters final:NVSDK_NGX_Parameter {
    bool present{true},flags_present{true};int flags{NVSDK_NGX_DLSS_Feature_Flags_MVLowRes};
    unsigned width{1672},height{1368},out_width{3344},out_height{2736};
    void Set(const char*,unsigned long long)override{}
    void Set(const char*,float)override{} void Set(const char*,double)override{}
    void Set(const char*,unsigned)override{} void Set(const char*,int)override{}
    void Set(const char*,ID3D11Resource*)override{} void Set(const char*,ID3D12Resource*)override{}
    void Set(const char*,void*)override{}
    NVSDK_NGX_Result Get(const char*,unsigned long long*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,float*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,double*)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D11Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,ID3D12Resource**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char*,void**)const override{return NVSDK_NGX_Result_FAIL_InvalidParameter;}
    NVSDK_NGX_Result Get(const char* key,int* value)const override {
        if(present&&flags_present&&!std::strcmp(key,NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags)){
            *value=flags;return NVSDK_NGX_Result_Success;}
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    NVSDK_NGX_Result Get(const char* key,unsigned* value)const override {
        if(!present)return NVSDK_NGX_Result_FAIL_InvalidParameter;
        if(!std::strcmp(key,NVSDK_NGX_Parameter_Width))*value=width;
        else if(!std::strcmp(key,NVSDK_NGX_Parameter_Height))*value=height;
        else if(!std::strcmp(key,NVSDK_NGX_Parameter_OutWidth))*value=out_width;
        else if(!std::strcmp(key,NVSDK_NGX_Parameter_OutHeight))*value=out_height;
        else return NVSDK_NGX_Result_FAIL_InvalidParameter;
        return NVSDK_NGX_Result_Success;
    }
    void Reset()override{present=false;}
};
const NVSDK_NGX_Handle* metadata_creation_handle{},*metadata_alias{};
VkCommandBuffer metadata_other_command{},metadata_callback_command{},metadata_callback_recording{};
Feature metadata_callback_feature{};unsigned metadata_native_slot{};
std::uint64_t metadata_outer_count{},metadata_native_count{};
void* metadata_callback_argument{};NVSDK_NGX_Result metadata_result{NVSDK_NGX_Result_Success};
NVSDK_NGX_Result metadata_create_original(VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    if(!out||!p)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=const_cast<NVSDK_NGX_Handle*>(metadata_creation_handle);if(p)p->Reset();return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result metadata_create1_original(VkDevice,VkCommandBuffer cmd,NVSDK_NGX_Feature type,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
    return metadata_create_original(cmd,type,p,out);
}
NVSDK_NGX_Result metadata_native_original(VkCommandBuffer cmd,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter*,void* callback) {
    metadata_callback_command=cmd;metadata_callback_argument=callback;
    {std::scoped_lock lock(hooks().mutex);metadata_callback_feature=hooks().features.at(h);}
    if(const auto scope=EvaluationDeviceScope::exact(cmd,metadata_callback_feature.type))
        metadata_callback_recording=scope->recording();else metadata_callback_recording={};
    metadata_native_count=native_ingress::upscaler_observation().evaluations;
    return metadata_result;
}
NVSDK_NGX_Result metadata_generic_original(VkCommandBuffer cmd,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* p,void* callback) {
    metadata_outer_count=native_ingress::upscaler_observation().evaluations;
    auto& changed=*static_cast<MetadataParameters*>(const_cast<NVSDK_NGX_Parameter*>(p));
    changed.present=true;changed.flags_present=true;
    std::swap(changed.width,changed.out_width);std::swap(changed.height,changed.out_height);
    const auto input=metadata_other_command?metadata_other_command:cmd;
    return metadata_native_slot==0?evaluate<0>(input,metadata_alias,p,callback):evaluate<1>(input,metadata_alias,p,callback);
}
void verify_metadata_dispatch(VkDevice device,VkCommandBuffer command,VkCommandBuffer other) {
    const auto cached_create=&create<2>;const auto cached_evaluate=&evaluate<2>;
    struct RestoreModules {
        struct Original{Create create;Create1 create1;Evaluate evaluate;bool typed;};
        std::array<Original,3> saved{};
        std::vector<const NVSDK_NGX_Handle*> handles;
        RestoreModules(){for(unsigned i=0;i<saved.size();++i){auto& m=hooks().modules[i];saved[i]={m.create,m.create1,m.evaluate,m.typed_implementation};}}
        ~RestoreModules(){std::scoped_lock lock(hooks().mutex);
            for(unsigned i=0;i<saved.size();++i){auto& m=hooks().modules[i];m.create=saved[i].create;m.create1=saved[i].create1;m.evaluate=saved[i].evaluate;m.typed_implementation=saved[i].typed;}
            for(auto* h:handles)hooks().features.erase(h);
            metadata_creation_handle=metadata_alias=nullptr;metadata_other_command={};}
    } restore;
    auto& generic=hooks().modules[2];generic.create=&metadata_create_original;
    generic.evaluate=&metadata_generic_original;generic.typed_implementation=false;
    for(unsigned index=0;index<2;++index){auto& m=hooks().modules[index];m.create=&metadata_create_original;
        m.create1=&metadata_create1_original;m.evaluate=&metadata_native_original;m.typed_implementation=true;}
    for(unsigned slot=0;slot<2;++slot) {
        metadata_native_slot=slot;const auto type=slot==0?NVSDK_NGX_Feature_SuperSampling:NVSDK_NGX_Feature_RayReconstruction;
        alignas(16) std::array<unsigned char,16> public_storage{},alias_storage{},late_storage{},direct_storage{};
        auto* public_handle=reinterpret_cast<const NVSDK_NGX_Handle*>(public_storage.data());
        auto* alias=reinterpret_cast<const NVSDK_NGX_Handle*>(alias_storage.data());
        auto* late=reinterpret_cast<const NVSDK_NGX_Handle*>(late_storage.data());
        auto* direct=reinterpret_cast<const NVSDK_NGX_Handle*>(direct_storage.data());
        restore.handles.insert(restore.handles.end(),{public_handle,alias,late,direct});
        MetadataParameters p;metadata_creation_handle=public_handle;NVSDK_NGX_Handle* made{};
        require_transport(cached_create(command,type,nullptr,&made)==NVSDK_NGX_Result_FAIL_InvalidParameter&&
            !made&&!hooks().features.contains(public_handle),"Vulkan null parameters were not forwarded unchanged");
        require_transport(NVSDK_NGX_SUCCEED(cached_create(command,type,&p,&made))&&!p.present&&made==public_handle,
            "native Vulkan original creator/result/mutation changed");
        const auto original=hooks().features.at(made);
        require_transport(original.creation_observed&&original.flags_known&&original.width==1672&&original.out_width==3344&&
            original.out_height==2736,"native Vulkan creation dimensions read after original consumed them");
        metadata_alias=alias;auto* callback=reinterpret_cast<void*>(std::uintptr_t{0x83});
        const auto before=native_ingress::upscaler_observation().evaluations;
        require_transport(NVSDK_NGX_SUCCEED(cached_evaluate(command,made,&p,callback)),"cached Vulkan alias result");
        require_transport(metadata_outer_count==before&&metadata_native_count==before+1&&metadata_callback_argument==callback,
            "Vulkan generic/native SR/RR boundary duplicated or original callback changed");
        require_transport(metadata_callback_feature.creation_observed&&metadata_callback_feature.out_width==3344&&
            metadata_callback_feature.out_height==2736&&metadata_callback_feature.width==1672&&
            metadata_callback_feature.creation_source_identity==original.identity&&
            metadata_callback_feature.device==device&&p.out_width==1672,
            "native Vulkan alias trusted inverted evaluation dimensions");
        require_transport(metadata_callback_recording==command&&metadata_callback_command==native(command)&&can_split(command),
            "typed native suffix lost its exact original recording/owner");
        metadata_result=NVSDK_NGX_Result_FAIL_InvalidParameter;
        require_transport(cached_evaluate(command,made,&p,callback)==metadata_result,"native Vulkan failure result changed");
        metadata_result=NVSDK_NGX_Result_Success;
        const auto alias_generation=hooks().features.at(alias).identity;
        metadata_creation_handle=public_handle;p.present=true;p.flags_present=false;
        p.width=900;p.height=600;p.out_width=1800;p.out_height=1200;
        require_transport(NVSDK_NGX_SUCCEED(cached_create(command,type,&p,&made))&&
            !hooks().features.at(made).flags_known,"Vulkan public recreation must preserve unknown actual creation flags");
        require_transport(NVSDK_NGX_SUCCEED(cached_evaluate(command,made,&p,callback))&&
            metadata_callback_feature.flags_known&&metadata_callback_feature.out_width==1800&&
            metadata_callback_feature.out_height==1200&&metadata_callback_feature.width==900&&
            metadata_callback_feature.creation_source_identity==hooks().features.at(made).identity&&
            metadata_callback_feature.identity!=alias_generation,
            "Vulkan recreated receipt failed to refresh alias generation or merge exact typed flags");
        metadata_alias=late;metadata_other_command=other;p.present=true;
        require_transport(NVSDK_NGX_SUCCEED(cached_evaluate(command,made,&p,callback))&&
            !metadata_callback_feature.creation_observed&&!metadata_callback_feature.out_width&&
            metadata_callback_feature.flags_known&&metadata_callback_feature.device==device,
            "Vulkan foreign command inherited an enclosing creation receipt");
        metadata_other_command={};metadata_creation_handle=direct;
        p.present=true;p.width=2048;p.height=1080;p.out_width=4096;p.out_height=2160;
        require_transport(NVSDK_NGX_SUCCEED(slot==0?create1<0>(device,command,type,&p,&made):
            create1<1>(device,command,type,&p,&made)),"explicit-device Vulkan creation");
        const auto direct_created=hooks().features.at(made);metadata_alias=direct;
        p.present=true;p.width=1672;p.height=1368;p.out_width=3344;p.out_height=2736;
        require_transport(NVSDK_NGX_SUCCEED(cached_evaluate(command,public_handle,&p,callback))&&
            metadata_callback_feature.creation_source_identity==direct_created.identity&&
            metadata_callback_feature.out_width==4096,"Vulkan public ancestor overwrote direct native creation");
        for(auto* h:{public_handle,alias,late,direct}){hooks().features.erase(h);std::erase(restore.handles,h);}
    }
    std::cout<<"Native Vulkan immutable creation/cached SR-RR aliases: exact native suffix/original recording, resource fallback, callback/result isolation PASS\n";
}
struct PublicFamilyProbe {
    HMODULE module{};Create cached_create{};Evaluate cached_evaluate{};Release cached_release{};
    alignas(16) std::array<unsigned char,16> public_storage{},native_storage{},alias_storage{},sr_storage{};
    const NVSDK_NGX_Handle* public_handle{reinterpret_cast<const NVSDK_NGX_Handle*>(public_storage.data())};
    const NVSDK_NGX_Handle* native_handle{reinterpret_cast<const NVSDK_NGX_Handle*>(native_storage.data())};
    const NVSDK_NGX_Handle* alias{reinterpret_cast<const NVSDK_NGX_Handle*>(alias_storage.data())};
    const NVSDK_NGX_Handle* sr{reinterpret_cast<const NVSDK_NGX_Handle*>(sr_storage.data())};
    const NVSDK_NGX_Handle* selected{};
    VkCommandBuffer command{},other{},received{};void* callback{};
    unsigned slot{1},public_calls{},native_calls{},release_calls{},create_calls{};
    bool cached_late{},foreign_command{},second_child{},recursive{},outer_failure{},reuse_on_release{};
    NVSDK_NGX_Result native_result{NVSDK_NGX_Result_Success};
    Feature received_feature{};
} public_family_probe;
NVSDK_NGX_Result family_create_original(VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle** out) {
    ++public_family_probe.create_calls;if(!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *out=const_cast<NVSDK_NGX_Handle*>(public_family_probe.public_handle);return NVSDK_NGX_Result_Success;
}
NVSDK_NGX_Result family_create1_original(VkDevice,VkCommandBuffer command,NVSDK_NGX_Feature type,
    NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out){return family_create_original(command,type,p,out);}
NVSDK_NGX_Result family_native_original(VkCommandBuffer command,const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter*,void* callback) {
    auto& probe=public_family_probe;++probe.native_calls;probe.received=command;probe.callback=callback;
    {std::scoped_lock lock(hooks().mutex);probe.received_feature=hooks().features.at(handle);}
    return probe.native_result;
}
NVSDK_NGX_Result family_public_original(VkCommandBuffer command,const NVSDK_NGX_Handle*,
    const NVSDK_NGX_Parameter* p,void* callback) {
    auto& probe=public_family_probe;++probe.public_calls;
    const auto actual=probe.foreign_command?probe.other:command;
    const auto invoke=[&](const NVSDK_NGX_Handle* handle){return probe.slot==0?
        evaluate<0>(actual,handle,p,callback):evaluate<1>(actual,handle,p,callback);};
    auto result=invoke(probe.selected);
    if(probe.second_child)result=invoke(probe.selected==probe.native_handle?probe.alias:probe.native_handle);
    if(probe.recursive){probe.recursive=false;result=probe.cached_evaluate(command,probe.public_handle,p,callback);probe.recursive=true;}
    return probe.outer_failure?NVSDK_NGX_Result_FAIL_InvalidParameter:result;
}
NVSDK_NGX_Result family_release_original(NVSDK_NGX_Handle*) {
    auto& probe=public_family_probe;++probe.release_calls;
    if(probe.reuse_on_release) {
        MetadataParameters p;NVSDK_NGX_Handle* made{};
        require_transport(NVSDK_NGX_SUCCEED(probe.cached_create(probe.command,NVSDK_NGX_Feature_RayReconstruction,&p,&made))&&
            made==probe.public_handle,"public release callback could not recreate its exact opaque handle");
        require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(probe.command,made,&p,probe.callback)),
            "public release callback could not evaluate its new generation");
    }
    return NVSDK_NGX_Result_Success;
}
void cache_public_family_creation(HMODULE module={}) {
    auto& probe=public_family_probe;if(probe.cached_late)return;
    probe.module=module?module:LoadLibraryW(L"luke-runtime-fixture.dll");
    require_transport(probe.module!=nullptr,"load marked public Vulkan NGX fixture");
    const auto configure=reinterpret_cast<void(*)(Create,Create1,Evaluate,Release)>(GetProcAddress(probe.module,"TestLukeVulkanNgxCallbacks"));
    probe.cached_create=reinterpret_cast<Create>(GetProcAddress(probe.module,"NVSDK_NGX_VULKAN_CreateFeature"));
    probe.cached_evaluate=reinterpret_cast<Evaluate>(GetProcAddress(probe.module,"NVSDK_NGX_VULKAN_EvaluateFeature"));
    probe.cached_release=reinterpret_cast<Release>(GetProcAddress(probe.module,"NVSDK_NGX_VULKAN_ReleaseFeature"));
    require_transport(configure&&probe.cached_create&&probe.cached_evaluate&&probe.cached_release,"cache declared public Vulkan NGX originals");
    configure(&family_create_original,&family_create1_original,&family_public_original,&family_release_original);
    MetadataParameters p;NVSDK_NGX_Handle* made{};
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_create({},NVSDK_NGX_Feature_RayReconstruction,&p,&made))&&
        made==probe.public_handle&&!hooks().features.contains(made),"actual cached public creation was not late/unobserved");
    probe.cached_late=true;
}
void verify_public_family_dispatch(VkDevice device,VkCommandBuffer command,VkCommandBuffer other) {
    cache_public_family_creation();install_luke_feature_dispatch();
    require_transport(hooks().modules[3].retained!=nullptr,"marked public Vulkan NGX entry was not intercepted");
    auto& probe=public_family_probe;probe.command=command;probe.other=other;
    struct Restore {
        Evaluate sr{hooks().modules[0].evaluate},rr{hooks().modules[1].evaluate};
        Create create0{hooks().modules[0].create},create1{hooks().modules[1].create};
        bool typed0{hooks().modules[0].typed_implementation},typed1{hooks().modules[1].typed_implementation};
        ~Restore(){hooks().modules[0].evaluate=sr;hooks().modules[1].evaluate=rr;
            hooks().modules[0].create=create0;hooks().modules[1].create=create1;
            hooks().modules[0].typed_implementation=typed0;hooks().modules[1].typed_implementation=typed1;
            for(auto* h:{public_family_probe.public_handle,public_family_probe.native_handle,public_family_probe.alias,public_family_probe.sr})hooks().features.erase(h);
            hooks().public_lifetimes.erase(public_family_probe.public_handle);metadata_creation_handle=nullptr;}
    } restore;
    for(unsigned i=0;i<2;++i){hooks().modules[i].typed_implementation=true;
        hooks().modules[i].evaluate=&family_native_original;hooks().modules[i].create=&metadata_create_original;}
    MetadataParameters p;p.width=17;p.height=9;p.out_width=34;p.out_height=18;
    metadata_creation_handle=probe.native_handle;NVSDK_NGX_Handle* made{};
    require_transport(NVSDK_NGX_SUCCEED(create<1>(command,NVSDK_NGX_Feature_RayReconstruction,&p,&made)),"observe actual native RR creation");
    const auto direct=hooks().features.at(made);
    metadata_creation_handle=probe.sr;p.present=true;
    require_transport(NVSDK_NGX_SUCCEED(create<0>(command,NVSDK_NGX_Feature_SuperSampling,&p,&made)),"observe separate typed SR creation");
    const auto callback=reinterpret_cast<void*>(std::uintptr_t{0x993});
    std::shared_ptr<PublicFeatureLifetime> lifetime;
    for(unsigned n=1;n<=6;++n) {
        p.present=true;probe.selected=n%2?probe.native_handle:probe.alias;
        const auto before_public=probe.public_calls,before_native=probe.native_calls;
        require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback)),"cached public alternating typed RR result");
        const auto found=hooks().public_lifetimes.find(probe.public_handle);
        require_transport(found!=hooks().public_lifetimes.end(),"two native handles split one proven public Vulkan production family");
        if(n==1)lifetime=found->second;
        require_transport(found->second==lifetime&&lifetime->sequence==n&&lifetime->type_known&&
            lifetime->type==NVSDK_NGX_Feature_RayReconstruction&&lifetime->device==device&&
            probe.public_calls==before_public+1&&probe.native_calls==before_native+1&&probe.callback==callback&&
            probe.received==native(command),"Vulkan public production clock/callback/native suffix changed");
        require_transport(!hooks().features.contains(probe.public_handle)&&
            hooks().features.at(probe.native_handle).creation_source_identity==direct.identity&&
            hooks().features.at(probe.native_handle).out_width==34,"late public family invented or replaced direct creation geometry");
        if(n%2==0)require_transport(!probe.received_feature.creation_observed&&!probe.received_feature.out_width&&
            probe.received_feature.flags_known,"late native alias inherited invented public creation dimensions");
    }
    // These captures deliberately lack guide resources, yet each actual native
    // body consumed one production. Failed captures may never close that gap.
    probe.native_result=NVSDK_NGX_Result_FAIL_InvalidParameter;
    require_transport(probe.cached_evaluate(command,probe.public_handle,&p,callback)==probe.native_result&&lifetime->sequence==7,
        "failed native Vulkan production did not consume exactly one clock value");
    probe.native_result=NVSDK_NGX_Result_Success;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==8,
        "failed Vulkan capture compressed the next native production interval");
    probe.foreign_command=true;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==8,
        "foreign native VkCommandBuffer acquired public production ownership");probe.foreign_command=false;
    const auto saved=hooks().features.at(probe.native_handle);probe.selected=probe.native_handle;
    hooks().features.at(probe.native_handle).device=reinterpret_cast<VkDevice>(std::uintptr_t{0x818});
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==8,
        "foreign native VkDevice acquired public production ownership");hooks().features.at(probe.native_handle)=saved;
    probe.slot=0;probe.selected=probe.sr;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==8,
        "wrong typed Vulkan feature acquired RR public production ownership");probe.slot=1;probe.selected=probe.native_handle;
    probe.second_child=true;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==9,
        "multiple native Vulkan children advanced a public invocation twice");probe.second_child=false;
    probe.outer_failure=true;
    require_transport(probe.cached_evaluate(command,probe.public_handle,&p,callback)==NVSDK_NGX_Result_FAIL_InvalidParameter&&lifetime->sequence==10,
        "public Vulkan failure/result changed or actual production was uncounted");probe.outer_failure=false;
    probe.recursive=true;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&p,callback))&&lifetime->sequence==11,
        "recursive public Vulkan dispatch admitted a second production");probe.recursive=false;
    MetadataParameters creation;NVSDK_NGX_Handle* recreated{};
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_create(command,NVSDK_NGX_Feature_RayReconstruction,&creation,&recreated))&&
        !hooks().public_lifetimes.contains(probe.public_handle),"observed public creation did not retire its late lifetime");
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,probe.public_handle,&creation,callback)),"new observed public generation evaluation");
    const auto previous=hooks().public_lifetimes.at(probe.public_handle);const auto previous_feature=hooks().features.at(probe.public_handle);
    require_transport(previous!=lifetime&&previous->sequence==1,"actual public creation reused an old Vulkan production clock");
    probe.reuse_on_release=true;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_release(const_cast<NVSDK_NGX_Handle*>(probe.public_handle))),"public release original result");
    const auto newest=hooks().public_lifetimes.at(probe.public_handle);
    require_transport(newest!=previous&&newest->sequence==1&&hooks().features.at(probe.public_handle).identity!=previous_feature.identity,
        "public release erased callback-reused feature generation or its new production clock");probe.reuse_on_release=false;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_release(const_cast<NVSDK_NGX_Handle*>(probe.public_handle)))&&
        !hooks().public_lifetimes.contains(probe.public_handle)&&!hooks().features.contains(probe.public_handle),"public Vulkan terminal release retained lifetime/geometry");
    probe.slot=0;probe.selected=probe.sr;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_create(command,NVSDK_NGX_Feature_SuperSampling,&creation,&recreated))&&
        NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,recreated,&creation,callback)),"genuine public Vulkan SR generation");
    const auto sr_lifetime=hooks().public_lifetimes.at(probe.public_handle);
    require_transport(sr_lifetime->sequence==1&&sr_lifetime->type_known&&sr_lifetime->type==NVSDK_NGX_Feature_SuperSampling,
        "typed Vulkan SR failed exact public family classification");
    hooks().modules[0].typed_implementation=false;
    const auto observed_before=native_ingress::upscaler_observation().evaluations;
    const auto published_before=dlss_motion_vector_statistics().published;
    const auto calls_before=probe.native_calls,public_before=probe.public_calls;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_evaluate(command,recreated,&creation,callback))&&
        sr_lifetime->sequence==1&&probe.native_calls==calls_before+1&&probe.public_calls==public_before+1&&
        probe.callback==callback&&native_ingress::upscaler_observation().evaluations==observed_before&&
        dlss_motion_vector_statistics().published==published_before,
        "typed-unavailable public Vulkan call fell back to generic raw capture or changed its original callback");
    hooks().modules[0].typed_implementation=true;
    require_transport(NVSDK_NGX_SUCCEED(probe.cached_release(const_cast<NVSDK_NGX_Handle*>(probe.public_handle)))&&
        !hooks().public_lifetimes.contains(probe.public_handle),"public Vulkan SR terminal lifetime survived");probe.slot=1;
    std::cout<<"Native Vulkan marked cached public family: late creator, exact native command/device/type, alternating raw handles, actual failed-production gaps, callbacks and release ABA PASS\n";
}
struct TransportProbe {
    static constexpr unsigned width=16,height=8;
    unsigned pixel_size{4},byte_count{width*height*4};
    HMODULE loader{};VkInstance instance{};VkPhysicalDevice physical{};VkDevice native_device{};
    VkQueue native_queue{};uint32_t family{};PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    std::shared_ptr<Device> device;std::shared_ptr<Slot> captured;
    VkCommandPool pool{};VkCommandBuffer command{};VkImage image{};VkImageView image_view{};VkDeviceMemory image_memory{};VkFence finished{};
    HostBuffer upload,readback;ComPtr<ID3D12Resource> d12_readback;ComPtr<ID3D12Fence> progress;
    uint64_t public_readback_value{100};
    ComPtr<ID3D12InfoQueue> debug;std::array<unsigned char,width*height*8> expected{};
    bool bootstrap{},rgba_motion{};
    Role capture_role{motion};
    unsigned initial_public_create{},initial_public_destroy{};
    PFN_vkGetDeviceProcAddr cached_allocation_dispatch{};
    PFN_vkGetDeviceProcAddr cached_public_dispatch{};
    void(*public_counts)(unsigned*,unsigned*,unsigned*){};
    PFN_vkCreateImage cached_image_create{};PFN_vkDestroyImage cached_image_destroy{};
    HMODULE luke_fixture{};
    std::unordered_map<std::string,PFN_vkVoidFunction> cached_instance_commands;
    std::unordered_map<std::string,PFN_vkVoidFunction> cached_export_commands;
    template<class T>T instance_api(const char* name)const {
        return reinterpret_cast<T>(gipa(instance,name));
    }
    template<class T>T raw(const char* name)const {
        return reinterpret_cast<T>(unhooked_command(gdpa(native_device,name)));
    }
    template<class T>T observed(const char* name)const {
        if(bootstrap) {
            if(const auto saved=cached_export_commands.find(name);saved!=cached_export_commands.end())
                return reinterpret_cast<T>(saved->second);
            const auto found=cached_instance_commands.find(name);
            return reinterpret_cast<T>(found!=cached_instance_commands.end()?found->second:gipa(instance,name));
        }
        return reinterpret_cast<T>(resolve_command(native_device,name,reinterpret_cast<PFN_vkVoidFunction>(raw<T>(name))));
    }
    uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags properties)const {
        for(uint32_t i=0;i<device->memory.memoryTypeCount;++i)
            if((bits&(1u<<i))&&(device->memory.memoryTypes[i].propertyFlags&properties)==properties)return i;
        throw std::runtime_error("native transport memory type unavailable");
    }
    void setup() {
        pixel_size=rgba_motion?8U:4U;byte_count=width*height*pixel_size;
        if(bootstrap){
            require_transport(luke_vulkan_process(),"bootstrap executable must be one of the two selected games");
            luke_fixture=LoadLibraryW(L"luke-runtime-fixture.dll");
            require_transport(luke_fixture&&native_ingress::luke_detected(),"load actual Luke marker fixture");
        }
        loader=LoadLibraryW(L"vulkan-1.dll");if(!loader)throw TransportSkip("Vulkan loader unavailable");
        if(bootstrap) {
            cached_public_dispatch=reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(luke_fixture,"vkGetDeviceProcAddr"));
            const auto configure=reinterpret_cast<void(*)(PFN_vkGetDeviceProcAddr)>(GetProcAddress(luke_fixture,"TestLukeVulkanDispatcher"));
            public_counts=reinterpret_cast<decltype(public_counts)>(GetProcAddress(luke_fixture,"TestLukeVulkanCounts"));
            require_transport(cached_public_dispatch&&configure&&public_counts,"cache marked public GDPA before interception");
            configure(reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader,"vkGetDeviceProcAddr")));
        }
        if(bootstrap)for(const auto* name:{"vkCreateCommandPool","vkAllocateCommandBuffers","vkBeginCommandBuffer"}) {
            // REAL VR/engines can cache actual export pointers before OFXR is
            // loaded. GIPA/GDPA replacement cannot observe these invocations.
            cached_export_commands.emplace(name,reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(loader,name)));
            require_transport(cached_export_commands.at(name)!=nullptr,"cache loader export before hooks");
        }
        if(bootstrap&&capture_role==motion&&!rgba_motion)cache_public_family_creation(luke_fixture);
        if(bootstrap)poll_capture();
        gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        require_transport(gipa!=nullptr,"Vulkan instance dispatcher unavailable");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
        require_transport(reinterpret_cast<PFN_vkCreateInstance>(gipa({},"vkCreateInstance"))(&info,nullptr,&instance),"create native probe instance");
        gdpa=instance_api<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
        if(bootstrap)for(const auto* name:{"vkCreateCommandPool","vkAllocateCommandBuffers","vkBeginCommandBuffer",
            "vkEndCommandBuffer","vkResetCommandBuffer","vkDestroyCommandPool","vkQueueSubmit"}) {
            // Actual engines cache device command addresses through GIPA before
            // creating VkDevice. No direct call to resolve_command may hide gaps.
            cached_instance_commands.emplace(name,gipa(instance,name));
            require_transport(cached_instance_commands.at(name)!=nullptr,"cache GIPA device command");
        }
        if(bootstrap) {
            gdpa=hooks().gdpa; // exact downstream trampoline, not our command wrapper
            const auto configure=reinterpret_cast<void(*)(PFN_vkGetDeviceProcAddr)>(
                GetProcAddress(luke_fixture,"TestLukeVulkanDispatcher"));
            configure(gdpa);
            // Reproduce REAL VR's redirected generic resolver: both public
            // probes now discover only its wrapper, while private allocations
            // retain the actual lower entry cached before interception.
            hooks().gdpa=cached_public_dispatch;
        }
        uint32_t count{};auto enumerate=instance_api<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        require_transport(enumerate(instance,&count,nullptr),"enumerate native devices");
        std::vector<VkPhysicalDevice> physical_devices(count);require_transport(enumerate(instance,&count,physical_devices.data()),"read native devices");
        ComPtr<ID3D12Debug> debug_controller;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller))))debug_controller->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;require_transport(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"create DXGI factory");
        device=std::make_shared<Device>();
        for(auto candidate:physical_devices) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&id;
            instance_api<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(candidate,&properties);
            if(!id.deviceLUIDValid)continue;LUID luid{};std::memcpy(&luid,id.deviceLUID,sizeof(luid));ComPtr<IDXGIAdapter1> adapter;
            if(SUCCEEDED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)))&&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device->owner)))){physical=candidate;break;}
        }
        if(!physical)throw TransportSkip("matched Vulkan/D3D12 physical device unavailable");
        auto queues=instance_api<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        count=0;queues(physical,&count,nullptr);std::vector<VkQueueFamilyProperties> families(count);queues(physical,&count,families.data());
        while(family<count&&!(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;
        if(family==count)throw TransportSkip("native graphics queue unavailable");
        auto extensions=instance_api<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        count=0;require_transport(extensions(physical,nullptr,&count,nullptr),"enumerate interop extensions");
        std::vector<VkExtensionProperties> names(count);require_transport(extensions(physical,nullptr,&count,names.data()),"read interop extensions");
        const char* required[]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
        for(const auto* name:required)if(std::none_of(names.begin(),names.end(),[&](const auto& entry){return !std::strcmp(entry.extensionName,name);}))
            throw TransportSkip("native Win32 external memory/semaphore unavailable");
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&timeline;
        instance_api<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,&features);
        if(!timeline.timelineSemaphore)throw TransportSkip("native timeline semaphore unavailable");
        timeline.pNext=nullptr;float priority=1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
        VkPhysicalDeviceVulkan12Features requested12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 requested{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};requested.pNext=&requested12;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};create.pNext=&requested;create.queueCreateInfoCount=1;create.pQueueCreateInfos=&queue;
        InteropDeviceRequest interop;require_transport(interop.prepare(create,names,true),"prepare actual native interop features");
        require_transport(create.pNext==&requested&&requested.pNext==&requested12&&!requested12.timelineSemaphore&&create.enabledExtensionCount==0&&create.ppEnabledExtensionNames==nullptr,"interop request edited caller-owned feature chain");
        require_transport(instance_api<PFN_vkCreateDevice>("vkCreateDevice")(physical,&interop.info,nullptr,&native_device),"create native probe device with immutable interop request");
        // The regular hardware case has not installed any Vulkan loader hooks.
        // Cache the real device dispatcher/entry first, then instrument that
        // exact address. Calling our create_image helper would hide this gap.
        cached_allocation_dispatch=gdpa;
        cached_image_create=reinterpret_cast<PFN_vkCreateImage>(cached_allocation_dispatch(native_device,"vkCreateImage"));
        cached_image_destroy=reinterpret_cast<PFN_vkDestroyImage>(cached_allocation_dispatch(native_device,"vkDestroyImage"));
        require_transport(cached_image_create&&cached_image_destroy,"cache actual device allocation entries");
        // The loader's successful creation receipt is the production entry
        // point. Without it the redirected probes cover only the public
        // wrapper and miss the cached downstream source used below.
        ::OFXR_ObserveVulkanDeviceDispatchV1(native_device,cached_allocation_dispatch);
        if(bootstrap) {
            require_transport(bool(hooks().luke_gdpa_hook),"early marked public dispatcher was not intercepted");
            require_transport(cached_public_dispatch(native_device,"vkGetDeviceQueue")==cached_allocation_dispatch(native_device,"vkGetDeviceQueue"),
                "public allocation interception changed unrelated device dispatcher result");
        }
        raw<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(native_device,family,0,&native_queue);
        device->binding={instance,physical,native_device,family,0};device->gipa=gipa;device->gdpa=gdpa;
        instance_api<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical,&device->memory);
        D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        require_transport(device->owner->CreateCommandQueue(&description,IID_PPV_ARGS(&device->queue)),"create exact private capture queue");
        if(bootstrap){
            const auto native_owner=configure_luke_owner(device->owner.Get(),device->queue.Get());
            require_transport(native_owner==native_device,"early observed native device must bind independently of XR API");
            {std::scoped_lock lock(hooks().mutex);device=hooks().devices.at(native_owner);}
            ComPtr<ID3D12Device> queried;ComPtr<ID3D12CommandQueue> producer;
            require_transport(!query_luke_context(&queried,&producer),"creation must not publish an unsubmitted Luke producer");
        }
        (void)device->owner.As(&debug);if(debug)debug->ClearStoredMessages();
        require_transport(initialize_commands(instance,physical,native_device,gipa,gdpa),"initialize observed native command stream");
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pool_info.queueFamilyIndex=family;pool_info.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        require_transport(observed<PFN_vkCreateCommandPool>("vkCreateCommandPool")(native_device,&pool_info,nullptr,&pool),"create observed native pool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};allocate.commandPool=pool;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
        require_transport(observed<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(native_device,&allocate,&command),"allocate observed native command");
        wchar_t executable[MAX_PATH]{};GetModuleFileNameW(nullptr,executable,MAX_PATH);
        const auto* basename=wcsrchr(executable,L'\\');basename=basename?basename+1:executable;
        const bool signed_motion=bootstrap&&!_wcsicmp(basename,L"DOOMTheDarkAges.exe");
        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image_info.imageType=VK_IMAGE_TYPE_2D;
        image_info.format=capture_role==motion?(rgba_motion?VK_FORMAT_R16G16B16A16_SFLOAT:
            signed_motion?VK_FORMAT_R16G16_SNORM:VK_FORMAT_R16G16_SFLOAT):
            VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        // An exact one-mip/one-layer view of a larger allocation proves that
        // VK_REMAINING_* is resolved against this image, not assumed to be one.
        image_info.extent={width*2,height*2,1};image_info.mipLevels=image_info.arrayLayers=2;image_info.samples=VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling=VK_IMAGE_TILING_OPTIMAL;image_info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        if(capture_role==output)image_info.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
        const auto original_image_info=image_info;
        unsigned public_get{},public_create{},public_destroy{};
        if(public_counts) {
            unsigned initial_get{};public_counts(&initial_get,&initial_public_create,&initial_public_destroy);
            // Public and cached-native calls must preserve their own original
            // counts. The retained hardware motion source bypasses this public
            // path; a separate allocation still exercises synchronous nesting.
            const auto public_make=reinterpret_cast<PFN_vkCreateImage>(cached_public_dispatch(native_device,"vkCreateImage"));
            const auto public_retire=reinterpret_cast<PFN_vkDestroyImage>(cached_public_dispatch(native_device,"vkDestroyImage"));
            VkImage public_image{};
            require_transport(public_make(native_device,&image_info,nullptr,&public_image),"public hardware allocation forwarding");
            require_transport(hooks().image_usage.contains(public_image)&&
                hooks().image_usage.at(public_image).device==native_device,
                "public wrapper lost exact nested downstream owner");
            public_retire(native_device,public_image,nullptr);
            require_transport(!hooks().image_usage.contains(public_image),"public hardware allocation retirement");
            public_counts(&public_get,&public_create,&public_destroy);
            require_transport(public_create==initial_public_create+1&&public_destroy==initial_public_destroy+1,
                "public hardware originals were not forwarded exactly once");
        }
        const auto creation_before=hooks().image_creations;
        require_transport(cached_image_create(native_device,&image_info,nullptr,&image),"observe cached raw actual native source allocation");
        require_transport(std::memcmp(&image_info,&original_image_info,sizeof(image_info))==0&&
            hooks().image_usage.contains(image)&&hooks().image_usage.at(image).device==native_device&&
            hooks().image_usage.at(image).mips==2&&hooks().image_usage.at(image).layers==2,
            "cached native device entry was missed or caller allocation descriptor was edited");
        require_transport(hooks().device_image_dispatch.contains(native_device)&&
            hooks().device_image_dispatch.at(native_device).resolver==cached_allocation_dispatch&&
            hooks().device_image_dispatch.at(native_device).creation&&
            hooks().device_image_dispatch.at(native_device).destruction,
            "successful loader-chain receipt did not cover exact downstream allocation entries");
        require_transport(hooks().image_creations==creation_before+1,"public/lower dispatch duplicated successful allocation receipt");
        if(public_counts) {
            unsigned get{},created{},destroyed{};public_counts(&get,&created,&destroyed);
            require_transport(created==public_create&&destroyed==public_destroy&&get>=public_get,
                "cached downstream creation unexpectedly used the public wrapper");
        }
        VkMemoryRequirements requirements{};raw<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(native_device,image,&requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=requirements.size;memory.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require_transport(raw<PFN_vkAllocateMemory>("vkAllocateMemory")(native_device,&memory,nullptr,&image_memory),"allocate native source memory");
        require_transport(raw<PFN_vkBindImageMemory>("vkBindImageMemory")(native_device,image,image_memory,0),"bind native source image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=image_info.format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,1,VK_REMAINING_MIP_LEVELS,1,VK_REMAINING_ARRAY_LAYERS};
        require_transport(raw<PFN_vkCreateImageView>("vkCreateImageView")(native_device,&view,nullptr,&image_view),"create actual remaining-subresources view");
        upload=host_buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT);readback=host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        for(unsigned i=0;i<byte_count;++i)expected[i]=static_cast<unsigned char>((i*13u+i/7u+3u)&255u);
        if(rgba_motion)for(unsigned i=0;i<width*height;++i) {
            const std::uint16_t channels[]{static_cast<std::uint16_t>(0x3000+i%16),
                static_cast<std::uint16_t>(0xb400+i%16),0x57b0,0xd5b0};
            std::memcpy(expected.data()+i*pixel_size,channels,sizeof(channels));
        }
        void* mapped{};require_transport(raw<PFN_vkMapMemory>("vkMapMemory")(native_device,upload.memory,0,byte_count,0,&mapped),"map native upload");
        std::memcpy(mapped,expected.data(),byte_count);raw<PFN_vkUnmapMemory>("vkUnmapMemory")(native_device,upload.memory);
        captured=std::make_shared<Slot>();require_transport(initialize_slot(*device,*captured),"create production transport slot");device->slots.push_back(captured);
        auto source=NVSDK_NGX_Create_ImageView_Resource_VK(image_view,image,view.subresourceRange,view.format,width,height,false);
        const auto legacy_shape=[](const auto& resource){const auto& info=resource.Resource.ImageViewInfo;
            return resource.Type==NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW&&info.Image&&info.Width&&info.Height&&
                info.SubresourceRange.layerCount==1&&info.SubresourceRange.levelCount==1;};
        require_transport(!legacy_shape(source),"V500 literal-count resource refusal was not reproduced");
        const auto receipt=image_receipt(&source,native_device);
        require_transport(!receipt.rejection&&receipt.allocation.device==native_device&&
            receipt.resolved.Resource.ImageViewInfo.SubresourceRange.levelCount==1&&
            receipt.resolved.Resource.ImageViewInfo.SubresourceRange.layerCount==1&&
            source.Resource.ImageViewInfo.SubresourceRange.levelCount==VK_REMAINING_MIP_LEVELS&&
            source.Resource.ImageViewInfo.SubresourceRange.layerCount==VK_REMAINING_ARRAY_LAYERS,
            "actual observed remaining counts did not resolve to the exact single subresource without mutating NGX");
        auto rejected=source;rejected.Resource.ImageViewInfo.SubresourceRange.baseMipLevel=0;
        require_transport((image_receipt(&rejected,native_device).rejection&8192)!=0,"remaining multiple mip levels incorrectly admitted");
        rejected=source;rejected.Resource.ImageViewInfo.SubresourceRange.baseArrayLayer=0;
        require_transport((image_receipt(&rejected,native_device).rejection&16384)!=0,"remaining multiple array layers incorrectly admitted");
        rejected=source;rejected.Resource.ImageViewInfo.Width=width+1;
        require_transport((image_receipt(&rejected,native_device).rejection&32768)!=0,"view size exceeding actual selected mip admitted");
        rejected=source;rejected.Resource.ImageViewInfo.SubresourceRange.baseMipLevel=2;
        require_transport((image_receipt(&rejected,native_device).rejection&2048)!=0,"out-of-allocation mip base admitted");
        rejected=source;rejected.Resource.ImageViewInfo.SubresourceRange.baseArrayLayer=2;
        require_transport((image_receipt(&rejected,native_device).rejection&4096)!=0,"out-of-allocation layer base admitted");
        rejected=source;rejected.Resource.ImageViewInfo.Format=VK_FORMAT_R32_SFLOAT;
        require_transport((image_receipt(&rejected,native_device).rejection&65536)!=0,"unproven allocation/view format mismatch admitted");
        require_transport((image_receipt(&source,reinterpret_cast<VkDevice>(std::uintptr_t{1})).rejection&128)!=0,"foreign allocation device admitted");
        rejected=NVSDK_NGX_Create_Buffer_Resource_VK(upload.buffer,byte_count,false);
        require_transport(image_receipt(&rejected,native_device).rejection==2,"buffer was reinterpreted as an image descriptor");
        source.Resource.ImageViewInfo.Image={};source.Resource.ImageViewInfo.Width=width+7;
        require_transport(receipt.resolved.Resource.ImageViewInfo.Image==image&&receipt.resolved.Resource.ImageViewInfo.Width==width,
            "borrowed NGX descriptor mutation retargeted exact frozen resource receipt");
        require_transport(surface(*device,captured->surfaces[capture_role],receipt.resolved.Resource.ImageViewInfo,capture_role),"import exact remaining-view production shared D3D12 buffer");
        const auto transport_format=captured->surfaces[capture_role].texture->GetDesc().Format;
        if(capture_role==motion)require_transport(transport_format==(rgba_motion?DXGI_FORMAT_R16G16B16A16_FLOAT:
            signed_motion?DXGI_FORMAT_R16G16_SNORM:DXGI_FORMAT_R16G16_FLOAT)&&
            luke_motion_srv_format(transport_format)==transport_format&&pixel_bytes(transport_format)==pixel_size,
            "native Vulkan motion transport lost its exact typed consumer contract");
        else require_transport(transport_format==DXGI_FORMAT_R11G11B10_FLOAT&&pixel_bytes(transport_format)==4&&
            format(VK_FORMAT_B10G11R11_UFLOAT_PACK32,color)==transport_format&&
            format(VK_FORMAT_B10G11R11_UFLOAT_PACK32,output)==transport_format,
            "native Vulkan packed RGB color/output lost its exact four-byte consumer contract");
        require_transport(format(VK_FORMAT_R16G16_UNORM,motion)==DXGI_FORMAT_UNKNOWN&&
            luke_motion_srv_format(DXGI_FORMAT_R16G16_UNORM)==DXGI_FORMAT_UNKNOWN&&
            luke_motion_srv_format(DXGI_FORMAT_R16G16_SINT)==DXGI_FORMAT_UNKNOWN&&
            luke_motion_srv_format(DXGI_FORMAT_R16G16_TYPELESS)==DXGI_FORMAT_UNKNOWN,
            "unsupported motion storage acquired a typed float contract");
        const auto& footprint=captured->surfaces[capture_role].footprint;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=footprint.Footprint.RowPitch*height;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        require_transport(device->owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&d12_readback)),"create D3D12 typed readback");
        require_transport(device->owner->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&progress)),"create private progress fence");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};require_transport(raw<PFN_vkCreateFence>("vkCreateFence")(native_device,&fence,nullptr,&finished),"create exact native suffix fence");
    }
    HostBuffer host_buffer(VkBufferUsageFlags usage) {
        HostBuffer result;VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};create.size=byte_count;create.usage=usage;
        require_transport(raw<PFN_vkCreateBuffer>("vkCreateBuffer")(native_device,&create,nullptr,&result.buffer),"create host transfer buffer");
        VkMemoryRequirements requirements{};raw<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(native_device,result.buffer,&requirements);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=requirements.size;
        memory.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        require_transport(raw<PFN_vkAllocateMemory>("vkAllocateMemory")(native_device,&memory,nullptr,&result.memory),"allocate host transfer memory");
        require_transport(raw<PFN_vkBindBufferMemory>("vkBindBufferMemory")(native_device,result.buffer,result.memory,0),"bind host transfer buffer");return result;
    }
    void image_barrier(VkImageLayout from,VkImageLayout to,VkAccessFlags source,VkAccessFlags destination,VkCommandBuffer received={}) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;barrier.subresourceRange=captured->surfaces[capture_role].source.SubresourceRange;
        barrier.oldLayout=from;barrier.newLayout=to;barrier.srcAccessMask=source;barrier.dstAccessMask=destination;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        observed<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(received?received:command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }
    void wait_private(uint64_t value) {
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require_transport(event!=nullptr,"create private progress event");
        const auto result=progress->SetEventOnCompletion(value,event);const auto waited=SUCCEEDED(result)?WaitForSingleObject(event,2000):WAIT_FAILED;CloseHandle(event);
        require_transport(result,"arm private progress event");require_transport(waited==WAIT_OBJECT_0,"unsubmitted native prefix blocked private queue");
    }
    void verify() {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        require_transport(observed<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&begin),"begin observed native recording");
        require_transport(can_split(command),"GIPA-cached original recording was not observed before SR");
        VkCommandBuffer other{};VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool=pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
        require_transport(observed<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(native_device,&allocation,&other),
            "allocate independent metadata command");
        // Exercise a current suffix whose raw handle is not the registered
        // original. No packet is submitted; reset proves cancellation before
        // the existing hardware round-trip starts its own exact fenced work.
        CommandHandoff metadata_packet{captured->timeline,1,2,3};
        require_transport(split(command,metadata_packet).result,"split exact original before native metadata dispatch");
        require_transport(native(command)!=command,"metadata suffix counterfactual missing");
        verify_metadata_dispatch(native_device,command,other);
        if(capture_role==motion&&!rgba_motion)verify_public_family_dispatch(native_device,command,other);
        require_transport(observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command,0),"cancel metadata-only recording");
        require_transport(observed<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&begin),"begin real transport after metadata cancellation");
        image_barrier(VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,1,1,1};region.imageExtent={width,height,1};
        raw<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(native(command),upload.buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
        const auto image_layout=capture_role==output?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const auto image_access=capture_role==output?VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_SHADER_READ_BIT;
        image_barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,image_layout,VK_ACCESS_TRANSFER_WRITE_BIT,image_access);
        require_transport(queue_family_for(command)==family,"transport must use observed native command-pool family");
        require_transport(retain_transport_recording(device,captured,command),"pin exact recording before native copy commands");
        auto& transport=captured->surfaces[capture_role];require_transport(copy_native(*device,transport,native(command),false,capture_role,family),"record production native freeze");
        // The first actual handoff supplies the raw suffix received by a
        // generic wrapper. A second inner handoff closes that received handle.
        // Its post-evaluate vkCmd calls must follow this exact live chain.
        require_transport(begin_d12(*captured,0),"begin first actual Vulkan transport split");
        copy_d12(captured->lists[0].Get(),transport,false);copy_d12(captured->lists[0].Get(),transport,true);
        Copyback copy{device.get(),&transport,capture_role,family};
        const auto first_receipt=handoff(device,captured,0,0,command,copy,false);
        require_transport(first_receipt.result,"first exact native handoff before wrapper receives suffix");
        const auto wrapper_received=native(command);
        require_transport(copy_native(*device,transport,native(wrapper_received),false,capture_role,family),"second exact native freeze");
        const unsigned phase=1;
        require_transport(begin_d12(*captured,phase),"begin exact D3D12 transport work");auto* list=captured->lists[phase].Get();copy_d12(list,transport,false);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=transport.texture.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=d12_readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;destination.PlacedFootprint=transport.footprint;
        d12_barrier(list,transport.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        d12_barrier(list,transport.texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);copy_d12(list,transport,true);
        const auto receipt=handoff(device,captured,phase,0,wrapper_received,copy,bootstrap);
        require_transport(receipt.result,"split with exact deferred production work");
        require_transport(native(wrapper_received)==native(command)&&native(wrapper_received)!=wrapper_received&&
            device_for(wrapper_received)==native_device&&queue_family_for(wrapper_received)==family,
            "nested typed split lost its outer wrapper continuation alias");
        require_transport(captured->pending_callbacks==2&&captured->d12_value==0&&!captured->submitted_phases,"recording prematurely submitted D3D12 work");
        if(bootstrap){ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
            require_transport(!query_luke_context(&owner,&queue),"recording must not publish an unsubmitted producer");}
        require_transport(device->queue->Signal(progress.Get(),1),"signal private queue before Vulkan submission");wait_private(1);
        image_barrier(image_layout,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image_access,VK_ACCESS_TRANSFER_READ_BIT,wrapper_received);
        observed<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(wrapper_received,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&region);
        image_barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,image_layout,VK_ACCESS_TRANSFER_READ_BIT,image_access,wrapper_received);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.buffer=readback.buffer;host.size=VK_WHOLE_SIZE;
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        observed<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(wrapper_received,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
        require_transport(observed<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(command),"end observed native continuation");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        require_transport(observed<PFN_vkQueueSubmit>("vkQueueSubmit")(native_queue,1,&submit,finished),"submit exact native production packet");
        require_transport(raw<PFN_vkWaitForFences>("vkWaitForFences")(native_device,1,&finished,VK_TRUE,5000000000ULL),"wait exact native suffix completion");
        require_transport(device->queue->Signal(progress.Get(),2),"signal exact D3D12 work completion");wait_private(2);
        require_transport(captured->fence->GetCompletedValue()>=captured->final_value&&captured->d12_done->GetCompletedValue()>=captured->d12_value&&receipt.lifetime->disposition==RecordingDisposition::submitted,"both exact transport fences must retire");
        require_transport(captured->complete()&&captured->recording_leases==1,"retired submitted slot remained pinned by executable command buffer");
        require_transport(observed<PFN_vkQueueSubmit>("vkQueueSubmit")(native_queue,1,&submit,VK_NULL_HANDLE)==VK_ERROR_FEATURE_NOT_PRESENT,
            "submitted split recording replay could overwrite a recycled transport slot");
        void* mapped{};require_transport(raw<PFN_vkMapMemory>("vkMapMemory")(native_device,readback.memory,0,byte_count,0,&mapped),"map native round-trip");
        require_transport(!std::memcmp(mapped,expected.data(),byte_count),"native round-trip changed exact typed motion/color/output bits");raw<PFN_vkUnmapMemory>("vkUnmapMemory")(native_device,readback.memory);
        D3D12_RANGE range{0,transport.footprint.Footprint.RowPitch*height};require_transport(d12_readback->Map(0,&range,&mapped),"map exact D3D12 typed capture");
        for(unsigned row=0;row<height;++row)require_transport(!std::memcmp(static_cast<unsigned char*>(mapped)+row*transport.footprint.Footprint.RowPitch,expected.data()+row*width*pixel_size,width*pixel_size),"D3D12 texture never received exact native motion/color/output bits");
        D3D12_RANGE no_write{};d12_readback->Unmap(0,&no_write);
        require_transport(observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command,0),"reset completed native recording");
        require_transport(captured->complete()&&!captured->recording_leases,"source recording disposal must release exact transport lease");
        if(bootstrap){ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
            require_transport(query_luke_context(&owner,&queue)&&owner==device->owner&&queue==device->queue,
                "exact accepted native prefix and D12 transport must publish their actual owner");
            device->production_submitted=false;}
        require_transport(observed<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(command,&begin),"begin native recording to abandon");
        captured->submitted_phases=0;for(unsigned role=0;role<role_count;++role)captured->native_before[role]=captured->surfaces[role].native_used;
        require_transport(retain_transport_recording(device,captured,command),"pin exact abandoned recording before native copy commands");
        require_transport(copy_native(*device,transport,native(command),false,capture_role,family),"record native freeze to abandon");
        require_transport(begin_d12(*captured,phase),"record D3D12 work to abandon");copy_d12(captured->lists[phase].Get(),transport,false);copy_d12(captured->lists[phase].Get(),transport,true);
        const auto d12_before=captured->d12_value;const auto abandoned=handoff(device,captured,phase,0,command,copy,bootstrap);require_transport(abandoned.result,"split exact abandoned native recording");
        require_transport(observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(command,0),"abandon exact native recording");
        require_transport(abandoned.lifetime->disposition==RecordingDisposition::abandoned&&!captured->pending_callbacks&&captured->d12_value==d12_before&&captured->complete(),"abandonment must retire without GPU work");
        if(bootstrap){ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
            require_transport(!query_luke_context(&owner,&queue),"abandoned recording must not publish a producer");}
        require_transport(device->queue->Signal(progress.Get(),3),"signal private queue after abandonment");wait_private(3);
        if(debug)for(UINT64 index=0;index<debug->GetNumStoredMessages();++index) {
            SIZE_T bytes{};debug->GetMessage(index,nullptr,&bytes);std::vector<unsigned char> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(debug->GetMessage(index,message,&bytes))&&(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION))
                throw std::runtime_error(message->pDescription);
        }
    }
    void cleanup() {
        // Called only after actual native suffix + private D3D12 fence proofs.
        observed<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(native_device,pool,nullptr);pool={};destroy_commands(native_device);
        for(auto& target:captured->surfaces)destroy_surface(*device,target);
        raw<PFN_vkDestroySemaphore>("vkDestroySemaphore")(native_device,captured->timeline,nullptr);captured->timeline={};device->slots.clear();captured.reset();
        for(auto* buffer:{&upload,&readback}){raw<PFN_vkDestroyBuffer>("vkDestroyBuffer")(native_device,buffer->buffer,nullptr);raw<PFN_vkFreeMemory>("vkFreeMemory")(native_device,buffer->memory,nullptr);}
        raw<PFN_vkDestroyImageView>("vkDestroyImageView")(native_device,image_view,nullptr);
        cached_image_destroy(native_device,image,nullptr);
        require_transport(!hooks().image_usage.contains(image),"cached actual native destroy did not retire hardware allocation");
        if(public_counts){unsigned get{},created{},destroyed{};public_counts(&get,&created,&destroyed);
            require_transport(created==initial_public_create+1&&destroyed==initial_public_destroy+1,"public create/destroy exact callback counts changed");}
        raw<PFN_vkFreeMemory>("vkFreeMemory")(native_device,image_memory,nullptr);
        raw<PFN_vkDestroyFence>("vkDestroyFence")(native_device,finished,nullptr);
        if(bootstrap)observed<PFN_vkDestroyDevice>("vkDestroyDevice")(native_device,nullptr);
        else raw<PFN_vkDestroyDevice>("vkDestroyDevice")(native_device,nullptr);
        if(bootstrap){hooks().gdpa=cached_allocation_dispatch;stop_owner(native_device);
            require_transport(!hooks().observed_devices.contains(native_device)&&!hooks().interop_devices.contains(native_device),
                "observed native device destruction must retire its exact bootstrap receipts");
            ComPtr<ID3D12Device> owner;ComPtr<ID3D12CommandQueue> queue;
            require_transport(!query_luke_context(&owner,&queue),"retired Vulkan owner must not survive XR recreation");}
        instance_api<PFN_vkDestroyInstance>("vkDestroyInstance")(instance,nullptr);device.reset();FreeLibrary(loader);
        if(luke_fixture)FreeLibrary(luke_fixture);
    }
};
// Actual public/native call chain on the same hardware transport. Inputs are
// overwritten in place by the wrapper before the typed call; its private SR
// output is then overwritten/copy-backed after that call has returned.
struct PublicGuideGpuProbe {
    static constexpr unsigned ow=512,oh=32;
    struct Image {VkImage image{};VkImageView view{};VkDeviceMemory memory{};
        NVSDK_NGX_Resource_VK resource{};VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};};
    TransportProbe& hardware;std::array<Image,5> images;VkFormat depth_format;
    OriginalParameters parameters{nullptr,{}};
    HostBuffer readback;HMODULE streamline{};
    decltype(&sl1::slSetConstants) set_constants{};decltype(&sl1::slEvaluateFeature) evaluate_sl{};
    Evaluate public_evaluate{};std::shared_ptr<Slot> prepared;
    alignas(16) std::array<unsigned char,16> public_storage{},native_storage{};
    const NVSDK_NGX_Handle* public_handle{reinterpret_cast<const NVSDK_NGX_Handle*>(public_storage.data())};
    const NVSDK_NGX_Handle* native_handle{reinterpret_cast<const NVSDK_NGX_Handle*>(native_storage.data())};
    bool no_child{},reset_before_child{};unsigned native_calls{},public_calls{};std::uint64_t before_publication{};
    Evaluate saved_native{};bool saved_typed{};
    inline static PublicGuideGpuProbe* current{};
    explicit PublicGuideGpuProbe(TransportProbe& probe,VkFormat format=VK_FORMAT_D32_SFLOAT):hardware(probe),depth_format(format){}
    void barrier(Image& image,VkCommandBuffer command,VkImageLayout layout) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image.image;
        b.subresourceRange=image.resource.Resource.ImageViewInfo.SubresourceRange;
        b.subresourceRange.aspectMask=transport_aspects(image.resource.Resource.ImageViewInfo.Format,b.subresourceRange.aspectMask,depth,false);
        b.oldLayout=image.layout;b.newLayout=layout;b.srcAccessMask=image.layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        hardware.raw<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(native(command),VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);image.layout=layout;
    }
    void clear(unsigned role,VkCommandBuffer command,std::array<float,4> values) {
        auto& image=images[role];const auto final=role==output||role==4?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier(image,command,VK_IMAGE_LAYOUT_GENERAL);VkClearColorValue value{};std::copy(values.begin(),values.end(),value.float32);
        auto range=image.resource.Resource.ImageViewInfo.SubresourceRange;
        if(image.resource.Resource.ImageViewInfo.Format==VK_FORMAT_D32_SFLOAT||image.resource.Resource.ImageViewInfo.Format==VK_FORMAT_D24_UNORM_S8_UINT) {
            range.aspectMask=transport_aspects(image.resource.Resource.ImageViewInfo.Format,range.aspectMask,depth,false);
            VkClearDepthStencilValue depth_value{values[0],0xa7};
            hardware.raw<PFN_vkCmdClearDepthStencilImage>("vkCmdClearDepthStencilImage")(native(command),image.image,VK_IMAGE_LAYOUT_GENERAL,&depth_value,1,&range);
        } else hardware.raw<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(native(command),image.image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
        if(final!=VK_IMAGE_LAYOUT_GENERAL)barrier(image,command,final);
    }
    void make(unsigned role,VkFormat format,unsigned width,unsigned height) {
        auto& image=images[role];VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;
        info.format=format;info.extent={width,height,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
        info.tiling=VK_IMAGE_TILING_OPTIMAL;info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
        if(format==VK_FORMAT_D32_SFLOAT||format==VK_FORMAT_D24_UNORM_S8_UINT)info.usage&=~VK_IMAGE_USAGE_STORAGE_BIT;
        require_transport(hardware.cached_image_create(hardware.native_device,&info,nullptr,&image.image),"public guide GPU image allocation");
        VkMemoryRequirements need{};hardware.raw<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(hardware.native_device,image.image,&need);
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};memory.allocationSize=need.size;
        memory.memoryTypeIndex=hardware.memory_type(need.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require_transport(hardware.raw<PFN_vkAllocateMemory>("vkAllocateMemory")(hardware.native_device,&memory,nullptr,&image.memory),"public guide GPU image memory");
        require_transport(hardware.raw<PFN_vkBindImageMemory>("vkBindImageMemory")(hardware.native_device,image.image,image.memory,0),"public guide GPU image binding");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image.image;view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=format;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if(format==VK_FORMAT_D32_SFLOAT||format==VK_FORMAT_D24_UNORM_S8_UINT)view.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        require_transport(hardware.raw<PFN_vkCreateImageView>("vkCreateImageView")(hardware.native_device,&view,nullptr,&image.view),"public guide GPU image view");
        image.resource=NVSDK_NGX_Create_ImageView_Resource_VK(image.view,image.image,view.subresourceRange,format,width,height,role>=output);
        // Exact DOOM receipt: valid D32 depth view, but NGX's range says COLOR.
        if(format==VK_FORMAT_D32_SFLOAT)image.resource.Resource.ImageViewInfo.SubresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
    }
    static NVSDK_NGX_Result create_original(VkCommandBuffer,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle** out) {
        if(!out)return NVSDK_NGX_Result_FAIL_InvalidParameter;*out=const_cast<NVSDK_NGX_Handle*>(current->public_handle);return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result create1_original(VkDevice,VkCommandBuffer command,NVSDK_NGX_Feature type,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** out) {
        return create_original(command,type,p,out);
    }
    static NVSDK_NGX_Result release_original(NVSDK_NGX_Handle*){return NVSDK_NGX_Result_Success;}
    static NVSDK_NGX_Result native_original(VkCommandBuffer command,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* p,void*) {
        auto& self=*current;++self.native_calls;
        require_transport(get<float>(p,NVSDK_NGX_Parameter_MV_Scale_X)==-9&&get<int>(p,NVSDK_NGX_Parameter_Reset)==1,
            "public trial changed the wrapper's original native scalar arguments");
        void* output_resource{};require_transport(NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_Output,&output_resource))&&
            static_cast<const NVSDK_NGX_Resource_VK*>(output_resource)->Resource.ImageViewInfo.Image==self.images[4].image,
            "public trial changed the wrapper's private native output");
        self.clear(4,command,{37.F/255.F,73.F/255.F,113.F/255.F,1});return NVSDK_NGX_Result_Success;
    }
    static NVSDK_NGX_Result wrapper_original(VkCommandBuffer command,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter* p,void* callback) {
        auto& self=*current;++self.public_calls;self.prepared.reset();
        for(const auto& slot:self.hardware.device->slots)if(slot->recording_leases&&slot->surfaces[motion].source.Image==self.images[motion].image)self.prepared=slot;
        require_transport(self.prepared!=nullptr,"public wrapper did not receive its actual pre-recorded guide slot");
        self.clear(motion,command,{7,9,0,0});self.clear(depth,command,{.125F,0,0,0});
        require_transport(sample_late_public_depth(*self.hardware.device,*self.prepared,command,
            queue_family_for(command)),"independent late depth witness recording");
        for(auto& value:self.parameters.floats){if(!strcmp(value.name,NVSDK_NGX_Parameter_MV_Scale_X))value.value=-9;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_Jitter_Offset_X))value.value=5;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec))value.value=99;}
        for(auto& value:self.parameters.ints)if(!strcmp(value.name,NVSDK_NGX_Parameter_Reset))value.value=1;
        self.parameters.images[output].resolved=self.images[4].resource;
        if(self.no_child) {
            require_transport(sample_native_public_color(*self.hardware.device,*self.prepared,command,p,
                queue_family_for(command)),"abandoned native color witness recording");
            return NVSDK_NGX_Result_Success;
        }
        if(self.reset_before_child){require_transport(self.hardware.observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(self.hardware.command,0),
                "discard original public preparation before actual native child");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            require_transport(self.hardware.observed<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(self.hardware.command,&begin),
                "reuse exact command address for fresh native epoch");}
        const auto result=evaluate<1>(command,self.native_handle,p,callback);
        if(!self.reset_before_child)require_transport(sample_native_public_color(*self.hardware.device,*self.prepared,command,p,
            queue_family_for(command)),"actual native output witness before wrapper final copyback");
        require_transport(dlss_motion_vector_publications()==self.before_publication,
            "public guides/output became visible before wrapper final copyback");
        // Integer-byte centers avoid half-way UNORM rounding assumptions.
        self.clear(4,command,{40.F/255.F,51.F/255.F,63.F/255.F,1});self.barrier(self.images[4],command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        self.barrier(self.images[output],command,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.extent={ow,oh,1};
        hardware_copy(self,command,region);
        self.barrier(self.images[4],command,VK_IMAGE_LAYOUT_GENERAL);self.barrier(self.images[output],command,VK_IMAGE_LAYOUT_GENERAL);
        return result;
    }
    static void hardware_copy(PublicGuideGpuProbe& self,VkCommandBuffer command,const VkImageCopy& region) {
        self.hardware.observed<PFN_vkCmdCopyImage>("vkCmdCopyImage")(command,self.images[4].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            self.images[output].image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&region);
    }
    static void sl_observer(void* command,uint32_t,uint32_t,uint32_t,const void*) {
        auto& self=*current;require_transport(NVSDK_NGX_SUCCEED(self.public_evaluate(static_cast<VkCommandBuffer>(command),
            self.public_handle,&self.parameters,nullptr)),"actual public guide GPU callback result");
    }
    void setup() {
        current=this;make(color,VK_FORMAT_R8G8B8A8_UNORM,16,8);make(motion,VK_FORMAT_R16G16_SFLOAT,16,8);
        require_transport(transport_aspects(VK_FORMAT_R32_SFLOAT,VK_IMAGE_ASPECT_COLOR_BIT,depth,true)==VK_IMAGE_ASPECT_COLOR_BIT&&
            transport_aspects(VK_FORMAT_D16_UNORM,VK_IMAGE_ASPECT_COLOR_BIT,depth,true)==VK_IMAGE_ASPECT_DEPTH_BIT&&
            transport_aspects(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_COLOR_BIT,depth,false)==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)&&
            transport_aspects(VK_FORMAT_D32_SFLOAT_S8_UINT,VK_IMAGE_ASPECT_COLOR_BIT,depth,true)==VK_IMAGE_ASPECT_DEPTH_BIT,
            "depth transport must preserve color depth and isolate packed depth/stencil correctly");
        make(depth,depth_format,16,8);make(output,VK_FORMAT_R8G8B8A8_UNORM,ow,oh);make(4,VK_FORMAT_R8G8B8A8_UNORM,ow,oh);
        const auto old_bytes=hardware.byte_count;hardware.byte_count=ow*oh*4;readback=hardware.host_buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT);hardware.byte_count=old_bytes;
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);auto root=std::filesystem::path(executable).parent_path();
        for(unsigned i=0;i<7&&!streamline;++i,root=root.parent_path())for(const auto* configuration:{L"Release",L""}) {
            const auto path=root/L"streamline-fixtures"/L"legacy"/configuration/L"sl.interposer.dll";
            if(std::filesystem::is_regular_file(path))streamline=LoadLibraryW(path.c_str());if(streamline)break;}
        require_transport(streamline!=nullptr,"build legacy Streamline fixture for public guide GPU transport");
        const auto observer=reinterpret_cast<void(*)(decltype(&sl_observer))>(GetProcAddress(streamline,"TestObserver"));
        set_constants=reinterpret_cast<decltype(set_constants)>(GetProcAddress(streamline,"slSetConstants"));
        evaluate_sl=reinterpret_cast<decltype(evaluate_sl)>(GetProcAddress(streamline,"slEvaluateFeature"));
        require_transport(observer&&set_constants&&evaluate_sl,"public guide GPU declared SL entrypoints");observer(&sl_observer);native_ingress::poll_streamline();
        const auto configure=reinterpret_cast<void(*)(Create,Create1,Evaluate,Release)>(GetProcAddress(hardware.luke_fixture,"TestLukeVulkanNgxCallbacks"));
        require_transport(configure!=nullptr,"actual marked public guide fixture configuration");
        configure(&create_original,&create1_original,&wrapper_original,&release_original);
        public_evaluate=reinterpret_cast<Evaluate>(GetProcAddress(hardware.luke_fixture,"NVSDK_NGX_VULKAN_EvaluateFeature"));
        saved_native=hooks().modules[1].evaluate;saved_typed=hooks().modules[1].typed_implementation;
        hooks().modules[1].typed_implementation=true;hooks().modules[1].evaluate=&native_original;
        Feature f{};f.identity=native_ingress::allocate_handle_key();f.creation_source_identity=f.identity;f.creation_observed=true;f.flags_known=true;
        f.device=hardware.native_device;f.type=NVSDK_NGX_Feature_RayReconstruction;f.flags=NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        f.width=16;f.height=8;f.out_width=ow;f.out_height=oh;hooks().features[native_handle]=f;
        configure_dlss_motion_vector_tracking(true);configure_luke_color_origin_extent(ow,oh);
    }
    void reset_parameters() {
        for(auto& value:parameters.uints){value.result=NVSDK_NGX_Result_Success;value.value=0;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_Width)||!strcmp(value.name,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width))value.value=16;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_Height)||!strcmp(value.name,NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height))value.value=8;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_OutWidth))value.value=ow;if(!strcmp(value.name,NVSDK_NGX_Parameter_OutHeight))value.value=oh;}
        for(auto& value:parameters.floats){value.result=NVSDK_NGX_Result_Success;value.value=1;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_Jitter_Offset_X))value.value=.125F;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_Jitter_Offset_Y))value.value=-.25F;
            if(!strcmp(value.name,NVSDK_NGX_Parameter_FrameTimeDeltaInMsec))value.value=17;}
        for(auto& value:parameters.ints){value.result=NVSDK_NGX_Result_Success;value.value=strcmp(value.name,NVSDK_NGX_Parameter_Reset)?NVSDK_NGX_DLSS_Feature_Flags_MVLowRes:0;}
        for(unsigned role=0;role<role_count;++role)parameters.images[role]=image_receipt(&images[role].resource,hardware.native_device);
    }
    void verify_snapshots(const std::shared_ptr<const DlssMotionVectorFrame>& frame,unsigned serial) {
        require_transport(frame && frame->raw_motion_native_span==1 && frame->raw_motion_native_span_confirmed,
            "frozen public N-1 motion lost its observed interval before publication");
        require_transport(frame&&frame->native_production_sequence==serial&&frame->camera_frame_token==serial&&
            frame->scale_x==1&&frame->jitter_x==.125F&&frame->jitter_y==-.25F&&frame->frame_time_delta_ms==17&&!frame->reset&&
            frame->camera_position[0]==float(serial),"public guides lost original scalars/camera/actual native clock");
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        require_transport(hardware.device->owner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"original guide GPU readback allocator");
        require_transport(hardware.device->owner->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"original guide GPU readback list");
        copy_d12(list.Get(),prepared->diagnostic_late_depth,false);
        copy_d12(list.Get(),prepared->diagnostic_native_color,false);
        std::array<ComPtr<ID3D12Resource>,4> reads;std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,4> footprints{};std::array<UINT64,4> sizes{};
        ID3D12Resource* sources[]{frame->motion_vectors.Get(),frame->depth.Get(),prepared->diagnostic_late_depth.texture.Get(),prepared->diagnostic_native_color.texture.Get()};
        for(unsigned i=0;i<4;++i){const auto desc=sources[i]->GetDesc();hardware.device->owner->GetCopyableFootprints(&desc,0,1,0,&footprints[i],nullptr,nullptr,&sizes[i]);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width=sizes[i];buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            require_transport(hardware.device->owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&reads[i])),"original guide GPU readback");
            const auto original_state=i>=2?D3D12_RESOURCE_STATE_COMMON:i==1?frame->depth_resource_state:frame->resource_state;
            d12_barrier(list.Get(),sources[i],original_state,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=sources[i];from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=reads[i].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprints[i];list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            d12_barrier(list.Get(),sources[i],D3D12_RESOURCE_STATE_COPY_SOURCE,original_state);}
        require_transport(list->Close(),"original guide GPU readback close");ID3D12CommandList* submitted[]{list.Get()};hardware.device->queue->ExecuteCommandLists(1,submitted);
        const auto completion=++hardware.public_readback_value;
        require_transport(hardware.device->queue->Signal(hardware.progress.Get(),completion),"original guide GPU readback completion");hardware.wait_private(completion);
        const std::array<std::uint16_t,2> expected_motion{0x3400,0xb800};const float expected_depth=.75F,expected_late=.125F;
        for(unsigned i=0;i<3;++i){void* bytes{};const D3D12_RANGE range{0,SIZE_T(sizes[i])};require_transport(reads[i]->Map(0,&range,&bytes),"original guide GPU map");
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x) {
                const auto* pixel=static_cast<char*>(bytes)+footprints[i].Offset+y*footprints[i].Footprint.RowPitch+x*4;
                if(i&&depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
                    float value{};std::memcpy(&value,pixel,4);
                    require_transport(std::isfinite(value)&&std::abs(value-(i==2?expected_late:expected_depth))<=1.F/16777215.F,
                        "D24 normalized guide includes stencil/undefined bits or lost depth precision");
                } else require_transport(!std::memcmp(pixel,i==2?static_cast<const void*>(&expected_late):
                    i?static_cast<const void*>(&expected_depth):expected_motion.data(),4),
                    "protected early guides or independent late depth witness changed");
            }D3D12_RANGE no_write{};reads[i]->Unmap(0,&no_write);}
        void* native_bytes{};D3D12_RANGE native_range{0,SIZE_T(sizes[3])};
        require_transport(reads[3]->Map(0,&native_range,&native_bytes),"native color witness GPU map");
        const std::array<unsigned char,4> native_color{37,73,113,255};
        for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x)require_transport(!std::memcmp(
            static_cast<char*>(native_bytes)+footprints[3].Offset+y*footprints[3].Footprint.RowPitch+x*4,native_color.data(),4),
            "native color witness captured wrapper final color instead of actual private native output");
        D3D12_RANGE native_no_write{};reads[3]->Unmap(0,&native_no_write);
    }
    void verify_d24_words() {
        // Exercise every depth bit, extremes and adversarial upper bytes on
        // the actual production decoder. Vulkan may zero its undefined X8.
        Surface decoded;
        require_transport(surface(*hardware.device,decoded,images[depth].resource.Resource.ImageViewInfo,depth),"D24 word decoder surface");
        ComPtr<ID3D12Resource> upload,read;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=decoded.footprint.Footprint.RowPitch*8;buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        require_transport(hardware.device->owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"D24 word upload");
        heap.Type=D3D12_HEAP_TYPE_READBACK;
        require_transport(hardware.device->owner->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)),"D24 word readback");
        const auto word=[](unsigned pixel) {
            const uint32_t low=pixel%32<24?(1u<<(pixel%32)):pixel%32==24?0u:pixel%32==25?0xffffffu:((pixel*123457u)&0xffffffu);
            return low|((pixel*73u&255u)<<24);
        };
        void* bytes{};D3D12_RANGE no_read{};
        require_transport(upload->Map(0,&no_read,&bytes),"D24 word upload map");std::memset(bytes,0,size_t(buffer.Width));
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x) {
            const auto bits=word(y*16+x);std::memcpy(static_cast<char*>(bytes)+y*decoded.footprint.Footprint.RowPitch+x*4,&bits,4);
        }
        upload->Unmap(0,nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        require_transport(hardware.device->owner->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"D24 word allocator");
        require_transport(hardware.device->owner->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"D24 word list");
        d12_barrier(list.Get(),decoded.buffer.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(decoded.buffer.Get(),0,upload.Get(),0,buffer.Width);
        d12_barrier(list.Get(),decoded.buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        copy_d12(list.Get(),decoded,false);
        d12_barrier(list.Get(),decoded.texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=decoded.texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=decoded.footprint;to.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R32_FLOAT;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        require_transport(list->Close(),"D24 word close");ID3D12CommandList* lists[]{list.Get()};hardware.device->queue->ExecuteCommandLists(1,lists);
        const auto completion=++hardware.public_readback_value;
        require_transport(hardware.device->queue->Signal(hardware.progress.Get(),completion),"D24 word completion");hardware.wait_private(completion);
        D3D12_RANGE range{0,size_t(buffer.Width)};require_transport(read->Map(0,&range,&bytes),"D24 word map");
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<16;++x) {
            float actual{};std::memcpy(&actual,static_cast<char*>(bytes)+y*decoded.footprint.Footprint.RowPitch+x*4,4);
            const double expected=double(word(y*16+x)&0xffffffu)/16777215.0;
            if(!std::isfinite(actual)||std::abs(double(actual)-expected)>1.0/16777215.0)
                std::cerr<<"D24 decode pixel="<<x<<','<<y<<" word="<<word(y*16+x)<<" actual="<<actual<<" expected="<<expected<<'\n';
            require_transport(std::isfinite(actual)&&std::abs(double(actual)-expected)<=1.0/16777215.0,"D24 decoder lost depth bits or included undefined upper byte");
        }
        D3D12_RANGE no_write{};read->Unmap(0,&no_write);destroy_surface(*hardware.device,decoded);
        std::cout<<"D24 production GPU decoder:128 words, every depth bit, zero/one, changing undefined upper bytes PASS\n";
    }
    void run(unsigned serial,bool abandoned=false,bool reset_child=false) {
        const auto copyback_pixels_before=tested_native_output_copyback_pixels;
        no_child=abandoned;reset_before_child=reset_child;reset_parameters();VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        std::array<VkImageLayout,5> original_layouts{};for(unsigned i=0;i<images.size();++i)original_layouts[i]=images[i].layout;
        require_transport(hardware.observed<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(hardware.command,&begin),"begin original public GPU recording");
        clear(color,hardware.command,{.2F,.2F,.2F,1});clear(motion,hardware.command,{.25F,-.5F,0,0});clear(depth,hardware.command,{.75F,0,0,0});
        clear(output,hardware.command,{0,0,0,1});clear(4,hardware.command,{0,0,0,1});
        sl1::Constants constants{};auto* projection=reinterpret_cast<float*>(&constants.cameraViewToClip);auto* inverse=reinterpret_cast<float*>(&constants.clipToCameraView);
        projection[0]=projection[5]=1;projection[11]=-1;projection[14]=.1F;inverse[0]=inverse[5]=1;inverse[11]=10;inverse[14]=-1;
        constants.cameraRight={1,0,0};constants.cameraUp={0,1,0};constants.cameraFwd={0,0,-1};constants.cameraPos={float(serial),.25F,.5F};
        constants.cameraNear=.1F;constants.cameraFar=100;constants.cameraFOV=1;
        before_publication=dlss_motion_vector_publications();require_transport(set_constants(constants,serial,0)&&
            evaluate_sl(reinterpret_cast<sl1::CommandBuffer*>(hardware.command),sl1::eFeatureDLSS,serial,0),"actual original public SL frame");
        if(abandoned||reset_child){require_transport(prepared&&(reset_child||prepared->recording_leases)&&
                (reset_child||prepared->surfaces[motion].native_used),"missing child failed to retain its actual partial GPU preparation");
            if(serial==0)require_transport(!prepared->native_before[motion]&&!prepared->native_before[depth]&&
                prepared->surfaces[motion].native_used&&prepared->surfaces[depth].native_used,
                "cold abandonment did not exercise false-to-true native ownership");
            const auto original=prepared->native_before;require_transport(hardware.observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(hardware.command,0),"abandon original public guide preparation");
            require_transport(prepared->diagnostic_late_depth.native_used==prepared->diagnostic_late_depth_before,
                "abandoned late depth witness retained nonexistent external ownership");
            require_transport(prepared->diagnostic_native_color.native_used==prepared->diagnostic_native_color_before,
                "abandoned native color witness retained nonexistent external ownership");
            require_transport(!prepared->recording_leases&&prepared->complete()&&dlss_motion_vector_publications()==before_publication,
                "abandoned original public preparation submitted/published or retained recording");
            for(unsigned r=0;r<role_count;++r)require_transport(prepared->surfaces[r].native_used==original[r],"unsubmitted original preparation did not restore exact native ownership");
            for(unsigned i=0;i<images.size();++i)images[i].layout=original_layouts[i];return;}
        require_transport(!prepared->complete(),"unsubmitted replacement recording reused retired predecessor fences");
        barrier(images[output],hardware.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);VkBufferImageCopy region{};
        region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={ow,oh,1};
        hardware.observed<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(hardware.command,images[output].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&region);
        barrier(images[output],hardware.command,VK_IMAGE_LAYOUT_GENERAL);
        if(depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
            barrier(images[depth],hardware.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy stencil{};stencil.imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};stencil.imageExtent={16,8,1};
            hardware.observed<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(hardware.command,images[depth].image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hardware.readback.buffer,1,&stencil);
            barrier(images[depth],hardware.command,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        require_transport(hardware.observed<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(hardware.command),"end actual public wrapper continuation");
        require_transport(hardware.raw<PFN_vkResetFences>("vkResetFences")(hardware.native_device,1,&hardware.finished),"reset public GPU suffix fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&hardware.command;
        require_transport(hardware.observed<PFN_vkQueueSubmit>("vkQueueSubmit")(hardware.native_queue,1,&submit,hardware.finished),"submit original public guide/color transaction");
        require_transport(hardware.raw<PFN_vkWaitForFences>("vkWaitForFences")(hardware.native_device,1,&hardware.finished,VK_TRUE,5000000000ULL),"wait original public GPU transaction");
        if(depth_format==VK_FORMAT_D24_UNORM_S8_UINT) {
            void* stencil{};require_transport(hardware.raw<PFN_vkMapMemory>("vkMapMemory")(hardware.native_device,hardware.readback.memory,0,128,0,&stencil),"D24 stencil witness");
            for(unsigned i=0;i<128;++i)require_transport(static_cast<unsigned char*>(stencil)[i]==0xa7,"depth transport modified native stencil");
            hardware.raw<PFN_vkUnmapMemory>("vkUnmapMemory")(hardware.native_device,hardware.readback.memory);
        }
        require_transport(tested_native_output_copyback_pixels-copyback_pixels_before==kLukeOriginWidth*kLukeOriginHeight,
            "active AFW native copyback still transfers the full output instead of exact marker pixels");
        void* bytes{};require_transport(hardware.raw<PFN_vkMapMemory>("vkMapMemory")(hardware.native_device,readback.memory,0,ow*oh*4,0,&bytes),"map final original public Vulkan color");
        std::uint32_t token{};std::array<float,ow> row{};
        for(unsigned y=0;y<oh;++y){for(unsigned x=0;x<ow;++x)row[x]=luke_origin_luma(static_cast<const std::byte*>(bytes)+(y*ow+x)*4,DXGI_FORMAT_R8G8B8A8_UNORM);
            const auto decoded=decode_luke_origin_row(row);require_transport(!decoded.ambiguous,"final original public color token ambiguous");if(decoded.token){require_transport(!token||token==decoded.token,"final public output contains competing tokens");token=decoded.token;}}
        const std::array<unsigned char,4> composite{40,51,63,255};
        // Marker-only copyback must preserve every native pixel outside the
        // actual bands, including nonzero row/column offsets and row padding.
        LukeColorOriginLayout marker_layout{};
        require_transport(compact_luke_origin_layout(ow,oh,ow,oh,marker_layout),"public marker layout");
        for(unsigned y=0;y<oh;++y)for(unsigned x=0;x<ow;++x) {
            bool marker_pixel=false;
            for(unsigned band=0;band<marker_layout.band_count;++band) {
                const auto& rect=marker_layout.bands[band];
                marker_pixel|=x>=rect.x&&x<rect.x+rect.width&&y>=rect.y&&y<rect.y+rect.height;
            }
            if(!marker_pixel)require_transport(!std::memcmp(static_cast<unsigned char*>(bytes)+(y*ow+x)*4,
                composite.data(),4),"marker-only copyback overwrote unchanged wrapper composite pixels");
        }
        const auto* actual=static_cast<unsigned char*>(bytes)+(oh-1)*ow*4;
        if(serial==1||std::memcmp(actual,composite.data(),4))std::cout<<"Public final GPU RGBA="<<unsigned(actual[0])<<','<<unsigned(actual[1])<<','
            <<unsigned(actual[2])<<','<<unsigned(actual[3])<<" expected="<<unsigned(composite[0])<<','<<unsigned(composite[1])<<','
            <<unsigned(composite[2])<<','<<unsigned(composite[3])<<" native-private RGBA=37,73,113,255\n";
        for(unsigned x=0;x<ow;++x)require_transport(!std::memcmp(static_cast<unsigned char*>(bytes)+((oh-1)*ow+x)*4,composite.data(),4),"final original public color was captured before wrapper post-SR copyback");
        hardware.raw<PFN_vkUnmapMemory>("vkUnmapMemory")(hardware.native_device,readback.memory);require_transport(token!=0,"final original public output lost physical CRC marker");
        const auto frame=resolve_luke_guide_token(token,hardware.device->queue.Get(),hardware.device->owner.Get());if(serial==2||serial==5)verify_snapshots(frame,serial);
        // Real games keep completed command buffers executable for many frames.
        // Their recording leases must not exhaust the bounded transport pool.
        require_transport(prepared->recording_leases!=0,"completed recording lease unexpectedly reset by test");
        require_transport(prepared->complete(),"completed public slot pinned until application command-buffer reset");
        ++prepared->final_value;
        require_transport(!prepared->complete(),"native completion fence bypassed while recycling a submitted slot");
        --prepared->final_value;++prepared->d12_value;
        require_transport(!prepared->complete(),"D3D12 completion fence bypassed while recycling a submitted slot");
        --prepared->d12_value;++prepared->pending_callbacks;
        require_transport(!prepared->complete(),"pending submit callback bypassed while recycling a submitted slot");
        --prepared->pending_callbacks;
        require_transport(hardware.observed<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(hardware.command,0),"retire exact completed public original recording");
    }
    void verify(){setup();
        if(depth_format==VK_FORMAT_D24_UNORM_S8_UINT)verify_d24_words();
        // The regular transport may already own a completed matching RG16
        // slot (Indiana). Isolate its pool so the cold rollback is genuinely
        // false->true->false on a new production slot, for both game routes.
        auto previous_slots=std::move(hardware.device->slots);hardware.device->slots.clear();run(0,true);
        hardware.device->slots.insert(hardware.device->slots.end(),previous_slots.begin(),previous_slots.end());
        run(1);run(2);run(3,true);run(3,false,true);run(4);run(5);
        require_transport(native_calls==5&&public_calls==7,"public transaction duplicated original calls or compacted actual production");
        if(hardware.debug)for(UINT64 index=0;index<hardware.debug->GetNumStoredMessages();++index) {
            SIZE_T bytes{};hardware.debug->GetMessage(index,nullptr,&bytes);std::vector<unsigned char> storage(bytes);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if(SUCCEEDED(hardware.debug->GetMessage(index,message,&bytes))&&(message->Severity==D3D12_MESSAGE_SEVERITY_ERROR||message->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION))
                throw std::runtime_error(message->pDescription);
        }
        std::cout<<"Native Vulkan public transaction GPU depth format="<<depth_format<<": original Motion/depth+scalars+SL camera before in-place wrapper overwrite; final original Output composite+CRC after private SR/copyback; exact original reset/abandon and recovered same slot PASS\n";
        const auto observer=reinterpret_cast<void(*)(decltype(&sl_observer))>(GetProcAddress(streamline,"TestObserver"));observer(nullptr);
        const auto lifetime=hooks().public_lifetimes.find(public_handle);if(lifetime!=hooks().public_lifetimes.end())retire_dlss_motion_vector_stream(kVulkanPublicStreamNamespace|lifetime->second->identity);
        hooks().public_lifetimes.erase(public_handle);hooks().features.erase(native_handle);
        hooks().modules[1].evaluate=saved_native;hooks().modules[1].typed_implementation=saved_typed;
        const auto configure=reinterpret_cast<void(*)(Create,Create1,Evaluate,Release)>(GetProcAddress(hardware.luke_fixture,"TestLukeVulkanNgxCallbacks"));
        configure(&family_create_original,&family_create1_original,&family_public_original,&family_release_original);
        for(const auto& slot:hardware.device->slots)if(slot!=hardware.captured){for(auto& surface:slot->surfaces)destroy_surface(*hardware.device,surface);
            destroy_surface(*hardware.device,slot->diagnostic_late_depth);destroy_surface(*hardware.device,slot->diagnostic_native_color);
            hardware.raw<PFN_vkDestroySemaphore>("vkDestroySemaphore")(hardware.native_device,slot->timeline,nullptr);slot->timeline={};}
        std::erase_if(hardware.device->slots,[&](const auto& slot){return slot!=hardware.captured;});prepared.reset();
        for(auto& image:images){hardware.raw<PFN_vkDestroyImageView>("vkDestroyImageView")(hardware.native_device,image.view,nullptr);
            hardware.cached_image_destroy(hardware.native_device,image.image,nullptr);hardware.raw<PFN_vkFreeMemory>("vkFreeMemory")(hardware.native_device,image.memory,nullptr);}
        hardware.raw<PFN_vkDestroyBuffer>("vkDestroyBuffer")(hardware.native_device,readback.buffer,nullptr);
        hardware.raw<PFN_vkFreeMemory>("vkFreeMemory")(hardware.native_device,readback.memory,nullptr);current=nullptr;
    }
};
}
int run_buffer_transport_test(bool bootstrap) {
    // A failed GPU proof retains resources until process exit instead of
    // destroying possibly submitted external resources or waiting forever.
    try {
        // The application's loader outlives device recreation. Keep that same
        // lifetime while the three role probes destroy/recreate their devices;
        // unloading/reloading it would discard process-global inline hooks.
        struct LoaderLifetime {
            HMODULE module{LoadLibraryW(L"vulkan-1.dll")};
            ~LoaderLifetime(){if(module)FreeLibrary(module);}
        } loader_lifetime;
        if(!loader_lifetime.module)throw TransportSkip("Vulkan loader unavailable");
        verify_image_entry_contract();for(unsigned variant=0;variant<4;++variant) {
            const auto role=variant==1?color:variant==2?output:motion;
            auto* probe=new TransportProbe;probe->bootstrap=bootstrap;probe->capture_role=role;probe->rgba_motion=variant==3;
            probe->setup();probe->verify();if(bootstrap&&variant==0)for(auto format:{VK_FORMAT_D32_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT}) {
                PublicGuideGpuProbe transaction(*probe,format);transaction.verify();
            }
            probe->cleanup();delete probe;
            std::cout<<"Native Vulkan exact GPU role="<<role<<" format="<<(variant==3?"RGBA16":role==motion?"RG16":"R11G11B10")
                <<" pixels=128 bytes_per_pixel="<<(variant==3?8:4)<<" PASS\n";
        }
        std::cout<<"Native Vulkan production shared-buffer transport PASS: successful loader-chain dispatcher receipt, cached downstream native allocation, RG16 motion and packed R11G11B10 color/output GPU pixels bit-identical in D3D12 and Vulkan, exact suffix/D3D12 fences, unsubmitted recording cancellation, zero D3D12 validation errors\n";return 0;
    }catch(const TransportSkip& why){std::cout<<"Native Vulkan transport SKIP: "<<why.what()<<'\n';return 77;}
    catch(const std::exception& why){std::cerr<<"Native Vulkan transport FAIL: "<<why.what()<<'\n';return 1;}
}
}
