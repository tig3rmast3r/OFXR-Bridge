#include "xrfg/standalone_launcher.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace xrfg::standalone {
namespace {

// Suffix of a cached layer DLL renamed aside because something still had it
// loaded when it had to be replaced.
constexpr wchar_t kSetAsideMarker[] = L".old-";

[[nodiscard]] bool files_identical(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    std::error_code error;
    const auto left_size = std::filesystem::file_size(left, error);
    if (error) return false;
    const auto right_size = std::filesystem::file_size(right, error);
    if (error || left_size != right_size) return false;
    std::ifstream left_stream(left, std::ios::binary);
    std::ifstream right_stream(right, std::ios::binary);
    if (!left_stream || !right_stream) return false;
    std::array<char, 64 * 1024> left_block{};
    std::array<char, 64 * 1024> right_block{};
    while (left_stream && right_stream) {
        left_stream.read(left_block.data(), left_block.size());
        right_stream.read(right_block.data(), right_block.size());
        const auto read = left_stream.gcount();
        if (read != right_stream.gcount() ||
            !std::equal(left_block.begin(), left_block.begin() + read,
                        right_block.begin())) {
            return false;
        }
    }
    return left_stream.eof() && right_stream.eof();
}

// Deleting one fails while a process still has it loaded, which is fine: it
// is tried again at the next arm.
void remove_unused_set_aside_copies(const std::filesystem::path& destination) {
    std::error_code error;
    const std::wstring prefix =
        destination.filename().wstring() + kSetAsideMarker;
    for (const auto& entry : std::filesystem::directory_iterator(
             destination.parent_path(), error)) {
        if (entry.path().filename().wstring().rfind(prefix, 0) == 0) {
            DeleteFileW(entry.path().c_str());
        }
    }
}

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
    case NvidiaInputScale::quarter:
        return "25";
    case NvidiaInputScale::full:
    default:
        return "100";
    }
}

LauncherSettings parse_settings(std::string_view text) {
    LauncherSettings settings;
    bool separation_seen = false;
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
                if (key == "frame_generation") {
                    settings.frame_generation = lower_ascii(value) == "dlss"
                        ? FrameGeneration::native_dlss : FrameGeneration::ofxr;
                } else if (key == "dlssg_resolution") {
                    const int percent = std::atoi(value.c_str());
                    settings.native_scale = percent >= 25 && percent <= 100 ? percent : 100;
                } else if (key == "backend") {
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
                            : value == "25" ? NvidiaInputScale::quarter
                            : NvidiaInputScale::full;
                } else if (key == "nvidia_bidirectional") {
                    settings.nvidia_bidirectional = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "deep_pipeline") {
                    settings.deep_pipeline = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "triple_frame_gen") {
                    settings.triple_frame_gen = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "excluded_processes") {
                    settings.excluded_processes = value;
                } else if (key == "separate_synthesis") {
                    separation_seen = true;
                    settings.separate_synthesis = value == "1" || lower_ascii(value) == "true";
                } else if (key == "single_swapchain_rings") {
                    settings.single_swapchain_rings = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "pacing") {
                    settings.pacing = value == "force_vdxr" ? RuntimePacing::force_vdxr : value == "force_steamvr" ? RuntimePacing::force_steamvr : RuntimePacing::automatic;
                } else if (key == "steamvr_separate_queue") {
                    settings.steamvr_separate_queue = value != "0" && lower_ascii(value) != "false";
                } else if (key == "vulkan_session_bridge") {
                    settings.vulkan_session_bridge = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "vulkan_bridge") {
                    // Not vulkan_support: while the option was off by default
                    // every save wrote vulkan_support=0, so that key says
                    // nothing about what the user chose and is ignored. The
                    // new key is written only from now on, when it is on by
                    // default and set to 0 only by hand.
                    settings.vulkan_support = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "d3d11_bridge") {
                    settings.d3d11_bridge = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "capture_at_end_frame") {
                    settings.capture_at_end_frame = value == "1" ||
                        lower_ascii(value) == "true";
                } else if (key == "overlay_position") {
                    settings.overlay_position = parse_overlay_position(lower_ascii(value));
                } else if (key == "crash_debug") {
                    settings.crash_debug = value == "1" || lower_ascii(value) == "true";
                } else if (key == "diagnostics") {
                    settings.diagnostics = value == "1" ||
                                           lower_ascii(value) == "true";
                } else if (key == "pause_hotkey") {
                    settings.pause_hotkey = lower_ascii(value);
                }
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    if (!separation_seen) settings.separate_synthesis = settings.single_swapchain_rings;
    return settings;
}

