#include "xrfg/independent_pacing.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

static void require(bool condition,const char* reason) {
    if(!condition) {std::fprintf(stderr,"FAIL: %s\n",reason);std::exit(1);}
}

static void delayed_completion_receipts() {
    constexpr std::int64_t base=10'000'000'000;
    for(unsigned hz:{72U,90U,120U})for(unsigned factor:{2U,3U,4U}) {
        const std::int64_t p=1'000'000'000/hz,target=p/2+2'000'000;
        xrfg::IndependentSourcePhase phase;
        std::int64_t trim{},first_cut{};
        for(unsigned n=0;n<30;++n) {
            const auto now=base+(n+3)*factor*p;
            // Source admission sees the completed head after a source interval
            // and asynchronous native copy-retirement delay. Native slots and
            // work duration stay fixed; a correction consumes existing slack.
            const auto entered=now-factor*p-9*p/10;
            xrfg::IndependentPairHead head{entered,entered-target-p+trim,now};
            const auto step=phase.observe(now,p,factor,1,3*p,(n+1)*factor,0,0,false,&head,true);
            require(step>=0 && step<=p && step<=entered-head.ready-target,
                "correction exceeds measured completed-head surplus");
            trim+=step;
            if(step && !first_cut)first_cut=(n+1)*factor*p;
        }
        require(trim==p && first_cut<=3*factor*p,
            "delayed completed 3X/4X heads cannot acquire phase");
        std::printf("RECEIPT_FRESHNESS hz=%u factor=%u removed_ms=%.6f first_cut_ms=%.6f\n",
            hz,factor,trim/1e6,first_cut/1e6);

        // Receipt freshness must not weaken the readiness, loss, source-age,
        // physical-progress or jitter checks at any supported multiplier.
        for(unsigned invalid=0;invalid<8;++invalid) {
            xrfg::IndependentSourcePhase veto;
            for(unsigned n=0;n<15;++n) {
                const auto now=base+(n+5)*factor*p;
                auto entered=now-factor*p-9*p/10;
                auto age=target+p;
                if(invalid==2)entered=now-2*factor*p;
                if(invalid==3)entered=now+p;
                if(invalid==7)age=target+p/8;
                xrfg::IndependentPairHead head{entered,entered-age,invalid==0?0:now};
                if(invalid==1)head.ready=entered+p;
                const auto delivered=invalid==6?n+1:(n+1)*factor;
                require(!veto.observe(now,p,factor,1,3*p,delivered,
                    invalid==4?n:0,invalid==5?n:0,false,&head,true),
                    "invalid or insufficient native evidence authorized phase correction");
            }
        }
    }
}

static void captured_3x_plateau(const char* path) {
    std::ifstream input(path);
    require(bool(input),"open captured r16 receipt fixture");
    constexpr std::int64_t p=11'111'128,target=p/2+2'000'000;
    xrfg::IndependentSourcePhase phase;
    std::string line;
    unsigned rows{},old_age_vetoes{},corrections{};
    std::int64_t first{},first_cut{},largest{};
    while(std::getline(input,line)) {
        for(auto& ch:line)if(ch==',')ch=' ';
        std::istringstream record(line);
        std::array<std::int64_t,7> r{};
        for(auto& value:r)require(bool(record>>value),"complete captured receipt");
        const auto now=r[0];
        xrfg::IndependentPairHead head{r[1],r[2],r[3]};
        if(!first)first=now;
        ++rows;
        if(now-head.entered>3*p)++old_age_vetoes;
        const auto step=phase.observe(now,p,3,1,0,r[4],r[5],r[6],false,&head,true);
        if(step) {
            require(step<=p && step<=head.entered-head.ready-target,
                "captured correction exceeds completed-head surplus");
            if(!first_cut)first_cut=now;
            largest=std::max(largest,step);
            ++corrections;
        }
    }
    require(rows>=150 && old_age_vetoes>=150,"fixture lacks the recorded 3X plateau");
    require(first_cut && first_cut-first<500'000'000,
        "captured completed 3X heads remain blocked by receipt freshness");
    std::printf("CAPTURED_RECEIPTS rows=%u old_age_vetoes=%u eligible_corrections=%u first_cut_after_ms=%.6f largest_ms=%.6f\n",
        rows,old_age_vetoes,corrections,(first_cut-first)/1e6,largest/1e6);
    // Original native slots and measurements are replayed without simulating
    // new GPU work or deadlines. This establishes correction eligibility and
    // bounds, not a counterfactual headset stabilization duration.
}

int main(int argc,char** argv) {
    require(argc==2,"captured r16 receipt fixture path");
    captured_3x_plateau(argv[1]);
    delayed_completion_receipts();
    std::puts("Completed-receipt freshness: captured 3X plateau, asynchronous retirement, measured surplus and protected evidence checks PASS");
}
