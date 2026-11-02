#include "xrfg/luke_origin_continuity.hpp"
#include <iostream>
#include <stdexcept>
namespace {
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
xrfg::LukeOriginSequenceFrame frame(unsigned step=0){xrfg::LukeOriginSequenceFrame f;
f.stream=7;f.owner=99;f.epoch=3;f.publication=100+step;f.token=100+step;f.native=800+step;
f.eye=step&1;f.contexts={10,20};f.captures={50+step/2,50+(step+1)/2};f.shape={3424,2736,2283,1824};return f;}
}
int main(){try{
 for(bool split:{false,true}) {
    xrfg::LukeOriginContinuityBank bank;
    const auto sample=[&](unsigned n){auto f=frame(n);if(split){f.stream=7+(n&1);f.native=900+n/2;}return f;};
    for(unsigned n=0;n<8;++n)bank.confirm(sample(n),true,100+n*10);
    for(unsigned n=8;n<40;++n) {
        const auto f=sample(n);auto p=bank.predict(100+n*10,f.eye,f.captures,f.contexts);
        check(p&&xrfg::LukeOriginContinuity::matches(p->frame,f),"shared/per-eye SR counter phase failed");bank.used(f.eye);
    }
    auto f=sample(40);check(!bank.predict(1200,f.eye,f.captures,f.contexts),"global inferred frames renewed their own second");
    bank.invalidate();check(!bank.predict(200,f.eye,f.captures,f.contexts),"global invalidation left other-eye model armed");
 }
 {xrfg::LukeOriginContinuityBank bank;
  for(unsigned n=0;n<4;++n)bank.confirm(frame(n),true,100+n*10);
  auto f=frame(4);check(!bank.predict(150,f.eye,f.captures,f.contexts),"bank inferred after only one fitted interval");}
 xrfg::LukeOriginContinuity c;auto a=frame(),b=frame(1);check(!c.confirm(a,true,100),"initial anchor reset");
 auto next=frame(1);check(!c.predict(101,next.eye,next.captures,next.contexts),"unlearned sequence guessed");
 check(!c.confirm(b,true,110),"confirmed next source reset");
 for(unsigned step=2;step<92;++step){auto f=frame(step);auto p=c.predict(110+(step-1)*11,f.eye,f.captures,f.contexts);
  check(p&&xrfg::LukeOriginContinuity::matches(p->frame,f),"fresh stereo sequence/native identity did not continue");c.used();}
 auto f=frame(92);check(c.predict(1110,f.eye,f.captures,f.contexts).has_value(),"inclusive one second lease failed");
 unsigned refusal{};
 check(!c.predict(1111,f.eye,f.captures,f.contexts,&refusal)&&refusal==2,"expired lease diagnostic changed actual timeout");
 check(!c.predict(109,f.eye,f.captures,f.contexts),"clock regression accepted");
 auto changed=f;changed.contexts[0]++;check(!c.predict(1000,changed.eye,changed.captures,changed.contexts),"replaced capture context inferred");
 changed=f;changed.captures[changed.eye]+=1;check(!c.predict(1000,changed.eye,changed.captures,changed.contexts),"broken eye alternation inferred");
 auto predicted=c.predict(1110,f.eye,f.captures,f.contexts);check(bool(predicted),"candidate missing");
 changed=f;changed.native++;check(!xrfg::LukeOriginContinuity::matches(predicted->frame,changed),"different native production accepted");
 changed=f;changed.shape[2]++;check(!xrfg::LukeOriginContinuity::matches(predicted->frame,changed),"resized guide accepted");
 changed=f;changed.owner++;check(!xrfg::LukeOriginContinuity::matches(predicted->frame,changed),"different runtime owner accepted");
 changed=f;changed.epoch--;check(!xrfg::LukeOriginContinuity::matches(predicted->frame,changed),"old epoch accepted");
 changed=f;changed.epoch++;check(xrfg::LukeOriginContinuity::matches(predicted->frame,changed),"fresh same-owner temporal reset rejected");
 check(!c.confirm(f,true,1110),"matching returned CRC discarded correct history");
 auto after=frame(93);check(c.predict(1120,after.eye,after.captures,after.contexts).has_value(),"fresh matching CRC did not renew anchor");
 c.used();after.token+=4;after.publication+=4;after.native+=4;
 check(c.confirm(after,true,1120),"mismatched returned CRC did not request history retirement");
 check(!c.predict(1121,after.eye,after.captures,after.contexts),"mismatch left speculative phase armed");
 xrfg::LukeOriginContinuity empty;check(!empty.confirm(frame(),false,0)&&!empty.predict(1,1,{50,51},{10,20}),"partial startup pair armed sequence");
 // Repeated/stale confirmed colours must not extend the lease.
 xrfg::LukeOriginContinuity stale;stale.confirm(frame(),true,0);stale.confirm(frame(1),true,10);
 stale.confirm(frame(1),true,2000);check(!stale.predict(2001,0,frame(2).captures,frame(2).contexts),"stale CRC renewed expired lease");
 xrfg::LukeOriginContinuity expired;expired.confirm(frame(),true,0);expired.confirm(frame(1),true,10);expired.used();
 check(expired.confirm(frame(2),true,1011),"CRC return after expired speculation did not retire old temporal history");
 std::cout<<"GTA one-second continuation: fresh capture cadence, native key, timeout, epoch/geometry, CRC resync and stale rejection PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
