#include <sl.h>
#include "xrfg/native_ingress/streamline1/sl1.h"
#include "xrfg/native_ingress/streamline_camera.hpp"
#include "xrfg/native_ingress/DeadSpaceCameraSource.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <string>
#include <new>
#include <limits>
#include <cmath>
#include <algorithm>
namespace camera=xrfg::native_ingress::streamline_camera;
namespace xrfg::native_ingress {void poll_streamline() noexcept;}
namespace {
int failures{},calls{};bool supported{},nested{},inside{};uint32_t expected_frame=0x43ff,expected_viewport=5;
const void* expected_constants{};bool expect_camera=true;void(*run_nested)();
uint32_t expected_rejection{};
bool projection_case{},luke_receipt{};
bool native_source_case{},expect_ambiguous{};
float rejected_fov=std::numeric_limits<float>::max();
void check(bool ok,const char* why){if(!ok){++failures;std::cerr<<"FAIL: "<<why<<'\n';}}
void observe(void* list,uint32_t feature,uint32_t frame,uint32_t viewport,const void* values){
    ++calls;check(list==nullptr&&feature==0,"original evaluate arguments preserved");
    check(frame==expected_frame&&viewport==expected_viewport,"original frame and viewport preserved");
    check(values==expected_constants,"original Constants address preserved");
    auto c=camera::current(luke_receipt);check(c.valid==(supported&&expect_camera),"exact camera admission");
    if(supported&&expect_ambiguous)check(c.ambiguous&&!c.valid&&(c.rejection&1024),"conflicting or unidentifiable chained viewports stay rejected");
    if(supported&&expected_rejection)check(c.observed&&!c.valid&&c.frame==frame&&c.viewport==viewport&&
        c.rejection==expected_rejection&&c.fov==rejected_fov&&c.near_z==.1f&&c.far_z==100,
        "exact rejected camera preserves field mask and original scalar bits");
    if(c.valid){check(c.frame==frame&&c.viewport==viewport,"camera belongs to exact evaluate");
        check(c.position[0]==7&&c.projection[0]==1&&(projection_case||native_source_case||c.inverse[15]==1)&&c.camera_motion==1&&c.inverted,"typed camera fields preserved");
        if(native_source_case)check(c.forward==std::array<float,3>{0,0,-1}&&c.right==std::array<float,3>{1,0,0},"native source camera did not survive exact SL evaluate scope");
        if(projection_case){check(std::abs(c.fov-2.F*std::atan(.5F))<1e-6F,"FOV was not derived from exact perspective");
            const auto raw=camera::current();check(raw.observed&&!raw.valid&&raw.rejection==64&&raw.fov==99,
                "Luke correction changed the original non-Luke receipt");}}
    if(nested&&!inside){inside=true;auto saved=c;run_nested();inside=false;
        auto restored=camera::current();check(restored.valid==saved.valid&&restored.frame==saved.frame&&restored.viewport==saved.viewport,"nested evaluate restores outer camera");}
}
template<class Constants> void initialize(Constants& v){
    for(auto* matrix:{&v.cameraViewToClip,&v.clipToCameraView,&v.clipToPrevClip,&v.prevClipToClip}){
        std::memset(matrix,0,sizeof(*matrix));auto* p=reinterpret_cast<float*>(matrix);p[0]=p[5]=p[10]=p[15]=1;
    }
    v.cameraPos={7,8,9};v.cameraUp={0,1,0};v.cameraRight={1,0,0};v.cameraFwd={0,0,1};
    v.cameraNear=.1f;v.cameraFar=100;v.cameraFOV=1;
    v.depthInverted=static_cast<decltype(v.depthInverted)>(1);v.cameraMotionIncluded=static_cast<decltype(v.cameraMotionIncluded)>(1);
}
template<class Constants,class Invoke>void projection_cases(Constants& v,Invoke invoke) {
    check(calls==5,"all original evaluations reached");
    initialize(v);projection_case=luke_receipt=true;expect_camera=true;expected_rejection=0;
    auto* p=reinterpret_cast<float*>(&v.cameraViewToClip);auto* inverse=reinterpret_cast<float*>(&v.clipToCameraView);
    std::fill_n(p,16,0.F);std::fill_n(inverse,16,0.F);
    p[0]=1;p[5]=2;p[11]=.06F;p[14]=-1;
    inverse[0]=1;inverse[5]=.5F;inverse[11]=-1;inverse[14]=1.F/.06F;
    v.cameraFOV=99;rejected_fov=99;
    ++expected_frame;invoke();check(v.cameraFOV==99,"FOV correction mutated caller Constants");
    expect_camera=false;expected_rejection=64;inverse[0]=2;++expected_frame;invoke();
    inverse[0]=1;inverse[15]=std::numeric_limits<float>::quiet_NaN();expected_rejection=66;++expected_frame;invoke();
    initialize(v);v.cameraFOV=99;expected_rejection=64;++expected_frame;invoke(); // orthographic/identity
    check(!camera::current(true).valid,"Luke receipt escaped exact evaluate scope");
}
template<class Constants,class Set,class Evaluate>void native_source_cases(Constants& v,Set set,Evaluate evaluate) {
    native_source_case=true;projection_case=luke_receipt=false;rejected_fov=1;
    for(unsigned kind=0;kind<4;++kind) {
        initialize(v);v.cameraFwd={-1,0,0};
        auto* p=reinterpret_cast<float*>(&v.cameraViewToClip);auto* inverse=reinterpret_cast<float*>(&v.clipToCameraView);
        std::fill_n(p,16,0.F);std::fill_n(inverse,16,0.F);
        p[0]=1;p[5]=2;p[11]=-1;p[14]=.1F;
        inverse[0]=1;inverse[5]=.5F;inverse[11]=10;inverse[14]=-1;
        std::array<unsigned char,dead_space_camera_source::packet_bytes> source{};
        const auto copy=[&](std::size_t offset,const void* value,std::size_t bytes){std::memcpy(source.data()+offset,value,bytes);};
        copy(0x70,&v.cameraPos,12);copy(0x7c,&v.cameraUp,12);copy(0x94,&v.cameraRight,12);
        const float back[]{0,0,1};copy(0x88,back,12);copy(0xa0,p,64);copy(0xe0,inverse,64);
        copy(0x120,&v.clipToPrevClip,64);copy(0x160,&v.prevClipToClip,64);
        if(kind==2)source[0xa0]^=1;
        if(kind==3)copy(0x88,&v.cameraRight,12);
        ++expected_frame;expect_camera=kind==0;expected_rejection=kind==0?0:8;
        {dead_space_camera_source::Scope native(source.data(),expected_frame+(kind==1?1:0));set();}
        check(!dead_space_camera_source::source.active,"native source leaked beyond original builder");
        evaluate();check(v.cameraFwd.x==-1&&v.cameraFwd.y==0&&v.cameraFwd.z==0,"caller SDK basis was modified");
    }
    native_source_case=false;
}
using Set1=decltype(&sl1::slSetConstants);using Eval1=decltype(&sl1::slEvaluateFeature);
Set1 set1;Eval1 eval1;PFun_slSetConstants* set2;PFun_slEvaluateFeature* eval2;PFun_slGetNewFrameToken* get_token;
void inner1(){auto f=expected_frame,v=expected_viewport;auto wanted=expect_camera;expected_frame=0x1234;expected_viewport=8;expect_camera=false;
    eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport);expected_frame=f;expected_viewport=v;expect_camera=wanted;}
