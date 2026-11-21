#pragma once

#include "xrfg/luke_interleaved_history.hpp"

namespace xrfg {

// A producer must identify all three camera observations on its native
// render clock and confirm the coordinate and depth contract of the field.
[[nodiscard]] inline bool motion_interval_camera_ready(const DlssMotionVectorFrame& frame) noexcept {
    if (!frame.raw_motion_variable_interval || !frame.motion_interval_native_view_rows ||
        frame.raw_camera_motion_included!=1 || !frame.luke_interleaved_history ||
        !frame.source_clock_owner || !frame.depth_inverted || !frame.depth ||
        !frame.previous_opposite_guide || !frame.previous_same_guide ||
        frame.depth_width!=frame.motion_width || frame.depth_height!=frame.motion_height ||
        !frame.depth_allocation || frame.depth_allocation->resource.Get()!=frame.depth.Get() ||
        !(frame.depth_resource_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        return false;
    return luke_interleaved_history_matches(dlss_native_camera_history(frame),
        dlss_native_camera_history(*frame.previous_opposite_guide),
        dlss_native_camera_history(*frame.previous_same_guide));
}

} // namespace xrfg
