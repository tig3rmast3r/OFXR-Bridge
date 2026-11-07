// OSR (OFXR Stereo Reconstruction), full reconstruction of the peer eye.
//
// The layer hands the AFW provider one real eye (colour, converted depth,
// composed motion) and the camera matrices of both eyes; the provider writes
// the other eye. Full OSR writes that eye itself from the same inputs:
//
//   1. OsrFullClear        zero the splat and offset targets (depth resolution)
//   2. OsrFullSplat        forward-splat the source depth into the destination
//                          eye, keeping the nearest depth per texel
//      OsrFullSplatHistory the same for the destination eye's own previous
//                          frame, each texel first moved by its object motion
//      OsrFullSplatOffset  record where in the previous frame each winning
//                          texel's surface is
//   3. OsrFullResolve      per colour pixel: the previous frame first where it
//                          shows the warp's surface (read at the nearest pixel),
//                          else the source eye (Catmull-Rom), with the halo,
//                          camera-attached and fringe rules; otherwise a band
//   4. OsrFullInpaint      fill the bands neither image saw: the background
//                          mirrored across the run's background edge, or the
//                          farthest surface found in eight directions
//
// Conventions follow osr_hole_fill.hlsl: texel row 0 is the top, clip y is up,
// matrices transform row vectors (p' = p * M) exactly as the provider's
// CameraData stores them, and depth is the provider's reversed depth: 1 = near,
// 0 = far or missing. A reprojected depth at or beyond the far plane (slightly
// negative with a finite far plane) is clamped to far, never rejected.

Texture2D<float>  SourceDepth  : register(t0); // depth resolution
Texture2D<float4> SourceColor  : register(t1); // colour resolution
Texture2D<uint>   StereoDepth  : register(t2); // splatted source depth, 0 = nothing landed
Texture2D<uint>   HistorySplat : register(t3); // splatted history depth
Texture2D<float>  HistoryDepth : register(t4); // destination eye, previous frame
Texture2D<float4> HistoryColor : register(t5);
Texture2D<uint>   Resolved     : register(t6); // colour resolution: depth bits | tag (0 hole, 1 stereo, 2 history, 3 moved)
Texture2D<float4> Warped       : register(t7);
Texture2D<float2> HistoryMotion : register(t8); // composed same-eye motion of the history frame
Texture2D<uint>   HistoryOffset : register(t9); // per destination texel: where in the history image its surface is
RWTexture2D<uint>   SplatOut    : register(u0);
RWTexture2D<uint>   SplatOut2   : register(u1);
RWTexture2D<float4> WarpedOut   : register(u2);
RWTexture2D<uint>   ResolvedOut : register(u3);
RWTexture2D<float4> Output      : register(u4);
RWTexture2D<uint>   OffsetOut   : register(u5);
// OSR-private receipt: history allowed, valid/motion eye masks, cut, votes,
// history motion allowed. Never read back by the production frame thread.
RWTexture2D<uint>   SceneState  : register(u6);
Texture2D<float4> LowerMap : register(t10);
RWTexture2D<float4> LowerMapOut : register(u7);
// Resolve only: current motion is bound at t8, not the history's older field.
// Uniforms: current->history rows0..3, native previous same->history4..7,
// current->native previous same camera8..11; inverse-projection W columns12/13;
// row14 = current MV scale.xy, native previous dejitter.zw; row15 reserved.
// rows16..19: native-old source clip -> current peer clip, camera-local.
// This keeps the old inverse projection while transferring the stereo baseline.
Buffer<float4> CurrentHistoryData : register(t11);
Texture2D<float> NativePreviousDepth : register(t12);
SamplerState LinearClamp : register(s0);

cbuffer Osr : register(b0) {
    row_major float4x4 Forward;         // splat: image clip -> destination clip
    row_major float4x4 BackwardSource;  // resolve: destination clip -> source clip
    row_major float4x4 BackwardHistory; // resolve: destination clip -> history clip;
                                        // history splat: history clip -> clip of that eye's frame before it
    uint2 DepthSize;
    uint2 ColorSize;
    uint Flags;           // 1 debug tint, 2 history available
    uint MaxRun;          // colour pixels walked per side before a run counts as open
    float DepthTolerance; // relative, on reversed depth
    // History texels at or above this reversed depth are camera-attached
    // whatever the motion (kOsrFullLockedDistanceMetres); 2 = none.
    float LockedDepth;
    float2 MotionScale;   // history splat: composed motion value -> depth texels; zero = none
    // Depth texel (i, j) holds the scene at (i, j) + 0.5 + Dejitter: the DLSS
    // depth is rendered with a sub-texel jitter the colour no longer has, and
    // the depth window's origin is rounded to a texel. Measured on Cyberpunk
    // F8s of 2026-11-01: colour edge = depth edge - 2.1 px/texel of jitter
    // + 1.4 px, so without this every silhouette moved with the jitter.
    float2 SourceDejitter;  // for SourceDepth
    float2 HistoryDejitter; // for HistoryDepth
};

static const uint TagStereo = 1u;
static const uint TagHistory = 2u;
static const uint TagMoved = 3u; // history the source eye should have seen: a surface that moved
// Reversed depth puts the sky and the far plane at exactly 0, where the
// inverse of an infinite projection has no finite point. A tiny far depth keeps
// the maths finite (about 10^5 m with a 0.1 m near plane) and leaves a nonzero
// splat word, so the sky is a surface, not a hole.
static const float FarDepth = 1e-6;
// Content at or below this reversed depth has no parallax: the sky and what
// the game draws without depth (Cyberpunk's distant imposters, glow, haze).
static const float SkyDepth = 2e-6;
// A 3x3 texel neighbourhood whose nearest depth exceeds its farthest by this
// ratio holds a silhouette or thin geometry (kOsrFullEdgeRatio).
static const float EdgeRatio = 1.10;
// Depth texels around a previous-frame pixel searched for a nearer surface
// whose anti-aliased halo it may carry (kOsrFullHaloRadius).
static const int HaloRadius = 2;
// Relative movement, in history depth texels, between a halo's depth and the
// nearer surface it belongs to, past which the halo is stale
// (kOsrFullHaloParallax).
static const float HaloParallax = 0.5;
// Camera-attached texels: camera share above this many texels with screen
// motion below this fraction of it (kOsrFullLocked*).
static const float LockedCameraTexels = 1.0;
static const float LockedRatio = 0.5;
static const uint HistoryLockedBit = 1u; // in history splat words
// Mean colour difference (0..1) between the motion-compensated and the
// camera-only previous frame above which the other eye's warp arbitrates.
static const float ArbiterGap = 0.02;
// Colour pixels walked per direction by the background fill of the inpaint.
static const int FillReach = 48;
static const int2 FillDirections[8] = {
    int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
    int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };

groupshared uint SceneValid[64];
groupshared uint SceneChanged[64];
groupshared uint SceneSource[64];

