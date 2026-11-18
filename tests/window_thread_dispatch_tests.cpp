#include "xrfg/window_thread_dispatch.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace std::chrono_literals;
namespace {
void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
std::timed_mutex mod_lock;
thread_local bool position_hook_active{};
bool probing{},compat{};
DWORD window_thread{};
std::atomic<unsigned> blocked_callbacks{},foreign_probes{},owner_probes{},deferred{},payload_errors{};
LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(xrfg::window_dispatch_detail::defer_foreign_probe(hwnd,message,wp,lp,compat)) {++deferred;return 0;}
    if(probing && message==WM_NULL) {
        if(wp!=71 || lp!=83)++payload_errors;
        if(GetCurrentThreadId()==window_thread)++owner_probes;else ++foreign_probes;
        std::scoped_lock lock(mod_lock);
        position_hook_active=true;
        SetWindowPos(hwnd,nullptr,10+int(owner_probes.load()+foreign_probes.load()),12,80,60,SWP_NOZORDER|SWP_NOACTIVATE);
        position_hook_active=false;
        return 0;
    }
    if(probing && message==WM_WINDOWPOSCHANGING && !position_hook_active) {
        // Bounded model of the native-mod hook lock: without the handoff the
        // worker holds it while SetWindowPos waits for this window callback.
        if(mod_lock.try_lock_for(60ms))mod_lock.unlock();else ++blocked_callbacks;
    }
    if(message==WM_APP+7)return 42;
    return DefWindowProcW(hwnd,message,wp,lp);
}
void pump(){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
void run_probe(HWND hwnd,bool enabled) {
    compat=enabled;
    std::atomic<bool> finished{};
    std::thread worker([&]{CallWindowProcW(procedure,hwnd,WM_NULL,71,83);finished=true;});
    const auto deadline=GetTickCount64()+2000;
    while(!finished && GetTickCount64()<deadline){pump();Sleep(1);}
    const bool returned=finished;
    // The 60ms lock probe above always releases the baseline instead of
    // leaving a real deadlocked fixture process/thread behind.
    worker.join();pump();
    check(returned,"foreign callback did not return");
}
}
int main() {
    try {
        using namespace xrfg::window_dispatch_detail;
        check(alanwake_process(L"I:\\AlanWake2\\AlanWake2.exe"),"AW2 executable qualification");
        check(alanwake_process(L"C:/Games/ALANWAKE2.EXE"),"case-insensitive executable qualification");
        check(!alanwake_process(L"OtherGame.exe")&&!alanwake_process(L"AlanWake2.exe.bak"),"other executables excluded");
        WNDCLASSW wc{};wc.lpfnWndProc=procedure;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"OFXRWindowThreadFixture";
        check(RegisterClassW(&wc)!=0,"register hidden fixture");
        HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"",WS_POPUP,0,0,80,60,nullptr,nullptr,wc.hInstance,nullptr);
        check(hwnd!=nullptr,"create hidden fixture");window_thread=GetCurrentThreadId();
        check(!defer_foreign_probe(hwnd,WM_NULL,0,0,true),"same-thread call remains synchronous");
        check(!defer_foreign_probe(nullptr,WM_NULL,0,0,true),"null HWND excluded");
        check(!defer_foreign_probe(GetDesktopWindow(),WM_NULL,0,0,true),"foreign process HWND excluded");
        probing=true;
        run_probe(hwnd,false);
        check(blocked_callbacks>0&&foreign_probes==1&&deferred==0,"baseline reproduces cross-thread window/lock inversion");
        const auto old_blocked=blocked_callbacks.load();
        for(unsigned i=0;i<32;++i)run_probe(hwnd,true);
        check(blocked_callbacks==old_blocked,"async owner handoff still stalls the window hook lock");
        check(deferred==32&&owner_probes==32&&foreign_probes==1&&payload_errors==0,"probes not delivered exactly once on HWND owner with original payload");
        std::atomic<LRESULT> other{};
        std::thread unrelated([&]{other=CallWindowProcW(procedure,hwnd,WM_APP+7,0,0);});unrelated.join();
        check(other==42,"non-probe return semantics changed");
        check(SendMessageW(hwnd,WM_NULL,71,83)==0&&owner_probes==33&&deferred==32,"normal owner dispatch changed");
        probing=false;DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,wc.hInstance);
        std::cout<<"Baseline lock inversion reproduced;32 foreign WM_NULL probes handed to HWND owner without lock waits; scope/payload/return checks PASS\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
