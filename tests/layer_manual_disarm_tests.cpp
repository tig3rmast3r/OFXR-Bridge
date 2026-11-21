#define main existing_call_chain_main
#include "layer_call_chain.cpp"
#undef main
#include "xrfg/eye_frame_metadata.h"
#include "xrfg/implicit_layer.hpp"
#include <filesystem>
#include <d3d12sdklayers.h>
#include <dxgidebug.h>
#include <memory>

// Read the runtime-facing D3D11 textures, after publication/retirement, not the
// D3D12 staging output or the independently submitted FPS quad.
std::array<std::size_t, 2> marker_pixels(
    const std::array<ComPtr<ID3D11Texture2D>, 3>& images) {
    std::array<std::size_t, 2> counts{};
    for (const auto& image : images) {
        D3D11_TEXTURE2D_DESC source{}; image->GetDesc(&source);
        auto desc = source;
        desc.ArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> readback;
        if (FAILED(g_d3d11_device->CreateTexture2D(&desc, nullptr, &readback)))
            throw std::runtime_error("create marker readback");
        for (UINT eye = 0; eye < 2; ++eye) {
            g_d3d11_context->CopySubresourceRegion(readback.Get(), 0, 0, 0, 0,
                image.Get(), D3D11CalcSubresource(0, eye, source.MipLevels), nullptr);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(g_d3d11_context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                throw std::runtime_error("map marker readback");
            for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
                const auto* p = static_cast<const unsigned char*>(mapped.pData) + y * mapped.RowPitch + x * 4;
                if (p[0] >= 190 && p[0] <= 192 && p[1] == 0 && p[2] == 255 && p[3] == 255) ++counts[eye];
            }
            g_d3d11_context->Unmap(readback.Get(), 0);
        }
    }
    return counts;
}

