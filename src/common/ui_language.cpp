#include "xrfg/ui_language.hpp"
#include "xrfg/autonomous_control.hpp"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
namespace xrfg::autonomous {
namespace {
struct Translation {const char* english;const char* chinese;};
#include "ui_translations.inc"
std::atomic<UiLanguage> language{UiLanguage::english};
std::atomic<ULONGLONG> next_read{};
std::mutex preference_mutex;
auto preference_file(){return global_configuration_path().parent_path()/L"interface.ini";}
}
UiLanguage ui_language() noexcept {return language.load(std::memory_order_relaxed);}
void refresh_ui_language(bool force) noexcept {
    const auto now=GetTickCount64();
    if(!force && now<next_read.load(std::memory_order_relaxed))return;
    try {
        std::scoped_lock lock(preference_mutex);
        if(!force && now<next_read.load(std::memory_order_relaxed))return;
        wchar_t value[32]{};
        GetPrivateProfileStringW(L"interface",L"language",L"en",value,32,preference_file().c_str());
        language.store(_wcsicmp(value,L"zh-CN")==0?UiLanguage::simplified_chinese:UiLanguage::english);
        next_read.store(now+250,std::memory_order_relaxed);
    } catch(...) {next_read.store(now+1000,std::memory_order_relaxed);}
}
bool set_ui_language(UiLanguage value) noexcept {
    if(value!=UiLanguage::english && value!=UiLanguage::simplified_chinese)return false;
    try {
        std::scoped_lock lock(preference_mutex);
        const auto file=preference_file();std::filesystem::create_directories(file.parent_path());
        if(!WritePrivateProfileStringW(L"interface",L"language",value==UiLanguage::english?L"en":L"zh-CN",file.c_str()))return false;
        language.store(value);next_read.store(GetTickCount64()+250,std::memory_order_relaxed);return true;
    }catch(...){return false;}
}
const char* ui_translate(const char* text) noexcept {
    if(!text || ui_language()==UiLanguage::english)return text;
    const auto found=std::lower_bound(std::begin(translations),std::end(translations),std::string_view(text),
        [](const Translation& entry,std::string_view key){return std::string_view(entry.english)<key;});
    return found!=std::end(translations) && std::string_view(found->english)==text?found->chinese:text;
}
std::string ui_translate_text(std::string_view value) {
    std::string text(value);
    if(ui_language()==UiLanguage::english)return text;
    const auto* exact=ui_translate(text.c_str());if(exact!=text.c_str())return exact;
    // Shared runtime status remains English. Translate only presentation fragments,
    // never configuration keys or the short words that might occur in file names.
    static const auto fragments=[] {
        std::vector<const Translation*> result;
        for(const auto& entry:translations)
            if(std::string_view(entry.english).size()>=18 || std::string_view(entry.english).starts_with(": ") || std::string_view(entry.english).starts_with(" + ") || std::string_view(entry.english).starts_with(" - "))result.push_back(&entry);
        std::stable_sort(result.begin(),result.end(),[](auto* a,auto* b){return std::string_view(a->english).size()>std::string_view(b->english).size();});
        return result;
    }();
    for(const auto* entry:fragments) {
        const std::string_view key(entry->english);std::size_t pos=0;
        while((pos=text.find(key,pos))!=std::string::npos){text.replace(pos,key.size(),entry->chinese);pos+=std::char_traits<char>::length(entry->chinese);}
    }
    return text;
}
std::wstring ui_translate_wide(std::wstring_view text) {
    if(ui_language()==UiLanguage::english || text.empty())return std::wstring(text);
    const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string bytes(n,'\0');WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),bytes.data(),n,nullptr,nullptr);
    bytes=ui_translate_text(bytes);
    const int size=MultiByteToWideChar(CP_UTF8,0,bytes.data(),int(bytes.size()),nullptr,0);
    std::wstring result(size,L'\0');MultiByteToWideChar(CP_UTF8,0,bytes.data(),int(bytes.size()),result.data(),size);return result;
}
}
