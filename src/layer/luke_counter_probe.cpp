#include "xrfg/luke_counter_probe.hpp"
#include "xrfg/luke_gta_headlock.hpp"
#include <windows.h>

namespace xrfg {
namespace {
bool read_self(std::uintptr_t address, void* out, std::size_t bytes) noexcept {
    SIZE_T got{};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        out, bytes, &got) && got == bytes;
}
struct Binding {
    std::uintptr_t base{};
    bool supported{};
    bool hfw{};
    Binding() noexcept {
        const auto module = GetModuleHandleW(L"dxgi.dll");
        // No module loading, hooks, patches, or added module references.
        if (!module || !GetProcAddress(module, "RealVRVersion") ||
            !GetProcAddress(module, "g_rvrShared")) return;
        base = reinterpret_cast<std::uintptr_t>(module);
        supported = luke_layout_matches(base, read_self);
        hfw = supported && luke_hfw_layout_matches(base, read_self);
    }
};
const Binding& binding() { static const Binding value; return value; }
struct GtaBinding {
    std::uintptr_t base{};bool supported{};
    GtaBinding() noexcept {
        // System DXGI and Luke's proxy may share a basename. Select the
        // already-loaded game-local module explicitly; never load or pin it.
        wchar_t path[32768]{};
        const auto length=GetModuleFileNameW(nullptr,path,32768);
        if(!length||length>=32768)return;
        auto* slash=wcsrchr(path,L'\\');if(!slash)return;
        if(wcscpy_s(slash+1,32768-static_cast<std::size_t>(slash+1-path),L"dxgi.dll"))return;
        const auto module=GetModuleHandleW(path);
        if(!module||!GetProcAddress(module,"RealVRVersion")||!GetProcAddress(module,"g_rvrShared"))return;
        base=reinterpret_cast<std::uintptr_t>(module);
        supported=luke_gta_headlock_layout(base,read_self);
    }
};
}
std::optional<LukeRenderIdentity> query_luke_render_identity() noexcept {
    const auto& binding = xrfg::binding();
    if (!binding.supported || GetModuleHandleW(L"dxgi.dll") != reinterpret_cast<HMODULE>(binding.base)) return {};
    return read_luke_identity(binding.base, read_self, binding.hfw);
}
bool luke_hfw_producer_supported() noexcept {
    const auto& b=binding();std::uint64_t object{},vt{},after{};
    return b.hfw && GetModuleHandleW(L"dxgi.dll")==reinterpret_cast<HMODULE>(b.base) &&
        read_self(b.base+kLukeRuntimePointer,&object,sizeof(object)) && object>=0x10000 &&
        object<0x00007fffffff0000ULL && read_self(object,&vt,sizeof(vt)) &&
        vt==b.base+kLukeHfwVtable && read_self(b.base+kLukeRuntimePointer,&after,sizeof(after)) && after==object;
}
std::optional<LukeGtaHeadlock> query_luke_gta_headlock(bool capture_tracking) noexcept {
    static const GtaBinding b;
    HMODULE current{};
    if(!b.supported||!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(b.base),&current)||
        current!=reinterpret_cast<HMODULE>(b.base))return {};
    auto value=read_luke_gta_headlock(b.base,read_self,capture_tracking);
    return value?value:read_luke_gta_headlock(b.base,read_self,capture_tracking);
}
} // namespace xrfg
