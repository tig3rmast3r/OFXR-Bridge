Texture2D<float4> Current : register(t0);
Texture2D<float4> Middle : register(t1);
Texture2D<float4> Previous : register(t2);
Texture2DArray<float2> Motion : register(t3);
RWByteAddressBuffer Decision : register(u0);
SamplerState LinearClamp : register(s0);
cbuffer Params : register(b0) {
    float4 RectCurrent;
    float4 RectMiddle;
    float4 RectPrevious;
    float2 MotionSize;
    float2 MotionScale;
    float2 Jitter;
    uint Enabled;
    uint Slice;
};
groupshared uint One[64];
groupshared uint Two[64];
// Same 576-sample, whole-frame colour vote and ambiguity threshold as OSR's
// RDR2 interval classifier. Coordinates retain each exact XR/native crop.
[numthreads(64,1,1)]
void Classify(uint lane : SV_GroupIndex) {
    uint one=0,two=0;
    if(Enabled)for(uint i=lane;i<576;i+=64) {
        float2 uv=.05+.9*(float2(i%32,i/32)+.5)/float2(32,18);
        float2 native=RectCurrent.xy+uv*RectCurrent.zw;
        int2 p=clamp(int2(native*MotionSize),0,int2(MotionSize)-1);
        float2 raw=Motion.Load(int4(p,Slice,0))*MotionScale+Jitter;
        if(!all(isfinite(raw))||any(abs(raw*MotionSize)>4096))continue;
        float2 at=native+raw;
        float2 r=(at-RectMiddle.xy)/RectMiddle.zw;
        float2 a=(at-RectPrevious.xy)/RectPrevious.zw;
        if(any(r<0)||any(r>=1)||any(a<0)||any(a>=1))continue;
        float3 b=Current.SampleLevel(LinearClamp,uv,0).rgb;
        float3 c=Middle.SampleLevel(LinearClamp,r,0).rgb;
        float3 d=Previous.SampleLevel(LinearClamp,a,0).rgb;
        if(!all(isfinite(b))||!all(isfinite(c))||!all(isfinite(d)))continue;
        float e1=dot(abs(b-c),float3(1,1,1))/3;
        float e2=dot(abs(b-d),float3(1,1,1))/3;
        one+=(e1<.15 && e1+.015<e2)?1:0;
        two+=(e2<.15 && e2+.015<e1)?1:0;
    }
    One[lane]=one;Two[lane]=two;GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride;stride>>=1) {
        if(lane<stride){One[lane]+=One[lane+stride];Two[lane]+=Two[lane+stride];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(!lane)Decision.Store3(0,uint3(One[0]>=32 && One[0]>2*Two[0]?1:
        (Two[0]>=32 && Two[0]>2*One[0]?2:0),One[0],Two[0]));
}
