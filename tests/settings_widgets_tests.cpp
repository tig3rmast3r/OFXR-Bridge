#include "xrfg/settings_widgets.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/rife_model_catalog.hpp"
#include <fstream>
#include "imgui.h"
#include "imgui_internal.h"

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace xrfg::autonomous;
int checks{};
void require(bool value, const std::string& detail) {
    ++checks;
    if (!value) throw std::runtime_error(detail);
}

struct Context {
    ImGuiContext* context{};
    SettingsWidgetState widgets{};
    ImVec2 size{};
    bool compact{};
    std::string rendered_text;
    std::function<bool(SettingsWidgetState&)> profile_header;
    const Snapshot* injected_snapshot{};
    explicit Context(ImVec2 dimensions, bool small) : size(dimensions), compact(small) {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = size;
        io.DeltaTime = 1.F / 60.F;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        ImGui::StyleColorsDark();
    }
    ~Context() {
        ImGui::SetCurrentContext(context);
        for (auto* texture : ImGui::GetPlatformIO().Textures) {
            texture->SetTexID(ImTextureID_Invalid);
            texture->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGui::DestroyContext(context);
    }
    void draw(bool capture_text=false) {
        ImGui::SetCurrentContext(context);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoSavedSettings);
        if(capture_text)ImGui::LogToBuffer();
        render_settings_widgets(widgets, injected_snapshot?*injected_snapshot:snapshot(), {.compact = compact,.desktop_profile_header = profile_header});
        if(capture_text){rendered_text=GImGui->LogBuffer.c_str();ImGui::LogFinish();}
        ImGui::End();
        ImGui::Render();
        // This fixture exercises CPU widgets/navigation, not GPU rendering.
        // Honor the vendored 1.92 dynamic texture protocol with CPU identities.
        for (auto* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->Status == ImTextureStatus_WantDestroy) {
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            } else if (texture->Status == ImTextureStatus_WantCreate ||
                texture->Status == ImTextureStatus_WantUpdates) {
                texture->SetTexID(static_cast<ImTextureID>(texture->UniqueID + 1));
                texture->SetStatus(ImTextureStatus_OK);
            }
        }
        require(ImGui::GetCurrentContext() == context, "shared widgets preserve caller's context");
        require(ImGui::GetDrawData()->TotalVtxCount > 0, "settings surface renders real draw data");
    }
    void activate(const char* child, const char* label) {
        ImGui::SetCurrentContext(context);
        for (auto* window : context->Windows) {
            if (std::strstr(window->Name, child)) {
                ImGui::ActivateItemByID(window->GetID(label));
                return;
            }
        }
        throw std::runtime_error("expected settings child was not rendered");
    }
    // Segmented controls and cards are buttons under their control's ID. RIFE
    // model stays a dropdown, reached through its popup.
    void choose(const char* label, int index, const char* item, const char* child = "/Generation_") {
        ImGui::SetCurrentContext(context);
        if (std::strcmp(label, "RIFE model") != 0) {
            for (auto* window : context->Windows) {
                if (std::strstr(window->Name, child)) {
                    ImGui::ActivateItemByID(ImHashStr(item, 0, window->GetID(label)));
                    draw();
                    draw();
                    return;
                }
            }
            throw std::runtime_error("expected settings child was not rendered");
        }
        activate(child, "##RIFE model");
        draw();
        draw();
        ImGui::SetCurrentContext(context);
        ImGuiWindow* popup{};
        for (auto* window : context->Windows)
            if (window->Active && (window->Flags & ImGuiWindowFlags_Popup) &&
                std::strstr(window->Name, "##Combo")) popup = window;
        if(!popup)throw std::runtime_error(std::string("combo did not open: ")+label+" in "+child+
            " generation="+std::to_string(snapshot().desired.frame_generation)+" backend="+std::to_string(snapshot().desired.backend));
        const auto seed = ImHashData(&index, sizeof(index), popup->IDStack.back());
        ImGui::ActivateItemByID(ImHashStr(item, 0, seed));
        draw();
        draw();
    }
    void activate_root(const char* label) {
        ImGui::SetCurrentContext(context);
        auto* window = ImGui::FindWindowByName("Settings");
        window->IDStack.push_back(window->GetID("OFXRBridgeSettings"));
        ImGui::ActivateItemByID(window->GetID((std::string("###") + label).c_str()));
        window->IDStack.pop_back();
    }
    void select_tab(const char* label) {
        const int before = widgets.section;
        activate("/Rail_", label);
        draw();
        draw();
        require(before != widgets.section || std::strcmp(label, "Frame generation") == 0 || widgets.section != 0,
            "rail entry responds to ImGui navigation activation");
    }
};

void rebase_and_publish() {
    auto editable=snapshot().desired;editable.advanced_mode=true;require(request(editable),"enable editable submodes for delta merge fixture");
    SettingsWidgetState tray, headset;
    const auto initial = snapshot();
    synchronize_settings_widgets(tray, initial);
    synchronize_settings_widgets(headset, initial);
    require(tray.initialized && tray.draft == initial.desired,
        "first draw uses current persisted values even at initial revision");
    tray.draft.enabled = !initial.desired.enabled;
    tray.dirty = true;
    require(publish_settings_widgets(tray), "tray edit published through real control channel");
    headset.draft.nr_working_scale = .61F;
    headset.draft.nr_intensity = .42F;
    headset.dirty = true;
    headset.scale_editing = true;
    const auto concurrent = snapshot();
    synchronize_settings_widgets(headset, concurrent);
    require(headset.draft.enabled == tray.draft.enabled && headset.draft.nr_working_scale == .61F,
        "incoming tray change rebases without losing local headset slider edit");
    require(headset.base == concurrent.desired, "new baseline records current external values");
    require(!publish_settings_widgets(headset) && snapshot().revision == concurrent.revision,
        "resource-changing resolution waits until slider release");
    auto other = concurrent.desired;
    other.afw_mode = 2;
    require(request_changes(concurrent.desired, other), "publish interleaved external edit");
    headset.scale_editing = false;
    require(publish_settings_widgets(headset), "released headset edit publishes atomically");
    const auto final = snapshot();
    require(final.desired.enabled == tray.draft.enabled && final.desired.afw_mode == 2 &&
        final.desired.nr_working_scale == .61F && final.desired.nr_intensity == .42F,
        "publication preserves both earlier and raced external edits");
    require(headset.draft == final.desired && !headset.dirty && !headset.save_failed,
        "successful UI publication reloads shared current settings");
    const auto revision = final.revision;
    require(!publish_settings_widgets(headset) && snapshot().revision == revision,
        "idle UI does not republish settings");
    headset.draft.native_scale = 73;
    headset.dirty = headset.scale_editing = true;
    finish_settings_widget_edits(headset);
    require(snapshot().desired.native_scale == 73 && !headset.dirty && !headset.scale_editing,
        "closing a held resolution edit publishes without another ImGui frame");
}

