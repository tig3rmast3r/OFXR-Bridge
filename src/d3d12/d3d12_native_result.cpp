#include "xrfg/d3d12_native_result.hpp"
#include "xrfg/bridge_flight_logger.hpp"
namespace xrfg {
std::uint32_t D3D12NativeFrameResult::resolve() noexcept {
    auto result = mask.load(std::memory_order_acquire);
    if (result != pending) return result;
    try {
        std::scoped_lock lock(mutex);
        result = mask.load(std::memory_order_relaxed);
        if (result != pending || !fence || !value || !readback) return result;
        const auto completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) {
            mask.store(0, std::memory_order_release);
            return 0;
        }
        if (completed < value) return pending;
        const D3D12_RANGE range{0, 2 * kMaxOutputs * sizeof(UINT)};
        void* mapped{};
        if (FAILED(readback->Map(0, &range, &mapped))) return pending;
        const auto* flags = static_cast<const UINT*>(mapped);
        unsigned disable_bits{};
        result = 0;
        for (unsigned output=0; output<outputs; ++output) {
            bool allowed = true;
            for (unsigned eye=0; eye<eyes; ++eye) {
                const auto i=eye*kMaxOutputs+output;
                if (flags[i]) { allowed=false; disable_bits |= 1U<<i; }
            }
            if (allowed) result |= 1U<<output;
        }
        const D3D12_RANGE no_write{0,0};
        readback->Unmap(0,&no_write);
        bridge_flight_logger().event(BridgeFlightOperation::native_dlssg_decision,
            result, publication, (std::uint64_t(eyes)<<32)|outputs, disable_bits);
        mask.store(result,std::memory_order_release);
        return result;
    } catch (...) { return pending; }
}
}
