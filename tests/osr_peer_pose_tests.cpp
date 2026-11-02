#include "xrfg/osr_peer_pose.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void near(const xrfg::Vec3& a,const xrfg::Vec3& b){check(std::abs(a.x-b.x)<1e-5F && std::abs(a.y-b.y)<1e-5F && std::abs(a.z-b.z)<1e-5F,"peer position mismatch");}
int main(int argc,char** argv){try {
    using namespace xrfg;
    std::array<float,16> a{1,0,0,0,0,1,0,0,0,0,1,0,10,20,30,1},b=a;
    Pose source;source.position={.4F,.7F,-.2F};b[12]+=.136F;
    auto peer=osr_peer_position(source,a,b,{1,1,1},.068F,.136F);
    check(peer.has_value(),"scaled renderer displacement accepted");near(*peer,{.468F,.7F,-.2F});
    b[12]=a[12]-.136F;near(*osr_peer_position(source,a,b,{1,1,1},.068F,.136F),{.332F,.7F,-.2F});
    b[12]=a[12];b[13]+=.136F;near(*osr_peer_position(source,a,b,{1,-1,1},.068F,.136F),{.4F,.632F,-.2F});
    b=a;b[12]+=.136F;source.orientation={0,std::sqrt(.5F),0,std::sqrt(.5F)};
    near(*osr_peer_position(source,a,b,{1,1,1},.068F,.136F),{.4F,.7F,-.268F});
    b=a;b[12]+=.1F;b[14]+=.1F; // actual peer camera can use another rig direction
    source.orientation={};peer=osr_peer_position(source,a,b,{1,1,-1},.068F,.136F);
    near(*peer,{.45F,.7F,-.25F});
    check(!osr_peer_position(source,a,b,{1,1,1},0,.136F),"missing baseline never creates a pose");
    a[0]=std::numeric_limits<float>::quiet_NaN();
    check(!osr_peer_position(source,a,b,{1,1,1},.068F,.136F),"invalid render basis refused");
    if(argc==2) {
        std::ifstream file(argv[1]);check(bool(file),"open sealed F8 replay");unsigned count{};
        Pose pose;std::array<float,3> signs{};float ipd{},game_ipd{};Vec3 expected;
        while(file>>pose.position.x>>pose.position.y>>pose.position.z) {
            file>>pose.orientation.x>>pose.orientation.y>>pose.orientation.z>>pose.orientation.w;
            for(auto& v:a)file>>v;for(auto& v:b)file>>v;for(auto& v:signs)file>>v;
            file>>ipd>>game_ipd>>expected.x>>expected.y>>expected.z;check(bool(file),"complete replay record");
            peer=osr_peer_position(pose,a,b,signs,ipd,game_ipd);check(peer.has_value(),"replay produces peer position");near(*peer,expected);++count;
        }
        check(count==2,"both recorded left/right F8 cases verified");std::cout<<"Two sealed F8 render-camera replays PASS\n";
    }
    std::cout<<"OSR peer pose: renderer displacement, unit scale, rig direction, axis signs and guards PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
