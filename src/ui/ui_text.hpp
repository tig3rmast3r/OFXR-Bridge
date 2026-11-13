#pragma once
#include "xrfg/ui_language.hpp"
#include "imgui.h"
#include <cstring>
#include <cstdio>
#include "imgui_internal.h"
namespace xrfg::ui {
inline std::string text(const char* value,const char* end=nullptr) {
    return autonomous::ui_translate_text(value?std::string_view(value,end?std::size_t(end-value):std::char_traits<char>::length(value)):std::string_view{});
}
// A canonical English suffix keeps focus/active IDs stable across language changes.
inline std::string label(const char* value) {
    if(!value)return {};
    if(value[0]=='#' && value[1]=='#')return value;
    const auto* suffix=strstr(value,"###");
    if(suffix) {
        const std::string visible(value,suffix);
        return std::string(autonomous::ui_translate(visible.c_str()))+suffix;
    }
    return std::string(autonomous::ui_translate(value))+"###"+value;
}
inline void TextUnformatted(const char* value,const char* end=nullptr){const auto s=text(value,end);ImGui::TextUnformatted(s.c_str());}
template<class... A> std::string formatted(const char* format,A... args) {
    const auto translated=autonomous::ui_translate(format);
    const auto count=std::snprintf(nullptr,0,translated,args...);
    if(count<0)return translated;
    std::string buffer(std::size_t(count)+1,'\0');std::snprintf(buffer.data(),buffer.size(),translated,args...);buffer.resize(count);
    return autonomous::ui_translate_text(buffer);
}
template<class... A> void TextDisabled(const char* format,A... args){
    if(autonomous::ui_language()==autonomous::UiLanguage::english){ImGui::TextDisabled(format,args...);return;}
    const auto s=formatted(format,args...);ImGui::TextDisabled("%s",s.c_str());
}
template<class... A> void Text(const char* format,A... args){
    if(autonomous::ui_language()==autonomous::UiLanguage::english){ImGui::Text(format,args...);return;}
    const auto s=formatted(format,args...);ImGui::TextUnformatted(s.c_str());
}
inline ImVec2 CalcTextSize(const char* value,const char* end=nullptr,bool hide=false,float wrap=-1.F){const auto s=text(value,end);return ImGui::CalcTextSize(s.c_str(),nullptr,hide,wrap);}
inline void RenderText(ImVec2 pos,const char* value,const char* end=nullptr,bool hide=true){const auto s=text(value,end);ImGui::RenderText(pos,s.c_str(),nullptr,hide);}
inline void RenderTextWrapped(ImVec2 pos,const char* value,const char* end,float wrap){const auto s=text(value,end);ImGui::RenderTextWrapped(pos,s.c_str(),nullptr,wrap);}
inline bool Button(const char* value,ImVec2 size={}){const auto s=label(value);return ImGui::Button(s.c_str(),size);}
inline bool InvisibleButton(const char* value,ImVec2 size,ImGuiButtonFlags flags=0){const auto s=label(value);return ImGui::InvisibleButton(s.c_str(),size,flags);}
inline bool Selectable(const char* value,bool selected=false,ImGuiSelectableFlags flags=0,ImVec2 size={}){const auto s=label(value);return ImGui::Selectable(s.c_str(),selected,flags,size);}
inline bool BeginCombo(const char* value,const char* preview,ImGuiComboFlags flags=0){const auto s=label(value);const std::string p=autonomous::ui_translate(preview);return ImGui::BeginCombo(s.c_str(),p.c_str(),flags);}
inline void AddText(ImDrawList* list,ImVec2 pos,ImU32 color,const char* value,const char* end=nullptr){const auto s=text(value,end);list->AddText(pos,color,s.c_str());}
inline void AddText(ImDrawList* list,ImFont* font,float size,ImVec2 pos,ImU32 color,const char* value,const char* end=nullptr,float wrap=0,const ImVec4* clip=nullptr){const auto s=text(value,end);list->AddText(font,size,pos,color,s.c_str(),nullptr,wrap,clip);}
}
