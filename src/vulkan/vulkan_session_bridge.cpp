#include "xrfg/vulkan_session_bridge.hpp"

#include "xrfg/bridge_flight_logger.hpp"

#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace xrfg {
namespace {

using Microsoft::WRL::ComPtr;

// How many command buffers publish and capture may have in flight before a
// call fails open. Each submission signals the shared fence, so a buffer is
// free again once the fence has passed the value it signalled; a frame's
// capture and publish take two, and the depth of the pipeline is a pair.
constexpr std::size_t kCommandBufferLimit = 16;

struct UniqueHandle {
    HANDLE value{};

    ~UniqueHandle() {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }

    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    // The Vulkan import of a Windows handle takes the handle over, so it
    // must not be closed here afterwards.
    void release() noexcept { value = nullptr; }
};

// A VkResult as an HRESULT the flight log can carry beside the stage: the
// negative Vulkan code in the low half, under an ITF facility so it cannot be
// mistaken for a Win32 or D3D code.
[[nodiscard]] HRESULT hresult_from_vulkan(VkResult result) noexcept {
    if (result >= VK_SUCCESS) {
        return S_OK;
    }
    return MAKE_HRESULT(
        1, FACILITY_ITF,
        static_cast<std::uint16_t>(-static_cast<std::int32_t>(result)));
}

[[nodiscard]] bool same_luid(const LUID& left, const LUID& right) noexcept {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
}

// The entry points the interop uses, resolved through the application's
// loader. Instance-level ones come from vkGetInstanceProcAddr, device-level
// ones from vkGetDeviceProcAddr so they dispatch straight into the driver
// for the application's device.
struct VulkanFunctions {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr{};
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2{};
    PFN_vkGetPhysicalDeviceMemoryProperties get_physical_device_memory_properties{};
    PFN_vkGetDeviceQueue get_device_queue{};
    PFN_vkCreateImage create_image{};
    PFN_vkDestroyImage destroy_image{};
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements{};
    PFN_vkAllocateMemory allocate_memory{};
    PFN_vkFreeMemory free_memory{};
    PFN_vkBindImageMemory bind_image_memory{};
    PFN_vkCreateSemaphore create_semaphore{};
    PFN_vkDestroySemaphore destroy_semaphore{};
    PFN_vkImportSemaphoreWin32HandleKHR import_semaphore_win32_handle{};
    PFN_vkCreateCommandPool create_command_pool{};
    PFN_vkDestroyCommandPool destroy_command_pool{};
    PFN_vkAllocateCommandBuffers allocate_command_buffers{};
    PFN_vkResetCommandBuffer reset_command_buffer{};
    PFN_vkBeginCommandBuffer begin_command_buffer{};
    PFN_vkEndCommandBuffer end_command_buffer{};
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier{};
    PFN_vkCmdCopyImage cmd_copy_image{};
    PFN_vkQueueSubmit queue_submit{};
    PFN_vkCreateFence create_fence{};
    PFN_vkDestroyFence destroy_fence{};
    PFN_vkWaitForFences wait_for_fences{};
    // Diagnostic sampling only; see Impl::sample.
    PFN_vkCreateBuffer create_buffer{};
    PFN_vkDestroyBuffer destroy_buffer{};
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements{};
    PFN_vkBindBufferMemory bind_buffer_memory{};
    PFN_vkMapMemory map_memory{};
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer{};

    [[nodiscard]] bool load_loader() noexcept {
        HMODULE module = GetModuleHandleW(L"vulkan-1.dll");
        if (module == nullptr) {
            module = LoadLibraryW(L"vulkan-1.dll");
        }
        if (module == nullptr) {
            return false;
        }
        get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            GetProcAddress(module, "vkGetInstanceProcAddr"));
        return get_instance_proc_addr != nullptr;
    }

