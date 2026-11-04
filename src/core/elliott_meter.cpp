#include "xrfg/presented_output_rate.hpp"
#include "xrfg/elliott_meter.hpp"
#include "xrfg/benchmark_suite.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <utility>
namespace xrfg {
namespace {
#include "xrfg/third_party/font8x8_basic.h"
}
namespace {

[[nodiscard]] Quaternion multiply(const Quaternion& a, const Quaternion& b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] Vec3 rotate(const Quaternion& q, const Vec3& v) noexcept {
    // v + 2 q.xyz x (q.xyz x v + w v)
    const Vec3 c{q.y * v.z - q.z * v.y + q.w * v.x,
                 q.z * v.x - q.x * v.z + q.w * v.y,
                 q.x * v.y - q.y * v.x + q.w * v.z};
    return {v.x + 2.0F * (q.y * c.z - q.z * c.y),
            v.y + 2.0F * (q.z * c.x - q.x * c.z),
            v.z + 2.0F * (q.x * c.y - q.y * c.x)};
}

}  // namespace

bool grip_upside_down(const Quaternion& grip, float angle_degrees) noexcept {
    const Quaternion unit = normalize(grip);
    // The degrees the user thinks in become the cosine the up axis is
    // compared with: past 120 degrees, its height is below -0.5.
    const float limit = std::cos(std::clamp(angle_degrees, 45.0F, 179.0F) *
                                 3.14159265358979323846F / 180.0F);
    return rotate(unit, {0.0F, 1.0F, 0.0F}).y < limit;
}

bool FlipGesture::observe(std::int64_t now_ns, bool left_flipped, bool right_flipped) noexcept {
    if (left_flipped || right_flipped) {
        upright_since_ = -1;
        // The panel stays on the hand that brought it up while that hand is
        // still turned; the other hand only takes it once that one is not.
        const bool kept = (hand_ == 0 && left_flipped) || (hand_ == 1 && right_flipped);
        if (!kept) hand_ = left_flipped ? 0 : 1;
        if (flipped_since_ < 0) {
            flipped_since_ = now_ns;
        } else if (!visible_ && now_ns - flipped_since_ >= kFlipShowDelayNs) {
            visible_ = true;
        }
    } else {
        flipped_since_ = -1;
        if (visible_) {
            if (upright_since_ < 0) {
                upright_since_ = now_ns;
            } else if (now_ns - upright_since_ >= kFlipHideDelayNs) {
                visible_ = false;
                upright_since_ = -1;
                hand_ = -1;
            }
        } else {
            hand_ = -1;
        }
    }
    return visible_;
}

Pose panel_pose_on_grip(const Pose& grip, float panel_height_m) noexcept {
    // Half a turn about the grip's Z, the controller's length. Upside down,
    // that turns the panel back upright, facing along +Z: back at the user.
    constexpr Quaternion kHalfTurnZ{0.0F, 0.0F, 1.0F, 0.0F};
    const Quaternion orientation = normalize(grip.orientation);
    // Along the grip's -Y, which is up while the controller is upside down:
    // xrFPS's 11 cm for its own small panel, here half this one's height and
    // a 5 cm margin, so its lower edge clears the controller.
    const float lift = std::max(panel_height_m, 0.0F) * 0.5F + 0.05F;
    const Vec3 offset = rotate(orientation, {0.0F, -lift, 0.0F});
    return {normalize(multiply(orientation, kHalfTurnZ)),
            {grip.position.x + offset.x, grip.position.y + offset.y, grip.position.z + offset.z}};
}

namespace {

constexpr int kGlyph = 8;

// Glyph masks at 1, 2 and 4 times the font's size. The larger two are grown
// by Scale2x (EPX): each pixel splits into four, and a corner takes the
// colour of the two neighbours that meet there when they agree. Diagonals
// come out as steps of the new size rather than blocks of the old, which
// reads markedly better at the size the panel is seen at than plain
// doubling, and it is all done once, here.
struct GlyphSet {
    int size{};
    std::vector<std::uint8_t> masks; // 128 glyphs, size x size each
};

[[nodiscard]] std::vector<std::uint8_t> scale2x(const std::vector<std::uint8_t>& source, int size) {
    std::vector<std::uint8_t> output(static_cast<std::size_t>(size) * size * 4);
    const auto at = [&](int x, int y) -> std::uint8_t {
        return x < 0 || y < 0 || x >= size || y >= size ? 0 : source[static_cast<std::size_t>(y) * size + x];
    };
    const int out_size = size * 2;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::uint8_t p = at(x, y), a = at(x, y - 1), b = at(x + 1, y),
                               c = at(x - 1, y), d = at(x, y + 1);
            std::uint8_t e0 = p, e1 = p, e2 = p, e3 = p;
            if (c == a && c != d && a != b) e0 = a;
            if (a == b && a != c && b != d) e1 = b;
            if (d == c && d != b && c != a) e2 = c;
            if (b == d && b != a && d != c) e3 = d;
            const std::size_t top = static_cast<std::size_t>(y * 2) * out_size + x * 2;
            output[top] = e0;
            output[top + 1] = e1;
            output[top + out_size] = e2;
            output[top + out_size + 1] = e3;
        }
    }
    return output;
}

