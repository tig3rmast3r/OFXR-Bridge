#include "xrfg/vulkan_command_stream.hpp"
#include "vulkan_command_internal.hpp"
#include "vulkan_command_tracker.hpp"
#include "vulkan_command_submit.hpp"
#include "xrfg/native_export_address.hpp"
#include <safetyhook.hpp>

namespace xrfg::native_vulkan {
namespace {
struct Segment { VkCommandBuffer prefix{},suffix{};CommandHandoff handoff;
    std::shared_ptr<RecordingLifetime> lifetime; };
struct CommandChain {
    VkCommandPool pool{};VkCommandBuffer actual{};
    VkCommandBufferLevel level{VK_COMMAND_BUFFER_LEVEL_PRIMARY};
    uint64_t epoch{};VkCommandBufferUsageFlags flags{};
    bool recording{},rendering{},unsupported_state{},submitted{};
    std::shared_ptr<RecordingReceipt> receipt;
    std::vector<Segment> segments;
    std::vector<std::shared_ptr<void>> retained;
    std::vector<std::function<void(VkCommandBuffer)>> state_commands;
};
struct DisposedRecordings {
    std::vector<CommandHandoff> handoffs;
    std::vector<std::shared_ptr<void>> retained;
};
struct DeviceState {
    VkInstance instance{};VkPhysicalDevice physical{};VkDevice device{};
    PFN_vkGetInstanceProcAddr gipa{};PFN_vkGetDeviceProcAddr gdpa{};
    std::recursive_mutex mutex;
    std::unordered_map<std::string,PFN_vkVoidFunction> functions;
    std::unordered_map<VkCommandPool,uint32_t> pools;
    std::unordered_map<VkCommandBuffer,CommandChain> commands;
    // Only continuations allocated by split() in this exact recording. A
    // wrapper may keep its received native prefix while an inner NGX call
    // advances the chain; later observed commands must reach the live suffix.
    std::unordered_map<VkCommandBuffer,VkCommandBuffer> continuation_owners;
    vk_state::CommandBufferStateTracker tracker;
    uint64_t next_epoch{};
};
struct UncommittedCommand {
    VkDevice device{};VkCommandPool pool{};VkCommandBuffer value{};PFN_vkFreeCommandBuffers free{};
    ~UncommittedCommand(){if(value&&free)free(device,pool,1,&value);}
};
struct Registry {
    std::mutex mutex;
    std::unordered_map<VkDevice,std::shared_ptr<DeviceState>> devices;
    std::unordered_map<void*,std::shared_ptr<DeviceState>> dispatch;
    std::unordered_map<VkCommandBuffer,std::weak_ptr<DeviceState>> commands;
    std::unordered_map<void*,SafetyHookInline> entry_hooks;
    std::unordered_map<std::string,PFN_vkVoidFunction> guide_fallbacks;
};
std::atomic<uint64_t> submission_serial{};
Registry& registry(){static auto* value=new Registry;return *value;}
void* dispatch_key(const void* object){return object?*static_cast<void* const*>(object):nullptr;}
std::shared_ptr<DeviceState> device_state(VkDevice device) {
    auto& r=registry();std::scoped_lock lock(r.mutex);
    auto found=r.devices.find(device);return found==r.devices.end()?nullptr:found->second;
}
std::shared_ptr<DeviceState> command_device(VkCommandBuffer command) {
    auto& r=registry();std::scoped_lock lock(r.mutex);
    if(auto found=r.commands.find(command);found!=r.commands.end())if(auto d=found->second.lock())return d;
    auto found=r.dispatch.find(dispatch_key(command));return found==r.dispatch.end()?nullptr:found->second;
}
std::shared_ptr<DeviceState> queue_device(VkQueue queue) {
    auto& r=registry();std::scoped_lock lock(r.mutex);
    auto found=r.dispatch.find(dispatch_key(queue));return found==r.dispatch.end()?nullptr:found->second;
}
template<class T>T load(DeviceState& device,const char* name) {
    std::scoped_lock lock(device.mutex);
    auto found=device.functions.find(name);
    if(found!=device.functions.end())return reinterpret_cast<T>(unhooked_command(found->second));
    auto function=device.gdpa?device.gdpa(device.device,name):nullptr;
    device.functions.emplace(name,function);return reinterpret_cast<T>(unhooked_command(function));
}
template<class T>T guide_fallback(const char* name) {
    auto& r=registry();PFN_vkVoidFunction fn{};
    {std::scoped_lock lock(r.mutex);auto it=r.guide_fallbacks.find(name);if(it!=r.guide_fallbacks.end())fn=it->second;}
    return reinterpret_cast<T>(unhooked_command(fn));
}
void original_locked(DeviceState& device,VkCommandBuffer& original) {
    if(auto alias=device.continuation_owners.find(original);alias!=device.continuation_owners.end())original=alias->second;
}
VkCommandBuffer native_locked(DeviceState& device,VkCommandBuffer& original) {
    original_locked(device,original);
    auto found=device.commands.find(original);
    return found!=device.commands.end()&&found->second.actual?found->second.actual:original;
}
void remember_state(DeviceState& device,VkCommandBuffer original,
    std::function<void(VkCommandBuffer)> replay) {
    auto& chain=device.commands[original];
    if(chain.state_commands.size()<4096)chain.state_commands.push_back(std::move(replay));
    else chain.unsupported_state=true;
}
template<class Fn>
void remember_viewport_count(DeviceState& d,VkCommandBuffer original,Fn fn,uint32_t count,const VkViewport* values) {
    std::vector<VkViewport> saved;if(count)saved.assign(values,values+count);
    remember_state(d,original,[=](VkCommandBuffer target){fn(target,count,saved.data());});
}
template<class Fn>
void remember_scissor_count(DeviceState& d,VkCommandBuffer original,Fn fn,uint32_t count,const VkRect2D* values) {
    std::vector<VkRect2D> saved;if(count)saved.assign(values,values+count);
    remember_state(d,original,[=](VkCommandBuffer target){fn(target,count,saved.data());});
}
template<class Fn>
void remember_vertex2(DeviceState& d,VkCommandBuffer original,Fn fn,uint32_t first,uint32_t count,
    const VkBuffer* buffers,const VkDeviceSize* offsets,const VkDeviceSize* sizes,const VkDeviceSize* strides) {
    std::vector<VkBuffer> saved_buffers;std::vector<VkDeviceSize> saved_offsets,saved_sizes,saved_strides;
    if(count){saved_buffers.assign(buffers,buffers+count);saved_offsets.assign(offsets,offsets+count);
        if(sizes)saved_sizes.assign(sizes,sizes+count);if(strides)saved_strides.assign(strides,strides+count);}
    remember_state(d,original,[=](VkCommandBuffer target){fn(target,first,count,saved_buffers.data(),saved_offsets.data(),
        sizes?saved_sizes.data():nullptr,strides?saved_strides.data():nullptr);});
}
void register_command(const std::shared_ptr<DeviceState>& device,VkCommandBuffer original) {
    auto& r=registry();std::scoped_lock lock(r.mutex);r.commands[original]=device;
}
void release_chain(DeviceState& device,CommandChain& chain,DisposedRecordings& abandoned) {
    if(chain.receipt) {
        auto expected=RecordingDisposition::recording;
        (void)chain.receipt->disposition.compare_exchange_strong(expected,RecordingDisposition::abandoned);
        chain.receipt->disposed.store(true,std::memory_order_release);
        chain.receipt.reset();
    }
    auto free=load<PFN_vkFreeCommandBuffers>(device,"vkFreeCommandBuffers");
    for(auto& segment:chain.segments) {
        if(segment.lifetime) {
            auto expected=RecordingDisposition::recording;
            if(segment.lifetime->disposition.compare_exchange_strong(expected,RecordingDisposition::abandoned)&&
                segment.handoff.abandoned)abandoned.handoffs.push_back(segment.handoff);
        }
        device.continuation_owners.erase(segment.suffix);
        if(free&&segment.suffix)free(device.device,chain.pool,1,&segment.suffix);
    }
    abandoned.retained.insert(abandoned.retained.end(),std::make_move_iterator(chain.retained.begin()),
        std::make_move_iterator(chain.retained.end()));
    chain.retained.clear();chain.segments.clear();chain.state_commands.clear();chain.actual={};
    chain.recording=false;chain.rendering=false;chain.unsupported_state=false;chain.submitted=false;
}
void notify_abandoned(const DisposedRecordings& abandoned) noexcept {
    for(const auto& handoff:abandoned.handoffs)if(handoff.abandoned)handoff.abandoned(handoff.submission_context.get());
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateCommandPool(VkDevice handle,
    const VkCommandPoolCreateInfo* info,const VkAllocationCallbacks* allocator,VkCommandPool* pool) {
    auto d=device_state(handle);if(!d){auto f=guide_fallback<PFN_vkCreateCommandPool>("vkCreateCommandPool");return f?f(handle,info,allocator,pool):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkCreateCommandPool>(*d,"vkCreateCommandPool");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    auto result=fn(handle,info,allocator,pool);if(result==VK_SUCCESS&&info&&pool) {
        std::scoped_lock lock(d->mutex);d->pools[*pool]=info->queueFamilyIndex;
    }return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkAllocateCommandBuffers(VkDevice handle,
    const VkCommandBufferAllocateInfo* info,VkCommandBuffer* commands) {
    auto d=device_state(handle);if(!d){auto f=guide_fallback<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");return f?f(handle,info,commands):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkAllocateCommandBuffers>(*d,"vkAllocateCommandBuffers");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    auto result=fn(handle,info,commands);if(result!=VK_SUCCESS||!info||!commands)return result;
    std::scoped_lock lock(d->mutex);auto family=d->pools.find(info->commandPool);
    if(family==d->pools.end())return result;
    d->tracker.OnAllocateCommandBuffers(info->commandPool,info->commandBufferCount,commands,family->second);
    for(uint32_t i=0;i<info->commandBufferCount;++i) {
        auto& chain=d->commands[commands[i]];chain.pool=info->commandPool;chain.level=info->level;
        chain.actual=commands[i];register_command(d,commands[i]);
    }return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkBeginCommandBuffer(VkCommandBuffer original,
    const VkCommandBufferBeginInfo* info) {
    auto d=command_device(original);if(!d){auto f=guide_fallback<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");return f?f(original,info):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkBeginCommandBuffer>(*d,"vkBeginCommandBuffer");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock lock(d->mutex);DisposedRecordings abandoned;
    auto& chain=d->commands[original];release_chain(*d,chain,abandoned);
    auto result=fn(original,info);if(result==VK_SUCCESS) {
        chain.actual=original;chain.recording=true;chain.flags=info?info->flags:0;chain.epoch=++d->next_epoch;
        d->tracker.OnBegin(original,info);
    }lock.unlock();notify_abandoned(abandoned);return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkEndCommandBuffer(VkCommandBuffer original) {
    auto d=command_device(original);if(!d){auto f=guide_fallback<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");return f?f(original):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkEndCommandBuffer>(*d,"vkEndCommandBuffer");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    std::scoped_lock lock(d->mutex);auto result=fn(native_locked(*d,original));
    if(result==VK_SUCCESS){d->commands[original].recording=false;d->tracker.OnEnd(original);}return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkResetCommandBuffer(VkCommandBuffer original,VkCommandBufferResetFlags flags) {
    auto d=command_device(original);if(!d){auto f=guide_fallback<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");return f?f(original,flags):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkResetCommandBuffer>(*d,"vkResetCommandBuffer");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock lock(d->mutex);DisposedRecordings abandoned;auto result=fn(original,flags);
    if(result==VK_SUCCESS){release_chain(*d,d->commands[original],abandoned);d->tracker.OnReset(original);}
    lock.unlock();notify_abandoned(abandoned);return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkFreeCommandBuffers(VkDevice handle,VkCommandPool pool,uint32_t count,
    const VkCommandBuffer* commands) {
    auto d=device_state(handle);if(!d){auto f=guide_fallback<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers");if(f)f(handle,pool,count,commands);return;}auto fn=load<PFN_vkFreeCommandBuffers>(*d,"vkFreeCommandBuffers");if(!fn)return;
    std::unique_lock lock(d->mutex);DisposedRecordings abandoned;for(uint32_t i=0;commands&&i<count;++i) {
        if(auto found=d->commands.find(commands[i]);found!=d->commands.end()){release_chain(*d,found->second,abandoned);d->commands.erase(found);}
    }d->tracker.OnFreeCommandBuffers(pool,count,commands);fn(handle,pool,count,commands);
    lock.unlock();notify_abandoned(abandoned);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkResetCommandPool(VkDevice handle,VkCommandPool pool,VkCommandPoolResetFlags flags) {
    auto d=device_state(handle);if(!d){auto f=guide_fallback<PFN_vkResetCommandPool>("vkResetCommandPool");return f?f(handle,pool,flags):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkResetCommandPool>(*d,"vkResetCommandPool");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    std::unique_lock lock(d->mutex);DisposedRecordings abandoned;auto result=fn(handle,pool,flags);if(result==VK_SUCCESS) {
        for(auto& [command,chain]:d->commands)if(chain.pool==pool)release_chain(*d,chain,abandoned);
        d->tracker.OnResetPool(pool);
    }lock.unlock();notify_abandoned(abandoned);return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkDestroyCommandPool(VkDevice handle,VkCommandPool pool,
    const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);if(!d){auto f=guide_fallback<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");if(f)f(handle,pool,allocator);return;}auto fn=load<PFN_vkDestroyCommandPool>(*d,"vkDestroyCommandPool");if(!fn)return;
    std::unique_lock lock(d->mutex);DisposedRecordings abandoned;
    for(auto iterator=d->commands.begin();iterator!=d->commands.end();) {
        if(iterator->second.pool==pool){release_chain(*d,iterator->second,abandoned);iterator=d->commands.erase(iterator);}else ++iterator;
    }d->tracker.OnDestroyPool(pool);d->pools.erase(pool);fn(handle,pool,allocator);
    lock.unlock();notify_abandoned(abandoned);
}
std::vector<Segment> submitted_segments(DeviceState& d,uint32_t count,const VkSubmitInfo* submits) {
    std::vector<Segment> result;
    for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<submits[i].commandBufferCount;++j) {
        auto found=d.commands.find(submits[i].pCommandBuffers[j]);if(found==d.commands.end())continue;
        if(found->second.submitted&&!found->second.segments.empty())throw VkResult(VK_ERROR_FEATURE_NOT_PRESENT);
        result.insert(result.end(),found->second.segments.begin(),found->second.segments.end());
    }return result;
}
detail::Handoff packet_handoff(const Segment& segment) {
    return {segment.prefix,segment.suffix,segment.handoff.timeline,
        segment.handoff.suffix_signal?segment.handoff.timeline:VK_NULL_HANDLE,
        segment.handoff.prefix_signal,segment.handoff.suffix_wait,segment.handoff.suffix_signal};
}
void mark_submitted(const std::vector<Segment>& segments) noexcept {
    for(const auto& segment:segments)if(segment.lifetime) {
        auto expected=RecordingDisposition::recording;
        (void)segment.lifetime->disposition.compare_exchange_strong(expected,RecordingDisposition::submitted);
    }
}
void mark_original_submitted(CommandChain& chain,VkQueue queue) noexcept {
    chain.submitted=true;
    if(chain.receipt) {
        chain.receipt->submitted_queue.store(queue,std::memory_order_relaxed);
        chain.receipt->submission_serial.store(++submission_serial,std::memory_order_release);
        auto expected=RecordingDisposition::recording;
        (void)chain.receipt->disposition.compare_exchange_strong(expected,RecordingDisposition::submitted);
    }
}
void notify_submitted(VkQueue queue,const std::vector<Segment>& segments) noexcept {
    for(const auto& segment:segments)if(segment.lifetime&&
        segment.lifetime->disposition==RecordingDisposition::submitted&&
        !segment.lifetime->callback_delivered.exchange(true)&&segment.handoff.submitted)
        segment.handoff.submitted(queue,segment.handoff.submission_context.get());
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkQueueSubmit(VkQueue queue,uint32_t count,const VkSubmitInfo* submits,VkFence fence) {
    auto d=queue_device(queue);if(!d){auto f=guide_fallback<PFN_vkQueueSubmit>("vkQueueSubmit");return f?f(queue,count,submits,fence):VK_ERROR_INITIALIZATION_FAILED;}
    auto fn=load<PFN_vkQueueSubmit>(*d,"vkQueueSubmit");if(!fn)return VK_ERROR_INITIALIZATION_FAILED;
    try {
        std::unique_lock lock(d->mutex);auto segments=submitted_segments(*d,count,submits);
        std::vector<std::unique_ptr<detail::Submit1>> packets;auto current=submits;uint32_t current_count=count;
        for(const auto& segment:segments) {
            auto packet=std::make_unique<detail::Submit1>();packet->stage=segment.handoff.wait_stage;
            if(!packet->build(current_count,current,packet_handoff(segment)))return VK_ERROR_FEATURE_NOT_PRESENT;
            current_count=uint32_t(packet->submits.size());current=packet->submits.data();packets.push_back(std::move(packet));
        }auto result=fn(queue,current_count,current,fence);
        if(result==VK_SUCCESS) {
            for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<submits[i].commandBufferCount;++j)
                if(auto found=d->commands.find(submits[i].pCommandBuffers[j]);found!=d->commands.end())mark_original_submitted(found->second,queue);
            mark_submitted(segments);
        }
        // Evaluate may hold its transport-owner mutex while recording a split.
        // Callbacks acquire that owner only after dropping the command mutex,
        // so concurrent recording/submission cannot invert the lock order.
        lock.unlock();if(result==VK_SUCCESS)notify_submitted(queue,segments);
        return result;
    }catch(VkResult error){return error;}catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
VkResult submit2(PFN_vkQueueSubmit2 fn,VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence) {
    auto d=queue_device(queue);if(!d||!fn)return VK_ERROR_INITIALIZATION_FAILED;
    try {
        std::unique_lock lock(d->mutex);std::vector<Segment> segments;
        for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<submits[i].commandBufferInfoCount;++j) {
            auto found=d->commands.find(submits[i].pCommandBufferInfos[j].commandBuffer);if(found==d->commands.end())continue;
            if(found->second.submitted&&!found->second.segments.empty())return VK_ERROR_FEATURE_NOT_PRESENT;
            segments.insert(segments.end(),found->second.segments.begin(),found->second.segments.end());
        }
        std::vector<std::unique_ptr<detail::Submit2>> packets;auto current=submits;uint32_t current_count=count;
        for(const auto& segment:segments) {
            auto packet=std::make_unique<detail::Submit2>();
            if(!packet->build(current_count,current,packet_handoff(segment)))return VK_ERROR_FEATURE_NOT_PRESENT;
            current_count=uint32_t(packet->submits.size());current=packet->submits.data();packets.push_back(std::move(packet));
        }auto result=fn(queue,current_count,current,fence);
        if(result==VK_SUCCESS) {
            for(uint32_t i=0;i<count;++i)for(uint32_t j=0;j<submits[i].commandBufferInfoCount;++j)
                if(auto found=d->commands.find(submits[i].pCommandBufferInfos[j].commandBuffer);found!=d->commands.end())mark_original_submitted(found->second,queue);
            mark_submitted(segments);
        }
        lock.unlock();if(result==VK_SUCCESS)notify_submitted(queue,segments);
        return result;
    }catch(...){return VK_ERROR_OUT_OF_HOST_MEMORY;}
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkQueueSubmit2(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence) {
    auto d=queue_device(queue);if(d)return submit2(load<PFN_vkQueueSubmit2>(*d,"vkQueueSubmit2"),queue,count,submits,fence);auto f=guide_fallback<PFN_vkQueueSubmit2>("vkQueueSubmit2");return f?f(queue,count,submits,fence):VK_ERROR_INITIALIZATION_FAILED;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkQueueSubmit2KHR(VkQueue queue,uint32_t count,const VkSubmitInfo2* submits,VkFence fence) {
    auto d=queue_device(queue);if(d)return submit2(load<PFN_vkQueueSubmit2>(*d,"vkQueueSubmit2KHR"),queue,count,submits,fence);auto f=guide_fallback<PFN_vkQueueSubmit2>("vkQueueSubmit2KHR");return f?f(queue,count,submits,fence):VK_ERROR_INITIALIZATION_FAILED;
}

#include "vulkan_command_dispatch.inc"
}

PFN_vkVoidFunction unhooked_command(PFN_vkVoidFunction function) noexcept {
    auto& r=registry();std::scoped_lock lock(r.mutex);
    const auto found=r.entry_hooks.find(reinterpret_cast<void*>(function));
    return found==r.entry_hooks.end()?function:found->second.original<PFN_vkVoidFunction>();
}
unsigned install_command_entry_hooks(void* loader_module) noexcept {
    try {
        const char* names[]{"vkCreateCommandPool","vkAllocateCommandBuffers","vkBeginCommandBuffer",
            "vkEndCommandBuffer","vkResetCommandBuffer","vkFreeCommandBuffers","vkResetCommandPool",
            "vkDestroyCommandPool","vkQueueSubmit","vkQueueSubmit2","vkQueueSubmit2KHR",
#include "vulkan_command_entry_names.inc"
        };
        auto& r=registry();
        for(const auto* name:names) {
            const auto address=native_export_address(static_cast<HMODULE>(loader_module),name);
            if(!address)continue;
            const auto replacement=resolve_command(VK_NULL_HANDLE,name,reinterpret_cast<PFN_vkVoidFunction>(address));
            if(replacement==reinterpret_cast<PFN_vkVoidFunction>(address))continue;
            std::scoped_lock lock(r.mutex);
            auto* key=reinterpret_cast<void*>(address);if(r.entry_hooks.contains(key))continue;
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(replacement),SafetyHookInline::StartDisabled);
            if(hook)r.entry_hooks.emplace(key,std::move(hook));
        }
        std::scoped_lock lock(r.mutex);
        for(auto& [address,hook]:r.entry_hooks)(void)hook.enable();
        return static_cast<unsigned>(r.entry_hooks.size());
    }catch(...){return 0;}
}
bool initialize_commands(VkInstance instance,VkPhysicalDevice physical,VkDevice device,
    PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa) {
    if(!device||!gdpa)return false;auto d=std::make_shared<DeviceState>();
    d->instance=instance;d->physical=physical;d->device=device;d->gipa=gipa;d->gdpa=gdpa;
    vk_state::VulkanCmdFns fns{};
#define VK_REPLAY(field,name) fns.field=load<PFN_##name>(*d,#name)
    VK_REPLAY(CmdBindPipeline,vkCmdBindPipeline);VK_REPLAY(CmdBindDescriptorSets,vkCmdBindDescriptorSets);
    VK_REPLAY(CmdPushConstants,vkCmdPushConstants);VK_REPLAY(CmdSetViewport,vkCmdSetViewport);
    VK_REPLAY(CmdSetScissor,vkCmdSetScissor);VK_REPLAY(CmdBindVertexBuffers,vkCmdBindVertexBuffers);
    VK_REPLAY(CmdBindIndexBuffer,vkCmdBindIndexBuffer);VK_REPLAY(CmdPipelineBarrier,vkCmdPipelineBarrier);
    VK_REPLAY(CmdSetCullMode,vkCmdSetCullMode);VK_REPLAY(CmdSetFrontFace,vkCmdSetFrontFace);
    VK_REPLAY(CmdSetPrimitiveTopology,vkCmdSetPrimitiveTopology);VK_REPLAY(CmdSetDepthTestEnable,vkCmdSetDepthTestEnable);
    VK_REPLAY(CmdSetDepthWriteEnable,vkCmdSetDepthWriteEnable);VK_REPLAY(CmdSetDepthCompareOp,vkCmdSetDepthCompareOp);
    VK_REPLAY(CmdSetDepthBoundsTestEnable,vkCmdSetDepthBoundsTestEnable);VK_REPLAY(CmdSetStencilTestEnable,vkCmdSetStencilTestEnable);
    VK_REPLAY(CmdSetStencilOp,vkCmdSetStencilOp);
#undef VK_REPLAY
    d->tracker.SetFunctionTable(fns);auto& r=registry();std::scoped_lock lock(r.mutex);
    if(r.devices.contains(device))return true;r.devices[device]=d;r.dispatch[dispatch_key(device)]=d;return true;
}
PFN_vkVoidFunction resolve_command(VkDevice device,const char* name,PFN_vkVoidFunction original) noexcept {
    if(!name||!original)return original;
    // GIPA may resolve every device command before VkDevice exists. The
    // wrapper routes each later call by its real device/command dispatch key;
    // downstream functions remain device-specific, loaded only after creation.
    if(device) {
        auto d=device_state(device);if(!d)return original;
        std::scoped_lock lock(d->mutex);d->functions[name]=original;
    }
#define VK_RESOLVE(name_literal) if(!strcmp(name,#name_literal))return reinterpret_cast<PFN_vkVoidFunction>(&hook_##name_literal)
    VK_RESOLVE(vkCreateCommandPool);VK_RESOLVE(vkAllocateCommandBuffers);VK_RESOLVE(vkBeginCommandBuffer);
    VK_RESOLVE(vkEndCommandBuffer);VK_RESOLVE(vkResetCommandBuffer);VK_RESOLVE(vkFreeCommandBuffers);
    VK_RESOLVE(vkResetCommandPool);VK_RESOLVE(vkDestroyCommandPool);VK_RESOLVE(vkQueueSubmit);
    VK_RESOLVE(vkQueueSubmit2);VK_RESOLVE(vkQueueSubmit2KHR);
#undef VK_RESOLVE
#include "vulkan_command_resolve.inc"
    return original;
}
VkCommandBuffer native(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return original;std::scoped_lock lock(d->mutex);return native_locked(*d,original);
}
VkDevice device_for(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return {};
    std::scoped_lock lock(d->mutex);original_locked(*d,original);return d->commands.contains(original)?d->device:VK_NULL_HANDLE;
}
uint32_t queue_family_for(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return UINT32_MAX;
    std::scoped_lock lock(d->mutex);original_locked(*d,original);auto command=d->commands.find(original);
    if(command==d->commands.end())return UINT32_MAX;
    const auto pool=d->pools.find(command->second.pool);return pool==d->pools.end()?UINT32_MAX:pool->second;
}
bool can_split(VkCommandBuffer original) noexcept {
    return split_refusal(original)==0;
}
unsigned split_refusal(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return 1;
    std::scoped_lock lock(d->mutex);original_locked(*d,original);auto found=d->commands.find(original);
    if(found==d->commands.end())return 2;const auto& chain=found->second;
    vk_state::CommandBufferState snapshot;
    return (!chain.recording?4U:0U)|(!chain.pool?8U:0U)|
        (chain.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY?16U:0U)|
        (chain.rendering?32U:0U)|(chain.unsupported_state?64U:0U)|
        (chain.submitted?128U:0U)|(!d->tracker.Snapshot(original,snapshot)?256U:0U);
}
bool retain_recording(VkCommandBuffer original,std::shared_ptr<void> resources) noexcept {
    if(!resources)return false;auto d=command_device(original);if(!d)return false;
    try {
        std::scoped_lock lock(d->mutex);original_locked(*d,original);auto found=d->commands.find(original);
        if(found==d->commands.end()||!found->second.recording||found->second.submitted)return false;
        found->second.retained.push_back(std::move(resources));return true;
    }catch(...){return false;}
}
std::shared_ptr<RecordingReceipt> recording_receipt(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return {};
    try {
        std::scoped_lock lock(d->mutex);original_locked(*d,original);
        const auto found=d->commands.find(original);if(found==d->commands.end())return {};
        auto& chain=found->second;
        if(!chain.receipt) {
            if(!chain.recording||chain.submitted||!chain.epoch)return {};
            auto receipt=std::make_shared<RecordingReceipt>();receipt->epoch=chain.epoch;
            chain.receipt=std::move(receipt);
        }
        return chain.receipt;
    }catch(...){return {};}
}
uint64_t guide_submission_cutoff() noexcept {return submission_serial.load(std::memory_order_acquire);}
std::shared_ptr<RecordingReceipt> guide_recording_receipt(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return {};
    {std::scoped_lock lock(d->mutex);auto it=d->commands.find(original);
     if(it==d->commands.end()||!it->second.recording||it->second.submitted||
        it->second.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||
        !(it->second.flags&VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT))return {};}
    return recording_receipt(original);
}
bool initialize_guide_commands(VkInstance instance,VkPhysicalDevice physical,VkDevice device,
    PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa) noexcept {
    try {
        if(!initialize_commands(instance,physical,device,gipa,gdpa))return false;
        const char* names[]{"vkCreateCommandPool","vkAllocateCommandBuffers","vkBeginCommandBuffer",
            "vkEndCommandBuffer","vkResetCommandBuffer","vkFreeCommandBuffers","vkResetCommandPool",
            "vkDestroyCommandPool","vkQueueSubmit","vkQueueSubmit2","vkQueueSubmit2KHR"};
        auto& r=registry();
        for(const char* name:names) {
            const auto address=gdpa(device,name);if(!address)continue;
            const auto replacement=resolve_command(device,name,address);
            if(address==replacement)return false;
            std::scoped_lock lock(r.mutex);auto* key=reinterpret_cast<void*>(address);
            if(const auto found=r.guide_fallbacks.find(name);found!=r.guide_fallbacks.end()&&found->second!=address)return false;
            r.guide_fallbacks[name]=address;
            if(r.entry_hooks.contains(key))continue;
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(replacement),SafetyHookInline::StartDisabled);
            if(!hook)return false;r.entry_hooks.emplace(key,std::move(hook));
        }
        std::scoped_lock lock(r.mutex);for(auto& [address,hook]:r.entry_hooks)(void)hook.enable();
        return true;
    }catch(...){return false;}
}
CommandSplit split(VkCommandBuffer original,const CommandHandoff& handoff,
    RecordCopyback copyback,void* context) noexcept {
    CommandSplit result;auto d=command_device(original);if(!d||!handoff.timeline||!handoff.prefix_signal||
        handoff.suffix_wait<=handoff.prefix_signal||
        (handoff.suffix_signal&&handoff.suffix_signal<=handoff.suffix_wait))return result;
    try {
        std::scoped_lock lock(d->mutex);original_locked(*d,original);auto found=d->commands.find(original);if(found==d->commands.end())return result;
        auto& chain=found->second;vk_state::CommandBufferState snapshot;
        if(!chain.recording||!chain.pool||chain.level!=VK_COMMAND_BUFFER_LEVEL_PRIMARY||chain.rendering||
            chain.unsupported_state||chain.submitted||!d->tracker.Snapshot(original,snapshot))return result;
        auto allocate=load<PFN_vkAllocateCommandBuffers>(*d,"vkAllocateCommandBuffers");
        auto begin=load<PFN_vkBeginCommandBuffer>(*d,"vkBeginCommandBuffer");
        auto end=load<PFN_vkEndCommandBuffer>(*d,"vkEndCommandBuffer");
        auto free=load<PFN_vkFreeCommandBuffers>(*d,"vkFreeCommandBuffers");
        if(!allocate||!begin||!end||!free)return result;
        // Finish fallible ownership allocation before ending the application's
        // prefix. Committing a successful split cannot then strand an ended
        // original because a vector allocation or shared token failed.
        chain.segments.reserve(chain.segments.size()+1);
        auto lifetime=std::make_shared<RecordingLifetime>();lifetime->epoch=chain.epoch;
        VkCommandBuffer suffix{};VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool=chain.pool;allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocation.commandBufferCount=1;
        result.result=allocate(d->device,&allocation,&suffix);if(result.result!=VK_SUCCESS)return result;
        UncommittedCommand unused{d->device,chain.pool,suffix,free};
        VkCommandBufferBeginInfo begun{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begun.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result.result=begin(suffix,&begun);
        if(result.result==VK_SUCCESS&&copyback)result.result=copyback(suffix,context);
        vk_state::ReplayParams replay;replay.RequiredGraphicsSetMask=~uint32_t{};
        replay.ReplayComputeToo=true;replay.ReplayVertexIndex=true;
        if(result.result==VK_SUCCESS&&!d->tracker.CaptureAndReplay(original,suffix,replay))result.result=VK_ERROR_FEATURE_NOT_PRESENT;
        if(result.result==VK_SUCCESS)for(const auto& state:chain.state_commands)state(suffix);
        // Allocate alias ownership before ending the prefix. A failed driver
        // end leaves no alias pointing at an uncommitted/freed continuation.
        if(result.result==VK_SUCCESS)d->continuation_owners.emplace(suffix,original);
        if(result.result==VK_SUCCESS)result.result=end(chain.actual?chain.actual:original);
        if(result.result!=VK_SUCCESS)d->continuation_owners.erase(suffix);
        if(result.result!=VK_SUCCESS)return result;
        chain.segments.push_back({chain.actual?chain.actual:original,suffix,handoff,lifetime});chain.actual=suffix;
        unused.value={};
        result.continuation=suffix;result.recording_epoch=chain.epoch;result.lifetime=std::move(lifetime);return result;
    }catch(...){result.result=VK_ERROR_OUT_OF_HOST_MEMORY;return result;}
}
uint64_t recording_epoch(VkCommandBuffer original) noexcept {
    auto d=command_device(original);if(!d)return 0;std::scoped_lock lock(d->mutex);
    original_locked(*d,original);auto found=d->commands.find(original);return found==d->commands.end()?0:found->second.epoch;
}
void destroy_commands(VkDevice device) noexcept {
    auto d=device_state(device);if(!d)return;
    DisposedRecordings abandoned;
    {std::scoped_lock lock(d->mutex);for(auto& [original,chain]:d->commands)release_chain(*d,chain,abandoned);d->commands.clear();}
    {
        auto& r=registry();std::scoped_lock lock(r.mutex);r.dispatch.erase(dispatch_key(device));r.devices.erase(device);
        for(auto iterator=r.commands.begin();iterator!=r.commands.end();) {
            if(iterator->second.lock()==d)iterator=r.commands.erase(iterator);else ++iterator;
        }
    }
    notify_abandoned(abandoned);
}
}
