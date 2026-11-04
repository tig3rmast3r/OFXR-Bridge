#include "xrfg/rife_game_history.hpp"
#include <iostream>
#include <stdexcept>
using namespace xrfg;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct GroupedNumbers:std::numpunct<char>{char do_thousands_sep()const override{return ',';}
    std::string do_grouping()const override{return "\3";}};
int main(){try{
    std::array<XrCompositionLayerProjectionView,4> views{};
    for(unsigned i=0;i<4;++i){views[i].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[i].subImage.swapchain=reinterpret_cast<XrSwapchain>(std::uintptr_t(i+1));
        views[i].subImage.imageRect.extent={4400,4030};}
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};projection.viewCount=2;projection.views=views.data();
    const XrCompositionLayerBaseHeader* layers[]{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};frame.layerCount=1;frame.layers=layers;
    auto single=rife_game_layout(&frame);check(single.count==1 && single.shapes[0]==RifeGameShape{4400,4030,1},"independent-eye chains use only one-eye engines");
    views[1].subImage.swapchain=views[0].subImage.swapchain;views[1].subImage.imageArrayIndex=1;
    const auto stereo=rife_game_layout(&frame);check(stereo.count==1 && stereo.shapes[0].eyes==2,"shared array uses only two-eye engines");
    views[1].subImage.imageRect.extent={4200,3900};
    check(rife_game_layout(&frame)==stereo,"asymmetric shared viewports use native RIFE's first-view shape and two-eye batch");
    views[1].subImage.imageRect.extent={4400,4030};
    views[1].subImage.imageArrayIndex=0;
    check(rife_game_layout(&frame)==single,"identical repeated subimages are one physical input, matching native mapping deduplication");
    views[1].subImage.imageRect.offset.x=4400;
    check(rife_game_layout(&frame)==stereo,"packed eye rectangles use their eye extent, not enclosing texture width");
    views[1].subImage.swapchain=reinterpret_cast<XrSwapchain>(2);projection.viewCount=4;
    views[2].subImage.imageRect.extent={2000,1800};views[3].subImage.imageRect.extent={2000,1800};
    const auto quad=rife_game_layout(&frame);check(quad.count==2 && quad.shapes[0].eyes==1 && quad.shapes[1].eyes==1,"quad views retain distinct focus/periphery shapes, without duplicate opposite-eye jobs");
    const auto folder=std::filesystem::temp_directory_path()/(L"ofxr-game-history-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    RifeGameHistory history;unsigned adapter_calls=0;
    const auto adapter=[&]{++adapter_calls;return 123ULL;};
    const auto exe=std::filesystem::path(L"C:/Games/My game/game.exe");
    check(history.observe(single,folder,exe,adapter),"first valid resolution saved without diagnostic logging");
    check(!history.observe(single,folder,exe,adapter) && adapter_calls==1,"unchanged frame never writes or asks GPU adapter again");
    check(history.observe(stereo,folder,exe,adapter),"eye-layout-only change replaces record");
    const auto previous_locale=std::locale::global(std::locale(std::locale::classic(),new GroupedNumbers));
    const bool saved=history.observe(quad,folder,exe,adapter);
    std::locale::global(previous_locale);
    check(saved,"new resolution replaces record under a game's non-default numeric locale");
    unsigned files=0;std::filesystem::path record;
    for(const auto& entry:std::filesystem::directory_iterator(folder)){++files;record=entry.path();}
    check(files==1,"one game retains only its last layout");
    std::ifstream stream(record);std::string json{std::istreambuf_iterator<char>(stream),{}};stream.close();
    check(json.find("\"width\":2000")!=std::string::npos && json.find("\"adapter_luid\":\"123\"")!=std::string::npos,"actual dimensions and adapter persisted");
    check(!history.observe({},folder,exe,adapter),"empty/invalid frame cannot erase last resolution");
    std::filesystem::remove(record);std::filesystem::remove(folder);
    std::cout<<"RIFE game history: single/stereo/packed/quad, latest-only and change-only writes PASS\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
