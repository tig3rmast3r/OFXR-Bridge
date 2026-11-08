#include "xrfg/autonomous_control.hpp"
#include "xrfg/upscaler_observation.hpp"
#include <windows.h>
#include <cwctype>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
int checks{};
void require(bool value, const char* detail) {
    ++checks;
    if (!value) throw std::runtime_error(detail);
}
void codec() {
    using xrfg::autonomous::GenerationState;
    using xrfg::autonomous::generation_status_text;
    require(generation_status_text(3,0,GenerationState::ready).starts_with("Hybrid fallback:"),
        "post-AFW Hybrid must identify optical-flow fallback");
    require(generation_status_text(0,1,GenerationState::missing_motion).starts_with("Fallback to optical flow:"),
        "OFXR missing vectors explicitly reports optical-flow fallback");
    require(generation_status_text(1,1,GenerationState::missing_motion).starts_with("Bypass:"),
        "FSR missing guides reports real-frame bypass");
    require(generation_status_text(2,1,GenerationState::missing_depth).find("depth")!=std::string_view::npos,
        "DLSS-G distinguishes missing depth");
    require(generation_status_text(2,1,GenerationState::unavailable).find("initialization")!=std::string_view::npos,
        "native provider initialization limit is visible");
    xrfg::native_ingress::observe_upscaler_inputs(true,false);
    const auto passive=xrfg::native_ingress::upscaler_observation();
    require(passive.evaluations==1 && passive.motion_inputs==1 && passive.depth_inputs==0 &&
        passive.motion && !passive.depth && passive.last_seen_ms!=0,
        "passive observation distinguishes motion from depth without activating guide capture");
    xrfg::autonomous::Settings desired;
    require(!desired.nr_second_pass,"second Neural Rendering pass defaults off");
    require(!xrfg::autonomous::parse_settings("[ofxr]\nnr_enabled=1\n").nr_second_pass,
        "existing profiles retain one Neural Rendering pass");
    auto second_pass=desired;second_pass.nr_second_pass=true;
    for(bool before:{false,true}) {
        second_pass.nr_before_sr=before;
        const auto restored=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(second_pass));
        require(restored.nr_second_pass && restored.nr_before_sr==before,
            "one second-pass flag persists for either pre-SR or post-SR placement");
    }
    second_pass=desired;second_pass.nr_second_pass=true;
    auto concurrent_nr=desired;concurrent_nr.enabled=false;concurrent_nr.afw_mode=1;
    concurrent_nr.nr_working_scale=.5F;concurrent_nr.nr_preset=2;
    const auto merged_nr=xrfg::autonomous::merge_changes(concurrent_nr,desired,second_pass);
    require(merged_nr.nr_second_pass && !merged_nr.enabled && merged_nr.afw_mode==3 &&
        merged_nr.nr_working_scale==.5F && merged_nr.nr_preset==2,
        "second-pass delta preserves visible concurrent settings and resets hidden Basic reconstruction mode");
    require(desired.luke_osr==2 && xrfg::autonomous::parse_settings("").luke_osr==2,
        "factory and missing profile reconstruction default to OSR");
    for(int saved:{0,1,2}) {
        const auto restored=xrfg::autonomous::parse_settings("[ofxr]\nluke_osr="+std::to_string(saved)+"\n");
        const int expected=saved==0?0:2;
        require(restored.luke_osr==expected,"preserve legacy AFW and migrate retired band fill to OSR");
        require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(restored)).luke_osr==expected,
            "reconstruction migration stays stable after profile save/reload");
    }
    require(desired.benchmark_auto_pick,"automatic benchmark choice defaults on");
    require(!desired.status_json,"status export defaults off");
    auto hidden_counter=desired;hidden_counter.overlay_enabled=false;hidden_counter.overlay_position=5;
    const auto counter_saved=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(hidden_counter));
    require(!counter_saved.overlay_enabled && counter_saved.overlay_position==5,"hidden counter retains its selected type across restart");
    const auto legacy_counter=xrfg::autonomous::parse_settings("[ofxr]\noverlay_position=0\n");
    require(!legacy_counter.overlay_enabled && legacy_counter.overlay_position==6,"old Off selection migrates to disabled with a reusable type");
    auto other_counter=desired;other_counter.overlay_position=3;
    auto visibility_only=desired;visibility_only.overlay_enabled=false;
    const auto counter_merged=xrfg::autonomous::merge_changes(other_counter,desired,visibility_only);
    require(!counter_merged.overlay_enabled && counter_merged.overlay_position==3,
        "visibility toggle preserves a concurrent counter type edit");
    auto exported=desired;exported.status_json=true;
    require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(exported)).status_json,"status exporter setting roundtrips in game profile");
    auto concurrent_export=desired;concurrent_export.nr_enabled=true;
    const auto merged_export=xrfg::autonomous::merge_changes(concurrent_export,desired,exported);
    require(merged_export.status_json&&merged_export.nr_enabled,"export edit preserves concurrent generation settings");
    auto automatic=desired;automatic.benchmark_auto_pick=true;
    require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(automatic)).benchmark_auto_pick,
        "automatic benchmark preference roundtrips in the existing game INI");
    auto concurrent_automatic=desired;concurrent_automatic.nr_enabled=true;
    const auto merged_automatic=xrfg::autonomous::merge_changes(concurrent_automatic,desired,automatic);
    require(merged_automatic.benchmark_auto_pick && merged_automatic.nr_enabled,"automatic choice edit preserves concurrent render preferences");
    require(desired.luke_shared_swapchains,"Luke compatible publication sharing defaults on");
    require(!xrfg::autonomous::parse_settings("[ofxr]\nluke_shared_swapchains=0\n").luke_shared_swapchains,
        "INI-only sharing can be explicitly disabled");
    auto shared=desired;shared.luke_shared_swapchains=true;
    auto shared_saved=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(shared));
    require(shared_saved.luke_shared_swapchains,"Luke startup choice roundtrips in canonical INI");
    xrfg::autonomous::select_generation(shared_saved,0,3);
    require(shared_saved.luke_shared_swapchains,"startup choice is global, independent of generation profiles");
    auto concurrent_shared=desired;concurrent_shared.enabled=false;
    const auto shared_merged=xrfg::autonomous::merge_changes(concurrent_shared,desired,shared);
    require(shared_merged.luke_shared_swapchains && !shared_merged.enabled,"startup choice merge preserves concurrent edits");
    require(desired.pacing_mode==1,"Basic mode defaults to Director on the private transport");
    require(xrfg::autonomous::parse_settings("[ofxr]\npacing_mode=2\n").pacing_mode==1,"basic mode replaces hidden legacy combined policy with Director");
    for(int mode=0;mode<3;++mode) {
        auto choice=desired;choice.advanced_mode=true;choice.pacing_mode=mode;
        require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(choice,true)).pacing_mode==mode,"three pacing modes persist with the global advanced policy");
    }
    require(xrfg::autonomous::parse_settings("[ofxr]\nsingle_director=1\n").pacing_mode==1,"V610 ON preserves sole director");
    require(xrfg::autonomous::parse_settings("[ofxr]\nAdvancedMode=1\nsingle_director=0\n").pacing_mode==2,"V610 OFF preserves framework plus director");
    require(xrfg::autonomous::parse_settings("[ofxr]\npacing_mode=0\nsingle_director=1\n").pacing_mode==0,"explicit new choice overrides old checkbox");
    auto experiment=desired;experiment.pacing_mode=1;
    auto experiment_saved=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(experiment));
    require(experiment_saved.pacing_mode==1,"global single-director setting persists in the existing INI");
    xrfg::autonomous::select_generation(experiment_saved,0,3);
    require(experiment_saved.pacing_mode==1,"changing generation profile preserves the global experiment");
    auto changed_elsewhere=desired;changed_elsewhere.enabled=false;
    const auto experiment_merged=xrfg::autonomous::merge_changes(changed_elsewhere,desired,experiment);
    require(experiment_merged.pacing_mode==1 && !experiment_merged.enabled,
        "global checkbox merge preserves another interface's concurrent edits");
    for (int inherited = 0; inherited != 4; ++inherited) {
        auto manual = desired;
        manual.backend = inherited;
        xrfg::autonomous::select_generation(manual, 2, inherited);
        require(manual.frame_generation == 2 && manual.backend == 0,
            "manual DLSS-G uses the benchmark presenter, independent of preceding OFXR");
        manual.backend = inherited;
        require(xrfg::autonomous::sanitize(manual).backend == 0,
            "DLSS-G IPC/INI canonicalization cannot restore an inherited OFXR presenter");
        require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(manual)).backend == 0,
            "saved DLSS-G selection matches the benchmark presenter");
        manual.frame_generation = 0;
        require(xrfg::autonomous::sanitize(manual).backend == inherited,
            "DLSS-G normalization does not change OFXR engine selection");
    }
    require(desired.lower_vram == 0, "Lower VRAM defaults off");
    require(!desired.r19_camera, "R19 camera defaults off for legacy profiles");
    desired.r19_camera=true;
    require(!xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(desired)).r19_camera,"Basic mode forces the hidden R19 flag off");
    auto advanced_r19=desired;advanced_r19.advanced_mode=true;
    require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(advanced_r19,true)).r19_camera,"Advanced mode retains the experimental R19 choice");
    auto r19_base=desired, r19_edit=desired, concurrent=desired;
    r19_edit.r19_camera=false; concurrent.enabled=false;
    const auto merged=xrfg::autonomous::merge_changes(concurrent,r19_base,r19_edit);
    require(!merged.r19_camera && !merged.enabled,"R19 live edit preserves concurrent FG edit");
    const auto old_memory = xrfg::autonomous::parse_settings("[ofxr]\nlower_vram=-1\n");
    require(old_memory.lower_vram == 0, "legacy route default migrates to Lower VRAM off");
    const auto explicit_memory = xrfg::autonomous::parse_settings("[ofxr]\nlower_vram=1\n");
    require(explicit_memory.lower_vram == 1 && explicit_memory.separate_synthesis == 1,
        "old Lower VRAM migrates both controls on");
    for (int separate : {0,1}) for (int reduce : {0,1}) {
        auto independent = desired;
        independent.separate_synthesis = separate; independent.lower_vram = reduce;
        auto restored = xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(independent));
        require(restored.separate_synthesis == reduce && restored.lower_vram == reduce,
            "Unified Lower VRAM requests both operations together");
    }
    const auto migrated_profiles = xrfg::autonomous::parse_settings(
        "[ofxr]\nbackend=1\n[route.ofxr_reference]\nlower_vram=1\n"
        "[route.ofxr_new]\nlower_vram=1\nseparate_synthesis=0\n");
    require(migrated_profiles.generation_profiles[1].separate_synthesis == 1 &&
        migrated_profiles.generation_profiles[2].separate_synthesis == 1,
        "Old partial controls migrate to combined Lower VRAM");
    for (int rate : {0,4,8,99,-1}) {
        auto settings = desired; settings.advanced_mode=true; settings.afw_shading_rate = rate;
        const auto restored=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(settings,true));
        require(restored.afw_shading_rate == (std::clamp(rate,0,8)==8 ? 4 : std::clamp(rate,0,8)), "AFW shading rate persistence and bounds");
    }
    for(int scale=0;scale<4;++scale) {
        auto quarter=desired;quarter.advanced_mode=true;quarter.scale=scale;
        const auto restored=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(quarter,true));
        require(restored.scale==scale,"all four flow resolutions survive restart");
    }
    desired.backend = 3;
    desired.triple_frame_gen = true;
    desired.luke_osr_lower_quality=true;
    desired.afw_mode = 2; desired.afw_shading_rate = 8;
    desired.nr_enabled = true;
    desired.nr_second_pass = true;
    desired.nr_working_scale = .731F;
    desired.nr_compare = 2;
    desired.nr_compare_split = .317F;
    desired.nr_compare_swap = true;
    desired.menu_scale = .85F;
    desired.menu_offset_x = -.13F;
    desired.menu_offset_y = .24F;
    require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(desired)) == xrfg::autonomous::sanitize(desired),
        "complete settings profile round trip");
    desired.lower_vram = 1;
    require(xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(desired)) == xrfg::autonomous::sanitize(desired),
        "Lower VRAM on profile round trip");
    const auto guarded = xrfg::autonomous::parse_settings(
        "[other]\nbackend=1\n[ofxr]\nbackend=77\nnr_intensity=nan\n"
        "native_scale=4\nnr_working_scale=0\nnr_compare_split=2\n"
        "frame_generation=1\nmotion_vectors=0\nnr_style=1e30\n");
    require(guarded.backend == 3, "backend bounded");
    require(guarded.nr_intensity == 1.F, "nonfinite profile value rejected");
    require(guarded.native_scale == 25, "native resolution bounded");
    require(guarded.nr_working_scale == .25F, "NR model resolution bounded");
    require(guarded.nr_compare_split == 1.F, "comparison fraction bounded");
    require(guarded.motion_vectors == 1, "FSR requires native guides");
    require(guarded.nr_style == 0, "out-of-range integral conversion rejected");
    const auto menu = xrfg::autonomous::parse_settings(
        "[ofxr]\nmenu_scale=2\nmenu_offset_x=5\nmenu_offset_y=nan\n");
    require(menu.menu_scale == 1.25F && menu.menu_offset_x == 1.F && menu.menu_offset_y == 0.F,
        "menu presentation bounded without changing game options");
    const auto legacy = xrfg::autonomous::parse_settings("[ofxr]\nbackend=3\n");
    require(legacy.menu_scale == xrfg::autonomous::kDefaultMenuScale && legacy.menu_offset_x == 0.F && legacy.menu_offset_y == 0.F,
        "existing profiles preserve centred V441-size default");
    desired.nr_local_tone = std::numeric_limits<float>::infinity();
    require(xrfg::autonomous::sanitize(desired).nr_local_tone == 1.F,
        "infinite requested runtime value sanitized");
}
void basic_defaults_policy() {
    using namespace xrfg::autonomous;
    Settings dirty;dirty.advanced_mode=true;dirty.profiles_initialized=true;
    dirty.pacing_mode=2;dirty.motion_vectors=1;dirty.preset=0;dirty.scale=3;dirty.backward=true;
    dirty.rife_model=2;dirty.r19_camera=true;dirty.afw_mode=2;dirty.afw_debug=true;
    dirty.afw_clear_before_warping=true;dirty.afw_ignore_motion_threshold=9;
    dirty.afw_depth_dilate_mode=2;dirty.afw_shading_rate=4;
    dirty.luke_advanced=true;dirty.luke_osr_cover=8;dirty.luke_osr_trim=2;dirty.luke_osr_fill=8;
    dirty.rife_resolution=25;dirty.rife_multiplier=3;dirty.increase_buffer=true;
    dirty.lower_vram=1;dirty.nr_enabled=true;dirty.nr_colour_strength=.3F;dirty.nr_debug_view=2;
    for(auto& route:dirty.generation_profiles){route.preset=2;route.scale=3;route.backward=true;route.motion_vectors=1;}
    const auto advanced=sanitize(dirty);
    require(advanced.preset==0&&advanced.scale==3&&advanced.backward&&advanced.afw_debug&&advanced.afw_mode==2,
        "Advanced retains editable optical and reconstruction settings");
    dirty.advanced_mode=false;const auto basic=sanitize(dirty);const Settings defaults;
    require(basic.preset==0&&basic.scale==3&&!basic.backward&&!basic.motion_vectors,
        "Basic retains visible Slow/size and resets hidden backward/native controls");
    require(basic.afw_mode==3&&!basic.afw_debug&&!basic.afw_clear_before_warping&&
        basic.afw_ignore_motion_threshold==defaults.afw_ignore_motion_threshold&&
        basic.afw_depth_dilate_mode==0&&basic.afw_shading_rate==0&&!basic.luke_advanced,
        "Basic resets every hidden reconstruction sub-control");
    require(!basic.r19_camera&&basic.pacing_mode==1&&basic.rife_model==3&&
        basic.luke_osr_cover==0&&basic.luke_osr_trim==0&&basic.luke_osr_fill==2,
        "Basic resets hidden experimental, model, pacing and retired band choices");
    require(basic.rife_resolution==25&&basic.rife_multiplier==3&&basic.increase_buffer&&
        basic.lower_vram==1&&basic.nr_enabled&&basic.nr_colour_strength==.3F&&basic.nr_debug_view==2,
        "Basic retains controls still visible in normal mode");
    for(unsigned i=0;i<basic.generation_profiles.size();++i) {
        const auto& route=basic.generation_profiles[i];
        require(route.preset==(i==2?0:1)&&route.scale==(i==7?5:(i<4||i==6)?3:2)&&!route.backward,
            "inactive engine profiles cannot restore hidden nondefault flow settings");
        require(route.motion_vectors==(i>=4&&i<=6?1:0),"required native guides remain enabled only on their routes");
    }
    require(parse_settings(serialize_settings(dirty))==basic,"saved Basic profile uses the enforced defaults");
    const auto old=parse_settings("[ofxr]\npreset=2\nscale=3\nbackward=1\n");
    require(old.preset==1&&old.scale==3&&!old.backward,
        "saved hidden Fast/backward normalize to Medium/Off while visible size survives");
    dirty.afw_mode=0;dirty.pacing_mode=0;auto off=sanitize(dirty);
    require(off.afw_mode==0&&off.pacing_mode==0,"Basic preserves visible reconstruction Off and Framework choices");
    for(int mode:{0,1,2,3,4}) {
        auto selected=dirty;select_generation(selected,mode,2);
        require(selected.preset==(mode==0?0:1)&&!selected.backward&&selected.scale==(mode==4?5:(mode==0||mode==3)?3:2),
            "changing engine in Basic cannot resurrect inactive tuning");
        require(selected.motion_vectors==(mode>0&&mode<4?1:0),"FSR/DLSS-G/Hybrid retain required native vectors");
    }
}
void route_profiles() {
    using namespace xrfg::autonomous;
    require(generation_ui_index(0)==0 && generation_ui_index(3)==1 && generation_ui_index(1)==2 && generation_ui_index(2)==3 && generation_ui_index(4)==4,
        "Hybrid UI order preserves existing persisted generator IDs");
    require(generation_from_ui(1)==3 && generation_from_ui(2)==1 && generation_from_ui(3)==2 && generation_from_ui(4)==4,"UI selection maps to stable route IDs");
    Settings hybrid;hybrid.advanced_mode=true;select_generation(hybrid,0,2);hybrid.scale=0;hybrid.motion_vectors=0;hybrid=sanitize(hybrid);
    select_generation(hybrid,3,2);hybrid.scale=1;hybrid.lower_vram=1;hybrid.hybrid_backend=1;hybrid=sanitize(hybrid);
    select_generation(hybrid,1,2);hybrid.native_scale=42;hybrid=sanitize(hybrid);
    hybrid=parse_settings(serialize_settings(hybrid,true));select_generation(hybrid,3,2);
    require(hybrid.scale==1 && hybrid.lower_vram==1 && hybrid.hybrid_backend==1,"Hybrid retains own engine and preferences");
    require(hybrid.backend==2,"Hybrid engine is separate from the OFXR engine");
    hybrid.hybrid_backend=3;hybrid=sanitize(hybrid);
    require(hybrid.hybrid_backend==2,"Hybrid never takes the 0.2.13 pacing route");
    Settings fresh;select_generation(fresh,3,0);
    require(fresh.scale==2,"Basic Hybrid flow resolution defaults to50 percent");
    auto older=parse_settings("[ofxr]\nframe_generation=3\nscale=2\n[route.hybrid]\nscale=2\n[route.ofxr_new]\nscale=1\n");
    require(older.scale==2 && older.generation_profiles[6].scale==2 && older.generation_profiles[2].scale==1,
        "Basic retains visible Hybrid and inactive optical flow size choices");
    older.scale=2;older=parse_settings(serialize_settings(older));
    require(older.scale==2,"Hybrid's chosen flow resolution survives a save");
    Settings multiplied;select_generation(multiplied,0,2);multiplied.frame_multiplier=4;multiplied=sanitize(multiplied);
    select_generation(multiplied,3,0);multiplied.frame_multiplier=3;multiplied=sanitize(multiplied);
    select_generation(multiplied,0,3);multiplied.frame_multiplier=4;multiplied=sanitize(multiplied);
    multiplied=parse_settings(serialize_settings(multiplied));
    require(multiplied.frame_multiplier==2 && generation_multiplier(multiplied)==2,"0.2.13 keeps its own 3X, never the OFXR multiplier");
    select_generation(multiplied,0,2);require(multiplied.frame_multiplier==4 && generation_multiplier(multiplied)==4,"OFXR 0.3.0 keeps its 4X");
    select_generation(multiplied,3,2);require(multiplied.frame_multiplier==3,"Hybrid keeps its own multiplier");
    select_generation(multiplied,0,0);require(multiplied.frame_multiplier==2,"an untouched engine stays at 2X");
    require(Settings{}.hybrid_depth_search && parse_settings("[ofxr]\nframe_generation=3\n").hybrid_depth_search,
        "Hybrid depth search is on by default, also for profiles saved before it");
    Settings depth;select_generation(depth,3,0);depth.hybrid_depth_search=false;
    depth=parse_settings(serialize_settings(depth));
    require(!depth.hybrid_depth_search,"Hybrid depth search off is saved");
    depth.hybrid_depth_search=true;
    Settings edited=depth;edited.hybrid_depth_search=false;
    require(!merge_changes(depth,depth,edited).hybrid_depth_search,
        "the UI's Depth search change survives the merge into the saved settings");
    require(!Settings{}.hybrid_hud_fix && !parse_settings("[ofxr]\nframe_generation=3\n").hybrid_hud_fix,
        "Hybrid's HUD fix is off by default, also for profiles saved before it");
    Settings hud;select_generation(hud,3,0);hud.hybrid_hud_fix=true;
    require(parse_settings(serialize_settings(hud)).hybrid_hud_fix,"Hybrid's HUD fix on is saved");
    Settings hud_edited=depth;hud_edited.hybrid_hud_fix=true;
    require(merge_changes(depth,depth,hud_edited).hybrid_hud_fix,
        "the UI's HUD fix change survives the merge into the saved settings");
    Settings dlssg;select_generation(dlssg,2,0);dlssg.triple_frame_gen=true;dlssg=sanitize(dlssg);
    require(runtime_multiplier(dlssg)==2 && generation_multiplier(dlssg)==3,"DLSS-G 3X stays on triple_frame_gen");
    dlssg.frame_multiplier=4;dlssg.triple_frame_gen=false;dlssg=parse_settings(serialize_settings(dlssg));
    require(dlssg.triple_frame_gen && runtime_multiplier(dlssg)==4 && generation_multiplier(dlssg)==4,
        "DLSS-G 4X holds 3X's switch on and reaches the layer as 4");
    select_generation(hybrid,0,2);
    require(hybrid.scale==0 && hybrid.lower_vram==0 && hybrid.motion_vectors==0,"Hybrid leaves existing OFXR profile unchanged");
    require(generation_status_text(3,1,GenerationState::missing_motion).starts_with("Fallback to optical flow:"),"Hybrid missing guides reports fallback");
    Settings saved;saved.advanced_mode=true;
    for (int i = 0; i < 6; ++i) {
        select_generation(saved, i < 4 ? 0 : i - 3, i < 4 ? i : 3);
        require(!saved.triple_frame_gen && saved.lower_vram == 0,
            "unseen routes have independent 2X and Lower VRAM off defaults");
        saved.motion_vectors = i % 2;
        saved.triple_frame_gen = i == 3;
        saved.lower_vram = i == 2;
        saved.separate_synthesis = i % 2;
        saved.preset = i % 3;
        saved.scale = (i + 1) % 3;
        saved.native_scale = 30 + i * 10;
        saved.backward = i % 2;
        saved.prefer_fps = !(i % 2);
        saved = sanitize(saved);
    }
    require(serialize_settings(saved).find("AdvancedMode")==std::string::npos,"game serialization excludes the global UI preference");
    const auto text = serialize_settings(saved,true);
    auto restored = parse_settings(text);
    require(restored == saved, "all six inactive and active profiles survive serialized restart");
    for (int i = 0; i < 6; ++i) {
        select_generation(restored, i < 4 ? 0 : i - 3, i < 4 ? i : 3);
        require(restored.motion_vectors == (i < 4 ? i % 2 : 1) &&
            restored.triple_frame_gen == (i == 3) && restored.lower_vram == (i == 2) &&
            restored.separate_synthesis == (i == 2) &&
            restored.preset == i % 3 && restored.scale == (i + 1) % 3 &&
            restored.native_scale == 30 + i * 10 && restored.backward == bool(i % 2) &&
            restored.prefer_fps == !bool(i % 2), "each route restores its own complete preferences");
    }
    Settings fresh_rife;
    select_generation(fresh_rife,4,2);fresh_rife=sanitize(fresh_rife);
    require(fresh_rife.scale==5 && fresh_rife.rife_model==3,"new RIFE profile defaults to4.4 exact1/6 motion inference");
    auto migrated_rife=parse_settings("[ofxr]\nframe_generation=0\n[route.ofxr_new]\nscale=2\n");
    select_generation(migrated_rife,4,2);migrated_rife=sanitize(migrated_rife);
    require(migrated_rife.scale==5,"RIFE profile seeds1/6 when upgrading an existing profile INI");
    const auto minimal_rife=parse_settings("[ofxr]\nframe_generation=4\n");
    require(minimal_rife.rife_model==3 && minimal_rife.scale==5,"RIFE-only INI defaults to4.4 and1/6");
    const auto explicit_rife=parse_settings("[ofxr]\nAdvancedMode=1\nframe_generation=4\nrife_model=2\nscale=3\n");
    require(explicit_rife.rife_model==2 && explicit_rife.scale==3,"explicit previous model/resolution remain user choices");
    require(parse_settings("[ofxr]\nframe_generation=4\nrife_model=2\n").rife_model==3,
        "basic mode ignores an old model choice and uses 4.4 without needing an open interface");
    auto basic=explicit_rife;basic.advanced_mode=false;
    require(sanitize(basic).rife_model==3,"leaving Advanced mode selects 4.4");
    require(minimal_rife.rife_resolution==18,"RIFE resolution defaults to Medium");
    for(int percent:{10,15,18,21,25,30,36}) {
        auto slider=minimal_rife;slider.rife_resolution=percent;
        require(parse_settings(serialize_settings(slider)).rife_resolution==percent,"RIFE preset percentage roundtrip");
    }
    auto bounded=minimal_rife;bounded.rife_resolution=1;require(sanitize(bounded).rife_resolution==10,"RIFE slider minimum");
    bounded.rife_resolution=99;require(sanitize(bounded).rife_resolution==36,"RIFE Godlike maximum");
    require(parse_settings("[ofxr]\nrife_resolution=23\n").rife_resolution==21,"legacy intermediate maps to nearest preset; equal distance prefers lower");
    require(parse_settings("[ofxr]\nrife_resolution=40\n").rife_resolution==36,"old upper bound migrates to Godlike");
    auto rife=parse_settings("[ofxr]\nAdvancedMode=1\nframe_generation=4\nrife_model=2\nmotion_vectors=1\ntriple_frame_gen=1\n");
    require(rife.frame_generation==4 && rife.rife_model==2 && rife.motion_vectors==0 && !rife.triple_frame_gen,"RIFE uses no engine guides and is 2X only");
    auto rife_roundtrip=parse_settings(serialize_settings(rife,true));
    require(rife_roundtrip.rife_model==2 && rife_roundtrip.frame_generation==4,"advanced RIFE model survives configuration roundtrip");
    auto fifth=parse_settings("[ofxr]\nAdvancedMode=1\nframe_generation=4\nrife_model=3\nscale=4\n");
    require(fifth.rife_model==3 && fifth.scale==4,"RIFE 4.4 and 20 percent survive normalization");
    auto fifth_roundtrip=parse_settings(serialize_settings(fifth,true));
    require(fifth_roundtrip.scale==4 && fifth_roundtrip.rife_model==3,"RIFE 20 percent is persisted in its own profile");
    select_generation(fifth,0,2);
    require(fifth.scale<=3,"ordinary OFXR never inherits unsupported fifth grid");
    select_generation(fifth,4,2);
    require(fifth.scale==4,"RIFE restores its fifth inference grid after route changes");
    auto multi=parse_settings("[ofxr]\nframe_generation=4\nrife_model=3\nrife_multiplier=4\nscale=5\n");
    auto multi_copy=parse_settings(serialize_settings(multi));
    require(multi_copy.rife_multiplier==4 && multi_copy.scale==5,"RIFE4X and exact1/6 persist");
    multi.rife_multiplier=8;multi=sanitize(multi);require(multi.rife_multiplier==4,"RIFE multiplier bounded to4X");
    auto retired=parse_settings("[ofxr]\nAdvancedMode=1\nframe_generation=4\nrife_model=1\n");
    require(retired.rife_model==2,"retired heavy Lite choice migrates to accepted 4.6");
    auto legacy = parse_settings("[ofxr]\nframe_generation=2\nbackend=3\nmotion_vectors=1\ntriple_frame_gen=1\nlower_vram=1\n");
    require(legacy.triple_frame_gen && legacy.lower_vram, "legacy active DLSS-G preferences retained");
    select_generation(legacy, 0, 3);
    require(!legacy.triple_frame_gen && !legacy.lower_vram && legacy.motion_vectors == 0,
        "legacy guide-only route does not seed OFXR with game vectors or 3X");
    auto base = saved;
    select_generation(base, 0, 3);
    auto tray = base;
    select_generation(tray, 2, 3);
    auto headset = base;
    headset.triple_frame_gen = false;
    headset.lower_vram = 1;
    auto merged = merge_changes(tray, base, headset);
    require(merged.frame_generation == 2 && !merged.triple_frame_gen && !merged.lower_vram,
        "stale headset DJ edits cannot overwrite active DLSS-G preferences");
    select_generation(merged, 0, 3);
    require(!merged.triple_frame_gen && merged.lower_vram,
        "stale headset edits remain attached to the original DJ profile");
    const auto hostile = parse_settings("[ofxr]\nAdvancedMode=1\nbackend=3\n[route.ofxr_djules]\nmotion_vectors=77\npreset=-99\nscale=nan\nnative_scale=999\n[route.dlssg]\nmotion_vectors=0\nnative_scale=-9\n");
    require(hostile.motion_vectors == 1 && hostile.preset == 0 && hostile.scale == 2 &&
        hostile.native_scale == 100 && hostile.generation_profiles[5].motion_vectors == 1 &&
        hostile.generation_profiles[5].native_scale == 25,
        "active and inactive route sections reject nonfinite values and clamp ranges");
}
} // namespace

