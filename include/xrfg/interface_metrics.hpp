#pragma once
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include "xrfg/delivery_cadence.hpp"
#include "xrfg/aer_framegen_evidence.hpp"

namespace xrfg {
// QPC nanoseconds on Windows. Display-time conversion uses this same domain.
std::int64_t interface_clock_ns() noexcept;
std::int64_t interface_qpc_ns(std::int64_t ticks) noexcept;

// Single-writer physical-consumer receipt. The consumer publishes once after
// native End; readers never call the runtime or lock the director. A receipt
// owns no cycle, overlay or session, so retained packets cannot form a cycle.
struct InterfaceDeliveryReceipt {
    const bool synthetic;
    const bool mixed_reference;
    // F8-only identity, frozen before private submission. No timing policy.
    std::uint64_t capture_cycle{},capture_owner{},capture_serial{};
    std::uint32_t capture_generated_index{};
    std::atomic<bool> synthetic_confirmed{false};
    std::atomic<int> outcome{0}; // 0 pending, 1 nonempty native success, -1 failure/empty
    std::int64_t entered_ns{},returned_ns{},display_ns{},prepared_ns{};
    explicit InterfaceDeliveryReceipt(bool generated,bool mixed=false) noexcept
        :synthetic(generated),mixed_reference(mixed) {}
    void completed(bool success,std::int64_t entered,std::int64_t returned,
                   std::int64_t display,std::int64_t prepared=0) noexcept {
        entered_ns=entered;returned_ns=returned;display_ns=display;prepared_ns=prepared;
        outcome.store(success?1:-1,std::memory_order_release);
    }
};
struct InterfaceNativeTiming {
    std::int64_t entered_ns{},returned_ns{},display_ns{};
};

// One app Wait/Begin/End ticket. It travels with the retained real, rather
// than sampling whatever newer frame is running on the producer thread.
class InterfaceCycle {
    mutable std::mutex mutex_;
    std::int64_t dependency_started_{};
    std::int64_t latency_start() const noexcept {
        return dependency_started_>0 ? std::min(started_ns,dependency_started_) : started_ns;
    }
    struct Interval { std::int64_t first{}, last{}; };
    std::array<Interval, 64> intervals_{};
    std::size_t count_{};
    bool valid_{true};
    std::int64_t producer_returned_{},production_started_{};
    struct SyntheticReceipt { std::int64_t accepted{}, period{}, entered{}; };
    std::array<SyntheticReceipt,4> synthetic_{};
    std::size_t synthetic_count_{};
    bool estimate_valid_{true};
    std::int64_t real_display_ns_{};
    bool native_required_{},native_valid_{true};
    std::array<std::shared_ptr<InterfaceDeliveryReceipt>,8> native_{};
    std::size_t native_count_{};
    bool native_real(InterfaceNativeTiming& out) const noexcept {
        const InterfaceDeliveryReceipt* first{};
        for(std::size_t i=0;i<native_count_;++i) {
            const auto& receipt=native_[i];
            if((receipt->synthetic && !receipt->mixed_reference) || receipt->outcome.load(std::memory_order_acquire)!=1)continue;
            if(receipt->entered_ns<started_ns || receipt->returned_ns<receipt->entered_ns)continue;
            if(!first || receipt->entered_ns<first->entered_ns)first=receipt.get();
        }
        if(!first || !native_valid_)return false;
        out={first->entered_ns,first->returned_ns,first->display_ns};return true;
    }
public:
    const std::int64_t started_ns;
    explicit InterfaceCycle(std::int64_t started,std::int64_t producer_start=0) noexcept
        : production_started_(producer_start),started_ns(started) {}
    // AER needs a later publication to interpolate an earlier same-eye source.
    // Retain the actual dependency clock; never infer a fixed refresh penalty.
    void source_dependency(std::int64_t started) noexcept {
        std::scoped_lock lock(mutex_);
        if(started>0 && started<=started_ns)
            dependency_started_=dependency_started_?std::min(dependency_started_,started):started;
    }
    void call(std::int64_t first, std::int64_t last) noexcept {
        std::scoped_lock lock(mutex_);
        if (!first || last < first || first < started_ns) { valid_=false; return; }
        // Union overlapping eye-thread calls. Their wall time is counted once.
        for (std::size_t i=0;i<count_;) {
            if (last < intervals_[i].first || first > intervals_[i].last) { ++i; continue; }
            first=std::min(first,intervals_[i].first);last=std::max(last,intervals_[i].last);
            intervals_[i]=intervals_[--count_];i=0;
        }
        if (count_==intervals_.size()) { valid_=false; return; }
        intervals_[count_++]={first,last};
    }
    void production_started(std::int64_t at) noexcept {
        std::scoped_lock lock(mutex_);production_started_=at;
    }
    bool production_cycle(std::int64_t source_end,float xr_estimated,float& game,float& total) const noexcept {
        std::scoped_lock lock(mutex_);
        if(production_started_<=0 || production_started_>source_end || !std::isfinite(xr_estimated))return false;
        game=float(source_end-production_started_)*1e-6F;
        total=xr_estimated+float(started_ns-production_started_)*1e-6F;
        return std::isfinite(total) && total>=game;
    }
    void producer_returned(std::int64_t at) noexcept {
        std::scoped_lock lock(mutex_);producer_returned_=at;
    }
    void require_native_delivery() noexcept {std::scoped_lock lock(mutex_);native_required_=true;}
    void attach_native_delivery(const std::shared_ptr<InterfaceDeliveryReceipt>& receipt) noexcept {
        std::scoped_lock lock(mutex_);native_required_=true;
        if(!receipt || native_count_==native_.size()){native_valid_=false;return;}
        native_[native_count_++]=receipt;
    }
    bool native_timing(bool& required,InterfaceNativeTiming& out) const noexcept {
        std::scoped_lock lock(mutex_);required=native_required_;
        return required && native_real(out);
    }
    void real_prediction(std::int64_t display) noexcept {
        std::scoped_lock lock(mutex_);real_display_ns_=display;
    }
    // Observation only: no sleep, queue operation or GPU synchronization.
    // Pending native results can confirm receipts later and out of order.
    void synthetic_accepted(std::int64_t at, std::int64_t period,std::int64_t entered=0) noexcept {
        std::scoped_lock lock(mutex_);
        if (at<started_ns || period<2'000'000 || period>50'000'000 || entered>at ||
            (entered>0 && entered<started_ns)) {estimate_valid_=false;return;}
        for(std::size_t i=0;i<synthetic_count_;++i) if(synthetic_[i].accepted==at)return;
        if(synthetic_count_==synthetic_.size()) {estimate_valid_=false;return;}
        synthetic_[synthetic_count_++]={at,period,entered>0?entered:at};
        std::sort(synthetic_.begin(),synthetic_.begin()+synthetic_count_,
            [](const auto& a,const auto& b){return a.accepted<b.accepted;});
    }
    // Minimum compatible with showing each confirmed synthetic before this real
    // on distinct refreshes. Preserve actual runtime completion; add only an
    // uncovered sequence floor, never a predicted display horizon or tail.
    // This assumes those outputs are shown, not proof of compositor scanout.
    bool read_minimum_cycle(std::int64_t accepted_ns, float& runtime_ms) const noexcept {
        std::scoped_lock lock(mutex_);
        if(!valid_ || started_ns<=0)return false;
        auto returned=accepted_ns;
        std::array<std::int64_t,4> synthetic_entries{};std::size_t outputs{};
        if(native_required_) {
            InterfaceNativeTiming real;
            if(!native_real(real))return false;
            returned=real.returned_ns;
            for(std::size_t i=0;i<native_count_;++i) {
                const auto& r=native_[i];
                if(!r->synthetic || r->mixed_reference || r->outcome.load(std::memory_order_acquire)!=1 ||
                    !r->synthetic_confirmed.load(std::memory_order_acquire))continue;
                if(r->entered_ns<started_ns || r->entered_ns>=real.entered_ns ||
                    outputs==synthetic_entries.size())return false;
                synthetic_entries[outputs++]=r->entered_ns;
            }
        } else {
            outputs=synthetic_count_;
            for(std::size_t i=0;i<outputs;++i) {
                if(synthetic_[i].accepted>returned)return false;
                synthetic_entries[i]=synthetic_[i].entered;
            }
        }
        if(returned<started_ns)return false;
        if(!estimate_valid_)return false;
        if(outputs) {
            if(!synthetic_count_)return false; // no observed physical period
            const auto period=synthetic_[0].period;
            for(std::size_t i=1;i<synthetic_count_;++i)if(synthetic_[i].period!=period)return false;
            std::sort(synthetic_entries.begin(),synthetic_entries.begin()+outputs);
            // Use submission ENTRY, not return: a runtime may block End until
            // after that image was already shown. Don't round late inputs onto
            // a fabricated phase grid; each remaining output only supplies P.
            for(std::size_t i=0;i<outputs;++i)
                returned=std::max(returned,synthetic_entries[i]+static_cast<std::int64_t>(outputs-i)*period);
        }
        runtime_ms=float(returned-latency_start())*1e-6F;
        return std::isfinite(runtime_ms);
    }
    bool read(std::int64_t accepted_ns, float& ofxr_ms, float& cycle_ms) const noexcept {
        std::scoped_lock lock(mutex_);
        if(native_required_) {
            InterfaceNativeTiming real;if(!native_real(real))return false;
            accepted_ns=real.returned_ns;
        }
        if (!valid_ || !started_ns || !producer_returned_ || accepted_ns<started_ns ||
            producer_returned_<started_ns) return false;
        std::int64_t held{};for(std::size_t i=0;i<count_;++i)held+=intervals_[i].last-intervals_[i].first;
        ofxr_ms=float(held)*1e-6F;
        cycle_ms=float(std::max(producer_returned_,accepted_ns)-latency_start())*1e-6F;
        return true;
    }
    // Wall-clock partition at the source End handoff, not GPU kernel durations.
    // Production requires an observed, source-associated engine origin. Missing
    // origins do not invalidate the independent generation/delivery intervals.
    // Native generation/preparation ends
    // only after the first confirmed synthetic's existing preparation fence
    // and copy have completed. Delivery owns the remainder to the real target.
    // No synthetic means zero OFXR generation, including the disabled baseline.
    bool stages(std::int64_t source_end,std::int64_t accepted,float estimated,
                float& game,float& generation,float& delivery) const noexcept {
        std::scoped_lock lock(mutex_);
        if(!valid_ || !started_ns || source_end<started_ns ||
            producer_returned_<source_end || !std::isfinite(estimated))return false;
        std::int64_t ready=source_end;
        if(native_required_) {
            InterfaceNativeTiming real;if(!native_real(real))return false;
            const InterfaceDeliveryReceipt* first{};
            bool has_synthetic{};
            for(std::size_t i=0;i<native_count_;++i) {
                const auto& r=native_[i];if(!r->synthetic && !r->mixed_reference)continue;
                has_synthetic=true;
                if(r->outcome.load(std::memory_order_acquire)!=1 ||
                   !r->synthetic_confirmed.load(std::memory_order_acquire) ||
                   r->entered_ns>real.entered_ns ||
                   (r->entered_ns==real.entered_ns && !r->mixed_reference))continue;
                if(!first || r->entered_ns<first->entered_ns)first=r.get();
            }
            if(has_synthetic) {
                if(!first || first->prepared_ns<source_end || first->prepared_ns>first->entered_ns)return false;
                ready=first->prepared_ns;
            }
        } else if(synthetic_count_) {
            if(!estimate_valid_ || synthetic_[0].accepted<source_end || accepted<synthetic_[0].accepted)return false;
            ready=synthetic_[0].accepted;
        }
        // This is a chronological partition at source End, not an attribution
        // of intercepted calls to a guessed engine render duration.
        game=production_started_>0 && production_started_<=source_end
            ? float(source_end-production_started_)*1e-6F : 0.F;
        generation=float(ready-source_end+started_ns-latency_start())*1e-6F;
        delivery=estimated-float(ready-latency_start())*1e-6F;
        // Floating-point subtraction may leave a few nanoseconds below zero.
        if(delivery<-.001F)return false;
        delivery=std::max(0.F,delivery);return true;
    }
};
// Construct before call locks; destruct after them and all frame teardown.
class InterfaceCallTiming {
    std::int64_t started_{};
    bool terminal_{};
public:
    std::shared_ptr<InterfaceCycle> cycle;
    InterfaceCallTiming(std::int64_t started, bool terminal=false) noexcept : started_(started),terminal_(terminal) {}
    std::int64_t started_ns() const noexcept { return started_; }
    ~InterfaceCallTiming() {
        if (!cycle) return;
        const auto ended=interface_clock_ns();cycle->call(started_,ended);
        if(terminal_)cycle->producer_returned(ended);
    }
};

struct InterfaceFrameStamp {
    std::uint64_t serial{};
    std::int64_t entered_ns{}, begun_ns{};
    std::shared_ptr<InterfaceCycle> cycle;
    // Exact source identity, independent of XR calls/slots; zero is unknown.
    std::uint64_t scene_owner{},scene_serial{};
    // AER output: one exact raw eye and one reconstructed eye. The source
    // clock includes the earlier same-eye endpoint required by interpolation.
    std::shared_ptr<AerFramegenEvidence> aer_evidence;
    unsigned aer_delivery{}; // release pattern, not pixel-freshness proof
    bool aer_mixed{};
    std::int64_t aer_history_started_ns{};
    void mark_aer_mixed() noexcept {
        aer_mixed=true;
        if(cycle)cycle->source_dependency(aer_history_started_ns);
    }
};
enum class InterfaceSource : std::uint32_t { none, steamvr_pre_submit_gpu, real_frame_qpc, predicted_display_qpc, application_cpu_qpc, steamvr_display, openxr_period, accepted_submissions, steamvr_delivery, producer_call_qpc, frame_cycle_qpc, frame_cycle_slot_estimate, steamvr_scene_delivery, accepted_scene_submissions, native_frame_cycle_qpc, native_predicted_display_qpc, runtime_accepted_delivery, runtime_return_cycle_qpc, native_runtime_return_cycle_qpc, minimum_cycle_qpc, native_minimum_cycle_qpc };
struct InterfaceTelemetry {
    // Cadence at the labelled output boundary; accepted confirms distinct new
    // outputs independently, never a claim of physical scanout on its own.
    DeliveryCadenceBatch output_cadence{},accepted_cadence{};
    std::uint64_t updated_ms{};
    float output_fps{}, game_fps{}, generated_fps{}, display_hz{};
    float gpu_render_ms{}, ofxr_delay_ms{}, presentation_ms{}, cpu_render_ms{};
    float ofxr_time_ms{}, cycle_length_ms{};
    bool rates_valid{}, generating{};
    bool aer_eye_equivalents{};
    bool gpu_valid{}, delay_valid{}, presentation_valid{}, cpu_valid{};
    bool ofxr_time_valid{}, cycle_valid{};
    InterfaceSource gpu_source{}, delay_source{}, presentation_source{}, cpu_source{}, display_source{}, output_source{};
    InterfaceSource ofxr_time_source{}, cycle_source{};
    float cycle_estimated_ms{};
    bool cycle_estimated_valid{};
    InterfaceSource cycle_estimated_source{};
    float game_time_ms{},generation_time_ms{},delivery_time_ms{};
    bool stages_valid{},game_time_valid{};
};

// Caller serializes access. Fixed storage; only the first successful real
// submission contributes. Failed attempts do not consume identity, and delayed
// repeats cannot become another latency sample after the time window expires.
class InterfaceTimingWindow {
    struct Sample {
        std::int64_t at{};
        float delay{}, presentation{}, cpu{};
        bool display_valid{}, cpu_valid{};
        std::int64_t accepted_ns{},source_entered_ns{};
        std::shared_ptr<InterfaceCycle> cycle;
    };
    std::array<Sample, 256> samples_{};
    std::array<std::uint64_t, 256> identities_{};
    std::size_t next_{}, identity_next_{};
public:
    bool submitted(InterfaceFrameStamp stamp, std::int64_t now,
        bool success, bool nonempty, bool synthetic, bool fresh,
        std::int64_t display_ns = 0, std::int64_t accepted_ns = 0) noexcept {
        if (!success || !nonempty || synthetic || !fresh || !stamp.serial ||
            !stamp.entered_ns || now < stamp.entered_ns) return false;
        if (std::find(identities_.begin(), identities_.end(), stamp.serial) != identities_.end()) return false;
        identities_[identity_next_++ % identities_.size()] = stamp.serial;
        auto& s = samples_[next_++ % samples_.size()];
        s = {now, float(now - stamp.entered_ns) * 1e-6F,
            float(display_ns - now) * 1e-6F,
            float(stamp.entered_ns - stamp.begun_ns) * 1e-6F,
            display_ns > 0, stamp.begun_ns > 0 && stamp.begun_ns <= stamp.entered_ns,
            accepted_ns ? accepted_ns : now, stamp.entered_ns, std::move(stamp.cycle)};
        return true;
    }
    void snapshot(std::int64_t now, InterfaceTelemetry& out) const noexcept {
        std::array<float, 256> delay{}, presentation{}, cpu{};
        std::array<float,256> ofxr{},cycle{};
        std::array<float,256> estimated{};
        std::array<float,256> game_stage{},generation_stage{},delivery_stage{};
        std::size_t nd{}, np{}, nc{}, nt{}, ne{},ns{},ng{};
        bool native_cycle{},native_estimate{};
        for (const auto& s : samples_) {
            if (!s.at || now < s.at || now - s.at > 1'000'000'000) continue;
            bool native_required{};InterfaceNativeTiming native;
            const bool delivered=s.cycle && s.cycle->native_timing(native_required,native);
            if(native_required && !delivered)continue;
            delay[nd++] = delivered?float(native.entered_ns-s.source_entered_ns)*1e-6F:s.delay;
            if(delivered) {
                if(native.display_ns>=native.entered_ns)presentation[np++]=float(native.display_ns-native.entered_ns)*1e-6F;
            } else if (s.display_valid) presentation[np++] = s.presentation;
            if (s.cpu_valid) cpu[nc++] = s.cpu;
            if(s.cycle && s.cycle->read(s.accepted_ns,ofxr[nt],cycle[nt])){++nt;native_cycle|=delivered;}
            float raw_estimate{};
            if(s.cycle && s.cycle->read_minimum_cycle(s.accepted_ns,raw_estimate)) {
                // V656: actual runtime return, bounded below by the refresh
                // spacing of confirmed synthetic(s) then real. No display
                // prediction or later producer return is substituted.
                estimated[ne++]=raw_estimate;native_estimate|=delivered;
                float unused_game{};
                if(s.cycle->stages(s.source_entered_ns,s.accepted_ns,raw_estimate,
                    unused_game,generation_stage[ns],delivery_stage[ns]))++ns;
                float game{},total{};
                if(s.cycle->production_cycle(s.source_entered_ns,raw_estimate,game,total)) {
                    game_stage[ng++]=game;
                }
            }
        }
        const auto median = [](auto& values, std::size_t count) {
            std::sort(values.begin(), values.begin() + count);
            return count % 2 ? values[count/2] : (values[count/2-1] + values[count/2]) * .5F;
        };
        out.delay_valid = nd != 0; if (nd) out.ofxr_delay_ms = median(delay, nd);
        out.presentation_valid = np != 0; if (np) out.presentation_ms = median(presentation, np);
        out.cpu_valid = nc != 0; if (nc) out.cpu_render_ms = median(cpu, nc);
        out.ofxr_time_valid=out.cycle_valid=nt!=0;
        if(nt) {out.ofxr_time_ms=median(ofxr,nt);out.cycle_length_ms=median(cycle,nt);}
        out.ofxr_time_source=nt?InterfaceSource::producer_call_qpc:InterfaceSource::none;
        out.cycle_source=nt?(native_cycle?InterfaceSource::native_frame_cycle_qpc:InterfaceSource::frame_cycle_qpc):InterfaceSource::none;
        out.cycle_estimated_valid=ne!=0;
        if(ne)out.cycle_estimated_ms=median(estimated,ne);
        out.cycle_estimated_source=ne?(native_estimate?InterfaceSource::native_minimum_cycle_qpc:
            InterfaceSource::minimum_cycle_qpc):InterfaceSource::none;
        out.stages_valid=ns!=0;
        out.game_time_valid=ng!=0;if(ng)out.game_time_ms=median(game_stage,ng);
        if(ns) {out.generation_time_ms=median(generation_stage,ns);
            out.delivery_time_ms=median(delivery_stage,ns);}
        out.delay_source = nd ? InterfaceSource::real_frame_qpc : InterfaceSource::none;
        out.presentation_source = np ? InterfaceSource::predicted_display_qpc : InterfaceSource::none;
        out.cpu_source = nc ? InterfaceSource::application_cpu_qpc : InterfaceSource::none;
    }
};
} // namespace xrfg
