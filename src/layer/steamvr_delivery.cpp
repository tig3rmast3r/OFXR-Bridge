#include "xrfg/steamvr_delivery.hpp"

#include "xrfg/bridge_flight_logger.hpp"
#include "xrfg/compositor_presentation_rate.hpp"

#include <openvr.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>

namespace xrfg {
namespace {

using PfnInitInternal2 =
    std::uint32_t(__cdecl*)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
using PfnShutdownInternal = void(__cdecl*)();
using PfnGetGenericInterface = void*(__cdecl*)(const char*, vr::EVRInitError*);
using PfnGetInitToken = std::uint32_t(__cdecl*)();

// The runtime registers itself here rather than in the registry, and this is
// where SteamVR itself looks. Parsed by hand: a JSON dependency for one string
// would be the larger cost. Deliberately duplicated from the standalone probe,
// which must keep building with no layer dependency at all.
[[nodiscard]] std::string runtime_path_from_vrpath() {
    char local[MAX_PATH]{};
    if (GetEnvironmentVariableA("LOCALAPPDATA", local, MAX_PATH) == 0) {
        return {};
    }
    const std::string file = std::string(local) + "\\openvr\\openvrpaths.vrpath";
    FILE* handle = nullptr;
    if (fopen_s(&handle, file.c_str(), "rb") != 0 || handle == nullptr) {
        return {};
    }
    std::string text;
    char buffer[4096];
    for (;;) {
        const std::size_t read = fread(buffer, 1, sizeof(buffer), handle);
        if (read == 0) {
            break;
        }
        text.append(buffer, read);
    }
    fclose(handle);

    const std::size_t key = text.find("\"runtime\"");
    if (key == std::string::npos) {
        return {};
    }
    const std::size_t open = text.find('[', key);
    if (open == std::string::npos) {
        return {};
    }
    const std::size_t first = text.find('"', open);
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = text.find('"', first + 1);
    if (last == std::string::npos) {
        return {};
    }
    std::string raw = text.substr(first + 1, last - first - 1);
    std::string path;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
            path.push_back('\\');
            ++i;
        } else {
            path.push_back(raw[i]);
        }
    }
    return path;
}

// How the compositor interface was obtained, recorded so a capture says which
// path worked rather than only that one did.
enum class AttachRoute : std::uint64_t {
    none = 0,
    // The process already had an OpenVR context and we borrowed it. Costs
    // nothing and adds no state, so it is tried first.
    existing_token = 1,
    // A second, background connection of our own. Never Scene: this must not
    // become the application the compositor is serving, and must not start
    // SteamVR if it is not already running.
    background_init = 2,
};

constexpr std::int64_t kWindowNanoseconds = 1'000'000'000;
// Mispresented-above-predicted values are small; anything past this is counted
// with the top bucket and is a loss under any baseline.
// Past this the last window is too old to describe what is on screen now, so
// report nothing rather than something stale.
constexpr std::int64_t kStaleNanoseconds = 5'000'000'000;