void route_motion_widgets() {
    auto settings = snapshot().desired;
    settings.frame_generation = 0; settings.backend = 3;
    settings.motion_vectors = 0; settings.triple_frame_gen = false;
    require(request(settings), "initialize OFXR optical-flow selection");
    Context tray(ImVec2(674, 861), false), headset(ImVec2(1024, 768), true);
    tray.draw(); tray.draw(); headset.draw(); headset.draw();
    tray.choose("OFXR engine", 3, "Nvidia 0.3.0");
    require(snapshot().desired.backend == 2, "new display order keeps 0.3.0 on persisted backend 2");
    headset.draw();
    headset.choose("OFXR engine", 2, "Nvidia 0.2.13");
    require(snapshot().desired.backend == 3, "new display order keeps 0.2.13 on persisted backend 3");
    tray.draw();
    tray.choose("Frame generation", 4, "RIFE");
    require(snapshot().desired.frame_generation == 4,
        "RIFE is displayed last while keeping its persisted mode ID 4");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR");
    require(snapshot().desired.motion_vectors == 0,
        "returning from RIFE preserves the independent OFXR optical-flow selection");
    tray.draw();
    tray.choose("Frame generation", 1, "Hybrid");
    require(snapshot().desired.frame_generation == 3, "actual tray selects Hybrid with stable mode ID");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR");
    require(snapshot().desired.motion_vectors == 0, "Hybrid preserves independent OFXR optical flow selection");
    tray.draw();
    tray.choose("Frame generation", 2, "FSR 3.1");
    require(snapshot().desired.frame_generation == 1, "actual tray selects FSR");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR");
    require(snapshot().desired.motion_vectors == 0,
        "returning from FSR restores OFXR optical flow");
    tray.draw();
    tray.choose("Frame generation", 3, "DLSS-G");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR");
    require(snapshot().desired.motion_vectors == 0,
        "returning from DLSS-G restores OFXR optical flow");
}
void route_multiplier_widgets() {
    auto settings = snapshot().desired;
    settings.frame_generation = 0; settings.backend = 3;
    settings.motion_vectors = 0; settings.triple_frame_gen = false;
    require(request(settings), "initialize independent multipliers");
    Context tray(ImVec2(674, 861), false), headset(ImVec2(1024, 768), true);
    tray.draw(); tray.draw(); headset.draw(); headset.draw();
    tray.choose("Multiplier", 1, "3X");
    require(snapshot().desired.triple_frame_gen, "actual DJ menu selects 3X");
    headset.draw();
    headset.choose("Frame generation", 3, "DLSS-G");
    require(!snapshot().desired.triple_frame_gen,
        "DLSS-G keeps its own 2X after DJ selects 3X");
    headset.choose("Multiplier", 1, "3X");
    tray.draw(); tray.choose("Frame generation", 0, "OFXR");
    // Since V609, DLSS-G has canonical backend0. Select Djules explicitly
    // after leaving it instead of assuming DLSS-G retained a Djules presenter.
    tray.choose("OFXR engine", 2, "Nvidia 0.2.13");
    tray.choose("Multiplier", 0, "2X");
    headset.draw(); headset.choose("Frame generation", 3, "DLSS-G");
    require(snapshot().desired.triple_frame_gen,
        "DLSS-G keeps its 3X after DJ selects 2X");
    tray.draw(); tray.choose("Frame generation", 0, "OFXR");
    tray.choose("OFXR engine", 2, "Nvidia 0.2.13");
    require(!snapshot().desired.triple_frame_gen, "DJ keeps its own 2X");
}
void rife_catalog() {
    const auto runtime=std::filesystem::temp_directory_path()/(L"OFXR model catalog "+std::to_wstring(GetCurrentProcessId()));
    const auto weights=runtime/L"models";std::filesystem::create_directories(weights);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{runtime};
    require(xrfg::available_rife_models(runtime).empty(),"missing weights produce no phantom versions");
    for(const auto* name:{"flownet_v4.4.pkl","flownet_v4.3.pkl","flownet_v4.25.lite.pkl","flownet_v99.pkl","notes.txt"})
        std::ofstream(weights/name)<<"catalog fixture";
    auto models=xrfg::available_rife_models(runtime);
    require(models.size()==2 && models[0].id==3 && models[0].name=="4.4" && models[1].id==4,
        "names come from installed compatible weights; retired and unknown networks are not selectable");
    std::ofstream(weights/"flownet_v4.6.pkl")<<"catalog fixture";
    models=xrfg::available_rife_models(runtime);
    require(models.size()==3 && models[0].name=="4.6","a new installed compatible version appears on refresh");
    std::filesystem::remove(weights/"flownet_v4.4.pkl");
    require(xrfg::available_rife_models(runtime).size()==2,"removed weights disappear without cached choices");
    require(xrfg::available_rife_models(L"relative/models").empty(),"no relative folder search");
}

void rife_widgets() {
    const auto prior=snapshot().desired;
    Context tray(ImVec2(674,861),false),headset(ImVec2(1024,768),true);
    auto rife=snapshot().desired;select_generation(rife,4,2);rife.advanced_mode=false;rife.rife_model=2;
    require(request(rife),"select basic RIFE with an older saved model");
    tray.draw();headset.draw();tray.select_tab("Frame generation");headset.select_tab("Frame generation");tray.draw(true);headset.draw(true);
    require(snapshot().desired.rife_model==3,"basic RIFE uses 4.4");
    require(tray.rendered_text.find("RIFE model")==std::string::npos && headset.rendered_text.find("RIFE model")==std::string::npos,
        "basic desktop and headset hide the model selector");
    auto advanced=snapshot().desired;advanced.advanced_mode=true;
    require(request(advanced),"enable advanced RIFE models");tray.draw();headset.draw();tray.draw(true);headset.draw(true);
    require(tray.rendered_text.find("RIFE model")!=std::string::npos && headset.rendered_text.find("RIFE model")!=std::string::npos,
        "Advanced shows the model selector on desktop and headset");
    const auto runtime=configuration_path().parent_path()/L"models with spaces";
    std::filesystem::create_directories(runtime/L"models");
    std::ofstream(runtime/L"models/flownet_v4.6.pkl")<<"catalog only";
    require(WritePrivateProfileStringW(L"tray",L"rife_runtime_dir",runtime.c_str(),global_configuration_path().c_str())!=0,
        "explicit isolated developer model catalog");
    tray.choose("RIFE model",0,"4.6");
    require(snapshot().desired.rife_model==2,"actual Advanced dropdown selects the only installed compatible model");
    advanced=snapshot().desired;advanced.advanced_mode=false;require(request(advanced),"leave Advanced");
    require(snapshot().desired.rife_model==3,"leaving Advanced restores 4.4 immediately");
    require(WritePrivateProfileStringW(L"tray",L"rife_runtime_dir",nullptr,global_configuration_path().c_str())!=0,"remove isolated developer override");
    std::error_code error;std::filesystem::remove_all(runtime,error);
    require(request(prior),"restore pre-RIFE widget settings");tray.select_tab("Overlay");
}

