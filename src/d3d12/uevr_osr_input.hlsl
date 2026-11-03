// OSR for UEVR's Alternating/AFR rendering: takes the source eye's DLSS depth
// and motion vectors out of the game's render targets, at the rectangle DLSS
// read them from, into the grids Full OSR samples (osr_full_warp.hlsl).
Texture2D<float> GuideDepth : register(t0);
Texture2D<float2> GuideMotion : register(t1);
RWTexture2D<float> DepthOut : register(u0);   // reversed depth, as UE renders it
RWTexture2D<float2> MotionOut : register(u1); // raw vectors; x guide scale = depth texels

cbuffer Window : register(b0) {
    uint2 DepthOrigin;
    uint2 MotionOrigin;
    uint2 Size;       // the depth window, the grid written
    uint2 MotionSize; // the vector window: the depth window, or the output size (full-resolution vectors)
};

[numthreads(8, 8, 1)]
void UevrOsrInputs(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    DepthOut[id.xy] = GuideDepth.Load(int3(id.xy + DepthOrigin, 0));
    // Full-resolution vectors (DLSS without MVLowRes, Hogwarts Legacy) cover
    // the same field at the output size: each depth texel takes the vector at
    // its centre. Their values stay in vector texels (the history scale says so).
    const uint2 m = min(uint2((float2(id.xy) + 0.5) * float2(MotionSize) / float2(Size)), MotionSize - 1);
    MotionOut[id.xy] = GuideMotion.Load(int3(m + MotionOrigin, 0));
}
