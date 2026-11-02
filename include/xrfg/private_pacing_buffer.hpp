#pragma once
#include "optical_pacing_reserve.hpp"

namespace xrfg {
// Same near-refresh learner as native VDXR. A larger FIFO alone does not
// build ready lead. The extra retained period is learned only at >=99% refresh.
// Refill only the newly learned increment, once; never make
// every already-ready packet pay an acceptedAt + historical-work floor.
class PrivatePacingBuffer {
    OpticalPacingReserve learner_;
    std::int64_t period_{},pending_{};
public:
    void period(std::int64_t value) noexcept {
        if(value!=period_) {*this={};period_=value;}
    }
    [[nodiscard]] std::int64_t reserve() const noexcept {return learner_.budget();}
    [[nodiscard]] std::int64_t take_refill() noexcept {
        const auto value=pending_;pending_=0;return value;
    }
    std::int64_t observe(std::int64_t returned,std::int64_t intended) noexcept {
        const auto old=reserve();
        const auto value=learner_.observe(returned,intended,period_,period_*7/4,99);
        pending_+=value-old;return value-old;
    }
};
}
