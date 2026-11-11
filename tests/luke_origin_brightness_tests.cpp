#include "xrfg/luke_origin_brightness.hpp"
#include "xrfg/luke_color_origin.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <string>
using xrfg::LukeOriginBrightness;
namespace {
unsigned checks{};
void require(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
struct Run {
    LukeOriginBrightness p;std::uint64_t now=1000;std::uint32_t token=1;unsigned base{};
    std::array<std::uint32_t,2> stamp() {
        auto c=p.choice(base,now);std::array<std::uint32_t,2> pair{token++,token++};
        p.stamped(pair[0],c,now);p.stamped(pair[1],c,now);return pair;
    }
    void tick(unsigned threshold,bool completed=true,bool dark=true) {
        auto pair=stamp();const bool valid=p.shift()>=threshold;
        if(!valid)pair={};p.observe(now,completed,!valid,pair,dark);now+=20;
    }
    void learn(unsigned threshold) {
        for(unsigned i=0;i<800;++i){tick(threshold);if(!p.probing()&&p.learned()==threshold)return;}
        throw std::runtime_error("failed to learn readable level "+std::to_string(threshold)+" current "+std::to_string(p.shift()));
    }
};
void policy_tests() {
    Run normal;for(unsigned i=0;i<500;++i)normal.tick(0);
    require(normal.p.shift()==0&&!normal.p.probing(),"readable baseline changed");
    Run r;r.learn(4);require(r.p.shift()==4&&r.p.learned()==4,"did not retain first sufficient brightness");
    for(unsigned i=0;i<2000;++i)r.tick(4);require(r.p.shift()==4&&!r.p.probing(),"working level did not persist");
    for(unsigned i=0;i<99;++i)r.tick(99);require(r.p.shift()==4&&!r.p.probing(),"transient loss started probe");
    // A subsequent check starts from normal and may settle LOWER if exposure changed.
    for(unsigned i=0;i<10&&!r.p.probing();++i)r.tick(99);
    require(r.p.probing(),"repeated loss did not restart check");
    r.learn(2);require(r.p.learned()==2,"recheck did not discover lower sufficient level");
    const auto saved=r.p.learned();bool saw_probe=false,rolled=false;
    for(unsigned i=0;i<800;++i){r.tick(99);saw_probe|=r.p.probing();if(saw_probe&&!r.p.probing()){rolled=true;break;}}
    require(rolled&&r.p.shift()==saved&&r.p.learned()==saved,"failed recheck did not restore last good brightness");
    for(unsigned i=0;i<1400;++i){r.tick(99);require(!r.p.probing()&&r.p.shift()==saved,"retry cooldown oscillates");}
    Run first;bool started=false;
    for(unsigned i=0;i<800;++i){first.tick(99);started|=first.p.probing();if(started&&!first.p.probing())break;}
    require(started&&first.p.shift()==0&&first.p.learned()==0,"initial failed probe did not restore baseline");
    Run generic;for(unsigned i=0;i<1000;++i){generic.stamp();generic.p.observe(generic.now,true,true,{});generic.now+=20;}
    require(!generic.p.probing()&&generic.p.shift()==0,"generic missing CRC started initial adaptation");
    require(r.p.permits_generic_retry(),"successful dark recovery did not enable broader recheck");
    Run broad;broad.learn(4);for(unsigned i=0;i<160&&!broad.p.probing();++i)broad.tick(99,true,false);
    require(broad.p.probing(),"broader retry after proven dark recovery did not start");
    Run transient;for(unsigned i=0;i<120&&!transient.p.probing();++i)transient.tick(99);
    for(unsigned i=0;i<80&&transient.p.probing();++i)transient.tick(0);
    require(!transient.p.permits_generic_retry(),"unmodified normal recovery incorrectly unlocked generic retries");
    Run errors;for(unsigned i=0;i<1000;++i)errors.tick(99,false);
    require(!errors.p.probing()&&errors.p.shift()==0,"transport errors changed brightness");
    Run ambiguity;for(unsigned i=0;i<1000;++i){ambiguity.stamp();ambiguity.p.observe(ambiguity.now,false,false,{});ambiguity.now+=20;}
    require(!ambiguity.p.probing(),"ambiguous results triggered probe");
    Run paused;for(unsigned i=0;i<100;++i)paused.tick(99);paused.now+=5000;
    paused.tick(99);require(!paused.p.probing(),"failure accumulated across pause");
    // No producer traffic cannot drive the control loop.
    Run absent;absent.p.choice(0,absent.now);
    for(unsigned i=0;i<1000;++i){absent.p.observe(absent.now,true,true,{});absent.now+=20;}
    require(!absent.p.probing()&&absent.p.shift()==0,"missing producer triggered brightness changes");
    // Old brightness epochs cannot commit a new level; neither can one valid eye.
    Run stale;auto old=stale.stamp();for(unsigned i=0;i<180;++i)stale.tick(99);
    require(stale.p.probing()&&stale.p.shift()>0,"probe setup");
    for(unsigned i=0;i<70;++i){auto current=stale.stamp();
        stale.p.observe(stale.now,true,false,std::array<std::uint32_t,2>{current[0],old[1]});stale.now+=20;}
    require(stale.p.probing()&&stale.p.learned()==0,"stale peer CRC committed candidate");
    // Frozen valid token is not multiple-frame evidence; hard deadline restores fallback.
    Run frozen;for(unsigned i=0;i<150;++i)frozen.tick(99);auto same=frozen.stamp();
    for(unsigned i=0;i<900;++i){frozen.stamp();frozen.p.observe(frozen.now,true,false,same);frozen.now+=20;}
    require(!frozen.p.probing()&&frozen.p.shift()==0,"frozen token or hung check escaped rollback");
    Run rdr;rdr.base=8;for(unsigned i=0;i<100;++i)rdr.tick(8);
    require(rdr.p.shift()==8&&rdr.p.learned()==8,"existing HDR unit floor changed");
    for(unsigned i=0;i<600;++i)rdr.tick(99);require(rdr.p.shift()==8,"brightness exceeded format cap");
    // A readback completed after a newer one cannot rewind the policy clock.
    auto c=r.p.choice(0,r.now);r.p.observe(r.now-1000,true,true,{});
    require(r.p.choice(0,r.now).epoch==c.epoch,"out-of-order result changed epoch");
}
void codec_tests() {
    using namespace xrfg;
    for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R11G11B10_FLOAT})
        for(unsigned shift=0;shift<=8;++shift)for(bool bright:{false,true}) {
            std::array<std::byte,16> pixel{};luke_origin_pixel(pixel.data(),format,true,bright,shift);
            require(std::abs(luke_origin_luma(pixel.data(),format)-std::ldexp(bright?1.f:kLukeOriginHdrWhite,shift))<.001f,"float level encoding");
            luke_origin_pixel(pixel.data(),format,false,bright,shift);require(luke_origin_luma(pixel.data(),format)==0,"black level lifted");
            std::array<float,kLukeOriginWidth> row{};
            for(unsigned x=0;x<row.size();++x){luke_origin_pixel(pixel.data(),format,luke_origin_white(123456,x),bright,shift);row[x]=luke_origin_luma(pixel.data(),format);}
            auto decoded=decode_luke_origin_row(row);require(decoded.token==123456&&!decoded.ambiguous,"level altered CRC encoding");
        }
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_UNORM})
        for(unsigned shift=0;shift<=8;++shift)for(bool white:{false,true}) {
            std::array<std::byte,16> a{},b{};luke_origin_pixel(a.data(),format,white,false,0);luke_origin_pixel(b.data(),format,white,true,shift);
            require(a==b,"integer producer changed");
        }
    LukeColorOriginLayout before{},after{};
    for(auto eye:{std::array<unsigned,2>{3651,3653},{3669,3671}}) {
        require(compact_luke_origin_layout(3696,3696,eye[0],eye[1],before),"capture baseline layout");
        for(unsigned shift=0;shift<=8;++shift) {
            require(compact_luke_origin_layout(3696,3696,eye[0],eye[1],after),"candidate layout");
            require(after.band_count==before.band_count&&after.scan.height==before.scan.height,"added marker or enlarged scan");
        }
    }
}
}
int main(){try{policy_tests();codec_tests();std::cout<<checks<<" checks PASS\n";return 0;}
catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
