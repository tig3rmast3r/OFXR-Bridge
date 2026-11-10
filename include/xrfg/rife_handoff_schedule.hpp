#pragma once
#include "private_output_distributor.hpp"
#include <algorithm>
#include <optional>
#include <cstdlib>

namespace xrfg {
constexpr bool rife_native_handoff_required(bool steamvr,bool enabled,int model,bool aer) noexcept {
    // AER already produces one mixed stereo packet per alternating input.
    // Rounding that packet to another learned native phase can hold capture
    // credit until a later refresh and lock the source into a lower cadence.
    // Ordinary paired RIFE retains the validated low-rate handoff policy.
    return steamvr && enabled && model>=0 && !aer;
}
// Native running-start phase, observed when Wait/Begin actually blocks. Display
// scanout is not this boundary: V954's vsync-1ms phase submitted at late start.
// Until a native gate is witnessed, preserve the V953 native policy.
class RifeHandoffSchedule {
    std::int64_t phase_{},sample_{},period_{},last_turn_{-1};
    std::uint64_t serial_{};
public:
    struct Target {std::int64_t entry{},handoff{};std::uint64_t slot{};};
    static constexpr std::int64_t submit_guard=100'000;
    std::optional<Target> claim(std::int64_t period,std::int64_t gate,
                               std::int64_t now) noexcept {
        if(period<2'000'000 || period>50'000'000 || gate<=0)return {};
        if(period_!=period){phase_=sample_=0;last_turn_=-1;period_=period;serial_=0;}
        if(gate>sample_) {
            if(!phase_)phase_=gate;
            else {
                const auto turns=(gate-phase_+period/2)/period;
                const auto error=gate-(phase_+turns*period);
                if(std::abs(error)<period/4)phase_+=std::clamp<std::int64_t>(error,-25'000,25'000);
            }
            sample_=gate;
        }
        const auto base=phase_+submit_guard;
        const auto steps=std::max(std::int64_t{},now+PrivateOutputSchedule::preparation_lead-base);
        const auto turn=std::max(last_turn_+1,(steps+period-1)/period);
        const auto handoff=base+turn*period;last_turn_=turn;
        // Enter AFTER this gate opens. A Wait just before it may immediately
        // return the previous native turn, even with a future prediction. End
        // follows Begin without a second phase wait. Native service duration
        // cannot advance entry back into the preceding turn or buy extra slots.
        return Target{handoff,handoff,++serial_};
    }
};
}
