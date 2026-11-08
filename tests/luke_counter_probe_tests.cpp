#include "xrfg/luke_counter_probe.hpp"
#include <iostream>
#include <fstream>
#include <iterator>
#include <string_view>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uintptr_t module=0x180000000, object=0x220000000;
struct Memory {
    std::map<std::uintptr_t, std::vector<unsigned char>> blocks;
    std::uintptr_t fail{};
    unsigned render_reads{};
    bool race{},owner_race{},vtable_race{};
    unsigned owner_reads{},vtable_reads{};
    std::uintptr_t render_address{object+0x841870},owner_address{module+xrfg::kLukeRuntimePointer};
    template<class T> void put(std::uintptr_t p, T v) {
        auto& b=blocks[p]; b.resize(sizeof(v)); std::memcpy(b.data(), &v, sizeof(v));
    }
    bool operator()(std::uintptr_t p, void* out, std::size_t n) noexcept {
        if (p == fail) return false;
        auto i=blocks.upper_bound(p);
        if (i==blocks.begin()) return false;
        --i;
        if (p-i->first > i->second.size() || n > i->second.size()-(p-i->first)) return false;
        std::memcpy(out, i->second.data()+(p-i->first), n);
        if (p==render_address && ++render_reads==2 && race) {
            std::uint64_t changed=43; std::memcpy(out, &changed, sizeof(changed));
        }
        if(p==owner_address&&++owner_reads==2&&owner_race){std::uint64_t changed=object+0x100;std::memcpy(out,&changed,8);}
        if(p==object&&++vtable_reads==2&&vtable_race){std::uint64_t changed=module+1;std::memcpy(out,&changed,8);}
        return true;
    }
    Memory() {
        auto& h=blocks[module]; h.resize(4096); h[0]='M'; h[1]='Z';
        auto word=[&](unsigned p, std::uint32_t v){std::memcpy(h.data()+p,&v,4);};
        word(0x3c,0x100);word(0x100,0x4550);word(0x108,1789728589);word(0x150,11407360);
        blocks[module+0x2b0a80]={0xf3,0x0f,0x10,0x89,0xb0,0x1f,0x84,0x00,0x0f,0x57,0xc0,0x0f,0x2f,0xc8,0x77,
            0x0f,0x48,0x8b,0x81,0x38,0x03,0x00,0x00,0xf3,0x0f,0x10,0x88,0xa4,0x03,0x00};
        blocks[module+0x2aca91]={0x48,0x8b,0xc8,0x83,0xe1,0x07,0x49,0x8b,0x9c,0xce,0xc0,0x0f,0x84,0x00};
        put(module+xrfg::kLukeRuntimePointer, std::uint64_t{object});
        put(object, std::uint64_t{module+xrfg::kLukeCrimsonVtable});
        put(object+0x841870,std::uint64_t{42});
    }
};
Memory november(bool hfw) {
    Memory m;auto& header=m.blocks[module];
    const auto word=[&](unsigned at,std::uint32_t value){std::memcpy(header.data()+at,&value,4);};
    word(0x108,1793909090);word(0x150,110219264);
    m.blocks[module+0x2ccf70]={0xf3,0x0f,0x10,0x89,0xc4,0x1e,0x84,0,0x0f,0x57,0xc0,0x0f,0x2f,0xc8,0x77,
        0x0f,0x48,0x8b,0x81,0x38,3,0,0,0xf3,0x0f,0x10,0x88,0xa4,3,0};
    m.blocks[module+0x2c8f11]=m.blocks[module+0x2aca91];
    m.blocks[module+0xbcbe7]={0x4c,0x89,0x35,0x12,0x99,0x6f,0};
    m.blocks[module+0xb3d4b]={0x48,0x8d,5,0xee,0x5a,0x4f,0,0x48,0x89,6};
    m.blocks[module+0xb001b]={0x48,0x8d,5,0xa6,0x85,0x4f,0,0x48,0x89,6};
    m.blocks[module+0x211e8b]={0x48,0x89,0x83,0x80,0x17,0x84,0,0xc6,0x84,0x19,0x20,0x13,0x84,0,1,0x48};
    m.owner_address=module+0x7b6500;m.render_address=object+0x841780;
    m.put(m.owner_address,std::uint64_t{object});
    m.put(object,std::uint64_t{module+(hfw?0x5a9840:0x5a85c8)});
    m.put(m.render_address,std::uint64_t{42});
    m.put(object+0x841870,std::uint64_t{777}); // old field is deliberately wrong
    m.put(module+0x5a9840+32*8,std::uint64_t{module+0x20f480});
    m.put(module+0x5a9840+30*8,std::uint64_t{module+0x20f420}); // new slot30 is FOV, not the old slot30
    return m;
}
void check(bool value, const char* why) {if(!value)throw std::runtime_error(why);}
int actual_image(const char* path) {
    std::ifstream file(path,std::ios::binary);std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file),{}};
    check(bytes.size()>4096,"actual DLL file missing");
    const auto get=[&]<class T>(std::size_t at){check(at+sizeof(T)<=bytes.size(),"raw PE field bounds");T v{};std::memcpy(&v,bytes.data()+at,sizeof(v));return v;};
    const auto nt=get.operator()<std::uint32_t>(0x3c);
    check(nt+256<bytes.size()&&get.operator()<std::uint32_t>(nt)==0x4550&&get.operator()<std::uint16_t>(nt+24)==0x20b,"actual PE64");
    const auto image_base=get.operator()<std::uint64_t>(nt+48);
    const auto size=get.operator()<std::uint32_t>(nt+24+56),headers=get.operator()<std::uint32_t>(nt+24+60);
    struct Section{std::uint32_t va,raw,count;};std::vector<Section> sections;
    const auto table=nt+24+get.operator()<std::uint16_t>(nt+20);
    for(unsigned i=0;i<get.operator()<std::uint16_t>(nt+6);++i) {
        const auto at=table+i*40;sections.push_back({get.operator()<std::uint32_t>(at+12),get.operator()<std::uint32_t>(at+20),get.operator()<std::uint32_t>(at+16)});
    }
    const auto read=[&](std::uintptr_t address,void* dst,std::size_t count) noexcept {
        if(address<image_base||address-image_base>size||count>size-(address-image_base))return false;
        auto* out=static_cast<unsigned char*>(dst);std::memset(out,0,count);
        for(std::size_t i=0;i<count;++i){const auto rva=address-image_base+i;
            if(rva<headers&&rva<bytes.size()){out[i]=bytes[rva];continue;}
            for(const auto& section:sections)if(rva>=section.va&&rva-section.va<section.count){
                const auto offset=section.raw+rva-section.va;if(offset>=bytes.size())return false;out[i]=bytes[offset];break;
            }
        }return true;
    };
    const auto layout=xrfg::luke_runtime_binding(image_base,read);check(layout.has_value(),"actual file layout rejected");
    check(xrfg::luke_hfw_layout_matches(image_base,read,*layout),"actual HFW selection/method witnesses rejected");
    std::cout<<"ACTUAL_HFW_LAYOUT_PASS owner="<<std::hex<<layout->runtime_pointer<<" vtable="<<layout->hfw_vtable
        <<" render="<<layout->render_offset<<" slot="<<std::dec<<layout->hfw_method_slot<<'\n';return 0;
}

}
int main(int argc,char** argv) {
    try {
        if(argc==3&&std::string_view(argv[1])=="--actual")return actual_image(argv[2]);
        Memory m;
        check(xrfg::luke_layout_matches(module,m),"supported signatures");
        const auto c=xrfg::read_luke_identity(module,m);
        check(c && c->render==42 && c->object==object,"valid identity");
        check(xrfg::read_luke_identity(module,m)==c,"duplicate identity");
        m=Memory{}; m.race=true;
        check(!xrfg::read_luke_identity(module,m),"race rejected");
        m=Memory{};m.fail=object+0x841870;
        check(!xrfg::read_luke_identity(module,m),"fault rejected");
        m=Memory{};m.put(object,std::uint64_t{module+1});
        check(!xrfg::read_luke_identity(module,m),"other game rejected");
        m=Memory{};m.put(module+xrfg::kLukeRuntimePointer,std::uint64_t{0});
        check(!xrfg::read_luke_identity(module,m),"missing runtime rejected");
        m=Memory{};m.blocks[module][0x108]^=1;
        check(!xrfg::luke_layout_matches(module,m),"wrong build rejected");
        m=Memory{};m.blocks[module+0x2b0a80][0]^=1;
        check(!xrfg::luke_layout_matches(module,m),"wrong HUD signature rejected");
        m=Memory{};m.blocks[module+0x2aca91][0]^=1;
        check(!xrfg::luke_layout_matches(module,m),"wrong mapping signature rejected");
        m=Memory{};std::uint32_t oversized=0xffffffff;std::memcpy(m.blocks[module].data()+0x3c,&oversized,4);
        check(!xrfg::luke_layout_matches(module,m),"invalid PE offset rejected");
        m=Memory{};m.put(object+0x841870,~std::uint64_t{0});
        check(!xrfg::read_luke_identity(module,m),"negative sentinel rejected");
        m=Memory{};
        m.blocks[module+0x20788b]={0x48,0x89,0x83,0x70,0x18,0x84,0x00,0xc6,0x84,0x19,0x10,0x14,0x84,0x00,0x01,0x48};
        m.put(module+xrfg::kLukeHfwVtable+30*8,std::uint64_t{module+0x204e80});
        m.put(object,std::uint64_t{module+xrfg::kLukeHfwVtable});
        check(xrfg::luke_hfw_layout_matches(module,m),"HFW source-selection signature");
        check(!xrfg::read_luke_identity(module,m),"HFW requires explicit layout authorization");
        check(xrfg::read_luke_identity(module,m,true)==c,"HFW selected source identity");
        m.blocks[module+0x20788b][0]^=1;
        check(!xrfg::luke_hfw_layout_matches(module,m),"HFW wrong selection signature rejected");
        m.blocks[module+0x20788b][0]^=1;
        m.put(module+xrfg::kLukeHfwVtable+30*8,std::uint64_t{module+1});
        check(!xrfg::luke_hfw_layout_matches(module,m),"HFW wrong vtable method rejected");
        for(bool hfw:{false,true}) {
            auto n=november(hfw);const auto layout=xrfg::luke_runtime_binding(module,n);
            check(layout&&*layout==xrfg::kLukeNovemberLayout,"November exact build selects its own layout");
            check(xrfg::luke_hfw_layout_matches(module,n,*layout),"November HFW source and shifted virtual slot");
            check(!hfw||!xrfg::read_luke_identity(module,n),"November class must never use September offsets");
            const auto id=xrfg::read_luke_identity(module,n,hfw,*layout);
            check(id&&id->object==object&&id->render==42,"November reads completed source, not stale777 field");
            if(hfw)check(!xrfg::read_luke_identity(module,n,false,*layout),"HFW still needs explicit source-selection authorization");
            n=november(hfw);n.race=true;check(!xrfg::read_luke_identity(module,n,hfw,*layout),"November render race refused");
            n=november(hfw);n.owner_race=true;check(!xrfg::read_luke_identity(module,n,hfw,*layout),"November owner replacement refused");
            n=november(hfw);n.vtable_race=true;check(!xrfg::read_luke_identity(module,n,hfw,*layout),"November class replacement refused");
            n=november(hfw);n.fail=object+0x841780;check(!xrfg::read_luke_identity(module,n,hfw,*layout),"November unavailable source counter refused");
            n=november(hfw);n.put(object+0x841780,~std::uint64_t{});check(!xrfg::read_luke_identity(module,n,hfw,*layout),"November unselected source sentinel refused");
        }
        for(auto witness:{0x2ccf70u,0x2c8f11u,0xbcbe7u,0xb3d4bu,0xb001bu}) {
            auto n=november(true);n.blocks[module+witness][0]^=1;
            check(!xrfg::luke_runtime_binding(module,n),"changed November structural witness must refuse binding");
        }
        auto n=november(true);n.blocks[module][0x108]^=1;check(!xrfg::luke_runtime_binding(module,n),"unrecognized newer build still refused");
        n=november(true);n.blocks[module+0x211e8b][0]^=1;
        check(!xrfg::luke_hfw_layout_matches(module,n,xrfg::kLukeNovemberLayout),"wrong November selected-source instruction refused");
        n=november(true);n.put(module+0x5a9840+32*8,std::uint64_t{module+0x20f420});
        check(!xrfg::luke_hfw_layout_matches(module,n,xrfg::kLukeNovemberLayout),"FOV virtual method cannot authorize source selection");
        std::cout << "Luke probe layout/identity/race/fault tests passed\n";
        return 0;
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
