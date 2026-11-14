#include "xrfg/ui_language.hpp"
#include "xrfg/ui_resolution.hpp"
#include "xrfg/autonomous_control.hpp"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace xrfg::autonomous;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int wmain(int argc,wchar_t**) {
    if(argc>1){refresh_ui_language(true);return ui_language()==UiLanguage::simplified_chinese?0:1;}
    const auto directory=std::filesystem::temp_directory_path()/(L"ofxr-ui-language-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",directory.c_str());
    try {
        refresh_ui_language(true);require(ui_language()==UiLanguage::english,"English remains default");
        require(std::strcmp(ui_translate("Frame generation"),"Frame generation")==0,"default caption unchanged");
        require(set_ui_language(UiLanguage::simplified_chinese),"save Chinese preference");
        require(std::strcmp(ui_translate("Frame generation"),"帧生成")==0,"Chinese lookup");
        require(ui_translate_wide(L"Settings...")==L"设置…","native wide captions");
        require(ui_translate_text("FSR: Active")=="FSR：已启用","shared status translated at presentation");
        require(std::strcmp(ui_translate("Unknown future caption"),"Unknown future caption")==0,"untranslated captions fall back to English");
        wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
        auto command=L"\""+std::wstring(executable)+L"\" child";STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
        require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child),"start independent UI reader");
        require(WaitForSingleObject(child.hProcess,10000)==WAIT_OBJECT_0,"reader exits");DWORD result=1;GetExitCodeProcess(child.hProcess,&result);CloseHandle(child.hThread);CloseHandle(child.hProcess);
        require(result==0,"a second process reads the saved language");
        require(set_ui_language(UiLanguage::english),"switch back to English");
        require(std::strcmp(ui_translate("Frame generation"),"Frame generation")==0,"English restored without restart");
        const auto a=xrfg::menu_resolution(1536,4096,4096),b=xrfg::menu_resolution(2048,4096,4096),c=xrfg::menu_resolution(4096,4096,4096),d=xrfg::menu_resolution(4096,1400,1024);
        require(a.width==1280 && a.height==960,"menu follows a smaller eye viewport");
        require(b.width==1536 && b.height==1152,"menu follows a larger eye viewport");
        require(c.width==2048 && c.height==1536,"memory use is bounded at 2x");
        require(d.width==1280 && d.height==960,"runtime size limits clamp density");
        require(xrfg::menu_resolution(4096,1023,4096).width==0,"too-small runtime fails closed");
        std::filesystem::remove(directory/L"interface.ini");std::filesystem::remove(directory);
        std::cout<<"Language default, switching, persistence, cross-process read, fallback and adaptive resolution passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
