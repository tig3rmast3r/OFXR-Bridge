#pragma once
#include <cstdint>

namespace xrfg {
// Diagnostic state only. Call on the existing AFW color/guide selection, never
// use this state to admit a color, choose a guide or reset a renderer.
struct LukeOriginLossCapture {
    unsigned missing{};
    unsigned captured_episodes{};
    std::uint64_t missing_since{};
    std::uint64_t recovered_since{}, last_request{};
    bool had_selection{};
    bool requested{};
    bool logging{};
    // F9 explicitly starts a new bounded diagnostic window. It must work even
    // when the owner enables logging after AFW has already become unavailable.
    void set_logging(bool enabled) noexcept {
        if(enabled&&!logging) {
            *this={};had_selection=true;
        }
        logging=enabled;
    }
    void captured(std::uint64_t now_ms) noexcept {
        requested=true;last_request=now_ms;recovered_since=0;++captured_episodes;
    }
    bool observe(bool selection_ready, bool diagnostic_scene,std::uint64_t now_ms) noexcept {
        if(captured_episodes>=4)return false;
        if (!diagnostic_scene) { missing=0;missing_since=0;recovered_since=0; return false; }
        if (selection_ready) {
            had_selection=true;missing=0;missing_since=0;
            if (!recovered_since || now_ms<recovered_since) recovered_since=now_ms;
            // One witness per episode. A genuine, sustained recovery rearms it;
            // transient valid rows within one effect cannot create a dump burst.
            if (requested && now_ms-recovered_since>=1000 &&
                now_ms>=last_request && now_ms-last_request>=10000) requested=false;
            return false;
        }
        recovered_since=0;
        if (!had_selection || requested) return false;
        if (!missing || now_ms<missing_since) {missing=0;missing_since=now_ms;}
        if (missing<1000000) ++missing;
        // The caller supplies the FINAL AFW source admission result, after
        // continuity. A missing CRC rescued by the bonus is not an N/A witness.
        return true;
    }
};
}
