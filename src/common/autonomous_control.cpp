#include "xrfg/autonomous_control.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/presented_output_rate.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <limits>
#include <sstream>
#include <type_traits>
#include <atomic>
#include <locale>
#include <map>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace xrfg::autonomous {
std::string_view overlay_limit_text(std::uint32_t mask) noexcept {
    if(mask&4) return "OpenXR swapchain limit reached: the headset F10 menu could not open. Turn off unused overlays and retry F10. Settings remain available on the desktop.";
    if(mask&2) return "OpenXR swapchain limit reached: the OFXR meter could not be created. Turn off unused overlays and select the meter again.";
    if(mask&1) return "OpenXR swapchain limit reached: the OFXR FPS overlay could not be created. Turn off unused overlays and select it again.";
    return {};
}
std::string_view generation_status_text(int mode, int motion_source, GenerationState state) noexcept {
    switch (state) {
    case GenerationState::paused: return "Frame generation paused: real frames only";
    case GenerationState::waiting: return "Waiting for compatible frames; real frames only";
    case GenerationState::recovering: return "Bypass: priming fresh stereo history; real frames only";
    case GenerationState::busy: return "Bypass: capture or GPU output busy; real frames only";
    case GenerationState::unavailable: return "DLSS-G bypass: feature unavailable on this adapter/runtime or first NGX initialization";
    case GenerationState::runtime_missing: return "FSR bypass: frame-generation runtime missing";
    case GenerationState::initialization_failed: return "Bypass: frame-generation initialization failed";
    case GenerationState::dispatch_failed: return "Bypass: provider rejected generation; real frames only";
    case GenerationState::native_declined: return "DLSS-G bypass: NGX disabled interpolation for this pair; real frames only";
    case GenerationState::multi_frame_unsupported: return "DLSS-G: 3X unsupported on this adapter; using 2X";
    default: break;
    }
    const bool fallback = (mode == 0 || mode == 3) && motion_source == 1;
    switch (state) {
    case GenerationState::missing_motion:
        return fallback ? "Fallback to optical flow: no compatible game motion vectors" :
            "Bypass: no compatible game motion vectors; real frames only";
    case GenerationState::missing_depth:
        return fallback ? "Fallback to optical flow: native depth unavailable" :
            "Bypass: native depth missing or its convention is unknown; real frames only";
    case GenerationState::invalid_guides:
        return fallback ? "Fallback to optical flow: native guides rejected" :
            "Bypass: native guides rejected (frame/device/camera identity); real frames only";
    case GenerationState::invalid_stereo:
        return fallback ? "Fallback to optical flow: distinct stereo guides unavailable" :
            "Bypass: distinct guides required for both eyes; real frames only";
    case GenerationState::temporal_mismatch:
        return fallback ? "Fallback to optical flow: guide history is discontinuous" :
            "Bypass: guide history is discontinuous; real frames only";
    case GenerationState::output_mismatch:
        return fallback ? "Fallback to optical flow: upscaler output does not match XR color" :
            "Bypass: upscaler output does not match XR color; real frames only";
    case GenerationState::queue_mismatch:
        return fallback ? "Fallback to optical flow: guide queue ownership mismatch" :
            "Bypass: guide queue ownership mismatch; real frames only";
    default:
        if (mode == 3) return motion_source == 1 ? "Hybrid: game motion vectors + FidelityFX optical flow" :
            "Hybrid fallback: FidelityFX optical flow; no complete native stereo guides";
        if (mode == 1) return "FSR: native stereo motion and depth guides active";
        if (mode == 2) return "DLSS-G: native stereo motion and depth guides active";
        return motion_source == 1 ? "OFXR: using compatible game motion vectors (optical flow bypassed)" :
            "OFXR: optical flow active";
    }
}
namespace {
constexpr std::uint32_t kMagic = 0x41465952;
constexpr std::uint32_t kAbi = 75901;
std::atomic_uint overlay_limits{};
static_assert(offsetof(RuntimeStatus, provider_error)==80); // V611 IPC layout
static_assert(offsetof(RuntimeStatus, afw_running)==77); // V611 AFW activity field
static_assert(offsetof(RuntimeStatus, overlay_limit_mask)==84); // V611 overlay refusal field
struct Shared {
    std::uint32_t magic{};
    std::uint32_t abi{};
    std::uint32_t bytes{};
    Snapshot snapshot{};
    wchar_t configuration_file[32768]{};
    alignas(8) std::uint64_t visible_until_ms[4]{};
    alignas(8) std::uint64_t benchmark_metrics_until_ms{};
    Settings benchmark_saved{};
    alignas(8) std::uint64_t benchmark_memory_request{};
    BenchmarkMemory benchmark_memory{};
};
static_assert(std::is_trivially_copyable_v<Shared>);

float bounded(float value, float minimum, float maximum, float fallback) {
    return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
}
std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r");
    if (first == value.npos) return {};
    const auto last = value.find_last_not_of(" \t\r");
    return std::string(value.substr(first, last - first + 1));
}
constexpr std::array<std::string_view, kGenerationProfileCount> profile_sections{
    "[route.ofxr_fidelityfx]", "[route.ofxr_reference]", "[route.ofxr_new]",
    "[route.ofxr_djules]", "[route.fsr]", "[route.dlssg]", "[route.hybrid]"};
std::size_t selected_profile(const Settings& s) noexcept {
    if (s.frame_generation==3) return 6;
    return s.frame_generation == 0 ? static_cast<std::size_t>(std::clamp(s.backend, 0, 3)) :
        static_cast<std::size_t>(std::clamp(s.frame_generation, 1, 2) + 3);
}
GenerationPreferences active_preferences(const Settings& s) noexcept {
    return {s.motion_vectors, s.preset, s.scale, s.backward, s.separate_synthesis, s.lower_vram,
        s.prefer_fps, s.triple_frame_gen, s.native_scale};
}
void restore_preferences(Settings& s) noexcept {
    const auto& p = s.generation_profiles[selected_profile(s)];
    s.motion_vectors = p.motion_vectors; s.preset = p.preset; s.scale = p.scale;
    s.backward = p.backward; s.separate_synthesis = p.separate_synthesis; s.lower_vram = p.lower_vram; s.prefer_fps = p.prefer_fps;
    s.triple_frame_gen = p.triple_frame_gen; s.native_scale = p.native_scale;
}
std::string configuration_text(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
Settings load(const std::filesystem::path& path) { return parse_settings(configuration_text(path)); }
std::string merge_sections(std::string_view old, std::string_view replacement) {
    std::vector<std::string> replaced;
    std::istringstream fresh{std::string(replacement)}; std::string line;
    while (std::getline(fresh,line)) {
        const auto clean=trim(line);
        if (clean.starts_with('[') && clean.ends_with(']')) replaced.push_back(clean);
    }
    std::istringstream input{std::string(old)}; std::string result; bool keep=true;
    while (std::getline(input,line)) {
        const auto clean=trim(line);
        if (clean.starts_with('[') && clean.ends_with(']'))
            keep=std::find(replaced.begin(),replaced.end(),clean)==replaced.end();
        if (keep) result+=line+'\n';
    }
    result+='\n'; result+=replacement;

    return result;
}
bool save_sections(const std::filesystem::path& path, std::string_view sections) {
    const auto text=merge_sections(configuration_text(path),sections);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
#ifdef _WIN32
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
#else
    temporary += ".tmp";
#endif
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output << text;
        output.close();
        if (!output) return false;
    }
#ifdef _WIN32
    const bool success = MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!success) DeleteFileW(temporary.c_str());
    return success;
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
#endif
}
bool save(const Shared& shared, const Settings& value) {
    return save_sections(std::filesystem::path(shared.configuration_file),serialize_settings(value));
}
std::filesystem::path process_profile;
std::atomic_bool process_profile_bound{};
std::uint64_t path_identity(const std::filesystem::path& path) {
    std::uint64_t identity=14695981039346659037ULL;
    for (const auto c:path.wstring()) {identity^=static_cast<std::uint64_t>(std::towlower(c));identity*=1099511628211ULL;}
    return identity;
}
#ifdef _WIN32
// Discovery only. Settings, leases and benchmark state use per-profile mappings.
struct DiscoveryState { DWORD pid{}; std::uint64_t heartbeat{}; wchar_t executable[260]{}; };
class Discovery {
    HANDLE mutex{},mapping{};
    DiscoveryState* data{};
public:
    Discovery() {
        const auto prefix=L"Local\\OFXRBridgeV667-discovery-"+std::to_wstring(path_identity(global_configuration_path()));
        mutex=CreateMutexW(nullptr,FALSE,(prefix+L"-mutex").c_str());
        mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(DiscoveryState),(prefix+L"-data").c_str());
        if(mapping)data=static_cast<DiscoveryState*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(DiscoveryState)));
    }
    ~Discovery(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(mutex)CloseHandle(mutex);}
    std::filesystem::path active() {
        if(!mutex || !data)return {};
        const auto result=WaitForSingleObject(mutex,0);
        if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED)return {};
        const auto state=*data;ReleaseMutex(mutex);
        const auto now=GetTickCount64();
        if(!state.pid || !state.heartbeat || now<state.heartbeat)return {};
        const auto handle=OpenProcess(SYNCHRONIZE,FALSE,state.pid);
        if(!handle)return {};
        const bool alive=WaitForSingleObject(handle,0)==WAIT_TIMEOUT;CloseHandle(handle);
        if(!alive)return {};
        const std::filesystem::path name(state.executable);
        return name==name.filename() ? game_configuration_path(name) : std::filesystem::path{};
    }
    void report(const RuntimeStatus& status) {
        static std::atomic_uint64_t last{};
        if(status.sessions && status.heartbeat_ms-last.load(std::memory_order_relaxed)<250)return;
        last.store(status.heartbeat_ms,std::memory_order_relaxed);
        if(!process_profile_bound.load(std::memory_order_acquire) || !mutex || !data)return;
        const auto result=WaitForSingleObject(mutex,0);
        if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED)return;
        if(status.sessions) {
            data->pid=status.process_id;data->heartbeat=status.heartbeat_ms;
            auto name=process_profile.filename().wstring();
            if(name.ends_with(L".ini"))name.resize(name.size()-4);
            wcsncpy_s(data->executable,name.c_str(),_TRUNCATE);
        } else if(data->pid==status.process_id)*data={};
        ReleaseMutex(mutex);
    }
};
Discovery& discovery(){static Discovery value;return value;}
#endif

