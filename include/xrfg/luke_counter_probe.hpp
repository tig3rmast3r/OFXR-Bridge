#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "xrfg/luke_render_identity.hpp"

namespace xrfg {
// Read-only, explicitly witnessed layouts. CPU completed-render identities,
// not an XR-call counter or a claim of pixel-identical AER submissions.
inline constexpr std::uintptr_t kLukeRuntimePointer = 0x797408;
inline constexpr std::uintptr_t kLukeCrimsonVtable = 0x585cc0;
inline constexpr std::uintptr_t kLukeHfwVtable = 0x587250;
struct LukeRuntimeLayout {
    std::uintptr_t runtime_pointer{},crimson_vtable{},hfw_vtable{},render_offset{};
    std::uintptr_t hfw_selected_code{},hfw_method{};
    unsigned hfw_method_slot{};
    bool operator==(const LukeRuntimeLayout&) const = default;
};
inline constexpr LukeRuntimeLayout kLukeSeptemberLayout{
    kLukeRuntimePointer,kLukeCrimsonVtable,kLukeHfwVtable,0x841870,0x20788b,0x204e80,30};
inline constexpr LukeRuntimeLayout kLukeNovemberLayout{
    0x7b6500,0x5a85c8,0x5a9840,0x841780,0x211e8b,0x20f480,32};

template<class Read>
std::optional<LukeRuntimeLayout> luke_runtime_binding(std::uintptr_t module,Read&& read) noexcept {
    std::array<unsigned char,4096> header{};
    if(!read(module,header.data(),header.size()))return {};
    const auto u32=[&](std::size_t offset){std::uint32_t v{};
        std::memcpy(&v,header.data()+offset,sizeof(v));return v;};
    if(header[0]!='M'||header[1]!='Z')return {};
    const auto nt=u32(0x3c);
    if(nt>header.size()-128||u32(nt)!=0x4550)return {};
    const bool september=u32(nt+8)==1789728589&&u32(nt+24+56)==11407360;
    const bool november=u32(nt+8)==1793909090&&u32(nt+24+56)==110219264;
    if(!september&&!november)return {};
    std::array<unsigned char,30> hud{
        0xf3,0x0f,0x10,0x89,0xb0,0x1f,0x84,0x00,0x0f,0x57,0xc0,0x0f,0x2f,0xc8,0x77,
        0x0f,0x48,0x8b,0x81,0x38,0x03,0x00,0x00,0xf3,0x0f,0x10,0x88,0xa4,0x03,0x00};
    if(november){hud[4]=0xc4;hud[5]=0x1e;}
    constexpr std::array<unsigned char,14> mapping{
        0x48,0x8b,0xc8,0x83,0xe1,0x07,0x49,0x8b,0x9c,0xce,0xc0,0x0f,0x84,0x00};
    std::array<unsigned char,hud.size()> live_hud{};
    std::array<unsigned char,mapping.size()> live_mapping{};
    if(!read(module+(november?0x2ccf70:0x2b0a80),live_hud.data(),live_hud.size())||live_hud!=hud||
        !read(module+(november?0x2c8f11:0x2aca91),live_mapping.data(),live_mapping.size())||live_mapping!=mapping)return {};
    if(november) {
        // Factory store -> runtime owner, plus both exact class constructors.
        // No production signature scan, module loading or guessed addresses.
        constexpr std::array<unsigned char,7> owner{0x4c,0x89,0x35,0x12,0x99,0x6f,0x00};
        constexpr std::array<unsigned char,10> hfw{0x48,0x8d,0x05,0xee,0x5a,0x4f,0x00,0x48,0x89,0x06};
        constexpr std::array<unsigned char,10> crimson{0x48,0x8d,0x05,0xa6,0x85,0x4f,0x00,0x48,0x89,0x06};
        std::array<unsigned char,7> live_owner{};
        std::array<unsigned char,10> live_hfw{},live_crimson{};
        if(!read(module+0xbcbe7,live_owner.data(),live_owner.size())||live_owner!=owner||
            !read(module+0xb3d4b,live_hfw.data(),live_hfw.size())||live_hfw!=hfw||
            !read(module+0xb001b,live_crimson.data(),live_crimson.size())||live_crimson!=crimson)return {};
    }
    return november?kLukeNovemberLayout:kLukeSeptemberLayout;
}

template<class Read>
bool luke_layout_matches(std::uintptr_t module,Read&& read) noexcept {
    return luke_runtime_binding(module,read).has_value();
}
template<class Read>
bool luke_hfw_layout_matches(std::uintptr_t module,Read&& read,
    const LukeRuntimeLayout& layout=kLukeSeptemberLayout) noexcept {
    // HFW's completed AER source selection, not its XR call counter.
    std::array<unsigned char,16> selected{
        0x48,0x89,0x83,0x70,0x18,0x84,0x00,0xc6,0x84,0x19,0x10,0x14,0x84,0x00,0x01,0x48};
    if(layout==kLukeNovemberLayout){selected[3]=0x80;selected[4]=0x17;selected[10]=0x20;selected[11]=0x13;}
    std::array<unsigned char,16> live{};std::uint64_t method{};
    return read(module+layout.hfw_selected_code,live.data(),live.size())&&live==selected&&
        read(module+layout.hfw_vtable+layout.hfw_method_slot*8,&method,sizeof(method))&&method==module+layout.hfw_method;
}

template<class Read>
std::optional<LukeRenderIdentity> read_luke_identity(std::uintptr_t module, Read&& read,
    bool allow_hfw = false,const LukeRuntimeLayout& layout=kLukeSeptemberLayout) noexcept {
    auto get = [&](std::uintptr_t address, auto& value) {
        return address >= 0x10000 && address < 0x00007fffffff0000ULL &&
            read(address, &value, sizeof(value));
    };
    std::uint64_t object{}, vt{}, render{}, render_after{}, final_object{}, final_vt{};
    if (!get(module+layout.runtime_pointer, object) || !object || !get(object, vt))
        return {};
    if ((vt != module+layout.crimson_vtable && !(allow_hfw && vt==module+layout.hfw_vtable)) || !get(object+layout.render_offset, render) ||
        !get(module+layout.runtime_pointer, final_object) || !get(object, final_vt) ||
        !get(object+layout.render_offset, render_after) || final_object != object || final_vt != vt ||
        render != render_after || render > 0x7fffffffffffffffULL) return {};
    return LukeRenderIdentity{object, render};
}
} // namespace xrfg
