#include "xrfg/composition_panels.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
using namespace xrfg::panels;
void check(bool b){if(!b)std::abort();}
int main(){
    Pose a{};a.type=XR_TYPE_COMPOSITION_LAYER_QUAD;
    a.swapchain=reinterpret_cast<XrSwapchain>(1);a.space=reinterpret_cast<XrSpace>(2);
    a.rect.extent={512,512};a.pose.orientation.w=1;
    auto b=a;b.rect.offset.x=512;b.pose.position.x=5;
    std::array before{a,b};auto c=a;c.pose.position.x=2;
    std::array now{b,c};auto m=match(before,now);
    check(m[0]==1&&m[1]==0);check(between(before[m[0]].pose,now[0].pose,.5f).position.x==5);
    check(between(before[m[1]].pose,now[1].pose,.5f).position.x==1);
    now[1].rect.extent.width=400;m=match(before,now);check(m[1]==0);
    now[0].rect.offset.x=700;now[1].rect.offset.x=800;m=match(before,now);
    check(m[0]==2&&m[1]==2); // two resized panels are ambiguous
    now={a,a};now[1].slice=1;m=match(before,now);
    check(m[0]==0); // exact match cannot be stolen by another panel
    auto cylinder=a;cylinder.type=XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
    check(!same_source(a,cylinder));auto other=a;other.space=reinterpret_cast<XrSpace>(3);
    check(match(std::span(&a,1),std::span(&other,1))[0]==1);
    auto q=a.pose;q.orientation.w=-1;check(between(a.pose,q,.5f).orientation.w==1);
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};quad.subImage.imageArrayIndex=3;
    quad.subImage.imageRect={{512,64},{256,128}};quad.eyeVisibility=XR_EYE_VISIBILITY_RIGHT;
    const auto p=pose(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),4);
    check(p&&p->slice==3&&p->rect.offset.x==512&&p->eyes==XR_EYE_VISIBILITY_RIGHT&&p->layer==4);
    std::cout<<"Shared-atlas kneeboard identity, reordered/resized panels, spaces, slices and pose fractions passed\n";
}
