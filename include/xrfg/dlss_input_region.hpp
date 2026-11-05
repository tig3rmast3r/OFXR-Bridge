#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace xrfg {

// Ported from 0.2 V521 (5d09020). An eye image is the game's render of its
// own field, so an eye pixel maps to the origin of its DLSS input region, not
// to wherever an upscaler chose to start reading. PureDark's Skyrim VR
// upscaler keeps its foveated offsets (0.02, 0.04) at foveated scale 1.0: it
// reads from (56, 88) and (2768, 88) of 2824-wide regions while its output
// rectangle says (0, 0), and pastes its output back by the same offset.
// Vectors read at the published base sat about 97 px across and 152 px down
// from their pixel: a halo round everything that moved against its background.
//
// The rule, per axis, with no game name: the input start the output's
// placement does not explain (base - output_base * extent / output_extent)
// must lie off the grid of region-sized cells by at least one texel and at
// most a tenth of a region. 0.3 adds one guard to 0.2's rule: a base at the
// texture's half, or a region flush with its far edge, is an eye's allocation
// slot and never an offset. At render scales between 0.9 and 1.0 such a slot
// lies within a tenth of a region of the grid.
// Returns the texels to remove from base; 0 leaves it as published.
[[nodiscard]] inline long dlss_input_offset_within_region(std::uint32_t base,
    std::uint32_t output_base, std::uint32_t extent, std::uint32_t output_extent,
    std::uint64_t texture_extent) noexcept {
    if (extent == 0 || output_extent == 0 || texture_extent == 0) return 0;
    if (std::uint64_t(base) * 2 == texture_extent ||
        std::uint64_t(base) + extent == texture_extent) return 0;
    const double region = static_cast<double>(extent);
    const double start = static_cast<double>(base) -
        static_cast<double>(output_base) * region / static_cast<double>(output_extent);
    const double offset = start - std::round(start / region) * region;
    if (std::abs(offset) < 0.5 || std::abs(offset) > 0.1 * region) return 0;
    const long removed = std::lround(offset);
    const long long moved = static_cast<long long>(base) - removed;
    if (moved < 0 || static_cast<std::uint64_t>(moved) + extent > texture_extent) return 0;
    return removed;
}

// A depth texture exactly two eyes wide holds the eyes' depth at that size,
// whatever smaller rectangle the upscaler reports. PureDark's Skyrim VR
// upscaler reports 2824 x 2212 of a 9740 x 3813 texture for both inputs; its
// vectors are that size, its depth fills 4870 x 3813 per eye. Read as
// reported, depth was zoomed 1.72 times and the right eye ran into the left's
// (F8 captures). Elsewhere a depth texture is sized to its render.
// Takes the reported rectangle, returns false to leave it as it is.
[[nodiscard]] inline bool dlss_depth_fills_eye_slot(std::uint32_t& x, std::uint32_t& y,
    std::uint32_t& width, std::uint32_t& height, std::uint32_t output_width,
    std::uint32_t output_height, std::uint32_t texture_width,
    std::uint32_t texture_height) noexcept {
    if (width == 0 || height == 0 || output_width == 0 || texture_height != output_height ||
        width >= output_width || height >= output_height ||
        std::uint64_t(texture_width) != std::uint64_t(output_width) * 2U) return false;
    x = std::lround(static_cast<double>(x) / static_cast<double>(width)) == 1 ? output_width : 0U;
    y = 0;
    width = output_width;
    height = output_height;
    return true;
}

}  // namespace xrfg
