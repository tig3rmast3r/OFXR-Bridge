#pragma once
#include <cstdint>

namespace xrfg {
struct CpuUsageStatus {
    std::uint64_t updated_ms{};
    float percent{};
    bool valid{};
};
}
