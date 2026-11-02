#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace xrfg::implicit_layer {

// One manual-reset kernel event per arm. Old readers retain their own event;
// re-arming cannot accidentally reactivate a session that was already stopped.
[[nodiscard]] std::wstring arm_signal_name(const std::filesystem::path& manifest);
[[nodiscard]] void* create_arm_signal(const std::filesystem::path& manifest,
                                      std::wstring* error = nullptr) noexcept;
[[nodiscard]] bool signal_arm_stop(const std::filesystem::path& manifest,
                                    std::wstring* error = nullptr) noexcept;
// The tray's "Pause frame generation" switch: a second manual-reset event
// per arm, set while paused and reset on resume. Unlike the stop it goes both
// ways, and a session that cannot open it is simply never paused. The name
// is the arm signal's with another prefix, so the layer finds it from the
// same `control_event` and nothing else has to travel through the ini.
[[nodiscard]] std::wstring pause_signal_name(const std::filesystem::path& manifest);
[[nodiscard]] void* create_pause_signal(const std::filesystem::path& manifest,
                                        std::wstring* error = nullptr) noexcept;
class ManualArmControl {
public:
    explicit ManualArmControl(const std::filesystem::path& module_directory) noexcept;
    ~ManualArmControl();
    ManualArmControl(const ManualArmControl&) = delete;
    ManualArmControl& operator=(const ManualArmControl&) = delete;
    [[nodiscard]] bool stop_requested() const noexcept;
    [[nodiscard]] bool pause_requested() const noexcept;
private:
    void* event_{};
    void* pause_event_{};
    bool managed_{};
};

inline constexpr wchar_t kRegistrySubkey[] =
    L"SOFTWARE\\Khronos\\OpenXR\\1\\ApiLayers\\Implicit";
inline constexpr wchar_t kManifestPrefix[] =
    L"XR_APILAYER_OFXRBridge_manual-";
inline constexpr wchar_t kManifestSuffix[] = L".json";
// The Vulkan implicit layer that serialises queue submissions, registered
// beside the OpenXR one while the bridge is armed with Vulkan support on.
// Same value scheme: the manifest path as the value name, DWORD 0.
inline constexpr wchar_t kVulkanRegistrySubkey[] =
    L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
inline constexpr wchar_t kVulkanManifestPrefix[] =
    L"OFXR_vulkan_queue_manual-";
inline constexpr wchar_t kVulkanLayerName[] = L"VK_LAYER_OFXRBridge_queue_serialize";
inline constexpr wchar_t kVulkanLayerDll[] = L"OFXR_vulkan_queue_layer.dll";

enum class RegistryScope {
    current_user,
    local_machine,
};

[[nodiscard]] RegistryScope registry_scope_for_integrity_rid(
    std::uint32_t integrity_rid) noexcept;
[[nodiscard]] RegistryScope preferred_registry_scope() noexcept;
[[nodiscard]] std::wstring_view registry_scope_name(RegistryScope scope) noexcept;

enum class ConfiguredFlowBackend {
    fidelity_fx,
    nvidia,
};

enum class ConfiguredFrameGeneration { ofxr, native_dlss };
[[nodiscard]] ConfiguredFrameGeneration read_frame_generation(
    const std::filesystem::path& module_directory) noexcept;
// Native DLSS Frame Generation's resolution in percent of each eye's:
// [ofxr] dlssg_resolution, 25 to 100, by default 100.
inline constexpr int kMinNativeDlssgScale = 25;
[[nodiscard]] int read_native_dlssg_scale(
    const std::filesystem::path& module_directory) noexcept;

enum class ConfiguredNvidiaPerformancePreset {
    slow,
    medium,
    fast,
};

enum class ConfiguredNvidiaInputScale {
    full,
    three_quarter,
    half,
    quarter,
    fifth, // RIFE only
};

struct ConfiguredNvidiaOptions {
    ConfiguredNvidiaPerformancePreset preset{
        ConfiguredNvidiaPerformancePreset::medium};
    ConfiguredNvidiaInputScale input_scale{
        ConfiguredNvidiaInputScale::half};
    bool bidirectional{};
};

