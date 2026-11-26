#pragma once
#include <array>
#include <cstdint>
namespace xrfg::native_ingress::streamline_camera {
struct Camera {
    std::uint32_t frame{}, viewport{};
    bool valid{}, inverted{}, ambiguous{};
    // Diagnostic only: projection=1, inverse=2, position=4, basis=8,
    // near=16, far=32, FOV=64, no evaluation scope=256,
    // no exact successful constants=512, ambiguous viewport=1024.
    std::uint32_t rejection{256};
    bool observed{};
    std::array<float,16> projection{}, inverse{}, previous{}, to_current{};
    std::array<float,3> position{}, right{}, up{}, forward{};
    float near_z{}, far_z{}, fov{};
    int camera_motion{-1};
};
// Guide consumers may reconstruct a missing/invalid FOV only from this exact
// packet's normalized perspective and mutually inverse matrices. The default
// receipt retains the original SDK scalar and admission decision for diagnosis.
Camera current(bool projection_receipt=false) noexcept;
bool hooked() noexcept;
}
