#pragma once
class QueueWithReportedDeviceAlias final : public ID3D12CommandQueue {
public:
    QueueWithReportedDeviceAlias(ID3D12CommandQueue* native, ID3D12Device* reported,
        bool exposes_native = false)
        : native_(native), reported_(reported), exposes_native_(exposes_native) {}
    LONG device_queries() noexcept { return InterlockedCompareExchange(&device_queries_, 0, 0); }
    void report_device(ID3D12Device* device) { reported_=device; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (exposes_native_ && iid == xrfg::d3d_native::streamline_native_guid)
            return native_->QueryInterface(__uuidof(ID3D12CommandQueue), object);
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) &&
            iid != __uuidof(ID3D12DeviceChild) && iid != __uuidof(ID3D12Pageable) &&
            iid != __uuidof(ID3D12CommandQueue)) return E_NOINTERFACE;
        *object = static_cast<ID3D12CommandQueue*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = InterlockedDecrement(&references_); if (!left) delete this;
        return static_cast<ULONG>(left);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override { return native_->GetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override { return native_->SetPrivateData(guid, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* data) override { return native_->SetPrivateDataInterface(guid, data); }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override { return native_->SetName(name); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid, void** device) override {
        InterlockedIncrement(&device_queries_); return reported_->QueryInterface(iid, device);
    }
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r, UINT n, const D3D12_TILED_RESOURCE_COORDINATE* c,
        const D3D12_TILE_REGION_SIZE* s, ID3D12Heap* h, UINT nr, const D3D12_TILE_RANGE_FLAGS* f,
        const UINT* o, const UINT* t, D3D12_TILE_MAPPING_FLAGS flags) override { native_->UpdateTileMappings(r, n, c, s, h, nr, f, o, t, flags); }
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d, const D3D12_TILED_RESOURCE_COORDINATE* dc,
        ID3D12Resource* s, const D3D12_TILED_RESOURCE_COORDINATE* sc, const D3D12_TILE_REGION_SIZE* z,
        D3D12_TILE_MAPPING_FLAGS f) override { native_->CopyTileMappings(d, dc, s, sc, z, f); }
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n, ID3D12CommandList* const* l) override { native_->ExecuteCommandLists(n, l); }
    void STDMETHODCALLTYPE SetMarker(UINT m, const void* d, UINT n) override { native_->SetMarker(m, d, n); }
    void STDMETHODCALLTYPE BeginEvent(UINT m, const void* d, UINT n) override { native_->BeginEvent(m, d, n); }
    void STDMETHODCALLTYPE EndEvent() override { native_->EndEvent(); }
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence* f, UINT64 v) override { return native_->Signal(f, v); }
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f, UINT64 v) override { return native_->Wait(f, v); }
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f) override { return native_->GetTimestampFrequency(f); }
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g, UINT64* c) override { return native_->GetClockCalibration(g, c); }
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override { return native_->GetDesc(); }
private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> native_;
    Microsoft::WRL::ComPtr<ID3D12Device> reported_;
    bool exposes_native_{};
    volatile LONG references_{1}, device_queries_{};
};
