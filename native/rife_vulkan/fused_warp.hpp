#pragma once
#include "layer.h"
#include "gpu.h"
#include "pipeline.h"
#include "command.h"
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <cmath>

// Fuses normalized-grid construction, permutation and border bilinear sampling.
// Coordinates/interpolation are FP32 even when image/flow storage is FP16.
// Pixel-coordinate flow avoids storing a low-precision normalized grid.
class OFXRWarp final : public ncnn::Layer {
    ncnn::Pipeline* pipeline_{};
public:
    OFXRWarp(){one_blob_only=false;support_inplace=false;support_vulkan=true;support_vulkan_packing=false;}
    int create_pipeline(const ncnn::Option& opt) override {
        if(!opt.use_vulkan_compute)return 0;
        static const char* shader=R"glsl(#version 450
layout(binding=0) readonly buffer Image { sfp image_data[]; };
layout(binding=1) readonly buffer Flow { sfp flow_data[]; };
layout(binding=2) writeonly buffer Result { sfp result_data[]; };
layout(push_constant) uniform parameter { int w; int h; int channels; int image_step; int flow_step; int result_step; } p;
void main() {
    int x=int(gl_GlobalInvocationID.x), y=int(gl_GlobalInvocationID.y), c=int(gl_GlobalInvocationID.z);
    if(x>=p.w || y>=p.h || c>=p.channels)return;
    int pixel=y*p.w+x;
    float fx=float(flow_data[pixel]), fy=float(flow_data[p.flow_step+pixel]);
    vec2 q=clamp(vec2(x,y)+vec2(fx,fy),vec2(0),vec2(p.w-1,p.h-1));
    ivec2 lo=ivec2(floor(q)), hi=min(lo+1,ivec2(p.w-1,p.h-1));
    vec2 t=q-vec2(lo);
    int base=c*p.image_step;
    float v00=float(image_data[base+lo.y*p.w+lo.x]);
    float v01=float(image_data[base+lo.y*p.w+hi.x]);
    float v10=float(image_data[base+hi.y*p.w+lo.x]);
    float v11=float(image_data[base+hi.y*p.w+hi.x]);
    result_data[c*p.result_step+pixel]=sfp(mix(mix(v00,v01,t.x),mix(v10,v11,t.x),t.y));
}
)glsl";
        ncnn::Option options=opt;options.use_fp16_arithmetic=false;
        std::vector<uint32_t> spirv;if(ncnn::compile_spirv_module(shader,options,spirv)!=0)return -1;
        pipeline_=new ncnn::Pipeline(vkdev);pipeline_->set_optimal_local_size_xyz(8,8,1);
        return pipeline_->create(spirv.data(),spirv.size()*sizeof(uint32_t),{});
    }
    int destroy_pipeline(const ncnn::Option&) override {delete pipeline_;pipeline_=nullptr;return 0;}
    // Explicit exporter diagnostic only. Live Engine rejects all CPU fallback
    // layers and supplies VkMat inputs, so this never handles game tensors.
    int forward(const std::vector<ncnn::Mat>& input,std::vector<ncnn::Mat>& output,const ncnn::Option& opt) const override {
        if(input.size()!=2||output.size()!=1||input[0].elempack!=1||input[1].elempack!=1||input[0].elemsize!=4u||input[1].elemsize!=4u)return -1;
        const auto& image=input[0];const auto& flow=input[1];auto& out=output[0];out.create(image.w,image.h,image.c,4u,opt.blob_allocator);
        const float* fx=flow.channel(0);const float* fy=flow.channel(1);
        for(int c=0;c<image.c;++c){const float* source=image.channel(c);float* result=out.channel(c);
            for(int y=0;y<image.h;++y)for(int x=0;x<image.w;++x){const int pixel=y*image.w+x;
                const float qx=std::clamp(float(x)+fx[pixel],0.f,float(image.w-1)),qy=std::clamp(float(y)+fy[pixel],0.f,float(image.h-1));
                const int lx=int(std::floor(qx)),ly=int(std::floor(qy)),hx=std::min(lx+1,image.w-1),hy=std::min(ly+1,image.h-1);const float tx=qx-lx,ty=qy-ly;
                result[pixel]=std::lerp(std::lerp(source[ly*image.w+lx],source[ly*image.w+hx],tx),std::lerp(source[hy*image.w+lx],source[hy*image.w+hx],tx),ty);}}
        return 0;
    }
    int forward(const std::vector<ncnn::VkMat>& input,std::vector<ncnn::VkMat>& output,ncnn::VkCompute& cmd,const ncnn::Option& opt) const override {
        if(input.size()!=2||output.size()!=1||input[0].elempack!=1||input[1].elempack!=1||input[1].c!=2||input[0].w!=input[1].w||input[0].h!=input[1].h)return -1;
        auto& out=output[0];out.create(input[0].w,input[0].h,input[0].c,input[0].elemsize,opt.blob_vkallocator);if(out.empty())return -100;
        std::vector<ncnn::vk_constant_type> constants(6);constants[0].i=input[0].w;constants[1].i=input[0].h;constants[2].i=input[0].c;
        constants[3].i=static_cast<int>(input[0].cstep);constants[4].i=static_cast<int>(input[1].cstep);constants[5].i=static_cast<int>(out.cstep);
        cmd.record_pipeline(pipeline_,{input[0],input[1],out},constants,out);return 0;
    }
};
static ncnn::Layer* create_ofxr_warp(void*) {return new OFXRWarp;}

class OFXRTime {
    std::unique_ptr<ncnn::Pipeline> pipeline_;
public:
    OFXRTime(const ncnn::VulkanDevice* device,const ncnn::Option& opt) {
        static const char* shader=R"glsl(#version 450
layout(binding=0) writeonly buffer Result { sfp data[]; };
layout(push_constant) uniform parameter { int count; float fraction; } p;
void main() {uint i=gl_GlobalInvocationID.x;if(i<uint(p.count))data[i]=sfp(p.fraction);}
)glsl";
        std::vector<uint32_t> spirv; if(ncnn::compile_spirv_module(shader,opt,spirv)!=0)throw std::runtime_error("Cannot compile Vulkan timestep fill");
        pipeline_=std::make_unique<ncnn::Pipeline>(device);pipeline_->set_optimal_local_size_xyz(128,1,1);
        if(pipeline_->create(spirv.data(),spirv.size()*sizeof(uint32_t),{})!=0)throw std::runtime_error("Cannot create Vulkan timestep pipeline");
    }
    ncnn::VkMat record(int w,int h,float fraction,ncnn::VkCompute& cmd,const ncnn::Option& opt) const {
        ncnn::VkMat result(w,h,1,opt.use_fp16_storage?2u:4u,opt.blob_vkallocator);
        if(result.empty())throw std::runtime_error("Cannot allocate Vulkan timestep");
        std::vector<ncnn::vk_constant_type> constants(2);constants[0].i=w*h;constants[1].f=fraction;
        ncnn::VkMat dispatcher;dispatcher.w=w*h;dispatcher.h=1;dispatcher.c=1;
        cmd.record_pipeline(pipeline_.get(),std::vector<ncnn::VkMat>{result},constants,dispatcher);return result;
    }
};
