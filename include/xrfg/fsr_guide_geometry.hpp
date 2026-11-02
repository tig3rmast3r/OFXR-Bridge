#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include <cmath>
#include <limits>

namespace xrfg {
struct FsrGuideRegions {
    std::array<std::uint32_t, 4> motion, depth;
};

// FSR copies guide texels into its existing private render-size inputs. A
// camera-placed periphery may be smaller than the XR colour without needing
// another resampling pass. Fractional crops cannot be represented by that copy
// and must be refused, rather than silently rounded into a different field.
[[nodiscard]] inline std::optional<FsrGuideRegions> fsr_guide_regions(
    const DlssMotionVectorFrame& frame) noexcept {
    const auto region = [](const std::array<float, 4>& rect,
        std::uint32_t x, std::uint32_t y, std::uint32_t width,
        std::uint32_t height) -> std::optional<std::array<std::uint32_t, 4>> {
        std::array<std::uint32_t, 4> out{};
        for (std::size_t i = 0; i < rect.size(); ++i) {
            const double value = rect[i], rounded = std::round(value);
            if (!std::isfinite(value) || std::abs(value - rounded) > 1e-3 ||
                rounded < 0 || rounded > std::numeric_limits<std::uint32_t>::max()) return {};
            out[i] = static_cast<std::uint32_t>(rounded);
        }
        if (!out[2] || !out[3] || out[0] < x || out[1] < y ||
            std::uint64_t(out[0]) + out[2] > std::uint64_t(x) + width ||
            std::uint64_t(out[1]) + out[3] > std::uint64_t(y) + height) return {};
        return out;
    };
    const auto motion = region(guide_motion_rect(frame), frame.motion_x,
        frame.motion_y, frame.motion_width, frame.motion_height);
    const auto depth = region(guide_depth_rect(frame), frame.depth_x,
        frame.depth_y, frame.depth_width, frame.depth_height);
    if (!motion || !depth) return {};
    return FsrGuideRegions{*motion, *depth};
}
} // namespace xrfg