class Channel {
public:
#ifdef _WIN32
    HANDLE mutex{};
    HANDLE mapping{};
    Shared* data{};
    explicit Channel(const std::filesystem::path& file) {
        // Profile identity is part of the mapping name, so fixture profiles
        // and future concurrent candidates cannot modify the user's settings.
        const auto identity=path_identity(file);
        const auto prefix = L"Local\\OFXRBridgeV667-" + std::to_wstring(identity);
        mutex = CreateMutexW(nullptr, FALSE, (prefix + L"-mutex").c_str());
        if (!mutex) return;
        const DWORD waited = WaitForSingleObject(mutex, 2000);
        if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) return;
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(Shared), (prefix + L"-control").c_str());
        if (mapping) data = static_cast<Shared*>(MapViewOfFile(
            mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (data && (data->magic != kMagic || data->abi != kAbi || data->bytes != sizeof(Shared))) {
            // A same-name object with another ABI is never overwritten.
            if (data->magic != 0) {
                UnmapViewOfFile(data);
                data = nullptr;
            } else {
                Shared initial{};
                initial.magic = kMagic;
                initial.abi = kAbi;
                initial.bytes = sizeof(Shared);
                initial.snapshot.desired = load(file);
                wcsncpy_s(initial.configuration_file,file.c_str(),_TRUNCATE);
                *data = initial;
            }
        }
        ReleaseMutex(mutex);
    }
    ~Channel() {
        if (data) UnmapViewOfFile(data);
        if (mapping) CloseHandle(mapping);
        if (mutex) CloseHandle(mutex);
    }
    bool lock(unsigned milliseconds) {
        if (!mutex || !data) return false;
        const DWORD waited = WaitForSingleObject(mutex, milliseconds);
        return waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    void unlock() { ReleaseMutex(mutex); }
#else
    std::mutex mutex;
    Shared storage{};
    explicit Channel(const std::filesystem::path& file) { storage.magic=kMagic;storage.abi=kAbi;storage.bytes=sizeof(Shared);storage.snapshot.desired=load(file);std::wcsncpy(storage.configuration_file,file.wstring().c_str(),32767); }
    Shared* data{&storage};
    bool lock(unsigned) { mutex.lock(); return true; }
    void unlock() { mutex.unlock(); }
#endif
};
Channel& channel() {
    if(process_profile_bound.load(std::memory_order_acquire)) {
        static Channel runtime(process_profile);return runtime;
    }
    const auto file=configuration_path();
    static std::mutex mutex;
    static std::map<std::filesystem::path,std::unique_ptr<Channel>> instances;
    std::scoped_lock lock(mutex);
    auto& instance=instances[file];if(!instance)instance=std::make_unique<Channel>(file);
    return *instance;
}
struct Lock {
    Channel& owner;
    bool acquired;
    explicit Lock(Channel& value, unsigned milliseconds = 0) : owner(value), acquired(value.lock(milliseconds)) {}
    ~Lock() { if (acquired) owner.unlock(); }
};
} // namespace

Settings sanitize(Settings value) noexcept {
    value.pacing_mode=std::clamp(value.pacing_mode,0,value.advanced_mode?2:1);
    value.benchmark_csv=true;
    if(!value.advanced_mode && value.frame_generation==0)value.motion_vectors=0;
    value.backend = std::clamp(value.backend, 0, 3);
    value.hybrid_backend = 0; // Migrate old selectable fallbacks to FidelityFX.
    value.motion_vectors = std::clamp(value.motion_vectors, 0, 1);
    value.frame_generation = std::clamp(value.frame_generation, 0, 3);
    // DLSS-G owns its generator. Do not inherit a previous OFXR presenter's
    // queue/history policy: benchmark and manual/INI/IPC selection must agree.
    if (value.frame_generation == 2) value.backend = 0;
    value.preset = std::clamp(value.preset, 0, 2);
    value.scale = std::clamp(value.scale, 0, 3);
    value.separate_synthesis = std::clamp(value.separate_synthesis, 0, 1);
    value.lower_vram = std::clamp(value.lower_vram, 0, 1);
    value.separate_synthesis = value.lower_vram;
    value.native_scale = std::clamp(value.native_scale, 25, 100);
    value.afw_mode = std::clamp(value.afw_mode, 0, 3);
    value.afw_ignore_motion_threshold = bounded(value.afw_ignore_motion_threshold, 0.F, 16.F, 2.5F);
    value.afw_depth_dilate_mode = std::clamp(value.afw_depth_dilate_mode, 0, 2);
    value.afw_shading_rate = std::clamp(value.afw_shading_rate, 0, 8);
    value.luke_osr = std::clamp(value.luke_osr, 0, 1);
    value.luke_osr_cover = std::clamp(value.luke_osr_cover, 0, 8);
    value.luke_osr_trim = std::clamp(value.luke_osr_trim, 0, 2);
    value.luke_osr_fill = std::clamp(value.luke_osr_fill, 1, 8);
    value.nr_intensity = bounded(value.nr_intensity, 0.F, 2.F, 1.F);
    value.nr_preset = std::clamp(value.nr_preset, 0, 3);
    value.nr_style = std::clamp(value.nr_style, 0, 2);
    value.nr_local_structure = bounded(value.nr_local_structure, 0.F, 2.F, 1.F);
    value.nr_local_tone = bounded(value.nr_local_tone, 0.F, 2.F, 1.F);
    value.nr_skin_structure = bounded(value.nr_skin_structure, -1.F, 2.F, -1.F);
    value.nr_transfer_strength = bounded(value.nr_transfer_strength, 0.F, 2.F, 1.F);
    value.nr_colour_strength = bounded(value.nr_colour_strength, 0.F, 1.F, 1.F);
    value.nr_max_ratio = bounded(value.nr_max_ratio, 1.F, 8.F, 2.F);
    value.nr_working_scale = bounded(value.nr_working_scale, .25F, 1.F, 1.F);
    value.nr_white_point_scale = bounded(value.nr_white_point_scale, .25F, 4.F, 1.F);
    value.nr_debug_view = std::clamp(value.nr_debug_view, 0, 3);
    value.nr_compare = std::clamp(value.nr_compare, 0, 2);
    value.nr_compare_split = bounded(value.nr_compare_split, 0.F, 1.F, .5F);
    value.nr_compare_zoom = bounded(value.nr_compare_zoom, 1.F, 2.F, 1.F);
    value.overlay_position = std::clamp(value.overlay_position, 0, 6);
    value.menu_scale = bounded(value.menu_scale, .5F, 1.25F, 1.F);
    value.menu_offset_x = bounded(value.menu_offset_x, -1.F, 1.F, 0.F);
    value.menu_offset_y = bounded(value.menu_offset_y, -1.F, 1.F, 0.F);
    // FSR and DLSS-G require engine guides. No optical-flow substitution is
    // silently offered for a guide-only generation backend.
    if (value.frame_generation != 0) value.motion_vectors = 1;
    // Legacy profiles only know the selected route. Keep its explicit values;
    // unseen routes start from their own defaults, never from another menu.
    if (!value.profiles_initialized) value.generation_profiles = {};
    value.profiles_initialized = true;
    value.generation_profiles[selected_profile(value)] = active_preferences(value);
    for (std::size_t i = 0; i < value.generation_profiles.size(); ++i) {
        auto& p = value.generation_profiles[i];
        p.motion_vectors = i >= 4 ? 1 : std::clamp(p.motion_vectors, 0, 1);
        p.preset = std::clamp(p.preset, 0, 2); p.scale = std::clamp(p.scale, 0, 3);
        p.separate_synthesis = std::clamp(p.separate_synthesis, 0, 1);
        p.lower_vram = std::clamp(p.lower_vram, 0, 1);
        p.separate_synthesis = p.lower_vram;
        p.native_scale = std::clamp(p.native_scale, 25, 100);
    }
    restore_preferences(value);
    return value;
}

void select_generation(Settings& value, int generation, int ofxr_engine) noexcept {
    value = sanitize(value); // commit outgoing edits before restoring the target
    value.frame_generation = std::clamp(generation, 0, 3);
    value.backend = value.frame_generation == 2 ? 0 : std::clamp(ofxr_engine, 0, 3);
    restore_preferences(value);
}