// One OpenVR connection for the whole process, opened at most once and
// deliberately never shut down.
//
// It used to be per session, and tearing it down is what killed Assetto Corsa.
// That title recreates its OpenXR session during start-up - three times, on
// every runtime - and SteamVR ships its OpenXR runtime and its OpenVR client
// in one binary, vrclient_x64.dll. Opening a background OpenVR connection into
// that binary and then closing it leaves the next xrCreateSession through it
// dereferencing null, inside the runtime, with the layer merely forwarding the
// call. Captured with symbols: the fault is in vrclient_x64 and the only frame
// of ours below it is the downstream create.
//
// Every session create before our connection attaches succeeds and the first
// one after it dies, and when the attach happened three seconds later in one
// run the crash moved three seconds later with it. The title ran on V185,
// which is the last build with no connection at all.
//
// So the connection outlives every session. Not shutting it down is the point,
// not an oversight: VR_ShutdownInternal is what does the damage, and a
// background connection costs nothing to leave open for a process that is
// about to exit anyway.
//
// The cost is paid at the other lifecycle boundary. Outliving the session also
// means outliving the *instance*, and an application that destroys its
// XrInstance and builds another one then hangs in xrCreateInstance - in
// SteamVR, with the layer forwarding the call, the same vrclient_x64.dll and
// the same connection, one boundary over. Measured on R.E.A.L. VR, which
// probes with a throwaway instance on every launch: five runs armed at launch,
// five hangs on the second create, and no hang in any run where the connection
// had not been opened. That is what SteamVrDelivery::mark_established() is
// for - the connection still never closes, it just never opens under a session
// that was only ever a probe.
struct ProcessConnection {
    std::mutex mutex;
    bool attempted{};
    bool usable{};
    HMODULE module{};
    vr::IVRCompositor* compositor{};
    vr::IVRSystem* system{};
    AttachRoute route{AttachRoute::none};
    // Only the background_init route owns anything to close. An existing_token
    // context was borrowed from the application and is not ours to shut down.
    PfnShutdownInternal shutdown{};
    bool owns_context{};
};

[[nodiscard]] ProcessConnection& process_connection() noexcept {
    static ProcessConnection connection;
    return connection;
}

} // namespace

struct SteamVrDelivery::Impl {
    std::mutex mutex;
    bool steamvr{};
    bool attempted{};
    bool usable{};
    HMODULE module{};
    PfnShutdownInternal shutdown{};
    vr::IVRCompositor* compositor{};
    // Held separately because GetTimeSinceLastVsync is on IVRSystem, not the
    // compositor. Its absence is not fatal: the delivered rate still works.
    vr::IVRSystem* system{};
    // display_period()'s cache.
    std::optional<std::chrono::nanoseconds> display_period;
    std::chrono::steady_clock::time_point display_period_read{};
    // The physical director must not take mutex: optional compositor polling
    // holds it across OpenVR calls, which can stall inside the runtime.
    std::atomic<std::int64_t> display_period_snapshot{};
    std::atomic<std::uint64_t> vsync_version{},vsync_counter{};
    std::atomic<std::int64_t> vsync_at_ns{};
    AttachRoute route{AttachRoute::none};
    bool owns_context{};
    ScenePresentationRate scene_receipts;
    std::optional<ScenePresentationRate::Rate> scene_rate;

    float delivered{};
    std::int64_t delivered_at_ns{-1};

    std::uint32_t last_frame_index{};
    bool reported_frames{};
    // settled_frames() keeps its own cursor, so it never takes a frame away
    // from last_presentation() or the other way round.
    std::uint32_t settled_last_index{};
    bool settled_reported{};

    std::optional<CompositorPresentationRate> presentation_rate;

    // Until the session proves it will be shown, no accessor may attach. See
    // SteamVrDelivery::mark_established().
    bool established{};

