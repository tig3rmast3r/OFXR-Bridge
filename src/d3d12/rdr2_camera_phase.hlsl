Texture2D<float4> Source : register(t0);
Texture2D<uint> Scene : register(t1);
RWTexture2D<float4> Output : register(u0);
cbuffer Parameters : register(b0) {
    row_major float4x4 TargetToSource;
    uint2 Size; uint2 Padding;
};
[numthreads(8,8,1)]
void CameraPhase(uint3 id : SV_DispatchThreadID) {
    if(any(id.xy>=Size))return;
    // The CPU never reads scene state. A GPU-detected cut uses an exact copy.
    if(Scene.Load(int3(0,0,0))==0 || Scene.Load(int3(2,0,0))!=0) {
        Output[id.xy]=Source.Load(int3(id.xy,0));return;
    }
    const float2 ndc=(float2(id.xy)+.5)/float2(Size)*float2(2,-2)+float2(-1,1);
    const float4 q=mul(float4(ndc,0,1),TargetToSource);
    if(!all(isfinite(q))||q.w<=1e-8){Output[id.xy]=Source.Load(int3(id.xy,0));return;}
    const float2 p=(q.xy/q.w*float2(.5,-.5)+.5)*float2(Size)-.5;
    const int2 lo=int2(floor(p)),limit=int2(Size)-1;const float2 f=frac(p);
    // Bounded yaw can expose a narrow border; clamp rather than introduce black.
    const float4 a=Source.Load(int3(clamp(lo,0,limit),0)),b=Source.Load(int3(clamp(lo+int2(1,0),0,limit),0));
    const float4 c=Source.Load(int3(clamp(lo+int2(0,1),0,limit),0)),d=Source.Load(int3(clamp(lo+1,0,limit),0));
    Output[id.xy]=lerp(lerp(a,b,f.x),lerp(c,d,f.x),f.y);
}
