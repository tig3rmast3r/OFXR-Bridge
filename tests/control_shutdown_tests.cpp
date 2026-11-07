#include "xrfg/autonomous_control.hpp"
#include <windows.h>
#include <cstdlib>
#include <cstdio>
#include <filesystem>

int main() {
    const auto fixture=std::filesystem::temp_directory_path()/
        (L"ofxr-late-overlay-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",fixture.c_str());
    // Register before the first channel exists: this models g_sessions being
    // destroyed after function-local control statics during DLL detach.
    std::atexit([] {
        using namespace xrfg::autonomous;
        set_interface_visible(InterfaceSurface::headset,true);
        if(!interface_telemetry_requested())std::_Exit(2);
        set_interface_visible(InterfaceSurface::headset,false);
        std::puts("Late overlay destruction retains a valid control channel PASS");
    });
    if(!xrfg::autonomous::initialize_game_configuration(L"C:/fixture/GWT.exe"))return 1;
    xrfg::autonomous::set_interface_visible(xrfg::autonomous::InterfaceSurface::headset,true);
    return 0;
}
