#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <windows.h>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: xrfg_layer_smoke <layer-dll>\n";
        return EXIT_FAILURE;
    }

    const HMODULE module = LoadLibraryA(argv[1]);
    if (module == nullptr) {
        std::cerr << "LoadLibrary failed: " << GetLastError() << '\n';
        return EXIT_FAILURE;
    }

    const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
        GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
    if (negotiate == nullptr) {
        std::cerr << "negotiation export is missing\n";
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    XrNegotiateLoaderInfo loader_info{};
    loader_info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader_info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader_info.structSize = sizeof(loader_info);
    loader_info.minInterfaceVersion = 1;
    loader_info.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader_info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader_info.maxApiVersion = XR_CURRENT_API_VERSION;

    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);

    const XrResult result = negotiate(
        &loader_info,
        "XR_APILAYER_XRFrameBridge_diagnostic",
        &request);
    if (XR_FAILED(result) || request.getInstanceProcAddr == nullptr ||
        request.createApiLayerInstance == nullptr || request.layerInterfaceVersion != 1 ||
        XR_VERSION_MAJOR(request.layerApiVersion) != 1) {
        std::cerr << "layer negotiation failed: " << result << '\n';
        FreeLibrary(module);
        return EXIT_FAILURE;
    }

    FreeLibrary(module);
    std::cout << "OpenXR layer export and negotiation smoke test passed\n";
    return EXIT_SUCCESS;
}
