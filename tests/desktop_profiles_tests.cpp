#include "xrfg/desktop_profile_picker.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <cstring>
using namespace xrfg::autonomous;
namespace {
void check(bool ok,const char* what){if(!ok)throw std::runtime_error(what);}
std::string read(const std::filesystem::path& file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const std::filesystem::path& file,float value){std::filesystem::create_directories(file.parent_path());std::ofstream out(file);out<<"[ofxr]\nnr_intensity="<<value<<"\n";check(bool(out),"create fixture profile");}
struct Frame {
    ImGuiContext* context=ImGui::CreateContext();DesktopProfiles profiles;SettingsWidgetState widgets;std::string rendered_text;
    Frame(){ImGui::SetCurrentContext(context);auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;io.BackendFlags|=ImGuiBackendFlags_RendererHasTextures;io.DisplaySize={720,900};io.DeltaTime=1.F/60;}
    ~Frame(){ImGui::SetCurrentContext(context);for(auto* t:ImGui::GetPlatformIO().Textures){t->SetTexID(ImTextureID_Invalid);t->SetStatus(ImTextureStatus_Destroyed);}ImGui::DestroyContext(context);}
    void draw(bool log=false) {
        ImGui::SetCurrentContext(context);ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0});ImGui::SetNextWindowSize({690,850});ImGui::Begin("Profile test");
        ScopedConfigurationSelection selected(profiles.selected());synchronize_settings_widgets(widgets,snapshot());
        if(log)ImGui::LogToBuffer();
        if(render_desktop_profile_picker(profiles,widgets,selected))synchronize_settings_widgets(widgets,snapshot());
        if(log){rendered_text=context->LogBuffer.c_str();ImGui::LogFinish();}
        ImGui::End();ImGui::Render();
        for(auto* t:ImGui::GetPlatformIO().Textures){if(t->Status==ImTextureStatus_WantDestroy){t->SetTexID(ImTextureID_Invalid);t->SetStatus(ImTextureStatus_Destroyed);}else if(t->Status==ImTextureStatus_WantCreate||t->Status==ImTextureStatus_WantUpdates){t->SetTexID(static_cast<ImTextureID>(t->UniqueID+1));t->SetStatus(ImTextureStatus_OK);}}
    }
    ImGuiWindow* popup(){for(auto* w:context->Windows)if(w->Active && (w->Flags&ImGuiWindowFlags_Popup) && std::strstr(w->Name,"##Combo"))return w;return nullptr;}
    void open(){ImGui::SetCurrentContext(context);auto* w=ImGui::FindWindowByName("Profile test");check(w!=nullptr,"desktop header rendered");ImGui::ActivateItemByID(w->GetID("##Settings"));draw();draw(true);check(popup()!=nullptr,"profile dropdown opens through real ImGui navigation");}
    void choose(const char* name){auto* w=popup();check(w!=nullptr,"profile list visible");ImGui::ActivateItemByID(w->GetID(name));draw();draw();}
};
}
int main(){
    try {
        const auto root=std::filesystem::temp_directory_path()/(L"ofxr-desktop-profiles-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());check(initialize_user_configuration(),"isolated desktop defaults");
        const auto global=global_configuration_path();const auto original=read(global);check(saved_game_profiles().empty(),"missing Profiles folder leaves only Global default");const auto alpha=game_configuration_path(L"Alpha.exe"),beta=game_configuration_path(L"Beta.exe");
        write(alpha,.25F);write(beta,.75F);write(root/L"Profiles"/L"ignored.ini.bak",.1F);std::filesystem::create_directory(root/L"Profiles"/L"directory.ini");write(root/L"Profiles"/L"nested"/L"hidden.ini",.1F);
        check(saved_game_profiles()==std::vector<std::filesystem::path>{alpha,beta},"enumerate sorted direct INI profiles only");
        DesktopProfiles model;check(model.selected()==global,"desktop starts at Global default");check(!model.choose(root/L"outside.ini"),"reject non-profile selection");
        check(model.choose(alpha),"choose saved offline profile");
        {ScopedConfigurationSelection selected(model.selected());check(snapshot().desired.nr_intensity==.25F,"offline profile loads its own values");auto change=snapshot().desired;change.nr_intensity=.4F;check(request(change),"save only chosen profile");
         {ScopedConfigurationSelection nested(beta);check(snapshot().desired.nr_intensity==.75F,"nested UI scopes select independently");}check(configuration_path()==alpha,"nested scope restores selected profile");}
        check(configuration_path()==global && read(global)==original,"desktop selection leaves tray/global routing and bytes unchanged");
        model.observe(ActiveGameProfile{beta,100});check(model.selected()==beta,"new game overrides offline choice");model.choose(global);model.observe(ActiveGameProfile{beta,100});check(model.selected()==global,"manual Global sticks for same live process");
        model.observe(std::nullopt);check(model.selected()==global,"busy discovery cannot spuriously switch profile");model.observe(ActiveGameProfile{beta,101});check(model.selected()==beta,"restart of same game automatically reattaches desktop");model.observe(ActiveGameProfile{});check(model.selected()==global,"automatic selection returns to defaults on disconnect");
        Frame frame;check(frame.profiles.choose(beta),"preview existing Beta");frame.draw();frame.open();
        const std::string logged=frame.rendered_text;check(logged.find("Global default")<logged.find("Alpha.exe.ini"),"Global default is above saved profiles in rendered popup");
        frame.choose("Alpha.exe.ini");check(frame.profiles.selected()==alpha && frame.widgets.draft.nr_intensity==.4F,"clicking saved profile reloads displayed values immediately");
        frame.open();const auto gamma=game_configuration_path(L"Gamma.exe");write(gamma,.9F);frame.draw();check(frame.profiles.saved().size()==2,"profile enumeration remains cached while popup stays open");
        frame.choose("Global default");check(frame.profiles.selected()==global,"Global default remains selectable");
        frame.open();check(frame.profiles.saved().size()==3,"reopening dropdown discovers new saved profile");frame.choose("Global default");
        std::filesystem::remove(gamma);frame.open();check(frame.profiles.saved().size()==2,"reopening dropdown removes deleted profile");frame.choose("Beta.exe.ini");check(frame.widgets.draft.nr_intensity==.75F,"switching back keeps independent persisted settings");
        check(read(global)==original,"opening/changing desktop profile never writes defaults");
        std::cout<<"Desktop scoped profiles, automatic selection policy, actual dropdown refresh/click/order and independent settings PASS\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
