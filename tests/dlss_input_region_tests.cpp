#include "xrfg/dlss_input_region.hpp"
#include <iostream>

namespace {
unsigned checks{}, failures{};
void expect(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << message << '\n'; }
}
}  // namespace

int main() {
    using xrfg::dlss_input_offset_within_region;
    // Skyrim VR, PureDark's upscaler with its foveated offsets at scale 1.0,
    // as the v852 d3d11_stream records show it (2026-11-04): 2824 x 2212 read
    // at (56, 88) and (2768, 88) of 9740 x 3813 vector and depth textures,
    // output 4870 x 3813 at (0, 0).
    expect(dlss_input_offset_within_region(56, 0, 2824, 4870, 9740) == 56, "left eye x");
    expect(dlss_input_offset_within_region(2768, 0, 2824, 4870, 9740) == -56, "right eye x");
    expect(dlss_input_offset_within_region(88, 0, 2212, 3813, 3813) == 88, "y");
    // On the grid: nothing.
    expect(dlss_input_offset_within_region(0, 0, 2824, 4870, 9740) == 0, "left at origin");
    expect(dlss_input_offset_within_region(2824, 0, 2824, 4870, 9740) == 0, "right at its region");
    // An eye at the half of a larger allocation, a third of a region off.
    expect(dlss_input_offset_within_region(4870, 0, 2824, 4870, 9740) == 0, "allocation half");
    // An output placed where its input is: consistent, nothing to remove.
    expect(dlss_input_offset_within_region(58, 100, 2824, 4870, 9740) == 0, "placed output");
    expect(dlss_input_offset_within_region(100, 100, 4870, 4870, 9740) == 0, "full resolution");
    expect(dlss_input_offset_within_region(10, 0, 0, 4870, 9740) == 0, "no extent");
    // 0.3's guard: side-by-side eyes in one double-wide target at a render
    // scale of 0.95, the right eye at the texture's half. 0.2's rule read it
    // as 243 texels off its grid.
    expect(dlss_input_offset_within_region(4870, 0, 4627, 4870, 9740) == 0, "slot at texture half");
    expect(dlss_input_offset_within_region(4870, 4870, 4627, 4870, 9740) == 0,
        "slot at texture half, placed output");
    // A region flush with the texture's far edge is a slot too.
    expect(dlss_input_offset_within_region(5113, 0, 4627, 4870, 9740) == 0, "flush with far edge");
    // A move that would leave the texture is refused.
    expect(dlss_input_offset_within_region(2768, 0, 2824, 4870, 5600) == 0, "moved past the texture");

    // Depth the size of the eyes, reported as the vectors' rectangle: Skyrim.
    using xrfg::dlss_depth_fills_eye_slot;
    std::uint32_t x = 56, y = 88, w = 2824, h = 2212;
    expect(dlss_depth_fills_eye_slot(x, y, w, h, 4870, 3813, 9740, 3813) &&
        x == 0 && y == 0 && w == 4870 && h == 3813, "left eye's slot");
    x = 2768; y = 88; w = 2824; h = 2212;
    expect(dlss_depth_fills_eye_slot(x, y, w, h, 4870, 3813, 9740, 3813) &&
        x == 4870 && y == 0 && w == 4870 && h == 3813, "right eye's slot");
    x = 10; y = 10; w = 2824; h = 2212;
    expect(!dlss_depth_fills_eye_slot(x, y, w, h, 4870, 3813, 4870, 3813) && x == 10 && w == 2824,
        "one eye's texture stays as reported");
    // Depth sized to its render stays as reported: Ready or Not, and the rest.
    x = 2840; y = 0; w = 2837; h = 2223;
    expect(!dlss_depth_fills_eye_slot(x, y, w, h, 4172, 3268, 5680, 2224) && x == 2840 && w == 2837,
        "a render-sized texture");
    x = 0; y = 0; w = 4870; h = 3813;
    expect(!dlss_depth_fills_eye_slot(x, y, w, h, 4870, 3813, 9740, 3813), "already the whole eye");
    std::cout << checks - failures << '/' << checks << " dlss input region checks passed\n";
    return failures == 0 ? 0 : 1;
}
