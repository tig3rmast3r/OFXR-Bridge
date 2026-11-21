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
groupshared float2 PatchBaseline[64];
groupshared float2 PatchError[64];
groupshared float2 PatchGain[64];
groupshared uint PatchCount[64];

struct Probe {
    int2 texel;
    float2 current_uv, native_uv, raw_motion, target_native_uv, adjacent_uv, same_eye_uv, errors;
    uint flags;
};

void ProjectProbe(uint i,out Probe p) {
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
}

void Measure(uint i,out Probe p) {
    ProjectProbe(i,p);
    if(!(p.flags&4))return;
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
    uint at=24+68*index;
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

float ColorError(float3 a,float3 b) {
    return dot(abs(a-b),float3(1,1,1))/3;
}

bool PatchFits(float2 center,float2 radius) {
    return all(center>=radius)&&all(center<1-radius);
}

void AccumulatePatch(uint i,float2 radius,inout float2 baseline,inout float2 warped,
    inout float2 gain,inout uint2 support,inout uint count) {
    Probe p;
    ProjectProbe(i,p);
    if(!(p.flags&4))return;
    // Measure the same native neighborhood in all three independently cropped images.
    float2 adjacent_radius=radius*RectCurrent.zw/RectMiddle.zw;
    float2 same_eye_radius=radius*RectCurrent.zw/RectPrevious.zw;
    float2 adjacent_start=(p.native_uv-RectMiddle.xy)/RectMiddle.zw;
    float2 same_eye_start=(p.native_uv-RectPrevious.xy)/RectPrevious.zw;
    if(!PatchFits(p.current_uv,radius)||
        !PatchFits(p.adjacent_uv,adjacent_radius)||!PatchFits(adjacent_start,adjacent_radius)||
        !PatchFits(p.same_eye_uv,same_eye_radius)||!PatchFits(same_eye_start,same_eye_radius))return;
    float2 before=0,after=0;
    const float2 taps[5]={float2(0,0),float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    [unroll] for(uint tap=0;tap<5;++tap) {
        float2 offset=taps[tap];
        float3 current=Current.SampleLevel(LinearClamp,p.current_uv+offset*radius,0).rgb;
        float2 da=offset*adjacent_radius,ds=offset*same_eye_radius;
        before.x+=ColorError(current,Middle.SampleLevel(LinearClamp,adjacent_start+da,0).rgb);
        before.y+=ColorError(current,Previous.SampleLevel(LinearClamp,same_eye_start+ds,0).rgb);
        after.x+=ColorError(current,Middle.SampleLevel(LinearClamp,p.adjacent_uv+da,0).rgb);
        after.y+=ColorError(current,Previous.SampleLevel(LinearClamp,p.same_eye_uv+ds,0).rgb);
    }
    if(!all(isfinite(before))||!all(isfinite(after)))return;
    // Bound individual patches so an occlusion or highlight cannot dominate.
    before=min(before*.2,.15);
    after=min(after*.2,.15);
    float2 improvement=clamp(before-after,-.04,.04);
    baseline+=before;
    warped+=after;
    gain+=improvement;
    support+=uint2(improvement.x>=.002?1:0,improvement.y>=.002?1:0);
    ++count;
}

bool PatchWins(uint candidate) {
    uint other=candidate^1;
    return PatchCount[0]>=64&&(candidate?Two[0]:One[0])>=32&&
        PatchGain[0][candidate]>=max(.00012*PatchCount[0],.02*PatchBaseline[0][candidate])&&
        PatchGain[0][candidate]>PatchGain[0][other]+.00020*PatchCount[0]&&
        PatchError[0][candidate]<PatchError[0][other];
}

// Start with the strong single-pixel votes. On ambiguity, test whether the raw
// motion improves many local patches relative to each reference's own baseline.
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
    uint2 votes=uint2(One[0],Two[0]);
    uint interval=votes.x>=32&&votes.x>2*votes.y?1:(votes.y>=32&&votes.y>2*votes.x?2:0);
    bool try_patch=Enabled!=0&&interval==0;
    uint2 patch_votes=0;
    if(try_patch) {
        uint width,height;
        Current.GetDimensions(width,height);
        float2 radius=4./float2(width,height);
        float2 baseline=0,warped=0,gain=0;
        uint count=0;
        for(uint i=lane;i<576;i+=64)AccumulatePatch(i,radius,baseline,warped,gain,patch_votes,count);
        // All lanes must consume the first vote before reusing the reduction storage.
        GroupMemoryBarrierWithGroupSync();
        One[lane]=patch_votes.x;Two[lane]=patch_votes.y;
        PatchBaseline[lane]=baseline;PatchError[lane]=warped;PatchGain[lane]=gain;PatchCount[lane]=count;
        GroupMemoryBarrierWithGroupSync();
        for(uint stride=32;stride;stride>>=1) {
            if(lane<stride) {
                One[lane]+=One[lane+stride];Two[lane]+=Two[lane+stride];
                PatchBaseline[lane]+=PatchBaseline[lane+stride];PatchError[lane]+=PatchError[lane+stride];
                PatchGain[lane]+=PatchGain[lane+stride];PatchCount[lane]+=PatchCount[lane+stride];
            }
            GroupMemoryBarrierWithGroupSync();
        }
        patch_votes=uint2(One[0],Two[0]);
        if(PatchWins(0))interval=1;
        else if(PatchWins(1))interval=2;
    }
    if(!lane) {
        Decision.Store3(0,uint3(interval,votes));
        Decision.Store3(12,uint3(try_patch?1:0,patch_votes));
    }
}
