#include "xrfg/luke_gta_headlock.hpp"
#include "xrfg/luke_stereo_pose.hpp"
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>
namespace {
void check(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
constexpr std::uintptr_t module=0x180000000,object=0x220000000;
struct Memory {
    std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
    unsigned reads{},tracking_reads{};bool race{},tracking_race{};
    template<class T> void put(std::uintptr_t p,T value) {
        auto& b=blocks[p];b.resize(sizeof(value));std::memcpy(b.data(),&value,sizeof(value));
    }
    Memory() {
        auto& h=blocks[module];h.resize(4096);h[0]='M';h[1]='Z';
        const auto u32=[&](unsigned p,std::uint32_t v){std::memcpy(h.data()+p,&v,4);};
        u32(0x3c,0x100);u32(0x100,0x4550);u32(0x108,1792152767);u32(0x100+24+56,11485184);
        blocks[module+0x1fedff]={0x8b,0x87,0x2c,0x11,0x84,0x00,0x89,0x87,0x58,0x6c,0x85,0x00};
        blocks[module+0x1fdef6]={0xf3,0x0f,0x5c,0xb3,0x18,0x11,0x84,0x00};
        put(module+xrfg::kLukeGtaRuntimePointer,std::uint64_t{object});
        put(object,std::uint64_t{module+xrfg::kLukeGtaRuntimeVtable});
        put(object+0x841790,std::uint64_t{79});
        auto& state=blocks[object+0x841118];state.resize(24);
        float yaw=.4F;std::int32_t mode=2;std::memcpy(state.data(),&yaw,4);std::memcpy(state.data()+20,&mode,4);
    }
    bool operator()(std::uintptr_t p,void* out,std::size_t bytes) {
        if(p==object+0x840ab8)++tracking_reads;
        auto it=blocks.find(p);if(it==blocks.end()||it->second.size()!=bytes)return false;
        std::memcpy(out,it->second.data(),bytes);
        if(p==object+0x840ab8&&tracking_race&&tracking_reads==2)
            static_cast<unsigned char*>(out)[0]^=1;
        if(p==object+0x841118&&race&&++reads==2)static_cast<unsigned char*>(out)[20]=0;
        return true;
    }
};
xrfg::Quaternion yaw(float angle) {return {0,std::sin(angle*.5F),0,std::cos(angle*.5F)};}
bool near(float a,float b) {return std::abs(a-b)<.00001F;}
}
int main() {try {
    Memory m;check(xrfg::luke_gta_headlock_layout(module,m),"pinned layout");
    auto state=xrfg::read_luke_gta_headlock(module,m);check(state&&state->active()&&state->render==79&&near(state->yaw_offset,.4F),"effective locked state");
    check(!state->tracking_valid&&m.tracking_reads==0,"disabled diagnostics read tracking memory");
    m=Memory{};
    auto& tracking=m.blocks[object+0x840ab8];tracking.resize(180);
    constexpr unsigned offsets[]{0,48,96,152};
    for(unsigned i=0;i<4;++i) {
        const std::array<float,7> pose{0,0,0,1,.1F+i,.2F+i,.3F+i};
        std::memcpy(tracking.data()+offsets[i],pose.data(),sizeof(pose));
    }
    state=xrfg::read_luke_gta_headlock(module,m,true);
    check(state&&state->active()&&state->tracking_valid&&near(state->tracking[25],3.1F),"matched producer tracking receipt");
    m.tracking_race=true;m.tracking_reads=0;
    state=xrfg::read_luke_gta_headlock(module,m,true);
    check(state&&state->active()&&!state->tracking_valid,"tracking race changed camera admission");
    m.tracking_race=false;
    const float invalid=std::numeric_limits<float>::quiet_NaN();
    std::memcpy(tracking.data(),&invalid,sizeof(invalid));
    state=xrfg::read_luke_gta_headlock(module,m,true);
    check(state&&state->active()&&!state->tracking_valid,"invalid tracking changed camera admission");
    m=Memory{};
    state=xrfg::read_luke_gta_headlock(module,m,true);
    check(state&&state->active()&&!state->tracking_valid,"missing tracking changed camera admission");
    // This remains active after input release; no button state is consulted.
    check(xrfg::read_luke_gta_headlock(module,m)->active(),"delayed unlock must remain locked");
    m.blocks[object+0x841118][20]=0;check(!xrfg::read_luke_gta_headlock(module,m)->active(),"effective unlock");
    m=Memory{};m.race=true;check(!xrfg::read_luke_gta_headlock(module,m),"racing state");
    m=Memory{};m.put(object,std::uint64_t{module+1});check(!xrfg::read_luke_gta_headlock(module,m),"other game");
    m=Memory{};m.blocks[module][0x108]^=1;check(!xrfg::luke_gta_headlock_layout(module,m),"other build");
    m=Memory{};m.blocks[module+0x1fdef6][0]^=1;check(!xrfg::luke_gta_headlock_layout(module,m),"wrong yaw semantics");
    m=Memory{};m.blocks[object+0x841118][20]=7;check(!xrfg::read_luke_gta_headlock(module,m),"unknown mode");
    m=Memory{};m.put(module+xrfg::kLukeGtaRuntimePointer,std::uint64_t{0});check(!xrfg::read_luke_gta_headlock(module,m),"retired object");
    // Independent physical eye rig with canted views and a different source
    // head orientation/position from the LocateViews used to measure the rig.
    std::array<xrfg::Pose,2> rig{};

    for(unsigned eye=0;eye<2;++eye) {
        const float sign=eye?1.F:-1.F;
        rig[eye].orientation=yaw(.3F+sign*.07F);
        rig[eye].position={8+sign*.032F*std::cos(.3F),9,10-sign*.032F*std::sin(.3F)};
    }
    for(unsigned eye=0;eye<2;++eye) {
        const float sign=eye?1.F:-1.F;
        const xrfg::Vec3 p{sign*.032F*std::cos(1.1F),0,-sign*.032F*std::sin(1.1F)};
        xrfg::Pose source{yaw(1.1F+sign*.07F),{p.x+2,p.y+3,p.z+4}};
        const auto peer=xrfg::luke_same_time_peer(rig,eye,source);check(bool(peer),"simultaneous peer");
        const auto expected=yaw(1.1F-sign*.07F);
        check(near(peer->orientation.y,expected.y)&&near(peer->orientation.w,expected.w),"preserve relative cant");
        check(near(peer->position.x,2-p.x)&&near(peer->position.y,3-p.y)&&near(peer->position.z,4-p.z),"same-time physical origin");
    }
    auto bad=rig;bad[1].position=bad[0].position;check(!xrfg::luke_same_time_peer(bad,0,rig[0]),"zero baseline");
    bad=rig;bad[0].orientation={0,0,0,0};check(!xrfg::luke_same_time_peer(bad,0,rig[0]),"invalid rig orientation");
    check(!xrfg::luke_same_time_peer(rig,2,rig[0]),"invalid eye");
    std::cout<<"GTA effective lock/unlock, pinned layout/race guards and simultaneous canted eye rig passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
