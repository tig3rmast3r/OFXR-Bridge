#include "xrfg/native_ingress/GtaRuntimeBinding.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace binding = xrfg::native_ingress::gta_runtime_binding;
using xrfg::native_ingress::real_vr_runtime_image::value;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Image {
    std::uintptr_t base{0x10000000};
    std::vector<unsigned char> bytes;
    template<class T> void put(std::size_t at, T v) {
        require(at <= bytes.size() && sizeof(v) <= bytes.size()-at, "fixture write bounds");
        std::memcpy(bytes.data()+at, &v, sizeof(v));
    }
    void code(std::size_t at, std::initializer_list<unsigned char> v) {
        require(at <= bytes.size() && v.size() <= bytes.size()-at, "fixture code bounds");
        std::copy(v.begin(), v.end(), bytes.begin()+at);
    }
    auto discover(unsigned* refusal = nullptr) const {
        return binding::discover(base, [&](std::uintptr_t address, void* to, std::size_t size) {
            if (address < base || address-base > bytes.size() || size > bytes.size()-(address-base)) return false;
            std::memcpy(to, bytes.data()+address-base, size); return true;
        }, refusal);
    }
};
struct Fixture {
    Image image; binding::Layout layout;
    std::uint32_t type{},owner_check{},wrapper{},ctor{},handler{},state{},yaw{},cached{};
};
Fixture fixture(std::uint32_t shift=0) {
    Fixture f;auto& m=f.image;m.bytes.resize(0xb000);
    m.code(0,{'M','Z'});m.put<std::uint32_t>(0x3c,0x100);m.put<std::uint32_t>(0x100,0x4550);
    m.put<std::uint16_t>(0x104,0x8664);m.put<std::uint16_t>(0x106,4);m.put<std::uint16_t>(0x114,240);
    m.put<std::uint16_t>(0x118,0x20b);m.put<std::uint32_t>(0x150,0xb000);
    m.put<std::uint32_t>(0x184,16);m.put<std::uint32_t>(0x1a0,0xa000);m.put<std::uint32_t>(0x1a4,6*12);
    const auto section=[&](unsigned n,unsigned rva,unsigned size,unsigned flags){
        const auto at=0x208+n*40;m.put(at+8,size);m.put(at+12,rva);m.put(at+16,size);m.put(at+36,flags);
    };
    section(0,0x1000,0x3000,0x60000020);section(1,0x5000,0x3000,0x40000040);
    section(2,0x8000,0x2000,0xc0000040);section(3,0xa000,0x1000,0x40000040);
    f.layout={0x9c00+shift,0x5208+shift,0x11000+shift,0x12000+shift,0x12800+shift};
    const auto& l=f.layout;f.type=0x8800+shift;f.cached=0x14000+shift;
    f.owner_check=0x1100+shift;f.wrapper=0x1300+shift;f.ctor=0x1600+shift;
    f.handler=0x1900+shift;f.state=0x2000+shift;f.yaw=0x2500+shift;
    constexpr char name[]=".?AVruntime_GTAVE@@";std::memcpy(m.bytes.data()+f.type+16,name,sizeof(name));
    const auto col=0x5100+shift;m.put<std::uint32_t>(col,1);m.put(col+12,f.type);m.put(col+20,col);
    m.put<std::uint64_t>(l.runtime_vtable-8,m.base+col);
    for(unsigned n=0;n<3;++n)m.put<std::uint64_t>(l.runtime_vtable+n*8,m.base+f.handler);
    unsigned n{};for(auto start:{f.owner_check,f.wrapper,f.ctor,f.handler,f.state,f.yaw}){
        m.put(0xa000+n*12,start);m.put(0xa004+n*12,start+0x180);++n;
    }
    m.code(f.owner_check,{0x48,0x8b,0x3d,0,0,0,0,0x4c,0x8b,0xf2,0x48,0x8b,0xe9,
        0x48,0x85,0xff,0x0f,0x84,0,0,0,0,0x48,0x8b,0x07,0x48,0x8d,0x0d,0,0,0,0,
        0x48,0x8b,0x90,0xe8,0,0,0,0x48,0x3b,0xc1});
    m.put<std::int32_t>(f.owner_check+3,l.runtime_pointer-f.owner_check-7);
    m.put<std::int32_t>(f.owner_check+28,l.runtime_vtable-f.owner_check-32);
    m.code(f.wrapper,{0x48,0x8b,0xd1,0x48,0x8b,0x0d,0,0,0,0,0x48,0x85,0xc9,0x0f,0x85,0,0,0,0,0xc3});
    m.put<std::int32_t>(f.wrapper+6,l.runtime_pointer-f.wrapper-10);
    m.put<std::int32_t>(f.wrapper+15,f.handler-f.wrapper-19);
    m.code(f.ctor,{0x48,0x8d,0x05,0,0,0,0,0x48,0x89,0x06});
    m.put<std::int32_t>(f.ctor+3,l.runtime_vtable-f.ctor-7);
    m.code(f.ctor+0x20,{0x33,0xc0,0x48,0x89,0x86});m.put(f.ctor+0x25,f.cached);
    m.code(f.handler,{0xe8,0,0,0,0});m.put<std::int32_t>(f.handler+1,f.state-f.handler-5);
    m.code(f.handler+0x20,{0xe8,0,0,0,0});m.put<std::int32_t>(f.handler+0x21,f.yaw-f.handler-0x25);
    m.code(f.state,{0x48,0x8b,0x81,0,0,0,0,0x48,0x8b,0xf9});m.put(f.state+3,l.render_offset);
    m.code(f.state+0x40,{0x8b,0x87,0,0,0,0,0x89,0x87,0,0,0,0});
    m.put(f.state+0x42,l.mode_offset);m.put(f.state+0x48,f.cached);
    m.code(f.yaw,{0x48,0x8b,0xd9});m.code(f.yaw+0x20,{0x8b,0xb3});m.put(f.yaw+0x22,l.mode_offset);
    m.code(f.yaw+0x30,{0x8b,0x83});m.put(f.yaw+0x32,f.cached);
    m.code(f.yaw+0x60,{0xf3,0x0f,0x5c,0xb3,0,0,0,0,0x88,0x83,0,0,0,0});
    m.put(f.yaw+0x64,l.yaw_offset);m.put(f.yaw+0x6a,l.yaw_offset+17);
    return f;
}
void tests() {
    for(auto shift:{0U,0x80U,0x180U}) {
        auto f=fixture(shift);unsigned refusal{};
        require(f.image.discover(&refusal)==f.layout,"relocated code/RTTI/owner and independent fields");
        f.image.put<std::uint32_t>(0x108,0xdeadbeef);require(f.image.discover()==f.layout,"unknown timestamp");
        f.image.put<std::uint32_t>(0x208+2*40+16,0x1000);require(f.image.discover()==f.layout,"owner in unread BSS");
        const auto original=f.image;
        f.image.bytes[f.type+16]='!';require(!f.image.discover(),"alien RTTI");f.image=original;
        f.image.put<std::int32_t>(f.owner_check+28,0);require(!f.image.discover(),"unlinked class witness");f.image=original;
        f.image.put<std::int32_t>(f.wrapper+6,0);require(!f.image.discover(),"wrapper owner mismatch");f.image=original;
        f.image.put<std::int32_t>(f.handler+1,0);require(!f.image.discover(),"missing state callback");f.image=original;
        f.image.put(f.ctor+0x25,f.cached+4);require(!f.image.discover(),"unlinked GTA cached field");f.image=original;
        f.image.put(f.yaw+0x22,f.layout.mode_offset+4);require(!f.image.discover(),"mode disagreement");f.image=original;
        f.image.put(f.yaw+0x32,f.cached+4);require(!f.image.discover(),"cached mode disagreement");f.image=original;
        f.image.bytes[f.yaw+2]=0xf9;require(!f.image.discover(),"wrong this register");f.image=original;
        f.image.put(f.state+3,0xffffffffU);require(!f.image.discover(),"unbounded object field");f.image=original;
        const auto duplicate=f.wrapper+0x80;
        std::copy_n(f.image.bytes.begin()+f.wrapper,20,f.image.bytes.begin()+duplicate);
        f.image.put<std::int32_t>(duplicate+6,f.layout.runtime_pointer-duplicate-10);
        f.image.put<std::int32_t>(duplicate+15,f.handler-duplicate-19);
        require(!f.image.discover(&refusal)&&refusal==5,"ambiguous camera wrappers");f.image=original;
        // Register allocation changes remain valid when all linked uses agree.
        f.image.bytes[f.owner_check+2]=0x1d;f.image.bytes[f.owner_check+15]=0xdb;f.image.bytes[f.owner_check+24]=3;
        f.image.bytes[f.state+9]=0xd9;f.image.bytes[f.state+0x41]=0x83;f.image.bytes[f.state+0x47]=0x83;
        require(f.image.discover()==f.layout,"consistent owner registers may change");
    }
    auto f=fixture();f.image.put<std::uint32_t>(0x150,4096);require(!f.image.discover(),"invalid image bounds");
    std::cout<<"GTA structural runtime discovery synthetic fixtures PASS\n";
}
Image map_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(bool(stream), "open supplied DLL");
    std::vector<unsigned char> raw{std::istreambuf_iterator<char>(stream), {}};
    const auto nt = value<std::uint32_t>(raw, 0x3c);
    require(raw.size() >= 4096 && nt <= raw.size()-264 && value<std::uint32_t>(raw, nt) == 0x4550, "DLL PE header");
    const auto size = value<std::uint32_t>(raw, nt+24+56);
    require(size >= 4096 && size <= 512*1024*1024, "DLL image size");
    Image m; m.base = value<std::uint64_t>(raw, nt+24+24); m.bytes.resize(size);
    const auto headers = value<std::uint32_t>(raw, nt+24+60);
    require(headers <= raw.size() && headers <= size, "DLL headers size");
    std::copy_n(raw.begin(), headers, m.bytes.begin());
    const auto table = nt+24+value<std::uint16_t>(raw, nt+20);
    const auto count = value<std::uint16_t>(raw, nt+6);
    require(count <= 32 && table <= raw.size() && count*40 <= raw.size()-table, "DLL sections");
    for (unsigned n = 0; n < count; ++n) {
        const auto at = table+n*40, rva = value<std::uint32_t>(raw, at+12);
        const auto bytes = value<std::uint32_t>(raw, at+16), offset = value<std::uint32_t>(raw, at+20);
        require(offset <= raw.size() && bytes <= raw.size()-offset && rva <= size && bytes <= size-rva, "DLL section bounds");
        std::copy_n(raw.begin()+offset, bytes, m.bytes.begin()+rva);
    }
    return m;
}
int main(int argc,char** argv) {
    try {
        tests();
        for(int n=1;n<argc;++n) {
            auto m=map_file(argv[n]);unsigned refusal{};
            const auto start=std::chrono::steady_clock::now();const auto l=m.discover(&refusal);
            if(!l){std::cerr<<argv[n]<<" refusal="<<refusal<<'\n';return 1;}
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            std::cout<<argv[n]<<" "<<ms<<"ms"<<std::hex<<" owner="<<l->runtime_pointer<<" vtable="<<l->runtime_vtable
                <<" render="<<l->render_offset<<" yaw="<<l->yaw_offset<<" mode="<<l->mode_offset<<std::dec<<'\n';
            const auto nt=value<std::uint32_t>(m.bytes,0x3c);m.put<std::uint32_t>(nt+8,0xfedcba98);
            require(m.discover()==l,"actual DLL altered timestamp");
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
