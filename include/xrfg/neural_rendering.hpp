#pragma once

#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace xrfg::neural_rendering {
struct Settings {
    bool enabled = false;
    bool before_sr = true;
    uint32_t preset = 0;
    float intensity = 1.f;
    uint32_t style = 0;
    float local_structure = 1.f;
    float local_tone = 1.f;
    float skin_structure = -1.f;
    bool auto_mask = true;
    float transfer_strength = 1.f;
    float colour_strength = 1.f;
    float max_ratio = 2.f;
    uint32_t debug_view = 0;
    uint32_t compare = 0;
    float compare_split = .5f;
    float compare_zoom = 1.f;
    bool compare_swap = false;
    float working_scale = 1.f;
    float white_point_scale = 1.f;
    bool diagnostic_logging = false;
    bool crash_debug = false;
    bool second_pass = false;
};
struct SubmissionReceipt {
    unsigned int handle = 0;
    uint64_t recording = 0;
    void* list = nullptr;
    std::shared_ptr<void> lifetime;
};
struct Status {
    bool enabled = false;
    bool running = false;
    bool before_sr = true;
    std::string reason;
    uint64_t evaluations = 0;
    // nvngx_dlssnr.dll, which OFXR cannot ship, was found neither beside OFXR
    // nor beside the game.
    bool model_missing = false;
};
// Configuration is serialized with evaluation; feature creation settings retain
// the accepted debounce/rebuild behavior and one history per native SR handle.
void configure(const Settings& settings);
// Crash-debug session lifetime, independent of NR/OFXR being selected.
// Empty without Crash debug; no rendering, GPU submission or polling.
std::shared_ptr<void> observe_device_removal(IUnknown* owner) noexcept;
// Only call for an exactly identified native SR/RR handle, never DLSS-G.
// The caller's parameter table is read only; Color/Output are edited in place.
void before_sr(unsigned int native_handle_id, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters, ID3D12CommandQueue* queue = nullptr);
void after_sr(unsigned int native_handle_id, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters, ID3D12CommandQueue* queue = nullptr);
// Evaluate under one configuration lock. False guarantees this call skipped
// post-SR NR without changing Output; true conservatively requires copyback.
bool after_sr_needs_copyback(unsigned int native_handle_id, ID3D12GraphicsCommandList* list,
    const NVSDK_NGX_Parameter* parameters, ID3D12CommandQueue* queue = nullptr);
// Detach BEFORE ExecuteCommandLists, complete AFTER actual submission. The
// detached recording serial prevents reused list pointers from stealing work.
std::vector<SubmissionReceipt> take_submission_receipts(unsigned int count,
    ID3D12CommandList* const* lists);
void complete_submission(ID3D12CommandQueue* queue,
    const std::vector<SubmissionReceipt>& receipts);
// Exact detached recording was reset/freed without Execute. Recreates its
// vendor history rather than reusing GPU initialization that never executed.
void abandon_submission(const std::vector<SubmissionReceipt>& receipts);
void retire_handle(unsigned int native_handle_id);
void retry();
Status status();
// Call only after application-owned NR recording/submission has quiesced.
void shutdown();
void request_capture(unsigned int frames);
} // namespace xrfg::neural_rendering
