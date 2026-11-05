#include "xrfg/menu_presentation.hpp"
#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace xrfg;
using namespace xrfg::autonomous;
int checks{};
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool approximately(float a, float b) { return std::abs(a - b) < .0001F; }
void inside(const MenuPresentationLayout& quad, MenuPresentationFov fov) {
    require(quad.valid, "quad has valid finite geometry");
    require(quad.x - quad.width * .5F >= fov.left * 1.2F - .0001F &&
        quad.x + quad.width * .5F <= fov.right * 1.2F + .0001F,
        "entire quad stays inside common horizontal FOV");
    require(quad.y - quad.height * .5F >= fov.down * 1.2F - .0001F &&
        quad.y + quad.height * .5F <= fov.up * 1.2F + .0001F,
        "entire quad stays inside common vertical FOV");
    require(approximately(quad.width / quad.height, 4.F / 3.F) && approximately(quad.z, -1.2F),
        "physical size preserves fixed canvas aspect and viewing distance");
}
void geometry() {
    Settings settings;
    auto quad = calculate_menu_presentation({}, settings);
    inside(quad, {});
    require(approximately(quad.width, 1.44F) && approximately(quad.height, 1.08F) &&
        approximately(quad.x, 0) && approximately(quad.y, 0), "default is three quarters of the fitted 80-percent quad");
    settings.menu_scale = 1;
    quad = calculate_menu_presentation({}, settings);
    require(approximately(quad.width, 1.92F) && approximately(quad.height, 1.44F), "1.00x matches the V441 fitted quad");
    settings.menu_scale = .5F;
    quad = calculate_menu_presentation({}, settings);
    require(approximately(quad.width, .96F) && approximately(quad.height, .72F), "half size keeps same texture aspect");
    settings.menu_scale = 1.25F;
    settings.menu_offset_x = 1;
    settings.menu_offset_y = -1;
    quad = calculate_menu_presentation({}, settings);
    inside(quad, {});
    require(approximately(quad.width, quad.view_width) && approximately(quad.offset_x, 0),
        "largest size fills limiting FOV axis and clamps its center");
    require(approximately(quad.offset_y, -.25F), "larger free axis retains bounded position adjustment");
    const MenuPresentationFov asymmetric{-.6F, .9F, -.5F, .75F};
    settings.menu_scale = 1;
    settings.menu_offset_x = settings.menu_offset_y = 0;
    quad = calculate_menu_presentation(asymmetric, settings);
    inside(quad, asymmetric);
    require(approximately(quad.x, .18F) && approximately(quad.y, .15F), "asymmetric FOV uses its actual common center");
    for (const auto size : {.5F, 1.F, 1.25F}) {
        settings.menu_scale = size;
        for (const auto x : {-1.F, 1.F}) for (const auto y : {-1.F, 1.F}) {
            settings.menu_offset_x = x;
            settings.menu_offset_y = y;
            inside(calculate_menu_presentation(asymmetric, settings), asymmetric);
        }
    }
    settings.menu_scale = std::numeric_limits<float>::quiet_NaN();
    settings.menu_offset_x = std::numeric_limits<float>::infinity();
    settings.menu_offset_y = -std::numeric_limits<float>::infinity();
    quad = calculate_menu_presentation({}, settings);
    require(quad.valid && approximately(quad.scale, kDefaultMenuScale) && approximately(quad.x, 0) && approximately(quad.y, 0),
        "nonfinite settings recover finite default geometry");
    settings.menu_scale = 99;
    require(approximately(calculate_menu_presentation({}, settings).scale, 1.25F), "oversized scale clamps");
    settings.menu_scale = -99;
    require(approximately(calculate_menu_presentation({}, settings).scale, .5F), "undersized scale clamps");
    require(!calculate_menu_presentation({1, -1, -1, 1}, settings).valid,
        "inverted FOV is rejected");
    require(!calculate_menu_presentation({0, 0, -1, 1}, settings).valid,
        "empty FOV is rejected");
    require(!calculate_menu_presentation({-1, 1, 0, 0}, settings).valid,
        "empty vertical FOV is rejected");
    require(!calculate_menu_presentation({-1, std::numeric_limits<float>::infinity(), -1, 1}, settings).valid,
        "nonfinite FOV is rejected");
}