Settings parse_settings(std::string_view text) {
    Settings value;
    int section = -2; // -1 ofxr, 0..5 generation profile, -2 ignored
    std::array<bool, kGenerationProfileCount> profile_seen{}, separation_seen{};
    bool flat_separation_seen{},pacing_seen{};
    int legacy_single_director=-1;
    std::istringstream input{std::string(text)};
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line == "[ofxr]" ? -1 : -2;
            for (std::size_t i = 0; i < profile_sections.size(); ++i)
                if (line == profile_sections[i]) { section = static_cast<int>(i); profile_seen[i] = true; }
            continue;
        }
        if (section == -2) continue;
        const auto equals = line.find('=');
        if (equals == line.npos) continue;
        const auto key = trim(std::string_view(line).substr(0, equals));
        const auto data = trim(std::string_view(line).substr(equals + 1));
        char* end = nullptr;
        const float number = std::strtof(data.c_str(), &end);
        if (!end || *end != '\0' || end == data.c_str() || !std::isfinite(number)) continue;
        // Bound before any float-to-integral conversion; hostile/unexpected
        // profile text must not produce an undefined out-of-range conversion.
        if (number < -1000000.F || number > 1000000.F) continue;
        if (section >= 0) {
            auto& profile = value.generation_profiles[static_cast<std::size_t>(section)];
            if(key=="separate_synthesis") separation_seen[static_cast<std::size_t>(section)]=true;
#define READ_PROFILE(name) if (key == #name) profile.name = static_cast<decltype(profile.name)>(number);
            READ_PROFILE(motion_vectors) READ_PROFILE(preset) READ_PROFILE(scale)
            READ_PROFILE(backward) READ_PROFILE(separate_synthesis) READ_PROFILE(lower_vram) READ_PROFILE(prefer_fps)
            READ_PROFILE(triple_frame_gen) READ_PROFILE(native_scale)
#undef READ_PROFILE
            continue;
        }
        if(key=="separate_synthesis") flat_separation_seen=true;
        if(key=="pacing_mode") pacing_seen=true;
        if(key=="AdvancedMode")value.advanced_mode=number!=0;
        if(key=="single_director") legacy_single_director=number!=0;
#define READ_FIELD(name) if (key == #name) value.name = static_cast<decltype(value.name)>(number);
        READ_FIELD(increase_buffer) READ_FIELD(enabled) READ_FIELD(afw_only) READ_FIELD(backend) READ_FIELD(motion_vectors)
        READ_FIELD(frame_generation) READ_FIELD(hybrid_backend) READ_FIELD(preset) READ_FIELD(scale)
        READ_FIELD(pacing_mode) READ_FIELD(backward) READ_FIELD(separate_synthesis) READ_FIELD(lower_vram) READ_FIELD(prefer_fps)
        READ_FIELD(triple_frame_gen) READ_FIELD(native_scale) READ_FIELD(r19_camera)
        READ_FIELD(afw_mode) READ_FIELD(afw_debug) READ_FIELD(afw_clear_before_warping)
        READ_FIELD(afw_ignore_motion_threshold) READ_FIELD(afw_depth_dilate_mode) READ_FIELD(afw_shading_rate)
        READ_FIELD(luke_osr) READ_FIELD(luke_osr_cover) READ_FIELD(luke_osr_trim) READ_FIELD(luke_osr_fill) READ_FIELD(luke_advanced)
        READ_FIELD(nr_enabled) READ_FIELD(nr_before_sr) READ_FIELD(nr_intensity)
        READ_FIELD(nr_preset) READ_FIELD(nr_style) READ_FIELD(nr_local_structure)
        READ_FIELD(nr_local_tone) READ_FIELD(nr_skin_structure) READ_FIELD(nr_auto_mask)
        READ_FIELD(nr_transfer_strength) READ_FIELD(nr_colour_strength)
        READ_FIELD(nr_max_ratio) READ_FIELD(nr_working_scale) READ_FIELD(nr_white_point_scale)
        READ_FIELD(nr_debug_view) READ_FIELD(nr_compare) READ_FIELD(nr_compare_split)
        READ_FIELD(nr_compare_zoom) READ_FIELD(nr_compare_swap)
        READ_FIELD(diagnostics) READ_FIELD(status_json) READ_FIELD(benchmark_csv) READ_FIELD(benchmark_auto_pick) READ_FIELD(overlay_position)
        READ_FIELD(menu_scale) READ_FIELD(menu_offset_x) READ_FIELD(menu_offset_y)
        READ_FIELD(luke_shared_swapchains)
#undef READ_FIELD
    }
    if(!pacing_seen && legacy_single_director>=0)value.pacing_mode=legacy_single_director?1:2;
    if(!flat_separation_seen) value.separate_synthesis=value.lower_vram;
    value.lower_vram = value.lower_vram > 0 || value.separate_synthesis > 0;
    for(std::size_t i=0;i<profile_seen.size();++i)
        if(!separation_seen[i]) value.generation_profiles[i].separate_synthesis=value.generation_profiles[i].lower_vram;
    for(auto& profile:value.generation_profiles)
        profile.lower_vram = profile.lower_vram > 0 || profile.separate_synthesis > 0;
    if (std::any_of(profile_seen.begin(), profile_seen.end(), [](bool seen) { return seen; })) {
        value.profiles_initialized = true;
        if (profile_seen[selected_profile(value)]) restore_preferences(value);
    }
    return sanitize(value);
}

std::string serialize_settings(const Settings& settings) {
    const Settings value = sanitize(settings);
    std::ostringstream output;
    output << "[ofxr]\nAdvancedMode=" << value.advanced_mode << '\n' << std::setprecision(9);
#define WRITE_FIELD(name) output << #name << '=' << value.name << '\n';
    WRITE_FIELD(increase_buffer) WRITE_FIELD(enabled) WRITE_FIELD(afw_only) WRITE_FIELD(backend) WRITE_FIELD(motion_vectors)
    WRITE_FIELD(frame_generation) WRITE_FIELD(hybrid_backend) WRITE_FIELD(preset) WRITE_FIELD(scale)
    WRITE_FIELD(pacing_mode) WRITE_FIELD(backward) WRITE_FIELD(separate_synthesis) WRITE_FIELD(lower_vram) WRITE_FIELD(prefer_fps)
    WRITE_FIELD(triple_frame_gen) WRITE_FIELD(native_scale) WRITE_FIELD(r19_camera)
    WRITE_FIELD(afw_mode) WRITE_FIELD(afw_debug) WRITE_FIELD(afw_clear_before_warping)
    WRITE_FIELD(afw_ignore_motion_threshold) WRITE_FIELD(afw_depth_dilate_mode) WRITE_FIELD(afw_shading_rate)
    WRITE_FIELD(luke_osr) WRITE_FIELD(luke_osr_cover) WRITE_FIELD(luke_osr_trim) WRITE_FIELD(luke_osr_fill) WRITE_FIELD(luke_advanced)
    WRITE_FIELD(nr_enabled) WRITE_FIELD(nr_before_sr) WRITE_FIELD(nr_intensity)
    WRITE_FIELD(nr_preset) WRITE_FIELD(nr_style) WRITE_FIELD(nr_local_structure)
    WRITE_FIELD(nr_local_tone) WRITE_FIELD(nr_skin_structure) WRITE_FIELD(nr_auto_mask)
    WRITE_FIELD(nr_transfer_strength) WRITE_FIELD(nr_colour_strength)
    WRITE_FIELD(nr_max_ratio) WRITE_FIELD(nr_working_scale) WRITE_FIELD(nr_white_point_scale)
    WRITE_FIELD(nr_debug_view) WRITE_FIELD(nr_compare) WRITE_FIELD(nr_compare_split)
    WRITE_FIELD(nr_compare_zoom) WRITE_FIELD(nr_compare_swap)
    WRITE_FIELD(diagnostics) WRITE_FIELD(status_json) WRITE_FIELD(benchmark_csv) WRITE_FIELD(benchmark_auto_pick) WRITE_FIELD(overlay_position)
    WRITE_FIELD(menu_scale) WRITE_FIELD(menu_offset_x) WRITE_FIELD(menu_offset_y)
    WRITE_FIELD(luke_shared_swapchains)
#undef WRITE_FIELD
    for (std::size_t i = 0; i < profile_sections.size(); ++i) {
        output << '\n' << profile_sections[i] << '\n';
        const auto& p = value.generation_profiles[i];
#define WRITE_PROFILE(name) output << #name << '=' << p.name << '\n';
        WRITE_PROFILE(motion_vectors) WRITE_PROFILE(preset) WRITE_PROFILE(scale)
        WRITE_PROFILE(backward) WRITE_PROFILE(separate_synthesis) WRITE_PROFILE(lower_vram) WRITE_PROFILE(prefer_fps)
        WRITE_PROFILE(triple_frame_gen) WRITE_PROFILE(native_scale)
#undef WRITE_PROFILE
    }
    return output.str();
}

std::filesystem::path global_configuration_path() {
#ifdef _WIN32
    wchar_t override_directory[32768]{};
    const auto override_length = GetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",
        override_directory, static_cast<DWORD>(std::size(override_directory)));
    if (override_length && override_length < std::size(override_directory)) {
        const std::filesystem::path override_path(override_directory);
        if (override_path.is_absolute()) return override_path / L"ofxr-bridge.ini";
    }
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local)))
        throw std::runtime_error("Local application data is unavailable");
    std::filesystem::path result(local);
    CoTaskMemFree(local);
    return result / L"ofxr-bridge" / L"ofxr-bridge.ini";
#else
    const auto* local = std::getenv("XDG_CONFIG_HOME");
    return std::filesystem::path(local ? local : ".") / "ofxr-bridge" / "ofxr-bridge.ini";
#endif
}

