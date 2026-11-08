#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <vector>

namespace xrfg {
// A bounded copy of existing CPU delivery receipts; no polling, allocation,
// GPU work or timing policy in record(). Only benchmark consumers keep a trace.
inline constexpr unsigned kDeliveryBatchSize=128;
struct DeliveryCadenceBatch {
    std::uint64_t epoch{},sequence{};
    std::int64_t now_ns{};
    unsigned count{};
    std::array<std::int64_t,kDeliveryBatchSize> times{};
};
class DeliveryCadenceLog {
    std::array<std::int64_t,kDeliveryBatchSize> times_{};
    std::uint64_t epoch_{1},sequence_{};
    std::int64_t last_{-1};
public:
    void invalidate() noexcept {++epoch_;sequence_=0;last_=-1;}
    void record(std::int64_t at) noexcept {
        if(at<0)return;
        // Delayed/out-of-order confirmation cannot invent a clean cadence.
        if(last_>=0&&at<=last_)invalidate();
        times_[sequence_%times_.size()]=at;++sequence_;last_=at;
    }
    DeliveryCadenceBatch snapshot(std::int64_t now) const noexcept {
        DeliveryCadenceBatch out{};out.epoch=epoch_;out.sequence=sequence_;
        out.now_ns=now;out.count=static_cast<unsigned>(std::min<std::uint64_t>(sequence_,times_.size()));
        for(unsigned i=0;i<out.count;++i)out.times[i]=times_[(sequence_-out.count+i)%times_.size()];
        return out;
    }
};
struct DeliveryCadenceStats {
    bool valid{};
    std::uint64_t frames{},missed_slots{};
    double seconds{},mean_fps{},jitter_ms{},p95_ms{},p99_ms{},late_percent{};
};
// Called only by the benchmark at its existing telemetry frequency. Each receipt
// is consumed once across overlapping batches, within this measured interval.
class BenchmarkCadenceSamples {
    std::uint64_t epoch_{},last_sequence_{};
    unsigned source_{};
    std::int64_t start_ns_{},now_ns_{};
    bool failed_{},started_{};
    std::vector<std::int64_t> times_;
public:
    bool attempted() const noexcept {return started_;}
    void unavailable() noexcept {if(started_)failed_=true;}
    void observe(const DeliveryCadenceBatch& b,unsigned source,std::uint64_t updated_ms,
        std::uint64_t start_ms) {
        if(started_&&(b.epoch!=epoch_||source!=source_)){failed_=true;return;}
        if(updated_ms<start_ms||!b.count||!b.epoch||b.count>kDeliveryBatchSize||b.sequence<b.count)return;
        if(!started_) {
            started_=true;epoch_=b.epoch;source_=source;
            start_ns_=b.now_ns-static_cast<std::int64_t>(updated_ms-start_ms)*1'000'000;
        }
        if(b.epoch!=epoch_||source!=source_||b.sequence<last_sequence_) {failed_=true;return;}
        const auto first=b.sequence-b.count+1;
        if(last_sequence_&&first>last_sequence_+1){failed_=true;return;}
        for(unsigned i=0;i<b.count;++i) {
            const auto seq=first+i;const auto at=b.times[i];
            if(seq<=last_sequence_)continue;
            if(at>=start_ns_&&at<=b.now_ns) {
                if(!times_.empty()&&at<=times_.back()){failed_=true;return;}
                times_.push_back(at);
            }
        }
        last_sequence_=b.sequence;now_ns_=b.now_ns;
    }
    DeliveryCadenceStats stats(double hz) const {
        DeliveryCadenceStats out{};
        if(failed_||times_.size()<3||!(hz>0))return out;
        const double period=1e3/hz;
        std::vector<double> intervals;intervals.reserve(times_.size());
        for(unsigned i=1;i<times_.size();++i)intervals.push_back(double(times_[i]-times_[i-1])*1e-6);
        double seconds=double(times_.back()-times_.front())*1e-9;
        const double leading=double(times_.front()-start_ns_)*1e-6;
        if(leading>1.5*period){seconds+=leading*1e-3;intervals.push_back(leading);}
        // Normal partial edge scanout is not a lost frame. A real stalled tail
        // is included, so stopping delivery cannot retain the earlier 90FPS.
        const double tail=double(now_ns_-times_.back())*1e-6;
        if(tail>1.5*period){seconds+=tail*1e-3;intervals.push_back(tail);}
        if(seconds<.25)return out;
        out.frames=times_.size()-1;out.seconds=seconds;out.mean_fps=out.frames/seconds;
        double mean{},squares{};unsigned late{};
        for(double dt:intervals) {
            mean+=dt;squares+=dt*dt;
            if(dt>1.5*period)++late;
            const auto slots=static_cast<std::uint64_t>(std::max(1.0,std::floor(dt/period+.5)));
            out.missed_slots+=slots-1;
        }
        mean/=intervals.size();out.jitter_ms=std::sqrt(std::max(0.0,squares/intervals.size()-mean*mean));
        std::sort(intervals.begin(),intervals.end());
        const auto percentile=[&](double q){return intervals[std::min(intervals.size()-1,
            static_cast<std::size_t>(std::ceil(q*intervals.size()))-1)];};
        out.p95_ms=percentile(.95);out.p99_ms=percentile(.99);out.late_percent=100.0*late/intervals.size();
        out.valid=std::isfinite(out.mean_fps)&&out.mean_fps>0;return out;
    }
};
} // namespace xrfg
