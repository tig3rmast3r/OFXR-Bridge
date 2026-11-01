#pragma once
#include <cmath>
#include <cstdint>

namespace xrfg {
// OSR (OFXR Stereo Reconstruction), full reconstruction: OFXR renders the peer
// eye itself from the inputs the AFW provider receives. The GPU passes are in
// src/d3d12/osr_full_warp.hlsl; these are the CPU-side rules.
//
// Persisted values stay stable: 0 AFW (legacy), 2 OSR. Retired band fill
// value 1 migrates to OSR; it is no longer a selectable reconstruction.
enum class OsrReconstruction : int { afw = 0, afw_band_fill = 1, full = 2 };
constexpr int normalize_osr_reconstruction(int value) noexcept {
    return value == static_cast<int>(OsrReconstruction::afw)
        ? static_cast<int>(OsrReconstruction::afw)
        : static_cast<int>(OsrReconstruction::full);
}

// Reversed depth is proportional to 1 / distance, so a relative tolerance on
// it is a relative tolerance on distance. A reprojected texel that finds a
// source depth within this of the expected one belongs to that surface.
constexpr float kOsrFullDepthTolerance = 0.03F;
// Longest hole run (colour pixels) the inpaint walks before it treats the run
// as open on that side. Stereo bands are tens of pixels; this bounds the cost.
constexpr std::uint32_t kOsrFullMaxRun = 256;
// Where depth texel (i, j) really sits in the eye image, in texels, beyond the
// window's rounding residual and the frame's jitter: depth texel i of the
// window is at i + 0.5 + (rounded origin - exact origin) + kOsrFullDepthAlign
// - jitter. Measured on sixteen Cyberpunk frames of 2026-11-01 (eye 4517 px,
// depth 2259 texels, window origin 3 for an exact 2.703): colour edge = depth
// edge - 1.97 px/texel of jitter + 1.31 px along x, - 1.69 px/texel - 0.05 px
// along y, i.e. 0.655 texel in x of which 0.297 is the rounding residual.
// The rest is most likely where Luke crops the eye out of the DLSS output
// (an integer pixel, where the projection says 5.4). Game and resolution
// dependent until that crop is read from the producer; the first run of
// 2026-11-01 applied the residual with the wrong sign and every silhouette
// carried a ghost strip on one side, flipping with the eye.
constexpr float kOsrFullDepthAlignX = 0.36F;
constexpr float kOsrFullDepthAlignY = 0.0F;
// A 3x3 depth-texel neighbourhood whose nearest depth exceeds its farthest by
// this ratio holds a silhouette or thin geometry; there the destination eye's
// own previous frame, motion compensated, replaces the warped source when it
// shows the same surface (osr_full_warp.hlsl EdgeRatio, kept in step).
constexpr float kOsrFullEdgeRatio = 1.10F;
// Depth texels around a previous-frame pixel searched for a nearer surface
// whose anti-aliased halo it may carry; such a pixel is not taken once that
// surface has moved away (osr_full_warp.hlsl HaloRadius, kept in step).
// 1 left the outline beside bright objects (lamp arm, F8s of 2026-11-01 13:21).
constexpr int kOsrFullHaloRadius = 2;
// A halo pixel is kept while its own depth and the nearer surface's depth,
// projected along the destination ray into the history frame, land within
// this many depth texels of each other (the edge has not moved relative to
// what lies behind it). A radius test kept a 3-5 px stale edge beside a
// bollard (F8 of 2026-11-01 13:32:41).
constexpr float kOsrFullHaloParallax = 0.5F;
// A history texel is attached to the camera (first-person weapon and hands)
// when the camera's share of its motion exceeds this many depth texels and
// its composed screen motion is below this fraction of that share; it then
// keeps its place in this eye's screen, and where the other eye's warp shows
// the same surface the warp wins, being of this very instant
// (osr_full_warp.hlsl LockedCameraTexels, LockedRatio, HistoryLockedBit).
// Strafing past a gun 30 cm away: share 160-230 texels, composed motion 3-29,
// which is composition residue at that depth, not the gun's own motion
// (Cyberpunk F8s of 2026-11-01 13:50).
// With 4 texels and 0.25, whenever a strafe slowed, started, stopped or
// reversed, the gun dropped out of the band for a frame and was reprojected as
// world geometry again (an occasional split-second jitter). World geometry sits
// at a ratio near 1, so 0.5 still separates it: on the strafing F8s of 13:50
// the gun and hands stay 100% flagged and world texels 0.02-0.13% of the image.
constexpr float kOsrFullLockedCameraTexels = 1.0F;
constexpr float kOsrFullLockedRatio = 0.5F;
// The camera's share only shows a camera-attached surface when the camera
// moves across it. Walking forward or back it grows with the distance from the
// view centre, so the gun aimed at the centre has almost none, was reprojected
// as world geometry with its composition residue and went transparent and
// flickered (Cyberpunk, user headset check 2026-11-01). Anything
// nearer than this is the player's own weapon, hands or cockpit and is
// camera-attached whatever the motion: on the Cyberpunk F8 of 18:53:57, 18.2%
// of depth texels lie within 0.5 m and still 18.2% within 0.75 m, the world
// starting beyond. A near wall taken for attached loses nothing where the
// other eye's warp shows it, which then wins as for the gun.
constexpr float kOsrFullLockedDistanceMetres = 0.6F;

// Reversed depth of a point straight ahead at view distance `distance` (game
// units) through a row-vector view_to_clip; 2 (never reached) when unknown.
inline float osr_full_depth_at(const float* view_to_clip, float distance) noexcept {
    float depth = 2.0F;
    if (view_to_clip != nullptr && std::isfinite(distance) && distance > 0.0F) {
        constexpr float signs[2]{1.0F, -1.0F};
        for (const float sign : signs) {
            const float z = sign * distance * view_to_clip[10] + view_to_clip[14];
            const float w = sign * distance * view_to_clip[11] + view_to_clip[15];
            if (w > 0.0F && std::isfinite(z / w) && z / w > 0.0F && z / w <= 1.0F) { depth = z / w; break; }
        }
    }
    return depth;
}
// The destination eye's last real frame may be this many publications old and
// still fill what the source eye cannot see. Luke alternates eyes, so 1 is
// the normal distance; a skipped frame makes it 3.
constexpr std::uint64_t kOsrFullMaxHistoryAge = 4;

// Whether the destination eye's own previous frame joins the reconstruction.
// effective_mode is the provider mode requested this frame after the priming
// rules (1 alternate eye only, 2 previous frame, 3 combined): the alternate-eye
// prime stays a pure stereo warp so both reconstructions behave alike. The
// history must be the frame the layer last accepted for that eye
// (history_publication == previous_source_publication), older than this
// publication and recent. Publications count across both eyes and the layer
// zeroes its per-eye record at every producer reset and origin change, so that
// continuity is the whole contract: stream identity is not compared, because a
// title may run one DLSS stream per eye (Cyberpunk 2077 does), and the first
// headset run of 2026-11-01 never used history for that reason.
inline bool osr_full_history_usable(int effective_mode, std::uint64_t publication,
    std::uint64_t history_publication, std::uint64_t previous_source_publication,
    std::uint64_t max_age = kOsrFullMaxHistoryAge) noexcept {
    if (effective_mode != 2 && effective_mode != 3) return false;
    if (!history_publication || history_publication != previous_source_publication) return false;
    if (history_publication >= publication) return false;
    return publication - history_publication <= max_age;
}

// Flags handed to the shaders (OsrFullConstants::flags).
constexpr std::uint32_t kOsrFullFlagDebug = 1u;
constexpr std::uint32_t kOsrFullFlagHistory = 2u;
// The previous frame of this eye is the primary source everywhere it shows the
// same surface, not only at silhouettes, bands and sky (osr_full_warp.hlsl).
constexpr std::uint32_t kOsrFullFlagHistoryFirst = 4u;

inline std::uint32_t osr_full_flags(bool debug, bool history, bool history_first = true) noexcept {
    return (debug ? kOsrFullFlagDebug : 0u) | (history ? kOsrFullFlagHistory : 0u) |
        (history && history_first ? kOsrFullFlagHistoryFirst : 0u);
}
}  // namespace xrfg