std::filesystem::path common_folder(){return global_configuration_path().parent_path();}
std::filesystem::path game_configuration_path(const std::filesystem::path& executable) {
    auto name=executable.filename();
    if(name.empty() || name==L"." || name==L"..")throw std::runtime_error("Invalid game executable name");
    name+=L".ini";return common_folder()/L"Profiles"/name;
}
std::filesystem::path configuration_path() {
    if(process_profile_bound.load(std::memory_order_acquire))return process_profile;
#ifdef _WIN32
    if(const auto active=discovery().active();!active.empty())return active;
#endif
    return global_configuration_path();
}
bool initialize_game_configuration(const std::filesystem::path& executable) noexcept {
    try {
        if(process_profile_bound.load(std::memory_order_acquire))return true;
        auto game=executable;
#ifdef _WIN32
        if(game.empty()) {wchar_t name[32768]{};const auto size=GetModuleFileNameW(nullptr,name,32768);if(!size || size>=32768)return false;game=name;}
#endif
        const auto file=game_configuration_path(game);
        // Serialize only game settings. Never clone tray/benchmark/driver state.
        // A dedicated creation mutex prevents two simultaneous first launches
        // overwriting one another's freshly created profile.
#ifdef _WIN32
        const auto name=L"Local\\OFXRBridgeV667-create-"+std::to_wstring(path_identity(file));
        HANDLE mutex=CreateMutexW(nullptr,FALSE,name.c_str());if(!mutex)return false;
        struct CreationGuard {HANDLE handle;bool locked{};~CreationGuard(){if(locked)ReleaseMutex(handle);CloseHandle(handle);}} guard{mutex};
        const auto waited=WaitForSingleObject(mutex,2000);
        if(waited!=WAIT_OBJECT_0 && waited!=WAIT_ABANDONED)return false;
        guard.locked=true;
#endif
        bool ok=true;
        if(std::filesystem::exists(file) && !std::filesystem::is_regular_file(file))return false;
        if(!std::filesystem::exists(file)) {
            auto text=serialize_settings(load(global_configuration_path()));
#ifdef _WIN32
            const auto preset=GetPrivateProfileIntW(L"dlssg",L"preset",1,global_configuration_path().c_str());
            text+="\n[dlssg]\npreset="+std::to_string(preset==0?0:preset==3?3:1)+"\n";
#endif
            ok=save_sections(file,text);
        }
        if(!ok)return false;
        process_profile=file;process_profile_bound.store(true,std::memory_order_release);
        auto& c=channel();Lock lock(c,2000);if(!lock.acquired)return false;
        // A desktop peer may have kept this mapping alive since the previous
        // game launch. Disk is authoritative at each new process start.
        const auto desired=load(file);
        if(c.data->snapshot.desired!=desired){c.data->snapshot.desired=desired;++c.data->snapshot.revision;}
        return true;
    }catch(...){return false;}
}
bool reset_to_defaults() noexcept {
    try {
        auto& c=channel();Lock lock(c,2000);if(!lock.acquired)return false;
        const auto desired=sanitize(Settings{});
        if(!save_sections(c.data->configuration_file,serialize_settings(desired)+"\n[dlssg]\npreset=1\n"))return false;
        auto& s=c.data->snapshot;
        if(benchmark_active(s.benchmark)) {s.benchmark.phase=BenchmarkPhase::stopped;std::snprintf(s.benchmark.message,sizeof(s.benchmark.message),"Stopped by Reset to defaults.");}
        s.desired=desired;++s.revision;return true;
    }catch(...){return false;}
}

bool write_configuration_sections(const std::filesystem::path& path, std::string_view sections) noexcept {
    try { auto& c=channel();Lock lock(c,2000);return lock.acquired && save_sections(path,sections); }
    catch (...) { return false; }
}
bool initialize_user_configuration() noexcept {
    try {
        const auto file=global_configuration_path();
        if(std::filesystem::exists(file))return std::filesystem::is_regular_file(file);
        const auto defaults=sanitize(Settings{});
        const auto settings=serialize_settings(defaults);
        if(!save_sections(file,settings+"\n[benchmark]\nenabled=1\n"))return false;
        return true;
    } catch (...) {return false;}
}
bool benchmark_enabled() noexcept {
#ifdef _WIN32
    try {return GetPrivateProfileIntW(L"benchmark",L"enabled",1,global_configuration_path().c_str())!=0;}
    catch (...) {}
#endif
    return false;
}

Snapshot snapshot() noexcept {
    try {
        auto& connection = channel();
        // A frame thread never waits for disk publication from the panel.
        // Retain the last coherent settings while another peer holds the lock.
        thread_local Snapshot last_good{};
        thread_local Channel* last_channel{};
        if(last_channel!=&connection) {last_good={load(connection.data ? std::filesystem::path(connection.data->configuration_file) : global_configuration_path()),1,{},false};last_channel=&connection;}
        Lock lock(connection);
        if (lock.acquired) last_good = connection.data->snapshot;
        Snapshot result = last_good;
#ifdef _WIN32
        const auto now = GetTickCount64();
        result.connected = result.runtime.sessions != 0 && result.runtime.process_id != 0 &&
            now >= result.runtime.heartbeat_ms && now - result.runtime.heartbeat_ms < 5000;
#endif
        return result;
    } catch (...) { return {}; }
}

bool request(Settings desired) noexcept {
    try {
        desired = sanitize(desired);
        auto& connection = channel();
        Lock lock(connection, 2000);
        if (!lock.acquired || !save(*connection.data,desired)) return false;
        if (connection.data->snapshot.desired != desired) {
            if(benchmark_active(connection.data->snapshot.benchmark)) {
                auto& b=connection.data->snapshot.benchmark;b.phase=BenchmarkPhase::stopped;
                b.requested_ms=GetTickCount64();
                std::snprintf(b.message,sizeof(b.message),"Stopped by a manual settings change; your edit is kept.");
            }
            connection.data->snapshot.desired = desired;
            ++connection.data->snapshot.revision;
        }
        return true;
    } catch (...) { return false; }
}

Settings merge_changes(Settings current, const Settings& prior, const Settings& edited) noexcept {
    current = sanitize(current);
    const auto base = sanitize(prior), desired = sanitize(edited);
    for (std::size_t i = 0; i < kGenerationProfileCount; ++i) {
        auto& p = current.generation_profiles[i];
        const auto& b = base.generation_profiles[i];
        const auto& d = desired.generation_profiles[i];
#define MERGE_PROFILE(name) if (b.name != d.name) p.name = d.name;
        MERGE_PROFILE(motion_vectors) MERGE_PROFILE(preset) MERGE_PROFILE(scale)
        MERGE_PROFILE(backward) MERGE_PROFILE(separate_synthesis) MERGE_PROFILE(lower_vram) MERGE_PROFILE(prefer_fps)
        MERGE_PROFILE(triple_frame_gen) MERGE_PROFILE(native_scale)
#undef MERGE_PROFILE
    }
#define MERGE_FIELD(name) if (base.name != desired.name) current.name = desired.name;
    MERGE_FIELD(increase_buffer) MERGE_FIELD(afw_only) MERGE_FIELD(pacing_mode) MERGE_FIELD(enabled) MERGE_FIELD(backend) MERGE_FIELD(frame_generation) MERGE_FIELD(hybrid_backend) MERGE_FIELD(r19_camera)
    MERGE_FIELD(afw_mode) MERGE_FIELD(afw_debug) MERGE_FIELD(afw_clear_before_warping)
    MERGE_FIELD(afw_ignore_motion_threshold) MERGE_FIELD(afw_depth_dilate_mode) MERGE_FIELD(afw_shading_rate)
    MERGE_FIELD(luke_osr) MERGE_FIELD(luke_osr_cover) MERGE_FIELD(luke_osr_trim) MERGE_FIELD(luke_osr_fill) MERGE_FIELD(luke_advanced)
    MERGE_FIELD(nr_enabled) MERGE_FIELD(nr_before_sr) MERGE_FIELD(nr_intensity)
    MERGE_FIELD(nr_preset) MERGE_FIELD(nr_style) MERGE_FIELD(nr_local_structure)
    MERGE_FIELD(nr_local_tone) MERGE_FIELD(nr_skin_structure) MERGE_FIELD(nr_auto_mask)
    MERGE_FIELD(nr_transfer_strength) MERGE_FIELD(nr_colour_strength)
    MERGE_FIELD(nr_max_ratio) MERGE_FIELD(nr_working_scale) MERGE_FIELD(nr_white_point_scale)
    MERGE_FIELD(nr_debug_view) MERGE_FIELD(nr_compare) MERGE_FIELD(nr_compare_split)
    MERGE_FIELD(nr_compare_zoom) MERGE_FIELD(nr_compare_swap)
    MERGE_FIELD(diagnostics) MERGE_FIELD(status_json) MERGE_FIELD(benchmark_csv) MERGE_FIELD(benchmark_auto_pick) MERGE_FIELD(overlay_position)
    MERGE_FIELD(menu_scale) MERGE_FIELD(menu_offset_x) MERGE_FIELD(menu_offset_y)
    MERGE_FIELD(luke_shared_swapchains)
#undef MERGE_FIELD
    if(base.advanced_mode!=desired.advanced_mode)current.advanced_mode=desired.advanced_mode;
    restore_preferences(current);
    return sanitize(current);
}

bool request_changes(const Settings& base, const Settings& desired) noexcept {
    try {
        auto& connection = channel();
        Lock lock(connection);
        if (!lock.acquired) return false;
        const auto merged = merge_changes(connection.data->snapshot.desired, base, desired);
        if (merged == connection.data->snapshot.desired) return true;
        if (!save(*connection.data,merged)) return false;
        if(benchmark_active(connection.data->snapshot.benchmark)) {
            auto& b=connection.data->snapshot.benchmark;b.phase=BenchmarkPhase::stopped;
            b.requested_ms=GetTickCount64();
            std::snprintf(b.message,sizeof(b.message),"Stopped by a manual settings change; your edit is kept.");
        }
        connection.data->snapshot.desired = merged;
        ++connection.data->snapshot.revision;
        return true;
    } catch (...) { return false; }
}

