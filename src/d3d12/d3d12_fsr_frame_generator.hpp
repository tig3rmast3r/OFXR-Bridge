#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <span>

namespace xrfg {

struct D3D12FsrEyeInput {
    ID3D12Resource* color{};
    std::uint32_t color_slice{};
    std::uint32_t output_slice{};
    D3D12ImageRect color_rect{};
    ID3D12Resource* depth{};
    D3D12ImageRect depth_rect{};
    D3D12_RESOURCE_STATES depth_state{};
    ID3D12Resource* motion_vectors{};
    D3D12ImageRect motion_rect{};
    D3D12_RESOURCE_STATES motion_state{};
    float motion_scale_x{1.0F};
    float motion_scale_y{1.0F};
    float jitter_x{};
    float jitter_y{};
    float frame_time_delta_ms{16.6667F};
    float camera_near{0.1F};
    float camera_far{1000.0F};
    float camera_fov_vertical{1.5707963F};
    Pose camera_pose{};
    Vec3 camera_position{};
    Vec3 camera_up{};
    Vec3 camera_right{};
    Vec3 camera_forward{};
    bool depth_inverted{};
    bool depth_infinite{};
    bool motion_vectors_jittered{};
    bool reset{};
    bool camera_data_valid{};
    std::uint32_t motion_slice{};
};

[[nodiscard]] bool fsr_generated_array_description(
    const D3D12_RESOURCE_DESC& destination_description,
    std::span<const D3D12FsrEyeInput> eyes,
    D3D12_RESOURCE_DESC* generated_description) noexcept;

class D3D12FsrFrameGenerator final {
public:
    D3D12FsrFrameGenerator() noexcept;
    ~D3D12FsrFrameGenerator();
    D3D12FsrFrameGenerator(const D3D12FsrFrameGenerator&) = delete;
    D3D12FsrFrameGenerator& operator=(const D3D12FsrFrameGenerator&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        const D3D12_RESOURCE_DESC& destination_description,
        DXGI_FORMAT view_format) noexcept;
    [[nodiscard]] HRESULT dispatch(
        ID3D12GraphicsCommandList* command_list,
        std::span<const D3D12FsrEyeInput> eyes,
        ID3D12Resource** generated_array) noexcept;
    [[nodiscard]] bool requires_idle_reconfigure(
        std::span<const D3D12FsrEyeInput> eyes) const noexcept;
    [[nodiscard]] bool output_is_current(unsigned view) const noexcept; // observed SDK reset output
    void reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
