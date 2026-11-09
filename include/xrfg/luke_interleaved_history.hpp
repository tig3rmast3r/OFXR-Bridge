#pragma once
#include "xrfg/dlss_motion_vectors.hpp"

namespace xrfg {
inline std::array<std::uint64_t,4> luke_interleaved_producer_key(
    const DlssNativeCameraHistory& a, const DlssNativeCameraHistory& b) noexcept {
    return a.stream < b.stream ? std::array<std::uint64_t,4>{a.stream,a.epoch,b.stream,b.epoch}
        : std::array<std::uint64_t,4>{b.stream,b.epoch,a.stream,a.epoch};
}
// A/B/A native features can publish one chronological AER camera sequence.
// Prove it from all three immutable production identities. Global adjacency
// alone never joins unrelated handles; native-family receipt sequences retain
// their existing single-stream contract.
inline bool luke_interleaved_history_matches(const DlssNativeCameraHistory& current,
    const DlssNativeCameraHistory& middle, const DlssNativeCameraHistory& older) noexcept {
    if (!current.stream || !middle.stream || current.stream == middle.stream ||
        older.stream != current.stream || !current.epoch || !middle.epoch ||
        current.epoch != older.epoch || current.serial < 2 || middle.serial < 2 ||
        older.serial + 1 != current.serial || current.publication < 3 ||
        older.publication != current.publication - 2 || middle.publication != current.publication - 1)
        return false;
    const bool shared_clock=current.source_clock_owner||middle.source_clock_owner||older.source_clock_owner||
        current.source_clock_sequence||middle.source_clock_sequence||older.source_clock_sequence;
    if(shared_clock) {
        if(!current.source_clock_owner||middle.source_clock_owner!=current.source_clock_owner||older.source_clock_owner!=current.source_clock_owner||
            !older.source_clock_sequence||older.source_clock_sequence==UINT64_MAX||middle.source_clock_sequence==UINT64_MAX||
            older.source_clock_sequence+1!=middle.source_clock_sequence||middle.source_clock_sequence+1!=current.source_clock_sequence||
            !older.native_production_sequence||!middle.native_production_sequence||
            older.native_production_sequence==UINT64_MAX||older.native_production_sequence+1!=current.native_production_sequence)return false;
    } else if(current.native_production_sequence||middle.native_production_sequence||older.native_production_sequence)return false;
    const auto next = [](std::uint32_t token) { ++token; return token ? token : 1; };
    if (!current.token || !middle.token || !older.token ||
        next(older.token) != middle.token || next(middle.token) != current.token) return false;
    if(current.raw_motion_native_span!=1 && current.raw_motion_native_span!=2)return false;
    for (const auto* frame : {&current, &middle, &older}) {
        if(shared_clock&&frame->source_clock_sequence!=frame->token)return false;
        if (!frame->valid || frame->reset || frame->jittered || frame->raw_motion_native_span != current.raw_motion_native_span ||
            !frame->motion_width || !frame->motion_height || !frame->output_width || !frame->output_height ||
            frame->motion_width != current.motion_width || frame->motion_height != current.motion_height ||
            frame->output_width != current.output_width || frame->output_height != current.output_height)
            return false;
    }
    return true;
}
} // namespace xrfg