std::string serialize_settings(const LauncherSettings& settings) {
    return "[tray]\r\nbackend=" + backend_ini_value(settings.backend) +
           "\r\nframe_generation=" + (settings.frame_generation == FrameGeneration::native_dlss ? "dlss" : "ofxr") +
           "\r\ndlssg_resolution=" + std::to_string(settings.native_scale) +
           "\r\nnvidia_preset=" +
           nvidia_preset_ini_value(settings.nvidia_preset) +
           "\r\nnvidia_input_scale=" +
           nvidia_input_scale_ini_value(settings.nvidia_input_scale) +
           "\r\nnvidia_bidirectional=" +
           (settings.nvidia_bidirectional ? "1" : "0") +
           "\r\ndeep_pipeline=" + (settings.deep_pipeline ? "1" : "0") +
           "\r\ntriple_frame_gen=" + (settings.triple_frame_gen ? "1" : "0") +
           "\r\nexcluded_processes=" + settings.excluded_processes +
           "\r\nseparate_synthesis=" + (settings.separate_synthesis ? "1" : "0") +
           "\r\nsingle_swapchain_rings=" + (settings.single_swapchain_rings ? "1" : "0") +
           "\r\npacing=" + (settings.pacing==RuntimePacing::force_vdxr ? "force_vdxr" : settings.pacing==RuntimePacing::force_steamvr ? "force_steamvr" : "auto") +
           "\r\nsteamvr_separate_queue=" + (settings.steamvr_separate_queue ? "1" : "0") +
           "\r\nvulkan_session_bridge=" + (settings.vulkan_session_bridge ? "1" : "0") +
           "\r\nvulkan_bridge=" + (settings.vulkan_support ? "1" : "0") +
           "\r\nd3d11_bridge=" + (settings.d3d11_bridge ? "1" : "0") +
           "\r\ncapture_at_end_frame=" + (settings.capture_at_end_frame ? "1" : "0") +
           "\r\ncrash_debug=" + (settings.crash_debug ? "1" : "0") +
           "\r\ndiagnostics=" + (settings.diagnostics ? "1" : "0") +
           "\r\noverlay_position=" + overlay_position_name(settings.overlay_position) +
           "\r\npause_hotkey=" + settings.pause_hotkey +
           "\r\n";
}

namespace {

[[nodiscard]] std::string json_string(std::string_view value) {
    return "\"" + escape_json(value) + "\"";
}

[[nodiscard]] std::string json_rate(float value) {
    char text[32]{};
    std::snprintf(text, sizeof(text), "%.1f",
        std::isfinite(value) ? static_cast<double>(value) : 0.0);
    return text;
}

[[nodiscard]] const char* json_bool(bool value) { return value ? "true" : "false"; }

}  // namespace

