#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>

namespace xrfg {
// OSR for stock UEVR's Alternating/AFR rendering method. UEVR renders one eye
// per game frame and ends one OpenXR frame per pair of game frames. OFXR
// splits each pair: when the first eye is released it reconstructs the other
// eye from it and ends an extra frame, and when the second eye is released it
// reconstructs the first eye from it before UEVR's own xrEndFrame. Every game
// frame then reaches the headset with one real and one reconstructed eye, as
// with Luke Ross's alternate-eye rendering. The GPU passes are Full OSR's
// (src/d3d12/osr_full_warp.hlsl); these are the CPU-side rules.
//
// Conventions: UE view space (x right, y up, z forward), row vectors
// (v' = v * M), reversed infinite depth (depth = near / z), as UE renders.

using UevrMatrix = std::array<float, 16>;

// UEVR's VR_RenderingMethod value for Alternating/AFR.
constexpr int kUevrRenderingMethodAlternating = 2;
// UEVR's VR_SynchronizationMode value for Very Late, its default.
constexpr int kUevrSynchronizationVeryLate = 2;

inline UevrMatrix uevr_identity() noexcept {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

inline UevrMatrix uevr_multiply(const UevrMatrix& a, const UevrMatrix& b) noexcept {
    UevrMatrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    return r;
}

// Rigid transform inverse (rotation rows 0-2, translation row 3).
inline UevrMatrix uevr_rigid_inverse(const UevrMatrix& m) noexcept {
    UevrMatrix r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 4 + j] = m[j * 4 + i];
    for (int j = 0; j < 3; ++j)
        r[12 + j] = -(m[12] * r[0 * 4 + j] + m[13] * r[1 * 4 + j] + m[14] * r[2 * 4 + j]);
    r[15] = 1;
    return r;
}

// General 4x4 inverse; false when singular.
inline bool uevr_invert(const UevrMatrix& m, UevrMatrix& out) noexcept {
    double a[4][8]{};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) a[i][j] = m[i * 4 + j];
        a[i][4 + i] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
        if (std::abs(a[pivot][c]) < 1e-12) return false;
        if (pivot != c) for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f != 0) for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<float>(a[i][4 + j]);
    return true;
}

// Yaw of an OpenXR orientation (x, y, z, w; y up), degrees.
inline float uevr_xr_yaw_degrees(const std::array<float, 4>& q) noexcept {
    return std::atan2(2.0F * (q[3] * q[1] + q[0] * q[2]), 1.0F - 2.0F * (q[1] * q[1] + q[2] * q[2])) * 57.29578F;
}
inline double uevr_wrap_degrees(double a) noexcept { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }

// The share of the engine camera's yaw step over a pair that a stick turned,
// degrees of UE yaw. The engine camera can also carry the head's own turn:
// in Atomic Heart (aim from a controller, the pawn's control rotation) the
// yaw UEVR adds stays put while the engine yaw follows the head, so 99% of
// its steps were the head's (r39 log), already in the frame's head pose.
// head_step: the HMD's yaw change over the pair; added_step: the change of
// the yaw UEVR added to the view. Their difference is the head turn the
// engine carried. A step mostly carried is the head's, one mostly not is a
// stick's (Dead Island 2), whole; a mixed one keeps what the head leaves.
inline double uevr_stick_yaw_step(double engine_step, double head_step, double added_step) noexcept {
    if (engine_step == 0) return 0;
    const double carried = uevr_wrap_degrees(head_step - added_step);
    const double magnitude = std::abs(engine_step);
    if (std::abs(carried) < 0.25 * magnitude) return engine_step;
    const double stick = uevr_wrap_degrees(engine_step - carried);
    return std::abs(stick) < 0.25 * magnitude ? 0 : stick;
}

// A camera turned about the world's up axis by degrees of UE yaw (X toward
// Y), in place: its axes rotate, its position stays.
inline UevrMatrix uevr_yaw_about_up(const UevrMatrix& m, double degrees) noexcept {
    const double r = degrees * 3.14159265358979323846 / 180.0;
    const float c = static_cast<float>(std::cos(r)), s = static_cast<float>(std::sin(r));
    UevrMatrix out = m;
    for (int row = 0; row < 3; ++row) {
        const float x = m[row * 4], y = m[row * 4 + 1];
        out[row * 4] = x * c - y * s;
        out[row * 4 + 1] = x * s + y * c;
    }
    return out;
}