// Only group zero participates. Reuse the clear dispatch, existing cameras,
// colour and depth; 576 samples independent of headset resolution.
void classify_scene(uint lane) {
    const uint eye = (Flags >> 4u) & 1u;
    const uint peer = 1u - eye;
    uint mask = (Flags & 8u) != 0 ? 0u : SceneState[uint2(1, 0)];
    const bool available = (Flags & 2u) != 0 && (mask & (1u << peer)) != 0;
    uint valid = 0, changed = 0, source_valid = 0;
    if (available) {
        [loop] for (uint i = lane; i < 576u; i += 64u) {
            const float2 uv = 0.05 + 0.9 * (float2(i % 32u, i / 32u) + 0.5) / float2(32, 18);
            const int2 t = clamp(int2(floor(uv * DepthSize - SourceDejitter)), 0, int2(DepthSize) - 1);
            const float z = SourceDepth.Load(int3(t, 0));
            if (!isfinite(z) || z < 0 || z > 1) continue;
            ++source_valid;
            const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            const float4 q = mul(mul(float4(ndc, max(z, FarDepth), 1), Forward), BackwardHistory);
            if (!all(isfinite(q)) || q.w <= 1e-8) continue;
            const float3 h = q.xyz / q.w;
            const float2 huv = float2(h.x * 0.5 + 0.5, 0.5 - h.y * 0.5);
            if (any(huv < 0.0) || any(huv >= 1.0)) continue;
            const int2 ht = clamp(int2(floor(huv * DepthSize - HistoryDejitter)), 0, int2(DepthSize) - 1);
            const float hz = HistoryDepth.Load(int3(ht, 0));
            const float3 a = max(SourceColor.SampleLevel(LinearClamp, uv, 0).rgb, 0);
            const float3 b = max(HistoryColor.SampleLevel(LinearClamp, huv, 0).rgb, 0);
            if (!isfinite(hz) || hz < 0 || hz > 1 || !all(isfinite(a)) || !all(isfinite(b))) continue;
            const float peak_a = max(a.r, max(a.g, a.b));
            const float peak_b = max(b.r, max(b.g, b.b));
            const float3 delta = abs(a - b);
            const float difference = max(delta.r, max(delta.g, delta.b)) / max(0.2, max(peak_a, peak_b));
            const float3 chroma_delta = abs(a / max(0.05, peak_a) - b / max(0.05, peak_b));
            const float chroma = max(chroma_delta.r, max(chroma_delta.g, chroma_delta.b));
            const bool depth_changed = abs(hz - max(h.z, 0.0)) > 0.25 * max(FarDepth, max(hz, h.z));
            ++valid;
            // Require a broad, strong change. Exposure-only flashes retain
            // both geometry and chromaticity; local motion cannot win the vote.
            if (difference > 0.45 && (depth_changed || chroma > 0.45)) ++changed;
        }
    }
    SceneValid[lane] = valid;
    SceneChanged[lane] = changed;
    SceneSource[lane] = source_valid;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            SceneValid[lane] += SceneValid[lane + stride];
            SceneChanged[lane] += SceneChanged[lane + stride];
            SceneSource[lane] += SceneSource[lane + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane == 0) {
        // A wholly different camera can have no overlap to compare. Discard
        // that history too; a normal partial head turn keeps its useful edge.
        const bool cut = (SceneValid[0] >= 288u && SceneChanged[0] * 4u >= SceneValid[0] * 3u) ||
            (SceneSource[0] >= 432u && SceneValid[0] <= 32u);
        SceneState[uint2(0, 0)] = available && !cut ? 1u : 0u;
        SceneState[uint2(2, 0)] = cut ? 1u : 0u;
        SceneState[uint2(3, 0)] = SceneValid[0] | (SceneChanged[0] << 16u);
        SceneState[uint2(4, 0)] = available && !cut && (mask & (4u << peer)) != 0 ? 1u : 0u;
        if (cut) mask = 0;
        // The current source is copied later in this same command list. Its
        // motion is usable only if its preceding same-eye frame survived.
        const bool previous_source_valid = (mask & (1u << eye)) != 0;
        mask &= ~(4u << eye);
        if (previous_source_valid) mask |= 4u << eye;
        SceneState[uint2(1, 0)] = mask | (1u << eye);
    }
}

[numthreads(8, 8, 1)]
void OsrFullClear(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    if (all(group == 0)) classify_scene(lane);
    if (any(id.xy >= DepthSize)) return;
    SplatOut[id.xy] = 0;
    SplatOut2[id.xy] = 0;
    OffsetOut[id.xy] = 0;
}

// Continuous texel positions (centres at .5) packed as 16.3 fixed point.
uint pack_position(float2 p) {
    uint2 f = uint2(clamp(p, 0.0, 8191.0) * 8.0 + 0.5);
    return f.x | (f.y << 16);
}
float2 unpack_position(uint v) { return float2(v & 0xffffu, v >> 16) / 8.0; }

// Where a history texel's surface is now. Its composed same-eye motion points
// to that eye's frame two publications back and includes the camera's share;
// the camera's share is what the texel's own depth projects to through the
// two cameras. What remains is the object's motion over two publications,
// which is extrapolated forward by one. Without motion data the texel stays.
//
// A texel whose screen motion is small against the camera's share is attached
// to the camera: the first-person weapon and hands. Strafing, the camera's
// share of a gun 30 cm away is 160-230 texels over two publications while its
// motion vectors say 3-29 (Cyberpunk F8s of 2026-11-01 13:50). Reprojecting
// it as world geometry throws it across the screen, and its composed motion
// is no measure of its own screen motion either: composition adds camera and
// stereo-interval terms evaluated at its depth, hundreds of texels at 30 cm,
// which do not cancel for an object moving with the camera (moved by half
// that residue it stayed unstable). It keeps its place in this
// eye's screen (locked = true, the caller skips the world reprojection).
//
// The camera's share only shows that when the camera moves across the texel.
// Walking forward it grows with the distance from the view centre, and the gun
// aimed at the centre has almost none; anything within LockedDepth (0.6 m) is
// the player's own weapon, hands or cockpit and is attached whatever the
// motion, with or without motion data. Outside that hard-near range, inferred
// screen locking still advances the measured residual screen motion; it only
// replaces the separate world-camera projection.
float2 history_texel_now(int2 t, float z, out bool locked) {
    float2 pos = float2(t) + 0.5 + HistoryDejitter;
    float2 now = pos; // single exit: fxc rejects early returns here
    locked = z >= LockedDepth;
    if (!locked && SceneState[uint2(4, 0)] != 0 && (MotionScale.x != 0.0 || MotionScale.y != 0.0)) {
        float2 total = HistoryMotion.Load(int3(t, 0)) * MotionScale;
        float2 ndc = float2(pos.x / DepthSize.x * 2.0 - 1.0, 1.0 - pos.y / DepthSize.y * 2.0);
        float4 q = mul(float4(ndc, z, 1.0), BackwardHistory);
        if (all(isfinite(total)) && all(abs(total) <= 4096.0) && all(isfinite(q)) && q.w > 1e-8) {
            float2 before = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x, (1.0 - q.y / q.w) * 0.5 * DepthSize.y);
            float2 camera = before - pos;
            float2 object = total - camera;
            const float camera_length = length(camera);
            locked = camera_length > LockedCameraTexels && length(total) < LockedRatio * camera_length;
            // Inferred screen lock cancels the camera's world reprojection,
            // not the surface's measured screen motion. A followed character
            // can still animate by several pixels between its real eye frames.
            // Hard-near weapons bypass this branch above: their composed field
            // can contain unreliable camera/stereo residue and stays ignored.
            if (locked) now = pos - 0.5 * total;
            else if (all(isfinite(object))) now = pos - 0.5 * object;
        }
    }
    return now;
}

// The history splat moves each texel to where its surface is now before
// projecting it into the destination; the second pass records, for every
// destination texel the nearest surface won, where that surface is in the
// history image, so the resolve can fetch its colour there.
bool history_landing(uint2 id, out float2 p, out uint word, out float2 origin) {
    p = 0; word = 0; origin = float2(id) + 0.5 + HistoryDejitter; // scene position in the history image
    bool ok = false;
    float z = SourceDepth.Load(int3(id, 0));
    if (isfinite(z) && z >= 0.0 && z <= 1.0) {
        z = max(z, FarDepth);
        bool locked;
        float2 now = history_texel_now(int2(id), z, locked);
        float2 ndc = float2(now.x / DepthSize.x * 2.0 - 1.0, 1.0 - now.y / DepthSize.y * 2.0);
        float4 q = mul(float4(ndc, z, 1.0), Forward);
        if (locked) {
            // Same eye, attached to the camera: the history image position is
            // the destination position, at the same depth. The lowest bit of
            // the depth word marks it for the resolve (far below depth
            // precision; non-attached words keep it clear).
            p = now - 0.5;
            word = asuint(z) | HistoryLockedBit;
            ok = all(isfinite(p));
            // Attached to a camera the head moves within (UEVR's game camera):
            // it lands where that camera puts it in the destination eye. In the
            // history splat BackwardSource holds that transform.
            if ((Flags & 64u) != 0) {
                const float4 l = mul(float4(ndc, z, 1.0), BackwardSource);
                const float2 at = float2((l.x / l.w + 1.0) * 0.5 * DepthSize.x - 0.5, (1.0 - l.y / l.w) * 0.5 * DepthSize.y - 0.5);
                const float zl = l.z / l.w;
                ok = l.w > 1e-8 && all(isfinite(at)) && isfinite(zl) && zl <= 1.0;
                if (ok) { p = at; word = asuint(max(zl, FarDepth)) | HistoryLockedBit; }
            }
        } else if (all(isfinite(q)) && q.w > 1e-8) {
            float2 landing = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x - 0.5, (1.0 - q.y / q.w) * 0.5 * DepthSize.y - 0.5);
            float zd = q.z / q.w;
            if (all(isfinite(landing)) && isfinite(zd) && zd <= 1.0) { // at or beyond the far plane is far (FarDepth below)
                p = landing;
                word = max(asuint(max(zd, FarDepth)), 2u) & ~HistoryLockedBit;
                ok = true;
            }
        }
    }
    return ok;
}

