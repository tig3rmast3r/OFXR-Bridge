#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace xrfg {

struct TwoFrameImageInfo;

enum class D3D11InteropInitializationStage : std::uint32_t {
    complete = 0,
    arguments = 1,
    context_device = 2,
    adapter_luid = 3,
    source_description = 4,
    source_validation = 5,
    current_validation = 6,
    synthetic_validation = 7,
    device5 = 8,
    context4 = 9,
    source_create_d3d12 = 10,
    source_create_handle = 11,
    source_device1 = 12,
    source_open_d3d11 = 13,
    current_create_d3d12 = 14,
    current_create_handle = 15,
    current_device1 = 16,
    current_open_d3d11 = 17,
    synthetic_create_d3d12 = 18,
    synthetic_create_handle = 19,
    synthetic_device1 = 20,
    synthetic_open_d3d11 = 21,
    create_fence = 22,
    create_fence_handle = 23,
    open_d3d11_fence = 24,
    create_event = 25,
};

// Last queued publication plus a non-blocking completed-fence observation.
struct D3D11PublicationSnapshot {
    std::uint64_t fence_identity{};
    std::uint64_t ready_value{};
    std::uint64_t copy_complete_value{};
    std::uint64_t completed_value{};
    std::uint32_t current_index{UINT32_MAX};
    std::uint32_t synthetic_index{UINT32_MAX};
};

// Immutable completion target returned by the publish transaction itself.
// Retains the fence, not the interop mutex or its mutable last-access cursor.
struct D3D11PublicationReceipt {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t copy_complete_value{};
    std::uint32_t current_destination_index{UINT32_MAX};
    std::uint32_t private_current_index{UINT32_MAX};
    std::uint32_t synthetic_destination_index{UINT32_MAX};
};

struct D3D11PublicationTargets {
    std::uint32_t current_index{};
    std::optional<std::uint32_t> synthetic_index;
};

[[nodiscard]] HRESULT wait_for_d3d11_publication(
    const D3D11PublicationReceipt& receipt) noexcept;

// Creates a private D3D12 device and direct queue on the adapter that owns the
// supplied D3D11 OpenXR device. The caller retains both returned objects.
[[nodiscard]] HRESULT create_d3d12_device_for_d3d11(
    ID3D11Device* d3d11_device,
    ID3D12Device** d3d12_device,
    ID3D12CommandQueue** d3d12_queue) noexcept;

// Owns the same-adapter resources and shared timeline fence used to move the
// released mip-zero OpenXR image from D3D11 into the existing D3D12 history and
// to publish current/synthetic D3D12 output back into private D3D11 OpenXR
// swapchains. Every transfer is GPU ordered; per-frame calls do not CPU-wait.
class D3D11D3D12SwapchainInterop final {
public:
    D3D11D3D12SwapchainInterop() noexcept;
    ~D3D11D3D12SwapchainInterop();

    D3D11D3D12SwapchainInterop(const D3D11D3D12SwapchainInterop&) = delete;
    D3D11D3D12SwapchainInterop& operator=(
        const D3D11D3D12SwapchainInterop&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D11Device* d3d11_device,
        ID3D11DeviceContext* d3d11_context,
        ID3D12Device* d3d12_device,
        ID3D12CommandQueue* d3d12_queue,
        std::span<ID3D11Texture2D* const> source_images,
        std::span<ID3D11Texture2D* const> current_destination_images,
        std::span<ID3D11Texture2D* const> synthetic_destination_images,
        D3D11InteropInitializationStage* failure_stage = nullptr) noexcept;

    [[nodiscard]] std::span<ID3D12Resource* const> source_images() const noexcept;
    [[nodiscard]] std::span<ID3D12Resource* const>
    current_destination_images() const noexcept;

    // Extend the current publication bank with one idle output. An appended
    // entry may be removed before it participates in any submission.
    [[nodiscard]] HRESULT append_current_destination(ID3D11Texture2D* image,
        Microsoft::WRL::ComPtr<ID3D12Resource>* backing) noexcept;
    void discard_unsubmitted_current_destination(ID3D11Texture2D* image) noexcept;
    [[nodiscard]] std::span<ID3D12Resource* const>
    synthetic_destination_images() const noexcept;

