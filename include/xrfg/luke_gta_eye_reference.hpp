#pragma once
#include "xrfg/luke_stereo_pose.hpp"
#include <algorithm>

namespace xrfg {
struct LukeGtaEyeCamera {
    std::array<float,16> view_to_world{};
    Quaternion xr_orientation{};
    unsigned eye{};
};
namespace gta_eye_reference_detail {
using V=std::array<double,3>;
inline V add(V a,V b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
inline V scale(V a,double s){return {a[0]*s,a[1]*s,a[2]*s};}
inline V cross(V a,V b){return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
inline double dot(V a,V b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
inline V rotate(Quaternion q,V p){const auto v=stereo_pose_detail::rotate(q,{float(p[0]),float(p[1]),float(p[2])});return {v.x,v.y,v.z};}
inline Quaternion mul(Quaternion a,Quaternion b){return stereo_pose_detail::product(a,b);}
inline Quaternion inv(Quaternion q){return stereo_pose_detail::inverse(q);}
struct Transform {Quaternion q{};V p{};};
inline Transform product(Transform a,Transform b){return {mul(a.q,b.q),add(a.p,rotate(a.q,b.p))};}
inline Transform inverse(Transform a){const auto q=inv(a.q);return {q,rotate(q,scale(a.p,-1))};}
inline V rotation_vector(Quaternion q){
    stereo_pose_detail::unit(q);if(q.w<0)q={-q.x,-q.y,-q.z,-q.w};
    const double s=std::sqrt(double(q.x)*q.x+double(q.y)*q.y+double(q.z)*q.z);
    const double k=s>1e-12?2*std::atan2(s,double(q.w))/s:2;
    return {q.x*k,q.y*k,q.z*k};
}
inline Transform fraction(Transform d,double a){
    const auto w=rotation_vector(d.q);const double theta=std::sqrt(dot(w,w));
    const double t2=theta*theta;
    const double A=theta<1e-4?1./12+t2/720:(1-theta*.5/std::tan(theta*.5))/t2;
    const auto wt=cross(w,d.p);const auto v=add(add(d.p,scale(wt,-.5)),scale(cross(w,wt),A));
    const double at=a*theta;
    const double B=theta<1e-4?a*a*.5:(1-std::cos(at))/t2;
    const double C=theta<1e-4?a*a*a/6:(at-std::sin(at))/(t2*theta);
    const auto wv=cross(w,v);
    const double k=theta>1e-12?std::sin(at*.5)/theta:a*.5;
    return {{float(w[0]*k),float(w[1]*k),float(w[2]*k),float(std::cos(at*.5))},
        add(add(scale(v,a),scale(wv,B)),scale(cross(w,wv),C))};
}
inline bool decode(const std::array<float,16>& m,Transform& out){
    if(!std::all_of(m.begin(),m.end(),[](float f){return std::isfinite(f);})||
        std::abs(m[3])+std::abs(m[7])+std::abs(m[11])>1e-4F||std::abs(m[15]-1)>1e-4F)return false;
    const V x{m[0],m[1],m[2]},y{m[4],m[5],m[6]},z{m[8],m[9],m[10]};
    if(std::abs(dot(x,x)-1)+std::abs(dot(y,y)-1)+std::abs(dot(z,z)-1)>1e-3||
        std::abs(dot(x,y))+std::abs(dot(x,z))+std::abs(dot(y,z))>1e-3||dot(cross(x,y),z)<.999)return false;
    Quaternion q;const double tr=m[0]+m[5]+m[10];
    if(tr>0){const double s=2*std::sqrt(tr+1);q={float((m[6]-m[9])/s),float((m[8]-m[2])/s),float((m[1]-m[4])/s),float(s*.25)};}
    else if(m[0]>m[5]&&m[0]>m[10]){const double s=2*std::sqrt(1+m[0]-m[5]-m[10]);q={float(s*.25),float((m[4]+m[1])/s),float((m[8]+m[2])/s),float((m[6]-m[9])/s)};}
    else if(m[5]>m[10]){const double s=2*std::sqrt(1+m[5]-m[0]-m[10]);q={float((m[4]+m[1])/s),float(s*.25),float((m[9]+m[6])/s),float((m[8]-m[2])/s)};}
    else {const double s=2*std::sqrt(1+m[10]-m[0]-m[5]);q={float((m[8]+m[2])/s),float((m[9]+m[6])/s),float(s*.25),float((m[1]-m[4])/s)};}
    if(!stereo_pose_detail::unit(q))return false;out={q,{m[12],m[13],m[14]}};return true;
}
inline std::array<float,16> encode(Transform t){
    stereo_pose_detail::unit(t.q);std::array<float,16> m{};m[15]=1;
    for(unsigned i=0;i<3;++i){V axis{};axis[i]=1;const auto c=rotate(t.q,axis);for(unsigned j=0;j<3;++j)m[i*4+j]=float(c[j]);m[12+i]=float(t.p[i]);}return m;
}
inline bool body(const LukeGtaEyeCamera& c,double baseline,Transform& b){
    Transform native;if(c.eye>1||!decode(c.view_to_world,native))return false;
    auto head=c.xr_orientation;if(!stereo_pose_detail::unit(head))return false;
    const Transform eye{head,rotate(head,{c.eye==0?-baseline:0,0,0})};
    b=product(native,inverse(eye));return true;
}
} // namespace gta_eye_reference_detail

// A/B/A native headlock views can carry alternating gameplay-camera references.
// Transfer only motion measured within ONE native eye to the exact peer's own
// reference. Do not replace that reference with the other eye's base. The
// fraction names the last N1 interval within the measured same-eye N2 interval.
// Stable/monotonic bases keep the existing current-scene path bit-for-bit.
inline std::optional<std::array<float,16>> luke_gta_peer_eye_reference(
    const LukeGtaEyeCamera& current,const LukeGtaEyeCamera& same,
    const LukeGtaEyeCamera& peer,float baseline,float interval_fraction) noexcept {
    using namespace gta_eye_reference_detail;
    if(current.eye>1||same.eye!=current.eye||peer.eye!=1-current.eye||
        !std::isfinite(baseline)||baseline<=0||!std::isfinite(interval_fraction)||
        interval_fraction<=0||interval_fraction>=1)return {};
    Transform c,s,p;if(!body(current,baseline,c)||!body(same,baseline,s)||!body(peer,baseline,p))return {};
    // Admit a monotonic physical head turn, not alternating physical eye
    // orientations (or a real reversal). Only the native gameplay base may
    // alternate. This preserves ordinary stereo/canted-eye histories.
    auto hc=current.xr_orientation,hs=same.xr_orientation,hp=peer.xr_orientation;
    if(!stereo_pose_detail::unit(hc)||!stereo_pose_detail::unit(hs)||!stereo_pose_detail::unit(hp))return {};
    if(dot(rotation_vector(mul(hc,inv(hp))),rotation_vector(mul(hp,inv(hs))))<=0)return {};
    const auto last=rotation_vector(mul(c.q,inv(p.q)));
    const auto previous=rotation_vector(mul(p.q,inv(s.q)));
    const auto delta=product(c,inverse(s));const auto total=rotation_vector(delta.q);
    // Opposing steps whose N2 change is smaller than either N1 step identify
    // the observed alternating reference. No fitted head-motion gain or yaw.
    if(dot(last,previous)>=0||dot(total,total)>=std::min(dot(last,last),dot(previous,previous)))return {};
    auto h=peer.xr_orientation;if(!stereo_pose_detail::unit(h))return {};
    const Transform peer_head{h,rotate(h,{peer.eye==0?-double(baseline):0,0,0})};
    return encode(product(product(fraction(delta,interval_fraction),p),peer_head));
}
} // namespace xrfg
