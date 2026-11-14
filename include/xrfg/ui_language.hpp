#pragma once
#include <string>
#include <string_view>
namespace xrfg::autonomous {
enum class UiLanguage { english, simplified_chinese };
// UI preference is global, separate from game profiles and the shared settings ABI.
UiLanguage ui_language() noexcept;
void refresh_ui_language(bool force=false) noexcept;
bool set_ui_language(UiLanguage language) noexcept;
const char* ui_translate(const char* english) noexcept;
std::string ui_translate_text(std::string_view english);
std::wstring ui_translate_wide(std::wstring_view english);
}
