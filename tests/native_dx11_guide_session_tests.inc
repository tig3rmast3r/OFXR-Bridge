// Exercise production session creation/release with actual native devices.
#include "xrfg/d3d11_guide_transport.hpp"
LUID native_guide_adapter{};
ID3D11Texture2D* native_sr_image{};
void native_dx11_guide_session_test() {
    std::cerr<<"DX11 guide session: create source device\n"<<std::flush;
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> source;ComPtr<ID3D11DeviceContext> context;
    check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,0,D3D11_SDK_VERSION,&source,nullptr,&context)),"session DX11 device");
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC adapter_desc{};
    check(SUCCEEDED(source.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&adapter_desc)),"session adapter");native_guide_adapter=adapter_desc.AdapterLuid;
    for (bool converted : {false,true}) {
        std::cerr<<"DX11 guide session: create session converted="<<converted<<'\n'<<std::flush;
        auto dispatch=std::make_shared<Dispatch>();dispatch->d3d11_bridge=converted;
        // This fixture isolates native/converted guide ownership and supplies
        // only the original runtime's image/release calls. Select that actual
        // Legacy runtime contract explicitly; complete private SteamVR endpoint
        // creation and image ownership are covered by private-runtime fixtures.
        dispatch->virtual_desktop_runtime=true;dispatch->inline_unless_pipelined=true;
        dispatch->runtime_name="Virtual Desktop";
        dispatch->get_instance_proc_addr=[](XrInstance,const char*,PFN_xrVoidFunction* function)->XrResult {
            *function=nullptr;return XR_ERROR_FUNCTION_UNSUPPORTED;
        };
        dispatch->get_d3d12_graphics_requirements=[](XrInstance,XrSystemId,XrGraphicsRequirementsD3D12KHR* requirements)->XrResult {
            requirements->adapterLuid=native_guide_adapter;requirements->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;
        };
        dispatch->create_session=[](XrInstance,const XrSessionCreateInfo*,XrSession* handle)->XrResult {
            *handle=reinterpret_cast<XrSession>(0x33029);return XR_SUCCESS;
        };
        dispatch->destroy_session=[](XrSession)->XrResult {return XR_SUCCESS;};
        dispatch->release_swapchain_image=[](XrSwapchain,const XrSwapchainImageReleaseInfo*)->XrResult {return XR_SUCCESS;};
        dispatch->enumerate_swapchain_images=[](XrSwapchain,std::uint32_t capacity,std::uint32_t* count,XrSwapchainImageBaseHeader* images)->XrResult {
            *count=1;
            if (capacity && images) reinterpret_cast<XrSwapchainImageD3D11KHR*>(images)[0].texture=native_sr_image;
            return XR_SUCCESS;
        };
        const auto instance=reinterpret_cast<XrInstance>(0x33029);
        {std::scoped_lock lock(g_state_mutex);g_instances[instance]=dispatch;}
        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};binding.device=source.Get();
        XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};info.next=&binding;info.systemId=1;
        XrSession handle{};check(XR_SUCCEEDED(layer_create_session_impl(instance,&info,&handle)),"actual session creation");
        std::cerr<<"DX11 guide session: session ready\n"<<std::flush;
        auto state=find_session(handle);check(state && bool(state->d3d11_bridge)==converted,"native/converted session path");
        ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;std::uint64_t owner{};
        check(SUCCEEDED(xrfg::acquire_d3d11_guide_owner(source.Get(),&device,&queue,&owner)),"session registers original application device");
        check(device.Get()==state->d3d12_device.Get() && queue.Get()==state->d3d12_queue.Get(),"actual session queue/device identity");
        check(xrfg::claim_d3d11_guides(source.Get(),owner),"guide owner claim");
        ComPtr<ID3D11Texture2D> native_image;
        if (!converted) {
            D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.ArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            check(SUCCEEDED(source->CreateTexture2D(&desc,nullptr,&native_image)),"native runtime XR eye");native_sr_image=native_image.Get();
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=16;
            ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            auto chain=std::make_shared<SwapchainState>(state,ci);chain->handle=reinterpret_cast<XrSwapchain>(0x330310);
            {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
            std::uint32_t count{};XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
            check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(chain->handle,0,&count,nullptr)),"native count-only enumeration");
            check(!xrfg::is_d3d11_sr_destination(native_image.Get()),"count query does not register guessed images");
            check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(chain->handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image))),"actual native image enumeration");
            check(xrfg::is_d3d11_sr_destination(native_image.Get()),"actual native enumerated eye registered for SR");
            {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(chain->handle);}
        }
        if (converted) {
            state->menu_enabled=true;state->dlss_motion_vectors=true;
            xrfg::configure_dlss_motion_vector_tracking(true);
            for (unsigned eye : {1u,0u}) {
                std::cerr<<"DX11 guide session: converted eye="<<eye<<" create bridge\n"<<std::flush;
                XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};ci.width=ci.height=8;
                ci.arraySize=ci.faceCount=ci.mipCount=ci.sampleCount=1;ci.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                ci.format=DXGI_FORMAT_R16G16B16A16_FLOAT;
                D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=8;
                desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=static_cast<DXGI_FORMAT>(ci.format);
                desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> runtime_image;
                check(SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&runtime_image))),"runtime color");
                auto chain=std::make_shared<SwapchainState>(state,ci);chain->handle=reinterpret_cast<XrSwapchain>(std::uintptr_t{0x330290}+eye);
                chain->dlss_logical_eye_count=2;chain->dlss_logical_eye_index=eye;chain->bridge=std::make_shared<xrfg::D3D11BridgeSwapchain>();
                ID3D12Resource* runtime_images[]{runtime_image.Get()};xrfg::D3D11BridgeSwapchainDescription requested{};
                requested.requested_format=static_cast<DXGI_FORMAT>(ci.format);requested.requested_sample_count=1;std::uint32_t failure_stage{};
                check(SUCCEEDED(chain->bridge->initialize(source.Get(),context.Get(),device.Get(),queue.Get(),runtime_images,requested,&failure_stage)),"actual application bridge");
                auto* actual_eye=chain->bridge->d3d11_images()[0];
                {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
                std::uint32_t count{};XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
                check(XR_SUCCEEDED(layer_enumerate_swapchain_images_impl(chain->handle,1,&count,reinterpret_cast<XrSwapchainImageBaseHeader*>(&image))),"actual converted image enumeration");
                check(image.texture==actual_eye && xrfg::is_d3d11_sr_destination(actual_eye),"only actual application-visible converted eye registered for SR");
                check(chain->d3d12_history && chain->d3d12_history->initialized(),"actual enumerated history");
                std::cerr<<"DX11 guide session: converted history ready\n"<<std::flush;
                xrfg::D3D11GuideTransport transport;
                check(SUCCEEDED(transport.initialize(context.Get(),device.Get(),queue.Get())) && SUCCEEDED(transport.begin()),"guide transport");
                const auto make_input=[&](DXGI_FORMAT format) {
                    D3D11_TEXTURE2D_DESC input{};input.Width=input.Height=8;input.ArraySize=input.MipLevels=1;input.SampleDesc.Count=1;
                    input.Format=format;input.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
                    ComPtr<ID3D11Texture2D> texture;check(SUCCEEDED(source->CreateTexture2D(&input,nullptr,&texture)),"guide input");return texture;
                };
                auto mv=make_input(DXGI_FORMAT_R16G16_FLOAT),depth=make_input(DXGI_FORMAT_R32_FLOAT);
                check(SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::motion,mv.Get())) &&
                    SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::depth,depth.Get())) &&
                    SUCCEEDED(transport.copy(xrfg::D3D11GuideTransport::output,actual_eye)),"owned guide GPU copies");
                xrfg::DlssMotionVectorPublication publication{};publication.stream=330290+eye;publication.direct_output_receipt=true;
                publication.output=transport.resource(xrfg::D3D11GuideTransport::output);publication.motion_vectors=transport.resource(xrfg::D3D11GuideTransport::motion);
                publication.depth=transport.resource(xrfg::D3D11GuideTransport::depth);publication.producer_command_list=transport.list();publication.producer_queue=queue.Get();publication.verified_producer_device=device.Get();
                publication.output_width=publication.output_height=publication.motion_width=publication.motion_height=publication.depth_width=publication.depth_height=8;
                publication.scale_x=publication.scale_y=1;publication.camera_near=.1f;publication.camera_far=100;
                publication.resource_state=publication.depth_resource_state=D3D12_RESOURCE_STATE_COMMON;
                auto frame=xrfg::publish_dlss_motion_vectors(publication);check(bool(frame),"guide publication");
                std::cerr<<"DX11 guide session: submit producer\n"<<std::flush;
                check(SUCCEEDED(transport.submit()),"actual producer submission");
                xrfg::set_d3d11_guide_receipt(actual_eye,{owner,frame->stream,frame->publication,8,8});
                {std::scoped_lock lock(g_state_mutex);g_swapchains[chain->handle]=chain;}
                const auto release=[&] {
                    chain->ownership_tracking_valid=true;chain->acquired_indices.push_back(0);chain->waited_count=1;
                    check(XR_SUCCEEDED(layer_release_swapchain_image_impl(chain->handle,nullptr)),"actual converted release");
                };
                release();auto resolved=chain->last_released_motion_vectors;
                std::cerr<<"DX11 guide session: first release complete\n"<<std::flush;
                check(resolved && resolved->eye_count==1 && resolved->eyes[0] && resolved->eyes[0]->publication==frame->publication &&
                    resolved->eyes[0]->stream==publication.stream,"actual eye keeps exact publication including right-eye index 1");
                xrfg::clear_d3d11_guide_receipt(actual_eye);release();check(!chain->last_released_motion_vectors,"missing receipt never borrows a guide");
                std::cerr<<"DX11 guide session: drain exact GPU owners\n"<<std::flush;
                check(SUCCEEDED(transport.drain()) && SUCCEEDED(chain->d3d12_history->wait_for_idle()) &&
                    SUCCEEDED(chain->bridge->wait_for_idle()),"real GPU completion");
                xrfg::retire_dlss_motion_vector_stream(publication.stream);
                {std::scoped_lock lock(g_state_mutex);g_swapchains.erase(chain->handle);}
            }
        }
        std::cerr<<"DX11 guide session: destroy session\n"<<std::flush;
        check(XR_SUCCEEDED(layer_destroy_session_impl(handle)),"session destruction");
        check(!xrfg::has_d3d11_sr_destinations(source.Get()),"session destruction retires all SR destinations");
        check(FAILED(xrfg::acquire_d3d11_guide_owner(source.Get(),device.ReleaseAndGetAddressOf(),queue.ReleaseAndGetAddressOf(),&owner)),"destroyed owner retired");
        {std::scoped_lock lock(g_state_mutex);g_instances.erase(instance);}
    }
    std::cout<<"Actual native/converted DX11 session ownership and two-eye release receipts PASS\n";
}
