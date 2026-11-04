#pragma once
// NR proxy only: do not alter the game's HDR image, exposure, guides or model settings.
// 16x16 stratified samples and a log-average luminance, retained entirely on GPU.
inline constexpr char kNrExposureShader[]=R"(
Texture2D<float4> source : register(t0);
RWTexture2D<float> exposure : register(u0);
cbuffer Seed : register(b0) { uint mode;float white;uint width,height;float transfer,colour;uint debug;float ratio;uint reset; };
groupshared float sums[256];
[numthreads(16,16,1)] void main(uint3 tid:SV_GroupThreadID,uint index:SV_GroupIndex) {
 uint w,h;source.GetDimensions(w,h);
 uint2 p=min(uint2((float2(tid.xy)+.5)*float2(w,h)/16),uint2(w-1,h-1));
 float3 rgb=max(source.Load(int3(p,0)).rgb,0);
 float luma=dot(rgb,float3(.2126,.7152,.0722));
 sums[index]=log2(max(isfinite(luma)?luma:0,1e-5));GroupMemoryBarrierWithGroupSync();
 for(uint step=128;step>0;step>>=1){if(index<step)sums[index]+=sums[index+step];GroupMemoryBarrierWithGroupSync();}
 if(index==0){float target=clamp(exp2(sums[0]/256)/.18,1,65536);
  float previous=reset?target:exposure[uint2(0,0)];
  if(!isfinite(previous)||previous<1)previous=target;
  exposure[uint2(0,0)]=lerp(previous,target,.125);}
})";
// Patch only WhitePoint in the accepted codec's immutable per-dispatch constants.
// Main encode/resolve bytecode, HDR reconstruction and Compare semantics are unchanged.
inline constexpr char kNrExposureConstantsShader[]=R"(
cbuffer Seed:register(b0){uint4 words[16];};
Texture2D<float> exposure:register(t0);
RWByteAddressBuffer constants:register(u0);
[numthreads(1,1,1)] void main(){
 for(uint i=0;i<16;++i)constants.Store4(i*16,words[i]);
 constants.Store(4,asuint(asfloat(words[0].y)*exposure.Load(int3(0,0,0))));
})";
