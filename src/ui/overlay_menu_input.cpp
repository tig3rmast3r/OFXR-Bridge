#include "xrfg/overlay_menu_input.hpp"
#include "xrfg/overlay_menu_input_state.hpp"
#include "xrfg/autonomous_control.hpp"
#include "vendor/imgui/imgui.h"
#include <windows.h>
#include <windowsx.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <safetyhook.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xrfg {
namespace {
using overlay_input_detail::ButtonMask;
using overlay_input_detail::VirtualCursor;
constexpr UINT control_message=WM_APP+0x4B3;
constexpr WPARAM control_cookie=0x4F465852;
thread_local unsigned bypass_depth{};
thread_local unsigned window_dispatch_depth{},capture_transition_depth{};
thread_local HRAWINPUT current_raw_message{};
struct Bypass {Bypass(){++bypass_depth;}~Bypass(){--bypass_depth;}};
struct WindowDispatch {
    unsigned saved_bypass{bypass_depth};
    WindowDispatch(){bypass_depth=0;++window_dispatch_depth;}
    ~WindowDispatch(){--window_dispatch_depth;bypass_depth=saved_bypass;}
};
struct Transition {Transition(){++capture_transition_depth;}~Transition(){--capture_transition_depth;}};
struct RawDecision {HRAWINPUT handle{};bool suppress{};USHORT allowed_mouse_ups{};};
enum class CapturePhase {idle,acquiring,active,restoring};
struct Capture {
    HWND window{};
    bool ready{},saved_clip_active{},deferred_clip{},deferred_clip_null{};
    RECT saved_clip{},requested_clip{};
    HWND saved_capture{},requested_capture{};
    bool deferred_capture{};
    std::array<RAWINPUTDEVICE,2> saved_raw{},requested_raw{};
    std::array<bool,2> had_raw{},deferred_raw{};
};
struct DirectHook {
    SafetyHookInline get_state,get_data,release;
    void* state_entry{};void* data_entry{};void* release_entry{};
};
struct State {
    std::mutex mutex,install_mutex;
    std::unordered_set<std::uint64_t> owners;
    std::unordered_map<HWND,WNDPROC> procedures;
    std::uint64_t active_owner{},next_owner{1};
    HWND target{};
    bool open{},focused{},toggle_latched{},esc_latched{},raw_mouse_seen{};
    POINT stable_cursor{},stable_physical_cursor{};
    VirtualCursor cursor;
    ButtonMask keyboard,mouse;
    std::vector<OverlayMenuInputEvent> events;
    std::array<RawDecision,128> decisions{};
    std::size_t decision_cursor{};
    Capture capture;
    CapturePhase phase{CapturePhase::idle};
    std::uint64_t revision{};
    wchar_t high_surrogate{};
    SafetyHookInline raw_data,raw_buffer,register_raw,set_pos,get_pos,set_physical,get_physical,
        clip,set_capture,release_capture,async_key,key_state,keyboard_state;
    bool hooks_ready{};
    std::array<DirectHook,8> direct;
    std::unordered_map<void*,unsigned> direct_kind;
    std::unordered_map<void*,overlay_input_detail::BufferedButtons> direct_buttons;
    HMODULE direct_module{};
};
State& state(){static auto* value=new State;return *value;}
HWND foreground_owned() {
    HWND foreground=GetForegroundWindow();DWORD pid{};
    if(!foreground||!GetWindowThreadProcessId(foreground,&pid)||pid!=GetCurrentProcessId())return nullptr;
    return GetAncestor(foreground,GA_ROOT);
}
bool blocking_locked(const State& s){return !bypass_depth&&s.open&&s.focused&&s.target==foreground_owned();}
void event_locked(State& s,OverlayMenuInputEvent e){
    // Bounded even when an XR session stops producing frames. Snapshot state
    // reconciles the final key/button state if the event queue reaches capacity.
    if(s.events.size()<2048)s.events.push_back(e);
}
void position_locked(State& s){event_locked(s,{OverlayMenuInputEventKind::mouse_position,s.cursor.x,s.cursor.y});}
SHORT physical_key(int key) {
    using Read=SHORT(WINAPI*)(int);
    static Read read=[] {
        HMODULE module=GetModuleHandleW(L"win32u.dll");
        return module?reinterpret_cast<Read>(GetProcAddress(module,"NtUserGetAsyncKeyState")):nullptr;
    }();
    if(read)return read(key);
    Bypass guard;return GetAsyncKeyState(key);
}
int raw_slot(const RAWINPUTDEVICE& r){return r.usUsagePage==1&&r.usUsage==2?0:r.usUsagePage==1&&r.usUsage==6?1:-1;}
bool same_rect(const RECT& a,const RECT& b){return a.left==b.left&&a.top==b.top&&a.right==b.right&&a.bottom==b.bottom;}
void service_capture(HWND window,std::uint64_t revision);
void detach_windows_if_idle();
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wp,LPARAM lp);