[[nodiscard]] const GlyphSet& glyphs(int scale) {
    static std::array<GlyphSet, 3> sets;
    static std::once_flag once;
    std::call_once(once, [] {
        for (std::size_t level = 0; level < sets.size(); ++level) {
            GlyphSet& set = sets[level];
            set.size = kGlyph << level;
            set.masks.resize(static_cast<std::size_t>(set.size) * set.size * 128);
        }
        for (int code = 0; code < 128; ++code) {
            std::vector<std::uint8_t> mask(kGlyph * kGlyph);
            for (int y = 0; y < kGlyph; ++y)
                for (int x = 0; x < kGlyph; ++x)
                    mask[static_cast<std::size_t>(y) * kGlyph + x] =
                        (font8x8_basic[code][y] >> x) & 1U;
            int size = kGlyph;
            for (std::size_t level = 0; level < sets.size(); ++level) {
                if (level > 0) {
                    mask = scale2x(mask, size);
                    size *= 2;
                }
                std::copy(mask.begin(), mask.end(),
                          sets[level].masks.begin() + static_cast<std::ptrdiff_t>(code) * size * size);
            }
        }
    });
    return sets[scale >= 4 ? 2 : scale >= 2 ? 1 : 0];
}

struct Rgba {
    std::uint8_t r, g, b, a;
};

// Colours as they should look, in sRGB.
constexpr Rgba kBackground{14, 18, 28, 232};
constexpr Rgba kBorder{64, 74, 98, 255};
constexpr Rgba kRule{48, 56, 76, 255};
constexpr Rgba kTones[]{
    {238, 241, 247, 255}, // normal
    {148, 158, 178, 255}, // dim
    {92, 224, 142, 255},  // good
    {255, 192, 72, 255},  // warn
    {255, 98, 98, 255},   // bad
    {112, 202, 255, 255}, // accent
};

class Canvas {
public:
    Canvas(std::uint32_t width, std::uint32_t height, bool bgra, bool linear)
        : width_(static_cast<int>(width)), height_(static_cast<int>(height)), bgra_(bgra),
          linear_(linear), pixels_(static_cast<std::size_t>(width) * height, 0) {}

