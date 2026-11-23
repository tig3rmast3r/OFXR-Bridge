#include "xrfg/ofr_reconstruction.hpp"
#include <iostream>
#include <stdexcept>
using namespace xrfg::ofr;
void check(bool value) {if(!value)throw std::runtime_error("OFR source policy mismatch");}
int main() {
    try {
        for(bool swap:{false,true})for(bool paired:{false,true}) {
            std::array<Source,2> previous{{{11,101,1},{22,202,1}}};
            unsigned outputs=0;
            for(unsigned tick=0;tick<600;++tick) {
                auto current=previous;
                const unsigned eye=(tick+newer_eye(swap))%2;
                if(paired){++current[0].serial;++current[1].serial;}else ++current[eye].serial;
                auto d=classify(previous,current);
                check(d.continuity && d.fresh==(paired?3:1u<<eye));
                check(d.output_count()==(paired?2u:1u));outputs+=d.output_count();
                // Extra EndFrames with the same sources cannot shift phase.
                for(unsigned repeat=0;repeat<tick%5;++repeat) {
                    auto same=classify(current,current);check(same.continuity && same.output_count()==0);
                }
                previous=current;
            }
            check(outputs==(paired?1200u:600u));
            auto next=previous;next[0].serial+=3;check(!classify(previous,next).continuity);
            next=previous;--next[1].serial;check(!classify(previous,next).continuity);
            next=previous;++next[0].epoch;check(!classify(previous,next).continuity);
            next=previous;next[0].serial=0;check(!classify(previous,next).continuity);
        }
        check(newer_eye(false)==1 && newer_eye(true)==0);
        for(bool paired:{false,true}) {
            SourceTracker tracker;
            std::array<Source,2> source{{{11,101,1},{22,202,1}}};
            check(tracker.observe(source).fresh==3);
            for(unsigned tick=0;tick<600;++tick) {
                const unsigned mask=paired?3:1u<<(tick%2);
                for(unsigned eye=0;eye<2;++eye)if(mask&(1u<<eye))++source[eye].serial;
                const auto d=tracker.observe(source);
                check(d.fresh==mask && d.continuity);
                // Generation may retire all its anchors after any failed job.
                // The observed source cursor must not become an empty pair.
                for(unsigned repeat=0;repeat<tick%4;++repeat)
                    check(tracker.observe(source).fresh==0);
            }
            source[0].serial+=4;
            check(!tracker.observe(source).continuity);
            ++source[1].serial;
            check(tracker.observe(source).fresh==2);
            ++source[0].serial;
            check(tracker.observe(source).continuity);
            auto invalid=source;invalid[0].serial=0;
            check(!tracker.observe(invalid).continuity);
            check(tracker.observe(source).fresh==3);
        }
        std::cout<<"OFR paired/alternating, swaps, 2400 steps, repeats, gaps and epochs PASS\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