bool attach(HWND window) {
    if(!window||window!=foreground_owned())return false;
    auto& s=state();std::scoped_lock installation(s.install_mutex);
    {std::scoped_lock lock(s.mutex);if(s.procedures.contains(window)){s.target=window;return true;}}
    auto previous=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window,GWLP_WNDPROC));
    if(!previous||previous==window_proc)return false;
    {std::scoped_lock lock(s.mutex);s.procedures.emplace(window,previous);}
    SetLastError(0);
    auto actual_previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(window_proc)));
    if(!actual_previous&&GetLastError()){std::scoped_lock lock(s.mutex);s.procedures.erase(window);return false;}
    {std::scoped_lock lock(s.mutex);s.procedures[window]=actual_previous;s.target=window;}
    return true;
}
void request_close() noexcept {
    auto& s=state();HWND window{};std::uint64_t revision{};
    {std::scoped_lock lock(s.mutex);
        if(!s.open)return;
        s.open=false;s.focused=false;s.active_owner=0;window=s.capture.window?s.capture.window:s.target;
        revision=++s.revision;
        s.cursor.reset_seeds();s.raw_mouse_seen=false;s.high_surrogate=0;
        event_locked(s,{OverlayMenuInputEventKind::focus,0,0,0,false});
    }
    // Capture APIs run on the HWND thread, without the state mutex. Posting
    // avoids SendMessage cross-thread lock inversions with the game's WndProc.
    if(window&&IsWindow(window)) {
        if(GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()&&!window_dispatch_depth&&!capture_transition_depth)service_capture(window,revision);
        else PostMessageW(window,control_message,control_cookie,LPARAM(revision));
    }
}
bool request_open(std::uint64_t owner) {
    auto& s=state();HWND window=foreground_owned();if(!window||!attach(window))return false;
    POINT cursor{},physical{};{Bypass bypass;GetCursorPos(&cursor);GetPhysicalCursorPos(&physical);}
    std::array<bool,256> held{};for(unsigned k=0;k<256;++k)held[k]=(physical_key(int(k))&0x8000)!=0;
    {std::scoped_lock lock(s.mutex);
        if(!s.owners.contains(owner)||!s.hooks_ready)return false;
        if(s.open)return s.active_owner==owner;
        // Capture restoration must finish before another ownership transition.
        if(s.phase!=CapturePhase::idle)return false;
        s.phase=CapturePhase::acquiring;s.capture={};s.capture.window=window;++s.revision;
        s.open=s.focused=true;s.active_owner=owner;s.stable_cursor=cursor;s.stable_physical_cursor=physical;
        // The game may have received these presses before the API layer loaded.
        // Seed their ownership as game-held so their eventual release passes.
        for(unsigned k=0;k<256;++k)if(held[k])s.keyboard.down[k]=true;
        constexpr unsigned buttons[]{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
        for(unsigned b=0;b<5;++b)if(held[buttons[b]])s.mouse.down[b]=true;
        s.cursor.reset_seeds();s.raw_mouse_seen=false;s.events.clear();
        event_locked(s,{OverlayMenuInputEventKind::focus,0,0,0,true});position_locked(s);
    }
    std::uint64_t revision{};{std::scoped_lock lock(s.mutex);revision=s.revision;}
    if(GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()&&!window_dispatch_depth&&!capture_transition_depth)service_capture(window,revision);
    else if(!PostMessageW(window,control_message,control_cookie,LPARAM(revision))){request_close();return false;}
    {std::scoped_lock lock(s.mutex);return s.open&&s.active_owner==owner;}
}
void service_capture(HWND window,std::uint64_t revision) {
    if(!window||GetWindowThreadProcessId(window,nullptr)!=GetCurrentThreadId()||capture_transition_depth)return;
    auto& s=state();Capture capture{};bool opening{};
    {std::scoped_lock lock(s.mutex);
        if(revision!=s.revision||s.capture.window!=window||s.phase==CapturePhase::idle||s.phase==CapturePhase::restoring)return;
        opening=s.open&&s.phase==CapturePhase::acquiring;
        if(s.phase==CapturePhase::active&&s.open)return;
        if(!opening&&!s.capture.ready){s.capture={};s.phase=CapturePhase::idle;return;}
        if(!opening)s.phase=CapturePhase::restoring;
    }
    Transition transition;
    if(opening) {
        if(window!=foreground_owned()){request_close();return;}
        Bypass bypass;
        capture.window=window;capture.ready=true;GetClipCursor(&capture.saved_clip);
        RECT desktop{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};
        desktop.right=desktop.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);
        desktop.bottom=desktop.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);
        capture.saved_clip_active=!same_rect(capture.saved_clip,desktop);
        GUITHREADINFO gui{sizeof(gui)};GetGUIThreadInfo(GetWindowThreadProcessId(window,nullptr),&gui);
        capture.saved_capture=gui.hwndCapture;
        UINT count{};GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE));
        if(count&&count<4096) {
            std::vector<RAWINPUTDEVICE> registered(count);
            UINT available=count;
            if(GetRegisteredRawInputDevices(registered.data(),&available,sizeof(RAWINPUTDEVICE))!=UINT(-1))
                for(unsigned i=0;i<available;++i)if(int slot=raw_slot(registered[i]);slot>=0){
                    capture.had_raw[slot]=true;capture.saved_raw[slot]=registered[i];
                }
        }
        {std::scoped_lock lock(s.mutex);
            if(!s.open||s.target!=window||revision!=s.revision){s.capture={};s.phase=CapturePhase::idle;return;}
            s.capture=capture;
        }
        // Preserve the accepted registration behavior, including NOLEGACY for
        // raw-only games. Only transition ownership changes in this fix.
        RAWINPUTDEVICE raw[2]{{1,2,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,window},
                             {1,6,RIDEV_INPUTSINK|RIDEV_DEVNOTIFY,window}};
        for(unsigned i=0;i<2;++i)if(capture.had_raw[i])raw[i].dwFlags|=capture.saved_raw[i].dwFlags&RIDEV_NOLEGACY;
        if(!RegisterRawInputDevices(raw,2,sizeof(RAWINPUTDEVICE)))request_close();
        // The headset cursor is independent of the desktop pointer. Keep
        // button messages owned by this foreground HWND even when the desktop
        // pointer leaves its client rectangle, without confining that pointer.
        bool wanted{};{std::scoped_lock lock(s.mutex);wanted=s.open&&revision==s.revision;}
        if(wanted){ClipCursor(nullptr);SetCapture(window);if(GetCapture()!=window)request_close();}
        bool close_again{};std::uint64_t next{};
        {std::scoped_lock lock(s.mutex);s.phase=CapturePhase::active;close_again=!s.open||revision!=s.revision;next=s.revision;}
        if(close_again)PostMessageW(window,control_message,control_cookie,LPARAM(next));
    } else {
        {std::scoped_lock lock(s.mutex);capture=s.capture;}
        Bypass bypass;
        for(unsigned i=0;i<2;++i) {
            RAWINPUTDEVICE restore{};
            if(capture.deferred_raw[i])restore=capture.requested_raw[i];
            else if(capture.had_raw[i])restore=capture.saved_raw[i];
            else restore={1,USHORT(i?6:2),RIDEV_REMOVE,nullptr};
            // Win32 requires NULL hwnd for RIDEV_REMOVE.
            if(restore.dwFlags&RIDEV_REMOVE)restore.hwndTarget=nullptr;
            RegisterRawInputDevices(&restore,1,sizeof(restore));
        }
        // A focus loss releases confinement instead of trapping the cursor in
        // a background game's saved rectangle.
        if(window!=foreground_owned())ClipCursor(nullptr);
        else if(capture.deferred_clip)ClipCursor(capture.deferred_clip_null?nullptr:&capture.requested_clip);
        else ClipCursor(capture.saved_clip_active?&capture.saved_clip:nullptr);
        HWND restore=capture.deferred_capture?capture.requested_capture:capture.saved_capture;
        if(restore&&IsWindow(restore)&&window==foreground_owned()&&
            GetWindowThreadProcessId(restore,nullptr)==GetCurrentThreadId())SetCapture(restore);
        else if(GetCapture()==window)ReleaseCapture();
        {std::scoped_lock lock(s.mutex);s.capture={};s.phase=CapturePhase::idle;}
        detach_windows_if_idle();
    }
}

