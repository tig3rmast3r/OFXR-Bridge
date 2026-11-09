#pragma once
// Read-only observation of REAL VR 2611.7.2's Vulkan DLSS-fix constants.
// Its embedded SPIR-V was inspected offline: cb0 is a 464-byte uniform buffer
// at set0/binding0, with the same member offsets as RealVrDlssCamera.h.
#include "xrfg/native_ingress/RealVrDlssCamera.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <span>

namespace xrfg::native_vulkan::luke_camera {
using Bytes=std::array<unsigned char,native_ingress::real_vr_dlss_camera::buffer_bytes>;
using Observer=void(*)(VkCommandBuffer,const Bytes&) noexcept;
using Reporter=void(*)(unsigned,std::int64_t,std::uint64_t,std::uint64_t) noexcept;
using CandidateGate=bool(*)(VkCommandBuffer) noexcept;
inline std::atomic<Observer> observer{};
inline std::atomic<Reporter> reporter{};
inline std::atomic<CandidateGate> candidate_gate{};
inline std::atomic_bool tracking_poisoned{};
// These creation, mapping and current-recording receipts establish identity.
// Missing retirement APIs cannot manufacture a new receipt: observed creates
// replace recycled handles, and a reset recording must bind its state anew.
inline constexpr std::uint64_t required_hooks=(1ULL<<0)|(1ULL<<2)|(1ULL<<3)|(1ULL<<4)|(1ULL<<6)|
    (1ULL<<9)|(1ULL<<10)|(1ULL<<14)|(1ULL<<16)|(1ULL<<18)|(1ULL<<19)|(1ULL<<20);
inline bool enabled() noexcept {return observer.load(std::memory_order_relaxed)!=nullptr;}
inline bool known_shader(std::size_t size,const void* bytes) noexcept {
    if(!bytes||reinterpret_cast<std::uintptr_t>(bytes)%alignof(std::uint32_t)||size<20||size%4||size>4*1024*1024)return false;
    try {
        const auto* words=static_cast<const std::uint32_t*>(bytes);
        if(words[0]!=0x07230203||!words[3]||words[3]>1000000||words[4])return false;
        std::vector<std::span<const std::uint32_t>> instructions;
        for(std::size_t at=5;at<size/4;) {
            const auto count=words[at]>>16;if(!count||count>size/4-at)return false;
            instructions.emplace_back(words+at,count);at+=count;
        }
        constexpr std::array<unsigned,49> offsets{0,64,128,144,152,160,168,176,184,192,200,208,216,
            224,232,240,248,256,264,272,280,288,296,304,316,320,332,336,348,352,364,
            368,380,384,396,400,404,408,412,416,420,424,428,432,436,440,444,448,452};
        const auto op=[](auto instruction){return instruction[0]&0xffff;};
        const auto vector_type=[&](unsigned id,unsigned count,bool floating) {
            unsigned component{};
            for(auto i:instructions)if(op(i)==23&&i.size()==4&&i[1]==id&&i[3]==count)component=i[2];
            for(auto i:instructions)if(i.size()>2&&i[1]==component&&i[2]==32&&
                ((floating&&op(i)==22&&i.size()==3)||(!floating&&op(i)==21&&i.size()==4&&!i[3])))return true;
            return false;
        };
        const auto matrix_type=[&](unsigned id) {
            for(auto i:instructions)if(op(i)==24&&i.size()==4&&i[1]==id&&i[3]==4&&vector_type(i[2],4,true))return true;
            return false;
        };
        const auto scalar_type=[&](unsigned id,unsigned kind) {
            for(auto i:instructions)if(i.size()>2&&i[1]==id&&i[2]==32&&
                ((!kind&&op(i)==22&&i.size()==3)||(kind&&op(i)==21&&i.size()==4&&i[3]==kind-1)))return true;
            return false;
        };
        constexpr std::array<unsigned,49> kinds{4,4,3,1,1,1,1,1,1,1,1,1,1,1,1,1,1,7,1,1,1,1,1,
            2,0,2,0,2,0,2,0,2,0,2,0,0,0,5,5,5,6,0,0,0,0,0,0,0,2};
        for(auto type:instructions) {
            if(op(type)!=30||type.size()!=51||!matrix_type(type[2])||!matrix_type(type[3])||
                !vector_type(type[19],2,false)||!vector_type(type[25],3,true))continue;
            bool typed=true;
            for(unsigned member=0;member<kinds.size();++member) {
                const auto id=type[member+2],kind=kinds[member];
                if(!(kind==4?matrix_type(id):kind==7?vector_type(id,2,false):kind==5?scalar_type(id,1):
                    kind==6?scalar_type(id,2):kind?vector_type(id,kind+1,true):scalar_type(id,0))){typed=false;break;}
            }
            if(!typed)continue;
            const auto structure=type[1];std::array<bool,49> seen{};bool block{},stride0{},stride1{},row0{},row1{};
            for(auto i:instructions) {
                if(op(i)==71&&i.size()==3&&i[1]==structure&&i[2]==2)block=true;
                if(op(i)==72&&i.size()==4&&i[1]==structure&&i[3]==4){if(i[2]==0)row0=true;if(i[2]==1)row1=true;}
                if(op(i)!=72||i.size()!=5||i[1]!=structure)continue;
                if(i[3]==35&&i[2]<offsets.size()&&i[4]==offsets[i[2]])seen[i[2]]=true;
                if(i[3]==7&&i[4]==16){if(i[2]==0)stride0=true;if(i[2]==1)stride1=true;}
            }
            if(!block||!stride0||!stride1||!row0||!row1||std::find(seen.begin(),seen.end(),false)!=seen.end())continue;
            for(auto pointer:instructions)if(op(pointer)==32&&pointer.size()==4&&pointer[2]==2&&pointer[3]==structure)
                for(auto variable:instructions)if(op(variable)==59&&variable.size()>=4&&variable[1]==pointer[1]&&variable[3]==2) {
                    bool set{},binding{};
                    for(auto i:instructions)if(op(i)==71&&i.size()==4&&i[1]==variable[2]&&!i[3]) {
                        set|=i[2]==34;binding|=i[2]==33;
                    }
                    if(set&&binding)return true;
                }
        }
    }catch(...) {}
    return false;
}
struct Memory {
    VkDeviceSize size{},offset{},mapped_size{};
    unsigned uniforms{};
    const unsigned char* mapped{};
    std::vector<unsigned char> shadow;
};
struct Buffer {VkDeviceSize size{},offset{};VkDeviceMemory memory{};};
struct Descriptor {VkDescriptorBufferInfo buffer{};bool dynamic{};};
struct Bound {VkPipeline pipeline{};VkDescriptorSet set{};std::uint32_t dynamic{};bool has_dynamic{};};
struct Device {
    std::unordered_map<VkDeviceMemory,Memory> memories;
    std::unordered_map<VkBuffer,Buffer> buffers;
    std::unordered_map<VkDescriptorSet,Descriptor> descriptors;
    std::unordered_map<VkDescriptorSet,VkDescriptorPool> descriptor_pools;
    std::unordered_set<VkShaderModule> shaders;
    std::unordered_set<VkPipeline> pipelines;
    std::unordered_set<VkPipeline> compute_pipelines;
    std::unordered_map<VkCommandBuffer,Bound> commands;
};
struct State {std::mutex mutex;std::unordered_map<VkDevice,Device> devices;
    std::atomic_uint64_t shader_calls{},shader_matches{},pipeline_matches{},dispatch_reads{},hook_failures{},
        dispatches{},scoped_dispatches{},entry_probes{},last_read_failure{},read_reports{};};
inline State& state(){static auto* value=new State;return *value;}
template<class F>void update(VkDevice device,F&& operation) noexcept {
    if(!enabled())return;
    try {auto& s=state();std::scoped_lock lock(s.mutex);operation(s.devices[device]);}
    catch(...) {tracking_poisoned.store(true,std::memory_order_release);observer.store(nullptr,std::memory_order_release);}
}
inline void allocated(VkDevice device,VkDeviceMemory memory,VkDeviceSize size) noexcept {
    update(device,[&](Device& d){d.memories[memory]={size};});
}
inline void freed(VkDevice device,VkDeviceMemory memory) noexcept {
    update(device,[&](Device& d){d.memories.erase(memory);});
}
inline void mapped(VkDevice device,VkDeviceMemory memory,VkDeviceSize offset,VkDeviceSize size,void* pointer) noexcept {
    update(device,[&](Device& d){auto it=d.memories.find(memory);if(it==d.memories.end())return;
        auto& m=it->second;m.shadow.clear();m.mapped=nullptr;
        if(offset>m.size)return;
        const auto count=size==VK_WHOLE_SIZE?m.size-offset:size;
        if(count>m.size-offset)return;
        m.offset=offset;m.mapped_size=count;m.mapped=static_cast<const unsigned char*>(pointer);});
}
inline void unmapped(VkDevice device,VkDeviceMemory memory) noexcept {
    update(device,[&](Device& d){auto it=d.memories.find(memory);if(it==d.memories.end())return;
        auto& m=it->second;m.shadow.clear();
        // REAL VR may unmap its host-visible upload before binding it. Keep a
        // bounded immutable copy of exactly that mapped interval, never Map a
        // game's resource or sample memory that was not observed mapped.
        if(m.mapped&&m.uniforms&&m.mapped_size<=4*1024*1024) {
            m.shadow.resize(static_cast<std::size_t>(m.mapped_size));SIZE_T read{};
            if(!ReadProcessMemory(GetCurrentProcess(),m.mapped,m.shadow.data(),m.shadow.size(),&read)||read!=m.shadow.size())m.shadow.clear();
        }
        m.mapped=nullptr;});
}
inline void buffer_created(VkDevice device,VkBuffer buffer,const VkBufferCreateInfo& info) noexcept {
    update(device,[&](Device& d){auto previous=d.buffers.find(buffer);
        if(previous!=d.buffers.end()){auto memory=d.memories.find(previous->second.memory);
            if(memory!=d.memories.end()&&memory->second.uniforms)--memory->second.uniforms;d.buffers.erase(previous);}
        if((info.usage&VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)&&info.size>=Bytes{}.size())d.buffers[buffer]={info.size};});
}
inline void buffer_destroyed(VkDevice device,VkBuffer buffer) noexcept {
    update(device,[&](Device& d){auto previous=d.buffers.find(buffer);
        if(previous!=d.buffers.end()){auto memory=d.memories.find(previous->second.memory);
            if(memory!=d.memories.end()&&memory->second.uniforms)--memory->second.uniforms;d.buffers.erase(previous);}});
}
inline void buffer_bound(VkDevice device,VkBuffer buffer,VkDeviceMemory memory,VkDeviceSize offset) noexcept {
    update(device,[&](Device& d){auto it=d.buffers.find(buffer);if(it==d.buffers.end())return;
        auto previous=d.memories.find(it->second.memory);if(previous!=d.memories.end()&&previous->second.uniforms)--previous->second.uniforms;
        auto next=d.memories.find(memory);if(next!=d.memories.end())++next->second.uniforms;
        it->second.memory=memory;it->second.offset=offset;});
}
inline void descriptors_written(VkDevice device,std::uint32_t count,const VkWriteDescriptorSet* writes,
    std::uint32_t copies,const VkCopyDescriptorSet* copy) noexcept {
    update(device,[&](Device& d){
        for(unsigned i=0;writes&&i<count;++i){const auto& w=writes[i];
            if(w.dstBinding||w.dstArrayElement||!w.descriptorCount)continue;
            d.descriptors.erase(w.dstSet);
            if(w.pBufferInfo&&(w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC))
                d.descriptors[w.dstSet]={w.pBufferInfo[0],w.descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC};
        }
        for(unsigned i=0;copy&&i<copies;++i){const auto& c=copy[i];
            if(c.dstBinding||c.dstArrayElement||!c.descriptorCount)continue;
            auto found=d.descriptors.find(c.srcSet);
            if(!c.srcBinding&&!c.srcArrayElement&&found!=d.descriptors.end())d.descriptors[c.dstSet]=found->second;
            else d.descriptors.erase(c.dstSet);
        }
    });
}
inline void descriptors_freed(VkDevice device,std::uint32_t count,const VkDescriptorSet* sets) noexcept {
    update(device,[&](Device& d){for(unsigned i=0;sets&&i<count;++i){d.descriptors.erase(sets[i]);d.descriptor_pools.erase(sets[i]);}});
}
inline void descriptors_allocated(VkDevice device,const VkDescriptorSetAllocateInfo& info,const VkDescriptorSet* sets) noexcept {
    update(device,[&](Device& d){for(unsigned i=0;sets&&i<info.descriptorSetCount;++i){
        d.descriptors.erase(sets[i]);d.descriptor_pools[sets[i]]=info.descriptorPool;}});
}
inline void pool_reset(VkDevice device,VkDescriptorPool pool) noexcept {
    update(device,[&](Device& d){for(auto it=d.descriptor_pools.begin();it!=d.descriptor_pools.end();) {
        if(it->second==pool){d.descriptors.erase(it->first);it=d.descriptor_pools.erase(it);}else ++it;}});
}
inline void shader_created(VkDevice device,VkShaderModule shader,const VkShaderModuleCreateInfo& info) noexcept {
    if(!enabled())return;
    const auto number=state().shader_calls.fetch_add(1,std::memory_order_relaxed);
    const bool known=known_shader(info.codeSize,info.pCode);
    if((number<64||known)&&reporter.load(std::memory_order_relaxed)) {
        std::uint64_t hash=1469598103934665603ULL;
        for(std::size_t i=0;info.pCode&&i<info.codeSize;++i){hash^=reinterpret_cast<const unsigned char*>(info.pCode)[i];hash*=1099511628211ULL;}
        reporter.load(std::memory_order_acquire)(1336,known?1:0,info.codeSize,hash);
    }
    update(device,[&](Device& d){d.shaders.erase(shader);if(known){d.shaders.insert(shader);++state().shader_matches;}});
}
inline void shader_destroyed(VkDevice device,VkShaderModule shader) noexcept {
    update(device,[&](Device& d){d.shaders.erase(shader);});
}
inline void pipelines_created(VkDevice device,std::uint32_t count,const VkComputePipelineCreateInfo* info,const VkPipeline* pipelines) noexcept {
    update(device,[&](Device& d){for(unsigned i=0;info&&pipelines&&i<count;++i){
        d.pipelines.erase(pipelines[i]);d.compute_pipelines.erase(pipelines[i]);
        if(pipelines[i])d.compute_pipelines.insert(pipelines[i]);
        if(pipelines[i]&&d.shaders.contains(info[i].stage.module)){
            d.pipelines.insert(pipelines[i]);++state().pipeline_matches;}}});
}
inline void pipeline_destroyed(VkDevice device,VkPipeline pipeline) noexcept {
    update(device,[&](Device& d){d.pipelines.erase(pipeline);d.compute_pipelines.erase(pipeline);});
}
inline void begin(VkDevice device,VkCommandBuffer command) noexcept {
    update(device,[&](Device& d){d.commands.erase(command);});
}
inline void bind_pipeline(VkDevice device,VkCommandBuffer command,VkPipelineBindPoint point,VkPipeline pipeline) noexcept {
    if(point==VK_PIPELINE_BIND_POINT_COMPUTE)update(device,[&](Device& d){d.commands[command].pipeline=pipeline;});
}
inline void bind_sets(VkDevice device,VkCommandBuffer command,VkPipelineBindPoint point,std::uint32_t first,
    std::uint32_t count,const VkDescriptorSet* sets,std::uint32_t dynamic_count,const std::uint32_t* dynamic) noexcept {
    if(point!=VK_PIPELINE_BIND_POINT_COMPUTE||first||!count||!sets)return;
    update(device,[&](Device& d){auto& b=d.commands[command];b.set=sets[0];b.has_dynamic=dynamic_count&&dynamic;
        b.dynamic=b.has_dynamic?dynamic[0]:0;});
}
inline bool read(Device& d,VkCommandBuffer command,Bytes& bytes,bool scoped_candidate=false,unsigned* rejection=nullptr) noexcept {
    const auto fail=[&](unsigned why){if(rejection)*rejection=why;return false;};
    if(rejection)*rejection=0;
    auto bound=d.commands.find(command);if(bound==d.commands.end())return fail(1);
    if(!(d.pipelines.contains(bound->second.pipeline)||(scoped_candidate&&d.compute_pipelines.contains(bound->second.pipeline))))return fail(2);
    auto ds=d.descriptors.find(bound->second.set);if(ds==d.descriptors.end())return fail(3);
    const auto& descriptor=ds->second;
    if(descriptor.dynamic&&!bound->second.has_dynamic)return fail(4);
    auto buffer=d.buffers.find(descriptor.buffer.buffer);if(buffer==d.buffers.end())return fail(5);
    const auto& b=buffer->second;auto memory=d.memories.find(b.memory);if(memory==d.memories.end())return fail(6);
    const auto& m=memory->second;
    const VkDeviceSize dyn=descriptor.dynamic?bound->second.dynamic:0;
    if(descriptor.buffer.offset>b.size||dyn>b.size-descriptor.buffer.offset)return fail(7);
    const auto offset=descriptor.buffer.offset+dyn;
    if(bytes.size()>b.size-offset||(descriptor.buffer.range!=VK_WHOLE_SIZE&&bytes.size()>descriptor.buffer.range))return fail(7);
    if(b.offset>m.size||offset>m.size-b.offset)return fail(8);
    const auto absolute=b.offset+offset;
    if(absolute<m.offset||absolute-m.offset>m.mapped_size||bytes.size()>m.mapped_size-(absolute-m.offset))return fail(9);
    const auto relative=absolute-m.offset;
    if(m.mapped){SIZE_T count{};if(ReadProcessMemory(GetCurrentProcess(),m.mapped+relative,bytes.data(),bytes.size(),&count)&&count==bytes.size())return true;return fail(10);}
    if(relative>m.shadow.size()||bytes.size()>m.shadow.size()-relative)return fail(11);
    std::memcpy(bytes.data(),m.shadow.data()+relative,bytes.size());return true;
}
inline void dispatch(VkDevice device,VkCommandBuffer command,bool entry=false) noexcept {
    const auto callback=observer.load(std::memory_order_acquire);if(!callback)return;
    const auto gate=candidate_gate.load(std::memory_order_acquire);
    const bool scoped_candidate=gate&&gate(command);
    ++state().dispatches;
    if(scoped_candidate){if(entry)++state().entry_probes;else ++state().scoped_dispatches;}
    Bytes bytes{};bool found{};unsigned rejection{};VkPipeline pipeline{};VkDescriptorSet set{};
    update(device,[&](Device& d){found=read(d,command,bytes,scoped_candidate,&rejection);
        if(auto it=d.commands.find(command);it!=d.commands.end()){pipeline=it->second.pipeline;set=it->second.set;}});
    if(scoped_candidate) {
        state().last_read_failure.store(rejection,std::memory_order_relaxed);
        if(rejection&&state().read_reports.fetch_add(1,std::memory_order_relaxed)<32)
            if(const auto report=reporter.load(std::memory_order_acquire))report(1338,rejection|(entry?256:0),
                (std::uint64_t)(pipeline),(std::uint64_t)(set));
    }
    if(found){++state().dispatch_reads;callback(command,bytes);}
}
}