// The 0.2 format (schema 1) plus latency_ms. Fields may be added within a
// schema; none changes meaning or disappears without a new number.
std::string build_status_json(const StatusInputs& inputs) {
    const std::uint32_t configured = inputs.settings.triple_frame_gen ? 3U : 2U;
    const bool session = inputs.session_present;
    const StatusSession& running = inputs.session;
    std::string json = "{\n";
    json += "  \"schema\": 1,\n";
    json += std::string("  \"armed\": ") + json_bool(inputs.armed) + ",\n";
    json += std::string("  \"paused\": ") + json_bool(inputs.paused) + ",\n";
    json += "  \"multiplier\": " +
        std::to_string(session ? running.multiplier : configured) + ",\n";
    json += std::string("  \"generating\": ") +
        (session ? json_bool(running.generating) : "null") + ",\n";
    json += "  \"app_fps\": " +
        (session && running.rates_valid ? json_rate(running.app_fps) : "null") + ",\n";
    json += "  \"output_fps\": " +
        (session && running.output_valid ? json_rate(running.output_fps) : "null") + ",\n";
    json += "  \"display_hz\": " +
        (session && running.display_hz > 0.0f ? json_rate(running.display_hz) : "null") +
        ",\n";
    json += "  \"latency_ms\": " +
        (session && running.latency_valid ? json_rate(running.latency_ms) : "null") + ",\n";
    json += "  \"updated\": " + json_string(inputs.updated) + ",\n";
    json += "  \"ofxr\": { \"build\": " + std::to_string(inputs.build) + " },\n";
    if (session) {
        json += "  \"session\": {\n";
        json += "    \"process\": " + json_string(running.process) + ",\n";
        json += "    \"pid\": " + std::to_string(running.pid) + ",\n";
        json += "    \"runtime\": " + json_string(running.runtime) + ",\n";
        json += std::string("    \"deep_pipeline\": ") + json_bool(running.deep_pipeline) + "\n";
        json += "  }\n";
    } else {
        json += "  \"session\": null\n";
    }
    json += "}\n";
    return json;
}

