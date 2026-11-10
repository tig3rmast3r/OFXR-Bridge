#pragma once
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/native_ingress/D3D_DeviceOwner.h"
#include "xrfg/bridge_flight_logger.hpp"
#include <atomic>

namespace xrfg {
// F9 only, bounded CPU receipts. 1490 reason/publication/kind,1491 consumer/
// allocation owner,1492 consumed/bound resource. No capture or GPU work.
inline void report_guide_allocation_check(unsigned reason,ID3D12Device* consumer,
    const DlssMotionVectorFrame& frame,ID3D12Resource* resource,
    const DlssSnapshotAllocation* allocation) noexcept {
    auto& logger=bridge_flight_logger();if(!logger.enabled() || reason>=7)return;
    static std::array<std::atomic_uint64_t,7> deadlines{};
    static std::array<std::atomic_uint,7> counts{};
    if(counts[reason].load(std::memory_order_relaxed)>=24)return;
    const auto now=GetTickCount64();auto deadline=deadlines[reason].load(std::memory_order_relaxed);
    if(now<deadline || !deadlines[reason].compare_exchange_strong(deadline,now+2000,std::memory_order_relaxed))return;
    if(counts[reason].fetch_add(1,std::memory_order_relaxed)>=24)return;
    logger.event(BridgeFlightOperation::luke_afw_stage,reason,1490,frame.publication,resource==frame.depth.Get());
    logger.event(BridgeFlightOperation::luke_afw_stage,reason,1491,
        reinterpret_cast<std::uintptr_t>(consumer),reinterpret_cast<std::uintptr_t>(allocation?allocation->device.Get():nullptr));
    logger.event(BridgeFlightOperation::luke_afw_stage,reason,1492,
        reinterpret_cast<std::uintptr_t>(resource),reinterpret_cast<std::uintptr_t>(allocation?allocation->resource.Get():nullptr));
}
// Resource.GetDevice can expose REAL VR's alias while the allocation receipt
// names its native owner (or vice versa). Only Luke consumers admit this case,
// and only for our retained motion/depth snapshots with proven shared backing.
// An adapter match by itself never establishes resource ownership.
inline bool guide_resource_on_device(ID3D12Device* consumer,
    const DlssMotionVectorFrame& frame, ID3D12Resource* resource,
    bool allow_luke_alias) noexcept {
    using Microsoft::WRL::ComPtr;
    if(!consumer || !resource)return false;
    const auto* bound=resource==frame.motion_vectors.Get()?frame.motion_allocation.get():
        resource==frame.depth.Get()?frame.depth_allocation.get():nullptr;
    if(allow_luke_alias && bound) {
        // The creator already retained the exact resource/allocation pair.
        // A proxy returned later by GetDevice is not new allocation evidence.
        const unsigned reason=bound->resource.Get()!=resource?4:
            !bound->device || !d3d_owner::same(consumer,bound->device.Get())?5:6;
        report_guide_allocation_check(reason,consumer,frame,resource,bound);
        return reason==6;
    }
    ComPtr<ID3D12Device> reported;
    ComPtr<IUnknown> expected_identity,reported_identity;
    if(FAILED(resource->GetDevice(IID_PPV_ARGS(&reported))) ||
        FAILED(consumer->QueryInterface(IID_PPV_ARGS(&expected_identity))) ||
        FAILED(reported.As(&reported_identity))) {
        if(allow_luke_alias)report_guide_allocation_check(1,consumer,frame,resource,nullptr);
        return false;
    }
    if(expected_identity.Get()==reported_identity.Get())return true;
    if(!allow_luke_alias)return false;
    auto* allocation=resource==frame.motion_vectors.Get()?frame.motion_snapshot_device.Get():
        resource==frame.depth.Get()?frame.depth_snapshot_device.Get():nullptr;
    const bool accepted=allocation && d3d_owner::same(consumer,allocation) &&
        d3d_owner::same(allocation,reported.Get());
    if(!accepted)report_guide_allocation_check(3,consumer,frame,resource,nullptr);
    return accepted;
}
} // namespace xrfg
