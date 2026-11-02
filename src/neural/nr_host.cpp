#include "nr_host.hpp"
#include <share.h>
#include "xrfg/ngx_feature_paths.hpp"

namespace {
struct NgxServices {
    HMODULE module{};
    NrNgxCore::Capability capability{};
    NrNgxCore::Destroy destroy{};
    bool initialized{};
};
NgxServices& services() { static auto* s=new NgxServices;return *s; }
std::filesystem::path module_path(HMODULE module) {
    std::array<wchar_t,32768> path{};
    auto count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
    return count&&count<path.size()?std::filesystem::path(std::wstring(path.data(),count)):
        std::filesystem::path{};
}
HMODULE driver_core() {
    // Reuse the game's dispatcher, retaining the module for all private blocks.
    for(auto name:{L"_nvngx.dll",L"nvngx.dll"}) {
        auto mod=GetModuleHandleW(name);
        if(mod&&GetProcAddress(mod,"NVSDK_NGX_D3D12_GetCapabilityParameters")) {
            HMODULE pinned{};
            if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(GetProcAddress(mod,"NVSDK_NGX_D3D12_GetCapabilityParameters")),
                &pinned))return pinned;
        }
    }
    // Driver NGXCore discovery follows the accepted DLSSTweaks registry contract.
    // No game-local proxy or OptiScaler is loaded as a dispatcher.
    std::array<wchar_t,32768> folder{};DWORD bytes=DWORD(folder.size()*sizeof(wchar_t));
    if(RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",L"NGXPath",
        RRF_RT_REG_SZ,nullptr,folder.data(),&bytes)==ERROR_SUCCESS) {
        for(auto name:{L"_nvngx.dll",L"nvngx.dll"}) {
            auto path=std::filesystem::path(folder.data())/name;
            if(auto mod=LoadLibraryExW(path.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH)) {
                if(GetProcAddress(mod,"NVSDK_NGX_D3D12_GetCapabilityParameters"))return mod;
                FreeLibrary(mod);
            }
        }
    }
    return nullptr;
}
}

std::filesystem::path Util::DllPath() {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Util::DllPath),&module);
    return module_path(module);
}
std::filesystem::path Util::ExePath() {return module_path(nullptr);}
std::optional<std::filesystem::path> Util::FindFilePath(
    const std::filesystem::path& directory,const char* filename) {
    auto candidate=directory/filename;std::error_code error;
    if(std::filesystem::is_regular_file(candidate,error))return candidate;
    return std::nullopt;
}
void Util::GetDeviceRemovedReason(ID3D12Device* device) {
    if(device)LOG_ERROR("NR device removal reason 0x{:X}",unsigned(device->GetDeviceRemovedReason()));
}
std::string wstring_to_string(const std::wstring& value) {
    if(value.empty())return {};
    const int size=WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),int(value.size()),result.data(),size,nullptr,nullptr);
    return result;
}
const std::wstring& NrApplicationDataPath() {
    static const std::wstring folder=Util::ExePath().parent_path().wstring();return folder;
}
void nr_host::log_text(const char* level,const std::string& message) {
    static auto* mutex=new std::mutex;std::scoped_lock lock(*mutex);
    auto line=std::format("OFXR NR {} {}\n",level,message);OutputDebugStringA(line.c_str());
    if(!NrFlightTrace::Enabled())return;
    static FILE* output=[] {
        auto path=Util::DllPath().parent_path()/std::format("ofxr-neural-pid{}.log",GetCurrentProcessId());
        return _wfsopen(path.c_str(),L"wb",_SH_DENYNO);
    }();
    if(output){fputs(line.c_str(),output);fflush(output);}
}
bool NrNgxCore::IsDx12Inited(){return services().initialized;}
NrNgxCore::Capability NrNgxCore::D3D12_GetCapabilityParameters(){return services().capability;}
NrNgxCore::Destroy NrNgxCore::D3D12_DestroyParameters(){return services().destroy;}
bool NrNgxCore::InitDx12(ID3D12Device* device) {
    auto& s=services();if(s.initialized)return true;
    if(!s.module)s.module=driver_core();if(!s.module||!device)return false;
    s.capability=reinterpret_cast<Capability>(GetProcAddress(s.module,"NVSDK_NGX_D3D12_GetCapabilityParameters"));
    s.destroy=reinterpret_cast<Destroy>(GetProcAddress(s.module,"NVSDK_NGX_D3D12_DestroyParameters"));
    if(!s.capability||!s.destroy)return false;
    NVSDK_NGX_Parameter* probe{};
    if(s.capability(&probe)==NVSDK_NGX_Result_Success&&probe) {
        s.destroy(probe);s.initialized=true;return true;
    }
    using Init=NVSDK_NGX_Result(*)(unsigned long long,const wchar_t*,ID3D12Device*,
        NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    auto init=reinterpret_cast<Init>(GetProcAddress(s.module,"NVSDK_NGX_D3D12_Init_Ext"));
    if(!init)return false;
    auto module_folder=Util::DllPath().parent_path().wstring();
    xrfg::NgxFeaturePaths search(nullptr,module_folder);
    s.initialized=init(0x24480451ULL,NrApplicationDataPath().c_str(),device,
        NVSDK_NGX_Version_API,search.get())==NVSDK_NGX_Result_Success;
    return s.initialized;
}
