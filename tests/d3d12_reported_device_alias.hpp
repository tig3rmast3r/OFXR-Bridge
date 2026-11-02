#pragma once
// Shared COM-alias fixture; forwards real hardware/WARP allocation and backing data.
// Real WARP operations are forwarded; only the reported COM device identity
// differs. Same adapter is intentionally NOT proof of snapshot ownership.
class ReportedDeviceAlias : public ID3D12Device {
public:
    explicit ReportedDeviceAlias(ID3D12Device* native, bool exposes_native = false,
        bool exposes_public_video=false,ID3D12Device* video_owner=nullptr)
        : native_(native),video_owner_(video_owner?video_owner:native),
          exposes_native_(exposes_native),exposes_public_video_(exposes_public_video) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (exposes_native_ && iid == xrfg::d3d_native::streamline_native_guid)
            return native_->QueryInterface(__uuidof(ID3D12Device), object);
        if(exposes_public_video_ && iid==__uuidof(ID3D12VideoDevice2)) {
            ++public_video_queries;
            return video_owner_->QueryInterface(iid,object);
        }
        if (iid != __uuidof(IUnknown) && iid != __uuidof(ID3D12Object) && iid != __uuidof(ID3D12Device))
            return E_NOINTERFACE;
        *object = static_cast<ID3D12Device*>(this); AddRef(); return S_OK;
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
    UINT STDMETHODCALLTYPE GetNodeCount() override { return native_->GetNodeCount(); }
    unsigned intercepted_queues{};
    unsigned public_video_queries{};
    HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC* d, REFIID i, void** p) override { ++intercepted_queues;return native_->CreateCommandQueue(d, i, p); }
    HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE t, REFIID i, void** p) override { return native_->CreateCommandAllocator(t, i, p); }
    HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, REFIID i, void** p) override { return native_->CreateGraphicsPipelineState(d, i, p); }
    HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC* d, REFIID i, void** p) override { return native_->CreateComputePipelineState(d, i, p); }
    HRESULT STDMETHODCALLTYPE CreateCommandList(UINT n, D3D12_COMMAND_LIST_TYPE t, ID3D12CommandAllocator* a, ID3D12PipelineState* s, REFIID i, void** p) override { return native_->CreateCommandList(n, t, a, s, i, p); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE f, void* d, UINT n) override { return native_->CheckFeatureSupport(f, d, n); }
    HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC* d, REFIID i, void** p) override { return native_->CreateDescriptorHeap(d, i, p); }
    UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE t) override { return native_->GetDescriptorHandleIncrementSize(t); }
    HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT n, const void* d, SIZE_T s, REFIID i, void** p) override { return native_->CreateRootSignature(n, d, s, i, p); }
    void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateConstantBufferView(d, h); }
    void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource* r, const D3D12_SHADER_RESOURCE_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateShaderResourceView(r, d, h); }
    void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource* r, ID3D12Resource* c, const D3D12_UNORDERED_ACCESS_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateUnorderedAccessView(r, c, d, h); }
    void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource* r, const D3D12_RENDER_TARGET_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateRenderTargetView(r, d, h); }
    void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateDepthStencilView(r, d, h); }
    void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC* d, D3D12_CPU_DESCRIPTOR_HANDLE h) override { native_->CreateSampler(d, h); }
    void STDMETHODCALLTYPE CopyDescriptors(UINT nd, const D3D12_CPU_DESCRIPTOR_HANDLE* d, const UINT* ds, UINT ns, const D3D12_CPU_DESCRIPTOR_HANDLE* s, const UINT* ss, D3D12_DESCRIPTOR_HEAP_TYPE t) override { native_->CopyDescriptors(nd, d, ds, ns, s, ss, t); }
    void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT n, D3D12_CPU_DESCRIPTOR_HANDLE d, D3D12_CPU_DESCRIPTOR_HANDLE s, D3D12_DESCRIPTOR_HEAP_TYPE t) override { native_->CopyDescriptorsSimple(n, d, s, t); }
    D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE GetResourceAllocationInfo(UINT m, UINT n, const D3D12_RESOURCE_DESC* d) override { return native_->GetResourceAllocationInfo(m, n, d); }
    D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE GetCustomHeapProperties(UINT n, D3D12_HEAP_TYPE t) override { return native_->GetCustomHeapProperties(n, t); }
    HRESULT STDMETHODCALLTYPE CreateCommittedResource(const D3D12_HEAP_PROPERTIES* h, D3D12_HEAP_FLAGS f, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID i, void** p) override { return native_->CreateCommittedResource(h, f, d, s, c, i, p); }
    HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC* d, REFIID i, void** p) override { return native_->CreateHeap(d, i, p); }
    HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap* h, UINT64 o, const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID i, void** p) override { return native_->CreatePlacedResource(h, o, d, s, c, i, p); }
    HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES s, const D3D12_CLEAR_VALUE* c, REFIID i, void** p) override { return native_->CreateReservedResource(d, s, c, i, p); }
    HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild* o, const SECURITY_ATTRIBUTES* a, DWORD d, LPCWSTR n, HANDLE* h) override { return native_->CreateSharedHandle(o, a, d, n, h); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE h, REFIID i, void** p) override { return native_->OpenSharedHandle(h, i, p); }
    HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(LPCWSTR n, DWORD d, HANDLE* h) override { return native_->OpenSharedHandleByName(n, d, h); }
    HRESULT STDMETHODCALLTYPE MakeResident(UINT n, ID3D12Pageable* const* p) override { return native_->MakeResident(n, p); }
    HRESULT STDMETHODCALLTYPE Evict(UINT n, ID3D12Pageable* const* p) override { return native_->Evict(n, p); }
    HRESULT STDMETHODCALLTYPE CreateFence(UINT64 v, D3D12_FENCE_FLAGS f, REFIID i, void** p) override { return native_->CreateFence(v, f, i, p); }
    HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return native_->GetDeviceRemovedReason(); }
    void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC* d, UINT f, UINT n, UINT64 b, D3D12_PLACED_SUBRESOURCE_FOOTPRINT* l, UINT* r, UINT64* s, UINT64* t) override { native_->GetCopyableFootprints(d, f, n, b, l, r, s, t); }
    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC* d, REFIID i, void** p) override { return native_->CreateQueryHeap(d, i, p); }
    HRESULT STDMETHODCALLTYPE SetStablePowerState(BOOL e) override { return native_->SetStablePowerState(e); }
    HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC* d, ID3D12RootSignature* s, REFIID i, void** p) override { return native_->CreateCommandSignature(d, s, i, p); }
    void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource* r, UINT* n, D3D12_PACKED_MIP_INFO* m, D3D12_TILE_SHAPE* s, UINT* t, UINT f, D3D12_SUBRESOURCE_TILING* d) override { native_->GetResourceTiling(r, n, m, s, t, f, d); }
    LUID STDMETHODCALLTYPE GetAdapterLuid() override { return native_->GetAdapterLuid(); }
private:
    Microsoft::WRL::ComPtr<ID3D12Device> native_;
    Microsoft::WRL::ComPtr<ID3D12Device> video_owner_;
    bool exposes_native_{};
    bool exposes_public_video_{};
    volatile LONG references_{1};
};
