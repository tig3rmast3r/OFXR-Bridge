#pragma once
#include "xrfg/pose.hpp"
#include <array>
#include <cmath>
#include <optional>

namespace xrfg::stereo_pose_detail {
inline bool unit(Quaternion& q) noexcept {
    const float n=q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w;
    if(!std::isfinite(n)||n<.25F||n>4.F)return false;
    const float s=1.F/std::sqrt(n);q={q.x*s,q.y*s,q.z*s,q.w*s};return true;
}
inline Quaternion inverse(Quaternion q) noexcept {return {-q.x,-q.y,-q.z,q.w};}
inline Quaternion product(Quaternion a,Quaternion b) noexcept {
    return {a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
inline Vec3 rotate(Quaternion q,Vec3 v) noexcept {
    const auto r=product(product(q,{v.x,v.y,v.z,0}),inverse(q));return {r.x,r.y,r.z};
}
inline bool finite(Vec3 v) noexcept {return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
}
namespace xrfg {
// The only input is a simultaneous runtime stereo pair. Never derive the rig
// from the two retained AER eyes, which describe different head times.
inline std::optional<Pose> luke_same_time_peer(const std::array<Pose,2>& pair,
    unsigned source_eye,Pose source) noexcept {
    if(source_eye>1||!stereo_pose_detail::finite(source.position))return {};
    auto a=pair[source_eye],b=pair[1-source_eye];
    if(!stereo_pose_detail::finite(a.position)||!stereo_pose_detail::finite(b.position)||
        !stereo_pose_detail::unit(a.orientation)||!stereo_pose_detail::unit(b.orientation)||
        !stereo_pose_detail::unit(source.orientation))return {};
    Vec3 delta{b.position.x-a.position.x,b.position.y-a.position.y,b.position.z-a.position.z};
    const float distance=std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    if(!std::isfinite(distance)||distance<.01F||distance>.20F)return {};
    const auto inverse=stereo_pose_detail::inverse(a.orientation);
    const auto local=stereo_pose_detail::rotate(inverse,delta);
    const auto offset=stereo_pose_detail::rotate(source.orientation,local);
    Pose out;
    out.orientation=stereo_pose_detail::product(source.orientation,
        stereo_pose_detail::product(inverse,b.orientation));
    out.position={source.position.x+offset.x,source.position.y+offset.y,source.position.z+offset.z};
    if(!stereo_pose_detail::unit(out.orientation)||!stereo_pose_detail::finite(out.position))return {};
    return out;
}
} // namespace xrfg
