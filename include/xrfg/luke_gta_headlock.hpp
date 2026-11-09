#pragma once
#include "xrfg/native_ingress/GtaRuntimeBinding.h"
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
using LukeGtaRuntimeLayout = native_ingress::gta_runtime_binding::Layout;
template<class Read>
std::optional<LukeGtaRuntimeLayout> luke_gta_headlock_binding(std::uintptr_t module,Read&& read,
    unsigned* refusal=nullptr) noexcept {
    return native_ingress::gta_runtime_binding::discover(module,read,refusal);
}
// The layout has already been structurally verified once for this module.
// Never select a layout from a version number or assume fixed object offsets.
template<class Read>
std::optional<LukeGtaHeadlock> read_luke_gta_headlock(std::uintptr_t module,Read&& read,
    bool capture_tracking,const LukeGtaRuntimeLayout& layout) noexcept {
    const auto get=[&](std::uintptr_t address,auto& value){
        return address>=0x10000&&address<0x00007fffffff0000ULL&&read(address,&value,sizeof(value));
    };
    std::uint64_t object{},vtable{},after{},after_vtable{},render{},after_render{};
    std::int32_t mode{},after_mode{};float yaw{},after_yaw{};
    if(!layout.runtime_pointer||!layout.runtime_vtable||!layout.render_offset||!layout.yaw_offset||!layout.mode_offset||
        !get(module+layout.runtime_pointer,object)||!object||!get(object,vtable)||
        vtable!=module+layout.runtime_vtable||!get(object+layout.render_offset,render)||
        !get(object+layout.yaw_offset,yaw)||!get(object+layout.mode_offset,mode)||
        !get(object+layout.yaw_offset,after_yaw)||!get(object+layout.mode_offset,after_mode)||
        !get(object+layout.render_offset,after_render)||!get(object,after_vtable)||
        !get(module+layout.runtime_pointer,after)||object!=after||vtable!=after_vtable||
        render>0x7fffffffffffffffULL||after_render>0x7fffffffffffffffULL||
        mode!=after_mode||yaw!=after_yaw)return {};
    if((mode!=0&&mode!=2)||!std::isfinite(yaw)||std::abs(yaw)>6.4F)return {};
    LukeGtaHeadlock result{object,render,mode,yaw};
    // Optional raw tracking records have a different ABI. The camera witnesses
    // do not prove their offsets; leave them unavailable rather than reading
    // the old fixed addresses in an otherwise newly discovered runtime.
    (void)capture_tracking;
    return result;
}
[[nodiscard]] std::optional<LukeGtaHeadlock> query_luke_gta_headlock(bool capture_tracking=false) noexcept;
} // namespace xrfg
