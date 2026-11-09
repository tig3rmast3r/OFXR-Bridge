// Persistent Radeon-first Vulkan RIFE worker. stdout is protocol only.
// Shared D3D12 FP16 planes -> flow/mask -> original-colour D3D12 composition.
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "net.h"
#include "gpu.h"
#include "command.h"
#include "pipelinecache.h"
#include "fused_warp.hpp"
#include "xrfg/rife_runtime_lease.hpp"
#include "json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#pragma comment(lib,"bcrypt.lib")
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void vkcheck(VkResult value,const char* operation="operation") { if(value != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan ")+operation+" failed: " + std::to_string(value)); }
// AppData staging plus an immutable cache key can exceed MAX_PATH even with a
// normal Windows user name. Use extended Unicode paths for file access, without
// depending on a machine-wide long-path policy or changing serialized paths.
static fs::path file_path(const fs::path& path) {
    auto value=fs::absolute(path).lexically_normal().make_preferred().native();
    if(value.starts_with(L"\\\\?\\"))return fs::path(value);
    if(value.starts_with(L"\\\\"))return fs::path(L"\\\\?\\UNC\\"+value.substr(2));
    return fs::path(L"\\\\?\\"+value);
}
static Json read_json(const fs::path& path) { std::ifstream file(file_path(path)); require(bool(file),"Cannot read Vulkan metadata"); return Json::parse(file); }
static void write_json(const fs::path& path,const Json& value) { std::ofstream file(file_path(path)); file << value.dump(2); require(bool(file),"Cannot write Vulkan result"); }
static std::string utf8(const fs::path& path) {const auto value=path.u8string();return std::string(reinterpret_cast<const char*>(value.data()),value.size());}
static std::string hex(const unsigned char* bytes,size_t n) { const char* h="0123456789abcdef"; std::string out;for(size_t i=0;i<n;++i){out+=h[bytes[i]>>4];out+=h[bytes[i]&15];}return out; }
static std::string sha256(const fs::path& path) {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
    require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 unavailable");
    struct Close { BCRYPT_ALG_HANDLE a; BCRYPT_HASH_HANDLE& h; ~Close(){if(h)BCryptDestroyHash(h);BCryptCloseAlgorithmProvider(a,0);} } close{algorithm,hash};
    require(BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0,"SHA256 initialization failed");
    std::ifstream input(file_path(path),std::ios::binary); require(bool(input),"Missing Vulkan artifact");
    std::array<unsigned char,65536> block{}; while(input.read(reinterpret_cast<char*>(block.data()),block.size()) || input.gcount())
        require(BCryptHashData(hash,block.data(),static_cast<ULONG>(input.gcount()),0)>=0,"SHA256 read failed");
    require(input.eof(),"Vulkan artifact read failed");std::array<unsigned char,32> digest{};
    require(BCryptFinishHash(hash,digest.data(),digest.size(),0)>=0,"SHA256 failed");return hex(digest.data(),digest.size());
}
static Json identity(int gpu) {
    require(gpu>=0 && gpu<ncnn::get_gpu_count(),"Vulkan adapter index is unavailable");
    const auto& info=ncnn::get_gpu_info(gpu);
    VkPhysicalDeviceIDProperties ids{}; ids.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};properties.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;properties.pNext=&ids;
    require(ncnn::vkGetPhysicalDeviceProperties2KHR!=nullptr,"Vulkan device identity is unavailable");
    ncnn::vkGetPhysicalDeviceProperties2KHR(info.physical_device(),&properties);
    uint64_t luid{};if(ids.deviceLUIDValid)std::memcpy(&luid,ids.deviceLUID,8);
    return {{"name",info.device_name()},{"vendor",info.vendor_id()},{"device",info.device_id()},
        {"driver",info.driver_version()},{"uuid",hex(ids.deviceUUID,16)},
        {"pipeline_uuid",hex(properties.properties.pipelineCacheUUID,16)}, {"luid",luid},
        {"fp16_storage",info.support_fp16_storage()},{"fp16_arithmetic",info.support_fp16_arithmetic()},
        {"cooperative_matrix",info.support_cooperative_matrix()},{"ncnn",OFXR_NCNN_REV}};
}
static int find_adapter(uint64_t luid) { for(int i=0;i<ncnn::get_gpu_count();++i)if(identity(i)["luid"].get<uint64_t>()==luid && luid)return i; throw std::runtime_error("No Vulkan adapter matches the D3D12 LUID"); }
static int preferred_adapter() {for(int i=0;i<ncnn::get_gpu_count();++i)if(ncnn::get_gpu_info(i).vendor_id()==0x1002)return i;return ncnn::get_default_gpu_index();}

// Win32 extension ABI is kept local because ncnn uses its portable Vulkan loader.
struct ImportMemoryWin32 { VkStructureType sType; const void* pNext; VkExternalMemoryHandleTypeFlagBits handleType; HANDLE handle; LPCWSTR name; };
struct MemoryWin32Properties { VkStructureType sType; void* pNext; uint32_t memoryTypeBits; };
using GetWin32Properties = VkResult(VKAPI_PTR*)(VkDevice,VkExternalMemoryHandleTypeFlagBits,HANDLE,MemoryWin32Properties*);
static constexpr auto D3D12_RESOURCE_HANDLE=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

struct Imported {
    const ncnn::VulkanDevice* device{}; VkBuffer buffer{}; VkDeviceMemory memory{};
    std::vector<ncnn::VkBufferMemory> slices;
    Imported(const ncnn::VulkanDevice* dev,HANDLE handle,uint64_t allocated,int w,int h,int channels,int eyes):device(dev) {
        struct Close {HANDLE h;~Close(){if(h)CloseHandle(h);}} close{handle};
        try {
            const auto bytes=uint64_t(w)*h*channels*eyes*2;
            require(bytes && allocated>=bytes,"Invalid shared buffer size");
            VkPhysicalDeviceExternalBufferInfo query{}; query.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
            query.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;query.handleType=D3D12_RESOURCE_HANDLE;
            VkExternalBufferProperties external{};external.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
            require(ncnn::vkGetPhysicalDeviceExternalBufferPropertiesKHR!=nullptr,"External memory queries unavailable");
            ncnn::vkGetPhysicalDeviceExternalBufferPropertiesKHR(dev->info.physical_device(),&query,&external);
            require((external.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)!=0,"D3D12 resource import unsupported on this Vulkan driver");
            VkExternalMemoryBufferCreateInfo external_create{};external_create.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;external_create.handleTypes=D3D12_RESOURCE_HANDLE;
            VkBufferCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;create.pNext=&external_create;create.size=bytes;create.usage=query.usage;create.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            vkcheck(ncnn::vkCreateBuffer(dev->vkdevice(),&create,nullptr,&buffer),"create external buffer");
            VkMemoryRequirements requirements{};ncnn::vkGetBufferMemoryRequirements(dev->vkdevice(),buffer,&requirements);
            require(allocated>=requirements.size,"D3D12 allocation is too small for Vulkan");
            auto get_properties=reinterpret_cast<GetWin32Properties>(ncnn::vkGetDeviceProcAddr(dev->vkdevice(),"vkGetMemoryWin32HandlePropertiesKHR"));
            require(get_properties!=nullptr,"Win32 external memory extension unavailable");
            MemoryWin32Properties properties{static_cast<VkStructureType>(1000073002),nullptr,0};
            vkcheck(get_properties(dev->vkdevice(),D3D12_RESOURCE_HANDLE,handle,&properties),"query Win32 handle");
            const auto bits=properties.memoryTypeBits&requirements.memoryTypeBits;
            require(bits!=0,"No compatible D3D12/Vulkan memory type");
            VkMemoryDedicatedAllocateInfo dedicated{};dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;dedicated.buffer=buffer;
            ImportMemoryWin32 import{static_cast<VkStructureType>(1000073000),&dedicated,D3D12_RESOURCE_HANDLE,handle,nullptr};
            VkMemoryAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;allocate.pNext=&import;allocate.allocationSize=allocated;
            allocate.memoryTypeIndex=dev->find_memory_index(bits,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,0);
            vkcheck(ncnn::vkAllocateMemory(dev->vkdevice(),&allocate,nullptr,&memory),"import D3D12 memory");
            vkcheck(ncnn::vkBindBufferMemory(dev->vkdevice(),buffer,memory,0),"bind D3D12 memory");
            slices.resize(eyes);
            for(int eye=0;eye<eyes;++eye){auto& slice=slices[eye];slice={};slice.buffer=buffer;slice.memory=memory;slice.offset=uint64_t(eye)*w*h*channels*2;slice.capacity=uint64_t(w)*h*channels*2;slice.memory_type_index=allocate.memoryTypeIndex;slice.stage_flags=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;}
        }catch(...){cleanup();throw;}
    }
    Imported(const Imported&)=delete;
    ~Imported(){cleanup();}
    void cleanup(){if(buffer)ncnn::vkDestroyBuffer(device->vkdevice(),buffer,nullptr);if(memory)ncnn::vkFreeMemory(device->vkdevice(),memory,nullptr);buffer={};memory={};}
    ncnn::VkMat view(int eye,int w,int h,int c,ncnn::VkAllocator* allocator) {return ncnn::VkMat(w,h,c,&slices.at(eye),2u,allocator);}
};

struct Engine {
    const ncnn::VulkanDevice* device;
    ncnn::VkBlobAllocator blobs;
    ncnn::VkStagingAllocator staging;
    ncnn::PipelineCache cache;
    ncnn::Net net;
    std::unique_ptr<OFXRTime> timestep_fill;
    int width,height;
    std::string profile;
    bool mixed{};
    Engine(int gpu,const fs::path& input_directory,int w,int h,const std::string& p):device(ncnn::get_gpu_device(gpu)),blobs(device),staging(device),cache(device),width(w),height(h),profile(p) {
        const auto directory=file_path(input_directory);
        require(device->is_valid(),"Vulkan device initialization failed");
        mixed=p.starts_with("mixed-");
        std::string policy;
        if(mixed){const auto end=p.find('-',6);require(end!=std::string::npos,"Invalid mixed profile");policy=p.substr(6,end-6);require(policy=="features"||policy=="refine"||policy=="final","Unknown mixed policy");}
        auto& opt=net.opt;opt.use_vulkan_compute=true;opt.num_threads=1;
        opt.use_fp16_packed=p!="fp32";opt.use_fp16_storage=p!="fp32";opt.use_fp16_arithmetic=p=="fp16"||p=="matrix"||p.ends_with("-fp16")||p.ends_with("-matrix");
        opt.use_cooperative_matrix=p=="matrix"||p.ends_with("-matrix");opt.use_bf16_storage=false;opt.use_bf16_packed=false;
        opt.use_int8_inference=false;opt.use_int8_storage=false;opt.use_int8_arithmetic=false;
        opt.blob_vkallocator=&blobs;opt.workspace_vkallocator=&blobs;opt.staging_vkallocator=&staging;opt.pipeline_cache=&cache;
        net.set_vulkan_device(device);
        require(net.register_custom_layer("OFXRWarp",create_ofxr_warp)==0,"Cannot register fused Vulkan warp");
        if(fs::is_regular_file(directory/"pipeline.cache"))require(cache.load_cache((directory/"pipeline.cache").c_str())==0,"Cannot load Vulkan pipeline cache");
        if(fs::is_regular_file(directory/"warp.cache"))require(device->get_pipeline_cache()->load_cache((directory/"warp.cache").c_str())==0,"Cannot load fused Vulkan pipeline cache");
        const auto graph_path=directory/(mixed?"flow."+policy+".param":"flow.ncnn.param");
        FILE* graph=_wfopen(graph_path.c_str(),L"rb");require(graph!=nullptr,"Cannot open RIFE Vulkan graph");
        const int graph_result=net.load_param(graph);fclose(graph);require(graph_result==0,"Cannot load RIFE Vulkan graph");
        require(net.input_names().size()==3 && std::any_of(net.output_names().begin(),net.output_names().end(),[](const char* name){return std::string(name)=="out0";}),"Unexpected RIFE Vulkan graph interface");
        for(const auto* layer:net.layers())if(!layer || !layer->support_vulkan)throw std::runtime_error("RIFE graph contains a CPU fallback operation: " + (layer?layer->type:"null"));
        FILE* weights=_wfopen((directory/"flow.ncnn.bin").c_str(),L"rb");require(weights!=nullptr,"Cannot open RIFE Vulkan weights");
        const int weight_result=net.load_model(weights);fclose(weights);require(weight_result==0,"Cannot load RIFE Vulkan weights");
        auto input_opt=net.opt;if(mixed){input_opt.use_fp16_storage=false;input_opt.use_fp16_packed=false;input_opt.use_fp16_arithmetic=false;}
        timestep_fill=std::make_unique<OFXRTime>(device,input_opt);
    }
    ncnn::VkMat run(const ncnn::VkMat& a,const ncnn::VkMat& b,float fraction,ncnn::VkCompute& cmd) {
        require(std::isfinite(fraction)&&fraction>=.05f&&fraction<=.95f,"Invalid interpolation fraction");
        auto input_opt=net.opt;if(mixed){input_opt.use_fp16_storage=false;input_opt.use_fp16_packed=false;input_opt.use_fp16_arithmetic=false;}
        auto timestep=timestep_fill->record(width,height,fraction,cmd,input_opt);
        ncnn::VkMat input_a,input_b;
        device->convert_packing(a,input_a,1,mixed?1:net.opt.use_fp16_storage?2:1,cmd,net.opt);
        device->convert_packing(b,input_b,1,mixed?1:net.opt.use_fp16_storage?2:1,cmd,net.opt);
        // ncnn's in-place layers require owned refcounts. A GPU copy also
        // keeps the caller's immutable input banks intact across fractions.
        if(!input_a.refcount){ncnn::VkMat copy;cmd.record_clone(input_a,copy,net.opt);input_a=copy;}
        if(!input_b.refcount){ncnn::VkMat copy;cmd.record_clone(input_b,copy,net.opt);input_b=copy;}
        auto ex=net.create_extractor();
        require(ex.input(net.input_names()[0],input_a)==0 && ex.input(net.input_names()[1],input_b)==0 && ex.input(net.input_names()[2],timestep)==0,"Cannot bind Vulkan RIFE inputs");
        ncnn::VkMat result;require(ex.extract("out0",result,cmd)==0,"Vulkan RIFE inference failed");
        require(result.w==width && result.h==height && result.c*result.elempack==5,"Unexpected Vulkan flow/mask shape");return result;
    }
    std::vector<float> evaluate(const ncnn::Mat& a,const ncnn::Mat& b,float fraction,int eyes,int rounds,double& ms) {
        ncnn::VkMat av,bv;{ncnn::VkCompute upload(device);upload.record_upload(a,av,net.opt);upload.record_upload(b,bv,net.opt);require(upload.submit_and_wait()==0,"Vulkan upload failed");}
        std::vector<double> times;std::vector<float> values;
        // Equal warm-up time avoids favouring later profiles merely because
        // the device raised its clocks after the first candidate's few frames.
        const auto warm_until=Clock::now()+std::chrono::milliseconds(rounds>1?100:0);
        for(int round=-2;round<rounds;){ncnn::VkCompute cmd(device);ncnn::VkMat result;
            const auto start=Clock::now();for(int eye=0;eye<eyes;++eye)result=run(av,bv,fraction,cmd);
            require(cmd.submit_and_wait()==0,"Vulkan submit failed");
            if(round>=0)times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
            if(round==rounds-1){ncnn::VkCompute download(device);ncnn::Mat out;download.record_download(result,out,net.opt);require(download.submit_and_wait()==0,"Vulkan result download failed");
                require(out.elemsize==4u && out.elempack==1 && out.c==5,"Unexpected result precision");
                for(int c=0;c<5;++c){const float* data=out.channel(c);values.insert(values.end(),data,data+width*height);}}
            if(round<0&&Clock::now()<warm_until)continue;
            ++round;
        }
        std::sort(times.begin(),times.end());ms=times[times.size()/2];return values;
    }
};
static ncnn::Mat read_input(const fs::path& path,int w,int h,int c) {
    ncnn::Mat out(w,h,c);std::ifstream file(file_path(path),std::ios::binary);require(bool(file),"Missing numerical reference");
    for(int channel=0;channel<c;++channel){float* target=out.channel(channel);file.read(reinterpret_cast<char*>(target),std::streamsize(w)*h*4);}
    require(bool(file)&&file.peek()==EOF,"Numerical reference has incorrect size");return out;
}
static Json compare(const std::vector<float>& value,const ncnn::Mat& reference,int w,int h) {
    double flow_sum=0,mask_sum=0,flow_max=0,mask_max=0;bool finite=true;
    for(int c=0;c<5;++c){const float* ref=reference.channel(c);for(int i=0;i<w*h;++i){const float x=value[size_t(c)*w*h+i];finite&=std::isfinite(x);const double error=std::abs(double(x)-ref[i]);
        if(c<4){flow_sum+=error;flow_max=std::max(flow_max,error);}else{mask_sum+=error;mask_max=std::max(mask_max,error);finite&=x>=-.001f&&x<=1.001f;}}}
    const double flow_mean=flow_sum/(4.0*w*h),mask_mean=mask_sum/(double(w)*h);
    return {{"pass",finite&&flow_mean<=.03&&flow_max<=.5&&mask_mean<=.003&&mask_max<=.03},
        {"flow_mean",flow_mean},{"flow_max",flow_max},{"mask_mean",mask_mean},{"mask_max",mask_max},{"finite",finite}};
}
static Json cpu_reference(const Json& request) {
    const fs::path dir=file_path(fs::u8path(request.at("directory").get<std::string>()));const int w=request.at("width"),h=request.at("height");
    ncnn::Net net;net.opt.use_vulkan_compute=false;net.opt.use_packing_layout=false;net.opt.use_fp16_storage=false;net.opt.use_fp16_arithmetic=false;net.opt.num_threads=1;
    require(net.register_custom_layer("OFXRWarp",create_ofxr_warp)==0,"Cannot register CPU diagnostic warp");
    FILE* graph=_wfopen((dir/"flow.ncnn.param").c_str(),L"rb");require(graph!=nullptr,"Cannot open diagnostic graph");const auto loaded=net.load_param(graph);fclose(graph);require(loaded==0,"Cannot load diagnostic graph");
    FILE* weights=_wfopen((dir/"flow.ncnn.bin").c_str(),L"rb");require(weights!=nullptr,"Cannot open diagnostic weights");const auto model=net.load_model(weights);fclose(weights);require(model==0,"Cannot load diagnostic weights");
    auto a=read_input(dir/"a.f32",w,h,3),b=read_input(dir/"b.f32",w,h,3);Json checks=Json::array();
    for(const auto& item:std::array<std::pair<float,const char*>,3>{{{.25f,"reference25.f32"},{.5f,"reference.f32"},{.75f,"reference75.f32"}}}){
        auto ex=net.create_extractor();ncnn::Mat t(w,h,1);t.fill(item.first);require(ex.input("in0",a)==0&&ex.input("in1",b)==0&&ex.input("in2",t)==0,"Cannot bind CPU reference inputs");ncnn::Mat out;require(ex.extract("out0",out)==0,"CPU ncnn diagnostic failed");
        std::vector<float> values;for(int c=0;c<5;++c){const float* data=out.channel(c);values.insert(values.end(),data,data+w*h);}checks.push_back({{"fraction",item.first},{"accuracy",compare(values,read_input(dir/item.second,w,h,5),w,h)}});}
    return {{"checks",checks}};
}
static Json prepare(const Json& request) {
    const fs::path dir=file_path(fs::u8path(request.at("directory").get<std::string>()));const int gpu=request.at("gpu"),w=request.at("width"),h=request.at("height"),eyes=request.at("eyes"),rounds=request.at("rounds");
    require(w>=32&&h>=32&&w<=16384&&h<=16384&&w%32==0&&h%32==0&&(eyes==1||eyes==2)&&rounds>=1&&rounds<=30,"Invalid preparation dimensions");
    const auto device=identity(gpu);require(device==request.at("device"),"Vulkan adapter identity changed during preparation");
    auto a=read_input(dir/"a.f32",w,h,3),b=read_input(dir/"b.f32",w,h,3),ref=read_input(dir/"reference.f32",w,h,5);
    const auto exporter=cpu_reference(request);
    for(const auto& check:exporter.at("checks"))require(check.at("accuracy").at("pass").get<bool>(),"Exported ncnn CPU graph differs from the original model");
    std::vector<std::string> profiles{"fp32"};if(device["fp16_storage"]){profiles.push_back("storage16");if(device["fp16_arithmetic"]){profiles.push_back("fp16");if(device["cooperative_matrix"])profiles.push_back("matrix");}}
    if(device["fp16_storage"])for(const auto* policy:{"features","refine","final"}){
        profiles.push_back(std::string("mixed-")+policy+"-storage16");
        if(device["fp16_arithmetic"]){profiles.push_back(std::string("mixed-")+policy+"-fp16");if(device["cooperative_matrix"])profiles.push_back(std::string("mixed-")+policy+"-matrix");}
    }
    Json candidates=Json::array();std::string best;double best_ms=1e30;bool baseline=false;
    for(const auto& p:profiles){try {Engine engine(gpu,dir,w,h,p);double ms{};const auto output=engine.evaluate(a,b,.5f,eyes,rounds,ms);auto accuracy=compare(output,ref,w,h);
        Json fractions=Json::array();bool fraction_pass=true;
        for(const auto& test:std::array<std::pair<float,const char*>,2>{{{.25f,"reference25.f32"},{.75f,"reference75.f32"}}}){
            auto reference=read_input(dir/test.second,w,h,5);double unused{};auto value=engine.evaluate(a,b,test.first,eyes,1,unused);auto verified=compare(value,reference,w,h);
            if(p=="fp32"){std::cerr<<"fp32 fraction "<<test.first<<" "<<verified.dump()<<std::endl;require(verified["flow_max"].get<double>()<.1&&verified["mask_max"].get<double>()<.005,"FP32 fractional graph differs from CPU");}
            fraction_pass&=verified.at("pass").get<bool>();fractions.push_back({{"fraction",test.first},{"accuracy",verified}});}
        for(const auto& test:std::array<std::pair<float,const char*>,3>{{{.25f,"reference-eye1-25.f32"},{.5f,"reference-eye1-50.f32"},{.75f,"reference-eye1-75.f32"}}}){
            auto reference=read_input(dir/test.second,w,h,5);double unused{};auto value=engine.evaluate(b,a,test.first,1,1,unused);auto verified=compare(value,reference,w,h);
            if(p=="fp32")require(verified["flow_max"].get<double>()<.1&&verified["mask_max"].get<double>()<.005,"FP32 second-eye graph differs from CPU");
            fraction_pass&=verified.at("pass").get<bool>();fractions.push_back({{"eye",1},{"fraction",test.first},{"accuracy",verified}});}
        candidates.push_back({{"profile",p},{"median_ms",ms},{"accuracy",accuracy},{"fractions",fractions}});
        std::cerr<<p<<" "<<ms<<" ms "<<accuracy.dump()<<std::endl;
        if(p=="fp32"){require(accuracy["flow_max"].get<double>()<.1&&accuracy["mask_max"].get<double>()<.005,"FP32 graph differs from the CPU model");baseline=fraction_pass;}
        if(baseline&&fraction_pass&&accuracy["pass"].get<bool>()&&ms<best_ms){require(engine.cache.save_cache((dir/"pipeline.cache").c_str())==0,"Cannot save Vulkan pipeline cache");require(engine.device->get_pipeline_cache()->save_cache((dir/"warp.cache").c_str())==0,"Cannot save fused Vulkan cache");best=p;best_ms=ms;}
    }catch(const std::exception& error){candidates.push_back({{"profile",p},{"error",error.what()}});std::cerr<<p<<": "<<error.what()<<std::endl;}}
    return {{"ok",!best.empty()},{"reference_pass",!best.empty()},{"exporter_reference",exporter},{"profile",best},{"median_ms",best_ms},{"candidates",candidates},{"device",device},{"mixed_precision",read_json(dir/"mixed-precision.json")}};
}

struct Configuration {int width{},height{},eyes{};std::array<std::unique_ptr<Imported>,3> buffers;};
struct Preparation {
    HANDLE out{},in{},job{},process{},thread{};
    ULONGLONG started=GetTickCount64();
    ~Preparation(){if(thread)CloseHandle(thread);if(process)CloseHandle(process);if(job)CloseHandle(job);if(in&&in!=INVALID_HANDLE_VALUE)CloseHandle(in);if(out&&out!=INVALID_HANDLE_VALUE)CloseHandle(out);}
    bool ready(){
        const auto state=WaitForSingleObject(process,0);
        if(state==WAIT_TIMEOUT){require(GetTickCount64()-started<270000,"Radeon preparation timed out; inspect the Vulkan cache log");return false;}
        DWORD exit{};require(state==WAIT_OBJECT_0&&GetExitCodeProcess(process,&exit)&&exit==0,"Radeon preparation failed; inspect the Vulkan cache log");return true;
    }
};
static std::unique_ptr<Preparation> prepare_missing_plan(const fs::path& root,int gpu,int w,int h,int eyes) {
    const auto python=root/L"python/python.exe",script=root/L"tools/vulkan_compile.py";
    require(fs::is_regular_file(python)&&fs::is_regular_file(script),"Radeon offline compiler is missing; reinstall RIFE");
    const auto quote=[](const fs::path& path){return L"\""+path.wstring()+L"\"";};
    const auto binary=root/L"vulkan/ofxr_rife_vulkan.exe";
    std::wstring command=quote(python)+L" -I -B "+quote(script)+L" --root "+quote(root)+L" --worker "+quote(binary)+L" --pnnx "+quote(root/L"vulkan/pnnx.exe")+
        L" --cache "+quote(root/L"vulkan/cache")+L" --gpu "+std::to_wstring(gpu)+L" --width "+std::to_wstring(w)+L" --height "+std::to_wstring(h)+L" --eyes "+std::to_wstring(eyes)+L" --rounds 3";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    const auto log=root/L"vulkan/cache"/(L"prepare-"+std::to_wstring(w)+L"x"+std::to_wstring(h)+L"-"+std::to_wstring(eyes)+L".log");
    HANDLE output=CreateFileW(file_path(log).c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(output!=INVALID_HANDLE_VALUE,"Cannot create Radeon preparation log");SetHandleInformation(output,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
    HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    auto owned=std::make_unique<Preparation>();auto& handles=*owned;handles.out=output;handles.in=input;
    require(input!=INVALID_HANDLE_VALUE,"Cannot isolate Radeon compiler input");
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;start.StartupInfo.hStdInput=input;start.StartupInfo.hStdOutput=start.StartupInfo.hStdError=output;
    SIZE_T size{};InitializeProcThreadAttributeList(nullptr,1,0,&size);std::vector<std::byte> storage(size);start.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&size)!=FALSE,"Cannot isolate Radeon compiler handles");
    struct Attributes {LPPROC_THREAD_ATTRIBUTE_LIST value;~Attributes(){DeleteProcThreadAttributeList(value);}} attributes{start.lpAttributeList};
    HANDLE inherited[]{output,input};require(UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE,"Cannot configure Radeon compiler handles");
    handles.job=CreateJobObjectW(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(handles.job&&SetInformationJobObject(handles.job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Cannot supervise Radeon compiler");
    PROCESS_INFORMATION process{};require(CreateProcessW(python.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,root.c_str(),&start.StartupInfo,&process)!=FALSE,"Cannot start Radeon compiler");
    handles.process=process.hProcess;handles.thread=process.hThread;
    if(!AssignProcessToJobObject(handles.job,process.hProcess)){TerminateProcess(process.hProcess,1);throw std::runtime_error("Cannot supervise Radeon compiler process");}
    ResumeThread(process.hThread);
    return owned; // Never hold a frame-completion receipt behind compilation.
}
struct Worker {
    fs::path root,cache_root;int gpu=-1;
    xrfg::RifeRuntimeReadLease runtime_lease;
    std::unique_ptr<xrfg::RifeRuntimeReadLease> plan_lease;
    int prepared_eyes{};
    std::unique_ptr<Engine> engine;
    std::map<uint64_t,Configuration> configurations;
    std::map<std::string,std::unique_ptr<Preparation>> preparations;
    std::set<std::string> ready_plans;
    Json source_identity;
    struct LoadedPlan {std::unique_ptr<Engine> engine;std::unique_ptr<xrfg::RifeRuntimeReadLease> lease;int eyes{};};
    std::future<LoadedPlan> loading;
    explicit Worker(fs::path runtime):root(std::move(runtime)),cache_root(file_path(root/"vulkan/cache")){require(runtime_lease.acquire(file_path(root)),"Vulkan runtime is busy; installation or repair is in progress");fs::create_directories(cache_root);}
    bool plan_ready(int w,int h,int eyes) {
        const auto key=std::to_string(w)+"x"+std::to_string(h)+"-"+std::to_string(eyes);
        if(ready_plans.contains(key))return true;
        if(source_identity.is_null()){
            std::array<wchar_t,32768> own{};GetModuleFileNameW(nullptr,own.data(),static_cast<DWORD>(own.size()));
            source_identity={{"worker",sha256(own.data())},{"weights",sha256(root/"models/flownet_v4.4.pkl")},
                {"network",sha256(root/"app/models/IFNet_HDv3_v4_4.py")},{"warp",sha256(root/"app/models/warplayer.py")},
                {"compiler",sha256(root/"tools/vulkan_compile.py")},{"pnnx",sha256(root/"vulkan/pnnx.exe")}};
        }
        auto found=preparations.find(key);
        bool published=false;
        if(found!=preparations.end()){
            if(!found->second->ready())return false;
            preparations.erase(found);published=true;
        }
        const auto index_path=cache_root/(std::to_string(w)+"x"+std::to_string(h)+"-"+std::to_string(eyes)+"-"+identity(gpu).at("uuid").get<std::string>()+".json");
        bool valid=false;
        try{
            const auto index=read_json(index_path);const auto relative=fs::u8path(index.at("plan").get<std::string>());
            require(!relative.is_absolute()&&relative.filename()=="plan.json"&&relative.parent_path().filename()==relative.parent_path(),"Invalid Vulkan plan index");
            const auto plan=read_json(cache_root/relative);const auto& spec=plan.at("spec");
            valid=plan.at("ok")==true&&plan.at("reference_pass")==true&&spec.at("device")==identity(gpu)&&spec.at("width")==w&&spec.at("height")==h&&spec.at("eyes")==eyes&&spec.at("model")=="4.4";
            for(auto it=source_identity.begin();it!=source_identity.end();++it)valid&=spec.at(it.key())==it.value();
        }catch(const std::exception&){valid=false;}
        if(valid){ready_plans.insert(key);return true;}
        require(!published,"Radeon compiler did not publish a matching plan; inspect the Vulkan cache log");
        preparations.emplace(key,prepare_missing_plan(root,gpu,w,h,eyes));return false;
    }
    LoadedPlan build_plan(int w,int h,int eyes) {
        const auto index=read_json(cache_root/(std::to_string(w)+"x"+std::to_string(h)+"-"+std::to_string(eyes)+"-"+identity(gpu).at("uuid").get<std::string>()+".json"));
        const fs::path relative_plan=fs::u8path(index.at("plan").get<std::string>());
        require(!relative_plan.is_absolute()&&relative_plan.filename()=="plan.json"&&relative_plan.parent_path().filename()==relative_plan.parent_path(),"Invalid Vulkan plan index");
        const fs::path plan_path=cache_root/relative_plan;
        require(plan_path.is_absolute()&&plan_path.parent_path().parent_path()==cache_root,"Vulkan plan escapes its cache");
        auto lease=std::make_unique<xrfg::RifeRuntimeReadLease>();require(lease->acquire(plan_path.parent_path()),"Vulkan plan is being prepared by another process");
        const auto plan=read_json(plan_path);const auto& spec=plan.at("spec");
        require(plan.at("ok")==true&&plan.at("reference_pass")==true&&spec.at("width")==w&&spec.at("height")==h&&spec.at("eyes")==eyes&&spec.at("model")=="4.4","Invalid Vulkan plan");
        require(spec.at("device")==identity(gpu),"GPU or driver changed; prepare Vulkan RIFE again");
        std::array<wchar_t,32768> own{};GetModuleFileNameW(nullptr,own.data(),static_cast<DWORD>(own.size()));
        require(spec.at("worker")==sha256(own.data()),"Vulkan worker changed; prepare again");
        require(spec.at("weights")==sha256(root/"models/flownet_v4.4.pkl"),"RIFE weights changed; prepare again");
        require(spec.at("network")==sha256(root/"app/models/IFNet_HDv3_v4_4.py")&&spec.at("warp")==sha256(root/"app/models/warplayer.py")&&spec.at("compiler")==sha256(root/"tools/vulkan_compile.py")&&spec.at("pnnx")==sha256(root/"vulkan/pnnx.exe"),"RIFE Vulkan source/compiler changed; prepare again");
        for(auto it=plan.at("files").begin();it!=plan.at("files").end();++it){fs::path relative=fs::u8path(it.key());require(!relative.is_absolute()&&it.key().find("..") == std::string::npos,"Invalid Vulkan artifact path");require(it.value()==sha256(plan_path.parent_path()/relative),"Vulkan cache integrity check failed");}
        const auto dir=fs::u8path(plan.at("directory").get<std::string>());require(dir.filename()==dir,"Invalid Vulkan plan directory");
        return {std::make_unique<Engine>(gpu,plan_path.parent_path()/dir,w,h,plan.at("profile").get<std::string>()),std::move(lease),eyes};
    }
    void install_plan(LoadedPlan loaded){engine=std::move(loaded.engine);plan_lease=std::move(loaded.lease);prepared_eyes=loaded.eyes;}
    bool engine_ready(int w,int h,int eyes){
        if(engine&&engine->width==w&&engine->height==h&&prepared_eyes==eyes)return true;
        if(loading.valid()){
            if(loading.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return false;
            install_plan(loading.get());
            if(engine->width==w&&engine->height==h&&prepared_eyes==eyes)return true;
        }
        // Build private model/allocators without holding a frame receipt. The
        // main protocol thread alone publishes/swaps the completed engine.
        loading=std::async(std::launch::async,[this,w,h,eyes]{return build_plan(w,h,eyes);});
        return false;
    }
    Json command(const Json& request) {
        const std::string op=request.at("op");
        if(op=="memory")return Json::object(); // no invented Torch/private VRAM accounting
        if(op=="release"){configurations.erase(request.at("id").get<uint64_t>());if(configurations.empty()){engine.reset();plan_lease.reset();}return Json::object();}
        if(op=="prepare"){
            const int w=request.at("width"),h=request.at("height"),eyes=request.at("eyes");
            require(w>=32&&h>=32&&w<=16384&&h<=16384&&w%32==0&&h%32==0&&(eyes==1||eyes==2),"Invalid Vulkan preparation shape");
            const int selected=find_adapter(request.at("luid"));require(gpu<0||gpu==selected,"Mixed Vulkan adapters in one worker");gpu=selected;
            const bool ready=plan_ready(w,h,eyes)&&engine_ready(w,h,eyes);
            return {{"backend","vulkan"},{"preparing",!ready}};
        }
        if(op=="configure") {
            const int w=request.at("width"),h=request.at("height"),eyes=request.at("eyes");const uint64_t luid=request.at("luid");
            require(w>=32&&h>=32&&w<=16384&&h<=16384&&w%32==0&&h%32==0&&(eyes==1||eyes==2),"Invalid Vulkan shared shape");
            const auto& desc=request.at("buffers");require(desc.size()==3,"Expected three shared buffers");
            // Every duplicated NT handle is owned immediately, including on
            // a plan/adapter/import failure before any GPU work is recorded.
            std::array<HANDLE,3> handles{};for(int i=0;i<3;++i)handles[i]=reinterpret_cast<HANDLE>(desc[i][0].get<uintptr_t>());
            struct Close {std::array<HANDLE,3>& h;~Close(){for(auto value:h)if(value)CloseHandle(value);}} close{handles};
            const int selected=find_adapter(luid);require(gpu<0||gpu==selected,"Mixed Vulkan adapters in one worker");gpu=selected;
            require(identity(gpu)["fp16_storage"].get<bool>(),"This device cannot import the shared FP16 RIFE planes");
            if(!engine_ready(w,h,eyes))return {{"backend","vulkan"},{"preparing",true}};
            const auto id=request.at("id").get<uint64_t>();configurations.erase(id);Configuration config;config.width=w;config.height=h;config.eyes=eyes;
            for(int i=0;i<3;++i){auto handle=handles[i];handles[i]=nullptr;config.buffers[i]=std::make_unique<Imported>(engine->device,handle,desc[i][1].get<uint64_t>(),w,h,i==2?5:3,eyes);}
            configurations.emplace(id,std::move(config));return {{"backend","vulkan"}};
        }
        require(op=="infer"||op=="infer_batch","Unknown Vulkan worker operation");
        Json requests=op=="infer"?Json::array({request}):request.at("requests");require(requests.is_array()&&!requests.empty()&&requests.size()<=3,"Invalid fractional batch");
        std::set<uint64_t> ids;int w=0,h=0,eyes=0;
        // Reject the complete batch before touching any external buffer.
        for(const auto& item:requests){const auto id=item.at("id").get<uint64_t>();const float t=item.value("fraction",.5f);
            require(item.at("model")==3&&ids.insert(id).second&&configurations.contains(id)&&std::isfinite(t)&&t>=.05f&&t<=.95f,"Invalid Vulkan RIFE batch request");
            const auto& c=configurations.at(id);if(w)require(w==c.width&&h==c.height&&eyes==c.eyes,"Fractional Vulkan shapes differ");w=c.width;h=c.height;eyes=c.eyes;}
        if(!engine_ready(w,h,eyes))return {{"backend","vulkan"},{"preparing",true}};
        ncnn::VkCompute cmd(engine->device);
        std::vector<ncnn::VkMat> retained;
        for(const auto& item:requests){auto& c=configurations.at(item.at("id").get<uint64_t>());
            for(int eye=0;eye<eyes;++eye){auto a=c.buffers[0]->view(eye,w,h,3,&engine->blobs),b=c.buffers[1]->view(eye,w,h,3,&engine->blobs),out=c.buffers[2]->view(eye,w,h,5,&engine->blobs);
                for(auto* binding:{&a,&b,&out})cmd.ofxr_external_barrier(*binding,true);
                auto result=engine->run(a,b,item.value("fraction",.5f),cmd);ncnn::VkMat half;
                engine->device->convert_packing(result,half,1,2,cmd,engine->net.opt);
                require(half.elemsize==2u&&half.elempack==1&&half.c==5,"Unexpected FP16 Vulkan output");
                const auto external=out.buffer();cmd.record_clone(half,out,engine->net.opt);require(out.buffer()==external,"Vulkan output escaped the shared allocation");
                for(auto* binding:{&a,&b,&out})cmd.ofxr_external_barrier(*binding,false);
                retained.push_back(result);retained.push_back(half);
            }
        }
        // One receipt for all eyes/fractions; the D3D12 caller already waits
        // for input completion and never trusts output from a failed worker.
        require(cmd.submit_and_wait()==0,"Vulkan inference completion failed");
        const std::vector<std::string> codes{"fp32","storage16","fp16","matrix","mixed-features-storage16","mixed-features-fp16","mixed-features-matrix","mixed-refine-storage16","mixed-refine-fp16","mixed-refine-matrix","mixed-final-storage16","mixed-final-fp16","mixed-final-matrix"};
        const auto selected=std::find(codes.begin(),codes.end(),engine->profile);
        return {{"backend","vulkan"},{"optimizing",false},{"profile",engine->profile},{"precision_profile",selected==codes.end()?0:1+std::distance(codes.begin(),selected)}};
    }
};

// Uses real D3D12 shared resources, external Vulkan ownership transfers and
// native flow/mask readback. No headset or game is needed on a Radeon tester.
static Json interop_check(const fs::path& root,int gpu,int w=64,int h=64) {
    using Microsoft::WRL::ComPtr;
    const auto id=identity(gpu);Worker worker(root);worker.gpu=gpu;
    require(w>=32&&h>=32&&w%32==0&&h%32==0,"Invalid interop fixture dimensions");
    const auto index=read_json(root/"vulkan/cache"/(std::to_string(w)+"x"+std::to_string(h)+"-2-"+id.at("uuid").get<std::string>()+".json"));
    const auto plan_path=root/"vulkan/cache"/fs::u8path(index.at("plan").get<std::string>());
    // Offline selftest has no frame deadline; live commands poll this work.
    worker.install_plan(worker.build_plan(w,h,2));
    const auto plan=read_json(plan_path);const auto dir=plan_path.parent_path()/fs::u8path(plan.at("directory").get<std::string>());
    constexpr int eyes=2;
    const auto a=read_input(dir/"a.f32",w,h,3),b=read_input(dir/"b.f32",w,h,3),ref=read_input(dir/"reference.f32",w,h,5);
    const auto hr=[](HRESULT value){if(FAILED(value))throw std::runtime_error("D3D12 error " + std::to_string(static_cast<unsigned long>(value)));};
    ComPtr<IDXGIFactory4> factory;hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)));
    LUID luid{};const uint64_t luid_bits=id.at("luid");std::memcpy(&luid,&luid_bits,8);
    ComPtr<IDXGIAdapter1> adapter;hr(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC queue_desc{};hr(device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);require(event!=nullptr,"Cannot create D3D12 completion event");
    struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{event};uint64_t serial=0;
    const auto submit=[&]{hr(list->Close());ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);hr(queue->Signal(fence.Get(),++serial));hr(fence->SetEventOnCompletion(serial,event));require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"D3D12 test completion timed out");hr(allocator->Reset());hr(list->Reset(allocator.Get(),nullptr));};
    const auto barrier=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){D3D12_RESOURCE_BARRIER item{};item.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;item.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&item);};
    const auto resource=[&](uint64_t bytes,D3D12_HEAP_TYPE heap,bool shared){D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(shared)desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result;hr(device->CreateCommittedResource(&properties,shared?D3D12_HEAP_FLAG_SHARED:D3D12_HEAP_FLAG_NONE,&desc,heap==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:heap==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&result)));return result;};
    std::array<ComPtr<ID3D12Resource>,3> buffers;std::array<ComPtr<ID3D12Resource>,2> uploads;
    Json descriptors=Json::array();
    for(int i=0;i<3;++i){const int channels=i==2?5:3;const uint64_t bytes=uint64_t(w)*h*channels*eyes*2;buffers[i]=resource(bytes,D3D12_HEAP_TYPE_DEFAULT,true);
        if(i<2){uploads[i]=resource(bytes,D3D12_HEAP_TYPE_UPLOAD,false);void* mapped{};hr(uploads[i]->Map(0,nullptr,&mapped));auto* half=static_cast<unsigned short*>(mapped);const auto& original=i==0?a:b;
            for(int eye=0;eye<eyes;++eye)for(int c=0;c<3;++c){const auto& selected=eye==0?original:(i==0?b:a);const float* plane=selected.channel(c);for(int pixel=0;pixel<w*h;++pixel)half[(eye*3+c)*w*h+pixel]=ncnn::float32_to_float16(plane[pixel]);}
            uploads[i]->Unmap(0,nullptr);barrier(buffers[i].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);list->CopyBufferRegion(buffers[i].Get(),0,uploads[i].Get(),0,bytes);barrier(buffers[i].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);}
        HANDLE handle{};hr(device->CreateSharedHandle(buffers[i].Get(),nullptr,GENERIC_ALL,nullptr,&handle));const auto desc=buffers[i]->GetDesc();const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);descriptors.push_back({reinterpret_cast<uintptr_t>(handle),allocation.SizeInBytes});}
    submit();
    worker.command({{"op","configure"},{"id",1},{"luid",luid_bits},{"width",w},{"height",h},{"eyes",eyes},{"buffers",descriptors}});
    worker.command({{"op","infer"},{"id",1},{"model",3},{"fraction",.5}});
    const uint64_t bytes=uint64_t(w)*h*eyes*5*2;
    std::array<ComPtr<ID3D12Resource>,3> outputs{buffers[2],resource(bytes,D3D12_HEAP_TYPE_DEFAULT,true),resource(bytes,D3D12_HEAP_TYPE_DEFAULT,true)};
    for(int bank=1;bank<3;++bank){Json bank_desc=Json::array();for(int i=0;i<3;++i){auto object=i==2?outputs[bank]:buffers[i];HANDLE handle{};hr(device->CreateSharedHandle(object.Get(),nullptr,GENERIC_ALL,nullptr,&handle));const auto desc=object->GetDesc();const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);bank_desc.push_back({reinterpret_cast<uintptr_t>(handle),allocation.SizeInBytes});}
        worker.command({{"op","configure"},{"id",bank+1},{"luid",luid_bits},{"width",w},{"height",h},{"eyes",eyes},{"buffers",bank_desc}});}
    worker.command({{"op","infer_batch"},{"requests",Json::array({{{"id",1},{"model",3},{"fraction",.25}},{{"id",2},{"model",3},{"fraction",.5}},{{"id",3},{"model",3},{"fraction",.75}}})}});
    auto readback=resource(bytes*3,D3D12_HEAP_TYPE_READBACK,false);
    for(int bank=0;bank<3;++bank){barrier(outputs[bank].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyBufferRegion(readback.Get(),bytes*bank,outputs[bank].Get(),0,bytes);barrier(outputs[bank].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);}submit();
    void* mapped{};hr(readback->Map(0,nullptr,&mapped));const auto* half=static_cast<const unsigned short*>(mapped);
    const char* references[3][2]={{"reference25.f32","reference-eye1-25.f32"},{"reference.f32","reference-eye1-50.f32"},{"reference75.f32","reference-eye1-75.f32"}};
    Json comparisons=Json::array();for(int bank=0;bank<3;++bank)for(int eye=0;eye<eyes;++eye){std::vector<float> output(size_t(w)*h*5);for(size_t i=0;i<output.size();++i)output[i]=ncnn::float16_to_float32(half[(size_t(bank)*eyes+eye)*output.size()+i]);auto reference=read_input(dir/references[bank][eye],w,h,5);comparisons.push_back({{"bank",bank},{"eye",eye},{"accuracy",compare(output,reference,w,h)}});}
    readback->Unmap(0,nullptr);for(int bank=1;bank<=3;++bank)worker.command({{"op","release"},{"id",bank}});hr(list->Close());
    const bool pass=std::all_of(comparisons.begin(),comparisons.end(),[](const Json& v){return v.at("accuracy").at("pass").get<bool>();});
    return {{"ok",pass},{"device",id},{"interop_pass",pass},{"fraction_batch",true},{"width",w},{"height",h},{"outputs",comparisons},{"profile",plan.at("profile")}};
}
static int run_main(int argc,char** argv) {
    try {
        require(ncnn::create_gpu_instance()==0,"Vulkan initialization failed");
        require(argc>=3,"Use --probe <gpu>, --prepare <request.json> or --worker <runtime>");
        const std::string op=argv[1];
        if(op=="--probe"){const int requested=std::stoi(argv[2]);const int selected=requested<0?preferred_adapter():requested;const auto id=identity(selected);std::cout<<Json({{"ok",true},{"gpu",selected},{"device",id}}).dump(2)<<std::endl;}
        else if(op=="--cpu-reference"){std::cout<<cpu_reference(read_json(fs::u8path(argv[2]))).dump(2)<<std::endl;}
        else if(op=="--prepare"){const auto request=read_json(fs::u8path(argv[2]));const auto result=prepare(request);write_json(fs::u8path(request.at("directory").get<std::string>())/"result.json",result);return result.at("ok").get<bool>()?0:1;}
        else if(op=="--selftest"){const auto result=interop_check(fs::u8path(argv[2]),argc>3?std::stoi(argv[3]):0,argc>4?std::stoi(argv[4]):64,argc>5?std::stoi(argv[5]):64);std::cout<<result.dump(2)<<std::endl;return result.at("ok").get<bool>()?0:1;}
        else if(op=="--worker") {Worker worker(fs::u8path(argv[2]));std::cout<<Json({{"ok",true},{"protocol",2},{"fraction_batch",true},{"backend","vulkan"}}).dump(-1,' ',false,Json::error_handler_t::strict)<<std::endl;
            for(std::string line;std::getline(std::cin,line);){if(line.size()>65536)break;try{auto result=worker.command(Json::parse(line));result["ok"]=true;std::cout<<result.dump()<<std::endl;}catch(const std::exception& error){std::cout<<Json({{"ok",false},{"error",error.what()}}).dump()<<std::endl;}}}
        else throw std::runtime_error("Unknown Vulkan worker mode");
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;std::cout<<Json({{"ok",false},{"error",error.what()}}).dump()<<std::endl;return 1;}
    return 0;
}
int wmain(int argc,wchar_t** arguments) {
    std::vector<std::string> values;for(int i=0;i<argc;++i)values.push_back(utf8(fs::path(arguments[i])));
    std::vector<char*> argv;for(auto& value:values)argv.push_back(value.data());
    return run_main(argc,argv.data());
}
