Texture2D<float4> Current : register(t0);
Texture2D<float4> Middle : register(t1);
Texture2D<float4> Previous : register(t2);
Texture2DArray<float2> Motion : register(t3);
Texture2D<float> Depth : register(t4);
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
    uint CameraEnabled;
    column_major float4x4 ToAdjacent;
    column_major float4x4 ToSameEye;
};

#define INTERVAL_CURRENT Current
#define INTERVAL_ADJACENT Middle
#define INTERVAL_SAME_EYE Previous
#include "motion_interval.hlsli"

void LoadIntervalMotion(float2 native_uv, float2 motion_size,
    out int2 texel, out float2 raw_motion, out bool valid) {
    texel=clamp(int2(native_uv*motion_size),0,int2(motion_size)-1);
    raw_motion=Motion.Load(int4(texel,Slice,0));
    valid=true;
}
float LoadIntervalDepth(int2 texel) {
    return Depth.Load(int3(texel,0));
}

// This is the same probe used for voting, with no second dispatch or
// reconstruction from a color history that may have advanced.
void StoreIntervalProbe(uint index,MotionIntervalProbe p) {
    uint at=36+68*index;
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

[numthreads(64,1,1)]
void Classify(uint lane : SV_GroupIndex) {
    MotionIntervalGeometry geometry={RectCurrent,RectMiddle,RectPrevious,MotionSize,MotionScale,Jitter};
    const MotionIntervalDecision result=ClassifyMotionInterval(lane,geometry,Enabled!=0,CameraEnabled!=0,
        ToAdjacent,ToSameEye,Capture!=0);
    if(!lane) {
        Decision.Store3(0,uint3(result.interval,result.point_votes));
        Decision.Store3(12,uint3(result.patch_attempted?1:0,result.patch_votes));
        const uint camera_interval=result.evidence==3u?result.interval:0u;
        Decision.Store3(24,uint3((result.camera_attempted?4u:0u)|camera_interval,result.camera_votes));
    }
}
