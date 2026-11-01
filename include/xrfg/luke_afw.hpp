#pragma once

#include "xrfg/d3d12_frame_synthesizer.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/embedded_control.hpp"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <future>

namespace xrfg {

// Experimental Luke Ross spatial stereo completion.  Luke remains the
// alternating-eye producer; this object consumes one final real eye plus the
// matching DLSS depth/motion bundle and writes only the missing peer eye.
class LukeAfwSynthesizer final {
public:
    LukeAfwSynthesizer() noexcept;
    ~LukeAfwSynthesizer();

    LukeAfwSynthesizer(const LukeAfwSynthesizer&) = delete;
    LukeAfwSynthesizer& operator=(const LukeAfwSynthesizer&) = delete;

    // plugin_path is PureDark's AFW provider. It is loaded only while AFW
    // renders the peer eye, from the first evaluation with AFW selected: with
    // Full OSR the layer renders it alone and the provider is never loaded,
    // so it may be missing.
    [[nodiscard]] HRESULT initialize(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        const std::filesystem::path& plugin_path) noexcept;

    [[nodiscard]] HRESULT synthesize(
        ID3D12Resource* source_color,
        ID3D12Resource* destination_color,
        const DlssMotionVectorFrame& guide,
        const D3D12ReprojectionView& source_view,
        const D3D12ReprojectionView& destination_view,
        std::uint32_t source_eye,
        float xr_eye_baseline,
        const embedded::AfwSettings& settings,
        bool native_peer_camera=false) noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    // Prepare a fresh isolated provider while published XR pixels stay intact.
    // Calls only poll ready futures; the render thread never joins pending work.
    void prepare_history_reset() noexcept;
    [[nodiscard]] bool history_reset_ready() noexcept;

private:
    struct Impl;
    HRESULT initialize_locked(ID3D12Device* device, ID3D12CommandQueue* queue,
        const std::filesystem::path& plugin_path, bool isolated_provider, bool native) noexcept;
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
    std::future<std::unique_ptr<Impl>> standby_;
    std::future<void> retirement_;
    bool standby_geometry_{},reset_prepared_{};
    // A provider that failed to load is not retried every frame; selecting
    // Full OSR again clears it.
    HRESULT provider_failure_{S_OK};
    void queue_standby_locked() noexcept;
    static HRESULT create_impl(ID3D12Device*,ID3D12CommandQueue*,
        const std::filesystem::path&,bool,bool,std::unique_ptr<Impl>&) noexcept;
};

}  // namespace xrfg