// The world turn that takes the axes of camera a onto those of camera b
// (row vectors: a's axes x turn = b's axes); positions play no part.
using UevrTurn = std::array<float, 9>;
inline UevrTurn uevr_turn_between(const UevrMatrix& a, const UevrMatrix& b) noexcept {
    UevrTurn r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[0 * 4 + i] * b[0 * 4 + j] + a[1 * 4 + i] * b[1 * 4 + j] + a[2 * 4 + i] * b[2 * 4 + j];
    return r;
}
// A camera's axes turned in world by turn, in place: its position stays.
inline UevrMatrix uevr_turn_in_place(const UevrMatrix& m, const UevrTurn& turn) noexcept {
    UevrMatrix out = m;
    for (int row = 0; row < 3; ++row)
        for (int j = 0; j < 3; ++j)
            out[row * 4 + j] = m[row * 4] * turn[j] + m[row * 4 + 1] * turn[3 + j] + m[row * 4 + 2] * turn[6 + j];
    return out;
}
// The turn's whole angle and its yaw about the world's up axis (UE yaw), degrees.
inline double uevr_turn_degrees(const UevrTurn& turn) noexcept {
    const double c = std::clamp((static_cast<double>(turn[0]) + turn[4] + turn[8] - 1.0) * 0.5, -1.0, 1.0);
    return std::acos(c) * 57.29577951308232;
}
inline double uevr_turn_yaw_degrees(const UevrTurn& turn) noexcept {
    return std::atan2(static_cast<double>(turn[1]), static_cast<double>(turn[0])) * 57.29577951308232;
}

// An OpenXR pose (stage space, metres).
struct UevrXrPose {
    std::array<float, 4> orientation{0, 0, 0, 1}; // x, y, z, w
    std::array<float, 3> position{};
};

// The view of one rendered eye as UEVR hands it to the engine (its
// post-CalculateStereoViewOffset location and rotation, in world units and
// degrees), with the location UEVR started from. UE5 worlds use doubles.
struct UevrEyeCamera {
    std::array<double, 3> location{};
    std::array<double, 3> rotation{}; // pitch, yaw, roll in degrees (FRotator)
    std::array<double, 3> base_location{};
    std::array<double, 3> base_rotation{}; // the engine's rotation before UEVR added the HMD's
    float world_to_meters{};
    std::uint64_t sequence{};
    std::uint64_t ticks{};
    // The eyes' poses UEVR had just located when it computed this view, i.e.
    // the pose this eye was rendered with, and their display time.
    std::array<UevrXrPose, 2> xr_eyes{};
    std::int64_t xr_time{};
    bool xr_valid{};
};

// FRotationMatrix axes: X forward, Y right, Z up, in world space.
inline void uevr_rotator_axes(const std::array<double, 3>& rotation,
    std::array<double, 3>& forward, std::array<double, 3>& right, std::array<double, 3>& up) noexcept {
    constexpr double rad = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(rotation[0] * rad), cp = std::cos(rotation[0] * rad);
    const double sy = std::sin(rotation[1] * rad), cy = std::cos(rotation[1] * rad);
    const double sr = std::sin(rotation[2] * rad), cr = std::cos(rotation[2] * rad);
    forward = {cp * cy, cp * sy, sp};
    right = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    up = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
}

// View-to-world of a UEVR eye, positions relative to `origin` so that large
// UE5 coordinates keep float precision.
inline UevrMatrix uevr_view_to_world(const UevrEyeCamera& camera,
    const std::array<double, 3>& origin) noexcept {
    std::array<double, 3> f{}, r{}, u{};
    uevr_rotator_axes(camera.rotation, f, r, u);
    UevrMatrix m{};
    for (int i = 0; i < 3; ++i) {
        m[0 + i] = static_cast<float>(r[i]);
        m[4 + i] = static_cast<float>(u[i]);
        m[8 + i] = static_cast<float>(f[i]);
        m[12 + i] = static_cast<float>(camera.location[i] - origin[i]);
    }
    m[15] = 1;
    return m;
}

// The game's own camera, before UEVR adds the head: what a first-person
// weapon hangs on (Dead Island 2: it stays where the body faces while the
// head turns).
inline UevrMatrix uevr_engine_to_world(const UevrEyeCamera& camera,
    const std::array<double, 3>& origin) noexcept {
    UevrEyeCamera engine = camera;
    engine.location = camera.base_location;
    engine.rotation = camera.base_rotation;
    return uevr_view_to_world(engine, origin);
}

// Reversed infinite projection of a field given by the tangents of its edges.
inline UevrMatrix uevr_projection_tangents(float l, float r, float u, float d, float near_plane) noexcept {
    UevrMatrix m{};
    m[0] = 2 / (r - l);
    m[8] = -(r + l) / (r - l);
    m[5] = 2 / (u - d);
    m[9] = -(u + d) / (u - d);
    m[11] = 1;
    m[14] = near_plane;
    return m;
}

