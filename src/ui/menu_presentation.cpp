#include "xrfg/menu_presentation.hpp"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xrfg {
namespace {
constexpr float distance = 1.2F;
constexpr float canvas_width = 1024.F, canvas_height = 768.F;
float finite_or(float value, float fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}
float offset_limit(float extent, float view_extent) noexcept {
    return std::max(0.F, 1.F - extent / view_extent);
}
bool native_drag_owner(const ImGuiContext& context, const ImGuiWindow* root) {
    const auto* owner = context.ActiveIdWindow;
    return root && owner && context.ActiveId != 0 && context.ActiveId == owner->MoveId &&
        owner->RootWindow == root->RootWindow && !(owner->Flags & ImGuiWindowFlags_Popup);
}
} // namespace

MenuPresentationLayout calculate_menu_presentation(MenuPresentationFov fov,
    const autonomous::Settings& settings) noexcept {
    MenuPresentationLayout result;
    if (!std::isfinite(fov.left) || !std::isfinite(fov.right) ||
        !std::isfinite(fov.down) || !std::isfinite(fov.up)) return result;
    fov.left = std::clamp(fov.left, -1.F, 1.F);
    fov.right = std::clamp(fov.right, -1.F, 1.F);
    fov.down = std::clamp(fov.down, -1.F, 1.F);
    fov.up = std::clamp(fov.up, -1.F, 1.F);
    if (fov.right - fov.left <= .0001F || fov.up - fov.down <= .0001F) return result;
    result.view_width = distance * (fov.right - fov.left);
    result.view_height = distance * (fov.up - fov.down);
    result.scale = std::clamp(finite_or(settings.menu_scale, autonomous::kDefaultMenuScale), .5F, 1.25F);
    result.width = .80F * std::min(result.view_width, result.view_height * (4.F / 3.F)) * result.scale;
    result.height = result.width * .75F;
    const auto limit_x = offset_limit(result.width, result.view_width);
    const auto limit_y = offset_limit(result.height, result.view_height);
    result.offset_x = std::clamp(finite_or(settings.menu_offset_x, 0.F), -limit_x, limit_x);
    result.offset_y = std::clamp(finite_or(settings.menu_offset_y, 0.F), -limit_y, limit_y);
    result.x = distance * (fov.left + fov.right) * .5F + result.offset_x * result.view_width * .5F;
    result.y = distance * (fov.down + fov.up) * .5F + result.offset_y * result.view_height * .5F;
    result.valid = true;
    return result;
}

void reset_menu_drag(MenuPresentationInteraction& interaction) noexcept {
    interaction = {};
}

MenuDragResult update_menu_drag(MenuPresentationInteraction& interaction,
    autonomous::SettingsWidgetState& widgets, const MenuPresentationLayout& layout,
    const char* root_window_name) {
    MenuDragResult result;
    auto* context = ImGui::GetCurrentContext();
    if (!context) return result;
    auto& io = ImGui::GetIO();
    auto* root = ImGui::FindWindowByName(root_window_name);
    const bool owned = native_drag_owner(*context, root);
    if (!layout.valid || !root || !ImGui::IsMousePosValid(&io.MousePos)) {
        if (interaction.dragging || interaction.pending_drag) {
            interaction.dragging = interaction.pending_drag = false;
            interaction.owner_move_id = 0;
            widgets.scale_editing = false;
            static_cast<void>(autonomous::publish_settings_widgets(widgets));
            result.finished = true;
        }
        return result;
    }
    if (interaction.pending_drag) {
        if (owned && io.MouseDown[0] && !context->HoveredIdIsDisabled &&
            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            interaction.pending_drag = false;
            interaction.dragging = true;
            interaction.owner_move_id = context->ActiveId;
            result.started = true;
        } else if (!io.MouseDown[0] || ImGui::GetFrameCount() > interaction.candidate_frame) {
            interaction.pending_drag = false;
            widgets.scale_editing = false;
        }
    }
    if (interaction.dragging) {
        const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        const bool continuing = owned && context->ActiveId == interaction.owner_move_id && io.MouseDown[0];
        if (continuing || released) {
            result.consumed_dx = io.MousePos.x - interaction.anchor_x;
            result.consumed_dy = io.MousePos.y - interaction.anchor_y;
            interaction.offset_x = std::clamp(interaction.offset_x +
                result.consumed_dx * layout.width / canvas_width / (layout.view_width * .5F),
                -offset_limit(layout.width, layout.view_width), offset_limit(layout.width, layout.view_width));
            interaction.offset_y = std::clamp(interaction.offset_y -
                result.consumed_dy * layout.height / canvas_height / (layout.view_height * .5F),
                -offset_limit(layout.height, layout.view_height), offset_limit(layout.height, layout.view_height));
            result.changed = widgets.draft.menu_offset_x != interaction.offset_x ||
                widgets.draft.menu_offset_y != interaction.offset_y;
            widgets.draft.menu_offset_x = interaction.offset_x;
            widgets.draft.menu_offset_y = interaction.offset_y;
            widgets.dirty |= result.changed;
            // The quad follows the mouse while the software pointer stays on
            // its grabbed texel. Producer rebases both snapshot and FIFO data.
            io.MousePos = io.MousePosPrev = ImVec2(interaction.anchor_x, interaction.anchor_y);
            io.MouseDelta = ImVec2(0, 0);
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }
        if (!continuing) {
            interaction.dragging = interaction.pending_drag = false;
            interaction.owner_move_id = 0;
            widgets.scale_editing = false;
            result.finished = true;
            static_cast<void>(autonomous::publish_settings_widgets(widgets));
        } else {
            widgets.scale_editing = true;
            result.active = true;
        }
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive() &&
        context->HoveredId == 0 && !context->HoveredIdIsDisabled &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_NoPopupHierarchy) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        // Capture eligibility now. Native EndFrame also focuses disabled items
        // with a MoveId, so the ID alone on the next frame is insufficient.
        interaction.pending_drag = true;
        interaction.candidate_frame = ImGui::GetFrameCount();
        interaction.anchor_x = io.MouseClickedPos[0].x;
        interaction.anchor_y = io.MouseClickedPos[0].y;
        interaction.offset_x = layout.offset_x;
        interaction.offset_y = layout.offset_y;
        widgets.scale_editing = true;
    }
    return result;
}

