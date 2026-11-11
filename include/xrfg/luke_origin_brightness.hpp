#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>

namespace xrfg {
// Process/game-session policy only. No profile writes, pixel guesses, extra
// marker locations or decoder changes. Callers serialize access.
class LukeOriginBrightness {
public:
    static constexpr unsigned max_shift=8;
    static constexpr std::uint64_t loss_ms=2000, dwell_ms=500,
        settle_ms=200, retry_ms=30000, probe_timeout_ms=15000, stamp_age_ms=1000;
    struct Stamp {std::uint32_t token{};std::uint64_t epoch{},at{};};
    struct Choice {unsigned shift{};std::uint64_t epoch{};};
private:
    std::array<Stamp,512> stamps_{};
    unsigned base_{},current_{},learned_{},next_{},misses_{},good_{};
    std::uint64_t epoch_{1},changed_{},probe_started_{},miss_started_{},good_started_{},
        retry_after_{},last_stamp_{},last_observed_{},last_good_stamp_{};
    bool initialized_{},probing_{},missing_{},adapted_{};
    void evidence_reset() noexcept {misses_=good_=0;missing_=false;good_started_=last_good_stamp_=0;}
    void choose(unsigned value,std::uint64_t now) noexcept {
        current_=value;++epoch_;changed_=now;evidence_reset();
    }
    void rollback(std::uint64_t now) noexcept {
        probing_=false;choose(learned_,now);retry_after_=now+retry_ms;
    }
    const Stamp* find(std::uint32_t token,std::uint64_t now) const noexcept {
        if(!token)return nullptr;
        // Newest wins if a token is ever stamped again; age/epoch still apply.
        for(unsigned n=0;n<stamps_.size();++n) {
            const auto& s=stamps_[(next_+stamps_.size()-1-n)%stamps_.size()];
            if(s.token==token)return s.epoch==epoch_ && now>=s.at && now-s.at<=stamp_age_ms?&s:nullptr;
        }
        return nullptr;
    }
public:
    Choice choice(unsigned base,std::uint64_t now) noexcept {
        if(!initialized_) {base_=current_=learned_=std::min(base,max_shift);initialized_=true;changed_=now;}
        if(probing_ && now>=probe_started_ && now-probe_started_>=probe_timeout_ms)rollback(now);
        return {current_,epoch_};
    }
    void stamped(std::uint32_t token,Choice choice,std::uint64_t now) noexcept {
        if(!initialized_||!token||choice.epoch!=epoch_||choice.shift!=current_)return;
        stamps_[next_++%stamps_.size()]={token,epoch_,now};next_%=stamps_.size();last_stamp_=now;
    }
    // completed_read=false includes GPU/map/timeout errors and pending work.
    // Missing=true means a completed, non-ambiguous CRC search found no token.
    // Initial activation additionally requires localized dark-marker evidence.
    // A partial/ambiguous/error pair cannot prove a working brightness.
    void observe(std::uint64_t now,bool completed_read,bool missing,
                 std::span<const std::uint32_t> tokens,bool dark_marker=false) noexcept {
        if(!initialized_||now<last_observed_)return;
        if(!completed_read){evidence_reset();last_observed_=now;return;}
        if(probing_ && now>=probe_started_ && now-probe_started_>=probe_timeout_ms){rollback(now);return;}
        const auto elapsed=now-last_observed_;last_observed_=now;
        if(elapsed>loss_ms)evidence_reset(); // Do not accumulate failures across pauses.
        if(now<last_stamp_||now-last_stamp_>stamp_age_ms){evidence_reset();return;}
        bool valid=!tokens.empty();std::uint64_t newest=0;
        for(auto token:tokens) {const auto* s=find(token,now);valid&=s!=nullptr;if(s)newest=std::max(newest,s->at);}
        if(valid) {
            misses_=0;missing_=false;
            if(!probing_)return;
            if(now-changed_<dwell_ms)return; // Retire in-flight older brightness.
            if(newest<=last_good_stamp_)return; // Re-reading a frozen pair is not proof.
            last_good_stamp_=newest;if(!good_++)good_started_=now;
            if(good_>=3 && now-good_started_>=settle_ms) {
                if(current_>base_)adapted_=true;
                learned_=current_;probing_=false;evidence_reset();retry_after_=0;
            }
            return;
        }
        // A valid but old/unowned CRC is not a brightness failure. Ambiguous
        // and transport-error results are excluded by the runtime adapter.
        if(!missing)return;
        if(!dark_marker&&!probing_&&!adapted_){evidence_reset();return;}
        good_=0;good_started_=last_good_stamp_=0;
        if(!missing_) {missing_=true;miss_started_=now;misses_=0;}
        ++misses_;
        if(!probing_) {
            if(now<retry_after_||now-miss_started_<loss_ms||misses_<16)return;
            probing_=true;probe_started_=now;choose(base_,now); // Retry from normal, retain learned fallback.
            return;
        }
        if(now-changed_<dwell_ms||misses_<8)return;
        if(current_>=max_shift){rollback(now);return;}
        choose(current_+1,now);
    }
    std::uint64_t epoch() const noexcept {return epoch_;}
    unsigned shift() const noexcept {return current_;}
    unsigned learned() const noexcept {return learned_;}
    bool permits_generic_retry() const noexcept {return probing_||adapted_;}
    bool probing() const noexcept {return probing_;}
};
} // namespace xrfg