// Reversed infinite projection of an OpenXR field of view (radians).
inline UevrMatrix uevr_projection(float angle_left, float angle_right,
    float angle_up, float angle_down, float near_plane) noexcept {
    return uevr_projection_tangents(std::tan(angle_left), std::tan(angle_right),
        std::tan(angle_up), std::tan(angle_down), near_plane);
}

// UEVR submits each AFR eye as a rectangle of its image (its view bounds crop
// the asymmetric field), while the game rendered, and DLSS reconstructed, the
// whole image. The submitted field belongs to the rectangle; the whole image's
// field extends it at the same tangent per pixel (rows run top to bottom).
struct UevrTangents { float left{}, right{}, up{}, down{}; };
inline UevrTangents uevr_image_tangents(float angle_left, float angle_right, float angle_up,
    float angle_down, int offset_x, int offset_y, int width, int height,
    std::uint32_t image_width, std::uint32_t image_height) noexcept {
    const float l = std::tan(angle_left), r = std::tan(angle_right);
    const float u = std::tan(angle_up), d = std::tan(angle_down);
    if (width <= 0 || height <= 0) return {l, r, u, d};
    const float sx = (r - l) / static_cast<float>(width);
    const float sy = (u - d) / static_cast<float>(height);
    UevrTangents t{};
    t.left = l - static_cast<float>(offset_x) * sx;
    t.right = t.left + static_cast<float>(image_width) * sx;
    t.up = u + static_cast<float>(offset_y) * sy;
    t.down = t.up - static_cast<float>(image_height) * sy;
    return t;
}

inline std::array<float, 9> uevr_quaternion_matrix(std::array<float, 4> q) noexcept {
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0) for (auto& v : q) v /= n;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    // Column-vector rotation R (v' = R v), row-major storage.
    return {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
            2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
            2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)};
}

