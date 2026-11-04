Texture2DArray<float4> PreviousFrame : register(t0);
Texture2DArray<float4> CurrentFrame : register(t1);
Texture2D<int2> BackwardFlow : register(t2);
Texture2D<uint> FlowAuxiliary : register(t3);
Texture2D<int2> ForwardFlow : register(t4);
Texture2D<uint> ForwardAuxiliary : register(t5);
Texture2DArray<float2> GameMotionVectors : register(t6);
Texture2DArray<float4> SceneOnlyFrame : register(t7);
RWTexture2D<float4> PackedColor : register(u0);
#ifdef XRFG_NVIDIA_LUMA_INPUT
RWTexture2D<float> NvidiaPreviousColor : register(u1);
RWTexture2D<float> NvidiaCurrentColor : register(u2);
#else
RWTexture2D<float4> NvidiaPreviousColor : register(u1);
RWTexture2D<float4> NvidiaCurrentColor : register(u2);
#endif

struct CameraMapping {
    float4 TargetToSourceRotation;
    float4 SourceTangents;
    float4 TargetTangents;
    float4 SourceRect;
    float4 TargetRect;
};

cbuffer SynthesisParameters : register(b0) {
    uint Width;
    uint Height;
    uint ArraySize;
    uint PackedWidth;
    uint PackedHeight;
    uint PackedEyeStride;
    uint FlowWidth;
    uint FlowHeight;
    uint FlowBlockSize;
    uint ViewIndex;
    uint UseGameMotion;
    uint GameMotionPadding;
    float4 GameMotionRect;
    float2 GameMotionScale;
    float2 GameMotionJitterDelta;
    CameraMapping PreviousMappings[2];
    uint Slice;
    // Bit 0 is the repeated-capture flag; bit 1 encodes sRGB flow input.
    // Bits 8-23 carry the synthetic's
    // position between the two captures in 1/65535ths - 0 at the previous
    // capture, 1 at the current. They share a slot because the root signature
    // is full: 62 constants plus two descriptor tables is the whole 64-DWORD
    // budget, leaving no room for a 63rd.
    uint SynthesisFlags;
};

uint repeated_capture_flag() {
    return SynthesisFlags & 1u;
}

// Djules 08bcfc40 / V416: sRGB source views return linear light. Preserve
// linear filtering and composition; encode only the optical-flow pack output.
float4 pack_flow_input_color(float4 color) {
    float4 packed = saturate(color);
    if ((SynthesisFlags & 2u) != 0u) {
        float3 low = packed.rgb * 12.92;
        float3 high = 1.055 * pow(max(packed.rgb, 0.0), 1.0 / 2.4) - 0.055;
        packed.rgb = packed.rgb <= 0.0031308 ? low : high;
    }
    return packed;
}

// An unset field decodes to 0.5, which is the fixed midpoint this shader
// used before the layer began deriving it.
float synthesis_fraction() {
    uint packed = (SynthesisFlags >> 8) & 0xffffu;
    return packed == 0u ? 0.5 : float(packed) / 65535.0;
}

float3 rotate_by_quaternion(float4 quaternion, float3 input_vector) {
    return input_vector + 2.0 * cross(
        quaternion.xyz,
        cross(quaternion.xyz, input_vector) + quaternion.w * input_vector);
}

struct MappedCoordinate {
    float2 coordinate;
    float valid;
    float padding;
};

MappedCoordinate map_target_to_source(
    float2 target_coordinate,
    CameraMapping mapping) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.padding = 0.0;
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent - 1.0;
    bool target_inside =
        target_coordinate.x >= target_minimum.x &&
        target_coordinate.y >= target_minimum.y &&
        target_coordinate.x <= target_maximum.x &&
        target_coordinate.y <= target_maximum.y;

    float2 target_uv =
        (target_coordinate - target_minimum + 0.5) / target_extent;
    float2 target_tangent = float2(
        lerp(mapping.TargetTangents.x, mapping.TargetTangents.y, target_uv.x),
        lerp(mapping.TargetTangents.z, mapping.TargetTangents.w, target_uv.y));
    float3 target_ray = float3(target_tangent, -1.0);
    float3 source_ray = rotate_by_quaternion(
        mapping.TargetToSourceRotation,
        target_ray);
    bool source_in_front = source_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-source_ray.z, 1.0e-5);
    float2 source_tangent = source_ray.xy * inverse_depth;
    float source_width = mapping.SourceTangents.y - mapping.SourceTangents.x;
    float source_height = mapping.SourceTangents.w - mapping.SourceTangents.z;
    bool valid_fov = source_width > 1.0e-7 && abs(source_height) > 1.0e-7;
    source_width = max(source_width, 1.0e-7);
    source_height = source_height < 0.0
                        ? min(source_height, -1.0e-7)
                        : max(source_height, 1.0e-7);
    float2 source_uv = float2(
        (source_tangent.x - mapping.SourceTangents.x) / source_width,
        (source_tangent.y - mapping.SourceTangents.z) / source_height);
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent;
    output.coordinate = source_minimum + source_uv * source_extent - 0.5;
    output.valid = target_inside && source_in_front && valid_fov &&
        output.coordinate.x >= source_minimum.x - 0.5 &&
        output.coordinate.y >= source_minimum.y - 0.5 &&
        output.coordinate.x <= source_maximum.x - 0.5 &&
        output.coordinate.y <= source_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

