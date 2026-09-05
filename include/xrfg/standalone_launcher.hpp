#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace xrfg::standalone {

inline constexpr wchar_t kLayerName[] =
    L"XR_APILAYER_XRFrameBridge_diagnostic";

enum class FlowBackend {
    fidelity_fx,
    nvidia,
};

enum class NvidiaPerformancePreset {
    slow,
    medium,
};

enum class NvidiaInputScale {
    full,
    three_quarter,
    half,
};

struct LauncherSettings {
    FlowBackend backend{FlowBackend::fidelity_fx};
    NvidiaPerformancePreset nvidia_preset{NvidiaPerformancePreset::medium};
    NvidiaInputScale nvidia_input_scale{NvidiaInputScale::half};
    bool nvidia_bidirectional{};
    bool diagnostics{};
};

[[nodiscard]] std::string backend_ini_value(FlowBackend backend);
[[nodiscard]] std::string nvidia_preset_ini_value(
    NvidiaPerformancePreset preset);
[[nodiscard]] std::string nvidia_input_scale_ini_value(
    NvidiaInputScale scale);
[[nodiscard]] LauncherSettings parse_settings(std::string_view text);
[[nodiscard]] std::string serialize_settings(const LauncherSettings& settings);

[[nodiscard]] std::string build_implicit_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version);

[[nodiscard]] std::string build_runtime_ini(
    const LauncherSettings& settings);

[[nodiscard]] std::filesystem::path runtime_version_directory(
    const std::filesystem::path& local_directory,
    std::uint32_t implementation_version);

// Replaces an existing cached layer instead of silently retaining it.
[[nodiscard]] bool install_runtime_layer_dll(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) noexcept;

[[nodiscard]] std::wstring quote_windows_argument(std::wstring_view argument);

} // namespace xrfg::standalone
