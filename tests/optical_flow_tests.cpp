#include "xrfg/image.hpp"
#include "xrfg/optical_flow.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kWidth = 32;
constexpr std::size_t kHeight = 24;
constexpr int kTranslation = 2;

int failures = 0;

void expect(bool condition, std::string_view description) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}
[[nodiscard]] xrfg::Rgba8 texture(std::size_t x, std::size_t y) noexcept {
    const std::uint32_t hash = static_cast<std::uint32_t>(
        x * 73U + y * 151U + x * y * 19U + (x ^ y) * 37U);
    return {
        static_cast<std::uint8_t>(20U + hash % 216U),
        static_cast<std::uint8_t>(20U + (hash * 3U + 17U) % 216U),
        static_cast<std::uint8_t>(20U + (hash * 7U + 43U) % 216U),
        255,
    };
}

[[nodiscard]] std::vector<xrfg::Rgba8> translated_frame(int translation) {
    std::vector<xrfg::Rgba8> frame(kWidth * kHeight, {0, 0, 0, 255});
    for (std::size_t y = 0; y < kHeight; ++y) {
        for (std::size_t x = 0; x < kWidth; ++x) {
            const int target_x = static_cast<int>(x) + translation;
            if (target_x >= 0 && target_x < static_cast<int>(kWidth)) {
                frame[y * kWidth + static_cast<std::size_t>(target_x)] = texture(x, y);
            }
        }
    }
    return frame;
}

[[nodiscard]] double mean_absolute_rgb_error(
    const std::vector<xrfg::Rgba8>& actual,
    const std::vector<xrfg::Rgba8>& expected) {
    double total = 0.0;
    std::size_t samples = 0;
    for (std::size_t y = 2; y + 2 < kHeight; ++y) {
        for (std::size_t x = 4; x + 4 < kWidth; ++x) {
            const std::size_t index = y * kWidth + x;
            total += std::abs(static_cast<int>(actual[index].r) - static_cast<int>(expected[index].r));
            total += std::abs(static_cast<int>(actual[index].g) - static_cast<int>(expected[index].g));
            total += std::abs(static_cast<int>(actual[index].b) - static_cast<int>(expected[index].b));
            samples += 3;
        }
    }
    return total / static_cast<double>(samples);
}

}  // namespace

int main() {
    const auto previous = translated_frame(0);
    const auto current = translated_frame(kTranslation);
    const auto expected_middle = translated_frame(kTranslation / 2);
    const xrfg::OpticalFlowSettings settings{3, 1, 0.5F};
    const xrfg::BidirectionalFlow flow = xrfg::estimate_bidirectional_flow_reference(
        previous,
        current,
        kWidth,
        kHeight,
        settings);

    const std::size_t previous_probe = 10 * kWidth + 12;
    const std::size_t current_probe = 10 * kWidth + 14;
    expect(
        flow.previous_to_current[previous_probe].horizontal == 2.0F &&
            flow.previous_to_current[previous_probe].vertical == 0.0F,
        "forward flow recovers a two-pixel translation");
    expect(
        flow.current_to_previous[current_probe].horizontal == -2.0F &&
            flow.current_to_previous[current_probe].vertical == 0.0F,
        "backward flow recovers the inverse translation");

    const auto synthesized = xrfg::synthesize_bidirectional_reference(
        previous,
        current,
        flow,
        0.5F,
        settings.consistency_threshold);
    std::vector<xrfg::Rgba8> naive(previous.size());
    for (std::size_t index = 0; index < previous.size(); ++index) {
        naive[index] = xrfg::interpolate_pixel(previous[index], current[index], 0.5F);
    }
    const double flow_error = mean_absolute_rgb_error(synthesized, expected_middle);
    const double naive_error = mean_absolute_rgb_error(naive, expected_middle);
    expect(flow_error < 1.0, "bidirectional synthesis reconstructs the translated midpoint");
    expect(flow_error < naive_error * 0.1, "optical flow materially beats same-pixel blending");

    bool rejected_bad_dimensions = false;
    try {
        (void)xrfg::estimate_bidirectional_flow_reference(
            previous,
            current,
            kWidth - 1,
            kHeight,
            settings);
    } catch (const std::invalid_argument&) {
        rejected_bad_dimensions = true;
    }
    expect(rejected_bad_dimensions, "mismatched dimensions are rejected");

    if (failures != 0) {
        std::cerr << failures << " optical-flow test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "Bidirectional CPU optical-flow reference passed; midpoint MAE="
              << flow_error << ", naive MAE=" << naive_error << '\n';
    return EXIT_SUCCESS;
}