MappedCoordinate map_source_to_target(
    float2 source_coordinate,
    CameraMapping mapping) {
    MappedCoordinate output;
    output.coordinate = float2(0.0, 0.0);
    output.valid = 0.0;
    output.padding = 0.0;
    float2 source_minimum = mapping.SourceRect.xy;
    float2 source_extent = mapping.SourceRect.zw;
    float2 source_maximum = source_minimum + source_extent - 1.0;
    bool source_inside =
        source_coordinate.x >= source_minimum.x &&
        source_coordinate.y >= source_minimum.y &&
        source_coordinate.x <= source_maximum.x &&
        source_coordinate.y <= source_maximum.y;

    float2 source_uv =
        (source_coordinate - source_minimum + 0.5) / source_extent;
    float2 source_tangent = float2(
        lerp(mapping.SourceTangents.x, mapping.SourceTangents.y, source_uv.x),
        lerp(mapping.SourceTangents.z, mapping.SourceTangents.w, source_uv.y));
    float3 source_ray = float3(source_tangent, -1.0);
    float4 source_to_target_rotation = float4(
        -mapping.TargetToSourceRotation.xyz,
        mapping.TargetToSourceRotation.w);
    float3 target_ray = rotate_by_quaternion(
        source_to_target_rotation,
        source_ray);
    bool target_in_front = target_ray.z < -1.0e-5;
    float inverse_depth = 1.0 / max(-target_ray.z, 1.0e-5);
    float2 target_tangent = target_ray.xy * inverse_depth;
    float target_width = mapping.TargetTangents.y - mapping.TargetTangents.x;
    float target_height = mapping.TargetTangents.w - mapping.TargetTangents.z;
    bool valid_fov = target_width > 1.0e-7 && abs(target_height) > 1.0e-7;
    target_width = max(target_width, 1.0e-7);
    target_height = target_height < 0.0
                        ? min(target_height, -1.0e-7)
                        : max(target_height, 1.0e-7);
    float2 target_uv = float2(
        (target_tangent.x - mapping.TargetTangents.x) / target_width,
        (target_tangent.y - mapping.TargetTangents.z) / target_height);
    float2 target_minimum = mapping.TargetRect.xy;
    float2 target_extent = mapping.TargetRect.zw;
    float2 target_maximum = target_minimum + target_extent;
    output.coordinate = target_minimum + target_uv * target_extent - 0.5;
    output.valid = source_inside && target_in_front && valid_fov &&
        output.coordinate.x >= target_minimum.x - 0.5 &&
        output.coordinate.y >= target_minimum.y - 0.5 &&
        output.coordinate.x <= target_maximum.x - 0.5 &&
        output.coordinate.y <= target_maximum.y - 0.5
            ? 1.0
            : 0.0;
    return output;
}

float4 bilinear_previous_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(
        top_left + int2(1, 1),
        int2(int(Width) - 1, int(Height) - 1));
    float2 fraction = bounded - float2(top_left);
    float4 top = lerp(
        PreviousFrame.Load(int4(top_left, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right.x, top_left.y, int(slice), 0)),
        fraction.x);
    float4 bottom = lerp(
        PreviousFrame.Load(int4(top_left.x, bottom_right.y, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right, int(slice), 0)),
        fraction.x);
    return lerp(top, bottom, fraction.y);
}

float4 bilinear_previous_source_extent(
    float2 coordinate,
    uint slice,
    uint2 extent) {
    extent = max(extent, uint2(1U, 1U));
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(extent - 1U));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(
        top_left + int2(1, 1),
        int2(extent) - int2(1, 1));
    float2 fraction = bounded - float2(top_left);
    float4 top = lerp(
        PreviousFrame.Load(int4(top_left, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right.x, top_left.y, int(slice), 0)),
        fraction.x);
    float4 bottom = lerp(
        PreviousFrame.Load(int4(top_left.x, bottom_right.y, int(slice), 0)),
        PreviousFrame.Load(int4(bottom_right, int(slice), 0)),
        fraction.x);
    return lerp(top, bottom, fraction.y);
}

float4 bilinear_current_source(float2 coordinate, uint slice) {
    float2 bounded = clamp(
        coordinate,
        float2(0.0, 0.0),
        float2(float(Width - 1), float(Height - 1)));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(
        top_left + int2(1, 1),
        int2(int(Width) - 1, int(Height) - 1));
    float2 fraction = bounded - float2(top_left);
    float4 top = lerp(
        CurrentFrame.Load(int4(top_left, int(slice), 0)),
        CurrentFrame.Load(int4(bottom_right.x, top_left.y, int(slice), 0)),
        fraction.x);
    float4 bottom = lerp(
        CurrentFrame.Load(int4(top_left.x, bottom_right.y, int(slice), 0)),
        CurrentFrame.Load(int4(bottom_right, int(slice), 0)),
        fraction.x);
    return lerp(top, bottom, fraction.y);
}

// The ratio the flow input was packed at, relative to the source image.
// One when the input is full resolution. Derived from the width alone and
// applied to both axes: the scale is uniform, and the packed height carries
// the stacked eyes on the FidelityFX path, so a height ratio would not
// describe it.
float2 flow_input_scale() {
    float ratio = min(
        1.0,
        float(PackedWidth) / max(float(Width), 1.0));
    return float2(ratio, ratio);
}

float2 flow_input_coordinate(float2 full_resolution_coordinate) {
    return (full_resolution_coordinate + 0.5) *
        flow_input_scale() - 0.5;
}

struct CameraSample {
    float4 color;
    float valid;
    float3 padding;
};

