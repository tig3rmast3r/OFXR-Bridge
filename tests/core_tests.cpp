#include "xrfg/image.hpp"
#include "xrfg/generation_backpressure.hpp"
#include "xrfg/pose.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

[[nodiscard]] bool near(float left, float right, float epsilon = 1.0e-4F) {
    return std::abs(left - right) <= epsilon;
}

[[nodiscard]] xrfg::Quaternion yaw(float degrees) {
    const float half_angle = degrees * std::numbers::pi_v<float> / 360.0F;
    return {0.0F, std::sin(half_angle), 0.0F, std::cos(half_angle)};
}

}  // namespace

int main() {
    const xrfg::Pose start{yaw(0.0F), {0.0F, 1.0F, 2.0F}};
    const xrfg::Pose end{yaw(90.0F), {2.0F, 3.0F, 6.0F}};
    const xrfg::Pose middle = xrfg::interpolate_pose(start, end, 0.5F);

    expect(near(middle.position.x, 1.0F), "position x midpoint");
    expect(near(middle.position.y, 2.0F), "position y midpoint");
    expect(near(middle.position.z, 4.0F), "position z midpoint");
    expect(near(middle.orientation.y, std::sin(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw sine");
    expect(near(middle.orientation.w, std::cos(std::numbers::pi_v<float> / 8.0F)), "45-degree yaw cosine");

    const auto same_rotation_other_sign = xrfg::slerp_shortest(yaw(30.0F), {
        -yaw(30.0F).x,
        -yaw(30.0F).y,
        -yaw(30.0F).z,
        -yaw(30.0F).w,
    }, 0.5F);
    expect(near(std::abs(same_rotation_other_sign.w), std::cos(std::numbers::pi_v<float> / 12.0F)),
           "quaternion sign uses shortest path");

    const xrfg::TimedPose timed_start{100, start};
    const xrfg::TimedPose timed_end{200, end};
    const auto timed_middle = xrfg::midpoint(timed_start, timed_end);
    expect(timed_middle.has_value() && timed_middle->time_ns == 150, "timestamp midpoint");
    expect(!xrfg::midpoint(timed_end, timed_start).has_value(), "reject non-monotonic timestamps");

    const xrfg::Rgba8 black{0, 0, 0, 0};
    const xrfg::Rgba8 white{255, 255, 255, 255};
    const auto gray = xrfg::interpolate_pixel(black, white, 0.5F);
    expect(gray.r == 128 && gray.g == 128 && gray.b == 128 && gray.a == 128, "pixel midpoint rounds correctly");

    using namespace std::chrono_literals;
    expect(!xrfg::should_enter_generation_cooldown(false, 200ms, 8'333'333),
            "FidelityFX ignores compositor end spikes");
    expect(!xrfg::should_enter_generation_cooldown(true, 50ms, 8'333'333),
           "NVIDIA tolerates low-frame-rate compositor pacing");
    expect(!xrfg::should_enter_generation_cooldown(
               true, 75ms, 8'333'333),
           "NVIDIA cooldown has a strict 75 ms minimum threshold");
    expect(xrfg::should_enter_generation_cooldown(true, 75'000'001ns, 8'333'333),
           "NVIDIA retains recovery above the 75 ms minimum threshold");
    expect(!xrfg::should_enter_generation_cooldown(true, 83ms, 13'888'889),
           "NVIDIA also requires more than six display periods");
    expect(xrfg::should_enter_generation_cooldown(true, 84ms, 13'888'889),
           "NVIDIA retains recovery beyond six display periods");
    expect(!xrfg::should_enter_generation_cooldown(true, 68ms, 0),
           "invalid display period cannot start cooldown");

    constexpr std::array uevr_slow_ends{
        68'522us, 18'226us, 22'262us, 19'471us, 19'330us,
        19'863us, 19'893us, 19'440us, 22'551us};
    std::size_t uevr_nvidia_cooldowns = 0;
    for (const auto elapsed : uevr_slow_ends) {
        uevr_nvidia_cooldowns += xrfg::should_enter_generation_cooldown(
            true, elapsed, 8'333'333);
    }
    expect(uevr_nvidia_cooldowns == 0,
           "retained low-rate UEVR timing replay causes no cooldowns");

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All XR Frame Bridge core tests passed\n";
    return EXIT_SUCCESS;
}
