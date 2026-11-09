#include "xrfg/fps_overlay_model.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }

int main(int argc, char** argv) {
    try {
        using namespace xrfg;
        require(displayed_rate(88.6F, 90) == 90 && displayed_rate(86.9F, 90) == 86.9F &&
            displayed_rate(89, 0) == 89 && !std::isfinite(displayed_rate(std::numeric_limits<float>::infinity(),90)),
            "refresh display rounding hid a real loss or invalid measurement");
        FpsCounter counter;
        require(!counter.snapshot(0).active, "startup green");
        for (int i = 0; i < 45; ++i) {
            const auto t = static_cast<std::int64_t>(i) * 1'000'000'000 / 45;
            counter.submitted(t, true);
            counter.submitted(t + 10'000'000, false);
        }
        auto snapshot = counter.snapshot(999'999'999);
        require(std::abs(snapshot.submitted_fps - 90) < 0.1f && snapshot.active,
                "output must count both originals and synthetics");
        counter.submitted(1'100'000'000, false);
        require(counter.snapshot(1'100'000'000).active, "S/B status flicker");
        require(!counter.snapshot(1'600'000'000).active, "fallback fails to expire green");
        require(counter.snapshot(3'000'000'000).submitted_fps == 0, "stale output FPS");
        counter.reset();
        counter.submitted(4'000'000'000, false);
        require(!counter.snapshot(4'050'000'000).active, "prime/repeat must not be green");
        FpsCounter deferred;
        deferred.submitted(100'000'000,false);
        deferred.submitted(300'000'000,true);
        deferred.submitted(50'000'000,true); // Earlier S confirmed after a newer S.
        require(deferred.snapshot(700'000'000).active,
            "late GPU confirmation must not replace the newer synthetic timestamp");
        FpsCounter unique;
        for (int i = 0; i < 144; ++i) {
            const auto t = static_cast<std::int64_t>(i) * 1'000'000'000 / 144;
            unique.submitted(t, i % 2 == 0, i < 78);
        }
        require(std::abs(unique.snapshot(999'999'999).submitted_fps - 78) < 0.1f,
            "66 repeated submissions must not inflate 78 new outputs to 144");
        unique.submitted(2'000'000'000, true, false);
        require(!unique.snapshot(2'000'000'000).active, "repeated synthetic must not extend green status");
        FpsCounter aer;
        for(int i=0;i<90;++i) {
            const auto t=std::int64_t(i)*1'000'000'000/90;
            aer.submitted(t,i%2==0,true,true);
            aer.submitted(t,true,false,true); // repeated presentation is not new content
        }
        const auto mixed=aer.snapshot(999'999'999);
        require(mixed.active && mixed.aer_eye_equivalents,"mixed AER output is generating");
        require(std::abs(mixed.submitted_fps-90)<.1F &&
            std::abs(mixed.real_fps-45)<.1F && std::abs(mixed.generated_fps-45)<.1F,
            "90 one-real/one-generated-eye outputs are 45+45 stereo equivalents, never 0+90 or 90+90");
        require(!aer.snapshot(3'000'000'000).aer_eye_equivalents,"mixed labels expire with their samples");
        require(rasterize_fps_overlay(24, 12, {144, true, false, 78, true}, false) ==
            rasterize_fps_overlay(24, 12, {0,true,false,78,true}, false), "fresh delivered count must override submissions");
        for (auto position : {FpsOverlayPosition::off, FpsOverlayPosition::upper_left,
            FpsOverlayPosition::upper_right, FpsOverlayPosition::lower_left, FpsOverlayPosition::lower_right}) {
            require(parse_overlay_position(overlay_position_name(position)) == position, "position round trip");
            if (position == FpsOverlayPosition::off) continue;
            const auto p = overlay_placement(position, -2, 2, -1.5f, 1.5f);
            const bool left = position == FpsOverlayPosition::upper_left || position == FpsOverlayPosition::lower_left;
            const bool upper = position == FpsOverlayPosition::upper_left || position == FpsOverlayPosition::upper_right;
            require((p.x < 0) == left && (p.y > 0) == upper, "wrong quadrant");
            require(std::abs(p.x) + p.width / 2 < 2 && std::abs(p.y) + p.height / 2 < 2,
                    "not inset in wide FOV");
            require(std::abs(std::abs(p.x) - 0.88f) < 0.0001f &&
                    std::abs(std::abs(p.y) - 0.78f) < 0.0001f,
                    "number must use the new inward position in every corner");
            require(std::abs(p.width - 0.192f) < 0.0001f &&
                    std::abs(p.height - 0.096f) < 0.0001f && p.z == -2.0f,
                    "number must be 20 percent smaller without changing depth");
            const auto asymmetric = overlay_placement(position, -0.65f, 0.9f, -0.75f, 0.8f);
            constexpr float span = 1.55f, anchor_width = span * 0.06f;
            constexpr float anchor_height = anchor_width / 2.0f;
            const float expected_x = 2.0f * (left ? -0.65f + span * 0.25f + anchor_width * 0.5f
                                                  : 0.9f - span * 0.25f - anchor_width * 0.5f);
            const float expected_y = 2.0f * (upper ? 0.8f - span * 0.29f - anchor_height * 0.5f
                                                  : -0.75f + span * 0.29f + anchor_height * 0.5f);
            require(std::abs(asymmetric.x - expected_x) < 0.0001f &&
                    std::abs(asymmetric.y - expected_y) < 0.0001f &&
                    std::abs(asymmetric.width - anchor_width * 1.6f) < 0.0001f &&
                    std::abs(asymmetric.height - anchor_height * 1.6f) < 0.0001f,
                    "smaller number must retain the inward asymmetric-FOV centre");
        }
        require(overlay_texture_width(4000) > overlay_texture_width(1600), "resolution adaptation");
        require(overlay_texture_width(1) == 96 && overlay_texture_width(UINT32_MAX) == 336, "texture bounds");
        const auto unavailable = rasterize_fps_overlay(240,120,snapshot,false);
        snapshot.delivered_valid=true;snapshot.delivered_fps=90;
        const auto green = rasterize_fps_overlay(240, 120, snapshot, false);
        require(green!=unavailable,"unknown delivery must not fall back to submissions");
        auto afw = snapshot; afw.afw_active = true;
        const auto blue = rasterize_fps_overlay(240, 120, afw, false);
        auto inactive = snapshot; inactive.active = false;
        const auto red = rasterize_fps_overlay(240, 120, inactive, false);
        require(std::count(green.begin(), green.end(), 0xff70eb40u) > 100, "green glyphs");
        require(std::count(blue.begin(), blue.end(), 0xffff8c32u) > 100,
                "AFW plus OFXR must be blue");
        require(std::count(red.begin(), red.end(), 0xff5050ffu) > 100, "red glyphs");
        for (std::size_t i = 0; i < green.size(); ++i) {
            require(green[i] == 0 || green[i] == 0xff70eb40u, "extra text or opaque background");
            require(red[i] == (green[i] ? 0xff5050ffu : 0), "active/inactive changes more than digit color");
        }
        require(std::count(green.begin(), green.end(), 0u) > green.size() / 2,
                "background must be fully transparent");
        const auto bgra = rasterize_fps_overlay(240, 120, inactive, true);
        require(std::count(bgra.begin(), bgra.end(), 0xffff5050u) > 100, "BGRA color order");
        // Exact oracle: the whole minimal image must contain only the digits 90.
        const auto minimal = rasterize_fps_overlay(24, 12, snapshot, false);
        const unsigned rows[2][7]{{14,17,17,15,1,1,14}, {14,17,19,21,25,17,14}};
        for (unsigned y = 0; y < 12; ++y) for (unsigned x = 0; x < 24; ++x) {
            bool stroke = false;
            for (unsigned digit = 0; digit < 2; ++digit) {
                const auto left = 6 + digit * 6;
                if (x >= left && x < left + 5 && y >= 2 && y < 9)
                    stroke = stroke || (rows[digit][y - 2] & (1u << (4 - (x - left))));
            }
            require(minimal[y * 24 + x] == (stroke ? 0xff70eb40u : 0u), "must render only output FPS number");
        }
        require(rasterize_fps_overlay(24, 12, {0,true,false,90.49f,true}, false) == minimal, "FPS rounding down");
        require(rasterize_fps_overlay(24, 12, {0,true,false,90.6f,true}, false) ==
                rasterize_fps_overlay(24, 12, {0,true,false,91,true}, false), "FPS rounding up");
        for (float value : {-1.0f, std::numeric_limits<float>::quiet_NaN()})
            require(rasterize_fps_overlay(24, 12, {value, false}, false) ==
                    rasterize_fps_overlay(24, 12, {0, false}, false), "invalid FPS clamp");
        require(rasterize_fps_overlay(24, 12, {0,true,false,1000,true}, false) ==
                rasterize_fps_overlay(24, 12, {0,true,false,999,true}, false), "three-digit FPS clamp");
        // Optional visual artifact, not a live image or runtime dependency.
        if (argc > 1) {
            std::ofstream file(argv[1], std::ios::binary);
            file << "P6\n240 240\n255\n";
            // Checkerboard is preview-only, to visualize transparent pixels.
            for (const auto* pixels : {&green, &red}) for (unsigned y = 0; y < 120; ++y) for (unsigned x = 0; x < 240; ++x) {
                const auto pixel = (*pixels)[y * 240 + x];
                const char checker = ((x / 12 + y / 12) % 2) ? 48 : 32;
                const char rgb[]{pixel ? static_cast<char>(pixel) : checker,
                    pixel ? static_cast<char>(pixel >> 8) : checker,
                    pixel ? static_cast<char>(pixel >> 16) : checker};
                file.write(rgb, 3);
            }
        }
        std::cout << "Output FPS, active/inactive hysteresis, positions, number-only and transparent raster tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