CameraSample sample_previous_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    MappedCoordinate mapped = map_target_to_source(
        target_coordinate,
        PreviousMappings[view_index]);
    if (mapped.valid > 0.5) {
        output.color = bilinear_previous_source(mapped.coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

CameraSample sample_previous_target_extent(
    float2 target_coordinate,
    uint slice,
    uint view_index,
    uint2 source_extent) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    MappedCoordinate mapped = map_target_to_source(
        target_coordinate,
        PreviousMappings[view_index]);
    if (mapped.valid > 0.5) {
        output.color = bilinear_previous_source_extent(
            mapped.coordinate, slice, source_extent);
        output.valid = 1.0;
    }
    return output;
}

CameraSample sample_current_target(
    float2 target_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    float2 target_minimum = PreviousMappings[view_index].TargetRect.xy;
    float2 target_maximum = target_minimum +
        PreviousMappings[view_index].TargetRect.zw;
    bool valid = target_coordinate.x >= target_minimum.x - 0.5 &&
                 target_coordinate.y >= target_minimum.y - 0.5 &&
                 target_coordinate.x <= target_maximum.x - 0.5 &&
                 target_coordinate.y <= target_maximum.y - 0.5;
    if (valid) {
        output.color = bilinear_current_source(target_coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

// Bit 4: the synthetic is shown from a camera of its own, C - the head's pose
// at the instant it is shown, which the layer submits it with - rather than
// from B's. The runtime then has nothing to turn back, where B's pose left it
// turning the synthetic back by the head's motion over the periods between,
// with no pixels for the trailing edge. C has B's field of view and
// rectangle, so the turn from C into B is a pure rotation; it rides in the
// other view's mapping, which a view's composition never reads otherwise.
bool synthetic_camera() {
    return (SynthesisFlags & 16u) != 0u;
}

float4 multiply_quaternions(float4 left, float4 right) {
    return float4(
        left.w * right.xyz + right.w * left.xyz + cross(left.xyz, right.xyz),
        left.w * right.w - dot(left.xyz, right.xyz));
}

// C's mapping into B: B's projection on both sides, turned by the rotation.
CameraMapping synthetic_to_current_mapping() {
    CameraMapping own = PreviousMappings[ViewIndex];
    CameraMapping mapping = own;
    mapping.TargetToSourceRotation = PreviousMappings[1 - ViewIndex].TargetToSourceRotation;
    mapping.SourceTangents = own.TargetTangents;
    mapping.SourceRect = own.TargetRect;
    return mapping;
}

// What an output pixel shows. Without a synthetic camera, the synthesis at
// that pixel of B. With one, the synthesis at the point of B the pixel's ray
// meets - every composition below is evaluated at a continuous point already,
// so the turn costs no second resample. A ray that leaves B's view takes A's
// colour where A saw it: the far side of a turn, whose content B has already
// turned away from. Where neither saw it, B's nearest edge is evaluated, so
// no pixel is left without content.
struct OutputSource {
    float2 current;
    float4 previous;
    bool from_previous;
};

OutputSource output_source(float2 pixel) {
    OutputSource output;
    output.current = pixel;
    output.previous = float4(0.0, 0.0, 0.0, 1.0);
    output.from_previous = false;
    if (synthetic_camera()) {
        CameraMapping to_current = synthetic_to_current_mapping();
        MappedCoordinate current = map_target_to_source(pixel, to_current);
        float4 rect = PreviousMappings[ViewIndex].TargetRect;
        output.current = clamp(current.coordinate, rect.xy, rect.xy + rect.zw - 1.0);
        if (current.valid < 0.5) {
            // C into A is C into B, then B into A.
            CameraMapping to_previous = PreviousMappings[ViewIndex];
            to_previous.TargetToSourceRotation = multiply_quaternions(
                to_previous.TargetToSourceRotation, to_current.TargetToSourceRotation);
            MappedCoordinate previous = map_target_to_source(pixel, to_previous);
            if (previous.valid >= 0.5) {
                output.previous = bilinear_previous_source(previous.coordinate, Slice);
                output.from_previous = true;
            }
        }
    }
    return output;
}

// Where a point of B lies in the synthetic's camera, for the mesh warps,
// which move their grid in B and draw it in C.
float2 current_to_output(float2 coordinate) {
    return synthetic_camera()
        ? map_source_to_target(coordinate, synthetic_to_current_mapping()).coordinate
        : coordinate;
}

[numthreads(8, 8, 1)]
void PackFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = thread_id.y / PackedEyeStride;
    uint local_y = thread_id.y - slice * PackedEyeStride;
    // A reduced input is resampled rather than point-sampled, so the flow
    // sees a filtered image instead of every fourth pixel.
    float2 input_scale = flow_input_scale();
    if (slice >= ArraySize) {
        PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
        return;
    }
    if (PackedWidth < Width) {
        float2 source_coordinate =
            (float2(float(thread_id.x), float(local_y)) + 0.5) /
                max(input_scale, float2(1.0e-6, 1.0e-6)) - 0.5;
        if (source_coordinate.y < float(Height)) {
            current_color =
                bilinear_current_source(source_coordinate, slice);
#ifdef XRFG_HYBRID_QUARTER_INPUT
            // One bilinear tap filters 2x2 pixels; at a quarter, four of
            // them cover the 4x4 the packed pixel stands for.
            if (input_scale.x < 0.3) {
                current_color = 0.25 * (
                    bilinear_current_source(source_coordinate + float2(-1.0, -1.0), slice) +
                    bilinear_current_source(source_coordinate + float2(1.0, -1.0), slice) +
                    bilinear_current_source(source_coordinate + float2(-1.0, 1.0), slice) +
                    bilinear_current_source(source_coordinate + float2(1.0, 1.0), slice));
            }
#endif
        }
    } else if (thread_id.x < Width && local_y < Height) {
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.x, local_y), int(slice), 0));
    }
    PackedColor[thread_id.xy] = pack_flow_input_color(current_color);
}

// The view whose target rectangle holds this pixel, for the pack, which runs
// per slice rather than per view. An array swapchain gives each view its own
// slice with the same rectangle, so among the views that contain the pixel
// the one on this slice wins; a double-wide has disjoint rectangles on one
// slice. 2 when no view claims the pixel.
uint pack_view_for_pixel(float2 target_coordinate, uint slice) {
    uint view = 2u;
    [unroll] for (uint v = 0; v < 2; ++v) {
        float4 rect = PreviousMappings[v].TargetRect;
        float2 minimum = rect.xy - 0.5;
        float2 maximum = rect.xy + rect.zw - 0.5;
        bool inside = rect.z > 0.0 && rect.w > 0.0 &&
            target_coordinate.x >= minimum.x && target_coordinate.y >= minimum.y &&
            target_coordinate.x <= maximum.x && target_coordinate.y <= maximum.y;
        if (inside && (view == 2u || v == slice)) {
            view = v;
        }
    }
    return view;
}

[numthreads(8, 8, 1)]
void PackNvidiaFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 previous_color = float4(0.0, 0.0, 0.0, 1.0);
    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = Slice;
    bool downscaled = PackedWidth < Width || PackedHeight < Height;
    // The previous frame reaches the engine warped into the current camera
    // through the OpenXR pose delta, so what it estimates is the residual
    // scene motion rather than the head rotation with the scene motion on
    // top. Head turns no longer exceed its search, its cost no longer reads
    // "large displacement" everywhere under rotation, and the previous
    // pair's vectors (temporal hints) stay small and correlated. The
    // composition reads the previous endpoint through the same mapping, so
    // the two agree by construction. Where this camera sees beyond the
    // previous frame the input is black; the composition already treats
    // those pixels as uncovered.
    if ((SynthesisFlags & 4u) != 0u && slice < ArraySize) {
        float2 target_coordinate = downscaled
            ? (float2(thread_id.xy) + 0.5) / flow_input_scale() - 0.5
            : float2(thread_id.xy);
        if (target_coordinate.x < float(Width) &&
            target_coordinate.y < float(Height)) {
            uint view_index = pack_view_for_pixel(target_coordinate, slice);
            if (view_index < 2u) {
                CameraSample previous = sample_previous_target(
                    target_coordinate, slice, view_index);
                if (previous.valid >= 0.5) {
                    previous_color = previous.color;
                }
            } else {
                previous_color =
                    bilinear_previous_source(target_coordinate, slice);
            }
            current_color = downscaled
                ? bilinear_current_source(target_coordinate, slice)
                : CurrentFrame.Load(int4(int2(thread_id.xy), int(slice), 0));
        }
    } else if (slice < ArraySize && downscaled) {
        float2 source_coordinate =
            (float2(thread_id.xy) + 0.5) / flow_input_scale() - 0.5;
        previous_color = bilinear_previous_source(source_coordinate, slice);
        current_color = bilinear_current_source(source_coordinate, slice);
    } else if (thread_id.x < Width && slice < ArraySize &&
               thread_id.y < Height) {
        previous_color = PreviousFrame.Load(
            int4(int2(thread_id.xy), int(slice), 0));
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.xy), int(slice), 0));
    }
