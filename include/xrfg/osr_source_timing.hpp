#pragma once
#include <cstdint>
#include <optional>

namespace xrfg {
// Observation at the native camera callback, not DLSS submission/display time.
// A test-build proxy for scene sampling; preserve the exact render identity.
struct OsrSourceTime {
    std::uint64_t owner{}, sequence{}, ticks{}, frequency{};
};
inline std::optional<double> osr_source_interval(const OsrSourceTime& newer,
    const OsrSourceTime& older, unsigned renders) noexcept {
    if (!newer.owner || newer.owner != older.owner || !newer.frequency ||
        newer.frequency != older.frequency || !older.ticks || newer.ticks <= older.ticks ||
        newer.sequence <= older.sequence || newer.sequence - older.sequence != renders) return {};
    const double seconds = double(newer.ticks - older.ticks) / double(newer.frequency);
    // Reject pauses, callback bursts and broken/duplicate source identities.
    if (seconds < .001 * renders || seconds > .050 * renders) return {};
    return seconds;
}
inline std::optional<float> osr_source_midpoint(const OsrSourceTime& current,
    const OsrSourceTime& peer, const OsrSourceTime& same) noexcept {
    const auto recent = osr_source_interval(current, peer, 1);
    const auto older = osr_source_interval(peer, same, 1);
    if (!recent || !older) return {};
    const double fraction = *recent / (*recent + *older);
    if (fraction < .15 || fraction > .85) return {};
    return float(fraction);
}
inline std::optional<float> osr_source_history_advance(const OsrSourceTime& current,
    const OsrSourceTime& peer, const OsrSourceTime& peer_previous_same) noexcept {
    const auto recent = osr_source_interval(current, peer, 1);
    const auto span = osr_source_interval(peer, peer_previous_same, 2);
    if (!recent || !span) return {};
    const double fraction = *recent / *span;
    if (fraction < .1 || fraction > 1.) return {};
    return float(fraction);
}
}
