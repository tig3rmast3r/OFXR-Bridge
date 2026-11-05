Texture2DArray<float4> PreviousFrame : register(t0);
Texture2DArray<float4> CurrentFrame : register(t1);
Texture2D<int2> BackwardFlow : register(t2);
Texture2D<uint> FlowAuxiliary : register(t3);
Texture2D<int2> ForwardFlow : register(t4);
Texture2D<uint> ForwardAuxiliary : register(t5);
Texture2DArray<float2> GameMotionVectors : register(t6);
Texture2DArray<float4> SceneOnlyFrame : register(t7);
// Hybrid's depth search (0.2 V505, V555): the game's depth beside its
// vectors, at their resolution, and the midpoint splat BuildDepthSplat
// writes, this eye's slices, one per synthetic output.
Texture2DArray<float> GameDepth : register(t8);
Texture2DArray<uint> DepthSplat : register(t9);
// Where this eye's depth is in its texture, four floats: x, y, width, height.
// Its own rectangle, since depth can be at render resolution beside vectors at
// the output's (Ready or Not); the constants have no room left for it.
ByteAddressBuffer GameDepthRegion : register(t11);
// Hybrid's HUD fix (0.2 V559, V573): the still mask BuildStillMask writes, at
// half the output's resolution over the whole image, four slices per image
// slice: 4s the texels whose edge has stayed in place for enough pairs in a
// row (a head-locked HUD's strokes and panel borders), 4s + 1 those grown by a
// texel, 4s + 2 and 4s + 3 the texel's ages in pairs, kept from pair to pair
// (two, read from one and written to the other by the pair's parity, since a
// cockpit stroke's age is read where the stroke was in A, another texel's).
// The composition keeps a masked pixel still and refuses a warped sample that
// lands on the mask, which would paint a copy of the HUD beside its place.
Texture2DArray<uint> StillMask : register(t10);
RWTexture2D<float4> PackedColor : register(u0);
#ifdef XRFG_DEPTH_SPLAT
// The pass that writes the splat, every eye's and output's slice.
RWTexture2DArray<uint> DepthSplatOut : register(u0);
#endif
#ifdef XRFG_STILL_MASK
// The passes that write the still mask, every image slice's.
RWTexture2DArray<uint> StillMaskOut : register(u0);
#endif
#ifdef XRFG_EYE_ORDER
// 0.2 V546: the eye-order measurement, in a descriptor block of its own: the
// other stream's vectors on the still mask's register and its verdict on the
// pack's, neither of which it uses.
Texture2DArray<float2> AlternateMotionVectors : register(t10);
RWByteAddressBuffer EyeOrderVerdict : register(u0);
#endif
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

// 0.2 V559: the previous frame at one of its own image pixels, with no pose
// mapping: where a head-locked HUD is, whatever the head did between the
// frames. The world moves across the image on a head turn; the HUD does not.
CameraSample sample_previous_image(
    float2 image_coordinate,
    uint slice,
    uint view_index) {
    CameraSample output;
    output.color = float4(0.0, 0.0, 0.0, 1.0);
    output.valid = 0.0;
    output.padding = float3(0.0, 0.0, 0.0);
    float2 source_minimum = PreviousMappings[view_index].SourceRect.xy;
    float2 source_maximum = source_minimum + PreviousMappings[view_index].SourceRect.zw;
    bool valid = image_coordinate.x >= source_minimum.x - 0.5 &&
        image_coordinate.y >= source_minimum.y - 0.5 &&
        image_coordinate.x <= source_maximum.x - 0.5 &&
        image_coordinate.y <= source_maximum.y - 0.5;
    if (valid) {
        output.color = bilinear_previous_source(image_coordinate, slice);
        output.valid = 1.0;
    }
    return output;
}

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
        // Bits 15 and 31 are the depth flags (game_depth_available).
        return float4(float(UseGameMotion & 0xffffU), float(UseGameMotion >> 16),
                      float(GameMotionPadding & 0x7fffU), float((GameMotionPadding >> 16) & 0x7fffU));
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
    // 0.2 V503, Hybrid's built-in: DLSS vectors are at render resolution, and
    // across a motion edge the blend of a thin bar's vector and the
    // background's is a motion neither surface has: the solve never settles
    // on the bar and the depth search refuses it. The nearest vector instead;
    // the flow covers what that costs under rotation.
    if (hybrid_layout) {
        raw = fraction.y < 0.5
            ? (fraction.x < 0.5 ? GameMotionVectors.Load(int4(top_left, 0, 0))
                                : GameMotionVectors.Load(int4(bottom_right.x, top_left.y, 0, 0)))
            : (fraction.x < 0.5 ? GameMotionVectors.Load(int4(top_left.x, bottom_right.y, 0, 0))
                                : GameMotionVectors.Load(int4(bottom_right, 0, 0)));
    }
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

// ---- Hybrid's depth search, 0.2's (V505, V544, V555) at its built-in
// values. GameMotionPadding bit 15: the guide's depth is at t8; bit 31: it
// is inverted, larger nearer. SynthesisFlags bit 5: this eye's splat is at
// t9; bits 27-28: which synthetic output this draw or splat pass is.
bool game_depth_available() { return (GameMotionPadding & 0x8000u) != 0u; }
bool depth_splat_available() { return (SynthesisFlags & 32u) != 0u; }
uint depth_splat_output() { return (SynthesisFlags >> 27) & 3u; }
// A surface is nearer than another by this factor, and its own samples in
// A and B must agree within this (0.2's Nearer and Depth agreement).
static const float kDepthNearerFactor = 1.5;
static const float kDepthAgreeLimit = 0.25;

float4 game_depth_region() { return asfloat(GameDepthRegion.Load4(0)); }

// The splat's grid: the smaller of depth and the vectors. Past either's size
// it would add memory and nothing else.
float2 depth_splat_extent() { return min(game_depth_region().zw, GameMotionRect.zw); }

// The depth texel game_nearness reads for `pixel`.
int2 game_depth_texel(float2 pixel) {
    float4 output_rect = game_motion_output_rect();
    float2 uv = (pixel - output_rect.xy + 0.5) / max(output_rect.zw, float2(1.0, 1.0));
    float4 region = game_depth_region();
    float2 maximum = region.xy + region.zw - 1.0;
    return int2(clamp(region.xy + uv * region.zw, region.xy, maximum));
}

