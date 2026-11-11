#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "xrfg/native_ingress/contracts/LukeVulkanProducer.h"
#include "xrfg/native_ingress/contracts/LukeCaptureParameters.h"
#include "xrfg/native_ingress/contracts/LukeAERMotionSnapshot.h"
#include <map>
#include <string>
#include "xrfg/native_ingress/contracts/VulkanInteropSubmit.h"
#include "xrfg/native_ingress/contracts/VulkanCaptureFeatures.h"
#include "xrfg/native_ingress/contracts/LibrarySearchPolicy.h"
#include "xrfg/native_ingress/contracts/rc_Shader_Vk.h"
#include "xrfg/native_ingress/contracts/VulkanCopyFormat.h"
#include "xrfg/luke_color_origin.hpp"
#include "xrfg/luke_origin_demand.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check(HRESULT value) { if (FAILED(value)) throw std::runtime_error("D3D12 HRESULT " + std::to_string(value)); }
void vkcheck(VkResult value) { if (value != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(value)); }

void submit_contracts() {
    for(int osr:{0,1,2,3})for(bool enabled:{false,true})for(bool aer:{false,true}) {
        for(int mode:{0,1,2})for(bool hybrid:{false,true}) {
            const bool active_aer=enabled&&aer;
            const bool expected=active_aer?(mode!=0||hybrid):osr!=0;
            expect(xrfg::luke_origin_marker_requested(enabled,aer,osr,mode,hybrid,-1)==expected,
                "Vulkan marker demand must follow the active consumer independently of saved OSR");
            expect(xrfg::luke_origin_marker_requested(enabled,aer,osr,mode,hybrid,2)==(!active_aer&&osr!=0),
                "RIFE cannot request native guides; global-off must retain OSR marker demand");
        }
    }
    struct MotionSource {
        NVSDK_NGX_Resource_VK* resource{};
        NVSDK_NGX_Result Get(const char*,void** value) const {
            *value=resource;return NVSDK_NGX_Result_Success;
        }
    } producer;
    NVSDK_NGX_Resource_VK native{};
    native.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    native.Resource.ImageViewInfo.Image=reinterpret_cast<VkImage>(11);
    native.Resource.ImageViewInfo.ImageView=reinterpret_cast<VkImageView>(12);
    native.Resource.ImageViewInfo.Width=1464;native.Resource.ImageViewInfo.Height=1464;
    producer.resource=&native;
    luke_vulkan::AERMotionSnapshot original;
    original.capture(producer);
    expect(original.valid && original.resource.Resource.ImageViewInfo.Image==reinterpret_cast<VkImage>(11),
        "native AER snapshot must bind this evaluation's ingress field");
    native.Resource.ImageViewInfo.Image=reinterpret_cast<VkImage>(99);
    native.Resource.ImageViewInfo.Width=2048;
    expect(original.resource.Resource.ImageViewInfo.Image==reinterpret_cast<VkImage>(11) &&
        original.resource.Resource.ImageViewInfo.Width==1464,
        "downstream descriptor mutation must not retarget the captured AER field");
    producer.resource=nullptr;original.capture(producer);
    expect(!original.valid && !original.resource.Resource.ImageViewInfo.Image,
        "missing current field cannot reuse a prior evaluation's descriptor");
    native.Type=NVSDK_NGX_RESOURCE_VK_TYPE_VK_BUFFER;producer.resource=&native;
    original.capture(producer);expect(!original.valid,"buffer cannot stand in for image motion");
    luke_vulkan::FrozenMotionReceipt frozen;
    const auto command=reinterpret_cast<VkCommandBuffer>(21);
    expect(!frozen.matches(command, 7),"no frozen GPU pixels before preparation");
    frozen.publish(command, 7);
    expect(frozen.matches(command, 7),"frozen motion belongs to this command/evaluation");
    expect(!frozen.matches(command, 8) && !frozen.matches(reinterpret_cast<VkCommandBuffer>(22), 7),
        "reused command or same-frame different command cannot consume old pixels");
    frozen.reset();
    expect(!frozen.matches(command, 7),"failed preparation/completed capture cannot leave a stale receipt");
    frozen.publish(VK_NULL_HANDLE, 7);
    expect(!frozen.valid,"invalid command cannot publish a frozen receipt");
    expect(library_search::accept_streamline_candidate(L"nvngx_dlss.dll",false) &&
        library_search::accept_streamline_candidate(L"nvngx_dlssd.dll",false),
        "native SR/RR dependencies cannot depend on enabled DLSSG output");
    expect(!library_search::accept_streamline_candidate(L"nvngx_dlssg.dll",false) &&
        !library_search::accept_streamline_candidate(L"nvngx.dll",false) &&
        !library_search::accept_streamline_candidate(L"other.dll",false) &&
        library_search::accept_streamline_candidate(L"nvngx_dlssg.dll",true),
        "do not disable the unrelated FG-bundle exclusion");
    VkPhysicalDeviceFeatures originalFeatures{};
    VkDeviceCreateInfo featureInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    featureInfo.pEnabledFeatures=&originalFeatures;
    vulkan_capture::FeatureRequest featureRequest;
    expect(featureRequest.prepare(featureInfo,true,true) &&
        featureInfo.pEnabledFeatures->shaderStorageImageExtendedFormats &&
        !originalFeatures.shaderStorageImageExtendedFormats,"core features must be enabled without mutating original");
    VkPhysicalDeviceFeatures2 originalFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features tail{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    originalFeatures2.pNext=&tail;featureInfo.pEnabledFeatures=nullptr;featureInfo.pNext=&originalFeatures2;
    vulkan_capture::FeatureRequest featureRequest2;
    expect(featureRequest2.prepare(featureInfo,true,true) && featureInfo.pNext!=&originalFeatures2 &&
        featureRequest2.core2.pNext==&tail && !originalFeatures2.features.shaderStorageImageExtendedFormats,
        "Features2 copy must preserve tail and original");
    featureInfo.pNext=&tail;tail.pNext=&originalFeatures2;originalFeatures2.pNext=nullptr;
    vulkan_capture::FeatureRequest buried;
    expect(!buried.prepare(featureInfo,true,true) && tail.pNext==&originalFeatures2,
        "unsupported buried feature must not be mutated or assumed enabled");
    for (auto format : {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT})
        expect(!vulkan_copy::typed_shader(rc_spv,format).empty(),"typed production copy shader");
    expect(vulkan_copy::typed_shader(rc_spv,VK_FORMAT_R16G16_UINT).empty() &&
        vulkan_copy::typed_shader(rc_spv,VK_FORMAT_R8G8B8A8_SRGB).empty(),"integer/sRGB storage reinterpretation");
    const auto cmd = [](uintptr_t n) { return reinterpret_cast<VkCommandBuffer>(n); };
    const auto sem = [](uintptr_t n) { return reinterpret_cast<VkSemaphore>(n); };
    vulkan_interop::Handoff h{cmd(1),cmd(2),cmd(3),sem(4),sem(5),sem(5),10,11,12,13};
    const VkCommandBuffer buffers[]{cmd(9),cmd(1),cmd(10)};
    const VkSemaphore wait[]{sem(6)}, signal[]{sem(7),sem(8)};
    const uint64_t waitValues[]{90}, signalValues[]{91,92};
    VkProtectedSubmitInfo protectedInfo{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.pNext=&protectedInfo; timeline.waitSemaphoreValueCount=1;timeline.pWaitSemaphoreValues=waitValues;
    timeline.signalSemaphoreValueCount=2;timeline.pSignalSemaphoreValues=signalValues;
    VkPipelineStageFlags stage=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo source{VK_STRUCTURE_TYPE_SUBMIT_INFO};source.pNext=&timeline;
    source.waitSemaphoreCount=1;source.pWaitSemaphores=wait;source.pWaitDstStageMask=&stage;
    source.signalSemaphoreCount=2;source.pSignalSemaphores=signal;
    source.commandBufferCount=3;source.pCommandBuffers=buffers;
    vulkan_interop::Submit1 packet;expect(packet.build(1,&source,h),"Submit1 build");
    protectedInfo.protectedSubmit=VK_TRUE;
    vulkan_interop::Submit1 protectedPacket;expect(!protectedPacket.build(1,&source,h),"protected capture must reject");
    protectedInfo.protectedSubmit=VK_FALSE;
    expect(source.pNext==&timeline && timeline.pNext==&protectedInfo && timeline.signalSemaphoreValueCount==2 &&
        signalValues[0]==91 && signalValues[1]==92,"application timeline mutated");
    expect(packet.submits.size()==3 && packet.submits[0].commandBufferCount==2 &&
        packet.submits[2].pCommandBuffers[0]==cmd(3) && packet.submits[2].pCommandBuffers[1]==cmd(10),"command split");
    expect(packet.firstTimeline.pWaitSemaphoreValues[0]==90 && packet.copyTimeline.pWaitSemaphoreValues[0]==11 &&
        packet.finalTimeline.pWaitSemaphoreValues[0]==12,"wrong D3D12/copyback dependency");
    expect(packet.finalTimeline.signalSemaphoreValueCount==3 && packet.signals[0]==91 && packet.signals[1]==92 &&
        packet.signals[2]==13 && packet.finalSignals[2]==sem(5),"lost real completion/signal identity");
    const uint32_t masks[]{1,1,1}, indices[]{0,0};
    VkDeviceGroupSubmitInfo group{VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO};
    group.commandBufferCount=3;group.pCommandBufferDeviceMasks=masks;
    group.waitSemaphoreCount=1;group.pWaitSemaphoreDeviceIndices=indices;
    group.signalSemaphoreCount=2;group.pSignalSemaphoreDeviceIndices=indices;group.pNext=&timeline;
    source.pNext=&group;vulkan_interop::Submit1 grouped;expect(grouped.build(1,&source,h),"grouped Submit1");
    expect(group.commandBufferCount==3 && group.signalSemaphoreCount==2 && group.pNext==&timeline &&
        grouped.finalGroup.commandBufferCount==2 && grouped.finalGroup.signalSemaphoreCount==3,"device group mutation");
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr};source.pNext=&unknown;
    vulkan_interop::Submit1 invalid;expect(!invalid.build(1,&source,h),"unknown metadata guessed");
    std::array<VkCommandBufferSubmitInfo,3> cmds{};
    for(unsigned i=0;i<3;++i) {cmds[i].sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;cmds[i].commandBuffer=buffers[i];cmds[i].deviceMask=1;}
    VkSemaphoreSubmitInfo wait2{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};wait2.semaphore=wait[0];wait2.value=90;
    wait2.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    auto signal2=wait2;signal2.semaphore=signal[0];signal2.value=91;
    VkSubmitInfo2 source2{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};source2.commandBufferInfoCount=3;source2.pCommandBufferInfos=cmds.data();
    source2.waitSemaphoreInfoCount=1;source2.pWaitSemaphoreInfos=&wait2;source2.signalSemaphoreInfoCount=1;source2.pSignalSemaphoreInfos=&signal2;
    vulkan_interop::Submit2 packet2;expect(packet2.build(1,&source2,h),"Submit2 build");
    source2.flags=VK_SUBMIT_PROTECTED_BIT;
    vulkan_interop::Submit2 protected2;expect(!protected2.build(1,&source2,h),"protected Submit2 must reject");
    source2.flags=0;
    expect(packet2.ready.value==11 && packet2.copied.value==12 && packet2.signals.back().value==13 &&
        packet2.signals.front().value==91 && source2.pSignalSemaphoreInfos==&signal2 && signal2.value==91,"Submit2 lost exact phase/signal");
    std::cout<<"Submit1/Submit2 immutable metadata and phase contracts PASS\n";
}

struct Gpu {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};VkQueue queue{};uint32_t family{};
    VkCommandPool pool{};std::array<VkCommandBuffer,4> commands{};
    VkImage local{},shared{};VkDeviceMemory localMemory{},sharedMemory{},readMemory{};
    VkBuffer readBuffer{};VkSemaphore texture{},copied{},userSignal{};VkFence finished{};
    VkImageView localView{},sharedView{};VkShaderModule shader{};VkPipeline pipeline{};
    VkPipelineLayout pipelineLayout{};VkDescriptorSetLayout setLayout{};VkDescriptorPool descriptors{};
    std::array<VkDescriptorSet,2> sets{};
    ComPtr<ID3D12Device> d3d;ComPtr<ID3D12CommandQueue> d3dQueue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> color;ComPtr<ID3D12Fence> externalFence;
    PFN_vkImportSemaphoreWin32HandleKHR importSemaphore{};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memoryProperties{};
    static constexpr unsigned width=512,height=32;
    uint32_t memoryType(uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p{};vkGetPhysicalDeviceMemoryProperties(physical,&p);
        for(uint32_t i=0;i<p.memoryTypeCount;++i) if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("no memory type");
    }
    VkSemaphore timeline() {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};info.pNext=&type;VkSemaphore result{};
        vkcheck(vkCreateSemaphore(device,&info,nullptr,&result));return result;
    }
    Gpu() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};create.pApplicationInfo=&app;
        vkcheck(vkCreateInstance(&create,nullptr,&instance));uint32_t count{};
        vkcheck(vkEnumeratePhysicalDevices(instance,&count,nullptr));expect(count>0,"no Vulkan GPU");
        std::vector<VkPhysicalDevice> devices(count);vkcheck(vkEnumeratePhysicalDevices(instance,&count,devices.data()));
        ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        for(auto candidate:devices) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&id;
            vkGetPhysicalDeviceProperties2(candidate,&properties);
            if(!id.deviceLUIDValid) continue;LUID luid{};std::memcpy(&luid,id.deviceLUID,sizeof(luid));
            ComPtr<IDXGIAdapter1> adapter;
            if(SUCCEEDED(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter))) &&
                SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d3d)))) {physical=candidate;break;}
        }
        expect(physical && d3d,"no matched Vulkan/D3D12 device");
        uint32_t queueCount{};vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,nullptr);
        std::vector<VkQueueFamilyProperties> families(queueCount);vkGetPhysicalDeviceQueueFamilyProperties(physical,&queueCount,families.data());
        while(family<queueCount && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
        expect(family<queueCount,"no graphics queue");float priority=1;
        VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qc.queueFamilyIndex=family;qc.queueCount=1;qc.pQueuePriorities=&priority;
        const char* extensions[]{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};features13.synchronization2=VK_TRUE;
        VkPhysicalDeviceTimelineSemaphoreFeatures features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
        features.timelineSemaphore=VK_TRUE;features.pNext=&features13;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.pNext=&features;dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
        VkPhysicalDeviceFeatures core{};core.shaderStorageImageExtendedFormats=VK_TRUE;dc.pEnabledFeatures=&core;
        dc.enabledExtensionCount=2;dc.ppEnabledExtensionNames=extensions;vkcheck(vkCreateDevice(physical,&dc,nullptr,&device));
        vkGetDeviceQueue(device,family,0,&queue);
        memoryProperties=reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(vkGetDeviceProcAddr(device,"vkGetMemoryWin32HandlePropertiesKHR"));
        importSemaphore=reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(vkGetDeviceProcAddr(device,"vkImportSemaphoreWin32HandleKHR"));
        expect(memoryProperties && importSemaphore,"missing Win32 interop functions");
        luke_vulkan::Producer producer;
        const auto deviceKey = reinterpret_cast<std::uintptr_t>(device);
        producer.record(deviceKey, d3d->GetAdapterLuid(), 1, true);
        ComPtr<ID3D12Device> earlyOwner, captureOwner;
        ComPtr<ID3D12CommandQueue> earlyQueue;
        check(producer.get(0, &earlyOwner, &earlyQueue));
        ComPtr<ID3D12CommandQueue> xrQueue;
        check(producer.get(deviceKey, &captureOwner, &xrQueue));
        expect(earlyOwner.Get() == captureOwner.Get() && earlyQueue.Get() == xrQueue.Get(),
            "early XR owner identity must remain stable");
        check(luke_vulkan::create_capture_queue(captureOwner.Get(), xrQueue.Get(), &d3dQueue));
        expect(d3dQueue.Get() != xrQueue.Get(), "native capture cannot block the XR owner queue");
        ComPtr<ID3D12Device> actualCaptureOwner;
        check(d3dQueue->GetDevice(IID_PPV_ARGS(&actualCaptureOwner)));
        expect(actualCaptureOwner.Get() == captureOwner.Get(), "capture must retain the exact owner device");
        // Reproduce the early Vulkan wait. The consumer must still submit and
        // complete its own work before Vulkan releases the capture queue.
        ComPtr<ID3D12Fence> pendingCapture, consumerProgress;
        check(captureOwner->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pendingCapture)));
        check(captureOwner->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumerProgress)));
        struct CaptureWaitRelease {
            ID3D12Fence* fence;
            ~CaptureWaitRelease() { if (fence) fence->Signal(1); }
            void release() { check(fence->Signal(1)); fence = nullptr; }
        } releaseCapture{pendingCapture.Get()};
        check(d3dQueue->Wait(pendingCapture.Get(), 1));
        check(xrQueue->Signal(consumerProgress.Get(), 1));
        struct EventHandle {
            HANDLE value;
            ~EventHandle() { if (value) CloseHandle(value); }
        } progressEvent{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
        expect(progressEvent.value != nullptr, "consumer progress event");
        check(consumerProgress->SetEventOnCompletion(1, progressEvent.value));
        const DWORD progress = WaitForSingleObject(progressEvent.value, 1000);
        // The scope guard also handles every intermediate failure above.
        releaseCapture.release();
        expect(progress == WAIT_OBJECT_0, "future Vulkan capture wait blocked the consumer queue");
        d3d = captureOwner;
        producer.record(deviceKey + 1, d3d->GetAdapterLuid(), 1, true);
        ID3D12Device* rejectedOwner{}; ID3D12CommandQueue* rejectedQueue{};
        expect(FAILED(producer.get(0, &rejectedOwner, &rejectedQueue)) && !rejectedOwner && !rejectedQueue,
            "ambiguous producer cannot be guessed");
        producer.forget(deviceKey + 1); producer.forget(deviceKey);
        expect(FAILED(producer.get(0, &rejectedOwner, &rejectedQueue)), "retired native device cannot be selected");
        check(d3d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        check(d3d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));check(list->Close());
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=width;desc.Height=height;
        desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        check(d3d->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&color)));
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R8G8B8A8_UNORM;
        image.extent={width,height,1};image.mipLevels=image.arrayLayers=1;image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.tiling=VK_IMAGE_TILING_OPTIMAL;image.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        vkcheck(vkCreateImage(device,&image,nullptr,&local));VkMemoryRequirements requirements{};vkGetImageMemoryRequirements(device,local,&requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkcheck(vkAllocateMemory(device,&allocation,nullptr,&localMemory));vkcheck(vkBindImageMemory(device,local,localMemory,0));
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        image.pNext=&external;vkcheck(vkCreateImage(device,&image,nullptr,&shared));vkGetImageMemoryRequirements(device,shared,&requirements);
        HANDLE handle{};check(d3d->CreateSharedHandle(color.Get(),nullptr,GENERIC_ALL,nullptr,&handle));
        VkMemoryWin32HandlePropertiesKHR properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        vkcheck(memoryProperties(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,handle,&properties));
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=shared;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        import.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;import.handle=handle;import.pNext=&dedicated;
        allocation.pNext=&import;allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits&properties.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkcheck(vkAllocateMemory(device,&allocation,nullptr,&sharedMemory));CloseHandle(handle);vkcheck(vkBindImageMemory(device,shared,sharedMemory,0));
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.viewType=VK_IMAGE_VIEW_TYPE_2D;
        view.format=VK_FORMAT_R8G8B8A8_UNORM;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        view.image=local;vkcheck(vkCreateImageView(device,&view,nullptr,&localView));
        view.image=shared;vkcheck(vkCreateImageView(device,&view,nullptr,&sharedView));
        VkDescriptorSetLayoutBinding bindings[2]{};for(unsigned i=0;i<2;++i) {bindings[i].binding=i;bindings[i].descriptorCount=1;
            bindings[i].descriptorType=i?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;}
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};layout.bindingCount=2;layout.pBindings=bindings;
        vkcheck(vkCreateDescriptorSetLayout(device,&layout,nullptr,&setLayout));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;
        vkcheck(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
        const auto typedCode=vulkan_copy::typed_shader(rc_spv,VK_FORMAT_R8G8B8A8_UNORM);
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=typedCode.size();
        std::vector<uint32_t> code(typedCode.size()/4);std::memcpy(code.data(),typedCode.data(),typedCode.size());sm.pCode=code.data();
        vkcheck(vkCreateShaderModule(device,&sm,nullptr,&shader));VkComputePipelineCreateInfo pc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pc.layout=pipelineLayout;pc.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pc.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pc.stage.module=shader;pc.stage.pName="CSMain";
        vkcheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pc,nullptr,&pipeline));
        for(auto format:{VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R32_SFLOAT}) {
            auto typed=vulkan_copy::typed_shader(rc_spv,format);
            std::vector<uint32_t> words(typed.size()/4);std::memcpy(words.data(),typed.data(),typed.size());
            VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};moduleInfo.codeSize=typed.size();moduleInfo.pCode=words.data();
            VkShaderModule module{};vkcheck(vkCreateShaderModule(device,&moduleInfo,nullptr,&module));
            auto createPipeline=pc;createPipeline.stage.module=module;VkPipeline typedPipeline{};
            vkcheck(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&createPipeline,nullptr,&typedPipeline));
            vkDestroyPipeline(device,typedPipeline,nullptr);vkDestroyShaderModule(device,module,nullptr);
        }
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=2;dp.poolSizeCount=2;dp.pPoolSizes=sizes;
        vkcheck(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors));VkDescriptorSetLayout layouts[]{setLayout,setLayout};
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=descriptors;da.descriptorSetCount=2;da.pSetLayouts=layouts;
        vkcheck(vkAllocateDescriptorSets(device,&da,sets.data()));
        for(unsigned direction=0;direction<2;++direction) {
            VkDescriptorImageInfo images[2]{{VK_NULL_HANDLE,direction?sharedView:localView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {VK_NULL_HANDLE,direction?localView:sharedView,VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[2]{};for(unsigned i=0;i<2;++i) {writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet=sets[direction];writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&images[i];}
            vkUpdateDescriptorSets(device,2,writes,0,nullptr);
        }
        check(d3d->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&externalFence)));
        check(d3d->CreateSharedHandle(externalFence.Get(),nullptr,GENERIC_ALL,nullptr,&handle));
        texture=timeline();copied=timeline();userSignal=timeline();
        VkImportSemaphoreWin32HandleInfoKHR si{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        si.semaphore=texture;si.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;si.handle=handle;
        vkcheck(importSemaphore(device,&si));CloseHandle(handle);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=width*height*8;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        vkcheck(vkCreateBuffer(device,&bi,nullptr,&readBuffer));vkGetBufferMemoryRequirements(device,readBuffer,&requirements);
        allocation.pNext=nullptr;allocation.allocationSize=requirements.size;
        allocation.memoryTypeIndex=memoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkcheck(vkAllocateMemory(device,&allocation,nullptr,&readMemory));vkcheck(vkBindBufferMemory(device,readBuffer,readMemory,0));
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkcheck(vkCreateCommandPool(device,&pi,nullptr,&pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=4;
        vkcheck(vkAllocateCommandBuffers(device,&ca,commands.data()));VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(vkCreateFence(device,&fi,nullptr,&finished));
    }
    void barrier(VkCommandBuffer command,VkImage image,VkImageLayout from,VkImageLayout to,
                 uint32_t source=VK_QUEUE_FAMILY_IGNORED,uint32_t destination=VK_QUEUE_FAMILY_IGNORED) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;b.image=image;
        b.srcQueueFamilyIndex=source;b.dstQueueFamilyIndex=destination;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED||source==VK_QUEUE_FAMILY_EXTERNAL?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask=destination==VK_QUEUE_FAMILY_EXTERNAL?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
    void copy(VkCommandBuffer command,VkImage source,VkImage target) {
        VkImageCopy c{};c.srcSubresource=c.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.extent={width,height,1};
        vkCmdCopyImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
    }
    void shaderCopy(VkCommandBuffer command,unsigned direction) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&sets[direction],0,nullptr);
        vkCmdDispatch(command,width/16,height/16,1);
    }
    void run(bool submit2,uint64_t generation) {
        vkcheck(vkResetFences(device,1,&finished));vkcheck(vkResetCommandPool(device,pool,0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        for(auto command:commands) vkcheck(vkBeginCommandBuffer(command,&begin));
        barrier(commands[0],local,generation==1?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue clear{{.25f,.5f,.75f,1.f}};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        barrier(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy before{};before.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};before.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readBuffer,1,&before);
        barrier(commands[0],shared,generation==1?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (generation%3) {
            barrier(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            barrier(commands[0],shared,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            shaderCopy(commands[0],0);
            barrier(commands[0],local,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(commands[0],shared,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,family,VK_QUEUE_FAMILY_EXTERNAL);
        } else {
            copy(commands[0],local,shared);
            barrier(commands[0],shared,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,family,VK_QUEUE_FAMILY_EXTERNAL);
        }
        // Simulate a downstream in-place producer write AFTER capture, in the
        // same command. The shared snapshot/copyback must retain pre-write
        // pixels (compared below), with both shader and transfer copy paths.
        barrier(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue downstream{{.9f,.1f,.2f,0.f}};
        vkCmdClearColorImage(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&downstream,1,&range);
        barrier(commands[0],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(commands[1],shared,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_QUEUE_FAMILY_EXTERNAL,family);
        barrier(commands[1],shared,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(commands[1],local,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (generation%3) {
            barrier(commands[1],shared,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            barrier(commands[1],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            shaderCopy(commands[1],1);
            barrier(commands[1],shared,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(commands[1],local,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        } else {
            copy(commands[1],shared,local);barrier(commands[1],local,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        VkBufferImageCopy region{};region.bufferOffset=width*height*4;
        region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={width,height,1};
        vkCmdCopyImageToBuffer(commands[2],local,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readBuffer,1,&region);
        for(auto command:commands) vkcheck(vkEndCommandBuffer(command));
        uint64_t captured=generation*10+1,ready=captured+1,copyDone=captured+2,completed=captured+3;
        check(d3dQueue->Wait(externalFence.Get(),captured));check(allocator->Reset());check(list->Reset(allocator.Get(),nullptr));
        xrfg::configure_luke_color_origin_extent(width,height);
        const uint32_t token=0x12340000+static_cast<uint32_t>(generation);
        // OSR is off for this real GPU round trip. Alternate Hybrid, FSR and
        // DLSS-G consumers; each still needs a CRC on the exact output image.
        const int mode=static_cast<int>(generation%3);
        const bool marker=xrfg::luke_origin_marker_requested(true,true,0,mode,mode==0,-1);
        expect(marker,"AER consumer lost marker demand with OSR off");
        if(marker)check(xrfg::mark_luke_color_origin(list.Get(),color.Get(),token,D3D12_RESOURCE_STATE_COMMON,d3d.Get(),{0,0,width,height}));
        check(list->Close());ID3D12CommandList* lists[]{list.Get()};d3dQueue->ExecuteCommandLists(1,lists);check(d3dQueue->Signal(externalFence.Get(),ready));
        const vulkan_interop::Handoff h{commands[0],commands[1],commands[2],texture,copied,copied,captured,ready,copyDone,completed};
        if(submit2) {
            VkCommandBufferSubmitInfo infos[2]{};for(unsigned i=0;i<2;++i) {infos[i].sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;infos[i].commandBuffer=commands[i?3:0];infos[i].deviceMask=1;}
            VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};signal.semaphore=userSignal;signal.value=generation;signal.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            VkSubmitInfo2 original{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};original.commandBufferInfoCount=2;original.pCommandBufferInfos=infos;
            original.signalSemaphoreInfoCount=1;original.pSignalSemaphoreInfos=&signal;
            vulkan_interop::Submit2 packet;expect(packet.build(1,&original,h),"real Submit2 build");
            vkcheck(vkQueueSubmit2(queue,static_cast<uint32_t>(packet.submits.size()),packet.submits.data(),finished));
        } else {
            VkCommandBuffer buffers[]{commands[0],commands[3]};uint64_t signalValue=generation;
            VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};ti.signalSemaphoreValueCount=1;ti.pSignalSemaphoreValues=&signalValue;
            VkSubmitInfo original{VK_STRUCTURE_TYPE_SUBMIT_INFO};original.pNext=&ti;original.commandBufferCount=2;original.pCommandBuffers=buffers;
            original.signalSemaphoreCount=1;original.pSignalSemaphores=&userSignal;
            vulkan_interop::Submit1 packet;expect(packet.build(1,&original,h),"real Submit1 build");
            vkcheck(vkQueueSubmit(queue,static_cast<uint32_t>(packet.submits.size()),packet.submits.data(),finished));
        }
        vkcheck(vkWaitForFences(device,1,&finished,VK_TRUE,5000000000));uint64_t value{};
        vkcheck(vkGetSemaphoreCounterValue(device,copied,&value));expect(value>=completed,"early slot completion");
        vkcheck(vkGetSemaphoreCounterValue(device,userSignal,&value));expect(value==generation,"lost original signal");
        void* mapped{};vkcheck(vkMapMemory(device,readMemory,0,VK_WHOLE_SIZE,0,&mapped));
        auto* baseline=static_cast<const std::byte*>(mapped);
        auto* pixels=baseline+width*height*4;std::vector<float> row(width);
        for(unsigned x=0;x<width;++x) row[x]=xrfg::luke_origin_luma(pixels+(2*width+x)*4,DXGI_FORMAT_R8G8B8A8_UNORM);
        const auto decoded=xrfg::decode_luke_origin_row(row);
        expect(decoded.token==token && !decoded.ambiguous,"Vulkan final color did not retain exact D3D12 origin");
        // UNORM clear rounding is implementation-dependent; compare the actual
        // original Vulkan bytes, not a CPU rounding assumption. Every unmarked
        // pixel must survive the complete cross-API handoff byte-for-byte.
        xrfg::LukeColorOriginLayout layout;
        expect(xrfg::compact_luke_origin_layout(width,height,width,height,layout),"test marker layout");
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            bool marked=false;
            for(unsigned i=0;i<layout.band_count;++i) {
                const auto& band=layout.bands[i];
                marked|=x>=band.x&&x<band.x+band.width&&y>=band.y&&y<band.y+band.height;
            }
            if(marked)continue;
            const auto offset=(y*width+x)*4;
            expect(std::memcmp(pixels+offset,baseline+offset,4)==0,"color changed outside marker");
        }
        vkUnmapMemory(device,readMemory);
    }
    ~Gpu() {
        if(!device) return;vkDeviceWaitIdle(device);
        vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyPipeline(device,pipeline,nullptr);
        vkDestroyPipelineLayout(device,pipelineLayout,nullptr);vkDestroyShaderModule(device,shader,nullptr);
        vkDestroyDescriptorSetLayout(device,setLayout,nullptr);vkDestroyImageView(device,localView,nullptr);vkDestroyImageView(device,sharedView,nullptr);
        vkDestroyFence(device,finished,nullptr);vkDestroyCommandPool(device,pool,nullptr);
        for(auto semaphore:{texture,copied,userSignal}) vkDestroySemaphore(device,semaphore,nullptr);
        vkDestroyBuffer(device,readBuffer,nullptr);vkDestroyImage(device,local,nullptr);vkDestroyImage(device,shared,nullptr);
        for(auto memory:{localMemory,sharedMemory,readMemory}) vkFreeMemory(device,memory,nullptr);
        vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    }
};
struct CaptureSource {
    NVSDK_NGX_Result Get(const char*, unsigned* value) const { *value = 2832; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char*, float* value) const { *value = -1.75f; return NVSDK_NGX_Result_Success; }
    NVSDK_NGX_Result Get(const char*, int* value) const { *value = 1; return NVSDK_NGX_Result_Success; }
    // No resource overloads: Vulkan-domain resource Get/Set must not be called.
};
struct CaptureDestination {
    std::map<std::string, unsigned> integers;
    std::map<std::string, float> floats;
    int reset{};
    void Set(const char* key, unsigned value) { integers[key] = value; }
    void Set(const char* key, float value) { floats[key] = value; }
    void Set(const char*, int value) { reset = value; }
};
int main() {
    try {
        CaptureSource source; CaptureDestination destination;
        luke_vulkan::copy_capture_metadata(source, destination);
        expect(destination.integers.size() == 13 && destination.floats.size() == 7 && destination.reset == 1,
            "all consumed metadata copied without resource overloads");
        expect(destination.integers.at(NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X) == 2832 &&
            destination.floats.at(NVSDK_NGX_Parameter_MV_Scale_X) == -1.75f,
            "nonzero crop and signed motion scale survive unchanged");
        submit_contracts();Gpu gpu;
        for(uint64_t frame=1;frame<=12;++frame) gpu.run(frame%2==0,frame);
        std::cout<<"Real matched-adapter Vulkan -> D3D12 marker -> Vulkan color, actual copy shader/transfer, Submit1/2, 12 frames PASS\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
