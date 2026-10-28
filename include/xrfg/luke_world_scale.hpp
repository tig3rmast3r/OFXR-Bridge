#pragma once

#include <array>
#include <cmath>

namespace xrfg {

// Both eye positions must come from one xrLocateViews result. Mixing retained
// AER poses from different times folds headset displacement into the IPD.
inline bool luke_xr_eye_baseline(const std::array<float, 3>& left,
    const std::array<float, 3>& right, float& result) noexcept {
    double squared_distance = 0.0;
    for (std::size_t axis = 0; axis < left.size(); ++axis) {
        if (!std::isfinite(left[axis]) || !std::isfinite(right[axis]))
            return false;
        const double delta = static_cast<double>(right[axis]) - left[axis];
        squared_distance += delta * delta;
    }
    const double distance = std::sqrt(squared_distance);
    if (!std::isfinite(distance) || distance < 0.01F || distance > 0.20F)
        return false;
    result = static_cast<float>(distance);
    return true;
}

// WorldScale and physical IPD alone do not define producer camera units.
// A calibrated units/metre factor is stable; WorldScale is applied once.
inline bool luke_game_eye_baseline(float xr_baseline, float world_scale,
    float units_per_metre, float& game_baseline) noexcept {
    if (!std::isfinite(xr_baseline) || !std::isfinite(world_scale) ||
        !std::isfinite(units_per_metre) || units_per_metre <= 0 ||
        xr_baseline < 0.01F || xr_baseline > 0.20F ||
        world_scale < 0.2F || world_scale > 5.0F) return false;
    game_baseline = float(double(xr_baseline)/world_scale*units_per_metre);
    return std::isfinite(game_baseline) && game_baseline > 0;
}

// Which way the producer's camera_right points between the eyes, measured
// from two consecutive AER cameras: +1 when the right eye's camera lies along
// +camera_right from the left eye's, -1 the other way, 0 when the separation
// along it is not one eye baseline (head motion, a non-alternating camera).
// The projection's x sign does not decide it: The Last of Us Part I mirrors
// x (view_to_clip[0] < 0) while its camera_right points to the right eye.
inline float luke_measured_right_axis(unsigned current_eye,
    const std::array<float, 3>& current, const std::array<float, 3>& other,
    const std::array<float, 3>& right, float game_baseline) noexcept {
    if (current_eye > 1 || !std::isfinite(game_baseline) || game_baseline <= 0) return 0;
    double along = 0, norm = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(current[axis]) || !std::isfinite(other[axis]) ||
            !std::isfinite(right[axis])) return 0;
        along += (double(other[axis]) - current[axis]) * right[axis];
        norm += double(right[axis]) * right[axis];
    }
    if (norm < .99 || norm > 1.01) return 0;
    const double ratio = std::abs(along) / game_baseline;
    if (ratio < .5 || ratio > 1.5) return 0;
    // The other camera is the right eye's when the current one is the left's.
    const float toward_other = along > 0 ? 1.F : -1.F;
    return current_eye == 0 ? toward_other : -toward_other;
}
// Adopts a measured direction after three agreeing frames; 0 until then.
// Then keeps it: the direction is the engine's, and REAL VR's occasional
// "eyes swapped" frames (The Last of Us Part I: a run of eleven at 42 s)
// would otherwise flip it for as long as they last.
class LukeRightAxisSign {
    float sign_{}, candidate_{};
    unsigned agree_{};
public:
    float observe(float measured) noexcept {
        if (measured == 0 || sign_ != 0) return sign_;
        if (measured == candidate_) ++agree_;
        else { candidate_ = measured; agree_ = 1; }
        if (agree_ >= 3) sign_ = candidate_;
        return sign_;
    }
    float sign() const noexcept { return sign_; }
};
}  // namespace xrfg
