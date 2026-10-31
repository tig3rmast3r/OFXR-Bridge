#pragma once
// Panel identity and pose interpolation, adapted from djules75/OFXR-Bridge
// V508 and V574 (2d9b3e1e, 4bf811e0). Keep shared-atlas panels distinct.
#include <openxr/openxr.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <vector>

namespace xrfg::panels {
struct Pose {
    XrStructureType type{};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    XrSpace space{XR_NULL_HANDLE};
    XrPosef pose{};
    XrRect2Di rect{};
    uint32_t slice{};
    XrEyeVisibility eyes{};
    uint32_t layer{};
};
inline std::optional<Pose> pose(const XrCompositionLayerBaseHeader* h,uint32_t layer=0) {
    if(!h)return {};
    if(h->type==XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const auto& q=*reinterpret_cast<const XrCompositionLayerQuad*>(h);
        return Pose{h->type,q.subImage.swapchain,q.space,q.pose,q.subImage.imageRect,
            q.subImage.imageArrayIndex,q.eyeVisibility,layer};
    }
    if(h->type==XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
        const auto& q=*reinterpret_cast<const XrCompositionLayerCylinderKHR*>(h);
        return Pose{h->type,q.subImage.swapchain,q.space,q.pose,q.subImage.imageRect,
            q.subImage.imageArrayIndex,q.eyeVisibility,layer};
    }
    return {};
}
inline bool same_source(const Pose& a,const Pose& b) {
    return a.type==b.type&&a.swapchain==b.swapchain&&a.space==b.space;
}
inline bool same_image(const Pose& a,const Pose& b) {
    return same_source(a,b)&&a.rect.offset.x==b.rect.offset.x&&a.rect.offset.y==b.rect.offset.y&&
        a.rect.extent.width==b.rect.extent.width&&a.rect.extent.height==b.rect.extent.height&&
        a.slice==b.slice&&a.eyes==b.eyes;
}
// Exact atlas rectangle/slice/eyes first. A changed rectangle can match only
// the sole unmatched panel of that source on BOTH sides (upstream V574).
inline std::vector<size_t> match(std::span<const Pose> before,std::span<const Pose> now) {
    std::vector<size_t> result(now.size(),before.size());
    std::vector<bool> taken(before.size());
    for(size_t i=0;i<now.size();++i)for(size_t j=0;j<before.size();++j)
        if(!taken[j]&&same_image(now[i],before[j])){result[i]=j;taken[j]=true;break;}
    for(size_t i=0;i<now.size();++i)if(result[i]==before.size()) {
        size_t left_before=0,left_now=0,candidate=before.size();
        for(size_t j=0;j<before.size();++j)if(!taken[j]&&same_source(now[i],before[j]))
            {++left_before;candidate=j;}
        for(size_t j=0;j<now.size();++j)if(result[j]==before.size()&&same_source(now[i],now[j]))++left_now;
        if(left_before==1&&left_now==1){result[i]=candidate;taken[candidate]=true;}
    }
    return result;
}
inline XrPosef between(const XrPosef& a,const XrPosef& b,float t) {
    XrPosef p=b;
    p.position={a.position.x+(b.position.x-a.position.x)*t,
        a.position.y+(b.position.y-a.position.y)*t,a.position.z+(b.position.z-a.position.z)*t};
    auto q=b.orientation;
    if(a.orientation.x*q.x+a.orientation.y*q.y+a.orientation.z*q.z+a.orientation.w*q.w<0)
        q={-q.x,-q.y,-q.z,-q.w};
    q={a.orientation.x+(q.x-a.orientation.x)*t,a.orientation.y+(q.y-a.orientation.y)*t,
        a.orientation.z+(q.z-a.orientation.z)*t,a.orientation.w+(q.w-a.orientation.w)*t};
    const float n=std::sqrt(q.x*q.x+q.y*q.y+q.z*q.z+q.w*q.w);
    if(!(n>1e-6f)||!std::isfinite(n))return b;
    p.orientation={q.x/n,q.y/n,q.z/n,q.w/n};return p;
}
}

