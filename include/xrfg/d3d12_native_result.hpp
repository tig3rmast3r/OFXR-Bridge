#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <span>
#include <memory>

namespace xrfg {
// An immutable pair's decision survives work-slot reuse and route changes.
// Only completed GPU readback is counted as interpolation. Never CPU-wait.
struct D3D12NativeFrameResult {
    static constexpr std::uint32_t pending = UINT32_MAX;
    // Generated outputs a pair can carry (4X), and so the readback's stride:
    // its interpolation-disable flags are eye * kMaxOutputs + output.
    static constexpr std::uint32_t kMaxOutputs = 3;
    std::atomic_uint32_t mask{pending};
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t value{}, publication{};
    std::uint32_t eyes{}, outputs{}, skip{}, detail{};
    std::uint32_t provider_error{}; // Actual NGX result; zero means no recorded refusal.
    std::mutex mutex;
    [[nodiscard]] std::uint32_t resolve() noexcept;
    [[nodiscard]] bool generated(unsigned output=0) noexcept {
        const auto result = resolve();
        return result != pending && output < outputs && (result & (1U << output));
    }
};
struct NativeFrameContentMetrics { bool synthetic{}, new_content{}, native_bypass{}; };
inline NativeFrameContentMetrics classify_native_frame_content(
    std::span<const std::shared_ptr<D3D12NativeFrameResult>> results,
    unsigned output, bool synthetic_slot, bool fresh) noexcept {
    bool interpolated=synthetic_slot, bypass=false;
    for(const auto& result:results) {
        const bool generated=result && result->generated(output);
        bypass |= !generated;
        interpolated &= generated;
    }
    return {interpolated && fresh, fresh && (!synthetic_slot || interpolated), bypass};
}
} // namespace xrfg
