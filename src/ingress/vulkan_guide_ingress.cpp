// DLSS guides from bridged Vulkan games other than REAL VR's (Star Citizen,
// No Man's Sky), ported from 0.2-V513/V514. The game's DLSS runs on its
// Vulkan device; the synthesis on the layer's D3D12 device. At each SR/RR
// evaluation, in the game's own command buffer ahead of DLSS, the vectors are
// copied into an image imported from a D3D12 shared texture, and the depth
// through a buffer into another (Vulkan copies no depth aspect into a colour
// image directly; through a buffer the packed depth-stencil formats give their
// depth too). Nothing else of the game is hooked: no command, image or device
// entry point, which is what made the native Vulkan ingress too costly for
// No Man's Sky (V457) and why that one stays REAL VR-only.
//
// The command buffer runs whenever the game submits it, which the layer does
// not see. The copies are published at the bridged xrReleaseSwapchainImage:
// by then the game has submitted the frame's work, and the bridge has just
// made the D3D12 queue wait for the game's queue; the snapshot copies are
// queued behind that wait (0.2's untracked mode, confirmed in Star Citizen).
// A game recording ahead may have one copy published before its submission,
// and DLSS on another queue than the session's is not ordered by that wait.
// A copy whose slot comes round again unpublished is dropped, never reused.
// DLSS's inputs are taken to be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
// as NVIDIA asks of them.
#include "xrfg/vulkan_native_ingress.hpp"
#include "xrfg/native_ingress.hpp"
#include "xrfg/dlss_motion_vectors.hpp"
#include "xrfg/luke_vulkan_scope.hpp"
#include "xrfg/bridge_flight_logger.hpp"
#include <nvsdk_ngx_vk.h>
#include <safetyhook.hpp>
#include <wrl/client.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace xrfg::native_vulkan {
namespace {
using Microsoft::WRL::ComPtr;

// Flight record vulkan_bridge results of this capture: 6 configured (a enabled,
// b hooked modules), 7 a stream's first publication (a stream | 1<<32 depth,
// b motion/depth VkFormat, c output extent), 8 a skip reason's first
// occurrence (a reason, b count, c detail), 9 dropped unsubmitted copies.
enum Skip : unsigned {
    unknown_feature = 1, missing_resources = 2, foreign_command_buffer = 3,
    transport_failed = 4, stage_failed = 5, placeholder_failed = 6, entry_points = 7,
    skip_count = 8,
};

struct VulkanApi {
    VulkanSessionBinding binding{};
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkCreateImage create_image{};
    PFN_vkDestroyImage destroy_image{};
    PFN_vkGetImageMemoryRequirements image_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindImageMemory bind_image_memory{};
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements buffer_requirements{};
    PFN_vkBindBufferMemory bind_buffer_memory{};
    PFN_vkCmdPipelineBarrier barrier{};
    PFN_vkCmdCopyImage copy_image{};
    PFN_vkCmdCopyImageToBuffer copy_image_to_buffer{};
    PFN_vkCmdCopyBufferToImage copy_buffer_to_image{};

    [[nodiscard]] bool load(const VulkanSessionBinding& session) noexcept {
        binding = session;
        const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
        const auto gipa = loader != nullptr
            ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"))
            : nullptr;
        if (gipa == nullptr || binding.instance == VK_NULL_HANDLE || binding.device == VK_NULL_HANDLE) return false;
        const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(binding.instance, "vkGetDeviceProcAddr"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            gipa(binding.instance, "vkGetPhysicalDeviceMemoryProperties"));
        if (gdpa == nullptr || properties == nullptr) return false;
        properties(binding.physical_device, &memory);
        const auto load = [&](auto& target, const char* name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(gdpa(binding.device, name));
            return target != nullptr;
        };
        return load(create_image, "vkCreateImage") && load(destroy_image, "vkDestroyImage") &&
            load(image_requirements, "vkGetImageMemoryRequirements") &&
            load(allocate_memory, "vkAllocateMemory") && load(free_memory, "vkFreeMemory") &&
            load(bind_image_memory, "vkBindImageMemory") && load(create_buffer, "vkCreateBuffer") &&
            load(destroy_buffer, "vkDestroyBuffer") &&
            load(buffer_requirements, "vkGetBufferMemoryRequirements") &&
            load(bind_buffer_memory, "vkBindBufferMemory") && load(barrier, "vkCmdPipelineBarrier") &&
            load(copy_image, "vkCmdCopyImage") && load(copy_image_to_buffer, "vkCmdCopyImageToBuffer") &&
            load(copy_buffer_to_image, "vkCmdCopyBufferToImage");
    }