    [[nodiscard]] std::uint32_t pack(Rgba color, float coverage = 1.0F) const noexcept {
        const auto channel = [&](std::uint8_t value) {
            float c = static_cast<float>(value) / 255.0F;
            if (linear_) c = c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
            return c;
        };
        const float alpha = static_cast<float>(color.a) / 255.0F * std::clamp(coverage, 0.0F, 1.0F);
        const auto byte = [](float value) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
        };
        // Premultiplied: the quad blends source alpha without the
        // unpremultiplied flag, as the counter's does.
        const std::uint32_t r = byte(channel(color.r) * alpha);
        const std::uint32_t g = byte(channel(color.g) * alpha);
        const std::uint32_t b = byte(channel(color.b) * alpha);
        const std::uint32_t a = byte(alpha);
        return (a << 24) | (bgra_ ? b | (g << 8) | (r << 16) : r | (g << 8) | (b << 16));
    }

    // A rounded rectangle over the top `used` rows of the canvas with a thin
    // border, its corners softened by coverage so the edge does not
    // stair-step; the rows below stay transparent. Only the corners need the
    // arithmetic, which matters on the game's render thread.
    void panel(int used, int radius, int border) {
        used = std::clamp(used, 2 * radius + 2, height_);
        const std::uint32_t fill = pack(kBackground);
        const std::uint32_t edge = pack(kBorder);
        std::fill(pixels_.begin(), pixels_.begin() + static_cast<std::ptrdiff_t>(used) * width_, fill);
        for (int y = 0; y < used; ++y) {
            const auto row = pixels_.begin() + static_cast<std::ptrdiff_t>(y) * width_;
            if (y < border || y >= used - border) {
                std::fill(row, row + width_, edge);
            } else {
                std::fill(row, row + border, edge);
                std::fill(row + width_ - border, row + width_, edge);
            }
        }
        for (int y = 0; y <= radius; ++y) {
            for (int x = 0; x <= radius; ++x) {
                const float dx = static_cast<float>(radius - x) - 0.5F;
                const float dy = static_cast<float>(radius - y) - 0.5F;
                const float outside = std::sqrt(std::max(dx, 0.0F) * std::max(dx, 0.0F) +
                                                std::max(dy, 0.0F) * std::max(dy, 0.0F)) -
                    static_cast<float>(radius);
                const float coverage = std::clamp(0.5F - outside, 0.0F, 1.0F);
                const bool on_edge = outside > -static_cast<float>(border);
                const std::uint32_t value = coverage <= 0.0F ? 0U
                    : coverage < 1.0F                        ? pack(on_edge ? kBorder : kBackground, coverage)
                    : on_edge                                ? edge
                                                             : fill;
                for (const auto& [cx, cy] : {std::pair{x, y}, std::pair{width_ - 1 - x, y},
                                             std::pair{x, used - 1 - y},
                                             std::pair{width_ - 1 - x, used - 1 - y}}) {
                    pixels_[static_cast<std::size_t>(cy) * width_ + cx] = value;
                }
            }
        }
    }

    void rule(int x0, int x1, int y, int thickness) {
        const std::uint32_t color = pack(kRule);
        for (int row = y; row < y + thickness && row < height_; ++row)
            for (int x = std::max(x0, 0); x < std::min(x1, width_); ++x)
                pixels_[static_cast<std::size_t>(row) * width_ + x] = color;
    }

    // Draws ASCII text with its top-left corner at (x, y), clipped to the
    // canvas; anything outside printable ASCII draws as '?'. Returns where
    // the next character would go.
    int text(int x, int y, std::string_view value, int scale, PanelTone tone) {
        const GlyphSet& set = glyphs(scale);
        const std::uint32_t color = pack(kTones[static_cast<int>(tone)]);
        for (char raw : value) {
            const auto code = static_cast<unsigned char>(raw);
            const int glyph = code >= 32 && code < 127 ? code : '?';
            const std::uint8_t* mask =
                set.masks.data() + static_cast<std::size_t>(glyph) * set.size * set.size;
            for (int row = 0; row < set.size; ++row) {
                const int py = y + row;
                if (py < 0 || py >= height_) continue;
                for (int column = 0; column < set.size; ++column) {
                    const int px = x + column;
                    if (px < 0 || px >= width_ || !mask[row * set.size + column]) continue;
                    pixels_[static_cast<std::size_t>(py) * width_ + px] = color;
                }
            }
            x += set.size;
        }
        return x;
    }

    std::vector<std::uint32_t> take() { return std::move(pixels_); }

private:
    int width_, height_;
    bool bgra_, linear_;
    std::vector<std::uint32_t> pixels_;
};

