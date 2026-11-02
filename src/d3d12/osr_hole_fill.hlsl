// OSR (OFXR Stereo Reconstruction), first pass: the stereo hole fill.
//
// AFW reconstructs the peer eye from the source eye's colour and depth plus the
// peer eye's own previous frame. Background that the source eye hides behind a
// near object and that the previous frame has not seen either (the band on the
// leading side of near objects while strafing) has no source pixels. This pass
// takes exactly the inputs AFW receives, finds that band from geometry alone and
// replaces the provider's streaked fill with a mirrored copy of the adjacent
// background. Nothing here reads provider code or state.
//
// Conventions follow luke_compose_motion.hlsl: texel row 0 is the top of the
// image, clip y is up, and matrices transform row vectors (p' = p * M), the same
// storage the provider's CameraData uses. Depth is the provider's: 1 = near.

Texture2D<float> SourceDepth : register(t0);
Texture2D<float4> Color : register(t1);
Texture2D<uint> CoverageIn : register(t2);
RWTexture2D<uint> CoverageOut : register(u0);
RWTexture2D<float4> Output : register(u1);

cbuffer Osr : register(b0) {
    row_major float4x4 SourceToDestination; // source clip -> destination clip
    uint2 DepthSize;
    uint2 ColorSize;
    float FillFraction;    // part of each hole run, from the occluder edge, that history cannot see
    uint Flags;            // bit 0: debug tint
    uint MinRun;           // holes narrower than this (depth texels) are splat cracks, not bands
    uint MaxRun;           // holes wider than this are not stereo bands
    uint BackgroundMargin; // covered texels beside the run, on the background side, treated as band
    uint OccluderMargin;   // covered texels beside the run, on the occluder side, treated as band
};

[numthreads(8, 8, 1)]
void OsrClearCoverage(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DepthSize)) return;
    CoverageOut[id.xy] = 0;
}

// Forward-splat every source depth texel into the destination eye. A texel
// records the nearest destination depth that lands on it; zero means no source
// surface reaches it, i.e. the destination sees something the source does not.
[numthreads(8, 8, 1)]
void OsrSplatCoverage(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DepthSize)) return;
    float z = SourceDepth.Load(int3(id.xy, 0));
    if (!isfinite(z) || z < 0.0 || z > 1.0) return;
    float2 ndc = float2((id.x + 0.5) / DepthSize.x * 2.0 - 1.0,
                        1.0 - (id.y + 0.5) / DepthSize.y * 2.0);
    float4 q = mul(float4(ndc, z, 1.0), SourceToDestination);
    if (!all(isfinite(q)) || q.w <= 1e-8) return;
    float2 p = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x - 0.5,
                      (1.0 - q.y / q.w) * 0.5 * DepthSize.y - 0.5);
    if (!all(isfinite(p))) return;
    uint word = max(asuint(saturate(q.z / q.w)), 1u);
    int2 base = int2(floor(p));
    // A 2x2 footprint closes the cracks of mild magnification; MinRun below
    // discards what remains of stronger stretching.
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 t = base + int2(dx, dy);
            if (all(t >= 0) && all(t < int2(DepthSize)))
                InterlockedMax(CoverageOut[t], word);
        }
}

float4 tint(float4 color, float3 mark, float amount) {
    return float4(lerp(color.rgb, mark, amount), color.a);
}

uint coverage_at(int x, int y) { return CoverageIn.Load(int3(x, y, 0)); }

[numthreads(8, 8, 1)]
void OsrFillHoles(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ColorSize)) return;
    float4 color = Color.Load(int3(id.xy, 0));
    float2 scale = float2(ColorSize) / float2(DepthSize);
    int2 c = clamp(int2(floor((float2(id.xy) + 0.5) / scale)), 0, int2(DepthSize) - 1);
    const int width = (int)DepthSize.x;
    const bool debug = (Flags & 1u) != 0;
    const bool own_hole = coverage_at(c.x, c.y) == 0;
    // Locate the hole run this pixel belongs to: its own, or one within the
    // margins when the pixel is a covered neighbour.
    const int search = max((int)BackgroundMargin, (int)OccluderMargin);
    int first = c.x, last = c.x; // hole texels [first, last]
    if (own_hole) {
        [loop] while (first > 0 && c.x - first < (int)MaxRun && coverage_at(first - 1, c.y) == 0) --first;
        [loop] while (last < width - 1 && last - c.x < (int)MaxRun && coverage_at(last + 1, c.y) == 0) ++last;
    } else {
        bool found = false;
        [loop] for (int k = 1; k <= search && !found; ++k) {
            if (c.x + k < width && coverage_at(c.x + k, c.y) == 0) { first = last = c.x + k; found = true; }
            else if (c.x - k >= 0 && coverage_at(c.x - k, c.y) == 0) { first = last = c.x - k; found = true; }
        }
        if (!found) { Output[id.xy] = color; return; }
        [loop] while (first > 0 && last - first < (int)MaxRun && coverage_at(first - 1, c.y) == 0) --first;
        [loop] while (last < width - 1 && last - first < (int)MaxRun && coverage_at(last + 1, c.y) == 0) ++last;
    }
    const int left = first - 1, right = last + 1; // bounding covered texels
    const int run = last - first + 1;
    // Runs reaching the image border, cracks and oversized holes keep the
    // provider's own fill.
    if (left < 0 || right >= width || run < (int)MinRun || run > (int)MaxRun) { Output[id.xy] = color; return; }
    const uint zl = coverage_at(left, c.y), zr = coverage_at(right, c.y);
    // Reversed depth: the larger word is the nearer surface, the occluder.
    const bool occluder_left = asfloat(zl) > asfloat(zr);
    const int fill_width = (int)ceil(FillFraction * run);
    if (own_hole) {
        const int distance = occluder_left ? c.x - left : right - c.x; // 1..run from the occluder
        if (distance > fill_width) {
            // The same eye's previous frame saw this part; the provider fills it.
            Output[id.xy] = debug ? tint(color, float3(0, 1, 1), 0.35) : color;
            return;
        }
    } else {
        // A margin texel joins the band within OccluderMargin on the occluder
        // side (the provider's smeared edge, counted as covered by the splat
        // footprint) and within BackgroundMargin on the background side, the
        // latter only when the whole run is being filled.
        const bool pixel_left_of_run = c.x < first;
        const int k = pixel_left_of_run ? first - c.x : c.x - last;
        const bool occluder_side = pixel_left_of_run == occluder_left;
        const bool joins = occluder_side ? (k <= (int)OccluderMargin && fill_width >= 1)
                                         : (k <= (int)BackgroundMargin && fill_width >= run);
        if (!joins) { Output[id.xy] = color; return; }
    }
    // Mirror the background across the run's background edge, pushed out by
    // the margin so the provider's contaminated edge texels are not copied,
    // softened by a 3x3 box so no column repeats as a streak.
    const int margin = (int)BackgroundMargin;
    const float xb = occluder_left ? (right + margin) * scale.x : (left + 1 - margin) * scale.x - 1.0;
    const int xs = (int)round(2.0 * xb - id.x);
    float4 sum = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) {
            int2 s = clamp(int2(xs + i, (int)id.y + j), 0, int2(ColorSize) - 1);
            sum += Color.Load(int3(s, 0));
        }
    const float4 fill = sum / 9.0;
    Output[id.xy] = debug ? tint(fill, own_hole ? float3(1, 0, 1) : float3(1, 0.5, 0), 0.5) : fill;
}
