#pragma once
#include "RealVrRuntimeImage.h"

namespace xrfg::native_ingress::real_vr_eye_layout {

enum class Abi { current, v26_3 };

struct Layout {
    std::uint32_t timestamp{}; // Flight-log label only.
    std::uint32_t draw_eye{}, original{}, endpoint_probe{}, swapchain_vtable{}, owner{};
    std::uint32_t code_begin{}, code_size{};
    Abi abi{};
};

namespace detail {
using namespace real_vr_runtime_image;

// The eye draw spans several chained unwind records. Resolve each witness to
// the same entry point even when the compiler moves blocks within the draw.
inline std::optional<std::uint32_t> function_root(const Image& image, std::uint32_t instruction) {
    const auto entries = image.at(image.unwind_rva, image.unwind_size);
    for (std::size_t i = 0; i < entries.size(); i += 12) {
        auto begin = value<std::uint32_t>(entries,i);
        if (instruction < begin || instruction >= value<std::uint32_t>(entries,i+4)) continue;
        auto unwind = value<std::uint32_t>(entries,i+8);
        for (unsigned depth = 0; depth < 8; ++depth) {
            const auto header = image.at(unwind,4);
            if (header.empty() || !image.has(begin,exec)) return {};
            if (!(header[0] & 0x20)) return begin;
            const auto parent = image.at(unwind + 4 + 2 * ((unsigned(header[2])+1) & ~1U),12);
            if (parent.empty()) return {};
            begin = value<std::uint32_t>(parent,0);
            unwind = value<std::uint32_t>(parent,8);
        }
        return {};
    }
    return {};
}

inline std::optional<Layout> discover(const Image& image) {
    const auto table = vtable(image,".?AV?$OpenXRTextureSwapChain@$0A@@@");
    if (!table) return {};
    const auto method = value<std::uint64_t>(image.at(*table+5*8,8),0);
    if (method < image.base || method-image.base > UINT32_MAX) return {};
    const auto release = static_cast<std::uint32_t>(method-image.base);
    const auto release_function = function(image,release);
    if (!release_function || (*release_function)[0] != release) return {};
    const auto release_body = image.at(release,(*release_function)[1]-release);
    unsigned release_calls{};
    for (std::size_t i = 0; i < release_body.size(); ++i) {
        if (!match(release_body,i,{0x33,0xd2,0x48,0x8b,0x49,0x10,0xe8})) continue;
        const auto target = image.relative(release+static_cast<std::uint32_t>(i)+6,5,1);
        if (target && function_root(image,*target) == *target) ++release_calls;
    }
    if (release_calls != 1) return {};

    std::optional<Layout> result;
    std::array<std::uint32_t,2> owners{};
    std::array<bool,2> ambiguous_owners{};
    unsigned acquire_count{};
    for (const auto& section : image.sections) {
        if (!(section.flags & exec)) continue;
        const std::span<const unsigned char> bytes = section.bytes;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const auto address = section.rva + static_cast<std::uint32_t>(i);
            if (bytes[i] == 0x4c && match(bytes,i,{0x4c,0x8d,0x41,0x20,0x48,0x8b,0x49,0x10,0x33,0xd2,0xe8})) {
                const auto target = image.relative(address+10,5,1);
                if (target && function_root(image,*target) == *target) ++acquire_count;
            }
            const bool modern_owner = bytes[i] == 0x48 &&
                match(bytes,i,{0x48,0x8b,0x3d,-1,-1,-1,-1,0x4c,0x8b,0xf2,0x48,0x8b,0xe9,0x48,0x85,0xff});
            const bool legacy_owner = bytes[i] == 0x4c &&
                match(bytes,i,{0x4c,0x8b,0x25,-1,-1,-1,-1,0x4c,0x89,0x65,0x28,0x4d,0x85,0xe4});
            if (modern_owner || legacy_owner) {
                const auto pointer = image.relative(address,7,3);
                if (function_root(image,address) && pointer && *pointer % 8 == 0 && image.has(*pointer,writable)) {
                    auto& owner = owners[legacy_owner];
                    ambiguous_owners[legacy_owner] |= owner && owner != *pointer;
                    owner = *pointer;
                }
            }
            if (bytes[i] != 0x89 ||
                !match(bytes,i,{0x89,0x54,0x24,0x10,0x55,0x53,0x56,0x57,0x41,-1,0x48,0x8d,0xac,0x24})) continue;
            const bool legacy = bytes[i+9] == 0x57;
            if (!legacy && bytes[i+9] != 0x54) continue;
            const auto entry = function(image,address);
            if (!entry || (*entry)[0] != address) continue;
            const auto prologue = image.at(address,std::min((*entry)[1]-address,128U));
            bool owner_setup{};
            for (std::size_t j = 0; j < prologue.size(); ++j)
                owner_setup |= match(prologue,j,{0x48,0x8b,0xb1,legacy?0xe8:0xb0,legacy?0x14:0x15,0,0,
                    0x48,0x8b,0xf9,0x4c,0x63,legacy?0xfa:0xe2});
            if (!owner_setup) continue;

            std::uint32_t original{}, endpoint{}, color_release{};
            // The mixed-texture path has no corresponding single render ID.
            for (std::size_t j = i+15; j < std::min(bytes.size(),i+16*1024); ++j) {
                const auto site = section.rva + static_cast<std::uint32_t>(j);
                if (bytes[j] == 0x49 &&
                    match(bytes,j-15,{0x49,0x8b,0x44,0x1e,0x58,0x48,0x89,0x44,0x24,0x20,0xe8}) &&
                    match(bytes,j,{0x49,0x8b,0x44,0x1e,0x10,0x4a,0x89,0x84,legacy?0xfe:0xe6,0x98,0x14,0,0}) &&
                    function_root(image,site) == address) {
                    if (original) return {};
                    original = site;
                }
                if (bytes[j] == 0x49 &&
                    match(bytes,j-14,{0x4a,0x8b,0x04,legacy?0x20:0x30,0x48,0x89,0x44,0x24,0x20,0xe8}) &&
                    match(bytes,j,{0x49,0x3b,0xdd,0xb8,0x30,0,0,0,0xb9,0x38,0,0,0,0x0f,legacy?0x4d:0x4c,0xc1,0x4a,0x8b,0x04,legacy?0x20:0x30}) &&
                    function_root(image,site) == address) {
                    if (endpoint) return {};
                    endpoint = site;
                }
                if (bytes[j] == 0x4a &&
                    match(bytes,j,{0x4a,0x8b,0x8c,legacy?0xff:0xe7,legacy?0xf0:0xf8,0x05,0,0,0x48,0x8b,0x01,0xff,0x50,0x28}) &&
                    function_root(image,site) == address) {
                    if (color_release) return {};
                    color_release = site;
                }
            }
            if (!endpoint || endpoint >= original || original >= color_release) continue;
            const auto draw = image.relative(original-5,5,1);
            if (!draw || function_root(image,*draw) != *draw || image.relative(endpoint-5,5,1) != draw) continue;
            if (result) return {};
            result = Layout{image.timestamp,address,original,endpoint-(legacy?14:0),*table,0,
                section.rva,static_cast<std::uint32_t>(section.bytes.size()),legacy?Abi::v26_3:Abi::current};
        }
    }
    if (!result || acquire_count != 1) return {};
    const bool legacy = result->abi == Abi::v26_3;
    if (ambiguous_owners[legacy]) return {};
    result->owner = owners[legacy];
    return result->owner ? result : std::nullopt;
}
} // namespace detail