#ifdef XRFG_NVIDIA_LUMA_INPUT
    const float3 weights = float3(0.2126, 0.7152, 0.0722);
    NvidiaPreviousColor[thread_id.xy] = dot(pack_flow_input_color(previous_color).rgb, weights);
    NvidiaCurrentColor[thread_id.xy] = dot(pack_flow_input_color(current_color).rgb, weights);
#else
    NvidiaPreviousColor[thread_id.xy] = pack_flow_input_color(previous_color);
    NvidiaCurrentColor[thread_id.xy] = pack_flow_input_color(current_color);
#endif
}

float2 load_backward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(BackwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

float2 load_forward_flow_clamped(
    int2 coordinate,
    int2 minimum,
    int2 maximum) {
    return float2(ForwardFlow.Load(int3(clamp(coordinate, minimum, maximum), 0)));
}

struct FlowGridBounds {
    int2 minimum;
    int2 maximum;
};

FlowGridBounds flow_grid_bounds(
    float4 image_rect,
    uint slice) {
    uint2 image_minimum = uint2(image_rect.xy);
    uint2 image_maximum = image_minimum + uint2(image_rect.zw) - 1U;
    // Always applied: the scale is one when the input is full resolution,
    // so this is the identity on an unscaled path.
    {
        float2 input_scale = flow_input_scale();
        uint2 scaled_minimum = uint2(floor(
            float2(image_minimum) * input_scale));
        uint2 scaled_end = uint2(ceil(
            float2(image_maximum + 1U) * input_scale));
        image_minimum = scaled_minimum;
        image_maximum = max(scaled_minimum, scaled_end - 1U);
    }
    FlowGridBounds bounds;
    bounds.minimum = int2(
        int(image_minimum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_minimum.y) / FlowBlockSize));
    bounds.maximum = int2(
        int(image_maximum.x / FlowBlockSize),
        int((slice * PackedEyeStride + image_maximum.y) / FlowBlockSize));
    bounds.maximum = min(
        bounds.maximum,
        int2(int(FlowWidth) - 1, int(FlowHeight) - 1));
    return bounds;
}