    [[nodiscard]] bool load_instance(VkInstance instance) noexcept {
        if (get_instance_proc_addr == nullptr || instance == VK_NULL_HANDLE) {
            return false;
        }
        get_device_proc_addr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            get_instance_proc_addr(instance, "vkGetDeviceProcAddr"));
        get_physical_device_properties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                get_instance_proc_addr(instance, "vkGetPhysicalDeviceProperties2"));
        if (get_physical_device_properties2 == nullptr) {
            // Vulkan 1.0 instances reach it through the KHR extension the
            // runtime asks the application to enable.
            get_physical_device_properties2 =
                reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                    get_instance_proc_addr(
                        instance, "vkGetPhysicalDeviceProperties2KHR"));
        }
        get_physical_device_memory_properties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
                get_instance_proc_addr(
                    instance, "vkGetPhysicalDeviceMemoryProperties"));
        return get_device_proc_addr != nullptr &&
            get_physical_device_properties2 != nullptr &&
            get_physical_device_memory_properties != nullptr;
    }

    [[nodiscard]] bool load_device(VkDevice device) noexcept {
        if (get_device_proc_addr == nullptr || device == VK_NULL_HANDLE) {
            return false;
        }
        const auto load = [&](auto& target, const char* name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(
                get_device_proc_addr(device, name));
            return target != nullptr;
        };
        return load(get_device_queue, "vkGetDeviceQueue") &&
            load(create_image, "vkCreateImage") &&
            load(destroy_image, "vkDestroyImage") &&
            load(get_image_memory_requirements, "vkGetImageMemoryRequirements") &&
            load(allocate_memory, "vkAllocateMemory") &&
            load(free_memory, "vkFreeMemory") &&
            load(bind_image_memory, "vkBindImageMemory") &&
            load(create_semaphore, "vkCreateSemaphore") &&
            load(destroy_semaphore, "vkDestroySemaphore") &&
            load(import_semaphore_win32_handle, "vkImportSemaphoreWin32HandleKHR") &&
            load(create_command_pool, "vkCreateCommandPool") &&
            load(destroy_command_pool, "vkDestroyCommandPool") &&
            load(allocate_command_buffers, "vkAllocateCommandBuffers") &&
            load(reset_command_buffer, "vkResetCommandBuffer") &&
            load(begin_command_buffer, "vkBeginCommandBuffer") &&
            load(end_command_buffer, "vkEndCommandBuffer") &&
            load(cmd_pipeline_barrier, "vkCmdPipelineBarrier") &&
            load(cmd_copy_image, "vkCmdCopyImage") &&
            load(queue_submit, "vkQueueSubmit") &&
            load(create_fence, "vkCreateFence") &&
            load(destroy_fence, "vkDestroyFence") &&
            load(wait_for_fences, "vkWaitForFences") &&
            load(create_buffer, "vkCreateBuffer") &&
            load(destroy_buffer, "vkDestroyBuffer") &&
            load(get_buffer_memory_requirements, "vkGetBufferMemoryRequirements") &&
            load(bind_buffer_memory, "vkBindBufferMemory") &&
            load(map_memory, "vkMapMemory") &&
            load(cmd_copy_image_to_buffer, "vkCmdCopyImageToBuffer");
    }
};

[[nodiscard]] HRESULT vulkan_adapter_luid(
    const VulkanFunctions& vk,
    VkPhysicalDevice physical_device,
    LUID* luid) noexcept {
    if (luid == nullptr || physical_device == VK_NULL_HANDLE ||
        vk.get_physical_device_properties2 == nullptr) {
        return E_POINTER;
    }
    VkPhysicalDeviceIDProperties id_properties{};
    id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &id_properties;
    vk.get_physical_device_properties2(physical_device, &properties);
    if (!id_properties.deviceLUIDValid) {
        return E_FAIL;
    }
    static_assert(sizeof(LUID) == VK_LUID_SIZE);
    std::memcpy(luid, id_properties.deviceLUID, sizeof(LUID));
    return S_OK;
}

[[nodiscard]] HRESULT adapter_for_luid(
    const LUID& luid,
    IDXGIAdapter1** adapter) noexcept {
    if (adapter == nullptr) {
        return E_POINTER;
    }
    *adapter = nullptr;
    ComPtr<IDXGIFactory4> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IDXGIAdapter> found;
    result = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(found.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    ComPtr<IDXGIAdapter1> adapter1;
    result = found.As(&adapter1);
    if (SUCCEEDED(result)) {
        DXGI_ADAPTER_DESC1 description{};
        result = adapter1->GetDesc1(&description);
        if (SUCCEEDED(result) && !same_luid(description.AdapterLuid, luid)) {
            result = DXGI_ERROR_NOT_FOUND;
        }
    }
    if (SUCCEEDED(result)) {
        *adapter = adapter1.Detach();
    }
    return result;
}

[[nodiscard]] std::optional<std::uint32_t> device_local_memory_type(
    const VkPhysicalDeviceMemoryProperties& properties,
    std::uint32_t type_bits) noexcept {
    std::optional<std::uint32_t> any;
    for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((type_bits & (1U << index)) == 0) {
            continue;
        }
        if ((properties.memoryTypes[index].propertyFlags &
             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
            return index;
        }
        if (!any) {
            any = index;
        }
    }
    return any;
}

// Whole-image barriers: a runtime may hand out private images with fewer
// mips than the create info asked for, and the copies only ever touch mip
// zero, so the range is whatever the image has.
[[nodiscard]] VkImageMemoryBarrier image_barrier(
    VkImage image,
    VkImageLayout old_layout,
    VkImageLayout new_layout,
    VkAccessFlags source_access,
    VkAccessFlags destination_access) noexcept {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    return barrier;
}

}  // namespace

