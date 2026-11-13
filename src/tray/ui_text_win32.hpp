#pragma once
#include "xrfg/ui_language.hpp"
#include <windows.h>
#include <commctrl.h>
#include <array>
namespace xrfg::ui {
inline BOOL AppendMenuW(HMENU menu,UINT flags,UINT_PTR id,LPCWSTR value) {
    if(!value || IS_INTRESOURCE(value) || (flags&MF_BITMAP))return ::AppendMenuW(menu,flags,id,value);
    const auto s=autonomous::ui_translate_wide(value);return ::AppendMenuW(menu,flags,id,s.c_str());
}
inline BOOL SetDlgItemTextW(HWND window,int id,LPCWSTR value){const auto s=autonomous::ui_translate_wide(value?value:L"");return ::SetDlgItemTextW(window,id,s.c_str());}
inline int MessageBoxW(HWND parent,LPCWSTR message,LPCWSTR title,UINT flags){const auto m=autonomous::ui_translate_wide(message?message:L""),t=autonomous::ui_translate_wide(title?title:L"");return ::MessageBoxW(parent,m.c_str(),t.c_str(),flags);}
inline INT_PTR DialogBoxParamW(HINSTANCE instance,LPCWSTR name,HWND parent,DLGPROC procedure,LPARAM param) {
    autonomous::refresh_ui_language(true);
    const WORD lang=autonomous::ui_language()==autonomous::UiLanguage::english?MAKELANGID(LANG_ENGLISH,SUBLANG_ENGLISH_US):MAKELANGID(LANG_CHINESE,SUBLANG_CHINESE_SIMPLIFIED);
    auto resource=FindResourceExW(instance,RT_DIALOG,name,lang);
    if(!resource)resource=FindResourceW(instance,name,RT_DIALOG);
    return resource?DialogBoxIndirectParamW(instance,static_cast<const DLGTEMPLATE*>(LockResource(LoadResource(instance,resource))),parent,procedure,param):-1;
}
inline HRESULT TaskDialogIndirect(const TASKDIALOGCONFIG* source,int* button,int* radio,BOOL* checked) {
    auto config=*source;std::array<std::wstring,5> strings;
    const auto translated=[](LPCWSTR value,std::wstring& storage)->LPCWSTR {
        if(!value || IS_INTRESOURCE(value))return value;
        storage=autonomous::ui_translate_wide(value);return storage.c_str();
    };
    config.pszWindowTitle=translated(config.pszWindowTitle,strings[0]);
    config.pszMainInstruction=translated(config.pszMainInstruction,strings[1]);
    config.pszContent=translated(config.pszContent,strings[2]);
    config.pszExpandedInformation=translated(config.pszExpandedInformation,strings[3]);
    config.pszFooter=translated(config.pszFooter,strings[4]);
    return ::TaskDialogIndirect(&config,button,radio,checked);
}
}