// Breaks a line at spaces so each piece is at most `columns` characters; a
// word longer than that is cut.
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, std::size_t columns) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view word = text.substr(start, end - start);
        while (word.size() > columns) {
            if (!current.empty()) {
                lines.push_back(std::move(current));
                current.clear();
            }
            lines.emplace_back(word.substr(0, columns));
            word.remove_prefix(columns);
        }
        if (!current.empty() && current.size() + 1 + word.size() > columns) {
            lines.push_back(std::move(current));
            current.clear();
        }
        if (!current.empty()) current += ' ';
        current += word;
        start = end + 1;
    }
    if (!current.empty() || lines.empty()) lines.push_back(std::move(current));
    return lines;
}

}  // namespace


std::string elliott_benchmark_text(const autonomous::Snapshot& state,std::uint64_t now_ms) {
    const auto& b=state.benchmark;
    if(!state.connected || !b.game_pid || b.game_pid!=state.runtime.process_id)return {};
    if(b.phase==autonomous::BenchmarkPhase::complete) {
        if(state.desired.afw_only && state.runtime.pacing_mode!=autonomous::kAfwOnlyPacingMode)
            return "Tests complete | restart game: Frame Reconstruction only";
        return "Tests complete";
    }
    if(!autonomous::benchmark_active(b) || !b.count || b.index>=b.count)return {};
    char text[96]{};
    if(b.phase==autonomous::BenchmarkPhase::applying) {
        std::snprintf(text,sizeof(text),"Test %u/%u | applying settings",b.index+1,b.count);
    } else {
        const auto elapsed=now_ms>=b.started_ms ? now_ms-b.started_ms : 0;
        const double left=double(autonomous::kBenchmarkDurationMs-std::min(elapsed,autonomous::kBenchmarkDurationMs))/1000.0;
        std::snprintf(text,sizeof(text),"Test %u/%u | %s | %.1fs left",b.index+1,b.count,
            elapsed<autonomous::kBenchmarkSettleMs ? "settling" : "measuring",left);
    }
    return text;
}

