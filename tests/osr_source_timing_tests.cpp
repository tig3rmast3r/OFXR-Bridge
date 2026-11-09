#include "xrfg/osr_source_timing.hpp"
#include <cmath>
#include <stdexcept>
#include <cstdio>
void require(bool value) { if(!value)throw std::runtime_error("source timing contract"); }
int main() {
    using xrfg::OsrSourceTime;
    const OsrSourceTime older{7,100,100000,1000000}, peer{7,101,108000,1000000}, now{7,102,120000,1000000};
    const auto f=xrfg::osr_source_midpoint(now,peer,older);
    require(f && std::abs(*f-.6F)<1e-6F);
    const OsrSourceTime previous_peer{7,99,88000,1000000};
    const auto a=xrfg::osr_source_history_advance(now,peer,previous_peer);
    require(a && std::abs(*a-.6F)<1e-6F);
    for(unsigned kind=0;kind<7;++kind) {
        auto broken=peer;
        switch(kind) {
        case 0:broken.owner=8;break;
        case 1:broken.sequence=100;break;
        case 2:broken.ticks=0;break;
        case 3:broken.ticks=120001;break;
        case 4:broken.frequency=999;break;
        case 5:broken.ticks=100500;break;
        case 6:broken.ticks=101500;break; // extreme ratio
        }
        require(!xrfg::osr_source_midpoint(now,broken,older));
    }
    auto pause=now;pause.ticks+=100000;
    require(!xrfg::osr_source_midpoint(pause,peer,older));
    require(!xrfg::osr_source_history_advance(pause,peer,previous_peer));
    auto large=older, middle=peer, current=now;
    large.ticks+=9000000000000000ULL;middle.ticks+=9000000000000000ULL;current.ticks+=9000000000000000ULL;
    require(xrfg::osr_source_midpoint(current,middle,large)==f);
    std::puts("Exact source timing: asymmetric intervals, identity, pause/burst/ratio refusal and large QPC PASS");
}
