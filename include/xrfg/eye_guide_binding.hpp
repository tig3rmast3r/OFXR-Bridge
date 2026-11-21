#pragma once

#include <cstdint>
#include <optional>

namespace xrfg {

enum class EyeIdentitySource : std::uint8_t { none, metadata, pixel_markers, both };

constexpr EyeIdentitySource operator|(EyeIdentitySource left, EyeIdentitySource right) noexcept {
    return static_cast<EyeIdentitySource>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}

// Producer identity shared by the source image and its captured guides.
// Independent of the guide publication sequence and any camera timing.
struct EyeGuideRenderIdentity {
    std::uint64_t owner{}, render{};
    std::uint32_t producer{};

    [[nodiscard]] bool matches(std::uint32_t producer_id, std::uint64_t stream_id,
        std::uint64_t render_id) const noexcept {
        return owner && producer && producer == producer_id && owner == stream_id && render == render_id;
    }
};

struct EyeGuideKey {
    std::uint64_t compositor_stream{}, producer_owner{}, guide_stream{}, guide_epoch{};
    std::uint32_t producer{};
    bool shared_render_identity{};
    bool operator==(const EyeGuideKey&) const = default;
};

// Readable markers establish the correspondence within one eye, producer
// lifetime and guide epoch. The two sides can name different owners and record
// the same render at adjacent steps of the producer clock.
struct EyeGuideBinding {
    EyeGuideKey key{};
    std::uint64_t last_render{};
    int release_offset{};
    unsigned anchors{};
    bool confirmed{};

    void observe(EyeGuideKey candidate, std::uint64_t render, std::uint64_t guide_render) noexcept {
        const auto distance = render >= guide_render ? render - guide_render : guide_render - render;
        if (!candidate.compositor_stream || !candidate.producer_owner ||
            !candidate.guide_stream || !candidate.guide_epoch || !candidate.producer ||
            distance > (candidate.shared_render_identity ? 1U : 0U)) {
            *this = {};
            return;
        }
        const auto offset = render >= guide_render ? int(distance) : -int(distance);
        if (key != candidate || offset != release_offset) {
            *this = {candidate, render, offset, 1, false};
        } else if (!confirmed && render > last_render) {
            last_render = render;
            const unsigned required = candidate.shared_render_identity ? 4 : 2;
            confirmed = ++anchors >= required;
        }
    }

    [[nodiscard]] bool accepts(std::uint32_t producer, std::uint64_t compositor_stream) const noexcept {
        return confirmed && key.producer == producer && key.compositor_stream == compositor_stream;
    }

    [[nodiscard]] std::optional<std::uint64_t> guide_render(std::uint64_t render) const noexcept {
        if (!confirmed || (release_offset > 0 && render == 0) ||
            (release_offset < 0 && render == UINT64_MAX)) return {};
        if (release_offset > 0) return render - 1;
        if (release_offset < 0) return render + 1;
        return render;
    }
};

// If the producer supplies only its own render ID, a previously decoded
// marker can identify a repeat of that exact render, including in a new slot.
// Keep only an ID and token. The registry still owns and validates the guide.
struct RepeatedEyeGuide {
    std::uint64_t stream{}, render{}, guide_stream{}, guide_epoch{};
    std::uint32_t producer{}, token{};

    [[nodiscard]] std::uint32_t find(std::uint32_t producer_id, std::uint64_t stream_id,
        std::uint64_t render_id) const noexcept {
        return producer == producer_id && stream == stream_id && render == render_id ? token : 0;
    }
};

[[nodiscard]] constexpr bool use_eye_metadata(
    std::uint32_t metadata_token, std::uint32_t marker_token, bool ambiguous_marker,
    bool exact_source_render = false) noexcept {
    return metadata_token && (!ambiguous_marker || exact_source_render) &&
        (!marker_token || marker_token == metadata_token);
}

} // namespace xrfg
