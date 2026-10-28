#pragma once
// Ported from wilsjo2/OptiScaler-DLSSNR-PreSR-Multipass v0.8.10,
// 6125a75b37f7e2a408e31934f4827e5660429203. Match private guides to
// model colour with independent point-sampled regions; preserve vector units.
inline constexpr char kNrResizeGuidesShader[]=R"(
cbuffer Params:register(b0) {
 uint mode;float white;uint width,height;
 float transfer,colour;uint debug;float maxRatio;
 uint passthrough;float mvScaleX,mvScaleY;uint guideWidth;
 uint guideHeight,compareMode;float compareSplit,compareZoom;
 uint compareSwap,depthX,depthY,depthWidth;
 uint depthHeight,motionX,motionY,motionWidth;
 uint motionHeight;
};
Texture2D<float4> depthIn:register(t0);
Texture2D<float4> motionIn:register(t1);
RWTexture2D<float4> depthOut:register(u0);
RWTexture2D<float4> motionOut:register(u1);
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 float2 uv=(float2(id.xy)+0.5)/float2(width,height);
 uint2 dp=uint2(depthX,depthY)+min(uint2(uv*float2(depthWidth,depthHeight)),uint2(depthWidth-1,depthHeight-1));
 uint2 mp=uint2(motionX,motionY)+min(uint2(uv*float2(motionWidth,motionHeight)),uint2(motionWidth-1,motionHeight-1));
 depthOut[id.xy]=depthIn.Load(int3(dp,0));
 motionOut[id.xy]=motionIn.Load(int3(mp,0));
}
)";
