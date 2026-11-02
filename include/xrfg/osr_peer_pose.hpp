#pragma once
#include "xrfg/pose.hpp"
#include <array>
#include <cmath>
#include <optional>
#include <span>

namespace xrfg {
// Convert the renderer's actual world-space eye displacement back into the
// source XR camera's metres/axes. Orientation is deliberately not changed.
inline std::optional<Vec3> osr_peer_position(const Pose& source,
    std::span<const float,16> source_view_to_world,
    std::span<const float,16> peer_view_to_world,
    std::array<float,3> xr_to_view_sign, float xr_baseline,float game_baseline) noexcept {
    if(!std::isfinite(xr_baseline) || !std::isfinite(game_baseline) ||
        xr_baseline<=0 || game_baseline<=0)return {};
    const auto& q=source.orientation;
    const float norm=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(norm) || norm<1e-8F)return {};
    const float inv=1/std::sqrt(norm);
    const std::array<float,4> unit{q.x*inv,q.y*inv,q.z*inv,q.w*inv};
    std::array<float,3> delta{},local{};
    for(unsigned i=0;i<3;++i)delta[i]=peer_view_to_world[12+i]-source_view_to_world[12+i];
    for(unsigned axis=0;axis<3;++axis) {
        if(std::abs(xr_to_view_sign[axis])!=1)return {};
        float dot{},length{};
        for(unsigned i=0;i<3;++i) {
            const float basis=source_view_to_world[axis*4+i];
            dot+=delta[i]*basis;length+=basis*basis;
        }
        if(!std::isfinite(dot) || !std::isfinite(length) || length<1e-8F)return {};
        local[axis]=dot/length*xr_to_view_sign[axis]*(xr_baseline/game_baseline);
    }
    const std::array<float,3> cross{
        unit[1]*local[2]-unit[2]*local[1],unit[2]*local[0]-unit[0]*local[2],unit[0]*local[1]-unit[1]*local[0]};
    const std::array<float,3> cross2{
        unit[1]*cross[2]-unit[2]*cross[1],unit[2]*cross[0]-unit[0]*cross[2],unit[0]*cross[1]-unit[1]*cross[0]};
    Vec3 result{source.position.x+local[0]+2*(unit[3]*cross[0]+cross2[0]),
        source.position.y+local[1]+2*(unit[3]*cross[1]+cross2[1]),
        source.position.z+local[2]+2*(unit[3]*cross[2]+cross2[2])};
    if(!std::isfinite(result.x)||!std::isfinite(result.y)||!std::isfinite(result.z))return {};
    return result;
}
}
