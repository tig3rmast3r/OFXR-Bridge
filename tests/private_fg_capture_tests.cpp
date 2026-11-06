// Actual D3D12 publication/copy/fences, fake native XR only. No headset or game.
#define main xrfg_unused_private_runtime_entry
#include "private_openxr_runtime_tests.cpp"
#undef main
#include "../src/common/two_frame_capture.cpp"
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iostream>

namespace {
std::shared_ptr<xrfg::Session> arm(const std::filesystem::path& folder){
    auto& state=xrfg::manager();
    if(state.writer.joinable())state.writer.join();
    state.session=std::make_shared<xrfg::Session>();state.session->directory=folder;
    state.session->want_deliveries=true;state.has_session=true;state.delivery_active=true;
    state.active_frame=1;state.afw_completed_frame=false;state.observed_end_frames=0;
    return state.session;
}
void finish_upstream(){
    xrfg::two_frame_capture_mark_afw_evaluation();xrfg::two_frame_capture_finish_end_frame(1);
    xrfg::two_frame_capture_mark_afw_evaluation();xrfg::two_frame_capture_finish_end_frame(2);
    check(!xrfg::two_frame_capture_images_active(),"AFW input capture closes after two evaluations");
}
XrResult submit(Fixture& f,XrSwapchain left,XrSwapchain right,unsigned value,const std::shared_ptr<xrfg::InterfaceDeliveryReceipt>& receipt){
    std::array<XrCompositionLayerProjectionView,2> views;
    for(unsigned i=0;i<2;++i){auto& v=views[i];v={XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};v.pose.orientation.w=1;
        v.pose.position={value+(i?.03F:-.03F),.5F,-1};v.fov={-.7F,.8F,.6F,-.5F};v.subImage.swapchain=i?right:left;v.subImage.imageRect.extent={1,1};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.space=reinterpret_cast<XrSpace>(9);projection.viewCount=2;projection.views=views.data();
    auto* layer=reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};end.displayTime=epoch+ns();end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;end.layerCount=1;end.layers=&layer;
    f.begin_virtual();return f.transport->end_frame(&end,receipt);
}
void sequence(const std::filesystem::path& folder,bool director,bool native12){
    native_contract_fixture_mode=true;native_director_only_fixture_mode=true;
    Fixture f(native12);f.throttled=false;f.unblock();f.expected_views=2;
    xr_ok(f.transport->set_director_enabled(director),"capture presenter selection");
    auto left=f.make_chain(),right=f.make_chain();
    f.write(left,9);f.write(right,19);
    check(!xrfg::two_frame_capture_delivery_active(),"F9 alone must not start image capture");
    xr_ok(submit(f,left,right,0,std::make_shared<xrfg::InterfaceDeliveryReceipt>(false)),"uncaptured baseline");
    f.wait_count(1);xr_ok(f.transport->drain(),"baseline drain");
    check(!xrfg::manager().session,"no F8 allocations or files");
    auto session=arm(folder);finish_upstream();
    // Native refusal is a recorded outcome, never relabelled as displayed.
    f.refuse_end_call=5;
    std::vector<std::shared_ptr<xrfg::InterfaceDeliveryReceipt>> receipts;
    for(unsigned i=0;i<8;++i){
        f.write(left,20+i*25);f.write(right,220-i*20);
        auto receipt=std::make_shared<xrfg::InterfaceDeliveryReceipt>(i%2!=0);
        receipt->capture_cycle=1000+i/2;receipt->capture_owner=777;receipt->capture_serial=900+i/2;
        receipt->capture_generated_index=i%2;receipt->synthetic_confirmed=i%2!=0;receipts.push_back(receipt);
        xr_ok(submit(f,left,right,i+1,receipt),"ordered capture packet");
    }
    f.wait_count(9);xr_ok(f.transport->drain(),"capture native drain");
    if(xrfg::manager().writer.joinable())xrfg::manager().writer.join();
    check(session->write_finished && session->safe_to_release,"capture writer retired safely");
    check(session->deliveries.size()==8 && session->delivery_views.size()==16 && session->shots.size()==16,"eight real/interpolated packets with both eyes");
    check(!xrfg::two_frame_capture_delivery_active(),"bounded capture window closed");
    for(unsigned i=0;i<8;++i){const auto& d=session->deliveries[i];
        check(d.completed && !d.recording && d.kind==int(i%2) && d.source_cycle==1000+i/2 && d.source_serial==900+i/2,"packet kind/epoch/order/completion");
        check(d.entered_ns>0 && d.returned_ns>=d.entered_ns && d.layer_count==1 && d.submitted_time>0,"actual native End witness");
        check((d.result==XR_SUCCESS)==(receipts[i]->outcome.load()==1),"native refusal not confused with accepted content");
        check((i==3)==(d.result!=XR_SUCCESS),"refused packet explicitly recorded");
        for(unsigned eye=0;eye<2;++eye){
            auto view=std::find_if(session->delivery_views.begin(),session->delivery_views.end(),[&](const auto& v){return v.token==d.token && v.view==eye;});
            check(view!=session->delivery_views.end() && view->rect==std::array<std::int32_t,4>{0,0,1,1} && view->native_chain && view->generation,"view/source/target identity");
            check(std::abs(view->pose[0]-(i+1+(eye?.03F:-.03F)))<1e-5F && view->pose[6]==1,"actual per-eye pose retained");
            auto shot=std::find_if(session->shots.begin(),session->shots.end(),[&](const auto& s){return s->info.stream==d.token && s->info.eye==int(eye);});
            check(shot!=session->shots.end() && SUCCEEDED((*shot)->status) && xrfg::completed(**shot),"exact completed image snapshot");
            check(view->image_api==(native12?12:11) && view->image_result==S_OK,"actual endpoint image API and result are explicit");
            unsigned char pixel{};
            if(native12){void* pixels{};ok((*shot)->readback->Map(0,nullptr,&pixels),"capture pixel readback");
                pixel=*static_cast<unsigned char*>(pixels);(*shot)->readback->Unmap(0,nullptr);
            }else{check((*shot)->cpu_ready && !(*shot)->readback,"pure D11 capture uses staging bytes, not an inferred D12 mirror");
                pixel=std::to_integer<unsigned char>((*shot)->cpu_pixels[0]);}
            check(pixel==(eye?220-i*20:20+i*25),"captured output changed after native-image reuse");
        }
    }
    check(std::filesystem::file_size(folder/"fg-deliveries.tsv")>100 && std::filesystem::file_size(folder/"fg-delivery-views.tsv")>100,"joined delivery evidence written");
    const auto old_token=session->deliveries[0].token;
    auto& state=xrfg::manager();state.session.reset();state.has_session=false;state.delivery_active=false;
    auto fresh=arm(folder/"late-token");
    xrfg::two_frame_capture_complete_delivery(old_token,XR_SUCCESS,1,999,999,999,true);
    check(fresh->deliveries.empty(),"old-capture token cannot mutate a new capture");
    fresh->want_deliveries=false;finish_upstream();if(state.writer.joinable())state.writer.join();state.session.reset();state.has_session=false;state.delivery_active=false;
    xr_ok(f.transport->destroy_swapchain(left),"left cleanup");xr_ok(f.transport->destroy_swapchain(right),"right cleanup");
    std::printf("FG_CAPTURE D%d %s: actual private copies, 8 ordered native receipts, exact both-eye pixels after target reuse, distinct poses, refusal, F9-only off and stale-token isolation PASS\n",native12?12:11,director?"Director":"Framework");
}
void repeat_and_limits(const std::filesystem::path& folder,bool native12){
    Fixture f(native12);f.throttled=false;f.unblock();f.expected_views=2;
    auto left=f.make_chain(true),right=f.make_chain(true);f.write(left,61);f.write(right,181);
    auto session=arm(folder/"repeat");finish_upstream();
    for(unsigned i=0;i<8;++i){
        xr_ok(submit(f,left,right,i,std::make_shared<xrfg::InterfaceDeliveryReceipt>(false)),"static capture repeat");
        f.wait_count(i+1);xr_ok(f.transport->drain(),"static captured repeat drain");
    }
    auto& state=xrfg::manager();if(state.writer.joinable())state.writer.join();
    check(session->safe_to_release && session->deliveries.size()==8 && session->delivery_views.size()==16 && session->shots.size()==2,"released native targets are not read again");
    for(std::size_t i=2;i<session->delivery_views.size();++i)
        check(session->delivery_views[i].reference_token==session->deliveries[0].token,"repeat references the original captured generation");
    check(f.real_acquires==2,"capture adds no native swapchain acquisition");
    state.session.reset();state.has_session=false;state.delivery_active=false;
    xr_ok(f.transport->destroy_swapchain(left),"repeat left cleanup");xr_ok(f.transport->destroy_swapchain(right),"repeat right cleanup");

    auto limited=arm(folder/"limits");limited->delivery_allocated=xrfg::kMaximumDeliveryBytes;
    auto l=f.make_chain(),r=f.make_chain();f.write(l,25);f.write(r,125);
    xr_ok(submit(f,l,r,1,std::make_shared<xrfg::InterfaceDeliveryReceipt>(false)),"budget failure cannot fail real submission");
    f.wait_count(9);xr_ok(f.transport->drain(),"limited capture drain");
    check(limited->deliveries.size()==1 && limited->deliveries[0].completed && limited->deliveries[0].result==XR_SUCCESS,"native success independent of readback budget");
    check(limited->shots.size()==2 && limited->allocated==0 && limited->nr_allocated==0,"separate FG budget preserves original/NR budgets");
    for(const auto& shot:limited->shots)check(shot->status==E_OUTOFMEMORY && !shot->readback,"bounded allocation failure is explicit");
    xrfg::TwoFrameDelivery pending;pending.kind=1;auto pending_token=xrfg::two_frame_capture_claim_delivery(pending);
    xrfg::two_frame_capture_finish_delivery_copy(pending_token,E_ABORT);
    auto started=std::chrono::steady_clock::now();finish_upstream();if(state.writer.joinable())state.writer.join();
    auto elapsed=std::chrono::steady_clock::now()-started;
    check(limited->write_finished && limited->safe_to_release && limited->sealed && !limited->deliveries.back().completed,"timeout preserves pending versus native-completed distinction");
    check(elapsed>=900ms && elapsed<3s && !xrfg::two_frame_capture_delivery_active(),"delivery window closes without requiring more game frames");
    check(xrfg::two_frame_capture_claim_delivery({})==0,"sealed capture refuses late claims");
    state.session.reset();state.has_session=false;state.delivery_active=false;
    xr_ok(f.transport->destroy_swapchain(l),"limited left cleanup");xr_ok(f.transport->destroy_swapchain(r),"limited right cleanup");
    std::puts("FG_CAPTURE static generation references, no read after release, independent budget failure, pending/cancelled metadata and bounded timeout PASS");
}
}
int main(){try{
    const auto folder=std::filesystem::current_path()/("fg-capture-test-"+std::to_string(GetCurrentProcessId()));std::filesystem::create_directories(folder);
    std::ofstream(folder/"ofxr_bridge.ini")<<"[diagnostics]\nlogging_enabled=1\nflush_each_event=0\n";
    auto& logger=xrfg::bridge_flight_logger();logger.initialize(folder);check(logger.enabled(),"test F9 logger");
    for(bool native12:{true,false}){
        auto path=folder/(native12?"d12":"d11");
        sequence(path/"framework",false,native12);sequence(path/"director",true,native12);repeat_and_limits(path,native12);
    }
    logger.shutdown();std::cout<<folder.string()<<'\n';return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
