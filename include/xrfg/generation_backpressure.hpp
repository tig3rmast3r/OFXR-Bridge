#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace xrfg {

[[nodiscard]] inline bool should_enter_generation_cooldown(
    bool nvidia_backend,
    std::chrono::steady_clock::duration synthetic_end_elapsed,
    std::int64_t display_period_nanoseconds) noexcept {
    if (!nvidia_backend || display_period_nanoseconds <= 0) {
        return false;
    }
    const auto elapsed_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            synthetic_end_elapsed).count();
    if (elapsed_nanoseconds <= 0) {
        return false;
    }

    constexpr std::int64_t kMinimumBackpressureNanoseconds = 75'000'000;
    constexpr std::int64_t kBackpressureDisplayPeriods = 6;
    const auto relative_limit =
        display_period_nanoseconds >
                std::numeric_limits<std::int64_t>::max() /
                    kBackpressureDisplayPeriods
            ? std::numeric_limits<std::int64_t>::max()
            : display_period_nanoseconds * kBackpressureDisplayPeriods;
    const auto backpressure_limit =
        relative_limit > kMinimumBackpressureNanoseconds
            ? relative_limit
            : kMinimumBackpressureNanoseconds;
    return elapsed_nanoseconds > backpressure_limit;
}

}  // namespace xrfg
