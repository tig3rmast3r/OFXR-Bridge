#pragma once
// REAL VR's DLSS fix (FixDLSS) feeds its own compute shader a 464-byte
// constant buffer with the camera it reconstructed for the frame: the
// current view-to-world transform, the two-frames-back world-to-view, the
// uv-to-view mapping and the near/far planes. A title without Streamline
// (Uncharted: Legacy of Thieves) still has that buffer, so it is the layer's
// camera source there. Layout from the shader's reflection (cb0, 49 vars):
// transf0 +0, transf_2 +64, camMoComp_2 +128, mvScale0 +144, mvOffset0 +152,
// mvScale_1 +160, mvOffset_1 +168, mvInvScale0 +176, mvInvOffset0 +184,
// uvJitter0 +192, uvJitter_1 +200, uvJitter_2 +208, mv2depthScale +216,
// dso0..3 +224..+248, motionMapSize (uint2) +256, mvSize +264, mvSizeRcp +272,
// uvMax +280, srcSubrectSize +288, srcSubrectSizeRcp +296,
// uvw2xyzScale0 +304 zNear0 +316, uvw2xyzOffset0 +320 zFar0 +332,
// uvw2xyzScale_1 +336 zNear_1 +348, uvw2xyzOffset_1 +352 zFar_1 +364,
// xyz2uvwScale_2 +368 zNear_2 +380, xyz2uvwOffset_2 +384 zFar_2 +396,
// minEstimatedZ +400, offsetZ +404, miscFlags +408, compatFlags +412, ...
// The shader builds a view point as ((u, v) * uvw2xyzScale0.xy +
// uvw2xyzOffset0.xy) * z with z = zNear0 / (depth * uvw2xyzScale0.z +
// uvw2xyzOffset0.z). transf0 (row-major rows, column vector) is not a world
// pose: it takes that point into the previous frame's view (X0 = transf_2 *
// transf0 * P0, while X1 = transf_2 * P1 with P1 built in the previous
// frame's view). Under alternate-eye rendering the previous frame is the
// other eye, so a still head gives the IPD alone; Uncharted showed
// (-0.0633, 0, 0) with no rotation. Chain composes consecutive frames into
// one world. The same shader bytes ship in REAL VR 2610.15.2 and 2610.16.2.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace xrfg::native_ingress::real_vr_dlss_camera {
inline constexpr std::size_t buffer_bytes = 464;

struct Camera {
    bool valid{};
    // From decode: relative to the previous frame's view. From Chain: world.
    std::array<float, 3> position{}, right{}, up{}, forward{};
    std::array<double, 16> transform{}; // transf0 rows, the shader's own view axes
    // transf_2 rows: the previous frame's view into the one before it, so the
    // previous frame's transf0. Comparing the two checks the frame assignment.
    std::array<double, 16> previous_transform{};
    bool flip_x{}, flip_y{};            // view axes the screen maps the other way
    std::array<float, 16> view_to_clip{}, clip_to_view{}; // the layer's row-vector, w = z form
    float near_z{}, far_z{}, fov_vertical{};
};

inline float read_float(const unsigned char* bytes, std::size_t offset) noexcept {
    float value{}; std::memcpy(&value, bytes + offset, sizeof(value)); return value;
}
inline std::uint32_t read_uint(const unsigned char* bytes, std::size_t offset) noexcept {
    std::uint32_t value{}; std::memcpy(&value, bytes + offset, sizeof(value)); return value;
}

// Whether a buffer has this layout's shape for a field of motion_width x
// motion_height: the sizes it carries agree with the field, the planes are
// ordered and the mapping is finite and non-degenerate. Identifies the
// buffer among a list's bound constants without knowing REAL VR's pipeline.
inline bool looks_like(const unsigned char* bytes, std::size_t size,
    std::uint32_t motion_width, std::uint32_t motion_height) noexcept {
    if (!bytes || size < buffer_bytes || !motion_width || !motion_height) return false;
    if (read_uint(bytes, 256) != motion_width || read_uint(bytes, 260) != motion_height) return false;
    const float mv_w = read_float(bytes, 264), mv_h = read_float(bytes, 268);
    if (mv_w != float(motion_width) || mv_h != float(motion_height)) return false;
    const float rcp_w = read_float(bytes, 272), rcp_h = read_float(bytes, 276);
    if (std::fabs(rcp_w * mv_w - 1.f) > 1e-3f || std::fabs(rcp_h * mv_h - 1.f) > 1e-3f) return false;
    const float near_plane = read_float(bytes, 316), far_plane = read_float(bytes, 332);
    if (!std::isfinite(near_plane) || !std::isfinite(far_plane) || near_plane <= 0.f || far_plane <= near_plane) return false;
    for (std::size_t offset = 304; offset < 316; offset += 4)
        if (!std::isfinite(read_float(bytes, offset))) return false;
    if (read_float(bytes, 304) == 0.f || read_float(bytes, 308) == 0.f || read_float(bytes, 312) == 0.f) return false;
    return true;
}

