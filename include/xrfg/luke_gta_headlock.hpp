#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>

namespace xrfg {
struct LukeGtaHeadlock {
    std::uintptr_t owner{};
    std::uint64_t render{};
    std::int32_t mode{};
    float yaw_offset{};
    // Optional diagnostic receipt captured with this original SR production.
    // Four producer tracking records, each quaternion XYZW + position XYZ.
    // These never select a camera or affect AFW admission.
    bool tracking_valid{};
    std::array<float,28> tracking{};
    [[nodiscard]] bool active() const noexcept { return mode==2; }
};
inline constexpr std::uintptr_t kLukeGtaRuntimePointer=0x7a9cf0;
inline constexpr std::uintptr_t kLukeGtaRuntimeVtable=0x596910;

// Read-only interoperability for the explicitly observed GTA runtime layout.
// Unknown builds/objects/racing state do not enable the special camera path.
template<class Read>
bool luke_gta_headlock_layout(std::uintptr_t module,Read&& read) noexcept {
    std::array<unsigned char,4096> header{};
    if(!read(module,header.data(),header.size()))return false;
    const auto u32=[&](std::size_t i){std::uint32_t v{};std::memcpy(&v,header.data()+i,4);return v;};
    if(header[0]!='M'||header[1]!='Z')return false;
    const auto nt=u32(0x3c);
    if(nt>header.size()-128||u32(nt)!=0x4550||u32(nt+8)!=1792152767||u32(nt+24+56)!=11485184)return false;
    constexpr std::array<unsigned char,12> state{
        0x8b,0x87,0x2c,0x11,0x84,0x00,0x89,0x87,0x58,0x6c,0x85,0x00};
    constexpr std::array<unsigned char,8> yaw{0xf3,0x0f,0x5c,0xb3,0x18,0x11,0x84,0x00};
    std::array<unsigned char,12> live_state{};std::array<unsigned char,8> live_yaw{};
    return read(module+0x1fedff,live_state.data(),live_state.size())&&live_state==state&&
        read(module+0x1fdef6,live_yaw.data(),live_yaw.size())&&live_yaw==yaw;
}
template<class Read>
std::optional<LukeGtaHeadlock> read_luke_gta_headlock(std::uintptr_t module,Read&& read,
    bool capture_tracking=false) noexcept {
    const auto get=[&](std::uintptr_t address,auto& value){
        return address>=0x10000&&address<0x00007fffffff0000ULL&&read(address,&value,sizeof(value));
    };
    std::uint64_t object{},vtable{},after{},after_vtable{},render{},after_render{};
    std::array<unsigned char,24> state{},after_state{};
    if(!get(module+kLukeGtaRuntimePointer,object)||!object||!get(object,vtable)||
        vtable!=module+kLukeGtaRuntimeVtable||!get(object+0x841790,render)||
        !get(object+0x841118,state)||!get(object+0x841118,after_state)||
        !get(object+0x841790,after_render)||!get(object,after_vtable)||
        !get(module+kLukeGtaRuntimePointer,after)||object!=after||vtable!=after_vtable||
        render>0x7fffffffffffffffULL||after_render>0x7fffffffffffffffULL||
        std::memcmp(state.data()+20,after_state.data()+20,4))return {};
    LukeGtaHeadlock result{object,render};
    std::memcpy(&result.yaw_offset,state.data(),4);std::memcpy(&result.mode,state.data()+20,4);
    if((result.mode!=0&&result.mode!=2)||!std::isfinite(result.yaw_offset)||std::abs(result.yaw_offset)>6.4F)return {};
    if(capture_tracking) {
        std::array<unsigned char,180> poses{},verified{};
        std::uint64_t final_render{},final_owner{},final_vtable{};
        if(read(object+0x840ab8,poses.data(),poses.size())&&
            read(object+0x840ab8,verified.data(),verified.size())&&poses==verified&&
            get(object+0x841790,final_render)&&final_render==render&&
            get(object,final_vtable)&&final_vtable==vtable&&
            get(module+kLukeGtaRuntimePointer,final_owner)&&final_owner==object) {
            constexpr std::array<std::size_t,4> offsets{0,48,96,152};
            bool valid=true;
            for(std::size_t pose=0;pose<offsets.size();++pose) {
                auto* values=result.tracking.data()+pose*7;
                std::memcpy(values,poses.data()+offsets[pose],7*sizeof(float));
                float norm{};
                for(unsigned i=0;i<7;++i)valid&=std::isfinite(values[i]);
                for(unsigned i=0;i<4;++i)norm+=values[i]*values[i];
                valid&=norm>.9F&&norm<1.1F;
            }
            result.tracking_valid=valid;
            if(!valid)result.tracking={};
        }
    }
    return result;
}
[[nodiscard]] std::optional<LukeGtaHeadlock> query_luke_gta_headlock(bool capture_tracking=false) noexcept;
} // namespace xrfg