    void attach() noexcept {
        attempted = true;
        if (!steamvr) {
            return;
        }
        ProcessConnection& shared = process_connection();
        std::scoped_lock connection_lock(shared.mutex);
        if (shared.attempted) {
            // Second and later sessions borrow what the first one opened.
            compositor = shared.compositor;
            system = shared.system;
            route = shared.route;
            usable = shared.usable;
            return;
        }
        // Latched on success only, at the tail. A failure here is usually
        // SteamVR not being up yet rather than anything permanent, and
        // applications create sessions early - Assetto Corsa makes three
        // before it settles. Latching on the first attempt would give the
        // whole process no delivery interface for the rest of its life on a
        // transient miss, and that failure is silent: no delivery means
        // measured pacing never engages, so SteamVR quietly reverts to the
        // behaviour this path exists to replace with nothing in the overlay to
        // say so. The per-session latch below still stops one session
        // retrying on every frame.
        const std::string runtime = runtime_path_from_vrpath();
        if (runtime.empty()) {
            bridge_flight_logger().event(
                BridgeFlightOperation::steamvr_delivery_attach, -1);
            return;
        }
#if defined(_WIN64)
        const std::string dll = runtime + "\\bin\\win64\\openvr_api.dll";
#else
        // 0.2 V576: the 32-bit layer loads SteamVR's 32-bit API.
        const std::string dll = runtime + "\\bin\\win32\\openvr_api.dll";
#endif
        module = LoadLibraryA(dll.c_str());
        if (module == nullptr) {
            bridge_flight_logger().event(
                BridgeFlightOperation::steamvr_delivery_attach,
                -2,
                GetLastError());
            return;
        }
        const auto generic = reinterpret_cast<PfnGetGenericInterface>(
            reinterpret_cast<void*>(
                GetProcAddress(module, "VR_GetGenericInterface")));
        const auto token = reinterpret_cast<PfnGetInitToken>(
            reinterpret_cast<void*>(GetProcAddress(module, "VR_GetInitToken")));
        if (generic == nullptr) {
            bridge_flight_logger().event(
                BridgeFlightOperation::steamvr_delivery_attach, -3);
            return;
        }

        // Borrow an existing context before making one. A second connection
        // from a process that is already a scene application is the risk in
        // this whole path, so it is the fallback rather than the first move.
        vr::EVRInitError error = vr::VRInitError_None;
        if (token != nullptr && token() != 0) {
            compositor = static_cast<vr::IVRCompositor*>(
                generic(vr::IVRCompositor_Version, &error));
            if (compositor != nullptr) {
                route = AttachRoute::existing_token;
            }
        }
        if (compositor == nullptr) {
            const auto init = reinterpret_cast<PfnInitInternal2>(
                reinterpret_cast<void*>(
                    GetProcAddress(module, "VR_InitInternal2")));
            shutdown = reinterpret_cast<PfnShutdownInternal>(
                reinterpret_cast<void*>(
                    GetProcAddress(module, "VR_ShutdownInternal")));
            if (init == nullptr || shutdown == nullptr) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::steamvr_delivery_attach, -4);
                return;
            }
            error = vr::VRInitError_None;
            init(&error, vr::VRApplication_Background, nullptr);
            if (error != vr::VRInitError_None) {
                bridge_flight_logger().event(
                    BridgeFlightOperation::steamvr_delivery_attach,
                    -5,
                    static_cast<std::uint64_t>(error));
                shutdown = nullptr;
                return;
            }
            owns_context = true;
            error = vr::VRInitError_None;
            compositor = static_cast<vr::IVRCompositor*>(
                generic(vr::IVRCompositor_Version, &error));
            route = AttachRoute::background_init;
        }
        if (compositor == nullptr) {
            bridge_flight_logger().event(
                BridgeFlightOperation::steamvr_delivery_attach,
                -6,
                static_cast<std::uint64_t>(error));
            return;
        }
        error = vr::VRInitError_None;
        system = static_cast<vr::IVRSystem*>(
            generic(vr::IVRSystem_Version, &error));
        usable = true;
        shared.module = module;
        shared.compositor = compositor;
        shared.system = system;
        shared.route = route;
        shared.usable = true;
        shared.attempted = true;
        shared.shutdown = shutdown;
        shared.owns_context = owns_context;
        bridge_flight_logger().event(
            BridgeFlightOperation::steamvr_delivery_attach,
            0,
            static_cast<std::uint64_t>(route),
            GetCurrentProcessId(),
            system != nullptr ? 1 : 0);
    }

    void poll_window() noexcept {
        std::array<vr::Compositor_FrameTiming, 64> window{};
        window[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const auto filled = compositor->GetFrameTimings(window.data(), 64);
        presentation_rate = filled <= window.size()
            ? compositor_presentation_rate(std::span<const vr::Compositor_FrameTiming>(window.data(), filled))
            : std::nullopt;
        scene_rate=filled<=window.size() ? scene_receipts.rate(
            std::span<const vr::Compositor_FrameTiming>(window.data(),filled)) : std::nullopt;
    }

    void close_window(std::int64_t now) noexcept {
        if (!presentation_rate) return;
        delivered = presentation_rate->fps;
        delivered_at_ns = now;
        // b is informational mispresentation count, never a deduction from
        // frames OpenVR explicitly says were presented.
        bridge_flight_logger().event(BridgeFlightOperation::steamvr_delivery, 0,
            static_cast<std::uint64_t>(delivered * 1000.0F),
            presentation_rate->mispresented, presentation_rate->intervals);
    }

};

