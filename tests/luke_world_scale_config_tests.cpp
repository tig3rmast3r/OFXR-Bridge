#include "xrfg/luke_world_scale_config.hpp"
#include "xrfg/luke_host_config.hpp"
#include "xrfg/diagnostic_hotkeys.hpp"
#include "xrfg/luke_native_motion.hpp"
#include "fixtures/gta_native_camera.hpp"
#include "fixtures/dead_space_native_basis.hpp"
#include <fstream>
#include <iostream>
int main() {
    const auto ini = std::filesystem::temp_directory_path() /
        (L"ofxr-world-scale-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const auto write = [&](const char* contents) {
        std::ofstream file(ini); file << contents; file.close();
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    };
    write("[DOOMTDA]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "DOOM reads its active Luke scale");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::right_down_forward,
        "DOOM selects right/down/forward decoding for both Up and Forward");
    write("[doomtda]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "Luke section matching is case insensitive");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::right_down_forward,
        "DOOM axis selection is case insensitive");
    write("[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Indiana reads its validated scale");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::native_view_rows,
        "Indiana retains accepted scene view rows and declares the same native-history basis");
    write("[DOOMTDA]\nUnused=1\n[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Unused DOOM section must not override the active Indiana section");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::native_view_rows,
        "An inactive DOOM section must not select its camera convention");
    for(const char* section:{"GTAVE","gtave"}) {
        write((std::string("[")+section+"]\nWorldScale=1.1\n").c_str());
        check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
            xrfg::LukeProducerCameraAxes::native_view_rows,
            "GTA Enhanced keeps original view rows for scene and native camera history");
    }
    for(const auto& sample:xrfg::tests::gta_native_camera_fixtures) {
        std::array<float,16> native{},reflected{};
        check(xrfg::native_motion_detail::transform(sample.current,sample.previous,native,1.F,true),
            "GTA captured native view-row transform is finite");
        check(xrfg::native_motion_detail::transform(sample.current,sample.previous,reflected),
            "GTA legacy reflected transform control is finite");
        float correct_error{},legacy_error{};
        for(unsigned i=0;i<16;++i) {
            correct_error=std::max(correct_error,std::abs(native[i]-sample.original_clip_history[i]));
            legacy_error=std::max(legacy_error,std::abs(reflected[i]-sample.original_clip_history[i]));
        }
        if(!(correct_error<.001F && legacy_error>.005F))
            std::cerr<<"GTA captured camera errors native="<<correct_error<<" reflected="<<legacy_error<<'\n';
        check(correct_error<.001F && legacy_error>.005F,
            "GTA native history must reproduce the original SDK clip matrix, not its reflected counterpart");
    }
    for(const char* section:{"DS2023","ds2023"}) {
        write((std::string("[")+section+"]\nWorldScale=1.1\n").c_str());
        const auto config=xrfg::luke_game_camera_config_from_ini(ini);
        check(config.axes==xrfg::LukeProducerCameraAxes::physical_forward && config.world_scale==1.1F,
            "Dead Space decodes the original recovered physical-forward camera at the scene boundary");
        for(const auto& sample:xrfg::tests::dead_space_basis_fixtures) {
            xrfg::LukeCameraBasis view{},legacy{};
            check(xrfg::luke_view_camera_basis(sample.producer,sample.perspective_w,view,config.axes),
                "Dead Space captured camera decodes");
            check(xrfg::luke_view_camera_basis(sample.producer,sample.perspective_w,legacy,xrfg::LukeProducerCameraAxes::view),
                "Dead Space old scene-camera control decodes");
            float difference{},old_difference{};
            for(unsigned i=0;i<3;++i) {
                difference=std::max(difference,std::abs(view[2][i]-sample.native_back[i]));
                old_difference=std::max(old_difference,std::abs(legacy[2][i]-sample.native_back[i]));
            }
            check(view[0]==sample.producer[0] && view[1]==sample.producer[1] && difference<.00001F && old_difference>1.F,
                "Dead Space scene back must equal the verified native source axis, with right/up unchanged");
        }
    }
    write("[GOT]\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F,
        "Ghost-only real configuration must initialize scale");
    write("[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F,
        "Horizon keeps its existing scale");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::physical_forward,
        "Horizon restores owner-accepted V33018 physical-forward contract");
    write("[KCD2]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.1F &&
        xrfg::luke_game_camera_config_from_ini(ini).axes == xrfg::LukeProducerCameraAxes::physical_forward,
        "KCD native physical-forward packet keeps accepted V33011 renderer handedness");
    write("[GENERAL]\nEnabled=1\n[UnknownLukeGame]\nWorldScale=0.8\n[RVR]\nQuality=2\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.8F,
        "Any Luke game section is supported without a whitelist");
    check(xrfg::luke_game_camera_config_from_ini(ini).axes ==
        xrfg::LukeProducerCameraAxes::view,
        "Unknown Luke titles retain the accepted view convention");
    write("[DOOMTDA]\nWorldScale=1.0\n[INDY]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Two active title sections reject ambiguous scale");
    write("[GOT]\nWorldScale=0.8\n[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Multiple WorldScale keys reject ambiguous configuration");
    write("[GOT]\nWorldScale=1.1\n[HFW]\nWorldScale=1.1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Equal scales with different camera axes stay ambiguous");
    // Uncharted: Legacy of Thieves keeps both of its games in one ini.
    write("[RVR]\nRenderMode2=6\n[U4ATE]\nFixDLSS=1\nWorldScale=1.0\n[UTLL]\nFixDLSS=1\nWorldScale=1.0\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 1.0F &&
        xrfg::luke_game_camera_config_from_ini(ini).axes == xrfg::LukeProducerCameraAxes::view,
        "Title sections that agree on the camera contract identify it");
    write("[GENERAL]\nEnabled=1\n");
    check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
        "Missing WorldScale rejects instead of assuming a default");
    for (const char* invalid : {"", "nan", "inf", "0.19", "5.01", "1.0garbage"}) {
        write((std::string("[GOT]\nWorldScale=") + invalid + "\n").c_str());
        check(xrfg::luke_world_scale_from_ini(ini) == 0.0F,
            "Malformed/out-of-range scale must reject");
    }
    std::filesystem::remove(ini);
    const auto host=std::filesystem::temp_directory_path()/
        (L"ofxr-luke-host-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(host/L"game");
    const auto runtime=host/L"runtime.ini";
    {std::ofstream file(runtime);file<<"[experimental]\nluke_afw_odd_serial_is_left=1\n";}
    check(xrfg::luke_host_odd_serial_is_left(host/L"game",runtime),"unconfigured host retains default phase");
    {std::ofstream file(host/L"game"/L"ofxr_bridge.ini");file<<"[experimental]\nluke_afw_odd_serial_is_left=0\n";}
    check(!xrfg::luke_host_odd_serial_is_left(host/L"game",runtime),"Horizon saved phase wins over standalone default");
    check(xrfg::luke_host_provider(host/L"game",host/L"bundled.dll")==host/L"bundled.dll","no game provider uses bundled ABI");
    {std::ofstream file(host/L"game"/L"PDAFWPlugin.dll");file<<"selection fixture";}
    check(xrfg::luke_host_provider(host/L"game",host/L"bundled.dll")==host/L"game"/L"PDAFWPlugin.dll","preserve game's OptiScaler provider");
    {std::ofstream file(host/L"bundled.dll");file<<"pinned beta6 fixture";}
    check(xrfg::luke_host_provider(host/L"game",host/L"bundled.dll")==host/L"bundled.dll",
        "pinned beta6 wins over game-local provider");
    std::filesystem::remove(host/L"bundled.dll");
    xrfg::DiagnosticKeyEdge key;
    check(!key.pressed(false)&&key.pressed(true)&&!key.pressed(true)&&!key.pressed(false)&&key.pressed(true),"held logging key toggles once, re-press toggles again");
    check(xrfg::kImageCaptureKey!=VK_F10 && xrfg::kLoggingKey!=VK_F10,"menu cannot trigger GPU capture or logging");
    for(const auto& file:{host/L"game"/L"PDAFWPlugin.dll",host/L"game"/L"ofxr_bridge.ini",runtime})std::filesystem::remove(file);
    std::filesystem::remove(host/L"game");std::filesystem::remove(host);
    return failures ? 1 : 0;
}
