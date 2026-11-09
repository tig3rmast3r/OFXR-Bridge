#pragma once
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
namespace detail {
template<class T> T value(std::span<const unsigned char> bytes, std::size_t at) {
    T out{}; if (at <= bytes.size() && sizeof(T) <= bytes.size()-at)
        std::memcpy(&out, bytes.data()+at, sizeof(T));
    return out;
}
inline bool match(std::span<const unsigned char> bytes, std::size_t at,
    std::initializer_list<int> pattern) {
    if (at > bytes.size() || pattern.size() > bytes.size()-at) return false;
    for (int b : pattern) { if (b >= 0 && bytes[at] != b) return false; ++at; }
    return true;
}
struct Section { std::uint32_t rva{}, size{}, flags{}; std::vector<unsigned char> bytes; };
struct Image {
    std::uintptr_t base{};
    std::vector<Section> sections;
    std::span<const unsigned char> at(std::uint32_t rva, std::size_t size) const {
        for (const auto& s : sections) if (rva >= s.rva && rva-s.rva <= s.bytes.size() &&
            size <= s.bytes.size()-(rva-s.rva)) return {s.bytes.data()+rva-s.rva,size};
        return {};
    }
    bool has(std::uint32_t rva, std::uint32_t flags) const {
        for (const auto& s : sections) if ((s.flags&flags)==flags &&
            rva>=s.rva && rva-s.rva<s.size) return true;
        return false;
    }
    std::optional<std::uint32_t> relative(std::uint32_t instruction, unsigned end, unsigned displacement) const {
        const auto bytes=at(instruction,end); if(bytes.empty())return {};
        const auto rva=std::int64_t(instruction)+end+value<std::int32_t>(bytes,displacement);
        if(rva<0 || rva>UINT32_MAX)return {};
        return static_cast<std::uint32_t>(rva);
    }
};
inline constexpr std::uint32_t exec=0x20000000, readable=0x40000000, writable=0x80000000;
}

template<class Read>
std::optional<Layout> discover(std::uintptr_t base, Read&& read, unsigned* refusal=nullptr) noexcept {
    const auto fail=[&](unsigned why)->std::optional<Layout>{if(refusal)*refusal=why;return {};};
    try {
        using namespace detail;
        std::array<unsigned char,4096> header{};
        if(!read(base,header.data(),header.size()))return fail(1);
        const auto nt=value<std::uint32_t>(header,0x3c);
        if(!match(header,0,{'M','Z'}) || nt>header.size()-264 ||
            value<std::uint32_t>(header,nt)!=0x4550 || value<std::uint16_t>(header,nt+4)!=0x8664 ||
            value<std::uint16_t>(header,nt+24)!=0x20b)return fail(1);
        const auto count=value<std::uint16_t>(header,nt+6);
        const auto optional=value<std::uint16_t>(header,nt+20);
        const auto size=value<std::uint32_t>(header,nt+24+56);
        const std::size_t table=nt+24+optional;
        if(!count || count>32 || optional<144 || table>header.size() || count*40>header.size()-table ||
            size<4096 || size>512*1024*1024)return fail(1);
        Image image;image.base=base;
        std::size_t total{};
        for(unsigned i=0;i<count;++i) {
            const auto at=table+i*40;
            Section s;s.rva=value<std::uint32_t>(header,at+12);s.size=value<std::uint32_t>(header,at+8);
            s.flags=value<std::uint32_t>(header,at+36);
            // Initialized bytes contain code, RTTI and unwind records. Never
            // copy the large zero-initialized game-state reservation.
            const auto bytes=std::min(s.size,value<std::uint32_t>(header,at+16));
            if(!(s.flags&readable))continue;
            if(s.rva>size || s.size>size-s.rva || (total+=bytes)>64*1024*1024)return fail(1);
            s.bytes.resize(bytes);
            if(bytes && !read(base+s.rva,s.bytes.data(),bytes))return fail(1);
            image.sections.push_back(std::move(s));
        }
        const auto unwind_rva=value<std::uint32_t>(header,nt+24+112+3*8);
        const auto unwind_size=value<std::uint32_t>(header,nt+24+112+3*8+4);
        const auto unwind=image.at(unwind_rva,unwind_size);
        if(unwind.empty() || unwind.size()%12)return fail(1);
        const auto function=[&](std::uint32_t address)->std::optional<std::array<std::uint32_t,2>> {
            for(std::size_t i=0;i<unwind.size();i+=12) {
                const auto begin=value<std::uint32_t>(unwind,i),end=value<std::uint32_t>(unwind,i+4);
                if(begin<=address && address<end && end-begin<=65536 && image.has(begin,exec) &&
                    !image.at(begin,end-begin).empty())return std::array{begin,end};
            }
            return {};
        };
        // The named RTTI type and its complete-object vtable identify RDR2,
        // independently of similar camera code used by another game.
        constexpr char type_name[]=".?AVruntime_RDR2@@";
        std::vector<std::uint32_t> types,locators,vtables;
        for(const auto& s:image.sections)if(!(s.flags&exec)) {
            for(std::size_t i=16;i+sizeof(type_name)<=s.bytes.size();++i)
                if(std::memcmp(s.bytes.data()+i,type_name,sizeof(type_name))==0)types.push_back(s.rva+static_cast<unsigned>(i)-16);
        }
        if(types.size()!=1)return fail(2);
        for(const auto& s:image.sections)if(!(s.flags&(exec|writable))) {
            for(std::size_t i=0;i+24<=s.bytes.size();i+=4) {
                if(value<std::uint32_t>(s.bytes,i)==1 && value<std::uint32_t>(s.bytes,i+4)==0 &&
                    value<std::uint32_t>(s.bytes,i+8)==0 && value<std::uint32_t>(s.bytes,i+12)==types[0] &&
                    value<std::uint32_t>(s.bytes,i+20)==s.rva+i)locators.push_back(s.rva+static_cast<unsigned>(i));
            }
        }
        if(locators.size()!=1)return fail(2);
        for(const auto& s:image.sections)if(!(s.flags&(exec|writable))) {
            for(std::size_t i=0;i+32<=s.bytes.size();i+=8)if(value<std::uint64_t>(s.bytes,i)==base+locators[0]) {
                bool methods=true;
                for(unsigned n=1;n<=3;++n) {
                    const auto address=value<std::uint64_t>(s.bytes,i+n*8);
                    methods&=address>=base && address-base<=UINT32_MAX && image.has(static_cast<std::uint32_t>(address-base),exec);
                }
                if(methods)vtables.push_back(s.rva+static_cast<unsigned>(i)+8);
            }
        }
        if(vtables.size()!=1)return fail(2);
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
