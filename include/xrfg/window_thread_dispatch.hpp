#pragma once
#include <windows.h>
#include <string_view>
#include <iterator>

namespace xrfg::window_dispatch_detail {
inline bool alanwake_process(std::wstring_view path) noexcept {
    const auto slash=path.find_last_of(L"/\\");
    if(slash!=std::wstring_view::npos)path.remove_prefix(slash+1);
    constexpr std::wstring_view expected=L"alanwake2.exe";
    if(path.size()!=expected.size())return false;
    for(std::size_t i=0;i<path.size();++i) {
        auto c=path[i];if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';
        if(c!=expected[i])return false;
    }
    return true;
}
inline bool alanwake_process() noexcept {
    static const bool enabled=[] {
        wchar_t path[32768]{};
        const auto size=GetModuleFileNameW(nullptr,path,DWORD(std::size(path)));
        return size && size<std::size(path) && alanwake_process({path,size});
    }();
    return enabled;
}
// A mod may call a saved WndProc directly from its polling worker, rather
// than SendMessage to the HWND owner. Its WM_NULL heartbeat can then enter
// a locked window-position hook and synchronously wait for that owner,
// whose own position callback needs the same hook lock. Keep this no-result,
// pointer-free probe on the actual window thread before forwarding it.
inline bool defer_foreign_probe(HWND window,UINT message,WPARAM wp,LPARAM lp,bool enabled) noexcept {
    if(!enabled || message!=WM_NULL || !window)return false;
    DWORD process{};
    const auto owner=GetWindowThreadProcessId(window,&process);
    if(!owner || owner==GetCurrentThreadId() || process!=GetCurrentProcessId())return false;
    // WM_NULL returns zero. Failure to post an optional heartbeat must not
    // re-enter the same synchronous foreign-thread path.
    PostMessageW(window,message,wp,lp);
    return true;
}
}
