#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/autonomous_control.hpp"
#include "../src/layer/autonomous_runtime.hpp"
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
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(bool pass, const char* message) { if (!pass) throw std::runtime_error(message); }
std::filesystem::path runtime_ini() {
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    return std::filesystem::path(exe).parent_path()/L"ofxr_bridge.ini";
}
void write_position(const std::filesystem::path& ini,const wchar_t* position) {
    // Exact desktop/F10 request -> runtime adapter -> INI writer -> receipt,
    // replacing the old fixture's direct INI write which bypassed this bug.
    check(ini==runtime_ini(),"overlay uses the real runtime adapter INI");
    constexpr const wchar_t* names[]{L"off",L"upper_left",L"upper_right",L"lower_left",L"lower_right",L"elliott_controller",L"elliott_bottom"};
    int selection=-1;for(int i=0;i<7;++i)if(std::wcscmp(position,names[i])==0)selection=i;
    check(selection>=0,"known display selection");
    const auto generation_revision=xrfg::embedded::snapshot().revision;
    auto desired=xrfg::autonomous::snapshot().desired;desired.overlay_position=selection;
    desired.overlay_enabled=selection!=0;
    check(xrfg::autonomous::request(desired),"real live display request accepted");
    xrfg::autonomous::apply_runtime_settings();
    check(xrfg::autonomous::applied_runtime_revision.load()==xrfg::autonomous::snapshot().revision,
        "display request must complete runtime application, not stick Applying settings");
    check(xrfg::embedded::overlay_setting()==selection,"real runtime display setting round trip");
    check(xrfg::embedded::snapshot().revision==generation_revision,"display-only switch must not reconfigure generation");
    xrfg::autonomous::publish_runtime_status();
    check(xrfg::autonomous::interface_state_text(xrfg::autonomous::snapshot())!="Applying settings",
        "shared desktop/headset status acknowledges new display mode");
}

