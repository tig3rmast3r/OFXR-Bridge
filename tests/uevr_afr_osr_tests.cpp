#include "xrfg/uevr_afr_osr.hpp"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
bool near(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }

std::array<float, 4> transform(const xrfg::UevrMatrix& m, std::array<float, 4> v) {
    std::array<float, 4> r{};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r[j] += v[k] * m[k * 4 + j];
    return r;
}
}

int main() {
    using namespace xrfg;
    // FRotator axes: yaw 90 looks along +Y, right is -X; pitch 90 looks up.
    {
        std::array<double, 3> f{}, r{}, u{};
        uevr_rotator_axes({0, 0, 0}, f, r, u);
        check(near(f[0], 1) && near(r[1], 1) && near(u[2], 1), "identity rotator: X forward, Y right, Z up");
        uevr_rotator_axes({0, 90, 0}, f, r, u);
        check(near(f[1], 1) && near(r[0], -1) && near(u[2], 1), "yaw 90 looks along +Y");
        uevr_rotator_axes({90, 0, 0}, f, r, u);
        check(near(f[2], 1) && near(u[0], -1), "pitch 90 looks up");
    }
    // View-to-world: view z (forward) maps to the rotator's forward, positions
    // relative to the origin.
    {
        UevrEyeCamera camera{};
        camera.location = {1000100.0, 200.0, 50.0};
        camera.rotation = {0, 90, 0};
        const auto m = uevr_view_to_world(camera, {1000000.0, 0, 0});
        const auto p = transform(m, {0, 0, 10, 1});
        check(near(p[0], 100) && near(p[1], 210) && near(p[2], 50), "10 units ahead lands along +Y from the relative position");
        const auto inv = uevr_rigid_inverse(m);
        const auto back = transform(inv, p);
        check(near(back[2], 10) && near(back[0], 0) && near(back[1], 0), "rigid inverse returns to view space");
    }
    // Projection: the field's edges land on the clip edges; depth = near / z.
    {
        const float left = -0.9F, right = 0.7F, up = 0.8F, down = -0.85F, near_plane = 10.0F;
        const auto p = uevr_projection(left, right, up, down, near_plane);
        const float z = 250.0F;
        const auto at_right = transform(p, {std::tan(right) * z, 0, z, 1});
        check(near(at_right[0] / at_right[3], 1.0, 1e-4), "right edge at x = 1");
        const auto at_left = transform(p, {std::tan(left) * z, 0, z, 1});
        check(near(at_left[0] / at_left[3], -1.0, 1e-4), "left edge at x = -1");
        const auto at_up = transform(p, {0, std::tan(up) * z, z, 1});
        check(near(at_up[1] / at_up[3], 1.0, 1e-4), "top edge at y = 1");
        check(near(at_up[2] / at_up[3], near_plane / z, 1e-6), "reversed infinite depth");
        UevrMatrix inverse{};
        check(uevr_invert(p, inverse), "projection inverts");
    }
    // Image field from a submitted rectangle: Dead Island 2 under UEVR AFR
    // submits the right eye as x 508..4172 of a 4172-wide image.
    {
        const float l = -0.9F, r = 0.75F, u = 0.85F, d = -0.9F;
        const auto t = uevr_image_tangents(l, r, u, d, 508, 0, 3664, 3268, 4172, 3268);
        const float sx = (std::tan(r) - std::tan(l)) / 3664.0F;
        check(near(t.left, std::tan(l) - 508 * sx, 1e-5) && near(t.right, std::tan(r), 1e-4),
            "right eye: the image extends the field to the left");
        check(near(t.up, std::tan(u), 1e-6) && near(t.down, std::tan(d), 1e-5), "full height: vertical field unchanged");
        const auto whole = uevr_image_tangents(l, r, u, d, 0, 0, 4172, 3268, 4172, 3268);
        check(near(whole.left, std::tan(l)) && near(whole.right, std::tan(r), 1e-5), "whole image: the submitted field");
    }
    // Eye to eye: the right eye 64 mm to the right of the left one, same
    // orientation. A point in the right eye's view maps 64 mm (x100 units)
    // further right in the left eye's view; depth is kept.
    {
        UevrXrPose left{{0, 0, 0, 1}, {-0.032F, 1.6F, 0}};
        UevrXrPose right{{0, 0, 0, 1}, {0.032F, 1.6F, 0}};
        const auto m = uevr_eye_to_eye(left, right, 100.0F);
        const auto p = transform(m, {0, 0, 300, 1});
        check(near(p[0], 6.4, 1e-3) && near(p[1], 0) && near(p[2], 300, 1e-3), "right eye origin sits +6.4 units in the left view");
        // Head turned 90 degrees left (about +Y): the eye offset is still along view x.
        const float s = std::sqrt(0.5F);
        UevrXrPose l2{{0, s, 0, s}, {0, 1.6F, 0.032F}};
        UevrXrPose r2{{0, s, 0, s}, {0, 1.6F, -0.032F}};
        const auto m2 = uevr_eye_to_eye(l2, r2, 100.0F);
        const auto p2 = transform(m2, {0, 0, 0, 1});
        check(near(p2[0], 6.4, 1e-3) && near(p2[2], 0, 1e-3), "head orientation does not change the eye offset in view space");
        // A canted eye: rotation carries over with z flipped consistently.
        const float c = std::cos(0.1F), sn = std::sin(0.1F);
        UevrXrPose r3{{0, -sn, 0, c}, {0.032F, 1.6F, 0}};
        const auto m3 = uevr_eye_to_eye(left, r3, 100.0F);
        const auto forward = transform(m3, {0, 0, 1, 0});
        // OpenXR yaw -0.2 rad turns the right eye's forward (-z) to the right.
        check(forward[0] > 0.19F && forward[0] < 0.2F && forward[2] > 0.97F, "canted right eye looks right in UE view axes");
    }
    // Eye phase: the location UEVR adds differs between eyes by the IPD along
    // the view's right axis, whatever the player's own motion.
    {
        UevrEyePhase phase;
        std::optional<unsigned> eye;
        int correct = 0, known = 0;
        for (std::uint64_t i = 0; i < 40; ++i) {
            UevrEyeCamera camera{};
            camera.sequence = 1000 + i;
            const unsigned truth = static_cast<unsigned>((i + 1) & 1); // sequence 1000 is the right eye
            // Strafing right 8 units per frame (more than the IPD) and turning.
            const double base_y = 8.0 * double(i);
            camera.rotation = {0, 2.0 * double(i), 0};
            std::array<double, 3> f{}, r{}, u{};
            uevr_rotator_axes(camera.rotation, f, r, u);
            camera.base_location = {0, base_y, 170};
            const double head = 3.0;  // common head offset along forward
            const double side = truth == 1 ? 3.2 : -3.2; // the right eye sits to the right
            for (int k = 0; k < 3; ++k)
                camera.location[k] = camera.base_location[k] + f[k] * head + r[k] * side;
            eye = phase.observe(camera, 1.0F);
            if (eye) { ++known; correct += *eye == truth ? 1 : 0; }
        }
        check(known == 39 && correct == known, "every view after the first is told its physical eye while strafing and turning");
    }
    // Yaw about up: UE yaw turns X toward Y; the position stays.
    {
        UevrEyeCamera camera{};
        camera.location = {10, 20, 30};
        const auto m = uevr_view_to_world(camera, {0, 0, 0});
        const auto turned = uevr_yaw_about_up(m, 90.0);
        const auto fwd = transform(turned, {0, 0, 1, 0});
        const auto pos = transform(turned, {0, 0, 0, 1});
        check(near(fwd[0], 0, 1e-5) && near(fwd[1], 1, 1e-5) && near(pos[0], 10) && near(pos[1], 20), "yaw 90 turns forward from +X to +Y in place");
    }
    // Queue: a release takes the oldest view of its eye; a newer frame's view
    // stays for its own release.
    {
        UevrEyeCameraQueue queue;
        UevrEyeCamera a{}; a.sequence = 1; a.ticks = 100;
        UevrEyeCamera b{}; b.sequence = 2; b.ticks = 110;
        UevrEyeCamera c{}; c.sequence = 3; c.ticks = 120;
        queue.push(a, 0); queue.push(b, 1); queue.push(c, 0);
        auto first = queue.take_oldest(125, 100);
        check(first && first->camera.sequence == 1 && first->eye == 0, "oldest view first, with its eye");
        auto second = queue.take_oldest(125, 100);
        check(second && second->camera.sequence == 2 && second->eye == 1, "then the next frame's view");
        auto stale = queue.take_oldest(400, 100);
        check(!stale, "too old: a view that was never rendered is dropped");
        // A lost view: the queue holds the next frame's other eye. Taking by
        // eye leaves it for its own release instead of using it here.
        UevrEyeCameraQueue lost;
        UevrEyeCamera r1{}; r1.sequence = 11; r1.ticks = 100;
        UevrEyeCamera l2{}; l2.sequence = 12; l2.ticks = 110;
        lost.push(r1, 1);
        check(!lost.take_eye(0, 115, 100), "no left view queued: none taken");
        lost.push(l2, 0);
        auto back_right = lost.take_eye(1, 118, 100);
        check(back_right && back_right->camera.sequence == 11, "the right release still gets its view");
        auto back_left = lost.take_eye(0, 125, 100);
        check(back_left && back_left->camera.sequence == 12, "and the next left release its own: back in step");
    }
    // Stick share of the engine's yaw step over a pair (r39 receipts).
    {
        check(near(uevr_stick_yaw_step(2.82, 0.03, 0.0), 2.82), "Dead Island 2: a stick turn keeps its whole step");
        check(near(uevr_stick_yaw_step(2.82, -0.8, -0.8), 2.82), "a head turn UEVR adds itself does not touch a stick step");
        check(near(uevr_stick_yaw_step(1.58, 1.60, 0.0), 0.0), "Atomic Heart: the engine carried the head's turn, no stick share");
        check(near(uevr_stick_yaw_step(-0.64, -0.62, 0.01), 0.0), "the same turning left");
        check(near(uevr_stick_yaw_step(3.0, 1.0, 0.0), 2.0), "stick and carried head together: the stick's part");
        check(near(uevr_stick_yaw_step(0.0, 1.5, 0.0), 0.0), "no engine step, no stick share");
    }
    // Turn between two cameras, applied to a third in place (Atomic Heart's
    // second frame of a pair, 1.03 deg behind its first).
    {
        UevrEyeCamera c{}; c.rotation = {10.0, -178.52, 2.0}; c.location = {50, 60, 70};
        const auto p = uevr_view_to_world(c, {0, 0, 0});
        const auto q = uevr_yaw_about_up(p, -1.03);
        const auto turn = uevr_turn_between(p, q);
        check(near(uevr_turn_degrees(turn), 1.03, 1e-3) && near(uevr_turn_yaw_degrees(turn), -1.03, 1e-3),
            "the turn between two cameras: its angle and its yaw");
        UevrEyeCamera d{}; d.rotation = {-5.0, 30.0, 0.0}; d.location = {1, 2, 3};
        const auto m = uevr_view_to_world(d, {0, 0, 0});
        const auto turned = uevr_turn_in_place(m, turn);
        const auto want = uevr_yaw_about_up(m, -1.03);
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && near(turned[i], want[i], 1e-4);
        check(same, "turned in place like a yaw about up, position kept");
        check(near(uevr_turn_degrees(uevr_turn_between(m, m)), 0.0, 0.05), "a camera onto itself: no turn");
    }
    if (failures == 0) std::printf("uevr afr osr tests passed\n");
    return failures == 0 ? 0 : 1;
}
