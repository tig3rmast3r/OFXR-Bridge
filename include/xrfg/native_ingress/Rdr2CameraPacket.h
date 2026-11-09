#pragma once
#include "RealVrDlssCamera.h"

namespace xrfg::native_ingress::rdr2_camera_packet {
inline bool matches_native_view(const unsigned char* bytes,std::size_t size,
    const std::array<unsigned char,48>& native) noexcept {
    if(!bytes||size<64)return false;
    for(unsigned row=0;row<4;++row)if(std::memcmp(bytes+row*16,native.data()+row*12,12))return false;
    return true;
}
struct Camera {
    std::array<float,3> position{},right{},up{},forward{};
    std::array<float,16> projection{},inverse{};
    float near_z{},far_z{},fov{};
};
// R7 PID46076: RAGE camera after REAL VR's native main-camera callback.
// Matrix rows are right, forward, up, position; the fourth lanes are padding
// (including NaN in the captured packet). FOV degrees/near/far/aspect at60..6c.
// Only the observed square source projection is established here. Cropped XR
// eye dimensions are deliberately not used to reconstruct this projection.
inline bool decode(const unsigned char* bytes,std::size_t size,unsigned width,
    unsigned height,bool inverted,Camera& out) noexcept {
    out={};if(!bytes||size<112||!width||width!=height)return false;
    const auto f=[&](unsigned offset){return real_vr_dlss_camera::read_float(bytes,offset);};
    Camera c;
    for(unsigned i=0;i<3;++i){c.right[i]=f(i*4);c.forward[i]=-f(16+i*4);
        c.up[i]=f(32+i*4);c.position[i]=f(48+i*4);}
    const auto dot=[](const auto& a,const auto& b){float sum{};for(unsigned i=0;i<3;++i)sum+=a[i]*b[i];return sum;};
    for(const auto* axis:{&c.right,&c.up,&c.forward})for(float v:*axis)if(!std::isfinite(v))return false;
    for(float v:c.position)if(!std::isfinite(v)||std::abs(v)>1e7F)return false;
    for(const auto* axis:{&c.right,&c.up,&c.forward})if(std::abs(dot(*axis,*axis)-1)>.002F)return false;
    if(std::abs(dot(c.right,c.up))>.002F||std::abs(dot(c.right,c.forward))>.002F||
        std::abs(dot(c.up,c.forward))>.002F)return false;
    std::array<float,3> cross{c.right[1]*c.up[2]-c.right[2]*c.up[1],
        c.right[2]*c.up[0]-c.right[0]*c.up[2],c.right[0]*c.up[1]-c.right[1]*c.up[0]};
    if(dot(cross,c.forward)<.998F)return false;
    const auto degrees=f(96),aspect=f(108);c.near_z=f(100);c.far_z=f(104);
    if(!std::isfinite(degrees)||degrees<10||degrees>170||!std::isfinite(aspect)||std::abs(aspect-1)>.002F||
        !std::isfinite(c.near_z)||c.near_z<=0||c.near_z>10||!std::isfinite(c.far_z)||c.far_z<=c.near_z)return false;
    c.fov=degrees*.017453292519943295F;
    const auto scale=1.F/std::tan(c.fov*.5F);auto& p=c.projection;
    p[0]=p[5]=scale;p[11]=-1;
    p[10]=inverted?c.near_z/(c.far_z-c.near_z):c.far_z/(c.near_z-c.far_z);
    p[14]=inverted?c.near_z*c.far_z/(c.far_z-c.near_z):c.near_z*c.far_z/(c.near_z-c.far_z);
    if(!real_vr_dlss_camera::invert(p,c.inverse))return false;
    out=c;return true;
}
}
