#include <optional>
#include "xrfg/presented_output_rate.hpp"
#include "xrfg/settings_widgets.hpp"
#include "xrfg/rife_resolution.hpp"
#include "xrfg/dlssg_profile.hpp"
#include "xrfg/benchmark_suite.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "xrfg/rife_model_catalog.hpp"
#include "xrfg/rife_runtime_locator.hpp"
#include <windows.h>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace xrfg::autonomous {
namespace {
#include "settings_fonts.inc"
#include "settings_icons.inc"

// The panel's design tokens. The headset values lift borders and secondary text
// because the compositor magnifies the quad.
struct Palette {
    ImU32 surface,raised,control,hover,line,soft,ink,muted,accent,on_accent,accent_soft,ok,warn,danger,warn_soft,danger_soft;
};
constexpr ImU32 rgb(unsigned value) {
    return IM_COL32((value>>16)&255,(value>>8)&255,value&255,255);
}
constexpr Palette kHeadsetPalette{rgb(0x0e121c),rgb(0x161c2a),rgb(0x1f2738),rgb(0x2c374e),rgb(0x6b7694),rgb(0x3a4460),
    rgb(0xeef1f7),rgb(0xaeb7ca),rgb(0x70caff),rgb(0x0b1220),rgb(0x1b3a52),rgb(0x5ce08e),rgb(0xffc048),rgb(0xff6262),rgb(0x3a2f18),rgb(0x3f1f24)};
constexpr Palette kDesktopPalette{rgb(0x0e121c),rgb(0x161c2a),rgb(0x1c2433),rgb(0x283247),rgb(0x68748f),rgb(0x30384c),
    rgb(0xeef1f7),rgb(0x949eb2),rgb(0x70caff),rgb(0x0b1220),rgb(0x1b3a52),rgb(0x5ce08e),rgb(0xffc048),rgb(0xff6262),rgb(0x3a2f18),rgb(0x3f1f24)};
struct Metrics {float metric,value,title,body,label,control,border,gap;};
constexpr Metrics kHeadsetMetrics{40,22,24,18,16,44,2,8};
constexpr Metrics kDesktopMetrics{34,19,20,15,13,36,1,6};
constexpr float kRailWidth=188,kRailNarrow=64,kStatusWidth=232,kHeaderHeight=52,kFooterHeight=40;
constexpr float kNarrowWindow=860,kPad=16,kSwitchWidth=52,kSwitchHeight=28,kCardRadius=6;

enum Face {face_sans,face_semibold,face_mono};
// Added in this order by apply_settings_style. A context without them, such as
// a test fixture, keeps ImGui's default font.
ImFont* face(Face id) {
    const auto& fonts=ImGui::GetIO().Fonts->Fonts;
    return int(id)<fonts.Size ? fonts[id] : ImGui::GetFont();
}
struct FaceScope {
    FaceScope(Face id,float size) {ImGui::PushFont(face(id),size);}
    ~FaceScope() {ImGui::PopFont();}
    FaceScope(const FaceScope&)=delete;
    FaceScope& operator=(const FaceScope&)=delete;
};
ImVec4 vec(ImU32 color) {return ImGui::ColorConvertU32ToFloat4(color);}

void draw_icon(ImDrawList* list,ImVec2 position,float size,ImU32 color,Icon icon) {
    const float scale=size/24.F,width=std::max(1.5F,2.F*scale);
    std::array<ImVec2,64> points;
    for(const float* data=kIcons[int(icon)];*data!=0;) {
        const int stored=int(data[0]),count=std::min(stored,int(points.size()));
        const bool closed=data[1]!=0;
        data+=2;
        for(int i=0;i<count;++i)points[i]=ImVec2(position.x+data[i*2]*scale,position.y+data[i*2+1]*scale);
        data+=stored*2;
        if(count==2 && std::abs(points[0].x-points[1].x)+std::abs(points[0].y-points[1].y)<.5F*scale)
            list->AddCircleFilled(points[0],width*.7F,color);
        else list->AddPolyline(points.data(),count,color,closed?ImDrawFlags_Closed:ImDrawFlags_None,width);
    }
}
} // namespace

void apply_settings_style(bool headset) {
    ImGui::StyleColorsDark();
    const auto& c=headset?kHeadsetPalette:kDesktopPalette;
    const auto& m=headset?kHeadsetMetrics:kDesktopMetrics;
    auto& s=ImGui::GetStyle();
    const auto set=[&](ImGuiCol slot,ImU32 color) {s.Colors[slot]=vec(color);};
    set(ImGuiCol_WindowBg,c.surface);set(ImGuiCol_PopupBg,c.raised);set(ImGuiCol_Text,c.ink);
    set(ImGuiCol_TextDisabled,c.muted);set(ImGuiCol_Border,c.line);set(ImGuiCol_Separator,c.soft);
    set(ImGuiCol_FrameBg,c.control);set(ImGuiCol_FrameBgHovered,c.hover);set(ImGuiCol_FrameBgActive,c.hover);
    set(ImGuiCol_Button,c.control);set(ImGuiCol_ButtonHovered,c.hover);set(ImGuiCol_ButtonActive,c.hover);
    set(ImGuiCol_Header,c.raised);set(ImGuiCol_HeaderHovered,c.hover);set(ImGuiCol_HeaderActive,c.hover);
    set(ImGuiCol_CheckMark,c.accent);set(ImGuiCol_SliderGrab,c.ink);set(ImGuiCol_SliderGrabActive,c.accent);
    set(ImGuiCol_NavCursor,c.ink);set(ImGuiCol_ScrollbarBg,c.surface);set(ImGuiCol_ScrollbarGrab,c.line);
    s.Colors[ImGuiCol_ChildBg]=ImVec4(0,0,0,0);
    s.WindowRounding=0;s.FrameRounding=4;s.ChildRounding=0;s.PopupRounding=4;s.WindowBorderSize=0;s.ChildBorderSize=0;
    s.FrameBorderSize=m.border;s.PopupBorderSize=m.border;s.GrabMinSize=16;s.ScrollbarSize=headset?14:12;
    s.DisabledAlpha=1;s.WindowPadding=ImVec2(0,0);
    auto& fonts=*ImGui::GetIO().Fonts;
    if(fonts.Fonts.Size)return;
    const auto add=[&](const unsigned char* data,std::size_t bytes,const char* name) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas=false;
        std::snprintf(config.Name,sizeof(config.Name),"%s",name);
        fonts.AddFontFromMemoryTTF(const_cast<unsigned char*>(data),int(bytes),m.body,&config);
    };
    add(kFont_sans_regular,sizeof(kFont_sans_regular),"Atkinson Hyperlegible Next");
    add(kFont_sans_semibold,sizeof(kFont_sans_semibold),"Atkinson Hyperlegible Next SemiBold");
    add(kFont_mono_semibold,sizeof(kFont_mono_semibold),"Atkinson Hyperlegible Mono SemiBold");
}

namespace {
enum class Tone {ok,warn,danger,idle};
struct Help {const char* title{};const char* body{};};

// Everything one frame of the panel needs. Help is collected while the controls
// draw, and the status pane shows it last.
struct Panel {
    SettingsWidgetState& ui;
    const Snapshot& state;
    bool compact;
    const Palette& c;
    const Metrics& m;
};
void offer_help(Panel& p,Help help) {
    p.ui.help_title=help.title;p.ui.help_body=help.body;
}
void hover_help(Panel& p,Help help) {
    if(!help.title)return;
    const auto& g=*ImGui::GetCurrentContext();
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || (g.NavCursorVisible && g.NavId && g.NavId==g.LastItemData.ID))
        offer_help(p,help);
}
// Labels stay plain text. Claiming them as items would take away the empty space
// update_menu_drag needs, so their help comes from the rectangle.
void label_help(Panel& p,Help help,ImVec2 min,ImVec2 max) {
    if(help.title && ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(min,max,false))
        offer_help(p,help);
}
ImU32 tone_color(const Panel& p,Tone tone) {
    return tone==Tone::ok ? p.c.ok : tone==Tone::warn ? p.c.warn : tone==Tone::danger ? p.c.danger : p.c.muted;
}
Icon tone_icon(Tone tone) {
    return tone==Tone::ok ? Icon::ok : tone==Tone::warn ? Icon::warn : tone==Tone::danger ? Icon::error : Icon::wait;
}
float right_edge() {return ImGui::GetCursorPosX()+ImGui::GetContentRegionAvail().x;}
float text_width(Face id,float size,const char* text) {
    FaceScope scope(id,size);
    return ImGui::CalcTextSize(text,nullptr,true).x;
}
void dashed_rect(ImDrawList* list,ImVec2 min,ImVec2 max,ImU32 color,float thickness) {
    constexpr float dash=7,gap=5;
    for(float x=min.x+3;x<max.x-3;x+=dash+gap) {
        const auto end=std::min(x+dash,max.x-3);
        list->AddLine({x,min.y},{end,min.y},color,thickness);list->AddLine({x,max.y},{end,max.y},color,thickness);
    }
    for(float y=min.y+3;y<max.y-3;y+=dash+gap) {
        const auto end=std::min(y+dash,max.y-3);
        list->AddLine({min.x,y},{min.x,end},color,thickness);list->AddLine({max.x,y},{max.x,end},color,thickness);
    }
}

void reason(const Panel& p,const char* text) {
    FaceScope scope(face_sans,p.m.label);
    ImGui::PushStyleColor(ImGuiCol_Text,p.c.muted);
    ImGui::PushTextWrapPos();
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}
void group_title(const Panel& p,const char* text) {
    ImGui::Dummy(ImVec2(0,2));
    FaceScope scope(face_semibold,p.m.label);
    ImGui::PushStyleColor(ImGuiCol_Text,p.c.muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}
void notice(const Panel& p,Tone tone,const char* text) {
    FaceScope scope(face_sans,p.m.label);
    const float width=ImGui::GetContentRegionAvail().x,inner=width-48;
    const auto height=ImGui::CalcTextSize(text,nullptr,false,inner).y+16;
    const auto position=ImGui::GetCursorScreenPos();
    const auto color=tone==Tone::warn ? p.c.warn : tone==Tone::danger ? p.c.danger : tone==Tone::ok ? p.c.ok : p.c.ink;
    const auto fill=tone==Tone::warn ? p.c.warn_soft : tone==Tone::danger ? p.c.danger_soft : p.c.raised;
    auto* list=ImGui::GetWindowDrawList();
    list->AddRectFilled(position,{position.x+width,position.y+height},fill,kCardRadius);
    draw_icon(list,{position.x+10,position.y+8},20,color,tone==Tone::idle ? Icon::help : tone_icon(tone));
    ImGui::SetCursorScreenPos({position.x+38,position.y+8});
    ImGui::PushStyleColor(ImGuiCol_Text,color);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+inner);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos({position.x,position.y+height});
    ImGui::Dummy(ImVec2(0,0));
}
void badge(const Panel& p,const char* text,ImVec2 position) {
    FaceScope scope(face_sans,p.m.label);
    const auto size=ImGui::CalcTextSize(text);
    const float height=24,icon=14,width=size.x+icon+20;
    position.y-=height*.5F;
    auto* list=ImGui::GetWindowDrawList();
    list->AddRectFilled(position,{position.x+width,position.y+height},p.c.warn_soft,4);
    draw_icon(list,{position.x+7,position.y+5},icon,p.c.warn,Icon::restart);
    ImGui::PushStyleColor(ImGuiCol_Text,p.c.warn);
    ImGui::RenderText({position.x+icon+12,position.y+(height-size.y)*.5F},text);
    ImGui::PopStyleColor();
}
// Left-hand label of a row, then the cursor placed for a right-aligned control.
void row(Panel& p,const char* label,Help help,float control_width,bool disabled=false,const char* note=nullptr) {
    const float right=right_edge();
    ImGui::AlignTextToFramePadding();
    if(disabled)ImGui::TextDisabled("%s",label);
    else ImGui::TextUnformatted(label);
    label_help(p,help,ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    if(note) {
        const auto max=ImGui::GetItemRectMax();
        badge(p,note,{max.x+8,(ImGui::GetItemRectMin().y+max.y)*.5F});
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),right-control_width));
}

