#pragma once
#include "xrfg/native_ingress/D3D_DeviceOwner.h"

namespace xrfg {
// Native discovery can expose another COM identity for the same backing device.
// Allocate against the queue's reported owner so strict history/synthesis checks
// remain valid. Adapter equality alone never permits this handoff.
inline HRESULT luke_execution_owner(ID3D12Device* native_device,
    ID3D12CommandQueue* queue, ID3D12Device** owner) {
    if(!owner)return E_POINTER;
    *owner=nullptr;
    if(!native_device || !queue || queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)
        return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID3D12Device> reported;
    const auto result=queue->GetDevice(IID_PPV_ARGS(&reported));
    if(FAILED(result))return result;
    if(!d3d_owner::same(native_device,reported.Get()))return E_INVALIDARG;
    *owner=reported.Detach();
    return S_OK;
}
}