    // Must be called while the application still owns the released source
    // image. Queues D3D11 mip-zero copies and makes the D3D12 queue wait for
    // them before any history access.
    [[nodiscard]] HRESULT prepare_capture(std::uint32_t source_index) noexcept;

    // Copy an exact retained endpoint into an existing source slot. This does
    // not change history or the registered application images; callers keep
    // the supplied texture alive through finish_capture completion.
    [[nodiscard]] HRESULT prepare_capture_image(std::uint32_t source_index,
        ID3D11Texture2D* image) noexcept;

    // Marks the shared source safe for the next D3D11 write after the history
    // copy submitted immediately before this call completes on the D3D12 queue.
    [[nodiscard]] HRESULT finish_capture() noexcept;

    // Makes the D3D12 queue wait for any earlier D3D11 publication copy before
    // the synthesizer can reuse a shared destination.
    // Optional OFXR lane. Capture/AFW retain their original queue and timeline.
    // Configure before use; failure leaves the original lane intact.
    [[nodiscard]] HRESULT configure_synthesis_queue(ID3D12CommandQueue* queue) noexcept;
    [[nodiscard]] HRESULT prepare_synthesis(bool isolated = false) noexcept;

    // Isolated worker with an independently fenced immutable input history.
    // Wait only for D11 accesses to the exact output slots being overwritten.
    // Raw/mutable source callers must keep prepare_synthesis instead.
    [[nodiscard]] HRESULT prepare_synthesis_targets(std::uint32_t current_index,
        std::optional<std::uint32_t> synthetic_index) noexcept;

    // Read the actual acquired OpenXR D3D11 output named by this immutable
    // publication receipt back into its shared D3D12 mirror. Call before XR
    // release, then capture history and finish_capture. GPU ordering only.
    [[nodiscard]] HRESULT capture_published_current(
        const D3D11PublicationReceipt& receipt) noexcept;

    // F8-only witness of the actual acquired XR image, paired to its immutable
    // receipt. Does not consume or advance the temporal synthesis history.
    [[nodiscard]] HRESULT diagnostic_capture_published_current(
        const D3D11PublicationReceipt& receipt,
        const TwoFrameImageInfo& info) noexcept;

    // Called immediately after the synthesizer submission on the same D3D12
    // queue. Queues a D3D11 wait and mip-zero copies into the acquired private
    // OpenXR images.
    [[nodiscard]] HRESULT publish(
        std::uint32_t current_destination_index,
        std::optional<std::uint32_t> synthetic_destination_index,
        D3D11PublicationReceipt* receipt = nullptr,
        std::optional<D3D11PublicationTargets> targets = {},
        bool isolated = false) noexcept;

    // Output already completed on its retained synthesis fence. Reject an
    // incomplete proof; never append a ready signal behind newer native work.
    // The private endpoint may opt into a direct owned-output receipt. It must
    // hold acquired output ownership, then transfer the exact receipt to its
    // image reader leases. Remapped/non-owned outputs retain the D11 copy.
    [[nodiscard]] HRESULT publish_completed(
        std::uint32_t current_destination_index,
        std::optional<std::uint32_t> synthetic_destination_index,
        ID3D12Fence* producer_fence, std::uint64_t producer_value,
        D3D11PublicationReceipt* receipt = nullptr,
        std::optional<D3D11PublicationTargets> targets = {},
        bool owned_completion_receipt = false) noexcept;


    [[nodiscard]] HRESULT wait_for_idle() noexcept;
    // Non-blocking observation only: never wait, flush or submit GPU work.
    [[nodiscard]] D3D11PublicationSnapshot publication_snapshot() const noexcept;
    // Diagnostic-only retained reference; no wait, copy or GPU submission.
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D>
        published_current_image(std::uint32_t index) const noexcept;
    [[nodiscard]] Microsoft::WRL::ComPtr<ID3D11Texture2D>
        published_image(std::uint32_t index,bool synthetic) const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;

    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
