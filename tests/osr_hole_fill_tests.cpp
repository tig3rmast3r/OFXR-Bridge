#include "xrfg/osr_hole_fill.hpp"

#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
bool near(float a, float b) { return std::fabs(a - b) < 1e-5F; }
}

int main() {
    using xrfg::OsrPoint;
    const OsrPoint source{0, 0, 0}, destination{0.065F, 0, 0};
    // Standing still: the previous destination frame is the destination.
    check(near(xrfg::osr_history_parameter(source, destination, destination), 1.0F), "still: t = 1");
    check(!xrfg::osr_update_leading(false, 1.0F), "still: not leading");
    check(near(xrfg::osr_fill_fraction(3, true, 1.0F, false, 4.0F), 0.0F), "still: nothing to fill");
    // Trailing eye (moving away from the destination side): history is beyond.
    check(near(xrfg::osr_history_parameter(source, destination, OsrPoint{0.085F, 0, 0}), 1.3077F), "trailing: t > 1");
    check(!xrfg::osr_update_leading(true, 1.3077F), "trailing: stops leading");
    check(near(xrfg::osr_fill_fraction(3, true, 1.3077F, false, 4.0F), 0.0F), "trailing: nothing to fill");
    // Leading eye at a slow strafe: history sits between the eyes.
    const float slow = xrfg::osr_history_parameter(source, destination, OsrPoint{0.045F, 0, 0});
    check(near(slow, 0.6923F), "leading slow: t = 0.69");
    check(xrfg::osr_update_leading(false, slow), "leading slow: starts leading");
    check(near(xrfg::osr_fill_fraction(3, true, slow, true, 1.0F), 0.3077F), "gain 1: inner 31% of the band");
    check(near(xrfg::osr_fill_fraction(3, true, slow, true, 4.0F), 1.0F), "gain 4: whole band");
    check(near(xrfg::osr_fill_fraction(3, true, 0.9F, true, 4.0F), 0.4F), "gain 4 at t 0.9: 40%");
    // Leading eye at a fast strafe: history is behind the source.
    const float fast = xrfg::osr_history_parameter(source, destination, OsrPoint{-0.045F, 0, 0});
    check(fast < 0.0F, "leading fast: t < 0");
    check(near(xrfg::osr_fill_fraction(3, true, fast, true, 1.0F), 1.0F), "leading fast: whole band at gain 1");
    // Hysteresis: jitter between 0.90 and 0.98 keeps the current state.
    check(!xrfg::osr_update_leading(false, 0.95F), "jitter from still: stays still");
    check(xrfg::osr_update_leading(true, 0.95F), "jitter while leading: stays leading");
    check(xrfg::osr_update_leading(false, 0.85F), "clear lead: starts");
    check(!xrfg::osr_update_leading(true, 0.99F), "clear stop: stops");
    check(xrfg::osr_update_leading(true, NAN), "invalid parameter keeps state");
    // Vertical head motion does not change the along-axis parameter.
    check(near(xrfg::osr_history_parameter(source, destination, OsrPoint{0.065F, 0.5F, 0}), 1.0F), "orthogonal motion ignored");
    // Degenerate baseline: nothing to fill.
    check(near(xrfg::osr_history_parameter(source, source, destination), 1.0F), "no baseline: t = 1");
    // Provider modes.
    check(near(xrfg::osr_fill_fraction(1, true, 0.5F, false, 4.0F), 1.0F), "alternate-eye prime: whole band");
    check(near(xrfg::osr_fill_fraction(3, false, 0.5F, false, 4.0F), 1.0F), "no history: whole band");
    check(near(xrfg::osr_fill_fraction(2, true, 0.5F, true, 4.0F), 0.0F), "previous-frame only: no stereo band");
    check(near(xrfg::osr_fill_fraction(3, true, NAN, true, 4.0F), 1.0F), "invalid parameter: fill");
    if (failures) return 1;
    std::printf("osr_hole_fill_tests: ok\n");
    return 0;
}
