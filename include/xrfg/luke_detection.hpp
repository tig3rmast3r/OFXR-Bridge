#pragma once

#include <cstdint>

namespace xrfg::native_ingress {
// Most processes read the last cached discovery result. The two selected
// Luke Vulkan games use the original V666 dynamic recognition path.
bool luke_detected() noexcept;
bool legacy_luke_vulkan_detection() noexcept;

// Startup/session boundary: force a fresh result. The existing hook-monitor
// thread passes false and scans only after a DLL load/unload invalidates it.
void refresh_luke_detection(bool force = true) noexcept;

// Called only after the existing hook monitor has pinned the layer. The loader
// callback does an atomic invalidation only; it performs no discovery or waits.
void start_luke_detection_notifications() noexcept;

#ifdef XRFG_LUKE_DETECTION_TESTS
std::uint64_t luke_detection_scan_count_for_test() noexcept;
bool luke_detection_notifications_ready_for_test() noexcept;
#endif
}
