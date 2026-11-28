#pragma once
#include "dlss_motion_vectors.hpp"
#include <d3d11_4.h>

namespace xrfg {
// An exact pending native SR frame carried through the application's small
// color passes. Consumption requires an actual enumerated OpenXR eye image,
// directly or through a staging target proven by its complete copy to that eye.
class D3D11SuperResolutionFrame {
public:
    virtual ~D3D11SuperResolutionFrame() = default;
    virtual HRESULT render(ID3D11DeviceContext*, ID3D11Resource* color,
                           ID3D11Resource* destination, ID3D11ShaderResourceView**) = 0;
    virtual void submitted(ID3D11DeviceContext*) noexcept = 0;
};
struct D3D11SuperResolutionOrigin {
    std::uint64_t frame{};
    std::uint32_t width{}, height{};
};
void register_d3d11_sr_destinations(std::uintptr_t session, std::uint64_t swapchain,
                                   ID3D11Texture2D* const* images, std::size_t count) noexcept;
void unregister_d3d11_sr_destinations(std::uint64_t swapchain) noexcept;
bool is_d3d11_sr_destination(ID3D11Resource*) noexcept;
// A full copy into an exact XR image proves the application's staging target.
// Resolve to the real enumerated image, never to a guessed same-size texture.
void observe_d3d11_sr_destination_copy(ID3D11Resource* destination, ID3D11Resource* source) noexcept;
HRESULT resolve_d3d11_sr_destination(ID3D11Resource* color_target, ID3D11Resource** actual_image) noexcept;
bool has_d3d11_sr_destinations(ID3D11Device*) noexcept;
std::uint64_t register_d3d11_sr_frame(const std::shared_ptr<D3D11SuperResolutionFrame>&) noexcept;
void retire_d3d11_sr_frame(std::uint64_t) noexcept;
std::shared_ptr<D3D11SuperResolutionFrame> resolve_d3d11_sr_frame(std::uint64_t) noexcept;
bool d3d11_sr_pending() noexcept;
void set_d3d11_sr_origin(ID3D11Resource*, const D3D11SuperResolutionOrigin&) noexcept;
D3D11SuperResolutionOrigin get_d3d11_sr_origin(ID3D11Resource*) noexcept;
void clear_d3d11_sr_origin(ID3D11Resource*) noexcept;
// One actual OpenXR D3D11 device -> its authoritative DX12 bridge owner.
void register_d3d11_guide_owner(std::uintptr_t session, ID3D11Device*, ID3D12Device*, ID3D12CommandQueue*) noexcept;
void unregister_d3d11_guide_owner(std::uintptr_t session) noexcept;
// Luke can render the game and submit OpenXR on distinct D3D11 devices. Capture
// then needs its own bridge on the observed game's device, not an unrelated XR
// session chosen by adapter/size. Keep this lease until its transport retires.
class D3D11GuideCaptureOwner;
HRESULT acquire_d3d11_guide_owner(ID3D11Device*, ID3D12Device**, ID3D12CommandQueue**, std::uint64_t*,
    std::shared_ptr<D3D11GuideCaptureOwner>* retained_capture = nullptr) noexcept;
HRESULT create_d3d11_guide_capture_owner(ID3D11Device*, std::shared_ptr<D3D11GuideCaptureOwner>&) noexcept;
// A source-owned D3D11 capture is distinct from an original game D3D12 queue.
// Admit it for reconstruction only after a submitted, exact-camera publication.
bool admit_d3d11_luke_camera_context(ID3D11Device*,std::uint64_t generation,
    const std::shared_ptr<const DlssMotionVectorFrame>&,std::uint32_t* rejection=nullptr) noexcept;
bool query_d3d11_luke_camera_context(ID3D12Device**,ID3D12CommandQueue**) noexcept;
bool claim_d3d11_guides(ID3D11Device*, std::uint64_t generation) noexcept;
bool d3d11_guides_claimed(ID3D11Device*) noexcept;
struct D3D11GuideReceipt {
    std::uint64_t owner{}, stream{}, publication{};
    std::uint32_t width{}, height{};
};
void set_d3d11_guide_receipt(ID3D11Resource*, const D3D11GuideReceipt&) noexcept;
D3D11GuideReceipt get_d3d11_guide_receipt(ID3D11Resource*) noexcept;
void clear_d3d11_guide_receipt(ID3D11Resource*) noexcept;
// Only a complete, same-size, single-slice copy carries ownership.
bool propagate_d3d11_guide_receipt(ID3D11Resource* destination, ID3D11Resource* source) noexcept;
std::shared_ptr<const DlssMotionVectorSet> resolve_d3d11_dlss_guides(ID3D11Resource*, ID3D12CommandQueue*) noexcept;
// Exact receipt lookup; never select another stream or a later publication.
std::shared_ptr<const DlssMotionVectorFrame> resolve_dlss_guide_publication(
    std::uint64_t stream, std::uint64_t publication, ID3D12CommandQueue*,std::uint32_t* rejection=nullptr) noexcept;
}
