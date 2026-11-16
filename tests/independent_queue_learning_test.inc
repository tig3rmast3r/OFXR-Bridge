// Real endpoint packets, already ready, held only by a blocked native End.
// Their own queue residence must not become evidence for more GPU allowance.
void independent_ready_queue_learning_test() {
    const auto saved=period;period=50'000'000;
    independent_fixture_mode=true;independent_fixture_auto=true;
    {
        Fixture f(true,true,{},false,true,false,8);f.throttled=false;
        const auto chain=f.make_chain();f.write(chain,80);
        std::vector<std::shared_ptr<xrfg::InterfaceDeliveryReceipt>> receipts;
        for(unsigned i=0;i<3;++i) {
            f.begin_virtual();f.transport->set_next_independent_deadline(ns()+period);
            auto receipt=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
            xr_ok(f.submit(chain,float(i+1),XR_NULL_HANDLE,receipt),"ready packet accepted before native stall");
            receipts.push_back(receipt);if(!i)f.wait_entered();
        }
        std::this_thread::sleep_for(std::chrono::nanoseconds(3*period));
        f.unblock();xr_ok(f.transport->drain(),"queued ready packets retire after native stall");
        const auto reserve=f.transport->independent_load().reserve;
        check(reserve==0,"ready queue residence trained automatic reserve as GPU lateness");
        check(receipts[0]->outcome==1 && receipts[1]->outcome==-2 && receipts[2]->outcome==-2,
            "expired ready packets must not purchase permission to present older images");
        f.begin_virtual();f.transport->set_next_independent_deadline(ns()+period);
        auto fresh=std::make_shared<xrfg::InterfaceDeliveryReceipt>(false);
        xr_ok(f.submit(chain,4,XR_NULL_HANDLE,fresh),"fresh packet after native stall");
        xr_ok(f.transport->drain(),"fresh recovered packet retires");
        check(fresh->outcome==1 && f.transport->independent_load().reserve==0,
            "queue-only stall damaged future fresh delivery");
        xr_ok(f.transport->destroy_swapchain(chain),"queue learning source cleanup");
    }
    independent_fixture_mode=false;independent_fixture_auto=false;period=saved;
    std::puts("READY_QUEUE no self-trained reserve; expired old packets retire; fresh delivery recovers PASS");
}