// How near the surface B shows at `pixel` is, larger nearer. Only its order
// is used.
float game_nearness(float2 pixel) {
    float depth = GameDepth.Load(int4(game_depth_texel(pixel), 0, 0));
    return (GameMotionPadding & 0x80000000u) != 0u ? depth : 1.0 - depth;
}

bool nearer(float a, float b) { return a > b * kDepthNearerFactor + 1.0e-6; }

// The splat's texel for a coordinate of the image: its grid over the output
// rectangle, counted from its origin (0.2's coverage mask grid, V539, was the
// vectors').
int2 coverage_mask_texel(float2 coordinate) {
    float4 output_rect = game_motion_output_rect();
    float2 uv = (coordinate - output_rect.xy + 0.5) / max(output_rect.zw, float2(1.0, 1.0));
    float2 extent = depth_splat_extent();
    float2 limit = max(ceil(extent), float2(1.0, 1.0));
    return int2(clamp(floor(uv * extent), float2(0.0, 0.0), limit));
}

// A splat word: bits 24-31 how near by the game's depth, on a log scale - 8
// steps a doubling of distance over 32 doublings from the near plane (V544:
// as it comes, a building and the terrain behind it tied) - and bits 0-23
// the displacement in quarter pixels, 12 bits an axis from -512 to +512. The
// largest word wins a texel; 0 is an empty one.
float coverage_mask_depth_priority(float nearness) {
    return saturate(1.0 + log2(max(nearness, 2.3283064e-10)) / 32.0);
}

uint coverage_mask_word(float2 motion, float priority) {
    uint2 quarter = uint2(clamp(round(motion * 4.0) + 2048.0, 0.0, 4095.0));
    return (uint(saturate(priority) * 255.0) << 24) | (quarter.x << 12) | quarter.y;
}

