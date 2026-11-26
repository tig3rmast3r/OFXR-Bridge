#include "xrfg/independent_pacing.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static void require(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(int argc,char** argv) {
    require(argc==2,"captured timing fixture path");std::ifstream input(argv[1]);require(bool(input),"read captured timing fixture");
    constexpr std::int64_t p=11'111'128,target=p/2+2'000'000;
    struct State {
        xrfg::IndependentDeliveryReserve reserve;
        xrfg::IndependentSourcePhase phase;
        xrfg::IndependentPairHead head;
        std::unordered_map<std::int64_t,std::int64_t> shifts;
        std::int64_t old_trim{},trim{},peak{},captured_peak{},last_step{},first_step{},old_first_step{};
        unsigned samples{},cuts{},old_cuts{};
    };
    std::array<State,2> states;for(auto& state:states)state.reserve.set_limit(4*p);
    unsigned rows{};std::string line;
    while(std::getline(input,line)) {
        for(char& ch:line)if(ch==',')ch=' ';
        std::istringstream row(line);std::array<std::int64_t,12> r{};
        for(auto& value:r)require(bool(row>>value),"complete captured record");
        require(r[1]>=0 && r[1]<2,"session-scoped record");auto& state=states[r[1]];++rows;
        if(r[0]==4)state.shifts[r[3]]=state.trim-state.old_trim;
        else if(r[0]==1) {
            xrfg::IndependentRecoveryTiming timing{r[5],true,true,bool(r[6])};
            state.reserve.observe_source(r[4],r[3]+r[5],p,timing);
            state.peak=std::max(state.peak,state.reserve.reserve());++state.samples;
        } else if(r[0]==2) {
            const auto shifted=r[5]+state.shifts[r[3]];
            state.head={r[4],shifted,r[6]?0:r[3]};
        } else if(r[0]==3) {
            const auto step=state.phase.observe(r[2],p,2,r[6],r[7],r[8],r[9],r[10],false,&state.head,true);
            if(step) {
                require(step<=p && step<=state.head.entered-state.head.ready-target,"phase change exceeds observed completed-head slack");
                if(!state.first_step)state.first_step=r[2];state.last_step=r[2];++state.cuts;
            }
            if(r[11]){++state.old_cuts;if(!state.old_first_step)state.old_first_step=r[2];}
            state.trim+=step;state.old_trim+=r[11];
        } else if(r[0]==5)state.captured_peak=std::max(state.captured_peak,r[4]);
    }
    require(rows>2500 && states[0].samples>500 && states[1].samples>200,"both complete recorded startup sequences replayed");
    require(states[0].captured_peak==2*p && !states[1].captured_peak,"fixture reproduces learned-tolerance and zero-tolerance entries");
    for(unsigned i=0;i<2;++i) {
        const auto& state=states[i];
        require(!state.peak,"recorded cold overrun/backlog trained multi-second tolerance");
        require(state.first_step && state.first_step<state.old_first_step,"captured startup acquisition still waits for old qualification/smoothing");
        require(2*state.cuts<state.old_cuts,"captured phase still chases many small historical-age corrections");
        std::printf("CAPTURE_REPLAY session=%u samples=%u captured_reserve=%.6fms new_peak=%.6fms phase=%.6fms cuts=%u old_cuts=%u acquisition_earlier=%.6fms (timing model; native slots unchanged)\n",
            i,state.samples,state.captured_peak/1e6,state.peak/1e6,state.trim/1e6,state.cuts,state.old_cuts,(state.old_first_step-state.first_step)/1e6);
    }
    // A repeated tail or delayed old receipt cannot be a second source. A
    // genuinely independent late source still trains the existing bounded policy.
    xrfg::IndependentDeliveryReserve evidence;evidence.set_limit(4*p);
    constexpr std::int64_t base=1'000'000'000;
    evidence.observe_source(base+21'639'296,base,p,{0,true,true,false});
    evidence.observe_source(base+21'639'296,base+p,p,{p,true,true,false});
    require(!evidence.reserve(),"one 2X source counted twice");
    evidence.observe_source(base+4*p,base+2*p,p,{0,true,true,true});
    evidence.observe_source(base+6*p,base+4*p,p,{0,true,true,false});
    require(!evidence.reserve(),"backlog kept the earlier transient as independent evidence");
    evidence.observe_source(base+8*p,base+6*p,p,{0,true,true,false});
    require(evidence.reserve()>0 && evidence.reserve()<=4*p,"genuine independent lateness learning was disabled");
    // Phase acquisition consumes existing surplus before rendering. Simulated
    // native slots and work duration stay fixed; two queued sources are allowed
    // to cross an adjustment before new evidence can earn another one.
    for(unsigned hz:{72U,90U,120U})for(unsigned divisor:{2U,3U,4U}) {
        const std::int64_t refresh=1'000'000'000/hz;
        const auto spare_target=refresh/2+2'000'000;
        xrfg::IndependentSourcePhase phase;std::int64_t trim{},settled{};
        for(unsigned n=0;n<90;++n) {
            const auto at=base+(n+1)*divisor*refresh;
            const auto age=spare_target+refresh-std::min(refresh,trim);
            xrfg::IndependentPairHead head{at-refresh/4,at-refresh/4-age,at};
            trim+=phase.observe(at,refresh,divisor,1,3*refresh,(n+1)*divisor,0,0,false,&head,true);
            if(trim==refresh && !settled)settled=at-base;
        }
        require(trim==refresh && settled<=4*divisor*refresh,"startup phase did not acquire from completed heads promptly");
        std::printf("PHASE_ACQUIRE hz=%u factor=%u recovered=%.6fms settled=%.6fms native slots unchanged\n",hz,divisor,trim/1e6,settled/1e6);
    }
    for(bool loss:{false,true}) {
        xrfg::IndependentSourcePhase phase;
        for(unsigned n=0;n<90;++n) {
            const auto at=base+(n+1)*2*p;
            xrfg::IndependentPairHead head{at-p/4,at-p/4-3*p,at};
            require(!phase.observe(at,p,2,2,4*p,n+1,loss?n:0,0,false,&head,true),
                "half-rate/loss cannot authorize startup phase acquisition");
        }
    }
    xrfg::IndependentSourcePhase jitter;
    for(unsigned n=0;n<90;++n) {
        const auto at=base+(n+1)*2*p;
        xrfg::IndependentPairHead head{at-p/4,at-p/4-(target+p/8),at};
        require(!jitter.observe(at,p,2,2,4*p,2*n,0,0,false,&head,true),
            "smoothed historical age must not shift a fresh head inside the jitter band");
    }
    for(unsigned invalid=0;invalid<4;++invalid) {
        xrfg::IndependentSourcePhase phase;
        for(unsigned n=0;n<12;++n) {
            const auto at=base+(n+1)*2*p;
            xrfg::IndependentPairHead head{at-p/4,at-p/4-3*p,at};
            if(invalid==0)head.source_due=0; // queued source
            if(invalid==1)head.ready=head.entered+p; // incomplete at native End
            if(invalid==2){head.entered-=4*p;head.ready-=4*p;} // stale receipt
            if(invalid==3){head.entered+=p;head.ready+=p;} // future receipt
            require(!phase.observe(at,p,2,2,4*p,2*(n+1),0,0,false,&head,true),
                "queued, incomplete, stale or future head authorized phase acquisition");
        }
    }
}