[[nodiscard]] ConfiguredFlowBackend read_flow_backend(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] deep_pipeline`: one display period of extra depth, bought with one
// display period of latency. On unless set to 0. Read once per session, at
// xrCreateSession; the tray calls it "Prefer FPS over latency".
[[nodiscard]] bool read_deep_pipeline(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] triple_frame_gen`: two synthetic frames per application frame
// instead of one, for an application running at a third of the display rate.
// Off unless set to 1. Read at xrCreateSession and then followed while the
// session runs, where the session can take either shape; it takes the
// shallow pipeline whatever deep_pipeline says. The tray calls it
// "3X Frame Gen".
[[nodiscard]] bool read_triple_frame_gen(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] vulkan_session_bridge`: a Vulkan game's session is handed to the
// runtime as a D3D12 one on the layer's device, the game rendering into
// Vulkan imports of the layer's shared textures, so every path downstream is
// the native D3D12 one and nothing is mirrored. On unless set to 0; needs
// vulkan_bridge (Vulkan support) on as well. Read at xrCreateInstance.
[[nodiscard]] bool read_vulkan_session_bridge(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] single_swapchain_rings`: one private swapchain per output with
// staging textures instead of two per output, on the paths where the
// synthesizer writes D3D12 images directly. On unless set to 0. Read at
// xrCreateSession.
[[nodiscard]] bool read_single_swapchain_rings(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] capture_at_end_frame`: on a native D3D12 session the history
// capture of an application image is queued at the application's xrEndFrame
// rather than at its xrReleaseSwapchainImage. A release only promises that
// the image's rendering has been *submitted*; an application whose
// submission thread trails the one that releases can release first, and a
// capture queued then holds the previous frame for that image. The runtime
// only reads at xrEndFrame and never sees that. On unless set to 0. Read at
// xrCreateSession.
[[nodiscard]] bool read_capture_at_end_frame(
    const std::filesystem::path& module_directory) noexcept;

// `[ofxr] excluded_processes`: executable names, separated by ';', in which
// the layer declines to load at negotiation. Absent, it is Pimax Home: a
// home environment is an OpenXR application like any other, takes the
// headset the moment a game leaves VR, and would otherwise carry the
// layer's buffers at the headset's full resolution - about 4 GB at a
// Crystal's - for as long as the game is out of VR, on top of the game's
// own. Written out explicitly empty, it excludes nothing.
inline constexpr wchar_t kDefaultExcludedProcesses[] = L"PimaxHome-Win64-Shipping.exe";
[[nodiscard]] std::vector<std::wstring> read_excluded_processes(
    const std::filesystem::path& module_directory) noexcept;

// Whether an executable's file name is in the list, compared without case.
[[nodiscard]] bool executable_is_excluded(
    std::wstring_view executable,
    const std::vector<std::wstring>& excluded) noexcept;

// This process's executable file name, without its directory.
[[nodiscard]] std::wstring current_executable_name() noexcept;

// Held by every running session that cannot follow the "3X Frame Gen"
// switch live - one started with "Prefer FPS over latency" off, or one the
// runtime's swapchain limit pushed back to a single synthetic. The tray asks
// whether any exists to tell the user a game restart is needed. A named
// event, so the handle count is the session count and nothing outlives the
// last session.
class FixedFrameMultiplierMarker {
public:
    FixedFrameMultiplierMarker() noexcept = default;
    ~FixedFrameMultiplierMarker();
    FixedFrameMultiplierMarker(const FixedFrameMultiplierMarker&) = delete;
    FixedFrameMultiplierMarker& operator=(const FixedFrameMultiplierMarker&) = delete;
    void hold() noexcept;
    void release() noexcept;

private:
    void* event_{};
};

// Whether a running game holds the marker above.
[[nodiscard]] bool running_session_needs_restart_for_3x() noexcept;

// `[ofxr] vulkan_bridge`: whether the layer generates for Vulkan sessions.
// On unless the key says 0. A Vulkan session needs the queue-serialising
// Vulkan layer registered beside this one, which the tray does at every arm
// unless tray.ini says vulkan_bridge=0; without it the presenter's
// submissions race the game's on its queue.
// [ofxr] d3d11_bridge: a D3D11 session is given to the runtime as a D3D12
// one, with the application's images bridged through shared textures. On
// unless the key says 0; kept as a switch for diagnosing a title against the
// direct D3D11 path.
[[nodiscard]] bool read_d3d11_bridge(
    const std::filesystem::path& module_directory) noexcept;
[[nodiscard]] bool read_vulkan_support(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] ConfiguredNvidiaOptions read_nvidia_options(
    const std::filesystem::path& module_directory) noexcept;

[[nodiscard]] bool register_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool unregister_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

[[nodiscard]] bool manifest_registered(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

// prefix says which of the tray's manifests: the OpenXR layer's
// (kManifestPrefix) or the Vulkan layer's (kVulkanManifestPrefix).
[[nodiscard]] bool owned_manifest_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& runtime_directory,
    std::wstring_view prefix = kManifestPrefix) noexcept;

// Recognizes only tray-owned manual manifests in RuntimeLayer (legacy) or
// RuntimeLayer/vNNN. Does not require the manifest or DLL to still exist.
[[nodiscard]] bool owned_registration_path(
    const std::filesystem::path& manifest,
    const std::filesystem::path& local_directory,
    std::wstring_view prefix = kManifestPrefix) noexcept;

// Disable before deleting; read back registry state, and retain the JSON when
// deregistration fails. The caller must report a false result, never claim Off.
[[nodiscard]] bool retire_manifest(
    const std::filesystem::path& manifest,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey) noexcept;

// Registry-first recovery across ALL versions, including missing JSON files.
// Removes only owned registration values and owned JSONs, never DLLs/settings.
[[nodiscard]] bool cleanup_owned_registrations(
    const std::filesystem::path& local_directory,
    RegistryScope scope,
    std::wstring* error = nullptr,
    std::wstring_view registry_subkey = kRegistrySubkey,
    std::wstring_view prefix = kManifestPrefix) noexcept;

} // namespace xrfg::implicit_layer