float2 coverage_mask_motion(uint word) {
    return (float2(float((word >> 12) & 4095u), float(word & 4095u)) - 2048.0) / 4.0;
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

// ---- Hybrid's HUD fix: 0.2's still mask (V559-V563, V573) at its built-in
// values. A translucent head-locked HUD over a moving background (Star
// Citizen's on a stick turn) changes with what is behind it, so the flow and
// the vectors there follow the background and every synthetic drags the HUD
// along the turn, N-1 copies a side at NX. Its edges do not move: a strong
// luma edge at the same place, with the same orientation and a similar
// strength, in A and in B for pairs in a row is content that stayed. A moving
// world edge coincides with another like it only by chance.
// SynthesisFlags bit 3: the fix is on. Bit 6: the mask is at t10; without one
// (no R8_UINT typed UAV load) the per-pixel stroke test stands in, which sees
// only the pixel before each edge. Hybrid's compositions alone carry them.
bool hud_fix_enabled() { return hybrid_layout && (SynthesisFlags & 8u) != 0u; }
bool still_mask_available() { return hybrid_layout && (SynthesisFlags & 64u) != 0u; }

static const uint kStillMaskSlices = 4u;

// The still mask's byte at a target coordinate, from the slice grown by a
// texel or the one as built: bit 7 a stroke of B is here, bit 6 it is a
// cockpit's (below), bits 0-2 and 3-5 where its match in A sits, -3..3 pixels
// each way, biased by 3. Zero: none.
uint still_mask_word(float2 coordinate, bool grown) {
    uint2 limit = (uint2(Width, Height) - uint2(1u, 1u)) >> 1u;
    int2 texel = clamp(int2(floor(coordinate * 0.5)), int2(0, 0), int2(limit));
    return StillMask.Load(int4(texel, int(kStillMaskSlices * Slice + (grown ? 1u : 0u)), 0));
}

// The stroke's tier, the word's top two bits. A helmet's (128) stays at its
// place in the image whatever the head does; a cockpit's (192) - a ship's
// instruments, drawn on its canopy - stays at its place in the cockpit, which
// the head pose maps like the cockpit's own struts. Both stay put while the
// world beyond them turns with the stick.
static const uint kStillHelmet = 128u;
static const uint kStillCockpit = 192u;
uint still_mask_tier(uint word) {
    return word & 192u;
}

uint still_mask_pack(int2 offset) {
    uint2 biased = uint2(clamp(offset, int2(-3, -3), int2(3, 3)) + int2(3, 3));
    return 128u | biased.x | (biased.y << 3u);
}

// The stroke's place in A relative to its place in B, in pixels.
float2 still_mask_offset(uint word) {
    return float2(int2(int(word & 7u), int((word >> 3u) & 7u)) - int2(3, 3));
}

// A stroke of the tier (0: either) on the grown slice.
bool still_mask_at(float2 coordinate, uint tier) {
    uint word = still_mask_word(coordinate, true);
    return tier == 0u ? (word & 128u) != 0u : still_mask_tier(word) == tier;
}

// Whether the path from a pixel to its warped sample crosses the mask: a
// translucent panel is a tint over the background, and a sample pulled from
// the other side of its border carries the wrong tint - a band of the panel
// beside itself, N - 1 copies deep. A tap every three pixels along the path
// (a stroke's grown band is six or so wide; four fixed taps let a fast turn's
// samples step over it), the last the landing test, 24 at most. Of one tier's
// strokes (0: either), the path in that tier's frame.
bool still_mask_crossed(float2 from, float2 to, uint tier) {
    bool crossed = false;
    uint taps = clamp(uint(ceil(length(to - from) / 3.0)), 1u, 24u);
    for (uint tap = 1u; tap <= taps; ++tap) {
        crossed = crossed || still_mask_at(lerp(from, to, float(tap) / float(taps)), tier);
    }
    return crossed;
}

// The value a stroke is measured on: luma, or the strongest colour channel
// where that says more - a dark blue HUD stroke on black has almost no luma
// (blue weighs 7% of it) and all of its contrast in colour.
float edge_luma(float4 color) {
    float3 rgb = saturate(color).rgb;
    return max(dot(rgb, float3(0.2126, 0.7152, 0.0722)), 0.6 * max(rgb.r, max(rgb.g, rgb.b)));
}

// A stroke probe at a target coordinate in one frame: the luma gradient a
// pixel either side, its strength, and whether it is a stroke rather than a
// step - the luma that rose across the pixel falls at least half of that
// again within three pixels along the gradient, or had fallen so just
// before. HUD text and lines are strokes; a surface's outline, and a
// repeating texture's steps (which coincide with themselves when the texture
// moves by its period), are steps and stay the flow's.
struct StrokeProbe {
    float2 gradient;
    float strength;
    float2 axis;
    // A strong edge, of either kind; a stroke is one the luma comes back
    // from within three pixels.
    bool edge;
    bool stroke;
    // The share of the rise over five pixels along the axis that happens
    // between the two adjacent ones, 0-1.
    float sharpness;
};

// The still mask's built-ins, the values the Star Citizen headset sessions
// settled on. An edge's threshold, 10/255. An edge whose adjacent pixels
// carry kStillSharp of its rise (a HUD's line or text, not a light's glow),
// and dimmer than kStillDim, is a dim HUD element's, judged on its own
// contrast (judge_still_age). Pairs in a row an edge must stay put before it
// is held (a step one more), and the share of the moved reading's error the
// still reading may have.
static const float kStillEdge = 10.0 / 255.0;
static const float kStillSharp = 0.5;
static const float kStillDim = 24.0 / 255.0;
static const uint kStillAgePairs = 2u;
static const float kStillMovedRatio = 0.05;

// Which frame a stroke probe reads: B, A at its own image pixels (a helmet's
// HUD), or A through the head pose (a cockpit's instruments).
static const uint kProbeCurrent = 0u;
static const uint kProbeImage = 1u;
static const uint kProbePose = 2u;

CameraSample probe_sample(float2 pixel, uint slice, uint frame) {
    CameraSample sample;
    if (frame == kProbeImage) {
        sample = sample_previous_image(pixel, slice, ViewIndex);
    } else if (frame == kProbePose) {
        sample = sample_previous_target(pixel, slice, ViewIndex);
    } else {
        sample = sample_current_target(pixel, slice, ViewIndex);
    }
    return sample;
}

StrokeProbe probe_image_stroke(float2 pixel, uint slice, uint frame) {
    StrokeProbe probe;
    probe.gradient = float2(0.0, 0.0);
    probe.strength = 0.0;
    probe.axis = float2(1.0, 0.0);
    probe.edge = false;
    probe.stroke = false;
    probe.sharpness = 0.0;
    // The edge between this pixel and the next, right and down: a difference
    // across adjacent pixels sees a pattern two pixels wide (a HUD panel's
    // scanlines), which one taken either side of the pixel cannot - its two
    // taps are always the same phase of it. A is read at its own image pixels
    // for a helmet's HUD, which sits at the same screen place in both frames
    // however the head moved, or through the pose for a cockpit's.
    CameraSample c = probe_sample(pixel, slice, frame);
    CameraSample r = probe_sample(pixel + float2(1.0, 0.0), slice, frame);
    CameraSample d = probe_sample(pixel + float2(0.0, 1.0), slice, frame);
    float luma_c = edge_luma(c.color);
    float luma_r = edge_luma(r.color), luma_d = edge_luma(d.color);
    probe.gradient = float2(luma_r - luma_c, luma_d - luma_c);
    probe.strength = length(probe.gradient);
    bool edge = min(c.valid, min(r.valid, d.valid)) >= 0.5 &&
        probe.strength >= kStillEdge;
    probe.edge = edge;
    [branch] if (edge) {
        probe.axis = abs(probe.gradient.x) >= abs(probe.gradient.y)
            ? float2(sign(probe.gradient.x), 0.0)
            : float2(0.0, sign(probe.gradient.y));
        // The luma rises from this pixel to the next along the axis (the
        // axis points the way it rises): the pixel is the minus side, the
        // next the plus side. Where the axis points left or up, the "next"
        // is the pixel itself and this one is the plus side.
        bool forward = probe.axis.x > 0.0 || probe.axis.y > 0.0;
        float luma_next = abs(probe.axis.x) > 0.0 ? luma_r : luma_d;
        float near_plus = forward ? luma_next : luma_c;
        float near_minus = forward ? luma_c : luma_next;
        float2 plus_at = forward ? abs(probe.axis) : float2(0.0, 0.0);
        float2 minus_at = forward ? float2(0.0, 0.0) : abs(probe.axis);
        CameraSample far_plus = probe_sample(pixel + plus_at + 2.0 * probe.axis, slice, frame);
        CameraSample far_minus = probe_sample(pixel + minus_at - 2.0 * probe.axis, slice, frame);
        // The luma rises along the axis across this pixel. A bright stroke
        // on the plus side falls again beyond it; a dark stroke on the
        // minus side rises again beyond it.
        float bright_stroke = near_plus - edge_luma(far_plus.color);
        float dark_stroke = edge_luma(far_minus.color) - near_minus;
        probe.stroke = far_plus.valid >= 0.5 && far_minus.valid >= 0.5 &&
            max(bright_stroke, dark_stroke) >= 0.5 * probe.strength;
        // How much of the rise across the five pixels from far minus to far
        // plus happens between the two adjacent ones: a HUD line or text's
        // edge rises within a pixel or two (a stroke comes back down beyond
        // it: 1), a light's glow gradually (about 0.2).
        float near_rise = near_plus - near_minus;
        float span_rise = edge_luma(far_plus.color) - edge_luma(far_minus.color);
        probe.sharpness = far_plus.valid >= 0.5 && far_minus.valid >= 0.5
            ? near_rise / max(max(span_rise, near_rise), 1.0e-4)
            : 0.0;
    }
    return probe;
}

// A through the pose at a target pixel is A's image read around the pixel's
// mapped place: one mapping a probe instead of one a tap (the mapping is a
// rotation, near enough a translation over five pixels).
StrokeProbe probe_stroke(float2 pixel, uint slice, uint frame) {
    StrokeProbe probe;
    [branch] if (frame == kProbePose) {
        MappedCoordinate mapped = map_target_to_source(pixel, PreviousMappings[ViewIndex]);
        probe = probe_image_stroke(mapped.coordinate, slice, kProbeImage);
        probe.edge = probe.edge && mapped.valid >= 0.5;
        probe.stroke = probe.stroke && mapped.valid >= 0.5;
    } else {
        probe = probe_image_stroke(pixel, slice, frame);
    }
    return probe;
}

// The same edge in both frames: the same orientation, a similar strength.
// (Not its colour: a translucent stroke's follows what is behind it, which a
// stick turn changes between the frames.) Both at least kStillEdge, the
// weaker at least 40% of the stronger, their orientations within a cosine of
// 0.85.
bool edges_match(StrokeProbe previous, StrokeProbe current) {
    float along = dot(previous.gradient, current.gradient) /
        max(previous.strength * current.strength, 1.0e-6);
    return previous.edge && current.edge &&
        min(previous.strength, current.strength) >= kStillEdge && along >= 0.85 &&
        min(previous.strength, current.strength) >= 0.4 * max(previous.strength, current.strength);
}

// The per-pixel form, for a pair without the still mask: a stroke at the same
// image place in A and in B.
bool edge_stationary_patch(float2 pixel, uint slice) {
    StrokeProbe now = probe_stroke(pixel, slice, kProbeCurrent);
    bool stationary = false;
    [branch] if (now.stroke) {
        StrokeProbe before = probe_stroke(pixel, slice, kProbeImage);
        stationary = before.stroke && edges_match(before, now);
    }
    return stationary;
}

// A masked pixel's own colour at this synthetic: the stroke moved by its own
// offset, scaled by the fraction, the two frames blended there. A stroke at X
// in B sat at X + offset in A, so at the fraction it is at X + (1 - f) *
// offset: this pixel reads A at p + f * offset and B at p - (1 - f) * offset.
// A is read at its image pixel for a helmet's stroke, through the pose for a
// cockpit's (what a still world pixel blends).
float4 still_mask_color(float2 pixel, uint word, float4 fallback) {
    float2 offset = still_mask_offset(word);
    float f = synthesis_fraction();
    CameraSample a = probe_sample(pixel + f * offset, Slice,
        still_mask_tier(word) == kStillCockpit ? kProbePose : kProbeImage);
    CameraSample b = sample_current_target(pixel - (1.0 - f) * offset, Slice, ViewIndex);
    float4 color = fallback;
    if (a.valid >= 0.5 && b.valid >= 0.5) {
        color = lerp(a.color, b.color, f);
    } else if (b.valid >= 0.5) {
        color = b.color;
    }
    return saturate(color);
}

// Where a head-locked HUD shown at this output pixel sits in B's image. In a
// synthetic camera the synthetic is submitted with the head's pose at its own
// instant, so the HUD keeps its image place: the output pixel, not the point
// of B its ray meets. In B's camera (0.2's only case) the compositor warps
// the frame to the head at its display time, so the HUD is placed between A's
// reprojected place and B's by the fraction - the pose shift of this pixel,
// scaled - and reads as steady against the head. On a stick turn the shift
// is zero.
float2 helmet_hud_source(float2 output_pixel, float2 pixel) {
    float2 source = pixel;
    if (synthetic_camera()) {
        source = output_pixel;
    } else {
        MappedCoordinate shifted = map_source_to_target(pixel, PreviousMappings[ViewIndex]);
        if (shifted.valid >= 0.5) {
            source = pixel - (1.0 - synthesis_fraction()) * (shifted.coordinate - pixel);
        }
    }
    return source;
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
bool game_motion_surface_at(float2 pixel, float2 start, out float4 previous_color,
    out float4 current_color, out float2 endpoint) {
    previous_color = float4(0.0, 0.0, 0.0, 1.0);
    current_color = previous_color;
    endpoint = start;
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

bool game_motion_surface(float2 pixel, float2 start, out float4 previous_color,
    out float4 current_color) {
    float2 endpoint;
    return game_motion_surface_at(pixel, start, previous_color, current_color, endpoint);
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
    // 0.2 V559: a warped sample refused for landing on the still mask; the
    // colour that stands in, applied last so nothing below reads the copy.
    bool hud_exclude = false;
    float4 hud_color = float4(0.0, 0.0, 0.0, 1.0);
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
            // 0.2 V559, V573: the still mask's word for this pixel. A cockpit's
            // stroke is not moved like a helmet's - the frame's pose places
            // the cockpit, as it does the struts - so it is looked for at the
            // pixel itself, where no helmet's stroke is.
            float2 hud_source = pixel;
            uint still_word = 0u;
            bool edge_stationary = false;
            [branch] if (hud_fix_enabled()) {
                hud_source = helmet_hud_source(float2(integer_pixel), pixel);
                [branch] if (still_mask_available()) {
                    still_word = still_mask_word(hud_source, true);
                    if (still_mask_tier(still_word) != kStillHelmet) {
                        uint cockpit_word = still_mask_word(pixel, true);
                        still_word = still_mask_tier(cockpit_word) == kStillCockpit ? cockpit_word : 0u;
                    }
                    edge_stationary = (still_word & 128u) != 0u;
                } else {
                    edge_stationary = edge_stationary_patch(hud_source, Slice);
                    still_word = still_mask_pack(int2(0, 0));
                }
                edge_stationary = edge_stationary && repeated_capture_flag() == 0 &&
                    current_fallback.valid >= 0.5;
            }
            // The mask first: a HUD element over uniform space passes the
            // unchanged-pixel test too, and that would hold it at B's place
            // without the placement above.
            bool preserve_stationary = !edge_stationary && validate_fast &&
                repeated_capture_flag() == 0 && current_fallback.valid >= 0.5 &&
                fast_stationary_patch(pixel, Slice, current_fallback.color);
            bool static_overlay = !edge_stationary && !preserve_stationary &&
                use_game_motion_pipeline && repeated_capture_flag() == 0 &&
                current_fallback.valid >= 0.5 &&
                game_motion_static_overlay(pixel, current_fallback.color);
            if (edge_stationary) {
                // A stroke that stayed is read from A at the same image pixel,
                // not through the pose (a head turn moves the world across
                // the image, not a HUD). The answer is the same whichever of
                // Hybrid's branches asks, so the flow need not be.
                output_color = still_mask_color(
                    still_mask_tier(still_word) == kStillCockpit ? pixel : hud_source,
                    still_word,
                    saturate(lerp(
                        sample_previous_target(pixel, Slice, ViewIndex).color,
                        current_fallback.color,
                        synthesis_fraction())));
                last_disagreement = 0.0;
            } else if (preserve_stationary) {
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
                // 0.2 V559: a warped sample that lands on the still mask - a
                // HUD stroke read from beside its place - would paint a copy
                // of it here. Take the other frame's sample alone; with both
                // on the mask, this pixel's own two.
                [branch] if (still_mask_available() && endpoints_valid) {
                    // The A sample's own image pixel, where a HUD would be,
                    // and the path to it from this pixel's own image place;
                    // a sample is refused for crossing the grown mask, not
                    // only for landing on it (the one as built misses half a
                    // stroke's pixels, and a "safe" sample taken there was
                    // the HUD again).
                    MappedCoordinate previous_source = map_target_to_source(
                        a_coordinate, PreviousMappings[ViewIndex]);
                    bool previous_on_mask = previous_source.valid >= 0.5 &&
                        still_mask_crossed(
                            previous_coverage.coordinate,
                            previous_source.coordinate, kStillHelmet);
                    // A cockpit's stroke sat in A where the pose puts it, so
                    // the A sample's path is tested in B's frame, as the B
                    // sample's is.
                    previous_on_mask = previous_on_mask ||
                        still_mask_crossed(pixel, a_coordinate, kStillCockpit);
                    bool current_on_mask = still_mask_crossed(pixel, b_coordinate, 0u);
                    if (previous_on_mask && current_on_mask) {
                        CameraSample previous_at_pixel = sample_previous_target(pixel, Slice, ViewIndex);
                        hud_exclude = true;
                        hud_color = still_mask_color(pixel, still_mask_word(b_coordinate, true),
                            saturate(lerp(
                                previous_at_pixel.valid >= 0.5 ? previous_at_pixel.color
                                                               : current_fallback.color,
                                current_fallback.color,
                                synthesis_fraction())));
                    } else if (previous_on_mask && current_sample.valid >= 0.5) {
                        hud_exclude = true;
                        hud_color = saturate(current_sample.color);
                    } else if (current_on_mask && previous_sample.valid >= 0.5) {
                        hud_exclude = true;
                        hud_color = saturate(previous_sample.color);
                    }
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
                        // 0.2 V505/V555: a nearer surface - a thin bar sweeping
                        // across background both frames show - can reach the
                        // pixel at the generated instant although the answer's
                        // samples agree, and the bar went missing from every
                        // generated frame. The splat names the nearest surface
                        // at this texel at this instant; where it is nearer than
                        // the answer by the margin, one solve from where it is
                        // in B, held to its own agreement.
                        if (hybrid_layout && game_depth_available() && depth_splat_available() &&
                            !(!camera_explains && solve_residual >= 0.5) &&
                            repeated_capture_flag() == 0) {
                            const float here = game_nearness(b_coordinate);
                            const uint rank = uint(saturate(coverage_mask_depth_priority(
                                here * kDepthNearerFactor + 1.0e-6)) * 255.0);
                            const uint word = DepthSplat.Load(int4(coverage_mask_texel(pixel),
                                int(depth_splat_output()), 0));
                            if (word != 0u && (word >> 24u) >= rank) {
                                const float2 start = pixel -
                                    (1.0 - synthesis_fraction()) * coverage_mask_motion(word);
                                float4 near_a, near_b;
                                float2 endpoint;
                                if (game_motion_surface_at(pixel, start, near_a, near_b, endpoint) &&
                                    nearer(game_nearness(endpoint), here) &&
                                    rgb_error(near_a, near_b) < kDepthAgreeLimit) {
                                    output_color = saturate(lerp(near_a, near_b, synthesis_fraction()));
                                    // Verified by depth and its own samples, so the
                                    // hybrid keeps it: the flow, following the
                                    // background, would take the bar back. 0.2 asked
                                    // the flow only past 62/255, this limit's size.
                                    last_disagreement = 0.0;
                                }
                            }
                        }
                    }
                    }
                }
            }
        }
    }
    if (hud_exclude) {
        output_color = hud_color;
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

#ifdef XRFG_DEPTH_SPLAT
// 0.2 V555: for each texel of B's vectors, the surface's word at the texel of
// the generated image it occupies at the generated instant - B's position
// plus the generated share of its displacement to A, the fixed point the
// search's solve converges to - on the 2 x 2 texels around that point, so no
// gap opens between neighbours; the nearest by the game's depth keeps a
// contested texel. The composition reads one word per pixel. Slice = the
// eye; a slice per synthetic output, to 4X.
static const uint kDepthSplatOutputs = 3u;

[numthreads(8, 8, 1)]
void ClearDepthSplat(uint3 id : SV_DispatchThreadID) {
    hybrid_layout = true;
    float2 limit = ceil(depth_splat_extent());
    if (float(id.x) <= limit.x && float(id.y) <= limit.y) {
        DepthSplatOut[uint3(id.xy, kDepthSplatOutputs * Slice + depth_splat_output())] = 0u;
    }
}

[numthreads(8, 8, 1)]
void BuildDepthSplat(uint3 id : SV_DispatchThreadID) {
    hybrid_layout = true;
    float2 extent = depth_splat_extent();
    if (float(id.x) >= extent.x || float(id.y) >= extent.y) return;
    // The output pixel under this texel's centre: game_motion_texel's map,
    // inverted.
    float4 output_rect = game_motion_output_rect();
    float2 uv = (float2(id.xy) + 0.5) / max(extent, float2(1.0, 1.0));
    float2 pixel = output_rect.xy + uv * output_rect.zw - 0.5;
    float2 motion = game_motion_for_pixel(pixel, Slice, ViewIndex);
    MappedCoordinate previous = map_source_to_target(pixel + motion, PreviousMappings[ViewIndex]);
    if (previous.valid < 0.5) return;
    float2 displacement = previous.coordinate - pixel;
    uint word = coverage_mask_word(displacement,
        coverage_mask_depth_priority(game_nearness(pixel)));
    float2 midpoint = pixel + (1.0 - synthesis_fraction()) * displacement;
    float2 per_pixel = extent / max(output_rect.zw, float2(1.0, 1.0));
    float2 at = (midpoint - output_rect.xy + 0.5) * per_pixel;
    int2 limit = int2(ceil(extent));
    int2 base = int2(floor(at - 0.5));
    uint slice = kDepthSplatOutputs * Slice + depth_splat_output();
    [unroll] for (uint corner = 0; corner < 4; ++corner) {
        int2 texel = base + int2(corner & 1u, corner >> 1);
        if (all(texel >= 0) && all(texel <= limit)) {
            InterlockedMax(DepthSplatOut[uint3(uint2(texel), slice)], word);
        }
    }
}
#endif

#ifdef XRFG_STILL_MASK
// 0.2 V559, V573: the still mask, at half the output's resolution over the
// whole image. BuildStillMask writes, per texel, whether the stroke at its
// even pixel stayed in place between the frames; DilateStillMask grows that
// by a texel into the slice a pixel's own stillness reads.

// Bit 25 of the mask passes' flags is the pair's parity: the ages are read
// from slice 4s + 2 + parity and written to the other, so a texel can read a
// neighbour's age from the last pair while the neighbour writes its new one.
uint still_age_read_slice() {
    return kStillMaskSlices * Slice + 2u + ((SynthesisFlags >> 25u) & 1u);
}
uint still_age_write_slice() {
    return kStillMaskSlices * Slice + 3u - ((SynthesisFlags >> 25u) & 1u);
}

// One tier's reading of a texel's edges in A: whether one matched, whether a
// matched one is a stroke, and whether the match is a dim element's.
struct StillMatch {
    bool matched;
    bool stroke;
    bool dim;
    float dim_strength;
};

// An edge is matched where the same edge is in A at the tier's place. An edge
// sharp in both frames and dimmer than kStillDim is a dim HUD element's,
// judged on its own contrast (`dim_strength`, its weakest matched strength):
// its colour differences are a few levels, which the fixed margin and share
// never let it win, and it ghosted on stick turns. Any other edge - a light's
// soft glow among them - takes the fixed margin. Dim is the reading only
// where no matched edge of the texel is a bright one.
StillMatch match_still_edges(float2 pixel, StrokeProbe now, StrokeProbe now_odd, uint frame) {
    StillMatch match;
    match.matched = false;
    match.stroke = false;
    match.dim = false;
    match.dim_strength = 1.0;
    bool bright = false;
    [branch] if (now.edge) {
        StrokeProbe before = probe_stroke(pixel, Slice, frame);
        bool sharp = before.sharpness >= kStillSharp && now.sharpness >= kStillSharp;
        float weakest = min(before.strength, now.strength);
        match.matched = edges_match(before, now);
        match.stroke = match.matched && now.stroke;
        if (match.matched && sharp && weakest < kStillDim) {
            match.dim = true;
            match.dim_strength = min(match.dim_strength, weakest);
        } else if (match.matched) {
            bright = true;
        }
    }
    [branch] if (now_odd.edge) {
        StrokeProbe before_odd = probe_stroke(pixel + float2(1.0, 1.0), Slice, frame);
        bool sharp_odd = before_odd.sharpness >= kStillSharp && now_odd.sharpness >= kStillSharp;
        float weakest_odd = min(before_odd.strength, now_odd.strength);
        bool matched_odd = edges_match(before_odd, now_odd);
        match.stroke = match.stroke || (matched_odd && now_odd.stroke);
        match.matched = match.matched || matched_odd;
        if (matched_odd && sharp_odd && weakest_odd < kStillDim) {
            match.dim = true;
            match.dim_strength = min(match.dim_strength, weakest_odd);
        } else if (matched_odd) {
            bright = true;
        }
    }
    match.dim = match.dim && !bright;
    return match;
}

// The age a matched edge has this pair, from the last pair's. Held only where
// holding explains the two frames better than moving: A at the tier's place
// (its own image place for a helmet's stroke, through the pose for a
// cockpit's) against B, and A where the game's vectors say this content came
// from against B, over the three pixels the probe read. A HUD stays still
// while the world moves past; world content, however its edges match by
// chance, is where the motion put it. Where nothing moved the two readings
// are alike and the age is left as it is: a HUD that has earned its age keeps
// it through a still view, and nothing ghosts there anyway.
//
// A cockpit's stroke is judged on its edge: A through the pose is a bilinear
// reading between pixels, which blurs a thin stroke by up to half its
// contrast, and the colour share is never won. It is held where the edge is
// not where the motion says the content came from (`moved_edge` false: no
// matching edge there in A) and the pose reading explains the colours at
// least as well as the moved one. A world edge the motion describes has its
// match at the moved place; one that happens to match at its pose place,
// where the motion is wrong (a light), is world content that stayed - what
// holding shows.
uint judge_still_age(
    float2 pixel, float2 residual, float pose_shift, uint frame, StillMatch match,
    bool moved_edge, uint previous_age) {
    // For a helmet's stroke, nothing moved on the screen only where neither
    // the content nor the head did. A cockpit's is read through the pose, so
    // only the content's motion beyond the pose - the stick's - tells it from
    // the world.
    bool helmet = frame == kProbeImage;
    uint age = previous_age;
    [branch] if (length(residual) >= 1.0 || (helmet && pose_shift >= 1.0)) {
        float still_error = 0.0, moved_error = 0.0, pose_error = 0.0;
        // Whether this pair can judge: every sample of holding's reading, and
        // of at least one of moving's, could be fetched (a sample off the
        // frame or a cropped viewport is no evidence).
        bool judged = true;
        // For a helmet's stroke with the head moving, moved is also read by
        // the head pose alone, and the better of the two readings counts. A
        // far light follows the pose exactly while the flow on a light and
        // its glow is poor; a head-locked HUD follows neither. Without head
        // motion the pose reading is the still one, so it is not taken (stick
        // turns are judged by the vectors alone). For a cockpit's the pose
        // reading IS the still one.
        bool pose_valid = helmet && pose_shift >= 2.0;
        bool flow_valid = true;
        float2 taps[3] = {float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0)};
        [unroll] for (uint tap = 0u; tap < 3u; ++tap) {
            float2 at = pixel + taps[tap];
            CameraSample b = sample_current_target(at, Slice, ViewIndex);
            CameraSample a_still = probe_sample(at, Slice, frame);
            CameraSample a_moved = sample_previous_target(at + residual, Slice, ViewIndex);
            CameraSample a_pose = sample_previous_target(at, Slice, ViewIndex);
            judged = judged && b.valid >= 0.5 && a_still.valid >= 0.5;
            flow_valid = flow_valid && a_moved.valid >= 0.5;
            pose_valid = pose_valid && a_pose.valid >= 0.5;
            still_error += rgb_error(a_still.color, b.color);
            moved_error += rgb_error(a_moved.color, b.color);
            pose_error += rgb_error(a_pose.color, b.color);
        }
        // Moved by whichever readings could be fetched - the pose alone where
        // the vector's points off the frame (a wild vector on a light).
        judged = judged && (flow_valid || pose_valid);
        moved_error = flow_valid && pose_valid ? min(moved_error, pose_error)
            : pose_valid ? pose_error : moved_error;
        // Still must beat moved by a margin, and by the share: a repeating
        // pattern moved by its period reads 0 either way, and that is not a
        // HUD. A dim HUD element's edge on its own scale - a margin of a tenth
        // of its contrast a pixel, and a share of at least half: over a few
        // levels of contrast, moving's error is only a little larger than the
        // contrast itself.
        float share = match.dim ? max(kStillMovedRatio, 0.5) : kStillMovedRatio;
        float margin = match.dim ? 3.0 * 0.1 * match.dim_strength : 3.0 * 2.0 / 255.0;
        bool still_wins = helmet ? judged && still_error + margin <= share * moved_error
                                 : judged && !moved_edge && still_error <= moved_error;
        // The age resets only where moving clearly explains the two frames
        // (its error under half of holding's), and is kept where neither
        // reading clearly wins: lines of HUD text stacked close together, on a
        // vertical head motion of about their spacing, are explained by the
        // pose reading about as well as by holding (one line lands where the
        // next was). A light on a head motion still resets (the pose explains
        // it).
        bool moved_wins = helmet ? judged && 2.0 * moved_error + margin <= still_error
                                 : judged && moved_edge && moved_error <= still_error;
        // Four bits an age (both tiers share the byte).
        age = still_wins ? min(previous_age + 1u, 15u) : moved_wins ? 0u : previous_age;
    }
    return age;
}

