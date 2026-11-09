#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>
#include "xrfg/delivery_cadence.hpp"

namespace xrfg {

enum class FpsOverlayPosition { off, upper_left, upper_right, lower_left, lower_right, elliott_controller, elliott_bottom };
[[nodiscard]] FpsOverlayPosition parse_overlay_position(std::string_view value) noexcept;
[[nodiscard]] const char* overlay_position_name(FpsOverlayPosition position) noexcept;

struct FpsSnapshot {
    float submitted_fps{};
    bool active{};
    bool afw_active{};
    float delivered_fps{};
    bool delivered_valid{};
    float real_fps{}, generated_fps{};
    bool sample_valid{};
    bool aer_eye_equivalents{};
};

// Caller serializes access. Fixed storage, monotonic wall-clock measurements;
// successful downstream submissions are NOT evidence of physical scanout.
[[nodiscard]] float displayed_rate(float rate, float refresh_hz) noexcept;

class FpsCounter {
public:
    void submitted(std::int64_t now_ns, bool synthetic, bool new_content = true, bool aer_mixed = false) noexcept;
    [[nodiscard]] FpsSnapshot snapshot(std::int64_t now_ns) const noexcept;
    [[nodiscard]] DeliveryCadenceBatch cadence(std::int64_t now_ns) const noexcept {return cadence_.snapshot(now_ns);}
    void reset() noexcept { *this = {}; }
private:
    DeliveryCadenceLog cadence_;
    struct Bucket {
        std::int64_t epoch{-1};
        std::uint32_t output{};
        std::uint32_t real{}, generated{};
        std::uint32_t mixed{};
    };
    Bucket& bucket(std::int64_t now_ns) noexcept;
    std::array<Bucket, 12> buckets_{};
    std::int64_t start_ns_{-1};
    std::int64_t last_synthetic_ns_{-1};
};

struct OverlayPlacement {
    float x{}, y{}, z{-2.0f}, width{}, height{};
};
// Tangent-space bounds common to both eyes. Inset AND capped to the central
// 90 degrees so wide-FOV headsets do not push the panel into peripheral optics.
[[nodiscard]] OverlayPlacement overlay_placement(
    FpsOverlayPosition position, float left, float right, float down, float up) noexcept;
[[nodiscard]] std::uint32_t overlay_texture_width(std::uint32_t eye_width) noexcept;
[[nodiscard]] std::vector<std::uint32_t> rasterize_fps_overlay(
    std::uint32_t width, std::uint32_t height, const FpsSnapshot& snapshot, bool bgra);

} // namespace xrfg
