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
    uint Capture;
};
groupshared uint One[64];
groupshared uint Two[64];

struct Probe {
    int2 texel;
    float2 current_uv, native_uv, raw_motion, target_native_uv, adjacent_uv, same_eye_uv, errors;
    uint flags;
};

void Measure(uint i,out Probe p) {
    p=(Probe)0;
    p.current_uv=.05+.9*(float2(i%32,i/32)+.5)/float2(32,18);
    p.native_uv=RectCurrent.xy+p.current_uv*RectCurrent.zw;
    p.texel=clamp(int2(p.native_uv*MotionSize),0,int2(MotionSize)-1);
    p.raw_motion=Motion.Load(int4(p.texel,Slice,0));
    p.flags=1;
    float2 raw=p.raw_motion*MotionScale+Jitter;
    if(!all(isfinite(raw))||any(abs(raw*MotionSize)>4096))return;
    p.flags|=2;
    p.target_native_uv=p.native_uv+raw;
    p.adjacent_uv=(p.target_native_uv-RectMiddle.xy)/RectMiddle.zw;
    p.same_eye_uv=(p.target_native_uv-RectPrevious.xy)/RectPrevious.zw;
    if(any(p.adjacent_uv<0)||any(p.adjacent_uv>=1)||any(p.same_eye_uv<0)||any(p.same_eye_uv>=1))return;
    p.flags|=4;
    float3 b=Current.SampleLevel(LinearClamp,p.current_uv,0).rgb;
    float3 c=Middle.SampleLevel(LinearClamp,p.adjacent_uv,0).rgb;
    float3 d=Previous.SampleLevel(LinearClamp,p.same_eye_uv,0).rgb;
    if(!all(isfinite(b))||!all(isfinite(c))||!all(isfinite(d)))return;
    float e1=dot(abs(b-c),float3(1,1,1))/3;
    float e2=dot(abs(b-d),float3(1,1,1))/3;
    p.errors=float2(e1,e2);
    p.flags|=8;
    if(e1<.15 && e1+.015<e2)p.flags|=16;
    if(e2<.15 && e2+.015<e1)p.flags|=32;
}

// This is the same probe used for voting, with no second dispatch or
// reconstruction from a color history that may have advanced.
void StoreProbe(uint index,Probe p) {
    uint at=12+68*index;
    Decision.Store2(at,asuint(p.texel));
    Decision.Store2(at+8,asuint(p.current_uv));
    Decision.Store2(at+16,asuint(p.native_uv));
    Decision.Store2(at+24,asuint(p.raw_motion));
    Decision.Store2(at+32,asuint(p.target_native_uv));
    Decision.Store2(at+40,asuint(p.adjacent_uv));
    Decision.Store2(at+48,asuint(p.same_eye_uv));
    Decision.Store2(at+56,asuint(p.errors));
    Decision.Store(at+64,p.flags);
}

// Same 576-sample, whole-frame colour vote and ambiguity threshold as OSR's
// RDR2 interval classifier. Coordinates retain each exact XR/native crop.
[numthreads(64,1,1)]
void Classify(uint lane : SV_GroupIndex) {
    uint one=0,two=0;
    if(Enabled)for(uint i=lane;i<576;i+=64) {
        Probe p;
        Measure(i,p);
        one+=(p.flags&16)?1:0;
        two+=(p.flags&32)?1:0;
        if(Capture)StoreProbe(i,p);
    }
    One[lane]=one;Two[lane]=two;GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride;stride>>=1) {
        if(lane<stride){One[lane]+=One[lane+stride];Two[lane]+=Two[lane+stride];}
        GroupMemoryBarrierWithGroupSync();
    }
    if(!lane)Decision.Store3(0,uint3(One[0]>=32 && One[0]>2*Two[0]?1:
        (Two[0]>=32 && Two[0]>2*One[0]?2:0),One[0],Two[0]));
}
