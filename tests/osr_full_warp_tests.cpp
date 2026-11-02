#include "xrfg/osr_full_warp.hpp"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
}

int main() {
    using xrfg::osr_full_history_usable;
    // Luke alternates eyes: the destination eye's last real frame is one
    // publication old and is the frame the layer accepted for that eye.
    check(osr_full_history_usable(3, 101, 100, 100), "combined: previous publication of the peer eye");
    check(osr_full_history_usable(2, 101, 100, 100), "previous-frame mode uses the history too");
    check(!osr_full_history_usable(1, 101, 100, 100), "alternate-eye prime stays a pure stereo warp");
    check(!osr_full_history_usable(0, 101, 100, 100), "mode off never uses history");
    // The history must be the frame the layer last accepted for that eye.
    check(!osr_full_history_usable(3, 101, 0, 100), "no history recorded");
    check(!osr_full_history_usable(3, 101, 98, 100), "history older than the accepted frame: the copy did not happen");
    check(!osr_full_history_usable(3, 101, 100, 0), "layer reset its eye history");
    // Age.
    check(!osr_full_history_usable(3, 100, 100, 100), "same publication is not history");
    check(!osr_full_history_usable(3, 99, 100, 100), "future publication is not history");
    check(osr_full_history_usable(3, 103, 100, 100), "a skipped frame leaves a three-publication gap");
    check(osr_full_history_usable(3, 104, 100, 100), "age limit inclusive");
    check(!osr_full_history_usable(3, 105, 100, 100), "too old: the scene moved on");
    check(osr_full_history_usable(3, 110, 100, 100, 10), "age limit is a parameter");
    // Flags.
    check(xrfg::osr_full_flags(false, false) == 0, "no flags");
    check(xrfg::osr_full_flags(true, false) == xrfg::kOsrFullFlagDebug, "debug flag");
    check(xrfg::osr_full_flags(false, true) == (xrfg::kOsrFullFlagHistory | xrfg::kOsrFullFlagHistoryFirst), "history is primary by default");
    check(xrfg::osr_full_flags(false, true, false) == xrfg::kOsrFullFlagHistory, "history only at edges when asked");
    check(xrfg::osr_full_flags(true, true) == 7, "all flags");
    check(static_cast<int>(xrfg::OsrReconstruction::full) == 2, "Full OSR is the third Reconstruction entry");
    // Camera-attached distance. Cyberpunk's reversed projection, near 0.02:
    // depth = 0.02 / distance.
    {
        float cyberpunk[16]{0.8693F, 0, 0, 0, 0, 1.0355F, 0, 0, 0, 0, 0, 1, 0, 0, 0.02F, 0};
        check(std::abs(xrfg::osr_full_depth_at(cyberpunk, 0.6F) - 0.02F / 0.6F) < 1e-6F, "reversed depth at 0.6 m");
        check(xrfg::osr_full_depth_at(cyberpunk, 0.6F) < xrfg::osr_full_depth_at(cyberpunk, 0.3F), "nearer is deeper in reversed depth");
        check(xrfg::osr_full_depth_at(cyberpunk, 0.0F) == 2.0F, "unknown units: nothing attached by distance");
        check(xrfg::osr_full_depth_at(nullptr, 0.6F) == 2.0F, "no projection: nothing attached by distance");
        float looking_down_minus_z[16]{0.8693F, 0, 0, 0, 0, 1.0355F, 0, 0, 0, 0, 0, -1, 0, 0, 0.02F, 0};
        check(std::abs(xrfg::osr_full_depth_at(looking_down_minus_z, 0.6F) - 0.02F / 0.6F) < 1e-6F, "-z forward view space");
    }
    if (failures) return 1;
    std::printf("osr_full_warp_tests: ok\n");
    return 0;
}
