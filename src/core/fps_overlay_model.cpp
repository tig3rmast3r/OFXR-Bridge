#include "xrfg/fps_overlay_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xrfg {

FpsOverlayPosition parse_overlay_position(std::string_view value) noexcept {
    if (value == "off") return FpsOverlayPosition::off;
    if (value == "upper_left") return FpsOverlayPosition::upper_left;
    if (value == "lower_left") return FpsOverlayPosition::lower_left;
    if (value == "lower_right") return FpsOverlayPosition::lower_right;
    return FpsOverlayPosition::upper_right;
}

const char* overlay_position_name(FpsOverlayPosition position) noexcept {
    switch (position) {
    case FpsOverlayPosition::off: return "off";
    case FpsOverlayPosition::upper_left: return "upper_left";
    case FpsOverlayPosition::lower_left: return "lower_left";
    case FpsOverlayPosition::lower_right: return "lower_right";
    default: return "upper_right";
    }
}

FpsCounter::Bucket& FpsCounter::bucket(std::int64_t now) noexcept {
    now = std::max<std::int64_t>(now, 0);
    if (start_ns_ < 0) start_ns_ = now;
    const auto epoch = now / 100'000'000;
    auto& entry = buckets_[static_cast<std::size_t>(epoch) % buckets_.size()];
    if (entry.epoch != epoch) entry = Bucket{epoch};
    return entry;
}

void FpsCounter::submitted(std::int64_t now, bool synthetic) noexcept {
    auto& entry = bucket(now);
    ++entry.output;
    if (synthetic) last_synthetic_ns_ = now;
}

FpsSnapshot FpsCounter::snapshot(std::int64_t now) const noexcept {
    FpsSnapshot result;
    if (start_ns_ < 0 || now < start_ns_) return result;
    const auto epoch = now / 100'000'000;
    const auto window_start = std::max(start_ns_, (epoch - 9) * 100'000'000);
    const float seconds = std::max(0.1f, static_cast<float>(now - window_start) * 1e-9f);
    for (const auto& entry : buckets_) {
        if (entry.epoch < 0 || entry.epoch > epoch || entry.epoch < epoch - 9) continue;
        result.submitted_fps += static_cast<float>(entry.output) / seconds;
    }
    // Hysteresis across alternating S/B slots; no green merely because armed,
    // initialized, primed, repeated, or because an attempted S failed.
    result.active = last_synthetic_ns_ >= 0 && now >= last_synthetic_ns_ &&
        now - last_synthetic_ns_ < 500'000'000;
    return result;
}

OverlayPlacement overlay_placement(
    FpsOverlayPosition position, float left, float right, float down, float up) noexcept {
    left = std::clamp(left, -1.0f, -0.1f);
    right = std::clamp(right, 0.1f, 1.0f);
    down = std::clamp(down, -1.0f, -0.1f);
    up = std::clamp(up, 0.1f, 1.0f);
    const float span_x = right - left, span_y = up - down;
    const float anchor_width = std::min(span_x * 0.06f, span_y * 0.15f);
    const float anchor_height = anchor_width / 2.0f;
    constexpr float size_scale = 0.80f;
    const float width = anchor_width * size_scale;
    const float height = anchor_height * size_scale;
    // Keep the small number comfortably inside the headset's visible area.
    // Mirror the same inset for all corners. Anchor with the V065 footprint so
    // reducing the visible size does not move the counter centre back outward.
    constexpr float horizontal_inset = 0.25f;
    constexpr float vertical_inset = 0.29f;
    const bool on_left = position == FpsOverlayPosition::upper_left ||
        position == FpsOverlayPosition::lower_left;
    const bool on_top = position == FpsOverlayPosition::upper_left ||
        position == FpsOverlayPosition::upper_right;
    return {2.0f * (on_left ? left + span_x * horizontal_inset + anchor_width * 0.5f
                           : right - span_x * horizontal_inset - anchor_width * 0.5f),
            2.0f * (on_top ? up - span_y * vertical_inset - anchor_height * 0.5f
                          : down + span_y * vertical_inset + anchor_height * 0.5f),
            -2.0f, width * 2.0f, height * 2.0f};
}

std::uint32_t overlay_texture_width(std::uint32_t eye_width) noexcept {
    return std::clamp((eye_width / 16 + 23) / 24 * 24, 96u, 336u);
}

namespace {
// Original 5x7 bitmap glyphs, rows from top to bottom. No font dependency.
std::array<unsigned, 7> glyph(char c) noexcept {
    switch (c) {
    case '0': return {14,17,19,21,25,17,14};
    case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31};
    case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2};
    case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14};
    case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14};
    case '9': return {14,17,17,15,1,1,14};
    default: return {};
    }
}
}

std::vector<std::uint32_t> rasterize_fps_overlay(
    std::uint32_t width, std::uint32_t height, const FpsSnapshot& snapshot, bool bgra) {
    if (width < 24 || width > 2048 || height < 12 || height > 1024) return {};
    const auto color = [bgra](unsigned r, unsigned g, unsigned b) {
        return 0xff000000u | (bgra ? b | (g << 8) | (r << 16)
                                          : r | (g << 8) | (b << 16));
    };
    // Premultiplied alpha: transparent black outside opaque digit strokes.
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height, 0);
    const auto status = snapshot.active ? color(64, 235, 112) : color(255, 80, 80);
    const unsigned scale = std::min(width / 24, height / 12);
    const float rate = std::isfinite(snapshot.submitted_fps) ? snapshot.submitted_fps : 0.0f;
    char text[4]{};
    const auto length = static_cast<unsigned>(std::snprintf(text, sizeof(text), "%d",
        static_cast<int>(std::clamp(rate, 0.0f, 999.0f) + 0.5f)));
    unsigned x = (width - (length * 6 - 1) * scale) / 2;
    const unsigned top = (height - 7 * scale) / 2;
    for (const char* p = text; *p; ++p, x += 6 * scale) {
        const auto rows = glyph(*p);
        for (unsigned y = 0; y < 7; ++y) for (unsigned column = 0; column < 5; ++column) {
            if ((rows[y] & (1u << (4 - column))) == 0) continue;
            for (unsigned sy = 0; sy < scale; ++sy) for (unsigned sx = 0; sx < scale; ++sx) {
                const unsigned px = x + column * scale + sx;
                const unsigned py = top + y * scale + sy;
                pixels[static_cast<std::size_t>(py) * width + px] = status;
            }
        }
    }
    return pixels;
}

} // namespace xrfg
