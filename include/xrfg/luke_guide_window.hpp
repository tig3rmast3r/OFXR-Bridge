#pragma once

#include "xrfg/luke_projection_ray.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace xrfg {

enum class LukeGuideWindowFailure : std::uint32_t {
    none, input_extent, nonfinite_matrix, projected_edges, outside_output,
    color_width_mismatch, color_height_mismatch, guide_extent, guide_origin, guide_bounds
};

struct LukeGuideWindow {
    std::uint32_t x{}, y{}, width{}, height{};
    bool raster_x{}, raster_y{};
    // The projected origin before rounding, in guide texels: x + fraction_x is
    // where the eye's left edge really falls. Sub-texel consumers (Full OSR)
    // add the fraction back; the integer crop is unchanged.
    float fraction_x{}, fraction_y{};
};

// Cropping preserves native pixel units. Integer ROI rounding must not change
// the accepted full-output/guide gain; a genuinely resized axis keeps its
// color/cropped-guide conversion. Callers already validate all extents.
[[nodiscard]] inline float luke_guide_motion_gain(float raw_scale,
    std::uint32_t output_extent, std::uint32_t guide_extent,
    std::uint32_t color_extent, std::uint32_t cropped_extent,
    bool raster_matched) noexcept {
    return raster_matched
        ? raw_scale * static_cast<float>(output_extent) / static_cast<float>(guide_extent)
        : raw_scale * static_cast<float>(color_extent) / static_cast<float>(cropped_extent);
}

// Project the physical OpenXR FOV through the producer's full DLSS matrix.
// The resulting ROI is in the guide's own pixel space, so it scales with any
// render/output extent and is not a title-specific pixel offset.
[[nodiscard]] inline std::optional<LukeGuideWindow> luke_guide_window(
    const std::array<float, 16>& projection,
    float angle_left, float angle_right, float angle_up, float angle_down,
    std::uint32_t output_width, std::uint32_t output_height,
    std::uint32_t guide_width, std::uint32_t guide_height,
    std::uint32_t color_width, std::uint32_t color_height,
    LukeGuideWindowFailure* failure = nullptr) noexcept {
    if (failure) *failure = LukeGuideWindowFailure::none;
    const auto reject = [&](LukeGuideWindowFailure reason) -> std::optional<LukeGuideWindow> {
        if (failure) *failure = reason;
        return std::nullopt;
    };
    if (!output_width || !output_height || !guide_width || !guide_height ||
        !color_width || !color_height) return reject(LukeGuideWindowFailure::input_extent);
    for (float value : projection)
        if (!std::isfinite(value)) return reject(LukeGuideWindowFailure::nonfinite_matrix);
    const double left = luke_projection_ray_ndc(std::tan(angle_left), projection[0],
        projection[8], projection[3], projection[11]);
    const double right = luke_projection_ray_ndc(std::tan(angle_right), projection[0],
        projection[8], projection[3], projection[11]);
    const double top = luke_projection_ray_ndc(std::tan(angle_up), projection[5],
        projection[9], projection[7], projection[11]);
    const double bottom = luke_projection_ray_ndc(std::tan(angle_down), projection[5],
        projection[9], projection[7], projection[11]);
    if (!std::isfinite(left) || !std::isfinite(right) ||
        !std::isfinite(top) || !std::isfinite(bottom) ||
        left >= right || bottom >= top) return reject(LukeGuideWindowFailure::projected_edges);
    const double x0 = (left + 1.0) * output_width * 0.5;
    const double x1 = (right + 1.0) * output_width * 0.5;
    const double y0 = (1.0 - top) * output_height * 0.5;
    const double y1 = (1.0 - bottom) * output_height * 0.5;
    // FOV defines the native sampling footprint. Luke's final eye may be
    // resized or raster-aligned independently of the DLSS output; its texture
    // dimensions do not define a different FOV or a different native crop.
    if (x0 < -1.0 || y0 < -1.0 || x1 > output_width + 1.0 || y1 > output_height + 1.0)
        return reject(LukeGuideWindowFailure::outside_output);
    const bool raster_x = color_width <= output_width &&
        std::abs((x1-x0)-double(color_width)) <= 1.0;
    const bool raster_y = color_height <= output_height &&
        std::abs((y1-y0)-double(color_height)) <= 1.0;
    const auto extent = [](std::uint32_t guide, double span,std::uint32_t color,
        std::uint32_t output, bool raster_matched) noexcept {
        // Preserve the accepted raster contract when physical FOV and actual
        // integer color differ by at most one output pixel. Floating-point
        // ceil would otherwise add a guide texel to an already rounded span.
        // A genuinely resized eye still uses its projected sampling footprint.
        if(raster_matched)
            return static_cast<std::uint32_t>((std::uint64_t(guide)*color+output-1)/output);
        return static_cast<std::uint32_t>(std::ceil(std::min(span,double(output))*guide/output));
    };
    const std::uint32_t width = extent(guide_width, x1-x0,
        color_width,output_width,raster_x);
    const std::uint32_t height = extent(guide_height, y1-y0,
        color_height,output_height,raster_y);
    if (!width || !height || width > guide_width || height > guide_height)
        return reject(LukeGuideWindowFailure::guide_extent);
    const auto rounded_x = std::llround(x0 * guide_width / output_width);
    const auto rounded_y = std::llround(y0 * guide_height / output_height);
    if (rounded_x < 0 || rounded_y < 0 ||
        rounded_x > guide_width || rounded_y > guide_height)
        return reject(LukeGuideWindowFailure::guide_origin);
    // Quantizing the viewport origin and extent independently can put the
    // final texel one pixel past the resource edge. Keep that last texel in
    // bounds without changing the projected span or stretching the image.
    if (rounded_x > static_cast<std::int64_t>(guide_width - width) + 1 ||
        rounded_y > static_cast<std::int64_t>(guide_height - height) + 1)
        return reject(LukeGuideWindowFailure::guide_bounds);
    LukeGuideWindow result{
        static_cast<std::uint32_t>(std::min<std::int64_t>(rounded_x,
            guide_width - width)),
        static_cast<std::uint32_t>(std::min<std::int64_t>(rounded_y,
            guide_height - height)),
        width, height, raster_x, raster_y};
    result.fraction_x = static_cast<float>(x0 * guide_width / output_width - double(result.x));
    result.fraction_y = static_cast<float>(y0 * guide_height / output_height - double(result.y));
    return result;
}

}  // namespace xrfg
