#include "xrfg/two_frame_capture.hpp"
namespace xrfg {
std::uint64_t two_frame_capture_claim_nr(std::uint64_t) noexcept{return 0;}
void two_frame_capture_nr_evaluation(const TwoFrameNrEvaluation&) noexcept{}
void two_frame_capture_record_ng_x(const TwoFrameImageInfo&,ID3D12Device*,ID3D12GraphicsCommandList*,ID3D12Resource*,D3D12_RESOURCE_STATES) noexcept{}
}