[numthreads(8, 8, 1)]
void OsrFullSplatHistory(uint3 id : SV_DispatchThreadID) {
    if (SceneState[uint2(0, 0)] == 0) return;
    if (any(id.xy >= DepthSize)) return;
    float2 p, origin; uint word;
    if (!history_landing(id.xy, p, word, origin)) return;
    int2 base = int2(floor(p));
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 t = base + int2(dx, dy);
            if (all(t >= 0) && all(t < int2(DepthSize)))
                InterlockedMax(SplatOut[t], word);
        }
}

[numthreads(8, 8, 1)]
void OsrFullSplatOffset(uint3 id : SV_DispatchThreadID) {
    if (SceneState[uint2(0, 0)] == 0) return;
    if (any(id.xy >= DepthSize)) return;
    float2 p, origin; uint word;
    if (!history_landing(id.xy, p, word, origin)) return;
    int2 base = int2(floor(p));
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 t = base + int2(dx, dy);
            if (all(t >= 0) && all(t < int2(DepthSize)) && SplatOut[t] == word) {
                // Equal-depth surfaces can reach the same texel from different
                // motion trajectories. Choose one origin deterministically;
                // an ordinary UAV store lets GPU scheduling pick a different
                // history sample on otherwise identical frames. Zero remains
                // the no-offset sentinel used by the clear and resolve.
                InterlockedMax(OffsetOut[t], pack_position(origin + (float2(t) - p)));
            }
        }
}

// Forward-splat every depth texel of an image into the destination eye. A
// destination texel keeps the nearest depth that lands on it. The 2x2
// footprint closes the cracks of mild magnification.
[numthreads(8, 8, 1)]
void OsrFullSplat(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= DepthSize)) return;
    float z = SourceDepth.Load(int3(id.xy, 0));
    if (!isfinite(z) || z < 0.0 || z > 1.0) return;
    z = max(z, FarDepth);
    float2 scene = float2(id.xy) + 0.5 + SourceDejitter;
    float2 ndc = float2(scene.x / DepthSize.x * 2.0 - 1.0, 1.0 - scene.y / DepthSize.y * 2.0);
    float4 q = mul(float4(ndc, z, 1.0), Forward);
    if (!all(isfinite(q)) || q.w <= 1e-8) return;
    float2 p = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x - 0.5,
                      (1.0 - q.y / q.w) * 0.5 * DepthSize.y - 0.5);
    if (!all(isfinite(p))) return;
    float zd = q.z / q.w;
    // Reversed depth: a slightly negative value is at or beyond a finite far
    // plane after reprojection; it is far, not invalid.
    if (!isfinite(zd) || zd > 1.0) return;
    uint word = max(asuint(max(zd, FarDepth)), 1u);
    int2 base = int2(floor(p));
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 t = base + int2(dx, dy);
            if (all(t >= 0) && all(t < int2(DepthSize)))
                InterlockedMax(SplatOut[t], word);
        }
}

// Colour at a fractional position, reconstructed with Catmull-Rom (9 bilinear
// taps) and clamped to the 2x2 nearest pixels so it cannot ring. Bilinear at a
// half-pixel offset averages two pixels and erases single-pixel detail: the
// generated eye kept 0.76 of the real frames' 1-pixel detail (mean |Laplacian|,
// Cyberpunk F8s of 2026-11-01 12:05/12:17) where AFW keeps 1.00, and in
// foliage the sharp/soft alternation between the eyes' frames is shimmer.
float4 sample_sharp(Texture2D<float4> tex, float2 uv) {
    const float2 size = float2(ColorSize);
    const float2 position = uv * size;
    const float2 centre1 = floor(position - 0.5) + 0.5;
    const float2 f = position - centre1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (centre1 - 1.0) / size;
    const float2 t3 = (centre1 + 2.0) / size;
    const float2 t12 = (centre1 + w2 / w12) / size;
    float4 result = 0;
    result += tex.SampleLevel(LinearClamp, float2(t0.x, t0.y), 0) * w0.x * w0.y;
    result += tex.SampleLevel(LinearClamp, float2(t12.x, t0.y), 0) * w12.x * w0.y;
    result += tex.SampleLevel(LinearClamp, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    result += tex.SampleLevel(LinearClamp, float2(t0.x, t12.y), 0) * w0.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(t12.x, t12.y), 0) * w12.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    result += tex.SampleLevel(LinearClamp, float2(t0.x, t3.y), 0) * w0.x * w3.y;
    result += tex.SampleLevel(LinearClamp, float2(t12.x, t3.y), 0) * w12.x * w3.y;
    result += tex.SampleLevel(LinearClamp, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    const int2 i0 = clamp(int2(centre1 - 0.5), int2(0, 0), int2(ColorSize) - 2);
    const float4 a = tex.Load(int3(i0, 0)), b = tex.Load(int3(i0 + int2(1, 0), 0));
    const float4 c = tex.Load(int3(i0 + int2(0, 1), 0)), d = tex.Load(int3(i0 + int2(1, 1), 0));
    return clamp(result, min(min(a, b), min(c, d)), max(max(a, b), max(c, d)));
}

// Colour of the pixel under uv, unfiltered. The history is this eye's own
// previous frame, so a pixel-exact copy of it keeps the real frames' detail.
// Each depth texel moves the history by its own motion vector, and on hair the
// vectors differ strand by strand, so any filter samples a different phase in
// every texel: Catmull-Rom kept only 0.68-0.89 of the real frames' 1-pixel
// detail on Joel's hair (The Last of Us, F8s of 2026-11-01 15:15-15:23) where
// AFW keeps 1.05, and the sharp/soft alternation between real and generated
// frames is the shimmer. The nearest pixel errs by half a pixel at most.
float4 sample_nearest(Texture2D<float4> tex, float2 uv) {
    const int2 i = clamp(int2(floor(uv * float2(ColorSize))), int2(0, 0), int2(ColorSize) - 1);
    return tex.Load(int3(i, 0));
}

// Does the image hold, around continuous texel position p, a surface at the
// expected reversed depth? The 2x2 texels under the bilinear footprint are
// tried so a depth gradient inside one texel does not reject its own surface.
bool surface_agrees(Texture2D<float> image, float2 p, float expected, float2 dejitter) {
    p -= dejitter; // scene position -> depth grid position
    int2 base = int2(floor(p - 0.5));
    bool agrees = false; // single exit: fxc's flow analysis rejects returns inside unrolled loops
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            int2 t = clamp(base + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1);
            float z = image.Load(int3(t, 0));
            // Two depths at sky level are the same surface. The splat clamps the
            // sky's 0 to FarDepth, and a finite far plane (Spider-Man) spreads
            // that by several percent when reprojected (0.92e-6..1.08e-6), past
            // the relative tolerance: half of the sky then disagreed with itself
            // and came out black (F8 of 2026-11-01 14:12:57, 9.5% of the eye).
            agrees = agrees || (isfinite(z) && z >= 0.0 &&
                ((z <= SkyDepth && expected <= SkyDepth) ||
                 abs(z - expected) <= DepthTolerance * max(z, expected) + FarDepth));
        }
    return agrees;
}

// Where the last successful fetch() read its image: scene position in depth
// texels and the expected depth. Per thread; read by resolve_history.
static float2 g_fetch_position = 0;
static float g_fetch_depth = 0;

