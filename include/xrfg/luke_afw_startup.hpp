#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace xrfg {

struct LukeAfwStartupKey {
    std::uint64_t stream{};
    std::uint64_t epoch{};

    bool operator==(const LukeAfwStartupKey&) const noexcept = default;
};

// A bounded startup rendezvous, not a per-frame pacing mechanism. Only the
// caller's successfully submitted fresh AFW frames are observations. A held
// pair is deliberately not an observation and cannot arm or break the streak.
class LukeAfwStartupRendezvous {
public:
    static constexpr unsigned stable_fresh_submissions = 3;
    static constexpr unsigned maximum_attempts_per_key = 2;

    void observe_fresh(const LukeAfwStartupKey& key,
                       std::uint64_t publication) noexcept {
        if (key.stream == 0 || key.epoch == 0 || publication == 0) {
            observe_failure();
            return;
        }
        if (!key_ || *key_ != key) {
            // A temporal epoch change is a scene/history reset, not another
            // device/route startup. Re-running the drained configuration here
            // reset the metrics and stalled every cut after three fresh frames.
            // A genuinely different producer still qualifies independently.
            const bool completed_stream = key_ && key_->stream == key.stream && completed_;
            key_ = key;
            completed_ = completed_stream;
            attempts_ = 0;
            observe_failure();
        }
        if (completed_ || attempts_ >= maximum_attempts_per_key) return;

        const bool contiguous = last_publication_ != 0 &&
            last_publication_ != std::numeric_limits<std::uint64_t>::max() &&
            publication == last_publication_ + 1;
        fresh_submissions_ = contiguous
            ? (fresh_submissions_ < stable_fresh_submissions
                ? fresh_submissions_ + 1 : stable_fresh_submissions)
            : 1;
        last_publication_ = publication;
    }

    void observe_failure() noexcept {
        fresh_submissions_ = 0;
        last_publication_ = 0;
    }

    [[nodiscard]] std::optional<LukeAfwStartupKey> pending() const noexcept {
        if (key_ && !completed_ && attempts_ < maximum_attempts_per_key &&
            fresh_submissions_ == stable_fresh_submissions) return key_;
        return std::nullopt;
    }

    void complete(const LukeAfwStartupKey& key, bool success) noexcept {
        const auto request = pending();
        if (!request || *request != key) return;
        ++attempts_;
        completed_ = success;
        // A failed attempt needs three new fresh publications before retry.
        // A successful one remains closed even after later missing frames.
        observe_failure();
    }

    // A successful ordinary OFXR On->Off drain can satisfy the same startup
    // rendezvous. The caller supplies the next physically delivered fresh key;
    // no fabricated observations or additional force are required.
    void satisfy(const LukeAfwStartupKey& key) noexcept {
        if (key.stream == 0 || key.epoch == 0) return;
        key_ = key;
        completed_ = true;
        attempts_ = 0;
        observe_failure();
    }

private:
    std::optional<LukeAfwStartupKey> key_;
    std::uint64_t last_publication_{};
    unsigned fresh_submissions_{};
    unsigned attempts_{};
    bool completed_{};
};

} // namespace xrfg
