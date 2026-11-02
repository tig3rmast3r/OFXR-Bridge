#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <d3d11_4.h>
#include <d3d12.h>
#include <windows.h>

namespace xrfg {
struct TwoFrameImageInfo;

// How the application's image reaches the runtime's for one bridged
// swapchain. Logged in the swapchain's d3d11_bridge record.
enum class D3D11BridgePath : std::uint32_t {
    // The application renders straight into the shared texture.
    direct = 0,
    // The application renders into a depth texture of its own; at release
    // it is copied into a shared texture of the typeless family, because
    // the driver refused to share a depth texture.
    depth_copy = 1,
    // The application renders into a multisampled texture of its own; at
    // release it is resolved into the single-sample shared texture. The
    // runtime's swapchain is single-sample.
    resolve = 2,
    // The application renders into a mipmapped texture of its own; at
    // release mip 0 of each slice is copied into a single-mip shared texture
    // and from there into mip 0 of the runtime's image. D3D11 will not open
    // a shared D3D12 texture with more than one mip in any combination of
    // format, flags or array size (measured with Cyberpunk 2077's four-mip
    // swapchains). The runtime's lower mips are not written, as on the
    // direct D3D11 path, where the synthesis also carries mip 0 only.
    mip_copy = 3,
    // Depth the driver will share in no form: the packed depth-stencil
    // families, where D3D11 opens neither a depth-flagged nor a bare
    // typeless shared texture (measured with Ready or Not's
    // D32_FLOAT_S8X24_UINT and with D24_UNORM_S8_UINT on the same GPU).
    // The application renders into a depth texture of the layer's and
    // nothing reaches the runtime's image; the layer strips
    // the depth information naming this swapchain from every submission, so
    // the runtime composes without depth, as it does for the many titles
    // that never submit any. The alternative was failing the creation, which
    // a title may not survive.
    depth_private = 4,
};

struct D3D11BridgeSwapchainDescription {
    // The format and sample count the application asked for.
    DXGI_FORMAT requested_format{DXGI_FORMAT_UNKNOWN};
    std::uint32_t requested_sample_count{1};
    bool unordered_access{};
    bool depth_stencil{};
};

// One application swapchain of a bridged D3D11 session: the runtime owns
// D3D12 images, the layer owns D3D12 shared textures of the same shape, and
// the application renders into those textures through D3D11 views - or,
// for depth the driver will not share and for multisampling, into a D3D11
// texture of its own that the release step copies or resolves into the
// shared one. See SessionState::d3d11_bridge in the layer for why the
// runtime is handed a D3D12 session at all.
//
// Every method returns immediately; the two sides are ordered on one shared
// fence, and the only CPU wait is wait_for_idle at teardown.
class D3D11BridgeSwapchain final {
public:
    D3D11BridgeSwapchain();
    ~D3D11BridgeSwapchain();
    D3D11BridgeSwapchain(const D3D11BridgeSwapchain&) = delete;
    D3D11BridgeSwapchain& operator=(const D3D11BridgeSwapchain&) = delete;

    // Creates one shared texture per runtime image, in the runtime image's
    // own format and shape, and whatever application-side texture the path
    // needs. Fails, with the stage in `failure_stage`, if nothing shareable
    // can be made for the shape.
    [[nodiscard]] HRESULT initialize(
        ID3D11Device* d3d11_device,
        ID3D11DeviceContext* d3d11_context,
        ID3D12Device* d3d12_device,
        ID3D12CommandQueue* d3d12_queue,
        std::span<ID3D12Resource* const> runtime_images,
        const D3D11BridgeSwapchainDescription& description,
        std::uint32_t* failure_stage) noexcept;

    [[nodiscard]] D3D11BridgePath path() const noexcept;
    [[nodiscard]] DXGI_FORMAT shared_format() const noexcept;
    // What the application is given: the shared textures on the direct
    // path, its own textures on the others.
    [[nodiscard]] std::span<ID3D11Texture2D* const> d3d11_images() const noexcept;
    [[nodiscard]] std::span<ID3D12Resource* const> shared_images() const noexcept;

    // The application is about to render into image `index` (its wait
    // returned): its context waits, on the GPU, for the layer's last read of
    // the shared texture.
    [[nodiscard]] HRESULT before_write(std::uint32_t index) noexcept;

    // The application released image `index`: on the copy and resolve paths
    // the application's texture is moved into the shared one first; then its
    // context signals, the layer's queue waits and copies the shared texture
    // into the runtime's image. What the layer queues after this on the same
    // queue - the history capture - reads the shared texture in order.
    [[nodiscard]] HRESULT release(std::uint32_t index) noexcept;

    // F8 only: snapshot the application-side shared image after release(),
    // ordered on this bridge's own queue and before the existing mark_read().
    // This is distinct from the runtime image and from AFW's history mirror.
    [[nodiscard]] HRESULT diagnostic_capture_application(
        std::uint32_t index, const TwoFrameImageInfo& info) noexcept;

    // Everything the layer will read from image `index` this frame has been
    // queued: signal, so the next before_write of this image waits for it.
    [[nodiscard]] HRESULT mark_read(std::uint32_t index) noexcept;

    [[nodiscard]] HRESULT wait_for_idle() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xrfg
