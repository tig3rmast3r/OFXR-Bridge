#include "xrfg/d3d11_guide_transport.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "d3d11_depth_unpack_shader.hpp"
#include <wrl/client.h>
#include <array>
#include <mutex>
#include <vector>

namespace xrfg {
using Microsoft::WRL::ComPtr;
struct D3D11GuideTransport::Impl {
    struct Texture {
        ComPtr<ID3D11Texture2D> d11; ComPtr<ID3D12Resource> d12; D3D11_TEXTURE2D_DESC source{};
        // A packed depth-stencil's depth plane, unpacked into d11 (R32_FLOAT)
        // through `target`, read through `read_view` made for `read_from`.
        bool unpacked{};
        ComPtr<ID3D11UnorderedAccessView> target;
        ComPtr<ID3D11ShaderResourceView> read_view;
        ComPtr<ID3D11Texture2D> read_from;
    };
    std::array<Texture,role_count> textures;
    ComPtr<ID3D11ComputeShader> unpack_shader;
    ComPtr<ID3D11Device1> d11;
    ComPtr<ID3D11DeviceContext4> context;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D11Fence> copy11,done11;
    ComPtr<ID3D12Fence> copy12,done12;
    HANDLE event{};
    std::uint64_t copied{},submitted{};
    bool recording{};
    ~Impl() { if (event) CloseHandle(event); }
    bool complete() const { return !submitted || !done12 || done12->GetCompletedValue()>=submitted; }
    HRESULT drain() {
        if (complete()) return S_OK;
        HRESULT result=done12->SetEventOnCompletion(submitted,event);
        if (FAILED(result)) return result;
        return WaitForSingleObject(event,5000)==WAIT_OBJECT_0 ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    HRESULT prepare(Role role, ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
        if (desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1) return E_INVALIDARG;
        auto& texture=textures[role]; const auto& old=texture.source;
        if (texture.d12 && old.Width==desc.Width && old.Height==desc.Height && old.Format==desc.Format) return S_OK;
        DXGI_FORMAT format=desc.Format;
        if (format==DXGI_FORMAT_D32_FLOAT) format=DXGI_FORMAT_R32_TYPELESS;
        if (format==DXGI_FORMAT_D16_UNORM) format=DXGI_FORMAT_R16_TYPELESS;
        // Packed depth shares no format with D3D12: its depth plane goes over
        // as a float, extracted by a shader (0.2 V521). Typed D24S8/D32S8X24
        // resources cannot be read by a shader at all.
        if (format==DXGI_FORMAT_D24_UNORM_S8_UINT || format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT) return DXGI_ERROR_UNSUPPORTED;
        const bool unpacked=depth_plane_view(format)!=DXGI_FORMAT_UNKNOWN;
        if (unpacked && (role!=depth || (desc.BindFlags&D3D11_BIND_SHADER_RESOURCE)==0)) return DXGI_ERROR_UNSUPPORTED;
        HRESULT result=S_OK;
        if (unpacked) {
            format=DXGI_FORMAT_R32_FLOAT;
            if (!unpack_shader) {
                result=d11->CreateComputeShader(g_xrfg_d3d11_depth_unpack_shader,sizeof(g_xrfg_d3d11_depth_unpack_shader),
                    nullptr,unpack_shader.GetAddressOf());
                if (FAILED(result)) return result;
            }
        }
        D3D12_RESOURCE_DESC shared{}; shared.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shared.Width=desc.Width; shared.Height=desc.Height; shared.DepthOrArraySize=1; shared.MipLevels=1;
        shared.Format=format; shared.SampleDesc.Count=1;
        shared.Flags=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};support.Format=format;
        if (unpacked || ((role==color || role==output) &&
            SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,&support,sizeof(support))) &&
            (support.Support1&D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)!=0 &&
            (support.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)!=0))
            shared.Flags|=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        Texture replacement;
        result=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&shared,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(replacement.d12.GetAddressOf()));
        if (FAILED(result)) return result;
        HANDLE handle{}; result=device->CreateSharedHandle(replacement.d12.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
        if (FAILED(result)) return result;
        result=d11->OpenSharedResource1(handle,IID_PPV_ARGS(replacement.d11.GetAddressOf())); CloseHandle(handle);
        if (FAILED(result)) return result;
        if (unpacked) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{}; view.Format=format; view.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
            result=d11->CreateUnorderedAccessView(replacement.d11.Get(),&view,replacement.target.GetAddressOf());
            if (FAILED(result)) return result;
        }
        replacement.source=desc; replacement.unpacked=unpacked; texture=std::move(replacement); return S_OK;
    }
    static DXGI_FORMAT depth_plane_view(DXGI_FORMAT format) {
        return format==DXGI_FORMAT_R24G8_TYPELESS ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS :
            format==DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_UNKNOWN;
    }
    static bool same(IUnknown* a, IUnknown* b) {
        ComPtr<IUnknown> x,y;
        if (a) a->QueryInterface(IID_PPV_ARGS(x.GetAddressOf()));
        if (b) b->QueryInterface(IID_PPV_ARGS(y.GetAddressOf()));
        return x && x.Get()==y.Get();
    }
    // On the game's context: every compute binding touched is put back, and a
    // depth buffer still bound for writing is unbound for the read, then rebound.
    HRESULT unpack(Texture& texture, ID3D11Texture2D* source) {
        if (!texture.read_view || texture.read_from.Get()!=source) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view{}; view.Format=depth_plane_view(texture.source.Format);
            view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
            texture.read_view.Reset(); texture.read_from.Reset();
            const HRESULT result=d11->CreateShaderResourceView(source,&view,texture.read_view.GetAddressOf());
            if (FAILED(result)) return result;
            texture.read_from=source;
        }
        ComPtr<ID3D11ComputeShader> shader; context->CSGetShader(shader.GetAddressOf(),nullptr,nullptr);
        ComPtr<ID3D11ShaderResourceView> read; context->CSGetShaderResources(0,1,read.GetAddressOf());
        ComPtr<ID3D11UnorderedAccessView> write; context->CSGetUnorderedAccessViews(0,1,write.GetAddressOf());
        std::array<ID3D11RenderTargetView*,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        ID3D11DepthStencilView* depth_view{};
        context->OMGetRenderTargets(static_cast<UINT>(targets.size()),targets.data(),&depth_view);
        bool unbound=false;
        if (depth_view) {
            ComPtr<ID3D11Resource> bound; depth_view->GetResource(bound.GetAddressOf());
            if (same(bound.Get(),source)) {
                context->OMSetRenderTargetsAndUnorderedAccessViews(static_cast<UINT>(targets.size()),targets.data(),nullptr,
                    0,D3D11_KEEP_UNORDERED_ACCESS_VIEWS,nullptr,nullptr);
                unbound=true;
            }
        }
        ID3D11ShaderResourceView* depth_read=texture.read_view.Get();
        ID3D11UnorderedAccessView* depth_write=texture.target.Get();
        const UINT keep_counter=static_cast<UINT>(-1);
        context->CSSetShader(unpack_shader.Get(),nullptr,0);
        context->CSSetShaderResources(0,1,&depth_read);
        context->CSSetUnorderedAccessViews(0,1,&depth_write,&keep_counter);
        context->Dispatch((texture.source.Width+7U)/8U,(texture.source.Height+7U)/8U,1);
        ID3D11ShaderResourceView* old_read=read.Get();
        ID3D11UnorderedAccessView* old_write=write.Get();
        context->CSSetShader(shader.Get(),nullptr,0);
        context->CSSetShaderResources(0,1,&old_read);
        context->CSSetUnorderedAccessViews(0,1,&old_write,&keep_counter);
        if (unbound)
            context->OMSetRenderTargetsAndUnorderedAccessViews(static_cast<UINT>(targets.size()),targets.data(),depth_view,
                0,D3D11_KEEP_UNORDERED_ACCESS_VIEWS,nullptr,nullptr);
        for (auto* target:targets) if (target) target->Release();
        if (depth_view) depth_view->Release();
        return S_OK;
    }
    static void retain(std::unique_ptr<Impl> pending) {
        // An incomplete GPU submission retains its allocator/resources until its
        // actual fence completes. A timeout never authorizes early destruction.
        static auto* mutex=new std::mutex;
        static auto* retired=new std::vector<std::unique_ptr<Impl>>;
        std::scoped_lock lock(*mutex);
        std::erase_if(*retired,[](const auto& value){return value->complete();});
        if (pending && !pending->complete()) retired->push_back(std::move(pending));
    }
};
D3D11GuideTransport::D3D11GuideTransport()=default;
D3D11GuideTransport::~D3D11GuideTransport() {
    if (impl_ && FAILED(impl_->drain())) Impl::retain(std::move(impl_));
}
HRESULT D3D11GuideTransport::initialize(ID3D11DeviceContext* context, ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
    try {
        if (!context || !device || !queue || context->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) return E_INVALIDARG;
        if (impl_) return E_UNEXPECTED;
        auto value=std::make_unique<Impl>(); value->device=device; value->queue=queue;
        ComPtr<ID3D11Device> source; context->GetDevice(source.GetAddressOf());
        HRESULT result=source.As(&value->d11); if (FAILED(result)) return result;
        result=context->QueryInterface(IID_PPV_ARGS(value->context.GetAddressOf())); if (FAILED(result)) return result;
        ComPtr<ID3D11Device5> device5; result=source.As(&device5); if (FAILED(result)) return result;
        ComPtr<ID3D12Device> queue_device; result=queue->GetDevice(IID_PPV_ARGS(queue_device.GetAddressOf()));
        ComPtr<IUnknown> identity_a,identity_b;
        device->QueryInterface(IID_PPV_ARGS(identity_a.GetAddressOf()));
        if (queue_device) queue_device.As(&identity_b);
        if (FAILED(result) || !identity_a || identity_a.Get()!=identity_b.Get()) return E_INVALIDARG;
        result=device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(value->copy11.GetAddressOf())); if (FAILED(result)) return result;
        HANDLE handle{}; result=value->copy11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle); if (FAILED(result)) return result;
        result=device->OpenSharedHandle(handle,IID_PPV_ARGS(value->copy12.GetAddressOf())); CloseHandle(handle); if (FAILED(result)) return result;
        result=device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(value->done12.GetAddressOf())); if (FAILED(result)) return result;
        result=device->CreateSharedHandle(value->done12.Get(),nullptr,GENERIC_ALL,nullptr,&handle); if (FAILED(result)) return result;
        result=device5->OpenSharedFence(handle,IID_PPV_ARGS(value->done11.GetAddressOf())); CloseHandle(handle); if (FAILED(result)) return result;
        result=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(value->allocator.GetAddressOf())); if (FAILED(result)) return result;
        result=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,value->allocator.Get(),nullptr,IID_PPV_ARGS(value->list.GetAddressOf())); if (FAILED(result)) return result;
        result=value->list->Close(); if (FAILED(result)) return result;
        value->event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if (!value->event) return HRESULT_FROM_WIN32(GetLastError());
        impl_=std::move(value); return S_OK;
    } catch (...) { return E_FAIL; }
}
HRESULT D3D11GuideTransport::begin() noexcept {
    if (!impl_) return E_UNEXPECTED;
    if (impl_->recording) return E_UNEXPECTED;
    HRESULT result=impl_->drain(); if (FAILED(result)) return result;
    result=impl_->allocator->Reset(); if (FAILED(result)) return result;
    result=impl_->list->Reset(impl_->allocator.Get(),nullptr); if (FAILED(result)) return result;
    impl_->recording=true; return S_OK;
}
HRESULT D3D11GuideTransport::copy(Role role, ID3D11Resource* source) noexcept {
    try {
        if (!impl_ || !impl_->recording || role<0 || role>=role_count || !source) return E_INVALIDARG;
        ComPtr<ID3D11Device> source_device; source->GetDevice(source_device.GetAddressOf());
        ComPtr<IUnknown> a,b; source_device.As(&a); impl_->d11.As(&b); if (!a || a.Get()!=b.Get()) return E_INVALIDARG;
        ComPtr<ID3D11Texture2D> texture; HRESULT result=source->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())); if (FAILED(result)) return result;
        result=impl_->prepare(role,texture.Get()); if (FAILED(result)) return result;
        if (impl_->textures[role].unpacked) {result=impl_->unpack(impl_->textures[role],texture.Get()); if (FAILED(result)) return result;}
        else impl_->context->CopyResource(impl_->textures[role].d11.Get(),texture.Get());
        result=impl_->context->Signal(impl_->copy11.Get(),++impl_->copied); if (FAILED(result)) return result;
        impl_->context->Flush(); return impl_->queue->Wait(impl_->copy12.Get(),impl_->copied);
    } catch (...) { return E_FAIL; }
}
HRESULT D3D11GuideTransport::submit() noexcept {
    if (!impl_ || !impl_->recording) return E_UNEXPECTED;
    HRESULT result=impl_->list->Close(); impl_->recording=false; if (FAILED(result)) return result;
    ID3D12CommandList* lists[]{impl_->list.Get()}; impl_->queue->ExecuteCommandLists(1,lists);
    result=impl_->queue->Signal(impl_->done12.Get(),++impl_->submitted);
    report_dlss_motion_vector_submission(impl_->queue.Get(),1,lists);
    return result;
}
HRESULT D3D11GuideTransport::copy_back(Role role, ID3D11Resource* target) noexcept {
    if (!impl_ || impl_->recording || !impl_->submitted || role<0 || role>=role_count || !target) return E_INVALIDARG;
    ComPtr<ID3D11Texture2D> texture; HRESULT result=target->QueryInterface(IID_PPV_ARGS(texture.GetAddressOf())); if (FAILED(result)) return result;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc); const auto& expected=impl_->textures[role].source;
    if (!impl_->textures[role].d11 || impl_->textures[role].unpacked || desc.Width!=expected.Width || desc.Height!=expected.Height || desc.Format!=expected.Format ||
        desc.ArraySize!=1 || desc.MipLevels!=1 || desc.SampleDesc.Count!=1) return E_INVALIDARG;
    result=impl_->context->Wait(impl_->done11.Get(),impl_->submitted); if (FAILED(result)) return result;
    impl_->context->CopyResource(target,impl_->textures[role].d11.Get());
    // Publish the D3D11 read completion before a later DX12 write/reallocation.
    result=impl_->context->Signal(impl_->copy11.Get(),++impl_->copied); if (FAILED(result)) return result;
    impl_->context->Flush(); result=impl_->queue->Wait(impl_->copy12.Get(),impl_->copied);
    if (FAILED(result)) return result;
    return impl_->queue->Signal(impl_->done12.Get(),++impl_->submitted);
}
HRESULT D3D11GuideTransport::drain() noexcept { return impl_ ? impl_->drain() : S_OK; }
ID3D12Resource* D3D11GuideTransport::resource(Role role) const noexcept { return impl_ && role>=0 && role<role_count ? impl_->textures[role].d12.Get() : nullptr; }
ID3D12GraphicsCommandList* D3D11GuideTransport::list() const noexcept { return impl_ && impl_->recording ? impl_->list.Get() : nullptr; }
ID3D12Device* D3D11GuideTransport::device() const noexcept { return impl_ ? impl_->device.Get() : nullptr; }
ID3D12CommandQueue* D3D11GuideTransport::queue() const noexcept { return impl_ ? impl_->queue.Get() : nullptr; }
}