// Project a destination pixel (ndc, splatted depth) into an image and fetch
// its colour when that image agrees it holds the surface.
bool fetch(Texture2D<float> depth, Texture2D<float4> color, float4x4 backward,
    float2 ndc, float z, float2 dejitter, bool nearest_pixel, out float4 result) {
    result = 0;
    bool found = false;
    float4 q = mul(float4(ndc, z, 1.0), backward);
    if (all(isfinite(q)) && q.w > 1e-8) {
        float2 p = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x,
                          (1.0 - q.y / q.w) * 0.5 * DepthSize.y); // continuous texel position, centres at .5
        float expected = max(q.z / q.w, 0.0); // at or beyond a finite far plane is far
        if (all(isfinite(p)) && all(p >= 0.0) && all(p < float2(DepthSize)) &&
            surface_agrees(depth, p, expected, dejitter)) {
            result = nearest_pixel ? sample_nearest(color, p / float2(DepthSize)) : sample_sharp(color, p / float2(DepthSize));
            g_fetch_position = p; g_fetch_depth = expected;
            found = true;
        }
    }
    return found;
}

// Resolve one destination pixel against one image. The pixel's own splatted
// depth is tried first. At a silhouette the 2x2 splat footprint dilates the
// near object by one texel: projecting such a halo pixel with the near depth
// lands beside the object in the image and disagrees. The background behind it
// sits in a neighbouring texel, so the farthest and then the nearest depth of
// the 3x3 neighbourhood are tried before the pixel counts as a hole. That keeps
// silhouettes at their true width and closes most cracks in thin geometry.
bool resolve_image(Texture2D<uint> splat, Texture2D<float> depth, Texture2D<float4> color,
    float4x4 backward, float2 dejitter, bool nearest_pixel, int2 c, float2 ndc, out float4 result, out uint word) {
    word = splat.Load(int3(c, 0));
    bool found = word != 0 && fetch(depth, color, backward, ndc, asfloat(word), dejitter, nearest_pixel, result);
    if (!found) {
        uint nearest = 0, farthest = 0xffffffffu;
        [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                uint v = splat.Load(int3(clamp(c + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0));
                nearest = max(nearest, v);
                farthest = v != 0 ? min(farthest, v) : farthest;
            }
        if (nearest != 0 && farthest != word && fetch(depth, color, backward, ndc, asfloat(farthest), dejitter, nearest_pixel, result)) {
            word = farthest; found = true;
        } else if (nearest != 0 && nearest != word && nearest != farthest &&
            fetch(depth, color, backward, ndc, asfloat(nearest), dejitter, nearest_pixel, result)) {
            word = nearest; found = true;
        }
    }
    return found;
}

// Halo search around depth texel t of an image: the nearest surface that is
// nearer than at_depth by EdgeRatio within HaloRadius (its depth in near) and
// the mean offset toward such texels, i.e. the direction across the edge.
float2 halo_direction(Texture2D<float> image, int2 t, float at_depth, out float near) {
    near = 0.0;
    float2 sum = 0;
    float count = 0;
    [unroll] for (int dy = -HaloRadius; dy <= HaloRadius; ++dy)
        [unroll] for (int dx = -HaloRadius; dx <= HaloRadius; ++dx) {
            const float z = image.Load(int3(clamp(t + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0));
            near = max(near, z);
            if (z > at_depth * EdgeRatio + FarDepth) { sum += float2(dx, dy); count += 1.0; }
        }
    return count > 0.0 ? sum / count : float2(0, 0);
}

// How far a halo moves ACROSS its edge, in depth texels: the destination ray
// projected into the image at the halo's depth and at the nearer surface's
// depth gives the parallax between them; only its component toward the
// nearer surface separates the halo from its edge. Moving along the edge (the
// top of a roof while strafing) keeps the halo on it: measuring the whole
// parallax dropped those halos and the row-wise inpaint drew a dashed line
// along every top edge (Spider-Man F8s of 2026-11-01 14:2x).
float halo_parallax(float4x4 backward, float2 ndc, float at_depth, float near, float2 toward) {
    const float4 qb = mul(float4(ndc, at_depth, 1.0), backward);
    const float4 qn = mul(float4(ndc, near, 1.0), backward);
    float parallax = 1e9;
    if (qb.w > 1e-8 && qn.w > 1e-8) {
        const float2 pb = float2(qb.x / qb.w, -qb.y / qb.w) * 0.5 * float2(DepthSize);
        const float2 pn = float2(qn.x / qn.w, -qn.y / qn.w) * 0.5 * float2(DepthSize);
        const float2 d = pn - pb;
        const float across = length(toward);
        parallax = across > 0.25 ? abs(dot(d, toward / across)) : length(d);
    }
    return parallax;
}

// Resolve a destination pixel against the history through the offsets the
// history splat recorded: the surface that won the texel is fetched where it
// is in the history image, so a texel moved by its own motion still finds its
// colour. Without an offset (no motion data, or the texel came from the 2x2
// footprint of a neighbour) the plain back-projection decides.
//
// A history pixel within one depth texel of a nearer surface in the history
// frame is that frame's anti-aliased silhouette: a blend of the object with
// what lies behind it, carrying the depth behind. When the object has moved
// relative to that background since (parallax while strafing), the blend is
// left where the edge used to be: a thin dashed copy of the edge beside the
// object, one frame's parallax away from it (Cyberpunk lamp post and sign,
// F8s of 2026-11-01 13:15, strafing left). The halo is searched over two
// depth texels (about 4 eye pixels): around a bright object the blend
// spreads past one texel through glow and the bright rim (F8s of 13:21).
// Whether the edge still stands where the halo is: the destination pixel's
// ray is projected into the history frame at the halo's own depth and at the
// nearer surface's depth. Standing still or turning, both land on the same
// spot and the old edge is the current edge. While strafing they separate by
// the parallax between the two depths; past half a texel the edge has moved
// relative to what lies behind it and the halo would be drawn beside it
// (a bollard, F8 of 13:32:41: 3-5 px, which a radius test kept).
//
// use_offsets = false gives the camera-only previous frame: the same surface
// without the object motion the history splat applied.
static float2 g_history_position = 0; // scene position (history texels) of the last resolve_history hit

// Does this frame's warp still hold a surface nearer than z within HaloRadius
// of destination texel c? The halo parallax test only sees the camera move an
// edge; an object that moves by itself (a running character's legs under a
// follow camera) leaves its previous-frame edge pixels where the edge used to
// be, with no parallax, and they were drawn as a thin outline of its old
// position (Spider-Man, F8s of 2026-11-02 09:41).
bool nearer_surface_present(int2 c, float z) {
    bool present = false;
    [unroll] for (int dy = -HaloRadius; dy <= HaloRadius; ++dy)
        [unroll] for (int dx = -HaloRadius; dx <= HaloRadius; ++dx) {
            const uint v = StereoDepth.Load(int3(clamp(c + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0));
            present = present || (v != 0 && asfloat(v) > z * EdgeRatio + FarDepth);
        }
    return present;
}

bool resolve_history(int2 c, float2 depth_pixel, float2 ndc, bool use_offsets, out float4 result, out uint word) {
    word = HistorySplat.Load(int3(c, 0));
    result = 0;
    bool found = false;
    float2 at_scene = 0;
    float at_depth = 0;
    uint packed = use_offsets ? HistoryOffset.Load(int3(c, 0)) : 0u;
    if (word != 0 && packed != 0) {
        float2 at = unpack_position(packed) + (depth_pixel - (float2(c) + 0.5));
        at = clamp(at, 0.0, float2(DepthSize) - 0.001);
        if (surface_agrees(HistoryDepth, at, asfloat(word), HistoryDejitter)) {
            result = sample_nearest(HistoryColor, at / float2(DepthSize));
            at_scene = at; at_depth = asfloat(word);
            found = true;
        }
    }
    if (!found) {
        found = resolve_image(HistorySplat, HistoryDepth, HistoryColor, BackwardHistory, HistoryDejitter, true, c, ndc, result, word);
        at_scene = g_fetch_position; at_depth = g_fetch_depth;
    }
    g_history_position = at_scene;
    if (found) {
        const int2 t = int2(floor(at_scene - HistoryDejitter));
        float history_near = 0.0;
        const float2 toward = halo_direction(HistoryDepth, t, at_depth, history_near);
        // The anti-aliased edge of a camera-attached surface (the weapon, the
        // arms) stays where it was in this eye's view: the surface keeps its
        // place, so its edge pixels are never stale. Both tests treat it as
        // world geometry: parallax fires whenever the camera moves, and the
        // warp, several pixels off that near, does not hold it where the
        // edge is. Dropped, the lit rim of the arm became a 2-3 px mirrored
        // line alternating with the real frames' rim, a band around the
        // weapon (Cyberpunk, F8 of 2026-11-02 10:06:52).
        if (history_near > at_depth * EdgeRatio + FarDepth && history_near < LockedDepth &&
            (halo_parallax(BackwardHistory, ndc, at_depth, history_near, toward) > HaloParallax ||
             !nearer_surface_present(c, at_depth)))
            found = false;
    }
    return found;
}

// Would the source eye have seen the surface at (ndc, z) in the destination
// as the nearest thing along its own ray? If the source holds something
// farther there, it would have seen this surface had it existed: a nearer
// surface in the previous frame is stale. If it holds the same or a nearer
// surface, this surface is one only the destination eye sees, such as the
// side of a first-person gun 30 cm from the face (Cyberpunk F8 of 2026-11-01
// 13:40:39: the right half of the pistol replaced by sky every other frame).
// Outside the source image counts as unseen.
bool source_sees_not_farther(float2 ndc, float z) {
    float4 q = mul(float4(ndc, z, 1.0), BackwardSource);
    bool not_farther = true;
    if (all(isfinite(q)) && q.w > 1e-8) {
        float2 p = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x, (1.0 - q.y / q.w) * 0.5 * DepthSize.y);
        float expected = max(q.z / q.w, 0.0); // at or beyond a finite far plane is far
        if (all(isfinite(p)) && all(p >= 0.0) && all(p < float2(DepthSize)) && isfinite(expected)) {
            float nearest = 0.0;
            int2 base = int2(floor(p - SourceDejitter - 0.5));
            [unroll] for (int dy = 0; dy < 2; ++dy)
                [unroll] for (int dx = 0; dx < 2; ++dx)
                    nearest = max(nearest, SourceDepth.Load(int3(clamp(base + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0)));
            not_farther = expected <= SkyDepth || nearest >= expected * (1.0 - DepthTolerance) - FarDepth;
        }
    }
    return not_farther;
}

// The source-side twin of the history halo rule: a warped pixel within the
// halo radius of a nearer surface in the source image is the source eye's
// anti-aliased silhouette carrying the depth behind. Along the destination
// ray its depth and the nearer surface's depth project apart by the stereo
// parallax, so it lands beside the object, not on its edge: a dashed line
// one disparity away, in the leading eye while strafing, once the previous
// frame's clean pixel there is dropped by its own halo rule (lamp post, F8 of
// 2026-11-01 13:50:23, strafing right, right eye). Such a pixel becomes part
// of the band and takes the mirrored background.
bool source_halo_stale(float2 at_scene, float at_depth, float2 ndc) {
    const int2 t = int2(floor(at_scene - SourceDejitter));
    float near = 0.0;
    const float2 toward = halo_direction(SourceDepth, t, at_depth, near);
    return near > at_depth * EdgeRatio + FarDepth &&
        halo_parallax(BackwardSource, ndc, at_depth, near, toward) > HaloParallax;
}

// Does the previous frame hold a surface at depth z within one texel of
// at_scene? Fine fringes (hair, foliage edges) have noisy depth: each eye and
// each frame covers different strands, so the exact texel often shows the
// background where the warp shows the strand. Taking the other eye there
// showed its strands every other frame: The Last of Us, Joel's hair against a
// bright wall, generated frame 34% further from its real neighbours than they
// are from each other at the fringe (F8 of 2026-11-01 14:45:38).
bool history_has_surface(float2 at_scene, float z) {
    const int2 t = int2(floor(at_scene - HistoryDejitter));
    bool found = false;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            const float zz = HistoryDepth.Load(int3(clamp(t + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0));
            found = found || (zz <= SkyDepth && z <= SkyDepth) ||
                abs(zz - z) <= DepthTolerance * max(zz, z) + FarDepth;
        }
    return found;
}

// Did this eye's previous frame look along the same ray? The destination ray
// projected into the previous frame at depth z and at infinity lands on one
// spot when the camera has not translated relative to the scene (standing
// still, or only turning the head): the previous frame then saw exactly what
// lies along this ray, and with object motion already compensated nothing can
// have made it stale. Judging staleness from the other eye's depth instead
// rejected correct pixels wherever that depth is noise (hair, thin edges): on
// The Last of Us captures of 2026-11-01 15:03:44 and 15:04:13, head still,
// the rejected pixels erred 30 and 23 levels against the next real frame and
// one in five was off by more than 40, while this eye's previous frame erred
// 3 and 4; the band fill put light background specks on Joel's hair there.
bool history_ray_unchanged(float2 ndc, float z) {
    const float4 qa = mul(float4(ndc, z, 1.0), BackwardHistory);
    const float4 qb = mul(float4(ndc, FarDepth, 1.0), BackwardHistory);
    bool unchanged = false;
    if (qa.w > 1e-8 && qb.w > 1e-8) {
        const float2 pa = float2(qa.x / qa.w, -qa.y / qa.w) * 0.5 * float2(DepthSize);
        const float2 pb = float2(qb.x / qb.w, -qb.y / qb.w) * 0.5 * float2(DepthSize);
        unchanged = length(pa - pb) <= HaloParallax;
    }
    return unchanged;
}

// Is the surface at (ndc, z) in the destination hidden from the source eye?
// The source's depth at its reprojected position is then nearer. That is what
// makes a history pixel legitimate band content: the stereo warp could not
// have it. A history surface the source eye sees at the same or a farther
// depth has moved since the previous frame (a swaying twig), or the stereo
// warp missed it; either way the previous frame must not paint it.
bool hidden_from_source(float2 ndc, float z) {
    float4 q = mul(float4(ndc, z, 1.0), BackwardSource);
    bool hidden = true; // outside the source image counts as unseen
    if (all(isfinite(q)) && q.w > 1e-8) {
        float2 p = float2((q.x / q.w + 1.0) * 0.5 * DepthSize.x, (1.0 - q.y / q.w) * 0.5 * DepthSize.y);
        float expected = max(q.z / q.w, 0.0); // at or beyond a finite far plane is far
        if (all(isfinite(p)) && all(p >= 0.0) && all(p < float2(DepthSize))) {
            float nearest = 0.0;
            int2 base = int2(floor(p - SourceDejitter - 0.5));
            [unroll] for (int dy = 0; dy < 2; ++dy)
                [unroll] for (int dx = 0; dx < 2; ++dx)
                    nearest = max(nearest, SourceDepth.Load(int3(clamp(base + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0)));
            hidden = nearest > expected * (1.0 + DepthTolerance) + FarDepth;
        }
    }
    return hidden;
}

static uint g_pick = 0;
static uint g_pick_word = 0;
static uint g_pick_tag = 0;
static float2 g_pick_at = 0;
// A CURRENT same-eye vector points two publications back. Reconstruct both
// endpoints in the peer history camera and interpolate their 3D positions at
// the intervening publication. The old extrapolation instead used the field
// from the previous peer frame, and the colour arbiter could cancel it locally.
float4 current_history_transform(float4 p, uint first) {
    return p.x * CurrentHistoryData[first] + p.y * CurrentHistoryData[first+1] +
           p.z * CurrentHistoryData[first+2] + p.w * CurrentHistoryData[first+3];
}
bool current_history(float2 source_at, float source_z, float2 target_at,
    out float4 color, out float2 previous_at, out bool screen_locked) {
    color = 0; previous_at = 0; screen_locked = false;
    bool found=false;
    [unroll] do {
    if ((Flags & 256u) == 0 || SceneState[uint2(4,0)] == 0 ||
        source_z <= SkyDepth || source_z >= LockedDepth) break;
    const int2 t = int2(floor(source_at - SourceDejitter));
    if (any(t < 0) || any(t >= int2(DepthSize))) break;
    const float2 total = HistoryMotion.Load(int3(t,0)) * CurrentHistoryData[14].xy;
    if (!all(isfinite(total)) || any(abs(total)>4096)) break;
    const float4 now = float4(source_at.x/DepthSize.x*2-1,1-source_at.y/DepthSize.y*2,source_z,1);
    const float4 camera_before = current_history_transform(now,8);
    if (!all(isfinite(camera_before)) || camera_before.w <= 1e-8) break;
    const float2 before_at = float2((camera_before.x/camera_before.w+1)*.5*DepthSize.x,
                                    (1-camera_before.y/camera_before.w)*.5*DepthSize.y);
    const float2 camera = before_at-source_at;
    // Screen attachment changes the coordinate model, NOT the age of the
    // motion field. Followed characters can animate while sharing the camera.
    const bool attached = length(camera)>LockedCameraTexels && length(total)<LockedRatio*length(camera);
    // A half native interval below half a colour pixel has no useful deformation.
    if (length((total-camera)*float2(ColorSize)/float2(DepthSize)) < 1.0) break;
    const float2 old_at = source_at + total;
    const int2 old_t = int2(floor(old_at-CurrentHistoryData[14].zw));
    if (any(old_t<0) || any(old_t>=int2(DepthSize))) break;
    const float old_z = NativePreviousDepth.Load(int3(old_t,0));
    if (!isfinite(old_z) || old_z<=SkyDepth || old_z>=LockedDepth) break;
    const float4 old = float4(old_at.x/DepthSize.x*2-1,1-old_at.y/DepthSize.y*2,old_z,1);
    const float nw = dot(now,CurrentHistoryData[12]), ow = dot(old,CurrentHistoryData[13]);
    if (!isfinite(nw) || !isfinite(ow) || nw<=1e-8 || ow<=1e-8) break;
    // Divide by homogeneous VIEW W before mixing: averaging NDC/reversed
    // depths would give the wrong depth for a surface moving toward the camera.
    float4 middle = .5*(current_history_transform(now,0)/nw + current_history_transform(old,4)/ow);
    if (!all(isfinite(middle)) || middle.w<=1e-8) break;
    previous_at=float2((middle.x/middle.w+1)*.5*DepthSize.x,(1-middle.y/middle.w)*.5*DepthSize.y);
    if (attached) {
        // Transport BOTH endpoints of the CURRENT vector to this eye. Their
        // individual depths give individual disparities; old local projection
        // is retained, so changing FOV is not treated as the same pixel grid.
        // The camera-local field keeps screen attachments in screen space.
        const float4 a = mul(now, Forward);
        const float4 b = current_history_transform(old,16);
        if (!all(isfinite(a)) || !all(isfinite(b)) || a.w<=1e-8 || b.w<=1e-8) break;
        const float2 pa=float2((a.x/a.w+1)*.5*DepthSize.x,(1-a.y/a.w)*.5*DepthSize.y);
        const float2 pb=float2((b.x/b.w+1)*.5*DepthSize.x,(1-b.y/b.w)*.5*DepthSize.y);
        previous_at=target_at+.5*(pb-pa);
        middle=.5*(a/nw+b/ow);
    }
    const float expected=middle.z/middle.w;
    if (any(previous_at<0) || any(previous_at>=float2(DepthSize)) || expected<=SkyDepth || expected>=LockedDepth) break;
    // The SAME existing3% tolerance, now evaluated at the intervening time.
    if (!surface_agrees(HistoryDepth,previous_at,expected,HistoryDejitter)) break;
    color=sample_nearest(HistoryColor,previous_at/float2(DepthSize));
    screen_locked=attached;
    found=true;
    } while(false);
    return found;
}

void resolve_pixel(uint3 id) {
    g_pick = 0;
    if (any(id.xy >= ColorSize)) return;
    float2 pixel = float2(id.xy) + 0.5;
    float2 ndc = float2(pixel.x / ColorSize.x * 2.0 - 1.0, 1.0 - pixel.y / ColorSize.y * 2.0);
    float2 depth_pixel = pixel * float2(DepthSize) / float2(ColorSize);
    int2 c = clamp(int2(floor(depth_pixel)), int2(0, 0), int2(DepthSize) - 1);
    float4 color = 0;
    uint word = 0;
    const bool history = (Flags & 2u) != 0 && SceneState[uint2(0, 0)] != 0;
    if (resolve_image(StereoDepth, SourceDepth, SourceColor, BackwardSource, SourceDejitter, false, c, ndc, color, word)) {
        const float2 stereo_at = g_fetch_position;
        const float stereo_depth = g_fetch_depth;
        // Where the warped source is weakest, this eye's own previous frame,
        // motion compensated, is the better picture of the same surface and
        // takes its place as a resolved pixel (not a band):
        // - Sky-depth content has no parallax, so the previous frame is exact
        //   for it, still or moving, and carries this eye's own glow and haze;
        //   the warped source leaves the source eye's glow beside a bright
        //   object, displaced by its disparity (Cyberpunk neon sign: 14 px of
        //   glow where the real eye has 2).
        // - At a silhouette or in thin geometry the depth grid is half the eye
        //   resolution, so the warped source places edges and blades at that
        //   granularity and they shimmer against the real eye; the previous
        //   frame is a real render of them (Cyberpunk bushes, 2026-11-01: the
        //   previous frame erred 6.6 against the next real frame where the
        //   warped source erred 9.7).
        // The previous frame must show the same surface: sky for sky, or a
        // depth within tolerance of the stereo depth; otherwise the warped
        // source stands.
        // This eye's own previous frame, motion compensated, is the primary
        // source wherever it shows the same surface; the warped other eye
        // fills the rest. Each eye's DLSS reconstructs sub-pixel detail with
        // its own jitter sequence, so the other eye's fine pattern differs
        // from this eye's: in dense foliage, showing it every other frame is
        // shimmer. Cyberpunk F8 of 2026-11-01 11:12, head still: this eye's
        // previous frame predicts the next real frame's fine detail with
        // correlation 0.95, the warped other eye 0.70; AFW's output matches
        // its previous frame exactly in fine detail. (Rounds 7-11 applied
        // this only at silhouettes, bands and sky.)
        if (history) {
            const float zs = asfloat(word & ~3u);
            const bool sky = zs <= SkyDepth;
            // A silhouette in the destination (depth contrast) or a band next
            // to this pixel (a texel nothing from the source reached). Either
            // way the source colour here is the source eye's anti-aliased
            // edge, a blend of the object with the background the source eye
            // saw beside it, and it lands 1-3 px from the band on both sides
            // as an outline. The previous frame holds this eye's own blend of
            // the same edge.
            uint nearest = 0, farthest = 0xffffffffu;
            bool hole = false;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    uint v = StereoDepth.Load(int3(clamp(c + int2(dx, dy), int2(0, 0), int2(DepthSize) - 1), 0));
                    nearest = max(nearest, v);
                    farthest = v != 0 ? min(farthest, v) : farthest;
                    hole = hole || v == 0;
                }
            const bool fringe = hole || (nearest != 0 && asfloat(nearest) > asfloat(farthest) * EdgeRatio + FarDepth);
            // Only surfaces visible in both eyes use the new pull. Bands,
            // fringes, HUD/near-camera surfaces and unavailable native history
            // retain their original treatment. No current-eye colour blend.
            float4 aligned = 0; float2 aligned_at = 0; bool aligned_locked=false;
            if (!fringe && current_history(stereo_at, stereo_depth, depth_pixel, aligned, aligned_at, aligned_locked)) {
                g_pick=2; g_pick_at=aligned_at;
                g_pick_word=(word&~3u)|(aligned_locked?HistoryLockedBit:0u); g_pick_tag=TagStereo;
                WarpedOut[id.xy]=aligned; ResolvedOut[id.xy]=(word&~3u)|TagStereo;
                return;
            }
            const bool prefer = (Flags & 4u) != 0 || sky || fringe;
            if (prefer) {
                float4 previous = 0; uint previous_word = 0;
                if (resolve_history(c, depth_pixel, ndc, true, previous, previous_word)) {
                    // The object motion applied to the previous frame comes
                    // from the composed motion vectors, which can be wrong for
                    // some materials: The Last of Us' menu curtains barely move
                    // between this eye's frames, yet their vectors claim about a
                    // stereo disparity, flipping sign with the eye (-41 then
                    // +32 px), and the moved curtain doubled against the other
                    // eye (F8 of 2026-11-01 14:28:11). Where the moved and the
                    // camera-only previous frame differ, the other eye's warp,
                    // which shows this very instant, decides which is right.
                    float2 picked_history_at = g_history_position;
                    float4 still = 0; uint still_word = 0;
                    if ((previous_word & HistoryLockedBit) == 0 && HistoryOffset.Load(int3(c, 0)) != 0 &&
                        resolve_history(c, depth_pixel, ndc, false, still, still_word)) {
                        const float3 third = float3(1.0, 1.0, 1.0) / 3.0;
                        const float gap = dot(abs(previous.rgb - still.rgb), third);
                        if (gap > ArbiterGap) {
                            const float moved_error = dot(abs(previous.rgb - color.rgb), third);
                            const float still_error = dot(abs(still.rgb - color.rgb), third);
                            // A colour tie cannot decide the object's temporal
                            // position. A binary choice made a one-level colour
                            // change jump a moving texture by its full offset.
                            // Clear winners retain their own-eye detail. Near
                            // a tie, fall back continuously to the current
                            // stereo surface, not a blend of two old positions.
                            const float confidence = saturate(abs(moved_error - still_error) / gap);
                            if (still_error < moved_error) {
                                previous = still; previous_word = still_word;
                                picked_history_at = g_history_position;
                            }
                            previous.rgb = lerp(color.rgb, previous.rgb, confidence);
                        }
                    }
                    // The second, camera-only lookup also updates the lookup
                    // scratch position. Validate the surface at the origin of
                    // the colour we actually chose, including when that lookup
                    // fails or the motion-compensated candidate wins.
                    const float2 previous_at = picked_history_at;
                    const float zh = asfloat(previous_word & ~3u);
                    const bool same = (sky ? zh <= SkyDepth : abs(zh - zs) <= DepthTolerance * max(zh, zs) + FarDepth) ||
                        history_has_surface(previous_at, zs);
                    // The previous frame shows a nearer surface than the warp:
                    // keep it when the source eye could not have seen it.
                    const bool nearer_unseen = zh > zs * (1.0 + DepthTolerance) + FarDepth &&
                        source_sees_not_farther(ndc, zh);
                    // A camera-attached surface keeps its place in this eye's
                    // previous frame, which knows exactly where it is in this
                    // eye's view. The warp places it through the stereo
                    // disparity, hundreds of pixels that near, so a small error
                    // in the eye separation moves it visibly: in Kingdom Come
                    // Deliverance II a crossbow 0.09 units away has about 1000
                    // px of disparity, the warp put it 26 px left of both real
                    // frames (2.6%, 1.6 mm of separation) and it shimmered
                    // (F8 of 2026-11-02 09:51:06). Where the warp shows such a
                    // surface the previous frame is taken; it also covers the
                    // strip of background the misplaced warp drew over.
                    const bool attached = (previous_word & HistoryLockedBit) != 0;
                    // Motion can keep a distant surface stationary on screen
                    // too: a third-person character followed by the camera.
                    // Its matching own-eye history is still valid. Reserve the
                    // special near-object rejection for actual near surfaces;
                    // otherwise the screen-lock heuristic makes moving clothes
                    // alternate between this eye's detail and the other eye's
                    // DLSS reconstruction on every AER publication.
                    const bool near_attached = attached && zh >= LockedDepth;
                    const bool warp_attached = (Flags & 128u) == 0 && zs >= LockedDepth;
                    // At a fringe, depth cannot tell a stale surface from a
                    // strand the other eye happens to miss: hair and foliage
                    // edges cover different strands in each eye and frame, and
                    // the other eye's depth there is as noisy as its colour.
                    // An edge that really moved has already been dropped by
                    // the halo rule inside resolve_history, so what survives is
                    // this eye's own fringe and is taken whatever the warp's
                    // depth says. Requiring depth agreement let the other eye's
                    // strands through in both directions: on Joel's hair (The
                    // Last of Us, F8 of 2026-11-01 14:45:38) 3% of fringe
                    // pixels flicked to the light background and 4.4% to a
                    // strand, the generated frame 34% further from its real
                    // neighbours than they are from each other.
                    if (warp_attached || (same && !near_attached) || nearer_unseen || (fringe && !attached)) {
                        g_pick=2;g_pick_at=picked_history_at;g_pick_word=previous_word;g_pick_tag=TagStereo;
                        WarpedOut[id.xy] = previous;
                        ResolvedOut[id.xy] = (previous_word & ~3u) | TagStereo;
                        return;
                    }
                }
            }
        }
        if (source_halo_stale(stereo_at, stereo_depth, ndc)) {
            WarpedOut[id.xy] = 0;
            ResolvedOut[id.xy] = 0; // a band pixel: the inpaint mirrors clean background
            return;
        }
        g_pick=1;g_pick_at=stereo_at;g_pick_word=word;g_pick_tag=TagStereo;
        WarpedOut[id.xy] = color;
        ResolvedOut[id.xy] = (word & ~3u) | TagStereo;
        return;
    }
    if (history && resolve_history(c, depth_pixel, ndc, true, color, word)) {
        // A camera-attached surface (the first-person weapon) keeps its screen
        // place; both tests below reproject it as world geometry, which it is
        // not. While strafing they failed on the weapon's own side faces that
        // only this eye sees, and the inpaint mirrored a second copy of the
        // weapon into them: a double image in parts of the weapon, orange
        // (moved) in debug.
        const bool legitimate = (word & HistoryLockedBit) != 0 ||
            hidden_from_source(ndc, asfloat(word & ~3u)) ||
            history_ray_unchanged(ndc, asfloat(word & ~3u));
        if(legitimate){g_pick=2;g_pick_at=g_history_position;g_pick_word=word;g_pick_tag=TagHistory;}
        WarpedOut[id.xy] = legitimate ? color : 0;
        ResolvedOut[id.xy] = (word & ~3u) | (legitimate ? TagHistory : TagMoved);
        return;
    }
    WarpedOut[id.xy] = 0;
    ResolvedOut[id.xy] = 0;
}

[numthreads(8, 8, 1)]
void OsrFullResolve(uint3 id : SV_DispatchThreadID) { resolve_pixel(id); }

// One geometric decision per2x2 block, followed by native-resolution colour
// gathers. This deliberately lowers silhouette/disocclusion decision precision;
// the original image is neither resized nor used as a low-resolution output.
float2 lower_project(float2 pixel,float z,bool history) {
    float2 ndc=float2(pixel.x/ColorSize.x*2.0-1.0,1.0-pixel.y/ColorSize.y*2.0);
    float4 p=history?mul(float4(ndc,z,1),BackwardHistory):mul(float4(ndc,z,1),BackwardSource);
    return float2((p.x/p.w+1)*.5*DepthSize.x,(1-p.y/p.w)*.5*DepthSize.y);
}
[numthreads(8,8,1)]
void OsrFullResolveMap(uint3 id:SV_DispatchThreadID) {
    uint2 base=id.xy*2;if(any(base>=ColorSize))return;
    resolve_pixel(uint3(base,0));
    uint metadata=g_pick+4*g_pick_tag;
    LowerMapOut[id.xy]=float4(g_pick_at,asfloat(g_pick_word),float(metadata));
}
[numthreads(8,8,1)]
void OsrFullResolveLower(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=ColorSize))return;
    float4 map=LowerMap.Load(int3(id.xy/2,0));
    uint metadata=uint(map.w);uint pick=metadata&3;uint word=asuint(map.z);
    float2 at=map.xy;
    uint2 base=(id.xy/2)*2;
    if(pick==0){WarpedOut[id.xy]=0;ResolvedOut[id.xy]=0;return;}
    if(pick==2 && (word&HistoryLockedBit)!=0)
        at+=float2(id.xy-base)*float2(DepthSize)/float2(ColorSize);
    else at+=lower_project(float2(id.xy)+.5,asfloat(word&~3u),pick==2)-lower_project(float2(base)+.5,asfloat(word&~3u),pick==2);
    if(!all(isfinite(at)) || any(at<0) || any(at>=float2(DepthSize))){WarpedOut[id.xy]=0;ResolvedOut[id.xy]=0;return;}

    [branch] if(pick==2)WarpedOut[id.xy]=sample_nearest(HistoryColor,at/float2(DepthSize));
    else WarpedOut[id.xy]=sample_sharp(SourceColor,at/float2(DepthSize));
    ResolvedOut[id.xy]=(word&~3u)|((metadata>>2)&3);
}

float4 tint(float4 color, float3 mark, float amount) {
    return float4(lerp(color.rgb, mark, amount), color.a);
}

// The resolved pixel at (x, y); zero weight when it is not resolved.
float4 resolved_pixel(int x, int y, out float weight) {
    const int2 s = clamp(int2(x, y), int2(0, 0), int2(ColorSize) - 1);
    const uint t = Resolved.Load(int3(s, 0)) & 3u;
    weight = (t == TagStereo || t == TagHistory) ? 1.0 : 0.0;
    return weight > 0.0 ? Warped.Load(int3(s, 0)) : 0;
}

// 3x3 box of the resolved pixels around (x, y); zero weight when none.
float4 resolved_box(int x, int y, out float weight) {
    float4 sum = 0; weight = 0;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) {
            int2 s = clamp(int2(x + i, y + j), int2(0, 0), int2(ColorSize) - 1);
            const uint t = Resolved.Load(int3(s, 0)) & 3u;
            if (t == TagStereo || t == TagHistory) { sum += Warped.Load(int3(s, 0)); weight += 1.0; }
        }
    return weight > 0.0 ? sum / weight : 0;
}

