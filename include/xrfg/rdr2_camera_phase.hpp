#pragma once
#include "xrfg/osr_source_timing.hpp"
#include "xrfg/pose.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace xrfg {
struct Rdr2CameraPhase { float seconds{}, yaw{}; };
inline double rdr2_angle_delta(double a,double b) noexcept {
    return std::remainder(a-b,6.283185307179586);
}
// Suppress the alternating component of sample timing without accumulating a
// clock or delaying a frame: t' = t + (previous_interval-current_interval)/4.
// Stable game yaw only; the caller supplies yaw with XR head yaw removed.
inline std::optional<Rdr2CameraPhase> rdr2_camera_phase(
    const OsrSourceTime& current,const OsrSourceTime& peer,const OsrSourceTime& same,
    double yaw,double peer_yaw,double same_yaw) noexcept {
    const auto recent=osr_source_interval(current,peer,1),older=osr_source_interval(peer,same,1);
    if(!recent||!older||!std::isfinite(yaw)||!std::isfinite(peer_yaw)||!std::isfinite(same_yaw))return {};
    const double a=rdr2_angle_delta(yaw,peer_yaw),b=rdr2_angle_delta(peer_yaw,same_yaw);
    const double va=a/ *recent,vb=b/ *older;
    // Cuts, reversals and acceleration are not alternating sample jitter.
    if(std::abs(a)>.08||std::abs(b)>.08||va*vb<=0||std::max(std::abs(va),std::abs(vb))>4.0||
       std::abs(va-vb)>.25*std::max(std::abs(va),std::abs(vb)))return {};
    const double seconds=std::clamp((*older-*recent)*.25,-.0035,.0035);
    const double correction=std::clamp((a+b)/(*recent+*older)*seconds,-.012,.012);
    return Rdr2CameraPhase{float(seconds),float(correction)};
}
using Rdr2Matrix=std::array<float,16>;
inline std::optional<double> rdr2_game_yaw(const Rdr2Matrix& view,Quaternion head) noexcept {
    const double norm=double(head.x)*head.x+double(head.y)*head.y+double(head.z)*head.z+double(head.w)*head.w;
    const double horizontal=double(view[8])*view[8]+double(view[9])*view[9];
    if(!std::isfinite(norm)||std::abs(norm-1)>.01||!std::isfinite(horizontal)||horizontal<.25)return {};
    const double head_yaw=std::atan2(2.0*(head.w*head.y+head.x*head.z),norm-2.0*(head.x*head.x+head.y*head.y));
    return std::atan2(view[9],view[8])-head_yaw;
}
inline bool rdr2_head_stable(Quaternion a,Quaternion b) noexcept {
    const double dot=double(a.x)*b.x+double(a.y)*b.y+double(a.z)*b.z+double(a.w)*b.w;
    const double aa=double(a.x)*a.x+double(a.y)*a.y+double(a.z)*a.z+double(a.w)*a.w;
    const double bb=double(b.x)*b.x+double(b.y)*b.y+double(b.z)*b.z+double(b.w)*b.w;
    return std::isfinite(dot)&&std::abs(aa-1)<.01&&std::abs(bb-1)<.01&&
        std::abs(dot)/std::sqrt(aa*bb)>=std::cos(.005); // at most .57 degree per native step
}
inline Rdr2Matrix rdr2_multiply(const Rdr2Matrix& a,const Rdr2Matrix& b) noexcept {
    Rdr2Matrix c{};for(unsigned i=0;i<4;++i)for(unsigned j=0;j<4;++j)for(unsigned k=0;k<4;++k)c[i*4+j]+=a[i*4+k]*b[k*4+j];return c;
}
// RAGE uses world Z up. Rotate the game camera around that axis while keeping
// the current head orientation. Translation and the original history stay put.
inline Rdr2Matrix rdr2_phase_mapping(const Rdr2Matrix& clip_to_view,const Rdr2Matrix& view_to_world,
    const Rdr2Matrix& world_to_view,const Rdr2Matrix& view_to_clip,float yaw) noexcept {
    auto v=view_to_world,w=world_to_view;
    for(unsigned i=0;i<3;++i){v[12+i]=w[12+i]=0;v[i*4+3]=w[i*4+3]=0;}v[15]=w[15]=1;
    const float c=std::cos(yaw),s=std::sin(yaw);
    const Rdr2Matrix rotation{c,s,0,0,-s,c,0,0,0,0,1,0,0,0,0,1};
    return rdr2_multiply(rdr2_multiply(rdr2_multiply(rdr2_multiply(clip_to_view,v),rotation),w),view_to_clip);
}
}
