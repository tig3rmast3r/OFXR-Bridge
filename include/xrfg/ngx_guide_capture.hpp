#pragma once
#include <d3d12.h>
#include <cstdint>

namespace xrfg {
// Runtime identity observed from successful xrGetInstanceProperties, not a user flag.
void set_ngx_capture_steamvr_runtime(bool steamvr) noexcept;
// Capture the game's NGX upscaler guides without replacing its upscaler.
// Called outside the loader lock. Disabling preserves submitted snapshots
// until the layer drains its queues and retires the guide registry.
void configure_ngx_guide_capture(ID3D12CommandQueue* queue, bool enabled) noexcept;
void stop_ngx_guide_capture(ID3D12CommandQueue* queue) noexcept;
// Successful application xrBeginFrame only; internal generated frames are excluded.
void observe_ngx_application_begin(std::uint64_t session) noexcept;
void update_ngx_guide_depth(float near_z, float far_z, float min_depth, float max_depth) noexcept;
// Lets NGX find the nvngx_dlssg.dll in directory when the game initialises
// NGX before OFXR does. Call before the game's renderer starts, outside the
// loader lock.
void prepare_ngx_feature_discovery(const wchar_t* directory) noexcept;
}
