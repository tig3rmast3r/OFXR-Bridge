#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"

namespace xrfg {
// Resource.GetDevice can expose REAL VR's alias while the allocation receipt
// names its native owner (or vice versa). Only Luke consumers admit this case,
// and only for our retained motion/depth snapshots with proven shared backing.
// An adapter match by itself never establishes resource ownership.
inline bool guide_resource_on_device(ID3D12Device* consumer,
    const DlssMotionVectorFrame& frame, ID3D12Resource* resource,
    bool allow_luke_alias) noexcept {
    using Microsoft::WRL::ComPtr;
    if(!consumer || !resource)return false;
    ComPtr<ID3D12Device> reported;
    ComPtr<IUnknown> expected_identity,reported_identity;
    if(FAILED(resource->GetDevice(IID_PPV_ARGS(&reported))) ||
        FAILED(consumer->QueryInterface(IID_PPV_ARGS(&expected_identity))) ||
        FAILED(reported.As(&reported_identity)))return false;
    if(expected_identity.Get()==reported_identity.Get())return true;
    if(!allow_luke_alias)return false;
    auto* allocation=resource==frame.motion_vectors.Get()?frame.motion_snapshot_device.Get():
        resource==frame.depth.Get()?frame.depth_snapshot_device.Get():nullptr;
    return allocation && d3d_owner::same(consumer,allocation) &&
        d3d_owner::same(allocation,reported.Get());
}
} // namespace xrfg