SteamVrDelivery::SubmissionProbe SteamVrDelivery::scene_submission_probe() noexcept {
    try {
        if(!impl_)return {};
        std::scoped_lock lock(impl_->mutex);
        if(!impl_->established)return {};
        if(!impl_->attempted)impl_->attach();
        if(!impl_->usable||!impl_->compositor)return {};
        vr::Compositor_CumulativeStats stats{};
        impl_->compositor->GetCumulativeStats(&stats,sizeof(stats));
        if(stats.m_nPid!=GetCurrentProcessId())return {};
        vr::Compositor_FrameTiming t{};t.m_nSize=sizeof(t);
        if(!impl_->compositor->GetFrameTiming(&t,0))return {};
        return {t.m_nFrameIndex,t.m_flSystemTimeInSeconds,t.m_flNewFrameReadyMs,true};
    }catch(...){return {};}
}
void SteamVrDelivery::scene_submission_completed(SubmissionProbe before,SceneIdentity scene) noexcept {
    const auto after=scene_submission_probe();
    // A changed current record (or a newly filled ready field in that same
    // record) witnesses the synchronous scene submission. Unchanged/stale
    // records and asynchronous handoffs cannot be attributed by guessing.
    const bool matched=before.valid&&after.valid&&after.frame>=before.frame&&std::isfinite(after.ready)&&after.ready!=0&&
        !(after.frame==before.frame&&after.time==before.time&&after.ready==before.ready);
    bridge_flight_logger().event(BridgeFlightOperation::presenter_source,matched?5650:5651,
        before.frame,after.frame,(before.valid?1U:0U)|(after.valid?2U:0U));
    if(!matched)return;
    try {std::scoped_lock lock(impl_->mutex);impl_->scene_receipts.record(after.frame,scene);
        bridge_flight_logger().event(BridgeFlightOperation::presenter_source,5652,
            after.frame,scene.owner,scene.serial);}
    catch(...){}
}
std::optional<ScenePresentationRate::Rate> SteamVrDelivery::scene_fps(std::int64_t now) noexcept {
    try {
        if(!delivered_fps(now))return {};
        std::scoped_lock lock(impl_->mutex);return impl_->scene_rate;
    }catch(...){return {};}
}

SteamVrDelivery::SteamVrDelivery(bool steamvr_runtime) noexcept
    : impl_(nullptr) {
    try {
        impl_ = std::make_unique<Impl>();
        impl_->steamvr = steamvr_runtime;
    } catch (...) {
    }
}

SteamVrDelivery::~SteamVrDelivery() {
    if (!impl_) {
        return;
    }
    // The connection is process-wide and is never torn down here; see
    // ProcessConnection. Shutting it down between sessions is what crashed
    // SteamVR's own runtime on the next xrCreateSession, and unloading the
    // module would do the same by another route. Only the per-session
    // accounting dies with this object.
}

