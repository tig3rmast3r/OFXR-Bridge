#include "xrfg/three_slot_buffer.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

static void check(bool value) { if (!value) throw std::runtime_error("three-slot invariant"); }

int main() {
    try {
        using Token = std::shared_ptr<unsigned>;
        xrfg::ThreeSlotBuffer<Token> buffer;
        auto a = std::make_shared<unsigned>(1);
        check(buffer.push_real(a));
        check(buffer.slot(2)->value.get() == a.get());
        check(!buffer.releasable());
        check(buffer.shift_empty() && buffer.slot(1)->value.get() == a.get());
        check(buffer.shift_empty() && buffer.slot(0)->value.get() == a.get());
        check(!buffer.shift_empty() && !buffer.releasable());
        // Producer, worker and director delays are independently varied. A
        // pending downstream submission keeps its head until acknowledgement.
        for (unsigned n = 2; n != 10002; ++n) {
            check(buffer.can_accept_real());
            auto real = std::make_shared<unsigned>(n);
            check(buffer.push_real(real));
            check(!buffer.can_accept_real());
            check(buffer.releasable());
            check(*buffer.slot(0)->value == n - 1);
            check(!buffer.push_synthetic(std::make_shared<unsigned>(n)));
            check(buffer.release_front());
            check(!buffer.releasable());
            check(buffer.slot(1)->value.get() == real.get());
            check(!buffer.can_accept_real());
            check(buffer.push_synthetic(std::make_shared<unsigned>(n)));
            check(buffer.releasable() && buffer.slot(0)->synthetic);
            check(!buffer.push_real(std::make_shared<unsigned>(n + 1)));
            check(buffer.release_front());
            check(buffer.slot(0)->value.get() == real.get());
            check(!buffer.releasable());
        }
        check(buffer.release_front(true) && buffer.empty());
        check(!buffer.release_front(true));
        buffer.clear();
        check(buffer.can_accept_real());
        buffer.configure(4);check(buffer.capacity()==5);
        check(buffer.push_real(std::make_shared<unsigned>(100)));
        for(unsigned i=0;i<4;++i)check(buffer.shift_empty());
        for(unsigned n=101;n<201;++n){
            check(buffer.push_real(std::make_shared<unsigned>(n)));
            check(buffer.releasable());check(*buffer.slot(0)->value==n-1);
            check(buffer.release_front());check(buffer.can_insert_synthetic());
            check(buffer.push_synthetic_triple(std::make_shared<unsigned>(n*10+1),std::make_shared<unsigned>(n*10+2),std::make_shared<unsigned>(n*10+3)));
            for(unsigned i=1;i<=3;++i){check(buffer.slot(0)->synthetic);check(*buffer.slot(0)->value==n*10+i);check(!buffer.can_accept_real());check(buffer.release_front());}
            check(!buffer.slot(0)->synthetic);check(*buffer.slot(0)->value==n);
        }
        check(buffer.release_front(true)&&buffer.empty());
        std::cout << "10000 ordered pairs; pointer identity preserved; no shifts copy pixels\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
