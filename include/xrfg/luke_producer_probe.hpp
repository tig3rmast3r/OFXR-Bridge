#pragma once
#include "xrfg/d3d12_native_binding.hpp"

namespace xrfg {
enum class LukeProducerQueueFactory { allocation, observed };
// Discover an implementation only. Never publish this temporary queue as the
// producer: that still requires observing the game's exact submitted SR list.
inline HRESULT probe_luke_producer_queue(ID3D12Device* input,
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>& output,
    LukeProducerQueueFactory factory=LukeProducerQueueFactory::allocation) {
    using Microsoft::WRL::ComPtr;
    output.Reset();
    ComPtr<ID3D12Device> device;
    auto result=d3d_native::allocation_owner(input,device);
    if(FAILED(result))return result;
    D3D12_COMMAND_QUEUE_DESC description{};description.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> probe,queue;
    // Allocation and observation can legitimately expose different Execute
    // implementations. Discover each through its own factory, never a cached
    // function from another device. Both still require the same backing proof.
    auto* source=factory==LukeProducerQueueFactory::observed?input:device.Get();
    result=source->CreateCommandQueue(&description,IID_PPV_ARGS(&probe));
    if(FAILED(result))return result;
    IUnknown* borrowed{};
    if(!d3d_native::get_borrowed(probe.Get(),&borrowed))return E_NOINTERFACE;
    result=borrowed->QueryInterface(IID_PPV_ARGS(&queue));
    if(FAILED(result))return result;
    ComPtr<ID3D12Device> owner;
    result=queue->GetDevice(IID_PPV_ARGS(&owner));
    if(FAILED(result))return result;
    if(queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT ||
        !d3d_owner::same(device.Get(),owner.Get()))return E_INVALIDARG;
    output=std::move(queue);
    return S_OK;
}
}
