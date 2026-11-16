#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace xrfg {
// Absolute producer appointments. Missing a tick never creates a catch-up burst.
// Used only before rendering. Recovery may defer a future appointment, never
// hold a rendered image or trigger a catch-up burst.
class IndependentSourceClock {
    std::int64_t next_{},interval_{};
public:
    std::int64_t claim(std::int64_t now,std::int64_t physical,unsigned divisor) noexcept {
        physical=std::clamp<std::int64_t>(physical,2'000'000,50'000'000);
        const auto interval=physical*std::clamp(divisor,1U,4U);
        if(interval_!=interval || !next_){interval_=interval;next_=now;}
        if(now>next_+interval/4)next_=now;
        const auto target=std::max(now,next_);next_+=interval;return target;
    }
    void defer_to(std::int64_t at) noexcept {next_=std::max(next_,at+interval_);}
    void postpone(std::int64_t delta) noexcept {if(next_)next_+=std::max(std::int64_t{},delta);}
    void reset() noexcept {*this={};}
};
// Recovery uses a short evidence window, not an unbroken multi-second streak.
// A single observation with no ready image is not proof of lost native cadence.
// Real progress must cover99% of physical slots over each half-second window;
// no progress, a long pause or a changed period invalidates qualification.
class IndependentRecoveryWindow {
    std::int64_t period_{},started_{},last_{};
    std::uint64_t good_{};
    bool qualified_{};
public:
    void reset() noexcept {*this={};}
    bool observe(std::int64_t now,std::int64_t period,std::uint64_t good) noexcept {
        if(period<=0 || now<=0){reset();return false;}
        if(period_!=period || !last_ || now<=last_ || now-last_>period*8) {
            reset();period_=period;started_=last_=now;return false;
        }
        last_=now;good_+=good;
        const auto elapsed=now-started_;
        if(elapsed>=500'000'000) {
            qualified_=good_*static_cast<std::uint64_t>(period)*100>=static_cast<std::uint64_t>(elapsed)*99;
            started_=now;good_=0;
        }
        return qualified_;
    }
};
// Consume surplus ready ownership by postponing future source appointments,
// never by accelerating native End or discarding rendered images. Qualification
// survives isolated empty-ready/idle samples. Each actual step still needs two
// ready outputs, fresh native progress and measured residence above the target.
class IndependentSourcePhase {
    std::int64_t period_{},last_{},trimmed_{};
    std::uint64_t delivered_{},expired_{},idle_{};
    IndependentRecoveryWindow recovery_;
public:
    void reset() noexcept {*this={};}
    std::int64_t trimmed() const noexcept {return trimmed_;}
    std::int64_t observe(std::int64_t now,std::int64_t physical,unsigned divisor,
        std::size_t ready,std::int64_t residence,std::uint64_t delivered,
        std::uint64_t expired,std::uint64_t idle) noexcept {
        if(divisor!=2 || physical<2'000'000 || physical>50'000'000 || now<=0){reset();return 0;}
        if(period_!=physical || delivered<delivered_ || expired<expired_ || idle<idle_) {
            reset();period_=physical;
        }
        if(last_ && now<=last_)return 0;
        const auto elapsed=last_?now-last_:0;last_=now;
        const auto fresh=delivered-delivered_;
        const auto lost=expired-expired_;
        const bool missed=lost || idle!=idle_;
        delivered_=delivered;expired_=expired;idle_=idle;
        const bool healthy=recovery_.observe(now,physical,fresh-std::min(fresh,lost));
        const auto target=physical/2+2'000'000;
        if(!healthy || !fresh || missed || !elapsed || elapsed>physical*8 || ready<2 || residence<=target)return 0;
        // At most8ms/second and200us per source. Closed-loop ready ownership
        // and residence end recovery; a lifetime one-P cap cannot recover a
        // second menu transient. No accumulated catch-up step after a veto.
        const auto step=std::min({elapsed*8/1000,std::int64_t{200'000},residence-target});
        trimmed_+=step;return step;
    }
};
// Automatic final-delivery allowance, not a generator sleep. Deadline losses
// earn margin quickly; only successful, comfortably early deliveries can retire
// it. Counting delivered frames alone cannot distinguish spare time from a
// provider failure, priming, or a runtime that no longer wants to render.
inline constexpr unsigned kIndependentMaximumReserveFrames=4;
class IndependentDeliveryReserve {
    std::int64_t reserve_{},refill_{},peak_{},limit_{};
    std::int64_t last_delivery_{};
    IndependentRecoveryWindow recovery_;
    unsigned late_{},window_{};
    void unstable() noexcept {last_delivery_=0;recovery_.reset();}
public:
    void set_limit(std::int64_t limit) noexcept {
        limit=std::max(std::int64_t{},limit);
        if(limit<reserve_) {
            refill_=std::max(std::int64_t{},refill_-(reserve_-limit));
            reserve_=limit;
        }
        limit_=limit;
        if(!limit_){*this={};}
    }
    std::int64_t reserve() const noexcept {return reserve_;}
    std::int64_t take_refill() noexcept {auto n=refill_;refill_=0;return n;}
    void observe(std::int64_t available,std::int64_t expected,std::int64_t period) noexcept {
        if(!limit_ || period<=0 || !expected)return;
        const auto late=available-expected;
        // Loading or a disconnected runtime is not evidence for a larger
        // buffer. Keep the established margin instead of suddenly dropping it.
        if(late>period*8){unstable();late_=window_=0;peak_=0;return;}
        if(late>period/2) {
            unstable();peak_=std::max(peak_,late);++late_;
            if(late_>=2) {
                const auto missed=(peak_+period/2)/period;
                const auto next=std::min(limit_,reserve_+missed*period);
                refill_+=std::max(std::int64_t{},next-reserve_);reserve_=next;
                late_=window_=0;peak_=0;
            }
        }
        if(++window_>=32){late_=window_=0;peak_=0;}
    }
    void delivered(std::int64_t ready,std::int64_t expected,
                   std::int64_t entered,std::int64_t period) noexcept {
        if(!limit_ || period<=0 || !ready || !expected || !entered)return;
        const auto slack=expected-ready;
        const bool spare=slack>=period/2 && entered<=expected+period/4;
        const auto previous=last_delivery_;last_delivery_=entered;
        const bool healthy=recovery_.observe(entered,period,spare?1:0);
        if(!healthy || !spare || !previous || entered<=previous || entered-previous>period*8)return;
        // Retire margin promptly once actual native deliveries prove spare
        // readiness. The current frame still vetoes unsafe cuts. In particular
        // this stops before shrinking tolerance underneath a queued late frame;
        // source-phase recovery must remove that age first.
        const auto step=std::min({(entered-previous)*8/1000,
            std::int64_t{200'000},slack-period/2});
        reserve_=std::max(std::int64_t{},reserve_-std::max(std::int64_t{},step));
    }
};
inline bool independent_expired(std::int64_t due,std::int64_t now,std::int64_t allowance,std::int64_t period) noexcept {
    // Due identifies a nominal physical slot. Test the current decision time,
    // never a future window end: that could reject an image before it is due.
    return due>0 && now>due+std::max(std::int64_t{},allowance)+period/2;
}
inline std::int64_t independent_window_end(std::int64_t returned,std::int64_t predicted,std::int64_t period) noexcept {
    // Some runtimes predict several periods ahead. Never turn that pose
    // horizon into additional buffering; use at most the current refresh.
    return predicted>0?std::min(predicted,returned+period):returned+period;
}
// Freeze the first output slot before generation, on the observed native grid.
// Rounding identifies a real slot; it never sleeps or advances a stale source.
inline std::int64_t independent_first_deadline(std::int64_t source,std::int64_t period,std::int64_t cycle) noexcept {
    const auto earliest=source+period;
    if(!cycle || period<=0)return earliest;
    const auto delta=earliest-cycle;
    const auto slots=delta>=0?(delta+period-1)/period:delta/period;
    return cycle+slots*period;
}
// Final-consumer clock only. Native period + one seeded phase form an absolute
// grid. Native entry work never changes the next appointment. A blocking Wait
// already supplies pacing; an immediately returning Wait uses this grid.
class IndependentDeliveryClock {
    std::int64_t period_{},target_{},last_handoff_{};
    bool delivered_{};
public:
    std::int64_t claim(std::int64_t now,std::int64_t period) noexcept {
        period=std::clamp<std::int64_t>(period,2'000'000,50'000'000);
        if(period_!=period || !target_){period_=period;target_=now;last_handoff_=0;delivered_=false;}
        // Skip whole missed appointments, never manufacture a catch-up burst.
        if(now>target_+period_/2)target_+=((now-target_+period_/2)/period_)*period_;
        if(last_handoff_ && target_<last_handoff_+period_/2+lead())target_+=period_;
        return target_;
    }
    std::int64_t target() const noexcept {return target_;}
    std::int64_t lead() const noexcept {return std::min<std::int64_t>(300'000,period_/8);}
    std::int64_t entry(std::int64_t now) const noexcept {return std::max(now,target_-lead());}
    bool delivered() const noexcept {return delivered_;}
    void finish(std::int64_t handoff=0) noexcept {
        // The first real handoff seeds phase once, including after loading.
        if(handoff) {
            if(!delivered_)target_=handoff;
            last_handoff_=handoff;delivered_=true;
        }
        target_+=period_;
    }
};
// At a source appointment, allow one outstanding batch plus earned reserve.
// The new source will add the next batch; allocated capacity is never a target.
inline std::int64_t independent_recovery_delay(unsigned work,unsigned divisor,
                                               std::int64_t period,std::int64_t reserve) noexcept {
    const auto baseline=std::clamp(divisor,1U,4U);
    const auto excess=static_cast<std::int64_t>(work)-baseline;
    return std::max(std::int64_t{},excess*period-std::max(std::int64_t{},reserve));
}
inline constexpr unsigned kIndependentSourceSlots=12; // lazy GPU backing
inline constexpr unsigned kIndependentQueuedSources=6;
// Admission BEFORE the next app render: one older GPU batch may overlap
// the source being rendered. Ready outputs awaiting XR are not GPU work.
inline constexpr unsigned kIndependentGpuBatches=2;
inline bool independent_source_budget_available(std::size_t sources,bool submitting,
    std::size_t completions,bool completing) noexcept {
    return sources+unsigned(submitting)+completions+unsigned(completing)<kIndependentGpuBatches;
}
// Bound work BEFORE rendering, including already-ready output ownership. Two
// source batches cover generation/native overlap; only measured availability
// jitter earns extra credits. Allocated storage is not a latency target.
inline bool independent_delivery_budget_available(std::size_t gpu_batches,
    std::size_t outputs,std::size_t native_pending,unsigned divisor,
    std::int64_t reserve,std::int64_t period) noexcept {
    const auto factor=std::clamp(divisor,1U,4U);
    const auto extra=period>0 ? std::clamp<std::int64_t>(
        (std::max<std::int64_t>(0,reserve)+period-1)/period,0,kIndependentMaximumReserveFrames) : 0;
    const auto used=gpu_batches*factor+outputs+native_pending;
    return used+factor<=2*factor+static_cast<std::size_t>(extra);
}
inline constexpr unsigned kIndependentOutputCredits=8;
}
