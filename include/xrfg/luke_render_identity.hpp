#pragma once
#include <cstdint>
#include <optional>

namespace xrfg {
struct LukeRenderIdentity {
    std::uint64_t object{}, render{};
    bool operator==(const LukeRenderIdentity&) const = default;
};
// Read-only, pinned Crimson runtime. Independent of diagnostic logging.
std::optional<LukeRenderIdentity> query_luke_render_identity() noexcept;
// Selected render in the active REAL VR owner's camera ring, when verified.
std::optional<LukeRenderIdentity> query_real_vr_selected_render() noexcept;
// Exact installed runtime layout and active HFW object; no GPU or game writes.
bool luke_hfw_producer_supported() noexcept;
} // namespace xrfg
