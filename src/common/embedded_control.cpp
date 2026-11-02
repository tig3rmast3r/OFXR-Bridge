#include "xrfg/embedded_control.hpp"
#include "xrfg/rife_resolution.hpp"
#include "xrfg/implicit_layer.hpp"
#include "xrfg/osr_full_warp.hpp"
#include <windows.h>
#include <map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cwchar>
#include <optional>
#include <iterator>

namespace xrfg::embedded {
namespace {
std::atomic_bool g_layer_active{false};
std::filesystem::path directory() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&directory), &module);
    wchar_t path[32768]{};
    GetModuleFileNameW(module, path, 32768);
    return std::filesystem::path(path).parent_path();
}
const std::filesystem::path& ini() { static auto path = directory() / L"ofxr_bridge.ini"; return path; }
struct Receipt {
    std::uint64_t revision{}; bool enabled{}; long error{};
    std::optional<bool> djules_prefer_fps;
    bool djules_triple{};
};
struct Control {
    std::mutex mutex;
    Settings settings;
    std::uint64_t revision{1}, next_id{1};
    std::map<std::uint64_t, Receipt> sessions;
    Control() {
        const auto options = implicit_layer::read_nvidia_options(directory());
        settings.backend = static_cast<int>(implicit_layer::read_flow_backend(directory()));
        wchar_t route[16]{};
        GetPrivateProfileStringW(L"ofxr", L"nvidia_route", L"legacy", route, 16, ini().c_str());
        if (settings.backend == 1 && _wcsicmp(route, L"new") == 0) settings.backend = 2;
        if (settings.backend == 1 && _wcsicmp(route, L"djules") == 0) settings.backend = 3;
        settings.preset = static_cast<int>(options.preset);
        settings.scale = static_cast<int>(options.input_scale);
        settings.backward = options.bidirectional;
        settings.lower_vram = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"ofxr", L"single_swapchain_rings", 0, ini().c_str())), 0, 1);
        settings.separate_synthesis = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"ofxr", L"separate_synthesis", settings.lower_vram, ini().c_str())), 0, 1);
        settings.pacing_mode = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"ofxr", L"pacing_mode", 2, ini().c_str())),0,2);
        settings.increase_buffer=GetPrivateProfileIntW(L"ofxr",L"increase_buffer",0,ini().c_str())!=0;
        settings.enabled = GetPrivateProfileIntW(L"ofxr", L"enabled", 1, ini().c_str()) != 0;
        wchar_t motion[16]{};
        GetPrivateProfileStringW(L"ofxr", L"motion_vectors", L"off", motion, 16, ini().c_str());
        settings.motion_vectors = _wcsicmp(motion, L"dlss") == 0 ? 1 : 0;
        wchar_t generation[16]{};
        GetPrivateProfileStringW(L"ofxr", L"frame_generation", L"ofxr", generation, 16, ini().c_str());
        settings.frame_generation = _wcsicmp(generation, L"fsr3") == 0 ? 1 : _wcsicmp(generation, L"dlss") == 0 ? 2 : 0;
        settings.rife_resolution=xrfg::rife_resolution_preset(static_cast<int>(GetPrivateProfileIntW(L"ofxr",L"rife_resolution",18,ini().c_str())));
        settings.rife_multiplier=std::clamp(static_cast<int>(GetPrivateProfileIntW(L"ofxr",L"rife_multiplier",2,ini().c_str())),2,4);
        settings.rife_model=std::clamp(static_cast<int>(GetPrivateProfileIntW(L"ofxr",L"rife_model",-1,ini().c_str())),-1,4);
        settings.hybrid = GetPrivateProfileIntW(L"ofxr",L"dlss_flow_hybrid",0,ini().c_str())!=0;
        settings.native_scale = std::clamp(GetPrivateProfileIntW(L"ofxr", L"dlssg_resolution", 100, ini().c_str()), 25u, 100u);
    }
};
Control& control() { static Control state; return state; }

