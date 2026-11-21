groupshared uint One[64];
groupshared uint Two[64];
groupshared uint2 CameraVotes[64];
groupshared float2 PatchBaseline[64];
groupshared float2 PatchError[64];
groupshared float2 PatchGain[64];
groupshared uint PatchCount[64];

struct MotionIntervalGeometry {
    float4 current_rect, adjacent_rect, same_eye_rect;
    float2 motion_size, motion_scale, jitter;
};
struct MotionIntervalDecision {
    uint interval, evidence;
    uint2 point_votes, patch_votes, camera_votes;
    bool patch_attempted, camera_attempted;
};
struct MotionIntervalProbe {
    int2 texel;
    float2 current_uv, native_uv, raw_motion, target_native_uv, adjacent_uv, same_eye_uv, errors;
    uint flags;
};

// The caller supplies the resource layout. All comparisons use the same
// normalized geometry and vote thresholds in AER and OSR.
void LoadIntervalMotion(float2 native_uv, float2 motion_size,
    out int2 texel, out float2 raw_motion, out bool valid);
float LoadIntervalDepth(int2 texel);
void StoreIntervalProbe(uint i, MotionIntervalProbe p);

void ProjectProbe(uint i,MotionIntervalGeometry geometry,out MotionIntervalProbe p) {
    p=(MotionIntervalProbe)0;
    p.current_uv=.05+.9*(float2(i%32,i/32)+.5)/float2(32,18);
    p.native_uv=geometry.current_rect.xy+p.current_uv*geometry.current_rect.zw;
    bool loaded;
    LoadIntervalMotion(p.native_uv,geometry.motion_size,p.texel,p.raw_motion,loaded);
    if (!loaded) return;
    p.flags=1;
    float2 raw=p.raw_motion*geometry.motion_scale+geometry.jitter;
    if(!all(isfinite(raw))||any(abs(raw*geometry.motion_size)>4096))return;
    p.flags|=2;
    p.target_native_uv=p.native_uv+raw;
    p.adjacent_uv=(p.target_native_uv-geometry.adjacent_rect.xy)/geometry.adjacent_rect.zw;
    p.same_eye_uv=(p.target_native_uv-geometry.same_eye_rect.xy)/geometry.same_eye_rect.zw;
    if(any(p.adjacent_uv<0)||any(p.adjacent_uv>=1)||any(p.same_eye_uv<0)||any(p.same_eye_uv>=1))return;
    p.flags|=4;
}

void Measure(uint i,MotionIntervalGeometry geometry,out MotionIntervalProbe p) {
    ProjectProbe(i,geometry,p);
    if(!(p.flags&4))return;
    float3 b=INTERVAL_CURRENT.SampleLevel(LinearClamp,p.current_uv,0).rgb;
    float3 c=INTERVAL_ADJACENT.SampleLevel(LinearClamp,p.adjacent_uv,0).rgb;
    float3 d=INTERVAL_SAME_EYE.SampleLevel(LinearClamp,p.same_eye_uv,0).rgb;
    if(!all(isfinite(b))||!all(isfinite(c))||!all(isfinite(d)))return;
    float e1=dot(abs(b-c),float3(1,1,1))/3;
    float e2=dot(abs(b-d),float3(1,1,1))/3;
    p.errors=float2(e1,e2);
    p.flags|=8;
    if(e1<.15 && e1+.015<e2)p.flags|=16;
    if(e2<.15 && e2+.015<e1)p.flags|=32;
}

float ColorError(float3 a,float3 b) {
    return dot(abs(a-b),float3(1,1,1))/3;
}

bool PatchFits(float2 center,float2 radius) {
    return all(center>=radius)&&all(center<1-radius);
}

