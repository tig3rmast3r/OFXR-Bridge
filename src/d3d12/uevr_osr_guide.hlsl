// Frame generation guides for UEVR's Alternating/AFR under OSR: each eye of
// every frame, the real one relocated to its slot and the reconstructed one,
// gets a depth and a motion field of its own, as a game that renders both eyes
// every frame would give DLSS-G. The depth is the eye's shown image's own (the
// warp's splat into that image); the motion points at where the surface was in
// that eye's previous frame, in this grid's texels (DLSS convention: previous
// minus current): the camera's share from the depth through the two shown
// cameras, plus the object's own share from the real eye's DLSS vectors.
Texture2D<uint>   ShownDepth   : register(t0); // the shown image's splatted depth: asuint, 0 = nothing landed
Texture2D<float>  SourceDepth  : register(t1); // the real eye's DLSS depth, as rendered
Texture2D<float2> SourceMotion : register(t2); // its raw DLSS vectors; x MotionScale = depth texels
RWTexture2D<float2> MotionOut  : register(u0);
RWTexture2D<float>  DepthOut   : register(u1);

cbuffer Guide : register(b0) {
    row_major float4x4 ShownToPrevious; // shown clip -> clip of this eye's previous shown frame
    row_major float4x4 ShownToSource;   // shown clip -> the real eye's render clip
    row_major float4x4 SourceToHistory; // the real eye's render clip -> clip of the frame its vectors point at
    uint2 Size;          // the grid, shared by every texture here
    float2 MotionScale;  // raw vectors -> texels; zero: no object motion
    float ObjectWeight;  // share of the vectors' object motion that falls in one frame
    uint Flags;          // 1: previous frame known (else no motion)
};

static const float FarDepth = 1e-6;

float shown_depth(int2 c) {
    // A crack of the splat takes the nearest neighbour that landed.
    uint best = ShownDepth.Load(int3(c, 0));
    if (best == 0u) {
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
                best = max(best, ShownDepth.Load(int3(clamp(c + int2(dx, dy), int2(0, 0), int2(Size) - 1), 0)));
    }
    return best != 0u ? asfloat(best) : FarDepth;
}

bool project(float2 pixel, float z, float4x4 m, out float2 landed, out float landed_z) {
    const float2 ndc = float2(pixel.x / Size.x * 2.0 - 1.0, 1.0 - pixel.y / Size.y * 2.0);
    const float4 q = mul(float4(ndc, max(z, FarDepth), 1.0), m);
    landed = 0; landed_z = 0;
    bool ok = all(isfinite(q)) && q.w > 1e-8;
    if (ok) {
        landed = float2((q.x / q.w + 1.0) * 0.5 * Size.x, (1.0 - q.y / q.w) * 0.5 * Size.y);
        landed_z = q.z / q.w;
        ok = all(isfinite(landed)) && isfinite(landed_z);
    }
    return ok;
}

[numthreads(8, 8, 1)]
void UevrOsrGuide(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= Size)) return;
    const float z = shown_depth(int2(id.xy));
    DepthOut[id.xy] = z;
    float2 motion = 0;
    const float2 pixel = float2(id.xy) + 0.5;
    float2 previous; float pz;
    if ((Flags & 1u) != 0 && project(pixel, z, ShownToPrevious, previous, pz)) {
        motion = previous - pixel;
        // The object's own motion, where the real eye saw this surface: its
        // vectors minus the camera's share they carry, cut to one frame.
        float2 source; float sz;
        if (ObjectWeight > 0 && (MotionScale.x != 0 || MotionScale.y != 0) &&
            project(pixel, z, ShownToSource, source, sz) && all(source >= 0) && all(source < float2(Size))) {
            const int2 s = int2(source);
            const float source_z = SourceDepth.Load(int3(s, 0));
            const float2 total = SourceMotion.Load(int3(s, 0)) * MotionScale;
            float2 before; float bz;
            if (isfinite(source_z) && all(isfinite(total)) && all(abs(total) < 4096) &&
                project(float2(s) + 0.5, source_z, SourceToHistory, before, bz)) {
                const float2 object = total - (before - (float2(s) + 0.5));
                if (all(isfinite(object)) && all(abs(object) < 4096)) motion += ObjectWeight * object;
            }
        }
        if (!all(isfinite(motion)) || any(abs(motion) > 8192)) motion = 0;
    }
    MotionOut[id.xy] = motion;
}
