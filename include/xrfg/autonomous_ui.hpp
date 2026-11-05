#pragma once
#include <windows.h>
#include <filesystem>

namespace xrfg::autonomous {
// Opens only the control panel. Activation remains an explicit tray command.
bool show_settings_window(HWND owner) noexcept;
void close_settings_window() noexcept;
// Captures this panel's own D3D11 backbuffer for layout verification.
// section is the rail entry; headset draws the panel with the headset's sizes.
bool save_settings_preview(const std::filesystem::path& output, int section = 0, bool headset = false) noexcept;
} // namespace xrfg::autonomous
