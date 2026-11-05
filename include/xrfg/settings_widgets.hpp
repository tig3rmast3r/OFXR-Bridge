#pragma once

#include "xrfg/autonomous_control.hpp"
#include <functional>

namespace xrfg::autonomous {
// Palette, metrics and, on first use in a context, the embedded fonts.
void apply_settings_style(bool headset=false);

// Keep one state per ImGui context. Pending edits are rebased onto changes from
// the other UI, and publication merges only edited fields under the IPC mutex.
struct SettingsWidgetState {
    Settings draft{};
    Settings base{};
    std::uint64_t observed_revision{};
    bool initialized{};
    std::filesystem::path configuration_file;
    bool dirty{};
    bool scale_editing{};
    bool save_failed{};
    bool dlssg_profile_loaded{};
    bool dlssg_target_saved{};
    unsigned dlssg_observed_pid{};
    double dlssg_target_refresh_at{};
    std::filesystem::path dlssg_executable;
    int dlssg_model{}; // 0 driver default, 1 A, 2 C; driver writes are explicit only
    std::string dlssg_profile_status;
    int section{}; // selected entry of the left rail
    double reset_armed_until{}; // Reset defaults needs a second click before this ImGui time
    const char* help_title{}; // help for what is hovered or focused this frame
    const char* help_body{};
    const char* footer_help_title{}; // from the headset footer, which draws after the panel
    const char* footer_help_body{};
    char help_buffer[420]{};
};

struct SettingsWidgetOptions {
    bool compact{}; // tighter spacing for the in-headset 1024 x 768 surface
    bool defer_publication{}; // containing menu publishes after its drag/size footer
    std::function<bool(SettingsWidgetState&)> desktop_profile_header; // never called for compact/headset
};

void synchronize_settings_widgets(SettingsWidgetState& ui, const Snapshot& state) noexcept;
bool publish_settings_widgets(SettingsWidgetState& ui) noexcept;
// Close/focus-loss is also the end of an edit; commit a held resolution even
// when the hidden menu will not draw the release event.
void finish_settings_widget_edits(SettingsWidgetState& ui) noexcept;
// Two clicks within a few seconds. True when the defaults were restored.
bool render_reset_defaults(SettingsWidgetState& ui);
// Shows help for a control outside the panel on the next frame.
void show_settings_help(SettingsWidgetState& ui,const char* title,const char* body) noexcept;

// The caller owns the current ImGui context, frame and containing window. This
// function owns no GPU or platform resources and draws the same options in both
// locations. Returns true when a changed draft was successfully published.
bool render_settings_widgets(SettingsWidgetState& ui, const Snapshot& state,
    SettingsWidgetOptions options = {});

} // namespace xrfg::autonomous
