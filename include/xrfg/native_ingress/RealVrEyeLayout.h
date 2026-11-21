#pragma once
#include <array>
#include <cstdint>

namespace xrfg::native_ingress::real_vr_eye_layout {

// Module builds with the same eye draw and selected-render ring ABI. The
// consumers still verify the loaded instructions and active vtable methods.
struct Layout {
    std::uint32_t timestamp, image_size, draw_eye, swapchain_vtable, release, acquire_index;
    std::uint32_t owner_reference, camera_method_slot, depth_method_slot;
};

inline constexpr std::array layouts{
    Layout{0x6ad0cdaf, 0xaf4000,  0x2a8d20, 0x606bf0, 0x2e2050, 0x2e2296, 0x1fbdd9, 50, 61},
    Layout{0x6aef4847, 0x691d000, 0x2bc140, 0x61d740, 0x2f52d0, 0x2f5516, 0x1fcc89, 52, 63},
    Layout{0x6af0c3f1, 0x691d000, 0x2bc2f0, 0x61d740, 0x2f5480, 0x2f56c6, 0x1fcc89, 52, 63},
    Layout{0x6afcf372, 0x691d000, 0x2bc360, 0x61d740, 0x2f54f0, 0x2f5736, 0x1fcc89, 52, 63},
};

[[nodiscard]] constexpr const Layout* find(std::uint32_t timestamp, std::uint32_t image_size) noexcept {
    for (const auto& layout : layouts)
        if (layout.timestamp == timestamp && layout.image_size == image_size) return &layout;
    return nullptr;
}

} // namespace xrfg::native_ingress::real_vr_eye_layout
