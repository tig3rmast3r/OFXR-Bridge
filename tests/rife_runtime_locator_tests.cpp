#include "xrfg/rife_runtime_locator.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
void stub_runtime(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"python");
    std::filesystem::create_directories(root/"app");
    std::ofstream(root/"python/python.exe")<<"test stub";
    std::ofstream(root/"app/worker.py")<<"test stub";
}
}
int main() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/fs::path("OFXR portable runtime test "+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto package=root/"folder with spaces"/"package";
        const auto shared=root/"runtime/rife";
        const auto bundled=package/"rife";
        stub_runtime(shared);stub_runtime(bundled);
        check(xrfg::find_shared_rife_runtime(package)==shared,"shared ancestor must win over an older bundled copy");
        check(xrfg::find_shared_rife_runtime(package/"ofxr")==shared,"DLL beside ofxr resolves shared runtime");
        const auto staged=root/"staged DLL outside package";
        fs::create_directories(staged);
        const auto ini=xrfg::with_shared_rife_runtime("[ofxr]\r\nenabled=1\r\n",package);
        check(ini.find("rife_runtime_dir=")!=ini.npos,"tray did not emit transient runtime locator");
        std::ofstream(staged/"ofxr_bridge.ini",std::ios::binary)<<ini;
        check(xrfg::staged_rife_runtime(staged)==shared,"staged DLL did not use original shared runtime");
        const auto moved=root/"moved package";
        fs::rename(package,moved);
        const auto moved_ini=xrfg::with_shared_rife_runtime("[ofxr]\r\nenabled=1\r\n",moved);
        std::ofstream(staged/"ofxr_bridge.ini",std::ios::binary)<<moved_ini;
        check(xrfg::staged_rife_runtime(staged)==shared,"restart after move lost shared runtime");
        check(moved_ini.find(package.generic_string())==moved_ini.npos,"generated INI retains previous package path");
        check(xrfg::find_shared_rife_runtime(root/"laboratory build")==shared,"unbundled laboratory fallback changed");
        check(xrfg::with_shared_rife_runtime("[ofxr]\r\n",root/"absent")==
              xrfg::with_shared_rife_runtime("[ofxr]\r\n",root),"ancestor resolution differs by missing leaf");
        fs::remove_all(root);
        std::cout<<"Portable RIFE locator: shared lookup, staged DLL, relocation and spaces PASS\n";
        return 0;
    } catch(const std::exception& error) {
        std::error_code ignored;fs::remove_all(root,ignored);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