float2 flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    bool use_nvidia_input_scale) {
    // The stride is zero where the backend packs eyes separately, so this
    // is the scaled coordinate on that path and the scaled coordinate plus
    // the eye offset on the stacked one.
    float2 scaled = flow_input_coordinate(pixel);
    float2 packed_coordinate = float2(
        scaled.x,
        scaled.y + float(slice * PackedEyeStride));
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_backward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_backward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_backward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale;
    // Flow measured on a reduced input has proportionally smaller vectors.
    result /= max(flow_input_scale(), float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float2 forward_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale,
    // True where the previous input was warped into the current camera in
    // the pack: its pixels then lie in the target rectangle, not the source.
    bool warped_previous) {
    float2 input_scale = flow_input_scale();
    float2 packed_coordinate = flow_input_coordinate(pixel);
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        warped_previous
            ? PreviousMappings[view_index].TargetRect
            : PreviousMappings[view_index].SourceRect,
        slice);
    float2 bounded = clamp(
        flow_coordinate,
        float2(bounds.minimum),
        float2(bounds.maximum));
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), bounds.maximum);
    float2 fraction = bounded - float2(top_left);
    float2 top = lerp(
        load_forward_flow_clamped(
            top_left,
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            int2(bottom_right.x, top_left.y),
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 bottom = lerp(
        load_forward_flow_clamped(
            int2(top_left.x, bottom_right.y),
            bounds.minimum,
            bounds.maximum),
        load_forward_flow_clamped(
            bottom_right,
            bounds.minimum,
            bounds.maximum),
        fraction.x);
    float2 result = lerp(top, bottom, fraction.y) * value_scale /
        max(input_scale, float2(1.0e-6, 1.0e-6));
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

// The hybrid composition runs both the game-motion and the optical-flow
// branches, so the flow keeps its constants and the DLSS output rectangle
// travels 16 bits a value in UseGameMotion (x, y) and GameMotionPadding
// (width, height).
static bool hybrid_layout = false;
// How far the last synthesize_midpoint's two samples disagreed: what the
// hybrid composition chooses by.
static float last_disagreement = 1.0;

// The DLSS output rectangle associated with this eye stream. In the
// game-motion branch the otherwise-unused optical-flow constants carry it.
float4 game_motion_output_rect() {
    if (hybrid_layout) {
        return float4(float(UseGameMotion & 0xffffU), float(UseGameMotion >> 16),
                      float(GameMotionPadding & 0xffffU), float(GameMotionPadding >> 16));
    }
    return float4(float(FlowWidth), float(FlowHeight), float(FlowBlockSize),
                  float(GameMotionPadding));
}

float2 game_motion_for_pixel(float2 pixel, uint slice, uint view_index) {
    float4 output_rect = game_motion_output_rect();
    float2 uv = (pixel - output_rect.xy + 0.5) /
        max(output_rect.zw, float2(1.0, 1.0));
    float2 coordinate = GameMotionRect.xy + uv * GameMotionRect.zw - 0.5;
    float2 minimum = GameMotionRect.xy;
    float2 maximum = GameMotionRect.xy + GameMotionRect.zw - 1.0;
    float2 bounded = clamp(coordinate, minimum, maximum);
    int2 top_left = int2(floor(bounded));
    int2 bottom_right = min(top_left + int2(1, 1), int2(maximum));
    float2 fraction = bounded - float2(top_left);
    // Each descriptor block owns one eye's independent mono DLSS resource.
    // Rotation produces a spatially varying field, so nearest-neighbour lookup
    // creates an avoidable endpoint error that uniform translation conceals.
    float2 top = lerp(
        GameMotionVectors.Load(int4(top_left, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right.x, top_left.y, 0, 0)),
        fraction.x);
    float2 bottom = lerp(
        GameMotionVectors.Load(int4(top_left.x, bottom_right.y, 0, 0)),
        GameMotionVectors.Load(int4(bottom_right, 0, 0)),
        fraction.x);
    float2 raw = lerp(top, bottom, fraction.y);
    float2 result = raw * GameMotionScale + GameMotionJitterDelta;
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

// The game's vector at the guide texel under pixel, unfiltered: enough to tell
// surfaces apart.
float2 game_motion_texel(float2 pixel) {
    float4 output_rect = game_motion_output_rect();
    float2 uv = (pixel - output_rect.xy + 0.5) /
        max(output_rect.zw, float2(1.0, 1.0));
    float2 maximum = GameMotionRect.xy + GameMotionRect.zw - 1.0;
    int2 texel = int2(clamp(GameMotionRect.xy + uv * GameMotionRect.zw,
        GameMotionRect.xy, maximum));
    return GameMotionVectors.Load(int4(texel, 0, 0)) * GameMotionScale;
}

bool coordinate_inside_rect(float2 coordinate, float4 rect) {
    float2 minimum = rect.xy - 0.5;
    float2 maximum = rect.xy + rect.zw - 0.5;
    return coordinate.x >= minimum.x && coordinate.y >= minimum.y &&
        coordinate.x <= maximum.x && coordinate.y <= maximum.y;
}

float rgb_error(float4 a, float4 b) {
    float3 difference = abs(a.rgb - b.rgb);
    return max(difference.r, max(difference.g, difference.b));
}

// Preserve a stationary neighbourhood after the existing XR camera mapping.
// Testing only the centre is insufficient (a moving edge can cross flat pixels).
bool fast_stationary_patch(float2 pixel, uint slice, float4 current) {
    CameraSample previous = sample_previous_target(pixel, slice, ViewIndex);
    static const float2 offsets[4] = {
        float2(-2, 0), float2(2, 0), float2(0, -2), float2(0, 2)
    };
    bool stationary = previous.valid >= 0.5 &&
        rgb_error(previous.color, current) <= 1.0 / 255.0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        CameraSample a = sample_previous_target(pixel + offsets[i], slice, ViewIndex);
        CameraSample b = sample_current_target(pixel + offsets[i], slice, ViewIndex);
        if (a.valid < 0.5 || b.valid < 0.5 || rgb_error(a.color, b.color) > 1.0 / 255.0) {
            stationary = false;
        }
    }
    return stationary;
}

struct FullscreenVertex {
    float4 position : SV_Position;
};

FullscreenVertex FullscreenTriangleVS(uint vertex_id : SV_VertexID) {
    FullscreenVertex output;
    float2 position = vertex_id == 0 ? float2(-1.0, -1.0)
                     : vertex_id == 1 ? float2(-1.0, 3.0)
                                      : float2(3.0, -1.0);
    output.position = float4(position, 0.0, 1.0);
    return output;
}

// Content that stayed where it was on screen although the game's vector there
// says it moved: a HUD drawn after DLSS has no vectors of its own. It needs
// both: unchanged at the pixel and its neighbours, and not what the vector
// predicts. Correct vectors explain a moving surface however flat or striped
// it is, so only content they do not describe is kept.
bool game_motion_static_overlay(float2 pixel, float4 current) {
    // The nearest texel: under a synthetic camera the pixel is a point of B
    // between texels.
    int2 texel = int2(pixel + 0.5);
    bool overlay = rgb_error(PreviousFrame.Load(int4(texel, int(Slice), 0)), current) <= 1.0 / 255.0;
    MappedCoordinate moved = (MappedCoordinate)0;
    if (overlay) {
        // Content its vector holds still is explained without a sample, and in
        // a still scene that is nearly every pixel.
        moved = map_source_to_target(pixel + game_motion_texel(pixel), PreviousMappings[ViewIndex]);
        overlay = moved.valid >= 0.5 && length(moved.coordinate - pixel) >= 0.5;
    }
    // Unchanged across the patch next: what moves fails that cheaply.
    int2 offsets[4] = {int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2)};
    float4 around[4] = {current, current, current, current};
    bool flat = true;
    if (overlay) {
        int2 limit = int2(int(Width) - 1, int(Height) - 1);
        [unroll] for (uint i = 0; i < 4; ++i) {
            int2 neighbour = clamp(texel + offsets[i], int2(0, 0), limit);
            around[i] = CurrentFrame.Load(int4(neighbour, int(Slice), 0));
            overlay = overlay && rgb_error(PreviousFrame.Load(int4(neighbour, int(Slice), 0)),
                around[i]) <= 1.0 / 255.0;
            flat = flat && rgb_error(around[i], current) <= 1.0 / 255.0;
        }
    }
    if (overlay) {
        // Somewhere in it not what the vector predicts. A repeating pattern
        // can match its own shift at one pixel; a flat patch cannot tell, and
        // its centre has already been asked.
        float unexplained = rgb_error(
            sample_previous_target(moved.coordinate, Slice, ViewIndex).color, current);
        if (unexplained <= 0.1 && !flat) {
            [unroll] for (uint i = 0; i < 4; ++i) {
                unexplained = max(unexplained, rgb_error(sample_previous_target(
                    moved.coordinate + float2(offsets[i]), Slice, ViewIndex).color, around[i]));
            }
        }
        overlay = unexplained > 0.1;
    }
    return overlay;
}

// The game-motion solve below, from start rather than the pixel, for a second
// surface: true, with its A and B samples, if two steps reach its fixed point.
bool game_motion_surface(float2 pixel, float2 start, out float4 previous_color,
    out float4 current_color) {
    previous_color = float4(0.0, 0.0, 0.0, 1.0);
    current_color = previous_color;
    float2 endpoint = start;
    float2 previous_coordinate = start;
    bool found = true;
    [unroll] for (uint step = 0; step < 3; ++step) {
        float2 backward = game_motion_for_pixel(endpoint, Slice, ViewIndex);
        MappedCoordinate mapped = map_source_to_target(endpoint + backward, PreviousMappings[ViewIndex]);
        found = found && mapped.valid >= 0.5;
        previous_coordinate = mapped.coordinate;
        float2 next = pixel - (1.0 - synthesis_fraction()) * (mapped.coordinate - endpoint);
        if (step == 2) {
            found = found && length(next - endpoint) < 0.5;
        } else {
            endpoint = next;
        }
    }
    if (found) {
        CameraSample a = sample_previous_target(previous_coordinate, Slice, ViewIndex);
        CameraSample b = sample_current_target(endpoint, Slice, ViewIndex);
        previous_color = a.color;
        current_color = b.color;
        found = a.valid >= 0.5 && b.valid >= 0.5;
    }
    return found;
}

// djules75 V510: compare colour AND opacity, trying still motion and the
// motion of nearby panel content at disagreeing transparent edges. This
// implementation uses this branch's flow_for_pixel/sample_* entry points.
float panel_error(CameraSample a,CameraSample b) {
    return a.valid>=.5 && b.valid>=.5 ? max(rgb_error(a.color,b.color),abs(a.color.a-b.color.a)) : 1.e6;
}
void consider_panel_motion(float2 pixel,float2 motion,float t,inout float error,inout float4 best) {
    CameraSample a=sample_previous_target(pixel+motion*t,Slice,ViewIndex);
    CameraSample b=sample_current_target(pixel-motion*(1-t),Slice,ViewIndex);
    float e=panel_error(a,b);
    if(e+1./255.<error){error=e;best=lerp(a.color,b.color,t);}
}
float4 synthesize_panel(float2 pixel,float value_scale,bool nvidia) {
    const float t=synthesis_fraction();
    float2 motion=repeated_capture_flag()!=0?float2(0,0):flow_for_pixel(pixel,Slice,ViewIndex,value_scale,nvidia);
    CameraSample a=sample_previous_target(pixel+motion*t,Slice,ViewIndex);
    CameraSample b=sample_current_target(pixel-motion*(1-t),Slice,ViewIndex);
    float error=panel_error(a,b);
    float4 best=lerp(a.color,b.color,t);
    if(repeated_capture_flag()==0 && error>2./255.) {
        CameraSample still_a=sample_previous_target(pixel,Slice,ViewIndex);
        CameraSample still_b=sample_current_target(pixel,Slice,ViewIndex);
        float still_error=panel_error(still_a,still_b);
        if(still_error+1./255.<error){error=still_error;best=lerp(still_a.color,still_b.color,t);}
        // Raster UI edges often move in whole texels. Keep the fractional
        // estimate unless the neighbouring integer estimate actually agrees
        // better in both colour and alpha; never quantize all panel motion.
        consider_panel_motion(pixel,round(motion),t,error,best);
        static const float2 directions[8]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1),
            float2(.7071,.7071),float2(-.7071,.7071),float2(.7071,-.7071),float2(-.7071,-.7071)};
        static const float reach[4]={6,12,24,40};
        float2 tried[3]={motion,float2(0,0),float2(1.e6,1.e6)};
        for(uint ring=0;ring<4&&error>2./255.;++ring)for(uint direction=0;direction<8;++direction) {
            float2 candidate=flow_for_pixel(pixel+directions[direction]*reach[ring],Slice,ViewIndex,value_scale,nvidia);
            if(length(candidate)<.5 || length(candidate-tried[0])<.5 ||
                length(candidate-tried[1])<.5 || length(candidate-tried[2])<.5)continue;
            tried[2]=tried[1];tried[1]=candidate;
            consider_panel_motion(pixel,candidate,t,error,best);
            if(error>2./255. && length(candidate-round(candidate))>.05)
                consider_panel_motion(pixel,round(candidate),t,error,best);
        }
    }
    last_disagreement=error;return saturate(best);
}