float read_float(const wchar_t* section, const wchar_t* key,
    float fallback) {
    wchar_t fallback_text[32]{};
    swprintf_s(fallback_text, L"%.3f", fallback);
    wchar_t value[32]{};
    GetPrivateProfileStringW(
        section, key, fallback_text, value, 32, ini().c_str());
    wchar_t* end{};
    const float parsed = std::wcstof(value, &end);
    return end != value && std::isfinite(parsed) ? parsed : fallback;
}

struct AfwControl {
    std::mutex mutex;
    AfwSnapshot snapshot;
    AfwControl() {
        auto& settings = snapshot.desired;
        settings.mode = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_afw_mode", 3, ini().c_str())), 0, 3);
        settings.debug = GetPrivateProfileIntW(
            L"experimental", L"luke_afw_debug", 0,
            ini().c_str()) != 0;
        settings.clear_before_warping = GetPrivateProfileIntW(
            L"experimental", L"luke_afw_clear_before_warping", 0,
            ini().c_str()) != 0;
        settings.ignore_motion_threshold = std::clamp(read_float(
            L"experimental", L"luke_afw_ignore_motion_threshold", 2.5F),
            0.0F, 64.0F);
        settings.shading_rate = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_afw_shading_rate", 0, ini().c_str())), 0, 8);
        settings.depth_dilate_mode = std::clamp(static_cast<int>(
            GetPrivateProfileIntW(L"experimental", L"luke_afw_depth_dilate_mode",
                0, ini().c_str())), 0, 2);
        settings.osr = normalize_osr_reconstruction(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_osr", 2, ini().c_str())));
        settings.osr_cover = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_osr_cover", 0, ini().c_str())), 0, 8);
        settings.osr_trim = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_osr_trim", 0, ini().c_str())), 0, 2);
        settings.osr_fill = std::clamp(static_cast<int>(GetPrivateProfileIntW(
            L"experimental", L"luke_osr_fill", 2, ini().c_str())), 1, 8);
    }
};
AfwControl& afw_control() { static AfwControl state; return state; }
bool write(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    return WritePrivateProfileStringW(section, key, value, ini().c_str()) != FALSE;
}
constexpr const wchar_t* positions[]{L"off", L"upper_left", L"upper_right", L"lower_left", L"lower_right", L"elliott_controller", L"elliott_bottom"};
}
Snapshot snapshot() {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    Snapshot s; s.desired = c.settings; s.revision = c.revision;
    s.sessions = static_cast<unsigned>(c.sessions.size());
    for (auto& [id, r] : c.sessions) {
        if (r.revision != c.revision) ++s.pending;
        else if (r.error) { ++s.errors; s.last_error = r.error; }
        else if (r.enabled) ++s.ready;
        else ++s.bypass;
    }
    return s;
}
bool request(Settings s) {
    if (s.pacing_mode < 0 || s.pacing_mode > 2 || s.backend < 0 || s.backend > 3 || s.preset < 0 || s.preset > 2 ||
        s.scale < 0 || s.scale > (s.rife_model>=0?6:3) || s.motion_vectors < 0 || s.motion_vectors > 1 ||
        s.frame_generation < 0 || s.frame_generation > 2 || s.rife_model < -1 || s.rife_model > 4 || s.rife_resolution!=xrfg::rife_resolution_preset(s.rife_resolution) || s.rife_multiplier<2 || s.rife_multiplier>4 ||
        s.native_scale < 25 || s.native_scale > 100 ||
        s.separate_synthesis < 0 || s.separate_synthesis > 1 || s.lower_vram < 0 || s.lower_vram > 1) return false;
    auto& c = control(); std::scoped_lock lock(c.mutex);
    const auto old = c.settings;
    bool ok = true;
    // Do not rewrite diagnostics, comments, or unrelated configuration.
    if (old.pacing_mode != s.pacing_mode) {
        wchar_t mode[8]{};swprintf_s(mode,L"%d",s.pacing_mode);
        ok &= write(L"ofxr", L"pacing_mode", mode);
    }
    if (old.enabled != s.enabled) ok &= write(L"ofxr", L"enabled", s.enabled ? L"1" : L"0");
    if(old.increase_buffer!=s.increase_buffer)ok &=write(L"ofxr",L"increase_buffer",s.increase_buffer?L"1":L"0");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"backend", s.backend ? L"nvidia" : L"fidelityfx");
    if (old.backend != s.backend) ok &= write(L"ofxr", L"nvidia_route", s.backend == 3 ? L"djules" : s.backend == 2 ? L"new" : L"legacy");
    const wchar_t* presets[]{L"slow", L"medium", L"fast"};
    const wchar_t* scales[]{L"100", L"75", L"50", L"25", L"20", L"1/6", L"1/3"};
    if (old.preset != s.preset) ok &= write(L"ofxr", L"nvidia_preset", presets[s.preset]);
    if (old.scale != s.scale) ok &= write(L"ofxr", L"nvidia_input_scale", scales[s.scale]);
    if(old.separate_synthesis != s.separate_synthesis) ok &= write(L"ofxr", L"separate_synthesis", s.separate_synthesis ? L"1" : L"0");
    if (old.lower_vram != s.lower_vram) ok &= write(L"ofxr", L"single_swapchain_rings",
        s.lower_vram ? L"1" : L"0");
    if (old.backward != s.backward) ok &= write(L"ofxr", L"nvidia_bidirectional", s.backward ? L"1" : L"0");
    if (old.motion_vectors != s.motion_vectors) ok &= write(L"ofxr", L"motion_vectors", s.motion_vectors ? L"dlss" : L"off");
    if (old.frame_generation != s.frame_generation) ok &= write(L"ofxr", L"frame_generation", s.frame_generation == 1 ? L"fsr3" : s.frame_generation == 2 ? L"dlss" : L"ofxr");
    if (old.hybrid != s.hybrid) ok &= write(L"ofxr",L"dlss_flow_hybrid",s.hybrid ? L"1" : L"0");
    if(old.rife_multiplier!=s.rife_multiplier){wchar_t value[16]{};swprintf_s(value,L"%d",s.rife_multiplier);ok &= write(L"ofxr",L"rife_multiplier",value);}
    if(old.rife_resolution!=s.rife_resolution){wchar_t value[16]{};swprintf_s(value,L"%d",s.rife_resolution);ok &= write(L"ofxr",L"rife_resolution",value);}
    if(old.rife_model!=s.rife_model){wchar_t value[16]{};swprintf_s(value,L"%d",s.rife_model);ok &= write(L"ofxr",L"rife_model",value);}
    if (old.native_scale != s.native_scale) {
        wchar_t value[16]{}; swprintf_s(value,L"%u",s.native_scale);
        ok &= write(L"ofxr",L"dlssg_resolution",value);
    }
    if (old != s) { c.settings = s; ++c.revision; }
    return ok;
}

