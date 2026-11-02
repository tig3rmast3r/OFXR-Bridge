Texture2D<float> SourceDepth : register(t0);
Texture2D<float2> CurrentMotion : register(t1);
Texture2D<float2> MiddleMotion : register(t2);
RWTexture2D<float> ConvertedDepth : register(u0);
// Measured projection-layer offset, observed native-depth domain, confidence.
RWTexture2D<float4> RemapState : register(u1);
cbuffer DepthDilation : register(b0)
{
    uint DepthInverted;
    uint DilateMode;
    uint SourceX;
    uint SourceY;
    uint2 CurrentMotionOrigin;
    uint2 MiddleMotionOrigin;
    float2 CurrentScale;
    float2 MiddleScale;
    float2 WindowExtent;
    uint StereoDepthValid;
    uint ResetCalibration;
    float4 CropDelta;
    float4 MiddleRow0, MiddleRow1, MiddleRow2, MiddleRow3;
    float4 PreviousRow0, PreviousRow1, PreviousRow2, PreviousRow3;
};

static const float DepthReprojectionTolerance = .0015;
static const float BiasCoherenceTolerance = .01;
static const uint CalibrationSamples = 1280;
static const uint CalibrationSupport = 32;
groupshared float4 Seeds[CalibrationSamples];
groupshared uint BiasHistogram[200];

float2 Project(float4 clip, bool middle)
{
    float4 q = middle
        ? clip.x * MiddleRow0 + clip.y * MiddleRow1 + clip.z * MiddleRow2 + clip.w * MiddleRow3
        : clip.x * PreviousRow0 + clip.y * PreviousRow1 + clip.z * PreviousRow2 + clip.w * PreviousRow3;
    return float2(q.x, -q.y) / q.w * .5 + .5;
}

float4 Clip(float2 uv, float depth)
{
    return float4(uv * float2(2, -2) + float2(-1, 1), depth, 1);
}

float2 CameraDifference(float2 uv, float depth)
{
    float4 clip = Clip(uv, depth);
    return 2 * Project(clip, true) - uv - Project(clip, false);
}

float NativeDepth(int2 pixel)
{
    return SourceDepth.Load(int3(pixel + int2(SourceX, SourceY), 0));
}

float2 NativeMotion(int2 pixel)
{
    return (CurrentMotion.Load(int3(pixel + CurrentMotionOrigin, 0)) * CurrentScale + CropDelta.xy) / WindowExtent;
}

bool NativeDepthConsistent(float2 uv, float depth, float2 current)
{
    float2 camera = Project(Clip(uv, depth), true) - uv;
    return all(isfinite(current)) && all(isfinite(camera)) &&
        length(current - camera) <= DepthReprojectionTolerance;
}

// Calibration requires the unchanged full TWO-dimensional three-camera fit
// AND a separate current native camera fit. Cancelled object velocity alone
// is not evidence of a projection-layer remap.
float4 CalibrationSeed(int2 pixel)
{
    float depth = NativeDepth(pixel);
    float repaired = depth;
    float error = 0;
    bool valid = false;
    float2 uv = (float2(pixel) + .5) / WindowExtent;
    float2 current = NativeMotion(pixel);
    if (isfinite(depth) && depth > 0 && depth < 1 &&
        all(isfinite(current)) && !NativeDepthConsistent(uv, depth, current)) {
    float2 priorPixel = (uv + current) * WindowExtent - .5;
    if (all(priorPixel >= 0) && all(priorPixel < WindowExtent - 1)) {
    int2 base = int2(floor(priorPixel));
    float2 weight = frac(priorPixel);
    float2 a = MiddleMotion.Load(int3(base + MiddleMotionOrigin, 0));
    float2 b = MiddleMotion.Load(int3(base + int2(1,0) + MiddleMotionOrigin, 0));
    float2 c = MiddleMotion.Load(int3(base + int2(0,1) + MiddleMotionOrigin, 0));
    float2 d = MiddleMotion.Load(int3(base + int2(1,1) + MiddleMotionOrigin, 0));
    float2 prior = (lerp(lerp(a,b,weight.x), lerp(c,d,weight.x), weight.y) * MiddleScale - CropDelta.xy) / WindowExtent;
    float2 observed = current - prior;
    if (all(isfinite(observed)) && length(observed - CameraDifference(uv, depth)) >= .02) {
    valid = true;
    [unroll] for (uint iteration = 0; iteration < 2; ++iteration) {
        float2 expected = CameraDifference(uv, repaired);
        float2 derivative = (CameraDifference(uv, repaired + .01) - expected) / .01;
        float denominator = dot(derivative, derivative);
        valid = valid && isfinite(denominator) && denominator >= 1e-6;
        repaired += dot(observed - expected, derivative) / max(denominator, 1e-6);
    }
    float fit = length(observed - CameraDifference(uv, repaired));
    float currentFit = length(current - (Project(Clip(uv, repaired), true) - uv));
    error = max(fit, currentFit);
    valid = valid && isfinite(repaired) && repaired > 0 && repaired < 1 &&
        isfinite(fit) && isfinite(currentFit) &&
        fit < DepthReprojectionTolerance && currentFit <= DepthReprojectionTolerance;
    }
    }
    }
    return float4(depth, depth - repaired, error, valid ? 1 : 0);
}

int2 SamplePixel(uint index)
{
    float2 coordinate;
    if (index < 1024) coordinate = (float2(index % 32, index / 32) + .5) / 32;
    else {
        uint edge = (index - 1024) / 64;
        float position = float((index - 1024) % 64) / 63;
        coordinate = edge == 0 ? float2(position, 0) :
            edge == 1 ? float2(position, 1) :
            edge == 2 ? float2(0, position) : float2(1, position);
    }
    return clamp(int2(coordinate * WindowExtent), int2(0,0), int2(WindowExtent) - 1);
}

