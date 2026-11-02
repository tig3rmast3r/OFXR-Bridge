#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace xrfg {
// Learns only bounded, sub-slot delivery jitter near the display rate. A quiet
// window is success: it must not consume the margin which made it quiet.
// One presenter thread owns this object. No allocation, UI FPS or prediction
// lead participates in the feedback signal.
class OpticalPacingReserve {
public:
    std::int64_t observe(std::int64_t returned, std::int64_t intended,
                         std::int64_t period, std::int64_t maximum = 0, int minimum_rate_percent = 94) noexcept {
        if (period < 2'000'000 || period > 50'000'000) return budget_;
        if (period_ != period) { *this = {}; period_ = period; }
        if (!last_) { first_ = last_ = returned; return budget_; }
        const auto interval = returned - last_;
        last_ = returned;
        // A hitch/session discontinuity is not a request to buffer an entire
        // frame. Preserve any learned floor, restart the observation window.
        if (interval <= 0 || interval >= period * 4) {
            clear_window(returned);
            return budget_;
        }
        const auto late = returned - intended;
        lateness_[count_] = late > 0 && late < period ? late : 0;
        const auto tolerance = (std::max)(std::int64_t{350'000}, period / 20);
        if (interval > period + tolerance || interval < period - tolerance) ++jitter_;
        if (late > tolerance && late < period) ++late_;
        if (++count_ < lateness_.size()) return budget_;
        const auto elapsed = returned - first_;
        const auto expected = period * static_cast<std::int64_t>(count_);
        // 94–104% of the requested refresh. Sustained GPU/producer starvation
        // cannot be fixed by adding a timing reserve and must not train it.
        const bool near_full = elapsed > 0 && expected * 100 >= elapsed * minimum_rate_percent &&
            expected * 100 <= elapsed * 104;
        if (near_full && jitter_ >= 4 && late_ >= 4) {
            std::sort(lateness_.begin(), lateness_.end());
            const auto p90 = lateness_[lateness_.size() * 9 / 10];
            // Lateness is the residual AFTER the already learned advance.
            const auto wanted = (std::min)(maximum > 0 ? maximum : period * 3 / 4, budget_ + p90 + 250'000);
            // A half-millisecond step limits one noisy window's influence.
            // No automatic decay: backend/refresh/session changes reset it.
            if (wanted > budget_)
                budget_ = (std::min)(wanted, budget_ + 500'000);
        }
        clear_window(returned);
        return budget_;
    }
    [[nodiscard]] std::int64_t budget() const noexcept { return budget_; }
private:
    void clear_window(std::int64_t at) noexcept {
        first_ = at; count_ = jitter_ = late_ = 0;
    }
    std::array<std::int64_t, 64> lateness_{};
    std::int64_t first_{}, last_{}, period_{}, budget_{};
    unsigned count_{}, jitter_{}, late_{};
};
}