struct Seg {const char* text;bool disabled{};Help help{};};
bool segmented(Panel& p,const char* label,const char* id,int& value,std::initializer_list<Seg> options,Help help,bool disabled=false,const char* note=nullptr) {
    constexpr float padding=12,minimum=52,gap=4;
    float total=-gap;
    std::array<float,8> widths{};
    int index=0;
    for(const auto& option:options) {
        widths[index]=std::max(minimum,text_width(face_semibold,p.m.body,option.text)+padding*2);
        total+=widths[index++]+gap;
    }
    row(p,label,help,total,disabled,note);
    ImGui::PushID(id);
    bool changed=false;
    index=0;
    for(const auto& option:options) {
        const bool selected=index==value,locked=disabled||option.disabled;
        if(index)ImGui::SameLine(0,gap);
        const auto fill=locked&&!selected ? IM_COL32_BLACK_TRANS : selected&&!locked ? p.c.accent : p.c.control;
        const auto text=selected&&!locked ? p.c.on_accent : locked&&!selected ? p.c.muted : p.c.ink;
        const auto over=locked ? fill : selected ? p.c.accent : p.c.hover;
        const auto edge=selected&&!locked ? p.c.accent : locked&&!selected ? IM_COL32_BLACK_TRANS : p.c.line;
        ImGui::PushStyleColor(ImGuiCol_Button,fill);ImGui::PushStyleColor(ImGuiCol_ButtonHovered,over);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,over);ImGui::PushStyleColor(ImGuiCol_Text,text);
        ImGui::PushStyleColor(ImGuiCol_Border,edge);
        ImGui::BeginDisabled(locked);
        {
            FaceScope scope(selected?face_semibold:face_sans,p.m.body);
            if(ImGui::Button(option.text,ImVec2(widths[index],p.m.control)) && !selected) {value=index;changed=true;}
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor(5);
        if(locked&&!selected)dashed_rect(ImGui::GetWindowDrawList(),ImGui::GetItemRectMin(),ImGui::GetItemRectMax(),p.c.line,p.m.border);
        hover_help(p,option.help.title?option.help:help);
        ++index;
    }
    ImGui::PopID();
    return changed;
}

// The control is the full row height, so the target is larger than what it shows.
bool switch_control(Panel& p,const char* id,bool& value,Help help,bool disabled=false) {
    ImGui::BeginDisabled(disabled);
    const bool pressed=ImGui::InvisibleButton(id,ImVec2(kSwitchWidth,p.m.control));
    ImGui::EndDisabled();
    const auto min=ImGui::GetItemRectMin();
    const float top=min.y+(p.m.control-kSwitchHeight)*.5F,radius=kSwitchHeight*.5F;
    const ImVec2 a{min.x,top},b{min.x+kSwitchWidth,top+kSwitchHeight};
    auto* list=ImGui::GetWindowDrawList();
    const bool on=value&&!disabled,hovered=ImGui::IsItemHovered()&&!disabled;
    list->AddRectFilled(a,b,disabled ? IM_COL32_BLACK_TRANS : on ? p.c.accent : hovered ? p.c.hover : p.c.control,radius);
    if(disabled)dashed_rect(list,a,b,p.c.line,p.m.border);
    else if(!on)list->AddRect(a,b,p.c.line,radius,0,p.m.border);
    list->AddCircleFilled({value ? b.x-radius : a.x+radius,top+radius},radius-5,on ? p.c.on_accent : p.c.muted);
    ImGui::RenderNavCursor(ImRect(a,b),ImGui::GetItemID());
    hover_help(p,help);
    if(pressed)value=!value;
    return pressed;
}
bool toggle_row(Panel& p,const char* label,const char* id,bool& value,Help help,bool disabled=false,const char* note=nullptr) {
    row(p,label,help,kSwitchWidth,disabled,note);
    return switch_control(p,id,value,help,disabled);
}
// Two switches on one line: each gets its own box, so neither reads as the other's.
bool toggle_box(Panel& p,const char* label,const char* id,bool& value,Help help,float width,bool disabled=false) {
    const auto position=ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(position,{position.x+width,position.y+p.m.control},p.c.raised,kCardRadius);
    ImGui::SetCursorScreenPos({position.x+12,position.y});
    ImGui::AlignTextToFramePadding();
    if(disabled)ImGui::TextDisabled("%s",label);
    else ImGui::TextUnformatted(label);
    label_help(p,help,ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
    ImGui::SameLine();
    ImGui::SetCursorScreenPos({position.x+width-12-kSwitchWidth,position.y});
    return switch_control(p,id,value,help,disabled);
}

struct Card {const char* name;const char* tag;bool warn{};Help help{};};
bool cards(Panel& p,const char* id,int& value,std::span<const Card> options) {
    constexpr float gap=4;
    const float count=float(options.size());
    const float width=(ImGui::GetContentRegionAvail().x-gap*(count-1))/count,inner=width-16;
    float tag_height=0;
    {
        FaceScope scope(face_sans,p.m.label);
        for(const auto& option:options)tag_height=std::max(tag_height,ImGui::CalcTextSize(option.tag,nullptr,false,inner).y);
    }
    const float name_height=p.m.body*1.25F,height=std::max(p.m.control+16,name_height+tag_height+16);
    ImGui::PushID(id);
    bool changed=false;
    int index=0;
    auto* list=ImGui::GetWindowDrawList();
    for(const auto& option:options) {
        if(index)ImGui::SameLine(0,gap);
        const bool selected=index==value;
        if(ImGui::InvisibleButton(option.name,ImVec2(width,height)) && !selected) {value=index;changed=true;}
        const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
        list->AddRectFilled(min,max,selected ? p.c.accent_soft : ImGui::IsItemHovered() ? p.c.hover : p.c.control,kCardRadius);
        list->AddRect(min,max,selected ? p.c.accent : p.c.line,kCardRadius,0,p.m.border);
        const float top=min.y+(height-name_height-tag_height)*.5F;
        {
            FaceScope scope(face_semibold,p.m.body);
            ImGui::RenderText({min.x+8,top},option.name);
        }
        {
            FaceScope scope(face_sans,p.m.label);
            ImGui::PushStyleColor(ImGuiCol_Text,option.warn ? p.c.warn : selected ? p.c.ink : p.c.muted);
            ImGui::RenderTextWrapped({min.x+8,top+name_height},option.tag,nullptr,inner);
            ImGui::PopStyleColor();
        }
        ImGui::RenderNavCursor(ImRect(min,max),ImGui::GetItemID());
        hover_help(p,option.help);
        ++index;
    }
    ImGui::PopID();
    return changed;
}

// ImGui keeps the input behaviour; the invisible frame is painted over with the
// thick track, the tall grab and the value on the right.
template<class Draw> bool slider_frame(Panel& p,const char* label,Help help,float position,const char* text,Draw&& draw) {
    const float width=p.compact?300.F:260.F,value_width=62;
    row(p,label,help,width);
    ImGui::SetNextItemWidth(width-value_width);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,IM_COL32_BLACK_TRANS);ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,IM_COL32_BLACK_TRANS);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,IM_COL32_BLACK_TRANS);ImGui::PushStyleColor(ImGuiCol_Border,IM_COL32_BLACK_TRANS);
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,IM_COL32_BLACK_TRANS);ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,IM_COL32_BLACK_TRANS);
    ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32_BLACK_TRANS);
    const bool changed=draw();
    ImGui::PopStyleColor(7);
    const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
    const float middle=(min.y+max.y)*.5F,track=10,grab=16;
    const float x=min.x+grab*.5F+std::clamp(position,0.F,1.F)*(max.x-min.x-grab);
    auto* list=ImGui::GetWindowDrawList();
    list->AddRectFilled({min.x,middle-track*.5F},{max.x,middle+track*.5F},p.c.control,track*.5F);
    list->AddRectFilled({min.x,middle-track*.5F},{x,middle+track*.5F},p.c.accent,track*.5F);
    list->AddRect({min.x,middle-track*.5F},{max.x,middle+track*.5F},p.c.line,track*.5F,0,p.m.border*.75F);
    list->AddRectFilled({x-grab*.5F,middle-14},{x+grab*.5F,middle+14},p.c.ink,4);
    ImGui::RenderNavCursor(ImRect(min,max),ImGui::GetItemID());
    hover_help(p,help);
    {
        FaceScope scope(face_mono,p.m.body*.9F);
        const auto size=ImGui::CalcTextSize(text);
        list->AddText({max.x+value_width-size.x,middle-size.y*.5F},p.c.ink,text);
    }
    return changed;
}
bool slider(Panel& p,const char* label,const char* id,float& value,float low,float high,const char* format,Help help) {
    char text[32];std::snprintf(text,sizeof(text),format,value);
    char hidden[80];std::snprintf(hidden,sizeof(hidden),"##%s",id);
    return slider_frame(p,label,help,(value-low)/(high-low),text,[&]{return ImGui::SliderFloat(hidden,&value,low,high,format);});
}
bool slider_int(Panel& p,const char* label,const char* id,int& value,int low,int high,const char* format,Help help) {
    char text[32];std::snprintf(text,sizeof(text),format,value);
    char hidden[80];std::snprintf(hidden,sizeof(hidden),"##%s",id);
    return slider_frame(p,label,help,float(value-low)/float(high-low),text,[&]{return ImGui::SliderInt(hidden,&value,low,high,format);});
}

// A closed row still states what is inside it. Call disclosure_end only when open.
bool disclosure(Panel& p,const char* title,const char* summary,Help help={},bool* bound=nullptr,bool open_by_default=false,std::optional<Icon> icon={}) {
    // Unset until clicked, so the default can follow a source that attaches later.
    auto* storage=ImGui::GetStateStorage();
    const auto key=ImGui::GetID(title);
    const int stored=storage->GetInt(key,-1);
    bool open=bound ? *bound : stored<0 ? open_by_default : stored!=0;
    if(ImGui::InvisibleButton(title,ImVec2(ImGui::GetContentRegionAvail().x,p.m.control))) {
        open=!open;
        storage->SetInt(key,open?1:0);
        if(bound)*bound=open;
    }
    const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
    auto* list=ImGui::GetWindowDrawList();
    list->AddRectFilled(min,max,ImGui::IsItemHovered() ? p.c.hover : p.c.raised,kCardRadius);
    const float middle=(min.y+max.y)*.5F;
    float x=min.x+10;
    draw_icon(list,{x,middle-9},18,p.c.muted,open ? Icon::chevron_down : Icon::chevron_right);
    x+=26;
    if(icon) {
        draw_icon(list,{x,middle-10},20,p.c.ink,*icon);
        x+=28;
    }
    ImGui::RenderText({x,middle-ImGui::GetFontSize()*.5F},title);
    ImGui::RenderNavCursor(ImRect(min,max),ImGui::GetItemID());
    hover_help(p,help);
    if(summary && *summary) {
        FaceScope scope(face_sans,p.m.label);
        const auto size=ImGui::CalcTextSize(summary);
        list->AddText({max.x-size.x-12,middle-size.y*.5F},p.c.muted,summary);
    }
    if(open)ImGui::Indent(12);
    return open;
}
void disclosure_end() {ImGui::Unindent(12);}