bool is_resolved(int x, int y) {
    const uint tag = Resolved.Load(int3(x, y, 0)) & 3u;
    return tag == TagStereo || tag == TagHistory;
}

// Fill a hole from the farthest surface around it, searched in eight
// directions. The row-wise mirror needs one end of the row to be background;
// where a post meets a rail both ends are the object, and the mirror pasted
// rail or post colour into the gap (The Last of Us stairs, F8 of 2026-11-01
// 14:31:28). Every direction that reaches the farthest surface contributes
// its first background pixel, weighted by inverse distance. (Mirroring the
// single nearest hit along its direction drew a slanted streak when the
// nearest background lay diagonally.)
float4 background_fill(int2 p, out float weight) {
    weight = 0;
    float depths[8];
    int steps[8];
    float far_depth = 1e9;
    [loop] for (int d = 0; d < 8; ++d) {
        steps[d] = 0; depths[d] = 1e9;
        [loop] for (int k = 1; k <= FillReach; ++k) {
            const int2 s = p + FillDirections[d] * k;
            if (any(s < 0) || any(s >= int2(ColorSize))) break;
            const uint r = Resolved.Load(int3(s, 0));
            const uint t = r & 3u;
            if (t == TagStereo || t == TagHistory) { steps[d] = k; depths[d] = asfloat(r & ~3u); break; }
        }
        if (steps[d] > 0) far_depth = min(far_depth, depths[d]);
    }
    float4 sum = 0;
    float total = 0;
    [loop] for (int e = 0; e < 8; ++e) {
        if (steps[e] > 0 && depths[e] <= far_depth * EdgeRatio + FarDepth) {
            const float w = 1.0 / (float(steps[e]) * length(float2(FillDirections[e])));
            sum += Warped.Load(int3(p + FillDirections[e] * steps[e], 0)) * w;
            total += w;
        }
    }
    float4 fill = 0;
    if (total > 0.0) { fill = sum / total; weight = 1.0; }
    return fill;
}

