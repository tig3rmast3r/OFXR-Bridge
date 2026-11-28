#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace xrfg {
// Match stock VR.hpp's effective AFR settings, not dormant saved toggles.
// is_native_stereo_fix_enabled() is false in AFR even when its value is true.
template<class Read>
unsigned uevr_stock_bootstrap_setting_reasons(Read&& read) {
    const auto yes=[](std::string_view value){return value=="true" || value=="1";};
    return (read("VR_RenderingMethod")!="2"?1u:0u) |
        (read("VR_SynchronizationMode")!="2"?2u:0u) |
        (!yes(read("VR_GhostingFix"))?4u:0u) |
        (yes(read("VR_Compatibility_SplitScreen"))?8u:0u) |
        (yes(read("VR_Compatibility_SceneView"))?16u:0u) |
        (!read("VR_GhostingFixBootstrapViewStates").empty()?32u:0u);
}

// CPU policy only. No resource ownership, sleeping, or frame scheduling.
struct UevrStockBootstrap {
    enum class Phase : unsigned { idle, stabilizing, pulse, cooldown, ready, exhausted };
    struct Eye {
        std::uint64_t stream{}, epoch{}, serial{}, publication{};
        unsigned consecutive{};
    };
    std::array<std::uintptr_t,3> scene{};
    std::array<Eye,2> eyes{};
    std::uint64_t frame{}, next_attempt{}, last_publication{};
    unsigned stable{}, attempts{}, pulse_remaining{};
    bool enabled{}, ready{};
    Phase phase{Phase::idle};

    bool begin(std::array<std::uintptr_t,3> key, bool wanted) noexcept {
        ++frame;
        if(!wanted || !key[0] || !key[1] || !key[2]) {
            enabled=false;ready=false;stable=attempts=pulse_remaining=0;
            eyes={};last_publication=0;phase=Phase::idle;return false;
        }
        if(!enabled || scene!=key) {
            scene=key;eyes={};last_publication=0;ready=false;
            stable=attempts=pulse_remaining=0;next_attempt=frame;enabled=true;
        }
        if(ready) {phase=Phase::ready;return false;}
        ++stable;
        if(stable>600) {pulse_remaining=0;phase=Phase::exhausted;return false;}
        if(pulse_remaining) {--pulse_remaining;phase=Phase::pulse;return true;}
        if(stable>=12 && frame>=next_attempt && attempts<3) {
            ++attempts;pulse_remaining=1;next_attempt=frame+32;
            phase=Phase::pulse;return true;
        }
        phase=attempts==3 && frame>=next_attempt?Phase::exhausted:
            attempts?Phase::cooldown:Phase::stabilizing;
        return false;
    }

    void finish(bool constructed_two) noexcept {
        if(phase!=Phase::pulse)return;
        // Native readiness checks may still return one view. Those probes
        // must not exhaust the actual two-view construction budget.
        if(!constructed_two)++pulse_remaining;
        else if(!pulse_remaining)next_attempt=frame+31;
    }

    void observe(unsigned eye,std::uint64_t stream,std::uint64_t epoch,
        std::uint64_t serial,std::uint64_t previous,std::uint64_t publication,bool reset) noexcept {
        if(!enabled || eye>1 || !stream || !epoch || !serial || !publication || publication<=last_publication)return;
        last_publication=publication;
        auto& prior=eyes[eye];
        const bool chained=!reset && prior.stream==stream && prior.epoch==epoch &&
            prior.serial==previous && serial>prior.serial;
        const auto count=chained?(prior.consecutive<2?prior.consecutive+1:2):1;
        prior={stream,epoch,serial,publication,count};
        const bool was_ready=ready;
        ready=!reset && eyes[0].stream && eyes[1].stream && eyes[0].stream!=eyes[1].stream &&
            eyes[0].consecutive>=2 && eyes[1].consecutive>=2;
        if(was_ready && !ready) {stable=attempts=pulse_remaining=0;next_attempt=frame;}
        if(ready)phase=Phase::ready;
    }
};

// The verified hook is immediately after CMP size_t,2 and before JAE.
// Only CF controls this unsigned branch. Preserve all other CPU state.
inline std::uintptr_t uevr_stock_bootstrap_flags(std::uintptr_t flags,bool pulse) noexcept {
    return pulse?flags|std::uintptr_t{1}:flags;
}
} // namespace xrfg
