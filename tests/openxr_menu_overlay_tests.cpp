#include "xrfg/openxr_fps_overlay.hpp"
#include "xrfg/menu_presentation.hpp"
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
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
unsigned checks{}, made{}, destroyed{}, max_layers{3}, count_seen{}, menu_seen{}, fps_seen{};
bool d11{};
bool refuse_menu_create{};
unsigned chain_limit=~0u,menu_releases{};
unsigned max_image_height=4096;bool policy_fixture{};
unsigned acquire_timeouts{};
void check(bool pass,const char* message) { ++checks; if (!pass) throw std::runtime_error(message); }
template<class T> T handle(std::uintptr_t v) { return reinterpret_cast<T>(v); }
ComPtr<ID3D12Device> device; ComPtr<ID3D12CommandQueue> queue; ComPtr<ID3D11Device> device11;
struct Chain {
    unsigned width{},height{},index{},published_index{},acquires{},waits{},releases{};
    bool timeout{true},held{},owned{};
    std::vector<ComPtr<ID3D12Resource>> t12;
    std::vector<ComPtr<ID3D11Texture2D>> t11;
};
std::map<XrSwapchain,Chain> chains;
xrfg::OverlayMenuInputFrame input;
const XrCompositionLayerBaseHeader* app{};
const void* next{};
XrPosef menu_pose{}; XrExtent2Df menu_size{};
XrResult XRAPI_CALL space(XrSession,const XrReferenceSpaceCreateInfo* i,XrSpace* out) {
    check(i->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_VIEW,"menu uses FPS VIEW space");
    *out=handle<XrSpace>(99); return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_space(XrSpace s) { check(s==handle<XrSpace>(99),"private space lifetime");return XR_SUCCESS; }
XrResult XRAPI_CALL properties(XrInstance,XrSystemId,XrSystemProperties* out) {
    out->graphicsProperties.maxSwapchainImageWidth=4096;
    out->graphicsProperties.maxSwapchainImageHeight=max_image_height;
    out->graphicsProperties.maxLayerCount=max_layers;return XR_SUCCESS;
}
XrResult XRAPI_CALL formats(XrSession,std::uint32_t capacity,std::uint32_t* count,std::int64_t* out) {
    *count=1;if(capacity)out[0]=DXGI_FORMAT_R8G8B8A8_UNORM;return XR_SUCCESS;
}
XrResult XRAPI_CALL create_chain(XrSession,const XrSwapchainCreateInfo* i,XrSwapchain* out) {
    if(chains.size()>=chain_limit || (i->width==1024 && (i->height==768||i->height==1280) && refuse_menu_create)) {
        refuse_menu_create=false;*out=XR_NULL_HANDLE;return XR_ERROR_LIMIT_REACHED;
    }
    *out=handle<XrSwapchain>(++made+10); auto& c=chains[*out]; c.width=i->width;c.height=i->height;
    c.timeout=!policy_fixture;
    check(i->arraySize==1&&i->sampleCount==1,"single image/single sample");
    if(d11) {
        c.t11.resize(3);D3D11_TEXTURE2D_DESC d{};d.Width=c.width;d.Height=c.height;
        d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        for(auto& t:c.t11)check(SUCCEEDED(device11->CreateTexture2D(&d,nullptr,&t)),"XR D11 image");
    }else {
        c.t12.resize(3);D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=c.width;d.Height=c.height;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for(auto& t:c.t12)check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,
            D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&t))),"XR D12 image");
    }return XR_SUCCESS;
}
XrResult XRAPI_CALL destroy_chain(XrSwapchain s) {check(!chains.at(s).held,"destroy only after returning the shared XR image");check(chains.erase(s)==1,"owned chain destruction");++destroyed;return XR_SUCCESS;}
XrResult XRAPI_CALL images(XrSwapchain s,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* out) {
    auto& c=chains.at(s);*count=3;
    if(capacity)for(unsigned n=0;n<3;++n) {
        if(d11)reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)[n].texture=c.t11[n].Get();
        else reinterpret_cast<XrSwapchainImageD3D12KHR*>(out)[n].texture=c.t12[n].Get();
    }return XR_SUCCESS;
}
XrResult XRAPI_CALL acquire(XrSwapchain s,const XrSwapchainImageAcquireInfo*,std::uint32_t* out) {
    if(acquire_timeouts){--acquire_timeouts;return XR_TIMEOUT_EXPIRED;}
    auto& c=chains.at(s);check(!c.held,"shared atlas cannot acquire the same XR chain twice");
    c.held=true;c.owned=false;*out=c.index=c.acquires++%3;return XR_SUCCESS;
}
XrResult XRAPI_CALL wait(XrSwapchain s,const XrSwapchainImageWaitInfo* i) {
    check(i->timeout==0,"menu wait remains nonblocking");auto& c=chains.at(s);++c.waits;
    check(c.held,"wait only after a successful shared acquire");
    if(c.timeout){c.timeout=false;return XR_TIMEOUT_EXPIRED;}c.owned=true;return XR_SUCCESS;
}
XrResult XRAPI_CALL release(XrSwapchain s,const XrSwapchainImageReleaseInfo*) {
    auto& c=chains.at(s);check(c.held&&c.owned,"release only a successfully waited shared image");
    c.held=c.owned=false;c.published_index=c.index;++c.releases;if(c.height>=768)++menu_releases;return XR_SUCCESS;
}
XrResult XRAPI_CALL end(XrSession,const XrFrameEndInfo* i) {
    check(i->next==next,"preserve endframe next chain");count_seen=i->layerCount;menu_seen=fps_seen=0;
    for(unsigned n=0;n<i->layerCount;++n) {
        if(i->layers[n]->type!=XR_TYPE_COMPOSITION_LAYER_QUAD){check(i->layers[n]==app,"preserve app layer identity");continue;}
        auto* q=reinterpret_cast<const XrCompositionLayerQuad*>(i->layers[n]);auto& c=chains.at(q->subImage.swapchain);
        check(c.releases!=0,"only released XR images composed");
        check(q->eyeVisibility==XR_EYE_VISIBILITY_BOTH&&q->space==handle<XrSpace>(99),"both eyes in shared VIEW space");
        check(q->subImage.imageRect.offset.y+q->subImage.imageRect.extent.height<=int(c.height),"UI region inside shared image");
        if(q->subImage.imageRect.offset.y==0 && q->subImage.imageRect.extent.height==768) {
            ++menu_seen;check(q->subImage.imageRect.extent.width==1024,"menu canvas 4:3");menu_pose=q->pose;menu_size=q->size;
            check(q->layerFlags==XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,"premultiplied menu alpha");
        }else ++fps_seen;
    }return XR_SUCCESS;
}
XrResult XRAPI_CALL get(XrInstance,const char* name,PFN_xrVoidFunction* out) {
#define FN(n,f) if(std::strcmp(name,n)==0){*out=reinterpret_cast<PFN_xrVoidFunction>(f);return XR_SUCCESS;}
    FN("xrCreateReferenceSpace",space) FN("xrDestroySpace",destroy_space) FN("xrGetSystemProperties",properties)
    FN("xrEnumerateSwapchainFormats",formats) FN("xrCreateSwapchain",create_chain) FN("xrDestroySwapchain",destroy_chain)
    FN("xrEnumerateSwapchainImages",images) FN("xrAcquireSwapchainImage",acquire) FN("xrWaitSwapchainImage",wait)
    FN("xrReleaseSwapchainImage",release)
#undef FN
    *out=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
}
xrfg::OverlayMenuInputFrame poll(void*) {return input;}
void position(const std::filesystem::path& path,const char* value) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);out<<"[overlay]\nposition="<<value<<'\n';
}
void bitmap(const Chain& chain, const std::filesystem::path& output, const unsigned char* data, std::size_t pitch) {
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);
    header.bfSize=header.bfOffBits+chain.width*chain.height*4;
    BITMAPINFOHEADER info{};info.biSize=sizeof(info);info.biWidth=chain.width;info.biHeight=-LONG(chain.height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;info.biSizeImage=chain.width*chain.height*4;
    std::ofstream out(output,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header),sizeof(header));out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    std::vector<unsigned char> row(chain.width*4); unsigned visible{};
    for(unsigned y=0;y<chain.height;++y) {
        const auto* source=data+y*pitch;
        for(unsigned x=0;x<chain.width;++x) {row[x*4]=source[x*4+2];row[x*4+1]=source[x*4+1];row[x*4+2]=source[x*4];row[x*4+3]=source[x*4+3];visible+=source[x*4]>100;}
        out.write(reinterpret_cast<const char*>(row.data()),row.size());
    }
    check(bool(out) && visible>1000,"real GPU menu contains rendered text and controls");
}
void preview12(const Chain& chain,const std::filesystem::path& output) {
    // Test-only readback. Runtime UI never adds this list or fence wait.
    const auto desc=chain.t12[chain.published_index]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
    device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))),"D12 readback resource");
    check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)))&&
        SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))),"D12 readback commands");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={chain.t12[chain.published_index].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=chain.t12[chain.published_index].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
    check(SUCCEEDED(list->Close())&&SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))),"D12 readback fence");
    ID3D12CommandList* commands[]{list.Get()};queue->ExecuteCommandLists(1,commands);check(SUCCEEDED(queue->Signal(fence.Get(),1)),"D12 readback signal");
    const auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);check(event!=nullptr,"D12 readback event");
    check(SUCCEEDED(fence->SetEventOnCompletion(1,event))&&WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"D12 readback completed");CloseHandle(event);
    void* mapped{};D3D12_RANGE range{0,SIZE_T(bytes)};check(SUCCEEDED(readback->Map(0,&range,&mapped)),"D12 actual menu readback");
    bitmap(chain,output,static_cast<const unsigned char*>(mapped)+footprint.Offset,footprint.Footprint.RowPitch);
    D3D12_RANGE empty{};readback->Unmap(0,&empty);
}
void preview11(const Chain& chain,const std::filesystem::path& output) {
    D3D11_TEXTURE2D_DESC desc{};chain.t11[chain.published_index]->GetDesc(&desc);
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(device11->CreateTexture2D(&desc,nullptr,&staging)),"menu preview staging");
    device11->GetImmediateContext(&context);context->CopyResource(staging.Get(),chain.t11[chain.published_index].Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};check(SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)),"menu preview real GPU readback");
    bitmap(chain,output,static_cast<const unsigned char*>(mapped.pData),mapped.RowPitch);
    context->Unmap(staging.Get(),0);
}
}
#include "overlay_atlas_policy_fixture.inc"
int main(int argc,char** argv) {
    auto fixture=std::filesystem::temp_directory_path()/(L"ofxr-menu-xr-"+std::to_wstring(GetCurrentProcessId()));
    try {
        std::filesystem::create_directories(fixture);SetEnvironmentVariableW(L"OFXR_BRIDGE_PROFILE_DIR",fixture.c_str());
        d11=argc>1&&std::string(argv[1])=="d3d11";
        if(d11)check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
            &device11,nullptr,nullptr)),"WARP11");
        else {
            ComPtr<IDXGIFactory4> f;ComPtr<IDXGIAdapter> w;
            check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))&&SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&w))),"WARP adapter");
            check(SUCCEEDED(D3D12CreateDevice(w.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"WARP12");
            D3D12_COMMAND_QUEUE_DESC q{};check(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"queue12");
        }
        auto ini=fixture/L"ofxr_bridge.ini";position(ini,"off");
        XrCompositionLayerProjectionView v{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.fov={-.65F,.55F,.42F,-.61F};
        v.subImage.imageRect.extent={2688,2784};
        XrCompositionLayerProjection p{XR_TYPE_COMPOSITION_LAYER_PROJECTION};p.viewCount=1;p.views=&v;
        app=reinterpret_cast<XrCompositionLayerBaseHeader*>(&p);std::array<const XrCompositionLayerBaseHeader*,2> layers{app,app};
        XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.layerCount=1;frame.layers=layers.data();
        XrBaseInStructure extension{XR_TYPE_UNKNOWN};next=frame.next=&extension;
        if(argc>2 && std::string_view(argv[2])=="policy") {
            atlas_policy_fixture(ini,frame);std::filesystem::remove_all(fixture);return 0;
        }
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
                device.Get(),queue.Get(),device11.Get(),ini,true,poll);
            overlay.set_native_swapchain_count_source([]()->std::optional<unsigned>{return 12u+static_cast<unsigned>(chains.size());});
            overlay.application_frame(&frame);check(made==0,"closed menu + FPS off creates no texture");
            input.open=input.focused=input.mouse_valid=true;input.mouse_x=300;input.mouse_y=240;
            refuse_menu_create=true;
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            check(made==0&&menu_seen==0,"refused menu must release capture and preserve application frame");
            // A second explicit F10 request after budget recovery retries the
            // creation; refusal must not latch menu_attempted for this session.
            acquire_timeouts=1;
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            check(made==1&&menu_seen==0&&chains.begin()->second.waits==0&&chains.begin()->second.releases==0,
                "busy optional acquire must not wait, draw, release or publish an unacquired image");
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            check(made==1&&menu_seen==0,"positive wait timeout cannot publish menu");
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            check(menu_seen==1&&fps_seen==0&&count_seen==2,"menu works with FPS off");
            const auto menu=[&]() -> Chain& {
                for(auto& [handle,chain]:chains)if(chain.height>=768)return chain;
                throw std::runtime_error("live menu chain missing");
            };
            check(menu().acquires==1&&menu().waits==2&&menu().releases==1,"retry same acquired menu image after timeout");
            if(argc>=3) {
                auto settings=xrfg::autonomous::snapshot().desired;
                xrfg::autonomous::select_generation(settings,1,settings.backend);
                check(xrfg::autonomous::request(settings),"preview FSR selection");
                xrfg::autonomous::RuntimeStatus status{};status.sessions=status.ready=1;
                status.applied_revision=xrfg::autonomous::snapshot().revision;
                status.generation_mode=1;status.generation_state=xrfg::autonomous::GenerationState::invalid_stereo;
                status.luke_detected=status.vulkan_detected=true;status.upscaler_evaluations=1;
                status.upscaler_motion=status.upscaler_depth=true;status.upscaler_last_seen_ms=GetTickCount64();
                if(argc>3) std::strcpy(status.message,"Guide input validation failed: this eye has no matching motion/depth publication for the released image. Waiting for a complete, current stereo pair before generation can resume.");
                xrfg::autonomous::report_runtime(status);
                xrfg::InterfaceTelemetry m{};m.output_fps=90;m.game_fps=m.generated_fps=45;m.display_hz=90;
                m.rates_valid=m.generating=m.gpu_valid=m.delay_valid=m.presentation_valid=m.cpu_valid=true;
                m.gpu_render_ms=8.2F;m.ofxr_delay_ms=12.4F;m.presentation_ms=4.6F;m.cpu_render_ms=10.3F;
                if(argc>3)m.presentation_ms=-24.5F;
                m.display_source=xrfg::InterfaceSource::steamvr_display;
                xrfg::autonomous::report_interface_telemetry(m);
                overlay.application_frame(&frame);overlay.end_frame(&frame,false);
                if(d11)preview11(menu(),std::filesystem::path(argv[2]));else preview12(menu(),std::filesystem::path(argv[2]));
            }
            check(menu_size.width>0&&std::abs(menu_size.height/menu_size.width-.75F)<1e-6F,"quad matches hit-test canvas ratio");
            auto pose=menu_pose;
            std::swap(v.fov.angleDown,v.fov.angleUp);overlay.application_frame(&frame);overlay.end_frame(&frame,true);
            check(menu_seen==1&&std::abs(menu_pose.position.y-pose.position.y)<1e-6F,"storage inversion never moves menu");
            std::swap(v.fov.angleDown,v.fov.angleUp);
            position(ini,"upper_right");std::this_thread::sleep_for(std::chrono::milliseconds(260));
            chain_limit=1; // opening any counter beside the menu must need no additional XR handle
            overlay.application_frame(&frame); // FPS first acquired image times out.
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&frame);overlay.end_frame(&frame,true);
            for(unsigned n=0;n<40 && !(menu_seen&&fps_seen);++n){std::this_thread::sleep_for(std::chrono::milliseconds(2));overlay.application_frame(&frame);overlay.end_frame(&frame,true);}
            check(menu_seen==1&&fps_seen==1&&count_seen==3,"both quads on original/synthetic compositor path");
            check(chains.size()==1,"menu and corner counter share exactly one native XR swapchain");
            position(ini,"elliott_bottom");std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            for(unsigned n=0;n<40 && !(menu_seen&&fps_seen);++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            }
            check(menu_seen==1&&fps_seen==1&&chains.size()==1,"menu and Elliott meter remain independent quads on one native handle");
            const auto handles=overlay.swapchain_handles();
            check(handles && (*handles)[1] && (*handles)[1]==(*handles)[2],"inventory reports the shared native UI handle");
            if(argc>=3) {
                auto image=std::filesystem::path(argv[2]);image.replace_extension(".meter.bmp");
                if(d11)preview11(menu(),image);else preview12(menu(),image);
            }
            position(ini,"upper_right");std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            overlay.set_session_focused(false);overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            for(unsigned n=0;n<40 && !fps_seen;++n){std::this_thread::sleep_for(std::chrono::milliseconds(2));overlay.application_frame(&frame);overlay.end_frame(&frame,false);}
            check(menu_seen==0&&fps_seen==1,"focus loss removes menu even if input source still asks open");
            overlay.set_session_focused(true);overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            for(unsigned n=0;n<40 && !menu_seen;++n){overlay.application_frame(&frame);overlay.end_frame(&frame,false);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
            check(menu_seen==1,"refocused menu can reopen");
            const auto submit_menu = [&] {
                const auto before = menu_releases;
                for (unsigned attempt = 0; attempt != 40 && menu_releases == before; ++attempt) {
                    overlay.application_frame(&frame);
                    overlay.end_frame(&frame, false);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                check(menu_releases > before && menu_seen == 1, "real menu draw completes after nonblocking retries");
                input.events.clear();
            };
            for (float scale : {.5F, .75F, 1.F, 1.25F}) {
                input.open = false; overlay.application_frame(&frame); overlay.end_frame(&frame, false);
                check(menu_seen == 0, "close before changed-settings reopen");
                auto settings = xrfg::autonomous::snapshot().desired;
                settings.menu_scale = scale;
                settings.menu_offset_x = 1.F; settings.menu_offset_y = -1.F;
                check(xrfg::autonomous::request(settings), "save size and position without a new XR session");
                input.open = true; submit_menu();
                const auto expected = xrfg::calculate_menu_presentation(
                    {std::tan(v.fov.angleLeft), std::tan(v.fov.angleRight),
                     std::tan(v.fov.angleDown), std::tan(v.fov.angleUp)}, settings);
                check(std::abs(menu_size.width - expected.width) < 1e-5F &&
                    std::abs(menu_size.height - expected.height) < 1e-5F &&
                    std::abs(menu_pose.position.x - expected.x) < 1e-5F &&
                    std::abs(menu_pose.position.y - expected.y) < 1e-5F,
                    "changed size and clamped position survive the production close/reopen path");
                check(chains.size()==1 && made-destroyed==chains.size(), "reopening retains one shared native atlas");
            }
            auto settings = xrfg::autonomous::snapshot().desired;
            settings.menu_scale = .5F; settings.menu_offset_x = settings.menu_offset_y = 0;
            check(xrfg::autonomous::request(settings), "centre smaller menu for actual blank-space drag");
            // Below the last control of the default (OFXR 0.3.0) page, which
            // has a Multiplier row since V857.
            input.mouse_x = 400; input.mouse_y = 700; submit_menu();
            const auto before_drag = menu_pose; const auto drag_size = menu_size;
            input.mouse_down[0] = true;
            input.events = {{xrfg::OverlayMenuInputEventKind::mouse_button, 0, 0, 0, true}};
            submit_menu();
            input.mouse_x = 420; input.mouse_y = 712;
            input.events = {{xrfg::OverlayMenuInputEventKind::mouse_position, 420, 712}};
            submit_menu();
            check(std::abs(menu_pose.position.x - before_drag.position.x - 20 * drag_size.width / 1024) < 1e-5F &&
                std::abs(menu_pose.position.y - before_drag.position.y + 12 * drag_size.height / 768) < 1e-5F,
                "production blank-child drag moves the composed quad by exact grabbed texel distances");
            // A real producer is countershifted by offset_pointer. This narrow
            // deterministic source rebases itself to the grabbed coordinates.
            input.mouse_x = 400; input.mouse_y = 700; input.mouse_down[0] = false;
            input.events = {{xrfg::OverlayMenuInputEventKind::mouse_position, 400, 700},
                {xrfg::OverlayMenuInputEventKind::mouse_button, 0, 0, 0, false}};
            submit_menu();
            const auto saved = xrfg::autonomous::snapshot().desired;
            check(saved.menu_offset_x > 0 && saved.menu_offset_y < 0,
                "production drag release persists position through shared IPC");
            input.open=false;overlay.application_frame(&frame);overlay.end_frame(&frame,false);check(menu_seen==0,"close is immediate");
            check(!xrfg::autonomous::interface_telemetry_requested(),"menu close stops new telemetry while independent FPS remains enabled");
            const auto stamp=xrfg::autonomous::snapshot().runtime.telemetry.updated_ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(260));overlay.application_frame(&frame);
            check(xrfg::autonomous::snapshot().runtime.telemetry.updated_ms==stamp,"counter-only refresh does not publish new UI telemetry");
            if(argc>=3) {
                const auto benchmark=[&](bool open) {
                    input.open=open;overlay.application_frame(&frame);
                    std::int64_t total{};const auto before=menu_releases;
                    for(unsigned n=0;n<100;++n) {
                        const auto start=std::chrono::steady_clock::now();overlay.application_frame(&frame);
                        total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
                        static_cast<void>(overlay.end_frame(&frame,false));std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    std::cout<<"UI "<<(open?"open":"closed, counter on")<<" application_frame CPU us/frame="<<total/100000.0<<"; menu draws="<<menu_releases-before<<'\n';
                };
                benchmark(false);benchmark(true);input.open=false;overlay.application_frame(&frame);
                check(!xrfg::autonomous::interface_telemetry_requested(),"benchmark close clears visibility");
            }
            input.open=false;position(ini,"off");std::this_thread::sleep_for(std::chrono::milliseconds(260));
            for(unsigned n=0;n<40 && !chains.empty();++n) {
                overlay.application_frame(&frame);overlay.end_frame(&frame,false);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            check(chains.empty(),"menu and counter off release the shared native reservation during the live session");
            XrFrameEndInfo empty=frame;empty.layerCount=0;empty.layers=nullptr;
            overlay.application_frame(&empty);overlay.end_frame(&empty,false);check(count_seen==0,"empty application frame remains empty");
            overlay.suspend();input.open=true;overlay.application_frame(&frame);overlay.end_frame(&frame,true);
            check(count_seen==1,"disarm removes both quads terminally");
        }
        check(made==destroyed&&chains.empty(),"menu/FPS GPU+XR lifetime balanced");
        // A second runtime exposes only one spare composition layer.
        max_layers=2;position(ini,"upper_right");
        {
            xrfg::OpenXrFpsOverlay overlay(handle<XrInstance>(1),handle<XrSession>(2),1,get,end,
                device.Get(),queue.Get(),device11.Get(),ini,true,poll);
            overlay.set_native_swapchain_count_source([]()->std::optional<unsigned>{return 12u+static_cast<unsigned>(chains.size());});
            overlay.application_frame(&frame);std::this_thread::sleep_for(std::chrono::milliseconds(260));
            overlay.application_frame(&frame);overlay.end_frame(&frame,false);
            check(menu_seen==1&&fps_seen==0&&count_seen==2,"one spare slot gives interactive menu priority");
            frame.layerCount=2;overlay.end_frame(&frame,true);check(count_seen==2&&menu_seen==0,"never discard app layers for menu");
        }
        check(made==destroyed&&chains.empty(),"second session lifetime balanced");
        std::filesystem::remove(xrfg::autonomous::configuration_path());
        std::filesystem::remove(ini);std::filesystem::remove(fixture);
        std::cout<<checks<<" OpenXR menu "<<(d11?"D11":"D12")<<" composition checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
