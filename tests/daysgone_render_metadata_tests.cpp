#include "xrfg/native_ingress/DaysGoneRenderBinding.h"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
namespace binding=xrfg::native_ingress::daysgone_render_binding;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Memory {
    std::map<std::uintptr_t,unsigned char> bytes;
    template<class T>void put(std::uintptr_t at,const T& value){
        const auto* p=reinterpret_cast<const unsigned char*>(&value);
        for(unsigned i=0;i<sizeof(value);++i)bytes[at+i]=p[i];
    }
    bool read(std::uintptr_t at,void* out,std::size_t n) const {
        auto* p=static_cast<unsigned char*>(out);
        for(std::size_t i=0;i<n;++i){auto it=bytes.find(at+i);if(it==bytes.end())return false;p[i]=it->second;}return true;
    }
};
constexpr std::uintptr_t base=0x180000000,owner=0x200000000,config=0x300000000,renderer=0x400000000,compositor=0x500000000;
const binding::Layout layout{0x100,0x200,0x300,0x400,0x500,0x1000,0x130,0x5c,0x68,0x1500,0xf4,0x1600,0x15b0};
const std::array<float,3> origin{123.25f,-456.5f,789};
void record(Memory& m,unsigned slot,std::uint64_t render,const std::array<float,3>& xyz,unsigned char used=0){
    const auto at=owner+layout.ring+slot*layout.stride;
    m.put(at,render);m.put(at+layout.origin,xyz);m.put(at+layout.used,used);
}
Memory memory(){
    Memory m;m.put(base+layout.owner,std::uint64_t{owner});m.put(owner,std::uint64_t{base+layout.vtable});
    m.put(base+layout.config,std::uint64_t{config});m.put(config+layout.adjustment,std::int32_t{});
    m.put(owner+layout.selected,UINT64_MAX);
    m.put(owner+layout.renderer,std::uint64_t{renderer});m.put(renderer+layout.compositor_owner,std::uint64_t{compositor});
    for(unsigned i=0;i<4;++i)record(m,i,UINT64_MAX,{});
    record(m,1,101,origin);return m;
}
void identity_tests(){
    auto m=memory();unsigned reason{};
    const auto query=[&]{return binding::read_identity(base,layout,origin,[&](auto a,auto p,auto n){return m.read(a,p,n);},&reason);};
    auto id=query();check(id&&id->object==compositor&&id->object!=owner&&id->render==101,"native record not qualified with eye-release queue owner (r8 regression)");
    // An unrelated later compositor selection cannot override the exact game camera.
    m.put(owner+layout.selected,std::uint64_t{98});check(query()==id,"stale selected render replaced camera match");
    auto before=query();check(xrfg::stable_real_vr_render_identity(before,query())==id,"stable DLSS bracket rejected");
    record(m,1,105,origin);check(!xrfg::stable_real_vr_render_identity(before,query()),"overwritten ring accepted across DLSS");
    m=memory();record(m,2,102,origin);check(!query()&&reason==5,"duplicate origins guessed newest render");
    m=memory();record(m,1,101,{1,2,3});check(!query()&&reason==4,"foreign camera origin accepted");
    m=memory();record(m,1,101,origin,1);check(!query(),"consumed camera reused without explicit selection");
    m.put(owner+layout.selected,std::uint64_t{101});check(query()==id,"explicit consumed selected camera refused");
    m=memory();m.put(config+layout.adjustment,std::int32_t{2});check(query()->render==103,"native render adjustment omitted");
    m.put(config+layout.adjustment,std::int32_t{-2});check(query()->render==99,"negative native adjustment omitted");
    record(m,1,1,origin);check(!query(),"native render adjustment underflow");
    m=memory();record(m,1,std::uint64_t(INT64_MAX)-2,origin);m.put(config+layout.adjustment,std::int32_t{3});check(!query(),"native render adjustment overflow");
    m=memory();record(m,1,102,origin);check(!query(),"wrong ring slot accepted");
    m=memory();m.put(owner,std::uint64_t{base+layout.vtable+8});check(!query()&&reason==2,"foreign active runtime accepted");
    m=memory();m.put(base+layout.owner,std::uint64_t{});check(!query(),"missing owner accepted");
    m=memory();m.put(base+layout.config,std::uint64_t{});check(!query()&&reason==3,"missing config accepted");
    m=memory();m.put(owner+layout.renderer,std::uint64_t{});check(!query()&&reason==7,"missing XR renderer accepted");
    m=memory();m.put(renderer+layout.compositor_owner,std::uint64_t{});check(!query()&&reason==7,"missing eye-release queue owner accepted");
    m=memory();m.bytes.erase(owner+layout.ring);check(!query()&&reason==3,"unreadable ring accepted");
    m=memory();auto nan=origin;nan[0]=std::numeric_limits<float>::quiet_NaN();
    check(!binding::read_identity(base,layout,nan,[&](auto a,auto p,auto n){return m.read(a,p,n);}),"nonfinite camera accepted");
    m=memory();unsigned reads{};
    const auto raced=binding::read_identity(base,layout,origin,[&](auto a,auto p,auto n){
        if(a==owner+layout.ring+layout.stride&&++reads==2)record(m,1,105,origin);
        return m.read(a,p,n);
    },&reason);check(!raced&&reason==6,"atomic producer replacement during read accepted");
    m=memory();reads=0;
    check(!binding::read_identity(base,layout,origin,[&](auto a,auto p,auto n){
        if(a==renderer+layout.compositor_owner&&++reads==2)m.put(a,std::uint64_t{compositor+8});
        return m.read(a,p,n);
    },&reason)&&reason==6,"compositor owner replaced during read accepted");
}
void actual_image(const char* path){
    std::ifstream file(path,std::ios::binary);std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),{}};
    const auto get=[&]<class T>(std::size_t at){check(at<=bytes.size()&&sizeof(T)<=bytes.size()-at,"raw PE bounds");T v{};std::memcpy(&v,bytes.data()+at,sizeof(v));return v;};
    const auto nt=get.operator()<std::uint32_t>(0x3c);
    check(get.operator()<std::uint32_t>(nt)==0x4550,"actual PE missing");
    const auto image_base=get.operator()<std::uint64_t>(nt+48);
    const auto size=get.operator()<std::uint32_t>(nt+24+56),headers=get.operator()<std::uint32_t>(nt+24+60);
    struct Section{std::uint32_t va,raw,count;};std::vector<Section> sections;
    const auto table=nt+24+get.operator()<std::uint16_t>(nt+20);
    for(unsigned i=0;i<get.operator()<std::uint16_t>(nt+6);++i){const auto at=table+i*40;
        sections.push_back({get.operator()<std::uint32_t>(at+12),get.operator()<std::uint32_t>(at+20),get.operator()<std::uint32_t>(at+16)});
    }
    const auto read=[&](std::uintptr_t at,void* dst,std::size_t count){
        if(at<image_base||at-image_base>size||count>size-(at-image_base))return false;
        const auto rva=at-image_base;auto* out=static_cast<unsigned char*>(dst);std::memset(out,0,count);
        if(rva<headers){const auto n=std::min(count,std::size_t(headers-rva));if(n>bytes.size()-rva)return false;std::memcpy(out,bytes.data()+rva,n);return true;}
        for(const auto& s:sections){if(rva<s.va||rva-s.va>=s.count)continue;
            const auto n=std::min(count,std::size_t(s.count-(rva-s.va))),offset=s.raw+rva-s.va;
            if(offset>bytes.size()||n>bytes.size()-offset)return false;std::memcpy(out,bytes.data()+offset,n);return true;
        }return true;
    };
    const auto l=binding::discover(image_base,read);
    check(l.has_value(),"installed REALVR DG producer discovery failed");
    std::cout<<"DG_NATIVE_LAYOUT_PASS matcher="<<std::hex<<l->matcher<<" writer="<<l->writer<<" owner="<<l->owner
        <<" ring="<<l->ring<<" origin="<<l->origin<<" selected="<<l->selected<<" config="<<l->config
        <<" renderer="<<l->renderer<<" compositor_owner="<<l->compositor_owner<<'\n';
    Memory live;live.put(image_base+l->owner,std::uint64_t{owner});live.put(owner,std::uint64_t{image_base+l->vtable});
    live.put(image_base+l->config,std::uint64_t{config});live.put(config+l->adjustment,std::int32_t{1});
    live.put(owner+l->selected,UINT64_MAX);
    live.put(owner+l->renderer,std::uint64_t{renderer});live.put(renderer+l->compositor_owner,std::uint64_t{compositor});
    for(unsigned i=0;i<4;++i){const auto at=owner+l->ring+i*l->stride;live.put(at,UINT64_MAX);
        live.put(at+l->origin,std::array<float,3>{});live.put(at+l->used,static_cast<unsigned char>(1));}
    const auto at=owner+l->ring+2*l->stride;live.put(at,std::uint64_t{102});live.put(at+l->origin,origin);
    live.put(at+l->used,static_cast<unsigned char>(0));
    const auto identity=binding::read_identity(image_base,*l,origin,[&](auto a,auto p,auto n){return live.read(a,p,n);});
    check(identity&&identity->render==103&&identity->object==compositor,"installed layout failed before native selection or used runtime owner");
    const auto eye=xrfg::native_ingress::real_vr_eye_layout::discover(image_base,read);
    check(eye.has_value(),"pre-hook eye layout unavailable");
    const auto eye_section=std::find_if(sections.begin(),sections.end(),[&](const auto& s){return eye->draw_eye>=s.va&&eye->draw_eye-s.va<s.count;});
    check(eye_section!=sections.end(),"eye draw outside initialized sections");
    const auto draw_at=eye_section->raw+eye->draw_eye-eye_section->va;
    // Reproduce live ordering: the release observer is installed before NGX
    // asks for the producer binding. Its detour replaces the draw prologue.
    bytes.at(draw_at)=0xe9;
    check(!binding::discover(image_base,read),"r9 live-hook failure control did not reproduce");
    const auto hooked=binding::discover(image_base,read,&*eye);
    check(hooked&&hooked->owner==l->owner&&hooked->renderer==l->renderer&&hooked->matcher==l->matcher,
        "producer cannot bind after eye observer installation (r9 regression)");
    auto foreign_eye=*eye;++foreign_eye.swapchain_vtable;
    check(!binding::discover(image_base,read,&foreign_eye),"foreign cached module layout accepted");
    // Mutating the exact matcher code must invalidate this binding, independent of timestamp.
    auto section=std::find_if(sections.begin(),sections.end(),[&](const auto& s){return l->matcher>=s.va&&l->matcher-s.va<s.count;});
    check(section!=sections.end(),"matcher outside sections");
    // Remove the exact 8+4-byte native-origin comparison in this function.
    bool changed{};
    for(std::size_t i=section->raw+l->matcher-section->va;i+7<bytes.size()&&i<section->raw+l->matcher-section->va+512;++i)
        if(xrfg::native_ingress::real_vr_runtime_image::match(bytes,i,{0x49,0x3b,0x80})){bytes[i]^=1;changed=true;break;}
    check(changed,"native origin comparison witness missing");check(!binding::discover(image_base,read,&*eye),"changed native matcher accepted with cached eye layout");
}
}
int main(int argc,char** argv){try{identity_tests();if(argc==2)actual_image(argv[1]);
    std::cout<<"Days Gone render metadata: "<<std::dec<<checks<<" checks passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
