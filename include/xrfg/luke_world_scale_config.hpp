#pragma once
#include "xrfg/luke_pose_camera.hpp"
#include <Windows.h>
#include <cmath>
#include <cwchar>
#include <filesystem>

namespace xrfg {
struct LukeGameCameraConfig {
    float world_scale{};
    LukeProducerCameraAxes axes{LukeProducerCameraAxes::view};
};
// The WorldScale-bearing section identifies the active Luke title. One REAL
// VR install can serve several titles (Uncharted: Legacy of Thieves keeps
// U4ATE and UTLL in one ini): their sections stand for the running title
// only while they agree on the whole camera contract.
// DOOM supplies right/down/forward rows; its entire input camera basis needs
// conversion to view right/up/back. KCD's native packet decoder exposes
// physical forward (-inverse-view Z), as in accepted OptiScaler V33011.
// Dead Space's verified original source also exposes physical forward:
// V680 F8 proves the scene camera must decode it back to the native view Z.
// Horizon retains V33018's physical-forward convention; other titles keep
// their later accepted view-row contracts. Indiana supplies native view rows
// directly, as proved by OptiScaler V30078 and the V700 captured clip history.
// GTA Enhanced GTAVE has the same native view-row contract: V679 captured
// original SL clip history matches it, while reflected native history does not.
inline LukeGameCameraConfig luke_game_camera_config_from_ini(
    const std::filesystem::path& ini) noexcept {
    wchar_t sections[32768]{};
    const DWORD length = GetPrivateProfileSectionNamesW(sections, 32768, ini.c_str());
    if (!length || length >= 32766) return {};
    LukeGameCameraConfig result{};
    bool found = false;
    for (const wchar_t* section = sections; *section;
         section += std::wcslen(section) + 1) {
        wchar_t value[64]{};
        const DWORD count = GetPrivateProfileStringW(section, L"WorldScale", L"",
            value, 64, ini.c_str());
        if (!count) continue;
        if (count >= 63) return {};
        wchar_t* end{};
        const float scale = std::wcstof(value, &end);
        if (end == value || *end != L'\0' || !std::isfinite(scale) ||
            scale < 0.2F || scale > 5.0F) return {};
        const auto axes = _wcsicmp(section, L"DOOMTDA") == 0
            ? LukeProducerCameraAxes::right_down_forward
            : ((_wcsicmp(section, L"HFW") == 0 || _wcsicmp(section, L"KCD2") == 0 ||
                _wcsicmp(section, L"DS2023") == 0)
                ? LukeProducerCameraAxes::physical_forward
                : ((_wcsicmp(section, L"INDY") == 0 || _wcsicmp(section, L"GTAVE") == 0)
                    ? LukeProducerCameraAxes::native_view_rows
                    : LukeProducerCameraAxes::view));
        if (found && (result.world_scale != scale || result.axes != axes)) return {};
        result.world_scale = scale;
        result.axes = axes;
        found = true;
    }
    return result;
}
inline float luke_world_scale_from_ini(const std::filesystem::path& ini) noexcept {
    return luke_game_camera_config_from_ini(ini).world_scale;
}
} // namespace xrfg
