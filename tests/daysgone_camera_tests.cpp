#include "xrfg/daysgone_camera.hpp"
#include "daysgone_motion_samples.hpp"
#include "daysgone_alignment_samples.hpp"
#include "daysgone_colour_samples.hpp"
#include "xrfg/osr_full_warp.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace xrfg::daysgone_camera;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
NativeCamera native(float x=0) {
    NativeCamera c;
    c.projection[0]=1.5f;c.projection[5]=2;c.projection[11]=1;c.projection[14]=10;
    c.projection_jitter[0]=.001f;c.projection_jitter[1]=-.002f;
    c.projection[8]=c.projection_jitter[0];c.projection[9]=c.projection_jitter[1];
    c.translated_view[0]=c.translated_view[5]=c.translated_view[10]=c.translated_view[15]=1;
    c.origin[0]=x;return c;
}
Packet packet() {
    Packet p;p.game_build=113;p.sequence=12;p.previous_sequence=11;p.frame=102;p.previous_frame=101;p.thread=77;p.flags=1;
    p.context=1;p.feature=2;p.color=3;p.output=4;p.depth=5;p.motion=6;
    p.render_width=960;p.render_height=720;p.output_width=1920;p.output_height=1440;
    p.jitter_x=.125f;p.jitter_y=-.25f;p.camera=native(2);p.previous_camera=native(1);return p;
}
Binding binding(const Packet& p) {
    return {p.context,p.feature,p.color,p.output,p.depth,p.motion,p.thread,p.render_width,p.render_height,
        p.output_width,p.output_height,p.jitter_x,p.jitter_y,true};
}
void run() {
    auto p=packet();const auto b=binding(p);xrfg::native_ingress::streamline_camera::Camera c;Rejection r;
    check(decode(p,b,11,c,r),"exact native camera rejected");
    check(c.valid&&c.observed&&c.rejection==0&&c.frame==102&&c.position[0]==2,"camera identity/position lost");
    check(c.projection[8]==0&&c.projection[9]==0,"projection jitter was not removed");
    check(c.inverted&&c.near_z==10&&c.far_z==0&&c.camera_motion==1,"native reversed depth/motion convention lost");
    check(std::abs(c.fov-2*std::atan(.5f))<1e-6f,"FOV does not belong to native projection");
    check(c.right==std::array<float,3>{1,0,0}&&c.up==std::array<float,3>{0,1,0}&&c.forward==std::array<float,3>{0,0,1},
        "native view basis changed");
    const auto identity=store(load(c.projection.data())*load(c.inverse.data()));
    for(unsigned i=0;i<16;++i)check(std::abs(identity[i]-(i%5==0?1.f:0.f))<1e-5f,"projection inverse invalid");
    const auto motion_identity=store(load(c.previous.data())*load(c.to_current.data()));
    for(unsigned i=0;i<16;++i)check(std::abs(motion_identity[i]-(i%5==0?1.f:0.f))<1e-5f,"history inverse invalid");
    check(std::abs(c.previous[8]-.15f)<1e-5f,"camera motion is not current clip to the exact preceding frame");
    xrfg::DlssMotionVectorPublication guide;apply(guide,p,c,99);
    check(guide.camera_data_valid&&guide.camera_matrices_valid&&guide.camera_frame_token==102&&
        guide.source_clock_owner==99&&guide.source_clock_sequence==12&&guide.native_production_sequence==12&&
        guide.depth_infinite&&guide.depth_inverted&&guide.camera_near==10,"exact camera did not enter the guide contract");
    check(!guide.compose_native_object_history&&!guide.osr_native_cabin_motion,
        "legacy AFW must retain the existing Days Gone motion contract");
    apply(guide,p,c,99,true);
    check(guide.compose_native_object_history&&guide.osr_native_cabin_motion&&
        guide.raw_motion_native_span==1&&guide.raw_motion_native_span_confirmed,
        "full OSR must retain original N1 attachment history and compose exact object intervals");
    apply(guide,p,c,99,false);
    check(!guide.compose_native_object_history&&!guide.osr_native_cabin_motion,
        "leaving full OSR must clear its N1 history policy");
    check(guide.osr_depth_alignment==std::optional{std::array<float,2>{0,0}},"Days Gone must not inherit the CP77 residual alignment");
    check(guide.raw_motion_color_aligned,"original Days Gone vectors already map unjittered resolved colour centres");
    double neutral_score{},legacy_score{};
    for(const auto& sample:daysgone_alignment_samples::cases) {
        const auto offset=xrfg::osr_full_depth_dejitter({sample.fraction,0},{sample.jitter,0},guide.osr_depth_alignment);
        const float residual=offset[0]+sample.fraction+sample.jitter;
        const auto index=static_cast<unsigned>(std::lround((residual+.4F)*100));
        check(index<sample.scores.size(),"alignment left the measured capture range");neutral_score+=sample.scores[index];
        const auto legacy=xrfg::osr_full_depth_dejitter({sample.fraction,0},{sample.jitter,0});
        check(std::abs(legacy[0]-offset[0]-.36F)<1e-6f,"existing titles must retain their established alignment");
        legacy_score+=sample.scores[76];
    }
    check(neutral_score>3.7&&neutral_score>legacy_score+1,
        "producer offset must align depth/colour edges across both eyes and captured jitter phases");
    std::cout<<"Depth/RGB edge support: native="<<neutral_score/4<<" legacy CP77="<<legacy_score/4<<'\n';
    const auto reject=[&](Packet bad,Rejection wanted,const char* why) {
        xrfg::native_ingress::streamline_camera::Camera result;
        check(!decode(bad,b,11,result,r)&&r==wanted&&!result.valid,why);
    };
    auto bad=p;bad.bytes--;reject(bad,Rejection::abi,"truncated ABI admitted");
    bad=p;bad.version=2;reject(bad,Rejection::abi,"unknown ABI admitted");
    bad=p;bad.provider=9;reject(bad,Rejection::abi,"unknown provider admitted");
    bad=p;bad.game_build=114;reject(bad,Rejection::abi,"unverified build admitted");
    bad=p;bad.flags=3;reject(bad,Rejection::abi,"unknown packet flags admitted");
    for(auto field:{&Packet::context,&Packet::feature,&Packet::color,&Packet::output,&Packet::depth,&Packet::motion}) {
        bad=p;++(bad.*field);reject(bad,Rejection::identity,"another evaluation's input admitted");
    }
    bad=p;bad.thread++;reject(bad,Rejection::identity,"another thread's camera admitted");
    bad=p;bad.render_width++;reject(bad,Rejection::extent,"another render extent admitted");
    bad=p;bad.output_height++;reject(bad,Rejection::extent,"another output extent admitted");
    bad=p;bad.jitter_x+=.01f;reject(bad,Rejection::jitter,"another jitter phase admitted");
    bad=p;bad.jitter_y=std::numeric_limits<float>::quiet_NaN();reject(bad,Rejection::jitter,"NaN jitter admitted");
    bad=p;bad.sequence=11;reject(bad,Rejection::clock,"stale packet admitted");
    bad=p;bad.sequence=0;reject(bad,Rejection::clock,"missing clock admitted");
    bad=p;bad.frame=0;reject(bad,Rejection::clock,"unknown game frame admitted");
    bad=p;bad.camera.projection[11]=-1;reject(bad,Rejection::projection,"foreign depth convention admitted");
    bad=p;bad.camera.projection[14]=-10;reject(bad,Rejection::projection,"negative near plane admitted");
    bad=p;bad.camera.projection[5]=0;reject(bad,Rejection::projection,"singular projection admitted");
    bad=p;bad.camera.projection[0]=std::numeric_limits<float>::infinity();reject(bad,Rejection::projection,"nonfinite projection admitted");
    bad=p;bad.camera.translated_view[0]=2;reject(bad,Rejection::basis,"scaled camera basis admitted");
    bad=p;bad.camera.translated_view[0]=-1;reject(bad,Rejection::basis,"reflected camera basis admitted");
    bad=p;bad.previous_frame-=1;reject(bad,Rejection::previous,"stale previous frame admitted");
    bad=p;bad.previous_sequence-=1;reject(bad,Rejection::previous,"missing previous view admitted");
    bad=p;bad.previous_camera.translated_view[5]=0;reject(bad,Rejection::previous,"invalid history camera admitted");
    bad=p;bad.flags=0;bad.previous_frame=0;bad.previous_sequence=0;
    check(decode(bad,b,11,c,r)&&c.previous==store(DirectX::XMMatrixIdentity()),"first/reset camera must prime without invented history");
    auto changed_binding=b;changed_binding.inverted=false;
    check(!decode(p,changed_binding,11,c,r),"non-inverted NGX feature admitted");
    p.game_build=107;check(decode(p,b,11,c,r),"verified classic camera ABI rejected");
}
}
int main(){try{run();
    std::array<double,15> original{},r12{},swapped{};
    for(unsigned i=0;i<original.size();++i){const auto& p=daysgone_colour_samples::points[i];
        original[i]=std::hypot(p[4]-p[2],p[5]-p[3]);
        r12[i]=std::hypot(p[4]+daysgone_colour_samples::old_added_phase[0]-p[2],p[5]+daysgone_colour_samples::old_added_phase[1]-p[3]);
        swapped[i]=std::hypot(-p[4]-p[2],-p[5]-p[3]);}
    for(auto* errors:{&original,&r12,&swapped})std::sort(errors->begin(),errors->end());
    check(original[7]<.25&&r12[7]>1&&swapped[7]>30,
        "actual character correspondence requires original N1, without repeated raster phase or eye reversal");
    std::cout<<"Colour correspondence median pixels: original="<<original[7]<<" r12="<<r12[7]<<" reversed="<<swapped[7]<<'\n';
    for(const auto& frame:daysgone_motion_samples::frames) {
        double adjacent_error{},same_error{};
        for(const auto& sample:frame.samples) {
            const std::array<double,4> clip{2*(sample[0]+.5)/1792-1,1-2*(sample[1]+.5)/1344,sample[2],1};
            const auto error=[&](const auto& matrix){
                std::array<double,4> previous{};
                for(unsigned col=0;col<4;++col)for(unsigned row=0;row<4;++row)previous[col]+=clip[row]*matrix[row*4+col];
                const auto x=(previous[0]/previous[3]-clip[0])*896-sample[3];
                const auto y=(previous[1]/previous[3]-clip[1])*(-672)-sample[4];return std::hypot(x,y);
            };
            adjacent_error+=error(frame.adjacent);same_error+=error(frame.same);
        }
        adjacent_error/=frame.samples.size();same_error/=frame.samples.size();
        check(adjacent_error<.2,"captured Days Gone original field no longer fits N1 camera");
        check(same_error>10,"N2 control must reject the captured Days Gone field");
        std::cout<<"Captured eye="<<frame.eye<<" N1 mean error="<<adjacent_error<<"px N2="<<same_error<<"px\n";
    }
    xrfg::DlssMotionVectorFrame sample{};sample.motion_width=sample.depth_width=1792;
    sample.motion_height=sample.depth_height=1344;sample.placed=true;
    sample.placed_motion_rect={55.0562F,0,1681.8876F,1344};sample.jitter_x=.1875F;sample.jitter_y=-.351852F;
    const auto legacy_motion=xrfg::guide_motion_sample_rect(sample),legacy_depth=xrfg::guide_depth_sample_rect(sample);
    check(legacy_motion==xrfg::guide_motion_rect(sample)&&legacy_depth==xrfg::guide_depth_rect(sample),"legacy guide lookup geometry changed");
    sample.raw_motion_color_aligned=true;
    const auto motion=xrfg::guide_motion_sample_rect(sample),depth=xrfg::guide_depth_sample_rect(sample);
    check(std::abs(motion[0]-55.2437F)<1e-5F&&std::abs(motion[1]+.351852F)<1e-6F,"colour-to-motion raster lookup omitted native jitter");
    check(motion==depth,"matching native depth and motion grids must share their lookup phase");
    check(motion[2]==1681.8876F&&motion[3]==1344,"raster phase must not resize guide coverage");
    sample.jittered=true;
    check(xrfg::guide_motion_sample_rect(sample)==legacy_motion,"legacy jittered-vector convention changed");
    std::cout<<checks<<" exact Days Gone camera/motion checks PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