float4 synthesize_midpoint(
    FullscreenVertex input,
    float flow_value_scale,
    bool use_nvidia_cost,
    bool use_nvidia_bidirectional,
    bool validate_fast,
    bool use_game_motion_pipeline,
    // The NVIDIA pack warps the previous frame into the current camera, so
    // its flow is the residual already and every previous endpoint is a
    // target coordinate. FidelityFX keeps its own unwarped history, so its
    // flow still carries the pose term and it is subtracted here.
    bool input_pose_compensated) {
    float4 output_color = float4(0.0, 0.0, 0.0, 1.0);
    last_disagreement = 1.0;
    uint2 integer_pixel = uint2(input.position.xy);
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height &&
        Slice < ArraySize;
    OutputSource source = output_source(float2(integer_pixel));
    if (in_bounds && source.from_previous) {
        output_color = saturate(source.previous);
        last_disagreement=0.0;

    } else if (in_bounds) {
        float2 pixel = source.current;
        CameraSample current_fallback = sample_current_target(
            pixel,
            Slice,
            ViewIndex);
        output_color = saturate(current_fallback.color);
        MappedCoordinate previous_coverage = map_target_to_source(
            pixel,
            PreviousMappings[ViewIndex]);
        bool scene_changed = repeated_capture_flag() == 0 && !use_game_motion_pipeline &&
            !use_nvidia_cost &&
            (FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0;
        if (previous_coverage.valid >= 0.5 && !scene_changed) {
            bool preserve_stationary = validate_fast && repeated_capture_flag() == 0 &&
                current_fallback.valid >= 0.5 &&
                fast_stationary_patch(pixel, Slice, current_fallback.color);
            bool static_overlay = !preserve_stationary && use_game_motion_pipeline &&
                repeated_capture_flag() == 0 && current_fallback.valid >= 0.5 &&
                game_motion_static_overlay(pixel, current_fallback.color);
            if (preserve_stationary) {
                output_color = saturate(lerp(
                    sample_previous_target(pixel, Slice, ViewIndex).color,
                    current_fallback.color,
                    synthesis_fraction()));
            } else if (static_overlay) {
                output_color = saturate(current_fallback.color);
                last_disagreement = 0.0;
            } else {
                float2 raw_backward = float2(0.0, 0.0);
                CameraSample previous_sample = (CameraSample)0;
                CameraSample current_sample = (CameraSample)0;
                bool endpoints_valid = true;
                // How far the solve below still was from a fixed point, and
                // its final B-to-A displacement.
                float solve_residual = 0.0;
                float2 last_displacement = float2(0.0, 0.0);
                // Where the A and B samples were taken.
                float2 a_coordinate = pixel, b_coordinate = pixel;
                if (use_game_motion_pipeline) {
                    // A B-to-A vector is attached to its B endpoint. Sampling
                    // it once at the desired midpoint is exact only for a
                    // constant translation field. Yaw is spatially varying:
                    // solve cB = midpoint - 0.5 * (cA(cB) - cB), while mapping
                    // cA through the previous OpenXR camera exactly.
                    float2 current_endpoint = pixel;
                    MappedCoordinate previous_endpoint;
                    // Keep the step count odd: see the covered-background
                    // case below.
                    [unroll] for (uint iteration = 0; iteration < 3; ++iteration) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, Slice, ViewIndex);
                        previous_endpoint = map_source_to_target(
                            current_endpoint + raw_backward,
                            PreviousMappings[ViewIndex]);
                        if (previous_endpoint.valid < 0.5) {
                            endpoints_valid = false;
                            break;
                        }
                        float2 target_displacement =
                            previous_endpoint.coordinate - current_endpoint;
                        current_endpoint =
                            pixel - (1.0 - synthesis_fraction()) * target_displacement;
                    }
                    if (endpoints_valid) {
                        raw_backward = repeated_capture_flag() != 0
                            ? float2(0.0, 0.0)
                            : game_motion_for_pixel(
                                current_endpoint, Slice, ViewIndex);
                        previous_endpoint = map_source_to_target(
                            current_endpoint + raw_backward,
                            PreviousMappings[ViewIndex]);
                        endpoints_valid = previous_endpoint.valid >= 0.5;
                        last_displacement = previous_endpoint.coordinate - current_endpoint;
                        solve_residual = length(pixel -
                            (1.0 - synthesis_fraction()) * last_displacement - current_endpoint);
                    }
                    if (endpoints_valid) {
                        a_coordinate = previous_endpoint.coordinate;
                        b_coordinate = current_endpoint;
                        previous_sample = sample_previous_target(
                            previous_endpoint.coordinate, Slice, ViewIndex);
                        current_sample = sample_current_target(
                            current_endpoint, Slice, ViewIndex);
                    }
                } else {
                    raw_backward = repeated_capture_flag() != 0
                        ? float2(0.0, 0.0)
                        : flow_for_pixel(
                            pixel,
                            Slice,
                            ViewIndex,
                            flow_value_scale,
                            use_nvidia_cost);
                    float2 residual_backward = input_pose_compensated
                        ? raw_backward
                        : raw_backward - (previous_coverage.coordinate - pixel);
                    a_coordinate = pixel + residual_backward * synthesis_fraction();
                    b_coordinate = pixel - residual_backward * (1.0 - synthesis_fraction());
                    previous_sample = sample_previous_target(
                        pixel + residual_backward * synthesis_fraction(), Slice, ViewIndex);
                    current_sample = sample_current_target(
                        pixel - residual_backward * (1.0 - synthesis_fraction()),
                        Slice,
                        ViewIndex);
                }
                if (endpoints_valid) {
                    if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
                    float4 flow_midpoint = lerp(
                        previous_sample.color, current_sample.color, synthesis_fraction());
                    float disagreement = max(
                        abs(previous_sample.color.r - current_sample.color.r),
                        max(
                            abs(previous_sample.color.g - current_sample.color.g),
                            abs(previous_sample.color.b - current_sample.color.b)));
                    // Game motion's disagreements are mostly resampling at sharp
                    // detail, which a gentle slope keeps on the warp.
                    float confidence = saturate(1.0 - disagreement * 3.0);
                    float4 stable_previous =
                        sample_previous_target(pixel, Slice, ViewIndex).color;
                    float4 stable_midpoint = lerp(
                        stable_previous, current_fallback.color, synthesis_fraction());
                    // The hybrid compares its two branches' samples blurred a
                    // little: sharp detail resampled at a fraction of a pixel
                    // disagrees even where the motion is exact.
                    float compared = disagreement;
                    if (hybrid_layout) {
                        float4 a_low = 0.0, b_low = 0.0;
                        [unroll] for (uint k = 0; k < 4; ++k) {
                            float2 o = float2((k & 1) != 0 ? 0.5 : -0.5, (k & 2) != 0 ? 0.5 : -0.5);
                            a_low += sample_previous_target(a_coordinate + o, Slice, ViewIndex).color;
                            b_low += sample_current_target(b_coordinate + o, Slice, ViewIndex).color;
                        }
                        compared = rgb_error(a_low, b_low) * 0.25;
                    }
                    last_disagreement = compared;
                    if (!use_game_motion_pipeline) {
                        // Optical flow is not trusted as the game's vectors
                        // are. Whichever explains both frames better is taken:
                        // the flow, or the camera alone - a still scene under a
                        // turning head. The flow keeps a tie. On recorded game
                        // frames this beat the flow's own confidence measures -
                        // NVIDIA's cost, the fast preset's endpoint check and
                        // the forward flow's consistency - which all leaned on
                        // the camera-only blend where the game had moved.
                        confidence = saturate(1.0 +
                            (rgb_error(stable_previous, current_fallback.color) - disagreement) * 8.0);
                    }
                    if (use_nvidia_bidirectional && repeated_capture_flag() == 0) {
                        // The bidirectional option still keeps the flow only
                        // where the forward flow leads back.
                        float2 previous_coordinate = pixel + raw_backward;
                        float consistency = 0.0;
                        if (coordinate_inside_rect(
                                previous_coordinate,
                                input_pose_compensated
                                    ? PreviousMappings[ViewIndex].TargetRect
                                    : PreviousMappings[ViewIndex].SourceRect)) {
                            float2 raw_forward = forward_flow_for_pixel(
                                previous_coordinate,
                                Slice,
                                ViewIndex,
                                flow_value_scale,
                                input_pose_compensated);
                            consistency = 1.0 - smoothstep(
                                2.0, 6.0, length(raw_backward + raw_forward));
                        }
                        confidence *= consistency;
                    }
                    if (!use_game_motion_pipeline) {
                        output_color = saturate(lerp(stable_midpoint, flow_midpoint, confidence));
                        last_disagreement = lerp(
                            rgb_error(stable_previous, current_fallback.color),
                            compared, confidence);
                    } else {
                        // OFXR-BRIDGE's XR mapping can explain a head turn
                        // even when the game publishes zero scene motion.
                        // Do not force a mismatching guide warp over that
                        // exact camera result. Moving guides still own the
                        // occlusion and fade solve below.
                        bool camera_explains = length(raw_backward) <= 0.1 &&
                            rgb_error(stable_previous, current_fallback.color) +
                                1.0 / 255.0 < disagreement;
                        // The game's vectors are trusted: away from a motion
                        // edge, warped samples that disagree are one surface
                        // whose shading changed - a fade, a flash - and their
                        // motion-compensated blend is the frame between.
                        output_color = saturate(camera_explains ? stable_midpoint : flow_midpoint);
                        // Background a leading edge is covering shows only in
                        // A. There the solve has no fixed point: B shows the
                        // occluder at the pixel, so it starts inside it and
                        // alternates outside and in. After its odd number of
                        // steps it ends outside, on the covered surface's
                        // displacement, which places A's sample.
                        if (!camera_explains && solve_residual >= 0.5) {
                            CameraSample covered = sample_previous_target(
                                pixel + synthesis_fraction() * last_displacement, Slice, ViewIndex);
                            if (covered.valid >= 0.5) output_color = saturate(covered.color);
                        } else if (!camera_explains && disagreement > 0.1 && repeated_capture_flag() == 0) {
                            // Where the motion is not uniform - a vector 16
                            // pixels away differs by a tenth of a pixel -
                            // disagreeing samples are two surfaces instead. A's
                            // is usually hidden behind what B shows, background
                            // a trailing edge uncovers, so B's warped sample
                            // stands in. On recorded game frames the tenth beat
                            // a whole pixel: steep but smooth motion, such as
                            // ground rushing past, gains from B's sample too.
                            float2 here = game_motion_texel(
                                pixel - (1.0 - synthesis_fraction()) * last_displacement);
                            bool motion_edge = false;
                            [unroll] for (uint probe = 0; probe < 4; ++probe) {
                                float2 offset = probe == 0 ? float2(16, 0) : probe == 1 ? float2(-16, 0)
                                              : probe == 2 ? float2(0, 16) : float2(0, -16);
                                motion_edge = motion_edge ||
                                    length(game_motion_texel(pixel + offset) - here) > 0.1;
                            }
                            if (motion_edge) {
                                output_color = saturate(lerp(
                                    current_sample.color, flow_midpoint, confidence));
                            }
                            // B's sample may in turn be background that B has
                            // already uncovered but a faster surface in front
                            // still covers at the generated instant. That
                            // surface is another solution of the solve, and the
                            // one whose own A and B samples agree is visible in
                            // both frames, so in front. It is looked for from at
                            // most two starts on another surface, nearest first.
                            uint solves = 0;
                            bool done = !motion_edge;
                            [loop] for (uint radius_index = 0; radius_index < 4 && !done; ++radius_index) {
                                float radius = 4.0 * float(1u << radius_index);
                                [loop] for (uint direction = 0; direction < 8 && !done; ++direction) {
                                    float angle = float(direction) * 0.78539816;
                                    float2 start = pixel + radius * float2(cos(angle), sin(angle));
                                    // A start on the same surface finds B's again.
                                    if (length(game_motion_texel(start) - here) > 1.0) {
                                        float4 a, b;
                                        ++solves;
                                        if (game_motion_surface(pixel, start, a, b) &&
                                            rgb_error(a, b) < 0.05) {
                                            output_color = saturate(lerp(a, b, synthesis_fraction()));
                                            done = true;
                                        }
                                        done = done || solves >= 2;
                                    }
                                }
                            }
                        }
                    }
                    }
                }
            }
        }
    }
    return output_color;
}

