#include "xrfg/luke_counter_probe.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/luke_gta_headlock.hpp"
#include "xrfg/native_ingress/RealVrEyeLayout.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

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
const GtaBinding& gta_binding() { static const GtaBinding value; return value; }

#if defined(_WIN64)
namespace eye_layout = native_ingress::real_vr_eye_layout;

struct SelectedRenderBinding {
    std::uintptr_t base{}, owner_pointer{};
    const eye_layout::Layout* layout{};
};

SelectedRenderBinding find_selected_render_binding() noexcept {
    const HMODULE modules[]{game_luke_module(), GetModuleHandleW(L"dxgi.dll"),
        GetModuleHandleW(L"dxgi2.dll"), GetModuleHandleW(L"RealVR64.dll")};
    for (const auto module : modules) {
        if (!module || !GetProcAddress(module, "RealVRVersion") || !GetProcAddress(module, "g_rvrShared")) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read_self(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 4096 - static_cast<LONG>(sizeof(nt)) ||
            !read_self(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) continue;
        const auto* layout = eye_layout::find(nt.FileHeader.TimeDateStamp, nt.OptionalHeader.SizeOfImage);
        if (!layout) continue;
        // Both wrappers load the active runtime pointer through RIP-relative
        // addressing. In 26.3 this is the D3D12 NGX evaluate wrapper itself.
        std::array<unsigned char, 14> load{};
        if (!read_self(base + layout->owner_reference, load.data(), load.size())) continue;
        constexpr std::array current_load{0x48, 0x8b, 0x3d};
        constexpr std::array current_use{0x4c, 0x8b, 0xf2};
        constexpr std::array v26_3_load{0x4c, 0x8b, 0x25};
        constexpr std::array v26_3_use{0x4c, 0x89, 0x65, 0x28, 0x4d, 0x85, 0xe4};
        const bool matched = layout->abi == eye_layout::Abi::v26_3
            ? std::equal(v26_3_load.begin(), v26_3_load.end(), load.begin()) &&
                std::equal(v26_3_use.begin(), v26_3_use.end(), load.begin() + 7)
            : std::equal(current_load.begin(), current_load.end(), load.begin()) &&
                std::equal(current_use.begin(), current_use.end(), load.begin() + 7);
        if (!matched) continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, load.data() + 3, sizeof(displacement));
        const auto owner_rva = std::int64_t(layout->owner_reference) + 7 + displacement;
        if (owner_rva < 0 || owner_rva > layout->image_size - sizeof(std::uintptr_t)) continue;
        return {base, base + static_cast<std::uintptr_t>(owner_rva), layout};
    }
    return {};
}

template<std::size_t size>
bool method_contains(std::uintptr_t module, std::uintptr_t method,
    const std::array<unsigned char, size>& instructions) noexcept {
    DWORD64 image_base{};
    const auto function = RtlLookupFunctionEntry(method, &image_base, nullptr);
    std::array<unsigned char, 8192> body{};
    if (!function || image_base != module || image_base + function->BeginAddress != method ||
        function->EndAddress <= function->BeginAddress) return false;
    const auto length = function->EndAddress - function->BeginAddress;
    if (length > body.size() || !read_self(method, body.data(), length)) return false;
    const auto end = body.begin() + length;
    return std::search(body.begin(), end, instructions.begin(), instructions.end()) != end;
}

struct SelectedRenderFields {
    std::uint32_t selected, camera_ring;
};

constexpr SelectedRenderFields selected_render_fields(eye_layout::Abi abi) noexcept {
    return abi == eye_layout::Abi::v26_3
        ? SelectedRenderFields{0x841808, 0x841340}
        : SelectedRenderFields{0x841780, 0x8412b8};
}

bool selected_render_methods(const SelectedRenderBinding& binding, std::uint64_t vtable,
    const SelectedRenderFields& fields) noexcept {
    std::array<unsigned char, 27> camera_selection{
        0x83,0xe0,0x03,0x48,0x69,0xd0,0x30,0x01,0,0,
        0x48,0x8b,0x84,0x3a,0,0,0,0,  // Load the camera-ring render ID.
        0x48,0x3b,0x87,0,0,0,0,0x75,0x08}; // Compare it with the selected render.
    std::memcpy(camera_selection.data() + 14, &fields.camera_ring, sizeof(fields.camera_ring));
    std::memcpy(camera_selection.data() + 21, &fields.selected, sizeof(fields.selected));
    auto alternate_register = camera_selection;
    alternate_register[13] = 0x1a;
    alternate_register[20] = 0x83;
    std::array<unsigned char, 9> depth_eye{0x8b,0x83,0,0,0,0,0x83,0xe0,1};
    std::memcpy(depth_eye.data() + 2, &fields.selected, sizeof(fields.selected));
    std::uintptr_t camera{}, depth{};
    return vtable >= binding.base && vtable - binding.base < binding.layout->image_size &&
        read_self(vtable + binding.layout->camera_method_slot * sizeof(camera), &camera, sizeof(camera)) &&
        read_self(vtable + binding.layout->depth_method_slot * sizeof(depth), &depth, sizeof(depth)) &&
        (method_contains(binding.base, camera, camera_selection) ||
            method_contains(binding.base, camera, alternate_register)) &&
        method_contains(binding.base, depth, depth_eye);
}
#endif
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
    const auto& b=gta_binding();
    if(!b.supported||!module_still_current(b.base))return {};
    auto value=read_luke_gta_headlock(b.base,read_self,capture_tracking,b.layout);
    return value?value:read_luke_gta_headlock(b.base,read_self,capture_tracking,b.layout);
}

std::optional<LukeRenderIdentity> query_real_vr_selected_render() noexcept {
#if defined(_WIN64)
    static const auto binding = find_selected_render_binding();
    if (!binding.layout || !module_still_current(binding.base)) return {};
    const auto read = [](std::uintptr_t address, std::uint64_t& value) {
        return read_self(address, &value, sizeof(value));
    };
    constexpr std::uintptr_t camera_stride = 0x130;
    const auto fields = selected_render_fields(binding.layout->abi);
    std::uint64_t owner{}, vtable{}, render{}, ring_render{}, after{}, after_owner{};
    if (!read(binding.owner_pointer, owner) || !owner || !read(owner, vtable)) return {};
    static std::atomic_uint64_t verified_vtable{}, refused_vtable{};
    if (vtable != verified_vtable.load(std::memory_order_relaxed)) {
        if (!vtable || vtable == refused_vtable.load(std::memory_order_relaxed)) return {};
        const bool verified = selected_render_methods(binding, vtable, fields);
        (verified ? verified_vtable : refused_vtable).store(vtable, std::memory_order_relaxed);
        bridge_flight_logger().event(BridgeFlightOperation::luke_afw_stage,
            verified ? 1 : 0, 1372, binding.owner_pointer - binding.base, vtable - binding.base);
        if (!verified) return {};
    }
    if (!read(owner + fields.selected, render) || render >= (1ULL << 63) ||
        !read(owner + fields.camera_ring + (render & 3) * camera_stride, ring_render) || ring_render != render ||
        !read(owner + fields.selected, after) || after != render ||
        !read(binding.owner_pointer, after_owner) || after_owner != owner) return {};
    return LukeRenderIdentity{owner, render};
#else
    return {};
#endif
}
} // namespace xrfg
