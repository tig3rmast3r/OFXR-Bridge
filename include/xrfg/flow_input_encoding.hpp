#pragma once
#include <dxgiformat.h>
#include <cstdint>

namespace xrfg {
enum class D3D12OpticalFlowInputEncoding {
    reference_linear,
    perceptual_srgb,
};

// The owner preserves legacy 0.2.1's color processing for comparisons. The
// existing NVIDIA new route (0.3.0) receives D17; this is route policy, not an
// old implementation retained as a safety fallback or a new user toggle.
[[nodiscard]] constexpr D3D12OpticalFlowInputEncoding flow_encoding_for_route(
    bool nvidia_new_route) noexcept {
    return nvidia_new_route ? D3D12OpticalFlowInputEncoding::perceptual_srgb
                           : D3D12OpticalFlowInputEncoding::reference_linear;
}

// Bit 0 and bits 8-23 keep their existing repeat/fraction meaning. The shared
// The perceptual route encodes linear FP16 samples in its 8-bit flow input.
// The original FP16 color remains available for synthesis and presentation.
[[nodiscard]] constexpr std::uint32_t flow_input_flags(
    DXGI_FORMAT view_format, D3D12OpticalFlowInputEncoding encoding) noexcept {
    return encoding == D3D12OpticalFlowInputEncoding::perceptual_srgb &&
        (view_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
         view_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
         view_format == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 2U : 0U;
}
} // namespace xrfg
