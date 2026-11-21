#pragma once
#include "dlss_motion_vectors.hpp"
#include <memory>

namespace xrfg {
// Session-owned colour witnesses. Never infer an interval from eye parity or
// from an SR feature's independent serial counter.
class AerMotionInterval {
public:
    struct Result {
        unsigned interval{},middle_interval{},one{},two{};
        unsigned patch_attempted{},patch_one{},patch_two{};
        unsigned corroboration_age{};
        unsigned camera_interval{},camera_one{},camera_two{};
        bool camera_attempted{};
    };
    // History arguments must come from the exact same-eye and opposite-eye
    // endpoints used in the GPU vote. Pass empty results without that match.
    [[nodiscard]] static Result decide(unsigned interval, unsigned one, unsigned two,
        const Result& previous_same, const Result& previous_opposite) noexcept;
    AerMotionInterval();
    ~AerMotionInterval();
    HRESULT observe(ID3D12Device*,ID3D12CommandQueue*,unsigned eye,ID3D12Resource* color,
        const DlssMotionVectorFrame& original,const DlssMotionVectorFrame& placed,
        Result&,ID3D12Fence**,std::uint64_t*,std::uint64_t swapchain=0,std::uint64_t release_serial=0) noexcept;
private:
    struct Impl;std::shared_ptr<Impl> impl_;
};
}
