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
constexpr xrfg::LukeGtaRuntimeLayout layout{0x2000,0x3000,0x100,0x200,0x280};
struct Memory {
    std::map<std::uintptr_t,std::vector<unsigned char>> blocks;
    unsigned mode_reads{},yaw_reads{};bool race{},yaw_race{};
    template<class T> void put(std::uintptr_t p,T value) {
        auto& b=blocks[p];b.resize(sizeof(value));std::memcpy(b.data(),&value,sizeof(value));
    }
    Memory() {
        put(module+layout.runtime_pointer,std::uint64_t{object});
        put(object,std::uint64_t{module+layout.runtime_vtable});
        put(object+layout.render_offset,std::uint64_t{79});
        put(object+layout.yaw_offset,.4F);put(object+layout.mode_offset,std::int32_t{2});
    }
    bool operator()(std::uintptr_t p,void* out,std::size_t bytes) {
        auto it=blocks.find(p);if(it==blocks.end()||it->second.size()!=bytes)return false;
        std::memcpy(out,it->second.data(),bytes);
        if(p==object+layout.mode_offset&&race&&++mode_reads==2)static_cast<unsigned char*>(out)[0]=0;
        if(p==object+layout.yaw_offset&&yaw_race&&++yaw_reads==2)static_cast<unsigned char*>(out)[0]^=1;
        return true;
    }
};
xrfg::Quaternion yaw(float angle) {return {0,std::sin(angle*.5F),0,std::cos(angle*.5F)};}
bool near(float a,float b) {return std::abs(a-b)<.00001F;}
}
int main() {try {
    Memory m;
    const auto read=[&](bool tracking=false){return xrfg::read_luke_gta_headlock(module,m,tracking,layout);};
    auto state=read();check(state&&state->active()&&state->render==79&&near(state->yaw_offset,.4F),"effective locked state");
    check(!read(true)->tracking_valid,"unverified diagnostic offsets must not be read");
    check(read()->active(),"delayed unlock must remain locked");
    m.put(object+layout.mode_offset,std::int32_t{0});check(!read()->active(),"effective unlock");
    m=Memory{};m.race=true;check(!read(),"racing mode");
    m=Memory{};m.yaw_race=true;check(!read(),"racing yaw");
    m=Memory{};m.put(object,std::uint64_t{module+1});check(!read(),"other game");
    m=Memory{};m.put(object+layout.mode_offset,std::int32_t{7});check(!read(),"unknown mode");
    m=Memory{};m.put(object+layout.yaw_offset,std::numeric_limits<float>::quiet_NaN());check(!read(),"nonfinite yaw");
    m=Memory{};m.put(module+layout.runtime_pointer,std::uint64_t{0});check(!read(),"retired object");
    m=Memory{};check(!xrfg::read_luke_gta_headlock(module,m,false,{}),"unverified layout");
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
    std::cout<<"GTA effective lock/unlock, discovered layout/race guards and simultaneous canted eye rig passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