void title(const Panel& p,const char* text) {
    FaceScope scope(face_semibold,p.m.title);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
}
// Right-aligned state with its icon, on the current line. right is in window coordinates.
void state_pill(const Panel& p,Tone tone,const char* text,float right) {
    FaceScope scope(face_semibold,p.m.body);
    const auto size=ImGui::CalcTextSize(text);
    constexpr float icon=20;
    const float x=right-size.x-icon-6;
    ImGui::SameLine();
    ImGui::SetCursorPosX(x);
    const auto position=ImGui::GetCursorScreenPos();
    const float height=ImGui::GetFrameHeight();
    draw_icon(ImGui::GetWindowDrawList(),{position.x,position.y+(height-icon)*.5F},icon,tone_color(p,tone),tone_icon(tone));
    ImGui::SetCursorPosX(x+icon+6);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text,tone_color(p,tone));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// Help text, condensed from the notes the panel used to show permanently.
constexpr Help kHelpEnable{"Frame generation","Turns generated frames on for this profile. Reconstruction and Neural Rendering stay independent."};
constexpr Help kHelpMultiplier{"Multiplier","Frames shown per real frame. 3X and 4X need the private SteamVR route and a D3D12 game, Luke Ross included. Native VDXR and Pimax run D3D12 games up to 3X. Other sessions stay at 2X."};
constexpr Help kHelpQuality{"Flow quality","NVIDIA optical flow preset. Slower presets cost more GPU time."};
constexpr Help kHelpResolution{"Flow resolution","Changes motion estimation only; output stays at full resolution. At high eye resolutions, 25% reduces OFA work with a detail tradeoff."};
constexpr Help kHelpBidirectional{"Bidirectional flow","Estimates motion in both directions."};
constexpr Help kHelpMotion{"Motion source","Optical flow, or the game's own motion vectors."};
constexpr Help kHelpPacing{"Pacing","Framework bypasses director pacing. Director uses only the final director. Changes apply live after queued frames complete."};
constexpr Help kHelpPacingAdvanced{"Pacing","Framework bypasses director pacing. Director uses only the final director. Framework+director combines both. Changes apply live after queued frames complete."};
constexpr Help kHelpBuffer{"Increase buffer","SteamVR: an adaptive timing reserve near full refresh. Framework uses 3 deliveries instead of 2; Director uses 4 instead of 3. Independent of Fast mode. Other private runtimes change capacity only. Changes apply after queued frames complete."};
constexpr Help kHelpFast{"Fast mode","Lower VRAM and latency, may reduce performance. Uses private synthesis textures and fewer XR swapchains. No effect with Luke Ross. At 3X and 4X it keeps the output capacity they need, so the VRAM saving may be limited."};
constexpr Help kHelpRifeQuality{"Quality","Motion inference uses this preset; output stays at full resolution."};
constexpr Help kHelpRifeMultiplier{"Multiplier","3X and 4X require the private route. Luke Ross also requires the isolated post-AFW synthesis queue. Additional intermediates increase GPU work and memory use."};
constexpr Help kHelpRifeModel{"RIFE model","Installed compatible model weights."};
constexpr Help kHelpDepth{"Depth search","Uses the game's depth to keep thin near objects (bars, poles, wires) solid in generated frames. Costs GPU time at high resolutions."};
constexpr Help kHelpHud{"HUD fix","Keeps a HUD and cockpit instruments steady in generated frames, instead of smearing or doubling with the scene when you turn. The effect depends on the game: designed first for Star Citizen, and most useful at 3X and 4X. Uses additional GPU time."};
constexpr Help kHelpPreset{"DLSS-G preset","C uses Lite blending; A uses Full quality. If the driver rejects C, OFXR tries A. A saved request does not verify the model loaded by NVIDIA; global overrides may still apply."};
constexpr Help kHelpGenerated{"Generated resolution","Resolution of DLSS-G generated frames. Requires compatible NVIDIA DLSS-G hardware and runtime."};
constexpr Help kHelpMethod{"Reconstruction","Rebuilds the eye the game skipped this frame. Applies live. Off stops it for every game, Luke Ross and UEVR alike; frame generation and Neural Rendering stay independent."};
constexpr Help kHelpOsr{"OSR","OFXR Stereo Reconstruction. OFXR renders the missing eye itself from the real eye's colour and depth, the camera of both eyes, and that eye's own previous frame. Used with Luke Ross and UEVR."};
constexpr Help kHelpOfr{"AER framegen","Declares AER input and alternates real/generated eyes using the selected generator. 2X only. Requires history; source phase is assumed. Saved per game."};
constexpr Help kHelpOfrSwap{"Swap","Reverses which eye is assumed older. Does not exchange left and right images. Re-primes both histories."};
constexpr Help kHelpAfw{"AFW","Legacy. Reconstructs the missing eye with the AFW provider. Luke Ross only."};
constexpr Help kHelpOnly{"Frame Reconstruction only","Removes frame generation from the pipeline. Each application frame goes straight to the headset, and only the missing eye is reconstructed. The best choice for latency."};
constexpr Help kHelpOsrQuality{"OSR quality","Faster reduces reconstruction work and may soften object edges. Colour stays full resolution. Live."};
constexpr Help kHelpFramewarp{"Framewarp mode","The debug modes leave out one source of the reconstructed eye."};
constexpr Help kHelpPreviousFrame{"Use previous frame","For debugging only, not a quality setting. Off rebuilds the eye from the other eye alone. Leave on for normal play."};
constexpr Help kHelpDebug{"Debug framewarp","Tints previous-frame pixels green and inpainted pixels magenta. Requires compatible source colour, depth and camera data."};
constexpr Help kHelpShading{"AFW shading rate","Beta6: reduces Combined warping work; output resolution stays unchanged. A quality change rebuilds AFW history while the game stays open."};
constexpr Help kHelpIgnore{"Ignore motion","Threshold for ignoring per-object motion vectors."};
constexpr Help kHelpDilation{"Depth dilation","Reduces ghosting and flicker, with less precise object silhouettes. Off by default."};
constexpr Help kHelpNeural{"Neural Rendering","Runs NVIDIA's model on compatible DLSS frames."};
constexpr Help kHelpSecondPass{"2x NR","Runs Neural Rendering twice with the same settings and working resolution. Uses additional GPU time and VRAM."};
constexpr Help kHelpBeforeSr{"Before SR","Runs the model before super resolution."};
constexpr Help kHelpFps{"On-screen FPS counter","Shows the counter in the headset."};
constexpr Help kHelpCounter{"Counter type","Fixed corners, or Elliottate's meter on the controller or stuck to the bottom of your view."};
constexpr Help kHelpJson{"Export status.json","Provides live status to other apps or mods that need it, such as autoFPS for MSFS 2024. Keeps FPS and latency readings active when all panels are closed."};
constexpr Help kHelpLogging{"Diagnostic logging","Writes flight logs. Can be switched during the current game session."};
constexpr Help kHelpAuto{"Auto pick best one","On completion, keeps the best valid result instead of restoring your settings."};
constexpr Help kHelpSuite{"Automatic tests","7.5 seconds per configuration. Close the menu and keep the same test scene. Stop restores your settings."};
constexpr Help kHelpOutput{"Output FPS","SteamVR uses compositor presents. VDXR and Pimax OpenXR use successful content submissions to the native runtime. Empty or failed submissions are excluded. Native OpenXR acceptance does not measure physical headset scanout."};
constexpr Help kHelpLukeOutput{"New submitted FPS","Distinct new images accepted by the OpenXR runtime per second; held images and repeats excluded. This measures delivery to the runtime, not physical headset scanout. N/A only when source identity is unavailable."};
constexpr Help kHelpReal{"Real","Successfully submitted real frames. Luke OFXR counter preserved; AFW-only held source identities are excluded."};
constexpr Help kHelpGeneratedRate{"Generated","Successfully submitted generated frames with native GPU confirmation."};
constexpr Help kHelpHzSteam{"Display Hz","SteamVR physical display frequency."};
constexpr Help kHelpHzOpenXr{"Display Hz","OpenXR shortest observed predicted display period; may reflect runtime throttling."};

constexpr std::array<Card,5> kRoutes{{
    {"OFXR","Fast",false,{"OFXR","Fastest route. Optical flow only, no game vectors needed."}},
    {"Hybrid","Needs DLSS",false,{"Hybrid","Game motion vectors plus optical flow. If game guides are unavailable or rejected, it uses optical flow alone."}},
    {"FSR 3.1","Needs DLSS",false,{"FSR 3.1","Uses game motion vectors and depth, so the game must run DLSS. Slow."}},
    {"DLSS-G","Needs DLSS",false,{"DLSS-G","The slowest route. Uses game motion vectors and depth, and needs compatible NVIDIA hardware and runtime."}},
    {"RIFE","Experimental",false,{"RIFE","Experimental portable Python/CUDA, NVIDIA only. RIFE 4.4 processes both eyes together, with automatic TensorRT optimization. Requires the RIFE runtime installed in AppData."}}}};
constexpr Card kEngineFidelity{"FidelityFX","Low latency, medium compat",false,{"FidelityFX","Low latency, medium compatibility. Offers Flow resolution only."}};
constexpr Card kEngine021{"Nvidia 0.2.1","Low latency, low compat",false,{"Nvidia 0.2.1","Low latency, low compatibility."}};
constexpr Card kEngine0213{"Nvidia 0.2.13","Medium latency, high compat",false,{"Nvidia 0.2.13","Medium latency, high compatibility. Stops at 3X."}};
constexpr Card kEngine030{"Nvidia 0.3.0","Medium latency, medium compat",false,{"Nvidia 0.3.0","Medium latency, medium compatibility."}};

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

