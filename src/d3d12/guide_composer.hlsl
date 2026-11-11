// An eye's DLSS vectors as they reach its own previous image (D3D12GuideComposer).
//
// A game that renders both eyes through one view writes each eye's vectors
// from the view's previous frame, which is the other eye's (B, this eye's
// image, points into R, the other eye's image just before it). R's own vectors
// lead on from there into A, this eye's previous image. Followed one after the
// other they give B -> A, and the eyes' different projections cancel.
//
// Decide looks at a grid of points: there vectors into the other eye differ
// between the eyes by what their projections differ by (for mirrored fields
// of view about twice the shift of a distant point from one eye's image to the
// other's), while vectors into each eye's own previous image differ by little.
// Compose follows B's vectors through R's where most points agree that they
// point into the other eye, and copies them otherwise.
Texture2DArray<float2> MotionB : register(t0);
Texture2DArray<float2> MotionR : register(t1);
Texture2DArray<float> DepthB : register(t2);
Texture2DArray<float> DepthR : register(t3);
RWTexture2D<float2> Composed : register(u0);
RWByteAddressBuffer Decision : register(u1);

cbuffer Params : register(b0) {
    uint2 OriginB;
    uint2 SizeB;
    uint2 OriginR;
    uint2 SizeR;
    uint SliceB;
    uint SliceR;
    // Motion units to output pixels.
    float2 ScaleB;
    float2 ScaleR;
    float2 Output;
    // Tangents of the eyes' fields of view: left, right, down, up.
    float4 TangentsB;
    float4 TangentsR;
    // Smaller projection differences, in output pixels, cannot be told from
    // motion, and leave the vectors as they are.
    float MinimumOffset;
    uint ForceCompose;
    uint2 DepthPadding;
    float4 DepthRectB;
    float4 DepthRectR;
    float4 NearFar;
    uint DepthFlags;
    uint RejectOcclusions;
    uint DepthSliceB;
    uint DepthSliceR;
};

float view_depth(float depth,float near_z,float far_z,bool inverted,bool infinite) {
    return infinite?near_z/max(inverted?depth:1-depth,1.e-8):
        near_z*far_z/max(inverted?near_z+depth*(far_z-near_z):far_z-depth*(far_z-near_z),1.e-8);
}

static const uint kGrid = 8;
groupshared uint g_cross;

float2 load_b(uint2 texel) { return MotionB.Load(int4(texel, SliceB, 0)) * ScaleB; }
float2 load_r(uint2 texel) { return MotionR.Load(int4(texel, SliceR, 0)) * ScaleR; }

// Where a distant point at u of one eye's image sits in the other's, less u,
// in output pixels.
float other_eye_shift(float u, float4 from, float4 to) {
    float t = lerp(from.x, from.y, u);
    return ((t - to.x) / (to.y - to.x) - u) * Output.x;
}

[numthreads(kGrid, kGrid, 1)]
void Decide(uint3 id : SV_GroupThreadID) {
    if (all(id.xy == 0)) g_cross = 0;
    GroupMemoryBarrierWithGroupSync();
    // The central 80%, where both eyes see the same scene.
    float2 u = 0.1 + 0.8 * (float2(id.xy) + 0.5) / kGrid;
    float2 b = load_b(OriginB + min(uint2(u * SizeB), SizeB - 1));
    float2 r = load_r(OriginR + min(uint2(u * SizeR), SizeR - 1));
    float expected = other_eye_shift(u.x, TangentsB, TangentsR) - other_eye_shift(u.x, TangentsR, TangentsB);
    float difference = b.x - r.x;
    if (abs(expected) >= MinimumOffset && abs(difference - expected) < abs(difference)) {
        InterlockedAdd(g_cross, 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (all(id.xy == 0)) {
        Decision.Store2(0, uint2(g_cross, kGrid * kGrid));
    }
}

[numthreads(8, 8, 1)]
void Compose(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= SizeB)) return;
    float2 raw = MotionB.Load(int4(OriginB + id.xy, SliceB, 0));
    uint2 votes = Decision.Load2(0);
    // Seven points in ten: the rest may be near objects, sky or particles.
    if (!ForceCompose && votes.x * 10 < votes.y * 7) {
        Composed[id.xy] = raw;
        return;
    }
    float2 b = raw * ScaleB;
    float2 u = (float2(id.xy) + 0.5) / float2(SizeB) + b / Output;
    if(RejectOcclusions) {
        bool valid=all(isfinite(raw)) && all(u>=0) && all(u<1);
        float db=DepthB.Load(int4(int2(DepthRectB.xy+(float2(id.xy)+.5)/SizeB*DepthRectB.zw),DepthSliceB,0));
        float dr=DepthR.Load(int4(int2(DepthRectR.xy+saturate(u)*DepthRectR.zw),DepthSliceR,0));
        float zb=view_depth(db,NearFar.x,NearFar.y,(DepthFlags&1)!=0,(DepthFlags&4)!=0);
        float zr=view_depth(dr,NearFar.z,NearFar.w,(DepthFlags&2)!=0,(DepthFlags&8)!=0);
        valid=valid && isfinite(db) && isfinite(dr) && db>=0 && db<=1 && dr>=0 && dr<=1 &&
            max(zb,zr)<=1.25*min(zb,zr)+.01;
        if(!valid) {
            // Hybrid-only invalid correspondence, never a fabricated vector.
            Composed[id.xy]=asfloat(uint2(0x7fc00000,0x7fc00000));return;
        }
    }
    uint2 r_texel = OriginR + uint2(clamp(u * float2(SizeR), 0.0, float2(SizeR) - 1.0));
    float2 r = load_r(r_texel);
    Composed[id.xy] = (b + r) / ScaleB;
}
