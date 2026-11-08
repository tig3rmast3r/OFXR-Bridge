#pragma once
#include "xrfg/fps_overlay_model.hpp"
#include <mutex>
#include <optional>
namespace xrfg {
// Successful native OpenXR content Ends, not producer enqueue or physical scanout.
class RuntimeAcceptanceRate {
    std::mutex mutex_;FpsCounter counter_;
public:
    DeliveryCadenceBatch cadence(std::int64_t now) {std::scoped_lock lock(mutex_);return counter_.cadence(now);}
    void accepted(std::int64_t at) {std::scoped_lock lock(mutex_);counter_.submitted(at,false,true);}
    std::optional<float> rate(std::int64_t now) {std::scoped_lock lock(mutex_);const auto s=counter_.snapshot(now);return s.sample_valid?std::optional<float>(s.submitted_fps):std::nullopt;}
};
}
