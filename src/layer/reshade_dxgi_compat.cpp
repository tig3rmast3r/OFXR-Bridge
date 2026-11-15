#include "xrfg/reshade_dxgi_compat.hpp"
#include <windows.h>
#include <dxgi1_6.h>
#include <intrin.h>
#include <safetyhook.hpp>
#include <atomic>
#include <mutex>

namespace xrfg {
namespace {
using CreateFactory2=HRESULT(WINAPI*)(UINT,REFIID,void**);
SafetyHookInline factory_hook;
CreateFactory2 original_factory{};
std::atomic<std::uint64_t> unwrapped{};
// ReShade 6.8's public proxy-unwrapping interface (source/com_utils.hpp).
constexpr GUID unwrapped_object={0x7f2c9a11,0x3b4e,0x4d6a,{0x81,0x2f,0x5e,0x9c,0xd3,0x7a,0x1b,0x42}};

HMODULE address_module(const void* address) noexcept {
    HMODULE module{};
    if(address)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&module);
    return module;
}
bool reshade_module(HMODULE module) noexcept {
    return module && GetProcAddress(module,"ReShadeVersion") &&
        GetProcAddress(module,"xrNegotiateLoaderApiLayerInterface");
}
HRESULT WINAPI create_factory(UINT flags,REFIID iid,void** output) {
    // Keep the real caller before invoking any trampoline/helper. The D3D11
    // runtime enumerates private adapter interfaces whose vtable is NOT that
    // of IDXGIAdapter. ReShade's late-load/unproxied-D3D12 path accidentally
    // wraps these and calls SetPrivateData on an internal DXGI method.
    const auto caller=address_module(_ReturnAddress());
    const auto d11=GetModuleHandleW(L"d3d11.dll");
    const auto result=original_factory(flags,iid,output);
    if(FAILED(result)||!output||!*output||!d11||caller!=d11)return result;
    if(iid!=__uuidof(IDXGIFactory)&&iid!=__uuidof(IDXGIFactory1)&&
       iid!=__uuidof(IDXGIFactory2)&&iid!=__uuidof(IDXGIFactory3)&&
       iid!=__uuidof(IDXGIFactory4)&&iid!=__uuidof(IDXGIFactory5)&&
       iid!=__uuidof(IDXGIFactory6)&&iid!=__uuidof(IDXGIFactory7))return result;
    auto* proxy=static_cast<IUnknown*>(*output);
    if(!reshade_module(address_module((*reinterpret_cast<void***>(proxy))[0])))return result;
    IUnknown* original{};
    if(FAILED(proxy->QueryInterface(unwrapped_object,reinterpret_cast<void**>(&original)))||!original)return result;
    // Query the originally requested interface: never substitute an IUnknown
    // or assume all factory versions have the same interface pointer.
    void* requested{};
    const auto query=original->QueryInterface(iid,&requested);
    original->Release();
    if(SUCCEEDED(query)&&requested) {
        *output=requested;
        proxy->Release();
        unwrapped.fetch_add(1,std::memory_order_relaxed);
    }
    return result;
}
}
bool ensure_reshade_dxgi_compat() noexcept {
    try {
        static std::mutex mutex;std::scoped_lock lock(mutex);
        if(factory_hook)return true;
        bool found=false;
        for(const auto* name:{L"ReShade64.dll",L"ReShade32.dll",L"dxgi.dll",L"d3d12.dll",L"d3d11.dll"})
            if(reshade_module(GetModuleHandleW(name))){found=true;break;}
        if(!found||!GetModuleHandleW(L"d3d11.dll"))return false;
        const auto dxgi=GetModuleHandleW(L"dxgi.dll");
        const auto entry=dxgi?GetProcAddress(dxgi,"CreateDXGIFactory2"):nullptr;
        if(!entry)return false;
        factory_hook=safetyhook::create_inline(reinterpret_cast<void*>(entry),
            reinterpret_cast<void*>(&create_factory),SafetyHookInline::StartDisabled);
        if(!factory_hook)return false;
        original_factory=factory_hook.original<CreateFactory2>();
        if(!factory_hook.enable()){factory_hook={};original_factory=nullptr;return false;}
        return true;
    }catch(...){return false;}
}
std::uint64_t reshade_dxgi_unwrapped_factories() noexcept {
    return unwrapped.load(std::memory_order_relaxed);
}
}