[numthreads(8, 8, 1)]
void BuildStillMask(uint3 id : SV_DispatchThreadID) {
    hybrid_layout = true;
    uint2 size = (uint2(Width, Height) + 1u) / 2u;
    if (id.x >= size.x || id.y >= size.y) return;
    // The texel's even pixel, on a pixel centre: between two the samples blur
    // a two-pixel stroke away. The odd one is the dilation's.
    float2 pixel = float2(id.xy) * 2.0;
    // A double-wide image's two views share the slice: each pass keeps to its
    // own. (0.2 built no mask for those, and the per-pixel test held half a
    // stroke.)
    if (!coordinate_inside_rect(pixel, PreviousMappings[ViewIndex].TargetRect)) return;
    uint word = 0u;
    // Both parities: the edge between adjacent pixels sits on one of the
    // texel's two columns and one of its two rows, so the even pixel and the
    // odd one diagonally next to it are both probed.
    StrokeProbe now = probe_stroke(pixel, Slice, kProbeCurrent);
    StrokeProbe now_odd = probe_stroke(pixel + float2(1.0, 1.0), Slice, kProbeCurrent);
    // The edge at its own image place in A: a head-locked HUD is there
    // whatever the head did. No search around it: a world edge moving a few
    // pixels a pair would match at its offset, and in text a neighbouring
    // stroke of the same glyph matches as well as this one. (The word's offset
    // bits stay for a HUD that sways of its own, should one turn up; none is
    // written now.)
    StillMatch helmet = match_still_edges(pixel, now, now_odd, kProbeImage);
    // And at its place in A through the head pose: a ship's instruments on
    // its canopy stay with the cockpit, which the head moves across the image
    // like the struts, while the world turns past them on the stick. Static
    // world content matches there too, but holding it there is what a still
    // world pixel shows anyway; only content the vectors say moved is judged.
    // Where the head moved less than half a pixel the two places are one, and
    // the helmet's reading is taken as it is.
    MappedCoordinate shifted = map_source_to_target(pixel, PreviousMappings[ViewIndex]);
    float pose_shift = shifted.valid >= 0.5 ? length(shifted.coordinate - pixel) : 0.0;
    StillMatch cockpit = helmet;
    [branch] if (pose_shift >= 0.5) {
        cockpit = match_still_edges(pixel, now, now_odd, kProbePose);
    }
    // And there for pairs in a row: a HUD is, a world edge that happens to
    // sit still on the screen for one pair (a light, a strut) seldom is once
    // the view moves. A stroke counts from its second pair, a step (a panel's
    // border, which a surface's outline resembles) from its third. The age
    // byte holds a helmet's age in bits 0-3 and a cockpit's in bits 4-7. A
    // helmet's stroke was at this texel in the last pair; a cockpit's where
    // the pose put it in A, which was the last pair's B.
    uint read_slice = still_age_read_slice();
    uint previous_helmet = StillMaskOut[uint3(id.xy, read_slice)] & 15u;
    uint previous_cockpit = 0u;
    [branch] if (cockpit.matched) {
        MappedCoordinate came_from = map_target_to_source(pixel, PreviousMappings[ViewIndex]);
        if (came_from.valid >= 0.5) {
            int2 texel = clamp(int2(floor((came_from.coordinate + 0.5) * 0.5)),
                int2(0, 0), int2(size) - int2(1, 1));
            previous_cockpit = StillMaskOut[uint3(texel, read_slice)] >> 4u;
        }
    }
    uint helmet_age = 0u, cockpit_age = 0u;
    [branch] if (helmet.matched || cockpit.matched) {
        MappedCoordinate came_from = map_source_to_target(
            pixel + game_motion_for_pixel(pixel, Slice, ViewIndex), PreviousMappings[ViewIndex]);
        float2 residual = came_from.valid >= 0.5 ? came_from.coordinate - pixel : float2(0.0, 0.0);
        [branch] if (helmet.matched) {
            helmet_age = judge_still_age(pixel, residual, pose_shift, kProbeImage, helmet,
                false, previous_helmet);
        }
        [branch] if (cockpit.matched) {
            // The same edge in A where the motion says this content came
            // from, at either parity.
            bool moved_edge = false;
            [branch] if (now.edge) {
                moved_edge = edges_match(probe_stroke(pixel + residual, Slice, kProbePose), now);
            }
            [branch] if (now_odd.edge && !moved_edge) {
                moved_edge = edges_match(
                    probe_stroke(pixel + float2(1.0, 1.0) + residual, Slice, kProbePose), now_odd);
            }
            cockpit_age = judge_still_age(pixel, residual, pose_shift, kProbePose, cockpit,
                moved_edge, previous_cockpit);
        }
    }
    // A helmet's stroke first: without head motion the two tiers are one, and
    // the helmet's placement is the plain one.
    if (helmet.matched && helmet_age >= kStillAgePairs + (helmet.stroke ? 0u : 1u)) {
        word = still_mask_pack(int2(0, 0));
    } else if (cockpit.matched && cockpit_age >= kStillAgePairs + (cockpit.stroke ? 0u : 1u)) {
        word = still_mask_pack(int2(0, 0)) | 64u;
    }
    StillMaskOut[uint3(id.xy, kStillMaskSlices * Slice)] = word;
    StillMaskOut[uint3(id.xy, still_age_write_slice())] = helmet_age | (cockpit_age << 4u);
}

