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

// Uses actual completed work, never memory percentage alone. Guard with the
// session pressure mutex; resource lifetimes remain with the application path.
struct PrivateResourceRecovery {
    bool requested{},active{},drained{};
    unsigned slow{},healthy{},stable{},budget_slow{};
    std::uint64_t last_budget{},budget_value{};
    std::uint64_t last_slow{},retry_at{},last_real{};
    void completed(std::uint64_t us,bool paired,std::uint64_t now,std::uint64_t budget_us=0) noexcept {
        if(active || !paired)return;
        // Consecutive completed pairs missing the selected real-frame budget
        // can sustain a collapse without ever reaching the old 100ms trigger.
        // Allow 25% timing slack; a healthy pair, changed budget, or long gap
        // clears this streak. No memory percentage or prime can request it.
        const auto difference=budget_value>budget_us ? budget_value-budget_us : budget_us-budget_value;
        const bool changed=!budget_value || !budget_us || difference>budget_value/20;
        if(changed || now-last_budget>2000)budget_slow=0;
        budget_value=budget_us;last_budget=now;
        if(budget_us && us>budget_us+budget_us/4) {
            if(++budget_slow>=6)requested=true;
        } else budget_slow=0;
        if(us>=100000) {
            if(now-last_slow>10000)slow=0;
            last_slow=now;healthy=0;
            if(++slow>=3)requested=true;
        } else if(us<50000 && ++healthy>=8) {slow=0;healthy=0;}
    }
    void begin() noexcept {requested=false;active=true;drained=false;stable=0;last_real=0;}
    void released(std::uint64_t now) noexcept {drained=true;retry_at=now+3000;stable=0;last_real=0;}
    bool real_frame(std::uint64_t now,std::uint64_t real_ns,std::uint64_t limit_ns) noexcept {
        if(!active || !drained)return false;
        if(last_real && real_ns>last_real && real_ns-last_real<=limit_ns)++stable;
        else stable=0;
        last_real=real_ns;
        return now>=retry_at && stable>=16;
    }
    void resumed() noexcept {*this={};}
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