// Maps destination-eye view coordinates to source-eye view coordinates (UE
// axes, row vectors), from the two eyes' OpenXR poses of one instant.
// `units_per_metre` turns OpenXR metres into world units.
inline UevrMatrix uevr_eye_to_eye(const UevrXrPose& source, const UevrXrPose& destination,
    float units_per_metre) noexcept {
    const auto rs = uevr_quaternion_matrix(source.orientation);
    const auto rd = uevr_quaternion_matrix(destination.orientation);
    // OpenXR view coordinates: R_s^T R_d (column vectors) takes destination to source.
    std::array<float, 9> rel{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += rs[k * 3 + i] * rd[k * 3 + j];
            rel[i * 3 + j] = s;
        }
    std::array<float, 3> delta{destination.position[0] - source.position[0],
        destination.position[1] - source.position[1], destination.position[2] - source.position[2]};
    std::array<float, 3> t{};
    for (int i = 0; i < 3; ++i)
        t[i] = rs[0 * 3 + i] * delta[0] + rs[1 * 3 + i] * delta[1] + rs[2 * 3 + i] * delta[2];
    // UE view axes flip OpenXR's z (forward instead of back): F rel F, F t.
    constexpr float flip[3]{1, 1, -1};
    UevrMatrix m{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m[j * 4 + i] = flip[i] * rel[i * 3 + j] * flip[j]; // row-vector: transpose
    for (int i = 0; i < 3; ++i) m[12 + i] = flip[i] * t[i] * units_per_metre;
    m[15] = 1;
    return m;
}

// UEVR reports each eye's view without saying which eye it is, and in AFR the
// engine always asks for the same view index. The eye is told from what UEVR
// added to the engine's own location: the head offset is common to both eyes
// of a pair, the eye separation is not, so the change of that addition from
// the previous view, along this view's right axis, is +IPD going left to right
// and -IPD going right to left, whatever the player's own motion (the engine
// location moves with the player and is subtracted out). Each view is decided
// on its own: a parity vote over the sequence locked onto the wrong eye after
// a missed or doubled view (Dead Island 2, 2026-11-02 18:24) and stayed there.
class UevrEyePhase {
public:
    // The eye of this view (0 left, 1 right), when the change is clear.
    std::optional<unsigned> observe(const UevrEyeCamera& camera, float min_separation_units) noexcept {
        std::array<double, 3> f{}, r{}, u{};
        uevr_rotator_axes(camera.rotation, f, r, u);
        const std::array<double, 3> added{camera.location[0] - camera.base_location[0],
            camera.location[1] - camera.base_location[1], camera.location[2] - camera.base_location[2]};
        std::optional<unsigned> eye;
        last_separation_ = 0;
        if (previous_valid_) {
            double s = 0;
            for (int i = 0; i < 3; ++i) s += (added[i] - previous_added_[i]) * r[i];
            last_separation_ = std::isfinite(s) ? s : 0;
            if (std::isfinite(s) && std::abs(s) >= min_separation_units) {
                // Physical: the viewpoint further to the right is the right eye,
                // whichever swapchain UEVR copies its image into. In Dead Island 2
                // under PureDark's fork the first frame of each pair, which UEVR
                // puts in the left eye's swapchain, is the right viewpoint (its
                // location is +IPD along right of the previous view, and the real
                // images show the left swapchain's content to the LEFT of the
                // right swapchain's, by a disparity that grows with nearness).
                eye = s > 0 ? 1u : 0u;
                ++decided_[*eye];
            }
        }
        previous_added_ = added;
        previous_valid_ = true;
        return eye;
    }
    void reset() noexcept { previous_valid_ = false; decided_ = {}; last_separation_ = 0; }
    // Views decided left and right so far, and the last separation (world units).
    [[nodiscard]] std::array<std::uint64_t, 2> decided() const noexcept { return decided_; }
    [[nodiscard]] double last_separation() const noexcept { return last_separation_; }

private:
    std::array<double, 3> previous_added_{};
    bool previous_valid_{};
    std::array<std::uint64_t, 2> decided_{};
    double last_separation_{};
};

// Views waiting for their rendered frame. UE's game thread runs ahead of the
// render thread that releases the eye images, one view per rendered frame, so
// the views and the releases stay in lockstep: a release takes the oldest view
// still young enough to be its frame's. Every view is queued, decided eye or
// not (eye 2), to keep that order. Taking by eye label instead threw the
// right view away whenever the label disagreed with the swapchain (Dead
// Island 2, 2026-11-02 18:24).
class UevrEyeCameraQueue {
public:
    struct Taken { UevrEyeCamera camera; unsigned eye; };
    void push(const UevrEyeCamera& camera, unsigned eye) {
        entries_.push_back({camera, eye});
        while (entries_.size() > kCapacity) entries_.pop_front();
    }
    // The oldest view of eye still young enough to be its frame's; views
    // queued before it are dropped, newer ones kept. Ordered matching alone
    // (take_oldest) stays one view off forever once a view is lost, and every
    // reconstruction then used the other eye's camera, mirrored (Dead Island
    // 2, 2026-11-02 22:05, after a hitch at 48 s). Undecided views are skipped.
    std::optional<Taken> take_eye(unsigned eye, std::uint64_t now_ticks, std::uint64_t max_age_ticks) {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const auto& entry = entries_[i];
            if (entry.eye != eye) continue;
            if (now_ticks >= entry.camera.ticks && now_ticks - entry.camera.ticks > max_age_ticks) continue;
            const Taken taken{entry.camera, entry.eye};
            entries_.erase(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(i) + 1);
            return taken;
        }
        // None: drop only what has aged out, keep the newer views for their own releases.
        while (!entries_.empty() && now_ticks >= entries_.front().camera.ticks &&
            now_ticks - entries_.front().camera.ticks > max_age_ticks) entries_.pop_front();
        return std::nullopt;
    }
    std::optional<Taken> take_oldest(std::uint64_t now_ticks, std::uint64_t max_age_ticks) {
        while (!entries_.empty()) {
            const auto entry = entries_.front();
            entries_.pop_front();
            if (now_ticks >= entry.camera.ticks && now_ticks - entry.camera.ticks > max_age_ticks) continue;
            return Taken{entry.camera, entry.eye};
        }
        return std::nullopt;
    }
    std::optional<UevrEyeCamera> take(unsigned eye, std::uint64_t now_ticks, std::uint64_t max_age_ticks) {
        while (!entries_.empty()) {
            const auto entry = entries_.front();
            entries_.pop_front();
            if (entry.eye != eye) continue;
            if (now_ticks >= entry.camera.ticks && now_ticks - entry.camera.ticks > max_age_ticks) continue;
            return entry.camera;
        }
        return std::nullopt;
    }
    [[nodiscard]] const UevrEyeCamera* front() const noexcept { return entries_.empty() ? nullptr : &entries_.front().camera; }
    void clear() noexcept { entries_.clear(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry { UevrEyeCamera camera; unsigned eye; };
    static constexpr std::size_t kCapacity = 8;
    std::deque<Entry> entries_;
};
}  // namespace xrfg
