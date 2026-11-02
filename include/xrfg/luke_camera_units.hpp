#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace xrfg {
struct LukeUnitCamera {
    std::array<float,3> position{}, right{};
    float delta_ms{};
    bool valid{};
};
// Three consecutive AER cameras: interpolate the two same-eye samples to
// the middle timestamp. Their difference from the middle opposite eye
// removes constant-velocity camera translation, unlike a two-eye distance.
inline float luke_camera_separation(const LukeUnitCamera& older,
    const LukeUnitCamera& middle, const LukeUnitCamera& newer) noexcept {
    if (!older.valid || !middle.valid || !newer.valid ||
        !std::isfinite(middle.delta_ms) || !std::isfinite(newer.delta_ms) ||
        middle.delta_ms <= 0 || newer.delta_ms <= 0) return 0;
    const double t = middle.delta_ms / (double(middle.delta_ms) + newer.delta_ms);
    double separation=0, norm=0, agreement=0;
    for(unsigned i=0;i<3;++i) {
        if (!std::isfinite(older.position[i]) || !std::isfinite(middle.position[i]) ||
            !std::isfinite(newer.position[i]) || !std::isfinite(older.right[i]) ||
            !std::isfinite(middle.right[i]) || !std::isfinite(newer.right[i])) return 0;
        const double axis = middle.right[i];
        norm += axis*axis;
        agreement += older.right[i]*newer.right[i];
        const double interpolated = older.position[i]*(1-t)+newer.position[i]*t;
        separation += (middle.position[i]-interpolated)*axis;
    }
    if (norm < .99 || norm > 1.01 || agreement < .99) return 0;
    const double result=std::abs(separation)/std::sqrt(norm);
    return std::isfinite(result) && result > 0 ? float(result) : 0;
}
class LukeCameraUnitCalibration {
    std::array<float,9> samples_{};
    unsigned count_{}, next_{};
    std::uint64_t epoch_{}, publication_{};
    float baseline_{};
public:
    float observe(std::uint64_t epoch, std::uint64_t publication, float sample) noexcept {
        if (epoch != epoch_) { samples_={}; count_=next_=0; baseline_=0; epoch_=epoch; publication_=0; }
        if (publication == publication_) return baseline_;
        if (publication < publication_) return 0;
        publication_=publication;
        // Engine units do not change when the head moves. Once established,
        // keep this calibration until an explicit producer epoch change.
        if (baseline_ > 0) return baseline_;
        if (!std::isfinite(sample) || sample <= 0) { count_=next_=0; return 0; }
        samples_[next_]=sample; next_=(next_+1)%samples_.size();
        count_=std::min<unsigned>(count_+1,samples_.size());
        baseline_=0;
        if (count_ == samples_.size()) {
            auto sorted=samples_; std::sort(sorted.begin(),sorted.end());
            const float median=sorted[4];
            if (sorted.back()-sorted.front() <= median*.03F) baseline_=median;
        }
        return baseline_;
    }
    unsigned samples() const noexcept { return count_; }
};
// GTA can change camera rigs at a scripted transition without changing the
// producer feature. Keep the established conversion through a temporal reset,
// but replace it only after another complete, consistent native-camera window.
// No latest-frame lookup: each immutable publication owns the returned value.
class LukeSceneCameraUnitCalibration {
    std::array<float,9> samples_{};
    unsigned count_{},next_{};
    std::uint64_t epoch_{},publication_{};
    float baseline_{};
public:
    float observe(std::uint64_t epoch,std::uint64_t publication,float sample) noexcept {
        if(publication==publication_)return baseline_;
        if(publication<publication_)return 0;
        publication_=publication;
        if(epoch!=epoch_){samples_={};count_=next_=0;epoch_=epoch;}
        if(!std::isfinite(sample)||sample<=0){count_=next_=0;return baseline_;}
        samples_[next_]=sample;next_=(next_+1)%samples_.size();
        count_=std::min<unsigned>(count_+1,samples_.size());
        if(count_==samples_.size()) {
            auto sorted=samples_;std::sort(sorted.begin(),sorted.end());const float median=sorted[4];
            if(sorted.back()-sorted.front()<=median*.03F &&
                (baseline_<=0||std::abs(median-baseline_)>baseline_*.03F))baseline_=median;
        }
        return baseline_;
    }
};
} // namespace xrfg