namespace {
struct HotkeyToken {
    std::string_view name;
    std::string_view display;
    unsigned value;
};
constexpr HotkeyToken kHotkeyModifiers[]{
    {"ctrl", "Ctrl", 2}, {"control", "Ctrl", 2}, {"alt", "Alt", 1},
    {"shift", "Shift", 4}, {"win", "Win", 8}};
// Virtual-key codes, spelled out so this file needs no Windows header.
constexpr HotkeyToken kHotkeyNamedKeys[]{
    {"scrolllock", "Scroll Lock", 0x91}, {"pause", "Pause", 0x13},
    {"insert", "Insert", 0x2D}, {"delete", "Delete", 0x2E},
    {"home", "Home", 0x24}, {"end", "End", 0x23},
    {"pageup", "Page Up", 0x21}, {"pagedown", "Page Down", 0x22},
    {"left", "Left", 0x25}, {"up", "Up", 0x26},
    {"right", "Right", 0x27}, {"down", "Down", 0x28},
    {"numpad0", "Num 0", 0x60}, {"numpad1", "Num 1", 0x61},
    {"numpad2", "Num 2", 0x62}, {"numpad3", "Num 3", 0x63},
    {"numpad4", "Num 4", 0x64}, {"numpad5", "Num 5", 0x65},
    {"numpad6", "Num 6", 0x66}, {"numpad7", "Num 7", 0x67},
    {"numpad8", "Num 8", 0x68}, {"numpad9", "Num 9", 0x69},
    {"numpadmultiply", "Num *", 0x6A}, {"numpadadd", "Num +", 0x6B},
    {"numpadsubtract", "Num -", 0x6D}, {"numpaddecimal", "Num .", 0x6E},
    {"numpaddivide", "Num /", 0x6F}};

// Any other key, by its virtual-key code: "vk" and two hex digits.
[[nodiscard]] unsigned parse_virtual_key(const std::string& token) noexcept {
    if (token.size() != 4 || token[0] != 'v' || token[1] != 'k') return 0;
    unsigned value = 0;
    for (std::size_t i = 2; i < 4; ++i) {
        const char c = token[i];
        const unsigned digit = c >= '0' && c <= '9' ? static_cast<unsigned>(c - '0')
            : c >= 'a' && c <= 'f' ? static_cast<unsigned>(c - 'a' + 10)
            : 16U;
        if (digit > 15) return 0;
        value = value * 16 + digit;
    }
    return value >= 1 && value <= 0xFE ? value : 0;
}

[[nodiscard]] std::string virtual_key_token(unsigned key) {
    constexpr char digits[] = "0123456789abcdef";
    return std::string("vk") + digits[(key >> 4) & 15] + digits[key & 15];
}

struct ParsedHotkey {
    Hotkey hotkey;
    std::string display;
};

std::optional<ParsedHotkey> parse_hotkey_parts(std::string_view text) {
    const std::string normalized = lower_ascii(trim_ascii(text));
    ParsedHotkey result;
    bool have_key = false;
    std::size_t offset = 0;
    while (offset <= normalized.size()) {
        const std::size_t plus = normalized.find('+', offset);
        const std::string token = trim_ascii(std::string_view(normalized).substr(
            offset, plus == std::string::npos ? std::string::npos : plus - offset));
        // The key comes last and once; an empty token is a stray '+'.
        if (token.empty() || have_key) return std::nullopt;
        bool matched = false;
        for (const auto& modifier : kHotkeyModifiers) {
            if (token != modifier.name) continue;
            if (result.hotkey.modifiers & modifier.value) return std::nullopt;
            result.hotkey.modifiers |= modifier.value;
            result.display += std::string(modifier.display) + " + ";
            matched = true;
            break;
        }
        if (!matched) {
            std::string display;
            unsigned key = 0;
            if (token.size() >= 2 && token.size() <= 3 && token[0] == 'f' &&
                token.find_first_not_of("0123456789", 1) == std::string::npos) {
                const int number = std::stoi(token.substr(1));
                if (number >= 1 && number <= 24) {
                    key = 0x70 + static_cast<unsigned>(number - 1);
                    display = "F" + std::to_string(number);
                }
            } else if (token.size() == 1 &&
                       ((token[0] >= 'a' && token[0] <= 'z') ||
                        (token[0] >= '0' && token[0] <= '9'))) {
                const char upper = token[0] >= 'a'
                    ? static_cast<char>(token[0] - 'a' + 'A')
                    : token[0];
                key = static_cast<unsigned char>(upper);
                display = std::string(1, upper);
            } else {
                for (const auto& named : kHotkeyNamedKeys) {
                    if (token != named.name) continue;
                    key = named.value;
                    display = std::string(named.display);
                    break;
                }
                if (key == 0) {
                    key = parse_virtual_key(token);
                    if (key != 0) {
                        display = "Key 0x" + virtual_key_token(key).substr(2);
                    }
                }
            }
            if (key == 0) return std::nullopt;
            result.hotkey.key = key;
            result.display += display;
            have_key = true;
        }
        if (plus == std::string::npos) break;
        offset = plus + 1;
    }
    if (!have_key) return std::nullopt;
    return result;
}
}

std::optional<Hotkey> parse_hotkey(std::string_view text) {
    const auto parsed = parse_hotkey_parts(text);
    if (!parsed) return std::nullopt;
    return parsed->hotkey;
}

std::string hotkey_display_name(std::string_view text) {
    const auto parsed = parse_hotkey_parts(text);
    return parsed ? parsed->display : std::string();
}

