#pragma once
#include "xrfg/scene_presentation_rate.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace xrfg {
// Unique image identities accepted by xrEndFrame, NOT proof of physical scanout.
// Fixed storage; delayed native GPU confirmation keeps the original acceptance time.
class AcceptedSceneRate {
    struct Sample {std::int64_t at{-1}; SceneIdentity scene;};
    std::array<SceneIdentity,2048> seen_{};
    std::array<Sample,1024> samples_{};
    std::size_t seen_next_{}, next_{};
    std::int64_t first_{-1}, unknown_{-1}, known_{-1};
public:
    std::array<std::int64_t,3> diagnostic_times() const noexcept {return {first_,unknown_,known_};}
    void accepted(std::int64_t at,SceneIdentity scene) noexcept {
        if(at<0)return;
        if(first_<0)first_=at;
        if(!scene) {unknown_=std::max(unknown_,at);return;}
        known_=std::max(known_,at);
        if(std::find(seen_.begin(),seen_.end(),scene)!=seen_.end())return;
        seen_[seen_next_++%seen_.size()]=scene;
        samples_[next_++%samples_.size()]={at,scene};
    }
    std::optional<ScenePresentationRate::Rate> rate(std::int64_t now) const noexcept {
        if(first_<0 || now<=first_ || (unknown_>=0 && known_<=unknown_))return {};
        // A single unclassified frame ends the old measurement interval.
        // Measure only the fully known suffix, with the same 100ms minimum
        // as startup; never count unknown images or reuse the pre-gap FPS.
        const auto start=std::max({first_,now-1'000'000'000,unknown_});
        if(now-start<100'000'000)return {};
        unsigned real{},generated{},count{};
        std::array<std::int64_t,1024> times;
        for(const auto& s:samples_)if(s.at>start && s.at<=now) {
            if(s.scene.output)++generated;else ++real;
            times[count++]=s.at;
        }
        // Counting whole images in the window makes a steady 90 Hz read 89 or
        // 90 depending on where its edges fall. Acceptance times are taken after
        // xrEndFrame, which can vary by 10 ms, so neither can its two end images
        // set the rate: fit the spacing through all of them. A stall keeps the
        // window, because images that stopped must drop the rate.
        double seconds=double(now-start)*1e-9;
        if(count>=4) {
            std::sort(times.begin(),times.begin()+count);
            const double middle=(count-1)*.5;
            double covariance{},variance{};
            for(unsigned i=0;i<count;++i) {
                const double offset=i-middle;
                covariance+=offset*double(times[i]-times[0])*1e-9;variance+=offset*offset;
            }
            const double period=covariance/variance;
            if(period>0 && double(now-times[count-1])*1e-9<=period*2)seconds=period*count;
        }
        const auto total=real+generated;
        return ScenePresentationRate::Rate{float(total/seconds),float(real/seconds),float(generated/seconds)};
    }
};
}
