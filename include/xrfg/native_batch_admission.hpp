#pragma once
#include <algorithm>
#include <cstdint>
#include <chrono>
namespace xrfg {
// Protected by the presenter's ownership mutex. Credits include the worker's
// deferred batch, not just descriptors already visible in the output FIFO.
class NativeBatchAdmission {
    unsigned pending_{};
    std::int64_t last_delivery_{},released_{},render_{},generation_{},period_{},handoff_{},miss_{},target_{},generation_jitter_{},delivery_started_{};
    bool sampled_{},async_gpu_handoff_{};
    std::int64_t native_gpu_window_{};
    static void observe(std::int64_t& value,std::int64_t sample) noexcept {
        if(sample<=0 || sample>250'000'000)return;
        // Respond immediately to slower work; decay slowly after recovery.
        value=value?std::max(sample,(value*7+sample)/8):sample;
    }
public:
    static std::int64_t now() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    std::int64_t gpu_overlap_allowance(std::int64_t physical) const noexcept {
        return async_gpu_handoff_ && physical>0 && native_gpu_window_>=physical*2
            ?std::min(physical/3,generation_*3/4):0;
    }
    void observe_gpu_submission_window(std::int64_t remaining) noexcept {native_gpu_window_=std::max<std::int64_t>(0,remaining);}
    std::int64_t gpu_submission_window() const noexcept {return native_gpu_window_;}
    void set_async_gpu_handoff(bool enabled) noexcept {async_gpu_handoff_=enabled;}
    struct Measurement {std::int64_t render,generation,handoff,miss,last_delivery,jitter,overlap;};
    Measurement measurement() const noexcept {return {render_,generation_,handoff_,miss_,last_delivery_,generation_jitter_,gpu_overlap_allowance(period_)};}
    void reset() noexcept {*this={};}
    void admitted(unsigned outputs) noexcept {pending_+=outputs;}
    void cancelled(unsigned outputs) noexcept {pending_-=std::min(pending_,outputs);}
    void delivery_started(std::int64_t at) noexcept {delivery_started_=at;}
    void delivered(std::int64_t at) noexcept {
        delivery_started_=0;
        if(pending_)--pending_;
        last_delivery_=at;
    }
    void prepared(std::int64_t duration) noexcept {
        // Actual native/copy work only. Delivery spacing and deliberate timer
        // waits are not preparation cost and must not create render-ahead.
        if(period_ && duration>=0 && duration<period_*2)
            handoff_=duration>=handoff_?duration:(handoff_*7+duration)/8;
    }
    void released(std::int64_t at,std::int64_t physical=0) noexcept {
        if(physical>=2'000'000 && physical<=50'000'000 && physical!=period_) {
            period_=physical;handoff_=miss_=generation_jitter_=native_gpu_window_=0;
        }
        target_=first_output_at(at,period_);
        // Bootstrap/starvation has no reserved future slot to miss.
        if(target_<=at)target_=0;
        released_=at;sampled_=false;
    }
    std::int64_t first_target() const noexcept {return target_;}
    void first_output(std::int64_t at,std::int64_t target,std::int64_t spare=0) noexcept {
        if(!period_ || !target)return;
        const auto late=at-target;
        // Correct only an actual first-slot miss, never normal inter-output
        // spacing. Proven intentional slack removes obsolete protection.
        // Sub-refresh End timing jitter is not a lost output slot.
        if(late>period_/2 && late<period_*2)
            miss_=std::min(period_/2,std::max(miss_,late+period_/16));
        else if(late<=250'000 && spare>period_/8)
            miss_=std::max<std::int64_t>(0,miss_-(spare-period_/8)/4);
    }
    void source_ready(std::int64_t at) noexcept {
        // First release ENTRY precedes inherited history/worker backpressure.
        if(released_ && !sampled_) {observe(render_,at-released_);sampled_=true;}
    }
    void generated(std::int64_t duration) noexcept {
        if(duration<=0 || duration>250'000'000)return;
        // Protect against recently observed GPU variability, rather than
        // accumulating a penalty for sub-slot native End timing jitter.
        if(generation_)generation_jitter_=std::min(period_?period_/4:5'000'000,
            std::max(generation_jitter_*63/64,duration-generation_));
        observe(generation_,duration);
    }
    unsigned pending() const noexcept {return pending_;}
    std::int64_t budget(std::int64_t period,unsigned outputs) const noexcept {
        const auto work=(render_?render_:period/2)+(generation_?generation_:period/2)+period/8+handoff_+miss_+std::min(period/4,generation_jitter_)-gpu_overlap_allowance(period);
        return std::clamp(work,period/4,period*std::clamp(outputs,2u,4u));
    }
    std::int64_t first_output_at(std::int64_t at,std::int64_t period) const noexcept {
        if(period<2'000'000 || period>50'000'000 || (!pending_ && !last_delivery_))return at;
        // Existing outputs occupy pending_ slots starting at next. Our FIRST
        // output belongs one slot beyond their final real, not in that slot.
        // Keep the same target after the final pending credit retires: an empty
        // queue with a live service clock still has a next physical slot.
        // An entered native End still owns its credit, but its slot is known.
        // Preserve that phase during bounded native CPU work; a long stall
        // falls back to conservative credit admission rather than inventing delivery.
        const bool entering=delivery_started_ && at>=delivery_started_ && at-delivery_started_<period/2;
        const auto next=entering?delivery_started_:(last_delivery_?std::max(at,last_delivery_+period):at+period);
        return next+pending_*period;
    }
    std::int64_t release_at(std::int64_t at,std::int64_t period,unsigned outputs) const noexcept {
        if(period<2'000'000 || period>50'000'000 || (!pending_ && !last_delivery_))return at;
        // Rendering and generation overlap the preceding batch. Retain real
        // credits through stalls; elapsed time cannot invent retired outputs.
        return std::max(at,first_output_at(at,period)-budget(period,outputs));
    }
};
}
