#pragma once
#include <array>
#include <cstdint>
namespace xrfg::ofr {
struct Source {
    std::uintptr_t resource{};
    std::uintptr_t epoch{};
    std::uint64_t serial{};
    bool operator==(const Source&) const = default;
};
struct Decision {
    unsigned fresh{};
    bool continuity{};
    bool paired() const noexcept { return fresh==3; }
    unsigned output_count() const noexcept { return fresh==3?2:fresh?1:0; }
};
// Source identities must describe actual releases, never EndFrame parity.
// A new release containing recopied old pixels remains unqualified by this policy.
inline Decision classify(const std::array<Source,2>& previous,
    const std::array<Source,2>& current) noexcept {
    Decision result{0,true};
    for(unsigned eye=0;eye<2;++eye) {
        const auto& a=previous[eye];const auto& b=current[eye];
        if(!b.resource || !b.epoch || !b.serial) return {0,false};
        if(a!=b)result.fresh|=1u<<eye;
        if(!a.serial || a.resource!=b.resource || a.epoch!=b.epoch ||
            b.serial<a.serial || b.serial-a.serial>1)result.continuity=false;
    }
    return result;
}
// Observation belongs to the source stream, not to the last generation job.
// A provider refusal, a skipped output or a menu must not erase which eye the
// application actually released. No resource ownership or GPU work is retained.
class SourceTracker {
    std::array<Source,2> previous_{};
    unsigned delivery_{}, last_fresh_{};
    bool observed_{};
public:
    void reset() noexcept { *this={}; }
    bool held(const std::array<Source,2>& current) const noexcept {
        return observed_ && previous_==current;
    }
    Decision observe(const std::array<Source,2>& current) noexcept {
        for(const auto& source:current) if(!source.resource || !source.epoch || !source.serial) {
            reset();return {0,false};
        }
        auto decision=classify(previous_,current);
        previous_=current;
        if(!observed_) {observed_=true;return decision;}
        if(!decision.fresh)return decision;
        const unsigned delivery=decision.paired()?2:1;
        decision.continuity &= (!delivery_ || delivery_==delivery) &&
            (delivery!=1 || last_fresh_!=decision.fresh);
        delivery_=delivery;last_fresh_=decision.fresh;
        return decision;
    }
};
// Temporal phase only; never exchange physical eye resources.
constexpr unsigned newer_eye(bool swap) noexcept { return swap?0:1; }
}