int wmain(int count, wchar_t** arguments) {
    try {
        {
            xrfg::autonomous::Snapshot sample;sample.connected=true;
            sample.runtime.video_memory={1,60,100,40,20,10,123,true,true};
            require(xrfg::autonomous::interface_vram_text(sample)=="VRAM 60%","VRAM includes worker once, not its Torch subset twice");
            sample.runtime.video_memory.rife_valid=false;
            require(xrfg::autonomous::interface_vram_text(sample)=="VRAM N/A","unknown active worker memory cannot masquerade as a complete total");
            sample.runtime.video_memory.rife_active=false;
            require(xrfg::autonomous::interface_vram_text(sample)=="VRAM 60%","ordinary routes retain existing meter");
        }

        if (count == 2 && (std::wstring_view(arguments[1]) == L"--child" ||
            std::wstring_view(arguments[1]) == L"--restart")) {
            const auto state = xrfg::autonomous::snapshot();
            require(state.desired.backend == 3 && state.desired.nr_enabled && state.desired.nr_second_pass && state.desired.nr_compare == 2,
                "child observes exact published settings");
            auto settings = state.desired;
            xrfg::autonomous::select_generation(settings, 0, 3);
            require(settings.triple_frame_gen && settings.motion_vectors == 0,
                "new process restores inactive DJ preferences");
            xrfg::autonomous::select_generation(settings, 2, 3);
            require(!settings.triple_frame_gen && settings.native_scale == 63,
                "new process restores independent inactive DLSS-G preferences");
            if (std::wstring_view(arguments[1]) == L"--restart") return 0;
            xrfg::autonomous::RuntimeStatus status;
            status.applied_revision = state.revision;
            status.sessions = 1;
            status.ready = 1;
            status.nr_running = true;
            xrfg::autonomous::report_runtime(status);
            return 0;
        }
        codec();
        basic_defaults_policy();
        route_profiles();
        const auto fixture = std::filesystem::temp_directory_path() /
            (L"ofxr-autonomous-control-fixture-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()));
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", fixture.c_str());
        require(xrfg::autonomous::configuration_path().parent_path() == fixture, "fixture profile isolation");
        std::uint64_t old_identity=14695981039346656037ULL;
        for(const auto c:xrfg::autonomous::configuration_path().wstring()) {
            old_identity^=std::uint64_t(std::towlower(c));old_identity*=1099511628211ULL;
        }
        const auto legacy_name=L"Local\\OFXRBridgeV558-"+std::to_wstring(old_identity)+L"-control";
        const HANDLE legacy_mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,512,legacy_name.c_str());
        require(legacy_mapping!=nullptr,"old control mapping fixture");
        auto* legacy=static_cast<std::uint32_t*>(MapViewOfFile(legacy_mapping,FILE_MAP_ALL_ACCESS,0,0,512));
        require(legacy!=nullptr,"old control mapping view");
        legacy[0]=0x41465852;legacy[1]=55801;legacy[2]=512;
        const auto original = xrfg::autonomous::snapshot();
        auto desired = original.desired;
        for (int rate : {0,4,8,99,-1}) {
        auto settings = desired; settings.advanced_mode=true; settings.afw_shading_rate = rate;
        const auto restored=xrfg::autonomous::parse_settings(xrfg::autonomous::serialize_settings(settings,true));
        require(restored.afw_shading_rate == (std::clamp(rate,0,8)==8 ? 4 : std::clamp(rate,0,8)), "AFW shading rate persistence and bounds");
    }
    desired.backend = 3;
        desired.nr_enabled = true;
        desired.nr_second_pass = true;
        desired.nr_compare = 2;
        xrfg::autonomous::select_generation(desired, 0, 3);
        desired.triple_frame_gen = true;
        desired.motion_vectors = 0;
        xrfg::autonomous::select_generation(desired, 2, 3);
        desired.triple_frame_gen = false;
        desired.native_scale = 63;
        xrfg::autonomous::select_generation(desired, 0, 3);
        require(xrfg::autonomous::request(desired), "atomic profile and shared publication");
        require(legacy[0]==0x41465852 && legacy[1]==55801 && legacy[2]==512,
            "new protocol coexists with the live old mapping without changing it");
        UnmapViewOfFile(legacy);CloseHandle(legacy_mapping);
        const auto updated = xrfg::autonomous::snapshot();
        require(updated.revision == original.revision + 1, "one desired settings revision");
        require(xrfg::autonomous::request(desired), "idempotent request");
        require(xrfg::autonomous::snapshot().revision == updated.revision, "unchanged settings retain revision");
        require(std::filesystem::is_regular_file(xrfg::autonomous::configuration_path()), "profile persisted");
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0,
            "find test process path");
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --child";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE, "start isolated control peer");
        require(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "control peer completes");
        DWORD result{};
        GetExitCodeProcess(child.hProcess, &result);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        require(result == 0, "peer reads same mapping and profile");
        const auto reported = xrfg::autonomous::snapshot();
        require(reported.connected && reported.runtime.ready == 1 && reported.runtime.nr_running,
            "real cross-process runtime report");
        require(reported.runtime.applied_revision == updated.revision, "reported applied revision matches exact settings");
        require(reported.runtime.process_id != GetCurrentProcessId(), "runtime status identifies its producer");
        // A new profile identity gives the peer a fresh shared mapping, so this
        // proves initialization from the persisted file rather than cached IPC.
        const auto restart_profile = fixture / L"restart";
        std::filesystem::create_directory(restart_profile);
        std::filesystem::copy_file(xrfg::autonomous::configuration_path(), restart_profile / L"ofxr-bridge.ini");
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", restart_profile.c_str());
        command = L"\"" + std::wstring(executable) + L"\" --restart";
        const bool restart_started = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
        SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR", fixture.c_str());
        require(restart_started, "start clean process from persisted route profiles");
        require(WaitForSingleObject(child.hProcess, 10000) == WAIT_OBJECT_0, "profile restart peer completes");
        GetExitCodeProcess(child.hProcess, &result);
        CloseHandle(child.hThread); CloseHandle(child.hProcess);
        require(result == 0, "disk-only process restart retains independent route preferences");
        std::filesystem::remove(restart_profile / L"ofxr-bridge.ini");
        std::filesystem::remove(restart_profile);
        // Simultaneous tray/headset edits are deltas against the state each
        // client observed, never an overwrite by a stale full draft.
        auto headset = updated.desired;
        headset.afw_mode = 1;
        auto tray = updated.desired;
        tray.nr_intensity = .375F;
        tray.menu_scale = .75F;
        require(xrfg::autonomous::request_changes(updated.desired, tray), "tray delta publication");
        require(xrfg::autonomous::request_changes(updated.desired, headset), "stale headset delta publication");
        auto merged = xrfg::autonomous::snapshot();
        require(merged.desired.afw_mode == 3 && merged.desired.nr_intensity == .375F,
            "independent visible edits survive while hidden Basic submode stays at default");
        require(merged.desired.menu_scale == .75F,
            "headset size survives an independent stale engine edit");
        require(merged.desired.backend == 3 && merged.desired.nr_enabled, "delta retains untouched runtime settings");
        auto generation = merged.desired;
        generation.frame_generation = 1;
        require(xrfg::autonomous::request_changes(merged.desired, generation), "generation switch delta");
        require(xrfg::autonomous::snapshot().desired.motion_vectors == 1,
            "merged generation selection still enforces native guides");
        using xrfg::autonomous::OverlaySwapchain;
        xrfg::autonomous::report_runtime({}); // current reporter takes over the completed peer fixture
        xrfg::autonomous::report_gpu_usage({GetTickCount64(),83,true});
        xrfg::autonomous::report_cpu_usage({GetTickCount64(),37,true});
        xrfg::autonomous::report_runtime({});
        require(xrfg::autonomous::snapshot().runtime.gpu_usage.valid && xrfg::autonomous::snapshot().runtime.gpu_usage.percent==83,
            "ordinary runtime heartbeats preserve independently sampled GPU telemetry over IPC");
        require(xrfg::autonomous::snapshot().runtime.cpu_usage.valid && xrfg::autonomous::snapshot().runtime.cpu_usage.percent==37,
            "ordinary runtime heartbeats preserve independently sampled CPU telemetry over IPC");
        xrfg::autonomous::report_overlay_limit(OverlaySwapchain::menu,true);
        xrfg::autonomous::report_overlay_limit(OverlaySwapchain::meter,true);
        require(xrfg::autonomous::snapshot().runtime.overlay_limit_mask==6,"independent overlay refusals reach shared UI");
        xrfg::autonomous::report_runtime({});
        require(xrfg::autonomous::snapshot().runtime.overlay_limit_mask==6,"runtime heartbeat preserves pending warnings");
        xrfg::autonomous::report_overlay_limit(OverlaySwapchain::menu,false);
        require(xrfg::autonomous::snapshot().runtime.overlay_limit_mask==2,"one successful surface leaves other warnings intact");
        xrfg::autonomous::report_overlay_limit(OverlaySwapchain::meter,false);
        require(xrfg::autonomous::snapshot().runtime.overlay_limit_mask==0,"all successful retries clear overlay warnings");
        xrfg::autonomous::RuntimeStatus budget_report{};
        budget_report.sessions=budget_report.ready=1;
        budget_report.swapchains.updated_ms=GetTickCount64();budget_report.swapchains.total=14;
        budget_report.swapchains.fallback=true;
        xrfg::autonomous::report_runtime(budget_report);
        const auto shared_budget=xrfg::autonomous::snapshot();
        require(shared_budget.runtime.swapchains.total==14 && shared_budget.runtime.swapchains.fallback,
            "versioned IPC retains swapchain inventory and actual fallback");
        require(xrfg::autonomous::interface_status_text(shared_budget).find("Swapchain fallback")!=std::string::npos,
            "actual fallback reaches shared status");
        budget_report.shared_queue=true;
        xrfg::autonomous::report_runtime(budget_report);
        auto queue_report=xrfg::autonomous::snapshot();
        require(queue_report.runtime.shared_queue &&
            xrfg::autonomous::interface_status_text(queue_report).starts_with("Shared queue"),
            "actual queue fallback survives IPC and appears first in status");
        const auto active_before=xrfg::autonomous::interface_generation_active(queue_report);
        budget_report.shared_queue=false;
        xrfg::autonomous::report_runtime(budget_report);
        queue_report=xrfg::autonomous::snapshot();
        require(xrfg::autonomous::interface_status_text(queue_report).find("Shared queue")==std::string::npos &&
            xrfg::autonomous::interface_generation_active(queue_report)==active_before,
            "queue warning clears without changing actual generator activity");
        using Surface=xrfg::autonomous::InterfaceSurface;
        for(auto surface:{Surface::desktop,Surface::headset,Surface::meter,Surface::status_export})
            xrfg::autonomous::set_interface_visible(surface,false);
        require(!xrfg::autonomous::interface_telemetry_requested(),"all hidden surfaces allow telemetry suspension");
        xrfg::autonomous::set_interface_visible(Surface::status_export,true);
        require(xrfg::autonomous::interface_telemetry_requested(),"exporter alone holds telemetry without an invisible desktop");
        xrfg::autonomous::set_interface_visible(Surface::headset,true);
        xrfg::autonomous::set_interface_visible(Surface::status_export,false);
        require(xrfg::autonomous::interface_telemetry_requested(),"exporter OFF cannot release headset lease");
        xrfg::autonomous::set_interface_visible(Surface::headset,false);
        require(!xrfg::autonomous::interface_telemetry_requested(),"exporter OFF releases its own telemetry request");
        // Only delete the single file/directory this fixture created.
        std::filesystem::remove(xrfg::autonomous::configuration_path());
        std::filesystem::remove(fixture);
        std::cout << checks << " autonomous control checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "autonomous control failed: " << error.what() << '\n';
        return 1;
    }
}
