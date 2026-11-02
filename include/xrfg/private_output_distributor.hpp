#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace xrfg {

// Forecast native Wait/Begin/Release work separately from the physical clock.
// A last-duration-only estimate misses every step of a rising blocking Wait.
// Learn its positive variation too; never include source/preparation waits or
// sleep here, and never change the physical EndFrame interval to gain margin.
class PrivateRuntimeBudget {
    std::int64_t cost_{},previous_{},variation_{};
    bool sampled_{};
public:
    // Owner-selected V589 comparison: open the native cycle 1ms earlier than
    // V588, in addition to its learned service cost/variation and 100us guard.
    static constexpr std::int64_t additional_lead=1'000'000;
    static constexpr std::int64_t presentation_margin=100'000+additional_lead;
    [[nodiscard]] std::int64_t lead(std::int64_t period) const noexcept {
        // One native Wait may overlap the current frame. Bound lookahead by
        // the existing three-credit pipeline, rather than truncating at1P.
        return std::clamp<std::int64_t>(cost_+variation_+presentation_margin,
            presentation_margin,period*2+presentation_margin);
    }
    void observe(std::int64_t native_cost,std::int64_t period) noexcept {
        native_cost=std::clamp<std::int64_t>(native_cost,0,period*2);
        if(sampled_) {
            const auto rise=std::max(std::int64_t{},native_cost-previous_);
            variation_=std::max(rise,variation_-(variation_+63)/64);
        }
        cost_=native_cost>=cost_?native_cost:
            std::max(native_cost,cost_-(cost_-native_cost+15)/16);
        previous_=native_cost;sampled_=true;
    }
};

// One clock for all SteamVR outputs. Units are QPC nanoseconds. It never
// borrows time from the following refresh to catch up after a late output.
class PrivateOutputSchedule {
    std::int64_t period_{}, due_{}, last_{}, hold_{}, refill_{};
    bool primed_{};
public:
    // Prepare metadata/optional logging ahead of the handoff, without moving
    // its clock or subtracting a margin from every physical refresh.
    static constexpr std::int64_t preparation_lead=100'000;
    void period(std::int64_t value) noexcept {
        if(value<2'000'000 || value>50'000'000)return;
        period_=value;
        if(primed_)due_=std::max(due_,last_+period_);
    }
    [[nodiscard]] std::int64_t period() const noexcept {return period_;}
    [[nodiscard]] std::int64_t delay() const noexcept {return hold_;}
    [[nodiscard]] std::int64_t deadline() const noexcept {return due_;}
    [[nodiscard]] std::int64_t preparation_deadline() const noexcept {
        return std::max(std::int64_t{},due_-preparation_lead);
    }
    [[nodiscard]] bool primed() const noexcept {return primed_;}
    // A source underrun needs an actual refill window, not just an immediate
    // late handoff with no ready lead left. Add its exact deficit as margin
    // once AFTER this late frame; the following deadlines retain that phase.
    // Do not age this frame's already selected runtime cycle with a refill.
    // The bounded pool
    // limits useful refill time (a stopped/paused application must not turn
    // into an arbitrarily long replay pause). This runs only at consumption;
    // no producer or GPU operation waits here.
    std::int64_t input_ready(std::int64_t at,std::size_t capacity) noexcept {
        if(!primed_ || at<=due_ || capacity<2)return 0;
        const auto deficit=at-due_;
        const auto refill=std::min(deficit,period_*static_cast<std::int64_t>(capacity-1));
        hold_+=deficit;due_=at;refill_=std::max(refill_,refill);
        return refill;
    }
    // Invoke once the FIFO head's exact GPU completion has been observed.
    // ready is the completion-observation timestamp, not a rounded XR slot.
    void ready(std::int64_t at) noexcept {
        if(!primed_){due_=at;primed_=true;return;}
        if(at>due_){hold_+=at-due_;due_=at;}
    }
    // Native Wait/Begin completion needs headroom before End. Pass its timestamp,
    // never a later image-copy/release completion: that would recharge the native
    // margin after each late GPU packet even when the native cycle was ready.
    // Shift only the missing margin, once, and retain its exact phase. This
    // changes the presentation clock only; producers/preparers keep running.
    std::int64_t runtime_ready(std::int64_t at,std::int64_t margin) noexcept {
        const auto target=at+std::max(std::int64_t{},margin);
        if(!primed_){due_=target;primed_=true;return 0;}
        if(target<=due_)return 0;
        const auto deficit=target-due_;hold_+=deficit;due_=target;return deficit;
    }
    [[nodiscard]] bool eligible(std::int64_t now) const noexcept {
        return primed_ && period_>0 && now>=due_ && (!last_ || now-last_>=period_);
    }
    // at is the actual beginning of the runtime handoff. This also accounts
    // for a late wakeup/long prior runtime call, without a short next interval.
    bool delivered(std::int64_t at) noexcept {
        if(!eligible(at))return false;
        hold_+=std::max(std::int64_t{},at-due_)+refill_;
        last_=at;due_=at+period_+refill_;refill_=0;
        return true;
    }
};