template<class T> T handle(std::uintptr_t value) { return reinterpret_cast<T>(value); }
const auto session = handle<XrSession>(2);
const auto overlay_space = handle<XrSpace>(4);
const auto overlay_chain = handle<XrSwapchain>(5);
const auto local_space=handle<XrSpace>(7);
XrSpace last_quad_space{};
float controller_x=1;bool controller_tracked=true;
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> queue;
ComPtr<ID3D11Device> device11;
std::vector<ComPtr<ID3D12Resource>> textures;
std::vector<ComPtr<ID3D11Texture2D>> textures11;
unsigned created{}, destroyed{}, space_created{}, space_destroyed{}, acquires{}, waits{}, releases{}, ends{};
std::unordered_set<XrSwapchain> active_chains;
unsigned acquired_index{}, last_released{}, width{}, height{};
bool timeout_next = true, fail_end{}, unsupported{}, d3d11{};
unsigned clock_conversions{};
bool clock_available=true;
bool display_clock(void*, XrTime, std::int64_t& out) {
    ++clock_conversions;
    out=xrfg::interface_clock_ns()-5'000'000;
    return clock_available;
}
const XrCompositionLayerBaseHeader* expected_layer{};
const void* expected_next{};
unsigned expected_count{1}, observed_count{};
XrPosef last_pose{};
XrResult XRAPI_CALL create_space(XrSession, const XrReferenceSpaceCreateInfo* info, XrSpace* output) {
    check((info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW || info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) && info->poseInReferenceSpace.orientation.w == 1,
          "overlay must own a VIEW space");
    ++space_created; *output = info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW?overlay_space:local_space; return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace space) {
    check((space == overlay_space || space==local_space), "destroyed app space"); ++space_destroyed; return XR_SUCCESS;
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
    ++created; *output = handle<XrSwapchain>(5+created-1);active_chains.insert(*output);return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_chain(XrSwapchain chain) {
    check(active_chains.erase(chain)==1, "destroyed app, unknown or already retired swapchain"); ++destroyed; return XR_SUCCESS;
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
        check(quad->type == XR_TYPE_COMPOSITION_LAYER_QUAD && (quad->space == overlay_space || quad->space==local_space) &&
            active_chains.contains(quad->subImage.swapchain) && quad->eyeVisibility == XR_EYE_VISIBILITY_BOTH,
            "invalid overlay quad");
        check(releases > 0, "unreleased overlay image");
        check(quad->layerFlags == XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
              "overlay must blend premultiplied source alpha");
        check(width==height*2 && quad->subImage.imageRect.offset.y==0,
            "non-SteamVR counter retains its direct native surface");
        last_pose = quad->pose;last_quad_space=quad->space;
    }
    if (fail_end) { fail_end = false; return XR_ERROR_RUNTIME_FAILURE; }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL locate(XrSpace grip,XrSpace local,XrTime,XrSpaceLocation* out){
    check(local==local_space,"controller read in LOCAL");
    out->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if(controller_tracked)out->locationFlags|=XR_SPACE_LOCATION_POSITION_TRACKED_BIT|XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
    out->pose.orientation={0,0,1,0};out->pose.position={controller_x,1,0};return XR_SUCCESS;
}
XrResult XRAPI_CALL get(XrInstance, const char* name, PFN_xrVoidFunction* output) {
    *output = nullptr;
    if (unsupported) return XR_ERROR_FUNCTION_UNSUPPORTED;
#define FN(n, f) if (std::strcmp(name, n) == 0) { *output = reinterpret_cast<PFN_xrVoidFunction>(f); return XR_SUCCESS; }
    FN("xrCreateReferenceSpace", create_space) FN("xrDestroySpace", destroy_space)
    FN("xrCreateSwapchain", create_chain) FN("xrDestroySwapchain", destroy_chain)
    FN("xrEnumerateSwapchainFormats", formats) FN("xrEnumerateSwapchainImages", enumerate)
    FN("xrAcquireSwapchainImage", acquire) FN("xrWaitSwapchainImage", wait) FN("xrReleaseSwapchainImage", release)
    FN("xrGetSystemProperties", properties) FN("xrLocateSpace",locate)
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
    const auto ini=runtime_ini();
    const bool ini_existed=std::filesystem::exists(ini);
    std::ifstream old_ini(ini,std::ios::binary);
    const std::string ini_backup{std::istreambuf_iterator<char>(old_ini),std::istreambuf_iterator<char>()};
    struct RestoreIni {std::filesystem::path path;bool existed;std::string bytes;
        ~RestoreIni(){if(existed){std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<bytes;}
            else {std::error_code ec;std::filesystem::remove(path,ec);}
            WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());}}
        restore_ini{ini,ini_existed,ini_backup};
    const auto profile=std::filesystem::temp_directory_path()/(L"ofxr-overlay-profile-"+std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(profile);SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",profile.c_str());
        auto initial=xrfg::autonomous::snapshot().desired;initial.overlay_position=2;
        check(xrfg::autonomous::request(initial),"initial classic settings");
        xrfg::autonomous::apply_runtime_settings();
        check(xrfg::autonomous::applied_runtime_revision.load()==xrfg::autonomous::snapshot().revision,"initial classic applied");
        check(!xrfg::embedded::set_overlay_setting(-1) && !xrfg::embedded::set_overlay_setting(7),"invalid position rejected");
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

        write_position(ini,L"elliott_bottom");timeout_next=true;
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),session,1,get,end,
                device.Get(),queue.Get(),device11.Get(),ini,false,nullptr,nullptr,display_clock);
            overlay.set_display_period(10'000'000);
            overlay.set_grip_spaces(handle<XrSpace>(11),handle<XrSpace>(12));
            overlay.application_frame(&info);
            check(waits==1&&releases==0,"meter timeout cannot release unowned image");
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==1,"timed out meter stays hidden");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==2,"bottom meter uploaded");
            check(created==1 && width==1024&&height==512&&last_pose.position.y==-.30F&&last_pose.position.z==-1.F,"upstream bottom placement on direct surface");
            check(pixel_count(0)>1024*100,"meter cropped lower rows transparent on GPU");
            const auto generation_before_toggle=xrfg::embedded::snapshot().revision;
            const auto toggle_counter=[&](bool enabled) {
                xrfg::autonomous::apply_toggle_shortcuts(false,false,true);
                xrfg::autonomous::apply_runtime_settings();
                const auto selected=xrfg::autonomous::snapshot().desired;
                check(selected.overlay_enabled==enabled && selected.overlay_position==6,"F6 action preserves selected bottom meter");
                check(xrfg::embedded::snapshot().revision==generation_before_toggle,"counter shortcut cannot rebuild generation");
                std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
                check(overlay.end_frame(&info,false)==XR_SUCCESS && observed_count==(enabled?2:1),
                    "counter shortcut hides/restores real OpenXR meter submission");
            };
            toggle_counter(false);toggle_counter(true);
            check(!overlay.marker_placement(),"panel is not corner diagnostic marker");
            xrfg::InterfaceFrameStamp meter_stamp{471,xrfg::interface_clock_ns(),0};
            check(overlay.end_frame(&info,false,true,{},0,meter_stamp)==XR_SUCCESS&&clock_conversions>0,
                "visible meter enables existing timing counter without menu");
            write_position(ini,L"elliott_controller");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==1,"flip show delay");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==2&&last_quad_space==local_space,"controller mode appears");
            const auto x=last_pose.position.x;controller_x+=.2F;
            check(overlay.end_frame(&info,true)==XR_SUCCESS,"synthetic meter submission");
            check(last_pose.position.x>x+.19F,"meter follows controller on synthetic submission without repaint");
            controller_tracked=false;check(overlay.end_frame(&info,false)==XR_SUCCESS,"untracked submission");
            check(observed_count==1,"lost tracking must hide, not freeze last pose");
            controller_tracked=true;
            overlay.set_session_focused(false);check(overlay.end_frame(&info,false)==XR_SUCCESS,"unfocused submission");
            check(observed_count==1,"unfocused meter hidden");overlay.set_session_focused(true);
            write_position(ini,L"upper_right");
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==2&&last_quad_space==overlay_space&&last_pose.position.x>0,"switch back to original corner");
            write_position(ini,L"off");std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==1,"off removes both meter types");
            check(!xrfg::autonomous::interface_telemetry_requested(),"meter lease independent of closed menu and cleared on off");
            write_position(ini,L"elliott_bottom");std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&info);
            info.layerCount=expected_count=2;
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==2,"layer budget preserves application layers");
            info.layerCount=expected_count=1;overlay.suspend();overlay.application_frame(&info);
            check(overlay.end_frame(&info,false)==XR_SUCCESS&&observed_count==1,"suspend removes meter");
        }
        check(created==destroyed&&space_created==space_destroyed,"meter resource lifetime");
        std::filesystem::remove(xrfg::autonomous::configuration_path());
        std::filesystem::remove(profile/L"Profiles");
        std::filesystem::remove(profile/L"ofxr-bridge.ini");
        std::filesystem::remove(profile);
        std::cout<<"Elliott meter GPU upload, live switching, controller tracking, ownership and original counters passed\n";return 0;
    } catch (const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
