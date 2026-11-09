#include "xrfg/rife_handoff_schedule.hpp"
#include <iostream>
#include <stdexcept>
static void need(bool value,const char* what){if(!value)throw std::runtime_error(what);}
int main()try{
    unsigned tested=0;
    for(const auto period:{11'111'111LL,8'333'333LL,13'888'889LL})for(const auto rate:{24,30,33,40,45}){
        xrfg::RifeHandoffSchedule schedule;
        constexpr std::int64_t origin=1'000'000'000;
        const auto gate=origin+period/3; // deliberately different from scanout-1ms
        std::int64_t returned=gate+500'000,last{};std::uint64_t serial{};
        for(int pair=0;pair<300;++pair)for(int output=0;output<2;++output){
            const auto ready=origin+pair*(1'000'000'000LL/rate)+(pair%7)*80'000;
            const auto now=std::max(ready,returned);
            const auto target=schedule.claim(period,gate,now);
            need(target.has_value(),"witnessed native gate rejected");
            need(target->entry>=now && target->handoff==target->entry,"native entry preceded its own witnessed gate");
            need(target->slot>serial,"duplicate native reservation");
            need((target->handoff-gate-schedule.submit_guard)%period==0,"handoff ignored native running-start phase");
            if(last)need(target->handoff-last>=period,"short catch-up burst");
            returned=target->handoff+200'000;last=target->handoff;serial=target->slot;++tested;
        }
        if(1'000'000'000LL/rate>=2*period)need(returned<=origin+299*(1'000'000'000LL/rate)+3*period,"native budget bought an extra period per output");
    }
    xrfg::RifeHandoffSchedule s;
    need(!s.claim(11'111'111,0,1'000'000'000),"no gate must retain V953 native fallback");
    need(!s.claim(1'000'000,1'000'000'000,1'000'000'000),"invalid refresh accepted");
    auto a=s.claim(11'111'111,1'000'000'000,1'000'000'000);
    auto b=s.claim(11'111'111,1'000'000'000,a->handoff+200'000);
    need(b->handoff-a->handoff==11'111'111,"cost outlier imposed another refresh");
    auto c=s.claim(11'111'111,1'011'101'111,b->handoff+200'000);
    need(c->handoff-b->handoff==11'101'111,"small earlier gate correction skipped a full slot");
    unsigned old_misses=0,new_shown=0;
    for(const auto period:{8'333'333LL,11'111'111LL,13'888'889LL}){
        xrfg::RifeHandoffSchedule learned;
        const auto gate=1'000'000'000LL+period/3;
        for(int i=0;i<120;++i){
            const auto target=learned.claim(period,gate,gate+i*period);
            const auto end=target->handoff+700'000;
            new_shown+=(end-gate)%period<period/2;
            const auto entry_before_gate=target->entry-2'000'000;
            const auto delta=entry_before_gate-gate;
            const auto old_turn=delta>=0?delta/period:-((-delta+period-1)/period);
            old_misses+=end>gate+old_turn*period+period/2;
        }
    }
    need(new_shown==360 && old_misses==360,"fixture failed to distinguish scanout from native consumption window");
    std::cout<<"RIFE native-gate claims "<<tested<<", 360 modeled consumption windows, no-gate fallback, entry-after-gate negative control and phase correction PASS\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