// The tray can learn a game while the panel is open, so this runs whether or
// not the preset's details are open.
void dlssg_preset_refresh(SettingsWidgetState& ui, const Snapshot& state, bool compact, bool force=false) {
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
    if (!ui.dlssg_profile_loaded || force) {
        resolve_target();
        refresh();
        ui.dlssg_target_refresh_at = ImGui::GetTime()+1.;
    } else if (!compact && ImGui::GetTime() >= ui.dlssg_target_refresh_at) {
        // Re-read the local target once per UI second; query NVAPI only if it changed.
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
}
void dlssg_preset(Panel& p) {
    using namespace xrfg::standalone;
    auto& ui=p.ui;
    dlssg_preset_refresh(ui,p.state,p.compact);
    int selected=ui.dlssg_model;
    if (segmented(p,"DLSS-G preset","DLSS-G preset",selected,{{"Game"},{"A Full"},{"C Lite"}},kHelpPreset,ui.dlssg_executable.empty(),"Next launch")) {
        const auto preset = selected == 1 ? DlssgPreset::a : selected == 2 ? DlssgPreset::c : DlssgPreset::game_default;
        std::wstring error;
        DlssgPreset saved{};
        if (write_dlssg_profile(ui.dlssg_executable, preset, error, &saved)) {
            const bool remembered = remember_dlssg_preset(configuration_path(),saved);
            dlssg_preset_refresh(ui,p.state,p.compact,true);
            ui.dlssg_profile_status = saved != preset
                ? "Preset C unavailable: Preset A saved and verified for the next launch."
                : "Saved and verified for the next launch. Restart the game if it is running.";
            if (!remembered) ui.dlssg_profile_status += " Could not remember the preset preference in ofxr-bridge.ini.";
        } else ui.dlssg_profile_status = utf8(error);
    }
}
void dlssg_preset_details(Panel& p) {
    auto& ui=p.ui;
    if(!disclosure(p,"Preset details",""))return;
    if (!ui.dlssg_executable.empty()) reason(p,("Game: "+utf8(ui.dlssg_executable.filename().wstring())).c_str());
    reason(p,ui.dlssg_profile_status.c_str());
    if (ImGui::Button("Refresh preset profile")) dlssg_preset_refresh(ui,p.state,p.compact,true);
    disclosure_end();
}

void generation_details(Panel& p) {
    const auto& state=p.state;
    if (!disclosure(p,"Details","")) return;
    if (state.runtime.shared_queue)
        reason(p,"Shared queue: AFW/capture and OFXR share a GPU queue. This may reduce performance.");
    const auto& telemetry = state.runtime.telemetry;
    char line[160];
    if (state.connected && telemetry.updated_ms && GetTickCount64()-telemetry.updated_ms<2000 && telemetry.cpu_valid)
        std::snprintf(line,sizeof(line),"OpenXR Begin-to-End interval: %.1f ms",telemetry.cpu_render_ms);
    else std::snprintf(line,sizeof(line),"OpenXR Begin-to-End interval: N/A");
    reason(p,line);
    std::snprintf(line,sizeof(line),"Requested %llu / applied %llu",
        static_cast<unsigned long long>(state.revision),
        static_cast<unsigned long long>(state.runtime.applied_revision));
    reason(p,line);
    if (!state.connected) {
        reason(p,"No active game report; upscaler inputs are not yet known.");
    } else {
        const auto& runtime=state.runtime;
        if (!runtime.upscaler_evaluations)
            reason(p,"Upscaler inputs: no native DLSS SR/RR evaluation observed.");
        else {
            const auto age=GetTickCount64()-runtime.upscaler_last_seen_ms;
            std::snprintf(line,sizeof(line),"Native DLSS SR/RR: %s. Motion vectors: %s. Depth: %s.",
                age<5000 ? "receiving inputs" : "last seen more than 5 seconds ago",
                runtime.upscaler_motion ? "present" : "missing",
                runtime.upscaler_depth ? "present" : "missing");
            reason(p,line);
        }
        std::snprintf(line,sizeof(line),"Owned guides: %llu published / %llu XR matches / %llu uses",
            static_cast<unsigned long long>(runtime.guide_publications),
            static_cast<unsigned long long>(runtime.guide_matches),
            static_cast<unsigned long long>(runtime.guide_uses));
        reason(p,line);
        reason(p,"Input detection does not prove a compatible stereo pair. Native ingress currently supports DLSS SR/RR; FSR/XeSS upscaler ingress is unavailable.");
        if (p.ui.dirty || runtime.applied_revision!=state.revision) reason(p,"Applying the requested generation settings...");
        else {
            const auto message=generation_status_text(runtime.generation_mode,runtime.motion_source,runtime.generation_state);
            reason(p,std::string(message).c_str());
        }
    }
    disclosure_end();
}

bool generation(Panel& p) {
    auto& ui=p.ui;
    auto& draft=ui.draft;
    const auto& state=p.state;
    bool changed=false;
    const float right=right_edge();
    title(p,"Frame generation");
    ImGui::SameLine();
    ImGui::SetCursorPosX(right-kSwitchWidth);
    changed|=switch_control(p,"Enable frame generation",draft.enabled,kHelpEnable);
    if(state.connected && state.runtime.pacing_mode==kAfwOnlyPacingMode) {
        notice(p,Tone::idle,"Frame Reconstruction only: direct OpenXR delivery, no frame generation. Disable it in Reconstruction and restart the game to restore OFXR frame generation. Neural Rendering remains available.");
        return changed;
    }

    if(draft.advanced_mode) {
        changed|=toggle_row(p,"AER framegen","AER framegen",draft.ofr_enabled,kHelpOfr);
        changed|=toggle_row(p,"Swap","AER Swap",draft.ofr_swap,kHelpOfrSwap,!draft.ofr_enabled);
    }
    if(draft.ofr_enabled) {
        notice(p,Tone::idle,"AER framegen: 2X only. Uses the selected generator, quality and pacing. OSR/AFW are bypassed while framegen is enabled.");
        if(state.connected) {
            const char* delivery=state.runtime.ofr_delivery==1?"Alternating eye releases":state.runtime.ofr_delivery==2?"Paired releases (phase assumed)":"Learning release cadence";
            reason(p,delivery);
        }
    }
    const bool no_vectors=state.connected && interface_vectors_text(state,GetTickCount64())=="None";
    auto routes=kRoutes;
    for(int i=1;i<4;++i)if(no_vectors) {routes[i].tag="No DLSS";routes[i].warn=true;}
    int selected_generation=generation_ui_index(draft.frame_generation);
    if(cards(p,"Frame generation",selected_generation,routes)) {
        select_generation(draft,generation_from_ui(selected_generation),draft.backend);
        changed=true;
    }

    const bool djules=draft.frame_generation==0 && draft.backend==3;
    const bool dlssg=draft.frame_generation==2;
    if(draft.frame_generation==4) {
        int multiplier=draft.ofr_enabled?0:draft.rife_multiplier-2;
        if(segmented(p,"Multiplier","RIFE multiplier",multiplier,{{"2X"},{"3X",draft.ofr_enabled},{"4X",draft.ofr_enabled}},kHelpRifeMultiplier)) {
            draft.rife_multiplier=multiplier+2;changed=true;
        }
    } else if(djules||dlssg) {
        // DLSS-G also offers 4X (multi-frame generation), saved as its
        // frame_multiplier; 0.2.13 stops at 3X.
        int multiplier=draft.ofr_enabled?0:dlssg && draft.frame_multiplier==4 ? 2 : draft.triple_frame_gen ? 1 : 0;
        const bool locked=draft.lower_vram && !draft.separate_synthesis;
        const bool picked=dlssg
            ? segmented(p,"Multiplier","Multiplier",multiplier,{{"2X"},{"3X",draft.ofr_enabled},{"4X",draft.ofr_enabled}},kHelpMultiplier,locked)
            : segmented(p,"Multiplier","Multiplier",multiplier,{{"2X"},{"3X",draft.ofr_enabled}},kHelpMultiplier,locked);
        if(picked) {
            draft.triple_frame_gen=multiplier>=1;
            if(dlssg)draft.frame_multiplier=multiplier==2 ? 4 : 2;
            changed=true;
        }
        if(dlssg && multiplier==2)
            reason(p,"4X needs a GPU with DLSS multi-frame generation and the private SteamVR route with a D3D12 game; otherwise it runs at the highest multiplier the session supports.");
    } else if(optical_multiplier_route(draft)) {
        // Saved per route: each OFXR engine and Hybrid keep their own.
        int multiplier=draft.ofr_enabled?0:std::clamp(draft.frame_multiplier,2,4)-2;
        if(segmented(p,"Multiplier","Multiplier",multiplier,{{"2X"},{"3X",draft.ofr_enabled},{"4X",draft.ofr_enabled}},kHelpMultiplier)) {
            draft.frame_multiplier=multiplier+2;changed=true;
        }
        if(!draft.ofr_enabled&&draft.frame_multiplier>2)
            reason(p,"Needs the private SteamVR route and a D3D12 game. VDXR and Pimax stop at 3X.");
    }

    bool show_flow=false,nvidia_flow=false;
    if(draft.frame_generation==0) {
        // UI order differs from persisted backend IDs: 0.2.13 is backend 3,
        // while 0.3.0 is backend 2. Keep each existing profile on its engine.
        constexpr int engine_order[]={0,1,3,2};
        constexpr std::array<Card,4> engines{kEngineFidelity,kEngine021,kEngine0213,kEngine030};
        int selected_engine=engine_order[std::clamp(draft.backend,0,3)];
        if(cards(p,"OFXR engine",selected_engine,engines)) {
            select_generation(draft,0,engine_order[selected_engine]);
            changed=true;
        }
        if(draft.advanced_mode)
            changed|=segmented(p,"Motion source","Motion source",draft.motion_vectors,{{"Optical flow"},{"Game motion vectors"}},kHelpMotion);
        if(draft.motion_vectors==0 || (state.runtime.luke_detected && draft.afw_mode!=0)) {
            // FidelityFX has no quality preset or second direction, only a resolution.
            show_flow=true;nvidia_flow=draft.backend!=0;
        }
    } else if(draft.frame_generation==4) {
        if(draft.advanced_mode) {
            const auto name=xrfg::rife_model_name(draft.rife_model);
            const float width=240;
            row(p,"RIFE model",kHelpRifeModel,width);
            ImGui::SetNextItemWidth(width);
            if(ImGui::BeginCombo("##RIFE model",name.c_str())) {
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
            hover_help(p,kHelpRifeModel);
        } else if(draft.rife_model!=3) {draft.rife_model=3;changed=true;}
        int resolution=xrfg::rife_resolution_index(draft.rife_resolution);
        if(segmented(p,"Quality","Quality",resolution,{{"Potato"},{"Low"},{"Medium"},{"High"},{"Very high"},{"Ultra"},{"Godlike"}},kHelpRifeQuality)) {
            draft.rife_resolution=xrfg::kRifeResolutions[resolution];changed=true;
        }
    } else if(draft.frame_generation==3) {
        // Hybrid's engine is its own (hybrid_backend), never the OFXR one.
        // 0.2.13 is left out: it is a pacing route as well as a flow.
        constexpr std::array<Card,3> engines{kEngineFidelity,kEngine021,kEngine030};
        changed|=cards(p,"Optical flow engine",draft.hybrid_backend,engines);
        show_flow=true;nvidia_flow=draft.hybrid_backend!=0;
        changed|=toggle_row(p,"Depth search","Depth search",draft.hybrid_depth_search,kHelpDepth);
        changed|=toggle_row(p,"HUD fix","HUD fix",draft.hybrid_hud_fix,kHelpHud);
    }

    if(state.runtime.luke_detected && draft.afw_mode!=0 &&
        (draft.motion_vectors!=0 || (draft.frame_generation>0 && draft.frame_generation<4)))
        notice(p,Tone::warn,"AFW reconstructs one eye without native guides for that eye. Post-AFW OFXR/Hybrid uses optical flow; FSR/DLSS-G requires a complete stereo guide pair.");
    if(dlssg) {
        dlssg_preset(p);
        changed|=slider_int(p,"Generated resolution","Generated resolution",draft.native_scale,25,100,"%d%%",kHelpGenerated);
        ui.scale_editing|=ImGui::IsItemActive();
    }

    const bool framework_only=state.connected && !pacing_comparison_runtime(state.runtime);
    group_title(p,"Delivery");
    int pacing=framework_only ? 0 : draft.pacing_mode;
    const bool picked_pacing=draft.advanced_mode
        ? segmented(p,"Pacing","Pacing",pacing,{{"Framework"},{"Director"},{"Framework+director"}},kHelpPacingAdvanced,framework_only)
        : segmented(p,"Pacing","Pacing",pacing,{{"Framework"},{"Director"}},kHelpPacing,framework_only);
    if(picked_pacing) {draft.pacing_mode=pacing;changed=true;}
    if(framework_only)reason(p,"VDXR transport uses Framework pacing. Change Pacing in the tray and restart the game to use director pacing.");
    const float half=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5F,second=ImGui::GetCursorPosX()+half+ImGui::GetStyle().ItemSpacing.x;
    changed|=toggle_box(p,"Increase buffer","Increase buffer",draft.increase_buffer,kHelpBuffer,half,framework_only);
    ImGui::SameLine(second);
    bool fast=draft.lower_vram!=0;
    if(toggle_box(p,"Fast mode","Fast mode",fast,kHelpFast,half,state.runtime.luke_detected)) {
        draft.lower_vram=draft.separate_synthesis=fast ? 1 : 0;
        changed=true;
    }
    if(state.runtime.luke_detected)reason(p,"Fast mode has no effect with Luke Ross.");
    if(state.connected && state.runtime.buffer_capacity) {
        char line[64];std::snprintf(line,sizeof(line),"Active bridge buffer: %u deliveries",state.runtime.buffer_capacity);
        reason(p,line);
    }

    // Disclosures come last, so opening one pushes no primary control away.
    if(show_flow) {
        constexpr const char* qualities[]{"Slow","Medium","Fast"};
        constexpr const char* resolutions[]{"100%","75%","50%","25%"};
        char summary[64];
        std::snprintf(summary,sizeof(summary),"%s%s%s%s",nvidia_flow ? qualities[std::clamp(draft.preset,0,2)] : "",nvidia_flow ? ", " : "",
            resolutions[std::clamp(draft.scale,0,3)],nvidia_flow&&draft.backward ? ", bidirectional" : "");
        if(disclosure(p,"Flow tuning",summary,kHelpResolution,nullptr,false,Icon::flow)) {
            if(nvidia_flow) {
                if(draft.advanced_mode)changed|=segmented(p,"Flow quality","Flow quality",draft.preset,{{"Slow"},{"Medium"},{"Fast"}},kHelpQuality);
                else changed|=segmented(p,"Flow quality","Flow quality",draft.preset,{{"Slow"},{"Medium"}},kHelpQuality);
            }
            changed|=segmented(p,"Flow resolution","Flow resolution",draft.scale,{{"100%"},{"75%"},{"50%"},{"25%"}},kHelpResolution);
            if(nvidia_flow && draft.advanced_mode)changed|=toggle_row(p,"Bidirectional flow","Bidirectional flow",draft.backward,kHelpBidirectional);
            disclosure_end();
        }
    }
    if(dlssg)dlssg_preset_details(p);
    generation_details(p);
    return changed;
}

// Setup notes for one source of eyes, open while that source is attached.
void reconstruction_setup(Panel& p,const char* title,bool attached,std::initializer_list<const char*> lines) {
    if(!disclosure(p,title,attached ? "In use" : "",{},nullptr,attached))return;
    for(const char* line:lines)reason(p,line);
    disclosure_end();
}

void session_swapchain_budget(Panel& p) {
    const auto& state=p.state;
    if (state.runtime.shared_queue)
        reason(p,"Shared queue: AFW/capture and OFXR share a GPU queue. This may reduce performance.");
    const auto& b=state.runtime.swapchains;
    if (!state.connected || !b.updated_ms || GetTickCount64()-b.updated_ms>3000) {
        reason(p,"Waiting for a current session inventory.");return;
    }
    char line[96];
    std::snprintf(line,sizeof(line),"Total swapchains: %u",b.total);reason(p,line);
    std::snprintf(line,sizeof(line),"OFXR: %u",b.current+b.synthetic+b.auxiliary_outputs+b.fps+b.meter+b.menu);reason(p,line);
    std::snprintf(line,sizeof(line),"Game: %u",b.application);reason(p,line);
    if(b.unassigned_native) {std::snprintf(line,sizeof(line),"Other / unclassified: %u",b.unassigned_native);reason(p,line);}
    if (const auto warning=swapchain_budget_warning(b);!warning.empty())
        notice(p,Tone::warn,std::string(warning).c_str());
    // Draw actual lines only. Empty/padded rows must not reserve a large
    // blank block before the ownership limitation text in the headset menu.
    std::string_view details(b.details,std::find(std::begin(b.details),std::end(b.details),'\0')-std::begin(b.details));
    while (!details.empty()) {
        const auto newline=details.find('\n');
        auto entry=details.substr(0,newline);
        const auto begin=entry.find_first_not_of(" \t\r");
        if (begin!=std::string_view::npos) {
            entry.remove_prefix(begin);
            entry=entry.substr(0,entry.find_last_not_of(" \t\r")+1);
            reason(p,std::string(entry).c_str());
        }
        if (newline==std::string_view::npos) break;
        details.remove_prefix(newline+1);
    }
    if (b.details_truncated) reason(p,"Additional app handles omitted from this panel.");
    if(b.unassigned_native)notice(p,Tone::warn,"Some native swapchains have no assigned owner.");
}

bool framewarp(Panel& p) {
    auto& draft=p.ui.draft;
    const auto& state=p.state;
    // Which source of eyes is attached and what reconstruction does with it,
    // as the headset meter reports them.
    const auto& r=state.runtime;
    const auto now=GetTickCount64();
    const bool fresh=r.heartbeat_ms && now>=r.heartbeat_ms && now-r.heartbeat_ms<2000;
    const bool osr=draft.luke_osr==2;
    const char* source=!state.connected ? "No active game" : r.luke_detected ? "Luke Ross detected" :
        r.uevr_osr ? "UEVR Alternating detected" : "None detected";
    const bool active=state.connected && fresh && draft.afw_mode!=0 && r.afw_running && !r.afw_flat_scene;
    const char* activity=draft.ofr_enabled&&draft.enabled ? "AER framegen" : draft.afw_mode==0 ? "Off" : active ? "Active" :
        state.connected&&fresh&&r.afw_flat_scene ? "2D scene (no stereo source)" : reconstruction_source(r)?"Waiting":"N/A";
    const float right=right_edge();
    title(p,"Reconstruction");state_pill(p,active?Tone::ok:Tone::idle,activity,right);
    row(p,"Source",{},text_width(face_sans,p.m.body,source));
    ImGui::AlignTextToFramePadding();ImGui::TextDisabled("%s",source);
    bool changed=false;
    int reconstruction=draft.afw_mode==0?0:osr?1:2;
    if(segmented(p,"Method","Reconstruction",reconstruction,{{"Off",false,kHelpMethod},{"OSR",false,kHelpOsr},{"AFW",false,kHelpAfw}},kHelpMethod)) {
        if(reconstruction==0)draft.afw_mode=0;
        else {draft.luke_osr=reconstruction==1?2:0;if(draft.afw_mode==0)draft.afw_mode=3;}
        changed=true;
    }
    if(draft.ofr_enabled&&draft.enabled)notice(p,Tone::idle,"AER framegen is enabled under Frame generation. OSR/AFW preferences are retained and resume when it is off.");
    if(draft.afw_mode!=0 && draft.luke_osr!=2 && r.uevr_osr)
        notice(p,Tone::warn,"UEVR is reconstructed by OSR only: select OSR to reconstruct its missing eye.");
    changed|=toggle_row(p,"Frame Reconstruction only","Frame Reconstruction only",draft.afw_only,kHelpOnly,false,"Restart game");
    if(state.connected && (state.runtime.pacing_mode==kAfwOnlyPacingMode)!=draft.afw_only)
        reason(p,"Frame Reconstruction only change saved. Restart the game to apply it.");
    if(draft.afw_mode!=0 && draft.luke_osr==2) {
        int quality=draft.luke_osr_lower_quality ? 1 : 0;
        if(segmented(p,"OSR quality","OSR quality",quality,{{"Normal"},{"Faster"}},kHelpOsrQuality)) {
            draft.luke_osr_lower_quality=quality!=0;changed=true;
        }
    }
    reconstruction_setup(p,"Setup: Luke Ross",r.luke_detected,{
        "In the mod menu, set the render mode to Legacy AER.",
        "Turn off Camera rotation compensation where offered. With it on, turns in vehicles happen in steps."});
    reconstruction_setup(p,"Setup: UEVR (Alternating / AFR)",r.uevr_osr,{
        "In UEVR, set the Rendering Method to Alternating/AFR and Synchronization to Very Late. The game must run on D3D12 with DLSS: OSR takes each eye's depth and motion vectors from DLSS. FSR and XeSS games are not supported yet.",
        "Ghosting Fix on (required): it gives each eye its own history. Without it the reconstructed eye ghosts everywhere.",
        "If UEVR's Ghosting status stays learning or reads failed closed, turn on Bootstrap Separate View States and restart the game; do not toggle it live. Some games need it, such as Hogwarts Legacy and Atomic Heart. Only some UEVR builds offer this option."});
    // Advanced is for tuning and debugging, shown only with AdvancedMode=1 in the
    // ini. There is no switch for that in the panel, on purpose.
    if(draft.afw_mode!=0 && draft.advanced_mode) {
        // Full OSR shows only the parameters it uses.
        const bool full_osr=draft.luke_osr==2;
        constexpr const char* afw_modes[]{"Alternate","Previous","Combined"};
        const char* summary=full_osr ? (draft.afw_mode==1 ? "Debug: other eye only" : "") : afw_modes[std::clamp(draft.afw_mode-1,0,2)];
        const bool was_open=draft.luke_advanced;
        if(disclosure(p,"Advanced",summary,{},&draft.luke_advanced)) {
            if(full_osr) {
                bool previous_frame=draft.afw_mode!=1;
                if(toggle_row(p,"Use previous frame (debug only)","Use previous frame",previous_frame,kHelpPreviousFrame)) {
                    draft.afw_mode=previous_frame ? 3 : 1;changed=true;
                }
                if(draft.afw_mode==1)notice(p,Tone::warn,"Debugging view: the eye is rebuilt from the other eye alone. Turn this back on for normal play.");
            } else {
                int selected=std::clamp(draft.afw_mode-1,0,2);
                if(segmented(p,"Framewarp mode","Framewarp mode",selected,{{"Alternate"},{"Previous"},{"Combined"}},kHelpFramewarp)) {
                    draft.afw_mode=selected+1;changed=true;
                }
            }
            if(draft.afw_mode==3 && !full_osr) {
                int selected=draft.afw_shading_rate==4 ? 1 : 0;
                if(segmented(p,"AFW shading rate","AFW shading rate",selected,{{"1x1"},{"2x2"}},kHelpShading)) {
                    draft.afw_shading_rate=selected==1 ? 4 : 0;changed=true;
                }
                if(state.connected && state.runtime.afw_shading_rate>=0 &&
                    state.runtime.afw_shading_rate!=draft.afw_shading_rate) {
                    char line[96];
                    std::snprintf(line,sizeof(line),"Quality change pending the next successful AFW evaluation. Current index: %d",state.runtime.afw_shading_rate);
                    reason(p,line);
                }
            }
            changed|=toggle_row(p,"Debug framewarp","Debug framewarp",draft.afw_debug,kHelpDebug);
            if(!full_osr) {
                if(draft.advanced_mode)changed|=toggle_row(p,"Clear before framewarp","Clear before framewarp",draft.afw_clear_before_warping,{"Clear before framewarp","Clears the output before warping."});
                changed|=slider(p,"Ignore motion","Ignore motion",draft.afw_ignore_motion_threshold,0,16,"%.2f",kHelpIgnore);
                changed|=segmented(p,"Depth dilation","Depth dilation",draft.afw_depth_dilate_mode,{{"Off"},{"Horizontal"},{"Square"}},kHelpDilation);
            }
            disclosure_end();
        }
        changed|=was_open!=draft.luke_advanced;
    }
    if(state.connected) {
        char line[96];
        std::snprintf(line,sizeof(line),"Ready frames: %llu / projections: %llu",
            static_cast<unsigned long long>(state.runtime.afw_ready_frames),
            static_cast<unsigned long long>(state.runtime.afw_projection_frames));
        reason(p,line);
    }
    return changed;
}

bool neural(Panel& p) {
    auto& ui=p.ui;
    auto& draft=ui.draft;
    const auto& state=p.state;
    bool changed=false;
    const float right=right_edge();
    title(p,"Neural Rendering");
    ImGui::SameLine();
    ImGui::SetCursorPosX(right-kSwitchWidth);
    changed|=switch_control(p,"Enable Neural Rendering",draft.nr_enabled,kHelpNeural);
    if(state.connected && state.runtime.vulkan_capture_suspended)
        notice(p,Tone::warn,"Vulkan Neural Rendering input capture is suspended.");
    if(draft.nr_enabled && state.connected && state.runtime.nr_model_missing)
        notice(p,Tone::danger,"nvngx_dlssnr.dll is missing. OFXR cannot include this NVIDIA file. "
            "Put it in OFXR's ofxr folder or next to the game, then restart the tray and the game.");
    else if(draft.nr_enabled && !state.runtime.nr_running)
        notice(p,Tone::idle,"Waiting for compatible DLSS frames and the Neural Rendering runtime");
    else if(state.runtime.nr_running)
        notice(p,Tone::ok,"Neural Rendering active");
    group_title(p,"Strength");
    changed|=slider(p,"Detail","Detail strength",draft.nr_transfer_strength,0,2,"%.2f",{"Detail strength","How much model detail is transferred."});
    changed|=slider(p,"Colour","Colour strength",draft.nr_colour_strength,0,1,"%.2f",{"Colour strength","How much model colour is transferred."});
    changed|=slider(p,"Intensity","Intensity",draft.nr_intensity,0,2,"%.2f",{"Intensity","Overall strength of the effect."});
    group_title(p,"Model");
    changed|=segmented(p,"Preset","Model preset",draft.nr_preset,{{"Default"},{"1"},{"2"},{"3"}},{"Model preset","Which NVIDIA model preset to run."});
    changed|=segmented(p,"Style","Style",draft.nr_style,{{"Standard"},{"Natural"},{"Cinematic"}},{"Style","Standard, Natural or Cinematic."});
    int scale=static_cast<int>(draft.nr_working_scale*100.F+.5F);
    if(slider_int(p,"Resolution","Model resolution",scale,25,100,"%d%%",{"Model resolution","Working resolution of the model."})) {
        draft.nr_working_scale=scale/100.F;
        changed=true;
    }
    ui.scale_editing|=ImGui::IsItemActive();
    const float half=(ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x)*.5F,second=ImGui::GetCursorPosX()+half+ImGui::GetStyle().ItemSpacing.x;
    changed|=toggle_box(p,"2x NR","2x NR",draft.nr_second_pass,kHelpSecondPass,half);
    ImGui::SameLine(second);
    changed|=toggle_box(p,"Before SR","Run before super resolution",draft.nr_before_sr,kHelpBeforeSr,half);
    if(disclosure(p,"Model options","")) {
        changed|=slider(p,"Local structure","Local structure",draft.nr_local_structure,0,2,"%.2f",{});
        changed|=slider(p,"Local tone","Local tone",draft.nr_local_tone,0,2,"%.2f",{});
        changed|=slider(p,"Skin structure","Skin structure",draft.nr_skin_structure,-1,2,"%.2f",{});
        changed|=toggle_row(p,"Auto skin mask","Auto skin mask",draft.nr_auto_mask,{});
        changed|=slider(p,"Paper white","Paper white",draft.nr_white_point_scale,.25F,4,"%.2fx",{});
        changed|=slider(p,"Luminance limit","Luminance limit",draft.nr_max_ratio,1,8,"%.2fx",{});
        disclosure_end();
    }
    if(disclosure(p,"Compare and inspect",draft.nr_compare||draft.nr_debug_view ? "On" : "Off")) {
        changed|=segmented(p,"Compare","Compare",draft.nr_compare,{{"Off"},{"Side by side"},{"Wipe"}},{});
        if(draft.nr_compare!=0) {
            changed|=toggle_row(p,"Swap sides","Swap sides",draft.nr_compare_swap,{});
            if(draft.nr_compare==1)
                changed|=slider(p,"Compare zoom","Compare zoom",draft.nr_compare_zoom,1,2,"%.2fx",{});
            else changed|=slider(p,"Wipe position","Wipe position",draft.nr_compare_split,0,1,"%.2f",{});
        }
        changed|=segmented(p,"Inspect","Inspect",draft.nr_debug_view,{{"Off"},{"Model input"},{"Raw output"},{"Difference"}},{});
        disclosure_end();
    }
    return changed;
}

// The meter is about six times the width of a corner counter, so it fills the
// tile's 44 x 32 screen: wide along the bottom of the view, or held above the
// controller that shows it.
void meter_tile(ImDrawList* list,const Palette& c,ImVec2 screen,bool on_controller) {
    const float width=on_controller ? 22.F : 28.F,height=on_controller ? 9.F : 11.F;
    const ImVec2 min{screen.x+(44-width)*.5F,screen.y+(on_controller ? 3.F : 15.F)};
    list->AddRectFilled(min,{min.x+width,min.y+height},c.accent,2);
    // Its four large numbers.
    const float column=(width-6)/4;
    for(int i=0;i<4;++i) {
        const float x=min.x+3+column*float(i)+column*.5F-1.5F;
        list->AddRectFilled({x,min.y+height*.5F-1.5F},{x+3,min.y+height*.5F+1.5F},c.surface);
    }
    if(!on_controller)return;
    const float middle=screen.x+22;
    // A VR controller from the side: tracking ring, then a slanted grip.
    list->AddCircle({middle-2,screen.y+20},4.5F,c.muted,16,2);
    list->AddQuadFilled({middle+.5F,screen.y+22},{middle+4.5F,screen.y+20.5F},{middle+7,screen.y+30},{middle+2.5F,screen.y+30},c.muted);
}

bool overlay(Panel& p) {
    auto& draft=p.ui.draft;
    bool changed=false;
    title(p,"Overlay");
    group_title(p,"Counter");
    changed|=toggle_row(p,"On-screen FPS counter","On-screen FPS counter",draft.overlay_enabled,kHelpFps);
    // The four corners: x and y as a share of the tile's small screen.
    // The last two types are the full meter, drawn by meter_tile().
    struct Position {const char* name;float x,y;};
    constexpr std::array<Position,6> positions{{{"Upper left",.08F,.12F},{"Upper right",.64F,.12F},{"Lower left",.08F,.66F},
        {"Lower right",.64F,.66F},{"On controller",0,0},{"Bottom edge",0,0}}};
    const float gap=4,width=(ImGui::GetContentRegionAvail().x-gap*5)/6,height=p.compact?92.F:80.F;
    auto* list=ImGui::GetWindowDrawList();
    ImGui::PushID("Counter type");
    for(int i=0;i<6;++i) {
        if(i)ImGui::SameLine(0,gap);
        const bool selected=draft.overlay_position==i+1;
        if(ImGui::InvisibleButton(positions[i].name,ImVec2(width,height)) && !selected) {draft.overlay_position=i+1;changed=true;}
        const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
        list->AddRectFilled(min,max,selected ? p.c.accent_soft : ImGui::IsItemHovered() ? p.c.hover : p.c.control,kCardRadius);
        list->AddRect(min,max,selected ? p.c.accent : p.c.line,kCardRadius,0,p.m.border);
        const ImVec2 map_min{(min.x+max.x)*.5F-22,min.y+8},map_max{map_min.x+44,map_min.y+32};
        list->AddRectFilled(map_min,map_max,p.c.surface,3);
        list->AddRect(map_min,map_max,p.c.line,3,0,p.m.border);
        if(i<4) {
            const ImVec2 mark{map_min.x+44*positions[i].x,map_min.y+32*positions[i].y};
            list->AddRectFilled(mark,{mark.x+12,mark.y+7},p.c.accent);
        } else meter_tile(list,p.c,map_min,i==4);
        {
            FaceScope scope(face_sans,p.m.label);
            const auto size=ImGui::CalcTextSize(positions[i].name,nullptr,false,width-8);
            ImGui::RenderTextWrapped({min.x+(width-std::min(size.x,width-8))*.5F,map_max.y+4},positions[i].name,nullptr,width-8);
        }
        ImGui::RenderNavCursor(ImRect(min,max),ImGui::GetItemID());
        hover_help(p,kHelpCounter);
    }
    ImGui::PopID();
    if(draft.advanced_mode && disclosure(p,"Experimental","")) {
        changed|=toggle_row(p,"Reduce black bars during headset rotation","R19 synthetic camera (experimental)",draft.r19_camera,{"Reduce black bars","Experimental. Reduces black bars during headset rotation."});
        disclosure_end();
    }
    return changed;
}

bool logging(Panel& p) {
    auto& draft=p.ui.draft;
    const auto& state=p.state;
    bool changed=false;
    title(p,"Logging");
    changed|=toggle_row(p,"Diagnostic logging","Diagnostic logging",draft.diagnostics,kHelpLogging);
    if(state.connected) reason(p,state.runtime.logging_active
        ? (draft.diagnostics ? "Logging active" : "Stopping logging...")
        : (draft.diagnostics ? "Starting logging..." : "Logging disabled"));
    changed|=toggle_row(p,"Export status.json","Export status.json",draft.status_json,kHelpJson);
    char total[32]{};
    if(state.connected && state.runtime.swapchains.updated_ms)
        std::snprintf(total,sizeof(total),"Total %u",state.runtime.swapchains.total);
    if(disclosure(p,"Swapchains",total)) {
        session_swapchain_budget(p);
        disclosure_end();
    }
    return changed;
}

bool suite(Panel& p,const char* name,const char* id,std::size_t count,bool disabled) {
    const auto position=ImGui::GetCursorScreenPos();
    const float width=ImGui::GetContentRegionAvail().x,height=p.m.control+16,button=84;
    ImGui::GetWindowDrawList()->AddRectFilled(position,{position.x+width,position.y+height},p.c.raised,kCardRadius);
    ImGui::SetCursorScreenPos({position.x+12,position.y+(height-p.m.body*1.25F-p.m.label*1.25F)*.5F});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,1));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(name);
    {
        const int minutes=std::max(1,int(std::ceil(double(count)*double(kBenchmarkDurationMs)/60000.0)));
        char detail[64];std::snprintf(detail,sizeof(detail),"Up to %zu tests, about %d min",count,minutes);
        FaceScope scope(face_sans,p.m.label);
        ImGui::TextDisabled("%s",detail);
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
    label_help(p,kHelpSuite,position,{position.x+width-button,position.y+height});
    ImGui::SetCursorScreenPos({position.x+width-button-8,position.y+8});
    ImGui::BeginDisabled(disabled);
    if(disabled) {
        ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32_BLACK_TRANS);
        ImGui::PushStyleColor(ImGuiCol_Text,p.c.muted);
    }
    const bool run=ImGui::Button(id,ImVec2(button,p.m.control));
    if(disabled)ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    hover_help(p,kHelpSuite);
    ImGui::SetCursorScreenPos({position.x,position.y+height});
    ImGui::Dummy(ImVec2(0,0));
    return run;
}

