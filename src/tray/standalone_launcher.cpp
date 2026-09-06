#include "xrfg/standalone_launcher.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace xrfg::standalone {
namespace {

[[nodiscard]] std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        throw std::runtime_error("WideCharToMultiByte failed");
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            output.data(),
            required,
            nullptr,
            nullptr) != required) {
        throw std::runtime_error("WideCharToMultiByte returned a short result");
    }
    return output;
}

[[nodiscard]] std::string trim_ascii(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

[[nodiscard]] std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](char character) {
        return static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    });
    return value;
}

[[nodiscard]] std::string escape_json(std::string_view value) {
    constexpr char hexadecimal[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() + 16);
    for (const unsigned char character : value) {
        switch (character) {
        case '\"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20) {
                output += "\\u00";
                output.push_back(hexadecimal[(character >> 4) & 0x0F]);
                output.push_back(hexadecimal[character & 0x0F]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

} // namespace

std::string backend_ini_value(FlowBackend backend) {
    return backend == FlowBackend::nvidia ? "nvidia" : "fidelityfx";
}

std::string nvidia_preset_ini_value(NvidiaPerformancePreset preset) {
    switch (preset) {
    case NvidiaPerformancePreset::slow:
        return "slow";
    case NvidiaPerformancePreset::fast:
        return "fast";
    case NvidiaPerformancePreset::medium:
    default:
        return "medium";
    }
}

std::string nvidia_input_scale_ini_value(NvidiaInputScale scale) {
    switch (scale) {
    case NvidiaInputScale::three_quarter:
        return "75";
    case NvidiaInputScale::half:
        return "50";
    case NvidiaInputScale::full:
    default:
        return "100";
    }
}

LauncherSettings parse_settings(std::string_view text) {
    LauncherSettings settings;
    std::string section;
    std::size_t offset = 0;
    while (offset <= text.size()) {
        const std::size_t newline = text.find('\n', offset);
        const std::size_t length = newline == std::string_view::npos
            ? text.size() - offset
            : newline - offset;
        std::string line = trim_ascii(text.substr(offset, length));
        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            section = lower_ascii(trim_ascii(
                std::string_view(line).substr(1, line.size() - 2)));
        } else if (section == "tray" && !line.empty() && line.front() != ';' &&
                   line.front() != '#') {
            const std::size_t equals = line.find('=');
            if (equals != std::string::npos) {
                const std::string key = lower_ascii(trim_ascii(
                    std::string_view(line).substr(0, equals)));
                const std::string value = trim_ascii(
                    std::string_view(line).substr(equals + 1));
                if (key == "backend") {
                    settings.backend = lower_ascii(value) == "nvidia"
                        ? FlowBackend::nvidia
                        : FlowBackend::fidelity_fx;
                } else if (key == "nvidia_preset") {
                    const std::string normalized = lower_ascii(value);
                    settings.nvidia_preset = normalized == "slow"
                        ? NvidiaPerformancePreset::slow
                        : normalized == "fast"
                            ? NvidiaPerformancePreset::fast
                            : NvidiaPerformancePreset::medium;
                } else if (key == "nvidia_input_scale") {
                    settings.nvidia_input_scale = value == "75"
                        ? NvidiaInputScale::three_quarter
                        : value == "50"
                            ? NvidiaInputScale::half
                            : NvidiaInputScale::full;
                } else if (key == "nvidia_bidirectional") {
                    settings.nvidia_bidirectional = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "overlay_position") {
                    settings.overlay_position = parse_overlay_position(lower_ascii(value));
                } else if (key == "diagnostics") {
                    settings.diagnostics = value == "1" ||
                                           lower_ascii(value) == "true";
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return settings;
}

std::string serialize_settings(const LauncherSettings& settings) {
    return "[tray]\r\nbackend=" + backend_ini_value(settings.backend) +
           "\r\nnvidia_preset=" +
           nvidia_preset_ini_value(settings.nvidia_preset) +
           "\r\nnvidia_input_scale=" +
           nvidia_input_scale_ini_value(settings.nvidia_input_scale) +
           "\r\nnvidia_bidirectional=" +
           (settings.nvidia_bidirectional ? "1" : "0") +
           "\r\ndiagnostics=" + (settings.diagnostics ? "1" : "0") +
           "\r\noverlay_position=" + overlay_position_name(settings.overlay_position) +
           "\r\n";
}

std::string build_implicit_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version) {
    const std::string escaped_path = escape_json(wide_to_utf8(
        std::filesystem::absolute(layer_dll).lexically_normal().native()));
    return "{\n"
           "  \"file_format_version\": \"1.0.0\",\n"
           "  \"api_layer\": {\n"
           "    \"name\": \"XR_APILAYER_XRFrameBridge_diagnostic\",\n"
           "    \"library_path\": \"" + escaped_path + "\",\n"
           "    \"api_version\": \"1.0\",\n"
           "    \"implementation_version\": \"" +
           std::to_string(implementation_version) + "\",\n"
           "    \"description\": \"OFXR Bridge V" +
           std::to_string(implementation_version) +
           " manual persistent implicit layer\",\n"
           "    \"disable_environment\": \"XRFG_DISABLE_OFXR_BRIDGE\"\n"
           "  }\n"
           "}\n";
}

std::string build_runtime_ini(
    const LauncherSettings& settings) {
    return "[ofxr]\r\nbackend=" + backend_ini_value(settings.backend) +
           "\r\nnvidia_preset=" +
           nvidia_preset_ini_value(settings.nvidia_preset) +
           "\r\nnvidia_input_scale=" +
           nvidia_input_scale_ini_value(settings.nvidia_input_scale) +
           "\r\nnvidia_bidirectional=" +
           (settings.nvidia_bidirectional ? "1" : "0") +
           "\r\n\r\n[diagnostics]\r\nlogging_enabled=" +
           (settings.diagnostics ? "1" : "0") +
           "\r\nmax_file_mb=32\r\nflush_each_event=0\r\n"
           "\r\n[overlay]\r\nposition=" + overlay_position_name(settings.overlay_position) + "\r\n";
}

std::filesystem::path runtime_version_directory(
    const std::filesystem::path& local_directory,
    std::uint32_t implementation_version) {
    std::wstring version = std::to_wstring(implementation_version);
    if (version.size() < 3) {
        version.insert(version.begin(), 3 - version.size(), L'0');
    }
    return local_directory / L"RuntimeLayer" / (L"v" + version);
}

bool install_runtime_layer_dll(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) noexcept {
    try {
        std::filesystem::create_directories(destination.parent_path());
        return CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
    } catch (...) {
        return false;
    }
}

std::wstring quote_windows_argument(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") ==
                                std::wstring_view::npos) {
        return std::wstring(argument);
    }
    std::wstring output(1, L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(L'\"');
        } else {
            output.append(backslashes, L'\\');
            output.push_back(character);
        }
        backslashes = 0;
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

} // namespace xrfg::standalone
