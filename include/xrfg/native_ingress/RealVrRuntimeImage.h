#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Bounded, read-only PE/RTTI witnesses shared by REAL VR runtime bindings.
// Only initialized section bytes are copied; large game-state BSS stays unread.
namespace xrfg::native_ingress::real_vr_runtime_image {

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
    std::uint32_t unwind_rva{},unwind_size{};
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

template<class Read>
std::optional<Image> load(std::uintptr_t base,Read&& read) {
    std::array<unsigned char,4096> header{};
    if(!read(base,header.data(),header.size()))return {};
    const auto nt=value<std::uint32_t>(header,0x3c);
    if(!match(header,0,{'M','Z'}) || nt>header.size()-264 ||
        value<std::uint32_t>(header,nt)!=0x4550 || value<std::uint16_t>(header,nt+4)!=0x8664 ||
        value<std::uint16_t>(header,nt+24)!=0x20b)return {};
    const auto count=value<std::uint16_t>(header,nt+6);
    const auto optional=value<std::uint16_t>(header,nt+20);
    const auto size=value<std::uint32_t>(header,nt+24+56);
    const std::size_t table=nt+24+optional;
    if(!count || count>32 || optional<144 || table>header.size() || count*40>header.size()-table ||
        size<4096 || size>512*1024*1024)return {};
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
        if(s.rva>size || s.size>size-s.rva || (total+=bytes)>64*1024*1024)return {};
        s.bytes.resize(bytes);
        if(bytes && !read(base+s.rva,s.bytes.data(),bytes))return {};
        image.sections.push_back(std::move(s));
    }
    const auto unwind_rva=value<std::uint32_t>(header,nt+24+112+3*8);
    const auto unwind_size=value<std::uint32_t>(header,nt+24+112+3*8+4);
    image.unwind_rva=unwind_rva; image.unwind_size=unwind_size;
    const auto unwind=image.at(unwind_rva,unwind_size);
    if(unwind.empty() || unwind.size()%12)return {};
    return image;
}
inline std::optional<std::array<std::uint32_t,2>> function(const Image& image,std::uint32_t address) {
    const auto unwind=image.at(image.unwind_rva,image.unwind_size);
    for(std::size_t i=0;i<unwind.size();i+=12) {
        const auto begin=value<std::uint32_t>(unwind,i),end=value<std::uint32_t>(unwind,i+4);
        if(begin<=address && address<end && end-begin<=65536 && image.has(begin,exec) &&
            !image.at(begin,end-begin).empty())return std::array{begin,end};
    }
    return {};
}
inline std::optional<std::uint32_t> vtable(const Image& image,std::string_view type_name) {
    std::vector<std::uint32_t> types,locators,vtables;
    for(const auto& s:image.sections)if(!(s.flags&exec)) {
        for(std::size_t i=16;i+type_name.size()+1<=s.bytes.size();++i)
            if(std::memcmp(s.bytes.data()+i,type_name.data(),type_name.size())==0 && s.bytes[i+type_name.size()]==0)types.push_back(s.rva+static_cast<unsigned>(i)-16);
    }
    if(types.size()!=1)return {};
    for(const auto& s:image.sections)if(!(s.flags&(exec|writable))) {
        for(std::size_t i=0;i+24<=s.bytes.size();i+=4) {
            if(value<std::uint32_t>(s.bytes,i)==1 && value<std::uint32_t>(s.bytes,i+4)==0 &&
                value<std::uint32_t>(s.bytes,i+8)==0 && value<std::uint32_t>(s.bytes,i+12)==types[0] &&
                value<std::uint32_t>(s.bytes,i+20)==s.rva+i)locators.push_back(s.rva+static_cast<unsigned>(i));
        }
    }
    if(locators.size()!=1)return {};
    for(const auto& s:image.sections)if(!(s.flags&(exec|writable))) {
        for(std::size_t i=0;i+32<=s.bytes.size();i+=8)if(value<std::uint64_t>(s.bytes,i)==image.base+locators[0]) {
            bool methods=true;
            for(unsigned n=1;n<=3;++n) {
                const auto address=value<std::uint64_t>(s.bytes,i+n*8);
                methods&=address>=image.base && address-image.base<=UINT32_MAX && image.has(static_cast<std::uint32_t>(address-image.base),exec);
            }
            if(methods)vtables.push_back(s.rva+static_cast<unsigned>(i)+8);
        }
    }
    if(vtables.size()!=1)return {};
    return vtables.front();
}
}
