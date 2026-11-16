#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include "xrfg/d3d12_native_binding.hpp"

namespace DlssNr
{
// Match the queue observer's canonical identity through all Streamline/ReShade
// layers. Keep the application's recording interface unchanged; only NR's
// submission-retirement key uses the underlying list.
inline Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> NrNativeCommandList(ID3D12CommandList* list)
{
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> identity;
    IUnknown* native{};
    if (xrfg::d3d_native::get_borrowed(list, &native))
        native->QueryInterface(IID_PPV_ARGS(&identity));
    return identity;
}
}
