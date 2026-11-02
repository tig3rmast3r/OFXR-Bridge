#include "xrfg/overlay_menu_input.hpp"
#include "xrfg/autonomous_control.hpp"
#include <filesystem>
#include "xrfg/overlay_menu_input_state.hpp"
#include "imgui.h"
#include <windows.h>
#include <windowsx.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <atomic>
#include <thread>

namespace {
unsigned checks{};
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
bool approx(float a,float b){return std::abs(a-b)<.01F;}
unsigned game_key_down{},game_key_up{},game_mouse_down{},game_mouse_up{},game_moves{};
xrfg::OverlayMenuInput* callback_input{};
bool callback_reopen{},callback_accepted{};
unsigned callback_attempts{};
LRESULT CALLBACK fixture_proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_KEYDOWN)++game_key_down;if(msg==WM_KEYUP)++game_key_up;
    if(msg==WM_LBUTTONDOWN)++game_mouse_down;if(msg==WM_LBUTTONUP)++game_mouse_up;
    if(msg==WM_MOUSEMOVE)++game_moves;
    if(msg==WM_CAPTURECHANGED&&callback_reopen&&callback_input) {
        callback_reopen=false;++callback_attempts;
        callback_accepted=callback_input->set_open(true);
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
RECT desktop_rect(){RECT r{GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),0,0};r.right=r.left+GetSystemMetrics(SM_CXVIRTUALSCREEN);r.bottom=r.top+GetSystemMetrics(SM_CYVIRTUALSCREEN);return r;}
bool equal_rect(const RECT& a,const RECT& b){return EqualRect(&a,&b)!=FALSE;}
std::vector<RAWINPUTDEVICE> registrations() {
    UINT count{};GetRegisteredRawInputDevices(nullptr,&count,sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> devices(count);
    if(count)require(GetRegisteredRawInputDevices(devices.data(),&count,sizeof(RAWINPUTDEVICE))!=UINT(-1),"query raw registrations");
    devices.resize(count);return devices;
}
bool registered_usage(unsigned usage,HWND hwnd){for(auto r:registrations())if(r.usUsagePage==1&&r.usUsage==usage&&r.hwndTarget==hwnd)return true;return false;}
struct Window {
    HWND hwnd{},previous_foreground{};RECT previous_clip{};
    Window() {
        previous_foreground=GetForegroundWindow();GetClipCursor(&previous_clip);
        WNDCLASSW wc{};wc.lpfnWndProc=fixture_proc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"OFXRMenuInputFixture";
        RegisterClassW(&wc);
        hwnd=CreateWindowExW(WS_EX_TOOLWINDOW,wc.lpszClassName,L"OFXR input fixture",WS_POPUP,130,170,512,384,nullptr,nullptr,wc.hInstance,nullptr);
        require(hwnd!=nullptr,"create owned fixture window");
        // A CTest child is often launched after the shell's foreground grant
        // expired. Temporarily join the current foreground input queue while
        // activating our owned fixture; keep the real foreground assertion.
        ShowWindow(hwnd,SW_SHOWNORMAL);
        const DWORD current_thread=GetCurrentThreadId();
        const DWORD foreground_thread=GetWindowThreadProcessId(GetForegroundWindow(),nullptr);
        const bool attached=foreground_thread && foreground_thread!=current_thread &&
            AttachThreadInput(current_thread,foreground_thread,TRUE)!=FALSE;
        for(unsigned attempt=0;attempt<50 && GetForegroundWindow()!=hwnd;++attempt) {
            SetForegroundWindow(hwnd);SetFocus(hwnd);
            MSG message{};
            while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                TranslateMessage(&message);DispatchMessageW(&message);
            }
            if(GetForegroundWindow()!=hwnd)Sleep(5);
        }
        if(attached)AttachThreadInput(current_thread,foreground_thread,FALSE);
        require(GetForegroundWindow()==hwnd,"fixture owns actual foreground");
    }
    ~Window(){ClipCursor(nullptr);ReleaseCapture();if(hwnd)DestroyWindow(hwnd);if(previous_foreground&&IsWindow(previous_foreground))SetForegroundWindow(previous_foreground);ClipCursor(&previous_clip);}
};
struct DirectDevices {
    HMODULE module{};IDirectInput8W* input{};IDirectInputDevice8W* mouse{};IDirectInputDevice8W* keyboard{};
    explicit DirectDevices(HWND hwnd) {
        // Devices exist BEFORE OverlayMenuInput installs its late hooks.
        module=LoadLibraryExW(L"dinput8.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);require(module!=nullptr,"load actual system DirectInput");
        using Factory=HRESULT(WINAPI*)(HINSTANCE,DWORD,REFIID,void**,IUnknown*);
        auto factory=reinterpret_cast<Factory>(GetProcAddress(module,"DirectInput8Create"));
        constexpr GUID iid{0xbf798031,0x483a,0x4da2,{0xaa,0x99,0x5d,0x64,0xed,0x36,0x97,0x00}};
        require(factory&&SUCCEEDED(factory(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,iid,reinterpret_cast<void**>(&input),nullptr)),"create pre-existing DirectInput interface");
        require(SUCCEEDED(input->CreateDevice(GUID_SysMouse,&mouse,nullptr))&&SUCCEEDED(input->CreateDevice(GUID_SysKeyboard,&keyboard,nullptr)),"create pre-existing native mouse and keyboard devices");
        require(SUCCEEDED(mouse->SetDataFormat(&c_dfDIMouse2))&&SUCCEEDED(keyboard->SetDataFormat(&c_dfDIKeyboard)),"configure native DirectInput data formats");
        for(auto* device:{mouse,keyboard}) {
            require(SUCCEEDED(device->SetCooperativeLevel(hwnd,DISCL_NONEXCLUSIVE|DISCL_FOREGROUND)),"native DirectInput cooperative window");
            DIPROPDWORD property{};property.diph.dwSize=sizeof(property);property.diph.dwHeaderSize=sizeof(DIPROPHEADER);property.diph.dwHow=DIPH_DEVICE;property.dwData=32;
            require(SUCCEEDED(device->SetProperty(DIPROP_BUFFERSIZE,&property.diph)),"configure native buffered DirectInput");
            require(SUCCEEDED(device->Acquire()),"acquire existing native DirectInput device");
        }
    }
    ~DirectDevices(){if(mouse){mouse->Unacquire();mouse->Release();}if(keyboard){keyboard->Unacquire();keyboard->Release();}if(input)input->Release();if(module)FreeLibrary(module);}
    void check_masked() {
        BYTE keys[256];std::memset(keys,0xFF,sizeof(keys));DIMOUSESTATE2 movement{};movement.lX=movement.lY=movement.lZ=555;
        require(SUCCEEDED(keyboard->GetDeviceState(sizeof(keys),keys)),"existing native DirectInput keyboard still returns success");
        for(BYTE key:keys)require(key==0,"existing native keyboard state masked while menu open");
        require(SUCCEEDED(mouse->GetDeviceState(sizeof(movement),&movement)),"existing native DirectInput mouse still returns success");
        require(movement.lX==0&&movement.lY==0&&movement.lZ==0,"existing native mouse motion masked while menu open");
        for(BYTE button:movement.rgbButtons)require(button==0,"existing native mouse buttons masked");
        for(auto* device:{mouse,keyboard}) {
            DIDEVICEOBJECTDATA data[32]{};DWORD count=32;
            require(SUCCEEDED(device->GetDeviceData(sizeof(DIDEVICEOBJECTDATA),data,&count,DIGDD_PEEK))&&count==0,"existing native buffered device cannot bypass menu mask");
        }
    }
};
void cursor_and_mask() {
    using namespace xrfg::overlay_input_detail;
    VirtualCursor raw;raw.relative(31,-19);require(approx(raw.x,543)&&approx(raw.y,365),"raw relative position is drawn framebuffer position");
    raw.relative(-10000,10000);require(raw.x==0&&raw.y==767,"raw cursor clamps inside framebuffer");
    raw.relative(NAN,4);require(raw.x==0&&raw.y==767,"nonfinite input cannot corrupt cursor");
    VirtualCursor one,two;one.client(150,200,512,384);one.client(182,216,512,384);
    two.client(350,500,1024,768);two.client(414,532,1024,768);
    require(approx(one.x,two.x)&&approx(one.y,two.y),"different desktop origin and 200 percent DPI keep identical client delta");
    require(approx(one.x,576)&&approx(one.y,416),"client delta scales into framebuffer coordinates");
    VirtualCursor absolute;absolute.absolute(40000,30000);require(absolute.x==512&&absolute.y==384,"first absolute raw packet seeds without teleport");
    absolute.absolute(41000,29000);require(approx(absolute.x,512+1000*1024.F/65535)&&approx(absolute.y,384-1000*768.F/65535),"absolute device contributes relative normalized delta");
    one.resize(2048,1536);require(approx(one.x,1152)&&approx(one.y,832),"framebuffer resize preserves cursor fraction");
    one.resize(0,0);require(one.x==0&&one.y==0,"zero dimensions safely clamp to one pixel");
    ButtonMask keys;require(!keys.event('W',true,false),"game press before menu passes");
    require(keys.event('W',true,true),"repeat is hidden while menu open");
    require(!keys.event('W',false,true),"release for pre-menu game press still passes");
    require(keys.event('Q',true,true),"menu press masked");require(keys.event('Q',false,true),"menu release masked");
    require(keys.event('E',true,true),"menu held press masked");require(keys.event('E',false,false),"closing menu preserves matching release ownership");
    require(!keys.event('E',true,false)&&!keys.event('E',false,false),"subsequent normal game input resumes");
    BufferedButtons buffered;
    require(!buffered.suppress(0x12340,true,false),"custom DirectInput offset game press is forwarded");
    auto peek=buffered;
    require(!peek.suppress(0x12340,false,true),"PEEK working copy forwards release owed to game");
    require(buffered.offsets[0x12340][0],"PEEK does not advance real buffered press ownership");
    require(!buffered.suppress(0x12340,false,true),"actual buffered read forwards same owed release");
    require(!buffered.offsets[0x12340][0],"actual buffered read advances release ownership");
    require(buffered.suppress(0x200000,true,true)&&buffered.suppress(0x200000,false,true),"custom DirectInput offset menu press and release remain masked");
    auto menu_peek=buffered;
    require(menu_peek.suppress(0x300000,true,true),"PEEK masks a new menu button press");
    require(!buffered.offsets.contains(0x300000),"PEEK cannot create ownership in real device state");
}
void controller_and_imgui() {
    Window window;
    DirectDevices direct(window.hwnd);
    auto original=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window.hwnd,GWLP_WNDPROC));
    auto saved_registrations=registrations();
    {
        xrfg::OverlayMenuInput input;
        require(!input.poll().open,"menu starts closed");
        const unsigned insert_before=game_key_down;
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_INSERT,0);SendMessageW(window.hwnd,WM_KEYUP,VK_INSERT,0);
        require(game_key_down==insert_before+1&&!input.poll().open,"closed menu preserves UEVR Insert binding");
        const unsigned toggle_before=game_key_down;
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F10,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F10,0);
        require(input.poll().open&&game_key_down==toggle_before,"F10 opens OFXR and masks its own gesture");
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F10,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F10,0);
        require(!input.poll().open&&game_key_down==toggle_before,"F10 closes OFXR with balanced gesture ownership");
        xrfg::Keybinds remapped;remapped.keys[0]={0,VK_F17};
        require(xrfg::autonomous::publish_keybinds(remapped),"publish live interface remap");
        const unsigned old_key_before=game_key_down;
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F10,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F10,0);
        require(!input.poll().open&&game_key_down==old_key_before+1,"old interface key returns to game after remapping");
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F17,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F17,0);
        require(input.poll().open,"new interface key opens without restarting input hooks");
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F17,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F17,0);
        require(!input.poll().open,"new interface key closes with balanced ownership");
        require(xrfg::autonomous::suspend_keybinds(true),"suspend interface while configuring");
        SendMessageW(window.hwnd,WM_KEYDOWN,VK_F17,0);SendMessageW(window.hwnd,WM_KEYUP,VK_F17,0);
        require(!input.poll().open,"capture gesture cannot open interface");
        xrfg::autonomous::suspend_keybinds(false);xrfg::autonomous::publish_keybinds({});
        SendMessageW(window.hwnd,WM_KEYDOWN,'W',0);SendMessageW(window.hwnd,WM_LBUTTONDOWN,0,0);
        unsigned key_up_before=game_key_up,mouse_up_before=game_mouse_up;
        SetCapture(window.hwnd);
        require(input.set_open(true),"explicit open accepts actual foreground owned HWND");
        require(input.poll().open,"production poll returns open menu");
        direct.check_masked();
        require(GetCapture()==window.hwnd,"menu owns temporary foreground window capture independently of its virtual cursor");
        require(ReleaseCapture()!=FALSE&&GetCapture()==window.hwnd,"game release request cannot drop temporary menu capture");
        require(registered_usage(2,window.hwnd)&&registered_usage(6,window.hwnd),"raw mouse and keyboard delivery attached to actual HWND");
        SendMessageW(window.hwnd,WM_KEYDOWN,'W',0);SendMessageW(window.hwnd,WM_KEYUP,'W',0);
        SendMessageW(window.hwnd,WM_LBUTTONUP,0,0);
        require(game_key_up==key_up_before+1&&game_mouse_up==mouse_up_before+1,"window chain receives game-held releases while menu open");
        const unsigned keys_before=game_key_down,mouse_before=game_mouse_down;
        SendMessageW(window.hwnd,WM_KEYDOWN,'Q',0);SendMessageW(window.hwnd,WM_KEYUP,'Q',0);
        SendMessageW(window.hwnd,WM_LBUTTONDOWN,0,0);SendMessageW(window.hwnd,WM_LBUTTONUP,0,0);
        require(game_key_down==keys_before&&game_mouse_down==mouse_before,"menu input is not forwarded to game");
        SendMessageW(window.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(200,100));
        SendMessageW(window.hwnd,WM_MOUSEMOVE,0,MAKELPARAM(216,108));
        auto frame=input.poll(1024,768);
        require(approx(frame.mouse_x,544)&&approx(frame.mouse_y,400),"production fallback moves virtual cursor by client delta without desktop origin projection");
        require(frame.mouse_valid&&frame.focused,"valid mouse belongs to focused headset menu");
        unsigned ordered_buttons{};for(auto e:frame.events)if(e.kind==xrfg::OverlayMenuInputEventKind::mouse_button)++ordered_buttons;
        require(ordered_buttons>=2,"ordered event queue preserves press and release between XR frames");
        POINT stable{},after{};GetCursorPos(&stable);require(SetCursorPos(stable.x+35,stable.y+30)!=FALSE,"game recenter reports success");GetCursorPos(&after);
        require(stable.x==after.x&&stable.y==after.y,"game recenter cannot move menu pointer or stable game cursor");
        RECT desktop=desktop_rect(),actual{};GetClipCursor(&actual);require(equal_rect(actual,desktop),"menu releases cursor confinement");
        RECT requested{std::max(desktop.left,stable.x-100),std::max(desktop.top,stable.y-100),
            std::min(desktop.right,stable.x+100),std::min(desktop.bottom,stable.y+100)};ClipCursor(&requested);GetClipCursor(&actual);
        require(equal_rect(actual,desktop),"game clip request deferred while menu open");
        SetCapture(window.hwnd);require(GetCapture()==window.hwnd,"game capture request cannot replace temporary menu capture");
        RAWINPUTDEVICE remove{1,2,RIDEV_REMOVE,nullptr};require(RegisterRawInputDevices(&remove,1,sizeof(remove))!=FALSE,"game raw removal accepted while menu open");
        require(registered_usage(2,window.hwnd),"deferred raw removal keeps menu delivery alive");
        auto context=ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().DisplaySize=ImVec2(1024,768);ImGui::GetIO().DeltaTime=1.F/90;
        ImGui::GetIO().BackendFlags|=ImGuiBackendFlags_RendererHasTextures;
        // Verify position separately from Dear ImGui's intentional trickling
        // of several press/release transitions into subsequent UI frames.
        auto position_frame=frame;position_frame.events.clear();
        xrfg::apply_imgui_input(position_frame);ImGui::NewFrame();
        require(ImGui::GetIO().MouseDrawCursor,"renderer draws cursor in its own ImGui context");
        require(approx(ImGui::GetIO().MousePos.x,frame.mouse_x)&&approx(ImGui::GetIO().MousePos.y,frame.mouse_y),"drawn ImGui pointer and production framebuffer coordinates coincide");
        ImGui::EndFrame();ImGui::DestroyContext(context);
        RAWINPUT raw[2]{};
        for(auto& packet:raw){packet.header.dwType=RIM_TYPEMOUSE;packet.header.dwSize=sizeof(RAWINPUT);}
        raw[0].data.mouse.lLastX=37;raw[0].data.mouse.lLastY=-11;
        raw[1].data.mouse.lLastX=-7;raw[1].data.mouse.lLastY=3;
        xrfg::overlay_input_detail::reduce_raw_buffer(raw,sizeof(raw),2);
        auto raw_frame=input.poll();
        require(approx(raw_frame.mouse_x,frame.mouse_x+30)&&approx(raw_frame.mouse_y,frame.mouse_y-8),"production buffered Raw Input reducer records each relative packet exactly once");
        require(raw[0].data.mouse.lLastX==0&&raw[0].data.mouse.lLastY==0&&raw[1].data.mouse.lLastX==0&&raw[1].data.mouse.lLastY==0,"production buffered Raw Input sanitizer hides camera motion from game");
        require(raw[0].header.dwSize==sizeof(RAWINPUT)&&raw[1].header.dwType==RIM_TYPEMOUSE,"masked raw packets keep valid driver metadata");
        raw[0].header.dwSize=UINT(-1);raw[0].data.mouse.lLastX=500;
        xrfg::overlay_input_detail::reduce_raw_buffer(raw,sizeof(raw),2);
        require(approx(input.poll().mouse_x,raw_frame.mouse_x),"malformed buffered packet is refused before reading beyond capacity");
        UINT queued_bytes{};require(GetRawInputBuffer(nullptr,&queued_bytes,sizeof(RAWINPUTHEADER))!=UINT(-1),"real Raw Input buffer query still succeeds while menu open");
        input.close();require(!input.poll().open,"close returns menu to closed state");
        GetClipCursor(&actual);if(!equal_rect(actual,requested))std::cerr<<"clip actual "<<actual.left<<","<<actual.top<<","<<actual.right<<","<<actual.bottom<<" requested "<<requested.left<<","<<requested.top<<","<<requested.right<<","<<requested.bottom<<"\n";
        require(equal_rect(actual,requested),"close restores latest game clip request");ClipCursor(nullptr);
        require(GetCapture()==window.hwnd,"close restores latest game capture request");ReleaseCapture();
        require(!registered_usage(2,window.hwnd),"close applies latest raw removal request");
        auto first_restored=registrations();
        for(auto before:saved_registrations)if(!(before.usUsagePage==1&&before.usUsage==2)) {
            bool preserved=false;for(auto after:first_restored)if(before.usUsagePage==after.usUsagePage&&before.usUsage==after.usUsage&&before.dwFlags==after.dwFlags&&before.hwndTarget==after.hwndTarget)preserved=true;
            require(preserved,"close preserves registrations unaffected by deferred game removal");
        }
        SendMessageW(window.hwnd,WM_KEYDOWN,'R',0);SendMessageW(window.hwnd,WM_LBUTTONDOWN,0,0);
        require(input.set_open(true),"reopen after close succeeds");
        RAWINPUT releases[2]{};releases[0].header.dwType=RIM_TYPEKEYBOARD;releases[0].header.dwSize=sizeof(RAWINPUT);
        releases[0].data.keyboard.VKey='R';releases[0].data.keyboard.Flags=RI_KEY_BREAK;releases[0].data.keyboard.Message=WM_KEYUP;
        releases[1].header.dwType=RIM_TYPEMOUSE;releases[1].header.dwSize=sizeof(RAWINPUT);releases[1].data.mouse.usButtonFlags=RI_MOUSE_LEFT_BUTTON_UP;
        xrfg::overlay_input_detail::reduce_raw_buffer(releases,sizeof(releases),2);
        require(releases[0].data.keyboard.VKey=='R'&&releases[0].data.keyboard.Flags==RI_KEY_BREAK,"buffered keyboard keeps release for game-held press");
        require(releases[1].data.mouse.usButtonFlags==RI_MOUSE_LEFT_BUTTON_UP,"buffered mouse keeps release for game-held press");
        input.close();ClipCursor(&requested);
        require(input.set_open(true),"menu can reopen with an active game clip");
        {
            Window other_window;
            require(!input.poll().open,"actual foreground switch closes menu");GetClipCursor(&actual);
            require(equal_rect(actual,desktop),"actual focus loss releases saved game clip instead of trapping cursor in other window");
        }
    }
    require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window.hwnd,GWLP_WNDPROC))==original,"last controller restores the surviving game WndProc");
    auto restored=registrations();
    for(auto before:saved_registrations) {
        // The fixture explicitly requested mouse removal while open. Its
        // latest request replaces the pre-open registration, as in a game.
        if(before.usUsagePage==1&&(before.usUsage==2||before.usUsage==6))continue;
        bool preserved=false;for(auto after:restored)if(before.usUsagePage==after.usUsagePage&&before.usUsage==after.usUsage&&before.dwFlags==after.dwFlags&&before.hwndTarget==after.hwndTarget)preserved=true;
        require(preserved,"session destruction preserves non-mouse/keyboard registrations across actual focus loss");
    }
    require(!registered_usage(2,window.hwnd)&&!registered_usage(6,window.hwnd),"session destruction removes its own raw target registrations");
}
void repeated_input_lifecycle() {
    Window window;DirectDevices direct(window.hwnd);
    auto original=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window.hwnd,GWLP_WNDPROC));
    {
        xrfg::OverlayMenuInput input;input.poll();
        require(input.set_open(true),"reentrant fixture opens menu");
        // Close releases the menu's native capture. The game's original
        // WndProc runs synchronously inside that API and tries to reopen.
        callback_input=&input;callback_reopen=true;callback_accepted=false;
        unsigned before=callback_attempts;input.close();
        require(callback_attempts==before+1,"actual ReleaseCapture synchronously reaches original WndProc");
        require(!callback_accepted&&!input.poll().open,"nested reopening cannot acquire resources during restoration");
        callback_input=nullptr;
        require(input.set_open(true),"reopening succeeds after restoration actually finishes");
        auto pointer=input.poll();
        SendMessageW(window.hwnd,WM_KEYDOWN,'Q',0);
        RAWINPUT packet{};packet.header.dwType=RIM_TYPEMOUSE;packet.header.dwSize=sizeof(packet);packet.data.mouse.lLastX=9;packet.data.mouse.lLastY=-3;
        RAWINPUT queued=packet;queued.data.mouse.lLastX=4;queued.data.mouse.lLastY=2;
        xrfg::overlay_input_detail::reduce_raw_buffer(&queued,sizeof(queued),1);
        input.offset_pointer(-25,15);
        xrfg::overlay_input_detail::reduce_raw_buffer(&packet,sizeof(packet),1);
        auto shifted=input.poll();
        require(approx(shifted.mouse_x,pointer.mouse_x-12)&&approx(shifted.mouse_y,pointer.mouse_y+14),"drag countershift preserves relative motion queued before and after rebasing");
        bool key_preserved{},position_rebased{};
        for(auto event:shifted.events){
            if(event.kind==xrfg::OverlayMenuInputEventKind::key&&event.code=='Q'&&event.down)key_preserved=true;
            if(event.kind==xrfg::OverlayMenuInputEventKind::mouse_position&&approx(event.x,pointer.mouse_x-21)&&approx(event.y,pointer.mouse_y+17))position_rebased=true;
        }
        require(key_preserved&&position_rebased,"countershift rebases queued positions while preserving keyboard events");
        SendMessageW(window.hwnd,WM_KEYUP,'Q',0);
        require(GetCapture()==window.hwnd&&shifted.open,"pointer countershift preserves capture and menu ownership");
        {xrfg::OverlayMenuInput observer;observer.offset_pointer(100,100);}
        require(approx(input.poll().mouse_x,shifted.mouse_x),"different input owner cannot countershift active menu pointer");
        input.offset_pointer(100000,-100000);auto bounded=input.poll();
        require(bounded.mouse_x==1023&&bounded.mouse_y==0,"drag countershift clamps to shared framebuffer");
        input.close();input.offset_pointer(-100,100);
        require(input.poll().mouse_x==1023,"closed owner cannot alter virtual pointer");
        for(unsigned iteration=0;iteration<32;++iteration) {
            require(input.set_open(true)&&input.poll().open,"repeated settings-style reopening succeeds");
            SendMessageW(window.hwnd,WM_KEYDOWN,'Q',0);SendMessageW(window.hwnd,WM_KEYUP,'Q',0);
            SendMessageW(window.hwnd,WM_LBUTTONDOWN,0,0);SendMessageW(window.hwnd,WM_LBUTTONUP,0,0);
            input.offset_pointer(-1,1);input.poll();direct.check_masked();
            require(GetCapture()==window.hwnd,"repeated reopen maintains actual native capture");
            ReleaseCapture();input.close();
            require(!input.poll().open&&GetCapture()==nullptr,"repeated close applies latest game release request");
        }
        // A separate render thread queues acquisition/close. The actual HWND
        // thread pumps those controls, just as in a game's normal message loop.
        std::atomic<bool> finished{},stop{},pending_reported{},pending_proved{};std::atomic<unsigned> worker_checks{};std::atomic<bool> worker_failed{};
        std::thread renderer([&] {
            for(unsigned iteration=0;iteration<24;++iteration) {
                if(stop)break;
                auto deadline=GetTickCount64()+2000;
                while(!input.set_open(true)&&!stop&&GetTickCount64()<deadline)SwitchToThread();
                if(iteration==0) {
                    auto pending=input.poll();
                    pending_proved=!pending.open&&pending.requested_open&&pending.transitioning;
                    // The production frontend must distinguish a pending
                    // acquisition from the user's actual close gesture.
                    if(!pending.open&&!pending.requested_open)input.close();
                    pending_reported=true;
                }
                while(!input.poll().open&&!stop&&GetTickCount64()<deadline)SwitchToThread();
                if(!input.poll().open){worker_failed=true;break;}
                ++worker_checks;input.offset_pointer(-1,1);input.close();
                for(unsigned closed_frame=0;closed_frame<16;++closed_frame)input.close();
                // A quick second request can be refused until the HWND thread
                // retires the previous capture. Retry only after it completes.
                if(iteration+1<24) {
                    while(!input.set_open(true)&&!stop&&GetTickCount64()<deadline)SwitchToThread();
                    if(!input.poll().open) {
                        while(!input.poll().open&&!stop&&GetTickCount64()<deadline)SwitchToThread();
                    }
                    if(!input.poll().open){worker_failed=true;break;}
                    ++worker_checks;input.close();
                }
                // Synchronize with the HWND queue before the next cycle.
                DWORD_PTR ignored{};
                if(!SendMessageTimeoutW(window.hwnd,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2000,&ignored)){worker_failed=true;break;}
            }
            finished=true;
        });
        auto pending_deadline=GetTickCount64()+2000;
        while(!pending_reported&&!finished&&GetTickCount64()<pending_deadline)SwitchToThread();
        auto deadline=GetTickCount64()+10000;
        while(!finished&&GetTickCount64()<deadline) {
            MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
            SwitchToThread();
        }
        bool completed=finished;stop=true;renderer.join();
        require(completed,"render/UI thread capture loop completes without deadlock");
        require(pending_reported&&pending_proved,"render-thread polling preserves requested menu before HWND acquisition pump");
        require(!worker_failed&&worker_checks==47,"cross-thread rapid reopening preserves exact acquisition/retirement ordering");
        input.close();require(!input.poll().open,"cross-thread fixture leaves menu closed");
    }
    require(reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window.hwnd,GWLP_WNDPROC))==original,"repeated controller lifecycle restores original WndProc");
}
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-keybind-input-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try{cursor_and_mask();controller_and_imgui();repeated_input_lifecycle();std::cout<<"Overlay menu input "<<checks<<" checks passed\n";return 0;}catch(const std::exception& e){std::cerr<<"Overlay menu input failure: "<<e.what()<<"\n";return 1;}}
