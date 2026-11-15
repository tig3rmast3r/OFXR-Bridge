#include "xrfg/gpu_route_support.hpp"
#include <initguid.h>
#include <dxcore.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

namespace xrfg {
namespace {
using Microsoft::WRL::ComPtr;
struct Adapter {LUID luid{};unsigned vendor{};};
struct Inventory {
    std::vector<Adapter> adapters;
    unsigned fallback{};
    Inventory() noexcept {
        try {
            // Never search the application directory or go through spoofed DXGI.
            const auto module=LoadLibraryExW(L"dxcore.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(!module)return;
            const auto create=reinterpret_cast<HRESULT(WINAPI*)(REFIID,void**)>(GetProcAddress(module,"DXCoreCreateAdapterFactory"));
            ComPtr<IDXCoreAdapterFactory> factory;
            if(!create||FAILED(create(IID_PPV_ARGS(&factory))))return;
            ComPtr<IDXCoreAdapterList> list;
            const GUID attribute=DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS;
            if(FAILED(factory->CreateAdapterList(1,&attribute,IID_PPV_ARGS(&list))))return;
            bool amd=false,nvidia=false;
            for(UINT i=0;i<list->GetAdapterCount();++i) {
                ComPtr<IDXCoreAdapter> adapter;bool hardware=false;DXCoreHardwareID id{};LUID luid{};
                if(FAILED(list->GetAdapter(i,IID_PPV_ARGS(&adapter)))||
                   FAILED(adapter->GetProperty(DXCoreAdapterProperty::IsHardware,sizeof(hardware),&hardware))||!hardware||
                   FAILED(adapter->GetProperty(DXCoreAdapterProperty::HardwareID,sizeof(id),&id))||
                   FAILED(adapter->GetProperty(DXCoreAdapterProperty::InstanceLuid,sizeof(luid),&luid)))continue;
                adapters.push_back({luid,id.vendorID});amd|=id.vendorID==0x1002;nvidia|=id.vendorID==0x10de;
            }
            // Before a game connects, do not disable NVIDIA on a mixed system.
            // The active session's exact LUID overrides this conservative default.
            fallback=nvidia?0x10de:amd?0x1002:0;
        }catch(...){}
    }
};
Inventory& inventory(){static Inventory value;return value;}
std::atomic_uint selected_vendor{};
}
unsigned rendering_gpu_vendor() noexcept {
    if(const auto selected=selected_vendor.load(std::memory_order_acquire))return selected;
    return inventory().fallback;
}
void set_rendering_gpu_luid(LUID luid) noexcept {
    for(const auto& adapter:inventory().adapters)
        if(adapter.luid.LowPart==luid.LowPart&&adapter.luid.HighPart==luid.HighPart) {
            selected_vendor.store(adapter.vendor,std::memory_order_release);return;
        }
    selected_vendor.store(0,std::memory_order_release);
}
}