void SteamVrDelivery::release_process_connection() noexcept {
    ProcessConnection& shared = process_connection();
    std::scoped_lock lock(shared.mutex);
    if (!shared.attempted) {
        return;
    }
    // Every session is destroyed before the instance that owns it, so no Impl
    // still holds these pointers by the time this runs. Both captures show the
    // order directly: session_destroy completes, then instance_destroy begins.
    const AttachRoute released = shared.route;
    const bool owned = shared.owns_context;
    if (owned && shared.shutdown != nullptr) {
        shared.shutdown();
    }
    shared.attempted = false;
    shared.usable = false;
    shared.compositor = nullptr;
    shared.system = nullptr;
    shared.route = AttachRoute::none;
    shared.shutdown = nullptr;
    shared.owns_context = false;
    // The module handle is kept: openvr_api.dll stays loaded and a reopen
    // resolves its exports again rather than reloading it.
    //
    // result=1 is a release, against 0 for an attach and negatives for the
    // ways an attach can fail. a is the route that was let go, c whether it
    // was ours to close.
    bridge_flight_logger().event(
        BridgeFlightOperation::steamvr_delivery_attach,
        1,
        static_cast<std::uint64_t>(released),
        GetCurrentProcessId(),
        owned ? 1 : 0);
}

void SteamVrDelivery::mark_established() noexcept {
    try {
        if (!impl_) {
            return;
        }
        std::scoped_lock lock(impl_->mutex);
        impl_->established = true;
    } catch (...) {
    }
}

