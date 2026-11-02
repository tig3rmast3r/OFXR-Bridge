Texture2D<float2> CurrentMotion : register(t0);
Texture2D<float> CurrentDepth : register(t1);
Texture2D<float2> MiddleMotion : register(t2);
Texture2D<float> MiddleDepth : register(t3);
RWTexture2D<float2> SameEyeMotion : register(u0);

cbuffer Composition : register(b0)
{
    uint2 SourceBase;
    uint2 CropOrigin;
    float2 GuideExtent;
    float2 RawScale;
    float2 JitterDifference;
    float RawNativeSpan;
    float Padding;
    column_major float4x4 ToOpposite;
    column_major float4x4 ToSame;
    column_major float4x4 StereoInterval;
};

// Finite half sentinel excludes invalid flow; no clamped/missing correspondence.
static const float2 InvalidMotion = float2(65504.0,65504.0);
[numthreads(8, 8, 1)]
void ComposeLukeSameEyeMotion(uint3 thread_id : SV_DispatchThreadID)
{
    uint width, height;
    SameEyeMotion.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height) return;
    uint2 source=CropOrigin+thread_id.xy;
    float2 raw=CurrentMotion.Load(int3(SourceBase+source,0));
    float depth=CurrentDepth.Load(int3(thread_id.xy,0));
    float2 uv=(float2(source)+0.5)/GuideExtent;
    float4 clip=float4(uv.x*2.0-1.0,1.0-uv.y*2.0,depth,1.0);
    float4 opposite=mul(ToOpposite,clip);
    float4 same=mul(ToSame,clip);
    bool object_history=Padding==1.0;
    float4 stereo=object_history?clip:mul(StereoInterval,clip);
    float4 far_clip=clip; far_clip.z=0.0;
    float4 stereo_far=object_history?far_clip:mul(StereoInterval,far_clip);
    if(!all(isfinite(raw)) || !isfinite(depth) || depth<0.0 || depth>1.0 ||
        !all(isfinite(opposite)) || !all(isfinite(same)) ||
        !all(isfinite(stereo)) || !all(isfinite(stereo_far)) ||
        opposite.w<=1e-8 || same.w<=1e-8 || stereo.w<=1e-8 || stereo_far.w<=1e-8) {
        SameEyeMotion[thread_id.xy]=InvalidMotion; return;
    }
    float4 raw_previous=RawNativeSpan==2.0?same:opposite;
    float2 difference=(same.xy/same.w-raw_previous.xy/raw_previous.w-
        (stereo.xy/stereo.w-stereo_far.xy/stereo_far.w))*float2(0.5,-0.5);
    // raw - camera(current->its declared previous) + camera(current->same): object motion
    // remains native. Depth at THIS pixel never samples another surface's field.
    float2 result=raw+(difference*GuideExtent+JitterDifference)/RawScale;
    if(object_history) {
        // Follow the native current->middle correspondence, rather than
        // assuming uniform head/camera or object velocity across A/B/A.
        float2 p=float2(source)+raw*RawScale;
        if(all(p>=0.0) && all(p<GuideExtent-1.0)) {
            int2 base=int2(floor(p)); float2 weight=frac(p);
            int2 address=int2(SourceBase)+base;
            float2 a=MiddleMotion.Load(int3(address,0));
            float2 b=MiddleMotion.Load(int3(address+int2(1,0),0));
            float2 c=MiddleMotion.Load(int3(address+int2(0,1),0));
            float2 d=MiddleMotion.Load(int3(address+int2(1,1),0));
            float za=MiddleDepth.Load(int3(address,0));
            float zb=MiddleDepth.Load(int3(address+int2(1,0),0));
            float zc=MiddleDepth.Load(int3(address+int2(0,1),0));
            float zd=MiddleDepth.Load(int3(address+int2(1,1),0));
            float z=lerp(lerp(za,zb,weight.x),lerp(zc,zd,weight.x),weight.y);
            float4 middle_clip=float4(((p+0.5)/GuideExtent)*float2(2,-2)+float2(-1,1),z,1);
            // In this explicitly admitted path StereoInterval carries the
            // exact native middle->older camera, not a stereo approximation.
            float4 older=mul(StereoInterval,middle_clip);
            if(all(isfinite(a)) && all(isfinite(b)) && all(isfinite(c)) && all(isfinite(d)) &&
                all(abs(a)<65504.0) && all(abs(b)<65504.0) &&
                all(abs(c)<65504.0) && all(abs(d)<65504.0) &&
                all(isfinite(float4(za,zb,zc,zd))) && z>0.0 && z<1.0 &&
                all(isfinite(older)) && older.w>1e-8) {
                float2 camera=(older.xy/older.w-middle_clip.xy)*float2(0.5,-0.5)*GuideExtent;
                // The measured middle interval already carries its jitter;
                // replace the missing-interval estimate instead of adding it twice.
                result+=lerp(lerp(a,b,weight.x),lerp(c,d,weight.x),weight.y)-
                    (camera+JitterDifference)/RawScale;
            }
        }
    }
    if(!all(isfinite(result)) || any(abs(result)>65504.0))
        result=InvalidMotion;
    SameEyeMotion[thread_id.xy]=result;
}
