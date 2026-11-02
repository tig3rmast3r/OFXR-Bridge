#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace xrfg {
// Input cadence, never compositor FPS (which repeats images at headset refresh).
// Enter after FPS from median frame duration stays >refresh-1 for1s.
// Keep the existing99% mean-rate guard so a median cannot hide missed outputs.
// Exit immediately on that guard or an idle gap; no1s delay on exit.
class PrivateRefreshWaitPolicy {
    std::array<std::int64_t,16> intervals_{};
    std::int64_t last_{},period_{},sum_{},qualified_since_{};
    unsigned count_{},cursor_{};
    bool active_{};
    void seed(std::int64_t at,std::int64_t period) noexcept {
        last_=at;period_=period;sum_=qualified_since_=0;count_=cursor_=0;active_=false;
    }
public:
    static constexpr std::int64_t qualification_ns=1'000'000'000;
    void observe(std::int64_t at,std::int64_t period) noexcept {
        if(period<2'000'000 || period>50'000'000 || at<=0) {seed(0,0);return;}
        if(!last_ || period!=period_ || at<=last_) {seed(at,period);return;}
        const auto interval=at-last_;
        if(interval>period*2) {seed(at,period);return;}
        last_=at;
        if(count_==intervals_.size())sum_-=intervals_[cursor_];else ++count_;
        intervals_[cursor_]=interval;sum_+=interval;
        cursor_=(cursor_+1)%intervals_.size();
        if(count_==intervals_.size()) {
            const auto nominal=period_*static_cast<std::int64_t>(count_);
            if(sum_*99>nominal*100) {
                if(active_)seed(at,period);
                else qualified_since_=0;
                return;
            }
            if(active_)return;
            auto sorted=intervals_;
            std::sort(sorted.begin(),sorted.end());
            // Inverse median frame duration, meaningful for paired FG outputs
            // too. At90Hz require strictly >89fps; at120Hz strictly >119fps.
            const auto median=sorted[7]+(sorted[8]-sorted[7])/2;
            const bool fast=median>0 && median*(1'000'000'000-period_)<1'000'000'000*period_;
            if(!fast)qualified_since_=0;
            else {
                if(!qualified_since_)qualified_since_=at;
                active_=at-qualified_since_>=qualification_ns;
            }
        }
    }
    bool enabled(std::int64_t now) noexcept {
        if(last_ && now-last_>period_*2)seed(0,0);
        return active_;
    }
};
}