void release_controls() {
    auto simple=Settings{};simple.backend=1;simple.scale=3;simple.nr_second_pass=true;require(request(simple),"seed simple release UI");
    Context desktop({690,900},false);desktop.draw();desktop.draw(true);
    require(desktop.rendered_text.find("Motion source")==std::string::npos,"basic mode hides motion source");
    require(desktop.rendered_text.find("Framework+director")==std::string::npos,"basic UI has no combined mode or help");
    desktop.choose("Pacing",0,"Framework");require(snapshot().desired.pacing_mode==0,"basic Framework selection works");
    desktop.choose("Pacing",1,"Director");require(snapshot().desired.pacing_mode==1,"basic Director selection works");
    require(desktop.rendered_text.find("Reset defaults")!=std::string::npos,"reset appears in the footer");
    const auto before_reset=snapshot().desired;
    desktop.activate_root("Reset defaults");desktop.draw();desktop.draw();
    require(snapshot().desired==before_reset,"the first Reset click only arms the button");
    desktop.activate_root("Reset defaults");desktop.draw();desktop.draw();
    require(snapshot().desired==sanitize(Settings{}),"a second Reset click restores all factory settings");
    const auto single_pass=snapshot().desired;
    desktop.select_tab("Neural Rendering");desktop.draw(true);
    require(desktop.rendered_text.find("2x NR")!=std::string::npos,
        "basic Neural Rendering UI exposes one second-pass checkbox");
    desktop.activate("/Neural_","2x NR");desktop.draw();
    auto double_pass=single_pass;double_pass.nr_second_pass=true;
    require(snapshot().desired==double_pass,
        "second-pass checkbox is editable with NR off and changes no shared NR parameters");
    desktop.activate("/Neural_","2x NR");desktop.draw();
    require(snapshot().desired==single_pass,"second-pass checkbox returns to the unchanged single-pass settings");
    desktop.select_tab("Overlay");desktop.draw(true);
    require(desktop.rendered_text.find("Experimental")==std::string::npos &&
        desktop.rendered_text.find("Output CSV")==std::string::npos &&
        desktop.rendered_text.find("Open ofxr-bridge.ini")==std::string::npos,"basic Display removes experimental and redundant controls");
    desktop.select_tab("Reconstruction");desktop.draw(true);
    require(snapshot().desired.luke_osr==2 && desktop.rendered_text.find("OSR quality")!=std::string::npos,
        "reset selects OSR in the actual desktop Reconstruction control");
    require(desktop.rendered_text.find("Setup: Luke Ross")!=std::string::npos &&
        desktop.rendered_text.find("Setup: UEVR (Alternating / AFR)")!=std::string::npos &&
        desktop.rendered_text.find("Source")!=std::string::npos,"Frame Reconstruction names its sources and their setup");
    desktop.choose("Reconstruction",2,"AFW","/Framewarp_");
    require(snapshot().desired.luke_osr==0,"second reconstruction choice selects legacy AFW");
    desktop.choose("Reconstruction",1,"OSR","/Framewarp_");
    require(snapshot().desired.luke_osr==2,"first reconstruction choice selects OSR");
    desktop.draw(true);
    require(desktop.rendered_text.find("AFW + OSR")==std::string::npos &&
        desktop.rendered_text.find("Band fill")==std::string::npos,"retired band-fill controls are absent");
    require(desktop.rendered_text.find("Frame Reconstruction only")!=std::string::npos && desktop.rendered_text.find("Clear before framewarp")==std::string::npos,"basic Luke UI exposes per-game Frame Reconstruction only without Clear");
    desktop.activate("/Framewarp_","Frame Reconstruction only");desktop.draw();
    require(snapshot().desired.afw_only,"actual AFW Only control persists settings");
    auto advanced=snapshot().desired;advanced.advanced_mode=true;advanced.afw_only=false;require(request(advanced),"enable advanced UI by configuration");
    desktop.select_tab("Frame generation");desktop.draw(true);
    require(desktop.rendered_text.find("Motion source")!=std::string::npos,"advanced mode restores motion source");
    desktop.choose("Pacing",2,"Framework+director");require(snapshot().desired.pacing_mode==2,"advanced combined selection works");
}
void actual_widgets() {
    auto advanced=snapshot().desired;advanced.advanced_mode=true;require(request(advanced),"enable advanced-only UI fixture");
    Context tray(ImVec2(674, 861), false);
    Context headset(ImVec2(1024, 768), true);
    const auto before = snapshot();
    tray.draw();
    headset.draw();
    require(snapshot().revision == before.revision, "opening either settings surface changes no values");
    tray.activate("/Generation_", "Enable frame generation");
    tray.draw();
    const auto enabled = snapshot();
    require(enabled.desired.enabled != before.desired.enabled,
        "real generation checkbox publishes hot setting");
    headset.draw();
    require(headset.widgets.draft.enabled == enabled.desired.enabled,
        "headset reflects hot tray checkbox edit");
    require(enabled.desired.lower_vram == 0, "opening settings preserves Lower VRAM off");
    tray.activate("/Generation_", "Fast mode");
    tray.draw(); tray.draw();
    require(snapshot().desired.separate_synthesis == 1 && snapshot().desired.lower_vram == 1,
        "Lower VRAM enables private textures and reduced rings together");
    headset.draw();
    require(headset.widgets.draft.lower_vram == 1, "headset reflects Lower VRAM on");
    headset.activate("/Generation_", "Fast mode");
    headset.draw(); headset.draw();
    require(snapshot().desired.separate_synthesis == 0 && snapshot().desired.lower_vram == 0,
        "Lower VRAM off restores both controls");
    tray.draw();
    require(tray.widgets.draft.lower_vram == 0, "tray reflects Lower VRAM off");
    headset.select_tab("Neural Rendering");
    headset.activate("/Neural_", "Enable Neural Rendering");
    headset.draw();
    require(snapshot().desired.nr_enabled != enabled.desired.nr_enabled,
        "real headset Neural Rendering checkbox publishes hot setting");
    tray.draw();
    require(tray.widgets.draft.nr_enabled == snapshot().desired.nr_enabled,
        "tray reflects hot headset checkbox edit");
    const auto before_second=snapshot().desired;
    headset.activate("/Neural_","2x NR");headset.draw();
    auto after_second=before_second;after_second.nr_second_pass=!after_second.nr_second_pass;
    require(snapshot().desired==after_second,
        "real headset second-pass checkbox publishes only its independent flag");
    tray.select_tab("Neural Rendering");tray.draw();
    require(tray.widgets.draft.nr_second_pass==after_second.nr_second_pass,
        "desktop reflects the hot headset second-pass edit");
    tray.activate("/Neural_","2x NR");tray.draw();headset.draw();
    require(snapshot().desired==before_second && headset.widgets.draft.nr_second_pass==before_second.nr_second_pass,
        "desktop second-pass edit reaches F10 without changing model, scale or frame generation");
    headset.select_tab("Reconstruction");
    // The provider controls sit behind the persisted Advanced parameters checkbox.
    headset.activate("/Framewarp_", "Advanced");
    headset.draw();
    require(snapshot().desired.luke_advanced, "Luke advanced checkbox publishes hot setting");
    headset.activate("/Framewarp_", "Debug framewarp");
    headset.draw();
    require(snapshot().desired.afw_debug != enabled.desired.afw_debug,
        "real Luke debug checkbox publishes hot setting");
    headset.select_tab("Overlay");
    headset.choose("Counter type", 4, "On controller", "/Overlay_");
    require(snapshot().desired.overlay_position == 5, "headset controller meter publishes existing display setting");
    tray.select_tab("Overlay");
    tray.choose("Counter type", 5, "Bottom edge", "/Overlay_");
    require(snapshot().desired.overlay_position == 6, "tray bottom meter preserves mode 6");
    headset.draw();
    require(headset.widgets.draft.overlay_position == 6, "both surfaces reflect new meter mode");
    const bool counter_was_enabled=snapshot().desired.overlay_enabled;
    headset.activate("/Overlay_", "On-screen FPS counter");headset.draw();headset.draw();
    require(snapshot().desired.overlay_enabled!=counter_was_enabled && snapshot().desired.overlay_position==6,
        "counter checkbox toggles visibility without discarding selected type");
    tray.draw();tray.draw();
    require(tray.widgets.draft.overlay_enabled==snapshot().desired.overlay_enabled,
        "desktop reflects headset counter visibility");
    tray.choose("Counter type", 0, "Upper left", "/Overlay_");
    require(snapshot().desired.overlay_position==1 && snapshot().desired.overlay_enabled!=counter_was_enabled,
        "type selection does not implicitly enable or disable the counter");
    tray.activate("/Overlay_", "On-screen FPS counter");tray.draw();tray.draw();
    require(snapshot().desired.overlay_enabled==counter_was_enabled && snapshot().desired.overlay_position==1,
        "counter toggle restores the selected type");
    headset.select_tab("Logging");
    headset.activate("/Logging_", "Diagnostic logging");
    headset.draw();
    require(snapshot().desired.diagnostics != enabled.desired.diagnostics,
        "real diagnostics checkbox publishes setting");
    headset.select_tab("Overlay");
    headset.activate("/Overlay_", "Experimental");headset.draw();headset.draw();
    const bool previous_camera=snapshot().desired.r19_camera;
    headset.activate("/Overlay_", "R19 synthetic camera (experimental)");headset.draw();headset.draw();
    require(snapshot().desired.r19_camera!=previous_camera,"experimental camera setting remains live in Display");
    headset.select_tab("Frame generation");
    tray.select_tab("Neural Rendering");
    tray.select_tab("Reconstruction");
    tray.select_tab("Logging");
    tray.select_tab("Tests");
    tray.select_tab("Overlay");
    auto status = snapshot().runtime;
    status.applied_revision = snapshot().revision;
    status.ready = 1;
    status.sessions = 1;
    report_runtime(status);
    const auto revision = snapshot().revision;
    tray.draw();
    headset.draw();
    require(snapshot().revision == revision, "applied-status updates are display-only");
    report_overlay_limit(OverlaySwapchain::menu,true);
    tray.draw(true);headset.draw(true);
    require(tray.rendered_text.find("OpenXR swapchain limit reached")!=std::string::npos &&
        headset.rendered_text.find("OpenXR swapchain limit reached")!=std::string::npos,
        "actual desktop and F10 widgets both render the persistent runtime limit warning");
    report_overlay_limit(OverlaySwapchain::menu,false);
    tray.draw(true);headset.draw(true);
    require(tray.rendered_text.find("OpenXR swapchain limit reached")==std::string::npos &&
        headset.rendered_text.find("OpenXR swapchain limit reached")==std::string::npos,
        "actual settings surfaces clear the warning after a successful retry");
    status=snapshot().runtime;status.swapchains.updated_ms=GetTickCount64();
    status.luke_detected=true;status.steamvr_runtime=true;status.vdxr_legacy=false;status.pacing_mode=2;
    status.swapchains.current=9;status.swapchains.menu=1;
    status.swapchains.fallback=true;status.swapchains.application=3;status.swapchains.total=14;
    status.swapchains.auxiliary_outputs=1;status.swapchains.private_snapshots=3;
    std::strcpy(status.swapchains.details,"App UNCLASSIFIED: 0x823, 16x16, array 1, format 28");
    report_runtime(status);
    tray.select_tab("Reconstruction");headset.select_tab("Reconstruction");
    tray.draw(true);headset.draw(true);
    for(const auto* view:{&tray,&headset}) {
        require(view->rendered_text.find("Use previous frame (debug only)")!=std::string::npos &&
            view->rendered_text.find("Total swapchains:")==std::string::npos,
            "Luke controls remain above collapsed swapchain Details");
        require(view->rendered_text.find("Swapchain fallback")!=std::string::npos,
            "status warns even with ownership Details collapsed");
    }
    require(tray.rendered_text.find("Swapchains")==std::string::npos && headset.rendered_text.find("Swapchains")==std::string::npos,
        "inventory is no longer a Reconstruction-only control");
    status.luke_detected=false;report_runtime(status);
    tray.select_tab("Logging");headset.select_tab("Logging");
    tray.activate("/Logging_","Swapchains");headset.activate("/Logging_","Swapchains");
    tray.draw(true);headset.draw(true);
    for(const auto* view:{&tray,&headset}) {
        require(view->rendered_text.find("Diagnostic logging")<view->rendered_text.find("Total swapchains:"),
            "expanded inventory belongs with diagnostic logging");
        require(view->rendered_text.find("Swapchain fallback")!=std::string::npos,
            "desktop/headset status and warning expose actual fallback");
        require(view->rendered_text.find("Total swapchains: 14")!=std::string::npos &&
            view->rendered_text.find("0x823")!=std::string::npos,
            "Logging exposes handle count and unknown app identity without Luke Ross");
    }
    require(tray.rendered_text.find("OFXR: 11")!=std::string::npos &&
        tray.rendered_text.find("Game: 3")!=std::string::npos &&
        tray.rendered_text.find("Counts cover")==std::string::npos,"concise owner split with no healthy boilerplate");
    require(tray.rendered_text.find("Swapchain budget fix")==std::string::npos &&
        headset.rendered_text.find("Swapchain budget fix")==std::string::npos,
        "INI-only startup sharing is absent from desktop and headset UI");
    status.luke_detected=true;report_runtime(status);
    tray.select_tab("Reconstruction");headset.select_tab("Reconstruction");
    auto quality_settings=snapshot().desired;quality_settings.afw_mode=3;quality_settings.luke_osr=0;
    require(request(quality_settings),"select Combined for live quality control");
    tray.draw();tray.draw();
    tray.choose("Reconstruction",0,"Off","/Framewarp_");
    require(snapshot().desired.afw_mode==0 && snapshot().desired.luke_osr==0,"primary Off also stops legacy AFW");
    tray.choose("Reconstruction",2,"AFW","/Framewarp_");
    require(snapshot().desired.afw_mode==3 && snapshot().desired.luke_osr==0,"primary AFW restores the legacy engine");
    tray.choose("Framewarp mode",0,"Alternate","/Framewarp_");
    require(snapshot().desired.afw_mode==1,"AFW mode first entry is Alternate eye, not Off");
    tray.choose("Framewarp mode",2,"Combined","/Framewarp_");
    require(snapshot().desired.afw_mode==3,"AFW mode third entry is Combined");
    status.afw_shading_rate=0;report_runtime(status);
    tray.choose("AFW shading rate",1,"2x2","/Framewarp_");
    require(snapshot().desired.afw_shading_rate==4,"live quality dropdown publishes beta6 2x2 index");
    tray.draw(true);
    require(tray.rendered_text.find("Quality change pending")!=std::string::npos &&
        tray.rendered_text.find("next launch")==std::string::npos,"AFW UI exposes active versus pending without claiming a restart");
    status.afw_shading_rate=4;report_runtime(status);tray.draw(true);
    require(tray.rendered_text.find("Quality change pending")==std::string::npos,
        "successful active quality update clears pending explanation");
    tray.choose("Reconstruction",1,"OSR","/Framewarp_");
    require(snapshot().desired.luke_osr==2,"desktop selects OSR from legacy AFW");
    tray.choose("OSR quality",1,"Faster","/Framewarp_");
    require(snapshot().desired.luke_osr_lower_quality,"desktop selects OSR cheap live");
    tray.choose("OSR quality",0,"Normal","/Framewarp_");
    require(!snapshot().desired.luke_osr_lower_quality,"desktop restores normal OSR live");
    headset.draw();headset.draw();
    headset.choose("Reconstruction",2,"AFW","/Framewarp_");
    require(snapshot().desired.luke_osr==0,"headset selects the second legacy AFW entry");
    headset.choose("Reconstruction",1,"OSR","/Framewarp_");
    require(snapshot().desired.luke_osr==2,"headset selects the first OSR entry");
    auto saved_dilate=snapshot().desired;saved_dilate.afw_depth_dilate_mode=2;
    require(request(saved_dilate),"load old profile with OSR depth dilation");
    tray.draw();tray.draw(true);headset.draw();headset.draw(true);
    for(auto* view:{&tray,&headset})require(
        view->rendered_text.find("AFW shading rate")==std::string::npos &&
        view->rendered_text.find("Ignore motion")==std::string::npos &&
        view->rendered_text.find("Depth dilation")==std::string::npos &&
        view->rendered_text.find("Use previous frame (debug only)")!=std::string::npos,
        "Full OSR hides dilation on desktop and headset");
    tray.choose("Reconstruction",2,"AFW","/Framewarp_");tray.draw(true);
    require(tray.rendered_text.find("Depth dilation")!=std::string::npos &&
        snapshot().desired.afw_depth_dilate_mode==2,"AFW retains its independent saved dilation");
    tray.choose("Depth dilation",0,"Off","/Framewarp_");
    tray.choose("Reconstruction",1,"OSR","/Framewarp_");
    tray.activate("/Framewarp_","Use previous frame");tray.draw();tray.draw();
    require(snapshot().desired.afw_mode==1,"Full OSR without the previous frame is the alternate-eye mode");
    tray.activate("/Framewarp_","Use previous frame");tray.draw();tray.draw();
    require(snapshot().desired.afw_mode==3,"Full OSR with the previous frame is Combined");
    tray.choose("Reconstruction",0,"Off","/Framewarp_");
    require(snapshot().desired.afw_mode==0,"primary Off disables reconstruction");
    tray.draw(true);headset.draw(true);
    require(tray.rendered_text.find("Use previous frame")==std::string::npos &&
        headset.rendered_text.find("Use previous frame")==std::string::npos,
        "primary Off hides both engines' advanced mode menus");
    require(snapshot().desired.luke_osr==2,"Off preserves the previous engine without re-enabling it");
    headset.choose("Reconstruction",1,"OSR","/Framewarp_");
    require(snapshot().desired.afw_mode==3 && snapshot().desired.luke_osr==2,"primary OSR restores an enabled reconstruction mode");

    auto osr_settings=snapshot().desired;osr_settings.afw_mode=3;
    osr_settings.luke_osr=0;require(request(osr_settings),"back to AFW");tray.draw();tray.draw(true);
    const auto content_height=[&](Context& view) {
        for(auto* window:view.context->Windows)
            if(window->Active && std::strstr(window->Name,"/Framewarp_"))return window->ContentSize.y;
        throw std::runtime_error("missing Luke child window");
    };
    tray.draw();tray.draw(); // settle ContentSize after the pending line disappears
    const auto unpadded=content_height(tray);
    std::strcpy(status.swapchains.details,"\n\n  \nApp UNCLASSIFIED: 0x823, 16x16, array 1, format 28\n\n\n");
    report_runtime(status);tray.draw();tray.draw();
    require(std::abs(content_height(tray)-unpadded)<1.F,
        "empty/padded ownership rows do not add a blank block before the final explanation");
    status.swapchains.fallback=false;status.swapchains.other=0;status.swapchains.details[0]=0;
    report_runtime(status);tray.draw(true);
    require(tray.rendered_text.find("Swapchain fallback")==std::string::npos,"fallback warning does not stick after a fresh healthy session");

}
void offline_preset() {
    using namespace xrfg::standalone;
    report_runtime({});
    auto desired=snapshot().desired;select_generation(desired,2,desired.backend);
    require(request(desired),"select native DLSS-G for offline profile editing");
    const auto selection=configuration_path();
    require(write_configuration_sections(selection,"[dlssg]\nexecutable=\n"),"start without a remembered game");
    Context already_open({900,1100},false);already_open.draw();already_open.draw();
    require(already_open.widgets.dlssg_executable.empty(),"desktop starts with no target");
    require(remember_dlssg_game(selection,process_executable()),"remember the game without changing its NVIDIA profile");
    already_open.activate("/Generation_","Preset details");already_open.draw();already_open.draw();
    already_open.activate("/Generation_","Refresh preset profile");already_open.draw();already_open.draw();
    require(already_open.widgets.dlssg_executable==process_executable(),
        "Refresh resolves a game learned after this settings window was opened");
    require(write_configuration_sections(selection,"[dlssg]\nexecutable=\n"),"reset target for automatic refresh fixture");
    Context learned_later({900,1100},false);learned_later.draw();learned_later.draw();
    require(learned_later.widgets.dlssg_executable.empty(),"automatic refresh fixture starts empty");
    require(remember_dlssg_game(selection,process_executable()),"tray learns target while desktop remains open");
    ImGui::SetCurrentContext(learned_later.context);ImGui::GetIO().DeltaTime=1.1F;
    learned_later.draw();
    require(learned_later.widgets.dlssg_executable==process_executable(),
        "open desktop automatically discovers the target after the game closed");
    Context desktop({900,1100},false);desktop.draw();desktop.draw(true);
    require(!snapshot().connected && desktop.widgets.dlssg_executable==process_executable(),
        "cold desktop settings restore last game while disconnected");
    require(desktop.rendered_text.find("Choose game executable")==std::string::npos &&
        desktop.rendered_text.find("DLSS-G preset")!=std::string::npos,
        "offline selector replaces file browser");
}
void automatic_buttons() {
    require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"enable benchmark GUI fixture in single INI");
    // Factory Auto pick is ON since V663. This click test exercises OFF->ON.
    auto initial=snapshot().desired;initial.benchmark_auto_pick=false;
    require(request(initial),"explicit unchecked state for auto-pick interaction fixture");
    RuntimeStatus runtime{};runtime.sessions=1;runtime.luke_detected=true;
    runtime.steamvr_runtime=true;runtime.vdxr_legacy=false;
    runtime.rife_max_multiplier=4;runtime.dlssg_preset_request=3;runtime.applied_revision=snapshot().revision;report_runtime(runtime);
    const auto prior=snapshot().desired;
    Context tray({690,900},false);tray.draw();tray.draw();tray.select_tab("Tests");tray.draw(true);
    for(const auto label:{"Without vectors","Complete","Luke Ross"})
        require(tray.rendered_text.find(label)!=std::string::npos,"All automatic controls are in Tests");
    require(tray.rendered_text.find("Open benchmark folder")==std::string::npos,"shared settings no longer opens shell folders");
    require(tray.rendered_text.find("Up to 35 tests")!=std::string::npos &&
        tray.rendered_text.find("Up to 59 tests")!=std::string::npos &&
        tray.rendered_text.find("Up to 42 tests")!=std::string::npos,
        "SteamVR Display describes its actual pacing and AFW matrix");
    tray.activate("/Tests_","###Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==35,
        "Display button starts the actual SteamVR optical matrix");
    tray.activate("/Tests_","###Stop tests");tray.draw();tray.draw();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==prior,"Display Stop restores original settings");
    runtime.steamvr_runtime=false;runtime.vdxr_runtime=true;runtime.vdxr_legacy=true;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("Up to 11 tests")!=std::string::npos && tray.rendered_text.find("Up to 17 tests")!=std::string::npos,
        "VDXR Display describes the Legacy spreadsheet suite");
    tray.activate("/Tests_","###Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==11,
        "Legacy Display starts Normal/Fast for every optical route");
    tray.activate("/Tests_","###Stop tests");tray.draw();tray.draw();
    runtime.vdxr_legacy=false;runtime.applied_revision=snapshot().revision;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("Up to 35 tests")!=std::string::npos,
        "opt-in VDXR exposes the full comparison in the actual Display surface");
    tray.activate("/Tests_","###Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==35,
        "VDXR Display button starts all three pacing variants");
    tray.activate("/Tests_","###Stop tests");tray.draw();tray.draw();
    auto simple=snapshot().desired;simple.advanced_mode=false;require(request(simple),"return to basic UI policy");
    require(simple.afw_mode!=0,"reconstruction stays on for the Advanced check");
    tray.select_tab("Reconstruction");tray.draw();tray.draw(true);
    require(tray.rendered_text.find("Debug framewarp")==std::string::npos &&
        tray.rendered_text.find("Use previous frame")==std::string::npos,"Advanced needs AdvancedMode=1");
    require(snapshot().desired.afw_mode==3&&!snapshot().desired.afw_debug&&
        snapshot().desired.afw_depth_dilate_mode==0,"hidden reconstruction values reset when Advanced is disabled");
    auto simple_flow=snapshot().desired;select_generation(simple_flow,0,2);
    simple_flow.preset=0;simple_flow.scale=3;simple_flow.backward=true;require(request(simple_flow),"old hidden flow values requested in Basic");
    Context basic_headset({1024,768},true);basic_headset.draw();basic_headset.draw();
    tray.select_tab("Frame generation");basic_headset.select_tab("Frame generation");tray.draw(true);basic_headset.draw(true);
    require(tray.rendered_text.find("Flow tuning")!=std::string::npos&&basic_headset.rendered_text.find("Flow tuning")!=std::string::npos&&
        snapshot().desired.preset==0&&snapshot().desired.scale==3&&!snapshot().desired.backward,
        "Basic desktop and headset expose flow size and retain Slow while resetting backward");
    tray.activate("/Generation_","Flow tuning");tray.draw();tray.draw(true);
    basic_headset.activate("/Generation_","Flow tuning");basic_headset.draw();basic_headset.draw(true);
    require(tray.rendered_text.find("Flow resolution")!=std::string::npos &&
        basic_headset.rendered_text.find("Flow resolution")!=std::string::npos &&
        tray.rendered_text.find("Bidirectional flow")==std::string::npos &&
        basic_headset.rendered_text.find("Bidirectional flow")==std::string::npos,
        "Basic flow size is editable in both surfaces while backward stays hidden");
    tray.choose("Flow quality",2,"Fast");
    require(snapshot().desired.preset==0,"hidden flow Fast cannot be activated in Basic");
    tray.choose("Flow quality",1,"Medium");tray.choose("Flow resolution",1,"75%");
    require(snapshot().desired.preset==1&&snapshot().desired.scale==1,
        "visible Basic flow quality and size publish their selected values");

    tray.select_tab("Tests");
    runtime.applied_revision=snapshot().revision;runtime.pacing_mode=1;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("Up to 35 tests")!=std::string::npos && tray.rendered_text.find("Up to 42 tests")!=std::string::npos,"basic Display shows current two/three-buffer counts");
    tray.activate("/Tests_","How tests work");tray.draw();tray.draw(true);
    require(tray.rendered_text.find("7.5 seconds per configuration")!=std::string::npos &&
        tray.rendered_text.find("2.5 seconds settling, then 5 seconds measured")!=std::string::npos,
        "Display describes the actual shorter benchmark timing");
    tray.activate("/Tests_","###Tests without vectors");tray.draw();tray.draw();
    require(snapshot().benchmark.count==35 && !snapshot().benchmark.advanced,"basic Display starts current buffer matrix");
    tray.activate("/Tests_","###Stop tests");tray.draw();tray.draw();
    require(tray.rendered_text.find("Auto pick best one")!=std::string::npos,"auto-pick checkbox is in Display");
    require(tray.rendered_text.find("On completion: within")==std::string::npos &&
        tray.rendered_text.find("Native vectors take priority")==std::string::npos,
        "automatic selection rules stay out of the product interface");
    tray.activate("/Tests_","Auto pick best one");tray.draw();tray.draw();
    require(snapshot().desired.benchmark_auto_pick,"actual checkbox saves the per-profile automatic preference");
    tray.activate("/Tests_","###Tests without vectors");tray.draw();tray.draw();
    require(snapshot().benchmark.auto_pick,"UI-started suite captures the auto-pick preference");
    tray.activate("/Tests_","###Stop tests");tray.draw();tray.draw();
    require(snapshot().desired.benchmark_auto_pick && snapshot().benchmark.phase==BenchmarkPhase::stopped,"Stop restores settings without clearing the auto-pick preference");
}
} // namespace