namespace {
struct SuiteResult {
    Settings settings;BenchmarkSamples samples;BenchmarkMemory memory;
    std::string label;
    bool complete{};
    std::uint32_t eye_long_side{};
    bool vectors{};
};
struct SuiteRecorder {
    Channel* controller{};
    std::uint64_t run{}, revision{}, started{}, next_sample{};
    unsigned index{};
    unsigned dlssg_preset{};
    std::ofstream stream;
    BenchmarkSamples samples;
    Settings settings;
    std::string label,game;
    std::filesystem::path layout_path;
    std::vector<SuiteResult> results;
    std::vector<BenchmarkStep> plan;
    bool afw_success{};
    bool vector_success{};
    std::uint32_t eye_long_side{};
    std::uint32_t bridge_buffer_capacity{};
    bool memory_requested{};
    bool steamvr{}; // pacing matrix; may also be opt-in VDXR
    bool legacy_vdxr{};
    std::string runtime;
    BenchmarkMemory memory{};
};
SuiteRecorder& suite_recorder() { static SuiteRecorder value;return value; }
std::string quoted(std::string text) {
    std::string result="\"";
    for(char c:text) {if(c=='\"')result+='\"';result+=c;}
    return result+'\"';
}
std::string utf8_path(const std::filesystem::path& p) {
    const auto value=p.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};
}
std::filesystem::path benchmark_game_path(unsigned pid) {
    const auto handle=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!handle)return {};
    wchar_t path[32768]{};DWORD size=32768;
    const bool found=QueryFullProcessImageNameW(handle,0,path,&size)!=FALSE;
    CloseHandle(handle);return found ? std::filesystem::path(path) : std::filesystem::path{};
}
bool suite_runtime_fresh(const RuntimeStatus& r) {
    const auto now=GetTickCount64();
    return r.process_id && r.sessions && now>=r.heartbeat_ms && now-r.heartbeat_ms<5000;
}
bool suite_publish(Shared& shared,Settings desired,std::uint64_t now) {
    desired=sanitize(desired);
    if(!save(shared,desired)) return false;
    auto& s=shared.snapshot;
    s.desired=desired;++s.revision;
    s.benchmark.revision=s.revision;
    s.benchmark.requested_ms=now;s.benchmark.started_ms=0;
    std::atomic_ref<std::uint64_t>(shared.benchmark_memory_request).store(0,std::memory_order_release);
    shared.benchmark_memory={};
    std::atomic_ref<std::uint64_t>(shared.benchmark_metrics_until_ms).store(GetTickCount64()+2000,std::memory_order_release);
    return true;
}
void suite_restore(Shared& shared,BenchmarkPhase phase,const char* message,std::uint64_t now) {
    const bool restored=suite_publish(shared,shared.benchmark_saved,now);
    auto& b=shared.snapshot.benchmark;
    b.phase=restored ? phase : BenchmarkPhase::error;
    std::atomic_ref<std::uint64_t>(shared.benchmark_metrics_until_ms).store(0,std::memory_order_release);
    std::snprintf(b.message,sizeof(b.message),"%s",restored ? message : "Could not restore the original settings; check the local profile.");
}
void csv_stat(std::ostream& stream,const std::vector<double>& values) {
    if(values.empty()) {stream<<"N/A,N/A,N/A";return;}
    stream<<*std::min_element(values.begin(),values.end())<<','<<benchmark_median(values)<<','<<*std::max_element(values.begin(),values.end());
}
void csv_mib(std::ostream& stream,bool valid,std::uint64_t bytes) {
    if(valid)stream<<double(bytes)/1048576.0;
    else stream<<"N/A";
}
bool same_trial(const Settings& a,const Settings& b) {
    return a.increase_buffer==b.increase_buffer && a.pacing_mode==b.pacing_mode && a.enabled==b.enabled && a.backend==b.backend && a.frame_generation==b.frame_generation &&
        a.triple_frame_gen==b.triple_frame_gen && a.prefer_fps==b.prefer_fps &&
        a.separate_synthesis==b.separate_synthesis && a.lower_vram==b.lower_vram &&
        a.afw_mode==b.afw_mode && (!a.afw_mode || a.afw_shading_rate==b.afw_shading_rate);
}
void csv_layout(const SuiteRecorder& rec,const BenchmarkStatus& b) {
    if(rec.layout_path.empty())return;
    const auto steps=benchmark_export_plan(b.full,b.luke,rec.steamvr,b.advanced);
    std::ofstream output(rec.layout_path,std::ios::trunc);output.imbue(std::locale::classic());
    if(!output)throw std::runtime_error("Cannot write layout CSV");
    output<<"Game,Build,Metric";
    for(const auto& step:steps) {
        auto label=step.disabled ? std::string("OFXR Off") : step.label;
        if(b.luke)label+=" / "+std::string(!step.afw ? "blank" : step.shading==0 ? "1x1" : step.shading==4 ? "2x2" : "4x4");
        output<<','<<quoted(label);
    }
    output<<'\n'<<std::fixed<<std::setprecision(3);
    const char* metrics[]{"Output FPS min","Output FPS median","Output FPS max","Cycle minimum min ms","Cycle minimum median ms","Cycle minimum max ms","VRAM end MiB","RAM working set end MiB","RAM private commit end MiB","Shared GPU end MiB"};
    for(unsigned metric=0;metric<10;++metric) {
        output<<quoted(rec.game)<<','<<XRFG_IMPLEMENTATION_VERSION<<','<<metrics[metric];
        for(const auto& step:steps) {
            const auto wanted=benchmark_settings(rec.settings,step);
            const SuiteResult* result=nullptr;
            for(const auto& candidate:rec.results)if(same_trial(wanted,candidate.settings))result=&candidate;
            output<<',';
            if(!result){output<<"n.a.";continue;}
            if(metric>=6) {
                const auto& m=result->memory;
                const bool valid=metric==6 ? m.vram_valid : metric==9 ? m.shared_gpu_valid : m.ram_valid;
                if(!valid)output<<"N/A";
                else output<<double(metric==6 ? m.vram_bytes : metric==7 ? m.ram_working_set_bytes : metric==8 ? m.ram_private_commit_bytes : m.shared_gpu_bytes)/1048576.0;
                continue;
            }
            const auto& values=metric<3 ? result->samples.fps : result->samples.latency;
            if(values.empty()){output<<"N/A";continue;}
            output<<(metric%3==0 ? *std::min_element(values.begin(),values.end()) : metric%3==1 ? benchmark_median(values) : *std::max_element(values.begin(),values.end()));
        }
        output<<'\n';
    }
    output.flush();if(!output)throw std::runtime_error("Layout CSV write failed");
}
void csv_finish(SuiteRecorder& rec,const BenchmarkStatus& b,std::uint64_t ended,const char* verdict) {
    if(!rec.revision)return;
    if(rec.stream.is_open()) {
        const auto& s=rec.settings;
        rec.stream<<XRFG_IMPLEMENTATION_VERSION<<','<<b.run_id<<','<<b.game_pid<<','<<quoted(rec.game)<<','<<rec.index+1<<','<<quoted(rec.label)<<','
            <<s.separate_synthesis<<','<<s.lower_vram<<','<<(s.triple_frame_gen ? 3 : 2)<<','<<s.prefer_fps<<','
            <<s.nr_enabled<<','<<s.afw_mode<<','<<s.afw_shading_rate<<','<<(s.frame_generation!=2 ? "N/A" :
                rec.dlssg_preset==1 ? "A startup request" : rec.dlssg_preset==3 ? "C startup request" : "Game default / unverified")
            <<','<<double(kBenchmarkSettleMs)/1000.0<<','
            <<std::clamp((double(ended)-double(rec.started)-double(kBenchmarkSettleMs))/1000.0,
                0.0,double(kBenchmarkMeasureMs)/1000.0)<<',';
        csv_stat(rec.stream,rec.samples.fps);rec.stream<<','<<rec.samples.fps.size()<<',';
        csv_stat(rec.stream,rec.samples.latency);rec.stream<<','<<rec.samples.latency.size()<<','
            <<rec.memory.updated_ms<<',';
        csv_mib(rec.stream,rec.memory.vram_valid,rec.memory.vram_bytes);rec.stream<<',';
        csv_mib(rec.stream,rec.memory.shared_gpu_valid,rec.memory.shared_gpu_bytes);rec.stream<<',';
        csv_mib(rec.stream,rec.memory.ram_valid,rec.memory.ram_working_set_bytes);rec.stream<<',';
        csv_mib(rec.stream,rec.memory.ram_valid,rec.memory.ram_private_commit_bytes);rec.stream<<','<<quoted(verdict)<<','<<s.pacing_mode<<','<<rec.runtime<<','<<rec.legacy_vdxr<<','<<rec.eye_long_side<<',';
        if(rec.samples.refresh_hz.empty())rec.stream<<"N/A";else rec.stream<<benchmark_median(rec.samples.refresh_hz);
        rec.stream<<','<<rec.vector_success<<','<<rec.bridge_buffer_capacity<<'\n';
        rec.stream.flush();
        if(!rec.stream)throw std::runtime_error("CSV write failed");
    }
    rec.results.push_back({rec.settings,rec.samples,rec.memory,rec.label,std::string_view(verdict)=="complete",rec.eye_long_side,rec.vector_success});
    csv_layout(rec,b);
    rec.revision=0;rec.samples={};
}
void csv_open(SuiteRecorder& rec,BenchmarkStatus& b,unsigned preset,const RuntimeStatus& runtime) {
    {auto* owner=rec.controller;rec=SuiteRecorder{};rec.controller=owner;}rec.run=b.run_id;
    rec.steamvr=pacing_comparison_runtime(runtime);
    rec.legacy_vdxr=runtime.vdxr_legacy;
    rec.runtime=runtime.vdxr_runtime?"VDXR":runtime.steamvr_runtime?"SteamVR":"Other";
    rec.dlssg_preset=preset;
    rec.plan=benchmark_execution_plan(b.full,b.luke,rec.steamvr,b.advanced,runtime.eye_long_side);
    rec.game=utf8_path(benchmark_game_path(b.game_pid).filename());
    if(!b.csv)return;
    const auto folder=common_folder()/L"Benchmarks";
    std::filesystem::create_directories(folder);
    const auto stem=L"OFXR-V"+std::to_wstring(XRFG_IMPLEMENTATION_VERSION)+L"-"+std::to_wstring(b.run_id);
    rec.layout_path=folder/(stem+L".csv");
    const auto path=folder/(stem+L"-details.csv");
    rec.stream.open(path,std::ios::out|std::ios::trunc);
    rec.stream.imbue(std::locale::classic());rec.stream<<std::fixed<<std::setprecision(3);
    if(!rec.stream)throw std::runtime_error("Cannot create benchmark CSV");
    rec.stream<<"Build,Run,GamePID,Game,Step,Route,SeparateA,ReduceB,Multiplier,PreferFPS,Neural,AFWMode,AFWShadingRate,DLSSGPreset,SettleSeconds,UsefulSeconds,OutputFPSMin,OutputFPSMedian,OutputFPSMax,FPSSamples,CycleMinimumMinMs,CycleMinimumMedianMs,CycleMinimumMaxMs,CycleSamples,MemorySnapshotMs,VRAMEndMiB,SharedGPUEndMiB,RAMWorkingSetEndMiB,RAMPrivateCommitEndMiB,Status,PacingMode,Runtime,LegacyVDXR,EyeLongSide,DisplayHz,NativeVectors,BridgeBuffer\n";
    rec.stream.flush();if(!rec.stream)throw std::runtime_error("Cannot write benchmark CSV header");
    std::snprintf(b.csv_path,sizeof(b.csv_path),"%s",utf8_path(rec.layout_path).c_str());
}
std::vector<BenchmarkScore> suite_scores(const SuiteRecorder& rec,bool luke) {
    std::vector<BenchmarkScore> scores;
    for(const auto& result:rec.results) {
        const auto& settings=result.settings;
        // The owner excludes the standalone FidelityFX route for image quality.
        // Hybrid has its own native-guide contract; Off/AFW-only are references.
        const bool fidelity=settings.enabled && settings.frame_generation==0 && settings.backend==0;
        auto score=benchmark_score(result.samples,result.complete && !fidelity);
        score.vectors=result.vectors;score.afw_only=luke && !settings.enabled && settings.afw_mode!=0;
        score.shading=settings.afw_shading_rate;score.eye_long_side=result.eye_long_side;
        score.triple=settings.enabled && settings.triple_frame_gen;
        score.display_hz=benchmark_median(result.samples.refresh_hz);scores.push_back(score);
    }
    return scores;
}
void suite_complete(Shared& shared,const SuiteRecorder& rec,std::uint64_t now) {
    auto& b=shared.snapshot.benchmark;
    if(!b.auto_pick) {
        suite_restore(shared,BenchmarkPhase::complete,"Complete. Original settings restored.",now);return;
    }
    const auto scores=suite_scores(rec,b.luke);
    const auto best=benchmark_best(scores,b.luke);
    if(!best) {
        suite_restore(shared,BenchmarkPhase::complete,"Complete. No valid automatic choice; original settings restored.",now);return;
    }
    const auto& result=rec.results[*best];const auto& score=scores[*best];
    auto selected=result.settings;
    // AFW-only trials run through the ordinary bridge to keep the matrix live.
    // Only the completed winner arms the direct AFW startup path for next launch.
    if(b.luke)selected.afw_only=score.afw_only;
    const bool restart_afw_only=b.luke && selected.afw_only &&
        shared.snapshot.runtime.pacing_mode!=kAfwOnlyPacingMode;
    if(!suite_publish(shared,selected,now)) {
        suite_restore(shared,BenchmarkPhase::error,"Could not save automatic choice; original settings restored.",now);return;
    }
    b.phase=BenchmarkPhase::complete;
    std::atomic_ref<std::uint64_t>(shared.benchmark_metrics_until_ms).store(0,std::memory_order_release);
    auto label=result.label;
    if(b.luke)label+=" / "+std::string(result.settings.afw_shading_rate==0?"1x1":result.settings.afw_shading_rate==4?"2x2":"4x4");
    const auto fps=std::minmax_element(result.samples.fps.begin(),result.samples.fps.end());
    const auto latency=std::minmax_element(result.samples.latency.begin(),result.samples.latency.end());
    std::snprintf(b.message,sizeof(b.message),"Complete. Selected %s: %.1f FPS (%.1f-%.1f), %.1f ms (%.1f-%.1f).%s",
        label.c_str(),score.fps,*fps.first,*fps.second,score.latency,*latency.first,*latency.second,
        restart_afw_only ? " Restart the game to activate AFW Only." : "");
}
} // namespace

