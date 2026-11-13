#pragma once

#include "xrfg/settings_widgets.hpp"
#include <cstdint>

namespace xrfg {

struct MenuPresentationFov {
    float left{-1.F}, right{1.F}, down{-1.F}, up{1.F}; // tangent bounds
};

struct MenuPresentationLayout {
    bool valid{};
    float x{}, y{}, z{-1.2F};
    float width{}, height{};
    float view_width{}, view_height{};
    float offset_x{}, offset_y{}, scale{1.F};
};

// The logical canvas remains 1024 x 768 (the texture can be supersampled). Normalized offsets move the center by half
// the common visible FOV span; the whole quad is clamped inside those bounds.
MenuPresentationLayout calculate_menu_presentation(MenuPresentationFov fov,
    const autonomous::Settings& settings) noexcept;

struct MenuPresentationInteraction {
    bool pending_drag{}, dragging{}, footer_scale_editing{};
    std::uint32_t owner_move_id{};
    int candidate_frame{};
    float anchor_x{}, anchor_y{}, offset_x{}, offset_y{};
    bool editing() const noexcept { return pending_drag || dragging || footer_scale_editing; }
};

struct MenuDragResult {
    bool active{}, started{}, finished{}, changed{};
    float consumed_dx{}, consumed_dy{}; // producer rebases pointer by their negatives
};

// Call in the menu's ImGui draw callback after all content, before the footer.
// Native NoMove window/child MoveId owns blank-space drags. No ImGui pointers
// are retained across frames or reopen. Widget/popup presses never become drag.
MenuDragResult update_menu_drag(MenuPresentationInteraction& interaction,
    autonomous::SettingsWidgetState& widgets, const MenuPresentationLayout& layout,
    const char* root_window_name = "OFXR menu");
void reset_menu_drag(MenuPresentationInteraction& interaction) noexcept;

// Parent reserves a fixed 52-pixel strip outside its scrollable content. While
// interaction.editing(), shared widgets must defer publication. Release/close
// commits through the same field-level delta API as every other UI option.
bool render_menu_footer(autonomous::SettingsWidgetState& widgets,
    MenuPresentationInteraction& interaction);

} // namespace xrfg
