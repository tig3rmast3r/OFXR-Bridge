#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <new>

namespace xrfg::owned_d11_output {
using Microsoft::WRL::ComPtr;

// Only OFXR-created private generation outputs carry this marker. It pins a
// D12 backing, never the D11 texture which owns the marker (no COM cycle).
// Copying the marker onto a different texture cannot authorize adoption.
inline constexpr GUID marker_id{
    0x47f891da,0x8d19,0x4df0,{0x8b,0xde,0x18,0x79,0x30,0x06,0x3c,0x41}};
struct __declspec(uuid("0E708156-E2FA-4931-954F-30C07504B17B")) Backing : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get(ID3D11Texture2D*,ID3D12Resource**)=0;
};
class Marker final : public Backing {
    std::atomic<ULONG> references_{1};
    ID3D11Texture2D* const image_; // weak; the image owns this marker
    ComPtr<ID3D12Resource> resource_;
public:
    Marker(ID3D11Texture2D* image,ID3D12Resource* resource) noexcept
        :image_(image),resource_(resource) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(id!=__uuidof(IUnknown) && id!=__uuidof(Backing))return E_NOINTERFACE;
        *output=static_cast<Backing*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left=--references_;if(!left)delete this;return left;
    }
    HRESULT STDMETHODCALLTYPE get(ID3D11Texture2D* image,ID3D12Resource** output) override {
        if(!output)return E_POINTER;*output=nullptr;
        if(image!=image_)return E_INVALIDARG;
        return resource_.CopyTo(output);
    }
};
inline bool same_identity(IUnknown* a,IUnknown* b) noexcept {
    if(!a || !b)return false;
    ComPtr<IUnknown> x,y;
    return SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&x))) &&
        SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&y))) && x==y;
}
inline bool same_adapter(ID3D11Device* d11,ID3D12Device* d12) noexcept {
    if(!d11 || !d12)return false;
    ComPtr<IDXGIDevice> device;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
    if(FAILED(d11->QueryInterface(IID_PPV_ARGS(&device))) ||
       FAILED(device->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))return false;
    const auto luid=d12->GetAdapterLuid();
    return luid.HighPart==desc.AdapterLuid.HighPart && luid.LowPart==desc.AdapterLuid.LowPart;
}
inline ComPtr<ID3D12Resource> backing_for(ID3D11Texture2D* image,ID3D12Device* owner) noexcept {
    if(!image || !owner)return {};
    ComPtr<IUnknown> stored;UINT size=sizeof(IUnknown*);
    if(FAILED(image->GetPrivateData(marker_id,&size,stored.GetAddressOf())) ||
       size!=sizeof(IUnknown*) || !stored)return {};
    ComPtr<Backing> marker;
    if(FAILED(stored.As(&marker)))return {};
    ComPtr<ID3D12Resource> resource;
    if(FAILED(marker->get(image,&resource)) || !resource)return {};
    ComPtr<ID3D12Device> actual;
    if(FAILED(resource->GetDevice(IID_PPV_ARGS(&actual))) || !same_identity(actual.Get(),owner))return {};
    D3D11_TEXTURE2D_DESC d11{};image->GetDesc(&d11);
    const auto d12=resource->GetDesc();
    if(d11.Usage!=D3D11_USAGE_DEFAULT || d11.CPUAccessFlags || d11.MipLevels!=1 ||
       d11.SampleDesc.Count!=1 || d11.SampleDesc.Quality ||
       d12.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d12.Width!=d11.Width ||
       d12.Height!=d11.Height || d12.DepthOrArraySize!=d11.ArraySize || d12.MipLevels!=1 ||
       d12.Format!=d11.Format || d12.SampleDesc.Count!=1 || d12.SampleDesc.Quality ||
       (d12.Flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))!=
           (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))return {};
    D3D12_HEAP_PROPERTIES heap{};D3D12_HEAP_FLAGS flags{};
    if(FAILED(resource->GetHeapProperties(&heap,&flags)) || heap.Type!=D3D12_HEAP_TYPE_DEFAULT ||
       !(flags&D3D12_HEAP_FLAG_SHARED))return {};
    return resource;
}

// Uses the same D12-created sharing contract as the existing interop mirrors.
// No keyed mutex or application/native-runtime texture is adopted. The caller
// may retain the ordinary private D11 + separate-mirror path on failure.
inline HRESULT create(ID3D11Device* d11,ID3D12Device* d12,
                      const D3D11_TEXTURE2D_DESC& desc,ID3D11Texture2D** output) noexcept {
    if(!output)return E_POINTER;*output=nullptr;
    if(!d11 || !d12 || !desc.Width || !desc.Height || !desc.ArraySize ||
       desc.ArraySize>D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION || desc.MipLevels!=1 ||
       desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality ||
       desc.Usage!=D3D11_USAGE_DEFAULT || desc.CPUAccessFlags ||
       !(desc.BindFlags&D3D11_BIND_RENDER_TARGET) ||
       (desc.BindFlags&D3D11_BIND_DEPTH_STENCIL) ||
       (desc.MiscFlags&(D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_RESTRICTED_CONTENT|
           D3D11_RESOURCE_MISC_HW_PROTECTED|D3D11_RESOURCE_MISC_GUARDED)) ||
       !same_adapter(d11,d12))return E_INVALIDARG;
    try {
        D3D12_RESOURCE_DESC shared{};shared.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shared.Width=desc.Width;shared.Height=desc.Height;
        shared.DepthOrArraySize=static_cast<UINT16>(desc.ArraySize);shared.MipLevels=1;
        shared.Format=desc.Format;shared.SampleDesc=desc.SampleDesc;
        shared.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if(desc.BindFlags&D3D11_BIND_UNORDERED_ACCESS)shared.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask=heap.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> backing;
        auto result=d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&shared,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&backing));
        if(FAILED(result))return result;
        ComPtr<ID3D12Device> actual;
        if(FAILED(backing->GetDevice(IID_PPV_ARGS(&actual))) || !same_identity(actual.Get(),d12))return E_INVALIDARG;
        HANDLE handle{};
        result=d12->CreateSharedHandle(backing.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
        if(FAILED(result))return result;
        ComPtr<ID3D11Device1> device1;ComPtr<ID3D11Texture2D> image;
        result=d11->QueryInterface(IID_PPV_ARGS(&device1));
        if(SUCCEEDED(result))result=device1->OpenSharedResource1(handle,IID_PPV_ARGS(&image));
        CloseHandle(handle);
        if(FAILED(result))return result;
        auto* marker=new Marker(image.Get(),backing.Get());
        result=image->SetPrivateDataInterface(marker_id,marker);marker->Release();
        if(FAILED(result))return result;
        if(!backing_for(image.Get(),d12))return E_INVALIDARG;
        return image.CopyTo(output);
    } catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
      catch(...) {return E_FAIL;}
}
} // namespace xrfg::owned_d11_output
