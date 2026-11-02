#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/autonomous_control.hpp"
#include "xrfg/luke_afw_source.hpp"
#include <openxr/openxr_platform.h>
#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
void write_position(const std::filesystem::path& ini, const wchar_t* position) {
    // Match the tray's file replacement, not the Win32 profile writer/cache.
    const std::filesystem::path temporary = ini.wstring() + L".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "[overlay]\r\nposition=";
        for (const wchar_t* c = position; *c; ++c) stream << static_cast<char>(*c);
        stream << "\r\n";
        stream.flush();
        check(static_cast<bool>(stream), "write atomic INI");
    }
    check(MoveFileExW(temporary.c_str(), ini.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0, "replace atomic INI");
}
template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
const auto session = handle<XrSession>(2);
const auto overlay_space = handle<XrSpace>(4);
const auto overlay_chain = handle<XrSwapchain>(5);
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D11Device> device11;
std::vector<ComPtr<ID3D12Resource>> textures;
std::vector<ComPtr<ID3D11Texture2D>> textures11;
unsigned created{}, destroyed{}, space_created{}, space_destroyed{}, acquires{}, waits{}, releases{}, ends{};
unsigned acquired_index{}, last_released{}, width{}, height{};
bool timeout_next = true, fail_end{}, unsupported{}, d3d11{};
unsigned clock_conversions{};
bool clock_available=true;
std::int64_t fixed_display_clock{};
bool display_clock(void*, XrTime, std::int64_t& out) {
    ++clock_conversions;
    out=fixed_display_clock?fixed_display_clock:xrfg::interface_clock_ns()-5'000'000;
    return clock_available;
}
const XrCompositionLayerBaseHeader* expected_layer{};
const void* expected_next{};
unsigned expected_count{1}, observed_count{};
XrPosef last_pose{};
XrResult XRAPI_CALL create_space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* output) {
    check(info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW && info->poseInReferenceSpace.orientation.w == 1,
          "overlay must own a VIEW space");
    ++space_created; *output = overlay_space; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace space) {
    check(space == overlay_space, "destroyed app space"); ++space_destroyed; return XR_SUCCESS;
}
XrResult XRAPI_CALL formats(XrSession, std::uint32_t capacity, std::uint32_t* count, std::int64_t* output) {
    *count = 1; if (capacity) output[0] = DXGI_FORMAT_R8G8B8A8_UNORM; return XR_SUCCESS;
}
XrResult XRAPI_CALL properties(XrInstance, XrSystemId, XrSystemProperties* output) {
    output->graphicsProperties = {4096, 4096, 2}; return XR_SUCCESS;
}
XrResult XRAPI_CALL create_chain(XrSession, const XrSwapchainCreateInfo* info, XrSwapchain* output) {
    check(info->arraySize == 1 && info->sampleCount == 1 && info->format == DXGI_FORMAT_R8G8B8A8_UNORM,
          "overlay swapchain geometry/format");
    check((info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0, "upload usage missing");
    width = info->width; height = info->height;
    if (d3d11) {
        textures11.resize(3);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (auto& texture : textures11) check(SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, &texture)), "D3D11 texture");
    } else {
        textures.resize(3);
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (auto& texture : textures) check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&texture))), "D3D12 texture");
    }
    ++created; *output = overlay_chain; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_chain(XrSwapchain chain) {
    check(chain == overlay_chain, "destroyed app swapchain"); ++destroyed; return XR_SUCCESS;
}
XrResult XRAPI_CALL enumerate(XrSwapchain, std::uint32_t capacity, std::uint32_t* count, XrSwapchainImageBaseHeader* output) {
    *count = 3;
    if (capacity) {
        if (d3d11) { auto* images = reinterpret_cast<XrSwapchainImageD3D11KHR*>(output);
            for (unsigned i = 0; i < 3; ++i) images[i].texture = textures11[i].Get();
        } else { auto* images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(output);
            for (unsigned i = 0; i < 3; ++i) images[i].texture = textures[i].Get(); }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, std::uint32_t* index) {
    acquired_index = acquires++ % 3; *index = acquired_index; return XR_SUCCESS;
}
XrResult XRAPI_CALL wait(XrSwapchain, const XrSwapchainImageWaitInfo* info) {
    check(info->timeout == 0, "overlay adds a blocking image wait"); ++waits;
    if (timeout_next) { timeout_next = false; return XR_TIMEOUT_EXPIRED; } return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain, const XrSwapchainImageReleaseInfo*) {
    ++releases; last_released = acquired_index; return XR_SUCCESS;
}
XrResult XRAPI_CALL end(XrSession, const XrFrameEndInfo* info) {
    ++ends;
    observed_count = info->layerCount;
    check(info->next == expected_next, "lost frame next chain");
    if (info->layerCount) check(info->layers[0] == expected_layer, "mutated original projection");
    if (info->layerCount > expected_count) {
        check(info->layerCount == expected_count + 1, "wrong layer count");
        const auto* quad = reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[expected_count]);
        check(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && quad->space == overlay_space &&
            quad->subImage.swapchain == overlay_chain && quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH,
            "invalid overlay quad");
        check(releases > 0 && quad->subImage.imageRect.extent.width == static_cast<int>(width), "unreleased overlay image");
        check(quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              "overlay must blend premultiplied source alpha");
        check(width == height * 2, "numeric overlay aspect ratio");
        last_pose = quad->pose;
    }
    if (fail_end) { fail_end = false; return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL get(XrInstance, const char* name, PFN_xrVoidFunction* output) {
    *output = nullptr;
    if (unsupported) return XR_ERROR_FUNCTION_UNSUPPORTED;
#define FN(n, f) if (std::strcmp(name, n) == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    FN("xrCreateReferenceSpace", create_space) FN("xrDestroySpace", destroy_space)
    FN("xrCreateSwapchain", create_chain) FN("xrDestroySwapchain", destroy_chain)
    FN("xrEnumerateSwapchainFormats", formats) FN("xrEnumerateSwapchainImages", enumerate)
    FN("xrAcquireSwapchainImage", acquire) FN("xrWaitSwapchainImage", wait) FN("xrReleaseSwapchainImage", release)
    FN("xrGetSystemProperties", properties)
#undef FN
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

// Test-only GPU readback proves that the production overlay actually uploaded
// its red/green bitmap. Production contains no such readbacks or queue drains.
std::size_t pixel_count(std::uint32_t wanted) {
    if (d3d11) {
        auto desc = D3D11_TEXTURE2D_DESC{}; textures11[last_released]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(SUCCEEDED(device11->CreateTexture2D(&desc, nullptr, &staging)), "staging11");
        ComPtr<ID3D11DeviceContext> context; device11->GetImmediateContext(&context);
        context->CopyResource(staging.Get(), textures11[last_released].Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map11");
        std::size_t result{};
        for (unsigned y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < width; ++x) result += row[x] == wanted;
        }
        context->Unmap(staging.Get(), 0); return result;
    }
    const auto desc = textures[last_released]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 bytes{};
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes; buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))), "readback12");
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))), "allocator12");
    check(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))), "list12");
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = textures[last_released].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{}, dest{};
    source.pResource = textures[last_released].Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dest.pResource = readback.Get(); dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dest.PlacedFootprint = footprint;
    list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
    check(SUCCEEDED(list->Close()), "close12");
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "fence12");
    check(SUCCEEDED(queue->Signal(fence.Get(), 1)), "signal12");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(event && SUCCEEDED(fence->SetEventOnCompletion(1, event)), "event12");
    check(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "GPU readback timed out"); CloseHandle(event);
    void* mapped{}; const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    check(SUCCEEDED(readback->Map(0, &range, &mapped)), "map12");
    std::size_t result{};
    for (unsigned y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < width; ++x) result += row[x] == wanted;
    }
    const D3D12_RANGE empty{}; readback->Unmap(0, &empty); return result;
}
}

