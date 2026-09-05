#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvOpticalFlowD3D12.h>

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

using CreateInstanceFunction = NV_OF_STATUS(NVOFAPI*)(
    std::uint32_t,
    NV_OF_D3D12_API_FUNCTION_LIST*);

void print_capability(
    const NV_OF_D3D12_API_FUNCTION_LIST& api,
    NvOFHandle handle,
    NV_OF_CAPS capability,
    const char* label) {
    std::uint32_t count = 0;
    NV_OF_STATUS status = api.nvOFGetCaps(handle, capability, nullptr, &count);
    if (status != NV_OF_SUCCESS || count == 0) {
        std::cout << label << ": status=" << status << " count=" << count << '\n';
        return;
    }

    std::vector<std::uint32_t> values(count);
    status = api.nvOFGetCaps(handle, capability, values.data(), &count);
    std::cout << label << ": status=" << status << " values=";
    for (std::uint32_t index = 0; index < count; ++index) {
        std::cout << (index == 0 ? "" : ",") << values[index];
    }
    std::cout << '\n';
}

void print_formats(
    const NV_OF_D3D12_API_FUNCTION_LIST& api,
    NvOFHandle handle,
    NV_OF_BUFFER_USAGE usage,
    const char* label) {
    std::uint32_t count = 0;
    NV_OF_STATUS status = api.nvOFGetSurfaceFormatCountD3D12(
        handle,
        usage,
        NV_OF_MODE_OPTICALFLOW,
        &count);
    if (status != NV_OF_SUCCESS || count == 0) {
        std::cout << label << ": status=" << status << " count=" << count << '\n';
        return;
    }

    std::vector<DXGI_FORMAT> formats(count);
    status = api.nvOFGetSurfaceFormatD3D12(
        handle,
        usage,
        NV_OF_MODE_OPTICALFLOW,
        formats.data());
    std::cout << label << ": status=" << status << " DXGI=";
    for (std::uint32_t index = 0; index < count; ++index) {
        std::cout << (index == 0 ? "" : ",")
                  << static_cast<unsigned>(formats[index]);
    }
    std::cout << '\n';
}

}  // namespace

int main() {
    ComPtr<IDXGIFactory6> factory;
    HRESULT result = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        std::cerr << "CreateDXGIFactory2 failed: 0x" << std::hex << result << '\n';
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_description{};
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(
                index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(candidate.GetAddressOf())) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(candidate->GetDesc1(&description)) ||
            description.VendorId != 0x10deU ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        adapter = candidate;
        adapter_description = description;
        break;
    }
    if (!adapter) {
        std::cerr << "No NVIDIA hardware adapter found\n";
        return 2;
    }

    ComPtr<ID3D12Device> device;
    result = D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(device.GetAddressOf()));
    if (FAILED(result)) {
        std::cerr << "D3D12CreateDevice failed: 0x" << std::hex << result << '\n';
        return 3;
    }

    HMODULE module = LoadLibraryExW(
        L"nvofapi64.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        std::cerr << "nvofapi64.dll load failed: " << GetLastError() << '\n';
        return 4;
    }

    const auto create_instance = reinterpret_cast<CreateInstanceFunction>(
        GetProcAddress(module, "NvOFAPICreateInstanceD3D12"));
    if (create_instance == nullptr) {
        std::cerr << "NvOFAPICreateInstanceD3D12 is unavailable\n";
        FreeLibrary(module);
        return 5;
    }

    NV_OF_D3D12_API_FUNCTION_LIST api{};
    NV_OF_STATUS status = create_instance(NV_OF_API_VERSION, &api);
    if (status != NV_OF_SUCCESS) {
        std::cerr << "Create API table failed: " << status << '\n';
        FreeLibrary(module);
        return 6;
    }

    NvOFHandle handle = nullptr;
    status = api.nvCreateOpticalFlowD3D12(device.Get(), &handle);
    if (status != NV_OF_SUCCESS || handle == nullptr) {
        std::cerr << "Create optical-flow handle failed: " << status << '\n';
        FreeLibrary(module);
        return 7;
    }

    std::wcout << L"adapter=" << adapter_description.Description << '\n';
    std::cout << "api_version=0x" << std::hex << NV_OF_API_VERSION << std::dec << '\n';
    D3D12_FEATURE_DATA_FORMAT_SUPPORT bgra_support{};
    bgra_support.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    result = device->CheckFeatureSupport(
        D3D12_FEATURE_FORMAT_SUPPORT,
        &bgra_support,
        sizeof(bgra_support));
    std::cout << "bgra8_support: status=0x" << std::hex << result
              << " support1=0x" << bgra_support.Support1
              << " support2=0x" << bgra_support.Support2 << std::dec << '\n';
    print_formats(api, handle, NV_OF_BUFFER_USAGE_INPUT, "input_formats");
    print_formats(api, handle, NV_OF_BUFFER_USAGE_OUTPUT, "output_formats");
    print_formats(api, handle, NV_OF_BUFFER_USAGE_COST, "cost_formats");
    print_capability(
        api,
        handle,
        NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES,
        "output_grid_sizes");
    print_capability(api, handle, NV_OF_CAPS_WIDTH_MIN, "width_min");
    print_capability(api, handle, NV_OF_CAPS_HEIGHT_MIN, "height_min");
    print_capability(api, handle, NV_OF_CAPS_WIDTH_MAX, "width_max");
    print_capability(api, handle, NV_OF_CAPS_HEIGHT_MAX, "height_max");

    const NV_OF_STATUS destroy_status = api.nvOFDestroy(handle);
    FreeLibrary(module);
    if (destroy_status != NV_OF_SUCCESS) {
        std::cerr << "Destroy optical-flow handle failed: " << destroy_status << '\n';
        return 8;
    }
    return 0;
}