void gpu_telemetry_and_quality() {
    Snapshot sample;sample.connected=true;
    sample.runtime.heartbeat_ms=GetTickCount64();sample.runtime.gpu_usage={GetTickCount64(),81,true};
    sample.runtime.cpu_usage={GetTickCount64(),37,true};
    sample.runtime.video_memory={GetTickCount64(),60,100,40,20,10,123,true,true};
    for(bool compact:{false,true}) {
        Context ui(compact?ImVec2(1024,768):ImVec2(690,900),compact);
        ui.injected_snapshot=&sample;ui.draw();ui.draw(true);
        require(ui.rendered_text.find("VRAM")!=std::string::npos && ui.rendered_text.find("60%")!=std::string::npos && ui.rendered_text.find("81%")!=std::string::npos,
            "desktop and headset headers both render independently measured memory and GPU percentage");
        const auto gpu=ui.rendered_text.find("GPU"),cpu=ui.rendered_text.find("CPU"),vram=ui.rendered_text.find("VRAM");
        require(gpu<cpu && cpu<vram && ui.rendered_text.find("37%")!=std::string::npos,
            "CPU meter is below GPU and above VRAM on desktop and headset");
        sample.runtime.cpu_usage.updated_ms=GetTickCount64()-4000;ui.draw(true);
        require(ui.rendered_text.find("37%")==std::string::npos,"stale CPU utilization is hidden");
        sample.runtime.cpu_usage.updated_ms=GetTickCount64();
        sample.runtime.gpu_usage.updated_ms=GetTickCount64()-4000;ui.draw(true);
        require(ui.rendered_text.find("81%")==std::string::npos,"stale utilization is not kept indefinitely");
        sample.runtime.gpu_usage.updated_ms=GetTickCount64();
    }
    auto desired=snapshot().desired;select_generation(desired,4,2);
    desired.rife_resolution=18;require(request(desired),"select RIFE Medium");
    Context desktop({690,900},false);desktop.draw();desktop.draw(true);
    require(desktop.rendered_text.find("Quality")!=std::string::npos && desktop.rendered_text.find("Medium")!=std::string::npos,
        "RIFE exposes Quality with Medium default");
    require(desktop.rendered_text.find("Motion inference resolution")==std::string::npos,"old resolution slider label removed");
    constexpr const char* names[]{"Potato","Low","Medium","High","Very high","Ultra","Godlike"};
    constexpr int values[]{10,15,18,21,25,30,36};
    for(int i=0;i<7;++i) {
        desktop.choose("Quality",i,names[i]);
        require(snapshot().desired.rife_resolution==values[i],"each named Quality selection publishes the exact preset");
    }
    Context headset({1024,768},true);headset.draw();headset.draw();
    headset.choose("Quality",2,"Medium");desktop.draw();
    require(snapshot().desired.rife_resolution==18 && desktop.widgets.draft.rife_resolution==18,
        "headset Quality selection synchronizes with the desktop");
}

