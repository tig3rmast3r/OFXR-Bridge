#include "xrfg/neural_rendering.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {try {
    using namespace xrfg::neural_rendering;
    unsigned checks=0;const auto expect=[&](bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);};
    Settings off;off.enabled=false;off.before_sr=false;
    Settings before;before.enabled=true;before.before_sr=true;
    Settings after;after.enabled=true;after.before_sr=false;
    configure(off);expect(!after_sr_needs_copyback(1,nullptr,nullptr),"disabled post-SR call leaves output unchanged");
    configure(before);expect(!after_sr_needs_copyback(1,nullptr,nullptr),"pre-SR mode leaves post-SR output unchanged");
    configure(after);expect(after_sr_needs_copyback(1,nullptr,nullptr),"selected post-SR path conservatively keeps copyback on refusal");
    configure(off);std::atomic<bool> ready{},done{};std::atomic<unsigned> wrong{};
    std::jthread toggles([&]{ready=true;for(unsigned i=0;i<2000;++i)configure(i%2?off:before);done=true;});
    while(!ready.load())std::this_thread::yield();
    do {if(after_sr_needs_copyback(1,nullptr,nullptr))++wrong;} while(!done.load());
    toggles.join();expect(wrong==0,"mode snapshot and evaluation share the same configuration lock");
    expect(status().evaluations==0,"no model evaluation is fabricated by the copyback query");
    shutdown();std::cout<<"Neural copyback checks="<<checks<<" failures=0; actual NR module, no model/device initialization\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
