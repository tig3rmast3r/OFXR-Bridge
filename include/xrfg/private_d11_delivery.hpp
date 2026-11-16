#pragma once
#include "xrfg/owned_d11_output.hpp"

namespace xrfg {
// The real D11 runtime gets its own immediate context/driver queue. The
// application's virtual images stay on its original device and are shared
// only after their producer readiness has completed.
inline HRESULT create_private_d11_delivery(ID3D11Device* source,ID3D11Device** output,
    ID3D11DeviceContext** context) noexcept {
    if(!source || !output || !context)return E_INVALIDARG;
    *output=nullptr;*context=nullptr;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    auto result=source->QueryInterface(IID_PPV_ARGS(&dxgi));
    if(SUCCEEDED(result))result=dxgi->GetAdapter(&adapter);
    const auto level=source->GetFeatureLevel();D3D_FEATURE_LEVEL actual{};
    if(SUCCEEDED(result))result=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,&level,1,D3D11_SDK_VERSION,output,&actual,context);
    if(SUCCEEDED(result)) {
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if(SUCCEEDED((*context)->QueryInterface(IID_PPV_ARGS(&multithread))))multithread->SetMultithreadProtected(TRUE);
    }
    return result;
}
// A source is immutable until the native copy fence completes. Cache this
// imported view with that virtual image, never by a recycled raw handle.
inline HRESULT open_d11_delivery_image(ID3D11Texture2D* source,ID3D11Device* destination,
    ID3D11Texture2D** output,ID3D12Device* backing_owner=nullptr) noexcept {
    if(!source || !destination || !output)return E_INVALIDARG;*output=nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device1> device;
    auto result=destination->QueryInterface(IID_PPV_ARGS(&device));if(FAILED(result))return result;
    HANDLE shared{};
    Microsoft::WRL::ComPtr<IUnknown> stored;UINT bytes=sizeof(IUnknown*);
    Microsoft::WRL::ComPtr<owned_d11_output::Backing> marker;
    if(SUCCEEDED(source->GetPrivateData(owned_d11_output::marker_id,&bytes,stored.GetAddressOf())) &&
        stored && SUCCEEDED(stored.As(&marker))) {
        Microsoft::WRL::ComPtr<ID3D12Resource> backing;Microsoft::WRL::ComPtr<ID3D12Device> owner;
        result=marker->get(source,&backing);
        // Preserve the proven creation owner through wrappers whose GetDevice
        // exposes another identity. The marker still validates the exact image.
        if(backing_owner)owner=backing_owner;
        else if(SUCCEEDED(result))result=backing->GetDevice(IID_PPV_ARGS(&owner));
        if(SUCCEEDED(result))result=owner->CreateSharedHandle(backing.Get(),nullptr,GENERIC_ALL,nullptr,&shared);
    } else {
        Microsoft::WRL::ComPtr<IDXGIResource1> resource;
        result=source->QueryInterface(IID_PPV_ARGS(&resource));
        if(SUCCEEDED(result))result=resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&shared);
    }
    if(SUCCEEDED(result))result=device->OpenSharedResource1(shared,IID_PPV_ARGS(output));
    if(shared)CloseHandle(shared);return result;
}
}
