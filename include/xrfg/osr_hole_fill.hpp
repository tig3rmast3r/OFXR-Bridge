#pragma once
#include <algorithm>
#include <cmath>

namespace xrfg {
// OSR (OFXR Stereo Reconstruction): CPU-side decisions for the stereo hole
// fill. See src/d3d12/osr_hole_fill.hlsl for the GPU passes.
//
// A stereo hole is destination-eye background that the source eye hides behind
// a nearer object. The provider fills it from the destination eye's previous
// frame when that frame saw it. Viewpoints along the source->destination axis
// see the band in proportion to how far along the axis they sit: the previous
// frame at parameter t covers the outer t of each run and leaves the inner
// (1 - t), next to the occluder, unseen. Behind the source (t < 0) it sees
// nothing of the band; beyond the destination (t > 1) it sees all of it.
struct OsrPoint { float x{}, y{}, z{}; };

inline float osr_history_parameter(const OsrPoint& source,
    const OsrPoint& destination, const OsrPoint& previous_destination) noexcept {
    const float dx = destination.x - source.x, dy = destination.y - source.y,
        dz = destination.z - source.z;
    const float length2 = dx * dx + dy * dy + dz * dz;
    if (!(length2 > 1e-12F) || !std::isfinite(length2)) return 1.0F; // no baseline: nothing to fill
    const float px = previous_destination.x - source.x,
        py = previous_destination.y - source.y, pz = previous_destination.z - source.z;
    return (px * dx + py * dy + pz * dz) / length2;
}

// Fraction of each hole run, measured from the occluder edge, that OSR fills.
// effective_mode is the provider mode actually requested this frame
// (1 alternate eye only, 2 previous frame only, 3 combined).
//
// Cyberpunk F8s of 2026-10-29 showed the provider leaves its smear over the
// whole band whenever the eye leads (t < 1), not only over the inner (1 - t):
// with the fraction at 0.62 the outer part kept dark pixels at positions
// 0.7-0.9 of the run. So the band is filled whole as soon as the eye leads,
// with a short ramp below t = 1 so standing still with head jitter does not
// flicker between the provider's clean history and the mirror.
// Standing still, t sits near 1 and head jitter would flip the fill on and off
// at the object's edge every frame, so the leading decision has hysteresis:
// it starts below kOsrLeadStart and stops above kOsrLeadStop.
constexpr float kOsrLeadStart = 0.90F;
constexpr float kOsrLeadStop = 0.98F;

inline bool osr_update_leading(bool leading, float history_parameter) noexcept {
    if (!std::isfinite(history_parameter)) return leading;
    return leading ? history_parameter < kOsrLeadStop : history_parameter < kOsrLeadStart;
}

// fill_gain scales (1 - t): 1 is the pure history model (only the part no
// viewpoint saw), larger values fill the whole band at ever slower strafes.
inline float osr_fill_fraction(int effective_mode, bool have_history,
    float history_parameter, bool leading, float fill_gain) noexcept {
    if (effective_mode == 1 || !have_history) return 1.0F; // stereo warp alone: the whole band is unseen
    if (effective_mode != 3) return 0.0F;                  // previous-frame only: no stereo band
    if (!std::isfinite(history_parameter)) return 1.0F;
    if (!leading) return 0.0F;                             // trailing eye or still: the provider's history is clean
    return std::clamp((1.0F - history_parameter) * std::max(fill_gain, 1.0F), 0.0F, 1.0F);
}
}  // namespace xrfg