    [[nodiscard]] std::uint32_t device_memory_type(std::uint32_t bits) const noexcept {
        std::uint32_t any = UINT32_MAX;
        for (std::uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
            if ((bits & (1U << index)) == 0) continue;
            if ((memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) return index;
            if (any == UINT32_MAX) any = index;
        }
        return any;
    }
};

// The D3D12 format a vector image's copy takes.
[[nodiscard]] DXGI_FORMAT vector_dxgi_format(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R32G32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case VK_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case VK_FORMAT_R16G16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// How a depth image reaches a colour copy the synthesizer reads.
struct DepthRoute {
    DXGI_FORMAT dxgi{DXGI_FORMAT_UNKNOWN};
    VkFormat shared{VK_FORMAT_UNDEFINED};
    VkImageAspectFlags barrier_aspect{};
    bool via_buffer{};
    std::uint32_t texel_bytes{};
};

[[nodiscard]] DepthRoute depth_route(VkFormat format) noexcept {
    constexpr VkImageAspectFlags depth = VK_IMAGE_ASPECT_DEPTH_BIT;
    constexpr VkImageAspectFlags both = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    switch (format) {
        case VK_FORMAT_D32_SFLOAT: return {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, depth, true, 4};
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, both, true, 4};
        case VK_FORMAT_D16_UNORM: return {DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, depth, true, 2};
        case VK_FORMAT_D16_UNORM_S8_UINT: return {DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, both, true, 2};
        case VK_FORMAT_R32_SFLOAT:
            return {DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, false, 4};
        case VK_FORMAT_R16_UNORM:
            return {DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, false, 2};
        default: return {};
    }
}

[[nodiscard]] HRESULT hresult_of(VkResult result) noexcept {
    return result == VK_SUCCESS ? S_OK
        : result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY ? E_OUTOFMEMORY
        : static_cast<HRESULT>(0x887A0000L | (static_cast<std::uint32_t>(-result) & 0xFFFFU));
}

class GuideTransport {
public:
    struct Staged {
        ID3D12Resource* motion{};
        ID3D12Resource* depth{};
        bool dropped{}; // a copy one round of slots old, never published
    };

    explicit GuideTransport(std::shared_ptr<VulkanApi> api) : vk_(std::move(api)) {}
    GuideTransport(const GuideTransport&) = delete;
    GuideTransport& operator=(const GuideTransport&) = delete;

    // Destroyed only after quiescent() held: callers keep a transport that is
    // not for the process instead (a feature release is rare).
    ~GuideTransport() {
        try {
            if (vk_) {
                for (Slot& slot : slots_) {
                    destroy(slot.motion);
                    destroy(slot.depth);
                    destroy_buffer(slot.buffer, slot.buffer_memory);
                }
                for (Shared& shared : retired_) destroy(shared);
                for (auto& [buffer, memory] : retired_buffers_) destroy_buffer(buffer, memory);
            }
            if (event_ != nullptr) CloseHandle(event_);
        } catch (...) {
        }
    }

    // No copy is held for publication, and every snapshot copy has run.
    [[nodiscard]] bool quiescent() noexcept {
        try {
            std::scoped_lock lock(mutex_);
            for (const Slot& slot : slots_) if (slot.pending) return false;
            if (!done_ || done_->GetCompletedValue() >= submitted_) return true;
            return event_ != nullptr && SUCCEEDED(done_->SetEventOnCompletion(submitted_, event_)) &&
                WaitForSingleObject(event_, 2000) == WAIT_OBJECT_0;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] HRESULT initialize(ID3D12Device* device, ID3D12CommandQueue* queue) noexcept {
        try {
            device_ = device;
            queue_ = queue;
            HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(done_.GetAddressOf()));
            for (Slot& slot : slots_) {
                if (SUCCEEDED(result)) result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(slot.allocator.GetAddressOf()));
                if (SUCCEEDED(result)) result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    slot.allocator.Get(), nullptr, IID_PPV_ARGS(slot.list.GetAddressOf()));
                if (SUCCEEDED(result)) result = slot.list->Close();
            }
            if (FAILED(result)) return result;
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return event_ != nullptr ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        } catch (...) {
            return E_FAIL;
        }
    }

