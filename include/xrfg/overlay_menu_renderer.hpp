#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace xrfg {
struct OverlayMenuInputFrame;
// Called with this renderer's private ImGuiContext current, inside its frame.
// The callback owns its ImGui Begin/End windows, and must not call Render.
using OverlayMenuDraw = void(*)(void* user);

class OverlayMenuRenderer final {
public:
    OverlayMenuRenderer() noexcept;
    ~OverlayMenuRenderer();
    OverlayMenuRenderer(const OverlayMenuRenderer&)=delete;
    OverlayMenuRenderer& operator=(const OverlayMenuRenderer&)=delete;
    HRESULT initialize(ID3D12Device* device12,ID3D12CommandQueue* queue12,
        ID3D11Device* device11,DXGI_FORMAT format,std::uint32_t image_count,
        std::uint32_t width=1024,std::uint32_t height=768,float density=1.F) noexcept;
    // Caller has legally acquired/waited this XR image. Its private D12 list is
    // open and the texture is already RENDER_TARGET; it remains in that state.
    // This records immutable per-image mesh/atlas/descriptors, never a frame ring.
    HRESULT render_d3d12(ID3D12Resource* texture,ID3D12GraphicsCommandList* list,
        std::uint32_t image_index,const OverlayMenuInputFrame& input,
        OverlayMenuDraw draw,void* user=nullptr) noexcept;
    // Driver-owned private context state; SwapDeviceContextState restores every
    // immediate-context binding, including SINGLETHREADED devices. Caller
    // serializes access and signals its fence after the immediate draw.
    HRESULT render_d3d11(ID3D11Texture2D* texture,std::uint32_t image_index,
        const OverlayMenuInputFrame& input,OverlayMenuDraw draw,void* user=nullptr) noexcept;
    // Call only after the recording actually executed and this exact fence
    // value was successfully queued. Signal failures retain/quarantine data.
    HRESULT submitted_d3d12(std::uint32_t image_index,ID3D12Fence* fence,
        std::uint64_t value) noexcept;
    HRESULT submitted_d3d11(std::uint32_t image_index,ID3D11Fence* fence,
        std::uint64_t value) noexcept;
    // Only for D12 recordings that have never executed. Reset/discard the list
    // before this call; it cannot be used to cancel GPU-owned resources.
    HRESULT abandon_d3d12(std::uint32_t image_index) noexcept;
    // Close/focus-loss transition only: release private UI drag/key ownership.
    // Does not mutate GPU resources or the game's separate ImGui context.
    void reset_input() noexcept;
    // Refuses unretired/removed-device receipts and unaccounted executed draws.
    // Caller drains its GPU owner first. Destruction retains unsafe resources.
    HRESULT shutdown() noexcept;
    [[nodiscard]] bool ready() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xrfg
