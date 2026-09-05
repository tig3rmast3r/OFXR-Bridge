Texture2DArray<float4> PreviousFrame : register(t0);
Texture2DArray<float4> CurrentFrame : register(t1);
Texture2D<int2> BackwardFlow : register(t2);
Texture2D<uint> FlowAuxiliary : register(t3);
Texture2D<int2> ForwardFlow : register(t4);
Texture2D<uint> ForwardAuxiliary : register(t5);
RWTexture2D<float4> PackedColor : register(u0);
RWTexture2D<float4> NvidiaPreviousColor : register(u1);
RWTexture2D<float4> NvidiaCurrentColor : register(u2);

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
    uint Slice;
    uint RepeatedCapture;
    uint ViewIndex;
    CameraMapping PreviousMappings[2];
};

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

float2 nvidia_flow_input_scale() {
    return min(
        float2(1.0, 1.0),
        float2(float(PackedWidth), float(PackedHeight)) /
            max(float2(float(Width), float(Height)), float2(1.0, 1.0)));
}

float2 nvidia_flow_input_coordinate(float2 full_resolution_coordinate) {
    return (full_resolution_coordinate + 0.5) *
        nvidia_flow_input_scale() - 0.5;
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

[numthreads(8, 8, 1)]
void PackFlowInput(uint3 thread_id : SV_DispatchThreadID) {
    if (thread_id.x >= PackedWidth || thread_id.y >= PackedHeight) {
        return;
    }

    float4 current_color = float4(0.0, 0.0, 0.0, 1.0);
    uint slice = thread_id.y / PackedEyeStride;
    uint local_y = thread_id.y - slice * PackedEyeStride;
    if (thread_id.x < Width && slice < ArraySize && local_y < Height) {
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.x, local_y), int(slice), 0));
    }
    PackedColor[thread_id.xy] = saturate(current_color);
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
    if (slice < ArraySize && downscaled) {
        float2 source_coordinate =
            (float2(thread_id.xy) + 0.5) / nvidia_flow_input_scale() - 0.5;
        previous_color = bilinear_previous_source(source_coordinate, slice);
        current_color = bilinear_current_source(source_coordinate, slice);
    } else if (thread_id.x < Width && slice < ArraySize &&
               thread_id.y < Height) {
        previous_color = PreviousFrame.Load(
            int4(int2(thread_id.xy), int(slice), 0));
        current_color = CurrentFrame.Load(
            int4(int2(thread_id.xy), int(slice), 0));
    }
    NvidiaPreviousColor[thread_id.xy] = saturate(previous_color);
    NvidiaCurrentColor[thread_id.xy] = saturate(current_color);
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
    uint slice,
    bool use_nvidia_input_scale) {
    uint2 image_minimum = uint2(image_rect.xy);
    uint2 image_maximum = image_minimum + uint2(image_rect.zw) - 1U;
    if (use_nvidia_input_scale) {
        float2 input_scale = nvidia_flow_input_scale();
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
    float2 packed_coordinate = float2(
        pixel.x,
        pixel.y + float(slice * PackedEyeStride));
    if (use_nvidia_input_scale) {
        packed_coordinate = nvidia_flow_input_coordinate(pixel);
    }
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice,
        use_nvidia_input_scale);
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
    if (use_nvidia_input_scale) {
        result /= max(nvidia_flow_input_scale(), float2(1.0e-6, 1.0e-6));
    }
    float magnitude = length(result);
    return magnitude > 512.0 ? result * (512.0 / magnitude) : result;
}

float2 forward_flow_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index,
    float value_scale) {
    float2 input_scale = nvidia_flow_input_scale();
    float2 packed_coordinate = nvidia_flow_input_coordinate(pixel);
    float2 flow_coordinate =
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5;
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].SourceRect,
        slice,
        true);
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