    // In the game's command buffer, ahead of its DLSS: the copies. hold() then
    // keeps the publication until the next flush.
    [[nodiscard]] HRESULT stage(VkCommandBuffer buffer, const NVSDK_NGX_ImageViewInfo_VK& motion,
        const NVSDK_NGX_ImageViewInfo_VK* depth, Staged* staged) noexcept {
        try {
            *staged = {};
            std::scoped_lock lock(mutex_);
            Slot& slot = slots_[next_];
            if (slot.pending) {
                slot.pending = false;
                staged->dropped = true;
            }
            // The slot's last snapshot copy reads these textures on D3D12.
            if (slot.read_done != 0 && done_->GetCompletedValue() < slot.read_done) {
                const HRESULT result = done_->SetEventOnCompletion(slot.read_done, event_);
                if (FAILED(result)) return result;
                if (WaitForSingleObject(event_, 100) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            const DXGI_FORMAT motion_format = vector_dxgi_format(motion.Format);
            if (motion_format == DXGI_FORMAT_UNKNOWN || motion.Width == 0 || motion.Height == 0)
                return DXGI_ERROR_UNSUPPORTED;
            HRESULT result = prepare(slot.motion, motion_format, motion.Format, motion.Width, motion.Height);
            if (FAILED(result)) return result;
            DepthRoute route{};
            bool depth_ready = false;
            if (depth != nullptr) {
                route = depth_route(depth->Format);
                if (route.dxgi != DXGI_FORMAT_UNKNOWN && depth->Width != 0 && depth->Height != 0) {
                    depth_ready = SUCCEEDED(prepare(slot.depth, route.dxgi, route.shared, depth->Width, depth->Height)) &&
                        (!route.via_buffer || SUCCEEDED(prepare_buffer(slot,
                            static_cast<VkDeviceSize>(depth->Width) * depth->Height * route.texel_bytes)));
                }
            }
            record(buffer, motion, slot.motion, depth_ready ? depth : nullptr, slot.depth, route, slot.buffer);
            recording_ = true;
            staged->motion = slot.motion.d3d12.Get();
            staged->depth = depth_ready ? slot.depth.d3d12.Get() : nullptr;
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    void hold(const DlssMotionVectorPublication& publication) noexcept {
        std::scoped_lock lock(mutex_);
        if (!recording_) return;
        recording_ = false;
        Slot& slot = slots_[next_];
        slot.publication = publication;
        slot.order = ++order_;
        slot.pending = true;
        next_ = (next_ + 1U) % kSlots;
    }

    // Publishes every held copy, oldest first: `publish` records the guide
    // registry's snapshot copy into the slot's list, which then goes to the
    // queue the bridge has just made wait for the game's queue.
    template <typename Publish>
    void flush(Publish&& publish) noexcept {
        std::scoped_lock lock(mutex_);
        std::array<Slot*, kSlots> ready{};
        std::uint32_t count = 0;
        for (Slot& slot : slots_) if (slot.pending) ready[count++] = &slot;
        std::sort(ready.begin(), ready.begin() + count, [](const Slot* a, const Slot* b) { return a->order < b->order; });
        for (std::uint32_t i = 0; i < count; ++i) {
            Slot& slot = *ready[i];
            slot.pending = false;
            if (FAILED(slot.allocator->Reset()) || FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) continue;
            slot.publication.producer_command_list = slot.list.Get();
            publish(slot.publication);
            if (FAILED(slot.list->Close())) continue;
            ID3D12CommandList* lists[]{slot.list.Get()};
            queue_->ExecuteCommandLists(1, lists);
            if (SUCCEEDED(queue_->Signal(done_.Get(), ++submitted_))) slot.read_done = submitted_;
        }
    }

private:
    static constexpr std::uint32_t kSlots = 4;

    struct Shared {
        ComPtr<ID3D12Resource> d3d12;
        VkImage image{};
        VkDeviceMemory memory{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        std::uint32_t width{}, height{};
    };
    struct Slot {
        Shared motion;
        Shared depth;
        VkBuffer buffer{};
        VkDeviceMemory buffer_memory{};
        VkDeviceSize buffer_size{};
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t read_done{};
        std::uint64_t order{};
        bool pending{};
        DlssMotionVectorPublication publication{};
    };

    void destroy(Shared& shared) noexcept {
        const VkDevice device = vk_->binding.device;
        if (shared.image != VK_NULL_HANDLE) vk_->destroy_image(device, shared.image, nullptr);
        if (shared.memory != VK_NULL_HANDLE) vk_->free_memory(device, shared.memory, nullptr);
        shared = {};
    }

    void destroy_buffer(VkBuffer& buffer, VkDeviceMemory& memory) noexcept {
        const VkDevice device = vk_->binding.device;
        if (buffer != VK_NULL_HANDLE) vk_->destroy_buffer(device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE) vk_->free_memory(device, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    // A D3D12 shared texture, imported into the game's device.
    [[nodiscard]] HRESULT prepare(Shared& shared, DXGI_FORMAT format, VkFormat vk_format,
        std::uint32_t width, std::uint32_t height) noexcept {
        if (shared.d3d12 && shared.format == format && shared.width == width && shared.height == height) return S_OK;
        // The old one may still be read by a copy in flight: kept until the
        // transport goes (rare: only a resolution change).
        if (shared.d3d12) retired_.push_back(std::move(shared));
        shared = {};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(shared.d3d12.GetAddressOf()));
        if (FAILED(result)) return result;
        HANDLE handle = nullptr;
        result = device_->CreateSharedHandle(shared.d3d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(result)) return result;
        VkExternalMemoryImageCreateInfo external{};
        external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = vk_format;
        info.extent = {width, height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        const VkDevice device = vk_->binding.device;
        VkResult vk_result = vk_->create_image(device, &info, nullptr, &shared.image);
        if (vk_result != VK_SUCCESS) {
            CloseHandle(handle);
            return hresult_of(vk_result);
        }
        VkMemoryRequirements requirements{};
        vk_->image_requirements(device, shared.image, &requirements);
        const std::uint32_t type = vk_->device_memory_type(requirements.memoryTypeBits);
        if (type == UINT32_MAX) {
            CloseHandle(handle);
            return E_FAIL;
        }
        VkImportMemoryWin32HandleInfoKHR import{};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = handle;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.image = shared.image;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &dedicated;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        vk_result = vk_->allocate_memory(device, &allocate, nullptr, &shared.memory);
        // An NT handle stays the importer's to close.
        CloseHandle(handle);
        if (vk_result != VK_SUCCESS) return hresult_of(vk_result);
        vk_result = vk_->bind_image_memory(device, shared.image, shared.memory, 0);
        if (vk_result != VK_SUCCESS) return hresult_of(vk_result);
        shared.format = format;
        shared.width = width;
        shared.height = height;
        return S_OK;
    }

    [[nodiscard]] HRESULT prepare_buffer(Slot& slot, VkDeviceSize size) noexcept {
        if (slot.buffer != VK_NULL_HANDLE && slot.buffer_size >= size) return S_OK;
        // As for an image: an old buffer may still be read by a copy in flight.
        if (slot.buffer != VK_NULL_HANDLE) retired_buffers_.emplace_back(slot.buffer, slot.buffer_memory);
        slot.buffer = VK_NULL_HANDLE;
        slot.buffer_memory = VK_NULL_HANDLE;
        const VkDevice device = vk_->binding.device;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult vk_result = vk_->create_buffer(device, &info, nullptr, &slot.buffer);
        if (vk_result != VK_SUCCESS) return hresult_of(vk_result);
        VkMemoryRequirements requirements{};
        vk_->buffer_requirements(device, slot.buffer, &requirements);
        const std::uint32_t type = vk_->device_memory_type(requirements.memoryTypeBits);
        if (type == UINT32_MAX) return E_FAIL;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        vk_result = vk_->allocate_memory(device, &allocate, nullptr, &slot.buffer_memory);
        if (vk_result != VK_SUCCESS) return hresult_of(vk_result);
        vk_result = vk_->bind_buffer_memory(device, slot.buffer, slot.buffer_memory, 0);
        if (vk_result != VK_SUCCESS) return hresult_of(vk_result);
        slot.buffer_size = size;
        return S_OK;
    }

    static VkImageMemoryBarrier image_barrier(VkImage image, VkImageAspectFlags aspect, std::uint32_t mip,
        std::uint32_t layer, VkImageLayout from, VkImageLayout to, VkAccessFlags source, VkAccessFlags destination) {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, mip, 1, layer, 1};
        return barrier;
    }

    void record(VkCommandBuffer buffer, const NVSDK_NGX_ImageViewInfo_VK& motion, const Shared& motion_copy,
        const NVSDK_NGX_ImageViewInfo_VK* depth, const Shared& depth_copy, const DepthRoute& route,
        VkBuffer staging) noexcept {
        constexpr VkImageLayout kInput = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const std::uint32_t motion_mip = motion.SubresourceRange.baseMipLevel;
        const std::uint32_t motion_layer = motion.SubresourceRange.baseArrayLayer;
        const std::uint32_t depth_mip = depth ? depth->SubresourceRange.baseMipLevel : 0;
        const std::uint32_t depth_layer = depth ? depth->SubresourceRange.baseArrayLayer : 0;
        std::array<VkImageMemoryBarrier, 4> before{};
        std::uint32_t count = 0;
        before[count++] = image_barrier(motion.Image, VK_IMAGE_ASPECT_COLOR_BIT, motion_mip, motion_layer,
            kInput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        before[count++] = image_barrier(motion_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (depth != nullptr) {
            before[count++] = image_barrier(depth->Image, route.barrier_aspect, depth_mip, depth_layer,
                kInput, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            before[count++] = image_barrier(depth_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        vk_->barrier(buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr, 0, nullptr, count, before.data());
        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, motion_mip, motion_layer, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {motion.Width, motion.Height, 1};
        vk_->copy_image(buffer, motion.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            motion_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        if (depth != nullptr) {
            if (route.via_buffer) {
                VkBufferImageCopy out{};
                out.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, depth_mip, depth_layer, 1};
                out.imageExtent = {depth->Width, depth->Height, 1};
                vk_->copy_image_to_buffer(buffer, depth->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &out);
                VkBufferMemoryBarrier through{};
                through.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
                through.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                through.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                through.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                through.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                through.buffer = staging;
                through.size = VK_WHOLE_SIZE;
                vk_->barrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    0, nullptr, 1, &through, 0, nullptr);
                VkBufferImageCopy in{};
                in.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                in.imageExtent = {depth->Width, depth->Height, 1};
                vk_->copy_buffer_to_image(buffer, staging, depth_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
            } else {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, depth_mip, depth_layer, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.extent = {depth->Width, depth->Height, 1};
                vk_->copy_image(buffer, depth->Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    depth_copy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            }
        }
        std::array<VkImageMemoryBarrier, 4> after{};
        count = 0;
        after[count++] = image_barrier(motion.Image, VK_IMAGE_ASPECT_COLOR_BIT, motion_mip, motion_layer,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kInput, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
        after[count++] = image_barrier(motion_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        if (depth != nullptr) {
            after[count++] = image_barrier(depth->Image, route.barrier_aspect, depth_mip, depth_layer,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, kInput, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT);
            after[count++] = image_barrier(depth_copy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        }
        vk_->barrier(buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            0, nullptr, 0, nullptr, count, after.data());
    }

    std::shared_ptr<VulkanApi> vk_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> done_;
    std::array<Slot, kSlots> slots_{};
    std::vector<Shared> retired_;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> retired_buffers_;
    std::uint32_t next_{};
    std::uint64_t order_{};
    std::uint64_t submitted_{};
    bool recording_{};
    HANDLE event_{};
    std::mutex mutex_;
};

using Create = NVSDK_NGX_Result (*)(VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Create1 = NVSDK_NGX_Result (*)(VkDevice, VkCommandBuffer, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using Evaluate = NVSDK_NGX_Result (*)(VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
using Release = NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);

struct Feature {
    NVSDK_NGX_Feature type{};
    int flags{};
    bool flags_known{};
    std::uint32_t width{}, height{}, out_width{}, out_height{};
    std::shared_ptr<GuideTransport> transport;
    ComPtr<ID3D12Resource> output_placeholder;
    std::uint64_t publications{};
};

struct Module {
    SafetyHookInline create, create1, evaluate, evaluate_c, release;
    Create make{};
    Create1 make1{};
    Evaluate eval{}, eval_c{};
    Release retire{};
    HMODULE retained{};
    bool typed{}; // the module implements the export: its features are SR (0) or RR (1)
};

struct State {
    std::mutex mutex;
    std::array<Module, 3> modules{};
    std::unordered_map<const NVSDK_NGX_Handle*, Feature> features;
    std::vector<std::shared_ptr<GuideTransport>> retired;
    std::shared_ptr<VulkanApi> api;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    std::array<std::uint64_t, skip_count> skips{};
    std::uint64_t dropped{};
    std::atomic_bool configured{}, active{};
    // Last state recorded as vulkan_bridge 6: the layer configures every frame.
    std::atomic_int recorded{-1};
};
State& state() {
    static auto* value = new State; // hooks outlive static destruction
    return *value;
}

template <class T> T get(const NVSDK_NGX_Parameter* p, const char* name, T fallback = {}) {
    T value{};
    return p && NVSDK_NGX_SUCCEED(p->Get(name, &value)) ? value : fallback;
}

void record_skip(unsigned reason, std::uint64_t detail = 0) noexcept {
    try {
        auto& s = state();
        std::uint64_t count{};
        {
            std::scoped_lock lock(s.mutex);
            count = ++s.skips[reason];
        }
        if (count == 1)
            bridge_flight_logger().event(BridgeFlightOperation::vulkan_bridge, 8, reason, count, detail);
    } catch (...) {
    }
}

[[nodiscard]] const NVSDK_NGX_ImageViewInfo_VK* image_view(const NVSDK_NGX_Parameter* p, const char* name) noexcept {
    void* pointer = get<void*>(p, name);
    if (pointer == nullptr) return nullptr;
    const auto* resource = static_cast<const NVSDK_NGX_Resource_VK*>(pointer);
    if (resource->Type != NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW ||
        resource->Resource.ImageViewInfo.Image == VK_NULL_HANDLE) return nullptr;
    return &resource->Resource.ImageViewInfo;
}

[[nodiscard]] bool guide_feature(NVSDK_NGX_Feature type) noexcept {
    return type == NVSDK_NGX_Feature_SuperSampling || type == NVSDK_NGX_Feature_RayReconstruction;
}

Feature creation_feature(NVSDK_NGX_Feature type, NVSDK_NGX_Parameter* p) {
    Feature f{};
    f.type = type;
    f.flags_known = p && NVSDK_NGX_SUCCEED(p->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &f.flags));
    f.width = get<UINT>(p, NVSDK_NGX_Parameter_Width);
    f.height = get<UINT>(p, NVSDK_NGX_Parameter_Height);
    f.out_width = get<UINT>(p, NVSDK_NGX_Parameter_OutWidth);
    f.out_height = get<UINT>(p, NVSDK_NGX_Parameter_OutHeight);
    return f;
}

void register_feature(NVSDK_NGX_Feature type, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle* handle) {
    if (!handle || !guide_feature(type)) return;
    auto& s = state();
    std::scoped_lock lock(s.mutex);
    s.features[handle] = creation_feature(type, p);
}

// A texture of the output's size: the registry matches guides to the XR image
// by the output rectangle; the output itself is never read on this path.
ComPtr<ID3D12Resource> output_placeholder(ID3D12Device* device, std::uint32_t width, std::uint32_t height) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resource.GetAddressOf())))) return {};
    return resource;
}

void flush_transport(GuideTransport& transport) noexcept {
    transport.flush([](DlssMotionVectorPublication& publication) {
        try {
            const auto frame = publish_dlss_motion_vectors(publication);
            if (!frame) return;
            auto& s = state();
            bool first = false;
            {
                std::scoped_lock lock(s.mutex);
                for (auto& [handle, feature] : s.features) {
                    static_cast<void>(handle);
                    if (reinterpret_cast<std::uintptr_t>(handle) != publication.stream) continue;
                    first = feature.publications++ == 0;
                    break;
                }
            }
            if (first)
                bridge_flight_logger().event(BridgeFlightOperation::vulkan_bridge, 7,
                    (publication.stream & 0xFFFFFFFFULL) | (publication.depth != nullptr ? 1ULL << 32U : 0ULL),
                    (std::uint64_t(publication.motion_width) << 32U) | publication.motion_height,
                    (std::uint64_t(publication.output_width) << 32U) | publication.output_height);
        } catch (...) {
        }
    });
}

void capture(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p,
    std::size_t slot) noexcept {
    try {
        auto& s = state();
        if (!s.active.load(std::memory_order_acquire) || buffer == VK_NULL_HANDLE || !handle || !p) return;
        std::shared_ptr<VulkanApi> api;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        Feature feature{};
        {
            std::scoped_lock lock(s.mutex);
            auto found = s.features.find(handle);
            // A feature created before the hooks: only a module implementing
            // the export itself says its type.
            if (found == s.features.end() && slot < 2 && s.modules[slot].typed) {
                auto recovered = creation_feature(
                    slot == 0 ? NVSDK_NGX_Feature_SuperSampling : NVSDK_NGX_Feature_RayReconstruction,
                    const_cast<NVSDK_NGX_Parameter*>(p));
                if (recovered.flags_known) found = s.features.emplace(handle, std::move(recovered)).first;
            }
            if (found == s.features.end()) {
                api.reset();
            } else {
                feature = found->second;
                api = s.api;
                device = s.device;
                queue = s.queue;
            }
        }
        if (!api) {
            record_skip(unknown_feature);
            return;
        }
        if (!device || !queue) return;
        const auto* output = image_view(p, NVSDK_NGX_Parameter_Output);
        const auto* motion = image_view(p, NVSDK_NGX_Parameter_MotionVectors);
        const auto* depth = image_view(p, NVSDK_NGX_Parameter_Depth);
        if (output == nullptr || motion == nullptr) {
            record_skip(missing_resources, (output ? 1U : 0U) | (motion ? 2U : 0U));
            return;
        }
        // The command buffer has to be the session device's: the loader keeps
        // a device's dispatch table at the start of all its objects.
        if (*reinterpret_cast<void* const*>(buffer) != *reinterpret_cast<void* const*>(api->binding.device)) {
            record_skip(foreign_command_buffer);
            return;
        }
        const std::uint32_t out_width = feature.out_width ? feature.out_width : output->Width;
        const std::uint32_t out_height = feature.out_height ? feature.out_height : output->Height;
        const auto output_x = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X);
        const auto output_y = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);
        std::shared_ptr<GuideTransport> transport;
        ComPtr<ID3D12Resource> placeholder;
        {
            std::scoped_lock lock(s.mutex);
            auto found = s.features.find(handle);
            if (found == s.features.end()) return;
            Feature& stored = found->second;
            if (!stored.transport) {
                auto created = std::make_shared<GuideTransport>(api);
                if (SUCCEEDED(created->initialize(device.Get(), queue.Get()))) stored.transport = std::move(created);
            }
            const std::uint64_t needed_width = std::uint64_t(output_x) + out_width;
            const std::uint64_t needed_height = std::uint64_t(output_y) + out_height;
            if (stored.transport && needed_width <= 16384 && needed_height <= 16384 &&
                (!stored.output_placeholder || stored.output_placeholder->GetDesc().Width < needed_width ||
                    stored.output_placeholder->GetDesc().Height < needed_height))
                stored.output_placeholder = output_placeholder(device.Get(),
                    static_cast<std::uint32_t>(needed_width), static_cast<std::uint32_t>(needed_height));
            transport = stored.transport;
            placeholder = stored.output_placeholder;
        }
        if (!transport) {
            record_skip(transport_failed);
            return;
        }
        if (!placeholder) {
            record_skip(placeholder_failed);
            return;
        }
        GuideTransport::Staged staged{};
        const HRESULT staged_result = transport->stage(buffer, *motion, depth, &staged);
        if (staged.dropped) {
            std::uint64_t dropped{};
            {
                std::scoped_lock lock(s.mutex);
                dropped = ++s.dropped;
            }
            if (dropped % 100 == 1)
                bridge_flight_logger().event(BridgeFlightOperation::vulkan_bridge, 9, dropped, 0, 0);
        }
        if (FAILED(staged_result)) {
            record_skip(stage_failed, static_cast<std::uint32_t>(staged_result));
            return;
        }
        // The same fields the D3D11 ingress publishes, from the same NGX keys.
        DlssMotionVectorPublication g{};
        g.stream = reinterpret_cast<std::uintptr_t>(handle);
        g.producer_queue = queue.Get();
        g.verified_producer_device = device.Get();
        g.output = placeholder.Get();
        g.motion_vectors = staged.motion;
        g.depth = staged.depth;
        g.output_width = out_width;
        g.output_height = out_height;
        // Rectangles stay inside the copies, which have the inputs' own extent.
        const auto available = [](std::uint32_t extent, std::uint32_t base) {
            return extent - std::min(base, extent);
        };
        const bool low = (feature.flags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
        const auto render_width = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,
            feature.width ? feature.width : motion->Width);
        const auto render_height = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,
            feature.height ? feature.height : motion->Height);
        g.output_x = output_x;
        g.output_y = output_y;
        g.motion_x = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X);
        g.motion_y = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
        g.motion_width = std::min<std::uint32_t>(low ? render_width : out_width, available(motion->Width, g.motion_x));
        g.motion_height = std::min<std::uint32_t>(low ? render_height : out_height, available(motion->Height, g.motion_y));
        if (depth) {
            g.depth_x = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X);
            g.depth_y = get<UINT>(p, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
            g.depth_width = std::min<std::uint32_t>(render_width, available(depth->Width, g.depth_x));
            g.depth_height = std::min<std::uint32_t>(render_height, available(depth->Height, g.depth_y));
        }
        g.scale_x = get<float>(p, NVSDK_NGX_Parameter_MV_Scale_X, 1);
        g.scale_y = get<float>(p, NVSDK_NGX_Parameter_MV_Scale_Y, 1);
        g.jitter_x = get<float>(p, NVSDK_NGX_Parameter_Jitter_Offset_X);
        g.jitter_y = get<float>(p, NVSDK_NGX_Parameter_Jitter_Offset_Y);
        g.reset = get<int>(p, NVSDK_NGX_Parameter_Reset) != 0;
        g.jittered = (feature.flags & NVSDK_NGX_DLSS_Feature_Flags_MVJittered) != 0;
        g.depth_inverted = (feature.flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
        g.frame_time_delta_ms = get<float>(p, NVSDK_NGX_Parameter_FrameTimeDeltaInMsec, 16.6667F);
        if (!(g.frame_time_delta_ms > 0.0F) || g.frame_time_delta_ms > 1000.0F) g.frame_time_delta_ms = 16.6667F;
        g.resource_state = g.depth_resource_state = g.output_resource_state = D3D12_RESOURCE_STATE_COMMON;
        static_cast<void>(native_ingress::apply_camera(g, nullptr));
        transport->hold(g);
    } catch (...) {
    }
}

thread_local unsigned evaluation_depth = 0;
thread_local bool evaluation_captured = false;

struct EvaluationScope {
    EvaluationScope() noexcept {
        if (evaluation_depth++ == 0) evaluation_captured = false;
    }
    ~EvaluationScope() { --evaluation_depth; }
};

// The first hooked module in a nested chain that knows the feature copies.
void capture_once(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle, const NVSDK_NGX_Parameter* p,
    std::size_t slot) noexcept {
    if (evaluation_captured || !state().active.load(std::memory_order_acquire)) return;
    bool known{};
    {
        auto& s = state();
        std::scoped_lock lock(s.mutex);
        known = s.features.contains(handle) || (slot < 2 && s.modules[slot].typed);
    }
    if (!known) return;
    evaluation_captured = true;
    capture(buffer, handle, p, slot);
}

template <std::size_t Slot>
NVSDK_NGX_Result create(VkCommandBuffer buffer, NVSDK_NGX_Feature type, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    const auto result = state().modules[Slot].make(buffer, type, p, out);
    if (NVSDK_NGX_SUCCEED(result) && out) register_feature(type, p, *out);
    return result;
}

template <std::size_t Slot>
NVSDK_NGX_Result create1(VkDevice device, VkCommandBuffer buffer, NVSDK_NGX_Feature type,
    NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** out) {
    const auto result = state().modules[Slot].make1(device, buffer, type, p, out);
    if (NVSDK_NGX_SUCCEED(result) && out) register_feature(type, p, *out);
    return result;
}

template <std::size_t Slot>
NVSDK_NGX_Result evaluate(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p, void* callback) {
    EvaluationScope scope;
    capture_once(buffer, handle, p, Slot);
    return state().modules[Slot].eval(buffer, handle, p, callback);
}

template <std::size_t Slot>
NVSDK_NGX_Result evaluate_c(VkCommandBuffer buffer, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* p, void* callback) {
    EvaluationScope scope;
    capture_once(buffer, handle, p, Slot);
    return state().modules[Slot].eval_c(buffer, handle, p, callback);
}

template <std::size_t Slot>
NVSDK_NGX_Result release(NVSDK_NGX_Handle* handle) {
    const auto stream = reinterpret_cast<std::uintptr_t>(handle);
    const auto result = state().modules[Slot].retire(handle);
    if (NVSDK_NGX_SUCCEED(result)) {
        auto& s = state();
        std::shared_ptr<GuideTransport> transport;
        bool known = false;
        {
            std::scoped_lock lock(s.mutex);
            if (const auto found = s.features.find(handle); found != s.features.end()) {
                known = true;
                transport = std::move(found->second.transport);
                s.features.erase(found);
            }
        }
        if (known) retire_dlss_motion_vector_stream(stream);
        // Destroyed here only if every copy is published and read; otherwise
        // a game command buffer or a snapshot copy may still use its objects.
        if (transport && !transport->quiescent()) {
            std::scoped_lock lock(s.mutex);
            s.retired.push_back(std::move(transport));
        }
    }
    return result;
}

template <std::size_t Slot>
void install(HMODULE module) noexcept {
    auto& m = state().modules[Slot];
    if (module == nullptr || m.retained) return;
    try {
        const auto c = GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature");
        const auto c1 = GetProcAddress(module, "NVSDK_NGX_VULKAN_CreateFeature1");
        const auto e = GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature");
        const auto ec = GetProcAddress(module, "NVSDK_NGX_VULKAN_EvaluateFeature_C");
        const auto r = GetProcAddress(module, "NVSDK_NGX_VULKAN_ReleaseFeature");
        if (!c || !e || !r) return;
        HMODULE implementation{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(e), &implementation);
        const bool separate_c = ec && ec != e;
        m.create = safetyhook::create_inline(c, reinterpret_cast<void*>(&create<Slot>), SafetyHookInline::StartDisabled);
        if (c1) m.create1 = safetyhook::create_inline(c1, reinterpret_cast<void*>(&create1<Slot>), SafetyHookInline::StartDisabled);
        m.evaluate = safetyhook::create_inline(e, reinterpret_cast<void*>(&evaluate<Slot>), SafetyHookInline::StartDisabled);
        if (separate_c)
            m.evaluate_c = safetyhook::create_inline(ec, reinterpret_cast<void*>(&evaluate_c<Slot>), SafetyHookInline::StartDisabled);
        m.release = safetyhook::create_inline(r, reinterpret_cast<void*>(&release<Slot>), SafetyHookInline::StartDisabled);
        if (!m.create || !m.evaluate || !m.release || (c1 && !m.create1) || (separate_c && !m.evaluate_c)) {
            m.create = {};
            m.create1 = {};
            m.evaluate = {};
            m.evaluate_c = {};
            m.release = {};
            return;
        }
        m.make = m.create.original<Create>();
        m.make1 = c1 ? m.create1.original<Create1>() : nullptr;
        m.eval = m.evaluate.original<Evaluate>();
        m.eval_c = separate_c ? m.evaluate_c.original<Evaluate>() : nullptr;
        m.retire = m.release.original<Release>();
        m.typed = Slot < 2 && implementation == module;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(e), &m.retained);
        static_cast<void>(m.create.enable());
        if (c1) static_cast<void>(m.create1.enable());
        static_cast<void>(m.evaluate.enable());
        if (separate_c) static_cast<void>(m.evaluate_c.enable());
        static_cast<void>(m.release.enable());
    } catch (...) {
    }
}

std::uint64_t installed_modules() noexcept {
    std::uint64_t count = 0;
    for (const auto& m : state().modules) count += m.retained ? 1 : 0;
    return count;
}
} // namespace

void configure_guide_capture(const VulkanSessionBinding& binding, ID3D12Device* device,
    ID3D12CommandQueue* queue, bool enabled) noexcept {
    try {
        // Capture stays disabled until the following submission/lifetime safety prerequisite.
        enabled=false;
        // REAL VR's Vulkan titles keep the native ingress; it hooks these exports.
        if (luke_vulkan_process()) return;
        auto& s = state();
        if (!enabled || !device || !queue || binding.device == VK_NULL_HANDLE) {
            s.active.store(false, std::memory_order_release);
            if (s.recorded.exchange(0) != 0)
                bridge_flight_logger().event(BridgeFlightOperation::vulkan_bridge, 6, 0, installed_modules(), 0);
            return;
        }
        std::shared_ptr<VulkanApi> api;
        {
            std::scoped_lock lock(s.mutex);
            if (s.api && s.api->binding.device == binding.device) api = s.api;
        }
        if (!api) {
            auto created = std::make_shared<VulkanApi>();
            if (!created->load(binding)) {
                record_skip(entry_points);
                return;
            }
            api = std::move(created);
        }
        {
            std::scoped_lock lock(s.mutex);
            s.api = api;
            s.device = device;
            s.queue = queue;
        }
        // The snapshot copies are this queue's submissions: their receipts
        // retire the registry's slots as for the D3D11 ingress.
        native_ingress::configure_queue(queue);
        configure_dlss_motion_vector_tracking(true);
        s.configured.store(true, std::memory_order_release);
        poll_guide_capture();
        s.active.store(true, std::memory_order_release);
        if (s.recorded.exchange(1) != 1)
            bridge_flight_logger().event(BridgeFlightOperation::vulkan_bridge, 6, 1, installed_modules(), 0);
    } catch (...) {
    }
}

void stop_guide_capture(ID3D12CommandQueue* queue) noexcept {
    try {
        auto& s = state();
        std::scoped_lock lock(s.mutex);
        if (s.queue.Get() == queue) {
            s.active.store(false, std::memory_order_release);
            s.recorded.store(-1);
        }
    } catch (...) {
    }
}

void flush_guide_capture() noexcept {
    try {
        auto& s = state();
        if (!s.active.load(std::memory_order_acquire)) return;
        std::vector<std::shared_ptr<GuideTransport>> transports;
        {
            std::scoped_lock lock(s.mutex);
            for (const auto& [handle, feature] : s.features) {
                static_cast<void>(handle);
                if (feature.transport) transports.push_back(feature.transport);
            }
        }
        for (const auto& transport : transports) flush_transport(*transport);
    } catch (...) {
    }
}

void poll_guide_capture() noexcept {
    try {
        // Only once a bridged Vulkan session asked for it; the hooks then stay.
        if (!state().configured.load(std::memory_order_acquire) || luke_vulkan_process()) return;
        static std::mutex installing;
        std::scoped_lock lock(installing);
        install<0>(GetModuleHandleW(L"nvngx_dlss.dll"));
        install<1>(GetModuleHandleW(L"nvngx_dlssd.dll"));
        install<2>(GetModuleHandleW(L"_nvngx.dll"));
    } catch (...) {
    }
}
} // namespace xrfg::native_vulkan