std::optional<std::string> hotkey_setting(Hotkey hotkey) {
    std::string setting;
    // "control" is a second spelling of ctrl, so one name per bit.
    for (const auto& modifier : kHotkeyModifiers) {
        if (modifier.name == "control") continue;
        if (hotkey.modifiers & modifier.value) {
            setting += std::string(modifier.name) + "+";
        }
    }
    if (hotkey.key >= 0x70 && hotkey.key <= 0x87) {
        setting += "f" + std::to_string(hotkey.key - 0x70 + 1);
    } else if (hotkey.key >= 'A' && hotkey.key <= 'Z') {
        setting += static_cast<char>(hotkey.key - 'A' + 'a');
    } else if (hotkey.key >= '0' && hotkey.key <= '9') {
        setting += static_cast<char>(hotkey.key);
    } else {
        bool named_key = false;
        for (const auto& named : kHotkeyNamedKeys) {
            if (hotkey.key != named.value) continue;
            setting += std::string(named.name);
            named_key = true;
            break;
        }
        if (!named_key) {
            if (hotkey.key < 1 || hotkey.key > 0xFE) return std::nullopt;
            setting += virtual_key_token(hotkey.key);
        }
    }
    // One rule for what is allowed: the parser's.
    if (parse_hotkey(setting) != hotkey) return std::nullopt;
    return setting;
}

std::string build_vulkan_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version) {
    const std::string escaped_path = escape_json(wide_to_utf8(
        std::filesystem::absolute(layer_dll).lexically_normal().native()));
    return "{\n"
           "  \"file_format_version\": \"1.2.0\",\n"
           "  \"layer\": {\n"
           "    \"name\": \"VK_LAYER_OFXRBridge_queue_serialize\",\n"
           "    \"type\": \"GLOBAL\",\n"
           "    \"library_path\": \"" + escaped_path + "\",\n"
           "    \"api_version\": \"1.3.296\",\n"
           "    \"implementation_version\": \"" +
           std::to_string(implementation_version) + "\",\n"
           "    \"description\": \"OFXR-BRIDGE 0.3.1 (v" +
           std::to_string(implementation_version) +
           "): serialises Vulkan queue submissions while the bridge is armed\",\n"
           "    \"functions\": {\n"
           "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"OFXR_vkNegotiateLoaderLayerInterfaceVersion\",\n"
           "      \"vkGetInstanceProcAddr\": \"OFXR_vkGetInstanceProcAddr\",\n"
           "      \"vkGetDeviceProcAddr\": \"OFXR_vkGetDeviceProcAddr\"\n"
           "    },\n"
           "    \"disable_environment\": {\n"
           "      \"OFXR_DISABLE_VULKAN_QUEUE_LAYER\": \"1\"\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string build_implicit_layer_manifest(
    const std::filesystem::path& layer_dll,
    std::uint32_t implementation_version) {
    const std::string escaped_path = escape_json(wide_to_utf8(
        std::filesystem::absolute(layer_dll).lexically_normal().native()));
    return "{\n"
           "  \"file_format_version\": \"1.0.0\",\n"
           "  \"api_layer\": {\n"
           "    \"name\": \"XR_APILAYER_OFXRBridge\",\n"
           "    \"library_path\": \"" + escaped_path + "\",\n"
           "    \"api_version\": \"1.0\",\n"
           "    \"implementation_version\": \"" +
           std::to_string(implementation_version) + "\",\n"
           "    \"description\": \"OFXR-BRIDGE 0.3.1 (v" +
           std::to_string(implementation_version) +
           ") manual persistent implicit layer\",\n"
           "    \"disable_environment\": \"XRFG_DISABLE_OFXR_BRIDGE\"\n"
           "  }\n"
           "}\n";
}