AfwSnapshot afw_snapshot() {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    auto result=c.snapshot;
    result.active_shading_rate=active_afw_shading_rate.load(std::memory_order_relaxed);
    return result;
}

bool request_afw(AfwSettings settings) {
    if (settings.mode < 0 || settings.mode > 3 ||
        settings.osr < 0 || settings.osr > 2 ||
        settings.osr_cover < 0 || settings.osr_cover > 8 ||
        settings.osr_trim < 0 || settings.osr_trim > 2 ||
        settings.osr_fill < 1 || settings.osr_fill > 8 ||
        settings.shading_rate < 0 || settings.shading_rate > 8 ||
        settings.depth_dilate_mode < 0 || settings.depth_dilate_mode > 2 ||
        !std::isfinite(settings.ignore_motion_threshold) ||
        settings.ignore_motion_threshold < 0.0F ||
        settings.ignore_motion_threshold > 64.0F) return false;
    settings.osr = normalize_osr_reconstruction(settings.osr);
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    const auto old = c.snapshot.desired;
    bool ok = true;
    if (old.mode != settings.mode) {
        const wchar_t* modes[]{L"0", L"1", L"2", L"3"};
        ok &= write(L"experimental", L"luke_afw_mode", modes[settings.mode]);
    }
    if (old.debug != settings.debug)
        ok &= write(L"experimental", L"luke_afw_debug",
            settings.debug ? L"1" : L"0");
    if (old.clear_before_warping != settings.clear_before_warping)
        ok &= write(L"experimental", L"luke_afw_clear_before_warping",
            settings.clear_before_warping ? L"1" : L"0");
    if (old.ignore_motion_threshold != settings.ignore_motion_threshold) {
        wchar_t value[32]{};
        swprintf_s(value, L"%.3f", settings.ignore_motion_threshold);
        ok &= write(L"experimental", L"luke_afw_ignore_motion_threshold", value);
    }
    if (old.depth_dilate_mode != settings.depth_dilate_mode) {
        const wchar_t* modes[]{L"0", L"1", L"2"};
        ok &= write(L"experimental", L"luke_afw_depth_dilate_mode",
            modes[settings.depth_dilate_mode]);
    }
    if (old.shading_rate != settings.shading_rate) {
        wchar_t value[16]{}; swprintf_s(value,L"%d",settings.shading_rate);
        ok &= write(L"experimental",L"luke_afw_shading_rate",value);
    }
    if (old.osr != settings.osr) {
        const wchar_t* modes[]{L"0", L"1", L"2"};
        ok &= write(L"experimental", L"luke_osr", modes[settings.osr]);
    }
    if (old.osr_cover != settings.osr_cover) {
        wchar_t value[16]{}; swprintf_s(value, L"%d", settings.osr_cover);
        ok &= write(L"experimental", L"luke_osr_cover", value);
    }
    if (old.osr_trim != settings.osr_trim) {
        wchar_t value[16]{}; swprintf_s(value, L"%d", settings.osr_trim);
        ok &= write(L"experimental", L"luke_osr_trim", value);
    }
    if (old.osr_fill != settings.osr_fill) {
        wchar_t value[16]{}; swprintf_s(value, L"%d", settings.osr_fill);
        ok &= write(L"experimental", L"luke_osr_fill", value);
    }
    if (!(old == settings)) {
        c.snapshot.desired = settings;
        ++c.snapshot.revision;
    }
    return ok;
}

