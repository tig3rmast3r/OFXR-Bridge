#include "xrfg/benchmark_suite.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace xrfg::autonomous;
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
#include "independent_benchmark_controller_tests.inc"
int main(int,char**) {
    const auto root=std::filesystem::temp_directory_path()/(L"ofxr-exhaustive-"+std::to_wstring(GetCurrentProcessId()));
    SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",root.c_str());
    try {
        std::filesystem::create_directories(root);
        require(WritePrivateProfileStringW(L"benchmark",L"enabled",L"1",global_configuration_path().c_str()),"isolated benchmark enable");
        independent_controller_cases();std::cout<<"Independent exhaustive live-controller,quality,optimization,CSV,capability and restore PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