void desktop_header_is_not_a_headset_control() {
    int called{};
    Context headset({1024,768},true);headset.profile_header=[&](auto&){++called;return false;};headset.draw();
    require(called==0,"headset never invokes the desktop profile picker");
    Context desktop({690,900},false);desktop.profile_header=[&](auto&){++called;return false;};desktop.draw();
    require(called==1,"desktop may render its profile header");
}

void aer_framegen_widgets() {
    auto saved=snapshot().desired;auto settings=saved;
    settings.frame_generation=4;settings.enabled=true;settings.advanced_mode=false;
    settings.ofr_enabled=false;settings.ofr_swap=true;settings.rife_multiplier=4;
    settings.afw_mode=3;settings.luke_osr=2;
    require(!sanitize(settings).ofr_swap,"Basic clears an Advanced-only saved Swap");
    require(request(settings),"AER UI basic setup");
    for(bool compact:{false,true}) {
        Context view(compact?ImVec2(1024,768):ImVec2(690,1000),compact);
        view.draw();view.draw(true);
        require(view.rendered_text.find("AER framegen")==std::string::npos && view.rendered_text.find("Swap")==std::string::npos,"AER and Swap hidden in Basic on desktop and VR");
        settings.advanced_mode=true;settings.ofr_swap=false;require(request(settings),"AER Advanced setup");view.draw();view.draw(true);
        require(view.rendered_text.find("AER framegen")!=std::string::npos && view.rendered_text.find("Swap")!=std::string::npos,"Advanced shows both AER flags");
        view.activate("/Generation_","AER framegen");view.draw();view.draw();
        require(snapshot().desired.ofr_enabled && snapshot().desired.frame_generation==4,"AER toggle preserves selected RIFE method");
        view.activate("/Generation_","AER Swap");view.draw();view.draw();
        require(snapshot().desired.ofr_swap,"Swap toggle saved");
        const auto multiplier=snapshot().desired.rife_multiplier;
        view.choose("RIFE multiplier",1,"3X");
        require(snapshot().desired.rife_multiplier==multiplier && generation_multiplier(snapshot().desired)==2,"AER disables higher multiplier buttons: before="+std::to_string(multiplier)+" after="+std::to_string(snapshot().desired.rife_multiplier)+" aer="+std::to_string(snapshot().desired.ofr_enabled)+" fg="+std::to_string(snapshot().desired.frame_generation));
        view.select_tab("Reconstruction");view.draw(true);
        require(view.rendered_text.find("OFR")==std::string::npos && view.rendered_text.find("OFR size")==std::string::npos,"Reconstruction no longer contains separate OFR method");
        require(view.rendered_text.find("OSR/AFW off, using AER Frame Gen")!=std::string::npos,
            "desktop and VR explain why reconstruction is disabled");
        view.choose("Reconstruction",0,"Off","/Framewarp_");
        view.activate("/Framewarp_","Frame Reconstruction only");view.draw();view.draw();
        require(snapshot().desired.afw_mode==3 && snapshot().desired.luke_osr==2 &&
            snapshot().desired.afw_only==settings.afw_only,"AER locks reconstruction controls without erasing preferences: mode="+
            std::to_string(snapshot().desired.afw_mode)+" osr="+std::to_string(snapshot().desired.luke_osr)+
            " only="+std::to_string(snapshot().desired.afw_only)+" expected_only="+std::to_string(settings.afw_only));
        auto resume=snapshot().desired;resume.ofr_enabled=false;require(request(resume),"disable AER to restore reconstruction");
        view.draw();view.choose("Reconstruction",0,"Off","/Framewarp_");
        require(snapshot().desired.afw_mode==0,"reconstruction controls work again after AER is disabled");
        settings.advanced_mode=false;settings.ofr_enabled=false;settings.ofr_swap=false;require(request(settings),"reset AER UI pass");
    }
    require(request(saved),"restore AER UI fixture settings");
}

