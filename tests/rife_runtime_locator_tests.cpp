#include "xrfg/rife_runtime_locator.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void seed(const std::filesystem::path& root){
    std::filesystem::create_directories(root/"python");std::filesystem::create_directories(root/"app");
    std::ofstream(root/"python/python.exe").put('x');std::ofstream(root/"app/worker.py").put('x');
}
}
int main(){
    namespace fs=std::filesystem;
    wchar_t original[32768]{};const auto previous=GetEnvironmentVariableW(L"LOCALAPPDATA",original,32768);
    const auto root=fs::temp_directory_path()/(L"OFXR AppData runtime test "+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        const auto local=root/"local appdata",managed=local/"ofxr-bridge/RIFE/runtime";
        const auto package=root/"lab/packages/folder with spaces",shared=root/"lab/runtime/rife",bundled=package/"rife";
        fs::create_directories(package);seed(shared);seed(bundled);
        check(SetEnvironmentVariableW(L"LOCALAPPDATA",local.c_str()),"isolated AppData");
        check(xrfg::staged_rife_runtime(package).empty(),"missing AppData must not use an ancestor or package runtime");
        seed(managed);
        check(xrfg::staged_rife_runtime(package)==managed,"AppData runtime is the only default");
        fs::remove(managed/"app/worker.py");
        check(xrfg::staged_rife_runtime(package).empty(),"incomplete AppData must not fall back");
        seed(managed);
        {std::ofstream f(package/"ofxr_bridge.ini");f<<"[ofxr]\nrife_runtime_dir="<<shared.generic_string()<<"\n";}
        check(xrfg::staged_rife_runtime(package)==shared,"manual absolute developer override");
        const auto native=root/"radeon runtime";
        fs::create_directories(native/"vulkan");
        std::ofstream(native/"vulkan/backend.json")<<"{}";
        check(!xrfg::valid_rife_runtime(native),"a Radeon marker without its native worker is incomplete");
        std::ofstream(native/"vulkan/ofxr_rife_vulkan.exe").put('x');
        check(xrfg::valid_rife_runtime(native),"native Radeon worker is a valid runtime without CUDA Python");
        {std::ofstream f(package/"ofxr_bridge.ini");f<<"[ofxr]\nrife_runtime_dir="<<native.generic_string()<<"\n";}
        check(xrfg::staged_rife_runtime(package)==native,"explicit native Radeon runtime selection");
        {std::ofstream f(package/"ofxr_bridge.ini");f<<"[ofxr]\nrife_runtime_dir=../runtime/rife\n";}
        check(xrfg::staged_rife_runtime(package).empty(),"relative manual override must fail, never trigger search");
        {std::ofstream f(package/"ofxr_bridge.ini");f<<"[ofxr]\nrife_runtime_dir="<<(root/"missing").generic_string()<<"\n";}
        check(xrfg::staged_rife_runtime(package).empty(),"invalid explicit override does not silently select another runtime");
        {std::ofstream f(package/"ofxr_bridge.ini");f<<"[ofxr]\nrife_runtime_dir=\n";}
        check(xrfg::staged_rife_runtime(package)==managed,"empty override restores AppData");
        fs::remove(package/"ofxr_bridge.ini");
        const auto moved=root/"elsewhere/package";fs::create_directories(moved.parent_path());fs::rename(package,moved);
        check(xrfg::staged_rife_runtime(moved)==managed,"moving the tray never changes the runtime default");
        if(previous&&previous<32768)SetEnvironmentVariableW(L"LOCALAPPDATA",original);else SetEnvironmentVariableW(L"LOCALAPPDATA",nullptr);
        check(fs::equivalent(root.parent_path(),fs::temp_directory_path())&&root.filename().wstring().starts_with(L"OFXR AppData runtime test "),"bounded temporary cleanup");
        fs::remove_all(root);
        std::cout<<"AppData-only RIFE: missing/damaged runtime, ignored ancestors/bundle, manual override and relocation PASS\n";return 0;
    }catch(const std::exception& e){if(previous&&previous<32768)SetEnvironmentVariableW(L"LOCALAPPDATA",original);else SetEnvironmentVariableW(L"LOCALAPPDATA",nullptr);std::cerr<<e.what()<<'\n';return 1;}
}
