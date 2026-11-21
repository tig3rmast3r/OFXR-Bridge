#include "xrfg/fps_overlay_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xrfg {

FpsOverlayPosition parse_overlay_position(std::string_view value) noexcept {
    if (value == "elliott_controller") return FpsOverlayPosition::elliott_controller;
    if (value == "elliott_bottom") return FpsOverlayPosition::elliott_bottom;
    if (value == "off") return FpsOverlayPosition::off;
    if (value == "upper_left") return FpsOverlayPosition::upper_left;
    if (value == "lower_left") return FpsOverlayPosition::lower_left;
    if (value == "lower_right") return FpsOverlayPosition::lower_right;
    return FpsOverlayPosition::upper_right;
}

const char* overlay_position_name(FpsOverlayPosition position) noexcept {
    switch (position) {
    case FpsOverlayPosition::elliott_controller: return "elliott_controller";
    case FpsOverlayPosition::elliott_bottom: return "elliott_bottom";
    case FpsOverlayPosition::off: return "off";
    case FpsOverlayPosition::upper_left: return "upper_left";
    case FpsOverlayPosition::lower_left: return "lower_left";
    case FpsOverlayPosition::lower_right: return "lower_right";
    default: return "upper_right";
    }
}

FpsCounter::Bucket& FpsCounter::bucket(std::int64_t now) noexcept {
    now = std::max<std::int64_t>(now, 0);
    if (start_ns_ < 0 || now < start_ns_) start_ns_ = now;
    const auto epoch = now / 100'000'000;
    auto& entry = buckets_[static_cast<std::size_t>(epoch) % buckets_.size()];
    if (entry.epoch != epoch) entry = Bucket{epoch};
    return entry;
}

void FpsCounter::submitted(std::int64_t now, bool synthetic, bool new_content, bool aer_mixed) noexcept {
    if (!new_content) return;
    cadence_.record(now);
    auto& entry = bucket(now);
    ++entry.output;
    if (aer_mixed) ++entry.mixed;
    else if (synthetic) ++entry.generated;
    else ++entry.real;
    if (synthetic || aer_mixed) last_synthetic_ns_ = std::max(last_synthetic_ns_,now);
}

FpsSnapshot FpsCounter::snapshot(std::int64_t now) const noexcept {
    FpsSnapshot result;
    if (start_ns_ < 0 || now < start_ns_) return result;
    result.sample_valid = true;
    const auto epoch = now / 100'000'000;
    const auto window_start = std::max(start_ns_, (epoch - 9) * 100'000'000);
    const float seconds = std::max(0.1f, static_cast<float>(now - window_start) * 1e-9f);
    unsigned outputs{}, mixed{};
    for (const auto& entry : buckets_) {
        if (entry.epoch < 0 || entry.epoch > epoch || entry.epoch < epoch - 9) continue;
        result.submitted_fps += static_cast<float>(entry.output) / seconds;
        result.real_fps += (static_cast<float>(entry.real) + .5F*entry.mixed) / seconds;
        result.generated_fps += (static_cast<float>(entry.generated) + .5F*entry.mixed) / seconds;
        outputs += entry.output;
        mixed += entry.mixed;
    }
    // Hysteresis across alternating S/B slots; no green merely because armed,
    // initialized, primed, repeated, or because an attempted S failed.
    result.active = last_synthetic_ns_ >= 0 && now >= last_synthetic_ns_ &&
        now - last_synthetic_ns_ < 500'000'000;
    result.aer_eye_equivalents = mixed != 0;
    result.aer_all_mixed = result.active && mixed && mixed == outputs && seconds >= 0.5f;
    return result;
}

float displayed_rate(float rate, float refresh_hz) noexcept {
    if (!(refresh_hz > 0.0f) || !std::isfinite(rate)) return rate;
    const float band = refresh_hz * 0.02f;
    return rate >= refresh_hz - band && rate <= refresh_hz + band ? refresh_hz : rate;
}

FpsDisplayRates displayed_fps_rates(FpsDisplayRates measured, float refresh_hz, bool aer_all_mixed) noexcept {
    // The measurement window can straddle a submission. At steady AER cadence
    // an admitted output contributes one eye to each rate. Require all three
    // measurements to be close before presenting the nominal cadence.
    if (!aer_all_mixed || !std::isfinite(refresh_hz) || refresh_hz <= 0.0f) return measured;
    const float half = refresh_hz * 0.5f;
    if (displayed_rate(measured.output, refresh_hz) == refresh_hz &&
        displayed_rate(measured.real, half) == half &&
        displayed_rate(measured.generated, half) == half)
        return {refresh_hz, half, half};
    return measured;
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
    case 'A': return {14,17,17,31,17,17,17};
    case 'B': return {30,17,17,30,17,17,30};
    case 'E': return {31,16,16,30,16,16,31};
    case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14};
    case 'M': return {17,27,21,21,17,17,17};
    case 'O': return {14,17,17,17,17,17,14};
    case 'P': return {30,17,17,30,16,16,16};
    case 'T': return {31,4,4,4,4,4,4};
    case 'X': return {17,17,10,4,10,17,17};
    case '-': return {0,0,0,31,0,0,0};
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
    const auto status = !snapshot.active ? color(255, 80, 80)
        : snapshot.afw_active ? color(50, 140, 255)
                              : color(64, 235, 112);
    const unsigned scale = std::min(width / 24, height / 12);
    const float measured = snapshot.delivered_fps;
    const float rate = std::isfinite(measured) ? measured : 0.0f;
    char text[4]{};
    const auto length = static_cast<unsigned>(snapshot.delivered_valid && std::isfinite(measured)
        ? std::snprintf(text, sizeof(text), "%d", static_cast<int>(std::clamp(rate, 0.0f, 999.0f) + 0.5f))
        : std::snprintf(text, sizeof(text), "--"));
    const std::string_view source = snapshot.eye_identity_source == EyeIdentitySource::metadata ? "META"
        : snapshot.eye_identity_source == EyeIdentitySource::pixel_markers ? "PIX"
        : snapshot.eye_identity_source == EyeIdentitySource::both ? "BOTH" : "";
    const auto label_scale = source.empty() ? 0u : scale / 2;
    const auto top = (height - 7 * scale - 8 * label_scale) / 2;
    const auto draw = [&](std::string_view line, unsigned line_top, unsigned size, std::uint32_t ink) {
        unsigned x = (width - (static_cast<unsigned>(line.size()) * 6 - 1) * size) / 2;
        for (const auto letter : line) {
            const auto rows = glyph(letter);
            for (unsigned y = 0; y < 7; ++y) for (unsigned column = 0; column < 5; ++column) {
                if ((rows[y] & (1u << (4 - column))) == 0) continue;
                for (unsigned sy = 0; sy < size; ++sy) for (unsigned sx = 0; sx < size; ++sx) {
                    const unsigned px = x + column * size + sx;
                    const unsigned py = line_top + y * size + sy;
                    pixels[static_cast<std::size_t>(py) * width + px] = ink;
                }
            }
            x += 6 * size;
        }
    };
    draw({text, length}, top, scale, status);
    if (label_scale) draw(source, top + 7 * scale + label_scale, label_scale, color(255, 255, 255));
    return pixels;
}

} // namespace xrfg