inline bool invert(const std::array<float, 16>& m, std::array<float, 16>& out) noexcept {
    std::array<double, 16> a{}, inv{};
    for (unsigned i = 0; i < 16; ++i) { a[i] = m[i]; inv[i] = (i % 5 == 0) ? 1.0 : 0.0; }
    for (unsigned c = 0; c < 4; ++c) {
        unsigned pivot = c;
        for (unsigned r = c + 1; r < 4; ++r) if (std::fabs(a[r * 4 + c]) > std::fabs(a[pivot * 4 + c])) pivot = r;
        if (std::fabs(a[pivot * 4 + c]) < 1e-12) return false;
        for (unsigned j = 0; j < 4; ++j) { std::swap(a[c * 4 + j], a[pivot * 4 + j]); std::swap(inv[c * 4 + j], inv[pivot * 4 + j]); }
        const double d = a[c * 4 + c];
        for (unsigned j = 0; j < 4; ++j) { a[c * 4 + j] /= d; inv[c * 4 + j] /= d; }
        for (unsigned r = 0; r < 4; ++r) if (r != c) {
            const double f = a[r * 4 + c];
            for (unsigned j = 0; j < 4; ++j) { a[r * 4 + j] -= f * a[c * 4 + j]; inv[r * 4 + j] -= f * inv[c * 4 + j]; }
        }
    }
    for (unsigned i = 0; i < 16; ++i) { out[i] = float(inv[i]); if (!std::isfinite(out[i])) return false; }
    return true;
}

// The camera this buffer describes, in the layer's convention: right is the
// screen's right, up the screen's up, the projection's x and y positive, and
// the game's own depth value its z/w: depth = (zNear0 / S.z) / z - O.z / S.z,
// i.e. [10] = -O.z / S.z, [14] = zNear0 / S.z, with w = z ([11] = 1); x and
// y from uv: x_ndc = 2 (x/z - O.x) / S.x - 1, y_ndc = 1 - 2 (y/z - O.y) / S.y.
// A view axis the shader maps the other way (S.x < 0: x to the screen's
// left; S.y > 0: y down, as v) is negated with its scale and offset, which
// leaves every uv the same point.
inline Camera decode(const unsigned char* bytes, std::size_t size) noexcept {
    Camera camera{};
    if (!bytes || size < buffer_bytes) return camera;
    std::array<float, 16> transf0{};
    for (unsigned i = 0; i < 16; ++i) transf0[i] = read_float(bytes, i * 4); // rows of world = M * point
    for (float v : transf0) if (!std::isfinite(v)) return camera;
    float sx = read_float(bytes, 304), sy = read_float(bytes, 308);
    const float sz = read_float(bytes, 312);
    float ox = read_float(bytes, 320), oy = read_float(bytes, 324);
    const float oz = read_float(bytes, 328);
    const float near_plane = read_float(bytes, 316), far_plane = read_float(bytes, 332);
    if (!std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz) || !std::isfinite(ox) || !std::isfinite(oy) ||
        !std::isfinite(oz) || sx == 0.f || sy == 0.f || sz == 0.f || !(near_plane > 0.f) || !(far_plane > near_plane)) return camera;
    camera.near_z = near_plane; camera.far_z = far_plane;
    const auto column = [&](unsigned c) {
        const auto& m = camera.transform;
        return std::array<float, 3>{float(m[0 * 4 + c]), float(m[1 * 4 + c]), float(m[2 * 4 + c])};
    };
    // The registers hold the rotation transposed relative to the translation
    // column: a stick turn in Uncharted moved the scene one way while the
    // steps composed as read turned the camera the other (F8 2026-10-28,
    // publication 25252: stored vectors x < 0, camera turn toward +right).
    // Translation-only steps (the IPD) read the same either way.
    const auto rows_rotation_transposed = [](std::array<double, 16>& m) {
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < i; ++j) std::swap(m[i * 4 + j], m[j * 4 + i]);
    };
    for (unsigned i = 0; i < 16; ++i) camera.transform[i] = transf0[i];
    for (unsigned i = 0; i < 16; ++i) camera.previous_transform[i] = read_float(bytes, 64 + i * 4);
    rows_rotation_transposed(camera.transform);
    rows_rotation_transposed(camera.previous_transform);
    camera.position = column(3);
    camera.right = column(0);
    camera.up = column(1);
    camera.forward = column(2);
    camera.flip_x = sx < 0.f;
    camera.flip_y = sy > 0.f;
    if (camera.flip_x) { for (float& v : camera.right) v = -v; sx = -sx; ox = -ox; }
    if (camera.flip_y) { for (float& v : camera.up) v = -v; sy = -sy; oy = -oy; }
    for (const auto* axis : {&camera.right, &camera.up, &camera.forward}) {
        const float length = std::sqrt((*axis)[0] * (*axis)[0] + (*axis)[1] * (*axis)[1] + (*axis)[2] * (*axis)[2]);
        if (!std::isfinite(length) || std::fabs(length - 1.f) > 0.05f) return camera;
    }
    auto& p = camera.view_to_clip; p = {};
    p[0] = 2.f / sx;                 // x_ndc from x/z
    p[8] = -2.f * ox / sx - 1.f;     // ... plus the offset, carried by z (w)
    p[5] = -2.f / sy;                // y_ndc from y/z; sy < 0 here, so positive
    p[9] = 2.f * oy / sy + 1.f;
    p[10] = -oz / sz;
    p[14] = near_plane / sz;
    p[11] = 1.f;
    if (!invert(p, camera.clip_to_view)) return camera;
    camera.fov_vertical = std::atan(std::fabs(oy)) + std::atan(std::fabs(sy + oy));
    camera.valid = std::isfinite(camera.fov_vertical) && camera.fov_vertical > 0.1f && camera.fov_vertical < 3.0f;
    return camera;
}