[numthreads(8, 8, 1)]
void DilateStillMask(uint3 id : SV_DispatchThreadID) {
    uint2 size = (uint2(Width, Height) + 1u) / 2u;
    if (id.x >= size.x || id.y >= size.y) return;
    int2 limit = int2(size) - int2(1, 1);
    // Its own word, else a neighbour's (they share a stroke's offset and
    // tier).
    uint grown = StillMaskOut[uint3(id.xy, kStillMaskSlices * Slice)];
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            int2 texel = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), limit);
            uint neighbour = StillMaskOut[uint3(texel, kStillMaskSlices * Slice)];
            grown = grown != 0u ? grown : neighbour;
        }
    }
    StillMaskOut[uint3(id.xy, kStillMaskSlices * Slice + 1u)] = grown;
}
#endif

#ifdef XRFG_EYE_ORDER
// 0.2 V546: which of two DLSS streams nothing else tells apart is this eye's
// (dlss_motion_vectors.hpp, report_dlss_eye_order). Every second texel of this
// stream's vectors (t6) where the other stream's (t10) part from them by more
// than a pixel, the current image's pixel is compared with the previous image
// read at the end of each vector: the eye's own vectors land on the surface
// the pixel shows, the other eye's a disparity to the side. EyeOrderVerdict
// words: 0 the pixels compared, 1 those this stream's vector matched better by
// more than the margin, 2 those the other's did.
float eye_order_tone(float4 color) {
    return sqrt(max(dot(color.rgb, float3(0.2126, 0.7152, 0.0722)), 0.0));
}

