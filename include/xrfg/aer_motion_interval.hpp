#pragma once
#include "dlss_motion_vectors.hpp"
#include <memory>

namespace xrfg {
// Session-owned colour witnesses. Never infer an interval from eye parity or
// from an SR feature's independent serial counter.
class AerMotionInterval {
public:
    struct Result {unsigned interval{},middle_interval{},one{},two{};};
    AerMotionInterval();
    ~AerMotionInterval();
    HRESULT observe(ID3D12Device*,ID3D12CommandQueue*,unsigned eye,ID3D12Resource* color,
        const DlssMotionVectorFrame& original,const DlssMotionVectorFrame& placed,
        Result&,ID3D12Fence**,std::uint64_t*) noexcept;
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