bool tests(Panel& p) {
    auto& ui=p.ui;
    auto& draft=ui.draft;
    const auto& state=p.state;
    bool changed=false;
    title(p,"Tests");
    const bool permitted=benchmark_enabled();
    const bool active=benchmark_active(state.benchmark);
    if(!permitted)notice(p,Tone::warn,"Benchmarks disabled. Enable benchmarks in the tray to enable sampling and these buttons.");
    else if(!state.connected)notice(p,Tone::idle,"Start a game to run tests.");
    if(active) {
        const auto& b=state.benchmark;
        const auto remaining=b.started_ms && GetTickCount64()>=b.started_ms
            ? std::max(0.0,double(kBenchmarkDurationMs)/1000.0-double(GetTickCount64()-b.started_ms)/1000.0)
            : double(kBenchmarkDurationMs)/1000.0;
        char line[192];
        std::snprintf(line,sizeof(line),"%u / %u: %s (%s, %.1f s)",b.index+1,b.count,b.label,
            b.phase==BenchmarkPhase::applying ? "applying" : "running",remaining);
        ImGui::TextUnformatted(line);
        const auto position=ImGui::GetCursorScreenPos();
        const float width=ImGui::GetContentRegionAvail().x,share=b.count ? float(b.index+1)/float(b.count) : 0;
        auto* list=ImGui::GetWindowDrawList();
        list->AddRectFilled(position,{position.x+width,position.y+10},p.c.control,5);
        list->AddRectFilled(position,{position.x+width*share,position.y+10},p.c.accent,5);
        ImGui::Dummy(ImVec2(0,10));
        if(ImGui::Button("Stop###Stop tests",ImVec2(84,p.m.control)))stop_benchmark();
    }
    if(state.benchmark.message[0])reason(p,state.benchmark.message);
    if(state.benchmark.csv_path[0]) reason(p,(std::string("CSV: ")+state.benchmark.csv_path).c_str());
    const bool modern=pacing_comparison_runtime(state.runtime);
    if(draft.ofr_enabled)notice(p,Tone::idle,"Turn AER framegen off before running the ordinary benchmark suites.");
    const bool locked=active||!permitted||!state.connected||draft.ofr_enabled;
    group_title(p,"Suites");
    const bool optical=suite(p,"Without vectors","Run###Tests without vectors",benchmark_plan(false,false,modern,draft.advanced_mode).size(),locked);
    const bool complete=suite(p,"Complete","Run###Complete tests",benchmark_plan(true,false,modern,draft.advanced_mode).size(),locked);
    const bool luke=suite(p,"Luke Ross","Run###Test Luke Ross",benchmark_plan(false,true,modern,draft.advanced_mode).size(),locked||!state.runtime.luke_detected);
    if(optical || complete || luke) {
        ui.dirty|=changed;finish_settings_widget_edits(ui);
        if(!ui.dirty) static_cast<void>(start_benchmark(complete,luke));
        synchronize_settings_widgets(ui,snapshot());changed=false;
    }
    changed|=toggle_row(p,"Auto pick best one","Auto pick best one",draft.benchmark_auto_pick,kHelpAuto,active);
    if(disclosure(p,"How tests work","")) {
        reason(p,"7.5 seconds per configuration: 2.5 seconds settling, then 5 seconds measured. CSV: min / median / max Output FPS and Cycle minimum. Works with diagnostic logging off. Neural and AFW choices are kept.");
        reason(p,modern ? "Framework and Director: Normal, Fast and Buffer. RIFE and vector routes also test Fast + Buffer."
            : "Legacy VDXR: Normal/Fast, using your selected RIFE Quality.");
        reason(p,"Luke Ross: OSR normal/cheap only, using your selected RIFE Quality.");
        reason(p,"RIFE uses one Quality selected before the suite: 2X, then 3X only if no preceding result holds full headset refresh. No Djules 3X or automatic 4X. TensorRT optimization completes before settling and measurement.");
        reason(p,"Suite length adjusts to measured results and session capabilities. Skipped configurations remain n.a. in CSV.");
        reason(p,"Close the menu and keep the same test scene. Stop restores original settings. Completion restores them unless Auto pick best one selects a valid result. Memory is sampled once near the end of each trial. Tests keep the startup DLSS-G preset and never change its NVIDIA profile. A is the initial preference; changing presets requires restarting the game.");
        disclosure_end();
    }
    return changed;
}

