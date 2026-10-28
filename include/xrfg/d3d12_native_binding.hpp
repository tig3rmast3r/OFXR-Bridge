#pragma once
#include <d3d12.h>
#include <d3d12video.h>
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include <wrl/client.h>
#include <utility>

namespace xrfg::d3d_native {
inline constexpr GUID streamline_native_guid{
    0xadec44e2, 0x61f0, 0x45c3, {0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff}};
inline constexpr GUID reshade_unwrapped_guid{
    0x7f2c9a11, 0x3b4e, 0x4d6a, {0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42}};

// Streamline/ReShade QI contracts own one reference on successful unwrapping.
// Input must own its underlying objects throughout use of the borrowed result.
inline bool get_borrowed(IUnknown* input, IUnknown** output,
    HRESULT* query_result = nullptr) noexcept {
    if (query_result) *query_result = E_POINTER;
    if (!output) return false;
    *output = nullptr;
    if (!input) return false;
    IUnknown* visited[8]{};
    auto* current = input;
    bool unwrapped = false;
    for (unsigned depth = 0; depth < 8; ++depth) {
        visited[depth] = current;
        IUnknown* next{};
        auto result = current->QueryInterface(streamline_native_guid,
            reinterpret_cast<void**>(&next));
        if (result == E_NOINTERFACE) {
            next = nullptr;
            result = current->QueryInterface(reshade_unwrapped_guid,
                reinterpret_cast<void**>(&next));
        }
        if (query_result) *query_result = result;
        if (result == E_NOINTERFACE) {
            *output = current;
            if (query_result && unwrapped) *query_result = S_OK;
            return true;
        }
        if (FAILED(result) || !next) return false;
        next->Release();
        if (next == current) {
            *output = current;
            return true;
        }
        for (unsigned i = 0; i <= depth; ++i) {
            if (visited[i] == next) {
                if (query_result) *query_result = E_UNEXPECTED;
                return false;
            }
        }
        current = next;
        unwrapped = true;
    }
    if (query_result) *query_result = E_UNEXPECTED;
    return false;
}

// Luke/REAL VR can expose a public native video interface while wrapping
// ID3D12Device and queue creation. Obtain its device via the supported COM
// contract and prove identical backing before private allocation. This never
// unwraps application queues/lists/resources or changes the runtime binding.
inline HRESULT allocation_owner(ID3D12Device* input,
    Microsoft::WRL::ComPtr<ID3D12Device>& output) noexcept {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Device> retained_input=input;
    output.Reset();if(!retained_input)return E_POINTER;
    // Pin successful device identities: an injector can log every otherwise
    // valid video-interface query. Do not repeat that query per guide/frame.
    // No failed result is cached; a full cache uses the same verified path.
    // The pins last the process: releasing a device while the layer detaches,
    // after the application has torn down its own, faults inside D3D12.
    struct Entry {ComPtr<IUnknown> identity;ComPtr<ID3D12Device> owner;};
    struct Cache {std::mutex mutex;std::array<Entry,16> entries;};
    static Cache& cache=*new Cache;
    ComPtr<IUnknown> identity;
    auto result=retained_input.As(&identity);
    if(FAILED(result))return result;
    {std::scoped_lock lock(cache.mutex);
        for(const auto& entry:cache.entries)
            if(entry.identity.Get()==identity.Get()){output=entry.owner;return S_OK;}}
    const auto accept=[&](ComPtr<ID3D12Device> owner) {
        std::scoped_lock lock(cache.mutex);
        for(auto& entry:cache.entries) {
            if(entry.identity.Get()==identity.Get()){output=entry.owner;return S_OK;}
            if(!entry.identity){entry.identity=identity;entry.owner=owner;break;}
        }
        output=std::move(owner);return S_OK;
    };
    IUnknown* borrowed{};
    if(!get_borrowed(retained_input.Get(),&borrowed))return E_INVALIDARG;
    ComPtr<ID3D12Device> device;
    result=borrowed->QueryInterface(IID_PPV_ARGS(&device));
    if(FAILED(result))return result;
    ComPtr<ID3D12VideoDevice2> video;
    result=device.As(&video);
    if(result==E_NOINTERFACE)return accept(std::move(device));
    if(FAILED(result))return result;
    ComPtr<ID3D12Device> candidate;
    result=video.As(&candidate);
    if(FAILED(result))return result;
    if(!d3d_owner::same(device.Get(),candidate.Get()))return E_INVALIDARG;
    return accept(std::move(candidate));
}

// Keep the application's binding untouched downstream, retaining canonical
// objects for guide ownership and bridge GPU allocations/synchronization.
// Adapter equality alone never admits a device/queue pair.
inline HRESULT canonical_binding(ID3D12Device* input_device,
    ID3D12CommandQueue* input_queue,
    Microsoft::WRL::ComPtr<ID3D12Device>& device,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& queue) noexcept {
    using Microsoft::WRL::ComPtr;
    device.Reset(); queue.Reset();
    IUnknown *native_device{}, *native_queue{};
    if (!get_borrowed(input_device, &native_device) ||
        !get_borrowed(input_queue, &native_queue)) return E_INVALIDARG;
    ComPtr<ID3D12Device> candidate_device;
    ComPtr<ID3D12CommandQueue> candidate_queue;
    auto result = native_device->QueryInterface(IID_PPV_ARGS(&candidate_device));
    if (FAILED(result)) return result;
    result = native_queue->QueryInterface(IID_PPV_ARGS(&candidate_queue));
    if (FAILED(result)) return result;
    ComPtr<ID3D12Device> reported_device;
    result = candidate_queue->GetDevice(IID_PPV_ARGS(&reported_device));
    if (FAILED(result)) return result;
    IUnknown* native_owner{};
    if (!get_borrowed(reported_device.Get(), &native_owner)) return E_INVALIDARG;
    ComPtr<IUnknown> device_identity, queue_owner_identity;
    result = candidate_device.As(&device_identity);
    if (FAILED(result)) return result;
    result = native_owner->QueryInterface(IID_PPV_ARGS(&queue_owner_identity));
    if (FAILED(result)) return result;
    if (device_identity.Get() != queue_owner_identity.Get()) return E_INVALIDARG;
    device = std::move(candidate_device); queue = std::move(candidate_queue);
    return S_OK;
}
} // namespace xrfg::d3d_native
