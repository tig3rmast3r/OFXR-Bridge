#pragma once
#include "RealVrRuntimeImage.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

namespace xrfg::native_ingress::rdr2_runtime_binding {
struct Layout {
    std::uint32_t owner{}, vtable{}, callback{}, matcher{};
    std::uint32_t render{}, selected{}, ring{}, stride{}, matrix{};
    bool operator==(const Layout&) const = default;
};
// Structural discovery only. RVAs and object offsets come from independently
// linked instructions/RTTI; neither the PE timestamp nor a version selects them.
namespace detail = real_vr_runtime_image;

template<class Read>
std::optional<Layout> discover(std::uintptr_t base, Read&& read, unsigned* refusal=nullptr) noexcept {
    const auto fail=[&](unsigned why)->std::optional<Layout>{if(refusal)*refusal=why;return {};};
    try {
        using namespace detail;
        const auto loaded=load(base,read);if(!loaded)return fail(1);
        const auto& image=*loaded;
        const auto function=[&](std::uint32_t rva){return detail::function(image,rva);};
        const auto found_vtable=vtable(image,".?AVruntime_RDR2@@");if(!found_vtable)return fail(2);
        const std::array vtables{*found_vtable};
        const auto is_method=[&](std::uint32_t rva) {
            for(unsigned i=0;i<256;++i) {
                const auto bytes=image.at(vtables[0]+i*8,8);if(bytes.empty())break;
                const auto address=value<std::uint64_t>(bytes,0);
                if(address<base || address-base>UINT32_MAX || !image.has(static_cast<std::uint32_t>(address-base),exec))break;
                if(address==base+rva)return true;
            }
            return false;
        };
        struct Wrapper {std::uint32_t owner{},callback{};bool gun{};};
        std::vector<Wrapper> wrappers;
        for(const auto& s:image.sections)if(s.flags&exec)for(std::size_t i=0;i+23<=s.bytes.size();++i) {
            if(!match(s.bytes,i,{0x4c,0x8b,0xc1,0x48,0x8b,0x0d,-1,-1,-1,-1,0x48,0x85,0xc9,0x74,7,
                -1,-1,0xe9,-1,-1,-1,-1,0xc3}))continue;
            const bool gun=match(s.bytes,i+15,{0xb2,1});
            if(!gun && !match(s.bytes,i+15,{0x33,0xd2}))continue;
            const auto start=s.rva+static_cast<unsigned>(i);
            const auto owner=image.relative(start+3,7,3),callback=image.relative(start+17,5,1);
            if(owner && callback && image.has(*owner,writable) && image.has(*callback,exec))
                wrappers.push_back({*owner,*callback,gun});
        }
        std::vector<Layout> candidates;
        for(const auto& wrapper:wrappers)if(!wrapper.gun) {
            if(std::count_if(wrappers.begin(),wrappers.end(),[&](const auto& w){return w.gun&&w.owner==wrapper.owner&&w.callback==wrapper.callback;})!=1)continue;
            const auto cb=function(wrapper.callback);if(!cb || (*cb)[0]!=wrapper.callback)continue;
            const auto body=image.at((*cb)[0],(*cb)[1]-(*cb)[0]);
            if(!match(body,0,{0x48,0x8b,0xc4,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}))continue;
            std::vector<std::uint32_t> renders,rings;
            unsigned owner_register=8;
            for(std::size_t i=0;i+11<=body.size();++i) {
                if(match(body,i,{0xf0,0x48,0x0f,0xc1,-1,-1,-1,-1,-1}) &&
                    (body[i+4]&0xf8)==0x90 && (body[i+4]&7)!=4) {
                    renders.push_back(value<std::uint32_t>(body,i+5));owner_register=body[i+4]&7;
                }
            }
            if(renders.size()!=1)continue;
            // The compiler may move the owner between nonvolatile registers.
            // Require RCX (this) copied into the same base used by both fields.
            bool owner_argument{};
            for(std::size_t i=0;i+3<=std::min<std::size_t>(body.size(),256);++i)
                owner_argument|=match(body,i,{0x48,0x8b,static_cast<int>(0xc1|(owner_register<<3))});
            if(!owner_argument)continue;
            for(std::size_t i=0;i+12<=body.size();++i)
                if(match(body,i,{0x48,0x8d,static_cast<int>(0x90|owner_register),-1,-1,-1,-1,0xb9,4,0,0,0}))
                    rings.push_back(value<std::uint32_t>(body,i+3));
            if(renders.size()!=1 || rings.empty() || !std::all_of(rings.begin(),rings.end(),[&](auto r){return r==rings[0];}))continue;
            // A virtual RDR2 method compares the GPU's 48-byte view against a
            // four-entry camera ring, then commits the exact selected render.
            for(const auto& s:image.sections)if(s.flags&exec)for(std::size_t i=0;i+41<=s.bytes.size();++i) {
                if(!match(s.bytes,i,{0x49,0x69,0xd6,-1,-1,-1,-1,0x48,0x81,0xc2,-1,-1,-1,-1,
                    0x49,0x03,0xd7,0x44,0x8d,0x43,0x30,0x48,0x8d,0x4d,0x80,0xe8,-1,-1,-1,-1,
                    0x85,0xc0,0x75,9,0x49,0x89,0xbf,-1,-1,-1,-1}))continue;
                const auto fn=function(s.rva+static_cast<unsigned>(i));if(!fn || !is_method((*fn)[0]))continue;
                const auto stride=value<std::uint32_t>(s.bytes,i+3),matrix=value<std::uint32_t>(s.bytes,i+10);
                const auto selected=value<std::uint32_t>(s.bytes,i+37),ring=rings[0],render=renders[0];
                if(stride<64 || stride>4096 || matrix<ring || matrix-ring+48>stride ||
                    ring>=64*1024*1024 || render>=64*1024*1024 || selected>=64*1024*1024 ||
                    selected<ring+4*stride || render<=selected || render-selected>64)continue;
                const auto method=image.at((*fn)[0],(*fn)[1]-(*fn)[0]);
                unsigned stores{},ring_loads{};
                for(std::size_t j=0;j+8<=method.size();++j) {
                    if(match(method,j,{0x49,0x89,0xbf,-1,-1,-1,-1}) && value<std::uint32_t>(method,j+3)==selected)++stores;
                    if(match(method,j,{0x4a,0x8b,0xbc,0x39,-1,-1,-1,-1}) && value<std::uint32_t>(method,j+4)==ring)++ring_loads;
                }
                if(stores!=2 || ring_loads!=1)continue;
                candidates.push_back({wrapper.owner,vtables[0],wrapper.callback,(*fn)[0],render,selected,ring,stride,matrix-ring});
            }
        }
        if(candidates.size()!=1)return fail(candidates.empty()?3:4);
        if(refusal)*refusal=0;
        return candidates.front();
    } catch(...) {return fail(5);}
}
}
