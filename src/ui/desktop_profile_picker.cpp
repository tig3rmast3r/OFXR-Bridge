#include "xrfg/desktop_profile_picker.hpp"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
namespace xrfg::autonomous {
namespace {
std::string label(const std::filesystem::path& file) {
    if(same_profile_file(file,global_configuration_path()))return "Global default";
    const auto name=file.filename().wstring();
    const auto size=WideCharToMultiByte(CP_UTF8,0,name.data(),static_cast<int>(name.size()),nullptr,0,nullptr,nullptr);
    std::string out(static_cast<std::size_t>(size),char{});
    WideCharToMultiByte(CP_UTF8,0,name.data(),static_cast<int>(name.size()),out.data(),size,nullptr,nullptr);return out;
}
}
bool render_desktop_profile_picker(DesktopProfiles& profiles,SettingsWidgetState& ui,ScopedConfigurationSelection& selection) {
    bool changed{};
    ImGui::SetNextItemWidth((std::min)(ImGui::GetContentRegionAvail().x,420.F));
    if(ImGui::BeginCombo("Settings",label(profiles.selected()).c_str())) {
        if(ImGui::IsWindowAppearing())profiles.refresh();
        const auto choose=[&](const std::filesystem::path& file) {
            const auto name=label(file);const bool selected=same_profile_file(file,profiles.selected());
            if(ImGui::Selectable(name.c_str(),selected)) {
                finish_settings_widget_edits(ui);
                const auto previous=profiles.selected();
                if(!ui.save_failed && profiles.choose(file)) {
                    {ScopedConfigurationSelection old(previous);set_interface_visible(InterfaceSurface::desktop,false);}
                    selection.select(profiles.selected());changed=true;
                }
            }
            if(selected)ImGui::SetItemDefaultFocus();
        };
        choose(global_configuration_path()); // Always first, independently of directory order.
        for(const auto& file:profiles.saved())choose(file);
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace xrfg::autonomous