[numthreads(256, 1, 1)]
void CalibrateLukeDepth(uint index : SV_GroupIndex)
{
    if (ResetCalibration != 0) {
        if (index == 0) { RemapState[int2(0,0)] = 0; RemapState[int2(0,1)] = 0; }
        DeviceMemoryBarrierWithGroupSync();
    }
    if (StereoDepthValid == 0) return;
    float4 saved = RemapState[int2(0,0)];
    bool calibrated = saved.w >= float(CalibrationSupport);
    if (index < 200) BiasHistogram[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (uint sample_index = index; sample_index < CalibrationSamples; sample_index += 256) {
        int2 pixel = SamplePixel(sample_index);
        float4 seed = 0;
        if (calibrated) {
            // Coverage grows only where the current native field validates
            // the already learned offset. Rotation cannot rewrite that offset.
            float depth = NativeDepth(pixel);
            float repaired = depth - saved.x;
            float2 uv = (float2(pixel) + .5) / WindowExtent;
            float2 current = NativeMotion(pixel);
            if (isfinite(depth) && repaired > 0 && repaired < 1 &&
                !NativeDepthConsistent(uv, depth, current) &&
                NativeDepthConsistent(uv, repaired, current)) seed = float4(depth, saved.x, 0, 1);
        } else seed = CalibrationSeed(pixel);
        Seeds[sample_index] = seed;
        if (!calibrated && seed.w != 0) {
            uint bin = uint(clamp(floor((seed.y + 1) / BiasCoherenceTolerance), 0, 199));
            InterlockedAdd(BiasHistogram[bin], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index != 0) return;
    if (calibrated) {
        float low = saved.y, high = saved.z;
        [loop] for (uint domain_index = 0; domain_index < CalibrationSamples; ++domain_index) {
            if (Seeds[domain_index].w != 0) { low = min(low, Seeds[domain_index].x); high = max(high, Seeds[domain_index].x); }
        }
        RemapState[int2(0,0)] = float4(saved.x, low, high, saved.w);
        return;
    }
    uint best = 0, peak = 0, total = 0;
    [loop] for (uint bin = 0; bin < 200; ++bin) {
        total += BiasHistogram[bin];
        if (BiasHistogram[bin] > peak) { peak = BiasHistogram[bin]; best = bin; }
    }
    float centre = -1 + (float(best) + .5) * BiasCoherenceTolerance;
    float sum = 0, low = 1, high = 0;
    uint support = 0;
    [loop] for (uint mean_index = 0; mean_index < CalibrationSamples; ++mean_index) {
        float4 seed = Seeds[mean_index];
        if (seed.w != 0 && abs(seed.y - centre) < BiasCoherenceTolerance) {
            sum += seed.y; low = min(low, seed.x); high = max(high, seed.x); ++support;
        }
    }
    if (support < CalibrationSupport || support * 4 < total * 3) return;
    float bias = sum / support, squaredError = 0;
    [loop] for (uint variance_index = 0; variance_index < CalibrationSamples; ++variance_index) {
        float4 seed = Seeds[variance_index];
        if (seed.w != 0 && abs(seed.y - centre) < BiasCoherenceTolerance)
            squaredError += (seed.y - bias) * (seed.y - bias);
    }
    float variance = squaredError / support;
    if (!isfinite(bias) || variance > BiasCoherenceTolerance * BiasCoherenceTolerance / 16) return;
    RemapState[int2(0,0)] = float4(bias, low, high, support);
    RemapState[int2(0,1)] = float4(variance, total, peak, 0);
}

float RemappedDepth(int2 pixel)
{
    float depth = NativeDepth(pixel);
    float4 calibration = RemapState[int2(0,0)];
    float result = depth;
    float corrected = depth - calibration.x;
    if (calibration.w >= float(CalibrationSupport) && isfinite(depth) &&
        depth >= calibration.y && depth <= calibration.z &&
        isfinite(corrected) && corrected > 0 && corrected < 1) {
        float2 uv = (float2(pixel) + .5) / WindowExtent;
        if (!NativeDepthConsistent(uv, depth, NativeMotion(pixel))) result = corrected;
    }
    return result;
}

// DilateMode's low byte is the dilation; bit 8 hands the provider the depth
// reversed (1 - z) with the matching projection, for a game that does not
// render reversed-Z itself (The Last of Us Part I).
[numthreads(8, 8, 1)]
void ConvertLukeDepth(uint3 thread_id : SV_DispatchThreadID)
{
    uint width, height;
    ConvertedDepth.GetDimensions(width, height);
    if (thread_id.x >= width || thread_id.y >= height) return;
    uint dilate = DilateMode & 0xFF;
    bool reverse = (DilateMode & 0x100) != 0;
    if (dilate == 0) {
        float value = RemappedDepth(int2(thread_id.xy));
        ConvertedDepth[thread_id.xy] = reverse ? 1 - value : value;
        return;
    }
    float nearest = DepthInverted != 0 ? 0 : 1;
    [unroll] for (int y = -1; y <= 1; ++y) {
        if (dilate == 1 && y != 0) continue;
        int sample_y = clamp(int(thread_id.y) + y, 0, int(height) - 1);
        [unroll] for (int x = -1; x <= 1; ++x) {
            int sample_x = clamp(int(thread_id.x) + x, 0, int(width) - 1);
            float candidate = RemappedDepth(int2(sample_x, sample_y));
            nearest = DepthInverted != 0 ? max(nearest, candidate) : min(nearest, candidate);
        }
    }
    ConvertedDepth[thread_id.xy] = reverse ? 1 - nearest : nearest;
}
