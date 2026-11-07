#pragma once
#include "xrfg/gpu_usage_status.hpp"
#include "xrfg/cpu_usage_status.hpp"
#include <windows.h>
#include <memory>
#include <span>
#include <string_view>

namespace xrfg {
struct GpuEngineSample {
    std::wstring_view instance;
    double percent{};
    bool valid{};
};
// Sum processes on each physical engine, then select the busiest engine on
// this adapter. Adding different engines would double-count concurrent work.
GpuUsageStatus gpu_busiest_engine(std::span<const GpuEngineSample>,LUID,std::uint64_t now_ms);

// PDH collection is owned by a session worker, never an XR submission thread.
// request() only renews a visibility lease and reads the cached result.
class GpuUsageSampler {
public:
    GpuUsageSampler();
    ~GpuUsageSampler();
    GpuUsageStatus request(LUID) noexcept;
    CpuUsageStatus cpu_usage() noexcept; // same visibility lease and worker
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