bool toggle_afw_debug() {
    auto settings = afw_snapshot().desired;
    settings.debug = !settings.debug;
    return request_afw(settings);
}

void report_afw_ingress(bool published, bool camera) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.ingress_frames;
    if (published) ++c.snapshot.published_ingress_frames;
    if (!camera) ++c.snapshot.missing_camera_ingress_frames;
}

void report_afw_guide_rejection(std::uint32_t mask) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.rejected_guide_pairs;
    c.snapshot.last_guide_rejection = mask;
}

void report_afw_prepare(std::int64_t reason) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.projection_frames;
    c.snapshot.flat_scene=reason==29;
    if(reason==29)return; // application scene is a 2D quad, not failed AFW
    if (reason == 0) ++c.snapshot.ready_frames;
    else if (reason == 5) ++c.snapshot.missing_capture_frames;
    else if (reason == 18) ++c.snapshot.busy_frames;
    else ++c.snapshot.failed_frames;
}

void report_afw_evaluation(std::uint64_t publication,
    std::uint32_t source_eye, std::uint32_t motion_width,
    std::uint32_t motion_height, std::uint32_t output_width,
    std::uint32_t output_height, float motion_scale_x,
    float motion_scale_y, long result) {
    auto& c = afw_control();
    std::scoped_lock lock(c.mutex);
    ++c.snapshot.evaluations;
    if (result >= 0) {
        ++c.snapshot.successful_evaluations;
        c.snapshot.last_success_ms=GetTickCount64();
    }
    c.snapshot.last_publication = publication;
    c.snapshot.last_source_eye = source_eye;
    c.snapshot.last_motion_width = motion_width;
    c.snapshot.last_motion_height = motion_height;
    c.snapshot.last_output_width = output_width;
    c.snapshot.last_output_height = output_height;
    c.snapshot.last_motion_scale_x = motion_scale_x;
    c.snapshot.last_motion_scale_y = motion_scale_y;
    c.snapshot.last_result = result;
}
std::uint64_t attach() { auto& c = control(); std::scoped_lock lock(c.mutex); auto id = c.next_id++; c.sessions.emplace(id, Receipt{}); return id; }
void detach(std::uint64_t id) { auto& c = control(); std::scoped_lock lock(c.mutex); c.sessions.erase(id); }
void applied(std::uint64_t id, std::uint64_t revision, bool enabled, long error) {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    if (auto it = c.sessions.find(id); it != c.sessions.end()) {
        it->second.revision = revision;
        it->second.enabled = enabled;
        it->second.error = error;
    }
}
DjulesPipelineSnapshot djules_pipeline_snapshot() {
    DjulesPipelineSnapshot s;
    s.triple_frame_gen = GetPrivateProfileIntW(L"ofxr", L"triple_frame_gen", 0, ini().c_str()) != 0;
    s.prefer_fps = s.triple_frame_gen ||
        GetPrivateProfileIntW(L"ofxr", L"deep_pipeline", 1, ini().c_str()) != 0;
    auto& c = control(); std::scoped_lock lock(c.mutex);
    for (const auto& [id, receipt] : c.sessions)
        if (receipt.djules_prefer_fps &&
            (*receipt.djules_prefer_fps != s.prefer_fps || receipt.djules_triple != s.triple_frame_gen))
            s.pending = true;
    return s;
}
bool set_djules_prefer_fps(bool enabled) {
    if (!enabled && GetPrivateProfileIntW(L"ofxr", L"triple_frame_gen", 0, ini().c_str())) return false;
    return write(L"ofxr", L"deep_pipeline", enabled ? L"1" : L"0");
}
bool set_djules_triple_frame_gen(bool enabled) {
    return write(L"ofxr", L"triple_frame_gen", enabled ? L"1" : L"0");
}
void register_djules_pipeline(std::uint64_t id, bool prefer_fps, bool triple) {
    auto& c = control(); std::scoped_lock lock(c.mutex);
    if (auto it = c.sessions.find(id); it != c.sessions.end()) {
        it->second.djules_prefer_fps = prefer_fps;
        it->second.djules_triple = triple;
    }
}
void set_layer_active(bool active) noexcept { g_layer_active.store(active, std::memory_order_release); }
bool layer_active() noexcept { return g_layer_active.load(std::memory_order_acquire); }
bool logging_setting() { return GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 0, ini().c_str()) != 0; }
bool set_logging_setting(bool enabled) { return write(L"diagnostics", L"logging_enabled", enabled ? L"1" : L"0"); }
bool cadence_setting() { return GetPrivateProfileIntW(L"diagnostics", L"cadence_logging_enabled", 0, ini().c_str()) != 0; }
bool set_cadence_setting(bool enabled) { return write(L"diagnostics", L"cadence_logging_enabled", enabled ? L"1" : L"0"); }
bool marker_setting() { return GetPrivateProfileIntW(L"diagnostics", L"synthetic_marker_enabled", logging_setting() || cadence_setting(), ini().c_str()) != 0; }
bool set_marker_setting(bool enabled) { return write(L"diagnostics", L"synthetic_marker_enabled", enabled ? L"1" : L"0"); }
int overlay_setting() {
    wchar_t value[32]{}; GetPrivateProfileStringW(L"overlay", L"position", L"upper_right", value, 32, ini().c_str());
    for (int i = 0; i < static_cast<int>(std::size(positions)); ++i) if (_wcsicmp(value, positions[i]) == 0) return i;
    return 2;
}
bool set_overlay_setting(int position) { return position >= 0 && position < static_cast<int>(std::size(positions)) && write(L"overlay", L"position", positions[position]); }
}