void AccumulatePatch(uint i,MotionIntervalGeometry geometry,float2 radius,inout float2 baseline,inout float2 warped,
    inout float2 gain,inout uint2 support,inout uint count) {
    MotionIntervalProbe p;
    ProjectProbe(i,geometry,p);
    if(!(p.flags&4))return;
    // Measure the same native neighborhood in all three independently cropped images.
    float2 adjacent_radius=radius*geometry.current_rect.zw/geometry.adjacent_rect.zw;
    float2 same_eye_radius=radius*geometry.current_rect.zw/geometry.same_eye_rect.zw;
    float2 adjacent_start=(p.native_uv-geometry.adjacent_rect.xy)/geometry.adjacent_rect.zw;
    float2 same_eye_start=(p.native_uv-geometry.same_eye_rect.xy)/geometry.same_eye_rect.zw;
    if(!PatchFits(p.current_uv,radius)||
        !PatchFits(p.adjacent_uv,adjacent_radius)||!PatchFits(adjacent_start,adjacent_radius)||
        !PatchFits(p.same_eye_uv,same_eye_radius)||!PatchFits(same_eye_start,same_eye_radius))return;
    float2 before=0,after=0;
    const float2 taps[5]={float2(0,0),float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    [unroll] for(uint tap=0;tap<5;++tap) {
        float2 offset=taps[tap];
        float3 current=INTERVAL_CURRENT.SampleLevel(LinearClamp,p.current_uv+offset*radius,0).rgb;
        float2 da=offset*adjacent_radius,ds=offset*same_eye_radius;
        before.x+=ColorError(current,INTERVAL_ADJACENT.SampleLevel(LinearClamp,adjacent_start+da,0).rgb);
        before.y+=ColorError(current,INTERVAL_SAME_EYE.SampleLevel(LinearClamp,same_eye_start+ds,0).rgb);
        after.x+=ColorError(current,INTERVAL_ADJACENT.SampleLevel(LinearClamp,p.adjacent_uv+da,0).rgb);
        after.y+=ColorError(current,INTERVAL_SAME_EYE.SampleLevel(LinearClamp,p.same_eye_uv+ds,0).rgb);
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

float2 NativeUV(float4 clip) {
    return float2(.5,-.5)*(clip.xy/clip.w)+.5;
}

void AccumulateCameraVote(MotionIntervalProbe p,MotionIntervalGeometry geometry,
    float4x4 to_adjacent,float4x4 to_same_eye,inout uint2 votes) {
    if (!(p.flags&4)) return;
    const float z=LoadIntervalDepth(p.texel);
    if (!isfinite(z) || z<=0 || z>=1) return;
    const float4 clip=float4(p.native_uv*float2(2,-2)+float2(-1,1),z,1);
    const float4 adjacent=mul(to_adjacent,clip), same_eye=mul(to_same_eye,clip);
    if (!all(isfinite(adjacent)) || !all(isfinite(same_eye)) ||
        adjacent.w<=1e-6 || same_eye.w<=1e-6 ||
        adjacent.z<0 || same_eye.z<0 || adjacent.z>adjacent.w || same_eye.z>same_eye.w) return;
    const float2 adjacent_uv=NativeUV(adjacent), same_eye_uv=NativeUV(same_eye);
    if (any(adjacent_uv<geometry.adjacent_rect.xy) || any(adjacent_uv>=geometry.adjacent_rect.xy+geometry.adjacent_rect.zw) ||
        any(same_eye_uv<geometry.same_eye_rect.xy) || any(same_eye_uv>=geometry.same_eye_rect.xy+geometry.same_eye_rect.zw)) return;
    const float one=length((p.target_native_uv-adjacent_uv)*geometry.motion_size);
    const float two=length((p.target_native_uv-same_eye_uv)*geometry.motion_size);
    // Only a close match to stationary geometry counts. Ignore moving
    // surfaces and samples where the two camera predictions are too close.
    votes+=uint2(one<=1.5 && two>=one+2.0, two<=1.5 && one>=two+2.0);
}

// Called by all lanes in one 64-thread group. Each dispatch supplies exact
// source images first, and enables the camera evidence only with verified maps.
MotionIntervalDecision ClassifyMotionInterval(uint lane, MotionIntervalGeometry geometry, bool enabled,
    bool camera_enabled, float4x4 to_adjacent, float4x4 to_same_eye, bool capture) {
    uint one=0,two=0;
    if(enabled)for(uint i=lane;i<576;i+=64) {
        MotionIntervalProbe p;
        Measure(i,geometry,p);
        one+=(p.flags&16)?1:0;
        two+=(p.flags&32)?1:0;
        if(capture)StoreIntervalProbe(i,p);
    }
    One[lane]=one;Two[lane]=two;GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride;stride>>=1) {
        if(lane<stride){One[lane]+=One[lane+stride];Two[lane]+=Two[lane+stride];}
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 votes=uint2(One[0],Two[0]);
    uint interval=votes.x>=32&&votes.x>2*votes.y?1:(votes.y>=32&&votes.y>2*votes.x?2:0);
    bool try_patch=enabled&&interval==0;
    uint2 patch_votes=0;
    uint2 camera_votes=0;
    uint camera_interval=0;
    if(try_patch) {
        uint width,height;
        INTERVAL_CURRENT.GetDimensions(width,height);
        float2 radius=4./float2(width,height);
        float2 baseline=0,warped=0,gain=0;
        uint count=0;
        for(uint i=lane;i<576;i+=64)AccumulatePatch(i,geometry,radius,baseline,warped,gain,patch_votes,count);
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
    const bool try_camera=camera_enabled && interval==0;
    if (try_camera) {
        for(uint i=lane;i<576;i+=64) {
            MotionIntervalProbe p;
            ProjectProbe(i,geometry,p);
            AccumulateCameraVote(p,geometry,to_adjacent,to_same_eye,camera_votes);
        }
        CameraVotes[lane]=camera_votes;
        GroupMemoryBarrierWithGroupSync();
        for(uint stride=32;stride;stride>>=1) {
            if(lane<stride) CameraVotes[lane]+=CameraVotes[lane+stride];
            GroupMemoryBarrierWithGroupSync();
        }
        camera_votes=CameraVotes[0];
        camera_interval=camera_votes.x>=96 && camera_votes.x>3*camera_votes.y?1:
            (camera_votes.y>=96 && camera_votes.y>3*camera_votes.x?2:0);
        interval=camera_interval;
    }
    MotionIntervalDecision result;
    result.interval=interval;
    result.evidence=camera_interval?3u:(interval?(try_patch?2u:1u):0u);
    result.point_votes=votes;
    result.patch_votes=patch_votes;
    result.camera_votes=camera_votes;
    result.patch_attempted=try_patch;
    result.camera_attempted=try_camera;
    return result;
}