bool render_menu_footer(autonomous::SettingsWidgetState& widgets,
    MenuPresentationInteraction& interaction) {
    constexpr float label_size = 16.F, step = .05F, spacing = 8.F;
    constexpr char reset_label[] = "Reset position / size";
    constexpr char size_help[] = "Scales the headset panel from 0.50x to 1.25x. Smaller keeps the edges within easy eye reach.";
    interaction.footer_scale_editing = false;
    ImGui::PushFont(ImGui::GetFont(), label_size);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 4));
    const auto& style = ImGui::GetStyle();
    const auto chord = autonomous::keybinds().bindings[KeybindAction::open_interface];
    char key[16] = "Menu key";
    if (!chord.modifiers && chord.key >= 0x70 && chord.key <= 0x87)
        std::snprintf(key, sizeof(key), "F%u", chord.key - 0x6F);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s menu, Esc close, drag empty space to move", key);
    const auto text_width = [&](const char* text) {
        return ImGui::CalcTextSize(text).x + style.FramePadding.x * 2;
    };
    const float button = ImGui::GetFrameHeight();
    const float group = text_width("Click again to reset") + ImGui::CalcTextSize("Size").x +
        button * 2 + ImGui::CalcTextSize("0.00x").x + text_width(reset_label) + spacing * 5;
    const float right = ImGui::GetWindowWidth() - 12.F;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), right - group));
    bool changed = autonomous::render_reset_defaults(widgets);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    const auto resize = [&](const char* label, float direction) {
        ImGui::SameLine();
        const bool pressed = ImGui::Button(label, ImVec2(button, button));
        if (ImGui::IsItemHovered()) autonomous::show_settings_help(widgets, "Interface size", size_help);
        if (!pressed) return;
        const float steps = std::round(widgets.draft.menu_scale / step) + direction;
        widgets.draft.menu_scale = std::clamp(steps * step, .5F, 1.25F);
        changed = true;
    };
    resize("-###Smaller size", -1);
    char scale[16];
    std::snprintf(scale, sizeof(scale), "%.2fx", widgets.draft.menu_scale);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(scale);
    resize("+###Larger size", 1);
    ImGui::SameLine();
    if (ImGui::Button(reset_label)) {
        widgets.draft.menu_scale = autonomous::kDefaultMenuScale;
        widgets.draft.menu_offset_x = widgets.draft.menu_offset_y = 0.F;
        changed = true;
        reset_menu_drag(interaction);
        widgets.scale_editing = false;
    }
    ImGui::PopStyleVar(2);
    ImGui::PopFont();
    widgets.dirty |= changed;
    return autonomous::publish_settings_widgets(widgets);
}

} // namespace xrfg
