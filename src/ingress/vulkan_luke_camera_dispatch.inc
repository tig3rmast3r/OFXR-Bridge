// Read-only REAL VR Vulkan camera resource observations.
VKAPI_ATTR VkResult VKAPI_CALL hook_vkAllocateMemory(VkDevice handle,const VkMemoryAllocateInfo* info,const VkAllocationCallbacks* allocator,VkDeviceMemory* memory) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkAllocateMemory>(*d,"vkAllocateMemory"):guide_fallback<PFN_vkAllocateMemory>("vkAllocateMemory");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,info,allocator,memory);
    if(result==VK_SUCCESS&&info&&memory)luke_camera::allocated(handle,*memory,info->allocationSize);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkFreeMemory(VkDevice handle,VkDeviceMemory memory,const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkFreeMemory>(*d,"vkFreeMemory"):guide_fallback<PFN_vkFreeMemory>("vkFreeMemory");
    if(!fn){return;}
    fn(handle,memory,allocator);
    luke_camera::freed(handle,memory);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkMapMemory(VkDevice handle,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,VkMemoryMapFlags flags,void** data) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkMapMemory>(*d,"vkMapMemory"):guide_fallback<PFN_vkMapMemory>("vkMapMemory");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,memory,offset,size,flags,data);
    if(result==VK_SUCCESS&&data)luke_camera::mapped(handle,memory,offset,size,*data);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkUnmapMemory(VkDevice handle,VkDeviceMemory memory) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkUnmapMemory>(*d,"vkUnmapMemory"):guide_fallback<PFN_vkUnmapMemory>("vkUnmapMemory");
    if(!fn){return;}
    luke_camera::unmapped(handle,memory);
    fn(handle,memory);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateBuffer(VkDevice handle,const VkBufferCreateInfo* info,const VkAllocationCallbacks* allocator,VkBuffer* buffer) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkCreateBuffer>(*d,"vkCreateBuffer"):guide_fallback<PFN_vkCreateBuffer>("vkCreateBuffer");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,info,allocator,buffer);
    if(result==VK_SUCCESS&&info&&buffer)luke_camera::buffer_created(handle,*buffer,*info);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkDestroyBuffer(VkDevice handle,VkBuffer buffer,const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkDestroyBuffer>(*d,"vkDestroyBuffer"):guide_fallback<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    if(!fn){return;}
    fn(handle,buffer,allocator);
    luke_camera::buffer_destroyed(handle,buffer);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkBindBufferMemory(VkDevice handle,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkBindBufferMemory>(*d,"vkBindBufferMemory"):guide_fallback<PFN_vkBindBufferMemory>("vkBindBufferMemory");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,buffer,memory,offset);
    if(result==VK_SUCCESS)luke_camera::buffer_bound(handle,buffer,memory,offset);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkBindBufferMemory2(VkDevice handle,uint32_t count,const VkBindBufferMemoryInfo* bindings) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkBindBufferMemory2>(*d,"vkBindBufferMemory2"):guide_fallback<PFN_vkBindBufferMemory2>("vkBindBufferMemory2");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,count,bindings);
    if(result==VK_SUCCESS&&bindings)for(unsigned i=0;i<count;++i)luke_camera::buffer_bound(handle,bindings[i].buffer,bindings[i].memory,bindings[i].memoryOffset);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkUpdateDescriptorSets(VkDevice handle,uint32_t count,const VkWriteDescriptorSet* writes,uint32_t copies,const VkCopyDescriptorSet* copy) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkUpdateDescriptorSets>(*d,"vkUpdateDescriptorSets"):guide_fallback<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    if(!fn){return;}
    fn(handle,count,writes,copies,copy);
    luke_camera::descriptors_written(handle,count,writes,copies,copy);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkAllocateDescriptorSets(VkDevice handle,const VkDescriptorSetAllocateInfo* info,VkDescriptorSet* sets) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkAllocateDescriptorSets>(*d,"vkAllocateDescriptorSets"):guide_fallback<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,info,sets);
    if(result==VK_SUCCESS&&info&&sets)luke_camera::descriptors_allocated(handle,*info,sets);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkFreeDescriptorSets(VkDevice handle,VkDescriptorPool pool,uint32_t count,const VkDescriptorSet* sets) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkFreeDescriptorSets>(*d,"vkFreeDescriptorSets"):guide_fallback<PFN_vkFreeDescriptorSets>("vkFreeDescriptorSets");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,pool,count,sets);
    if(result==VK_SUCCESS)luke_camera::descriptors_freed(handle,count,sets);
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkResetDescriptorPool(VkDevice handle,VkDescriptorPool pool,VkDescriptorPoolResetFlags flags) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkResetDescriptorPool>(*d,"vkResetDescriptorPool"):guide_fallback<PFN_vkResetDescriptorPool>("vkResetDescriptorPool");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,pool,flags);
    if(result==VK_SUCCESS)luke_camera::pool_reset(handle,pool);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkDestroyDescriptorPool(VkDevice handle,VkDescriptorPool pool,const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkDestroyDescriptorPool>(*d,"vkDestroyDescriptorPool"):guide_fallback<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool");
    if(!fn){return;}
    fn(handle,pool,allocator);
    luke_camera::pool_reset(handle,pool);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateShaderModule(VkDevice handle,const VkShaderModuleCreateInfo* info,const VkAllocationCallbacks* allocator,VkShaderModule* shader) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkCreateShaderModule>(*d,"vkCreateShaderModule"):guide_fallback<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,info,allocator,shader);
    if(result==VK_SUCCESS&&info&&shader)luke_camera::shader_created(handle,*shader,*info);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkDestroyShaderModule(VkDevice handle,VkShaderModule shader,const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkDestroyShaderModule>(*d,"vkDestroyShaderModule"):guide_fallback<PFN_vkDestroyShaderModule>("vkDestroyShaderModule");
    if(!fn){return;}
    fn(handle,shader,allocator);
    luke_camera::shader_destroyed(handle,shader);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkCreateComputePipelines(VkDevice handle,VkPipelineCache cache,uint32_t count,const VkComputePipelineCreateInfo* info,const VkAllocationCallbacks* allocator,VkPipeline* pipelines) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkCreateComputePipelines>(*d,"vkCreateComputePipelines"):guide_fallback<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,cache,count,info,allocator,pipelines);
    if(result==VK_SUCCESS&&info&&pipelines)luke_camera::pipelines_created(handle,count,info,pipelines);
    return result;
}
VKAPI_ATTR void VKAPI_CALL hook_vkDestroyPipeline(VkDevice handle,VkPipeline pipeline,const VkAllocationCallbacks* allocator) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkDestroyPipeline>(*d,"vkDestroyPipeline"):guide_fallback<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    if(!fn){return;}
    fn(handle,pipeline,allocator);
    luke_camera::pipeline_destroyed(handle,pipeline);
}
VKAPI_ATTR VkResult VKAPI_CALL hook_vkBindBufferMemory2KHR(VkDevice handle,uint32_t count,const VkBindBufferMemoryInfo* bindings) {
    auto d=device_state(handle);auto fn=d?load<PFN_vkBindBufferMemory2KHR>(*d,"vkBindBufferMemory2KHR"):guide_fallback<PFN_vkBindBufferMemory2KHR>("vkBindBufferMemory2KHR");
    if(!fn){return VK_ERROR_INITIALIZATION_FAILED;}
    const auto result=fn(handle,count,bindings);
    if(result==VK_SUCCESS&&bindings)for(unsigned i=0;i<count;++i)luke_camera::buffer_bound(handle,bindings[i].buffer,bindings[i].memory,bindings[i].memoryOffset);
    return result;
}