std::string build_runtime_ini(
    const LauncherSettings& settings,
    unsigned max_file_mb,
    bool flush_each_event) {
    return "[ofxr]\r\nbackend=" + backend_ini_value(settings.backend) +
           "\r\nframe_generation=" + (settings.frame_generation == FrameGeneration::native_dlss ? "dlss" : "ofxr") +
           "\r\ndlssg_resolution=" + std::to_string(settings.native_scale) +
           "\r\nmotion_vectors=dlss" +
           "\r\ndlss_flow_hybrid=0\r\nr19_camera=0" +
           "\r\nnvidia_preset=" +
           nvidia_preset_ini_value(settings.nvidia_preset) +
           "\r\nnvidia_input_scale=" +
           nvidia_input_scale_ini_value(settings.nvidia_input_scale) +
           "\r\nnvidia_bidirectional=" +
           (settings.nvidia_bidirectional ? "1" : "0") +
           "\r\ndeep_pipeline=" + (settings.deep_pipeline ? "1" : "0") +
           "\r\ntriple_frame_gen=" + (settings.triple_frame_gen ? "1" : "0") +
           "\r\nexcluded_processes=" + settings.excluded_processes +
           "\r\nseparate_synthesis=" + (settings.separate_synthesis ? "1" : "0") +
           "\r\nsingle_swapchain_rings=" + (settings.single_swapchain_rings ? "1" : "0") +
           "\r\npacing=" + (settings.pacing==RuntimePacing::force_vdxr ? "force_vdxr" : settings.pacing==RuntimePacing::force_steamvr ? "force_steamvr" : "auto") +
           "\r\nafw_only=" + (settings.afw_only ? "1" : "0") +
           "\r\nsteamvr_separate_queue=" + (settings.steamvr_separate_queue ? "1" : "0") +
           "\r\nvulkan_session_bridge=" + (settings.vulkan_session_bridge ? "1" : "0") +
           "\r\nvulkan_bridge=" + (settings.vulkan_support ? "1" : "0") +
           "\r\nd3d11_bridge=" + (settings.d3d11_bridge ? "1" : "0") +
           "\r\ncapture_at_end_frame=" + (settings.capture_at_end_frame ? "1" : "0") +
           "\r\n\r\n[diagnostics]\r\nlogging_enabled=" +
           (settings.diagnostics ? "1" : "0") +
           "\r\ncrash_debug=" + (settings.crash_debug ? "1" : "0") +
           "\r\nmax_file_mb=" + std::to_string(max_file_mb) +
           "\r\nflush_each_event=" + (flush_each_event ? "1" : "0") + "\r\n"
           "timing_capture=0\r\npose_cadence_capture=0\r\ngpu_instrumentation_enabled=0\r\n"
           "\r\n[overlay]\r\nposition=" + overlay_position_name(settings.overlay_position) + "\r\n";
}

