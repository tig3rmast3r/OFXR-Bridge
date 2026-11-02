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
        unsigned real{},generated{};
        for(const auto& s:samples_)if(s.at>start && s.at<=now) {
            if(s.scene.output)++generated;else ++real;
        }
        const double seconds=double(now-start)*1e-9;
        return ScenePresentationRate::Rate{float((real+generated)/seconds),float(real/seconds),float(generated/seconds)};
    }
};
}