float4 SynthesizeMidpointPS(FullscreenVertex input) : SV_Target {
    if((SynthesisFlags&128u)!=0u)return synthesize_panel(floor(input.position.xy),1.0,false);
    return synthesize_midpoint(input, 1.0, false, false, false, false, false);
}

float4 SynthesizeGameMotionMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false, false, true, false);
}

// The game's vectors and FidelityFX's optical flow both: per pixel, whichever
// explains both frames better. Content the vectors do not describe - a
// shadow, a reflection - moves as the image does, which the flow follows.
// The vectors keep a tie.
float4 SynthesizeHybridMidpointPS(FullscreenVertex input) : SV_Target {
    hybrid_layout = true;
    float4 vectors = synthesize_midpoint(input, 1.0, false, false, false, true, false);
    float vectors_disagreement = last_disagreement;
    // Where the vectors explain both frames, the flow need not be asked.
    [branch] if (vectors_disagreement < 0.02) return vectors;
    float4 flow = synthesize_midpoint(input, 1.0, false, false, false, false, false);
    return lerp(flow, vectors, saturate(1.0 + (last_disagreement - vectors_disagreement) * 8.0));
}

// The same composition with NVIDIA's optical flow, under the flow settings of
// the Hybrid route: the OFXR engine's flow branch, quality and direction.
float4 synthesize_hybrid_nvidia(FullscreenVertex input, bool bidirectional, bool fast) {
    hybrid_layout = true;
    float4 vectors = synthesize_midpoint(input, 1.0, false, false, false, true, false);
    float vectors_disagreement = last_disagreement;
    float4 result = vectors;
    // Where the vectors explain both frames, the flow need not be asked.
    [branch] if (vectors_disagreement >= 0.02) {
        float4 flow = synthesize_midpoint(input, 1.0 / 32.0, true, bidirectional, fast, false,
            (SynthesisFlags & 4u) != 0u);
        result = lerp(flow, vectors, saturate(1.0 + (last_disagreement - vectors_disagreement) * 8.0));
    }
    return result;
}

float4 SynthesizeHybridNvidiaMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_hybrid_nvidia(input, false, false);
}

float4 SynthesizeHybridNvidiaBidirectionalMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_hybrid_nvidia(input, true, false);
}

float4 SynthesizeHybridNvidiaFastMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_hybrid_nvidia(input, false, true);
}

float4 SynthesizeHybridNvidiaFastBidirectionalMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_hybrid_nvidia(input, true, true);
}

// The NVIDIA variants read flow in signed S10.5 fixed point, and from a
// previous input the pack warped into the current camera (PackNvidiaFlowInput).
float4 SynthesizeNvidiaMidpointPS(FullscreenVertex input) : SV_Target {
    if((SynthesisFlags&128u)!=0u)return synthesize_panel(floor(input.position.xy),1.0/32.0,true);
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, false, false, (SynthesisFlags & 4u) != 0u);
}

float4 SynthesizeNvidiaBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    if((SynthesisFlags&128u)!=0u)return synthesize_panel(floor(input.position.xy),1.0/32.0,true);
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, false, false, (SynthesisFlags & 4u) != 0u);
}

float4 SynthesizeNvidiaFastMidpointPS(FullscreenVertex input) : SV_Target {
    if((SynthesisFlags&128u)!=0u)return synthesize_panel(floor(input.position.xy),1.0/32.0,true);
    return synthesize_midpoint(input, 1.0 / 32.0, true, false, true, false, (SynthesisFlags & 4u) != 0u);
}

float4 SynthesizeNvidiaFastBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    if((SynthesisFlags&128u)!=0u)return synthesize_panel(floor(input.position.xy),1.0/32.0,true);
    return synthesize_midpoint(input, 1.0 / 32.0, true, true, true, false, (SynthesisFlags & 4u) != 0u);
}

float4 ReprojectGeneratedPS(FullscreenVertex input) : SV_Target {
    uint2 integer_pixel = uint2(input.position.xy);
    if (integer_pixel.x >= Width || integer_pixel.y >= Height ||
        Slice >= ArraySize) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 pixel = float2(integer_pixel);
    CameraSample generated = sample_previous_target(pixel, Slice, ViewIndex);
    if (generated.valid >= 0.5) {
        return saturate(generated.color);
    }
    return saturate(sample_current_target(pixel, Slice, ViewIndex).color);
}

// FSR receives the scene-only image. The displayed native HUD is restored
// here, after FSR dispatch and camera reprojection, from the exact same-frame
// final-minus-scene colour difference. No HUD pixels enter FSR optical flow or
// its temporal history. Both sources are sampled in the XR linear-light view.
float4 ReprojectGeneratedWithLateHudPS(FullscreenVertex input) : SV_Target {
    uint2 integer_pixel = uint2(input.position.xy);
    if (integer_pixel.x >= Width || integer_pixel.y >= Height ||
        Slice >= ArraySize) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    const float2 pixel = float2(integer_pixel);
    CameraSample generated = sample_previous_target(pixel, Slice, ViewIndex);
    float4 scene = SceneOnlyFrame.Load(int4(integer_pixel, Slice, 0));
    float4 final_color = CurrentFrame.Load(int4(integer_pixel, Slice, 0));
    float3 base_color = generated.valid >= 0.5
        ? generated.color.rgb : scene.rgb;
    return float4(saturate(base_color + final_color.rgb - scene.rgb), 1.0);
}
