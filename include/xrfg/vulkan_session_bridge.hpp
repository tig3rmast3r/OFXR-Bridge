#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>

#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>


namespace xrfg {
[[nodiscard]] bool vulkan_queue_serialized(const struct VulkanSessionBinding&) noexcept;
[[nodiscard]] bool vulkan_queue_layer_loaded() noexcept;

// How the bridge may use the application's queue. With the OFXR queue layer
// every submission is serialized. An application that admits no implicit
// Vulkan layer (Star Citizen) has no layer at all: then the bridge touches
// the queue only inside the calls XR_KHR_vulkan_enable lets the runtime use
// it (xrBeginFrame, xrEndFrame, xrAcquireSwapchainImage,
// xrReleaseSwapchainImage), which the application already synchronizes.
// A loaded layer that does not serialize this device is not trusted.
enum class VulkanQueueAccess { refused, serialized, synchronized_calls };
[[nodiscard]] constexpr VulkanQueueAccess vulkan_queue_access(
    bool serialized, bool layer_loaded) noexcept {
    return serialized ? VulkanQueueAccess::serialized
        : layer_loaded ? VulkanQueueAccess::refused : VulkanQueueAccess::synchronized_calls;
}

// The application's Vulkan session as XrGraphicsBindingVulkanKHR describes
// it. The runtime works on this queue inside the frame calls, and Vulkan
// requires a queue to be driven by one thread at a time, so every submission
// the interop makes has to come from the thread the application makes its
// OpenXR calls on.
struct VulkanSessionBinding {
    VkInstance instance{};
    VkPhysicalDevice physical_device{};
    VkDevice device{};
    std::uint32_t queue_family{};
    std::uint32_t queue_index{};
};

// The D3D12 format a shared texture takes for a Vulkan swapchain format, or
// DXGI_FORMAT_UNKNOWN where the synthesizer has nothing to say about it.
[[nodiscard]] DXGI_FORMAT dxgi_format_for_vulkan(VkFormat format) noexcept;
// The depth formats the Vulkan bridge hands a D3D12 runtime for a Vulkan
// depth swapchain, or DXGI_FORMAT_UNKNOWN.
[[nodiscard]] DXGI_FORMAT dxgi_depth_format_for_vulkan(VkFormat format) noexcept;
// The reverse of both, for a D3D12 runtime's format list shown to a Vulkan
// application; VK_FORMAT_UNDEFINED where there is no Vulkan equivalent.
[[nodiscard]] VkFormat vulkan_format_for_dxgi(DXGI_FORMAT format) noexcept;

// Creates a private D3D12 device and direct queue on the adapter the Vulkan
// physical device reports through its LUID. The caller retains both returned
// objects.
[[nodiscard]] HRESULT create_d3d12_device_for_vulkan(
    const VulkanSessionBinding& binding,
    ID3D12Device** d3d12_device,
    ID3D12CommandQueue** d3d12_queue) noexcept;

// How a Vulkan application's image reaches the runtime's on a bridged
// session. Logged in the swapchain's vulkan_bridge record.
enum class VulkanBridgePath : std::uint32_t {
    // The application renders into the shared texture, imported as its
    // swapchain image; at release it is copied into the runtime's image.
    direct = 0,
    // Depth: the application renders into a depth image of the layer's own
    // and nothing reaches the runtime's image. The layer strips the depth
    // information naming this swapchain from every submission, so the
    // runtime composes without depth. A D3D12 depth texture cannot be
    // shared, and a Vulkan depth image cannot be given a D3D12 runtime, so
    // this is the only shape depth can take on the bridge.
    depth_private = 4,
};

struct VulkanBridgeSwapchainDescription {
    // The application's create info, before translation.
    VkFormat requested_format{VK_FORMAT_UNDEFINED};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t array_size{1};
    std::uint32_t mip_levels{1};
    std::uint32_t sample_count{1};
    bool unordered_access{};
    bool depth_stencil{};
};

// One application swapchain of a bridged Vulkan session: the runtime (a
// D3D12 session on the layer's device) owns D3D12 images, the layer owns
// D3D12 shared textures of the same shape, and the application renders into
// those textures through Vulkan images imported from them. Why a Vulkan session
// is handed a D3D12 runtime at all: the interop mirrors every image three
// times over (measured 24 copies per swapchain in No Man's Sky); bridged,
// the layer's paths are the native D3D12 ones and nothing is mirrored.
//
// Every method returns immediately; the two sides are ordered on one shared
// fence, and the only CPU waits are at initialization and teardown.
class VulkanBridgeSwapchain final {
public:
    VulkanBridgeSwapchain() noexcept;
    ~VulkanBridgeSwapchain();
    VulkanBridgeSwapchain(const VulkanBridgeSwapchain&) = delete;
    VulkanBridgeSwapchain& operator=(const VulkanBridgeSwapchain&) = delete;

    // Creates one shared texture per runtime image, in the runtime image's
    // own shape, imports each into the application's device, and makes the
    // fence both sides signal. Fails, with the stage in `failure_stage`, if
    // nothing shareable can be made for the shape.
    [[nodiscard]] HRESULT initialize(
        const VulkanSessionBinding& binding,
        ID3D12Device* d3d12_device,
        ID3D12CommandQueue* d3d12_queue,
        std::span<ID3D12Resource* const> runtime_images,
        const VulkanBridgeSwapchainDescription& description,
        std::uint32_t* failure_stage) noexcept;

    [[nodiscard]] VulkanBridgePath path() const noexcept;
    [[nodiscard]] DXGI_FORMAT shared_format() const noexcept;
    // What the application is given: the imported images on the direct
    // path, its own depth images on the other.
    [[nodiscard]] std::span<const VkImage> vulkan_images() const noexcept;
    [[nodiscard]] std::span<ID3D12Resource* const> shared_images() const noexcept;

    // The application acquired image `index`. In synchronized-calls mode the
    // queue work of initialize (the initial layout) and of before_write is
    // made here instead, since xrCreateSwapchain and xrWaitSwapchainImage may
    // not use the queue; queued before the application's rendering either way.
    [[nodiscard]] HRESULT after_acquire(std::uint32_t index) noexcept;
    [[nodiscard]] bool synchronized_calls_only() const noexcept;

    // The application is about to render into image `index` (its wait
    // returned): its queue waits, on the GPU, for the layer's last read of
    // the shared texture. A no-op in synchronized-calls mode (after_acquire).
    [[nodiscard]] HRESULT before_write(std::uint32_t index) noexcept;

    // The application released image `index`: its queue signals once the
    // rendering is done, the layer's queue waits and copies the shared
    // texture into the runtime's image. What the layer queues after this on
    // the same queue - the history capture - reads the shared texture in
    // order.
    [[nodiscard]] HRESULT release(std::uint32_t index) noexcept;

    // Everything the layer will read from image `index` this frame has been
    // queued: signal, so the next before_write of this image waits for it.
    [[nodiscard]] HRESULT mark_read(std::uint32_t index) noexcept;

    [[nodiscard]] HRESULT wait_for_idle() noexcept;

private:
    struct Impl;
    mutable std::mutex mutex_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xrfg
