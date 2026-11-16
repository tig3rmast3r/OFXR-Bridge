#include "xrfg/d3d12_guide_ownership.hpp"
#include <dxgi1_4.h>
#include <iostream>
#include <stdexcept>
#include <array>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks{};
void require(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
void hr(HRESULT result,const char* text){require(SUCCEEDED(result),text);}
#include "d3d12_reported_device_alias.hpp"
class D3D12WarpFixture {
    ComPtr<ID3D12Device> device_;
public:
    D3D12WarpFixture(){ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> adapter;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"WARP factory");
        hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)),"WARP adapter");
        hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device_)),"WARP device");}
    ID3D12Device* device(){return device_.Get();}
};
ComPtr<ID3D12Resource> create_source_texture(D3D12WarpFixture& f){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=d.Height=4;
    d.DepthOrArraySize=d.MipLevels=1;d.Format=DXGI_FORMAT_R32_FLOAT;d.SampleDesc.Count=1;
    ComPtr<ID3D12Resource> resource;hr(f.device()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource)),"tiny guide resource");return resource;
}
class GuideResourceProxy final : public ID3D12Resource {
public:
    explicit GuideResourceProxy(ID3D12Resource* resource,ID3D12Device* owner) : resource_(resource),owner_(owner) {}
    LONG device_queries() noexcept { return InterlockedCompareExchange(&device_queries_, 0, 0); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12Object) ||
            iid == __uuidof(ID3D12DeviceChild) || iid == __uuidof(ID3D12Pageable) || iid == __uuidof(ID3D12Resource)) {
            *object = static_cast<ID3D12Resource*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&references_);
        if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        return resource_->GetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        return resource_->SetPrivateData(guid, size, data);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override {
        return resource_->SetPrivateDataInterface(guid, data);
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return resource_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** device) override {
        InterlockedIncrement(&device_queries_);
        if (!device) return E_POINTER;
        *device=nullptr;return owner_?owner_->QueryInterface(iid,device):E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Map(UINT subresource, const D3D12_RANGE* range, void** data) override {
        return resource_->Map(subresource, range, data);
    }
    void STDMETHODCALLTYPE Unmap(UINT subresource, const D3D12_RANGE* range) override { resource_->Unmap(subresource, range); }
    D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override { return resource_->GetDesc(); }
    D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE GetGPUVirtualAddress() override { return resource_->GetGPUVirtualAddress(); }
    HRESULT STDMETHODCALLTYPE WriteToSubresource(UINT subresource, const D3D12_BOX* box, const void* data,
        UINT row_pitch, UINT depth_pitch) override {
        return resource_->WriteToSubresource(subresource, box, data, row_pitch, depth_pitch);
    }
    HRESULT STDMETHODCALLTYPE ReadFromSubresource(void* data, UINT row_pitch, UINT depth_pitch,
        UINT subresource, const D3D12_BOX* box) override {
        return resource_->ReadFromSubresource(data, row_pitch, depth_pitch, subresource, box);
    }
    HRESULT STDMETHODCALLTYPE GetHeapProperties(D3D12_HEAP_PROPERTIES* properties, D3D12_HEAP_FLAGS* flags) override {
        return resource_->GetHeapProperties(properties, flags);
    }
private:
    ComPtr<ID3D12Resource> resource_;
    ComPtr<ID3D12Device> owner_;
    volatile LONG references_{1}, device_queries_{};
};
#include "guide_device_ownership.inc"
void test_proxy_owners(D3D12WarpFixture& fixture){
    auto resource=create_source_texture(fixture);
    ComPtr<ReportedDeviceAlias> reshade,streamline,sl_over_rs,rs_over_sl;
    reshade.Attach(new ReportedDeviceAlias(fixture.device(),true,false,nullptr,xrfg::d3d_native::reshade_unwrapped_guid));
    streamline.Attach(new ReportedDeviceAlias(fixture.device(),true));
    sl_over_rs.Attach(new ReportedDeviceAlias(reshade.Get(),true));
    rs_over_sl.Attach(new ReportedDeviceAlias(streamline.Get(),true,false,nullptr,xrfg::d3d_native::reshade_unwrapped_guid));
    const auto ref_count=[](IUnknown* p){const auto n=p->AddRef();p->Release();return n;};
    const auto before=ref_count(fixture.device());
    for(auto* owner:{reshade.Get(),streamline.Get(),sl_over_rs.Get(),rs_over_sl.Get()}){
        ComPtr<GuideResourceProxy> wrapped;wrapped.Attach(new GuideResourceProxy(resource.Get(),owner));
        for(auto* consumer:{static_cast<ID3D12Device*>(fixture.device()),static_cast<ID3D12Device*>(reshade.Get()),static_cast<ID3D12Device*>(rs_over_sl.Get())}){
            xrfg::DlssMotionVectorFrame frame;frame.motion_vectors=wrapped;frame.depth=wrapped;
            for(bool aliases:{false,true})for(unsigned repeat=0;repeat<16;++repeat)
                require(xrfg::guide_resource_on_device(consumer,frame,wrapped.Get(),aliases),"native DLSS-G ownership rejects ReShade/Streamline guide owner");
        }
    }
    require(ref_count(fixture.device())==before,"ownership queries leak device references");
    ComPtr<UnprovedSameAdapterAlias> foreign;foreign.Attach(new UnprovedSameAdapterAlias(fixture.device()));
    ComPtr<ReportedDeviceAlias> foreign_proxy;foreign_proxy.Attach(new ReportedDeviceAlias(foreign.Get(),true,false,nullptr,xrfg::d3d_native::reshade_unwrapped_guid));
    for(auto* owner:{static_cast<ID3D12Device*>(foreign.Get()),static_cast<ID3D12Device*>(foreign_proxy.Get()),static_cast<ID3D12Device*>(nullptr)}){
        ComPtr<GuideResourceProxy> wrapped;wrapped.Attach(new GuideResourceProxy(resource.Get(),owner));
        xrfg::DlssMotionVectorFrame frame;frame.motion_vectors=wrapped;frame.depth=wrapped;
        require(!xrfg::guide_resource_on_device(fixture.device(),frame,wrapped.Get(),false),"foreign same-adapter or missing owner admitted");
    }
    std::cout<<"ReShade/Streamline in both nesting orders:consumer/reported owners accepted;foreign/missing refused;references stable\n";
}
}
int main(){try{D3D12WarpFixture fixture;test_proxy_owners(fixture);test_guide_device_ownership(fixture);
    std::cout<<checks<<" guide owner checks PASS (WARP,no GPU dispatch)\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
