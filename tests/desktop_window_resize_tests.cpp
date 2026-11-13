#include "xrfg/autonomous_ui.hpp"
#include "xrfg/ui_language.hpp"
#include "xrfg/autonomous_control.hpp"
#include "imgui_impl_win32.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace xrfg::autonomous;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main() {
    const auto directory=std::filesystem::temp_directory_path()/(L"ofxr-ui-resize-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",directory.c_str());
    try {
        require(initialize_user_configuration(),"isolated configuration initialized");
        require(show_settings_window(nullptr),"real desktop settings window opens");
        HWND window{};
        EnumThreadWindows(GetCurrentThreadId(),[](HWND candidate,LPARAM output)->BOOL {
            wchar_t name[80]{};GetClassNameW(candidate,name,80);
            if(wcscmp(name,L"OFXRBridgeSettingsV449")==0){*reinterpret_cast<HWND*>(output)=candidate;return FALSE;}return TRUE;
        },reinterpret_cast<LPARAM>(&window));
        require(window!=nullptr,"own settings window found");
        int previous_width{};
        for(const auto dimensions:{SIZE{900,650},SIZE{1180,820}}) {
            const auto dpi=ImGui_ImplWin32_GetDpiScaleForHwnd(window);
            RECT frame{0,0,LONG(dimensions.cx*dpi),LONG(dimensions.cy*dpi)};
            AdjustWindowRectExForDpi(&frame,WS_OVERLAPPEDWINDOW,FALSE,WS_EX_APPWINDOW,UINT(96*dpi));
            SetWindowPos(window,nullptr,0,0,frame.right-frame.left,frame.bottom-frame.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            RECT client{};GetClientRect(window,&client);
            const auto path=directory/L"preview.bmp";
            require(save_settings_preview(path,0,false),"render resized native backbuffer");
            std::ifstream file(path,std::ios::binary);BITMAPFILEHEADER header{};BITMAPINFOHEADER bitmap{};
            file.read(reinterpret_cast<char*>(&header),sizeof(header));file.read(reinterpret_cast<char*>(&bitmap),sizeof(bitmap));
            require(bitmap.biWidth==client.right && -bitmap.biHeight==client.bottom,"capture matches native client pixels instead of a fixed bitmap");
            require(bitmap.biWidth!=previous_width,"resizing changes the actual rendering resolution");previous_width=bitmap.biWidth;
        }
        require(set_ui_language(UiLanguage::simplified_chinese),"Chinese selected");
        require(save_settings_preview(directory/L"preview.bmp",0,false),"same native window renders Chinese");
        require(set_ui_language(UiLanguage::english),"English restored");
        require(save_settings_preview(directory/L"preview.bmp",0,false),"same native window renders English again");
        close_settings_window();
        for(const auto name:{L"preview.bmp",L"interface.ini",L"ofxr-bridge.ini"})std::filesystem::remove(directory/name);
        std::filesystem::remove(directory);
        std::cout<<"Native desktop resize, physical pixel dimensions and language switching passed\n";return 0;
    }catch(const std::exception& error){close_settings_window();std::cerr<<error.what()<<'\n';return 1;}
}
