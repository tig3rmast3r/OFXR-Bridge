// Exercise the actual layer's API translation without activating a Vulkan
// layer globally or requiring a game/runtime. GPU transfer tests are separate.
void vulkan_bridge_contract_test() {
    auto s = session();
    s->presenter_active = false;
    s->dispatch->enumerate_swapchain_formats = +[](XrSession, std::uint32_t cap,
        std::uint32_t* count, std::int64_t* formats) -> XrResult {
        constexpr std::int64_t source[]{DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_UNKNOWN};
        if (!count) return XR_ERROR_VALIDATION_FAILURE;
        *count = 4;
        if (!cap) return XR_SUCCESS;
        if (cap < 4) return XR_ERROR_SIZE_INSUFFICIENT;
        if (!formats) return XR_ERROR_VALIDATION_FAILURE;
        std::copy(std::begin(source), std::end(source), formats);
        return XR_SUCCESS;
    };
    { std::scoped_lock lock(g_state_mutex); g_sessions[s->handle] = s; }
    std::uint32_t count = 0;
    check(layer_enumerate_swapchain_formats(s->handle, 0, &count, nullptr) == XR_SUCCESS && count == 4,
        "ordinary D3D12 format enumeration changed");
    s->vulkan_bridge = std::make_unique<SessionState::VulkanBridge>();
    check(layer_enumerate_swapchain_formats(s->handle, 0, &count, nullptr) == XR_SUCCESS && count == 2,
        "Vulkan format size query did not omit duplicates/unknown formats");
    std::array<std::int64_t, 2> formats{-1, -1};
    check(layer_enumerate_swapchain_formats(s->handle, 1, &count, formats.data()) == XR_ERROR_SIZE_INSUFFICIENT &&
        formats[0] == -1 && formats[1] == -1, "undersized Vulkan format fill modified output");
    check(layer_enumerate_swapchain_formats(s->handle, 2, &count, formats.data()) == XR_SUCCESS &&
        formats[0] == VK_FORMAT_R8G8B8A8_UNORM && formats[1] == VK_FORMAT_D32_SFLOAT,
        "Vulkan format order/translation changed");
    check(layer_enumerate_swapchain_formats(s->handle, 2, nullptr, formats.data()) == XR_ERROR_VALIDATION_FAILURE,
        "Vulkan format null count accepted");
    check(!xrfg::vulkan_queue_serialized({}), "null/unregistered Vulkan device passed serialization gate");
    // Star Citizen: no implicit Vulkan layer at all -> synchronized calls;
    // a loaded layer that does not serialize the device is still refused.
    using Access = xrfg::VulkanQueueAccess;
    check(xrfg::vulkan_queue_access(true, true) == Access::serialized &&
        xrfg::vulkan_queue_access(false, true) == Access::refused &&
        xrfg::vulkan_queue_access(false, false) == Access::synchronized_calls,
        "Vulkan queue access policy changed");
    check(!xrfg::vulkan_queue_layer_loaded() &&
        xrfg::vulkan_queue_access(xrfg::vulkan_queue_serialized({}), xrfg::vulkan_queue_layer_loaded()) ==
            Access::synchronized_calls,
        "a process without the queue layer did not select synchronized calls");
    {
        xrfg::VulkanBridgeSwapchain unused;
        check(unused.after_acquire(0) == E_UNEXPECTED && !unused.synchronized_calls_only() &&
            unused.before_write(0) == E_UNEXPECTED, "uninitialized Vulkan bridge accepted queue work");
    }
    check(xrfg::dxgi_format_for_vulkan(VK_FORMAT_UNDEFINED) == DXGI_FORMAT_UNKNOWN,
        "unsupported Vulkan color format silently translated");
    s->vulkan_bridge.reset();
    { std::scoped_lock lock(g_state_mutex); g_sessions.erase(s->handle); }
    std::cout << "Actual Vulkan layer API: deduplicated format translation, capacity and serialization gate\n";
}