template<class Read>
std::optional<Layout> discover(std::uintptr_t base, Read&& read) noexcept {
    try {
        const auto image = real_vr_runtime_image::load(base,read);
        return image ? detail::discover(*image) : std::nullopt;
    } catch (...) { return {}; }
}

struct SelectedRenderFields {
    std::uint32_t selected, camera_ring;
};

inline constexpr SelectedRenderFields selected_render_fields(Abi abi) noexcept {
    return abi == Abi::v26_3
        ? SelectedRenderFields{0x841808, 0x841340}
        : SelectedRenderFields{0x841780, 0x8412b8};
}

// A non-code pointer (normally the next RTTI locator) ends the active vtable.
template<class Matches>
bool has_selected_render_methods(const Layout& layout, std::uintptr_t base,
    std::span<const std::uintptr_t> methods, const SelectedRenderFields& fields, Matches&& match_method) {
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
    const auto code = base + layout.code_begin;
    const auto end = std::find_if(methods.begin(),methods.end(),[&](auto method) {
        return method < code || method-code >= layout.code_size;
    });
    constexpr int depth_after_camera = 11;
    unsigned pairs{};
    for (auto method = methods.begin(); end-method > depth_after_camera; ++method) {
        if ((match_method(*method,camera_selection) ||
                match_method(*method,alternate_register)) &&
            match_method(method[depth_after_camera],depth_eye)) {
            if (++pairs > 1) return false;
        }
    }
    return pairs == 1;
}

#if defined(_WIN64)
// Both consumers share the result of the same one-time image inspection.
std::optional<Layout> bind_module(std::uintptr_t base) noexcept;
#endif

} // namespace xrfg::native_ingress::real_vr_eye_layout
