#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <nvsdk_ngx.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "NrFlightTrace.h"
#include "DredMarkers.h"
#include "xrfg/neural_rendering.hpp"

// Narrow services for the extracted NR module. These do not intercept or
// replace SR, driver dispatch, game configuration, or any OptiScaler runtime.
template<class T> struct NrValue {
    T value{};
    T value_or_default() const noexcept { return value; }
    NrValue& operator=(T replacement) noexcept { value=replacement; return *this; }
};
struct NrConfiguration {
    NrValue<bool> DlssNrEnabled{false}, DlssNrRunBeforeSr{true};
    NrValue<bool> DlssNrSecondPass{false};
    NrValue<uint32_t> DlssNrPreset{0}, DlssNrStyle{0}, DlssNrDebugView{0}, DlssNrCompare{0};
    NrValue<float> DlssNrIntensity{1.f}, DlssNrLocalStructure{1.f}, DlssNrLocalTone{1.f};
    NrValue<float> DlssNrSkinStructure{-1.f}, DlssNrTransferStrength{1.f}, DlssNrColourStrength{1.f};
    NrValue<float> DlssNrMaxRatio{2.f}, DlssNrCompareSplit{.5f}, DlssNrCompareZoom{1.f};
    NrValue<float> DlssNrWorkingScale{1.f}, DlssNrWhitePointScale{1.f};
    NrValue<bool> DlssNrAutoMask{true}, DlssNrCompareSwap{false};
    NrValue<bool> LogToFile{false}, LogToConsole{false}, LogToNGX{false};
    static NrConfiguration* Instance() { static auto* c=new NrConfiguration; return c; }
};

namespace nr_host {
void log_text(const char* level, const std::string& message);
template<class... T> void log(const char* level,std::string_view message,T&&... arguments) {
    if(!NrFlightTrace::Enabled() && std::string_view(level)!="ERROR")return;
    try { log_text(level,std::vformat(message,std::make_format_args(arguments...))); }
    catch(...) { log_text(level,std::string(message)); }
}
}
#define LOG_INFO(...) ::nr_host::log("INFO", __VA_ARGS__)
#define LOG_WARN(...) ::nr_host::log("WARN", __VA_ARGS__)
#define LOG_ERROR(...) ::nr_host::log("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...) ::nr_host::log("DEBUG", __VA_ARGS__)
#define SAFE_RELEASE(p) do { if(p) { (p)->Release(); (p)=nullptr; } } while(0)
std::string wstring_to_string(const std::wstring& value);
namespace Util {
std::filesystem::path DllPath();
std::filesystem::path ExePath();
std::optional<std::filesystem::path> FindFilePath(const std::filesystem::path& directory,
    const char* filename);
void GetDeviceRemovedReason(ID3D12Device* device);
}
const std::wstring& NrApplicationDataPath();
class NrNgxCore {
public:
    using Capability=NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter**);
    using Destroy=NVSDK_NGX_Result(*)(NVSDK_NGX_Parameter*);
    static bool IsDx12Inited();
    static bool InitDx12(ID3D12Device* device);
    static Capability D3D12_GetCapabilityParameters();
    static Destroy D3D12_DestroyParameters();
};
