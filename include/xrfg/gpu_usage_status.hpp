#pragma once
#include <cstdint>

namespace xrfg {
struct GpuUsageStatus {
    std::uint64_t updated_ms{};
    float percent{};
    bool valid{};
};
}
