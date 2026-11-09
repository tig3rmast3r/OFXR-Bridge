// Exact original entry ownership for cached native device dispatch.
struct CameraEntry {const char* name{};void* address{};PFN_vkVoidFunction original{};SafetyHookInline hook;bool active{};};
std::unordered_map<void*,CameraEntry*> camera_entry_addresses;
thread_local unsigned camera_entry_failure{};
std::array<CameraEntry,8> camera_vkAllocateMemory_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkAllocateMemory(VkDevice handle,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* memory) {
    const auto& entry=camera_vkAllocateMemory_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkAllocateMemory(handle,info,allocator,memory);
}
std::array<CameraEntry,8> camera_vkFreeMemory_entries;
template<unsigned I>void VKAPI_CALL camera_vkFreeMemory(VkDevice handle,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    const auto& entry=camera_vkFreeMemory_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkFreeMemory(handle,memory,allocator);
}
std::array<CameraEntry,8> camera_vkMapMemory_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkMapMemory(VkDevice handle,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data) {
    const auto& entry=camera_vkMapMemory_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkMapMemory(handle,memory,offset,size,flags,data);
}
std::array<CameraEntry,8> camera_vkUnmapMemory_entries;
template<unsigned I>void VKAPI_CALL camera_vkUnmapMemory(VkDevice handle,VkDeviceMemory memory) {
    const auto& entry=camera_vkUnmapMemory_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkUnmapMemory(handle,memory);
}
std::array<CameraEntry,8> camera_vkCreateBuffer_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkCreateBuffer(VkDevice handle,const VkBufferCreateInfo* info,const VkAllocationCallbacks* allocator,VkBuffer* buffer) {
    const auto& entry=camera_vkCreateBuffer_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkCreateBuffer(handle,info,allocator,buffer);
}
std::array<CameraEntry,8> camera_vkDestroyBuffer_entries;
template<unsigned I>void VKAPI_CALL camera_vkDestroyBuffer(VkDevice handle,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    const auto& entry=camera_vkDestroyBuffer_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkDestroyBuffer(handle,buffer,allocator);
}
std::array<CameraEntry,8> camera_vkBindBufferMemory_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkBindBufferMemory(VkDevice handle,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) {
    const auto& entry=camera_vkBindBufferMemory_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkBindBufferMemory(handle,buffer,memory,offset);
}
std::array<CameraEntry,8> camera_vkBindBufferMemory2_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkBindBufferMemory2(VkDevice handle,uint32_t count,const VkBindBufferMemoryInfo* bindings) {
    const auto& entry=camera_vkBindBufferMemory2_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkBindBufferMemory2(handle,count,bindings);
}
std::array<CameraEntry,8> camera_vkUpdateDescriptorSets_entries;
template<unsigned I>void VKAPI_CALL camera_vkUpdateDescriptorSets(VkDevice handle,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy) {
    const auto& entry=camera_vkUpdateDescriptorSets_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkUpdateDescriptorSets(handle,count,writes,copies,copy);
}
std::array<CameraEntry,8> camera_vkAllocateDescriptorSets_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkAllocateDescriptorSets(VkDevice handle,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* sets) {
    const auto& entry=camera_vkAllocateDescriptorSets_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkAllocateDescriptorSets(handle,info,sets);
}
std::array<CameraEntry,8> camera_vkFreeDescriptorSets_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkFreeDescriptorSets(VkDevice handle,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets) {
    const auto& entry=camera_vkFreeDescriptorSets_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkFreeDescriptorSets(handle,pool,count,sets);
}
std::array<CameraEntry,8> camera_vkResetDescriptorPool_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkResetDescriptorPool(VkDevice handle,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    const auto& entry=camera_vkResetDescriptorPool_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkResetDescriptorPool(handle,pool,flags);
}
std::array<CameraEntry,8> camera_vkDestroyDescriptorPool_entries;
template<unsigned I>void VKAPI_CALL camera_vkDestroyDescriptorPool(VkDevice handle,VkDescriptorPool pool,const VkAllocationCallbacks* allocator) {
    const auto& entry=camera_vkDestroyDescriptorPool_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkDestroyDescriptorPool(handle,pool,allocator);
}
std::array<CameraEntry,8> camera_vkCreateShaderModule_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkCreateShaderModule(VkDevice handle,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* allocator,VkShaderModule* shader) {
    const auto& entry=camera_vkCreateShaderModule_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkCreateShaderModule(handle,info,allocator,shader);
}
std::array<CameraEntry,8> camera_vkDestroyShaderModule_entries;
template<unsigned I>void VKAPI_CALL camera_vkDestroyShaderModule(VkDevice handle,VkShaderModule shader,const VkAllocationCallbacks* allocator) {
    const auto& entry=camera_vkDestroyShaderModule_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkDestroyShaderModule(handle,shader,allocator);
}
std::array<CameraEntry,8> camera_vkCreateComputePipelines_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkCreateComputePipelines(VkDevice handle,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* info,const VkAllocationCallbacks* allocator,VkPipeline* pipelines) {
    const auto& entry=camera_vkCreateComputePipelines_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkCreateComputePipelines(handle,cache,count,info,allocator,pipelines);
}
std::array<CameraEntry,8> camera_vkDestroyPipeline_entries;
template<unsigned I>void VKAPI_CALL camera_vkDestroyPipeline(VkDevice handle,VkPipeline pipeline,const VkAllocationCallbacks* allocator) {
    const auto& entry=camera_vkDestroyPipeline_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkDestroyPipeline(handle,pipeline,allocator);
}
std::array<CameraEntry,8> camera_vkBindBufferMemory2KHR_entries;
template<unsigned I>VkResult VKAPI_CALL camera_vkBindBufferMemory2KHR(VkDevice handle,uint32_t count,const VkBindBufferMemoryInfo* bindings) {
    const auto& entry=camera_vkBindBufferMemory2KHR_entries[I];CameraEntryScope scope(entry.name,entry.original);
    return hook_vkBindBufferMemory2KHR(handle,count,bindings);
}
std::array<CameraEntry,8> camera_vkCmdBindPipeline_entries;
template<unsigned I>void VKAPI_CALL camera_vkCmdBindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
    const auto& entry=camera_vkCmdBindPipeline_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkCmdBindPipeline(commandBuffer,pipelineBindPoint,pipeline);
}
std::array<CameraEntry,8> camera_vkCmdBindDescriptorSets_entries;
template<unsigned I>void VKAPI_CALL camera_vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets) {
    const auto& entry=camera_vkCmdBindDescriptorSets_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkCmdBindDescriptorSets(commandBuffer,pipelineBindPoint,layout,firstSet,descriptorSetCount,pDescriptorSets,dynamicOffsetCount,pDynamicOffsets);
}
std::array<CameraEntry,8> camera_vkCmdDispatch_entries;
template<unsigned I>void VKAPI_CALL camera_vkCmdDispatch(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    const auto& entry=camera_vkCmdDispatch_entries[I];CameraEntryScope scope(entry.name,entry.original);
    hook_vkCmdDispatch(commandBuffer,groupCountX,groupCountY,groupCountZ);
}
bool install_camera_native_entry(const char* name,PFN_vkVoidFunction address) {
    camera_entry_failure=0;if(!address){camera_entry_failure=1;return false;}
    std::scoped_lock lock(camera_entry_mutex);
    auto* key=reinterpret_cast<void*>(address);
    if(auto found=camera_entry_addresses.find(key);found!=camera_entry_addresses.end()) {
        auto& entry=*found->second;
        if(entry.name&&(!strcmp(name,entry.name)||
            ((!strcmp(name,"vkBindBufferMemory2")||!strcmp(name,"vkBindBufferMemory2KHR"))&&
             (!strcmp(entry.name,"vkBindBufferMemory2")||!strcmp(entry.name,"vkBindBufferMemory2KHR")))))return entry.active;
        // A shared unsupported stub (or an ambiguous cross-API dispatcher)
        // carries no typed operation identity. Restore it instead of routing
        // a different Vulkan signature through the first observer installed.
        if(entry.hook)(void)entry.hook.disable();entry.active=false;entry.name=nullptr;camera_entry_failure=2;return false;
    }
    if(!strcmp(name,"vkAllocateMemory")) {
        const std::array<PFN_vkAllocateMemory,8> targets{&camera_vkAllocateMemory<0>,&camera_vkAllocateMemory<1>,&camera_vkAllocateMemory<2>,&camera_vkAllocateMemory<3>,&camera_vkAllocateMemory<4>,&camera_vkAllocateMemory<5>,&camera_vkAllocateMemory<6>,&camera_vkAllocateMemory<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkAllocateMemory_entries[i].address) {
            auto& entry=camera_vkAllocateMemory_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkFreeMemory")) {
        const std::array<PFN_vkFreeMemory,8> targets{&camera_vkFreeMemory<0>,&camera_vkFreeMemory<1>,&camera_vkFreeMemory<2>,&camera_vkFreeMemory<3>,&camera_vkFreeMemory<4>,&camera_vkFreeMemory<5>,&camera_vkFreeMemory<6>,&camera_vkFreeMemory<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkFreeMemory_entries[i].address) {
            auto& entry=camera_vkFreeMemory_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkMapMemory")) {
        const std::array<PFN_vkMapMemory,8> targets{&camera_vkMapMemory<0>,&camera_vkMapMemory<1>,&camera_vkMapMemory<2>,&camera_vkMapMemory<3>,&camera_vkMapMemory<4>,&camera_vkMapMemory<5>,&camera_vkMapMemory<6>,&camera_vkMapMemory<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkMapMemory_entries[i].address) {
            auto& entry=camera_vkMapMemory_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkUnmapMemory")) {
        const std::array<PFN_vkUnmapMemory,8> targets{&camera_vkUnmapMemory<0>,&camera_vkUnmapMemory<1>,&camera_vkUnmapMemory<2>,&camera_vkUnmapMemory<3>,&camera_vkUnmapMemory<4>,&camera_vkUnmapMemory<5>,&camera_vkUnmapMemory<6>,&camera_vkUnmapMemory<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkUnmapMemory_entries[i].address) {
            auto& entry=camera_vkUnmapMemory_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCreateBuffer")) {
        const std::array<PFN_vkCreateBuffer,8> targets{&camera_vkCreateBuffer<0>,&camera_vkCreateBuffer<1>,&camera_vkCreateBuffer<2>,&camera_vkCreateBuffer<3>,&camera_vkCreateBuffer<4>,&camera_vkCreateBuffer<5>,&camera_vkCreateBuffer<6>,&camera_vkCreateBuffer<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCreateBuffer_entries[i].address) {
            auto& entry=camera_vkCreateBuffer_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkDestroyBuffer")) {
        const std::array<PFN_vkDestroyBuffer,8> targets{&camera_vkDestroyBuffer<0>,&camera_vkDestroyBuffer<1>,&camera_vkDestroyBuffer<2>,&camera_vkDestroyBuffer<3>,&camera_vkDestroyBuffer<4>,&camera_vkDestroyBuffer<5>,&camera_vkDestroyBuffer<6>,&camera_vkDestroyBuffer<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkDestroyBuffer_entries[i].address) {
            auto& entry=camera_vkDestroyBuffer_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkBindBufferMemory")) {
        const std::array<PFN_vkBindBufferMemory,8> targets{&camera_vkBindBufferMemory<0>,&camera_vkBindBufferMemory<1>,&camera_vkBindBufferMemory<2>,&camera_vkBindBufferMemory<3>,&camera_vkBindBufferMemory<4>,&camera_vkBindBufferMemory<5>,&camera_vkBindBufferMemory<6>,&camera_vkBindBufferMemory<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkBindBufferMemory_entries[i].address) {
            auto& entry=camera_vkBindBufferMemory_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkBindBufferMemory2")) {
        const std::array<PFN_vkBindBufferMemory2,8> targets{&camera_vkBindBufferMemory2<0>,&camera_vkBindBufferMemory2<1>,&camera_vkBindBufferMemory2<2>,&camera_vkBindBufferMemory2<3>,&camera_vkBindBufferMemory2<4>,&camera_vkBindBufferMemory2<5>,&camera_vkBindBufferMemory2<6>,&camera_vkBindBufferMemory2<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkBindBufferMemory2_entries[i].address) {
            auto& entry=camera_vkBindBufferMemory2_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkUpdateDescriptorSets")) {
        const std::array<PFN_vkUpdateDescriptorSets,8> targets{&camera_vkUpdateDescriptorSets<0>,&camera_vkUpdateDescriptorSets<1>,&camera_vkUpdateDescriptorSets<2>,&camera_vkUpdateDescriptorSets<3>,&camera_vkUpdateDescriptorSets<4>,&camera_vkUpdateDescriptorSets<5>,&camera_vkUpdateDescriptorSets<6>,&camera_vkUpdateDescriptorSets<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkUpdateDescriptorSets_entries[i].address) {
            auto& entry=camera_vkUpdateDescriptorSets_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkAllocateDescriptorSets")) {
        const std::array<PFN_vkAllocateDescriptorSets,8> targets{&camera_vkAllocateDescriptorSets<0>,&camera_vkAllocateDescriptorSets<1>,&camera_vkAllocateDescriptorSets<2>,&camera_vkAllocateDescriptorSets<3>,&camera_vkAllocateDescriptorSets<4>,&camera_vkAllocateDescriptorSets<5>,&camera_vkAllocateDescriptorSets<6>,&camera_vkAllocateDescriptorSets<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkAllocateDescriptorSets_entries[i].address) {
            auto& entry=camera_vkAllocateDescriptorSets_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkFreeDescriptorSets")) {
        const std::array<PFN_vkFreeDescriptorSets,8> targets{&camera_vkFreeDescriptorSets<0>,&camera_vkFreeDescriptorSets<1>,&camera_vkFreeDescriptorSets<2>,&camera_vkFreeDescriptorSets<3>,&camera_vkFreeDescriptorSets<4>,&camera_vkFreeDescriptorSets<5>,&camera_vkFreeDescriptorSets<6>,&camera_vkFreeDescriptorSets<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkFreeDescriptorSets_entries[i].address) {
            auto& entry=camera_vkFreeDescriptorSets_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkResetDescriptorPool")) {
        const std::array<PFN_vkResetDescriptorPool,8> targets{&camera_vkResetDescriptorPool<0>,&camera_vkResetDescriptorPool<1>,&camera_vkResetDescriptorPool<2>,&camera_vkResetDescriptorPool<3>,&camera_vkResetDescriptorPool<4>,&camera_vkResetDescriptorPool<5>,&camera_vkResetDescriptorPool<6>,&camera_vkResetDescriptorPool<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkResetDescriptorPool_entries[i].address) {
            auto& entry=camera_vkResetDescriptorPool_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkDestroyDescriptorPool")) {
        const std::array<PFN_vkDestroyDescriptorPool,8> targets{&camera_vkDestroyDescriptorPool<0>,&camera_vkDestroyDescriptorPool<1>,&camera_vkDestroyDescriptorPool<2>,&camera_vkDestroyDescriptorPool<3>,&camera_vkDestroyDescriptorPool<4>,&camera_vkDestroyDescriptorPool<5>,&camera_vkDestroyDescriptorPool<6>,&camera_vkDestroyDescriptorPool<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkDestroyDescriptorPool_entries[i].address) {
            auto& entry=camera_vkDestroyDescriptorPool_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCreateShaderModule")) {
        const std::array<PFN_vkCreateShaderModule,8> targets{&camera_vkCreateShaderModule<0>,&camera_vkCreateShaderModule<1>,&camera_vkCreateShaderModule<2>,&camera_vkCreateShaderModule<3>,&camera_vkCreateShaderModule<4>,&camera_vkCreateShaderModule<5>,&camera_vkCreateShaderModule<6>,&camera_vkCreateShaderModule<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCreateShaderModule_entries[i].address) {
            auto& entry=camera_vkCreateShaderModule_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkDestroyShaderModule")) {
        const std::array<PFN_vkDestroyShaderModule,8> targets{&camera_vkDestroyShaderModule<0>,&camera_vkDestroyShaderModule<1>,&camera_vkDestroyShaderModule<2>,&camera_vkDestroyShaderModule<3>,&camera_vkDestroyShaderModule<4>,&camera_vkDestroyShaderModule<5>,&camera_vkDestroyShaderModule<6>,&camera_vkDestroyShaderModule<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkDestroyShaderModule_entries[i].address) {
            auto& entry=camera_vkDestroyShaderModule_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCreateComputePipelines")) {
        const std::array<PFN_vkCreateComputePipelines,8> targets{&camera_vkCreateComputePipelines<0>,&camera_vkCreateComputePipelines<1>,&camera_vkCreateComputePipelines<2>,&camera_vkCreateComputePipelines<3>,&camera_vkCreateComputePipelines<4>,&camera_vkCreateComputePipelines<5>,&camera_vkCreateComputePipelines<6>,&camera_vkCreateComputePipelines<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCreateComputePipelines_entries[i].address) {
            auto& entry=camera_vkCreateComputePipelines_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkDestroyPipeline")) {
        const std::array<PFN_vkDestroyPipeline,8> targets{&camera_vkDestroyPipeline<0>,&camera_vkDestroyPipeline<1>,&camera_vkDestroyPipeline<2>,&camera_vkDestroyPipeline<3>,&camera_vkDestroyPipeline<4>,&camera_vkDestroyPipeline<5>,&camera_vkDestroyPipeline<6>,&camera_vkDestroyPipeline<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkDestroyPipeline_entries[i].address) {
            auto& entry=camera_vkDestroyPipeline_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkBindBufferMemory2KHR")) {
        const std::array<PFN_vkBindBufferMemory2KHR,8> targets{&camera_vkBindBufferMemory2KHR<0>,&camera_vkBindBufferMemory2KHR<1>,&camera_vkBindBufferMemory2KHR<2>,&camera_vkBindBufferMemory2KHR<3>,&camera_vkBindBufferMemory2KHR<4>,&camera_vkBindBufferMemory2KHR<5>,&camera_vkBindBufferMemory2KHR<6>,&camera_vkBindBufferMemory2KHR<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkBindBufferMemory2KHR_entries[i].address) {
            auto& entry=camera_vkBindBufferMemory2KHR_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCmdBindPipeline")) {
        const std::array<PFN_vkCmdBindPipeline,8> targets{&camera_vkCmdBindPipeline<0>,&camera_vkCmdBindPipeline<1>,&camera_vkCmdBindPipeline<2>,&camera_vkCmdBindPipeline<3>,&camera_vkCmdBindPipeline<4>,&camera_vkCmdBindPipeline<5>,&camera_vkCmdBindPipeline<6>,&camera_vkCmdBindPipeline<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCmdBindPipeline_entries[i].address) {
            auto& entry=camera_vkCmdBindPipeline_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCmdBindDescriptorSets")) {
        const std::array<PFN_vkCmdBindDescriptorSets,8> targets{&camera_vkCmdBindDescriptorSets<0>,&camera_vkCmdBindDescriptorSets<1>,&camera_vkCmdBindDescriptorSets<2>,&camera_vkCmdBindDescriptorSets<3>,&camera_vkCmdBindDescriptorSets<4>,&camera_vkCmdBindDescriptorSets<5>,&camera_vkCmdBindDescriptorSets<6>,&camera_vkCmdBindDescriptorSets<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCmdBindDescriptorSets_entries[i].address) {
            auto& entry=camera_vkCmdBindDescriptorSets_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    if(!strcmp(name,"vkCmdDispatch")) {
        const std::array<PFN_vkCmdDispatch,8> targets{&camera_vkCmdDispatch<0>,&camera_vkCmdDispatch<1>,&camera_vkCmdDispatch<2>,&camera_vkCmdDispatch<3>,&camera_vkCmdDispatch<4>,&camera_vkCmdDispatch<5>,&camera_vkCmdDispatch<6>,&camera_vkCmdDispatch<7>};
        for(unsigned i=0;i<8;++i)if(!camera_vkCmdDispatch_entries[i].address) {
            auto& entry=camera_vkCmdDispatch_entries[i];
            auto hook=safetyhook::create_inline(key,reinterpret_cast<void*>(targets[i]),SafetyHookInline::StartDisabled);if(!hook){camera_entry_failure=3;return false;}
            HMODULE retained{};if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(key),&retained)){camera_entry_failure=4;return false;}
            entry.name=name;entry.address=key;entry.original=hook.original<PFN_vkVoidFunction>();entry.hook=std::move(hook);
            camera_entry_addresses[key]=&entry;entry.active=bool(entry.hook.enable());if(!entry.active)camera_entry_failure=5;return entry.active;
        }
        camera_entry_failure=6;return false;
    }
    return false;
}