// World cameras: world(N) = world(P) * transf0(N), P the frame REAL VR stepped
// from. P is not always the publication before N: in Uncharted a quarter of
// the frames step from an earlier one or from the other eye's (their transf_2
// differs from the previous transf0 by twice the IPD), sometimes for a second
// at a time. transf_2(N) is P's own transf0, so P is the recent frame whose
// transf0 equals it. When none does, P is a frame the layer never captured
// (about 30% of Uncharted's evaluations publish no guide; two published
// frames in a row then show the same eye), and transf_2(N) is the step into
// P from the frame before it, the last one the chain holds:
// world(N) = world(last) * transf_2(N) * transf0(N). The world is the view of
// the frame the chain started on; only relations between cameras matter.
class Chain {
    struct Frame { std::array<double, 16> step{}, world{}; };
    static constexpr unsigned kRecent = 16;
    std::array<Frame, kRecent> recent_{};
    unsigned count_{}, next_{};
    std::uint64_t world_id_{};
    static bool same(const std::array<double, 16>& a, const std::array<double, 16>& b) noexcept {
        for (unsigned i = 0; i < 12; ++i) if (std::abs(a[i] - b[i]) > 1e-3) return false;
        return true;
    }
    static std::array<double, 16> product(const std::array<double, 16>& a, const std::array<double, 16>& b) noexcept {
        std::array<double, 16> result{};
        for (unsigned r = 0; r < 4; ++r)
            for (unsigned c = 0; c < 4; ++c)
                for (unsigned k = 0; k < 4; ++k)
                    result[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
        return result;
    }
public:
    static constexpr int kRestarted = 0, kBridged = -1;
    // Identifies the current world; a restart starts a new one. Never 0 once used.
    std::uint64_t world() const noexcept { return world_id_; }
    // Whether constants read from a buffer of unknown assignment are a new
    // frame stepping from one held here: not a held frame's own step, and its
    // transf_2 a held frame's transf0. Returns how far back that frame is, 0 if not.
    unsigned continues(const Camera& candidate) const noexcept {
        for (unsigned back = 1; back <= count_; ++back)
            if (same(recent_[(next_ + kRecent - back) % kRecent].step, candidate.transform)) return 0;
        for (unsigned back = 1; back <= count_; ++back)
            if (same(recent_[(next_ + kRecent - back) % kRecent].step, candidate.previous_transform)) return back;
        return 0;
    }
    // >0: how many frames back the frame REAL VR stepped from was (1: the
    // previous one). kBridged: it was a frame never seen, entered from the
    // last one. kRestarted: a new world starts at this frame (the first).
    int advance(const Camera& relative, Camera& out) noexcept {
        int depth = kRestarted;
        std::array<double, 16> world{};
        for (unsigned back = 1; back <= count_; ++back) {
            const auto& frame = recent_[(next_ + kRecent - back) % kRecent];
            if (!same(frame.step, relative.previous_transform)) continue;
            world = product(frame.world, relative.transform);
            depth = static_cast<int>(back);
            break;
        }
        if (depth == kRestarted && count_) {
            const auto& last = recent_[(next_ + kRecent - 1) % kRecent];
            world = product(product(last.world, relative.previous_transform), relative.transform);
            depth = kBridged;
        }
        if (depth == kRestarted) {
            for (unsigned i = 0; i < 4; ++i) world[i * 5] = 1.0;
            ++world_id_;
        }
        recent_[next_] = {relative.transform, world};
        next_ = (next_ + 1) % kRecent;
        count_ = std::min(count_ + 1, kRecent);
        out = relative;
        const auto column = [&](unsigned c, double sign) {
            return std::array<float, 3>{float(sign * world[c]), float(sign * world[4 + c]), float(sign * world[8 + c])};
        };
        out.position = column(3, 1.0);
        out.right = column(0, relative.flip_x ? -1.0 : 1.0);
        out.up = column(1, relative.flip_y ? -1.0 : 1.0);
        out.forward = column(2, 1.0);
        out.transform = world;
        return depth;
    }
};
} // namespace xrfg::native_ingress::real_vr_dlss_camera
