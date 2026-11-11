#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <memory>
#include <span>

namespace xrfg {

// Gives each eye's DLSS vectors as they reach that eye's own previous image
// where the game wrote them into the other eye's (see
// DlssMotionVectorFrame::bridge and guide_composer.hlsl). The GPU decides,
// every pair, whether the vectors point into the other eye; where they do not,
// they are copied unchanged.
class D3D12GuideComposer {
public:
    static constexpr UINT kSlots = 8;

    [[nodiscard]] HRESULT initialize(ID3D12Device* device) noexcept;

    // Records into `list` and returns `set` with each eye that has a bridge
    // given vectors of its own, written to the slot's textures: the slot must
    // not be recorded again until this list has completed. Eyes without one,
    // and every eye when nothing can be done, are returned as they are.
    [[nodiscard]] std::shared_ptr<const DlssMotionVectorSet> compose(
        ID3D12GraphicsCommandList* list, UINT slot,
        const std::shared_ptr<const DlssMotionVectorSet>& set,
        std::span<const D3D12ReprojectionView> views,bool reject_occlusions=false) noexcept;

    // Pairs composed, for diagnostics: the vectors were given a bridge and the
    // passes were recorded (the GPU still decides whether to follow it).
    [[nodiscard]] std::uint64_t composed() const noexcept { return composed_; }

private:
    struct EyeResources {
        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        Microsoft::WRL::ComPtr<ID3D12Resource> decision;
        std::shared_ptr<const DlssSnapshotAllocation> allocation;
        UINT width{};
        UINT height{};
    };

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> decide_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compose_;
    std::array<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, kSlots> heaps_;
    std::array<std::array<EyeResources, kDlssMotionVectorEyeCount>, kSlots> eyes_;
    UINT increment_{};
    std::uint64_t composed_{};
};

}  // namespace xrfg
