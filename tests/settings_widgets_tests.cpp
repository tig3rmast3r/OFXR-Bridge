#include "xrfg/settings_widgets.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "xrfg/dlssg_profile.hpp"
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
void require(bool value, const char* detail) {
    ++checks;
    if (!value) throw std::runtime_error(detail);
}

struct Context {
    ImGuiContext* context{};
    SettingsWidgetState widgets{};
    ImVec2 size{};
    bool compact{};
    std::string rendered_text;
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
        render_settings_widgets(widgets, snapshot(), {.compact = compact});
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
        if (context->FrameCount >= 2) {
            unsigned tab_vertices{};
            for (int i = 0; i < context->TabBars.GetMapSize(); ++i) {
                auto* bar = context->TabBars.TryGetMapData(i);
                if (!bar) continue;
                for (const auto& vertex : bar->Window->DrawList->VtxBuffer)
                    if (bar->BarRect.Contains(vertex.pos)) ++tab_vertices;
            }
            require(tab_vertices > 24, "settings tab strip is visibly rendered after layout warmup");
        }
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
    void choose(const char* label, int index, const char* item, const char* child = "/Generation_") {
        activate(child, label);
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
    void select_tab(const char* label) {
        ImGui::SetCurrentContext(context);
        for (int i = 0; i < context->TabBars.GetMapSize(); ++i) {
            auto* bar = context->TabBars.TryGetMapData(i);
            if (!bar) continue;
            for (auto& tab : bar->Tabs) {
                if (std::strcmp(ImGui::TabBarGetTabName(bar, &tab), label) == 0) {
                    ImGui::ActivateItemByID(tab.ID);
                    draw();
                    draw();
                    require(bar->SelectedTabId == tab.ID, "tab responds to ImGui navigation activation");
                    return;
                }
            }
        }
        throw std::runtime_error("expected settings tab was not rendered");
    }
};

void rebase_and_publish() {
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
    tray.choose("OFXR engine", 3, "Nvidia 0.3.0 (medium latency, medium compatibility)");
    require(snapshot().desired.backend == 2, "new display order keeps 0.3.0 on persisted backend 2");
    headset.draw();
    headset.choose("OFXR engine", 2, "Nvidia 0.2.13 (medium latency, high compatibility)");
    require(snapshot().desired.backend == 3, "new display order keeps 0.2.13 on persisted backend 3");
    tray.draw();
    tray.choose("Frame generation", 1, "Hybrid (slow, needs DLSS)");
    require(snapshot().desired.frame_generation == 3, "actual tray selects Hybrid with stable mode ID");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR (fast)");
    require(snapshot().desired.motion_vectors == 0, "Hybrid preserves independent OFXR optical flow selection");
    tray.draw();
    tray.choose("Frame generation", 2, "FSR 3.1 (slow, needs DLSS)");
    require(snapshot().desired.frame_generation == 1, "actual tray selects FSR");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR (fast)");
    require(snapshot().desired.motion_vectors == 0,
        "returning from FSR restores OFXR optical flow");
    tray.draw();
    tray.choose("Frame generation", 3, "DLSS Frame Generation (slowest, needs DLSS)");
    headset.draw();
    headset.choose("Frame generation", 0, "OFXR (fast)");
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
    headset.choose("Frame generation", 3, "DLSS Frame Generation (slowest, needs DLSS)");
    require(!snapshot().desired.triple_frame_gen,
        "DLSS-G keeps its own 2X after DJ selects 3X");
    headset.choose("Multiplier", 1, "3X");
    tray.draw(); tray.choose("Frame generation", 0, "OFXR (fast)");
    // Since V609, DLSS-G has canonical backend0. Select Djules explicitly
    // after leaving it instead of assuming DLSS-G retained a Djules presenter.
    tray.choose("OFXR engine", 2, "Nvidia 0.2.13 (medium latency, high compatibility)");
    tray.choose("Multiplier", 0, "2X");
    headset.draw(); headset.choose("Frame generation", 3, "DLSS Frame Generation (slowest, needs DLSS)");
    require(snapshot().desired.triple_frame_gen,
        "DLSS-G keeps its 3X after DJ selects 2X");
    tray.draw(); tray.choose("Frame generation", 0, "OFXR (fast)");
    tray.choose("OFXR engine", 2, "Nvidia 0.2.13 (medium latency, high compatibility)");
    require(!snapshot().desired.triple_frame_gen, "DJ keeps its own 2X");
}
void release_controls() {
    auto simple=Settings{};simple.backend=1;simple.scale=3;require(request(simple),"seed simple release UI");
    Context desktop({690,900},false);desktop.draw();desktop.draw(true);
    require(desktop.rendered_text.find("Motion source")==std::string::npos,"basic mode hides motion source");
    require(desktop.rendered_text.find("Framework+director")==std::string::npos,"basic UI has no combined mode or help");
    desktop.choose("Pacing",0,"Framework");require(snapshot().desired.pacing_mode==0,"basic Framework selection works");
    desktop.choose("Pacing",1,"Director");require(snapshot().desired.pacing_mode==1,"basic Director selection works");
    require(desktop.rendered_text.find("Reset to defaults")!=std::string::npos,"reset appears beside enable");
    require(desktop.rendered_text.find("Pacing")<desktop.rendered_text.find("Frame generationOFXR"),"pacing is first under enable");
    desktop.activate("/Generation_","Reset to defaults");desktop.draw();desktop.draw();
    require(snapshot().desired==sanitize(Settings{}),"actual Reset button restores all factory settings");
    desktop.select_tab("Display");desktop.draw(true);
    require(desktop.rendered_text.find("Experimental features")==std::string::npos &&
        desktop.rendered_text.find("Output CSV")==std::string::npos &&
        desktop.rendered_text.find("Open ofxr-bridge.ini")==std::string::npos,"basic Display removes experimental and redundant controls");
    desktop.select_tab("Luke Ross");desktop.draw(true);
    require(desktop.rendered_text.find("Frame Reconstruction only (AFW or OSR, restart game)")!=std::string::npos && desktop.rendered_text.find("Clear before framewarp")==std::string::npos,"basic Luke UI exposes per-game Frame Reconstruction only without Clear");
    desktop.activate("/Framewarp_","Frame Reconstruction only (AFW or OSR, restart game)");desktop.draw();
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
    tray.activate("/Generation_", "Fast mode (lower vram, lower latency, may reduce performance)");
    tray.draw(); tray.draw();
    require(snapshot().desired.separate_synthesis == 1 && snapshot().desired.lower_vram == 1,
        "Lower VRAM enables private textures and reduced rings together");
    headset.draw();
    require(headset.widgets.draft.lower_vram == 1, "headset reflects Lower VRAM on");
    headset.activate("/Generation_", "Fast mode (lower vram, lower latency, may reduce performance)");
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
    headset.select_tab("Luke Ross");
    // The provider controls sit behind the persisted Advanced parameters checkbox.
    headset.activate("/Framewarp_", "Advanced parameters");
    headset.draw();
    require(snapshot().desired.luke_advanced, "Luke advanced checkbox publishes hot setting");
    headset.activate("/Framewarp_", "Debug framewarp");
    headset.draw();
    require(snapshot().desired.afw_debug != enabled.desired.afw_debug,
        "real Luke debug checkbox publishes hot setting");
    headset.select_tab("Display");
    headset.choose("Headset FPS counter", 5, "Elliottate's follow controller", "/Display_");
    require(snapshot().desired.overlay_position == 5, "headset controller meter publishes existing display setting");
    tray.select_tab("Display");
    tray.choose("Headset FPS counter", 6, "Elliottate's stick to bottom", "/Display_");
    require(snapshot().desired.overlay_position == 6, "tray bottom meter preserves mode 6");
    headset.draw();
    require(headset.widgets.draft.overlay_position == 6, "both surfaces reflect new meter mode");
    headset.activate("/Display_", "Diagnostic logging");
    headset.draw();
    require(snapshot().desired.diagnostics != enabled.desired.diagnostics,
        "real diagnostics checkbox publishes setting");
    headset.activate("/Display_", "Experimental features");headset.draw();headset.draw();
    const bool previous_camera=snapshot().desired.r19_camera;
    headset.activate("/Display_", "Reduce black bars during headset rotation###R19 synthetic camera (experimental)");headset.draw();headset.draw();
    require(snapshot().desired.r19_camera!=previous_camera,"experimental camera setting remains live in Display");
    headset.select_tab("Frame generation");
    tray.select_tab("Neural Rendering");
    tray.select_tab("Luke Ross");
    tray.select_tab("Display");
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
    tray.select_tab("Luke Ross");headset.select_tab("Luke Ross");
    tray.draw(true);headset.draw(true);
    for(const auto* view:{&tray,&headset}) {
        require(view->rendered_text.find("Framewarp mode")!=std::string::npos &&
            view->rendered_text.find("Total swapchains:")==std::string::npos,
            "Luke controls remain above collapsed swapchain Details");
        require(view->rendered_text.find("Swapchain fallback")!=std::string::npos,
            "status warns even with ownership Details collapsed");
    }
    tray.activate("/Framewarp_","Details");headset.activate("/Framewarp_","Details");
    tray.draw(true);headset.draw(true);
    for(const auto* view:{&tray,&headset}) {
        require(view->rendered_text.find("Framewarp mode")<view->rendered_text.find("Total swapchains:"),
            "expanded swapchain details follow the Luke controls");
        require(view->rendered_text.find("Swapchain fallback")!=std::string::npos,
            "desktop/headset status and warning expose actual fallback");
        require(view->rendered_text.find("Total swapchains: 14")!=std::string::npos &&
            view->rendered_text.find("0x823")!=std::string::npos,
            "Luke tab exposes handle count and unknown app identity");
    }
    require(tray.rendered_text.find("OFXR: 11")!=std::string::npos &&
        tray.rendered_text.find("Game / Luke Ross: 3")!=std::string::npos &&
        tray.rendered_text.find("Counts cover")==std::string::npos,"concise owner split with no healthy boilerplate");
    require(tray.rendered_text.find("Swapchain budget fix")==std::string::npos &&
        headset.rendered_text.find("Swapchain budget fix")==std::string::npos,
        "INI-only startup sharing is absent from desktop and headset UI");
    auto quality_settings=snapshot().desired;quality_settings.afw_mode=3;
    require(request(quality_settings),"select Combined for live quality control");
    tray.draw();tray.draw();status.afw_shading_rate=0;report_runtime(status);
    tray.choose("AFW shading rate",1,"2x2 (half per axis)","/Framewarp_");
    require(snapshot().desired.afw_shading_rate==4,"live quality dropdown publishes beta6 2x2 index");
    tray.draw(true);
    require(tray.rendered_text.find("Quality change pending")!=std::string::npos &&
        tray.rendered_text.find("next launch")==std::string::npos,"AFW UI exposes active versus pending without claiming a restart");
    status.afw_shading_rate=4;report_runtime(status);tray.draw(true);
    require(tray.rendered_text.find("Quality change pending")==std::string::npos,
        "successful active quality update clears pending explanation");
    auto osr_settings=snapshot().desired;osr_settings.luke_osr=2;
    require(request(osr_settings),"select Full OSR");
    tray.draw();tray.draw(true);
    require(tray.rendered_text.find("AFW shading rate")==std::string::npos &&
        tray.rendered_text.find("Ignore motion")==std::string::npos &&
        tray.rendered_text.find("Depth dilation")==std::string::npos &&
        tray.rendered_text.find("Framewarp mode")!=std::string::npos,
        "Full OSR advanced parameters show only what it uses, Off included");
    tray.choose("Framewarp mode",1,"Other eye only (for debug)","/Framewarp_");
    require(snapshot().desired.afw_mode==1,"Full OSR Other eye only is the alternate-eye mode");
    tray.choose("Framewarp mode",2,"On","/Framewarp_");
    require(snapshot().desired.afw_mode==3,"Full OSR On is Combined");
    tray.choose("Framewarp mode",0,"Off","/Framewarp_");
    require(snapshot().desired.afw_mode==0,"Full OSR keeps Off");
    osr_settings=snapshot().desired;osr_settings.afw_mode=3;
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
        desktop.rendered_text.find("DLSS-G preset (next launch)")!=std::string::npos,
        "offline selector replaces file browser");
    desktop.activate("/Generation_","DLSS-G preset (next launch)");desktop.draw();desktop.draw();
    bool opened=false;for(auto* window:desktop.context->Windows)
        opened|=window->Active && (window->Flags&ImGuiWindowFlags_Popup) && std::strstr(window->Name,"##Combo");
    require(opened,"actual preset dropdown opens with no game connected; no driver write performed");
}
void automatic_buttons() {
    require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",configuration_path().c_str()),"enable benchmark GUI fixture in single INI");
    // Factory Auto pick is ON since V663. This click test exercises OFF->ON.
    auto initial=snapshot().desired;initial.benchmark_auto_pick=false;
    require(request(initial),"explicit unchecked state for auto-pick interaction fixture");
    RuntimeStatus runtime{};runtime.sessions=1;runtime.luke_detected=true;
    runtime.steamvr_runtime=true;runtime.vdxr_legacy=false;
    runtime.dlssg_preset_request=3;runtime.applied_revision=snapshot().revision;report_runtime(runtime);
    const auto prior=snapshot().desired;
    Context tray({690,900},false);tray.draw();tray.draw();tray.select_tab("Display");tray.draw(true);
    for(const auto label:{"Tests without vectors","Complete tests","Test Luke Ross"})
        require(tray.rendered_text.find(label)!=std::string::npos,"All automatic controls are in Display");
    require(tray.rendered_text.find("Open benchmark folder")==std::string::npos,"shared settings no longer opens shell folders");
    require(tray.rendered_text.find("25 optical or 43 complete")!=std::string::npos &&
        tray.rendered_text.find("Luke Ross: up to 30 tests")!=std::string::npos,
        "SteamVR Display describes its actual pacing and AFW matrix");
    tray.activate("/Display_","Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==25,
        "Display button starts the actual SteamVR optical matrix");
    tray.activate("/Display_","Stop tests");tray.draw();tray.draw();
    require(snapshot().benchmark.phase==BenchmarkPhase::stopped && snapshot().desired==prior,"Display Stop restores original settings");
    runtime.steamvr_runtime=false;runtime.vdxr_runtime=true;runtime.vdxr_legacy=true;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("Legacy VDXR: up to 9 optical or 15 complete")!=std::string::npos,
        "VDXR Display describes the Legacy spreadsheet suite");
    tray.activate("/Display_","Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==9,
        "Legacy Display starts Normal/Fast for every optical route");
    tray.activate("/Display_","Stop tests");tray.draw();tray.draw();
    runtime.vdxr_legacy=false;runtime.applied_revision=snapshot().revision;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("25 optical or 43 complete")!=std::string::npos,
        "opt-in VDXR exposes the full comparison in the actual Display surface");
    tray.activate("/Display_","Tests without vectors");tray.draw();tray.draw();
    require(benchmark_active(snapshot().benchmark) && snapshot().benchmark.count==25,
        "VDXR Display button starts all three pacing variants");
    tray.activate("/Display_","Stop tests");tray.draw();tray.draw();
    auto simple=snapshot().desired;simple.advanced_mode=false;require(request(simple),"return to basic UI policy");
    runtime.applied_revision=snapshot().revision;runtime.pacing_mode=1;report_runtime(runtime);tray.draw(true);
    require(tray.rendered_text.find("33 optical or 57 complete")!=std::string::npos && tray.rendered_text.find("Luke Ross: up to 39 tests")!=std::string::npos,"basic Display shows current two/three-buffer counts");
    require(tray.rendered_text.find("7.5 seconds per configuration")!=std::string::npos &&
        tray.rendered_text.find("2.5 seconds settling, then 5 seconds measured")!=std::string::npos,
        "Display describes the actual shorter benchmark timing");
    tray.activate("/Display_","Tests without vectors");tray.draw();tray.draw();
    require(snapshot().benchmark.count==33 && !snapshot().benchmark.advanced,"basic Display starts current buffer matrix");
    tray.activate("/Display_","Stop tests");tray.draw();tray.draw();
    require(tray.rendered_text.find("Auto pick best one")!=std::string::npos,"auto-pick checkbox is in Display");
    require(tray.rendered_text.find("On completion: within")==std::string::npos &&
        tray.rendered_text.find("Native vectors take priority")==std::string::npos,
        "automatic selection rules stay out of the product interface");
    tray.activate("/Display_","Auto pick best one");tray.draw();tray.draw();
    require(snapshot().desired.benchmark_auto_pick,"actual checkbox saves the per-profile automatic preference");
    tray.activate("/Display_","Tests without vectors");tray.draw();tray.draw();
    require(snapshot().benchmark.auto_pick,"UI-started suite captures the auto-pick preference");
    tray.activate("/Display_","Stop tests");tray.draw();tray.draw();
    require(snapshot().desired.benchmark_auto_pick && snapshot().benchmark.phase==BenchmarkPhase::stopped,"Stop restores settings without clearing the auto-pick preference");
}
} // namespace

int main(int argc, char** argv) {
    const auto profile = std::filesystem::temp_directory_path() /
        (L"ofxr-settings-widgets-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", profile.c_str());
    try {
        if (argc == 2 && std::strcmp(argv[1], "--route-motion") == 0) route_motion_widgets();
        else if (argc == 2 && std::strcmp(argv[1], "--route-multiplier") == 0) route_multiplier_widgets();
        else { rebase_and_publish(); release_controls(); actual_widgets(); route_motion_widgets(); route_multiplier_widgets(); automatic_buttons(); offline_preset(); }
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
