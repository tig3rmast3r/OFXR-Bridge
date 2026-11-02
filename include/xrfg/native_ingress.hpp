#pragma once
#include "dlss_motion_vectors.hpp"
#include "luke_detection.hpp"
#include <cstdint>

namespace xrfg::native_ingress {
// Native production, never newest-frame camera recovery. The current Streamline
// frame/viewport or KCD list-bound packet owns all camera/history metadata.
bool apply_camera(DlssMotionVectorPublication& publication,
    ID3D12GraphicsCommandList* command_list) noexcept;
void configure_queue(ID3D12CommandQueue* queue) noexcept;
void install_device(ID3D12Device* device) noexcept;
// Install successful-Reset observation on the canonical original guide list.
// This grants no Luke/producer authority and applies equally to ordinary DLSS.
// Once installed, observation is an atomic-only fast path on later evaluations.
void observe_guide_list(ID3D12GraphicsCommandList* list) noexcept;
// Bind only a native queue that actually executes an observed game SR/RR list.
// Layer-created queues and queue creation alone are not producer evidence.
void observe_luke_production(ID3D12GraphicsCommandList* list) noexcept;
bool query_luke_context(ID3D12Device** device, ID3D12CommandQueue** queue) noexcept;
// REAL VR's DLSS-fix constant buffer as the camera source for a REAL VR title
// without Streamline (Uncharted). At capture, the fix's buffer is found among
// the list's bound compute constants by its shape and remembered for the
// guide about to be published; at that list's execution, after REAL VR has
// filled it, its camera is attached to the guide.
// bound_after: real_vr_bind_mark() taken when the evaluation began.
bool take_real_vr_camera_binding(ID3D12GraphicsCommandList* list,
    std::uint32_t motion_width, std::uint32_t motion_height, std::uint64_t bound_after) noexcept;
std::uint64_t real_vr_bind_mark() noexcept;
void register_pending_real_vr_camera(std::uint64_t stream, std::uint64_t publication) noexcept;
void poll_streamline() noexcept;
void poll_d3d11_ngx() noexcept;
// One line in ofxr-ngx-capture-pid*.log, written only while diagnostics are on.
void capture_log(const char* message, std::uint64_t a = 0, std::uint64_t b = 0,
    std::uint64_t c = 0, std::uint64_t d = 0) noexcept;
// The public NGX SDK intentionally makes native handles opaque. Each observed
// creation receives a process-unique NR key; never inspect private handle bytes.
std::uint32_t allocate_handle_key() noexcept;
void stop_queue(ID3D12CommandQueue* queue) noexcept;
struct Statistics {
    bool luke{}, streamline{};
    std::uint64_t camera_accepted{}, camera_rejected{}, submissions{};
};
Statistics statistics() noexcept;
// Read-only F9 snapshot; never establishes a producer or changes hook policy.
struct LukeContextDiagnostics {
    bool admitted{}, execute_hook{}, create_queue_hook{};
    std::uint32_t pending_lists{}, registered_queues{};
    std::uintptr_t execute_entry{}, device{}, queue{};
    std::uint64_t submissions{};
    std::uintptr_t execute_game_entry{}; // second hooked implementation, 0 if none
};
LukeContextDiagnostics luke_context_diagnostics() noexcept;
}