Tone status_tone(std::string_view text) {
    const auto has=[&](std::string_view part) {return text.find(part)!=std::string_view::npos;};
    if(has("failed")||has("rejected")||has("refused")||has("missing")||has("mismatch"))return Tone::danger;
    if(has(": Active")||has("detected")||has("TensorRT FP16"))return Tone::ok;
    if(has(": Off")||has(": Waiting")||has("No active game")||has("please wait"))return Tone::idle;
    return Tone::warn;
}

// One status line with its icon; long text wraps inside the pane.
void status_line(const Panel& p,Tone tone,std::string_view text) {
    const auto position=ImGui::GetCursorScreenPos();
    draw_icon(ImGui::GetWindowDrawList(),{position.x,position.y+2},16,tone_color(p,tone),tone_icon(tone));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+22);
    ImGui::TextUnformatted(text.data(),text.data()+text.size());
}

void value_cell(const Panel& p,const char* label,float number,bool valid,bool rate,float x,float y) {
    ImGui::SetCursorPos({x,y});
    {
        FaceScope scope(face_sans,p.m.label);
        ImGui::TextDisabled("%s",label);
    }
    ImGui::SetCursorPosX(x);
    char text[32]="N/A";
    const bool shown=valid && std::isfinite(number);
    if(shown)std::snprintf(text,sizeof(text),rate ? "%.0f" : "%.1f",number);
    {
        FaceScope scope(face_mono,p.m.value);
        ImGui::TextUnformatted(text);
    }
    if(shown && !rate) {
        ImGui::SameLine(0,2);
        FaceScope scope(face_sans,p.m.label);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY()+(p.m.value-p.m.label)*.6F);
        ImGui::TextDisabled("ms");
    }
}

