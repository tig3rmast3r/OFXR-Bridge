#include "xrfg/image.hpp"
#include "xrfg/optical_flow.hpp"
#include "xrfg/pose.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kWidth = 96;
constexpr std::size_t kHeight = 54;
constexpr int kPreviousSquareLeft = 28;
constexpr int kCurrentSquareLeft = 36;
constexpr int kSquareTop = 17;
constexpr int kSquareSize = 20;

[[nodiscard]] xrfg::Quaternion yaw(float degrees) {
    const float half_angle = degrees * std::numbers::pi_v<float> / 360.0F;
    return {0.0F, std::sin(half_angle), 0.0F, std::cos(half_angle)};
}

[[nodiscard]] std::vector<xrfg::Rgba8> make_frame(int square_left) {
    std::vector<xrfg::Rgba8> pixels(kWidth * kHeight);
    for (std::size_t y = 0; y < kHeight; ++y) {
        for (std::size_t x = 0; x < kWidth; ++x) {
            auto& pixel = pixels[y * kWidth + x];
            pixel = {
                static_cast<std::uint8_t>(20 + (x * 70) / kWidth + ((x + y) % 3) * 3),
                static_cast<std::uint8_t>(30 + (y * 80) / kHeight + ((x * 3 + y) % 5) * 2),
                static_cast<std::uint8_t>(90 + ((x * 7 + y * 11) % 23)),
                255,
            };
            const int local_x = static_cast<int>(x) - square_left;
            const int local_y = static_cast<int>(y) - kSquareTop;
            if (local_x >= 0 && local_x < kSquareSize &&
                local_y >= 0 && local_y < kSquareSize) {
                const int checker = ((local_x / 2) ^ (local_y / 2)) & 1;
                pixel = {
                    static_cast<std::uint8_t>(190 + (local_x * 5 + local_y * 3) % 55),
                    static_cast<std::uint8_t>(40 + checker * 70 + (local_y * 7) % 30),
                    static_cast<std::uint8_t>(25 + (local_x * 11 + local_y * 13) % 80),
                    255,
                };
            }
        }
    }
    return pixels;
}

[[nodiscard]] double moving_region_error(
    std::span<const xrfg::Rgba8> actual,
    std::span<const xrfg::Rgba8> expected) {
    double total = 0.0;
    std::size_t samples = 0;
    const int middle_left = (kPreviousSquareLeft + kCurrentSquareLeft) / 2;
    for (int y = kSquareTop - 3; y < kSquareTop + kSquareSize + 3; ++y) {
        for (int x = middle_left - 7; x < middle_left + kSquareSize + 7; ++x) {
            const std::size_t index =
                static_cast<std::size_t>(y) * kWidth + static_cast<std::size_t>(x);
            total += std::abs(static_cast<int>(actual[index].r) - static_cast<int>(expected[index].r));
            total += std::abs(static_cast<int>(actual[index].g) - static_cast<int>(expected[index].g));
            total += std::abs(static_cast<int>(actual[index].b) - static_cast<int>(expected[index].b));
            samples += 3;
        }
    }
    return total / static_cast<double>(samples);
}

void write_ppm(
    const std::filesystem::path& path,
    std::span<const xrfg::Rgba8> pixels) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot create " + path.string());
    }
    stream << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
    for (const auto& pixel : pixels) {
        const char rgb[] = {
            static_cast<char>(pixel.r),
            static_cast<char>(pixel.g),
            static_cast<char>(pixel.b),
        };
        stream.write(rgb, sizeof(rgb));
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::filesystem::path output_directory =
            argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("reference-output");
        std::filesystem::create_directories(output_directory);

        const auto previous_image = make_frame(kPreviousSquareLeft);
        const auto current_image = make_frame(kCurrentSquareLeft);
        const auto expected_middle_image = make_frame(
            (kPreviousSquareLeft + kCurrentSquareLeft) / 2);
        const xrfg::OpticalFlowSettings flow_settings{10, 2, 1.5F};
        const auto flow = xrfg::estimate_bidirectional_flow_reference(
            previous_image,
            current_image,
            kWidth,
            kHeight,
            flow_settings);
        const auto middle_image = xrfg::synthesize_bidirectional_reference(
            previous_image,
            current_image,
            flow,
            0.5F,
            flow_settings.consistency_threshold);
        std::vector<xrfg::Rgba8> naive_middle(previous_image.size());
        for (std::size_t index = 0; index < previous_image.size(); ++index) {
            naive_middle[index] = xrfg::interpolate_pixel(
                previous_image[index],
                current_image[index],
                0.5F);
        }

        write_ppm(output_directory / "previous.ppm", previous_image);
        write_ppm(output_directory / "bidirectional-middle.ppm", middle_image);
        write_ppm(output_directory / "expected-middle.ppm", expected_middle_image);
        write_ppm(output_directory / "current.ppm", current_image);

        const xrfg::TimedPose previous_pose{
            1'000'000'000,
            {yaw(0.0F), {0.0F, 1.65F, 0.0F}},
        };
        const xrfg::TimedPose current_pose{
            1'022'222'222,
            {yaw(10.0F), {0.02F, 1.65F, -0.01F}},
        };
        const auto middle_pose = xrfg::midpoint(previous_pose, current_pose);
        if (!middle_pose) {
            std::cerr << "invalid pose timestamps\n";
            return 1;
        }

        const double flow_error = moving_region_error(middle_image, expected_middle_image);
        const double naive_error = moving_region_error(naive_middle, expected_middle_image);
        std::cout << "Generated four bidirectional-flow reference images in "
                  << output_directory.string() << '\n';
        std::cout << "Input cadence: 45 Hz; intended synthetic cadence: 90 Hz\n";
        std::cout << "Midpoint time: " << middle_pose->time_ns << " ns\n";
        std::cout << "Midpoint position: " << middle_pose->pose.position.x << ", "
                  << middle_pose->pose.position.y << ", "
                  << middle_pose->pose.position.z << '\n';
        std::cout << "Moving-region RGB MAE: flow=" << flow_error
                  << ", same-pixel blend=" << naive_error << '\n';
        std::cout << "CPU reference only: no OpenXR GPU capture or doubled submit is active.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "xrfg_reference: " << error.what() << '\n';
        return 1;
    }
}
