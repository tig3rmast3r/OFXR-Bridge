#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_motion_format.hpp"
#include "xrfg/luke_pose_camera.hpp"
#include "xrfg/luke_interleaved_history.hpp"
#include <cmath>
#include <algorithm>

namespace xrfg {
struct LukeNativeMotionConstants {
    std::array<std::uint32_t,2> source_base{},crop{};
    std::array<float,2> extent{},scale{},jitter_difference{},padding{};
    std::array<float,16> opposite{},same{};
    // Producer depth-dependent stereo interval. Exact native history already
    // includes it in opposite/same; identity makes its disparity zero there.
    std::array<float,16> stereo{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
};
static_assert(sizeof(LukeNativeMotionConstants)==60*sizeof(std::uint32_t));
namespace native_motion_detail {
using Matrix=std::array<std::array<double,4>,4>;
inline Matrix matrix(const std::array<float,16>& values) noexcept {
    Matrix m{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)m[r][c]=values[c*4+r];return m;
}
inline Matrix multiply(const Matrix& a,const Matrix& b) noexcept {
    Matrix m{};for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)
        for(unsigned k=0;k<4;++k)m[r][c]+=a[r][k]*b[k][c];return m;
}
inline bool inverse(Matrix a,Matrix& out) noexcept {
    out={};for(unsigned i=0;i<4;++i)out[i][i]=1;
    for(unsigned c=0;c<4;++c) {
        unsigned pivot=c;for(unsigned r=c+1;r<4;++r)if(std::abs(a[r][c])>std::abs(a[pivot][c]))pivot=r;
        if(!std::isfinite(a[pivot][c]) || std::abs(a[pivot][c])<1e-10)return false;
        std::swap(a[c],a[pivot]);std::swap(out[c],out[pivot]);double d=a[c][c];
        for(unsigned j=0;j<4;++j){a[c][j]/=d;out[c][j]/=d;}
        for(unsigned r=0;r<4;++r)if(r!=c){d=a[r][c];for(unsigned j=0;j<4;++j){a[r][j]-=d*a[c][j];out[r][j]-=d*out[c][j];}}
    }
    return true;
}
inline bool transform(const DlssNativeCameraHistory& current,
    const DlssNativeCameraHistory& target,std::array<float,16>& output,float up_sign=1.F,
    bool native_view_rows=false) noexcept {
    Matrix source{},destination{},to_target{};source[3][3]=destination[3][3]=1;
    for(unsigned r=0;r<3;++r) {
        source[r][0]=current.right[r];source[r][1]=up_sign*current.up[r];
        source[r][2]=(native_view_rows?1.F:std::copysign(1.F,current.projection[11]))*current.forward[r];
        destination[r][0]=target.right[r];destination[r][1]=up_sign*target.up[r];
        destination[r][2]=(native_view_rows?1.F:std::copysign(1.F,target.projection[11]))*target.forward[r];
        // Subtract world origins in double before rotation; no 2km float cancellation.
        source[r][3]=double(current.position[r])-target.position[r];
    }
    if(!inverse(destination,to_target))return false;
    const auto m=multiply(multiply(matrix(target.projection),multiply(to_target,source)),matrix(current.inverse_projection));
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c) {
        output[c*4+r]=static_cast<float>(m[r][c]);if(!std::isfinite(output[c*4+r]))return false;
    }
    return true;
}
inline Matrix guide_to_window(unsigned width,unsigned height,
    unsigned x,unsigned y,unsigned window_width,unsigned window_height) noexcept {
    Matrix m{}; m[2][2]=m[3][3]=1;
    m[0][0]=double(width)/window_width;
    m[1][1]=double(height)/window_height;
    m[0][3]=(double(width)-2.0*x)/window_width-1.0;
    m[1][3]=1.0-(double(height)-2.0*y)/window_height;
    return m;
}
inline std::array<float,16> values(const Matrix& m) noexcept {
    std::array<float,16> out{};
    for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)out[c*4+r]=float(m[r][c]);
    return out;
}
inline std::array<float,16> window_to_guide_transform(
    const std::array<float,16>& transform,const Matrix& to_window) noexcept {
    Matrix to_guide{};inverse(to_window,to_guide);
    return values(multiply(multiply(to_guide,matrix(transform)),to_window));
}
inline std::array<float,16> guide_to_window_transform(
    const std::array<float,16>& transform,const Matrix& to_window) noexcept {
    Matrix to_guide{};inverse(to_window,to_guide);
    return values(multiply(multiply(to_window,matrix(transform)),to_guide));
}
}
inline bool luke_native_motion_constants(const DlssMotionVectorFrame& guide,
    unsigned crop_x,unsigned crop_y,LukeNativeMotionConstants& out,unsigned& failure,
    LukeProducerCameraAxes axes=LukeProducerCameraAxes::view) noexcept {
    out={};failure=1;
    if(!guide.previous_opposite_guide || !guide.previous_same_native_camera)return false;
    const auto current=dlss_native_camera_history(guide);
    const auto middle=dlss_native_camera_history(*guide.previous_opposite_guide);
    const auto& older=*guide.previous_same_native_camera;
    failure=2;
    const bool interleaved = guide.luke_interleaved_history &&
        luke_interleaved_history_matches(current, middle, older);
    if((!interleaved && (guide.serial<3 || !guide.stream || !guide.epoch || !guide.publication ||
        middle.stream!=current.stream || older.stream!=current.stream ||
        middle.epoch!=current.epoch || older.epoch!=current.epoch ||
        middle.serial+1!=current.serial || older.serial+2!=current.serial ||
        middle.publication+1!=current.publication || older.publication+2!=current.publication)) ||
        guide.previous_serial!=older.serial)return false;
    failure=3;
    if((guide.raw_motion_native_span!=1&&guide.raw_motion_native_span!=2)||
        middle.raw_motion_native_span!=current.raw_motion_native_span||
        older.raw_motion_native_span!=current.raw_motion_native_span)return false;
    for(const auto* f:{&current,&middle,&older}) {
        if(!f->valid || !f->token || f->reset || f->jittered ||
            !std::isfinite(f->jitter_x) || !std::isfinite(f->jitter_y) ||
            std::abs(std::abs(f->projection[11])-1.F)>1e-5F)return false;
        for(const auto* values:{&f->projection,&f->inverse_projection})
            for(float v:*values)if(!std::isfinite(v))return false;
        for(const auto* values:{&f->position,&f->right,&f->up,&f->forward})
            for(float v:*values)if(!std::isfinite(v))return false;
    }
    auto next=[](std::uint32_t v){++v;return v?v:1;};
    failure=4;
    if(current.native_production_sequence || middle.native_production_sequence || older.native_production_sequence) {
        // KCD camera receipt IDs count probes rather than rendered frames.
        // Require all three exact SR productions, including every failed
        // capture; do not compact successful publications or mix identities.
        if(!older.native_production_sequence || !middle.native_production_sequence || !current.native_production_sequence ||
            older.native_production_sequence==UINT64_MAX || middle.native_production_sequence==UINT64_MAX ||
            older.native_production_sequence+1!=middle.native_production_sequence ||
            middle.native_production_sequence+1!=current.native_production_sequence)return false;
    } else if(next(older.token)!=middle.token || next(middle.token)!=current.token)return false;
    failure=5;
    if(!current.motion_width || !current.motion_height || guide.motion_width!=guide.depth_width ||
        guide.motion_height!=guide.depth_height)return false;
    for(const auto* f:{&middle,&older})if(f->motion_width!=current.motion_width || f->motion_height!=current.motion_height ||
        f->output_width!=current.output_width || f->output_height!=current.output_height)return false;
    failure=6;
    if(!std::isfinite(guide.scale_x) || !std::isfinite(guide.scale_y) || !guide.scale_x || !guide.scale_y)return false;
    failure=7;
    if(!guide.motion_vectors || !guide.motion_retention || !guide.depth_retention || !guide.motion_snapshot_device ||
        guide.motion_slice || !(guide.resource_state&D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))return false;
    const auto d=guide.motion_vectors->GetDesc();failure=8;
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize!=1 || d.SampleDesc.Count!=1 ||
        (d.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || luke_motion_srv_format(d.Format)==DXGI_FORMAT_UNKNOWN ||
        std::uint64_t(guide.motion_x)+guide.motion_width>d.Width || std::uint64_t(guide.motion_y)+guide.motion_height>d.Height)return false;
    out.source_base={guide.motion_x,guide.motion_y};out.crop={crop_x,crop_y};
    out.extent={float(guide.motion_width),float(guide.motion_height)};out.scale={guide.scale_x,guide.scale_y};
    // N-2 input already carries its same-eye jitter difference. Only N-1 input
    // needs the missing middle->older jitter interval added to reconstruction.
    out.jitter_difference=guide.raw_motion_native_span==2?std::array<float,2>{0,0}:
        std::array<float,2>{middle.jitter_x-older.jitter_x,middle.jitter_y-older.jitter_y};
    out.padding[0]=float(guide.raw_motion_native_span);failure=9;
    // DOOM's original pre-wrapper packet supplies DOWN, while its captured
    // motion/depth raster and projection use view UP. Decode all three native
    // cameras exactly as the scene camera does; never flip the motion field.
    const float up_sign=axes==LukeProducerCameraAxes::right_down_forward?-1.F:1.F;
    // Indiana's declared cameraFwd is already the view-to-world third row.
    // Preserve it at all three endpoints, as in the accepted OptiScaler scene
    // camera. Projection handedness is not a reason to reflect that row again.
    const bool native_view_rows=axes==LukeProducerCameraAxes::native_view_rows;
    if(!native_motion_detail::transform(current,middle,out.opposite,up_sign,native_view_rows) ||
        !native_motion_detail::transform(current,older,out.same,up_sign,native_view_rows))return false;
    if(guide.compose_native_object_history) {
        failure=10;
        const auto& peer=*guide.previous_opposite_guide;
        if(guide.raw_motion_native_span!=1 || !peer.compose_native_object_history ||
            !peer.motion_vectors || !peer.depth || !peer.motion_retention || !peer.depth_retention ||
            peer.motion_x!=guide.motion_x || peer.motion_y!=guide.motion_y ||
            peer.depth_x!=guide.motion_x || peer.depth_y!=guide.motion_y ||
            peer.depth_width!=guide.motion_width || peer.depth_height!=guide.motion_height ||
            peer.scale_x!=guide.scale_x || peer.scale_y!=guide.scale_y)
            return false;
        Microsoft::WRL::ComPtr<IUnknown> owner,motion_owner,depth_owner;
        if(!peer.motion_snapshot_device || !peer.depth_snapshot_device ||
            FAILED(guide.motion_snapshot_device.As(&owner)) ||
            FAILED(peer.motion_snapshot_device.As(&motion_owner)) ||
            FAILED(peer.depth_snapshot_device.As(&depth_owner)) ||
            owner.Get()!=motion_owner.Get() || owner.Get()!=depth_owner.Get())return false;
        const auto mv=peer.motion_vectors->GetDesc(), dz=peer.depth->GetDesc();
        if(mv.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || dz.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            mv.DepthOrArraySize!=1 || dz.DepthOrArraySize!=1 || mv.SampleDesc.Count!=1 || dz.SampleDesc.Count!=1 ||
            std::uint64_t(peer.motion_x)+peer.motion_width>mv.Width || std::uint64_t(peer.motion_y)+peer.motion_height>mv.Height ||
            std::uint64_t(peer.depth_x)+peer.depth_width>dz.Width || std::uint64_t(peer.depth_y)+peer.depth_height>dz.Height ||
            (mv.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) || (dz.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))return false;
        if(!native_motion_detail::transform(middle,older,out.stereo,up_sign,native_view_rows))return false;
        out.padding[1]=1.F;
    }
    failure=0;return true;
}
}