void verify_eye_frame_metadata(HMODULE module, XrSwapchain swapchain,
    PFN_xrAcquireSwapchainImage acquire, PFN_xrWaitSwapchainImage wait_image,
    PFN_xrReleaseSwapchainImage release_image, bool native_runtime) {
    const auto publish = reinterpret_cast<PFN_ofxrReleaseSwapchainImageWithEyeMetadataV1>(
        GetProcAddress(module, OFXR_RELEASE_WITH_EYE_METADATA_V1));
    const auto get = reinterpret_cast<PFN_ofxrGetReleasedEyeFrameMetadataV1>(
        GetProcAddress(module, OFXR_GET_RELEASED_EYE_METADATA_V1));
    const auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    require(publish && get, "find eye metadata exports");
    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    const XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const auto acquire_waited = [&] {
        std::uint32_t index{};
        require(XR_SUCCEEDED(acquire(swapchain, nullptr, &index)) &&
            XR_SUCCEEDED(wait_image(swapchain, &wait_info)), "waited eye metadata image");
        return index;
    };
    const auto read = [&] {
        OfxrReleasedEyeFrameMetadataV1 receipt{};
        require(XR_SUCCEEDED(get(swapchain, &receipt)), "read eye metadata receipt");
        return receipt;
    };
    g_acquire_ahead_active = true;
    std::uint64_t serial = 0;
    for (const auto producer : {OFXR_EYE_SOURCE_REAL_VR, OFXR_EYE_SOURCE_UEVR, OFXR_EYE_SOURCE_MUTAR}) {
        OfxrEyeFrameMetadataV1 frame{sizeof(frame), static_cast<std::uint32_t>(producer),
            acquire_waited(), 2, {{7, 101, 41, 0}, {7, 102, 42, 1}}};
        const auto first_slot = frame.image_index;
        require(XR_SUCCEEDED(publish(release_image, swapchain, &release_info, &frame)), "publish stereo receipt");
        const auto first = read();
        require(first.release_serial > serial && first.frame.producer == frame.producer &&
            first.frame.image_index == first_slot && first.frame.eyes[0].render_id == 101,
            "first release stores the supplied render");

        // Simulate a mod copying its retained left eye into a different XR
        // image alongside a newly rendered right eye.
        frame.image_index = acquire_waited();
        frame.eyes[1] = {7, 103, 43, 1};
        require(frame.image_index != first_slot, "copied-forward eye uses a different XR slot");
        require(XR_SUCCEEDED(publish(release_image, swapchain, &release_info, &frame)), "publish carried eye");
        const auto second = read();
        require(second.release_serial == first.release_serial + 1 &&
            second.frame.image_index == frame.image_index &&
            second.frame.eyes[0].stream_id == 7 && second.frame.eyes[0].render_id == 101 &&
            second.frame.eyes[0].guide_token == 41 && second.frame.eyes[1].render_id == 103,
            "copied-forward eye keeps its original render and guide across releases");
        serial = second.release_serial;
    }

    OfxrEyeFrameMetadataV1 frame{sizeof(frame), OFXR_EYE_SOURCE_REAL_VR,
        acquire_waited(), 1, {{7, 104, 44, 0}}};
    if (native_runtime) {
        g_fail_next_release = true;
        require(publish(release_image, swapchain, &release_info, &frame) == XR_ERROR_RUNTIME_FAILURE &&
            read().release_serial == serial, "failed release does not publish a receipt");
    }
    require(XR_SUCCEEDED(release_image(swapchain, &release_info)) &&
        read().release_serial == 0, "ordinary release cannot inherit metadata");

    frame.image_index = (acquire_waited() + 1) % 3;
    require(XR_SUCCEEDED(publish(release_image, swapchain, &release_info, &frame)) &&
        read().release_serial == 0, "wrong slot cannot attach metadata or fail the XR release");
    frame.image_index = acquire_waited();
    frame.eye_count = 2;
    frame.eyes[1] = frame.eyes[0];
    require(XR_SUCCEEDED(publish(release_image, swapchain, &release_info, &frame)) &&
        read().release_serial == 0, "ambiguous eye records cannot attach metadata");
    g_acquire_ahead_active = false;
    std::cout << "Eye frame metadata: all three producers preserve copied-forward render IDs across slots\n";
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 1;
    const std::string mode = argv[2];
    const bool eye_metadata_native = mode == "eye-metadata-native";
    const bool eye_metadata = eye_metadata_native || mode == "eye-metadata";
    const bool luke_no_guides=mode=="luke-no-guides" || mode=="luke-fsr-no-guides" || mode=="luke-dlssg-no-guides";
    const bool luke_color=mode=="luke-color-only" || mode=="luke-color-afw-off" || mode=="luke-color-private";
    const bool luke_fixture_mode=luke_no_guides || luke_color;
    g_luke_scratch_mode=luke_fixture_mode;
    const bool control_mode = argc == 4 && std::string_view(argv[3]) == "controls";
    g_recycle_control_slots=control_mode;
    const std::string marker_mode = argc == 4 && !control_mode && !luke_fixture_mode ? argv[3] : "";
    if (!marker_mode.empty() && (mode != "d3d11" ||
        (marker_mode != "on" && marker_mode != "off" && marker_mode != "hidden"))) return 1;
    g_flight_simulator_mode = mode == "flight";
    g_steamvr_runtime_mode = g_steamvr_presenter_mode = mode == "steamvr";
    g_d3d11_interop_mode = mode == "d3d11" || luke_fixture_mode;
    g_test_application_thread_id = GetCurrentThreadId();
    const auto root = std::filesystem::temp_directory_path() /
        (L"ofxr-disarm-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const auto manifest = root / L"XR_APILAYER_XRFrameBridge_manual-1-1.json";
    HMODULE module = nullptr;
    HMODULE luke_fixture = nullptr;
    HANDLE signal = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrDestroySession destroy_session = nullptr;
    PFN_xrDestroySwapchain destroy_swapchain = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    const auto require = [](bool value, const char* message) {
        if (!value) throw std::runtime_error(message);
    };
    int result = 1;
    const bool retain_failure = std::getenv("XRFG_TEST_KEEP_FAILURE") != nullptr;
    std::unique_ptr<xrfg_fixture::ExternalControlProfile> control_profile;
    if (retain_failure) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }
    try {
        std::filesystem::create_directories(root);
        const auto dll = root / L"XR_APILAYER_XRFrameBridge_diagnostic.dll";
        std::filesystem::copy_file(argv[1], dll);
        signal = xrfg::implicit_layer::create_arm_signal(manifest);
        require(signal != nullptr, "create isolated arm signal");
        const auto control = xrfg::implicit_layer::arm_signal_name(manifest);
        std::string ascii_control;
        for (const wchar_t c : control) ascii_control.push_back(static_cast<char>(c));
        {
            std::ofstream ini(root / L"ofxr_bridge.ini");
            ini << "[ofxr]\nbackend=fidelityfx\ncontrol_event=" << ascii_control
                << "\n[diagnostics]\nlogging_enabled=" << (retain_failure || marker_mode == "on" || marker_mode == "hidden" ? 1 : 0)
                << "\nsynthetic_marker_enabled=" << (marker_mode == "on" || marker_mode == "hidden" ? 1 : 0)
                << "\nflush_each_event=" << (luke_no_guides ? 1 : 0)
                << "\n[overlay]\nposition=" << (!marker_mode.empty() && marker_mode != "hidden" ? "upper_right" : "off") << '\n';
        }
        control_profile = std::make_unique<xrfg_fixture::ExternalControlProfile>(root / L"ofxr_bridge.ini");
        if (eye_metadata_native) {
            auto desired = xrfg::autonomous::snapshot().desired;
            desired.advanced_mode = true;
            desired.independent_pacing = false;
            require(xrfg::autonomous::request(desired), "select isolated native-runtime fixture");
        }
        if(luke_fixture_mode) {
            require(argc==4,"Luke runtime fixture path");
            luke_fixture=LoadLibraryA(argv[3]);require(luke_fixture!=nullptr,"load Luke marker");
            auto desired=xrfg::autonomous::snapshot().desired;
            desired.afw_mode=mode=="luke-color-afw-off" ? 0 : 3;desired.motion_vectors=0;
            desired.enabled=mode!="luke-no-guides";
            desired.frame_generation=mode=="luke-fsr-no-guides" ? 1 : mode=="luke-dlssg-no-guides" ? 2 : 0;
            desired.separate_synthesis=mode=="luke-color-private" ? 1 : 0;
            require(xrfg::autonomous::request(desired),"request native-vector Luke route");
        }
        require(g_d3d11_interop_mode ? initialize_d3d11() : initialize_d3d12(), "initialize GPU fixture");
        if (!marker_mode.empty()) {
            // Deterministic black images: no uninitialized texture can look like
            // a marker, including private slots the fake runtime never acquires.
            for (const auto* images : {&g_d3d11_application_swapchain_images,
                    &g_d3d11_current_swapchain_images, &g_d3d11_synthetic_swapchain_images}) {
                for (const auto& image : *images) {
                    D3D11_RENDER_TARGET_VIEW_DESC rtv{};
                    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray.ArraySize = 2;
                    ComPtr<ID3D11RenderTargetView> target;
                    require(SUCCEEDED(g_d3d11_device->CreateRenderTargetView(image.Get(), &rtv, &target)), "initialize marker target");
                    constexpr float black[4]{0, 0, 0, 1};
                    g_d3d11_context->ClearRenderTargetView(target.Get(), black);
                }
            }
            require(wait_for_queue_idle(), "initialize marker pixels");
        }
        module = LoadLibraryW(dll.c_str());
        require(module != nullptr, "load exact candidate DLL");
        const auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
            GetProcAddress(module, "xrNegotiateLoaderApiLayerInterface"));
        require(negotiate != nullptr, "negotiation export");
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader);
        loader.minInterfaceVersion = 1;
        loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        XrNegotiateApiLayerRequest request{};
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        require(XR_SUCCEEDED(negotiate(&loader, kLayerName, &request)), "negotiate");
        XrApiLayerNextInfo next{};
        next.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        next.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        next.structSize = sizeof(next);
        strcpy_s(next.layerName, kLayerName);
        next.nextGetInstanceProcAddr = fake_get_instance_proc_addr;
        next.nextCreateApiLayerInstance = fake_create_api_layer_instance;
        XrApiLayerCreateInfo layer{};
        layer.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
        layer.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
        layer.structSize = sizeof(layer);
        layer.nextInfo = &next;
        XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        info.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
        require(XR_SUCCEEDED(request.createApiLayerInstance(&info, &layer, &instance)), "create instance");
        const auto get = request.getInstanceProcAddr;
        const auto create_session = get_layer_function<PFN_xrCreateSession>(get, "xrCreateSession");
        const auto begin_session = get_layer_function<PFN_xrBeginSession>(get, "xrBeginSession");
        end_session = get_layer_function<PFN_xrEndSession>(get, "xrEndSession");
        destroy_session = get_layer_function<PFN_xrDestroySession>(get, "xrDestroySession");
        destroy_instance = get_layer_function<PFN_xrDestroyInstance>(get, "xrDestroyInstance");
        const auto create_swapchain = get_layer_function<PFN_xrCreateSwapchain>(get, "xrCreateSwapchain");
        destroy_swapchain = get_layer_function<PFN_xrDestroySwapchain>(get, "xrDestroySwapchain");
        const auto enumerate = get_layer_function<PFN_xrEnumerateSwapchainImages>(get, "xrEnumerateSwapchainImages");
        const auto acquire = get_layer_function<PFN_xrAcquireSwapchainImage>(get, "xrAcquireSwapchainImage");
        const auto wait_image = get_layer_function<PFN_xrWaitSwapchainImage>(get, "xrWaitSwapchainImage");
        const auto release_image = get_layer_function<PFN_xrReleaseSwapchainImage>(get, "xrReleaseSwapchainImage");
        const auto wait = get_layer_function<PFN_xrWaitFrame>(get, "xrWaitFrame");
        const auto begin = get_layer_function<PFN_xrBeginFrame>(get, "xrBeginFrame");
        const auto end = get_layer_function<PFN_xrEndFrame>(get, "xrEndFrame");
        require(create_session && begin_session && create_swapchain && enumerate && acquire && wait_image && release_image && wait && begin && end,
            "frame functions");
        XrGraphicsBindingD3D12KHR binding12{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding12.device = g_device.Get(); binding12.queue = g_queue.Get();
        XrGraphicsBindingD3D11KHR binding11{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding11.device = g_d3d11_device.Get();
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        session_info.systemId = 1;
        session_info.next = g_d3d11_interop_mode ? static_cast<void*>(&binding11) : static_cast<void*>(&binding12);
        require(XR_SUCCEEDED(create_session(instance, &session_info, &session)), "create session");
        XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        require(XR_SUCCEEDED(begin_session(session, &begin_info)), "begin session");
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = DXGI_FORMAT_R8G8B8A8_UNORM; sc.sampleCount = 1;
        sc.width = sc.height = 4; sc.faceCount = 1; sc.arraySize = 2; sc.mipCount = 1;
        require(XR_SUCCEEDED(create_swapchain(session, &sc, &swapchain)), "create swapchain");
        std::array<XrSwapchainImageD3D12KHR, 3> images12{};
        std::array<XrSwapchainImageD3D11KHR, 3> images11{};
        for (auto& image : images12) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR;
        for (auto& image : images11) image.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
        uint32_t count{};
        require(XR_SUCCEEDED(enumerate(swapchain, 3, &count, g_d3d11_interop_mode
            ? reinterpret_cast<XrSwapchainImageBaseHeader*>(images11.data())
            : reinterpret_cast<XrSwapchainImageBaseHeader*>(images12.data()))), "enumerate images");
        const auto capture = [&] {
            XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = XR_INFINITE_DURATION;
            XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; uint32_t index{};
            require(XR_SUCCEEDED(acquire(swapchain, &a, &index)) && XR_SUCCEEDED(wait_image(swapchain, &w)) &&
                XR_SUCCEEDED(release_image(swapchain, &r)), "capture/release image");
        };
        const auto submit = [&](XrTime time) {
            std::array<XrCompositionLayerProjectionView, 2> views{};
            for (uint32_t i = 0; i < 2; ++i) {
                const auto view = fake_submitted_view_for_time(time, i);
                views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                views[i].pose = view.pose; views[i].fov = view.fov;
                views[i].subImage.swapchain = swapchain;
                views[i].subImage.imageRect.extent = {4, 4}; views[i].subImage.imageArrayIndex = i;
            }
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            projection.space = g_space; projection.viewCount = 2; projection.views = views.data();
            const auto* base = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
            XrFrameEndInfo e{XR_TYPE_FRAME_END_INFO};
            e.displayTime = time; e.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            e.layerCount = 1; e.layers = &base;
            // The layer runs its inline second cycle from inside this call;
            // the fake runtime throttles that wait and not the application's.
            g_application_in_end_frame.store(true, std::memory_order_release);
            const XrResult end_result = end(session, &e);
            g_application_in_end_frame.store(false, std::memory_order_release);
            require(XR_SUCCEEDED(end_result), "end frame");
        };
        XrFrameState current{XR_TYPE_FRAME_STATE};
        if (!eye_metadata) require(XR_SUCCEEDED(wait(session, nullptr, &current)), "first wait");
        // One more warm-up frame than the promotion needs on its own: the frame
        // that arms generation passes through, so the first pair - and the
        // throttled inline cycle the promotion counts - is one frame later.
        for (int i = 0; !eye_metadata && i < 9; ++i) {
            require(XR_SUCCEEDED(begin(session, nullptr)), "warmup begin");
            capture();
            XrFrameState next_frame{XR_TYPE_FRAME_STATE};
            if (g_flight_simulator_mode) require(XR_SUCCEEDED(wait(session, nullptr, &next_frame)), "overlapping wait");
            submit(current.predictedDisplayTime + (g_flight_simulator_mode ? kFakeDisplayPeriod : 0));
            require(wait_for_queue_idle(), "warmup GPU retirement");
            if (g_flight_simulator_mode) current = next_frame;
            else require(XR_SUCCEEDED(wait(session, nullptr, &current)), "warmup wait");
        }
        if (luke_no_guides || eye_metadata) {
            if (eye_metadata) {
                verify_eye_frame_metadata(module, swapchain, acquire, wait_image, release_image, eye_metadata_native);
            } else {
                require(g_synthetic_release_calls.load()==0, "missing native producer generated synthetic frames");
                require(g_create_swapchain_calls.load()==1, "missing native producer allocated private swapchains");
                require(g_application_release_calls.load()==9 && g_end_frame_calls.load()>=9,
                    "missing native producer interrupted original XR frames");
                std::cout << "Missing native producer: real frames progress, no generation or private swapchains PASS\n";
            }
            require(XR_SUCCEEDED(end_session(session)), "end fixture session");
            require(XR_SUCCEEDED(destroy_swapchain(swapchain)), "destroy fixture swapchain");
            swapchain=XR_NULL_HANDLE;
            require(XR_SUCCEEDED(destroy_session(session)), "destroy fixture session");
            session=XR_NULL_HANDLE;
            require(XR_SUCCEEDED(destroy_instance(instance)), "destroy fixture instance");
            instance=XR_NULL_HANDLE;
            FreeLibrary(module);module=nullptr;
            control_profile.reset();
            if(signal)CloseHandle(signal);
            std::error_code ignored;std::filesystem::remove_all(root,ignored);
            return 0;
        }
        if(luke_color) {
            require(xrfg::autonomous::snapshot().runtime.afw_running==0,
                "no AFW output may be reported without native guides");
            require(g_application_release_calls.load()==9 && g_create_swapchain_calls.load()==6,
                "color-only Luke captures and private outputs not initialized");
        }
        require(g_synthetic_release_calls.load() > 0, "generation active before Disarm");
        if (g_steamvr_presenter_mode || g_flight_simulator_mode)
            require(current.predictedDisplayPeriod == kFakeDisplayPeriod * 2, "presenter active before Disarm");
        const auto menu_request = [&](int enabled, int backend, int preset, int scale, int backward) {
            auto desired = xrfg::autonomous::snapshot().desired;
            desired.enabled = enabled != 0;
            desired.backend = backend;
            desired.preset = preset;
            desired.scale = scale;
            desired.backward = backward != 0;
            return xrfg::autonomous::request(desired);
        };
        if (control_mode)
            require(menu_request(0, 0, 1, 2, 0), "request reversible bypass through standalone IPC");
        else
            require(xrfg::implicit_layer::signal_arm_stop(manifest), "signal Disarm");
        // Begin/end the already-waited frame first. It may carry the old period,
        // but no new synthetic can be prepared after this stop boundary.
        require(XR_SUCCEEDED(begin(session, nullptr)), "stop boundary begin");
        capture(); submit(current.predictedDisplayTime);
        const auto synthetic_after_stop = g_synthetic_release_calls.load();
        std::size_t records_after_stop{};
        { std::scoped_lock lock(g_end_records_mutex); records_after_stop = g_end_records.size(); }
        for (int i = 0; i < 6; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "disarmed wait");
            require(frame.predictedDisplayPeriod == kFakeDisplayPeriod, "native application period after Disarm");
            require(XR_SUCCEEDED(begin(session, nullptr)), "disarmed begin");
            capture(); submit(frame.predictedDisplayTime);
        }
        require(g_synthetic_release_calls.load() == synthetic_after_stop, "no synthesis after Disarm");
        { std::scoped_lock lock(g_end_records_mutex);
          require(g_end_records.size() >= records_after_stop + 6, "original frames continue");
          for (std::size_t i = records_after_stop; i < g_end_records.size(); ++i)
              require(g_end_records[i].target == SubmittedTarget::original, "only originals after Disarm"); }
        require(wait_for_queue_idle(), "final GPU retirement");
        if (control_mode) {
        // Force context recreation with a different options tuple while keeping
        // FidelityFX so this same test runs on WARP and non-NVIDIA machines.
        require(menu_request(1, 0, 2, 1, 1) != 0, "request re-enable and options change");
        for (int i = 0; i < 10; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "re-enabled wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "re-enabled begin");
            capture(); submit(frame.predictedDisplayTime);
            require(wait_for_queue_idle(), "re-enabled GPU retirement");
        }
        if(g_synthetic_release_calls.load()<=synthetic_after_stop) {
            const auto s=xrfg::autonomous::snapshot();
            std::cerr<<"control receipt revision "<<s.revision<<" applied "<<s.runtime.applied_revision
                <<" enabled "<<s.desired.enabled<<" backend "<<s.desired.backend
                <<" preset "<<s.desired.preset<<" scale "<<s.desired.scale<<" state "
                <<unsigned(s.runtime.generation_state)<<" pending "<<s.runtime.pending<<'\n';
        }
        require(g_synthetic_release_calls.load() > synthetic_after_stop, "synthesis resumes in same XR session");
        const auto before_failure = g_synthetic_release_calls.load();
        require(menu_request(1, 1, 1, 2, 0) != 0, "request unavailable NVIDIA on WARP");
        for (int i = 0; i < 4; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "failed-config wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "failed-config begin");
            capture(); submit(frame.predictedDisplayTime);
        }
        // D3D12 fixture is WARP; the D3D11 interop fixture uses the real adapter
        // and may successfully run NVIDIA, so it cannot assert unavailability.
        if (!g_d3d11_interop_mode)
            require(g_synthetic_release_calls.load() == before_failure, "failed backend must bypass");
        require(menu_request(1, 0, 1, 2, 0) != 0, "recover FidelityFX after configuration failure");
        for (int i = 0; i < 10; ++i) {
            XrFrameState frame{XR_TYPE_FRAME_STATE};
            require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "recovered wait");
            require(XR_SUCCEEDED(begin(session, nullptr)), "recovered begin");
            capture(); submit(frame.predictedDisplayTime);
            require(wait_for_queue_idle(), "recovered GPU retirement");
        }
        require(g_synthetic_release_calls.load() > before_failure, "same-session recovery after backend failure");
        const auto before_logging = g_synthetic_release_calls.load();
        const auto logging_switch = [&](bool enabled) {
            auto desired = xrfg::autonomous::snapshot().desired;
            desired.diagnostics = enabled;
            require(xrfg::autonomous::request(desired), "publish live logging switch");
            const auto deadline = GetTickCount64() + 3000;
            bool applied = false;
            while (GetTickCount64() < deadline) {
                XrFrameState frame{XR_TYPE_FRAME_STATE};
                require(XR_SUCCEEDED(wait(session, nullptr, &frame)), "live logging wait");
                require(XR_SUCCEEDED(begin(session, nullptr)), "live logging begin");
                capture(); submit(frame.predictedDisplayTime);
                require(wait_for_queue_idle(), "live logging GPU retirement");
                const auto status = xrfg::autonomous::snapshot();
                if (status.runtime.applied_revision == status.revision &&
                    status.runtime.logging_active == enabled) { applied = true; break; }
                Sleep(20);
            }
            require(applied, "loaded layer reports actual live logger state without session restart");
        };
        const auto log_paths = [&] {
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::directory_iterator(root))
                if (entry.path().filename().wstring().starts_with(L"ofxr-bridge-flight-") &&
                    entry.path().extension() == L".log") paths.push_back(entry.path());
            return paths;
        };
        logging_switch(false);
        const auto before_logs = log_paths().size();
        logging_switch(true);
        require(log_paths().size() == before_logs + 1, "late logging creates a new flight log");
        logging_switch(false); // retiring the writer drains the first log
        const auto first_logs = log_paths();
        logging_switch(true);
        require(log_paths().size() == first_logs.size() + 1, "logging restart preserves previous log and creates a distinct file");
        logging_switch(false);
        for (const auto& path : log_paths()) {
            std::ifstream input(path);
            const std::string contents{std::istreambuf_iterator<char>(input), {}};
            // F9 off intentionally records no startup diagnostics. Every new
            // file identifies its logger version/PID; an initially enabled
            // run additionally retains the actually observed runtime identity.
            require(contents.find("op=logger result=") != std::string::npos &&
                (!retain_failure || contents.find("op=runtime_identity") != std::string::npos),
                "late-started log identifies its build/process without inventing unobserved startup data");
            require(contents.find("op=downstream_first_end_frame") != std::string::npos,
                "live logging records frames from the existing loaded session");
        }
        require(g_synthetic_release_calls.load() > before_logging,
            "generation continues while logging toggles in the same session");
        logging_switch(retain_failure);
        std::cout << "Live logging IPC: off/on/off/on/off, distinct files, retained startup metadata, no session restart\n";
        require(GetPrivateProfileIntW(L"diagnostics", L"logging_enabled", 42, (root / L"ofxr_bridge.ini").c_str()) == (retain_failure ? 1 : 0),
            "menu preserves diagnostic flag");
        const auto reported = xrfg::autonomous::snapshot();
        require(reported.connected && reported.runtime.applied_revision == reported.revision,
            "real standalone IPC settings reached the loaded layer");
        }
        if (!marker_mode.empty()) {
            const auto synthetic = marker_pixels(g_d3d11_synthetic_swapchain_images);
            for (const auto count : synthetic)
                require((count > 0) == (marker_mode == "on"), "D3D11 published marker/debug/Off gate");
            for (const auto* images : {&g_d3d11_application_swapchain_images, &g_d3d11_current_swapchain_images})
                for (const auto count : marker_pixels(*images)) require(count == 0, "marker contaminated an original");
            std::cout << "D3D11 runtime-facing synthetic marker " << marker_mode << ": "
                      << synthetic[0] << '/' << synthetic[1] << " purple pixels; originals clean\n";
        }
        result = 0;
    } catch (const std::exception& error) { std::cerr << mode << ": " << error.what() << '\n'; }
    if (session && end_session) end_session(session);
    if (swapchain && destroy_swapchain) destroy_swapchain(swapchain);
    if (session && destroy_session) destroy_session(session);
    if (instance && destroy_instance) destroy_instance(instance);
    if(control_mode && result==0 && std::any_of(g_control_slot_live.begin()+1,g_control_slot_live.end(),[](bool live){return live;})) {
        std::cerr<<"private control slots remain live after teardown\n";result=1;
    }
    if (module) FreeLibrary(module);
    // The production hook monitor outlives XR teardown. Keep the marker
    // module loaded until process exit, just as the actual game injector is.
    control_profile.reset();
    if (signal) CloseHandle(signal);
    std::error_code ignored;
    if (retain_failure && result) {
        std::cerr << "Evidence: " << root.string() << '\n';
        ComPtr<IDXGIInfoQueue> info;
        const GUID all{0xe48ae283,0xda80,0x490b,{0x87,0xe6,0x43,0xe9,0xa9,0xcf,0xda,0x08}};
        if (SUCCEEDED(DXGIGetDebugInterface1(0,IID_PPV_ARGS(&info)))) {
            for(UINT64 i=0;i<info->GetNumStoredMessages(all);++i) {
                SIZE_T bytes{}; info->GetMessage(all,i,nullptr,&bytes);
                std::vector<unsigned char> data(bytes);auto* m=reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(data.data());
                if(SUCCEEDED(info->GetMessage(all,i,m,&bytes)) && m->Severity<=DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR)
                    std::cerr << m->pDescription << '\n';
            }
        }
    }
    else if (root.is_absolute() && root.filename().wstring().starts_with(L"ofxr-disarm-")) std::filesystem::remove_all(root, ignored);
    if (!result) std::cout << "Live Disarm: " << mode << " passed\n";
    return result;
}
