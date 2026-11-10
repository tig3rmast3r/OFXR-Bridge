#pragma once
#include <atomic>
#include <cstdint>

namespace xrfg {
struct AerFramegenEvidenceSnapshot {
    std::uint64_t outputs{}, bypasses{}, unknown{}, last_output_ms{}, last_bypass_ms{};
    unsigned delivery{};
};
// Observer only. Each applied generator configuration owns a different object;
// delayed receipts can update their old object without activating the new mode.
// No GPU work, waits, resource ownership or scheduler decisions belong here.
struct AerFramegenEvidence {
    std::atomic_uint64_t outputs{}, bypasses{}, unknown{}, last_output_ms{}, last_bypass_ms{};
    std::atomic_uint delivery{};
    void confirmed(std::uint64_t now, unsigned source_delivery) noexcept {
        delivery.store(source_delivery, std::memory_order_relaxed);
        outputs.fetch_add(1, std::memory_order_relaxed);
        last_output_ms.store(now, std::memory_order_release);
    }
    void bypassed(std::uint64_t now) noexcept {
        bypasses.fetch_add(1, std::memory_order_relaxed);
        last_bypass_ms.store(now, std::memory_order_release);
    }
    AerFramegenEvidenceSnapshot snapshot() const noexcept {
        return {outputs.load(), bypasses.load(), unknown.load(),
            last_output_ms.load(), last_bypass_ms.load(), delivery.load()};
    }
};
} // namespace xrfg