// Stereo and valid same-eye history pass through. Only holes and rejected
// history need invention. Bound the unresolved run at the nearest valid pixel,
// including history: crossing the green history band moved the mirror axis
// beyond that band and duplicated distant background details into purple holes.
[numthreads(8, 8, 1)]
void OsrFullInpaint(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= ColorSize)) return;
    const bool debug = (Flags & 1u) != 0;
    uint r = Resolved.Load(int3(id.xy, 0));
    float4 color = Warped.Load(int3(id.xy, 0));
    const uint tag = r & 3u;
    if (tag == TagStereo || tag == TagHistory) {
        Output[id.xy] = debug && tag == TagHistory ? tint(color, float3(0, 1, 0), 0.35) : color;
        return;
    }
    const int x = int(id.x), y = int(id.y), width = int(ColorSize.x);
    int first = x, last = x;
    [loop] while (first > 0 && x - first < int(MaxRun) && !is_resolved(first - 1, y)) --first;
    [loop] while (last < width - 1 && last - x < int(MaxRun) && !is_resolved(last + 1, y)) ++last;
    const int left = first - 1, right = last + 1;
    const bool have_left = left >= 0 && x - first < int(MaxRun);
    const bool have_right = right < width && last - x < int(MaxRun);
    const bool moved = tag == TagMoved;
    float4 fill = 0; float weight = 0;
    if (have_left && have_right) {
        const float zl = asfloat(Resolved.Load(int3(left, y, 0)) & ~3u);
        const float zr = asfloat(Resolved.Load(int3(right, y, 0)) & ~3u);
        const bool occluder_left = zl > zr;
        // The row tells which side is background only when its two ends differ.
        const bool clear = max(zl, zr) > min(zl, zr) * EdgeRatio + FarDepth;
        const int edge = occluder_left ? right : left;
        if (!clear) fill = background_fill(int2(x, y), weight);
        else {
            // Reflect about the shared edge, halfway between the last hole
            // texel and its first valid neighbour. The closest hole therefore
            // starts with that neighbour, not one texel farther away.
            int xs = 2 * edge - x + (occluder_left ? -1 : 1);
            xs = occluder_left ? max(xs, right) : min(xs, left);
            // The mirrored pixel itself, not a 3x3 average: a blurred strip in
            // an otherwise sharp image reads as a band. While strafing, the
            // leading eye's band is background no input has seen (the other
            // eye and this eye's previous frame are both behind it) and is
            // filled this way in every frame that eye is generated.
            // The mirror lands on background only if nothing nearer than the
            // background edge lies there. Where a lone background pixel ends
            // the row inside an object (wall seen through a gap in a rail),
            // the mirror lands back on the object and pastes its colour into
            // the band: light strokes beside the stair posts of The Last of
            // Us (F8 of 2026-11-01 15:03:26, rail colour 50 levels over the
            // dark wall) and light specks on Joel's hair (15:15:33). Those
            // pixels take the background fill: error against the next real
            // frame 6.9 -> 4.8, 15.0 -> 4.0 and 60.6 -> 16.0 levels.
            const int xm = clamp(xs, 0, width - 1);
            const float z_edge = occluder_left ? zr : zl;
            const uint rm = Resolved.Load(int3(xm, y, 0));
            const uint tm = rm & 3u;
            const bool nearer = (tm == TagStereo || tm == TagHistory) &&
                asfloat(rm & ~3u) > z_edge * EdgeRatio + FarDepth;
            if (!nearer) {
                fill = resolved_pixel(xm, y, weight);
                if (weight == 0.0) fill = resolved_box(xm, y, weight);
            }
            if (weight == 0.0) fill = background_fill(int2(x, y), weight);
            if (weight == 0.0) fill = resolved_box(edge, y, weight);
        }
    } else if (have_left || have_right) {
        fill = resolved_box(have_left ? left : right, y, weight);
    }
    if (weight == 0.0) fill = background_fill(int2(x, y), weight);
    if (weight == 0.0) { Output[id.xy] = debug ? float4(1, 0, 1, 1) : float4(0, 0, 0, 1); return; }
    if (debug) fill = moved ? tint(fill, float3(1, 0.5, 0), 0.5)
        : tint(fill, float3(1, 0, 1), 0.5);
    Output[id.xy] = fill;
}