struct Surface {
    ImGuiContext* context{};
    SettingsWidgetState widgets{};
    MenuPresentationInteraction interaction{};
    MenuDragResult drag{};
    ImRect checkbox{}, disabled_checkbox{}, slider{}, popup_button{}, reset_button{};
    ImGuiWindow* child{};
    bool checked{}, disabled_checked{}, popup_requested{};
    float value{.5F};
    Surface() {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.F / 60.F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::StyleColorsDark();
        synchronize_settings_widgets(widgets, snapshot());
        draw();
        draw();
    }
    ~Surface() {
        ImGui::SetCurrentContext(context);
        for (auto* texture : ImGui::GetPlatformIO().Textures) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGui::DestroyContext(context);
    }
    void position(ImVec2 point) {
        ImGui::SetCurrentContext(context);
        ImGui::GetIO().AddMousePosEvent(point.x, point.y);
    }
    void button(bool held) {
        ImGui::SetCurrentContext(context);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, held);
    }
    void draw() {
        ImGui::SetCurrentContext(context);
        ImGui::NewFrame();
        synchronize_settings_widgets(widgets, snapshot());
        // The shared renderer resets this each frame and defers publication.
        widgets.scale_editing = false;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1024, 768));
        ImGui::Begin("OFXR menu", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("Shared settings");
        ImGui::BeginChild("Content", ImVec2(0, 664), ImGuiChildFlags_None, ImGuiWindowFlags_NoMove);
        child = ImGui::GetCurrentWindow();
        ImGui::Checkbox("Enabled fixture", &checked);
        checkbox = context->LastItemData.Rect;
        ImGui::BeginDisabled();
        ImGui::Checkbox("Disabled fixture", &disabled_checked);
        disabled_checkbox = context->LastItemData.Rect;
        ImGui::EndDisabled();
        ImGui::SetNextItemWidth(250);
        ImGui::SliderFloat("Slider fixture", &value, 0, 1);
        slider = context->LastItemData.Rect;
        if (ImGui::Button("Popup fixture") || popup_requested) ImGui::OpenPopup("Popup");
        popup_requested = false;
        popup_button = context->LastItemData.Rect;
        if (ImGui::BeginPopup("Popup")) {
            ImGui::TextUnformatted("Popup blank space must not drag the menu");
            ImGui::Dummy(ImVec2(260, 100));
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(500, 900)); // force a native scrollbar
        ImGui::EndChild();
        drag = update_menu_drag(interaction, widgets,
            calculate_menu_presentation({}, widgets.draft));
        ImGui::Separator();
        render_menu_footer(widgets, interaction);
        reset_button = context->LastItemData.Rect;
        ImGui::End();
        ImGui::Render();
        for (auto* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_WantUpdates) {
                texture->SetTexID(static_cast<ImTextureID>(texture->UniqueID + 1));
                texture->SetStatus(ImTextureStatus_OK);
            } else if (texture->Status == ImTextureStatus_WantDestroy) {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        require(ImGui::GetCurrentContext() == context, "presentation preserves caller's ImGui context");
        require(ImGui::GetDrawData()->TotalVtxCount > 0, "presentation produces real widget draw data");
        require(reset_button.Min.y > 690 && reset_button.Max.y < 768,
            "size buttons and reset remain outside content scroller and inside canvas");
        require(reset_button.Max.x <= 1024, "footer controls fit canvas horizontally");
    }
    void press(ImVec2 point) {
        position(point); draw();
        button(true); draw();
    }
    void release(ImVec2 point) {
        position(point); button(false); draw(); draw();
    }
    void close() {
        finish_settings_widget_edits(widgets);
        reset_menu_drag(interaction);
        ImGui::ClearActiveID();
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        ImGui::GetIO().ClearEventsQueue();
        ImGui::ClosePopupsOverWindow(nullptr, true);
    }
};
ImVec2 center(const ImRect& rect) { return rect.GetCenter(); }
void baseline(Surface& surface) {
    require(request(Settings{}), "fixture publishes an isolated baseline");
    surface.close();
    surface.draw();
    surface.draw();
}
void drag_blank(Surface& surface, ImVec2 anchor) {
    baseline(surface);
    const auto initial = snapshot();
    surface.press(anchor);
    require(surface.interaction.pending_drag && !surface.interaction.dragging,
        "blank press awaits native MoveId after EndFrame");
    surface.position(ImVec2(anchor.x + 40, anchor.y + 25));
    surface.draw();
    require(surface.drag.started && surface.drag.active && surface.drag.changed,
        "native NoMove blank owner starts physical quad drag");
    require(approximately(surface.drag.consumed_dx, 40) && approximately(surface.drag.consumed_dy, 25),
        "drag reports framebuffer delta for producer pointer rebasing");
    require(surface.widgets.draft.menu_offset_x > 0 && surface.widgets.draft.menu_offset_y < 0,
        "screen movement maps to physical right and down");
    auto& io = ImGui::GetIO();
    require(approximately(io.MousePos.x, anchor.x) && approximately(io.MousePos.y, anchor.y) &&
        approximately(io.MousePosPrev.x, anchor.x) && approximately(io.MouseDelta.x, 0),
        "held drag freezes current and previous pointer at grabbed texel");
    require(surface.widgets.dirty && surface.widgets.scale_editing &&
        snapshot().revision == initial.revision, "held drag changes pose without committing every motion");
    auto concurrent = snapshot().desired;
    concurrent.nr_intensity = .42F;
    require(request_changes(snapshot().desired, concurrent), "other UI changes independent option during drag");
    surface.position(ImVec2(anchor.x + 10, anchor.y - 5));
    surface.draw();
    require(surface.drag.active && approximately(surface.drag.consumed_dx, 10) &&
        approximately(surface.widgets.draft.nr_intensity, .42F), "continued drag rebases concurrent tray edits");
    surface.position(ImVec2(anchor.x + 5, anchor.y));
    surface.button(false);
    surface.draw();
    require(surface.drag.finished && !surface.interaction.editing(), "release retires drag ownership");
    require(approximately(surface.drag.consumed_dx, 5), "release consumes final movement before publishing");
    require(!surface.widgets.dirty && snapshot().desired.menu_offset_x > 0 &&
        approximately(snapshot().desired.nr_intensity, .42F), "release commits only presentation delta preserving other UI edit");
    surface.draw();
}
void interactions() {
    Surface surface;
    drag_blank(surface, ImVec2(920, 14)); // root header's blank area
    drag_blank(surface, ImVec2(920, 350)); // child content's blank area
    baseline(surface);
    surface.press(center(surface.checkbox));
    surface.draw();
    require(!surface.interaction.dragging && !surface.interaction.pending_drag,
        "checkbox owns click instead of moving quad");
    surface.release(center(surface.checkbox));
    require(surface.checked, "checkbox retains actual click behavior");
    surface.press(center(surface.disabled_checkbox));
    surface.draw();
    require(!surface.interaction.dragging && !surface.interaction.pending_drag && !surface.disabled_checked,
        "disabled item focus MoveId cannot start blank drag");
    surface.release(center(surface.disabled_checkbox));
    surface.press(center(surface.slider));
    surface.position(ImVec2(surface.slider.Min.x + 230, surface.slider.GetCenter().y));
    surface.draw();
    require(!surface.interaction.editing() && surface.value > .5F,
        "normal slider remains interactive and cannot drag quad");
    surface.release(center(surface.slider));
    const auto scrollbar = ImGui::GetWindowScrollbarRect(surface.child, ImGuiAxis_Y).GetCenter();
    surface.press(scrollbar);
    surface.draw();
    require(!surface.interaction.editing(), "native scrollbar press does not move quad");
    surface.release(scrollbar);
    surface.child->Scroll.y = 0;
    surface.popup_requested = true;
    surface.draw(); surface.draw();
    require(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel),
        "fixture owns a real open ImGui popup");
    ImGuiWindow* popup = surface.context->OpenPopupStack.back().Window;
    surface.press(ImVec2(popup->Pos.x + 180, popup->Pos.y + 60));
    surface.draw();
    require(!surface.interaction.editing(), "blank popup area cannot drag underlying quad");
    surface.release(ImVec2(popup->Pos.x + 180, popup->Pos.y + 60));
    surface.close(); surface.draw(); surface.draw();
    baseline(surface);
    const auto revision = snapshot().revision;
    ImGui::SetCurrentContext(surface.context);
    ImGui::ActivateItemByID(ImGui::FindWindowByName("OFXR menu")->GetID("###Larger size"));
    surface.draw(); surface.draw();
    require(!surface.interaction.dragging && !surface.interaction.pending_drag &&
        snapshot().revision != revision && approximately(snapshot().desired.menu_scale, .8F) && !surface.widgets.dirty,
        "footer size button publishes one step without becoming blank drag");
    surface.press(center(surface.reset_button));
    surface.release(center(surface.reset_button));
    require(approximately(snapshot().desired.menu_scale, kDefaultMenuScale) && approximately(snapshot().desired.menu_offset_x, 0) &&
        approximately(snapshot().desired.menu_offset_y, 0), "reset restores size and position together");
    surface.press(ImVec2(920, 350));
    surface.position(ImVec2(960, 375)); surface.draw();
    surface.close();
    require(!surface.interaction.editing() && !surface.widgets.dirty &&
        snapshot().desired.menu_offset_x > 0, "closing held drag flushes values and clears all retained IDs");
    surface.draw(); surface.draw();
    require(!surface.interaction.editing() && approximately(surface.drag.consumed_dx, 0),
        "reopening without fresh click cannot continue old drag");
    surface.press(ImVec2(920, 350));
    surface.position(ImVec2(930, 350)); surface.draw();
    require(surface.drag.started && surface.drag.active, "fresh drag works after close and reopen");
    surface.release(ImVec2(920, 350));
}
} // namespace

int main() {
    const auto profile = std::filesystem::temp_directory_path() /
        (L"ofxr-menu-presentation-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", profile.c_str());
    try {
        geometry();
        interactions();
        std::filesystem::remove(configuration_path());
        std::filesystem::remove(profile);
        std::cout << checks << " menu presentation checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(configuration_path());
        std::filesystem::remove(profile);
        std::cerr << "menu presentation failed after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