std::optional<float> SteamVrDelivery::delivered_fps(std::int64_t now) noexcept {
    try {
        if (!impl_) {
            return std::nullopt;
        }
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->established) {
            return std::nullopt;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable) {
            return std::nullopt;
        }
        impl_->poll_window();
        impl_->close_window(now);
        if (impl_->delivered_at_ns < 0 ||
            now - impl_->delivered_at_ns > kStaleNanoseconds) {
            return std::nullopt;
        }
        return impl_->delivered;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SteamVrDelivery::VsyncAnchor>
SteamVrDelivery::vsync_anchor(bool nonblocking) noexcept {
    try {
        if (!impl_) {
            return std::nullopt;
        }
        std::unique_lock lock(impl_->mutex,std::defer_lock);
        if(nonblocking){if(!lock.try_lock())return std::nullopt;}else lock.lock();
        if (!impl_->established) {
            return std::nullopt;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable || impl_->system == nullptr) {
            return std::nullopt;
        }
        float seconds = 0.0F;
        std::uint64_t counter = 0;
        const auto entered = std::chrono::steady_clock::now();
        // Read the clock immediately after, not before: the value is relative
        // to the moment of the call, and anything between the two becomes
        // phase error in whatever uses it.
        const bool ok = impl_->system->GetTimeSinceLastVsync(&seconds, &counter);
        const auto now = std::chrono::steady_clock::now();
        // Documented to return false with zeroes when the runtime has no vsync
        // times. A negative or absurd value would be worse than none, since a
        // caller would treat it as a real anchor.
        if (!ok || !(seconds >= 0.0F) || seconds > 1.0F) {
            return std::nullopt;
        }
        VsyncAnchor anchor{};
        anchor.at = now - std::chrono::nanoseconds(
            static_cast<std::int64_t>(static_cast<double>(seconds) * 1e9));
        anchor.frame_counter = counter;
        anchor.cost = std::chrono::duration_cast<std::chrono::microseconds>(
            now - entered);
        impl_->vsync_version.fetch_add(1,std::memory_order_acq_rel);
        impl_->vsync_counter.store(counter,std::memory_order_relaxed);
        impl_->vsync_at_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(anchor.at.time_since_epoch()).count(),std::memory_order_relaxed);
        impl_->vsync_version.fetch_add(1,std::memory_order_release);
        return anchor;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SteamVrDelivery::VsyncAnchor> SteamVrDelivery::cached_vsync_anchor() const noexcept {
    if(!impl_)return {};
    for(unsigned attempt=0;attempt<3;++attempt) {
        const auto before=impl_->vsync_version.load(std::memory_order_acquire);
        if(before&1)continue;
        const auto at=impl_->vsync_at_ns.load(std::memory_order_relaxed);
        const auto counter=impl_->vsync_counter.load(std::memory_order_relaxed);
        if(before!=impl_->vsync_version.load(std::memory_order_acquire))continue;
        if(at<=0 || !counter)return {};
        VsyncAnchor result;result.at=std::chrono::steady_clock::time_point(std::chrono::nanoseconds(at));result.frame_counter=counter;return result;
    }
    return {};
}

std::optional<std::chrono::nanoseconds>
SteamVrDelivery::display_period() noexcept {
    try {
        if (!impl_) {
            return std::nullopt;
        }
        std::unique_lock lock(impl_->mutex, std::try_to_lock);
        if (!lock) return cached_display_period();
        if (!impl_->established) {
            return std::nullopt;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable || impl_->system == nullptr) {
            return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        if (impl_->display_period_read != std::chrono::steady_clock::time_point{} &&
            now - impl_->display_period_read < std::chrono::seconds(2)) {
            return impl_->display_period;
        }
        impl_->display_period_read = now;
        vr::ETrackedPropertyError error = vr::TrackedProp_Success;
        const float hertz = impl_->system->GetFloatTrackedDeviceProperty(
            vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &error);
        if (error != vr::TrackedProp_Success || !(hertz >= 20.0F) || hertz > 500.0F) {
            impl_->display_period.reset();
            impl_->display_period_snapshot.store(0, std::memory_order_release);
            return std::nullopt;
        }
        impl_->display_period = std::chrono::nanoseconds(
            static_cast<std::int64_t>(1e9 / static_cast<double>(hertz)));
        impl_->display_period_snapshot.store(impl_->display_period->count(), std::memory_order_release);
        return impl_->display_period;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::chrono::nanoseconds>
SteamVrDelivery::cached_display_period() const noexcept {
    if (!impl_) return std::nullopt;
    const auto period = impl_->display_period_snapshot.load(std::memory_order_acquire);
    return period > 0 ? std::optional{std::chrono::nanoseconds(period)} : std::nullopt;
}

std::optional<std::chrono::nanoseconds>
SteamVrDelivery::frame_time_remaining() noexcept {
    try {
        if (!impl_) {
            return std::nullopt;
        }
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->established) {
            return std::nullopt;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable || impl_->compositor == nullptr) {
            return std::nullopt;
        }
        const float seconds = impl_->compositor->GetFrameTimeRemaining();
        if (!(seconds > -1.0F) || seconds > 1.0F) {
            return std::nullopt;
        }
        return std::chrono::nanoseconds(
            static_cast<std::int64_t>(static_cast<double>(seconds) * 1e9));
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<SteamVrDelivery::FramePresentation>
SteamVrDelivery::settled_frames() noexcept {
    std::vector<FramePresentation> output;
    try {
        if (!impl_) {
            return output;
        }
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->established) {
            return output;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable) {
            return output;
        }
        constexpr std::uint32_t kLookback = 64;
        std::array<vr::Compositor_FrameTiming, kLookback> timings{};
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const std::uint32_t filled =
            impl_->compositor->GetFrameTimings(timings.data(), kLookback);
        if (filled == 0 || filled > kLookback) {
            return output;
        }
        // Ascending, oldest to newest. A frame is settled once a newer one has
        // been presented: the newest ones report zero presents only because
        // nothing has happened to them yet. Everything up to and including the
        // newest presented frame is final, and a zero there is a frame the
        // compositor had and never showed.
        std::uint32_t newest_presented = filled;
        for (std::uint32_t offset = filled; offset-- > 0;) {
            if (timings[offset].m_nNumFramePresents != 0) {
                newest_presented = offset;
                break;
            }
        }
        if (newest_presented == filled) {
            return output;
        }
        for (std::uint32_t offset = 0; offset <= newest_presented; ++offset) {
            const auto& timing = timings[offset];
            if (impl_->settled_reported &&
                timing.m_nFrameIndex <= impl_->settled_last_index) {
                continue;
            }
            FramePresentation frame{};
            frame.frame_index = timing.m_nFrameIndex;
            frame.mispresented = timing.m_nNumMisPresented;
            frame.presents = timing.m_nNumFramePresents;
            frame.dropped = timing.m_nNumDroppedFrames;
            frame.skipped = impl_->settled_reported
                ? timing.m_nFrameIndex - impl_->settled_last_index - 1
                : 0;
            frame.reprojection_flags = timing.m_nReprojectionFlags;
            frame.total_render_gpu_us = static_cast<std::uint32_t>(
                std::clamp(static_cast<double>(timing.m_flTotalRenderGpuMs) * 1000.0,
                    0.0, 1'000'000'000.0));
            frame.compositor_render_gpu_us = static_cast<std::uint32_t>(
                std::clamp(static_cast<double>(timing.m_flCompositorRenderGpuMs) * 1000.0,
                    0.0, 1'000'000'000.0));
            frame.ready_vsyncs = timing.m_nNumVSyncsReadyForUse;
            frame.vsyncs_to_first_view = timing.m_nNumVSyncsToFirstView;
            frame.system_time_seconds = timing.m_flSystemTimeInSeconds;
            frame.wait_get_poses_called_ms = timing.m_flWaitGetPosesCalledMs;
            frame.new_poses_ready_ms = timing.m_flNewPosesReadyMs;
            frame.new_frame_ready_ms = timing.m_flNewFrameReadyMs;
            frame.compositor_update_start_ms =
                timing.m_flCompositorUpdateStartMs;
            frame.compositor_update_end_ms = timing.m_flCompositorUpdateEndMs;
            frame.compositor_render_start_ms =
                timing.m_flCompositorRenderStartMs;
            frame.client_frame_interval_ms = timing.m_flClientFrameIntervalMs;
            frame.compositor_idle_cpu_ms = timing.m_flCompositorIdleCpuMs;
            output.push_back(frame);
            impl_->settled_last_index = timing.m_nFrameIndex;
            impl_->settled_reported = true;
        }
    } catch (...) {
    }
    return output;
}

std::optional<SteamVrDelivery::GpuRenderTiming> SteamVrDelivery::gpu_render_timing(std::int64_t now) noexcept {
    try {
        if (!impl_) return std::nullopt;
        std::unique_lock lock(impl_->mutex, std::try_to_lock);
        if (!lock || !impl_->established) return std::nullopt;
        if (!impl_->attempted) impl_->attach();
        if (!impl_->usable || !impl_->compositor) return std::nullopt;
        vr::Compositor_FrameTiming timing{};
        timing.m_nSize = sizeof(timing);
        if (!impl_->compositor->GetFrameTiming(&timing, 1)) return std::nullopt;
        GpuRenderTiming result{timing.m_nFrameIndex, timing.m_flPreSubmitGpuMs};
        // Observe the existing delivery cache; never close its window or consume
        // either presentation cursor for a UI request.
        if (impl_->delivered_at_ns >= 0 && now >= impl_->delivered_at_ns && now - impl_->delivered_at_ns <= kStaleNanoseconds)
            result.delivered_fps = impl_->delivered;
        if (impl_->system) {
            vr::ETrackedPropertyError error{};
            const auto hz = impl_->system->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                vr::Prop_DisplayFrequency_Float, &error);
            if (error == vr::TrackedProp_Success && std::isfinite(hz) && hz > 0)
                result.display_period = std::chrono::nanoseconds(static_cast<std::int64_t>(1.0e9 / hz));
            if (error == vr::TrackedProp_Success && std::isfinite(hz) && hz >= 20 && hz <= 500) {
                impl_->display_period = std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / static_cast<double>(hz)));
                impl_->display_period_snapshot.store(impl_->display_period->count(), std::memory_order_release);
                impl_->display_period_read = std::chrono::steady_clock::now();
            } else {
                impl_->display_period.reset();
                impl_->display_period_snapshot.store(0, std::memory_order_release);
            }
        }
        return result;
    } catch (...) { return std::nullopt; }
}

std::optional<SteamVrDelivery::FramePresentation>
SteamVrDelivery::last_presentation() noexcept {
    try {
        if (!impl_) {
            return std::nullopt;
        }
        std::scoped_lock lock(impl_->mutex);
        if (!impl_->established) {
            return std::nullopt;
        }
        if (!impl_->attempted) {
            impl_->attach();
        }
        if (!impl_->usable) {
            return std::nullopt;
        }
        // Frames ago 0 is the one still in flight: nothing has happened to it
        // yet, so it reports zero presents and zero mispresents whatever the
        // compositor ends up doing. Reading it made this record uniformly zero
        // across 6161 submissions while frames were demonstrably being lost.
        // Walk back to the newest frame that has actually been presented.
        constexpr std::uint32_t kLookback = 8;
        std::array<vr::Compositor_FrameTiming, kLookback> timings{};
        timings[0].m_nSize = sizeof(vr::Compositor_FrameTiming);
        const std::uint32_t filled =
            impl_->compositor->GetFrameTimings(timings.data(), kLookback);
        if (filled == 0 || filled > kLookback) {
            return std::nullopt;
        }
        // Ascending, oldest to newest.
        // Every frame in the window, not just the one reported below. The
        // caller looks once per submission and the window holds more than one
        // frame, so a reason raised on a frame it does not land on would
        // otherwise never be seen.
        std::uint32_t flags_window = 0;
        for (std::uint32_t i = 0; i < filled; ++i) {
            if (timings[i].m_nNumFramePresents != 0) {
                flags_window |= timings[i].m_nReprojectionFlags;
            }
        }

        for (std::uint32_t offset = filled; offset-- > 0;) {
            const auto& timing = timings[offset];
            if (timing.m_nNumFramePresents == 0) {
                continue;
            }
            if (impl_->reported_frames && 
                timing.m_nFrameIndex <= impl_->last_frame_index) {
                return std::nullopt; // Nothing has settled since the last call.
            }
            FramePresentation presentation{};
            presentation.frame_index = timing.m_nFrameIndex;
            presentation.mispresented = timing.m_nNumMisPresented;
            presentation.presents = timing.m_nNumFramePresents;
            presentation.dropped = timing.m_nNumDroppedFrames;
            presentation.skipped = impl_->reported_frames
                ? timing.m_nFrameIndex - impl_->last_frame_index - 1
                : 0;
            presentation.reprojection_flags = timing.m_nReprojectionFlags;
            presentation.reprojection_flags_window = flags_window;
            presentation.total_render_gpu_us = static_cast<std::uint32_t>(
                timing.m_flTotalRenderGpuMs * 1000.0F);
            presentation.compositor_render_gpu_us = static_cast<std::uint32_t>(
                timing.m_flCompositorRenderGpuMs * 1000.0F);
            presentation.ready_vsyncs = timing.m_nNumVSyncsReadyForUse;
            presentation.vsyncs_to_first_view = timing.m_nNumVSyncsToFirstView;
            presentation.system_time_seconds = timing.m_flSystemTimeInSeconds;
            presentation.wait_get_poses_called_ms =
                timing.m_flWaitGetPosesCalledMs;
            presentation.new_poses_ready_ms = timing.m_flNewPosesReadyMs;
            presentation.new_frame_ready_ms = timing.m_flNewFrameReadyMs;
            presentation.compositor_update_start_ms =
                timing.m_flCompositorUpdateStartMs;
            presentation.compositor_update_end_ms =
                timing.m_flCompositorUpdateEndMs;
            presentation.compositor_render_start_ms =
                timing.m_flCompositorRenderStartMs;
            presentation.client_frame_interval_ms =
                timing.m_flClientFrameIntervalMs;
            presentation.compositor_idle_cpu_ms = timing.m_flCompositorIdleCpuMs;
            impl_->last_frame_index = timing.m_nFrameIndex;
            impl_->reported_frames = true;
            return presentation;
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace xrfg