void aer_native_guide_widgets() {
    const auto saved=snapshot().desired;
    constexpr const char* cards[]{"OFXR","FSR 3.1","DLSS-G","Hybrid","RIFE"};
    constexpr int ui_indices[]{0,2,3,1,4};
    for(bool compact:{false,true})for(int method:{0,1,2,3,4}) {
        auto settings=saved;settings.advanced_mode=true;settings.ofr_enabled=true;
        settings.ofr_swap=true;settings.enabled=true;settings.afw_mode=3;
        select_generation(settings,method,2);
        require(request(settings),"AER native-guide selected method setup");
        Snapshot sample=snapshot();sample.connected=true;sample.runtime.luke_detected=true;
        sample.runtime.vdxr_legacy=false;sample.runtime.ofr_delivery=1;
        // Historical activity is not evidence of current per-eye availability.
        sample.runtime.guide_publications=99;sample.runtime.guide_matches=99;sample.runtime.guide_uses=99;
        Context view(compact?ImVec2(1024,768):ImVec2(690,1200),compact);
        view.injected_snapshot=&sample;view.draw();view.draw(true);
        require(snapshot().desired.frame_generation==method && snapshot().desired.ofr_enabled && snapshot().desired.ofr_swap,
            "AER display never coerces the selected method or phase while inputs are missing");
        if(method==1 || method==2)
            require(view.rendered_text.find("Waiting for native DLSS motion vectors and depth")!=std::string::npos,
                "FSR/DLSS-G report missing AER native inputs instead of claiming readiness from old counters");
        require(view.rendered_text.find("AFW reconstructs one eye without native guides")==std::string::npos,
            "AER bypasses saved AFW preferences, so post-AFW warnings do not apply");
        sample.runtime.upscaler_evaluations=1;sample.runtime.upscaler_motion=true;
        sample.runtime.upscaler_depth=true;sample.runtime.upscaler_last_seen_ms=GetTickCount64();
        view.draw(true);
        require(view.rendered_text.find("Native DLSS motion vectors and depth observed")!=std::string::npos &&
            view.rendered_text.find("each eye's own released frame")!=std::string::npos,
            "fresh native input observation remains distinct from exact per-eye guide validation");
        sample.runtime.upscaler_last_seen_ms=GetTickCount64()-5000;view.draw(true);
        require(view.rendered_text.find("Native DLSS motion vectors and depth observed")==std::string::npos,
            "stale DLSS evaluations do not advertise available native AER inputs");
        sample.runtime.upscaler_last_seen_ms=GetTickCount64();sample.runtime.upscaler_depth=false;view.draw(true);
        require(view.rendered_text.find("Native DLSS motion vectors and depth observed")==std::string::npos,
            "motion observation without depth does not advertise complete native AER inputs");
        // All cards remain selectable before guide capture starts, allowing the
        // chosen consumer to request inputs without a UI/capture deadlock.
        view.injected_snapshot=nullptr;view.draw();
        const int next=(method+1)%5;
        view.choose("Frame generation",ui_indices[next],cards[next]);
        require(snapshot().desired.frame_generation==next && snapshot().desired.ofr_enabled &&
            generation_multiplier(snapshot().desired)==2,"AER allows each selected method and keeps the 2X cap");
    }
    require(request(saved),"restore AER native guide UI settings");
}

