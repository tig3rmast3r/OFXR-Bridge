#include <vulkan/vulkan.h>
#include "../src/ingress/vulkan_luke_camera.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include "xrfg/vulkan_command_stream.hpp"
#include "xrfg/native_ingress/Rdr2CameraPacket.h"
#include "fixtures/rdr2_native_camera.hpp"
#include "xrfg/luke_interleaved_history.hpp"
using namespace xrfg::native_vulkan;
namespace lc=xrfg::native_vulkan::luke_camera;
namespace rv=xrfg::native_ingress::real_vr_dlss_camera;
namespace xrfg::native_vulkan {bool test_camera_native_entry_forwarding();}
namespace {
unsigned checks{},calls{};lc::Bytes observed{};
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void receive(VkCommandBuffer,const lc::Bytes& bytes)noexcept {++calls;observed=bytes;}
template<class T>T handle(unsigned value){return reinterpret_cast<T>(std::uintptr_t(value));}
lc::Bytes camera() {
    lc::Bytes bytes{};
    const auto f=[&](unsigned at,float value){std::memcpy(bytes.data()+at,&value,4);};
    const auto u=[&](unsigned at,unsigned value){std::memcpy(bytes.data()+at,&value,4);};
    for(unsigned i=0;i<4;++i){f(i*20,1);f(64+i*20,1);}
    f(12,.064F);f(304,2);f(308,-2);f(312,.999975F);f(316,.1F);
    f(320,-1);f(324,1);f(328,.000025F);f(332,4000);
    u(256,1920);u(260,1080);f(264,1920);f(268,1080);f(272,1.F/1920);f(276,1.F/1080);
    return bytes;
}
void shader_layout_tests() {
    std::vector<std::uint32_t> code{0x07230203,0x10000,0,16,0};
    const auto emit=[&](unsigned op,std::initializer_list<unsigned> words) {
        code.push_back((static_cast<unsigned>(words.size()+1)<<16)|op);code.insert(code.end(),words);
    };
    emit(22,{1,32});emit(21,{2,32,0});emit(21,{3,32,1});
    emit(23,{4,1,2});emit(23,{5,1,3});emit(23,{6,1,4});emit(24,{7,6,4});emit(23,{8,2,2});
    constexpr std::array<unsigned,49> types{7,7,6,4,4,4,4,4,4,4,4,4,4,4,4,4,4,8,4,4,4,4,4,
        5,1,5,1,5,1,5,1,5,1,5,1,1,1,2,2,2,3,1,1,1,1,1,1,1,5};
    code.push_back((51U<<16)|30);code.push_back(9);code.insert(code.end(),types.begin(),types.end());
    constexpr std::array<unsigned,49> offsets{0,64,128,144,152,160,168,176,184,192,200,208,216,
        224,232,240,248,256,264,272,280,288,296,304,316,320,332,336,348,352,364,
        368,380,384,396,400,404,408,412,416,420,424,428,432,436,440,444,448,452};
    std::size_t near_offset{};
    for(unsigned i=0;i<49;++i){if(i==24)near_offset=code.size()+4;emit(72,{9,i,35,offsets[i]});}
    emit(71,{9,2});emit(72,{9,0,7,16});emit(72,{9,1,7,16});
    const auto major=code.size()+3;emit(72,{9,0,4});emit(72,{9,1,4});
    emit(32,{10,2,9});emit(59,{10,11,2});emit(71,{11,34,0});
    const auto binding=code.size()+3;emit(71,{11,33,0});
    require(lc::known_shader(code.size()*4,code.data()),"exact camera ABI did not recognize a differently compiled module");
    code[near_offset]=320;require(!lc::known_shader(code.size()*4,code.data()),"wrong camera member offset accepted");code[near_offset]=316;
    code[major]=5;require(!lc::known_shader(code.size()*4,code.data()),"column-major camera matrix accepted");code[major]=4;
    code[binding]=1;require(!lc::known_shader(code.size()*4,code.data()),"wrong uniform binding accepted");code[binding]=0;
    require(!lc::known_shader(code.size()*4-1,code.data()),"partial SPIR-V word accepted");
}
}
int native_shader_test(const char* path) {
    auto module=LoadLibraryW(L"vulkan-1.dll");require(module!=nullptr,"load Vulkan");
    auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module,"vkGetInstanceProcAddr"));
    auto create_instance=reinterpret_cast<PFN_vkCreateInstance>(gipa({},"vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;VkInstance instance{};
    require(create_instance(&info,nullptr,&instance)==VK_SUCCESS,"native camera Vulkan instance");
    auto enumerate=reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance,"vkEnumeratePhysicalDevices"));
    unsigned count{};require(enumerate(instance,&count,nullptr)==VK_SUCCESS&&count,"native camera Vulkan GPU");
    std::vector<VkPhysicalDevice> physical(count);enumerate(instance,&count,physical.data());
    auto queues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    queues(physical[0],&count,nullptr);std::vector<VkQueueFamilyProperties> properties(count);queues(physical[0],&count,properties.data());
    unsigned family{};while(family<count&&!(properties[family].queueFlags&VK_QUEUE_COMPUTE_BIT))++family;
    require(family<count,"native camera compute queue");float priority=1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};device_info.queueCreateInfoCount=1;device_info.pQueueCreateInfos=&queue;
    auto create_device=reinterpret_cast<PFN_vkCreateDevice>(gipa(instance,"vkCreateDevice"));VkDevice device{};
    require(create_device(physical[0],&device_info,nullptr,&device)==VK_SUCCESS,"native camera Vulkan device");
    auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance,"vkGetDeviceProcAddr"));
    lc::observer.store(&receive);install_luke_camera_device_dispatch(device,gipa,gdpa);
    require(!lc::tracking_poisoned.load(),"native camera entry installation poisoned tracking");
    require(!(lc::state().hook_failures.load()&lc::required_hooks),"required native camera observation entry missing");
    std::ifstream in(path,std::ios::binary|std::ios::ate);require(bool(in),"native shader input");const auto size=in.tellg();
    std::vector<std::uint32_t> words((static_cast<std::size_t>(size)+3)/4);in.seekg(0);in.read(reinterpret_cast<char*>(words.data()),size);
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};shader_info.codeSize=static_cast<std::size_t>(size);shader_info.pCode=words.data();
    auto create_shader=reinterpret_cast<PFN_vkCreateShaderModule>(gdpa(device,"vkCreateShaderModule"));VkShaderModule shader{};
    require(create_shader(device,&shader_info,nullptr,&shader)==VK_SUCCESS,"native shader creation");
    require(lc::state().shader_matches.load()==1,"actual native shader did not reach camera observer");
    reinterpret_cast<PFN_vkDestroyShaderModule>(gdpa(device,"vkDestroyShaderModule"))(device,shader,nullptr);
    destroy_commands(device);reinterpret_cast<PFN_vkDestroyDevice>(gdpa(device,"vkDestroyDevice"))(device,nullptr);
    reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance,"vkDestroyInstance"))(instance,nullptr);
    std::cout<<"Native GPU camera shader observation PASS\n";return 0;
}
void rdr2_packet_tests() {
    namespace packet=xrfg::native_ingress::rdr2_camera_packet;
    for(const auto* bytes:{&xrfg::tests::rdr2_legacy_camera,&xrfg::tests::rdr2_aer2_camera}) {
        std::array<unsigned char,48> native{};
        for(unsigned row=0;row<4;++row)std::memcpy(native.data()+row*12,bytes->data()+row*16,12);
        require(packet::matches_native_view(bytes->data(),bytes->size(),native),"GPU-selected native RAGE view did not match its camera packet");
        native[39]^=1;
        require(!packet::matches_native_view(bytes->data(),bytes->size(),native),"different render view borrowed the selected token's camera");
        packet::Camera c;
        require(packet::decode(bytes->data(),bytes->size(),2432,2432,true,c),"captured RAGE camera rejected (unused padding may contain NaN)");
        require(std::abs(c.forward[0]+rv::read_float(bytes->data(),16))<1e-6F,"RAGE forward axis was not converted to negative-Z view space");
        const auto depth=[&](float z){return (c.projection[10]*z+c.projection[14])/-z;};
        require(std::abs(depth(-c.near_z)-1)<1e-5F&&std::abs(depth(-c.far_z))<1e-5F,"native reverse-Z planes do not map to1/0");
        for(unsigned r=0;r<4;++r)for(unsigned col=0;col<4;++col) {
            float sum{};for(unsigned k=0;k<4;++k)sum+=c.projection[r*4+k]*c.inverse[k*4+col];
            require(std::abs(sum-(r==col?1.F:0.F))<1e-4F,"native projection inverse is inconsistent");
        }
        require(packet::decode(bytes->data(),bytes->size(),2432,2432,false,c),"normal-Z creation contract rejected");
        require(std::abs(depth(-c.near_z))<1e-5F&&std::abs(depth(-c.far_z)-1)<1e-5F,"native normal-Z planes do not map to0/1");
        require(!packet::decode(bytes->data(),bytes->size(),2432,2042,true,c),"cropped XR eye ratio substituted for square game projection");
        require(!packet::decode(bytes->data(),99,2432,2432,true,c),"truncated camera packet admitted");
        auto bad=*bytes;const float invalid=0;std::memcpy(bad.data()+100,&invalid,4);
        require(!packet::decode(bad.data(),bad.size(),2432,2432,true,c),"invalid native near plane admitted");
        bad=*bytes;std::memcpy(bad.data(),&invalid,4);
        require(!packet::decode(bad.data(),bad.size(),2432,2432,true,c),"nonorthonormal native camera admitted");
    }
}
void rdr2_history_tests() {
    xrfg::DlssNativeCameraHistory current{},middle{},older{};
    current.stream=older.stream=7;middle.stream=8;
    current.epoch=middle.epoch=older.epoch=1;
    current.serial=196;middle.serial=186;older.serial=195;
    current.publication=386;middle.publication=385;older.publication=384;
    current.token=1886;middle.token=1885;older.token=1884;
    current.native_production_sequence=215;middle.native_production_sequence=215;older.native_production_sequence=214;
    for(auto* frame:{&current,&middle,&older}) {
        frame->valid=true;frame->motion_width=frame->motion_height=1216;frame->output_width=frame->output_height=2432;
    }
    const auto matches=[&]{return xrfg::luke_interleaved_history_matches(current,middle,older);};
    require(!matches(),"independent native counters joined without an exact shared camera-source clock");
    for(auto* frame:{&current,&middle,&older}){frame->source_clock_owner=0x52445232;frame->source_clock_sequence=frame->token;}
    require(matches(),"captured RDR2 A/B/A camera history with two native counters was rejected");
    ++middle.source_clock_owner;require(!matches(),"different native source owners joined");--middle.source_clock_owner;
    --middle.source_clock_sequence;require(!matches(),"missed native render joined");++middle.source_clock_sequence;
    --middle.token;require(!matches(),"camera token disagreed with native render receipt");++middle.token;
    ++current.native_production_sequence;require(!matches(),"missed production in the same eye feature joined");--current.native_production_sequence;
    middle.source_clock_owner=0;require(!matches(),"missing opposite-eye source receipt joined");middle.source_clock_owner=current.source_clock_owner;
    middle.reset=true;require(!matches(),"opposite-eye reset joined old camera history");middle.reset=false;
    require(matches(),"restored RDR2 native source receipts failed");
}
int main(int argc,char** argv)try {
    if(argc==3&&std::string_view(argv[1])=="native-shader")return native_shader_test(argv[2]);
    shader_layout_tests();
    rdr2_packet_tests();
    rdr2_history_tests();
    const auto device=handle<VkDevice>(1),other=handle<VkDevice>(2);
    const auto memory=handle<VkDeviceMemory>(3);const auto buffer=handle<VkBuffer>(4);
    const auto set=handle<VkDescriptorSet>(5);const auto pipeline=handle<VkPipeline>(6);
    const auto shader=handle<VkShaderModule>(7);const auto command=handle<VkCommandBuffer>(8);
    const auto pool=handle<VkDescriptorPool>(9),unrelated_pool=handle<VkDescriptorPool>(10);
    std::array<unsigned char,2048> upload{};const auto bytes=camera();
    std::memcpy(upload.data()+448,bytes.data(),bytes.size());
    lc::observer.store(&receive);
    lc::allocated(device,memory,upload.size());lc::mapped(device,memory,128,1024,upload.data()+128);
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=1024;info.usage=VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    lc::buffer_created(device,buffer,info);lc::buffer_bound(device,buffer,memory,256);
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorSetCount=1;allocation.descriptorPool=pool;
    lc::descriptors_allocated(device,allocation,&set);
    VkDescriptorBufferInfo binding{buffer,64,464};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=set;write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;write.pBufferInfo=&binding;
    lc::descriptors_written(device,1,&write,0,nullptr);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.stage.module=shader;
    lc::pipelines_created(device,1,&ci,&pipeline);
    lc::bind_pipeline(device,command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    const unsigned dynamic=128;
    lc::bind_sets(device,command,VK_PIPELINE_BIND_POINT_COMPUTE,0,1,&set,1,&dynamic);
    lc::dispatch(device,command);require(!calls,"unknown shader authorized a camera");
    // This fixture supplies a witnessed pipeline independently of the pinned
    // vendor shader bytes; the actual installed shader hash was checked offline.
    lc::update(device,[&](lc::Device& d){d.shaders.insert(shader);});
    lc::pipelines_created(device,1,&ci,&pipeline);
    lc::dispatch(device,command);require(calls==1&&observed==bytes,"bound allocation/map/descriptor/dynamic offsets lost exact bytes");
    require(rv::looks_like(observed.data(),observed.size(),1920,1080)&&rv::decode(observed.data(),observed.size()).valid,
        "observed REAL VR packet failed existing projection/pose validation");
    require(!rv::looks_like(observed.data(),observed.size(),1280,720),"wrong motion field admitted a camera");
    lc::dispatch(other,command);require(calls==1,"different device borrowed a packet");
    lc::bind_sets(device,command,VK_PIPELINE_BIND_POINT_COMPUTE,0,1,&set,0,nullptr);
    lc::dispatch(device,command);require(calls==1,"missing dynamic offset guessed zero");
    lc::bind_sets(device,command,VK_PIPELINE_BIND_POINT_COMPUTE,0,1,&set,1,&dynamic);
    binding.range=463;lc::descriptors_written(device,1,&write,0,nullptr);
    lc::dispatch(device,command);require(calls==1,"undersized descriptor range read beyond its view");
    binding.range=464;lc::descriptors_written(device,1,&write,0,nullptr);
    lc::unmapped(device,memory);upload.fill(0);
    lc::dispatch(device,command);require(calls==2&&observed==bytes,"unmapped upload lost its immutable snapshot");
    lc::pool_reset(device,unrelated_pool);lc::dispatch(device,command);
    require(calls==3,"unrelated descriptor-pool reset erased a live camera binding");
    lc::pool_reset(device,pool);lc::dispatch(device,command);require(calls==3,"retired pool retained a stale descriptor");
    lc::descriptors_allocated(device,allocation,&set);lc::descriptors_written(device,1,&write,0,nullptr);
    lc::mapped(device,memory,128,1024,upload.data()+128);lc::dispatch(device,command);
    require(calls==4&&observed!=bytes,"new Map reused old unmap shadow data");
    lc::freed(device,memory);lc::dispatch(device,command);require(calls==4,"freed memory retained a camera");
    lc::begin(device,command);lc::dispatch(device,command);require(calls==4,"new recording retained old compute bindings");
    require(!lc::known_shader(0,nullptr)&&!lc::known_shader(bytes.size(),bytes.data()),"unverified shader accepted");
    rv::Chain chain;rv::Camera world;const auto decoded=rv::decode(bytes.data(),bytes.size());
    chain.advance(decoded,world);require(world.valid&&chain.world()!=0,"relative camera did not establish a world");
    require(test_camera_native_entry_forwarding(),"native entry originals crossed devices/entries or recursed through another dispatch layer");
    lc::observer.store(nullptr);std::cout<<"Vulkan REAL VR camera: "<<checks<<" checks PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
