#include <optional>
#include "xrfg/presented_output_rate.hpp"
#include "xrfg/settings_widgets.hpp"
#include "xrfg/rife_resolution.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "imgui.h"
#include "xrfg/rife_model_catalog.hpp"
#include "xrfg/rife_runtime_locator.hpp"
#include <windows.h>

#include <algorithm>
#include <initializer_list>
#include <iterator>
#include <cmath>
#include <cstdio>

namespace xrfg::autonomous {
void apply_settings_style() {
    ImGui::StyleColorsDark();
    auto& s = ImGui::GetStyle();
    // Upstream status-panel sRGB palette, adapted to ImGui controls.
    s.Colors[ImGuiCol_WindowBg] = ImVec4(14/255.F,18/255.F,28/255.F,232/255.F);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(0,0,0,0);
    s.Colors[ImGuiCol_Text] = ImVec4(238/255.F,241/255.F,247/255.F,1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(148/255.F,158/255.F,178/255.F,1);
    s.Colors[ImGuiCol_Border] = ImVec4(64/255.F,74/255.F,98/255.F,1);
    s.Colors[ImGuiCol_Separator] = ImVec4(48/255.F,56/255.F,76/255.F,1);
    s.WindowRounding = 0; s.FrameRounding = 4;
}
namespace {

void status(const Snapshot& state) {
    const auto& m = state.runtime.telemetry;
    const auto now = GetTickCount64();
    const bool current = state.connected && m.updated_ms && now >= m.updated_ms && now - m.updated_ms < 2000;
    const float header_x=ImGui::GetCursorPosX(),header_width=ImGui::GetContentRegionAvail().x;
    ImGui::TextDisabled("OFXR-BRIDGE 0.3.2 (v%d)",XRFG_IMPLEMENTATION_VERSION);
    const auto vram=interface_vram_text(state);
    const auto gpu=interface_gpu_text(state,now);
    const auto vram_width=ImGui::CalcTextSize(vram.c_str()).x;
    const auto telemetry_width=vram_width+ImGui::GetStyle().ItemSpacing.x+ImGui::CalcTextSize(gpu.c_str()).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),header_x+(header_width-telemetry_width)*.5F));
    ImGui::TextUnformatted(vram.c_str());
    if(ImGui::IsItemHovered()){
        const auto& m=state.runtime.video_memory;
        ImGui::SetTooltip("Dedicated VRAM / DXGI budget. Game: %.0f MiB; RIFE worker: %.0f MiB%s.\nPyTorch reserved (included in worker): %.0f MiB. Shared imports counted once.",
            m.game_bytes/1048576.0,m.rife_private_bytes/1048576.0,
            m.rife_active&&!m.rife_valid?" (sample pending)":"",m.rife_reserved_bytes/1048576.0);
    }
    ImGui::SameLine();ImGui::TextUnformatted(gpu.c_str());
    if(ImGui::IsItemHovered())ImGui::SetTooltip("GPU utilization on the rendering adapter, across all processes (including RIFE and compositor).\nBusiest physical engine, sampled in the background about once per second.");
    const auto value = [](const char* label, float number, bool valid, bool rate,
                          const char* tip, bool late = false) {
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", label);
        char text[64] = "N/A";
        if (valid && std::isfinite(number)) {
            if (rate) std::snprintf(text, sizeof(text), "%.0f", number);
            else if (late && number < 0) std::snprintf(text, sizeof(text), "Late by %.1f ms", -number);
            else std::snprintf(text, sizeof(text), "%.1f ms", number);
        }
        ImGui::PushFont(ImGui::GetFont(), rate ? 28.F : 21.F);
        ImGui::TextWrapped("%s", text);
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
    };
    if (ImGui::BeginTable("Interface metrics", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        value(state.runtime.luke_detected?"New submitted FPS":"Output FPS", m.output_fps, current && output_rate_valid(m), true,
            state.runtime.luke_detected ? "Distinct new images successfully accepted by the OpenXR runtime per second; held images and repeats excluded. This measures delivery to the runtime, not physical headset scanout. N/A only when source identity is unavailable." : "SteamVR uses compositor presents. VDXR and Pimax OpenXR use successful content submissions to the native runtime. Empty or failed submissions are excluded. Native OpenXR acceptance does not measure physical headset scanout.");
        value("Real submitted", m.game_fps, current && m.rates_valid, true, "Successfully submitted real frames. Luke OFXR counter preserved; AFW-only held source identities are excluded.");
        value("Gen submitted", m.generated_fps, current && m.rates_valid, true, "Successfully submitted generated frames with native GPU confirmation.");
        value("Display Hz", m.display_hz, current && m.display_hz > 0, true,
            m.display_source == InterfaceSource::steamvr_display ? "SteamVR physical display frequency." : "OpenXR shortest observed predicted display period; may reflect runtime throttling.");
        ImGui::EndTable();
        if(ImGui::BeginTable("Interface timings",3,ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            value("OFXR time",m.ofxr_time_ms,current && m.ofxr_time_valid,false,
                "CPU wall time inside the intercepted application OpenXR calls, including their waits. Overlapping calls are counted once. Not GPU synthesis time.");
            value("Cycle length",m.cycle_length_ms,current && m.cycle_valid,false,
                "Application WaitFrame entry through completion of the same real frame at the native runtime or a later producer return. Includes the private delivery path; not engine-start or panel latency.");
            value("Cycle minimum",m.cycle_estimated_ms,current && m.cycle_estimated_valid,false,
                "WaitFrame entry through observed real runtime End completion, bounded below by one physical refresh per confirmed generated output before that real. Already-spaced native delivery is not charged twice. Assumes those outputs are shown distinctly; not a display confirmation. No runtime prediction horizon or producer tail.");
            ImGui::EndTable();
        }
        const auto text = interface_state_text(state);
        const auto color = interface_generation_active(state) || interface_afw_only_active(state,now) ? ImVec4(.36F,.88F,.56F,1) :
            text == "Fallback" || text == "Applying settings" ? ImVec4(1,.75F,.28F,1) : ImVec4(.75F,.8F,.88F,1);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::Text("Vectors: %s", interface_vectors_text(state, now).data());
    const auto reconstruction = interface_reconstruction_text(state,now);
    ImGui::TextColored(ImVec4(.36F,.88F,.56F,1),"%.*s",static_cast<int>(reconstruction.size()),reconstruction.data());
    const auto explanation = interface_status_text(state,false);
    ImGui::TextWrapped("Status: %s", explanation.c_str());
    ImGui::Separator();
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}
void dlssg_preset(SettingsWidgetState& ui, const Snapshot& state, bool compact) {
    using namespace xrfg::standalone;
    const auto selection_file = configuration_path();
    if (!state.connected) ui.dlssg_observed_pid=0;
    const auto resolve_target = [&] {
        auto path = compact ? process_executable() :
            state.connected ? game_executable(state.runtime.process_id) : std::filesystem::path{};
        if (!compact && path.empty()) path = last_dlssg_game(selection_file);
        if (path == ui.dlssg_executable) return false;
        ui.dlssg_executable = path;
        ui.dlssg_target_saved = false;
        return true;
    };
    const auto refresh = [&] {
        DlssgProfile profile; std::wstring error;
        const bool read = !ui.dlssg_executable.empty() && read_dlssg_profile(ui.dlssg_executable, profile, error);
        ui.dlssg_model = read && profile.enabled && profile.custom_override ?
            (profile.preset == 3 ? 2 : profile.preset == 1 ? 1 : 0) : 0;
        ui.dlssg_profile_status = read ? "Game profile read. Changes apply on the next game launch." :
            ui.dlssg_executable.empty() ? "Start a game with OFXR once. Its preset can then be changed here while the game is closed." : utf8(error);
        ui.dlssg_profile_loaded = true;
    };
    if (!ui.dlssg_profile_loaded) {
        resolve_target();
        refresh();
        ui.dlssg_target_refresh_at = ImGui::GetTime()+1.;
    } else if (!compact && ImGui::GetTime() >= ui.dlssg_target_refresh_at) {
        // The tray can learn a game while this window is already open, even
        // if it exits before the DLSS-G controls are visited again. Re-read
        // the local target once per UI second; query NVAPI only if it changed.
        ui.dlssg_target_refresh_at = ImGui::GetTime()+1.;
        if (resolve_target()) refresh();
    }
    if ((compact && !ui.dlssg_target_saved) ||
        (!compact && state.connected && ui.dlssg_observed_pid != state.runtime.process_id)) {
        const auto path = compact ? process_executable() : game_executable(state.runtime.process_id);
        if (!path.empty()) {
            ui.dlssg_observed_pid = state.runtime.process_id;
            if (path != ui.dlssg_executable) { ui.dlssg_executable = path; ui.dlssg_target_saved = false; refresh(); }
            if (!ui.dlssg_target_saved) ui.dlssg_target_saved = remember_dlssg_game(selection_file, path);
        }
    }
    if (!ui.dlssg_executable.empty()) ImGui::TextWrapped("Game: %s", utf8(ui.dlssg_executable.filename().wstring()).c_str());
    ImGui::BeginDisabled(ui.dlssg_executable.empty());
    int selected = ui.dlssg_model;
    if (ImGui::Combo("DLSS-G preset (next launch)", &selected, "Game default\0Preset A (Full, quality)\0Preset C (Lite, faster)\0")) {
        const auto preset = selected == 1 ? DlssgPreset::a : selected == 2 ? DlssgPreset::c : DlssgPreset::game_default;
        std::wstring error;
        DlssgPreset saved{};
        if (write_dlssg_profile(ui.dlssg_executable, preset, error, &saved)) {
            const bool remembered = remember_dlssg_preset(selection_file,saved);
            refresh(); ui.dlssg_profile_status = saved != preset
                ? "Preset C unavailable: Preset A saved and verified for the next launch."
                : "Saved and verified for the next launch. Restart the game if it is running.";
            if (!remembered) ui.dlssg_profile_status += " Could not remember the preset preference in ofxr-bridge.ini.";
        } else ui.dlssg_profile_status = utf8(error);
    }
    ImGui::EndDisabled();
    if (ImGui::Button("Refresh preset profile")) {
        resolve_target();
        refresh();
        ui.dlssg_target_refresh_at = ImGui::GetTime()+1.;
    }
    ImGui::TextWrapped("%s", ui.dlssg_profile_status.c_str());
    ImGui::TextWrapped("C uses Lite blending; A uses Full quality. If the driver rejects C, OFXR tries A. A saved request does not verify the model loaded by NVIDIA; global overrides may still apply.");
}
bool generation(SettingsWidgetState& ui, const Snapshot& state, bool compact) {
    auto& draft=ui.draft;
    bool changed=ImGui::Checkbox("Enable frame generation",&draft.enabled);
    ImGui::SameLine();
    if(ImGui::Button("Reset to defaults")) {
        if(reset_to_defaults()) {ui.dirty=false;ui.scale_editing=false;ui.dlssg_profile_loaded=false;synchronize_settings_widgets(ui,snapshot());changed=false;}
        else ui.save_failed=true;
    }
    const bool framework_only=state.connected && !pacing_comparison_runtime(state.runtime);
    int pacing=framework_only?0:draft.pacing_mode;
    ImGui::BeginDisabled(framework_only || (state.connected && state.runtime.pacing_mode==kAfwOnlyPacingMode));
    if(ImGui::Combo("Pacing",&pacing,draft.advanced_mode?"Framework\0Director\0Framework+director\0":"Framework\0Director\0")) {draft.pacing_mode=pacing;changed=true;}
    ImGui::EndDisabled();
    ImGui::TextWrapped(framework_only ? "VDXR transport uses Framework pacing. Change Pacing in the tray and restart the game to use director pacing."
        : draft.advanced_mode ? "Framework bypasses director pacing; Director uses only the final director; Framework+director combines both. Changes apply live after queued frames complete."
        : "Framework bypasses director pacing; Director uses only the final director. Changes apply live after queued frames complete.");
    ImGui::BeginDisabled(framework_only || (state.connected && state.runtime.pacing_mode==kAfwOnlyPacingMode));
    if(ImGui::Checkbox("Increase buffer",&draft.increase_buffer))changed=true;
    ImGui::EndDisabled();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))ImGui::SetTooltip("SteamVR: enables an adaptive timing reserve near full refresh. Framework uses 3 deliveries instead of 2; Director uses 4 instead of its Normal 3, retaining one extra delivery while the reserve grows. Independent of Fast mode. Other private runtimes change capacity only; Legacy and direct Frame Reconstruction only are unaffected. Changes apply after queued frames complete.");
    if(state.connected && state.runtime.buffer_capacity)
        ImGui::Text("Active bridge buffer: %u deliveries",state.runtime.buffer_capacity);
    if(state.connected && state.runtime.pacing_mode==kAfwOnlyPacingMode) {
        ImGui::TextWrapped("Frame Reconstruction only: direct OpenXR delivery, no frame generation. Disable it in the Frame Reconstruction tab and restart the game to restore OFXR frame generation. Neural Rendering remains available.");
        return changed;
    }
    int selected_generation = generation_ui_index(draft.frame_generation);
    if (ImGui::Combo("Frame generation", &selected_generation,
            "OFXR (fast)\0RIFE (experimental, NVIDIA)\0Hybrid (slow, needs DLSS)\0FSR 3.1 (slow, needs DLSS)\0DLSS Frame Generation (slowest, needs DLSS)\0")) {
        select_generation(draft, generation_from_ui(selected_generation), draft.backend);
        changed = true;
    }
    if (draft.frame_generation == 0) {
        // UI order differs from persisted backend IDs: 0.2.13 is backend 3,
        // while 0.3.0 is backend 2. Keep each existing profile on its engine.
        constexpr int engine_order[] = {0, 1, 3, 2};
        int selected_engine = engine_order[std::clamp(draft.backend, 0, 3)];
        if (ImGui::Combo("OFXR engine", &selected_engine,
                "FidelityFX (low latency, medium compatibility)\0Nvidia 0.2.1 (low latency, low compatibility)\0Nvidia 0.2.13 (medium latency, high compatibility)\0Nvidia 0.3.0 (medium latency, medium compatibility)\0")) {
            select_generation(draft, 0, engine_order[selected_engine]);
            changed = true;
        }
        if(draft.advanced_mode)changed |= ImGui::Combo("Motion source", &draft.motion_vectors,
            "Optical flow\0Game motion vectors\0");
        if (draft.backend != 0 && (draft.motion_vectors == 0 ||
                (state.runtime.luke_detected && draft.afw_mode != 0))) {
            changed |= ImGui::Combo("Flow quality", &draft.preset, "Slow\0Medium\0Fast\0");
            changed |= ImGui::Combo("Flow resolution", &draft.scale, "100%\0" "75%\0" "50%\0" "25% (lower flow detail)\0");
            ImGui::TextWrapped("Flow resolution changes motion estimation only; output stays at full resolution. At high eye resolutions, 25% reduces OFA work with a detail tradeoff.");
            changed |= ImGui::Checkbox("Bidirectional flow", &draft.backward);
        }
    } else if (draft.frame_generation==4) {
        if(draft.advanced_mode) {
            if(ImGui::BeginCombo("RIFE model",xrfg::rife_model_name(draft.rife_model).c_str())) {
                // The only automatic location is managed AppData. Honor a
                // deliberately configured development override, never scan.
                wchar_t override_path[32768]{};
                GetPrivateProfileStringW(L"tray",L"rife_runtime_dir",L"",override_path,32768,
                    global_configuration_path().c_str());
                const auto root=*override_path ? std::filesystem::path(override_path) : xrfg::managed_rife_runtime();
                const auto models=xrfg::available_rife_models(root);
                int model_index=0;
                for(const auto& model:models) {
                    ImGui::PushID(model_index++);
                    if(ImGui::Selectable(model.name.c_str(),draft.rife_model==model.id)) {
                        draft.rife_model=model.id;changed=true;
                    }
                    if(draft.rife_model==model.id)ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                if(models.empty())ImGui::TextDisabled("No supported model weights installed");
                ImGui::EndCombo();
            }
        } else if(draft.rife_model!=3) {draft.rife_model=3;changed=true;}
        int multiplier=draft.rife_multiplier-2;
        if(ImGui::Combo("RIFE multiplier",&multiplier,"2X\0" "3X\0" "4X\0")){draft.rife_multiplier=multiplier+2;changed=true;}
        int resolution=xrfg::rife_resolution_index(draft.rife_resolution);
        if(ImGui::Combo("Quality",&resolution,
                "Potato\0Low\0Medium\0High\0Very high\0Ultra\0")) {
            draft.rife_resolution=xrfg::kRifeResolutions[resolution];changed=true;
        }
        ImGui::TextWrapped("3X/4X require the private route. Luke Ross also requires the isolated post-AFW synthesis queue. Additional intermediates increase GPU work and memory use.");
        ImGui::TextWrapped("Experimental portable Python/CUDA. RIFE 4.4 processes both eyes together, with automatic TensorRT optimization. Motion inference uses the selected quality preset; output stays at full resolution. Requires the RIFE runtime installed in AppData.");
    } else if (draft.frame_generation==3) {
        ImGui::TextWrapped("Game motion vectors + FidelityFX optical flow (25% per axis). If game guides are unavailable or rejected, uses FidelityFX optical flow.");
    } else {
        ImGui::TextDisabled("Uses game motion vectors and depth");
    }
    if (state.runtime.luke_detected && draft.afw_mode != 0 &&
        (draft.motion_vectors != 0 || (draft.frame_generation > 0 && draft.frame_generation < 4)))
        ImGui::TextWrapped("AFW reconstructs one eye without native guides for that eye. Post-AFW OFXR/Hybrid uses optical flow; FSR/DLSS-G requires a complete stereo guide pair.");
    const bool djules = draft.frame_generation == 0 && draft.backend == 3;
    const bool dlssg = draft.frame_generation == 2;
    if (djules || dlssg) {
        int multiplier = draft.triple_frame_gen ? 1 : 0;
        ImGui::BeginDisabled(draft.lower_vram && !draft.separate_synthesis);
        if (ImGui::Combo("Multiplier", &multiplier, "2X\0" "3X\0")) {
            draft.triple_frame_gen = multiplier == 1;
            changed = true;
        }
        ImGui::EndDisabled();
    }
    if (dlssg) {
        dlssg_preset(ui, state, compact);
        changed |= ImGui::SliderInt("Generated resolution", &draft.native_scale, 25, 100, "%d%%");
        ui.scale_editing |= ImGui::IsItemActive();
        ImGui::TextWrapped("Requires compatible NVIDIA DLSS-G hardware and runtime");
    }
    bool lower_vram = draft.lower_vram != 0;
    ImGui::BeginDisabled(state.runtime.luke_detected);
    if(ImGui::Checkbox("Fast mode (lower vram, lower latency, may reduce performance)", &lower_vram)) {
        draft.lower_vram = draft.separate_synthesis = lower_vram ? 1 : 0;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(state.runtime.luke_detected ? "Fast mode has no effect with Luke Ross."
        : "Uses private synthesis textures and fewer XR swapchains.");
    if(pacing_comparison_runtime(state.runtime) && draft.triple_frame_gen && !state.runtime.luke_detected)
        ImGui::TextWrapped("Fast mode preserves the output capacity needed for 3X; VRAM reduction may be limited.");
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("Details")) return changed;
    if (state.runtime.shared_queue)
        ImGui::TextWrapped("Shared queue: AFW/capture and OFXR share a GPU queue. This may reduce performance.");
    const auto& telemetry = state.runtime.telemetry;
    if (state.connected && telemetry.updated_ms && GetTickCount64()-telemetry.updated_ms<2000 && telemetry.cpu_valid)
        ImGui::Text("OpenXR Begin-to-End interval: %.1f ms", telemetry.cpu_render_ms);
    else ImGui::TextDisabled("OpenXR Begin-to-End interval: N/A");
    ImGui::TextDisabled("Requested %llu / applied %llu",
        static_cast<unsigned long long>(state.revision),
        static_cast<unsigned long long>(state.runtime.applied_revision));
    if (!state.connected) {
        ImGui::TextWrapped("No active game report; upscaler inputs are not yet known.");
    } else {
        const auto& runtime=state.runtime;
        if (!runtime.upscaler_evaluations)
            ImGui::TextWrapped("Upscaler inputs: no native DLSS SR/RR evaluation observed.");
        else {
            const auto age=GetTickCount64()-runtime.upscaler_last_seen_ms;
            ImGui::TextWrapped("Native DLSS SR/RR: %s. Motion vectors: %s. Depth: %s.",
                age<5000 ? "receiving inputs" : "last seen more than 5 seconds ago",
                runtime.upscaler_motion ? "present" : "missing",
                runtime.upscaler_depth ? "present" : "missing");
        }
        ImGui::TextWrapped("Owned guides: %llu published / %llu XR matches / %llu uses",
            static_cast<unsigned long long>(runtime.guide_publications),
            static_cast<unsigned long long>(runtime.guide_matches),
            static_cast<unsigned long long>(runtime.guide_uses));
        ImGui::TextWrapped("Input detection does not prove a compatible stereo pair. Native ingress currently supports DLSS SR/RR; FSR/XeSS upscaler ingress is unavailable.");
        if (ui.dirty || runtime.applied_revision!=state.revision)
            ImGui::TextWrapped("Applying the requested generation settings...");
        else {
            const auto message=generation_status_text(runtime.generation_mode,runtime.motion_source,runtime.generation_state);
            ImGui::TextWrapped("%.*s",static_cast<int>(message.size()),message.data());
        }
    }
    return changed;
}

// Setup notes for one source of eyes, open while that source is attached.
void reconstruction_setup(const char* title,bool attached,std::initializer_list<const char*> lines) {
    if(attached)ImGui::SetNextItemOpen(true,ImGuiCond_Appearing);
    if(!ImGui::CollapsingHeader(title))return;
    for(const char* line:lines)ImGui::TextWrapped("%s",line);
}

bool framewarp(Settings& draft, const Snapshot& state) {
    // Which source of eyes is attached and what reconstruction does with it,
    // as the headset meter reports them.
    const auto& r=state.runtime;
    const auto now=GetTickCount64();
    const bool fresh=r.heartbeat_ms && now>=r.heartbeat_ms && now-r.heartbeat_ms<2000;
    const bool osr=draft.luke_osr==2;
    const char* source=!state.connected ? "no active game" : r.luke_detected ? "Luke Ross detected" :
        r.uevr_osr ? "UEVR Alternating detected" : "none detected";
    const bool active=state.connected && draft.afw_mode!=0 && fresh && r.afw_running && !r.afw_flat_scene;
    const char* activity=draft.afw_mode==0 ? "Off" : active ? "Active" :
        state.connected && fresh && r.afw_flat_scene ? "2D scene (no stereo source)" :
        reconstruction_source(r) ? "Waiting" : "N/A";
    ImGui::Text("Source: %s",source);
    ImGui::SameLine();
    if(active)ImGui::TextColored(ImVec4(.4F,.9F,.6F,1),"%s: %s",osr?"OSR":"AFW",activity);
    else ImGui::TextDisabled("%s: %s",osr?"OSR":"AFW",activity);
    ImGui::Separator();
    bool changed=false;
    int reconstruction = draft.afw_mode==0 ? 0 : osr ? 1 : 2;
    if (ImGui::Combo("Reconstruction", &reconstruction, "Off\0OSR\0AFW (Luke Ross only)\0")) {
        if(reconstruction==0)draft.afw_mode=0;
        else {
            draft.luke_osr=reconstruction==1 ? 2 : 0;
            if(draft.afw_mode==0)draft.afw_mode=3;
        }
        changed = true;
    }
    if(draft.afw_mode==0)
        ImGui::TextWrapped("Reconstruction is off for every game, Luke Ross and UEVR alike. Frame generation and Neural Rendering remain independent.");
    else {
        ImGui::TextWrapped(draft.luke_osr==2
            ? "OSR (OFXR Stereo Reconstruction): OFXR renders the missing eye itself from the real eye's colour and depth, the camera of both eyes, and that eye's own previous frame for what the real eye cannot see. Used with Luke Ross and UEVR. Applies live."
            : "AFW (legacy): reconstructs the missing eye with the AFW provider. Luke Ross only. Applies live.");
        if(draft.luke_osr!=2 && r.uevr_osr)
            ImGui::TextWrapped("UEVR is reconstructed by OSR only: select OSR to reconstruct its missing eye.");
    }
    changed|=ImGui::Checkbox("Frame Reconstruction only (AFW or OSR, restart game)",&draft.afw_only);
    ImGui::TextWrapped("Removes OFXR frame generation from the pipeline: each application frame goes straight to the headset, and only the missing eye is reconstructed. The best choice for latency.");
    if(state.connected && (state.runtime.pacing_mode==kAfwOnlyPacingMode)!=draft.afw_only)
        ImGui::TextWrapped("Frame Reconstruction only change saved. Restart the game to apply it.");
    reconstruction_setup("Setup: Luke Ross",r.luke_detected,{
        "In the mod menu, set the render mode to Legacy AER.",
        "Turn off Camera rotation compensation where offered. With it on, turns in vehicles happen in steps."});
    reconstruction_setup("Setup: UEVR (Alternating / AFR)",r.uevr_osr,{
        "In UEVR, set the Rendering Method to Alternating/AFR and Synchronization to Very Late. The game must run on D3D12 with DLSS: OSR takes each eye's depth and motion vectors from DLSS. FSR and XeSS games are not supported yet.",
        "Ghosting Fix on (required): it gives each eye its own history. Without it the reconstructed eye ghosts everywhere.",
        "If UEVR's Ghosting status stays learning or reads failed closed, turn on Bootstrap Separate View States and restart the game; do not toggle it live. Some games need it, such as Hogwarts Legacy and Atomic Heart. Only some UEVR builds offer this option."});
    if(draft.afw_mode==0)return changed;
    if(draft.luke_osr==2){
        int quality=draft.luke_osr_lower_quality?1:0;
        if(ImGui::Combo("OSR quality",&quality,"OSR (normal)\0OSR (lower quality, faster)\0")){
            draft.luke_osr_lower_quality=quality!=0;changed=true;
        }
        ImGui::TextWrapped("Lower quality reduces reconstruction work and may soften object edges. Colour stays full resolution. Applies to Luke Ross and UEVR, live.");
    }
    changed |= ImGui::Checkbox("Advanced parameters", &draft.luke_advanced);
    if (draft.luke_advanced) {
        // Full OSR shows only the parameters it uses.
        const bool full_osr = draft.luke_osr==2;
        if (full_osr)
            ImGui::TextWrapped("Framewarp mode: Other eye only leaves out this eye's previous frame, for debugging. Debug framewarp tints previous-frame pixels green and inpainted pixels magenta.");
        if (full_osr) {
            int selected=draft.afw_mode==1 ? 0 : 1;
            if (ImGui::Combo("Framewarp mode", &selected, "Other eye only (for debug)\0On\0")) {
                draft.afw_mode=selected==0 ? 1 : 3;changed=true;
            }
        } else {
            int selected=std::clamp(draft.afw_mode-1,0,2);
            if(ImGui::Combo("Framewarp mode", &selected,
                "Alternate eye (for debug)\0Previous frame (for debug)\0Combined\0")) {
                draft.afw_mode=selected+1;changed=true;
            }
        }
        if (draft.afw_mode == 3 && !full_osr) {
            constexpr int rates[]{0,4};
            int selected=draft.afw_shading_rate==4 ? 1 : 0;
            if(ImGui::Combo("AFW shading rate",&selected,
                "1x1 (full quality)\0" "2x2 (half per axis)\0")) {
                draft.afw_shading_rate=rates[selected];changed=true;
            }
            ImGui::TextWrapped("Beta6: reduces Combined warping work; output resolution stays unchanged. A quality change rebuilds AFW history while the game stays open.");
            if(state.connected && state.runtime.afw_shading_rate>=0 &&
                state.runtime.afw_shading_rate!=draft.afw_shading_rate)
                ImGui::TextWrapped("Quality change pending the next successful AFW evaluation. Current index: %d",state.runtime.afw_shading_rate);
        }
        changed |= ImGui::Checkbox("Debug framewarp", &draft.afw_debug);
        ImGui::TextWrapped("Requires compatible source colour, depth and camera data. Luke Ross is read from RealVR.ini, UEVR from its own settings; other compatible mods are reported as Other.");
        if (!full_osr) {
            if(draft.advanced_mode)changed |= ImGui::Checkbox("Clear before framewarp", &draft.afw_clear_before_warping);
            changed |= ImGui::SliderFloat("Ignore motion", &draft.afw_ignore_motion_threshold, 0, 16, "%.2f");
            ImGui::TextWrapped("Threshold for ignoring per-object motion vectors.");
        }
        if (!full_osr) {
            changed |= ImGui::Combo("Depth dilation", &draft.afw_depth_dilate_mode,
                "Off\0Horizontal\0Square\0");
            ImGui::TextWrapped("Reduces ghosting and flicker, with less precise object silhouettes. Off by default.");
        }
    }
    if (state.connected) ImGui::TextDisabled("Ready frames: %llu / projections: %llu",
        static_cast<unsigned long long>(state.runtime.afw_ready_frames),
        static_cast<unsigned long long>(state.runtime.afw_projection_frames));
    return changed;
}

bool neural(SettingsWidgetState& ui, const Snapshot& state) {
    auto& draft = ui.draft;
    bool changed = ImGui::Checkbox("Enable Neural Rendering", &draft.nr_enabled);
    changed |= ImGui::Checkbox("2x NR", &draft.nr_second_pass);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Runs Neural Rendering twice with the same settings and working resolution. Uses additional GPU time and VRAM.");
    changed |= ImGui::Checkbox("Run before super resolution", &draft.nr_before_sr);
    changed |= ImGui::SliderFloat("Detail strength", &draft.nr_transfer_strength, 0, 2, "%.2f");
    changed |= ImGui::SliderFloat("Colour strength", &draft.nr_colour_strength, 0, 1, "%.2f");
    changed |= ImGui::SliderFloat("Intensity", &draft.nr_intensity, 0, 2, "%.2f");
    changed |= ImGui::Combo("Model preset", &draft.nr_preset, "Default\0Preset 1\0Preset 2\0Preset 3\0");
    changed |= ImGui::Combo("Style", &draft.nr_style, "Standard\0Natural\0Cinematic\0");
    int scale = static_cast<int>(draft.nr_working_scale * 100.F + .5F);
    if (ImGui::SliderInt("Model resolution", &scale, 25, 100, "%d%%")) {
        draft.nr_working_scale = scale / 100.F;
        changed = true;
    }
    ui.scale_editing |= ImGui::IsItemActive();
    if (ImGui::CollapsingHeader("Model options")) {
        changed |= ImGui::SliderFloat("Local structure", &draft.nr_local_structure, 0, 2, "%.2f");
        changed |= ImGui::SliderFloat("Local tone", &draft.nr_local_tone, 0, 2, "%.2f");
        changed |= ImGui::SliderFloat("Skin structure", &draft.nr_skin_structure, -1, 2, "%.2f");
        changed |= ImGui::Checkbox("Auto skin mask", &draft.nr_auto_mask);
        changed |= ImGui::SliderFloat("Paper white", &draft.nr_white_point_scale, .25F, 4, "%.2fx");
        changed |= ImGui::SliderFloat("Luminance limit", &draft.nr_max_ratio, 1, 8, "%.2fx");
    }
    if (ImGui::CollapsingHeader("Compare and inspect")) {
        changed |= ImGui::Combo("Compare", &draft.nr_compare, "Off\0Side by side\0Wipe\0");
        if (draft.nr_compare != 0) {
            changed |= ImGui::Checkbox("Swap sides", &draft.nr_compare_swap);
            if (draft.nr_compare == 1)
                changed |= ImGui::SliderFloat("Compare zoom", &draft.nr_compare_zoom, 1, 2, "%.2fx");
            else changed |= ImGui::SliderFloat("Wipe position", &draft.nr_compare_split, 0, 1, "%.2f");
        }
        changed |= ImGui::Combo("Inspect", &draft.nr_debug_view,
            "Off\0Model input\0Raw model output\0Difference\0");
    }
    if (state.connected && state.runtime.vulkan_capture_suspended)
        ImGui::TextWrapped("Vulkan Neural Rendering input capture is suspended.");
    if (draft.nr_enabled && !state.runtime.nr_running)
        ImGui::TextWrapped("Waiting for compatible DLSS frames and the Neural Rendering runtime");
    else if (state.runtime.nr_running)
        ImGui::TextColored(ImVec4(.4F, .9F, .6F, 1), "Neural Rendering active");
    return changed;
}

bool display(SettingsWidgetState& ui, const Snapshot& state) {
    auto& draft=ui.draft;
    bool changed = ImGui::Checkbox("On-screen FPS counter", &draft.overlay_enabled);
    int counter_type=draft.overlay_position-1;
    if(ImGui::Combo("Counter type", &counter_type,
        "Upper left\0Upper right\0Lower left\0Lower right\0Elliottate's follow controller\0Elliottate's stick to bottom\0")) {
        draft.overlay_position=counter_type+1;changed=true;
    }
    changed |= ImGui::Checkbox("Export status.json", &draft.status_json);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps FPS and latency readings active when all panels are closed.");
    changed |= ImGui::Checkbox("Diagnostic logging", &draft.diagnostics);
    ImGui::TextWrapped("Logging can be switched during the current game session");
    if (state.connected) ImGui::TextDisabled(state.runtime.logging_active
        ? (draft.diagnostics ? "Logging active" : "Stopping logging...")
        : (draft.diagnostics ? "Starting logging..." : "Logging disabled"));
    ImGui::SeparatorText("Automatic tests");
    const bool permitted=benchmark_enabled();
    if (!permitted) ImGui::TextWrapped("Benchmarks disabled. Enable benchmarks in the tray to enable sampling and these buttons.");
    const bool active=benchmark_active(state.benchmark);
    ImGui::TextWrapped("7.5 seconds per configuration: 2.5 seconds settling, then 5 seconds measured. CSV: min / median / max Output FPS and Cycle minimum. Works with diagnostic logging off. Neural and AFW choices are kept.");
    ImGui::BeginDisabled(active);
    changed |= ImGui::Checkbox("Auto pick best one", &draft.benchmark_auto_pick);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(active || !permitted || !state.connected);
    const bool optical=ImGui::Button("Tests without vectors");
    ImGui::SameLine();
    const bool complete=ImGui::Button("Complete tests");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(active || !permitted || !state.connected || !state.runtime.luke_detected);
    const bool luke=ImGui::Button("Test Luke Ross");
    ImGui::EndDisabled();
    if(optical || complete || luke) {
        ui.dirty|=changed;finish_settings_widget_edits(ui);
        if(!ui.dirty) static_cast<void>(start_benchmark(complete,luke));
        synchronize_settings_widgets(ui,snapshot());changed=false;
    }
    if(active) {
        if(ImGui::Button("Stop tests"))stop_benchmark();
        const auto& b=state.benchmark;
        const auto remaining=b.started_ms && GetTickCount64()>=b.started_ms
            ? std::max(0.0,double(kBenchmarkDurationMs)/1000.0-double(GetTickCount64()-b.started_ms)/1000.0)
            : double(kBenchmarkDurationMs)/1000.0;
        ImGui::TextWrapped("%u / %u: %s (%s, %.1f s)",b.index+1,b.count,b.label,
            b.phase==BenchmarkPhase::applying ? "applying" : "running",remaining);
    }
    if(state.benchmark.message[0])ImGui::TextWrapped("%s",state.benchmark.message);
    if(state.benchmark.csv_path[0])ImGui::TextWrapped("CSV: %s",state.benchmark.csv_path);
    const bool modern=pacing_comparison_runtime(state.runtime);
    ImGui::TextWrapped("Up to %zu optical or %zu complete tests, including one OFXR Off reference.",
        benchmark_plan(false,false,modern,draft.advanced_mode).size(),
        benchmark_plan(true,false,modern,draft.advanced_mode).size());
    ImGui::TextWrapped(modern
        ? "Framework and Director, Normal/Fast and buffer comparison; Advanced also includes combined pacing."
        : "Legacy VDXR: Framework Normal/Fast, including RIFE Medium and High.");
    ImGui::TextWrapped("Luke Ross: up to %zu tests. AFW 1x1, AFW 2x2, OSR and OSR cheap, each with RIFE Medium and High.",
        benchmark_plan(false,true,modern,draft.advanced_mode).size());
    ImGui::TextWrapped("RIFE 4.4: Medium and High at 2X first. Test all 3X configurations only if no preceding result holds full refresh, then 4X if needed. Higher multipliers require session support. TensorRT optimization is setup, not measured time.");
    ImGui::TextWrapped("Suite length adjusts to measured results and session capabilities. Skipped configurations remain n.a. in CSV.");
    ImGui::TextWrapped("Close the menu and keep the same test scene. Stop restores original settings. Completion restores them unless Auto pick best one selects a valid result. Memory is sampled once near the end of each trial. Tests keep the startup DLSS-G preset and never change its NVIDIA profile. A is the initial preference; changing presets requires restarting the game.");
    if(draft.advanced_mode && ImGui::CollapsingHeader("Experimental features")) {
        changed |= ImGui::Checkbox("Reduce black bars during headset rotation###R19 synthetic camera (experimental)", &draft.r19_camera);
    }
    return changed;
}
} // namespace

void synchronize_settings_widgets(SettingsWidgetState& ui, const Snapshot& state) noexcept {
    const auto file=configuration_path();
    if(ui.configuration_file!=file) {ui=SettingsWidgetState{};ui.configuration_file=file;}
    if (!ui.initialized) {
        ui.draft = ui.base = state.desired;
        ui.initialized = true;
    } else if (ui.observed_revision != state.revision) {
        ui.draft = ui.dirty ? merge_changes(state.desired, ui.base, ui.draft) : state.desired;
        ui.base = state.desired;
    }
    ui.observed_revision = state.revision;
}

bool publish_settings_widgets(SettingsWidgetState& ui) noexcept {
    if(!ui.configuration_file.empty() && ui.configuration_file!=configuration_path()){ui.dirty=false;ui.initialized=false;return false;}
    if (!ui.dirty || ui.scale_editing) return false;
    if (!request_changes(ui.base, ui.draft)) {
        ui.save_failed = true;
        return false;
    }
    // A competing publisher may already have changed another field. Reload the
    // actual shared snapshot instead of claiming our former full draft is current.
    // snapshot() may briefly return its last coherent value while another peer
    // holds the mutex. Retire this published delta before attempting that reload
    // so a subsequent local edit cannot accidentally replay it.
    ui.draft = sanitize(ui.draft);
    ui.base = ui.draft;
    ui.dirty = false;
    ui.save_failed = false;
    synchronize_settings_widgets(ui, snapshot());
    return true;
}

void finish_settings_widget_edits(SettingsWidgetState& ui) noexcept {
    ui.scale_editing = false;
    static_cast<void>(publish_settings_widgets(ui));
}

static void luke_swapchain_budget(const Snapshot& state) {
    if (state.runtime.shared_queue)
        ImGui::TextWrapped("Shared queue: AFW/capture and OFXR share a GPU queue. This may reduce performance.");
    const auto& b=state.runtime.swapchains;
    ImGui::SeparatorText("OpenXR swapchains");
    if (!state.connected || !b.updated_ms || GetTickCount64()-b.updated_ms>3000) {
        ImGui::TextDisabled("Waiting for a current Luke session inventory.");return;
    }
    ImGui::Text("Total swapchains: %u",b.total);
    ImGui::Text("OFXR: %u",b.current+b.synthetic+b.auxiliary_outputs+b.fps+b.meter+b.menu);
    ImGui::Text("Game / Luke Ross: %u",b.application);
    if(b.unassigned_native)ImGui::Text("Other / unclassified: %u",b.unassigned_native);
    if (const auto warning=swapchain_budget_warning(b);!warning.empty()) {
        ImGui::TextColored(ImVec4(1.F,.75F,.25F,1.F),"Warning");
        ImGui::TextWrapped("%.*s",static_cast<int>(warning.size()),warning.data());
    }
    // Draw actual lines only. Empty/padded rows must not reserve a large
    // blank block before the ownership limitation text in the headset menu.
    std::string_view details(b.details,std::find(std::begin(b.details),std::end(b.details),'\0')-std::begin(b.details));
    while (!details.empty()) {
        const auto newline=details.find('\n');
        auto line=details.substr(0,newline);
        const auto begin=line.find_first_not_of(" \t\r");
        if (begin!=std::string_view::npos) {
            line.remove_prefix(begin);
            line=line.substr(0,line.find_last_not_of(" \t\r")+1);
            ImGui::PushTextWrapPos();
            ImGui::TextUnformatted(line.data(),line.data()+line.size());
            ImGui::PopTextWrapPos();
        }
        if (newline==std::string_view::npos) break;
        details.remove_prefix(newline+1);
    }
    if (b.details_truncated) ImGui::TextDisabled("Additional app handles omitted from this panel.");
    if(b.unassigned_native)ImGui::TextColored(ImVec4(1.F,.75F,.25F,1.F),"Warning: some native swapchains have no assigned owner.");
}

bool render_settings_widgets(SettingsWidgetState& ui, const Snapshot& initial,
    SettingsWidgetOptions options) {
    set_interface_visible(options.compact ? InterfaceSurface::headset : InterfaceSurface::desktop, true);
    synchronize_settings_widgets(ui, initial);
    ui.scale_editing = false;
    ImGui::PushID("OFXRBridgeSettings");
    if (options.compact) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 7));
    status(initial);
    std::optional<Snapshot> selected;
    if(!options.compact && options.desktop_profile_header) {
        if(options.desktop_profile_header(ui)) {
            selected=snapshot();synchronize_settings_widgets(ui,*selected);
        }
    } else {
        const auto profile=configuration_path();
        ImGui::TextWrapped("Settings: %s",profile==global_configuration_path()?"Global defaults":utf8(profile.filename().wstring()).c_str());
    }
    const auto& state=selected?*selected:initial;
    if(const auto warning=overlay_limit_text(state.runtime.overlay_limit_mask);!warning.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.F,.75F,.25F,1.F));
        ImGui::TextWrapped("%.*s",static_cast<int>(warning.size()),warning.data());
        ImGui::PopStyleColor();
    }
    if (ui.save_failed)
        ImGui::TextColored(ImVec4(1, .5F, .4F, 1), "Could not save settings. Check access to the local profile.");
    bool changed{};
    if (ImGui::BeginTabBar("Options", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        if (ImGui::BeginTabItem("Frame generation")) {
            ImGui::BeginChild("Generation", ImVec2(0, 0));
            ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * .56F, 180.F, 580.F));
            changed |= generation(ui, state, options.compact);
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Frame Reconstruction")) {
            ImGui::BeginChild("Framewarp", ImVec2(0, 0));
            ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * .56F, 180.F, 580.F));
            changed |= framewarp(ui.draft, state);
            if (ImGui::CollapsingHeader("Details (Luke Ross swapchains)",ImGuiTreeNodeFlags_NoAutoOpenOnLog)) luke_swapchain_budget(state);
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Neural Rendering")) {
            ImGui::BeginChild("Neural", ImVec2(0, 0));
            ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * .56F, 180.F, 580.F));
            changed |= neural(ui, state);
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            ImGui::BeginChild("Display", ImVec2(0, 0));
            ImGui::PushItemWidth(std::clamp(ImGui::GetContentRegionAvail().x * .56F, 180.F, 580.F));
            changed |= display(ui, state);
            ImGui::PopItemWidth();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (options.compact) ImGui::PopStyleVar();
    ImGui::PopID();
    ui.dirty |= changed;
    return !options.defer_publication && publish_settings_widgets(ui);
}

} // namespace xrfg::autonomous
