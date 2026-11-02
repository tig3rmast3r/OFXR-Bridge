#pragma once
#include "desktop_profiles.hpp"
#include "settings_widgets.hpp"
namespace xrfg::autonomous {
// Desktop only. True means the selected configuration changed and needs reload.
bool render_desktop_profile_picker(DesktopProfiles&,SettingsWidgetState&,ScopedConfigurationSelection&);
}
