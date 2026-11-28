#pragma once
#include "daysgone_camera_packet.hpp"
#include "native_ingress/streamline_camera.hpp"
#include "dlss_motion_vectors.hpp"
#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace xrfg::daysgone_camera {
struct Binding {
    std::uint64_t context{},feature{},color{},output{},depth{},motion{};
    std::uint32_t thread{},render_width{},render_height{},output_width{},output_height{};
    float jitter_x{},jitter_y{};
    bool inverted{};
};
enum class Rejection : std::uint32_t {
    none,abi,identity,extent,jitter,clock,projection,basis,previous,unreadable
};
using Matrix=std::array<float,16>;
inline DirectX::XMMATRIX load(const float* values) noexcept {
    DirectX::XMFLOAT4X4 data;std::memcpy(&data,values,sizeof(data));
    return DirectX::XMLoadFloat4x4(&data);
}
inline Matrix store(DirectX::FXMMATRIX value) noexcept {
    DirectX::XMFLOAT4X4 data;DirectX::XMStoreFloat4x4(&data,value);
    Matrix out;std::memcpy(out.data(),&data,sizeof(data));return out;
}
inline bool finite(const float* values,unsigned count) noexcept {
    for(unsigned i=0;i<count;++i)if(!std::isfinite(values[i]))return false;
    return true;
}
inline bool decode_native(const NativeCamera& native,native_ingress::streamline_camera::Camera& out,
    Rejection& reason) noexcept {
    if(!finite(native.projection,16)||!finite(native.projection_jitter,2)||!finite(native.origin,3)) {
        reason=Rejection::projection;return false;
    }
    std::copy_n(native.projection,16,out.projection.begin());
    out.projection[8]-=native.projection_jitter[0];out.projection[9]-=native.projection_jitter[1];
    const auto& p=out.projection;
    // Verified Days Gone FViewMatrices: positive-Z, reversed infinite depth.
    if(!(p[0]>0&&p[5]>0&&p[14]>0)||std::abs(p[11]-1)>1e-4f||std::abs(p[15])>1e-4f||
        std::abs(p[10])>1e-4f)return reason=Rejection::projection,false;
    for(unsigned i:{1U,2U,3U,4U,6U,7U,12U,13U})
        if(std::abs(p[i])>1e-4f)return reason=Rejection::projection,false;
    const auto projection=load(p.data());
    DirectX::XMVECTOR determinant;const auto inverse=DirectX::XMMatrixInverse(&determinant,projection);
    if(!std::isfinite(DirectX::XMVectorGetX(determinant))||std::abs(DirectX::XMVectorGetX(determinant))<1e-8f)
        return reason=Rejection::projection,false;
    out.inverse=store(inverse);
    if(!finite(out.inverse.data(),16))return reason=Rejection::projection,false;
    const auto* v=native.translated_view;
    if(!finite(v,16)||std::abs(v[3])>1e-4f||std::abs(v[7])>1e-4f||std::abs(v[11])>1e-4f||std::abs(v[15]-1)>1e-4f)
        return reason=Rejection::basis,false;
    out.right={v[0],v[4],v[8]};out.up={v[1],v[5],v[9]};out.forward={v[2],v[6],v[10]};
    const auto dot=[](const auto& a,const auto& b){float s{};for(unsigned i=0;i<3;++i)s+=a[i]*b[i];return s;};
    if(std::abs(dot(out.right,out.right)-1)>.002f||std::abs(dot(out.up,out.up)-1)>.002f||
        std::abs(dot(out.forward,out.forward)-1)>.002f||std::abs(dot(out.right,out.up))>.002f||
        std::abs(dot(out.up,out.forward))>.002f||std::abs(dot(out.forward,out.right))>.002f)
        return reason=Rejection::basis,false;
    const std::array<float,3> cross{out.right[1]*out.up[2]-out.right[2]*out.up[1],
        out.right[2]*out.up[0]-out.right[0]*out.up[2],out.right[0]*out.up[1]-out.right[1]*out.up[0]};
    if(std::abs(dot(cross,out.forward)-1)>.002f)return reason=Rejection::basis,false;
    std::copy_n(native.origin,3,out.position.begin());out.near_z=p[14];out.far_z=0;
    out.fov=2.f*std::atan(1.f/p[5]);out.inverted=true;out.camera_motion=1;
    out.observed=out.valid=true;out.rejection=0;return true;
}
inline DirectX::XMMATRIX camera_to_world(const native_ingress::streamline_camera::Camera& c) noexcept {
    return DirectX::XMMatrixSet(c.right[0],c.right[1],c.right[2],0,c.up[0],c.up[1],c.up[2],0,
        c.forward[0],c.forward[1],c.forward[2],0,c.position[0],c.position[1],c.position[2],1);
}
inline bool decode(const Packet& packet,const Binding& b,std::uint64_t last_sequence,
    native_ingress::streamline_camera::Camera& out,Rejection& reason) noexcept {
    out={};reason=Rejection::none;
    if(packet.bytes!=sizeof(Packet)||packet.version!=1||packet.provider!=1||
        (packet.game_build!=113&&packet.game_build!=107)||(packet.flags&~1U))return reason=Rejection::abi,false;
    if(!b.context||!b.feature||!b.color||!b.output||!b.depth||!b.motion||packet.thread!=b.thread||
        packet.context!=b.context||packet.feature!=b.feature||packet.color!=b.color||packet.output!=b.output||
        packet.depth!=b.depth||packet.motion!=b.motion)return reason=Rejection::identity,false;
    if(!b.render_width||!b.render_height||!b.output_width||!b.output_height||
        packet.render_width!=b.render_width||packet.render_height!=b.render_height||
        packet.output_width!=b.output_width||packet.output_height!=b.output_height)return reason=Rejection::extent,false;
    if(!std::isfinite(packet.jitter_x)||!std::isfinite(packet.jitter_y)||
        packet.jitter_x!=b.jitter_x||packet.jitter_y!=b.jitter_y)return reason=Rejection::jitter,false;
    if(!packet.frame||!packet.sequence||packet.sequence<=last_sequence)return reason=Rejection::clock,false;
    if(!b.inverted)return reason=Rejection::projection,false;
    native_ingress::streamline_camera::Camera current;
    if(!decode_native(packet.camera,current,reason))return false;
    current.frame=packet.frame;current.viewport=0;
    current.previous=current.to_current=store(DirectX::XMMatrixIdentity());
    if(packet.flags&1U) {
        if(!packet.previous_sequence||packet.previous_sequence+1!=packet.sequence||
            packet.previous_frame+1!=packet.frame)return reason=Rejection::previous,false;
        native_ingress::streamline_camera::Camera previous;
        if(!decode_native(packet.previous_camera,previous,reason))return reason=Rejection::previous,false;
        const auto clip_to_world=load(current.inverse.data())*camera_to_world(current);
        const auto world_to_previous=DirectX::XMMatrixInverse(nullptr,camera_to_world(previous));
        const auto transform=clip_to_world*world_to_previous*load(previous.projection.data());
        current.previous=store(transform);current.to_current=store(DirectX::XMMatrixInverse(nullptr,transform));
        if(!finite(current.previous.data(),16)||!finite(current.to_current.data(),16))return reason=Rejection::previous,false;
    }
    out=current;return true;
}
inline void apply(DlssMotionVectorPublication& g,const Packet& packet,
    const native_ingress::streamline_camera::Camera& camera,std::uint64_t source,bool full_osr=false) noexcept {
    g.camera_view_to_clip=camera.projection;g.camera_clip_to_view=camera.inverse;
    g.camera_position=camera.position;g.camera_up=camera.up;g.camera_right=camera.right;g.camera_forward=camera.forward;
    g.camera_near=camera.near_z;g.camera_far=camera.far_z;g.camera_fov_vertical=camera.fov;
    g.depth_inverted=true;g.depth_infinite=true;g.camera_data_valid=g.camera_matrices_valid=true;
    g.camera_frame_token=packet.frame;g.camera_frame_token_valid=true;g.protect_snapshot_lifetime=true;
    g.raw_camera_motion_included=1;g.raw_clip_to_previous_clip=camera.previous;g.raw_previous_clip_to_clip=camera.to_current;
    g.native_production_sequence=packet.sequence;g.raw_motion_native_span=1;g.raw_motion_native_span_confirmed=true;
    // The stock addon supplies original adjacent-eye N1 (owner F8 PID38260).
    // Reuse CP77's depth-validated original history for attached surfaces and
    // its exact two-interval object composition. Legacy AFW keeps its contract.
    g.compose_native_object_history=full_osr;
    g.osr_native_cabin_motion=full_osr;
    // The addon raster and integer REALVR colour crop share native pixel
    // centres. Window rounding and this frame's jitter are handled by OSR;
    // CP77's measured residual +.36 texel is not part of this producer.
    g.osr_depth_alignment=std::array<float,2>{0,0};
    g.raw_motion_color_aligned=true;
    g.source_clock_owner=source;g.source_clock_sequence=packet.sequence;
    if(!(packet.flags&1U))g.reset=true;
}
}