int main(int argc, char** argv) {
    const auto ini = std::filesystem::temp_directory_path() /
        (L"ofxr-overlay-test-" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    const auto profile=std::filesystem::temp_directory_path()/(L"ofxr-overlay-profile-"+std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(profile);SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",profile.c_str());
        d3d11 = argc > 1 && std::string(argv[1]) == "d3d11";
        if (d3d11) {
            check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device11, nullptr, nullptr)), "WARP D3D11");
        } else {
            ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter> warp;
            check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "WARP adapter");
            check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP D3D12");
            D3D12_COMMAND_QUEUE_DESC desc{};
            check(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))), "queue12");
        }
        XrCompositionLayerProjectionView view{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        view.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
        view.subImage.imageRect.extent = {2688, 2784};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.space = handle<XrSpace>(99); projection.viewCount = 1; projection.views = &view;
        expected_layer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection);
        std::array<const XrCompositionLayerBaseHeader*, 2> original{expected_layer, expected_layer};
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO}; info.layerCount = 1; info.layers = original.data();
        const XrBaseInStructure extension{XR_TYPE_UNKNOWN}; expected_next = &extension; info.next = expected_next;
        write_position(ini, L"upper_right");
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1), session, 1, get, end,
                device.Get(), queue.Get(), device11.Get(), ini, false, nullptr, nullptr, display_clock);
            overlay.application_frame(&info);
            check(acquires == 1 && waits == 1 && releases == 0, "timeout ownership violated");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1, "uninitialized quad submitted");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(acquires == 1 && waits == 2 && releases == 1, "must retry same acquired image after timeout");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "fallback missing overlay");
            check(pixel_count(0xff5050ffu) > 100, "inactive bitmap missing red on GPU");
            check(pixel_count(0) > static_cast<std::size_t>(width) * height / 2,
                  "numeric overlay background must be transparent on GPU");
            check(last_pose.position.x > 0 && last_pose.position.y > 0, "default quadrant");
            check(last_pose.position.x > 0.879f && last_pose.position.x < 0.881f &&
                  last_pose.position.y > 0.779f && last_pose.position.y < 0.781f,
                  "runtime quad must move left and down without changing quadrant");
            const unsigned release_count = releases;
            overlay.application_frame(&info);
            check(releases == release_count, "upload unthrottled");
            check(overlay.end_frame(&info, true) == XR_SUCCESS && overlay.metrics().active, "successful synthetic not active");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "original loses overlay");
            check(pixel_count(0xff70eb40u) > 100, "active bitmap missing green on GPU");
            XrFrameEndInfo empty = info; empty.layerCount = 0; empty.layers = nullptr;
            const float before_empty = overlay.metrics().submitted_fps;
            check(overlay.end_frame(&empty, true) == XR_SUCCESS && observed_count == 0, "added overlay to shouldRender=false/empty frame");
            check(overlay.metrics().submitted_fps < before_empty + 0.1f, "counted empty synthetic");
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            fail_end = true;
            check(overlay.end_frame(&info, true) == XR_ERROR_RUNTIME_FAILURE && !overlay.metrics().active, "failed end marked active");
            // The physical S slot still exists, but its content is a copy of B.
            // Exercise the actual overlay submission/bitmap boundary on both APIs.
            overlay.reset_metrics();
            auto native_bypass=std::make_shared<xrfg::D3D12NativeFrameResult>();
            native_bypass->outputs=1;native_bypass->skip=1;native_bypass->detail=5;
            native_bypass->mask.store(0,std::memory_order_release);
            const std::array native_results{native_bypass};
            check(overlay.end_frame(&info,true,true,native_results)==XR_SUCCESS,
                "native copy physical submission failed");
            check(!overlay.metrics().active && overlay.metrics().submitted_fps==0,
                "native copy must neither add FPS nor turn green");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(pixel_count(0xff5050ffu)>100 && pixel_count(0xff70eb40u)==0,
                "copied native S rendered a green bitmap");
            check(overlay.end_frame(&info,false,true,native_results)==XR_SUCCESS &&
                overlay.metrics().submitted_fps>0 && !overlay.metrics().active,
                "real B after native copy must count once and remain red");
            for (auto name : {L"upper_left", L"lower_left", L"lower_right", L"upper_right", L"off"}) {
                write_position(ini, name);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS, "position frame");
                if (std::wstring(name) == L"off") {
                    check(observed_count == 1, "live Off failed");
                    check(!overlay.marker_placement(), "Off left diagnostic marker enabled");
                }
                else {
                    check(observed_count == 2, "position lost overlay");
                    check(overlay.marker_placement().has_value(), "visible counter has no marker placement");
                    const auto text = std::wstring(name);
                    check((last_pose.position.x < 0) == (text.find(L"left") != text.npos) &&
                          (last_pose.position.y > 0) == (text.find(L"upper") != text.npos), "live quadrant wrong");
                }
            }
            write_position(ini, L"upper_right");
            std::this_thread::sleep_for(std::chrono::milliseconds(260)); overlay.application_frame(&info);
            info.layerCount = expected_count = 2;
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "exceeded runtime maxLayerCount");
            overlay.reset_metrics(); check(!overlay.metrics().active && overlay.metrics().submitted_fps == 0, "session reset");
            info.layerCount = expected_count = 1;
            // Asymmetric/narrow FOV catches accidental fallback to the default
            // 90-degree placement. Flipping image storage must not move the quad.
            const auto saved_fov = view.fov;
            view.fov = {-0.65f, 0.55f, 0.42f, -0.61f};
            for (auto name : {L"upper_left", L"upper_right", L"lower_left", L"lower_right"}) {
                write_position(ini, name);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "normal FOV placement");
                const auto upright_pose = last_pose;
                std::swap(view.fov.angleUp, view.fov.angleDown);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
                overlay.application_frame(&info);
                check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "inverted FOV placement");
                check(std::abs(last_pose.position.x - upright_pose.position.x) < 1.0e-6f &&
                      std::abs(last_pose.position.y - upright_pose.position.y) < 1.0e-6f &&
                      std::abs(last_pose.position.z - upright_pose.position.z) < 1.0e-6f,
                      "vertical FOV inversion moved the overlay");
                check(view.fov.angleDown > view.fov.angleUp, "overlay reordered the application's FOV");
                std::swap(view.fov.angleUp, view.fov.angleDown);
            }
            view.fov = saved_fov;
            write_position(ini, L"upper_right");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, true) == XR_SUCCESS && observed_count == 2, "overlay visible before manual stop");
            // Exercise production latency admission, rate limit and hidden gate
            // on the exact same runtime composition path as the GPU counter.
            using namespace xrfg::autonomous;
            RuntimeStatus runtime{};runtime.sessions=runtime.ready=1;report_runtime(runtime);
            set_interface_visible(InterfaceSurface::desktop,true);
            const auto entry=xrfg::interface_clock_ns()-25'000'000;
            auto cycle=std::make_shared<xrfg::InterfaceCycle>(entry-30'000'000,entry-30'000'000);
            cycle->call(entry-30'000'000,entry-25'000'000);
            cycle->call(entry,entry+1'000'000);
            xrfg::InterfaceFrameStamp stamp{400,entry,entry-10'000'000,cycle};
            fail_end=true;check(XR_FAILED(overlay.end_frame(&info,false,true,{},0,stamp)),"failed downstream sample");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"successful retry of same frame");
            auto conversions=clock_conversions;
            check(overlay.end_frame(&info,true,true,{},0,stamp)==XR_SUCCESS &&
                overlay.end_frame(&info,false,false,{},0,stamp)==XR_SUCCESS && clock_conversions==conversions,"synthetic/repeat never convert display clock");
            // Complete the app-facing tail after the downstream runtime call.
            const auto returned=xrfg::interface_clock_ns();
            cycle->call(entry+1'000'000,returned);cycle->producer_returned(returned);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            auto measured=snapshot().runtime.telemetry;
            check(measured.delay_valid && measured.ofxr_delay_ms>=25 && measured.cpu_valid &&
                measured.presentation_valid && measured.presentation_ms<=-5,"real frame pipeline and late display clock are published");
            check(measured.cycle_valid && measured.ofxr_time_valid && measured.cycle_length_ms>=55 &&
                measured.ofxr_time_ms>=30, "same accepted frame publishes complete cycle after app tail returns");
            const auto publication=measured.updated_ms;overlay.application_frame(&info);
            check(snapshot().runtime.telemetry.updated_ms==publication,"telemetry publication limited to 4 Hz");
            overlay.reset_metrics();clock_available=false;
            stamp={401,xrfg::interface_clock_ns()-5'000'000,0};
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"no clock extension remains a valid real submission");
            overlay.application_frame(&info);measured=snapshot().runtime.telemetry;
            check(measured.delay_valid && !measured.presentation_valid && !measured.cpu_valid,"clock conversion absence yields independent N/A sources");
            // Pair-slot estimate through the actual runtime submission wrapper,
            // independently of unavailable XrTime clock conversion.
            overlay.reset_metrics();overlay.set_display_period(10'000'000);
            auto estimate_start=xrfg::interface_clock_ns()-5'000'000;
            auto estimate_cycle=std::make_shared<xrfg::InterfaceCycle>(estimate_start);
            stamp={403,estimate_start,0,estimate_cycle};
            fail_end=true;check(XR_FAILED(overlay.end_frame(&info,true,true,{},0,stamp)),"failed synthetic has no slot receipt");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"real without successful synthetic remains valid");
            estimate_cycle->producer_returned(xrfg::interface_clock_ns());
            float estimate{};check(estimate_cycle->read_minimum_cycle(xrfg::interface_clock_ns(),estimate),"successful real runtime return is observable without a generated frame");
            overlay.reset_metrics();estimate_start=xrfg::interface_clock_ns()-5'000'000;
            estimate_cycle=std::make_shared<xrfg::InterfaceCycle>(estimate_start);stamp={404,estimate_start,0,estimate_cycle};
            conversions=clock_conversions;
            check(overlay.end_frame(&info,true,true,{},0,stamp)==XR_SUCCESS,"successful synthetic receipt");
            check(clock_conversions==conversions,"slot estimate never converts a synthetic display clock");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"matching real receipt");
            estimate_cycle->producer_returned(xrfg::interface_clock_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            measured=snapshot().runtime.telemetry;
            check(measured.cycle_valid && measured.cycle_estimated_valid && measured.cycle_estimated_ms>=14 &&
                measured.cycle_estimated_ms>measured.cycle_length_ms+5 && !measured.presentation_valid,
                "real composition path publishes measured plus estimated cycle without clock extension");
            // A pending NGX decision contributes only after existing nonblocking
            // content polling confirms actual generation; a native copy never does.
            overlay.reset_metrics();estimate_start=xrfg::interface_clock_ns()-5'000'000;
            estimate_cycle=std::make_shared<xrfg::InterfaceCycle>(estimate_start);stamp={405,estimate_start,0,estimate_cycle};
            auto pending=std::make_shared<xrfg::D3D12NativeFrameResult>();pending->outputs=1;
            std::array native{pending};
            check(overlay.end_frame(&info,true,true,native,0,stamp)==XR_SUCCESS,"pending native receipt");
            const auto real_clock=xrfg::interface_clock_ns();
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"native real receipt");
            estimate_cycle->producer_returned(xrfg::interface_clock_ns());
            check(estimate_cycle->read_minimum_cycle(real_clock,estimate),"real runtime completion does not depend on confirming unrelated synthetic pixels");
            pending->mask.store(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(snapshot().runtime.telemetry.cycle_estimated_valid,"native confirmation retains original acceptance receipt for slot estimate");
            // V632: a VDXR-style accepted S/B pair must not throw away its
            // real converted future target just because it has a synthetic.
            overlay.reset_metrics();clock_available=true;
            set_interface_visible(InterfaceSurface::desktop,true);
            estimate_start=xrfg::interface_clock_ns()-2'000'000;
            fixed_display_clock=estimate_start+42'000'000;
            estimate_cycle=std::make_shared<xrfg::InterfaceCycle>(estimate_start);
            stamp={406,estimate_start,0,estimate_cycle};
            check(overlay.end_frame(&info,true,true,{},0,stamp)==XR_SUCCESS,"queued S acceptance");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"queued B acceptance with real clock");
            estimate_cycle->producer_returned(xrfg::interface_clock_ns());
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            measured=snapshot().runtime.telemetry;
            check(measured.cycle_estimated_valid && measured.cycle_estimated_ms>=2 && measured.cycle_estimated_ms<42.F &&
                measured.cycle_estimated_source==xrfg::InterfaceSource::minimum_cycle_qpc,
                "overlay/benchmark exclude the42ms predicted target and report actual runtime return");
            fixed_display_clock=0;overlay.reset_metrics();
            set_interface_visible(InterfaceSurface::desktop,false);conversions=clock_conversions;
            stamp={402,xrfg::interface_clock_ns(),0};
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS && clock_conversions==conversions,"hidden UI cannot read display clock");
            clock_available=true;
            set_interface_visible(InterfaceSurface::desktop,true);
            overlay.set_luke_scene_measurement(true);
            overlay.reset_metrics();
            stamp={600,xrfg::interface_clock_ns(),0};stamp.scene_owner=42;stamp.scene_serial=7;
            fail_end=true;
            check(XR_FAILED(overlay.end_frame(&info,false,true,{},0,stamp)),"Luke failed send");
            check(!overlay.metrics().sample_valid,"Failed Luke send cannot count");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"Luke unique real accepted without compositor timing");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            check(overlay.metrics().sample_valid,"Known accepted identity is measured without SteamVR");
            stamp.scene_serial=8;
            check(overlay.end_frame(&info,true,true,{},0,stamp)==XR_SUCCESS,"Luke confirmed generated send");
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"Luke matching real send");
            overlay.application_frame(&info);measured=snapshot().runtime.telemetry;
            check(measured.output_source==xrfg::InterfaceSource::accepted_scene_submissions &&
                measured.output_fps>0 && measured.rates_valid && measured.generated_fps>0 && measured.game_fps>0,
                "Luke counters available with asynchronous or absent SteamVR timing");
            const auto totals=overlay.metrics();
            for(int n=0;n<12;++n)check(overlay.end_frame(&info,false,false,{},0,stamp)==XR_SUCCESS,"Luke held repeat");
            check(overlay.metrics().delivered_fps<=totals.delivered_fps+.01F,"held repeats cannot inflate total");
            check(overlay.metrics().generated_fps>0,"existing real/generated counter preserved");
            // Scene-cut fallback has exact original CRCs but no synthesized
            // peer yet. It is measured as real content, not another AFW output.
            overlay.reset_metrics();
            stamp={601,xrfg::interface_clock_ns(),0};
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"unknown cut frame accepted");
            check(!overlay.metrics().delivered_valid,"unknown cut frame invented a total");
            const auto original=xrfg::luke_original_scene_identity({1001,1002},{0,1});
            stamp.scene_owner=original.owner;stamp.scene_serial=original.serial;
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"known original fallback accepted");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stamp.scene_serial=xrfg::luke_original_scene_identity({1003,1002},{0,1}).serial;
            check(overlay.end_frame(&info,false,true,{},0,stamp)==XR_SUCCESS,"next original fallback accepted");
            overlay.application_frame(&info);measured=snapshot().runtime.telemetry;
            check(measured.output_source==xrfg::InterfaceSource::accepted_scene_submissions &&
                measured.output_fps>0 && measured.output_fps<20 && measured.generated_fps==0,
                "cut recovery stayed N/A for one second or invented generated/refresh-rate FPS");
            overlay.reset_metrics();
            check(!overlay.metrics().sample_valid,"Luke reset clears counts");
            overlay.set_luke_scene_measurement(false);
            const auto uploads_before_stop = releases;
            overlay.suspend();
            check(!overlay.marker_placement(), "manual stop left diagnostic marker enabled");
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1, "manual stop removes quad immediately");
            overlay.reset_metrics();
            std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 1 && releases == uploads_before_stop,
                  "stopped overlay never uploads/reappears after refresh or metric reset");
        }
        check(created >= 2 && created == destroyed && space_created == 1 && space_destroyed == 1, "overlay Off/reopen lifetime leak");
        unsupported = true;
        info.layerCount = expected_count = 2;
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1), session, 1, get, end,
                device.Get(), queue.Get(), device11.Get(), ini);
            overlay.application_frame(&info);
            check(overlay.end_frame(&info, false) == XR_SUCCESS && observed_count == 2, "optional API failure broke game");
        }
        std::filesystem::remove(ini);std::filesystem::remove(xrfg::autonomous::configuration_path());std::filesystem::remove(profile);
        std::cout << "Overlay " << (d3d11 ? "D3D11" : "D3D12")
                  << " GPU colors, composition, timeout ownership, live controls, counters and lifetime passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::error_code ignored; std::filesystem::remove(ini, ignored); return 1;
    }
}
