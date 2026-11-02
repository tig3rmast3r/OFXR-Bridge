#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include "xrfg/luke_render_identity.hpp"
#include "xrfg/scene_presentation_rate.hpp"
namespace xrfg {
// Meter identity for the unmodified application stereo pair. A valid color
// token proves the pixels' origin even while AFW waits for camera history.
// This is never a guide-admission or synthetic-output identity.
inline SceneIdentity luke_original_scene_identity(
    const std::array<std::uint32_t,2>& tokens,
    const std::array<std::uint32_t,2>& logical_eyes) noexcept {
    if (!tokens[0] || !tokens[1] || logical_eyes[0]>1 || logical_eyes[1]>1 ||
        logical_eyes[0]==logical_eyes[1]) return {};
    const auto left=logical_eyes[0]==0?tokens[0]:tokens[1];
    const auto right=logical_eyes[0]==0?tokens[1]:tokens[0];
    return {0x4c554b4500000001ULL,(std::uint64_t(left)<<32)|right,0};
}
// HFW's render counter can stay unchanged across a fresh capture of the other
// eye. AFW resolves the actual color-carried CRC token downstream; do not discard
// that capture using the coarser cross-eye counter. Raw OFXR keeps its old filter.
inline bool luke_hfw_source_is_duplicate(
    const std::optional<LukeRenderIdentity>& source,
    const std::optional<LukeRenderIdentity>& previous,
    bool afw_selected, std::optional<std::uint32_t> captured_eye) noexcept {
    return !source || (source == previous &&
        !(afw_selected && captured_eye && *captured_eye < 2));
}
// Publications must come from CRC-validated tokens in these exact two colors.
// Release order and DLSS preset do not identify the new AER source. Equal fresh
// publications in different eye colors are ambiguous and must not select an eye.
inline std::optional<std::size_t> select_luke_afw_color_source(
    const std::array<std::uint64_t,2>& publications,std::uint64_t last) noexcept {
    const bool a=publications[0]>last,b=publications[1]>last;
    if(!a&&!b)return {};
    if(a&&b&&publications[0]==publications[1])return {};
    return a&&(!b||publications[0]>publications[1])?0u:1u;
}
}
