// 0.2 V576: the 32-bit layer's FidelityFX. AMD ships the FidelityFX SDK's
// libraries for x64 only, so a Win32 build links these instead. The first
// call the synthesizer makes, the scratch size, reports nothing to allocate
// and its initialisation fails the way a missing backend does; nothing past
// it is reached. The rest exist for the linker.

#include <windows.h>

#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <FidelityFX/host/ffx_opticalflow.h>

size_t ffxGetScratchMemorySizeDX12(size_t) { return 0; }

FfxDevice ffxGetDeviceDX12(ID3D12Device*) { return nullptr; }

FfxErrorCode ffxGetInterfaceDX12(FfxInterface*, FfxDevice, void*, size_t, size_t) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxCommandList ffxGetCommandListDX12(ID3D12CommandList*) { return nullptr; }

FfxResource ffxGetResourceDX12(const ID3D12Resource*, FfxResourceDescription,
                               const wchar_t*, FfxResourceStates) {
    return {};
}

DXGI_FORMAT ffxGetDX12FormatFromSurfaceFormat(FfxSurfaceFormat) {
    return DXGI_FORMAT_UNKNOWN;
}

FfxResourceDescription ffxGetResourceDescriptionDX12(const ID3D12Resource*, FfxResourceUsage) {
    return {};
}

FfxErrorCode ffxOpticalflowContextCreate(FfxOpticalflowContext*,
                                         FfxOpticalflowContextDescription*) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode ffxOpticalflowGetSharedResourceDescriptions(
    FfxOpticalflowContext*, FfxOpticalflowSharedResourceDescriptions*) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode ffxOpticalflowContextDispatch(FfxOpticalflowContext*,
                                           const FfxOpticalflowDispatchDescription*) {
    return FFX_ERROR_BACKEND_API_ERROR;
}

FfxErrorCode ffxOpticalflowContextDestroy(FfxOpticalflowContext*) {
    return FFX_ERROR_BACKEND_API_ERROR;
}
