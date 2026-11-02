#include "xrfg/rife_runtime_locator.hpp"
#include "xrfg/rife_runtime_lease.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void seed(const std::filesystem::path& root){std::filesystem::create_directories(root/"python");std::filesystem::create_directories(root/"app");std::ofstream(root/"python/python.exe").put('x');std::ofstream(root/"app/worker.py").put('x');}
int main(){
    namespace fs=std::filesystem;
    wchar_t original[32768]{};const auto previous=GetEnvironmentVariableW(L"LOCALAPPDATA",original,32768);
    const auto temporary=fs::temp_directory_path()/(L"ofxr-rife-locator-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    try {
        require(!fs::exists(temporary),"fresh isolated test root");fs::create_directories(temporary);
        const auto local=temporary/"local appdata";fs::create_directories(local);
        require(SetEnvironmentVariableW(L"LOCALAPPDATA",local.c_str()),"set isolated AppData");
        const auto managed=local/"ofxr-bridge/RIFE/runtime",legacy=temporary/"lab/runtime/rife",package=temporary/"lab/packages/v837";
        seed(managed);seed(legacy);fs::create_directories(package);
        require(xrfg::managed_rife_runtime()==managed,"managed AppData location");
        require(xrfg::staged_rife_runtime(package)==managed,"managed runtime must precede legacy discovery");
        const auto module=temporary/"staged module";fs::create_directories(module);
        {std::ofstream ini(module/"ofxr_bridge.ini");ini<<"[ofxr]\nrife_runtime_dir="<<legacy.generic_string()<<"\n";}
        require(xrfg::staged_rife_runtime(module)==legacy,"explicit owned runtime INI must retain priority");
        fs::remove(managed/"app/worker.py");
        require(xrfg::staged_rife_runtime(package).empty(),"incomplete AppData must not discover legacy runtime");
        require(xrfg::staged_rife_runtime(module)==legacy,"manual developer INI still works without AppData");
        const auto lease=legacy.parent_path()/L".rife.lease";
        {
            xrfg::RifeRuntimeReadLease first,second;
            require(first.acquire(legacy)&&second.acquire(legacy),"multiple worker read leases");
            const auto writer=CreateFileW(lease.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
            require(writer==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION,"installer replacement must fail while a worker holds the runtime");
        }
        const auto writer=CreateFileW(lease.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(writer!=INVALID_HANDLE_VALUE,"installer obtains lease after workers retire");
        {xrfg::RifeRuntimeReadLease worker;require(!worker.acquire(legacy),"worker must not start while installation owns the runtime");}
        CloseHandle(writer);
        {xrfg::RifeRuntimeReadLease worker;require(worker.acquire(legacy),"worker can resume after installer releases lease");}
        if(previous && previous<32768)SetEnvironmentVariableW(L"LOCALAPPDATA",original);else SetEnvironmentVariableW(L"LOCALAPPDATA",nullptr);
        const auto full=fs::absolute(temporary).lexically_normal(),parent=fs::absolute(fs::temp_directory_path()).lexically_normal();
        require(fs::equivalent(full.parent_path(),parent) && full.filename().wstring().starts_with(L"ofxr-rife-locator-"),"checked temporary cleanup destination");
        fs::remove_all(full);
        std::cout<<"AppData-only/manual-INI locator and actual shared/exclusive Windows runtime leases PASS\n";return 0;
    }catch(const std::exception& e){if(previous && previous<32768)SetEnvironmentVariableW(L"LOCALAPPDATA",original);else SetEnvironmentVariableW(L"LOCALAPPDATA",nullptr);std::cerr<<e.what()<<'\n';return 1;}
}
