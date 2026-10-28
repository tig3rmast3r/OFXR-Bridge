#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <atomic>
#include <memory>

namespace xrfg::native_vulkan {
enum class RecordingDisposition : unsigned char { recording, submitted, abandoned };
// Receipt for an observed original recording, even before any split exists.
// Submitted means the original driver accepted Submit1/2, not GPU retirement.
// Disposal abandons only an epoch that was never successfully submitted.
struct RecordingReceipt {
    std::atomic<RecordingDisposition> disposition{RecordingDisposition::recording};
    uint64_t epoch{};
    std::atomic<bool> disposed{false};
    std::atomic<VkQueue> submitted_queue{VK_NULL_HANDLE};
    std::atomic<uint64_t> submission_serial{};
};
inline bool guide_submission_precedes(const RecordingReceipt& receipt,VkQueue queue,uint64_t cutoff) noexcept {
 const auto serial=receipt.submission_serial.load(std::memory_order_acquire);
 return serial&&serial<=cutoff&&receipt.disposition.load(std::memory_order_acquire)==RecordingDisposition::submitted&&
     receipt.submitted_queue.load(std::memory_order_acquire)==queue;
}
inline bool guide_recording_reusable(const std::shared_ptr<RecordingReceipt>& receipt,
 uint64_t read_done,uint64_t completed) noexcept {
 return (!receipt||receipt->disposed.load(std::memory_order_acquire))&&
     completed!=~uint64_t{0}&&completed>=read_done;
}
// Shared identity belongs to one successful split in one recording epoch.
// Abandonment is an exact reset/free/begin event, never an elapsed-frame guess.
struct RecordingLifetime {
    std::atomic<RecordingDisposition> disposition{RecordingDisposition::recording};
    std::atomic<bool> callback_delivered{false};
    uint64_t epoch{};
};
// One external D3D12-fence timeline orders prefix capture -> D3D12 work ->
// continuation copyback. Optional suffix signal proves native copyback retired.
struct CommandHandoff {
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t prefix_signal = 0;
    uint64_t suffix_wait = 0;
    uint64_t suffix_signal = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    std::shared_ptr<void> submission_context;
    // Queue work must be deferred to this callback. A recording that the game
    // never submits must never leave a D3D12 queue waiting for its prefix.
    void (*submitted)(VkQueue,void*) noexcept = nullptr;
    void (*abandoned)(void*) noexcept = nullptr;
};
struct CommandSplit {
    VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
    VkCommandBuffer continuation = VK_NULL_HANDLE;
    uint64_t recording_epoch = 0;
    std::shared_ptr<RecordingLifetime> lifetime;
};
bool initialize_commands(VkInstance instance,VkPhysicalDevice physical_device,
    VkDevice device,PFN_vkGetInstanceProcAddr gipa,PFN_vkGetDeviceProcAddr gdpa);
// Called by the autonomous GIPA/GDPA hooks. Null device denotes GIPA resolution
// before device creation; actual calls route by their observed dispatch owner.
// Returns a hook only when an
// original exists; never invents unsupported commands or mutations of pNext.
PFN_vkVoidFunction resolve_command(VkDevice device,const char* name,
    PFN_vkVoidFunction original) noexcept;
// Early selected-game coverage of actual loader exports, including pointers
// cached before GIPA/GDPA interception. Internal work uses their trampolines.
unsigned install_command_entry_hooks(void* loader_module) noexcept;
PFN_vkVoidFunction unhooked_command(PFN_vkVoidFunction function) noexcept;
// The current actual prefix/continuation for native NGX and raw copy commands.
VkCommandBuffer native(VkCommandBuffer original) noexcept;
// Returns only the device associated with an observed native allocation.
VkDevice device_for(VkCommandBuffer original) noexcept;
// External-memory ownership transfers use the observed original command-pool
// family; the OpenXR binding's queue may belong to another family entirely.
// UINT32_MAX means the application's allocation/pool was not observed.
uint32_t queue_family_for(VkCommandBuffer original) noexcept;
bool can_split(VkCommandBuffer original) noexcept;
// 1 missing device, 2 missing allocation, 4 not recording, 8 missing pool,
// 16 secondary, 32 rendering, 64 replay overflow, 128 submitted, 256 no state.
unsigned split_refusal(VkCommandBuffer original) noexcept;
// Own capture resources before the first raw copy, including recordings whose
// later split fails. References survive submission and are released outside the
// command mutex only when the application resets/frees that exact recording.
// Vulkan requires those disposal operations to follow GPU retirement.
bool retain_recording(VkCommandBuffer original,std::shared_ptr<void> resources) noexcept;
// Obtain before raw copies. The same original/owned continuation shares this
// exact epoch receipt; failed driver submission leaves it recording. It adds
// no segments, callbacks, waits or submission mutations.
std::shared_ptr<RecordingReceipt> recording_receipt(VkCommandBuffer original) noexcept;
// Minimal lifetime/submit observation: no draw/descriptor/image interception.
bool initialize_guide_commands(VkInstance,VkPhysicalDevice,VkDevice,
    PFN_vkGetInstanceProcAddr,PFN_vkGetDeviceProcAddr) noexcept;
std::shared_ptr<RecordingReceipt> guide_recording_receipt(VkCommandBuffer) noexcept;
uint64_t guide_submission_cutoff() noexcept;
using RecordCopyback = VkResult(*)(VkCommandBuffer continuation,void* context);
// Ends current prefix, begins an owned suffix, records optional native copyback
// before replaying accepted bindings/push constants/dynamic state. Original
// waits stay on prefix; application signals/fence remain on the final suffix.
// Repeated splits on one original command buffer preserve recording order.
CommandSplit split(VkCommandBuffer original,const CommandHandoff& handoff,
    RecordCopyback copyback = nullptr,void* context = nullptr) noexcept;
uint64_t recording_epoch(VkCommandBuffer original) noexcept;
// Removes all device metadata after the application's queue work is quiescent.
void destroy_commands(VkDevice device) noexcept;
} // namespace xrfg::native_vulkan
