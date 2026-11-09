#include "xrfg/luke_counter_probe.hpp"
#include "xrfg/luke_gta_headlock.hpp"
#include <windows.h>
#include "xrfg/bridge_flight_logger.hpp"

namespace xrfg {
namespace {
bool read_self(std::uintptr_t address, void* out, std::size_t bytes) noexcept {
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        out, bytes, &got) && got == bytes;
}
HMODULE game_luke_module() noexcept {
    wchar_t path[32768]{};const auto length=GetModuleFileNameW(nullptr,path,32768);
    if(!length||length>=32768)return {};
    auto* slash=wcsrchr(path,L'\\');if(!slash)return {};
    if(wcscpy_s(slash+1,32768-static_cast<std::size_t>(slash+1-path),L"dxgi.dll"))return {};
    const auto module=GetModuleHandleW(path);
    if(!module||!GetProcAddress(module,"RealVRVersion")||!GetProcAddress(module,"g_rvrShared"))return {};
    return module;
}
bool module_still_current(std::uintptr_t base) noexcept {
    HMODULE current{};
    return base&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(base),&current)&&
        current==reinterpret_cast<HMODULE>(base);
}
struct Binding {
    std::uintptr_t base{};
    bool supported{},hfw{};
    LukeRuntimeLayout layout{kLukeSeptemberLayout};
    Binding() noexcept {
        const auto module=game_luke_module();if(!module)return;
        base=reinterpret_cast<std::uintptr_t>(module);
        if(const auto observed=luke_runtime_binding(base,read_self)) {
            layout=*observed;supported=true;
            hfw=luke_hfw_layout_matches(base,read_self,layout);
        }
    }
};
const Binding& binding() { static const Binding value; return value; }
struct GtaBinding {
    std::uintptr_t base{};bool supported{};
    LukeGtaRuntimeLayout layout{};
    GtaBinding() noexcept {
        // Select the already-loaded game-local proxy, never the system DXGI.
        const auto module=game_luke_module();if(!module)return;
        base=reinterpret_cast<std::uintptr_t>(module);
        unsigned refusal{};
        if(const auto detected=luke_gta_headlock_binding(base,read_self,&refusal)) {
            layout=*detected;supported=true;
        }
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            supported?1:-static_cast<std::int64_t>(refusal),1370,layout.runtime_pointer,layout.runtime_vtable);
        if(supported)bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            layout.render_offset,1371,layout.yaw_offset,layout.mode_offset);
    }
};
}
std::optional<LukeRenderIdentity> query_luke_render_identity() noexcept {
    const auto& binding = xrfg::binding();
    if (!binding.supported || !module_still_current(binding.base)) return {};
    return read_luke_identity(binding.base, read_self, binding.hfw,binding.layout);
}
bool luke_hfw_producer_supported() noexcept {
    const auto& b=binding();std::uint64_t object{},vt{},after{};
    return b.hfw && module_still_current(b.base) &&
        read_self(b.base+b.layout.runtime_pointer,&object,sizeof(object)) && object>=0x10000 &&
        object<0x00007fffffff0000ULL && read_self(object,&vt,sizeof(vt)) &&
        vt==b.base+b.layout.hfw_vtable && read_self(b.base+b.layout.runtime_pointer,&after,sizeof(after)) && after==object;
}
std::optional<LukeGtaHeadlock> query_luke_gta_headlock(bool capture_tracking) noexcept {
    static const GtaBinding b;
    HMODULE current{};
    if(!b.supported||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(b.base),&current)||
        current!=reinterpret_cast<HMODULE>(b.base))return {};
    auto value=read_luke_gta_headlock(b.base,read_self,capture_tracking,b.layout);
    return value?value:read_luke_gta_headlock(b.base,read_self,capture_tracking,b.layout);
}
} // namespace xrfg
