#pragma once
#include <d3d11_4.h>
#include <d3d12.h>
#include <memory>

namespace xrfg {
// Per-NGX-stream transport. No global texture cache and no extra SR dispatch.
class D3D11GuideTransport final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    enum Role { color, motion, depth, output, role_count };
    D3D11GuideTransport();
    ~D3D11GuideTransport();
    HRESULT initialize(ID3D11DeviceContext*, ID3D12Device*, ID3D12CommandQueue*) noexcept;
    HRESULT begin() noexcept;
    HRESULT copy(Role, ID3D11Resource*) noexcept;
    HRESULT submit() noexcept;
    HRESULT copy_back(Role, ID3D11Resource*) noexcept;
    HRESULT drain() noexcept;
    ID3D12Resource* resource(Role) const noexcept;
    ID3D12GraphicsCommandList* list() const noexcept;
    ID3D12Device* device() const noexcept;
    ID3D12CommandQueue* queue() const noexcept;
};
}