int main(int argc, char** argv) {
    const auto profile = std::filesystem::temp_directory_path() /
        (L"ofxr-settings-widgets-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", profile.c_str());
    try {
        if (argc == 2 && std::strcmp(argv[1], "--aer-framegen") == 0) {aer_framegen_widgets();aer_native_guide_widgets();}
        else if (argc == 2 && std::strcmp(argv[1], "--gpu-telemetry") == 0) gpu_telemetry_and_quality();
        else if (argc == 2 && std::strcmp(argv[1], "--desktop-header") == 0) desktop_header_is_not_a_headset_control();
        else if (argc == 2 && std::strcmp(argv[1], "--route-motion") == 0) route_motion_widgets();
        else if (argc == 2 && std::strcmp(argv[1], "--route-multiplier") == 0) route_multiplier_widgets();
        else if (argc == 2 && std::strcmp(argv[1], "--aer-priority") == 0) aer_framegen_widgets();
        else { desktop_header_is_not_a_headset_control(); rebase_and_publish(); rife_catalog();rife_widgets();release_controls(); actual_widgets(); route_motion_widgets(); route_multiplier_widgets(); automatic_buttons(); offline_preset(); aer_framegen_widgets();aer_native_guide_widgets(); }
        std::filesystem::remove(configuration_path());
        std::filesystem::remove(profile/L"dlssg-profile-selection.ini");
        std::filesystem::remove(profile);
        std::cout << checks << " shared settings widget checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::filesystem::remove(configuration_path());
        std::filesystem::remove(profile/L"dlssg-profile-selection.ini");
        std::filesystem::remove(profile);
        std::cerr << "settings widgets failed: " << error.what() << '\n';
        return 1;
    }
}
