#include "xrfg/private_pacing_buffer.hpp"
#include <iostream>
#include <stdexcept>
void need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main() try {
    constexpr std::int64_t p=11'111'111,base=1'000'000'000;
    xrfg::PrivatePacingBuffer buffer;buffer.period(p);
    std::int64_t filled{};
    for(int n=0;n<1600;++n) {
        const auto wanted=base+n*p;
        buffer.observe(wanted+(n%2?0:std::max(std::int64_t{},3'000'000-filled)),wanted);
        const auto increment=buffer.take_refill();
        need(increment>=0 && increment<=500'000,"a noisy window filled more than the legacy step");
        filled+=increment;
        need(buffer.take_refill()==0,"the same reserve was charged to the next ready packet");
    }
    need(filled>=2'500'000 && filled<=3'500'000 && buffer.reserve()==filled,"VDXR learner did not retain the filled reserve");
    for(int n=1600;n<2200;++n) {
        const auto wanted=base+n*p;buffer.observe(wanted,wanted);
        need(buffer.take_refill()==0 && buffer.reserve()==filled,"quiet output decayed or recharged its reserve");
    }
    buffer.period(8'333'333);
    need(buffer.reserve()==0 && buffer.take_refill()==0,"refresh change retained old timing");
    xrfg::PrivatePacingBuffer slow;slow.period(p);
    for(int n=0;n<900;++n){const auto wanted=base+n*p;slow.observe(base+n*p*5/4+(n%2)*2'000'000,wanted);}
    need(slow.reserve()==0 && slow.take_refill()==0,"sustained under-refresh throughput bought useless latency");
    std::cout<<"Legacy learner reuse; bounded one-time refill, retained quiet reserve, slow-input exclusion and refresh reset PASS\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
