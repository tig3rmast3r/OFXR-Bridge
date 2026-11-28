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
// Real progress must cover99% of physical slots over the evidence window;
// no progress, a long pause or a changed period invalidates qualification.
class IndependentRecoveryWindow {
    std::int64_t period_{},started_{},last_{};
    std::uint64_t good_{};
    bool qualified_{};
public:
    void reset() noexcept {*this={};}
    bool observe(std::int64_t now,std::int64_t period,std::uint64_t good,
                 std::int64_t window=500'000'000) noexcept {
        if(period<=0 || now<=0){reset();return false;}
        if(period_!=period || !last_ || now<=last_ || now-last_>period*8) {
            reset();period_=period;started_=last_=now;return false;
        }
        last_=now;good_+=good;
        const auto elapsed=now-started_;
        if(elapsed>=window) {
            qualified_=good_*static_cast<std::uint64_t>(period)*100>=static_cast<std::uint64_t>(elapsed)*99;
            started_=now;good_=0;
        }
        return qualified_;
    }
};
// Split AER can have two accepted packets while only one (or neither) has
// completed its private copy. That is GPU overlap, not surplus ready content.
// Keep the validated stereo policy; AER recovery uses the actual ready FIFO.
inline std::size_t independent_phase_ready_count(bool split_aer,
    std::size_t pending,std::size_t prepared) noexcept {
    return split_aer?prepared:pending;
}
// First synthetic of a reconstructed stereo pair, measured at native End.
// A queued GPU publication supplies its completed copy time.
struct IndependentPairHead {
    std::int64_t entered{},ready{};
    std::int64_t source_due{};
};
// Explicit within-batch scheduling, carried with the source's End reservation.
// Offsets change accounting only; spacing protection retains the current
// native prediction while bounding compressed CPU handoffs.
struct IndependentRecoveryTiming {
    std::int64_t scheduled_offset{};
    bool accelerated{};
    bool protect_next_output{};
    bool source_queued{}; // prior generation/descriptor backlog, not a new GPU lateness sample
    // A previous GPU batch alone is normal overlap under the two-source cap.
    // It still cannot earn expiry reserve, but a completed native head can
    // prove excess phase age and authorize latency-reducing phase trimming.
    bool source_overlap_only{};
};
inline std::int64_t independent_phase_source_due(std::int64_t due,const IndependentRecoveryTiming& timing) noexcept {
    return timing.source_queued && !timing.source_overlap_only?0:due;
}
// Acquire phase from distinct, completed source heads, never the smoothed
// residence of both outputs. Corrections consume only observed ready slack;
// native submission, prediction and GPU ownership do not wait for acquisition.
class IndependentPhaseAcquisition {
    std::int64_t receipt_{},first_{},minimum_{},last_{},cooldown_due_{};
    std::uint64_t progress_{};
    unsigned heads_{};
    void clear() noexcept {first_=minimum_=last_=0;progress_=0;heads_=0;}
public:
    std::int64_t observe(std::int64_t now,std::int64_t p,unsigned divisor,
        const IndependentPairHead& head,std::uint64_t fresh,bool missed) noexcept {
        if(missed){clear();receipt_=std::max(receipt_,head.entered);return 0;}
        if(first_)progress_+=fresh;
        if(head.entered<=receipt_)return 0;
        receipt_=head.entered;
        const auto target=p/2+2'000'000;
        const auto age=head.entered-head.ready;
        // A head is sampled at the next source appointment, after its copy
        // completion can be retired on a later native cycle. Allow that source
        // interval, one native cycle and ordinary quarter-refresh jitter for
        // 3X/4X. Keep the validated 2X freshness bound unchanged.
        const auto receipt_window=divisor<=2?3*p:(std::int64_t(divisor)+1)*p+p/4;
        if(!head.source_due || head.source_due<=cooldown_due_ || !head.ready ||
            head.ready>head.entered || head.entered>now || now-head.entered>receipt_window ||
            age>4*p || age<=target+p/4 || !fresh) {clear();return 0;}
        if(!first_ || head.entered-last_>std::int64_t(divisor)*p*2) {
            clear();first_=head.entered;minimum_=age;
        }
        minimum_=std::min(minimum_,age);last_=head.entered;
        if(++heads_<3)return 0;
        const auto elapsed=head.entered-first_;
        const bool full_rate=elapsed>0 && progress_*std::uint64_t(p)*100>=std::uint64_t(elapsed)*90;
        const auto step=full_rate?std::min(p,minimum_-target):0;
        clear();
        if(step>0)cooldown_due_=head.source_due+2*std::int64_t(divisor)*p;
        return std::max(std::int64_t{},step);
    }
};
// Consume surplus output ownership by postponing future source appointments,
// never by accelerating native End or discarding rendered images. Qualification
// survives isolated empty-ready/idle samples. Split AER and optical batches may
// have one completed FIFO head while their peer is in native End:
// a quarter-refresh hysteresis band distinguishes extra age from steady jitter.
class IndependentSourcePhase {
    std::int64_t period_{},last_{},trimmed_{};
    std::uint64_t delivered_{},expired_{},idle_{};
    IndependentRecoveryWindow recovery_;
    IndependentPhaseAcquisition acquisition_;
    bool split_aer_{},one_recovering_{},stereo_mode_{};
    bool paired_head_mode_{};
    unsigned stereo_divisor_{};
    std::int64_t last_head_{};
public:
    void reset() noexcept {*this={};}
    std::int64_t trimmed() const noexcept {return trimmed_;}
    std::int64_t observe(std::int64_t now,std::int64_t physical,unsigned divisor,
        std::size_t ready,std::int64_t residence,std::uint64_t delivered,
        std::uint64_t expired,std::uint64_t idle,bool split_aer=false,
        const IndependentPairHead* head=nullptr,bool stereo_recovery=false) noexcept {
        if((divisor<1 || divisor>4) || physical<2'000'000 || physical>50'000'000 || now<=0){reset();return 0;}
        const bool paired=head && divisor==2 && !split_aer && !stereo_recovery;
        const bool stereo=stereo_recovery && divisor>=2 && !split_aer && !paired;
        const bool one_ready=split_aer || stereo;
        if(period_!=physical || split_aer_!=split_aer || paired_head_mode_!=paired || stereo_mode_!=stereo ||
            (stereo && stereo_divisor_!=divisor) ||
            delivered<delivered_ || expired<expired_ || idle<idle_) {
            reset();period_=physical;split_aer_=split_aer;
            paired_head_mode_=paired;stereo_mode_=stereo;stereo_divisor_=stereo?divisor:0;
            if(paired)last_head_=head->entered;
        }
        if(last_ && now<=last_)return 0;
        const auto elapsed=last_?now-last_:0;last_=now;
        const auto fresh=delivered-delivered_;
        const auto lost=expired-expired_;
        const bool missed=lost || idle!=idle_;
        delivered_=delivered;expired_=expired;idle_=idle;
        if(stereo && head) {
            const auto step=acquisition_.observe(now,physical,divisor,*head,fresh,missed);
            trimmed_+=step;return step;
        }
        const bool healthy=recovery_.observe(now,physical,fresh-std::min(fresh,lost),
            stereo?125'000'000:500'000'000);
        auto target=physical/2+2'000'000;
        if(paired) {
            // Sampling the queue from the next app Wait misses a head that
            // has already left for SteamVR. Count each successful head once.
            // The following real necessarily occupies a later refresh.
            if(head->entered<=last_head_ || head->entered>now || now-head->entered>physical*3 ||
                head->ready<=0)return 0;
            last_head_=head->entered;
            residence=head->entered-head->ready;
            target=std::min<std::int64_t>(1'000'000,physical/8);
        }
        if(one_ready) {
            // A real native output can already own the other prepared image;
            // requiring two remaining FIFO entries then preserves an extra
            // physical slot. Arm only above clear excess age, finish at the
            // old target, and stay idle inside the band after settling.
            if(missed || elapsed>physical*8 || residence<=target)one_recovering_=false;
            if(healthy && fresh && !missed && elapsed && elapsed<=physical*8 &&
                ready && residence>target+physical/4)one_recovering_=true;
        }
        if(!healthy || !fresh || missed || !elapsed || elapsed>physical*8 ||
            (!paired && ready<(one_ready?1U:2U)) || residence<=target || (one_ready&&!one_recovering_))return 0;
        // Optical stereo:24ms/second,500us per source after125ms of progress.
        // Other paths retain8ms/second,200us and500ms. Closed-loop ownership
        // and residence end recovery; a lifetime one-P cap cannot recover a
        // second menu transient. No accumulated catch-up step after a veto.
        const auto step=std::min({elapsed*(stereo?24:8)/1000,
            std::int64_t{stereo?500'000:200'000},residence-target});
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
    bool accelerated_{};
    std::int64_t source_due_{},source_period_{};
    std::int64_t delivery_peak_{},delivery_source_due_{},delivery_source_period_{};
    unsigned delivery_late_{},delivery_window_{};
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
    void observe(std::int64_t available,std::int64_t expected,std::int64_t period,unsigned threshold=2) noexcept {
        if(!limit_ || period<=0 || !expected)return;
        const auto late=available-expected;
        // Loading or a disconnected runtime is not evidence for a larger
        // buffer. Keep the established margin instead of suddenly dropping it.
        if(late>period*8){unstable();late_=window_=0;peak_=0;return;}
        if(late>period/2) {
            unstable();peak_=std::max(peak_,late);++late_;
            if(late_>=std::max(threshold,2U)) {
                const auto missed=(peak_+period/2)/period;
                const auto next=std::min(limit_,reserve_+missed*period);
                refill_+=std::max(std::int64_t{},next-reserve_);reserve_=next;
                late_=window_=0;peak_=0;
            }
        }
        if(++window_>=32){late_=window_=0;peak_=0;}
    }
    bool observe_source(std::int64_t available,std::int64_t due,std::int64_t period,
        const IndependentRecoveryTiming& timing) noexcept {
        if(!timing.accelerated){observe(available,due+reserve_,period);return true;}
        if(!due || period<=0)return false;
        const auto first=due-std::clamp(timing.scheduled_offset,std::int64_t{},3*period);
        if(source_period_!=period){source_period_=period;source_due_=0;late_=window_=0;peak_=0;}
        if(first<=source_due_)return false;
        source_due_=first;
        if(timing.source_queued && !timing.source_overlap_only) {
            // One overrun and the work queued behind it are one disturbance.
            // Keep real expiry strict; do not turn that backlog into tolerance.
            late_=window_=0;peak_=0;return false;
        }
        // Normal GPU overlap is steady state at 3X/4X under load. Reject a brief
        // 1-2 source disturbance while still letting sustained overlap lateness
        // earn bounded reserve promptly instead of dropping 12+ frames first.
        observe(available,first+reserve_,period,timing.source_overlap_only?3U:2U);return true;
    }
    bool observe_delivery_expiry(std::int64_t prepared,std::int64_t due,std::int64_t period,
        const IndependentRecoveryTiming& timing) noexcept {
        if(!limit_ || period<=0 || !due || !prepared)return false;
        const auto first=timing.accelerated?due-std::clamp(timing.scheduled_offset,std::int64_t{},3*period):due;
        if(delivery_source_period_!=period) {
            delivery_source_period_=period;delivery_source_due_=0;
            delivery_late_=delivery_window_=0;delivery_peak_=0;
        }
        if(first<=delivery_source_due_)return false;
        delivery_source_due_=first;
        if(timing.accelerated && timing.source_queued && !timing.source_overlap_only) {
            delivery_late_=delivery_window_=0;delivery_peak_=0;return false;
        }
        const auto late=prepared-(due+reserve_);
        if(late>period*8){unstable();delivery_late_=delivery_window_=0;delivery_peak_=0;return false;}
        if(late>period/2) {
            unstable();delivery_peak_=std::max(delivery_peak_,late);++delivery_late_;
            if(delivery_late_>=4U) {
                const auto missed=(delivery_peak_+period/2)/period;
                const auto next=std::min(limit_,reserve_+missed*period);
                refill_+=std::max(std::int64_t{},next-reserve_);reserve_=next;
                late_=window_=0;peak_=0;
                delivery_late_=delivery_window_=0;delivery_peak_=0;
                return true;
            }
        }
        if(++delivery_window_>=32){delivery_late_=delivery_window_=0;delivery_peak_=0;}
        return false;
    }
    void delivered(std::int64_t ready,std::int64_t expected,
                   std::int64_t entered,std::int64_t period,bool accelerated=false) noexcept {
        if(!limit_ || period<=0 || !ready || !expected || !entered)return;
        if(accelerated_!=accelerated){unstable();accelerated_=accelerated;}
        const auto slack=expected-ready;
        const bool spare=slack>=period/2 && entered<=expected+period/4;
        const auto previous=last_delivery_;last_delivery_=entered;
        const bool healthy=recovery_.observe(entered,period,spare?1:0,accelerated?125'000'000:500'000'000);
        if(!healthy || !spare || !previous || entered<=previous || entered-previous>period*8)return;
        delivery_late_=delivery_window_=0;delivery_peak_=0;
        // Retire margin promptly once actual native deliveries prove spare
        // readiness. The current frame still vetoes unsafe cuts. In particular
        // this stops before shrinking tolerance underneath a queued late frame;
        // source-phase recovery must remove that age first.
        const auto step=std::min({(entered-previous)*(accelerated?24:8)/1000,
            std::int64_t{accelerated?500'000:200'000},slack-period/2});
        reserve_=std::max(std::int64_t{},reserve_-std::max(std::int64_t{},step));
    }
};
inline std::int64_t independent_queue_residence(std::int64_t entered,std::int64_t ready,
    std::int64_t period,std::int64_t scheduled_offset=0) noexcept {
    const auto age=std::max(std::int64_t{},entered-ready);
    return std::clamp(age-std::clamp(scheduled_offset,std::int64_t{},period*3),
        std::int64_t{},period*4);
}
inline bool independent_expired(std::int64_t due,std::int64_t now,std::int64_t allowance,std::int64_t period) noexcept {
    // Due identifies a nominal physical slot. Test the current decision time,
    // never a future window end: that could reject an image before it is due.
    return due>0 && now>due+std::max(std::int64_t{},allowance)+period/2;
}
// An empty-queue replay cutoff is not the latest safe fresh-image handoff.
// Guided images already copied/queued inside the same native window remain
// eligible until 300us before its end; the existing End guard stays at 250us.
inline std::int64_t independent_readiness_deadline(std::int64_t window,bool guided) noexcept {
    return window-(guided?300'000:1'250'000);
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
// Independent rounding can assign a new head the preceding batch's tail slot.
// These are expiry slots only: the native consumer never waits on them.
class IndependentBatchDeadlines {
    std::int64_t period_{},next_{};
public:
    std::int64_t claim(std::int64_t candidate,std::int64_t period,unsigned outputs) noexcept {
        if(candidate<=0 || period<=0){reset();return candidate;}
        if(period_!=period){reset();period_=period;}
        const auto first=std::max(candidate,next_);
        next_=first+std::clamp(outputs,1U,4U)*period;
        return first;
    }
    void reset() noexcept {*this={};}
};
// Final-consumer clock only. Native period + one seeded phase form an absolute
// grid. Native entry work never changes the next appointment. A blocking Wait
// already supplies pacing; an immediately returning Wait uses this grid.
class IndependentDeliveryClock {
    std::int64_t period_{},target_{},last_handoff_{};
    std::int64_t paired_earliest_{};
    std::int64_t minimum_next_{};
    bool delivered_{};
public:
    std::int64_t claim(std::int64_t now,std::int64_t period,bool distinct_prediction=true) noexcept {
        period=std::clamp<std::int64_t>(period,2'000'000,50'000'000);
        if(period_!=period || !target_){period_=period;target_=now;last_handoff_=paired_earliest_=minimum_next_=0;delivered_=false;}
        if(!distinct_prediction)paired_earliest_=0;
        // Skip whole missed appointments, never manufacture a catch-up burst.
        if(now>target_+period_/2)target_+=((now-target_+period_/2)/period_)*period_;
        if(last_handoff_ && !paired_earliest_ && !minimum_next_ && target_<last_handoff_+period_/2+lead())target_+=period_;
        return target_;
    }
    std::int64_t target() const noexcept {return target_;}
    std::int64_t lead() const noexcept {return std::min<std::int64_t>(300'000,period_/8);}
    std::int64_t entry(std::int64_t now,bool use_grid=true,bool optical_spacing=true) const noexcept {
        return std::max({paired_earliest_,optical_spacing?minimum_next_:0,
            use_grid?std::max(now,target_-lead()):now});
    }
    std::int64_t entry_in_window(std::int64_t now,bool use_grid,std::int64_t cutoff) const noexcept {
        const auto spaced=entry(now,use_grid);
        const auto original=entry(now,use_grid,false);
        // Spacing alone must never buy another native refresh. Retain the
        // original eligible handoff when its current window is too short.
        return spaced>=cutoff && original<cutoff?original:spaced;
    }
    bool delivered() const noexcept {return delivered_;}
    void finish(std::int64_t handoff=0,bool pair_head=false,bool protect_spacing=false) noexcept {
        // The first real handoff seeds phase once, including after loading.
        if(handoff) {
            if(!delivered_)target_=handoff;
            last_handoff_=handoff;delivered_=true;
        }
        target_+=period_;
        // A late first synthetic can leave its real less than half a refresh
        // away. Keep the next distinct native slot and its minimum spacing.
        // The following appointment stays on the physical grid.
        paired_earliest_=pair_head && handoff && target_-lead()<handoff+period_/2
            ? handoff+period_/2 : 0;
        // A prefetched blocking Wait may complete before the preceding End.
        // Optical output spacing must follow that actual End, independent of
        // whether our absolute grid predicts a short interval.
        minimum_next_=protect_spacing && handoff?handoff+period_/2:0;
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