// The value keeps the shared text, such as "81%" or "N/A".
void resource_bar(const Panel& p,const char* label,const std::string& text,double percent,ImVec2 position,float width) {
    const auto space=text.find(' ');
    const auto value=space==std::string::npos ? text : text.substr(space+1);
    FaceScope scope(face_sans,p.m.label);
    const float label_width=48,value_width=48,height=p.m.label*1.3F;
    ImGui::PushStyleColor(ImGuiCol_Text,p.c.muted);
    ImGui::RenderText(position,label);
    ImGui::PopStyleColor();
    const auto size=ImGui::CalcTextSize(value.c_str());
    ImGui::RenderText({position.x+width-size.x,position.y},value.c_str());
    auto* list=ImGui::GetWindowDrawList();
    const ImVec2 a{position.x+label_width,position.y+height*.5F-5},b{position.x+width-value_width-6,position.y+height*.5F+5};
    list->AddRectFilled(a,b,p.c.control,5);
    if(value!="N/A")list->AddRectFilled(a,{a.x+float((b.x-a.x)*std::clamp(percent,0.0,100.0)/100.0),b.y},p.c.accent,5);
}

void status_pane(Panel& p) {
    auto& ui=p.ui;
    const auto& state=p.state;
    const auto& m=state.runtime.telemetry;
    const auto now=GetTickCount64();
    const bool current=state.connected && m.updated_ms && now>=m.updated_ms && now-m.updated_ms<2000;
    const float left=kPad,width=kStatusWidth-kPad*2,cell=width*.5F,label=p.m.label*1.3F;
    const auto base=ImGui::GetWindowPos();
    float y=12;
    ImGui::SetCursorPos({left,y});
    {
        FaceScope scope(face_sans,p.m.label);
        ImGui::TextDisabled("%s",state.runtime.luke_detected ? "New submitted FPS" : "Output FPS");
    }
    ImGui::SetCursorPosX(left);
    {
        FaceScope scope(face_mono,p.m.metric);
        char text[32]="N/A";
        if(current && output_rate_valid(m) && std::isfinite(m.output_fps))std::snprintf(text,sizeof(text),"%.0f",m.output_fps);
        ImGui::TextUnformatted(text);
    }
    const float hero=label+p.m.metric*1.2F+8;
    label_help(p,state.runtime.luke_detected ? kHelpLukeOutput : kHelpOutput,{base.x+left,base.y+y},{base.x+left+width,base.y+y+hero});
    y+=hero;
    const float row_height=label+p.m.value*1.3F+8;
    const auto cell_help=[&](float x,float top,Help help) {
        label_help(p,help,{base.x+x,base.y+top},{base.x+x+cell,base.y+top+row_height});
    };
    value_cell(p,"Real",m.game_fps,current && m.rates_valid,true,left,y);cell_help(left,y,kHelpReal);
    value_cell(p,"Generated",m.generated_fps,current && m.rates_valid,true,left+cell,y);cell_help(left+cell,y,kHelpGeneratedRate);
    y+=row_height;
    value_cell(p,"Display Hz",m.display_hz,current && m.display_hz>0,true,left,y);
    cell_help(left,y,m.display_source==InterfaceSource::steamvr_display ? kHelpHzSteam : kHelpHzOpenXr);
    value_cell(p,"Cycle min",m.cycle_estimated_ms,current && m.cycle_estimated_valid,false,left+cell,y);
    {
        const auto format=[](char* out,std::size_t size,bool valid,float value) {
            if(valid && std::isfinite(value))std::snprintf(out,size,"%.1f ms",value);
            else std::snprintf(out,size,"N/A");
        };
        char ofxr[16],cycle[16];
        format(ofxr,sizeof(ofxr),current && m.ofxr_time_valid,m.ofxr_time_ms);
        format(cycle,sizeof(cycle),current && m.cycle_valid,m.cycle_length_ms);
        std::snprintf(ui.help_buffer,sizeof(ui.help_buffer),
            "WaitFrame entry through observed real runtime End completion, bounded below by one physical refresh per confirmed generated output. OFXR time %s (CPU wall time inside the intercepted OpenXR calls). Cycle length %s (WaitFrame entry through completion of the same real frame).",ofxr,cycle);
        cell_help(left+cell,y,{"Cycle minimum",ui.help_buffer});
    }
    y+=row_height;
    const auto& memory=state.runtime.video_memory;
    const double vram_percent=memory.budget_bytes ? 100.*double(memory.used_bytes)/double(memory.budget_bytes) : 0;
    resource_bar(p,"GPU",interface_gpu_text(state,now),state.runtime.gpu_usage.percent,{base.x+left,base.y+y},width);
    resource_bar(p,"CPU",interface_cpu_text(state,now),state.runtime.cpu_usage.percent,{base.x+left,base.y+y+label+4},width);
    resource_bar(p,"VRAM",interface_vram_text(state),vram_percent,{base.x+left,base.y+y+(label+4)*2},width);
    constexpr Help resources{"GPU, CPU and VRAM","CPU: total system processor time across all processes and logical processors, sampled about once per second. GPU: busiest physical engine on the rendering adapter, across all processes, sampled about once per second. VRAM: dedicated memory against the DXGI budget, shared imports counted once."};
    label_help(p,resources,{base.x+left,base.y+y},{base.x+left+width,base.y+y+label*3+8});
    y+=label*3+20;
    ImGui::GetWindowDrawList()->AddLine({base.x,base.y+y},{base.x+kStatusWidth,base.y+y},p.c.soft,p.m.border);
    y+=12;

    ImGui::Indent(left);
    ImGui::SetCursorPosY(y);
    FaceScope scope(face_sans,p.m.label);
    ImGui::PushTextWrapPos(kStatusWidth-kPad);
    const auto heading=[&](const char* text) {
        FaceScope semibold(face_semibold,p.m.body);
        ImGui::TextUnformatted(text);
    };
    if(ui.help_title) {
        const auto position=ImGui::GetCursorScreenPos();
        draw_icon(ImGui::GetWindowDrawList(),{position.x,position.y+1},18,p.c.ink,Icon::help);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+24);
        heading(ui.help_title);
        ImGui::TextUnformatted(ui.help_body);
    } else {
        heading("Status");
        // Problems first, because a long list clips at the bottom of the pane.
        std::vector<std::pair<Tone,std::string>> lines;
        const auto vectors=interface_vectors_text(state,now);
        lines.push_back({vectors=="DLSS" ? Tone::ok : Tone::idle,"Vectors: "+std::string(vectors)});
        const auto reconstruction=interface_reconstruction_text(state,now);
        lines.push_back({reconstruction.ends_with(": Off") ? Tone::idle : Tone::ok,std::string(reconstruction)});
        const auto explanation=interface_status_text(state,false);
        std::string_view rest=explanation;
        while(!rest.empty()) {
            const auto split=rest.find(" | ");
            const auto part=rest.substr(0,split);
            lines.push_back({status_tone(part),std::string(part)});
            if(split==std::string_view::npos)break;
            rest.remove_prefix(split+3);
        }
        std::stable_sort(lines.begin(),lines.end(),[](const auto& a,const auto& b) {
            const auto rank=[](Tone tone) {return tone==Tone::danger ? 0 : tone==Tone::warn ? 1 : tone==Tone::ok ? 2 : 3;};
            return rank(a.first)<rank(b.first);
        });
        for(const auto& line:lines)status_line(p,line.first,line.second);
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(left);
}