bool start_benchmark(bool full,bool luke) noexcept {
    if (!benchmark_enabled()) return false;
    try {
        auto& c=channel();Lock lock(c,2000);if(!lock.acquired)return false;
        auto& shared=*c.data;auto& s=shared.snapshot;auto& b=s.benchmark;
        if(benchmark_active(b))return false;
        if(!suite_runtime_fresh(s.runtime)) {
            b={};b.phase=BenchmarkPhase::error;
            std::snprintf(b.message,sizeof(b.message),"Open the game and load the test scene first.");return false;
        }
        if(s.runtime.pacing_mode==kAfwOnlyPacingMode) {
            b={};b.phase=BenchmarkPhase::error;
            std::snprintf(b.message,sizeof(b.message),"AFW only is active. Disable it in the Luke Ross tab and restart the game for the OFXR matrix.");return false;
        }
        if(luke && !s.runtime.luke_detected) {
            b={};b.phase=BenchmarkPhase::error;
            std::snprintf(b.message,sizeof(b.message),"Luke Ross was not detected in the active game.");return false;
        }
        shared.benchmark_saved=s.desired;
        const auto now=GetTickCount64();
        const auto run=std::max(b.run_id+1,(now<<16)^GetCurrentProcessId());
        b={};b.full=full;b.luke=luke;b.advanced=s.desired.advanced_mode;b.auto_pick=s.desired.benchmark_auto_pick;b.csv=s.desired.benchmark_csv;b.game_pid=s.runtime.process_id;
        b.run_id=run;
        const auto plan=benchmark_execution_plan(full,luke,pacing_comparison_runtime(s.runtime),b.advanced,s.runtime.eye_long_side);b.count=static_cast<unsigned>(plan.size());
        b.phase=BenchmarkPhase::applying;
        std::snprintf(b.label,sizeof(b.label),"%s",plan[0].label.c_str());
        if(!suite_publish(shared,benchmark_settings(shared.benchmark_saved,plan[0]),now)) {
            b.phase=BenchmarkPhase::error;std::snprintf(b.message,sizeof(b.message),"Could not save the first test settings.");return false;
        }
        suite_recorder().controller=&c;
        return true;
    } catch(...) {return false;}
}
void stop_benchmark() noexcept {
    try {
        auto& rec=suite_recorder();auto& c=rec.controller?*rec.controller:channel();Lock lock(c,2000);if(!lock.acquired)return;
        if(benchmark_active(c.data->snapshot.benchmark))
            suite_restore(*c.data,BenchmarkPhase::stopped,"Stopped. Original settings restored.",GetTickCount64());
    } catch(...) {}
}
void tick_benchmark(std::uint64_t now) noexcept {
    try {
        const bool permitted=benchmark_enabled();
        auto& rec=suite_recorder();auto& c=rec.controller?*rec.controller:channel();Lock lock(c);if(!lock.acquired)return;
        if(!now)now=GetTickCount64();
        auto& shared=*c.data;auto& s=shared.snapshot;auto& b=s.benchmark;
        const auto own_pid=GetCurrentProcessId();
        if (!permitted && benchmark_active(b))
            suite_restore(shared,BenchmarkPhase::stopped,"Benchmarks disabled in ofxr-bridge.ini; original settings restored.",now);
        if(!benchmark_active(b)) {
            std::atomic_ref<std::uint64_t>(shared.benchmark_memory_request).store(0,std::memory_order_release);
            std::atomic_ref<std::uint64_t>(shared.benchmark_metrics_until_ms).store(0,std::memory_order_release);
            if(rec.run==b.run_id && b.controller_pid==own_pid) {
                csv_finish(rec,b,b.requested_ms,b.phase==BenchmarkPhase::complete ? "complete" : "stopped / partial");
                rec.stream.close();
            }
            rec.controller=nullptr;
            return;
        }
        if(b.controller_pid && b.controller_pid!=own_pid)return;
        b.controller_pid=own_pid;rec.controller=&c;
        std::atomic_ref<std::uint64_t>(shared.benchmark_metrics_until_ms).store(GetTickCount64()+2000,std::memory_order_release);
        try {
            if(rec.run!=b.run_id)csv_open(rec,b,s.runtime.dlssg_preset_request,s.runtime);
            if(!suite_runtime_fresh(s.runtime) || s.runtime.process_id!=b.game_pid) {
                csv_finish(rec,b,now,"game disconnected / partial");
                suite_restore(shared,BenchmarkPhase::error,"Game disconnected. Original settings restored.",now);rec.stream.close();return;
            }
            if(s.revision!=b.revision) {
                b.phase=BenchmarkPhase::stopped;
                std::snprintf(b.message,sizeof(b.message),"Settings changed outside the suite; stopped.");return;
            }
            if(b.phase==BenchmarkPhase::applying) {
                const bool quality_pending=b.luke && s.desired.afw_mode &&
                    s.runtime.afw_shading_rate!=s.desired.afw_shading_rate;
                const bool pacing_pending=rec.steamvr && s.runtime.pacing_mode!=static_cast<unsigned>(s.desired.pacing_mode);
                const bool buffer_pending=rec.steamvr && !b.advanced &&
                    s.runtime.buffer_capacity!=static_cast<unsigned>(s.desired.increase_buffer?3:2);
                if(s.runtime.applied_revision!=b.revision || s.runtime.pending || quality_pending || pacing_pending || buffer_pending) {
                    if(now>=b.requested_ms+30000)
                        suite_restore(shared,BenchmarkPhase::error,"Settings not applied within 30 seconds; stopped.",now);
                    return;
                }
                b.phase=BenchmarkPhase::running;b.started_ms=now;
                rec.revision=b.revision;rec.index=b.index;rec.started=now;
                rec.settings=s.desired;rec.label=b.label;rec.samples={};
                rec.afw_success=false;rec.vector_success=false;rec.eye_long_side=0;
                rec.bridge_buffer_capacity=s.runtime.buffer_capacity;
                rec.memory_requested=false;rec.memory={};
                rec.next_sample=now;
            }
            if(now>=rec.next_sample) {
                rec.next_sample=now+250;
                if(s.runtime.applied_revision==b.revision && s.runtime.telemetry.updated_ms<=GetTickCount64()) {
                    rec.bridge_buffer_capacity=s.runtime.buffer_capacity;
                    const auto previous=rec.samples.last_ms;
                    rec.samples.observe(s.runtime.telemetry,b.started_ms,std::min(now+1,b.started_ms+kBenchmarkDurationMs));
                    if(rec.samples.last_ms!=previous) {
                        if(const auto side=s.runtime.eye_long_side)rec.eye_long_side=rec.eye_long_side?std::min(rec.eye_long_side,side):side;
                        rec.vector_success |= s.desired.enabled && s.runtime.telemetry.generating && s.runtime.motion_source==1 &&
                            (s.runtime.generation_state==GenerationState::ready || s.runtime.generation_state==GenerationState::multi_frame_unsupported);
                    }
                }
                rec.afw_success |= s.runtime.afw_running;
            }
            if(!rec.memory_requested && now>=b.started_ms+kBenchmarkMemoryRequestMs) {
                shared.benchmark_memory={};
                std::atomic_ref<std::uint64_t>(shared.benchmark_memory_request).store(b.revision,std::memory_order_release);
                rec.memory_requested=true;
            }
            if(shared.benchmark_memory.revision==b.revision && shared.benchmark_memory.updated_ms>=rec.started)
                rec.memory=shared.benchmark_memory;
            if(now<b.started_ms+kBenchmarkDurationMs)return;
            csv_finish(rec,b,now,b.luke && s.desired.afw_mode && !rec.afw_success ? "no confirmed AFW output" : rec.samples.fps.empty() ? "no measured presentation rate" :
                s.desired.enabled && !rec.samples.generated ? "no confirmed generation" : "complete");
            const auto scores=suite_scores(rec,b.luke);
            if(!b.luke) {
                bool sufficient=false;
                for(std::size_t i=0;i<rec.results.size();++i)
                    sufficient |= benchmark_2x_sufficient(rec.results[i].settings,scores[i]);
                if(sufficient)rec.plan.erase(std::remove_if(rec.plan.begin()+b.index+1,rec.plan.end(),
                    [](const auto& step){return step.triple;}),rec.plan.end());
                b.count=static_cast<unsigned>(rec.plan.size());
            }
            // Finish the complete full-quality pass and compare the first half-quality
            // AFW-only reference before deciding whether reduced-quality trials add value.
            const bool luke_checkpoint=b.luke && b.index>0 && !rec.settings.enabled &&
                rec.settings.afw_shading_rate==4;
            if(luke_checkpoint && benchmark_luke_early_complete(scores)) {
                b.count=b.index+1;suite_complete(shared,rec,now);rec.stream.close();return;
            }
            if(++b.index>=rec.plan.size()) {
                suite_complete(shared,rec,now);rec.stream.close();return;
            }
            b.phase=BenchmarkPhase::applying;
            std::snprintf(b.label,sizeof(b.label),"%s",rec.plan[b.index].label.c_str());
            if(!suite_publish(shared,benchmark_settings(shared.benchmark_saved,rec.plan[b.index]),now))
                suite_restore(shared,BenchmarkPhase::error,"Could not apply the next test; stopped.",now);
        } catch(...) {
            rec.stream.close();suite_restore(shared,BenchmarkPhase::error,"CSV output failed; suite stopped. Check the Benchmarks folder.",now);
        }
    } catch(...) {}
}

