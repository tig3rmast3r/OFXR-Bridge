#pragma once
#include "xrfg/d3d12_frame_synthesizer.hpp"

namespace xrfg {
struct RifeMemoryStatus {
    std::uint64_t updated_ms{}, private_bytes{}, reserved_bytes{};
    std::uint32_t process_id{};
    bool active{}, valid{};
};
// Nonblocking cached telemetry; requests are serviced by the next inference.
RifeMemoryStatus rife_memory_for_adapter(LUID adapter,bool request) noexcept;
struct RifeExecutionStatus {
    std::uint64_t updated_ms{};
    bool active{}, optimizing{}, tensorrt{};
    unsigned error{}; // 0 none, 1 timeout, 2 unavailable, 3 other optimization failure
};
// CPU-only cached worker receipts, independent of logging and memory sampling.
RifeExecutionStatus rife_execution_status() noexcept;
// Experimental portable-Python generator. Owns no XR objects or pacing policy.
// The caller queues both producer receipts before generate(). The only CPU
// synchronization is with this worker's exact GPU transactions, never a game tail.
class D3D12Rife {
public:
    // queue_ordered is only for callers publishing an exact completion fence on
    // this same queue. Inputs/outputs remain retained until GPU retirement.
    enum class Completion { wait, queue_ordered };
    struct Timing {ID3D12QueryHeap* queries{};ID3D12Resource* readback{};UINT base{};bool first{true},last{true};ID3D12QueryHeap* input_queries{};};
    D3D12Rife();
    ~D3D12Rife();
    HRESULT initialize(ID3D12Device*, ID3D12CommandQueue*, DXGI_FORMAT, int model, float scale) noexcept;
    HRESULT generate(ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* output,
        std::span<const D3D12ReprojectionView> av, std::span<const D3D12ReprojectionView> bv,
        std::span<const D3D12ReprojectionView> target,
        std::span<const std::array<float,4>> a_rotations,
        std::span<const std::array<float,4>> b_rotations,
        D3D12_RESOURCE_STATES output_state, float fraction=.5F, Timing timing={},
        Completion completion=Completion::wait) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