struct Section {const char* name;const char* child;Icon icon;};
constexpr std::array<Section,6> kSections{{
    {"Frame generation","Generation",Icon::frames},{"Reconstruction","Framewarp",Icon::eyes},{"Neural Rendering","Neural",Icon::neural},
    {"Overlay","Overlay",Icon::gauge},{"Logging","Logging",Icon::log},{"Tests","Tests",Icon::stopwatch}}};

// Each entry states the section's current value, so the pipeline reads at a glance.
std::string section_summary(const Panel& p,int index,bool& live,bool& alert) {
    const auto& draft=p.ui.draft;
    const auto& state=p.state;
    live=false;
    alert=false;
    switch(index) {
    case 0: {

        live=interface_generation_active(state);
        if(!draft.enabled)return "Off";
        constexpr const char* names[]{"OFXR","FSR 3.1","DLSS-G","Hybrid","RIFE"};
        std::string text=names[std::clamp(draft.frame_generation,0,4)];
        if(draft.frame_generation!=1)text+=" "+std::to_string(generation_multiplier(draft))+"X";
        if(draft.ofr_enabled)text+=" / AER";
        return text;
    }
    case 1: {

        live=state.connected && draft.afw_mode!=0 && state.runtime.afw_running && !state.runtime.afw_flat_scene;
        if(draft.afw_mode==0)return "Off";
        std::string text=draft.luke_osr==2 ? "OSR" : "AFW";
        if(draft.afw_only)text+=" only";
        return text;
    }
    case 2:
        live=state.connected && draft.nr_enabled && state.runtime.nr_running;
        alert=state.connected && draft.nr_enabled && state.runtime.nr_model_missing;
        return alert ? "Model missing" : draft.nr_enabled ? "On" : "Off";
    case 3: return draft.overlay_enabled ? "Counter on" : "Counter off";
    case 4:
        live=state.connected && state.runtime.logging_active;
        return draft.diagnostics ? "On" : "Off";
    default:
        return benchmark_active(state.benchmark) ? "Running" : benchmark_enabled() ? "Ready" : "Off in tray";
    }
}

void rail(Panel& p,float width) {
    auto& ui=p.ui;
    const bool narrow=width<kRailWidth;
    const float pad=8,height=p.compact?60.F:54.F,item=width-pad*2;
    auto* list=ImGui::GetWindowDrawList();
    ImGui::SetCursorPosY(pad);
    for(int i=0;i<int(kSections.size());++i) {
        ImGui::SetCursorPosX(pad);
        const auto& section=kSections[i];
        const bool selected=ui.section==i;
        if(ImGui::InvisibleButton(section.name,ImVec2(item,height)))ui.section=i;
        bool live=false,alert=false;
        const auto summary=section_summary(p,i,live,alert);
        const auto min=ImGui::GetItemRectMin(),max=ImGui::GetItemRectMax();
        if(selected) {
            list->AddRectFilled(min,max,p.c.accent_soft,kCardRadius);
            list->AddRect(min,max,p.c.accent,kCardRadius,0,p.m.border);
        } else if(ImGui::IsItemHovered())list->AddRectFilled(min,max,p.c.hover,kCardRadius);
        constexpr float icon=24;
        draw_icon(list,{narrow ? min.x+(item-icon)*.5F : min.x+10,min.y+(height-icon)*.5F},icon,alert ? p.c.danger : p.c.ink,section.icon);
        if(!narrow) {
            const float line=p.m.label*1.25F,x=min.x+44,top=min.y+(height-line*2)*.5F;
            {
                FaceScope scope(face_semibold,p.m.label);
                list->AddText({x,top},p.c.ink,section.name);
            }
            FaceScope scope(face_sans,p.m.label);
            list->AddText({x,top+line},alert ? p.c.danger : live ? p.c.ok : selected ? p.c.ink : p.c.muted,summary.c_str());
        }
        ImGui::RenderNavCursor(ImRect(min,max),ImGui::GetItemID());
    }
}

Tone state_tone(const Snapshot& state,std::string_view text) {
    if(state.desired.enabled && state.desired.frame_generation==4 && state.runtime.rife_error)return Tone::danger;
    if(interface_generation_active(state) || interface_afw_only_active(state,GetTickCount64()))return Tone::ok;
    return text=="Fallback" || text=="Applying settings" ? Tone::warn : Tone::idle;
}

void header(Panel& p,SettingsWidgetOptions& options,std::optional<Snapshot>& selected,const Snapshot& state) {
    auto& ui=p.ui;
    const float height=ImGui::GetWindowHeight();
    const float line=ImGui::GetFrameHeight();
    ImGui::SetCursorPos({kPad,(height-line)*.5F});
    {
        FaceScope scope(face_semibold,p.m.body);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("OFXR");
    }
    ImGui::SameLine();
    {
        FaceScope scope(face_sans,p.m.label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("0.3.3 (v%d)",XRFG_IMPLEMENTATION_VERSION);
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX()+8,kRailWidth+kPad));
    if(!p.compact && options.desktop_profile_header) {
        if(options.desktop_profile_header(ui)) {
            selected=snapshot();synchronize_settings_widgets(ui,*selected);
        }
    } else {
        const auto profile=configuration_path();
        const auto text=profile==global_configuration_path() ? std::string("Global defaults") : utf8(profile.filename().wstring());
        const auto position=ImGui::GetCursorScreenPos();
        draw_icon(ImGui::GetWindowDrawList(),{position.x,position.y+(line-20)*.5F},20,p.c.ink,Icon::gamepad);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()+28);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text.c_str());
    }
    const auto text=interface_state_text(state);
    const std::string label(text);
    state_pill(p,state_tone(state,text),label.c_str(),ImGui::GetWindowWidth()-kPad);
}
} // namespace

void synchronize_settings_widgets(SettingsWidgetState& ui, const Snapshot& state) noexcept {
    const auto file=configuration_path();
    if(ui.configuration_file!=file) {
        const int section=ui.section;
        ui=SettingsWidgetState{};
        ui.configuration_file=file;
        ui.section=section;
    }
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

bool render_reset_defaults(SettingsWidgetState& ui) {
    constexpr double confirm_seconds=4;
    const auto now=ImGui::GetTime();
    const bool armed=ui.reset_armed_until>now;
    const ImVec2 size{ImGui::CalcTextSize("Click again to reset").x+ImGui::GetStyle().FramePadding.x*2,0};
    if(armed) {
        ImGui::PushStyleColor(ImGuiCol_Text,kHeadsetPalette.danger);
        ImGui::PushStyleColor(ImGuiCol_Button,kHeadsetPalette.danger_soft);
    }
    const bool pressed=ImGui::Button(armed ? "Click again to reset###Reset defaults" : "Reset defaults###Reset defaults",size);
    if(armed)ImGui::PopStyleColor(2);
    if(!pressed)return false;
    if(!armed) {ui.reset_armed_until=now+confirm_seconds;return false;}
    ui.reset_armed_until=0;
    if(reset_to_defaults()) {
        ui.dirty=false;ui.scale_editing=false;ui.dlssg_profile_loaded=false;
        synchronize_settings_widgets(ui,snapshot());
        return true;
    }
    ui.save_failed=true;
    return false;
}

void show_settings_help(SettingsWidgetState& ui,const char* title,const char* body) noexcept {
    ui.footer_help_title=title;ui.footer_help_body=body;
}

bool render_settings_widgets(SettingsWidgetState& ui, const Snapshot& initial,
    SettingsWidgetOptions options) {
    set_interface_visible(options.compact ? InterfaceSurface::headset : InterfaceSurface::desktop, true);
    synchronize_settings_widgets(ui, initial);
    ui.scale_editing = false;
    ui.help_title=ui.footer_help_title;ui.help_body=ui.footer_help_body;
    ui.footer_help_title=ui.footer_help_body=nullptr;
    Panel panel{ui,initial,options.compact,options.compact?kHeadsetPalette:kDesktopPalette,options.compact?kHeadsetMetrics:kDesktopMetrics};
    const auto& m=panel.m;
    const auto& c=panel.c;
    ImGui::PushID("OFXRBridgeSettings");
    FaceScope body(face_sans,m.body);
    const float font=ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(12,std::max(2.F,(m.control-font)*.5F)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(8,m.gap));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,m.border);
    constexpr auto fixed=ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse;
    const auto origin=ImGui::GetCursorScreenPos();
    const auto region=ImGui::GetContentRegionAvail();
    const float footer=options.compact?0:kFooterHeight;
    const float content=std::max(120.F,region.y-kHeaderHeight-footer);

    std::optional<Snapshot> selected;
    ImGui::BeginChild("Header",ImVec2(region.x,kHeaderHeight),ImGuiChildFlags_None,fixed);
    header(panel,options,selected,initial);
    ImGui::EndChild();
    const auto& state=selected?*selected:initial;
    Panel current{ui,state,options.compact,c,m};

    bool changed{};
    const float rail_width=region.x<kNarrowWindow ? kRailNarrow : kRailWidth;
    const float main_width=std::max(120.F,region.x-rail_width-kStatusWidth);
    ImGui::SetCursorScreenPos({origin.x,origin.y+kHeaderHeight});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,c.raised);
    ImGui::BeginChild("Rail",ImVec2(rail_width,content),ImGuiChildFlags_None,fixed);
    ImGui::PopStyleColor();
    rail(current,rail_width);
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({origin.x+rail_width,origin.y+kHeaderHeight});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(kPad,12));
    ui.section=std::clamp(ui.section,0,int(kSections.size())-1);
    ImGui::BeginChild(kSections[ui.section].child,ImVec2(main_width,content),ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    if(const auto warning=overlay_limit_text(state.runtime.overlay_limit_mask);!warning.empty())
        notice(current,Tone::warn,std::string(warning).c_str());
    if(ui.save_failed)notice(current,Tone::danger,"Could not save settings. Check access to the local profile.");
    switch(ui.section) {
    case 0: changed|=generation(current);break;
    case 1: changed|=framewarp(current);break;
    case 2: changed|=neural(current);break;
    case 3: changed|=overlay(current);break;
    case 4: changed|=logging(current);break;
    default: changed|=tests(current);break;
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos({origin.x+rail_width+main_width,origin.y+kHeaderHeight});
    ImGui::PushStyleColor(ImGuiCol_ChildBg,c.raised);
    ImGui::BeginChild("Status",ImVec2(kStatusWidth,content),ImGuiChildFlags_None,fixed);
    ImGui::PopStyleColor();
    status_pane(current);
    ImGui::EndChild();

    auto* list=ImGui::GetWindowDrawList();
    const float top=origin.y+kHeaderHeight,bottom=top+content,divider=m.border;
    list->AddLine({origin.x,top},{origin.x+region.x,top},c.soft,divider);
    list->AddLine({origin.x+rail_width,top},{origin.x+rail_width,bottom},c.soft,divider);
    list->AddLine({origin.x+rail_width+main_width,top},{origin.x+rail_width+main_width,bottom},c.soft,divider);
    ui.dirty |= changed;
    if(footer>0) {
        list->AddLine({origin.x,bottom},{origin.x+region.x,bottom},c.soft,divider);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(10,5));
        FaceScope scope(face_sans,m.label);
        const float width=ImGui::CalcTextSize("Click again to reset").x+20;
        ImGui::SetCursorScreenPos({origin.x+12,bottom+6});
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Changes apply as you make them");
        ImGui::SetCursorScreenPos({origin.x+region.x-width-12,bottom+6});
        render_reset_defaults(ui);
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    return !options.defer_publication && publish_settings_widgets(ui);
}

} // namespace xrfg::autonomous
