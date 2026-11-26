// Pimax PID39332 r10, after85s: exact grid/head/follower/window QPC ns.
// Native predictions advance one physical refresh for every recorded pair.
void spacing_replay_tests() {
 constexpr long long p=11'111'128;
 constexpr std::array<std::array<long long,4>,27> samples{{
  {{261870839060460LL,261870843586900LL,261870846912700LL,261870858018328LL}},
  {{261870861282716LL,261870863978500LL,261870868218900LL,261870879323528LL}},
  {{261870883504972LL,261870884055100LL,261870889174700LL,261870900279328LL}},
  {{261876439068972LL,261876438598700LL,261876444056100LL,261876455158628LL}},
  {{261876505735740LL,261876506378600LL,261876509715000LL,261876520819828LL}},
  {{261876616847020LL,261876619391000LL,261876622975800LL,261876634082028LL}},
  {{261881394632060LL,261881396221100LL,261881401389600LL,261881412493928LL}},
  {{261881505743340LL,261881507031500LL,261881510610400LL,261881521715628LL}},
  {{261881572410108LL,261881573862400LL,261881577618900LL,261881588723728LL}},
  {{261882983523364LL,261882986423500LL,261882991341500LL,261883002447528LL}},
  {{261883005745620LL,261883008851000LL,261883013189000LL,261883024291728LL}},
  {{261884716859332LL,261884720747300LL,261884723997300LL,261884735094028LL}},
  {{261884939081892LL,261884942454100LL,261884947167100LL,261884958272128LL}},
  {{261884983526404LL,261884989477000LL,261884990427700LL,261885001532928LL}},
  {{261885016859788LL,261885007015600LL,261885012246000LL,261885023352228LL}},
  {{261885061304300LL,261885049846000LL,261885054767100LL,261885065873228LL}},
  {{261886461306428LL,261886451234900LL,261886455381000LL,261886466484828LL}},
  {{261886483528684LL,261886474959400LL,261886478552700LL,261886489658128LL}},
  {{261886505750940LL,261886495763300LL,261886499365700LL,261886510467828LL}},
  {{261886527973196LL,261886518725200LL,261886521196600LL,261886532301528LL}},
  {{261891472425156LL,261891464024600LL,261891466956200LL,261891478061628LL}},
  {{261891494647412LL,261891485077300LL,261891489208100LL,261891500313728LL}},
  {{261891516869668LL,261891508612600LL,261891510269100LL,261891521374128LL}},
  {{261892250204116LL,261892241693200LL,261892246752000LL,261892257856628LL}},
  {{261895472431236LL,261895464795900LL,261895468094000LL,261895479199228LL}},
  {{261895494653492LL,261895486059700LL,261895489380200LL,261895500484728LL}},
  {{261895516875748LL,261895507581800LL,261895511825700LL,261895522930028LL}},
 }};
 for(const auto& sample:samples) {
  const auto [target,head,follower,window]=sample;
  xrfg::IndependentDeliveryClock old_policy,protected_policy;
  for(auto* clock:{&old_policy,&protected_policy}) {
   clock->claim(target-p,p);clock->finish(target-p);
   clock->claim(target,p);
  }
  old_policy.finish(head);protected_policy.finish(head,false,true);
  old_policy.claim(follower,p,true);protected_policy.claim(follower,p,true);
  check(old_policy.entry(follower,false)==follower && follower-head<p/2);
  const auto protected_entry=protected_policy.entry(follower,false);
  check(protected_entry==head+p/2);
  check(protected_entry<window-250'000); // same prediction/window; no renewal
  check(protected_policy.entry_in_window(follower,false,window-1'250'000)==protected_entry);
  check(protected_policy.entry_in_window(follower,false,follower+1)==follower);
  check(protected_policy.entry_in_window(follower,false,follower)==protected_entry);
 }
 for(const long long period:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  constexpr long long base=1'000'000'000;
  xrfg::IndependentDeliveryClock clock;
  for(unsigned n=0;n<1000;++n) {
   const auto now=base+n*period;
   clock.claim(now,period);
   check(clock.entry(now,false)==now); // healthy blocking Wait adds no delay
   clock.finish(now,false,true);
  }
  const auto next=base+1000*period;
  clock.claim(next,period);clock.finish(next,false,false);
  check(clock.entry(next+1,false)==next+1); // policy disabled; no stale floor
 }
 std::puts("OPTICAL_SPACING PID39332:27 recorded compressed pairs protected within original native windows;72/90/120Hz healthy handoffs add no wait;disabled policy clears PASS");
}