DXGI_FORMAT dxgi_format_for_vulkan(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_UNORM;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return DXGI_FORMAT_R11G11B10_FLOAT;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT dxgi_depth_format_for_vulkan(VkFormat format) noexcept {
    switch (format) {
        case VK_FORMAT_D32_SFLOAT:
            return DXGI_FORMAT_D32_FLOAT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case VK_FORMAT_D16_UNORM:
            return DXGI_FORMAT_D16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

VkFormat vulkan_format_for_dxgi(DXGI_FORMAT format) noexcept {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return VK_FORMAT_R16G16B16A16_UNORM;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case DXGI_FORMAT_D32_FLOAT:
            return VK_FORMAT_D32_SFLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            return VK_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_D16_UNORM:
            return VK_FORMAT_D16_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

HRESULT create_d3d12_device_for_vulkan(
    const VulkanSessionBinding& binding,
    ID3D12Device** d3d12_device,
    ID3D12CommandQueue** d3d12_queue) noexcept {
    if (d3d12_device != nullptr) {
        *d3d12_device = nullptr;
    }
    if (d3d12_queue != nullptr) {
        *d3d12_queue = nullptr;
    }
    if (d3d12_device == nullptr || d3d12_queue == nullptr ||
        binding.instance == VK_NULL_HANDLE ||
        binding.physical_device == VK_NULL_HANDLE) {
        return E_POINTER;
    }
    try {
        VulkanFunctions vk;
        if (!vk.load_loader() || !vk.load_instance(binding.instance)) {
            return E_FAIL;
        }
        LUID luid{};
        HRESULT result = vulkan_adapter_luid(vk, binding.physical_device, &luid);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<IDXGIAdapter1> adapter;
        result = adapter_for_luid(luid, adapter.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        ComPtr<ID3D12Device> device;
        result = D3D12CreateDevice(
            adapter.Get(),
            D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(device.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        // As for a D3D11 session: every piece of synthesis runs here while the
        // game submits its next frame, and the pair behind it has a slot to
        // meet where the game's frame can absorb a delay.
        queue_description.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        ComPtr<ID3D12CommandQueue> queue;
        result = device->CreateCommandQueue(
            &queue_description,
            IID_PPV_ARGS(queue.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        *d3d12_device = device.Detach();
        *d3d12_queue = queue.Detach();
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

// ---------------------------------------------------------------------------
// The Vulkan bridge.

bool vulkan_queue_layer_loaded() noexcept {
    return GetModuleHandleW(L"OFXR_vulkan_queue_layer.dll") != nullptr;
}

bool vulkan_queue_serialized(const VulkanSessionBinding& binding) noexcept {
    if (binding.device == VK_NULL_HANDLE) return false;
    const HMODULE module = GetModuleHandleW(L"OFXR_vulkan_queue_layer.dll");
    if (!module) return false;
    using Query = std::uint32_t(VKAPI_PTR*)(VkDevice);
    const auto query = reinterpret_cast<Query>(GetProcAddress(module, "OFXR_vulkanQueueDeviceSerialized"));
    return query && query(binding.device) != 0;
}

struct VulkanBridgeSwapchain::Impl {
    static constexpr std::size_t kCopyListCount = 4;
    struct SharedImage {
        ComPtr<ID3D12Resource> d3d12;
        VkImage image{};
        VkDeviceMemory memory{};
    };
    struct CommandBuffer {
        VkCommandBuffer buffer{};
        std::uint64_t signal_value{};
    };
    struct CopyList {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        std::uint64_t fence_value{};
    };

    VulkanFunctions vk;
    VulkanSessionBinding binding;
    VkQueue queue{};
    ComPtr<ID3D12Device> d3d12_device;
    ComPtr<ID3D12CommandQueue> d3d12_queue;
    ComPtr<ID3D12Fence> d3d12_fence;
    VkSemaphore semaphore{};
    VkCommandPool command_pool{};
    HANDLE fence_event{};
    VulkanBridgePath path{VulkanBridgePath::direct};
    DXGI_FORMAT shared_format{DXGI_FORMAT_UNKNOWN};
    VulkanBridgeSwapchainDescription description;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::vector<ComPtr<ID3D12Resource>> runtime_images;
    std::vector<SharedImage> shared;
    std::vector<ID3D12Resource*> shared_views;
    // What the application is handed: shared[i].image on the direct path,
    // own images (with own_memories) on depth_private.
    std::vector<VkImage> application_images;
    std::vector<VkDeviceMemory> own_memories;
    std::vector<CommandBuffer> command_buffers;
    std::array<CopyList, kCopyListCount> copy_lists;
    std::size_t next_copy_list{};
    std::vector<std::uint64_t> last_read;
    // Synchronized-calls mode (no queue layer): queue work only at acquire
    // and release; the initial layout waits for the first acquire.
    bool calls_only{};
    bool initial_layout_pending{};
    std::uint64_t next_value{1};
    std::uint64_t last_signalled{};
    bool enabled{};

    ~Impl() {
        if (binding.device != VK_NULL_HANDLE) {
            for (SharedImage& image : shared) {
                if (image.image != VK_NULL_HANDLE && vk.destroy_image) {
                    vk.destroy_image(binding.device, image.image, nullptr);
                }
                if (image.memory != VK_NULL_HANDLE && vk.free_memory) {
                    vk.free_memory(binding.device, image.memory, nullptr);
                }
            }
            if (path == VulkanBridgePath::depth_private) {
                for (VkImage image : application_images) {
                    if (image != VK_NULL_HANDLE && vk.destroy_image) {
                        vk.destroy_image(binding.device, image, nullptr);
                    }
                }
            }
            for (VkDeviceMemory memory : own_memories) {
                if (memory != VK_NULL_HANDLE && vk.free_memory) {
                    vk.free_memory(binding.device, memory, nullptr);
                }
            }
            if (command_pool != VK_NULL_HANDLE && vk.destroy_command_pool) {
                vk.destroy_command_pool(binding.device, command_pool, nullptr);
            }
            if (semaphore != VK_NULL_HANDLE && vk.destroy_semaphore) {
                vk.destroy_semaphore(binding.device, semaphore, nullptr);
            }
        }
        if (fence_event != nullptr) {
            CloseHandle(fence_event);
        }
    }

    [[nodiscard]] std::uint64_t allocate() noexcept {
        if (next_value == std::numeric_limits<std::uint64_t>::max()) {
            enabled = false;
        }
        return next_value++;
    }

    [[nodiscard]] VkImageAspectFlags aspect() const noexcept {
        if (!description.depth_stencil) {
            return VK_IMAGE_ASPECT_COLOR_BIT;
        }
        switch (description.requested_format) {
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D16_UNORM_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
            default:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
        }
    }

    // A shared texture in the runtime image's shape, imported into the
    // application's device as the image it will render into.
    [[nodiscard]] HRESULT create_shared(
        const D3D12_RESOURCE_DESC& runtime_description,
        SharedImage* output,
        std::uint32_t* failure_stage) noexcept {
        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC resource_description = runtime_description;
        resource_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_description.SampleDesc.Count = 1;
        resource_description.SampleDesc.Quality = 0;
        // Simultaneous access keeps the texture uncompressed, which is what
        // lets both APIs read it as plain memory.
        resource_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
            D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        if (description.unordered_access) {
            resource_description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        }
        HRESULT result = d3d12_device->CreateCommittedResource(
            &heap_properties, D3D12_HEAP_FLAG_SHARED, &resource_description,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(output->d3d12.GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 10;
            return result;
        }
        UniqueHandle handle;
        result = d3d12_device->CreateSharedHandle(
            output->d3d12.Get(), nullptr, GENERIC_ALL, nullptr, &handle.value);
        if (FAILED(result)) {
            *failure_stage = 11;
            return result;
        }
        VkExternalMemoryImageCreateInfo external_info{};
        external_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        external_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.pNext = &external_info;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = description.requested_format;
        image_info.extent.width = static_cast<std::uint32_t>(resource_description.Width);
        image_info.extent.height = resource_description.Height;
        image_info.extent.depth = 1;
        image_info.mipLevels = resource_description.MipLevels;
        image_info.arrayLayers = resource_description.DepthOrArraySize;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (description.unordered_access) {
            image_info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
        }
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult vk_result = vk.create_image(
            binding.device, &image_info, nullptr, &output->image);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 12;
            return hresult_from_vulkan(vk_result);
        }
        VkMemoryRequirements requirements{};
        vk.get_image_memory_requirements(binding.device, output->image, &requirements);
        const auto memory_type = device_local_memory_type(
            memory_properties, requirements.memoryTypeBits);
        if (!memory_type) {
            *failure_stage = 13;
            return E_FAIL;
        }
        VkImportMemoryWin32HandleInfoKHR import_info{};
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import_info.handle = handle.value;
        VkMemoryDedicatedAllocateInfo dedicated_info{};
        dedicated_info.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated_info.pNext = &import_info;
        dedicated_info.image = output->image;
        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.pNext = &dedicated_info;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = *memory_type;
        vk_result = vk.allocate_memory(
            binding.device, &allocate_info, nullptr, &output->memory);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 14;
            return hresult_from_vulkan(vk_result);
        }
        handle.release();
        vk_result = vk.bind_image_memory(binding.device, output->image, output->memory, 0);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 15;
            return hresult_from_vulkan(vk_result);
        }
        return S_OK;
    }

    // Depth the runtime never sees: an image of the layer's on the
    // application's device, in the format the application asked for.
    [[nodiscard]] HRESULT create_own_depth(
        VkImage* image, VkDeviceMemory* memory, std::uint32_t* failure_stage) noexcept {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = description.requested_format;
        image_info.extent.width = description.width;
        image_info.extent.height = description.height;
        image_info.extent.depth = 1;
        image_info.mipLevels = description.mip_levels;
        image_info.arrayLayers = description.array_size;
        image_info.samples = static_cast<VkSampleCountFlagBits>(
            description.sample_count == 0 ? 1 : description.sample_count);
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult vk_result = vk.create_image(binding.device, &image_info, nullptr, image);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 16;
            return hresult_from_vulkan(vk_result);
        }
        VkMemoryRequirements requirements{};
        vk.get_image_memory_requirements(binding.device, *image, &requirements);
        const auto memory_type = device_local_memory_type(
            memory_properties, requirements.memoryTypeBits);
        if (!memory_type) {
            *failure_stage = 17;
            return E_FAIL;
        }
        VkMemoryAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate_info.allocationSize = requirements.size;
        allocate_info.memoryTypeIndex = *memory_type;
        vk_result = vk.allocate_memory(binding.device, &allocate_info, nullptr, memory);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 18;
            return hresult_from_vulkan(vk_result);
        }
        vk_result = vk.bind_image_memory(binding.device, *image, *memory, 0);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 19;
            return hresult_from_vulkan(vk_result);
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT acquire_command_buffer(VkCommandBuffer* output) noexcept {
        const std::uint64_t completed = d3d12_fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            enabled = false;
            return E_FAIL;
        }
        for (CommandBuffer& candidate : command_buffers) {
            if (candidate.signal_value <= completed) {
                const VkResult reset = vk.reset_command_buffer(candidate.buffer, 0);
                if (reset != VK_SUCCESS) {
                    return hresult_from_vulkan(reset);
                }
                candidate.signal_value = std::numeric_limits<std::uint64_t>::max();
                *output = candidate.buffer;
                return S_OK;
            }
        }
        if (command_buffers.size() >= kCommandBufferLimit) {
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        }
        VkCommandBufferAllocateInfo allocate_info{};
        allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocate_info.commandPool = command_pool;
        allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate_info.commandBufferCount = 1;
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        const VkResult result = vk.allocate_command_buffers(
            binding.device, &allocate_info, &buffer);
        if (result != VK_SUCCESS) {
            return hresult_from_vulkan(result);
        }
        try {
            command_buffers.push_back({buffer, std::numeric_limits<std::uint64_t>::max()});
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        *output = buffer;
        return S_OK;
    }

    // One submission on the application's queue: an optional command
    // buffer, a wait for the shared fence at wait_value (0: none) and a
    // signal at signal_value once it has run. Made from the thread the
    // application makes its OpenXR calls on; the queue layer serialises it
    // against the application's own submissions, or, without the layer, it
    // is made only inside the calls the application synchronizes.
    [[nodiscard]] HRESULT submit(
        VkCommandBuffer buffer,
        std::uint64_t wait_value,
        std::uint64_t signal_value,
        VkFence fence = VK_NULL_HANDLE) noexcept {
        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo timeline_info{};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline_info.waitSemaphoreValueCount = wait_value != 0 ? 1U : 0U;
        timeline_info.pWaitSemaphoreValues = &wait_value;
        timeline_info.signalSemaphoreValueCount = 1;
        timeline_info.pSignalSemaphoreValues = &signal_value;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.pNext = &timeline_info;
        submit_info.waitSemaphoreCount = wait_value != 0 ? 1U : 0U;
        submit_info.pWaitSemaphores = &semaphore;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = buffer != VK_NULL_HANDLE ? 1U : 0U;
        submit_info.pCommandBuffers = &buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &semaphore;
        const VkResult result = vk.queue_submit(queue, 1, &submit_info, fence);
        if (result != VK_SUCCESS) {
            enabled = false;
            return hresult_from_vulkan(result);
        }
        if (buffer != VK_NULL_HANDLE) {
            for (CommandBuffer& candidate : command_buffers) {
                if (candidate.buffer == buffer) {
                    candidate.signal_value = signal_value;
                }
            }
        }
        last_signalled = signal_value;
        return S_OK;
    }

    [[nodiscard]] VkImageMemoryBarrier whole_barrier(
        VkImage image, VkImageLayout from, VkImageLayout to,
        VkAccessFlags source_access, VkAccessFlags destination_access) const noexcept {
        VkImageMemoryBarrier barrier = image_barrier(
            image, from, to, source_access, destination_access);
        barrier.subresourceRange.aspectMask = aspect();
        return barrier;
    }

    // The images start UNDEFINED and are put once into the layout OpenXR
    // promises an application at xrWaitSwapchainImage. A transition out of
    // UNDEFINED may discard contents, so it cannot be left to a frame. The
    // one CPU wait at initialization.
    [[nodiscard]] HRESULT transition_to_initial_layout(std::uint32_t* failure_stage) noexcept {
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        HRESULT result = acquire_command_buffer(&buffer);
        if (FAILED(result)) {
            *failure_stage = 40;
            return result;
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vk_result = vk.begin_command_buffer(buffer, &begin_info);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 41;
            return hresult_from_vulkan(vk_result);
        }
        const VkImageLayout initial = description.depth_stencil
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        for (VkImage image : application_images) {
            const VkImageMemoryBarrier barrier = whole_barrier(
                image, VK_IMAGE_LAYOUT_UNDEFINED, initial, 0,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
            vk.cmd_pipeline_barrier(
                buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }
        vk_result = vk.end_command_buffer(buffer);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 42;
            return hresult_from_vulkan(vk_result);
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vk_result = vk.create_fence(binding.device, &fence_info, nullptr, &fence);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 43;
            return hresult_from_vulkan(vk_result);
        }
        result = submit(buffer, 0, allocate(), fence);
        if (SUCCEEDED(result)) {
            vk_result = vk.wait_for_fences(
                binding.device, 1, &fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max());
            if (vk_result != VK_SUCCESS) {
                result = hresult_from_vulkan(vk_result);
            }
        }
        vk.destroy_fence(binding.device, fence, nullptr);
        if (FAILED(result)) {
            *failure_stage = 44;
        }
        return result;
    }

    [[nodiscard]] HRESULT initialize(
        const VulkanSessionBinding& input_binding,
        ID3D12Device* input_d3d12_device,
        ID3D12CommandQueue* input_d3d12_queue,
        std::span<ID3D12Resource* const> input_runtime_images,
        const VulkanBridgeSwapchainDescription& requested,
        std::uint32_t* failure_stage) noexcept {
        *failure_stage = 0;
        if (input_binding.instance == VK_NULL_HANDLE ||
            input_binding.physical_device == VK_NULL_HANDLE ||
            input_binding.device == VK_NULL_HANDLE ||
            input_d3d12_device == nullptr || input_d3d12_queue == nullptr ||
            input_runtime_images.empty() || requested.width == 0 ||
            requested.height == 0 || requested.array_size == 0 ||
            requested.requested_format == VK_FORMAT_UNDEFINED) {
            *failure_stage = 1;
            return E_INVALIDARG;
        }
        // Multisampled colour would need a resolve the way the D3D11
        // bridge does it; no Vulkan title seen asks for one.
        if (!requested.depth_stencil && requested.sample_count > 1) {
            *failure_stage = 2;
            return E_NOTIMPL;
        }
        if (!vk.load_loader()) {
            *failure_stage = 3;
            return E_FAIL;
        }
        if (!vk.load_instance(input_binding.instance)) {
            *failure_stage = 4;
            return E_FAIL;
        }
        if (!vk.load_device(input_binding.device)) {
            *failure_stage = 5;
            return E_FAIL;
        }
        LUID luid{};
        HRESULT result = vulkan_adapter_luid(vk, input_binding.physical_device, &luid);
        if (FAILED(result) || !same_luid(luid, input_d3d12_device->GetAdapterLuid())) {
            *failure_stage = 6;
            return FAILED(result) ? result : E_INVALIDARG;
        }
        binding = input_binding;
        d3d12_device = input_d3d12_device;
        d3d12_queue = input_d3d12_queue;
        description = requested;
        vk.get_physical_device_memory_properties(binding.physical_device, &memory_properties);
        vk.get_device_queue(binding.device, binding.queue_family, binding.queue_index, &queue);
        if (queue == VK_NULL_HANDLE) {
            *failure_stage = 7;
            return E_FAIL;
        }
        for (ID3D12Resource* image : input_runtime_images) {
            if (image == nullptr) {
                *failure_stage = 8;
                return E_INVALIDARG;
            }
            runtime_images.emplace_back(image);
        }
        const D3D12_RESOURCE_DESC runtime_description = runtime_images.front()->GetDesc();
        shared_format = runtime_description.Format;
        path = requested.depth_stencil ? VulkanBridgePath::depth_private
                                       : VulkanBridgePath::direct;
        if (path == VulkanBridgePath::direct) {
            shared.resize(runtime_images.size());
            for (SharedImage& image : shared) {
                result = create_shared(runtime_description, &image, failure_stage);
                if (FAILED(result)) {
                    return result;
                }
                shared_views.push_back(image.d3d12.Get());
                application_images.push_back(image.image);
            }
        } else {
            for (std::size_t index = 0; index < runtime_images.size(); ++index) {
                VkImage image = VK_NULL_HANDLE;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                result = create_own_depth(&image, &memory, failure_stage);
                if (image != VK_NULL_HANDLE) {
                    application_images.push_back(image);
                }
                if (memory != VK_NULL_HANDLE) {
                    own_memories.push_back(memory);
                }
                if (FAILED(result)) {
                    return result;
                }
            }
        }
        last_read.assign(runtime_images.size(), 0);

        result = d3d12_device->CreateFence(
            0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(d3d12_fence.GetAddressOf()));
        if (FAILED(result)) {
            *failure_stage = 20;
            return result;
        }
        UniqueHandle fence_handle;
        result = d3d12_device->CreateSharedHandle(
            d3d12_fence.Get(), nullptr, GENERIC_ALL, nullptr, &fence_handle.value);
        if (FAILED(result)) {
            *failure_stage = 21;
            return result;
        }
        VkSemaphoreTypeCreateInfo type_info{};
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        semaphore_info.pNext = &type_info;
        VkResult vk_result = vk.create_semaphore(binding.device, &semaphore_info, nullptr, &semaphore);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 22;
            return hresult_from_vulkan(vk_result);
        }
        VkImportSemaphoreWin32HandleInfoKHR import_info{};
        import_info.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
        import_info.semaphore = semaphore;
        import_info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
        import_info.handle = fence_handle.value;
        vk_result = vk.import_semaphore_win32_handle(binding.device, &import_info);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 23;
            return hresult_from_vulkan(vk_result);
        }
        fence_handle.release();
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event == nullptr) {
            *failure_stage = 24;
            return HRESULT_FROM_WIN32(GetLastError());
        }
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
            VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = binding.queue_family;
        vk_result = vk.create_command_pool(binding.device, &pool_info, nullptr, &command_pool);
        if (vk_result != VK_SUCCESS) {
            *failure_stage = 25;
            return hresult_from_vulkan(vk_result);
        }
        for (CopyList& copy : copy_lists) {
            result = d3d12_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(copy.allocator.GetAddressOf()));
            if (SUCCEEDED(result)) {
                result = d3d12_device->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT, copy.allocator.Get(), nullptr,
                    IID_PPV_ARGS(copy.list.GetAddressOf()));
            }
            if (SUCCEEDED(result)) {
                result = copy.list->Close();
            }
            if (FAILED(result)) {
                *failure_stage = 30;
                return result;
            }
        }
        if (calls_only) {
            initial_layout_pending = true;
        } else {
            result = transition_to_initial_layout(failure_stage);
            if (FAILED(result)) {
                return result;
            }
        }
        enabled = true;
        return S_OK;
    }

    [[nodiscard]] HRESULT take_copy_list(CopyList** output) noexcept {
        CopyList& copy = copy_lists[next_copy_list];
        next_copy_list = (next_copy_list + 1) % kCopyListCount;
        if (copy.fence_value != 0 && d3d12_fence->GetCompletedValue() < copy.fence_value) {
            const HRESULT result = d3d12_fence->SetEventOnCompletion(copy.fence_value, fence_event);
            if (FAILED(result)) {
                return result;
            }
            if (WaitForSingleObject(fence_event, 2000) != WAIT_OBJECT_0)
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        HRESULT result = copy.allocator->Reset();
        if (FAILED(result)) {
            return result;
        }
        result = copy.list->Reset(copy.allocator.Get(), nullptr);
        if (FAILED(result)) {
            return result;
        }
        *output = &copy;
        return S_OK;
    }

    [[nodiscard]] HRESULT wait_for_last_read(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == VulkanBridgePath::depth_private || last_read[index] == 0) {
            return S_OK;
        }
        return submit(VK_NULL_HANDLE, last_read[index], allocate());
    }

    [[nodiscard]] HRESULT after_acquire(std::uint32_t index) noexcept {
        if (!calls_only) {
            return S_OK;
        }
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (initial_layout_pending) {
            std::uint32_t stage = 0;
            const HRESULT result = transition_to_initial_layout(&stage);
            if (FAILED(result)) {
                enabled = false;
                return result;
            }
            initial_layout_pending = false;
        }
        return wait_for_last_read(index);
    }

    [[nodiscard]] HRESULT before_write(std::uint32_t index) noexcept {
        if (calls_only) {
            return enabled && index < runtime_images.size() ? S_OK : E_INVALIDARG;
        }
        return wait_for_last_read(index);
    }

    [[nodiscard]] HRESULT release(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == VulkanBridgePath::depth_private) {
            return S_OK;
        }
        // The application's writes are made available to the other API
        // before the signal: the image stays in the layout OpenXR requires
        // at release.
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        HRESULT result = acquire_command_buffer(&buffer);
        if (FAILED(result)) {
            return result;
        }
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult vk_result = vk.begin_command_buffer(buffer, &begin_info);
        if (vk_result != VK_SUCCESS) {
            return hresult_from_vulkan(vk_result);
        }
        const VkImageMemoryBarrier flush = whole_barrier(
            application_images[index],
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT);
        vk.cmd_pipeline_barrier(
            buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, 1, &flush);
        vk_result = vk.end_command_buffer(buffer);
        if (vk_result != VK_SUCCESS) {
            return hresult_from_vulkan(vk_result);
        }
        const std::uint64_t rendered = allocate();
        result = submit(buffer, 0, rendered);
        if (FAILED(result)) {
            return result;
        }
        result = d3d12_queue->Wait(d3d12_fence.Get(), rendered);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        CopyList* copy = nullptr;
        result = take_copy_list(&copy);
        if (FAILED(result)) {
            return result;
        }
        ID3D12Resource* const runtime_image = runtime_images[index].Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = runtime_image;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        copy->list->ResourceBarrier(1, &barrier);
        copy->list->CopyResource(runtime_image, shared[index].d3d12.Get());
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copy->list->ResourceBarrier(1, &barrier);
        result = copy->list->Close();
        if (FAILED(result)) {
            return result;
        }
        ID3D12CommandList* const lists[] = {copy->list.Get()};
        d3d12_queue->ExecuteCommandLists(1, lists);
        const std::uint64_t copied = allocate();
        result = d3d12_queue->Signal(d3d12_fence.Get(), copied);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        copy->fence_value = copied;
        last_signalled = copied;
        last_read[index] = copied;
        return S_OK;
    }

    [[nodiscard]] HRESULT mark_read(std::uint32_t index) noexcept {
        if (!enabled || index >= runtime_images.size()) {
            return E_INVALIDARG;
        }
        if (path == VulkanBridgePath::depth_private) {
            return S_OK;
        }
        const std::uint64_t value = allocate();
        const HRESULT result = d3d12_queue->Signal(d3d12_fence.Get(), value);
        if (FAILED(result)) {
            enabled = false;
            return result;
        }
        last_signalled = value;
        last_read[index] = value;
        return S_OK;
    }

    [[nodiscard]] HRESULT wait_for_idle() noexcept {
        if (!d3d12_fence || last_signalled == 0 ||
            d3d12_fence->GetCompletedValue() >= last_signalled) {
            return S_OK;
        }
        const HRESULT result = d3d12_fence->SetEventOnCompletion(last_signalled, fence_event);
        if (FAILED(result)) {
            return result;
        }
        return WaitForSingleObject(fence_event, 5000) == WAIT_OBJECT_0 ? S_OK : E_FAIL;
    }
};

VulkanBridgeSwapchain::VulkanBridgeSwapchain() noexcept = default;

VulkanBridgeSwapchain::~VulkanBridgeSwapchain() {
    try {
        std::scoped_lock lock(mutex_);
        if (impl_ != nullptr && FAILED(impl_->wait_for_idle())) {
            static_cast<void>(impl_.release());
        }
    } catch (...) {
    }
}

HRESULT VulkanBridgeSwapchain::initialize(
    const VulkanSessionBinding& binding,
    ID3D12Device* d3d12_device,
    ID3D12CommandQueue* d3d12_queue,
    std::span<ID3D12Resource* const> runtime_images,
    const VulkanBridgeSwapchainDescription& description,
    std::uint32_t* failure_stage) noexcept {
    std::uint32_t stage = 0;
    const auto access = vulkan_queue_access(vulkan_queue_serialized(binding), vulkan_queue_layer_loaded());
    if (access == VulkanQueueAccess::refused) {
        if (failure_stage) *failure_stage = 100;
        return E_NOINTERFACE;
    }
    try {
        std::scoped_lock lock(mutex_);
        auto candidate = std::make_unique<Impl>();
        candidate->calls_only = access == VulkanQueueAccess::synchronized_calls;
        const HRESULT result = candidate->initialize(
            binding, d3d12_device, d3d12_queue, runtime_images, description, &stage);
        if (failure_stage != nullptr) {
            *failure_stage = stage;
        }
        if (FAILED(result)) {
            return result;
        }
        impl_ = std::move(candidate);
        return S_OK;
    } catch (...) {
        if (failure_stage != nullptr) {
            *failure_stage = stage != 0 ? stage : 99;
        }
        return E_FAIL;
    }
}

VulkanBridgePath VulkanBridgeSwapchain::path() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ ? impl_->path : VulkanBridgePath::direct;
}

DXGI_FORMAT VulkanBridgeSwapchain::shared_format() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ ? impl_->shared_format : DXGI_FORMAT_UNKNOWN;
}

std::span<const VkImage> VulkanBridgeSwapchain::vulkan_images() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ ? std::span<const VkImage>(impl_->application_images) : std::span<const VkImage>();
}

std::span<ID3D12Resource* const> VulkanBridgeSwapchain::shared_images() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ ? std::span<ID3D12Resource* const>(impl_->shared_views)
                 : std::span<ID3D12Resource* const>();
}

HRESULT VulkanBridgeSwapchain::after_acquire(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->after_acquire(index) : E_UNEXPECTED;
    } catch (...) {
        return E_FAIL;
    }
}

bool VulkanBridgeSwapchain::synchronized_calls_only() const noexcept {
    std::scoped_lock lock(mutex_);
    return impl_ && impl_->calls_only;
}

HRESULT VulkanBridgeSwapchain::before_write(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->before_write(index) : E_UNEXPECTED;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT VulkanBridgeSwapchain::release(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->release(index) : E_UNEXPECTED;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT VulkanBridgeSwapchain::mark_read(std::uint32_t index) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->mark_read(index) : E_UNEXPECTED;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT VulkanBridgeSwapchain::wait_for_idle() noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return impl_ ? impl_->wait_for_idle() : S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

}  // namespace xrfg