[numthreads(1, 1, 1)]
void ClearEyeOrder(uint3 id : SV_DispatchThreadID) {
    EyeOrderVerdict.Store4(0, uint4(0u, 0u, 0u, 0u));
}

// 0 not compared, 1 compared without a winner, 2 this stream's vector won,
// 3 the other's.
uint eye_order_vote(uint2 cell) {
    uint vote = 0u;
    float2 extent = GameMotionRect.zw;
    int2 texel = int2(cell) * 2;
    float4 output_rect = game_motion_output_rect();
    float2 pixel = output_rect.xy +
        (float2(texel) + 0.5) / max(extent, float2(1.0, 1.0)) * output_rect.zw - 0.5;
    float2 limit = float2(float(Width) - 1.0, float(Height) - 1.0);
    bool inside = float(texel.x) < extent.x && float(texel.y) < extent.y &&
        all(pixel >= 0.0) && all(pixel <= limit);
    [branch] if (inside) {
        int2 at = int2(GameMotionRect.xy) + texel;
        float2 own = GameMotionVectors.Load(int4(at, 0, 0)) * GameMotionScale +
            GameMotionJitterDelta;
        float2 other = AlternateMotionVectors.Load(int4(at, 0, 0)) * GameMotionScale +
            GameMotionJitterDelta;
        float2 own_end = pixel + own;
        float2 other_end = pixel + other;
        bool compared = length(own - other) > 1.0 &&
            all(own_end >= 0.0) && all(own_end <= limit) &&
            all(other_end >= 0.0) && all(other_end <= limit);
        [branch] if (compared) {
            float now = eye_order_tone(bilinear_current_source(pixel, Slice));
            float own_error = abs(now - eye_order_tone(bilinear_previous_source(own_end, Slice)));
            float other_error =
                abs(now - eye_order_tone(bilinear_previous_source(other_end, Slice)));
            const float margin = 1.0 / 64.0;
            vote = own_error + margin < other_error ? 2u
                : (other_error + margin < own_error ? 3u : 1u);
        }
    }
    return vote;
}

groupshared uint eye_order_counts[3];

[numthreads(8, 8, 1)]
void MeasureEyeOrder(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    hybrid_layout = true;
    if (index < 3u) eye_order_counts[index] = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint vote = eye_order_vote(id.xy);
    if (vote != 0u) {
        InterlockedAdd(eye_order_counts[0], 1u);
        if (vote == 2u) {
            InterlockedAdd(eye_order_counts[1], 1u);
        } else if (vote == 3u) {
            InterlockedAdd(eye_order_counts[2], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index == 0u && eye_order_counts[0] != 0u) {
        uint ignored;
        EyeOrderVerdict.InterlockedAdd(0, eye_order_counts[0], ignored);
        EyeOrderVerdict.InterlockedAdd(4, eye_order_counts[1], ignored);
        EyeOrderVerdict.InterlockedAdd(8, eye_order_counts[2], ignored);
    }
}
#endif
