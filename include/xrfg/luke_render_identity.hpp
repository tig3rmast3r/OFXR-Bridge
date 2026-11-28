#pragma once
#include <cstdint>
#include <optional>
#include <array>

namespace xrfg {
struct LukeRenderIdentity {
    std::uint64_t object{}, render{};
    bool operator==(const LukeRenderIdentity&) const = default;
};
// Read-only, pinned Crimson runtime. Independent of diagnostic logging.
std::optional<LukeRenderIdentity> query_luke_render_identity() noexcept;
// Selected render in the active REAL VR owner's camera ring, when verified.
std::optional<LukeRenderIdentity> query_real_vr_selected_render() noexcept;
// Days Gone's native ring matcher identifies a camera before "selected" is
// populated later in the render pipeline. Exact XYZ, guarded layout, no writes.
std::optional<LukeRenderIdentity> query_daysgone_camera_render(
    const std::array<float,3>& origin,unsigned* rejection=nullptr) noexcept;
// Both observations bracket the same native evaluation. A changed/missing
// producer is not an identity for that output, and UE's frame counter is never
// substituted for REAL VR's selected render.
inline std::optional<LukeRenderIdentity> stable_real_vr_render_identity(
    const std::optional<LukeRenderIdentity>& before,
    const std::optional<LukeRenderIdentity>& after) noexcept {
    if(!before||!after||!before->object||*before!=*after||before->render>=(1ULL<<63))return {};
    return before;
}
// Exact installed runtime layout and active HFW object; no GPU or game writes.
bool luke_hfw_producer_supported() noexcept;
} // namespace xrfg
