cbuffer Params : register(b0) {
    float4 rotation;
    float4 sourceFov;
    float4 targetFov;
    float4 sourceRect;
    uint workWidth, workHeight, sourceSlice, planeOffset;
    uint textureWidth, textureHeight, proxy, sourceSliceA;
    float4 rotationA;
    float4 sourceFovA;
};
Texture2DArray<float4> source : register(t0);
Texture2DArray<float4> peerSource : register(t1);
Buffer<float> result : register(t2);
RWBuffer<float> packed : register(u0);
SamplerState linearClamp : register(s0);
float3 encode(float3 c) {
    float3 v=saturate(c);
    if (proxy==1) v=.5 + .5*c/(1+abs(c));
    else if (proxy==2) v=lerp(1.055*pow(max(c,1e-8),1.0/2.4)-.055,12.92*c,c<=.0031308);
    return v;
}
float3 decode(float3 c) {
    float3 v=c;
    if (proxy==1) { float3 x=clamp(2*c-1,-.999,.999); v=x/(1-abs(x)); }
    else if (proxy==2) v=lerp(pow(max((c+.055)/1.055,1e-8),2.4),c/12.92,c<=.04045);
    return v;
}
float2 sourceUV(float2 uv, float4 rot, float4 fov) {
    float3 ray=float3(lerp(targetFov.x,targetFov.y,uv.x),lerp(targetFov.z,targetFov.w,uv.y),-1);
    ray += 2*cross(rot.xyz,cross(rot.xyz,ray)+rot.w*ray);
    float2 p=ray.xy/max(-ray.z,1e-6);
    float2 v=(p-float2(fov.x,fov.z))/float2(fov.y-fov.x,fov.w-fov.z);
    float2 pixel=sourceRect.xy+clamp(v,0,1)*sourceRect.zw;
    pixel=clamp(pixel,sourceRect.xy+.5,sourceRect.xy+sourceRect.zw-.5);
    return pixel/float2(textureWidth,textureHeight);
}
[numthreads(8,8,1)] void pack(uint3 tid:SV_DispatchThreadID) {
    if(tid.x>=workWidth || tid.y>=workHeight) return;
    float2 uv=(float2(tid.xy)+.5)/float2(workWidth,workHeight);
    float3 rgb=encode(source.SampleLevel(linearClamp,float3(sourceUV(uv,rotation,sourceFov),sourceSlice),0).rgb);
    uint base=planeOffset+tid.y*workWidth+tid.x, plane=workWidth*workHeight;
    packed[base]=rgb.r; packed[base+plane]=rgb.g; packed[base+2*plane]=rgb.b;
}
struct Vertex {float4 position:SV_Position;float2 uv:TEXCOORD;};
Vertex vertex(uint id:SV_VertexID) {Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 loadFlow(int2 p) {
    p=clamp(p,int2(0,0),int2(workWidth-1,workHeight-1));
    uint base=planeOffset+p.y*workWidth+p.x,plane=workWidth*workHeight;
    return float4(result[base],result[base+plane],result[base+2*plane],result[base+3*plane]);
}
float loadMask(int2 p) {
    p=clamp(p,int2(0,0),int2(workWidth-1,workHeight-1));
    return result[planeOffset+4*workWidth*workHeight+p.y*workWidth+p.x];
}
float4 unpack(Vertex v):SV_Target {
    float2 p=v.uv*float2(workWidth,workHeight)-.5,f=frac(p);int2 q=int2(floor(p));
    float4 flow=lerp(lerp(loadFlow(q),loadFlow(q+int2(1,0)),f.x),lerp(loadFlow(q+int2(0,1)),loadFlow(q+1),f.x),f.y);
    float mask=saturate(lerp(lerp(loadMask(q),loadMask(q+int2(1,0)),f.x),lerp(loadMask(q+int2(0,1)),loadMask(q+1),f.x),f.y));
    float2 inverseSize=1.0/float2(workWidth,workHeight);
    float4 a=source.SampleLevel(linearClamp,float3(sourceUV(v.uv+flow.xy*inverseSize,rotationA,sourceFovA),sourceSliceA),0);
    float4 b=peerSource.SampleLevel(linearClamp,float3(sourceUV(v.uv+flow.zw*inverseSize,rotation,sourceFov),sourceSlice),0);
    return float4(decode(lerp(encode(b.rgb),encode(a.rgb),mask)),lerp(b.a,a.a,mask));
}
