#pragma once

#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace xrfg {

enum class D3D12HistoryInitializationStage : std::uint32_t {
    complete = 0,
    arguments = 1,
    first_source = 2,
    source_description = 3,
    source_validation = 4,
    create_slot_resource = 5,
    create_slot_allocator = 6,
    create_slot_command_list = 7,
    close_slot_command_list = 8,
    create_fence = 9,
    create_event = 10,
};

struct D3D12HistoryCaptureTicket {
    std::uint64_t serial{};
    std::uint64_t fence_value{};
    std::uint32_t slot{};
    std::uint32_t source_index{};
};

struct D3D12HistoryConsumerLease {
    std::uint64_t capture_serial{};
    std::uint64_t lease_serial{};
    std::uint32_t slot{};
};

class D3D12SwapchainHistory final {
public:
    static constexpr std::uint32_t kSlotCount = 3;

    D3D12SwapchainHistory() noexcept;
    ~D3D12SwapchainHistory();

    D3D12SwapchainHistory(const D3D12SwapchainHistory&) = delete;
    D3D12SwapchainHistory& operator=(const D3D12SwapchainHistory&) = delete;

    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        std::span<ID3D12Resource* const> source_images,
        // Native OpenXR resources use their attachment state. Same-adapter
        // D3D11 interop mirrors use COMMON at both API ownership boundaries.
        D3D12_RESOURCE_STATES release_state,
        D3D12HistoryInitializationStage* failure_stage = nullptr) noexcept;

    // Queues the source-to-history copy and returns after signaling its fence;
    // it never waits for GPU completion on the release path. If the next ring
    // slot or its allocator is still in use, ERROR_BUSY is returned and the
    // caller must fail open rather than overwrite it.
    [[nodiscard]] HRESULT capture(
        std::uint32_t source_index,
        D3D12HistoryCaptureTicket* ticket) noexcept;

    [[nodiscard]] HRESULT commit(const D3D12HistoryCaptureTicket& ticket) noexcept;

    void discard(const D3D12HistoryCaptureTicket& ticket) noexcept;

    // Acquires exclusive read access to a committed history slot. The returned
    // COM objects are AddRef'd. The consumer must either retire the lease with
    // its GPU-completion fence or cancel it before submitting any GPU access.
    [[nodiscard]] HRESULT acquire_consumer(
        const D3D12HistoryCaptureTicket& ticket,
        D3D12HistoryConsumerLease* lease,
        ID3D12Resource** resource,
        ID3D12Fence** producer_fence) noexcept;

    [[nodiscard]] HRESULT retire_consumer(
        const D3D12HistoryConsumerLease& lease,
        ID3D12Fence* completion_fence,
        std::uint64_t completion_value) noexcept;

    void cancel_consumer(const D3D12HistoryConsumerLease& lease) noexcept;

    [[nodiscard]] HRESULT wait_for_idle() noexcept;

    [[nodiscard]] HRESULT invalidate() noexcept;

    [[nodiscard]] bool initialized() const noexcept;

private:
    struct Impl;

    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