unsigned key_code(const RAWKEYBOARD& key) {
    unsigned code=key.VKey;
    if(code==VK_SHIFT)code=MapVirtualKeyW(key.MakeCode,MAPVK_VSC_TO_VK_EX);
    if(code==VK_CONTROL)code=(key.Flags&RI_KEY_E0)?VK_RCONTROL:VK_LCONTROL;
    if(code==VK_MENU)code=(key.Flags&RI_KEY_E0)?VK_RMENU:VK_LMENU;
    return code;
}
void text_locked(State& s,unsigned value) {
    if(value>=0xD800&&value<=0xDBFF){s.high_surrogate=wchar_t(value);return;}
    if(value>=0xDC00&&value<=0xDFFF&&s.high_surrogate){
        value=0x10000+((unsigned(s.high_surrogate)-0xD800)<<10)+(value-0xDC00);
    }
    s.high_surrogate=0;
    if(value>=32&&value<=0x10FFFF)event_locked(s,{OverlayMenuInputEventKind::text,0,0,value});
}
bool key_locked(State& s,unsigned code,bool down) {
    if(code>=256)return false;
    bool changed=s.keyboard.down[code]!=down;
    const auto keys=autonomous::keybinds();
    const auto chord=keys.bindings[KeybindAction::open_interface];
    const bool shortcut=!keys.suspended && code==chord.key && keybind_matches(chord,[&](unsigned k){return k==code?down:(physical_key(int(k))&0x8000)!=0;});
    bool suppress=s.keyboard.event(code,down,s.open||shortcut);
    if(s.open&&changed)event_locked(s,{OverlayMenuInputEventKind::key,0,0,code,down});
    return suppress;
}
RawDecision process_raw_locked(State& s,HRAWINPUT handle,const RAWINPUT& raw) {
    if(s.owners.empty())return {handle};
    // A mod may read input without forwarding WM_INPUT to our WndProc.
    // Deduplicate only within our exact message dispatch: Windows recycles
    // HRAWINPUT values, so retaining one outside it can freeze later packets.
    const bool cache=handle && handle==current_raw_message;
    if(cache)for(const auto& cached:s.decisions)if(cached.handle==handle)return cached;
    RawDecision decision{handle};bool open=blocking_locked(s);
    if(raw.header.dwType==RIM_TYPEKEYBOARD) {
        auto code=key_code(raw.data.keyboard);bool down=!(raw.data.keyboard.Flags&RI_KEY_BREAK);
        // State from background INPUTSINK packets is never given to the menu.
        if(open)decision.suppress=key_locked(s,code,down);
        else if(code<256)decision.suppress=s.keyboard.event(code,down,false);
    } else if(raw.header.dwType==RIM_TYPEMOUSE) {
        const auto& mouse=raw.data.mouse;
        if(open) {
            s.raw_mouse_seen=true;
            if(mouse.usFlags&MOUSE_MOVE_ABSOLUTE)s.cursor.absolute(float(mouse.lLastX),float(mouse.lLastY));
            else s.cursor.relative(float(mouse.lLastX),float(mouse.lLastY));
            position_locked(s);decision.suppress=true;
            if(mouse.usButtonFlags&RI_MOUSE_WHEEL)event_locked(s,{OverlayMenuInputEventKind::wheel,0,float(SHORT(mouse.usButtonData))/WHEEL_DELTA});
            if(mouse.usButtonFlags&RI_MOUSE_HWHEEL)event_locked(s,{OverlayMenuInputEventKind::wheel,float(SHORT(mouse.usButtonData))/WHEEL_DELTA,0});
        }
        for(unsigned i=0;i<5;++i) {
            const USHORT press=USHORT(1u<<(i*2)),release=USHORT(2u<<(i*2));
            if(mouse.usButtonFlags&press){
                bool changed=!s.mouse.down[i];s.mouse.event(i,true,open);
                if(open&&changed)event_locked(s,{OverlayMenuInputEventKind::mouse_button,0,0,i,true});
            }
            if(mouse.usButtonFlags&release){
                bool changed=s.mouse.down[i];bool blocked=s.mouse.event(i,false,open);
                if(!blocked)decision.allowed_mouse_ups|=release;
                if(open&&changed)event_locked(s,{OverlayMenuInputEventKind::mouse_button,0,0,i,false});
            }
        }
        if(!open&&decision.allowed_mouse_ups!=mouse.usButtonFlags)
            decision.suppress=false;
    }
    if(cache)s.decisions[s.decision_cursor++%s.decisions.size()]=decision;
    return decision;
}
void sanitize(RAWINPUT& raw,const RawDecision& decision) {
    if(!decision.suppress)return;
    if(raw.header.dwType==RIM_TYPEMOUSE) {
        raw.data.mouse={};raw.data.mouse.usButtonFlags=decision.allowed_mouse_ups;
    } else if(raw.header.dwType==RIM_TYPEKEYBOARD) {
        raw.data.keyboard={};raw.data.keyboard.Flags=RI_KEY_BREAK;raw.data.keyboard.Message=WM_KEYUP;
    }
}
UINT WINAPI hooked_raw_data(HRAWINPUT handle,UINT command,void* data,UINT* size,UINT header_size) {
    auto& s=state();UINT result=s.raw_data.stdcall<UINT>(handle,command,data,size,header_size);
    if(!bypass_depth&&command==RID_INPUT&&data&&result!=UINT(-1)&&result>=sizeof(RAWINPUTHEADER)&&header_size==sizeof(RAWINPUTHEADER)) {
        auto& raw=*static_cast<RAWINPUT*>(data);
        if((raw.header.dwType==RIM_TYPEMOUSE&&result>=offsetof(RAWINPUT,data)+sizeof(RAWMOUSE))||
           (raw.header.dwType==RIM_TYPEKEYBOARD&&result>=offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD))) {
            std::scoped_lock lock(s.mutex);sanitize(raw,process_raw_locked(s,handle,raw));
        }
    }
    return result;
}
UINT WINAPI hooked_raw_buffer(RAWINPUT* data,UINT* size,UINT header_size) {
    auto& s=state();UINT capacity=size?*size:0;
    UINT result=s.raw_buffer.stdcall<UINT>(data,size,header_size);
    if(!bypass_depth&&data&&result!=UINT(-1)&&header_size==sizeof(RAWINPUTHEADER)) {
        overlay_input_detail::reduce_raw_buffer(data,capacity,result);
    }
    return result;
}
BOOL WINAPI hooked_register(const RAWINPUTDEVICE* devices,UINT count,UINT size) {
    auto& s=state();
    if(bypass_depth||!devices||size!=sizeof(RAWINPUTDEVICE))return s.register_raw.stdcall<BOOL>(devices,count,size);
    std::vector<RAWINPUTDEVICE> pass;bool intercepted=false;
    {std::scoped_lock lock(s.mutex);
        bool block=blocking_locked(s)&&s.capture.ready;
        for(unsigned i=0;i<count;++i) {
            int slot=raw_slot(devices[i]);
            if(slot>=0&&s.capture.ready){
                // During posted close/focus loss, preserve newer game requests
                // too. They pass immediately and are not overwritten by the
                // older saved registration when the HWND thread retires us.
                s.capture.deferred_raw[slot]=true;s.capture.requested_raw[slot]=devices[i];
                if(block){intercepted=true;continue;}
            }
            pass.push_back(devices[i]);
        }
    }
    if(!pass.empty())return s.register_raw.stdcall<BOOL>(pass.data(),UINT(pass.size()),size);
    return intercepted?TRUE:s.register_raw.stdcall<BOOL>(devices,count,size);
}
BOOL WINAPI hooked_set_pos(int x,int y){auto& s=state();if(bypass_depth)return s.set_pos.stdcall<BOOL>(x,y);{std::scoped_lock lock(s.mutex);if(blocking_locked(s))return TRUE;}return s.set_pos.stdcall<BOOL>(x,y);}
BOOL WINAPI hooked_get_pos(POINT* point){auto& s=state();if(bypass_depth)return s.get_pos.stdcall<BOOL>(point);{std::scoped_lock lock(s.mutex);if(point&&blocking_locked(s)){*point=s.stable_cursor;return TRUE;}}return s.get_pos.stdcall<BOOL>(point);}
BOOL WINAPI hooked_set_physical(int x,int y){auto& s=state();if(bypass_depth)return s.set_physical.stdcall<BOOL>(x,y);{std::scoped_lock lock(s.mutex);if(blocking_locked(s))return TRUE;}return s.set_physical.stdcall<BOOL>(x,y);}
BOOL WINAPI hooked_get_physical(POINT* point){auto& s=state();if(bypass_depth)return s.get_physical.stdcall<BOOL>(point);{std::scoped_lock lock(s.mutex);if(point&&blocking_locked(s)){*point=s.stable_physical_cursor;return TRUE;}}return s.get_physical.stdcall<BOOL>(point);}
BOOL WINAPI hooked_clip(const RECT* rect){auto& s=state();if(bypass_depth)return s.clip.stdcall<BOOL>(rect);{std::scoped_lock lock(s.mutex);if(blocking_locked(s)&&s.capture.ready){s.capture.deferred_clip=true;s.capture.deferred_clip_null=!rect;if(rect)s.capture.requested_clip=*rect;return TRUE;}}return s.clip.stdcall<BOOL>(rect);}
HWND WINAPI hooked_set_capture(HWND window){auto& s=state();if(bypass_depth)return s.set_capture.stdcall<HWND>(window);{std::scoped_lock lock(s.mutex);if(blocking_locked(s)&&s.capture.ready){HWND previous=s.capture.deferred_capture?s.capture.requested_capture:s.capture.saved_capture;s.capture.deferred_capture=true;s.capture.requested_capture=window;return previous;}}return s.set_capture.stdcall<HWND>(window);}
BOOL WINAPI hooked_release_capture(){auto& s=state();if(bypass_depth)return s.release_capture.stdcall<BOOL>();{std::scoped_lock lock(s.mutex);if(blocking_locked(s)&&s.capture.ready){s.capture.deferred_capture=true;s.capture.requested_capture=nullptr;return TRUE;}}return s.release_capture.stdcall<BOOL>();}
SHORT WINAPI hooked_async_key(int key){auto& s=state();if(bypass_depth)return s.async_key.stdcall<SHORT>(key);{std::scoped_lock lock(s.mutex);if(blocking_locked(s))return 0;}return s.async_key.stdcall<SHORT>(key);}
SHORT WINAPI hooked_key_state(int key){auto& s=state();if(bypass_depth)return s.key_state.stdcall<SHORT>(key);{std::scoped_lock lock(s.mutex);if(blocking_locked(s))return 0;}return s.key_state.stdcall<SHORT>(key);}
BOOL WINAPI hooked_keyboard_state(BYTE* keys){auto& s=state();if(bypass_depth)return s.keyboard_state.stdcall<BOOL>(keys);{std::scoped_lock lock(s.mutex);if(keys&&blocking_locked(s)){std::memset(keys,0,256);return TRUE;}}return s.keyboard_state.stdcall<BOOL>(keys);}
unsigned direct_kind(void* device) {
    auto& s=state();{std::scoped_lock lock(s.mutex);if(auto found=s.direct_kind.find(device);found!=s.direct_kind.end())return found->second;}
    // GetCapabilities has the same ABI for A/W interfaces and custom formats.
    // Device type is authoritative; buffer length is never a type heuristic.
    auto** table=*reinterpret_cast<void***>(device);
    using Caps=HRESULT(STDMETHODCALLTYPE*)(void*,DIDEVCAPS*);
    DIDEVCAPS caps{};caps.dwSize=sizeof(caps);
    unsigned kind{};
    if(SUCCEEDED(reinterpret_cast<Caps>(table[3])(device,&caps))) {
        unsigned type=GET_DIDEVICE_TYPE(caps.dwDevType);
        if(type==DI8DEVTYPE_MOUSE)kind=1;
        if(type==DI8DEVTYPE_KEYBOARD)kind=2;
    }
    {std::scoped_lock lock(s.mutex);if(s.direct_kind.size()<256)s.direct_kind[device]=kind;}
    return kind;
}
template<unsigned Index> HRESULT STDMETHODCALLTYPE direct_state(void* device,DWORD bytes,void* data) {
    auto& s=state();auto result=s.direct[Index].get_state.stdcall<HRESULT>(device,bytes,data);
    if(!bypass_depth&&SUCCEEDED(result)&&data&&direct_kind(device)) {
        std::scoped_lock lock(s.mutex);if(blocking_locked(s))std::memset(data,0,bytes);
    }
    return result;
}
template<unsigned Index> HRESULT STDMETHODCALLTYPE direct_data(void* device,DWORD stride,DIDEVICEOBJECTDATA* data,DWORD* count,DWORD flags) {
    auto& s=state();DWORD capacity=count?*count:0;
    auto result=s.direct[Index].get_data.stdcall<HRESULT>(device,stride,data,count,flags);
    if(!bypass_depth&&SUCCEEDED(result)&&count&&direct_kind(device)&&data&&
       stride>=offsetof(DIDEVICEOBJECTDATA,dwSequence)+sizeof(DWORD)&&stride<4096&&*count<=capacity&&*count<65536) {
        const DWORD received=*count;
        std::vector<bool> buttons(received);
        auto** table=*reinterpret_cast<void***>(device);
        using ObjectInfo=HRESULT(STDMETHODCALLTYPE*)(void*,void*,DWORD,DWORD);
        auto object_info=reinterpret_cast<ObjectInfo>(table[14]);
        for(DWORD i=0;i<received;++i) {
            auto* event=reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data)+std::size_t(i)*stride);
            DIDEVICEOBJECTINSTANCEW info{};info.dwSize=sizeof(info);
            HRESULT described=object_info(device,&info,event->dwOfs,DIPH_BYOFFSET);
            DWORD type=info.dwType;
            if(FAILED(described)) {
                DIDEVICEOBJECTINSTANCEA ansi{};ansi.dwSize=sizeof(ansi);
                described=object_info(device,&ansi,event->dwOfs,DIPH_BYOFFSET);type=ansi.dwType;
            }
            buttons[i]=SUCCEEDED(described)&&(DIDFT_GETTYPE(type)&DIDFT_BUTTON)!=0;
        }
        std::scoped_lock lock(s.mutex);bool open=blocking_locked(s);
        // PEEK reads cannot transfer press ownership. Run the exact ordered
        // filter against a copy and commit only when the native queue advances.
        auto working=s.direct_buttons[device];DWORD kept{};
        for(DWORD i=0;i<received;++i) {
            auto* event=reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<BYTE*>(data)+std::size_t(i)*stride);
            bool suppress=buttons[i]?working.suppress(event->dwOfs,(event->dwData&0x80)!=0,open):open;
            if(!suppress) {
                if(kept!=i)std::memmove(reinterpret_cast<BYTE*>(data)+std::size_t(kept)*stride,event,stride);
                ++kept;
            }
        }
        if(!(flags&DIGDD_PEEK))s.direct_buttons[device]=std::move(working);
        if(kept<received)std::memset(reinterpret_cast<BYTE*>(data)+std::size_t(kept)*stride,0,std::size_t(received-kept)*stride);
        *count=kept;
    }
    return result;
}
template<unsigned Index> ULONG STDMETHODCALLTYPE direct_release(void* device) {
    auto& s=state();auto count=s.direct[Index].release.stdcall<ULONG>(device);
    if(!count){std::scoped_lock lock(s.mutex);s.direct_kind.erase(device);s.direct_buttons.erase(device);}return count;
}
void discover_direct_input() {
    auto& s=state();HMODULE module=GetModuleHandleW(L"dinput8.dll");if(!module)return;
    std::scoped_lock install(s.install_mutex);if(s.direct_module==module)return;
    using Factory=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
    auto factory=reinterpret_cast<Factory>(GetProcAddress(module,"DirectInput8Create"));if(!factory)return;
    constexpr GUID iid_w{0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
    constexpr GUID iid_a{0xbf798030,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
    constexpr GUID mouse_guid{0x6f1d2b60,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00}};
    constexpr GUID keyboard_guid{0x6f1d2b61,0xd5a0,0x11cf,{0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00}};
    const std::array<void*,8> replacements_state{reinterpret_cast<void*>(direct_state<0>),reinterpret_cast<void*>(direct_state<1>),reinterpret_cast<void*>(direct_state<2>),reinterpret_cast<void*>(direct_state<3>),reinterpret_cast<void*>(direct_state<4>),reinterpret_cast<void*>(direct_state<5>),reinterpret_cast<void*>(direct_state<6>),reinterpret_cast<void*>(direct_state<7>)};
    const std::array<void*,8> replacements_data{reinterpret_cast<void*>(direct_data<0>),reinterpret_cast<void*>(direct_data<1>),reinterpret_cast<void*>(direct_data<2>),reinterpret_cast<void*>(direct_data<3>),reinterpret_cast<void*>(direct_data<4>),reinterpret_cast<void*>(direct_data<5>),reinterpret_cast<void*>(direct_data<6>),reinterpret_cast<void*>(direct_data<7>)};
    const std::array<void*,8> replacements_release{reinterpret_cast<void*>(direct_release<0>),reinterpret_cast<void*>(direct_release<1>),reinterpret_cast<void*>(direct_release<2>),reinterpret_cast<void*>(direct_release<3>),reinterpret_cast<void*>(direct_release<4>),reinterpret_cast<void*>(direct_release<5>),reinterpret_cast<void*>(direct_release<6>),reinterpret_cast<void*>(direct_release<7>)};
    bool any{};
    for(const GUID& iid:{iid_w,iid_a}) {
        IDirectInput8W* input{};
        if(FAILED(factory(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,iid,reinterpret_cast<void**>(&input),nullptr))||!input)continue;
        // CreateDevice's signature and vtable slot are identical for A/W.
        for(const GUID& guid:{mouse_guid,keyboard_guid}) {
            IDirectInputDevice8W* device{};
            if(FAILED(input->CreateDevice(guid,&device,nullptr))||!device)continue;
            auto** table=*reinterpret_cast<void***>(device);
            for(unsigned i=0;i<s.direct.size();++i) {
                auto& hook=s.direct[i];
                if(hook.state_entry==table[9]){any=true;break;}
                if(hook.state_entry)continue;
                hook.state_entry=table[9];hook.data_entry=table[10];hook.release_entry=table[2];
                hook.get_state=safetyhook::create_inline(table[9],replacements_state[i],SafetyHookInline::StartDisabled);
                if(hook.get_state&&hook.get_state.enable())any=true;
                // A/W devices may share the buffered/release function while
                // using different state functions. Never hook the same entry twice.
                bool data_seen=false,release_seen=false;
                for(unsigned previous=0;previous<i;++previous){data_seen|=s.direct[previous].data_entry==table[10];release_seen|=s.direct[previous].release_entry==table[2];}
                if(!data_seen){hook.get_data=safetyhook::create_inline(table[10],replacements_data[i],SafetyHookInline::StartDisabled);if(hook.get_data)(void)hook.get_data.enable();}
                if(!release_seen){hook.release=safetyhook::create_inline(table[2],replacements_release[i],SafetyHookInline::StartDisabled);if(hook.release)(void)hook.release.enable();}
                break;
            }
            device->Release();
        }
        input->Release();
    }
    if(any) {
        HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(factory),&pinned);
        s.direct_module=module;
    }
}
void install_hooks() {
    auto& s=state();std::scoped_lock install(s.install_mutex);
    if(s.hooks_ready)return;
    HMODULE user=GetModuleHandleW(L"user32.dll");if(!user)return;
    auto hook=[&](SafetyHookInline& destination,const char* name,void* replacement) {
        if(destination)return true;void* entry=reinterpret_cast<void*>(GetProcAddress(user,name));if(!entry)return false;
        destination=safetyhook::create_inline(entry,replacement,SafetyHookInline::StartDisabled);
        if(!destination)return false;return bool(destination.enable());
    };
    const bool ready=hook(s.raw_data,"GetRawInputData",reinterpret_cast<void*>(hooked_raw_data))&&
        hook(s.raw_buffer,"GetRawInputBuffer",reinterpret_cast<void*>(hooked_raw_buffer))&&
        hook(s.register_raw,"RegisterRawInputDevices",reinterpret_cast<void*>(hooked_register))&&
        hook(s.set_pos,"SetCursorPos",reinterpret_cast<void*>(hooked_set_pos))&&
        hook(s.get_pos,"GetCursorPos",reinterpret_cast<void*>(hooked_get_pos))&&
        hook(s.clip,"ClipCursor",reinterpret_cast<void*>(hooked_clip))&&
        hook(s.set_capture,"SetCapture",reinterpret_cast<void*>(hooked_set_capture))&&
        hook(s.release_capture,"ReleaseCapture",reinterpret_cast<void*>(hooked_release_capture))&&
        hook(s.async_key,"GetAsyncKeyState",reinterpret_cast<void*>(hooked_async_key))&&
        hook(s.key_state,"GetKeyState",reinterpret_cast<void*>(hooked_key_state))&&
        hook(s.keyboard_state,"GetKeyboardState",reinterpret_cast<void*>(hooked_keyboard_state));
    hook(s.set_physical,"SetPhysicalCursorPos",reinterpret_cast<void*>(hooked_set_physical));
    hook(s.get_physical,"GetPhysicalCursorPos",reinterpret_cast<void*>(hooked_get_physical));
    {std::scoped_lock lock(s.mutex);s.hooks_ready=ready;}
}

LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    WindowDispatch dispatch;
    auto& s=state();WNDPROC previous{};bool open{},has_owners{};
    {std::scoped_lock lock(s.mutex);auto found=s.procedures.find(window);if(found!=s.procedures.end())previous=found->second;open=s.open&&s.target==window;has_owners=!s.owners.empty();}
    if(message==control_message&&wp==control_cookie){service_capture(window,std::uint64_t(lp));return 0;}
    if(!has_owners){
        if(message==WM_NCDESTROY){std::scoped_lock lock(s.mutex);s.procedures.erase(window);}
        return previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
    }
    if(message==WM_KILLFOCUS||(message==WM_ACTIVATEAPP&&!wp)||message==WM_CANCELMODE||message==WM_NCDESTROY||
        (message==WM_CAPTURECHANGED&&reinterpret_cast<HWND>(lp)!=window))request_close();
    if(message==WM_INPUT) {
        struct RawMessageScope {
            HRAWINPUT previous{current_raw_message};
            explicit RawMessageScope(HRAWINPUT value){current_raw_message=value;}
            ~RawMessageScope(){current_raw_message=previous;}
        } raw_scope(reinterpret_cast<HRAWINPUT>(lp));
        RawDecision decision{};UINT size{};
        {Bypass bypass;GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,nullptr,&size,sizeof(RAWINPUTHEADER));}
        if(size>=sizeof(RAWINPUTHEADER)&&size<1024*1024) {
            std::vector<BYTE> bytes(size);UINT available=size;
            UINT read{};{Bypass bypass;read=GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,bytes.data(),&available,sizeof(RAWINPUTHEADER));}
            if(read!=UINT(-1)&&read==size&&available==size&&
               ((reinterpret_cast<const RAWINPUT*>(bytes.data())->header.dwType==RIM_TYPEMOUSE&&size>=offsetof(RAWINPUT,data)+sizeof(RAWMOUSE))||
                (reinterpret_cast<const RAWINPUT*>(bytes.data())->header.dwType==RIM_TYPEKEYBOARD&&size>=offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD)))) {
                auto& raw=*reinterpret_cast<const RAWINPUT*>(bytes.data());
                std::scoped_lock lock(s.mutex);decision=process_raw_locked(s,reinterpret_cast<HRAWINPUT>(lp),raw);
                // NOLEGACY keyboard registration has no WM_CHAR. Translate with
                // the actual HWND thread layout; flags=4 leaves dead-key state.
                if(blocking_locked(s)&&raw.header.dwType==RIM_TYPEKEYBOARD&&!(raw.data.keyboard.Flags&RI_KEY_BREAK)&&
                   s.capture.had_raw[1]&&(s.capture.saved_raw[1].dwFlags&RIDEV_NOLEGACY)) {
                    BYTE keys[256]{};for(unsigned k=0;k<256;++k)if(s.keyboard.down[k])keys[k]=0x80;
                    wchar_t text[8]{};int length=ToUnicodeEx(raw.data.keyboard.VKey,raw.data.keyboard.MakeCode,keys,text,8,4,GetKeyboardLayout(0));
                    for(int i=0;i<length;++i)text_locked(s,unsigned(text[i]));
                }
            }
        }
        // A game-held keyboard/mouse release must reach its own input handler.
        bool allowed_release=!decision.suppress||decision.allowed_mouse_ups!=0;
        LRESULT result=open&&!allowed_release?DefWindowProcW(window,message,wp,lp):
            previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
        {std::scoped_lock lock(s.mutex);for(auto& cached:s.decisions)if(cached.handle==reinterpret_cast<HRAWINPUT>(lp))cached={};}
        return result;
    }
    bool consume=false,toggle=false,close=false;
    {std::scoped_lock lock(s.mutex);
        bool active=s.target==window&&blocking_locked(s);
        if(message==WM_KEYDOWN||message==WM_SYSKEYDOWN||message==WM_KEYUP||message==WM_SYSKEYUP) {
            unsigned code=unsigned(wp);bool down=message==WM_KEYDOWN||message==WM_SYSKEYDOWN;
            if(code==VK_SHIFT)code=MapVirtualKeyW((lp>>16)&0xFF,MAPVK_VSC_TO_VK_EX);
            if(code==VK_CONTROL)code=(lp&(1LL<<24))?VK_RCONTROL:VK_LCONTROL;
            if(code==VK_MENU)code=(lp&(1LL<<24))?VK_RMENU:VK_LMENU;
            consume=key_locked(s,code,down);
            const auto keys=autonomous::keybinds();const auto chord=keys.bindings[KeybindAction::open_interface];
            if(code==chord.key){
                const bool matched=keybind_matches(chord,[&](unsigned k){return k==code?down:(physical_key(int(k))&0x8000)!=0;});
                if(matched&&!s.toggle_latched&&!keys.suspended)toggle=true;
                s.toggle_latched=matched;
            }
            if(code==VK_ESCAPE){if(down&&!s.esc_latched&&active)close=true;s.esc_latched=down;}
        } else if(active&&(message==WM_CHAR||message==WM_UNICHAR)) {
            if(message==WM_UNICHAR&&wp==UNICODE_NOCHAR)return TRUE;
            text_locked(s,unsigned(wp));consume=true;
        } else if(active&&message==WM_MOUSEMOVE) {
            if(!s.raw_mouse_seen){RECT rect{};GetClientRect(window,&rect);s.cursor.client(float(GET_X_LPARAM(lp)),float(GET_Y_LPARAM(lp)),float(rect.right),float(rect.bottom));position_locked(s);}consume=true;
        } else if(message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_RBUTTONDOWN||message==WM_RBUTTONUP||
            message==WM_MBUTTONDOWN||message==WM_MBUTTONUP||message==WM_XBUTTONDOWN||message==WM_XBUTTONUP||
            message==WM_LBUTTONDBLCLK||message==WM_RBUTTONDBLCLK||message==WM_MBUTTONDBLCLK||message==WM_XBUTTONDBLCLK) {
            unsigned button=message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_LBUTTONDBLCLK?0:
                message==WM_RBUTTONDOWN||message==WM_RBUTTONUP||message==WM_RBUTTONDBLCLK?1:
                message==WM_MBUTTONDOWN||message==WM_MBUTTONUP||message==WM_MBUTTONDBLCLK?2:GET_XBUTTON_WPARAM(wp)==XBUTTON1?3:4;
            bool down=message!=WM_LBUTTONUP&&message!=WM_RBUTTONUP&&message!=WM_MBUTTONUP&&message!=WM_XBUTTONUP;
            bool changed=s.mouse.down[button]!=down;consume=s.mouse.event(button,down,active);
            if(active&&changed)event_locked(s,{OverlayMenuInputEventKind::mouse_button,0,0,button,down});
        } else if(active&&(message==WM_MOUSEWHEEL||message==WM_MOUSEHWHEEL)) {
            // Raw wheel is authoritative when present, legacy otherwise.
            if(!s.raw_mouse_seen)event_locked(s,{OverlayMenuInputEventKind::wheel,message==WM_MOUSEHWHEEL?float(GET_WHEEL_DELTA_WPARAM(wp))/WHEEL_DELTA:0,
                message==WM_MOUSEWHEEL?float(GET_WHEEL_DELTA_WPARAM(wp))/WHEEL_DELTA:0});consume=true;
        } else if(active&&(message==WM_SETCURSOR||message==WM_MOUSEACTIVATE))consume=true;
    }
    if(toggle) {
        std::uint64_t owner{};bool current{};{std::scoped_lock lock(s.mutex);current=s.open;if(!s.owners.empty())owner=*s.owners.begin();}
        if(current)request_close();else if(owner)request_open(owner);
        consume=true;
    }
    if(close){request_close();consume=true;}
    if(message==WM_NCDESTROY){std::scoped_lock lock(s.mutex);s.procedures.erase(window);if(s.target==window)s.target=nullptr;}
    if(consume)return message==WM_MOUSEACTIVATE?MA_ACTIVATE:0;
    return previous?CallWindowProcW(previous,window,message,wp,lp):DefWindowProcW(window,message,wp,lp);
}
ImGuiKey imgui_key(unsigned key) {
    if(key>='0'&&key<='9')return ImGuiKey(ImGuiKey_0+key-'0');
    if(key>='A'&&key<='Z')return ImGuiKey(ImGuiKey_A+key-'A');
    if(key>=VK_F1&&key<=VK_F24)return ImGuiKey(ImGuiKey_F1+key-VK_F1);
    if(key>=VK_NUMPAD0&&key<=VK_NUMPAD9)return ImGuiKey(ImGuiKey_Keypad0+key-VK_NUMPAD0);
    switch(key) {
    case VK_TAB:return ImGuiKey_Tab;case VK_LEFT:return ImGuiKey_LeftArrow;case VK_RIGHT:return ImGuiKey_RightArrow;
    case VK_UP:return ImGuiKey_UpArrow;case VK_DOWN:return ImGuiKey_DownArrow;case VK_PRIOR:return ImGuiKey_PageUp;
    case VK_NEXT:return ImGuiKey_PageDown;case VK_HOME:return ImGuiKey_Home;case VK_END:return ImGuiKey_End;
    case VK_INSERT:return ImGuiKey_Insert;case VK_DELETE:return ImGuiKey_Delete;case VK_BACK:return ImGuiKey_Backspace;
    case VK_SPACE:return ImGuiKey_Space;case VK_RETURN:return ImGuiKey_Enter;case VK_ESCAPE:return ImGuiKey_Escape;
    case VK_LCONTROL:return ImGuiKey_LeftCtrl;case VK_RCONTROL:return ImGuiKey_RightCtrl;
    case VK_LSHIFT:return ImGuiKey_LeftShift;case VK_RSHIFT:return ImGuiKey_RightShift;
    case VK_LMENU:return ImGuiKey_LeftAlt;case VK_RMENU:return ImGuiKey_RightAlt;
    case VK_LWIN:return ImGuiKey_LeftSuper;case VK_RWIN:return ImGuiKey_RightSuper;case VK_APPS:return ImGuiKey_Menu;
    case VK_OEM_1:return ImGuiKey_Semicolon;case VK_OEM_PLUS:return ImGuiKey_Equal;case VK_OEM_COMMA:return ImGuiKey_Comma;
    case VK_OEM_MINUS:return ImGuiKey_Minus;case VK_OEM_PERIOD:return ImGuiKey_Period;case VK_OEM_2:return ImGuiKey_Slash;
    case VK_OEM_3:return ImGuiKey_GraveAccent;case VK_OEM_4:return ImGuiKey_LeftBracket;case VK_OEM_5:return ImGuiKey_Backslash;
    case VK_OEM_6:return ImGuiKey_RightBracket;case VK_OEM_7:return ImGuiKey_Apostrophe;
    case VK_CAPITAL:return ImGuiKey_CapsLock;case VK_SCROLL:return ImGuiKey_ScrollLock;case VK_NUMLOCK:return ImGuiKey_NumLock;
    case VK_SNAPSHOT:return ImGuiKey_PrintScreen;case VK_PAUSE:return ImGuiKey_Pause;
    case VK_DECIMAL:return ImGuiKey_KeypadDecimal;case VK_DIVIDE:return ImGuiKey_KeypadDivide;case VK_MULTIPLY:return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT:return ImGuiKey_KeypadSubtract;case VK_ADD:return ImGuiKey_KeypadAdd;
    default:return ImGuiKey_None;
    }
}
}
void overlay_input_detail::reduce_raw_buffer(void* data,std::size_t byte_capacity,unsigned packet_count) {
    if(!data||byte_capacity<sizeof(RAWINPUTHEADER))return;
    auto& s=state();std::scoped_lock lock(s.mutex);if(s.owners.empty())return;
    auto* current=static_cast<RAWINPUT*>(data);auto* end=static_cast<BYTE*>(data)+byte_capacity;
    for(unsigned i=0;i<packet_count&&reinterpret_cast<BYTE*>(current)+sizeof(RAWINPUTHEADER)<=end;++i) {
        if(current->header.dwSize<sizeof(RAWINPUTHEADER)||reinterpret_cast<BYTE*>(current)+current->header.dwSize>end)break;
        if((current->header.dwType==RIM_TYPEMOUSE&&current->header.dwSize>=offsetof(RAWINPUT,data)+sizeof(RAWMOUSE))||
           (current->header.dwType==RIM_TYPEKEYBOARD&&current->header.dwSize>=offsetof(RAWINPUT,data)+sizeof(RAWKEYBOARD)))
            sanitize(*current,process_raw_locked(s,nullptr,*current));
        constexpr std::uintptr_t alignment=sizeof(void*);
        auto next=(reinterpret_cast<std::uintptr_t>(current)+current->header.dwSize+alignment-1)&~(alignment-1);
        current=reinterpret_cast<RAWINPUT*>(next);
    }
}
OverlayMenuInput::OverlayMenuInput(){install_hooks();discover_direct_input();auto& s=state();std::scoped_lock lock(s.mutex);owner_=s.next_owner++;s.owners.insert(owner_);}
OverlayMenuInput::~OverlayMenuInput(){
    close();auto& s=state();HWND window{};bool last{};std::uint64_t revision{};
    {std::scoped_lock lock(s.mutex);s.owners.erase(owner_);last=s.owners.empty();window=s.capture.window?s.capture.window:s.target;revision=s.revision;}
    if(!last)return;
    if(window&&IsWindow(window)&&GetWindowThreadProcessId(window,nullptr)!=GetCurrentThreadId()) {
        DWORD_PTR ignored{};SendMessageTimeoutW(window,control_message,control_cookie,LPARAM(revision),SMTO_ABORTIFHUNG|SMTO_BLOCK,50,&ignored);
    }
    if(window&&GetWindowThreadProcessId(window,nullptr)==GetCurrentThreadId()&&!window_dispatch_depth)service_capture(window,revision);
    detach_windows_if_idle();
}
namespace {
void detach_windows_if_idle() {
    auto& s=state();
    std::scoped_lock installation(s.install_mutex);
    std::vector<std::pair<HWND,WNDPROC>> procedures;
    {std::scoped_lock lock(s.mutex);if(!s.owners.empty()||s.phase!=CapturePhase::idle)return;for(auto pair:s.procedures)procedures.push_back(pair);}
    for(auto [hwnd,previous]:procedures)if(IsWindow(hwnd)&&reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd,GWLP_WNDPROC))==window_proc) {
        SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous));
        std::scoped_lock lock(s.mutex);s.procedures.erase(hwnd);
    }
    // A later WndProc hook may still chain to us; keep its original procedure
    // record alive and pass through. No game chain is overwritten to unhook it.
}
}
bool OverlayMenuInput::set_open(bool open){if(!open){close();return true;}return request_open(owner_);}
void OverlayMenuInput::close() noexcept {auto& s=state();bool owned{};{std::scoped_lock lock(s.mutex);owned=s.active_owner==owner_||(!s.open&&s.phase!=CapturePhase::idle);}if(owned)request_close();}
void OverlayMenuInput::offset_pointer(float delta_x,float delta_y) noexcept {
    auto& s=state();std::scoped_lock lock(s.mutex);
    if(!s.open||s.phase!=CapturePhase::active||s.active_owner!=owner_)return;
    const float old_x=s.cursor.x,old_y=s.cursor.y;s.cursor.relative(delta_x,delta_y);
    const float applied_x=s.cursor.x-old_x,applied_y=s.cursor.y-old_y;
    for(auto& event:s.events)if(event.kind==OverlayMenuInputEventKind::mouse_position) {
        event.x=std::clamp(event.x+applied_x,0.F,float(s.cursor.width-1));
        event.y=std::clamp(event.y+applied_y,0.F,float(s.cursor.height-1));
    }
    position_locked(s);
}
OverlayMenuInputFrame OverlayMenuInput::poll(unsigned width,unsigned height) {
    discover_direct_input();auto& s=state();HWND foreground=foreground_owned();bool lost{};
    {std::scoped_lock lock(s.mutex);lost=s.open&&s.target!=foreground;}
    if(lost)request_close();
    HWND control_window{};std::uint64_t revision{};
    {std::scoped_lock lock(s.mutex);control_window=s.capture.window;revision=s.revision;}
    if(control_window&&GetWindowThreadProcessId(control_window,nullptr)==GetCurrentThreadId()&&!window_dispatch_depth)service_capture(control_window,revision);
    if(foreground)attach(foreground);
    const auto keys=autonomous::keybinds();
    const bool toggle_down=keybind_matches(keys.bindings[KeybindAction::open_interface],[](unsigned k){return (physical_key(int(k))&0x8000)!=0;}),escape=(physical_key(VK_ESCAPE)&0x8000)!=0;
    bool toggle{},closing{};
    {std::scoped_lock lock(s.mutex);
        toggle=foreground&&!keys.suspended&&toggle_down&&!s.toggle_latched;s.toggle_latched=toggle_down;
        closing=s.open&&escape&&!s.esc_latched;s.esc_latched=escape;
    }
    if(toggle){bool open{};{std::scoped_lock lock(s.mutex);open=s.open;}if(open)request_close();else request_open(owner_);}
    if(closing)request_close();
    std::array<bool,256> physical{};
    for(unsigned k=0;k<256;++k)physical[k]=(physical_key(int(k))&0x8000)!=0;
    OverlayMenuInputFrame frame;
    {std::scoped_lock lock(s.mutex);
        const float scale_x=float(std::max(1u,width))/s.cursor.width,scale_y=float(std::max(1u,height))/s.cursor.height;
        for(auto& event:s.events)if(event.kind==OverlayMenuInputEventKind::mouse_position){event.x*=scale_x;event.y*=scale_y;}
        s.cursor.resize(width,height);frame.open=s.open&&s.phase==CapturePhase::active&&s.active_owner==owner_;frame.focused=frame.open&&s.focused&&s.target==foreground;
        frame.requested_open=s.open&&s.active_owner==owner_;
        frame.transitioning=s.phase==CapturePhase::acquiring||s.phase==CapturePhase::restoring||(!s.open&&s.phase==CapturePhase::active);
        frame.mouse_valid=frame.focused;frame.mouse_x=s.cursor.x;frame.mouse_y=s.cursor.y;
        if(frame.open) {
            frame.keys=physical;
            for(unsigned b=0;b<5;++b)frame.mouse_down[b]=s.mouse.down[b];
            frame.events.swap(s.events);
            for(const auto& event:frame.events)if(event.kind==OverlayMenuInputEventKind::wheel){frame.wheel_x+=event.x;frame.wheel_y+=event.y;}
        } else {s.events.clear();}
    }
    return frame;
}
void apply_imgui_input(const OverlayMenuInputFrame& frame) {
    if(!ImGui::GetCurrentContext())return;
    auto& io=ImGui::GetIO();io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor=frame.open&&frame.mouse_valid;
    io.AddFocusEvent(frame.open&&frame.focused);
    for(const auto& event:frame.events) {
        switch(event.kind) {
        case OverlayMenuInputEventKind::mouse_position:io.AddMousePosEvent(event.x,event.y);break;
        case OverlayMenuInputEventKind::mouse_button:if(event.code<5)io.AddMouseButtonEvent(int(event.code),event.down);break;
        case OverlayMenuInputEventKind::wheel:io.AddMouseWheelEvent(event.x,event.y);break;
        case OverlayMenuInputEventKind::key:if(auto key=imgui_key(event.code);key!=ImGuiKey_None)io.AddKeyEvent(key,event.down);break;
        case OverlayMenuInputEventKind::text:io.AddInputCharacter(event.code);break;
        case OverlayMenuInputEventKind::focus:io.AddFocusEvent(event.down);break;
        }
    }
    io.AddMousePosEvent(frame.mouse_valid?frame.mouse_x:-FLT_MAX,frame.mouse_valid?frame.mouse_y:-FLT_MAX);
    for(unsigned b=0;b<5;++b)io.AddMouseButtonEvent(int(b),frame.open&&frame.mouse_down[b]);
    for(unsigned k=0;k<256;++k)if(auto key=imgui_key(k);key!=ImGuiKey_None)io.AddKeyEvent(key,frame.open&&frame.keys[k]);
    io.AddKeyEvent(ImGuiMod_Ctrl,frame.open&&(frame.keys[VK_CONTROL]||frame.keys[VK_LCONTROL]||frame.keys[VK_RCONTROL]));
    io.AddKeyEvent(ImGuiMod_Shift,frame.open&&(frame.keys[VK_SHIFT]||frame.keys[VK_LSHIFT]||frame.keys[VK_RSHIFT]));
    io.AddKeyEvent(ImGuiMod_Alt,frame.open&&(frame.keys[VK_MENU]||frame.keys[VK_LMENU]||frame.keys[VK_RMENU]));
    io.AddKeyEvent(ImGuiMod_Super,frame.open&&(frame.keys[VK_LWIN]||frame.keys[VK_RWIN]));
}
}