float nvidia_cost_for_pixel(float2 pixel, uint slice, uint view_index) {
    float2 packed_coordinate = nvidia_flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].TargetRect,
        slice,
        true);
    return float(FlowAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

float nvidia_forward_cost_for_pixel(
    float2 pixel,
    uint slice,
    uint view_index) {
    float2 packed_coordinate = nvidia_flow_input_coordinate(pixel);
    int2 coordinate = int2(round(
        (packed_coordinate + 0.5) / float(FlowBlockSize) - 0.5));
    FlowGridBounds bounds = flow_grid_bounds(
        PreviousMappings[view_index].SourceRect,
        slice,
        true);
    return float(ForwardAuxiliary.Load(int3(clamp(
        coordinate,
        bounds.minimum,
        bounds.maximum), 0)) & 0xffU);
}

bool coordinate_inside_rect(float2 coordinate, float4 rect) {
    float2 minimum = rect.xy - 0.5;
    float2 maximum = rect.xy + rect.zw - 0.5;
    return coordinate.x >= minimum.x && coordinate.y >= minimum.y &&
        coordinate.x <= maximum.x && coordinate.y <= maximum.y;
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

float4 synthesize_midpoint(
    FullscreenVertex input,
    float flow_value_scale,
    bool use_nvidia_cost,
    bool use_nvidia_bidirectional) {
    float4 output_color = float4(0.0, 0.0, 0.0, 1.0);
    uint2 integer_pixel = uint2(input.position.xy);
    bool in_bounds = integer_pixel.x < Width && integer_pixel.y < Height &&
        Slice < ArraySize;
    if (in_bounds) {
        float2 pixel = float2(integer_pixel);
        CameraSample current_fallback = sample_current_target(
            pixel,
            Slice,
            ViewIndex);
        output_color = saturate(current_fallback.color);
        MappedCoordinate previous_coverage = map_target_to_source(
            pixel,
            PreviousMappings[ViewIndex]);
        bool scene_changed = RepeatedCapture == 0 && !use_nvidia_cost &&
            (FlowAuxiliary.Load(int3(1, 0, 0)) & 0x0fU) != 0;
        if (previous_coverage.valid >= 0.5 && !scene_changed) {
            // The raw B-to-A flow includes head rotation. The OpenXR mapping
            // supplies that camera component, leaving residual scene motion.
            float2 raw_backward = RepeatedCapture != 0
                ? float2(0.0, 0.0)
                : flow_for_pixel(
                    pixel,
                    Slice,
                    ViewIndex,
                    flow_value_scale,
                    use_nvidia_cost);
            float2 pose_backward = previous_coverage.coordinate - pixel;
            float2 residual_backward = raw_backward - pose_backward;
            CameraSample previous_sample = sample_previous_target(
                pixel + residual_backward * 0.5,
                Slice,
                ViewIndex);
            CameraSample current_sample = sample_current_target(
                pixel - residual_backward * 0.5,
                Slice,
                ViewIndex);
            if (previous_sample.valid >= 0.5 && current_sample.valid >= 0.5) {
                float4 flow_midpoint = 0.5 * (
                    previous_sample.color + current_sample.color);
                float disagreement = max(
                    abs(previous_sample.color.r - current_sample.color.r),
                    max(
                        abs(previous_sample.color.g - current_sample.color.g),
                        abs(previous_sample.color.b - current_sample.color.b)));
                float confidence = saturate(1.0 - disagreement * 6.0);
                float4 stable_midpoint = 0.5 * (
                    sample_previous_target(pixel, Slice, ViewIndex).color +
                    current_fallback.color);
                if (use_nvidia_cost) {
                    float consistency_confidence = 1.0;
                    float cost_confidence = 1.0;
                    if (RepeatedCapture == 0) {
                        if (use_nvidia_bidirectional) {
                            float2 previous_coordinate = pixel + raw_backward;
                            if (coordinate_inside_rect(
                                    previous_coordinate,
                                    PreviousMappings[ViewIndex].SourceRect)) {
                                float2 raw_forward = forward_flow_for_pixel(
                                    previous_coordinate,
                                    Slice,
                                    ViewIndex,
                                    flow_value_scale);
                                float cycle_error = length(
                                    raw_backward + raw_forward);
                                consistency_confidence =
                                    1.0 - smoothstep(2.0, 6.0, cycle_error);
                                float maximum_cost = max(
                                    nvidia_cost_for_pixel(
                                        pixel,
                                        Slice,
                                        ViewIndex),
                                    nvidia_forward_cost_for_pixel(
                                        previous_coordinate,
                                        Slice,
                                        ViewIndex));
                                cost_confidence = saturate(
                                    1.0 - maximum_cost / 255.0);
                            } else {
                                consistency_confidence = 0.0;
                                cost_confidence = 0.0;
                            }
                        } else {
                            cost_confidence = saturate(
                                1.0 - nvidia_cost_for_pixel(
                                    pixel,
                                    Slice,
                                    ViewIndex) / 255.0);
                        }
                    }
                    confidence *= consistency_confidence * cost_confidence;
                    output_color = saturate(lerp(
                        stable_midpoint,
                        flow_midpoint,
                        confidence));
                } else {
                    output_color = saturate(lerp(
                        stable_midpoint,
                        flow_midpoint,
                        confidence));
                }
            }
        }
    }
    return output_color;
}

float4 SynthesizeMidpointPS(FullscreenVertex input) : SV_Target {
    return synthesize_midpoint(input, 1.0, false, false);
}

float4 SynthesizeNvidiaMidpointPS(FullscreenVertex input) : SV_Target {
    // NVIDIA OFA stores flow in signed S10.5 fixed point.
    return synthesize_midpoint(input, 1.0 / 32.0, true, false);
}

float4 SynthesizeNvidiaBidirectionalMidpointPS(
    FullscreenVertex input) : SV_Target {
    // NVIDIA OFA stores flow in signed S10.5 fixed point.
    return synthesize_midpoint(input, 1.0 / 32.0, true, true);
}
