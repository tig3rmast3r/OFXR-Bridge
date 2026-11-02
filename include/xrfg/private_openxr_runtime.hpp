#pragma once

#include <memory>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xrfg/private_output_distributor.hpp"

namespace xrfg {
struct InterfaceDeliveryReceipt;
// Downstream functions are the REAL runtime, never the layer intercepts.
struct PrivateRuntimeDispatch {
    XrInstance instance{};
    XrSession session{};
    PFN_xrWaitFrame wait{};
    PFN_xrBeginFrame begin{};
    PFN_xrEndFrame end{};
    PFN_xrCreateSwapchain create{};
    PFN_xrDestroySwapchain destroy{};
    PFN_xrEnumerateSwapchainImages enumerate{};
    PFN_xrAcquireSwapchainImage acquire{};
    PFN_xrWaitSwapchainImage wait_image{};
    PFN_xrReleaseSwapchainImage release{};
    PFN_xrConvertTimeToWin32PerformanceCounterKHR to_qpc{};
};

// SteamVR Begin/End and image release belong to the consumer; one independent
// Wait worker prepares the next runtime clock. All application/generator
// images are private, with leases (not copies) transferred at virtual EndFrame.
// Source read leases end only after the copy GPU fence completes. Real target
// ownership and FIFO credits remain until native End; source generations prevent
// a rewritten private image from reusing an older prepared copy.
// A preparer owns real acquisition/copy/GPU waits; the physical frame thread
// consumes completed packets only and releases that FIFO head's real targets.
// The next image may be acquired/waited/copied while an older RELEASED image
// is presented, but stays unreleased until its own EndFrame can capture it.
// This retains last-released image semantics without additional XR swapchains.
// Native D3D11 and D3D12 are supported; existing Vulkan/D3D11 translation binds
// D3D12 below this endpoint and therefore uses the identical D3D12 transport.
class PrivateOpenXrRuntime final {
public:
    struct Options {
        PrivateRuntimeDispatch real;
        ID3D12Device* device12{};
        ID3D12CommandQueue* queue12{}; // the actual runtime binding queue
        ID3D12CommandQueue* producer_queue12{}; // private writes, never future work on queue12
        ID3D11Device* device11{};
        ID3D11DeviceContext* context11{};
        std::size_t capacity{2};
        std::function<std::int64_t()> physical_period;
        // Cached physical vsync only. Must not call a runtime, lock an observer
        // or wait from the consumer. Empty until the session clock is known.
        std::function<std::optional<PrivatePhysicalClock>()> physical_clock;
        std::function<bool()> trace_enabled;
        // stage: 1 accepted, 2 accepted by real runtime, -1 failed; sequence,
        // QPC ns and accumulated phase displacement (not a latency meter).
        // Stage3 instead carries submitted and raw predicted XR displayTime.
        // Stages4/5: preparer source GPU ready / real image copy complete,
        // with the real target still waited/unreleased until FIFO delivery.
        // Stages7/6: saved real Wait entry/return (overlaps preparation, emitted
        // when the ready packet is known). Stage8: saved handoff edge
        // after the residual wait/copy, emitted after real End to keep the edge
        // free of diagnostic work. QPC ns, c=0. Stage3 runs in the early window.
        // Stage10: input readiness before the real-chain preparation gate;
        // c=1 fence completion observed, c=0 acceptance lower bound when
        // already complete at dequeue. Stage11: underrun refill, QPC ns and
        // exact margin retained after the late frame for subsequent deliveries.
        // A real-chain backlog never triggers a refill.
        // Stages12/13/14: targets acquired/waited, copy submitted/signalled,
        // and copy-fence completion observed. F9-only CPU QPC observations,
        // not GPU execution timestamps. Real release follows on the consumer,
        // after stage5 and before this packet's native EndFrame.
        // Stages15/16: release entry QPC/duration, Begin-complete QPC/next
        // native-entry budget. Optional CPU observations, never GPU queries.
        // Stage17: exact native-margin deficit retained in the output phase.
        // Stage18: native Wait entry QPC and worker thread ID; a is the
        // per-start accepted-input ordinal (0 for the seed), not packet ID.
        // V593: ordinal0 also identifies unsubmitted native-clock renewals.
        // Stage23: packet ID, expired raw prediction converted to QPC, planned
        // handoff QPC. Stage24: final raw prediction QPC and renewed-cycle count.
        // Renewals keep the same owned packet; only the old native Begin is
        // discarded through OpenXR's XR_FRAME_DISCARDED transition, without End.
        // Stage25: one preventive native-window renewal, raw prediction QPC
        // and required display-edge+guard coverage (V651). This is bounded to one
        // attempt per packet, separate from expired-cycle recovery. A still-current
        // replacement may retain a shorter horizon; never spin for that preference.
        // Stage27: allocation-only publication mapping; source handle in a,
        // virtual output handle in b and native sink in c (not QPC times).
        // Stage26: source read leases released after completed copy; packet ID,
        // QPC ns and lease count. This does not retire real targets or FIFO credits.
        std::function<void(int,std::uint64_t,std::int64_t,std::int64_t)> trace;
        std::function<void(std::int64_t)> accepted_output; // optional native End success, never enqueue
        bool director_enabled{true}; // false: ordered native-runtime paced bypass
        // Luke D11/SteamVR only. Freeze image readiness at Release, not after
        // newer game commands at End. Admission/work budget is selected separately.
        bool d11_release_readiness{};
    };
    explicit PrivateOpenXrRuntime(Options);
    ~PrivateOpenXrRuntime();
    PrivateOpenXrRuntime(const PrivateOpenXrRuntime&)=delete;
    PrivateOpenXrRuntime& operator=(const PrivateOpenXrRuntime&)=delete;
    HRESULT initialize();
    XrResult start(); // after successful real xrBeginSession
    void stop(); // wakes producers; consumer closes/joins native Wait; drains GPU reads
    XrResult drain();
    // Drain accepted packets and change only final delivery policy. Session,
    // native binding, private images and worker ownership remain unchanged.
    XrResult set_director_enabled(bool);
    XrResult set_buffer_capacity(std::size_t);
    [[nodiscard]] std::size_t buffer_capacity() const noexcept;
    [[nodiscard]] bool director_enabled() const noexcept;
    XrResult wait_frame(const XrFrameWaitInfo*, XrFrameState*);
    XrResult predict_frame(XrFrameState*); // upper virtual application clock
    XrResult begin_frame(const XrFrameBeginInfo*);
    XrResult end_frame(const XrFrameEndInfo*,
        const std::shared_ptr<InterfaceDeliveryReceipt>& delivery={});
    XrResult create_swapchain(const XrSwapchainCreateInfo*, XrSwapchain*,
        D3D12_RESOURCE_STATES image_state=D3D12_RESOURCE_STATE_RENDER_TARGET,
        std::uint32_t image_count=3,
        // Internal generated outputs only: compatible rings of one source eye
        // share a native sink; the producer handle/images remain independent.
        XrSwapchain publication_source=XR_NULL_HANDLE,
        // Internal generated D11 outputs only. A confirmed owner can supply
        // one shared backing for D11 publication and D12 synthesis; the native
        // XR sink and every other output still retain independent allocations.
        ID3D12Device* d11_output_owner=nullptr);
    XrResult destroy_swapchain(XrSwapchain);
    XrResult enumerate_images(XrSwapchain,std::uint32_t,std::uint32_t*,XrSwapchainImageBaseHeader*);
    XrResult acquire_image(XrSwapchain,const XrSwapchainImageAcquireInfo*,std::uint32_t*);
    XrResult wait_image(XrSwapchain,const XrSwapchainImageWaitInfo*);
    XrResult release_image(XrSwapchain,const XrSwapchainImageReleaseInfo*);
    // Completion proof for the EXACT acquired output; never a guessed serial.
    // Caller already CPU-observed this publication's fence. Unproven/foreign
    // images still go through release_image and its original-context marker.
    XrResult release_completed_image(XrSwapchain,const XrSwapchainImageReleaseInfo*,
        std::uint32_t index,ID3D11Texture2D* published_image,ID3D12Fence*,std::uint64_t value);
    [[nodiscard]] bool owns(XrSwapchain) const;
    struct SwapchainMapping { XrSwapchain handle{}, native{}; };
    // CPU-only inventory; busy transport returns no partial snapshot.
    [[nodiscard]] std::optional<std::vector<SwapchainMapping>> native_swapchain_mapping() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xrfg