struct PrivatePhysicalClock {
    std::int64_t at{},period{};
    std::uint64_t counter{};
};

// Absolute display slots, rather than a new period measured from every actual
// End. Buffer eligibility belongs to each accepted packet. A late copy cannot
// move the headset clock or discard the FIFO head.
class PrivatePhysicalSchedule {
    PrivatePhysicalClock clock_{};
    std::int64_t due_{},last_at_{};
    std::uint64_t slot_{},last_slot_{};
    bool valid_{},delivered_{};
public:
    // Shift the actual native End phase, not just its preceding Wait/Begin.
    // Reapply this constant to each RAW clock sample, never to the period or
    // an already shifted deadline: the lead cannot accumulate across frames.
    static constexpr std::int64_t handoff_lead=1'000'000;
    // End enters before this slot's physical vsync. A prediction covering
    // that display edge is current; requiring another complete period here
    // discards an otherwise valid native cycle and can stall Begin by 1P.
    [[nodiscard]] static bool prediction_covers_display(
        std::int64_t prediction,std::int64_t handoff) noexcept {
        return prediction>handoff+handoff_lead+PrivateOutputSchedule::preparation_lead;
    }
    bool clock(PrivatePhysicalClock value,bool stabilize=false) noexcept {
        if(value.at<=handoff_lead || !value.counter || value.period<2'000'000 || value.period>50'000'000)return false;
        if(valid_ && value.counter<clock_.counter)return true; // retain the newer observation
        if(stabilize && valid_ && value.period==clock_.period) {
            // Increase buffer already supplies admission headroom. Do not turn
            // noisy observer timestamps into alternating extra refresh slots.
            // Follow the same physical counter/frequency, with a bounded phase
            // correction; no extra packet, pair or historical service floor.
            if(value.counter==clock_.counter)return true;
            const auto steps=value.counter-clock_.counter;
            if(steps<10'000) {
                const auto predicted=clock_.at+handoff_lead+
                    static_cast<std::int64_t>(steps)*clock_.period;
                const auto error=value.at-predicted;
                if(error>-value.period/4 && error<value.period/4)
                    value.at=predicted+std::clamp<std::int64_t>(error,-25'000,25'000);
            }
        }
        clock_=value;clock_.at-=handoff_lead;valid_=true;return true;
    }
    [[nodiscard]] bool valid() const noexcept {return valid_;}
    // No service-history floor after preparation: its exact fences are already
    // complete. Admission capacity must not postpone a usable physical slot.
    [[nodiscard]] std::int64_t delay() const noexcept {return 0;}
    [[nodiscard]] std::int64_t deadline() const noexcept {return due_;}
    [[nodiscard]] std::uint64_t slot() const noexcept {return slot_;}
    void adopt_handoff(std::int64_t at) noexcept {if(!delivered_)last_at_=std::max(last_at_,at);}
    std::int64_t claim(std::int64_t accepted,std::int64_t available,std::int64_t native_ready,
                       std::int64_t now,std::int64_t native_margin) noexcept {
        if(!valid_)return 0;
        auto earliest=std::max({accepted,available,native_ready+native_margin,
            now+PrivateOutputSchedule::preparation_lead});
        if(!delivered_ && last_at_)earliest=std::max(earliest,last_at_+clock_.period);
        auto steps=std::max(std::int64_t{},earliest-clock_.at);
        steps=(steps+clock_.period-1)/clock_.period;
        slot_=clock_.counter+static_cast<std::uint64_t>(steps);
        if(delivered_ && slot_<=last_slot_)slot_=last_slot_+1;
        due_=clock_.at+static_cast<std::int64_t>(slot_-clock_.counter)*clock_.period;
        if(delivered_) {
            // Bounded correction of late CPU entry on the next absolute slot.
            // The grid itself always advances by exactly P. At90Hz this floor
            // remains >11ms; no interval-minus-margin oscillator or catch-up.
            const auto correction=std::min<std::int64_t>(100'000,clock_.period/100);
            due_=std::max(due_,last_at_+clock_.period-correction);
        }
        return due_;
    }
    bool delivered(std::int64_t at) noexcept {
        if(!valid_ || at<due_ || (delivered_ && slot_<=last_slot_))return false;
        last_at_=at;last_slot_=slot_;delivered_=true;return true;
    }
};

// A slot is reserved BEFORE its producer records/writes the private image.
// Accepted entries and reservations both occupy capacity. pop() transfers a
// slot to the preparation/presentation pipeline; only retire() after delivery
// makes it writable by a producer again.
// Thus dequeue never authorizes overwrite of an image still being copied.
template<class T> class PrivateOutputFifo {
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> ready_;
    std::size_t capacity_,occupied_{},reserved_{},inflight_{};
    bool stopped_{};
public:
    explicit PrivateOutputFifo(std::size_t capacity):capacity_(capacity){}
    PrivateOutputFifo(const PrivateOutputFifo&)=delete;
    PrivateOutputFifo& operator=(const PrivateOutputFifo&)=delete;
    bool reserve(std::size_t count=1) {
        std::unique_lock lock(mutex_);
        if(!count || count>capacity_)return false;
        changed_.wait(lock,[&]{return stopped_ || (count<=capacity_ && occupied_<=capacity_-count);});
        if(stopped_)return false;
        occupied_+=count;reserved_+=count;return true;
    }
    bool accept(T value) {
        {std::scoped_lock lock(mutex_);if(stopped_ || !reserved_)return false;
         ready_.push_back(std::move(value));--reserved_;}
        changed_.notify_all();return true;
    }
    // Future admission only: keep all previously owned/reserved entries.
    bool set_capacity(std::size_t capacity) {
        if(!capacity)return false;
        {std::scoped_lock lock(mutex_);capacity_=capacity;}
        changed_.notify_all();return true;
    }
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&]{return stopped_ || !ready_.empty();});
        if(stopped_)return {};
        T value=std::move(ready_.front());ready_.pop_front();++inflight_;return value;
    }
    void retire(std::size_t count=1) {
        {std::scoped_lock lock(mutex_);count=std::min(count,inflight_);inflight_-=count;occupied_-=count;}
        changed_.notify_all();
    }
    void cancel(std::size_t count=1) {
        {std::scoped_lock lock(mutex_);count=std::min(count,reserved_);reserved_-=count;occupied_-=count;}
        changed_.notify_all();
    }
    void stop() {
        {std::scoped_lock lock(mutex_);stopped_=true;}
        changed_.notify_all();
    }
    [[nodiscard]] std::size_t occupied() const {std::scoped_lock lock(mutex_);return occupied_;}
    [[nodiscard]] std::size_t queued() const {std::scoped_lock lock(mutex_);return ready_.size();}
};
} // namespace xrfg