void inner2(){auto f=expected_frame,v=expected_viewport;auto wanted=expect_camera;expected_frame=0x1234;expected_viewport=8;expect_camera=false;
    sl::FrameToken* token{};get_token(token,&expected_frame);sl::ViewportHandle viewport(expected_viewport);const sl::BaseStructure* inputs[]={&viewport};
    eval2(sl::kFeatureDLSS,*token,inputs,1,nullptr);expected_frame=f;expected_viewport=v;expect_camera=wanted;}
}
int main(int argc,char** argv){
    if(argc!=4)return 2;const bool legacy=std::string(argv[2])=="1";supported=std::string(argv[3])=="yes";
    const auto module=LoadLibraryA(argv[1]);if(!module)return 3;
    auto set_observer=reinterpret_cast<void(*)(decltype(&observe))>(GetProcAddress(module,"TestObserver"));
    auto outcome=reinterpret_cast<void(*)(int)>(GetProcAddress(module,"TestOutcome"));set_observer(observe);
    xrfg::native_ingress::poll_streamline();xrfg::native_ingress::poll_streamline();
    check(camera::hooked()==supported,"version/export gate and repeat installation");
    // Guarded SDK-sized Constants catches reading a legacy record as modern layout.
    auto* page=static_cast<char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    DWORD previous{};VirtualProtect(page+4096,4096,PAGE_NOACCESS,&previous);
    if(legacy){
        auto* v=new(page+4096-sizeof(sl1::Constants))sl1::Constants;initialize(*v);expected_constants=v;
        set1=reinterpret_cast<Set1>(GetProcAddress(module,"slSetConstants"));eval1=reinterpret_cast<Eval1>(GetProcAddress(module,"slEvaluateFeature"));run_nested=inner1;
        check(set1(*v,expected_frame,expected_viewport),"legacy successful result preserved");nested=true;
        check(eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport),"legacy evaluate result preserved");nested=false;
        check(!camera::current().valid,"camera cleared after evaluate");
        outcome(0);expected_frame=0x4400;expect_camera=false;
        check(!set1(*v,expected_frame,expected_viewport),"legacy failure preserved");
        check(!eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport),"legacy evaluate failure preserved");
        outcome(1);expected_frame=0x43ff;expected_viewport=6;
        eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport);
        expected_frame=0x4455;expected_viewport=5;expected_rejection=64;v->cameraFOV=std::numeric_limits<float>::max();
        check(set1(*v,expected_frame,expected_viewport),"rejected legacy constants original result preserved");
        check(eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport),"rejected legacy evaluate original result preserved");
        projection_cases(*v,[&]{check(set1(*v,expected_frame,expected_viewport),"projection legacy original Set result");
            check(eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport),"projection legacy original Evaluate result");});
        if(supported){expected_viewport=0;native_source_cases(*v,[&]{check(set1(*v,expected_frame,expected_viewport),"native source original Set result");},
            [&]{check(eval1(nullptr,sl1::eFeatureDLSS,expected_frame,expected_viewport),"native source original Evaluate result");});}
    }else{
        auto* v=new(page+4096-sizeof(sl::Constants))sl::Constants;initialize(*v);expected_constants=v;
        set2=reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(module,"slSetConstants"));eval2=reinterpret_cast<PFun_slEvaluateFeature*>(GetProcAddress(module,"slEvaluateFeature"));
        get_token=reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(module,"slGetNewFrameToken"));run_nested=inner2;
        sl::FrameToken* token{};get_token(token,&expected_frame);sl::ViewportHandle viewport(expected_viewport);const sl::BaseStructure* inputs[]={&viewport};
        check(set2(*v,*token,viewport)==sl::Result::eOk,"modern successful result preserved");nested=true;
        check(eval2(sl::kFeatureDLSS,*token,inputs,1,nullptr)==sl::Result::eOk,"modern evaluate result preserved");nested=false;
        check(!camera::current().valid,"modern camera cleared after evaluate");
        outcome(0);expected_frame=0x4400;expect_camera=false;get_token(token,&expected_frame);
        check(set2(*v,*token,viewport)==sl::Result::eErrorInvalidParameter,"modern failure preserved");
        check(eval2(sl::kFeatureDLSS,*token,inputs,1,nullptr)==sl::Result::eErrorInvalidParameter,"modern evaluate failure preserved");
        outcome(1);expected_frame=0x43ff;get_token(token,&expected_frame);
        sl::ViewportHandle other(6);const sl::BaseStructure* conflict[]={&viewport,&other};
        eval2(sl::kFeatureDLSS,*token,conflict,2,nullptr);
        expected_frame=0x4455;expected_rejection=64;get_token(token,&expected_frame);v->cameraFOV=std::numeric_limits<float>::max();
        check(set2(*v,*token,viewport)==sl::Result::eOk,"rejected modern constants original result preserved");
        check(eval2(sl::kFeatureDLSS,*token,inputs,1,nullptr)==sl::Result::eOk,"rejected modern evaluate original result preserved");
        projection_cases(*v,[&]{get_token(token,&expected_frame);
            check(set2(*v,*token,viewport)==sl::Result::eOk,"projection modern original Set result");
            check(eval2(sl::kFeatureDLSS,*token,inputs,1,nullptr)==sl::Result::eOk,"projection modern original Evaluate result");});
        initialize(*v);projection_case=luke_receipt=false;expect_camera=true;expected_rejection=0;
        ++expected_frame;get_token(token,&expected_frame);
        check(set2(*v,*token,viewport)==sl::Result::eOk,"first eye constants for chained viewport");
        check(set2(*v,*token,other)==sl::Result::eOk,"second eye constants for chained viewport");
        sl::Resource wrapper{};wrapper.next=&viewport;const sl::BaseStructure* chained[]={&wrapper};
        check(eval2(sl::kFeatureDLSS,*token,chained,1,nullptr)==sl::Result::eOk,"chained viewport keeps original evaluate result");
        sl::ViewportHandle duplicate(expected_viewport);viewport.next=&duplicate;
        eval2(sl::kFeatureDLSS,*token,chained,1,nullptr);
        expect_camera=false;expect_ambiguous=true;viewport.next=&other;
        eval2(sl::kFeatureDLSS,*token,chained,1,nullptr);
        viewport.next=nullptr;wrapper.next=&wrapper;expected_viewport=UINT32_MAX;
        eval2(sl::kFeatureDLSS,*token,chained,1,nullptr);
        expect_ambiguous=false;
    }
    check(calls==((legacy&&!supported)?9:13),"all original, projection, chained and native source evaluations reached");check(!camera::current().valid,"no camera leaks outside evaluate");
    VirtualFree(page,0,MEM_RELEASE);std::cout<<"Streamline "<<(legacy?1:2)<<" supported="<<supported<<" original_calls="<<calls<<" failures="<<failures<<'\n';return failures?1:0;
}
