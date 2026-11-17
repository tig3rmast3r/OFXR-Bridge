#pragma once

#include <chrono>
#include <cstdint>
#include <limits>

namespace xrfg {

// Guarded by the session's pressure mutex. Memory is diagnostic only: resident
// generation/game resources may keep usage high even while synthesis is idle.
// Actual slow GPU completion starts a bounded cooldown, then allows a retry.
struct OpticalGpuPressure {
    bool memory_pressure{};
    std::uint64_t recover_after_ms{};
    void delayed(std::uint64_t now) noexcept { recover_after_ms = now + 2000; }
    bool completed(std::uint64_t elapsed_us, bool paired, std::uint64_t now) noexcept {
        if (!paired || elapsed_us < 100000) return false;
        delayed(now);
        return true;
    }
    void observe(std::uint64_t usage, std::uint64_t budget, std::uint64_t) noexcept {
        if (!budget) return;
        const long double ratio = static_cast<long double>(usage) / budget;
        memory_pressure = ratio >= .95L;
    }
    bool real_only(std::uint64_t now) const noexcept {
        return now < recover_after_ms;
    }
};

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
