// Exercise the shipping recorder with a private session, without sending
// keyboard input or adding a test-only control to the distributed layer.
#include "../src/common/two_frame_capture.cpp"
#include "xrfg/d3d11_d3d12_interop.hpp"
#include "xrfg/d3d11_bridge.hpp"
#include "xrfg/luke_origin_loss_capture.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void success(HRESULT value, const char* message) { require(SUCCEEDED(value), message); }
}

int main() {
    using Microsoft::WRL::ComPtr;
    try {
        xrfg::LukeOriginLossCapture loss;
        xrfg::LukeOriginLossCapture late;
        late.set_logging(true);
        require(late.observe(false,true,1),"F9 enabled during existing N/A must capture without earlier healthy sample");
        late.captured(1);late.set_logging(true);
        require(!late.observe(false,true,2),"same F9 window must stay bounded");
        late.set_logging(false);late.set_logging(true);
        require(late.observe(false,true,3),"new explicit F9 window must rearm persistent N/A");
        late.captured(3);
        require(!late.observe(true,true,4),"bonus-admitted source must not trigger unavailable capture");
        for(unsigned i=0;i<100;++i)require(!loss.observe(false,true,i*1000),"startup must not trigger loss capture");
        require(!loss.observe(true,true,100000),"valid selection triggered diagnostic");
        require(loss.observe(false,true,100001),"first missing marker was not captured immediately");
        loss.captured(100001);
        for(unsigned i=0;i<20;++i)require(!loss.observe(false,true,100002+i*10),"same effect triggered repeated captures");
        require(!loss.observe(true,true,100200),"recovery did not reset missing streak");
        for(unsigned i=0;i<20;++i)require(!loss.observe(false,true,100201+i*10),"short recovery rearmed capture");
        require(!loss.observe(false,false,100399),"black source triggered diagnostic");
        require(!loss.observe(false,true,100500),"same episode rearmed without genuine recovery");
        require(!loss.observe(true,true,109999),"recovery triggered capture");
        require(!loss.observe(true,true,110999),"rearm triggered capture");
        require(loss.observe(false,true,111000),"next effect did not rearm after stable recovery/cooldown");
        loss.captured(111000);
        require(!loss.observe(false,true,211000),"persistent missing marker retriggered without recovery");
        require(!loss.observe(false,false,212000)&&loss.missing==0,"black source threshold not cancelled");
        require(!loss.observe(true,true,212100),"recovery triggered capture");
        require(!loss.observe(true,true,212600),"brief valid marker triggered capture");
        require(!loss.observe(false,true,212700),"less than 1s recovery rearmed capture");
        require(!loss.observe(true,true,220000),"recovery triggered capture");
        require(!loss.observe(true,true,221100),"recovery triggered capture");
        require(loss.observe(false,true,221200),"second later episode failed to rearm");
        loss.captured(221200);
        require(!loss.observe(true,true,300000)&&!loss.observe(true,true,301100),"fourth episode rearm triggered capture");
        require(loss.observe(false,true,301200),"fourth independent effect was not captured");loss.captured(301200);
        require(!loss.observe(true,true,320000)&&!loss.observe(true,true,321100)&&!loss.observe(false,true,321200),
            "automatic witness limit must bound disk use without touching manual F8");
        require(!xrfg::two_frame_capture_request_origin_loss(),"automatic capture enabled with F9 OFF");
        ComPtr<ID3D11Device> d11;
        ComPtr<ID3D11DeviceContext> context;
        success(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_SINGLETHREADED, nullptr, 0, D3D11_SDK_VERSION,
            &d11, nullptr, &context), "D11 device");
        ComPtr<ID3D12Device> d12;
        ComPtr<ID3D12CommandQueue> queue;
        success(xrfg::create_d3d12_device_for_d3d11(d11.Get(), &d12, &queue), "matching D12 device");
        D3D11_TEXTURE2D_DESC description{};
        description.Width = 256; description.Height = 64;
        description.ArraySize = 2; description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> source, current, synthetic;
        success(d11->CreateTexture2D(&description, nullptr, &source), "source");
        success(d11->CreateTexture2D(&description, nullptr, &current), "current");
        success(d11->CreateTexture2D(&description, nullptr, &synthetic), "synthetic");
        ID3D11Texture2D* sources[]{source.Get()};
        ID3D11Texture2D* currents[]{current.Get()};
        ID3D11Texture2D* synthetics[]{synthetic.Get()};
        xrfg::D3D11D3D12SwapchainInterop bridge;
        success(bridge.initialize(d11.Get(), context.Get(), d12.Get(), queue.Get(),
            sources, currents, synthetics), "interop");
        const auto mirror = bridge.current_destination_images()[0];
        HANDLE shared{};
        success(d12->CreateSharedHandle(mirror, nullptr, GENERIC_ALL, nullptr, &shared), "mirror share");
        ComPtr<ID3D11Device1> device1;
        ComPtr<ID3D11Texture2D> alias;
        success(d11.As(&device1), "device1");
        const HRESULT opened = device1->OpenSharedResource1(shared, IID_PPV_ARGS(&alias));
        CloseHandle(shared);
        success(opened, "mirror alias");
        const std::array<std::uint32_t, 2> expected{0xff13579bu, 0xff2468acu};
        std::vector<std::uint32_t> pixels(description.Width * description.Height);
        for (unsigned eye = 0; eye < 2; ++eye) {
            std::fill(pixels.begin(), pixels.end(), expected[eye]);
            context->UpdateSubresource(alias.Get(), eye, nullptr, pixels.data(), description.Width * 4, 0);
        }
        // The ordinary source handshake orders preceding D11 alias writes.
        success(bridge.prepare_capture(0), "seed order");
        xrfg::D3D11PublicationReceipt receipt;
        success(bridge.publish(0, std::nullopt, &receipt), "publish exact XR image");
        success(xrfg::wait_for_d3d11_publication(receipt), "publication completion");

        xrfg::TwoFrameImageInfo info{};
        info.stage = "afw_xr_terminal_eye";
        info.capture_serial = 933;
        info.stream = 1234;
        info.swapchain = 5678;
        info.display_time = 9012;
        info.width = description.Width; info.height = description.Height;
        const auto before = bridge.publication_snapshot();
        require(bridge.diagnostic_capture_published_current(receipt, info) == S_FALSE,
            "inactive recorder must bypass");
        const auto after = bridge.publication_snapshot();
        require(before.ready_value == after.ready_value &&
            before.copy_complete_value == after.copy_complete_value,
            "inactive capture advanced publication");
        xrfg::diagnostic_ring_enabled = true;
        require(!xrfg::two_frame_capture_images_active() &&
            bridge.diagnostic_capture_published_current(receipt, info) == S_FALSE,
            "F9 ring must not enable full-resolution F8 copies");
        xrfg::diagnostic_ring_enabled = false;

        auto& manager = xrfg::manager();
        manager.session = std::make_shared<xrfg::Session>();
        manager.active_frame = 1;
        auto session = manager.session;
        require(xrfg::two_frame_capture_claim_dlssg_pair(51), "first native F8 stream");
        require(!xrfg::two_frame_capture_claim_dlssg_pair(51), "duplicate native F8 stream");
        require(xrfg::two_frame_capture_claim_dlssg_pair(52), "second native F8 stream");
        require(!xrfg::two_frame_capture_claim_dlssg_pair(53), "native F8 stream bound");
        // Poison only the D12 mirror after XR publication. A mirror-only
        // witness would now return the wrong bytes for both eyes.
        std::fill(pixels.begin(), pixels.end(), 0xffeeffffu);
        for (unsigned eye = 0; eye < 2; ++eye)
            context->UpdateSubresource(alias.Get(), eye, nullptr, pixels.data(), description.Width * 4, 0);
        for (unsigned eye = 0; eye < 2; ++eye) {
            info.eye = static_cast<int>(eye); info.slice = eye;
            success(bridge.diagnostic_capture_published_current(receipt, info), "terminal recorder");
        }
        success(bridge.wait_for_idle(), "diagnostic retirement");
        require(session->shots.size() == 2, "two exact eye shots");
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& shot = *session->shots[eye];
            require(shot.owned_allocator && shot.producer_list && shot.fence &&
                xrfg::completed(shot), "retained submitted allocator/list/fence");
            require(shot.info.capture_serial == 933 && shot.info.eye == static_cast<int>(eye) &&
                shot.info.swapchain == 5678 && shot.info.display_time == 9012,
                "terminal identity changed");
            void* data{};
            success(shot.readback->Map(0, nullptr, &data), "readback map");
            bool exact = true;
            for (UINT y = 0; y < description.Height; ++y) {
                const auto* row = reinterpret_cast<const std::uint32_t*>(
                    static_cast<const unsigned char*>(data) + shot.footprint.Offset +
                    y * shot.footprint.Footprint.RowPitch);
                for (UINT x = 0; x < description.Width; ++x) exact &= row[x] == expected[eye];
            }
            shot.readback->Unmap(0, nullptr);
            require(exact, "witness read poisoned mirror instead of actual XR publication");
        }
        // Exhaustion must fail the diagnostic without changing XR pixels or
        // its immutable publication receipt; the final retirement still runs.
        session->allocated = xrfg::kMaximumBytes;
        require(bridge.diagnostic_capture_published_current(receipt, info) == E_OUTOFMEMORY,
            "bounded recorder budget");
        success(bridge.wait_for_idle(), "failed diagnostic retirement");
        require(receipt.copy_complete_value == before.copy_complete_value,
            "diagnostic rewrote publication receipt");
        manager.active_frame = 0;
        require(!xrfg::two_frame_capture_claim_dlssg_pair(54), "inactive native F8 claim");
        manager.session.reset();
        // Snapshot the application's original D11 shared image, not the
        // history mirror or the downstream runtime texture. Poison the latter
        // explicitly before taking the diagnostic to expose a wrong source.
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rt{};rt.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt.Width=description.Width;rt.Height=description.Height;rt.DepthOrArraySize=2;rt.MipLevels=1;
        rt.SampleDesc.Count=1;rt.Format=DXGI_FORMAT_B8G8R8A8_UNORM;rt.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> runtime_image;
        success(d12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&rt,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&runtime_image)),"original fixture runtime texture");
        xrfg::D3D11BridgeSwapchain original;
        ID3D12Resource* runtime_images[]{runtime_image.Get()};
        xrfg::D3D11BridgeSwapchainDescription request{};request.requested_format=rt.Format;
        std::uint32_t failure{};
        success(original.initialize(d11.Get(),context.Get(),d12.Get(),queue.Get(),runtime_images,request,&failure),"original bridge initialize");
        require(original.diagnostic_capture_application(0,info)==S_FALSE,"inactive original capture touched resources");
        for(unsigned eye=0;eye<2;++eye) {
            std::fill(pixels.begin(),pixels.end(),expected[eye]);
            context->UpdateSubresource(original.d3d11_images()[0],eye,nullptr,pixels.data(),description.Width*4,0);
        }
        success(original.release(0),"original application release");
        ComPtr<ID3D12DescriptorHeap> rtv;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
        success(d12->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)),"original fixture RTV heap");
        D3D12_RENDER_TARGET_VIEW_DESC vd{};vd.Format=rt.Format;vd.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;vd.Texture2DArray.ArraySize=2;
        d12->CreateRenderTargetView(runtime_image.Get(),&vd,rtv->GetCPUDescriptorHandleForHeapStart());
        ComPtr<ID3D12CommandAllocator> poison_allocator;ComPtr<ID3D12GraphicsCommandList> poison_list;
        success(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&poison_allocator)),"original poison allocator");
        success(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,poison_allocator.Get(),nullptr,IID_PPV_ARGS(&poison_list)),"original poison list");
        const float magenta[]{1,0,1,1};poison_list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),magenta,0,nullptr);
        success(poison_list->Close(),"original poison close");ID3D12CommandList* poison[]{poison_list.Get()};queue->ExecuteCommandLists(1,poison);
        manager.session=std::make_shared<xrfg::Session>();manager.active_frame=1;manager.has_session=true;
        require(!xrfg::two_frame_capture_claim_application(0),"null original identity claimed");
        require(xrfg::two_frame_capture_claim_application(123),"first original identity refused");
        require(!xrfg::two_frame_capture_claim_application(123),"original identity duplicated");
        for(unsigned key=124;key<131;++key)require(xrfg::two_frame_capture_claim_application(key),"bounded original identity refused");
        require(!xrfg::two_frame_capture_claim_application(131),"original source limit exceeded");
        for(unsigned eye=0;eye<2;++eye){info.stage="xr_application_original";info.slice=eye;info.eye=eye;
            success(original.diagnostic_capture_application(0,info),"ordered original capture");}
        success(original.mark_read(0),"original capture retirement fence");success(original.wait_for_idle(),"original capture completed");
        require(manager.session->shots.size()==2,"original eye snapshots missing");
        for(unsigned eye=0;eye<2;++eye) {
            const auto& shot=*manager.session->shots[eye];void* data{};
            success(shot.readback->Map(0,nullptr,&data),"original readback map");
            for(unsigned y=0;y<description.Height;++y) {
                const auto* row=reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(data)+shot.footprint.Offset+y*shot.footprint.Footprint.RowPitch);
                for(unsigned x=0;x<description.Width;++x)require(row[x]==expected[eye],"original capture read poisoned runtime/history pixels");
            }
            shot.readback->Unmap(0,nullptr);
        }
        manager.active_frame=0;manager.session.reset();manager.has_session=false;
        require(xrfg::two_frame_capture_record_d3d11(info,nullptr,nullptr,nullptr)==S_FALSE,
            "inactive native D11 capture must not touch devices");
        auto native_desc=description;native_desc.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;
        ComPtr<ID3D11Texture2D> native_source;
        success(d11->CreateTexture2D(&native_desc,nullptr,&native_source),"native GTA-format original texture");
        for(unsigned eye=0;eye<2;++eye){std::fill(pixels.begin(),pixels.end(),expected[eye]);
            context->UpdateSubresource(native_source.Get(),eye,nullptr,pixels.data(),description.Width*4,0);}
        manager.session=std::make_shared<xrfg::Session>();manager.active_frame=1;manager.has_session=true;
        info.stage="xr_application_original_d11";
        for(unsigned eye=0;eye<2;++eye){info.eye=eye;info.slice=eye;
            success(xrfg::two_frame_capture_record_d3d11(info,d12.Get(),context.Get(),native_source.Get()),"native original D11 readback");}
        // The writer must own immutable CPU bytes; later game writes cannot
        // replace the snapshot, and no background D11 context call is needed.
        std::fill(pixels.begin(),pixels.end(),0xffff00ffu);
        for(unsigned eye=0;eye<2;++eye)context->UpdateSubresource(native_source.Get(),eye,nullptr,pixels.data(),description.Width*4,0);
        require(manager.session->shots.size()==2,"native original eye shots missing");
        for(unsigned eye=0;eye<2;++eye) {
            const auto& shot=*manager.session->shots[eye];
            require(xrfg::completed(shot)&&!shot.readback&&shot.cpu_ready,"native original did not publish completed CPU ownership");
            for(unsigned y=0;y<description.Height;++y) {
                const auto* row=reinterpret_cast<const std::uint32_t*>(shot.cpu_pixels.data()+shot.footprint.Offset+y*shot.footprint.Footprint.RowPitch);
                for(unsigned x=0;x<description.Width;++x)require(row[x]==expected[eye],"native original pixels changed after game reused texture");
            }
        }
        info.slice=2;
        require(xrfg::two_frame_capture_record_d3d11(info,d12.Get(),context.Get(),native_source.Get())==E_INVALIDARG,"native original accepted invalid array slice");
        info.slice=0;manager.session->allocated=xrfg::kMaximumBytes;
        require(xrfg::two_frame_capture_record_d3d11(info,d12.Get(),context.Get(),native_source.Get())==E_OUTOFMEMORY,"native original exceeded existing capture budget");
        const auto native_directory=std::filesystem::temp_directory_path()/
            (L"xrfg-native-original-"+std::to_wstring(GetCurrentProcessId()));
        xrfg::TwoFrameGuideContract tracking_contract{};
        tracking_contract.stage="afw_evaluate";tracking_contract.publication=101;
        tracking_contract.gta_tracking_render={100,99,98};
        tracking_contract.gta_tracking_mode={2,2,0};
        tracking_contract.gta_tracking_valid={true,false,true};
        for(unsigned i=0;i<3;++i)for(unsigned j=0;j<28;++j)
            tracking_contract.gta_tracking_poses[i][j]=float(i*100+j)+.125F;
        xrfg::two_frame_capture_guide_contract(tracking_contract);
        manager.session->directory=native_directory;manager.session->sealed=true;
        xrfg::write_session(manager.session);
        {
            std::ifstream guide(native_directory/"guide-contracts.tsv");
            std::string header,line;std::getline(guide,header);std::getline(guide,line);
            const auto split=[](const std::string& text) {
                std::vector<std::string> values;std::istringstream input(text);std::string v;
                while(std::getline(input,v,'\t'))values.push_back(v);
                return values;
            };
            const auto columns=split(header),values=split(line);
            require(columns.size()==values.size(),"tracking TSV header/value count differs");
            const auto field=[&](const char* name)->std::string {
                const auto it=std::find(columns.begin(),columns.end(),name);
                require(it!=columns.end(),"tracking column missing");return values[it-columns.begin()];
            };
            require(field("gta_current_render")=="100"&&field("gta_peer_render")=="99"&&
                field("gta_same_render")=="98","tracking production order changed");
            require(field("gta_current_tracking_valid")=="1"&&field("gta_peer_tracking_valid")=="0"&&
                field("gta_same_tracking_valid")=="1","tracking validity missing");
            require(field("gta_current_tracking").starts_with("0.125,")&&
                field("gta_current_tracking").ends_with(",27.125")&&
                field("gta_same_tracking").ends_with(",227.125"),"tracking pose components lost");
        }
        for(unsigned eye=0;eye<2;++eye) {
            std::ifstream raw(native_directory/("image-"+std::to_string(eye)+".raw"),std::ios::binary);
            std::uint32_t first{};raw.read(reinterpret_cast<char*>(&first),sizeof(first));
            require(raw.gcount()==sizeof(first)&&first==expected[eye],"native original CPU writer lost its captured pixels");
        }
        manager.active_frame=0;manager.session.reset();manager.has_session=false;
        std::error_code native_cleanup;std::filesystem::remove_all(native_directory,native_cleanup);
        // Drive the actual automatic request through the shipping session
        // lifecycle, with no keyboard injection and no AFW success required.
        const auto diagnostic=std::filesystem::temp_directory_path()/
            (L"xrfg-origin-loss-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(diagnostic);
        {std::ofstream ini(diagnostic/L"ofxr_bridge.ini");ini<<"[diagnostics]\nlogging_enabled=1\n";}
        xrfg::bridge_flight_logger().initialize(diagnostic);
        require(xrfg::bridge_flight_logger().enabled(),"private F9 fixture not enabled");
        xrfg::diagnostic_ring_enabled=true;
        require(!xrfg::two_frame_capture_request_origin_loss(),"RAM ring unexpectedly enabled full capture");
        xrfg::diagnostic_ring_enabled=false;
        require(xrfg::two_frame_capture_request_origin_loss(),"F9 automatic request refused");
        require(!xrfg::two_frame_capture_request_origin_loss(),"overlapping automatic request accepted");
        require(manager.active_frame==1&&manager.session&&!manager.pending_start,
            "automatic witness was deferred until the next application frame");
        info.stage="luke_origin_loss_color";info.eye=0;info.slice=0;
        success(xrfg::two_frame_capture_record_queue(info,d12.Get(),queue.Get(),mirror,D3D12_RESOURCE_STATE_COMMON),
            "first missing-color witness not recorded immediately");
        const auto loss_session=manager.session;
        require(loss_session->shots.size()==1&&loss_session->shots[0]->info.capture_serial==info.capture_serial,
            "first missing-color witness identity lost");
        require(xrfg::two_frame_capture_begin_end_frame()==1,
            "next application frame did not join the already-started capture");
        manager.session->directory=diagnostic/L"capture";
        std::filesystem::create_directories(manager.session->directory);
        require(manager.session->automatic_origin_loss,"automatic capture reason lost");
        xrfg::two_frame_capture_finish_end_frame(0);
        for(unsigned i=0;i<8;++i)xrfg::two_frame_capture_finish_end_frame(1);
        require(!manager.active_frame&&manager.session->sealed,"missing AFW kept diagnostic open past eight frames");
        if(manager.writer.joinable())manager.writer.join();
        std::ifstream summary(diagnostic/L"capture"/L"README.txt");
        const std::string text((std::istreambuf_iterator<char>(summary)),{});
        require(text.find("automatic GTA origin loss (F9)")!=std::string::npos,"automatic capture reason absent from output");
        manager.session.reset();manager.has_session=false;
        require(xrfg::two_frame_capture_request_origin_loss(),"later recovered episode could not start another capture");
        manager.session->directory=diagnostic/L"capture-2";
        for(unsigned i=0;i<8;++i)xrfg::two_frame_capture_finish_end_frame(1);
        if(manager.writer.joinable())manager.writer.join();
        manager.session.reset();manager.has_session=false;
        xrfg::bridge_flight_logger().shutdown();
        require(!xrfg::two_frame_capture_request_origin_loss(),"automatic request survived F9 OFF");
        std::error_code cleanup;std::filesystem::remove_all(diagnostic,cleanup);
        success(d12->GetDeviceRemovedReason(), "device retained");
        std::cout << "F8 terminal pixels: exact original D11 publication in both slices despite poisoned D12 mirror; inactive/ring bypass, allocator retention and budget retirement pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