std::uint64_t take_benchmark_memory_request() noexcept {
    try {
        auto& c=channel();
        if(!c.data || !std::atomic_ref<std::uint64_t>(c.data->benchmark_memory_request).load(std::memory_order_acquire))return 0;
        Lock lock(c);if(!lock.acquired)return 0;
        const auto& b=c.data->snapshot.benchmark;
        if(!benchmark_active(b) || b.game_pid!=GetCurrentProcessId())return 0;
        return std::atomic_ref<std::uint64_t>(c.data->benchmark_memory_request).exchange(0,std::memory_order_acq_rel);
    } catch(...) {return 0;}
}
void report_benchmark_memory(BenchmarkMemory memory) noexcept {
    try {
        auto& c=channel();Lock lock(c);if(!lock.acquired)return;
        const auto& b=c.data->snapshot.benchmark;
        if(benchmark_active(b) && b.game_pid==GetCurrentProcessId() && memory.revision==b.revision)
            c.data->benchmark_memory=memory;
    } catch(...) {}
}

void report_runtime(RuntimeStatus status) noexcept {
    try {
        auto& connection = channel();
        Lock lock(connection);
        if (!lock.acquired) return;
#ifdef _WIN32
        status.process_id = GetCurrentProcessId();
        status.heartbeat_ms = GetTickCount64();
#endif
        status.overlay_limit_mask=overlay_limits.load(std::memory_order_relaxed);
        status.message[sizeof(status.message) - 1] = '\0';
        if (connection.data->snapshot.runtime.process_id == status.process_id) {
            status.telemetry = connection.data->snapshot.runtime.telemetry;
            status.video_memory = connection.data->snapshot.runtime.video_memory;
        }
        connection.data->snapshot.runtime = status;
#ifdef _WIN32
        discovery().report(status);
#endif
    } catch (...) {}
}
void report_video_memory(VideoMemoryStatus value) noexcept {
    try {
        auto& c=channel(); Lock lock(c); if(!lock.acquired)return;
        auto& runtime=c.data->snapshot.runtime;
        if(runtime.process_id && runtime.process_id!=GetCurrentProcessId())return;
        runtime.video_memory=value;
    } catch(...) {}
}
std::string interface_vram_text(const Snapshot& value) {
    const auto& memory=value.runtime.video_memory;
    if(!value.connected || !memory.updated_ms || !memory.budget_bytes)return "VRAM N/A";
    char text[48]{};
    std::snprintf(text,sizeof(text),"VRAM %.0f%%",100.*double(memory.used_bytes)/double(memory.budget_bytes));
    return text;
}
void report_overlay_limit(OverlaySwapchain surface, bool refused) noexcept {
    const auto bit=static_cast<unsigned>(surface);
    if(refused) overlay_limits.fetch_or(bit,std::memory_order_relaxed);
    else overlay_limits.fetch_and(~bit,std::memory_order_relaxed);
    try {
        auto& connection=channel();Lock lock(connection);
        if(!lock.acquired)return;
        auto& runtime=connection.data->snapshot.runtime;
#ifdef _WIN32
        if(runtime.process_id && runtime.process_id!=GetCurrentProcessId())return;
#endif
        runtime.overlay_limit_mask=overlay_limits.load(std::memory_order_relaxed);
    } catch(...) {}
}
void set_interface_visible(InterfaceSurface surface, bool visible) noexcept {
    try {
        auto& c = channel(); if (!c.data) return;
        const auto index = static_cast<unsigned>(surface); if (index > 3) return;
        std::atomic_ref<std::uint64_t>(c.data->visible_until_ms[index]).store(
            visible ? GetTickCount64() + 1000 : 0, std::memory_order_release);
    } catch (...) {}
}
bool interface_telemetry_requested() noexcept {
    try {
        auto& c = channel(); if (!c.data) return false;
        // CSV samples need the same metrics with F9 and both menus off.
        const auto now = GetTickCount64();
        if(std::atomic_ref<std::uint64_t>(c.data->benchmark_metrics_until_ms).load(std::memory_order_acquire)>now)return true;
        for (auto& deadline : c.data->visible_until_ms)
            if (std::atomic_ref<std::uint64_t>(deadline).load(std::memory_order_acquire) > now) return true;
    } catch (...) {}
    return false;
}
void report_interface_telemetry(InterfaceTelemetry value) noexcept {
    try {
        auto& c = channel(); Lock lock(c); if (!lock.acquired) return;
        if (c.data->snapshot.runtime.process_id && c.data->snapshot.runtime.process_id != GetCurrentProcessId()) return;
        value.updated_ms = GetTickCount64(); c.data->snapshot.runtime.telemetry = value;
    } catch (...) {}
}
std::string_view interface_vectors_text(const Snapshot& s, std::uint64_t now) noexcept {
    const auto& r = s.runtime;
    // Native SR/RR is the only typed vector ingress in V456. FSR FG is a
    // consumer, never evidence of FSR-origin vectors.
    return s.connected && r.upscaler_evaluations && r.upscaler_motion &&
        now >= r.upscaler_last_seen_ms && now - r.upscaler_last_seen_ms < 2000 ? "DLSS" : "None";
}
std::string_view interface_state_text(const Snapshot& s) noexcept {
    if (!s.connected) return "Waiting";
    const auto& r = s.runtime;
    if(interface_afw_only_active(s,GetTickCount64())) {
        const bool nr=s.desired.nr_enabled && r.nr_running;
        if(s.desired.luke_osr) return nr ? "AFW + OSR + Neural" : "AFW + OSR";
        return nr ? "AFW + Neural" : "AFW";
    }
    if (r.generation_state == GenerationState::paused) return "Paused";
    if (r.pending || r.applied_revision != s.revision) return "Applying settings";
    const bool active = r.telemetry.updated_ms && GetTickCount64() - r.telemetry.updated_ms < 2000 && r.telemetry.generating;
    if (active && (r.generation_mode == 0 || r.generation_mode == 3) && r.motion_source == 1 &&
        r.generation_state != GenerationState::ready) return "Fallback";
    if (active) {
        const auto now=GetTickCount64();
        const bool fresh=r.heartbeat_ms && now>=r.heartbeat_ms && now-r.heartbeat_ms<2000;
        const bool afw=fresh && r.afw_running && s.desired.afw_mode!=0;
        const bool nr=fresh && r.nr_running && s.desired.nr_enabled;
        // Static views retain noexcept/zero allocation on both UI surfaces.
        // Columns: base, + AFW, + Neural, + AFW + Neural, + AFW + OSR, + AFW + OSR + Neural.
        static constexpr std::string_view names[7][6]={
            {"FidelityFX","FidelityFX + AFW","FidelityFX + Neural","FidelityFX + AFW + Neural","FidelityFX + AFW + OSR","FidelityFX + AFW + OSR + Neural"},
            {"OFXR 0.2.1","OFXR 0.2.1 + AFW","OFXR 0.2.1 + Neural","OFXR 0.2.1 + AFW + Neural","OFXR 0.2.1 + AFW + OSR","OFXR 0.2.1 + AFW + OSR + Neural"},
            {"OFXR 0.3.0","OFXR 0.3.0 + AFW","OFXR 0.3.0 + Neural","OFXR 0.3.0 + AFW + Neural","OFXR 0.3.0 + AFW + OSR","OFXR 0.3.0 + AFW + OSR + Neural"},
            {"OFXR 0.2.13","OFXR 0.2.13 + AFW","OFXR 0.2.13 + Neural","OFXR 0.2.13 + AFW + Neural","OFXR 0.2.13 + AFW + OSR","OFXR 0.2.13 + AFW + OSR + Neural"},
            {"Hybrid","Hybrid + AFW","Hybrid + Neural","Hybrid + AFW + Neural","Hybrid + AFW + OSR","Hybrid + AFW + OSR + Neural"},
            {"FSR","FSR + AFW","FSR + Neural","FSR + AFW + Neural","FSR + AFW + OSR","FSR + AFW + OSR + Neural"},
            {"DLSS-G","DLSS-G + AFW","DLSS-G + Neural","DLSS-G + AFW + Neural","DLSS-G + AFW + OSR","DLSS-G + AFW + OSR + Neural"}};
        const int route=r.generation_mode==1 ? 5 : r.generation_mode==2 ? 6 :
            r.generation_mode==3 ? 4 : std::clamp(s.desired.backend,0,3);
        const bool osr=afw && s.desired.luke_osr!=0;
        return names[route][osr ? (nr?5:4) : (afw?1:0)+(nr?2:0)];
    }
    return r.ready ? "Real frames only" : "Waiting";
}
bool interface_generation_active(const Snapshot& s) noexcept {
    const auto text=interface_state_text(s);
    return s.desired.enabled && (text.starts_with("FidelityFX") || text.starts_with("OFXR ") ||
        text.starts_with("Hybrid") || text.starts_with("FSR") || text.starts_with("DLSS-G"));
}
bool interface_afw_only_active(const Snapshot& s,std::uint64_t now) noexcept {
    const auto& r=s.runtime;
    return s.connected && r.heartbeat_ms && now>=r.heartbeat_ms && now-r.heartbeat_ms<2000 &&
        r.luke_detected && s.desired.afw_mode!=0 && r.afw_running &&
        (!s.desired.enabled || r.pacing_mode==kAfwOnlyPacingMode) && !r.pending && r.applied_revision==s.revision;
}
std::string_view swapchain_budget_warning(const SwapchainBudgetStatus& value) noexcept {
    if (value.fallback) return "Swapchain fallback: auxiliary allocation refused; synchronous Luke delivery is active until the game restarts.";
    if (value.refused) return "Swapchain allocation refused: one or more game outputs could not obtain generation resources.";
    if (value.unassigned_native) return "Unclassified native swapchain: an observed OpenXR output has no assigned owner. See Details.";
    if (value.other) return "Unclassified app swapchain: not observed in an eye or auxiliary layer. See handle and dimensions below.";
    return {};
}
std::string interface_status_text(const Snapshot& s) {
    if (!s.connected) return "No active game";
    std::string result;
    const auto add = [&](std::string_view text) {
        if (text.empty() || result.find(text) != std::string::npos) return;
        if (!result.empty()) result += " | "; result += text;
    };
    const auto& r = s.runtime;
    if (r.shared_queue) add("Shared queue");
    if(pacing_comparison_runtime(r) && r.pacing_mode>=0 && r.pacing_mode!=kAfwOnlyPacingMode && r.pacing_mode!=s.desired.pacing_mode)
        add("Applying pacing policy");
    switch (r.generation_state) {
    case GenerationState::missing_motion: add("Missing game vectors"); break;
    case GenerationState::missing_depth: add("Missing depth"); break;
    case GenerationState::invalid_guides: add("Invalid vectors / guide identity"); break;
    case GenerationState::invalid_stereo: add("Missing stereo pair"); break;
    case GenerationState::temporal_mismatch: add("Frame mismatch / missing history pair"); break;
    case GenerationState::output_mismatch: add("Game output does not match the eye image"); break;
    case GenerationState::queue_mismatch: add("Queue mismatch"); break;
    case GenerationState::unavailable: add("DLSS-G provider unavailable"); break;
    case GenerationState::runtime_missing: add("FSR provider missing"); break;
    case GenerationState::initialization_failed: add("Provider initialization failed"); break;
    case GenerationState::dispatch_failed: add(r.generation_mode == 2 ? "NGX rejected generation" : r.generation_mode == 1 ? "FSR rejected generation" : "OFXR rejected generation"); break;
    case GenerationState::native_declined: add("NGX refused interpolation"); break;
    case GenerationState::multi_frame_unsupported: add("Native 3X unsupported: using 2X"); break;
    case GenerationState::recovering: add("Priming fresh stereo history"); break;
    case GenerationState::busy: add("Capture / GPU output busy"); break;
    default: break;
    }
    add(std::string_view(r.message, std::find(std::begin(r.message), std::end(r.message), '\0') - std::begin(r.message)));
    if (r.provider_error) {
        char code[48]{};
        std::snprintf(code, sizeof(code), "NGX result 0x%08X", r.provider_error);
        add(code);
    }
    if (r.swapchains.fallback) add("Swapchain fallback");
    else if (r.swapchains.refused) add("Swapchain allocation refused");
    if (r.swapchains.other) add("Unclassified app swapchain");
    if (r.overlay_limit_mask) add("Overlay swapchain limit reached");
    if (r.luke_detected) add("Luke Ross detected");
    if (r.vulkan_detected) add("Vulkan detected");
    if (r.vulkan_capture_suspended) add((r.generation_mode == 0 || r.generation_mode == 3)
        ? "Vulkan game-vector capture suspended: OFXR uses optical flow"
        : "Vulkan game-vector capture suspended: FSR / DLSS-G real-frame bypass");
    const auto now=GetTickCount64();
    const bool generating=r.telemetry.updated_ms && now>=r.telemetry.updated_ms &&
        now-r.telemetry.updated_ms<2000 && r.telemetry.generating;
    const bool runtime_fresh=r.heartbeat_ms && now>=r.heartbeat_ms && now-r.heartbeat_ms<2000;
    const auto generator=r.generation_mode==1 ? "FSR" : r.generation_mode==2 ? "DLSS-G" : "OFXR";
    add(std::string(generator)+(s.desired.enabled ? (generating ? ": Active" : ": Waiting") : ": Off"));
    const char* afw_label=s.desired.luke_osr ? "AFW + OSR" : "AFW";
    add(!r.luke_detected ? std::string(afw_label)+": N/A" : s.desired.afw_mode==0 ? std::string(afw_label)+": Off" :
        runtime_fresh && r.afw_flat_scene ? std::string(afw_label)+": 2D scene (no stereo source)" :
        runtime_fresh && r.afw_running ? std::string(afw_label)+": Active" : std::string(afw_label)+": Waiting");
    add(!s.desired.nr_enabled ? "Neural Render: Off" : runtime_fresh && r.nr_running ? "Neural Render: Active" : "Neural Render: Waiting");
    return result;
}
} // namespace xrfg::autonomous