std::string build_runtime_ini(
    const LauncherSettings& launcher,
    const autonomous::Settings& controls,
    unsigned max_file_mb,
    bool flush_each_event) {
    const auto desired = autonomous::sanitize(controls);
    LauncherSettings settings = launcher;
    settings.backend = desired.backend == 0 ? FlowBackend::fidelity_fx : FlowBackend::nvidia;
    settings.frame_generation = desired.frame_generation == 2 ? FrameGeneration::native_dlss : FrameGeneration::ofxr;
    settings.native_scale = desired.native_scale;
    settings.nvidia_preset = desired.preset == 0 ? NvidiaPerformancePreset::slow
        : desired.preset == 2 ? NvidiaPerformancePreset::fast : NvidiaPerformancePreset::medium;
    settings.nvidia_input_scale = desired.scale == 0 ? NvidiaInputScale::full
        : desired.scale == 1 ? NvidiaInputScale::three_quarter
        : desired.scale == 3 ? NvidiaInputScale::quarter : NvidiaInputScale::half;
    settings.nvidia_bidirectional = desired.backward;
    settings.deep_pipeline = desired.prefer_fps;
    settings.triple_frame_gen = desired.triple_frame_gen;
    settings.separate_synthesis = desired.separate_synthesis > 0;
    settings.single_swapchain_rings = desired.lower_vram > 0;
    settings.afw_only=desired.afw_only;
    settings.diagnostics = desired.diagnostics;
    settings.overlay_position = static_cast<FpsOverlayPosition>(desired.overlay_position);
    std::string result = build_runtime_ini(settings, max_file_mb, flush_each_event);
    const auto replace_value = [&](std::string_view key, std::string_view value) {
        const std::string prefix = "\r\n" + std::string(key) + "=";
        const auto position = result.find(prefix);
        if (position == result.npos) return;
        const auto start = position + prefix.size();
        result.replace(start, result.find("\r\n", start) - start, value);
    };
    replace_value("motion_vectors", desired.motion_vectors ? "dlss" : "off");
    replace_value("dlss_flow_hybrid", desired.frame_generation==3 ? "1" : "0");
    if(desired.frame_generation==3) {
        replace_value("backend", backend_ini_value(FlowBackend::fidelity_fx));
        replace_value("nvidia_route", "legacy");
    }
    replace_value("r19_camera", desired.r19_camera ? "1" : "0");
    if (desired.frame_generation == 1) replace_value("frame_generation", "fsr3");
    const auto chosen_backend=desired.frame_generation==3 ? 0 : desired.backend;
    const auto route = chosen_backend == 3 ? "djules" : chosen_backend == 2 ? "new" : "legacy";
    const auto first_section = result.find("\r\n\r\n[diagnostics]");
    result.insert(first_section,
        "\r\nenabled=" + std::string(desired.enabled ? "1" : "0") +
        "\r\nnvidia_route=" + route);
    result += "\r\n[experimental]\r\nluke_afw_mode=" + std::to_string(desired.afw_mode) +
        "\r\nluke_afw_debug=" + std::string(desired.afw_debug ? "1" : "0") +
        "\r\nluke_afw_clear_before_warping=" + (desired.afw_clear_before_warping ? "1" : "0") +
        "\r\nluke_afw_ignore_motion_threshold=" + std::to_string(desired.afw_ignore_motion_threshold) +
        "\r\nluke_afw_depth_dilate_mode=" + std::to_string(desired.afw_depth_dilate_mode) + "\r\n" +
        "luke_afw_shading_rate=" + std::to_string(desired.afw_shading_rate) + "\r\n";
    return result;
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
        remove_unused_set_aside_copies(destination);
        if (files_identical(source, destination)) {
            return true;
        }
        if (CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
            return true;
        }
        const DWORD copy_error = GetLastError();
        if (copy_error != ERROR_SHARING_VIOLATION &&
            copy_error != ERROR_USER_MAPPED_FILE) {
            return false;
        }
        // Windows renames a loaded DLL but will not overwrite it. Whoever has
        // it keeps the old copy; the next process loads the new one.
        auto aside = destination;
        aside += kSetAsideMarker + std::to_wstring(GetTickCount64());
        if (!MoveFileExW(destination.c_str(), aside.c_str(), 0)) {
            return false;
        }
        return CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE;
    } catch (...) {
        return false;
    }
}

bool stage_runtime_support_files(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& destination_directory,
    std::wstring* error) noexcept {
    try {
        const wchar_t* required[]{
            L"ofxr_amd_fidelityfx_framegeneration_dx12.dll",
            L"PDAFWPlugin.dll",
            L"nvngx.dll_dlssnr.dll", L"nvngx.dll_dlssg.dll"};
        // Validate the complete package before staging any support file.
        for (const auto* name : required) {
            const auto source = source_directory / name;
            if (!std::filesystem::is_regular_file(source)) {
                if (error) *error = L"The standalone package is incomplete. Missing runtime helper:\r\n" + source.wstring();
                return false;
            }
        }
        for (const auto* name : required) {
            if (!install_runtime_layer_dll(source_directory / name, destination_directory / name)) {
                if (error) *error = L"Could not prepare standalone runtime helper:\r\n" + (destination_directory / name).wstring();
                return false;
            }
        }
        const auto vendor = source_directory / L"nvngx_dlssnr.dll";
        if (std::filesystem::is_regular_file(vendor) &&
            !install_runtime_layer_dll(vendor, destination_directory / vendor.filename())) {
            if (error) *error = L"Could not prepare the supplied Neural Rendering runtime:\r\n" + vendor.wstring();
            return false;
        }
        return true;
    } catch (...) {
        if (error) *error = L"Could not prepare the standalone runtime helper files.";
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