std::vector<std::uint32_t> rasterize_elliott_meter(const autonomous::Snapshot& state,
    std::uint64_t now_ms, bool bgra, bool linear, std::uint32_t* used_height) {
    constexpr int unit=2, body=2, big=4, margin=24, w=kElliottMeterWidth, h=kElliottMeterHeight;
    const auto& m=state.runtime.telemetry;
    const bool current=state.connected && m.updated_ms && now_ms>=m.updated_ms && now_ms-m.updated_ms<2000;
    const auto status=autonomous::interface_state_text(state);
    // State labels include the actual generator and active optional stages.
    // An enabled optional stage must run; AFW/OSR has work only with Luke Ross
    // or UEVR's Alternating/AFR.
    const auto& r=state.runtime;
    const bool optimizing=status=="Optimizing, please wait";
    const bool fresh=r.heartbeat_ms && now_ms>=r.heartbeat_ms && now_ms-r.heartbeat_ms<2000;
    const bool optional_ready=(!state.desired.nr_enabled || (fresh && r.nr_running)) &&
        (!autonomous::reconstruction_source(r) || state.desired.afw_mode==0 || (fresh && r.afw_running));
    const bool ofxr_active=m.generating && state.desired.enabled && autonomous::interface_generation_active(state) && !r.provider_error;
    const bool rife_error=state.desired.enabled && state.desired.frame_generation==4 && r.rife_error;
    const bool healthy=current && !rife_error && (ofxr_active || autonomous::interface_afw_only_active(state,now_ms)) && optional_ready;
    const auto tone=healthy ? PanelTone::good : PanelTone::bad;
    const auto metric_tone=[&](bool valid){return healthy && valid ? PanelTone::good : PanelTone::bad;};
    const auto value=[](float v,bool valid,bool rate){
        char b[64]="N/A";
        if(valid && std::isfinite(v))std::snprintf(b,sizeof(b),rate?"%.0f":"%.1f ms",v);
        return std::string(b);
    };
    struct Number { std::string value; const char* label; PanelTone tone; };
    const Number numbers[]{
        {value(m.output_fps,current&&output_rate_valid(m),true),state.runtime.luke_detected?"New submitted FPS":"Output FPS",tone},
        {value(m.game_fps,current&&m.rates_valid,true),"Real submitted",metric_tone(m.rates_valid)},
        {value(m.generated_fps,current&&m.rates_valid,true),"Gen submitted",metric_tone(m.rates_valid)},
        {value(m.display_hz,current&&m.display_hz>0,true),"Display Hz",PanelTone::dim},
        {value(m.ofxr_time_ms,current&&m.ofxr_time_valid,false),"OFXR time",metric_tone(m.ofxr_time_valid)},
        {value(m.cycle_length_ms,current&&m.cycle_valid,false),"Cycle length",metric_tone(m.cycle_valid)},
        {value(m.cycle_estimated_ms,current&&m.cycle_estimated_valid,false),"Cycle minimum",metric_tone(m.cycle_estimated_valid)}};
    const std::string vectors=std::string(autonomous::interface_vectors_text(state,now_ms));
    const std::string explanation=autonomous::interface_status_text(state,false);
    const auto reconstruction=autonomous::interface_reconstruction_text(state,now_ms);
    const auto vector_rows=wrap(vectors,51), status_rows=wrap(explanation,51);
    const auto benchmark=elliott_benchmark_text(state,now_ms);
    const int rows_top=214+(benchmark.empty()?0:28), line_height=22;
    const int count=std::min<int>(int(vector_rows.size()+status_rows.size()+1),(h-rows_top-8)/line_height);
    const int used=rows_top+count*line_height+8;
    if(used_height)*used_height=unsigned(used);
    Canvas c(w,h,bgra,linear);c.panel(used,16,2);
    c.text(margin,18,"OFXR 0.3.3",body,PanelTone::accent);
    const auto gpu_memory=autonomous::interface_vram_text(state)+"  "+autonomous::interface_gpu_text(state,now_ms);
    const int header_status_scale=status.size()>24 ? 1 : body;
    const int status_x=w-margin-8*header_status_scale*int(status.size());
    const int gpu_left=margin+8*body*10+16; // title plus gap
    const int gpu_available=status_x-16-gpu_left;
    const int gpu_scale=8*body*int(gpu_memory.size())<=gpu_available?body:1;
    c.text(gpu_left+(gpu_available-8*gpu_scale*int(gpu_memory.size()))/2,18,gpu_memory,gpu_scale,PanelTone::dim);
    c.text(w-margin-8*header_status_scale*int(status.size()),18,status,header_status_scale,
        optimizing ? PanelTone::warn : tone);
    c.rule(margin,w-margin,50,2);
    for(int i=0;i<int(std::size(numbers));++i){
        const int column=(w-2*margin)/(i<4?4:3);
        const int centre=margin+column*(i<4?i:i-4)+column/2;
        const auto& n=numbers[i]; const int y=i<4?62:138;
        const int scale=i<4?big:body;
        const auto lines=wrap(n.value,std::size_t(column/(8*scale)));
        int yy=y;
        for(std::size_t k=0;k<std::min<std::size_t>(lines.size(),2);++k){
            c.text(centre-8*scale*int(lines[k].size())/2,yy,lines[k],scale,n.tone);yy+=8*scale+2;
        }
        const int label_scale=8*body*int(std::string_view(n.label).size())>column-8 ? 1 : body;
        c.text(centre-8*label_scale*int(std::string_view(n.label).size())/2,i<4?100:184,n.label,label_scale,PanelTone::dim);
    }
    c.rule(margin,w-margin,124,2);c.rule(margin,w-margin,206,2);
    if(!benchmark.empty())c.text(margin,214,benchmark,body,PanelTone::accent);
    int y=rows_top, drawn=0;
    const auto rows=[&](const char* label,const auto& pieces){
        if(drawn>=count)return;
        c.text(margin,y,label,body,PanelTone::dim);
        for(const auto& text:pieces){if(drawn++>=count)break;c.text(184,y,text,body,PanelTone::normal);y+=line_height;}
    };
    if(drawn<count){c.text(margin,y,reconstruction,body,PanelTone::good);++drawn;y+=line_height;}
    rows("Vectors:",vector_rows);rows("Status:",status_rows);
    return c.take();
}
} // namespace xrfg
