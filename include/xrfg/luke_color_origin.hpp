#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace xrfg {
// Compact Manchester token: sync16 + token32 + CRC16. Spatial copies share ID.
// Tokens are process-unique; the full publication stays in the guide registry.
inline constexpr unsigned kLukeOriginBits = 64;
inline constexpr unsigned kLukeOriginWidth = kLukeOriginBits * 4;
constexpr std::uint16_t luke_origin_crc(std::uint32_t token) noexcept {
    std::uint16_t crc = 0xffff;
    for (int byte = 3; byte >= 0; --byte) {
        crc ^= static_cast<std::uint16_t>((token >> (byte * 8)) & 255) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}
constexpr bool luke_origin_bit(std::uint32_t token, unsigned bit) noexcept {
    if (bit < 16) return ((0xd3a5U >> (15 - bit)) & 1) != 0;
    if (bit < 48) return ((token >> (47 - bit)) & 1) != 0;
    return ((luke_origin_crc(token) >> (63 - bit)) & 1) != 0;
}
constexpr bool luke_origin_white(std::uint32_t token, unsigned x) noexcept {
    return (x % 4 < 2) == luke_origin_bit(token, x / 4);
}
// Image batches also report the first/last scanned row holding the token.
struct LukeColorOriginDecode { std::uint32_t token{}; bool ambiguous{}; int first_row{-1}, last_row{-1}; };
inline LukeColorOriginDecode decode_luke_origin_row(std::span<const float> row) noexcept {
    try {
        if (row.size() < 96 || row.size() > 16384) return {};
        // The physical R8 row can retain only three quantization steps of
        // marker contrast after overlay dimming. CRC/preamble still govern
        // identity; equal/erased Manchester pairs remain undecodable.
        constexpr float gradient_contrast = .001f;
        constexpr double edge_contrast = .002, bit_contrast = .001;
        struct Edge { double x; bool falling; };
        std::vector<Edge> edges; edges.reserve(row.size());
        for (std::size_t x = 0; x + 1 < row.size();) {
            const float first = row[x + 1] - row[x];
            if (!std::isfinite(first) || std::abs(first) < gradient_contrast) { ++x; continue; }
            const bool falling = first < 0;
            double weight{}, position{};
            do {
                const float difference = row[x + 1] - row[x];
                if (!std::isfinite(difference) || std::abs(difference) < gradient_contrast || (difference < 0) != falling) break;
                const double magnitude = std::abs(difference);
                weight += magnitude; position += (x + .5) * magnitude; ++x;
            } while (x + 1 < row.size());
            if (weight >= edge_contrast) edges.push_back({position / weight, falling});
        }
        const auto sample = [&](double x) {
            if (x < 0 || x > static_cast<double>(row.size() - 1)) return static_cast<double>(NAN);
            const auto a = static_cast<std::size_t>(x), b = std::min(a + 1, row.size() - 1);
            return row[a] + (row[b] - row[a]) * (x - a);
        };
        const auto prefix_matches = [&](double origin, double cell) {
            for (unsigned bit = 0; bit < 16; ++bit) {
                const double difference = sample(origin + (2. * bit - .5) * cell)
                    - sample(origin + (2. * bit + .5) * cell);
                if (!std::isfinite(difference) || std::abs(difference) < bit_contrast ||
                    (difference > 0) != (((0xd3a5U >> (15 - bit)) & 1) != 0)) return false;
            }
            return true;
        };
        const auto fit_edges = [&](double& origin, double& cell, unsigned count, bool known_prefix) {
            double n{}, sx{}, sy{}, sxx{}, sxy{};
            for (unsigned bit = 0; bit < count; ++bit) {
                const double target = origin + 2. * bit * cell;
                double distance = .4 * cell;
                const Edge* best{};
                auto it = std::lower_bound(edges.begin(), edges.end(), target - distance,
                    [](const Edge& edge, double value) { return edge.x < value; });
                for (; it != edges.end() && it->x <= target + distance; ++it) {
                    if (known_prefix && it->falling != (((0xd3a5U >> (15 - bit)) & 1) != 0)) continue;
                    const double error = std::abs(it->x - target);
                    if (error < distance) { distance = error; best = &*it; }
                }
                if (!best) continue;
                const double x = 2. * bit, y = best->x - origin;
                ++n; sx += x; sy += y; sxx += x * x; sxy += x * y;
            }
            // One blended midpoint can disappear into the overlay/background
            // gradient. Fit the remaining sync edges; every decoded sync bit
            // and the complete payload CRC are still required below.
            if (n < (known_prefix ? 15. : 64.)) return false;
            const double pitch = (n * sxy - sx * sy) / (n * sxx - sx * sx);
            if (pitch < .7 || pitch > 4.05) return false;
            origin += (sy - pitch * sx) / n; cell = pitch;
            return true;
        };
        LukeColorOriginDecode result{};
        // Infer continuous pitch from bit0 and either final sync midpoint.
        // Bit14 rises, bit15 falls. Using only bit15 loses a valid marker when
        // that one edge is blended into the changing overlay/background.
        // No rigid scale grid, guessed publication or partial-prefix identity.
        for (std::size_t first = 0; first < edges.size(); ++first) {
            if (!edges[first].falling) continue;
            for (std::size_t last = first + 1; last < edges.size(); ++last) {
                const double length = edges[last].x - edges[first].x;
                if (length > 120.) break;
                if (length < 21.) continue;
                const unsigned anchor = edges[last].falling ? 15 : 14;
                double origin = edges[first].x, cell = length / (2. * anchor);
                // Necessary full-payload extent, NOT another decode tolerance.
                // The existing fits move origin <8px and retain >.94 of this
                // pitch (15/16 prefix edges, then all64 payload edges). Even
                // the looser 100-cell extent cannot fit here: its last sample
                // would be outside the row. Keep every remaining fit/CRC check.
                if (origin + 100. * cell > static_cast<double>(row.size()-1) + 8.) continue;
                if (!prefix_matches(origin, cell) || !fit_edges(origin, cell, 16, true)) continue;
                fit_edges(origin, cell, kLukeOriginBits, false);
                std::uint16_t checksum{}; std::uint32_t token{}; bool valid = true;
                for (unsigned bit = 0; bit < kLukeOriginBits; ++bit) {
                    const double difference = sample(origin + (2. * bit - .5) * cell)
                        - sample(origin + (2. * bit + .5) * cell);
                    if (!std::isfinite(difference) || std::abs(difference) < bit_contrast) { valid = false; break; }
                    const unsigned value = difference > 0 ? 1 : 0;
                    if (bit < 16) {
                        if (value != ((0xd3a5U >> (15 - bit)) & 1)) { valid = false; break; }
                    } else if (bit < 48) token = (token << 1) | value;
                    else checksum = static_cast<std::uint16_t>((checksum << 1) | value);
                }
                if (!valid || !token || checksum != luke_origin_crc(token)) continue;
                if (result.token && result.token != token) result.ambiguous = true;
                result.token = token;
            }
        }
        return result;
    } catch (...) { return {}; }
}
inline unsigned luke_origin_pixel_bytes(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R11G11B10_FLOAT: return 4;
    default: return 0;
    }
}
inline DXGI_FORMAT luke_origin_upload_format(DXGI_FORMAT resource, DXGI_FORMAT proven_view) noexcept {
    if (proven_view == DXGI_FORMAT_UNKNOWN || proven_view == resource) return resource;
    if ((resource == DXGI_FORMAT_R16G16B16A16_TYPELESS && proven_view == DXGI_FORMAT_R16G16B16A16_FLOAT) ||
        (resource == DXGI_FORMAT_R32G32B32A32_TYPELESS && proven_view == DXGI_FORMAT_R32G32B32A32_FLOAT)) return proven_view;
    return DXGI_FORMAT_UNKNOWN;
}
inline void luke_origin_pixel(std::byte* pixel, DXGI_FORMAT format, bool white) noexcept {
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        const auto level = static_cast<std::uint16_t>(white ? 0x3c00 : 0);
        const std::uint16_t values[]{level, level, level, 0x3c00};
        std::memcpy(pixel, values, sizeof(values));
    } else if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        const auto level = static_cast<std::uint16_t>(white ? 0xffff : 0);
        const std::uint16_t values[]{level, level, level, 0xffff};
        std::memcpy(pixel, values, sizeof(values));
    } else if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        // Unsigned 5-bit exponents, 6/6/5-bit mantissas. Exactly 1.0 RGB.
        const std::uint32_t value = white ? (0x3c0U | (0x3c0U << 11) | (0x1e0U << 22)) : 0;
        std::memcpy(pixel, &value, sizeof(value));
    } else if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        const float level = white ? 1.f : 0.f, values[]{level, level, level, 1.f};
        std::memcpy(pixel, values, sizeof(values));
    } else {
        const std::uint32_t value = format == DXGI_FORMAT_R10G10B10A2_UNORM
            ? (white ? 0xffffffffU : 0xc0000000U) : (white ? 0xffffffffU : 0xff000000U);
        std::memcpy(pixel, &value, sizeof(value));
    }
}
inline float luke_origin_luma(const std::byte* pixel, DXGI_FORMAT format) noexcept {
    // Decode all color channels in the stored/encoded domain. Closed color
    // processing can erase blue-only Manchester contrast while RGB retains it.
    // Ignore alpha; keep the existing marker, decoder thresholds and CRC.
    const auto luminance = [](float red, float green, float blue) {
        return .2126f * red + .7152f * green + .0722f * blue;
    };
    if (format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::uint16_t channels[4]{}; std::memcpy(channels, pixel, sizeof(channels));
        const auto half = [](std::uint16_t bits) {
            const int exponent = (bits >> 10) & 31; const unsigned mantissa = bits & 1023;
            if (exponent == 31) return static_cast<float>(NAN);
            const float value = exponent ? std::ldexp(1.f + mantissa / 1024.f, exponent - 15)
                : std::ldexp(static_cast<float>(mantissa), -24);
            return bits & 0x8000 ? -value : value;
        };
        return luminance(half(channels[0]), half(channels[1]), half(channels[2]));
    }
    if (format == DXGI_FORMAT_R32G32B32A32_FLOAT) {
        float channels[4]{}; std::memcpy(channels, pixel, sizeof(channels));
        return luminance(channels[0], channels[1], channels[2]);
    }
    if (format == DXGI_FORMAT_R16G16B16A16_UNORM) {
        std::uint16_t channels[4]{}; std::memcpy(channels, pixel, sizeof(channels));
        return luminance(channels[0] / 65535.f, channels[1] / 65535.f, channels[2] / 65535.f);
    }
    if (format == DXGI_FORMAT_R11G11B10_FLOAT) {
        std::uint32_t value{}; std::memcpy(&value, pixel, sizeof(value));
        const auto channel = [](unsigned bits, unsigned mantissa_bits) {
            const unsigned exponent = bits >> mantissa_bits;
            const unsigned mantissa = bits & ((1U << mantissa_bits) - 1);
            if (exponent == 31) return static_cast<float>(NAN);
            return exponent ? std::ldexp(1.f + std::ldexp(static_cast<float>(mantissa), -int(mantissa_bits)), int(exponent) - 15)
                : std::ldexp(static_cast<float>(mantissa), 1 - 15 - int(mantissa_bits));
        };
        return luminance(channel(value & 0x7ff, 6), channel((value >> 11) & 0x7ff, 6), channel(value >> 22, 5));
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        std::uint32_t value{}; std::memcpy(&value, pixel, sizeof(value));
        return luminance((value & 1023) / 1023.f,
            ((value >> 10) & 1023) / 1023.f, ((value >> 20) & 1023) / 1023.f);
    }
    const auto channel = [&](unsigned index) {
        return std::to_integer<unsigned char>(pixel[index]) / 255.f;
    };
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return luminance(channel(0), channel(1), channel(2));
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return luminance(channel(2), channel(1), channel(0));
    default: return NAN;
    }
}
// Immutable mapped readback descriptions; no GPU work or identity inference.
struct LukeColorOriginImage {
    const std::byte* pixels{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    unsigned width{},height{},row_pitch{};
    bool rgb_fallback{}; // chromatic postprocessing; full sync/payload/CRC only
};
// Optional diagnostic receipt: any finite nonzero RGB luma in this read ROI,
// not a whole-frame visibility or scene-validity decision. CRC is unchanged.
void decode_luke_origin_images(std::span<const LukeColorOriginImage> images,
    std::span<LukeColorOriginDecode> results, std::span<bool> nonblack = {}) noexcept;
// A final color is cropped from a taller producer at an offset fixed by the
// host (Luke's square render to a wider eye: 591 Pimax rows). The scan must
// cover every possible offset, but once a CRC-valid band is found the same
// geometry puts it in the same rows. Decode those rows plus the protected
// translation margin first; any miss, ambiguity or geometry change decodes
// the whole scan in the same read. A conflicting token outside the window is
// seen at the next whole scan, not before: a periodic whole scan to look for
// one cost a 3-6 ms hitch every 32 frames for a case never observed.
inline constexpr unsigned kLukeOriginWindowMargin = 16;
struct LukeOriginRowWindow {
    std::uint64_t geometry{}; // 0: unknown; caller-defined scan identity
    unsigned first{}, last{}; // inclusive scan rows of the last decoded band
};
// Returns one bit per image whose result came from its window alone.
unsigned decode_luke_origin_images_windowed(std::span<const LukeColorOriginImage> images,
    std::span<const std::uint64_t> geometry, std::span<LukeOriginRowWindow> windows,
    std::span<LukeColorOriginDecode> results, std::span<bool> nonblack = {}) noexcept;
struct LukeColorOriginRect { unsigned x{}, y{}, width{}, height{}; };
inline constexpr unsigned kLukeOriginHeight = 8;
inline constexpr unsigned kLukeOriginMargin = 2;
struct LukeColorOriginLayout {
    std::array<LukeColorOriginRect,8> bands{};
    unsigned band_count{};
    LukeColorOriginRect scan;
};
// Cover every possible unscaled horizontal eye crop with complete, disjoint
// copies of ONE token. A narrow common intersection need not contain a marker.
// Geometry selects positions, never an eye, frame or alternate metadata source.
inline bool compact_luke_origin_layout(unsigned output_width, unsigned output_height,
    unsigned color_width, unsigned color_height, LukeColorOriginLayout& layout,
    unsigned inset_x = 0, unsigned inset_y = 0) noexcept {
    layout = {};
    if (!color_width || !color_height || output_width>16384 || output_height>8192 ||
        color_width > output_width || color_height > output_height ||
        inset_x>16384 || inset_y>8192 ||
        color_width<kLukeOriginWidth+2*(kLukeOriginMargin+inset_x)) return false;
    const unsigned horizontal_margin=kLukeOriginMargin+inset_x;
    const unsigned dx = output_width - color_width, dy = output_height - color_height;
    if (static_cast<std::uint64_t>(dy) + kLukeOriginHeight + 2 * kLukeOriginMargin + 2*inset_y > color_height)
        return false;
    LukeColorOriginLayout candidate{};
    const unsigned capacity=color_width-kLukeOriginWidth-2*horizontal_margin;
    unsigned x=std::min(dx,capacity)+horizontal_margin,uncovered=0;
    for(;;) {
        if(candidate.band_count==candidate.bands.size()||
            x+kLukeOriginWidth+horizontal_margin>output_width)return false;
        // A complete band survives crop starts in this inclusive interval.
        const unsigned end=x+kLukeOriginWidth+horizontal_margin;
        const unsigned first=end>color_width?end-color_width:0;
        const unsigned last=x-horizontal_margin;
        if(first>uncovered)return false; // Reject gaps; no inferred crop.
        candidate.bands[candidate.band_count++]={x,dy+kLukeOriginMargin+inset_y,kLukeOriginWidth,kLukeOriginHeight};
        candidate.scan.width=std::min(color_width,end);
        if(last>=dx)break;
        uncovered=last+1;
        // Next placement covers the remaining crop interval without overwriting
        // any Manchester cell in the preceding copy.
        x=std::max(std::min(dx,uncovered+capacity)+horizontal_margin,
            x+kLukeOriginWidth+2*kLukeOriginMargin);
    }
    // Read only the protected band plus bounded final-color translation, not
    // the empty inset/FPS area. Default geometry remains byte-for-byte equal.
    const unsigned translation_x=std::min(inset_x,16U),translation_y=std::min(inset_y,16U);
    candidate.scan.x=inset_x-translation_x;
    candidate.scan.y=inset_y-translation_y;
    candidate.scan.width=std::min(color_width,candidate.bands[candidate.band_count-1].x+
        kLukeOriginWidth+kLukeOriginMargin+translation_x)-candidate.scan.x;
    candidate.scan.height=dy+kLukeOriginHeight+2*kLukeOriginMargin+inset_y+
        translation_y-candidate.scan.y;
    layout=candidate;
    return true;
}
// Geometry negotiation is independent of identity. The first consumer knows
// the actual eye extent before any marker read; SR warmup may precede it.
void configure_luke_color_origin_extent(unsigned color_width, unsigned color_height) noexcept;
// Two immutable black/white cell uploads per device/format/height. No per-frame
// upload allocation, CPU rewrite, producer-submission receipt or 64-frame cap.
HRESULT mark_luke_color_origin(ID3D12GraphicsCommandList* list, ID3D12Resource* color,
    std::uint32_t token, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    ID3D12Device* native_device = nullptr, LukeColorOriginRect rect = {},
    DXGI_FORMAT proven_color_view = DXGI_FORMAT_UNKNOWN,
    LukeColorOriginRect* written_rectangle = nullptr) noexcept;
// Reads only the compact corner ROI in one GPU submission/fence/map. Every
// row is independently CRC-checked; distinct valid tokens reject the batch.
// Missing/ambiguous/expired marker is an error; no selected-metadata fallback.
HRESULT read_luke_color_origin(ID3D12CommandQueue* queue, ID3D12Resource* color,
    std::uint32_t* token, D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
    DWORD timeout_ms = 250) noexcept;
// Two independently decoded colors in one submission/fence/map. The return
// value covers shared transport; per-eye results retain missing/ambiguous errors.
struct LukeColorOriginReadReceipt {
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t value{};
    bool submitted{};
};
HRESULT read_luke_color_origin_pair(ID3D12CommandQueue* queue,
    const std::array<ID3D12Resource*, 2>& colors,
    std::array<std::uint32_t, 2>& tokens, std::array<HRESULT, 2>& results,
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON,
    DWORD timeout_ms = 250, std::array<bool,2>* nonblack = nullptr,
    LukeColorOriginReadReceipt* receipt = nullptr) noexcept;
} // namespace xrfg
