void minimum_spacing_rules() {
    constexpr auto P=11'111'111LL;
    float value{};
    InterfaceCycle queued(100'000'000);queued.real_prediction(250'000'000);queued.producer_returned(300'000'000);
    queued.synthetic_accepted(120'250'000,P,120'000'000);
    check(queued.read_minimum_cycle(121'000'000,value) && std::abs(value-31.111111F)<.001F,
        "one-ms S/B acceptance gap still retains one90Hz refresh");
    check(queued.read_minimum_cycle(146'000'000,value) && value==46,
        "already longer observed delivery is retained, never charged another refresh");
    queued.synthetic_accepted(120'250'000,P,120'000'000);
    check(queued.read_minimum_cycle(121'000'000,value) && std::abs(value-31.111111F)<.001F,"duplicate generation receipt cannot add another slot");
    InterfaceCycle triple(100'000'000);
    triple.synthetic_accepted(120'250'000,P,120'000'000);triple.synthetic_accepted(121'250'000,P,121'000'000);
    check(triple.read_minimum_cycle(122'000'000,value) && std::abs(value-42.222222F)<.001F,"3X retains two refresh separations");
    InterfaceCycle late_second(100'000'000);
    late_second.synthetic_accepted(120'250'000,P,120'000'000);late_second.synthetic_accepted(136'250'000,P,136'000'000);
    check(late_second.read_minimum_cycle(137'000'000,value) && std::abs(value-47.111111F)<.001F,
        "late intermediate output retains one remaining refresh without inventing a phase grid");
    InterfaceCycle high_refresh(100'000'000);high_refresh.synthetic_accepted(120'250'000,8'333'333,120'000'000);
    check(high_refresh.read_minimum_cycle(121'000'000,value) && std::abs(value-28.333333F)<.001F,"120Hz uses8.333ms, not a hardcoded11ms");
    InterfaceCycle unknown(100'000'000);unknown.synthetic_accepted(120'000'000,0);
    check(!unknown.read_minimum_cycle(121'000'000,value),"unknown generated-output refresh cannot fabricate a minimum");
    InterfaceCycle changed(100'000'000);changed.synthetic_accepted(120'000'000,P);changed.synthetic_accepted(121'000'000,8'333'333);
    check(!changed.read_minimum_cycle(122'000'000,value),"mid-pair physical refresh change has no consistent bound");
    // A runtime can hold S End after S was already displayed. Entry is the
    // earliest-possible image time; using its return would add a false slot.
    InterfaceCycle native(100'000'000);native.synthetic_accepted(105'000'000,P,104'000'000);
    auto s=std::make_shared<InterfaceDeliveryReceipt>(true),b=std::make_shared<InterfaceDeliveryReceipt>(false);
    s->synthetic_confirmed=true;native.attach_native_delivery(s);native.attach_native_delivery(b);
    s->completed(true,120'000'000,135'000'000,250'000'000);
    check(!native.read_minimum_cycle(110'000'000,value),"virtual real acceptance cannot close a pending native cycle");
    b->completed(true,136'000'000,137'000'000,260'000'000);
    check(native.read_minimum_cycle(110'000'000,value) && value==37,"long S End cannot double-charge already elapsed display spacing");
    InterfaceCycle compressed(100'000'000);compressed.synthetic_accepted(105'000'000,P,104'000'000);
    auto cs=std::make_shared<InterfaceDeliveryReceipt>(true),cb=std::make_shared<InterfaceDeliveryReceipt>(false);
    cs->synthetic_confirmed=true;compressed.attach_native_delivery(cs);compressed.attach_native_delivery(cb);
    cs->completed(true,120'000'000,120'250'000,250'000'000);cb->completed(true,121'000'000,121'250'000,260'000'000);
    check(compressed.read_minimum_cycle(110'000'000,value) && std::abs(value-31.111111F)<.001F,
        "native Framework burst uses native timing, not earlier virtual acceptance or future prediction");
}

void aer4_minimum_cycle() {
    constexpr std::int64_t ms=1'000'000;
    InterfaceCycle cycle(100*ms);
    cycle.producer_returned(108*ms);
    cycle.source_dependency(56*ms);
    auto first_midpoint=std::make_shared<InterfaceDeliveryReceipt>(true,false,true);
    auto first_reference=std::make_shared<InterfaceDeliveryReceipt>(true,true);
    auto second_midpoint=std::make_shared<InterfaceDeliveryReceipt>(true,false,true);
    auto second_reference=std::make_shared<InterfaceDeliveryReceipt>(false,true);
    for(const auto& receipt:{first_midpoint,first_reference,second_midpoint,second_reference}) {
        cycle.attach_native_delivery(receipt);
        receipt->synthetic_confirmed=true;
    }
    first_midpoint->completed(true,195*ms,196*ms,205*ms,191*ms);
    first_reference->completed(true,204*ms,205*ms,214*ms,192*ms);
    cycle.synthetic_accepted(196*ms,11*ms);
    float minimum{};
    check(cycle.read_minimum_cycle(108*ms,minimum) && minimum==150,
        "AER 4X counts the leading midpoint and keeps the measured source dependency");
    second_midpoint->completed(true,217*ms,218*ms,228*ms,210*ms);
    second_reference->completed(true,228*ms,229*ms,239*ms,213*ms);
    cycle.synthetic_accepted(218*ms,11*ms);
    check(cycle.read_minimum_cycle(108*ms,minimum) && minimum==150,
        "the later AER 4X midpoint does not invalidate or extend the first mixed reference");
    float game{},generation{},delivery{};
    check(cycle.stages(104*ms,108*ms,minimum,game,generation,delivery) &&
        generation==131 && delivery==15,
        "AER 4X stage boundaries use the leading midpoint readiness");

    InterfaceCycle ordinary(100*ms);
    ordinary.attach_native_delivery(first_midpoint);
    auto real=std::make_shared<InterfaceDeliveryReceipt>(false);
    ordinary.attach_native_delivery(real);
    ordinary.synthetic_accepted(196*ms,11*ms);
    real->completed(true,194*ms,195*ms,204*ms);
    check(!ordinary.read_minimum_cycle(108*ms,minimum),
        "a generated output after a normal real reference remains out of order");
}
