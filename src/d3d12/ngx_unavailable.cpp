// 0.2 V576 in 0.3: the 32-bit layer's NGX. NVIDIA ships the NGX library
// for x64 only, so a Win32 build links these instead. Initialisation fails
// as an unsupported feature does, so native DLSS-G reports itself
// unavailable and nothing past that is reached; the rest exist for the
// linker. Neural Rendering needs none of the library.

#include <windows.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

namespace {
constexpr NVSDK_NGX_Result kUnavailable = NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

extern "C" {

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Init_with_ProjectID(const char*, NVSDK_NGX_EngineType,
    const char*, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version) {
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_GetCapabilityParameters(NVSDK_NGX_Parameter** parameters) {
    if (parameters) *parameters = nullptr;
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_AllocateParameters(NVSDK_NGX_Parameter** parameters) {
    if (parameters) *parameters = nullptr;
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_DestroyParameters(NVSDK_NGX_Parameter*) {
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
    NVSDK_NGX_Parameter*, NVSDK_NGX_Handle** handle) {
    if (handle) *handle = nullptr;
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList*,
    const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback_C) {
    return kUnavailable;
}

NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_ReleaseFeature(NVSDK_NGX_Handle*) {
    return kUnavailable;
}

void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter*, const char*, ID3D12Resource*) {}
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter*, const char*, float) {}
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter*, const char*, unsigned int) {}
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter*, const char*, void*) {}

}  // extern "C"
