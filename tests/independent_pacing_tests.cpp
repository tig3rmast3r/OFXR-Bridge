#include "xrfg/independent_pacing.hpp"
#include <cstdlib>
#include <cstdio>
#include <array>
#include <deque>
#include <vector>
#define check(condition) do {if(!(condition)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition);std::abort();}}while(false)
void automatic_buffer_tests();
void source_phase_tests();
void aer_copy_overlap_tests();
void stereo_recovery_tests();
void batch_deadline_tests();
static void osr_pair_phase_tests() {
 for(const std::int64_t period:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  xrfg::IndependentSourcePhase phase;
  const auto margin=std::min<std::int64_t>(1'000'000,period/8);
  constexpr std::int64_t base=1'000'000'000;
  std::int64_t shift=0;
  std::uint64_t delivered=0;
  auto now=base;
  xrfg::IndependentPairHead head{};
  // At each source appointment the previous pair has already retired. Keep
  // the native S/B slots fixed while the next source gradually starts later.
  for(unsigned pair=0;pair<300;++pair) {
   now+=2*period;delivered+=2;
   head.entered=now-period/4;
   head.ready=head.entered-(period+margin-shift);
   const auto step=phase.observe(now,period,2,0,2*period,delivered,0,0,false,&head);
   check(step>=0 && step<=200'000 && head.ready+step<=head.entered-margin);
   shift+=step;
  }
  check(shift==period && phase.trimmed()==period);
  for(bool tagged:{false,true})for(bool distinct:{false,true}) {
   xrfg::IndependentDeliveryClock grid;
   grid.claim(base,period);grid.finish(base);
   check(grid.claim(base+1,period)==base+period);
   const auto late=base+period+3*period/4;
   grid.finish(late,tagged);
   grid.claim(late+1,period); // before the prefetched Wait is consumed
   const auto target=grid.claim(late+2,period,distinct);
   if(tagged && distinct) {
    check(target==base+2*period);
    const auto real=grid.entry(late+2);
    check(real==late+period/2 && grid.entry(late+2,false)==real);
    grid.finish(real);
    check(grid.claim(real+1,period)==base+3*period);
   } else check(target==base+3*period);
  }
  // A long residence of the real, stale observations, GPU completion after
  // native entry, and missed deliveries cannot earn another adjustment.
  const auto observe=[&](std::uint64_t idle) {
   return phase.observe(now,period,2,3,4*period,delivered,0,idle,false,&head);
  };
  now+=2*period;delivered+=2;check(!observe(0)); // same receipt
  now+=2*period;delivered+=2;
  head={now-4*period,now-6*period};check(!observe(0)); // newly seen but stale
  now+=2*period;delivered+=2;
  head={now-period,now-period/2};check(!observe(0));
  now+=2*period;delivered+=1;
  head={now-period,now-3*period};check(!observe(1));
  // A long run below refresh cannot trim, even with a full reported queue.
  phase.reset();
  for(unsigned n=0;n<300;++n) {
   now+=2*period;++delivered;head={now-period,now-3*period};
   check(!observe(1));
  }
  std::printf("OSR stereo pair: recovered %.3fms with zero pending at source appointments, stable native slots\n",shift/1e6);
 }
}
int main(){
 source_phase_tests();
 aer_copy_overlap_tests();
 stereo_recovery_tests();
 batch_deadline_tests();
 osr_pair_phase_tests();
 // Captured-shaped startup backlog: older work completes in19ms and the
 // current job in44ms, followed by25ms/source. Keep exactly the same GPU
 // service and output schedule. A source budget can still remove19ms of
 // steady source age by admitting the next render later, not its output.
 const auto pipeline=[](bool bounded) {
  std::array<std::array<long long,2>,512> samples{};
  std::deque<long long> gpu{19,44};long long previous_end=0,tail=44;
  for(auto& sample:samples) {
   const auto requested=previous_end;auto start=requested;
   while(!gpu.empty() && gpu.front()<=start)gpu.pop_front();
   while(bounded && !xrfg::independent_source_budget_available(gpu.size(),false,0,false)) {
    start=gpu.front();gpu.pop_front();
   }
   const auto rendered=start+25;tail=std::max(tail,rendered)+25;
   gpu.push_back(tail);sample={tail,tail-requested};previous_end=rendered;
  }
  return samples;
 };
 const auto old_pipeline=pipeline(false),bounded_pipeline=pipeline(true);
 for(std::size_t i=0;i<old_pipeline.size();++i) {
  check(old_pipeline[i][0]==bounded_pipeline[i][0]); // same frames, same delivery times
  if(i){check(old_pipeline[i][1]==69);check(bounded_pipeline[i][1]==50);}
 }
 std::puts("SOURCE_BUDGET_MODEL unchanged25ms output cadence,512 outputs; steady input age69->50ms PASS (model,not game evidence)");
 for(auto p:{8'333'333LL,11'111'111LL,13'888'889LL})for(unsigned factor:{1U,2U,3U,4U}){
  xrfg::IndependentSourceClock clock;long long now=1'000'000'000,previous=0;
  for(int i=0;i<1000;++i){auto at=clock.claim(now,p,factor);if(previous)check(at-previous==p*factor);previous=at;now=at+100;}
  auto late=clock.claim(previous+p*factor*5+123,p,factor);
  check(late==previous+p*factor*5+123);
  check(clock.claim(late+1,p,factor)==late+p*factor);
  clock.reset();check(clock.claim(late,p,1)==late);
 }
 const long long p=11'111'111,base=1'000'000'000;
 check(xrfg::independent_first_deadline(base,p,base)==base+p);
 check(xrfg::independent_first_deadline(base+1,p,base)==base+2*p);
 check(xrfg::independent_first_deadline(base+3*p+1,p,base+8*p)==base+5*p);
 check(!xrfg::independent_expired(base+p,base+p,0,p));
 check(!xrfg::independent_expired(base+p,base+p+180'000,0,p));
 check(xrfg::independent_expired(base+p,base+2*p,0,p));
 check(!xrfg::independent_expired(base+p,base+2*p,p,p));
 check(xrfg::independent_expired(base+p,base+3*p,p,p));
 check(xrfg::independent_window_end(base,base+3*p,p)==base+p);
 check(xrfg::independent_window_end(base,base+p/2,p)==base+p/2);
 check(xrfg::independent_window_end(base,0,p)==base+p);
 check(xrfg::independent_window_end(base,base-p,p)==base-p);
 xrfg::IndependentDeliveryReserve high;high.set_limit(3*p);
 high.observe(base+3*p+p*3/5,base+p,p);high.observe(base+3*p+p*3/5,base+p,p);
 check(high.reserve()==3*p);
 // Captured DetNoir pattern: late S (~2.8P) alternates with B (~1.8P).
 // Low must earn its ONE period, even though it cannot cover every miss.
 // The selected limit is a ceiling, never a loading/stall classifier.
 for(auto physical:{8'333'333LL,11'111'111LL,13'888'889LL})for(unsigned cap=0;cap<=3;++cap) {
  xrfg::IndependentDeliveryReserve captured;captured.set_limit(cap*physical);
  for(unsigned pair=0;pair<32;++pair) {
   for(auto late:{physical*28/10,physical*18/10}) {
    captured.observe(base+late,base+captured.reserve(),physical);
    check(captured.reserve()<=cap*physical);
   }
   check(captured.reserve()==cap*physical);
  }
  check(captured.take_refill()==cap*physical && !captured.take_refill());
  captured.observe(base+captured.reserve()+9*physical,base+captured.reserve(),physical);
  check(captured.reserve()==cap*physical && !captured.take_refill()); // pause holds learned margin
  captured.set_limit(0);
  for(int i=0;i<64;++i)captured.observe(base+physical*3,base,physical);
  check(captured.reserve()==0 && !captured.take_refill());
 }
 for(unsigned queued=0;queued<4;++queued)for(bool submit:{false,true})
  for(unsigned ready_queue=0;ready_queue<4;++ready_queue)for(bool completing:{false,true})
   check(xrfg::independent_source_budget_available(queued,submit,ready_queue,completing)==
       (queued+unsigned(submit)+ready_queue+unsigned(completing)<2));
 xrfg::IndependentSourceClock clock;check(clock.claim(base,p,2)==base);
 clock.defer_to(base+6*p);check(clock.claim(base+6*p+1,p,2)==base+8*p);
 check(xrfg::independent_recovery_delay(2,2,p,0)==0);
 check(xrfg::independent_recovery_delay(4,2,p,0)==2*p);
 check(xrfg::independent_recovery_delay(8,2,p,0)==6*p);
 check(xrfg::independent_recovery_delay(3,1,p,p/2)==2*p-p/2);
 // Owner packet12846 was prepared and discarded BEFORE its due timestamp.
 check(!xrfg::independent_expired(166666720271833LL,166666715211900LL,0,p));
 for(auto physical:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  xrfg::IndependentDeliveryClock grid;
  check(grid.claim(base,physical)==base);
  grid.finish(base); // first native handoff seeds the phase only once
  long long last=base;
  for(int i=1;i<=3000;++i) {
   const auto target=grid.claim(last+1,physical);
   check(target==base+i*physical);
   const auto entry=grid.entry(last+1);
   check(entry==target-grid.lead() && grid.lead()==300'000);
   const auto overhead=150'000+(i%7)*30'000; // measured-scale Wait/Begin/Release cost
   last=entry+overhead;
   grid.finish(last);
  }
  check(grid.target()==base+3001*physical); // no accumulated +0.2ms/frame
  const auto delayed=last+7*physical;
  auto recovered=grid.claim(delayed,physical);
  check(recovered>=delayed-physical/2 && recovered<=delayed+physical/2);
  grid.finish(delayed);
  check(grid.claim(delayed+1,physical)>=delayed+physical/2); // no short catch-up burst
 }
 xrfg::IndependentDeliveryReserve reserve;reserve.set_limit(p);
 for(int i=0;i<256;++i)reserve.observe(base,base+p,p);
 check(!reserve.reserve() && !reserve.take_refill());
 for(int i=0;i<64;++i)reserve.observe(base+p+180'000,base+p,p);
 check(!reserve.reserve()); // same-slot scheduling jitter is not a miss
 reserve.observe(base+2*p+180'000,base+p,p);check(!reserve.reserve());
 reserve.observe(base+2*p+180'000,base+p,p);check(reserve.reserve()==p);
 check(reserve.take_refill()==p && !reserve.take_refill());
 const auto before=reserve.reserve();for(int i=0;i<128;++i)reserve.observe(base,base+p,p);
 check(reserve.reserve()==before); // observations alone cannot lower allowance
 for(int i=0;i<256;++i)reserve.observe(base+2*p,base+p,p);
 check(reserve.reserve()<=p);
 const auto held=reserve.reserve();reserve.observe(base+10*p,base+p,p);check(reserve.reserve()==held);
 for(int frames=0;frames<=3;++frames) {
  xrfg::IndependentDeliveryReserve capped;capped.set_limit(frames*p);
  for(int i=0;i<100;++i){capped.observe(base+2*p,base+p,p);check(capped.reserve()<=frames*p);}
  check(capped.reserve()==frames*p);
  check(capped.take_refill()<=frames*p && !capped.take_refill());
  capped.set_limit(p);check(capped.reserve()<=p);
  capped.set_limit(0);check(!capped.reserve() && !capped.take_refill());
  for(int i=0;i<64;++i)capped.observe(base+2*p,base+p,p);
  check(!capped.reserve() && !capped.take_refill());
  capped.set_limit(3*p);capped.observe(base+2*p,base+p,p);capped.observe(base+2*p,base+p,p);
  capped.set_limit(0);check(!capped.take_refill());
 }
 automatic_buffer_tests();
 for(unsigned factor=1;factor<=4;++factor)for(unsigned extra=0;extra<=4;++extra)
  for(unsigned ready=0;ready<=16;++ready)for(unsigned native=0;native<=8;++native)
   check(xrfg::independent_delivery_budget_available(0,ready,native,factor,extra*p,p)==
       (ready+native+factor<=2*factor+extra));
 check(!xrfg::independent_delivery_budget_available(0,0,2,1,0,p));
 check(xrfg::independent_delivery_budget_available(0,0,1,1,0,p));
 check(xrfg::independent_delivery_budget_available(0,0,2,1,p,p));
 check(!xrfg::independent_delivery_budget_available(1,2,1,2,0,p));
 std::puts("DELIVERY_BUDGET 1X..4X: GPU batches plus ready/native ownership; two-batch baseline and bounded earned credits PASS");
 std::puts("Independent source clock:72/90/120Hz,1X..4X,late return/no rounding skip/no burst/reset PASS");
}
struct RecoverySample {long long at{},age{};};
std::vector<RecoverySample> transient_queue_model(long long p,bool recover,unsigned factor=2,bool split_aer=false,bool optical_stereo=false) {
 struct Frame {long long ready{},origin{};unsigned id{};long long offset{};};
 constexpr long long base=1'000'000'000;
 std::deque<Frame> queue;
 for(unsigned n=0;n<4;++n)queue.push_back({base-p/2,base-p,n});
 unsigned produced=4,delivered=0,idle=0,pauses=0;
 long long residence=0,native=base,previous_source=0;
 xrfg::IndependentSourceClock clock;xrfg::IndependentSourcePhase phase;
 auto source=clock.claim(base,p,factor);
 std::vector<RecoverySample> out;
 unsigned slot=0;
 const auto consume=[&] {
  // Two finite missed native appointments leave queued age, not a permanent
  // extra generator cost. Afterwards the runtime has its original capacity.
  if(slot==static_cast<unsigned>(2'000'000'000/p) || slot==static_cast<unsigned>(4'000'000'000/p)) {
   ++idle;++pauses;
  } else if(!queue.empty() && queue.front().ready<=native) {
   auto frame=queue.front();queue.pop_front();check(frame.id==delivered++);
   const auto age=xrfg::independent_queue_residence(native,frame.ready,p,frame.offset);
   residence=residence?residence+(age-residence)/16:age;
   out.push_back({native-base,native-frame.origin});
  } else ++idle;
  ++slot;native+=p;
 };
 while(native<base+12'000'000'000LL) {
  if(source<=native) {
   unsigned ready=0;for(const auto& frame:queue)if(frame.ready<=source)++ready;
   const auto step=recover?phase.observe(source,p,factor,ready,residence,delivered,0,idle,split_aer,nullptr,optical_stereo):0;
   check(step>=0 && step<=(optical_stereo?500'000:200'000) &&
       step<=factor*p*(optical_stereo?24:8)/1000+(optical_stereo?14'000:2'000));
   clock.postpone(step);const auto actual=source+step;
   if(previous_source)check(actual-previous_source>=factor*p); // never a producer catch-up
   previous_source=actual;
   while(native<actual)consume();
   for(unsigned n=0;n<factor;++n)queue.push_back({actual+p/2,actual,produced++,optical_stereo?n*p:0});
   source=clock.claim(actual+100,p,factor);
  }else consume();
 }
 check(pauses==2 && idle==2); // all other native appointments deliver fresh work
 while(!queue.empty())consume();
 check(delivered==produced); // no lost/invented/reordered frame IDs
 return out;
}
void source_phase_tests() {
 for(const auto p:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  const auto old=transient_queue_model(p,false),recovered=transient_queue_model(p,true);
  const auto age=[](const auto& values,long long first,long long last) {
   std::vector<long long> window;for(auto sample:values)if(sample.at>=first && sample.at<last)window.push_back(sample.age);
   check(!window.empty());std::sort(window.begin(),window.end());return window[window.size()/2];
  };
  const auto old_late=age(old,10'000'000'000LL,11'000'000'000LL);
  const auto recovered_late=age(recovered,10'000'000'000LL,11'000'000'000LL);
  std::printf("TRANSIENT_QUEUE Hz=%.1f old=%.3fms recovered=%.3fms improvement=%.3fms;every output ID/native appointment retained\n",1e9/p,old_late/1e6,recovered_late/1e6,(old_late-recovered_late)/1e6);
  check(old_late-recovered_late>=2*p);
  check(recovered_late<3*p);
  for(unsigned factor:{2U,3U,4U}) {
   const auto fast_stereo=transient_queue_model(p,true,factor,false,true);
   check(age(fast_stereo,10'000'000'000LL,11'000'000'000LL)<(factor+1)*p);
  }
  std::printf("FAST_STEREO_FIFO Hz=%.1f: normalized batch slots,125ms qualification,500us/24ms-per-second bounded steps; repeated transients; every output ID/native appointment retained PASS (model)\n",1e9/p);
  for(unsigned factor:{1U,3U,4U}) {
   const auto baseline=transient_queue_model(p,false,factor);
   const auto multiframe=transient_queue_model(p,true,factor);
   const auto before=age(baseline,10'000'000'000LL,11'000'000'000LL);
   const auto after=age(multiframe,10'000'000'000LL,11'000'000'000LL);
   check(before-after>=p);
   std::printf("MULTIFRAME_RECOVERY factor=%u Hz=%.1f old=%.3fms recovered=%.3fms; all output IDs/native appointments retained\n",factor,1e9/p,before/1e6,after/1e6);
  }
  for(unsigned factor:{1U,2U,4U}) {
   const auto baseline=transient_queue_model(p,false,factor);
   const auto aer=transient_queue_model(p,true,factor,true);
   const auto before=age(baseline,10'000'000'000LL,11'000'000'000LL);
   const auto after=age(aer,10'000'000'000LL,11'000'000'000LL);
   check(before-after>=p);
   std::printf("AER_BAND_FIFO factor=%u Hz=%.1f old=%.3f recovered=%.3fms;exact IDs/native slots retained\n",factor,1e9/p,before/1e6,after/1e6);
  }
  xrfg::IndependentSourcePhase phase;
  constexpr long long base=1'000'000'000;
  const auto gap_interval=static_cast<unsigned>(1'000'000'000LL/(2*p))+1;
  long long shift=0;
  for(unsigned n=0;n<1200;++n) {
   // Isolated zero-ready and idle samples every~1s used to reset the whole
   // 2s qualification. Native progress is otherwise full rate.
   const bool gap=n%gap_interval==0;
   const auto step=phase.observe(base+n*2*p,p,2,gap?0:2,3*p,2*n-n/gap_interval,0,n/gap_interval);
   check(step>=0 && step<=200'000);
   if(gap)check(!step);
   shift+=step;
  }
  check(phase.trimmed()==shift && shift>2*p); // no lifetime one-P lockout
  const auto before=phase.trimmed();
  check(!phase.observe(base+2500*p,p,2,2,3*p,2402,0,42)); // pause invalidates stale qualification
  check(phase.trimmed()==before);
  for(unsigned divisor:{1U,5U})check(!phase.observe(base+2600*p,p,divisor,4,4*p,2404,0,42));
  check(!phase.trimmed());
  for(unsigned ready:{0U,1U}) {
   phase.reset();
   for(unsigned n=0;n<1200;++n)check(!phase.observe(base+n*2*p,p,2,ready,3*p,2*n,0,0));
  }
  phase.reset();
  for(unsigned n=0;n<1200;++n)check(!phase.observe(base+n*2*p,p,2,2,3*p,2*n,n/8,0));
  phase.reset();
  for(unsigned n=0;n<1200;++n)check(!phase.observe(base+n*2*p,p,2,2,p/2+2'000'000,2*n,0,0));
 }
 std::puts("SOURCE_PHASE_RECOVERY 72/90/120Hz:repeated transient queues drain with no extra native misses or lost IDs;500ms evidence,8ms/s,200us steps,two-ready/current-loss/pause/overload veto PASS (model,not game)");
}
void batch_deadline_tests() {
 for(const long long p:{8'333'333LL,11'111'111LL,13'888'889LL})for(unsigned factor:{2U,3U,4U}) {
  constexpr long long base=1'000'000'000;
  xrfg::IndependentBatchDeadlines slots;
  long long tail=0;
  for(unsigned n=0;n<500;++n) {
   // Source jitter either side of the native rounding boundary produces
   // factor-1/factor+1 jumps despite a constant average source cadence.
   const auto candidate=base+n*factor*p+(n%2?0:p);
   const auto first=slots.claim(candidate,p,factor);
   check(!tail || first>=tail+p);
   check(first==base+n*factor*p+p); // no accumulated artificial slot drift
   tail=first+(factor-1)*p;
  }
  const auto after_loading=tail+100*p;
  check(slots.claim(after_loading,p,1)==after_loading);
  check(slots.claim(after_loading+p,p,factor)==after_loading+p); // prime reserves one output
  slots.reset();check(slots.claim(base,p,factor)==base);
 }
 // PID47564: previous real and next synthetic both had this same due time.
 // The synthetic completed0.605ms late, within the5.556ms tolerance, but its
 // first available native cycle was11.186ms late. It was falsely discarded.
 constexpr long long p=11'111'128,prior_first=260272992257280;
 const auto collision=prior_first+p,first_native=260273014554400;
 xrfg::IndependentBatchDeadlines slots;
 check(slots.claim(prior_first,p,2)==prior_first);
 const auto corrected=slots.claim(collision,p,2);
 check(corrected==collision+p);
 check(xrfg::independent_expired(collision,first_native,0,p));
 check(!xrfg::independent_expired(corrected,first_native,0,p));
 check(slots.claim(260273036701792,p,2)==260273036701792);
 std::puts("BATCH_DEADLINES PID47564 collision negative/recovery;2X/3X/4X jitter,prime,pause,reset;distinct expiry slots without handoff waits PASS");
}
void stereo_recovery_tests() {
 for(const long long p:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  constexpr long long base=1'000'000'000;
  const auto target=p/2+2'000'000;
  // One completed FIFO head, peer in native End. Full physical progress
  // continues after a transient, but the old two-pending rule cannot recover.
  const auto run=[&](bool enable) {
   xrfg::IndependentSourcePhase phase;
   xrfg::IndependentDeliveryReserve reserve;reserve.set_limit(4*p);
   reserve.observe(base+2*p,base,p);reserve.observe(base+2*p,base,p);
   check(reserve.reserve()==2*p);
   long long at=base,removed=0,first_zero=0;
   for(unsigned n=0;n<800;++n) {
    at+=2*p;
    const auto age=std::max(target,target+p-removed);
    const auto step=phase.observe(at,p,2,1,age,2*n,0,0,false,nullptr,enable);
    check(step>=0 && step<=(enable?500'000:200'000));
    removed+=step;
    // Apply the production age guard; two delivered physical outputs per
    // source keep the reserve learner's real-rate evidence independent.
    for(unsigned i=0;i<2;++i)if(age<=target) {
     const auto entered=at+i*p;
     const auto due=entered+p/2;
     reserve.delivered(entered-age,due+reserve.reserve(),entered,p,enable);
     if(!reserve.reserve() && !first_zero)first_zero=entered-base;
    }
   }
   if(enable)check(removed>=p && reserve.reserve()==0 && first_zero<2'500'000'000);
   else check(removed==0 && reserve.reserve()==2*p);
   return removed;
  };
  run(false);run(true);
  // R7-shaped stable S/R pair: ~6-7ms followed by that plus one refresh.
  // Counting the deliberate second slot keeps the old age above its guard
  // and drives needless phase corrections. Correct accounting must settle
  // without source cuts, and return2P allowance in under1.5 seconds.
  for(bool corrected:{false,true}) {
   xrfg::IndependentSourcePhase phase;
   xrfg::IndependentDeliveryReserve allowance;allowance.set_limit(4*p);
   allowance.observe(base+2*p,base,p);allowance.observe(base+2*p,base,p);
   long long smooth=0,first_zero=0;
   for(unsigned n=0;n<240;++n) {
    const auto entered=base+(n+1)*p;
    const auto baseline=p/2+700'000+std::int64_t(n%5)*20'000;
    const auto offset=n%2?p:0;
    const auto raw=baseline+offset;
    const auto age=xrfg::independent_queue_residence(entered,entered-raw,p,corrected?offset:0);
    smooth=smooth?smooth+(age-smooth)/16:age;
    if(smooth<=target)allowance.delivered(entered-raw,entered+150'000+allowance.reserve(),entered,p,corrected);
    if(!allowance.reserve() && !first_zero)first_zero=entered-base;
    if(n%2) {
     const auto step=phase.observe(entered,p,2,1,smooth,n+1,0,0,false,nullptr,true);
     if(corrected)check(!step);
    }
   }
   if(corrected) {
    check(allowance.reserve()==0 && first_zero<1'500'000'000 && !phase.trimmed());
    std::printf("STABLE_BATCH_ACCOUNTING Hz=%.1f allowance reaches zero in %.3fms; no needless phase cuts PASS (model)\n",1e9/p,first_zero/1e6);
   }
   else check(allowance.reserve()>0 && phase.trimmed()>0);
  }
  check(xrfg::independent_queue_residence(base,base+p,p,p)==0);
  check(xrfg::independent_queue_residence(base+3*p,base,p,p)==2*p);
  check(xrfg::independent_queue_residence(base+3*p,base,p)==3*p);
  // Preserve current-frame safety even at the faster slope: a queued late
  // frame must remain eligible, and half-rate delivery cannot qualify cuts.
  for(bool half_rate:{false,true}) {
   xrfg::IndependentDeliveryReserve allowance;allowance.set_limit(4*p);
   allowance.observe(base+3*p,base,p);allowance.observe(base+3*p,base,p);
   long long entered=base;
   for(unsigned n=0;n<1000;++n) {
    entered+=(half_rate?2:1)*p;
    const auto due=half_rate?entered+p:entered-3*p;
    allowance.delivered(entered-p,due+allowance.reserve(),entered,p,true);
    check(!xrfg::independent_expired(due,entered,allowance.reserve(),p));
   }
   check(half_rate?allowance.reserve()==3*p:allowance.reserve()>3*p-p/2);
  }
  // Disabled/invalid multipliers retain the existing policy.
  for(unsigned factor:{1U,5U}) {
   xrfg::IndependentSourcePhase old_policy,opted;
   for(unsigned n=0;n<500;++n) {
    const auto at=base+n*factor*p;
    check(old_policy.observe(at,p,factor,1,3*p,n*factor,0,0)==
          opted.observe(at,p,factor,1,3*p,n*factor,0,0,false,nullptr,true));
   }
  }
  // No chase inside ordinary copy/refresh jitter, even with many pending
  // copies. Only completed heads count; empty and losing cadence veto cuts.
  for(unsigned count:{0U,1U}) {
   xrfg::IndependentSourcePhase phase;
   for(unsigned n=0;n<500;++n)
    check(!phase.observe(base+n*2*p,p,2,count,target+p/8,2*n,0,0,false,nullptr,true));
  }
  for(bool idle:{false,true}) {
   xrfg::IndependentSourcePhase phase;
   for(unsigned n=0;n<500;++n)
    check(!phase.observe(base+n*2*p,p,2,1,3*p,2*n,idle?0:n,idle?n:0,false,nullptr,true));
  }
  // Disarm on a new loss or mode change; do not reuse old qualification.
  xrfg::IndependentSourcePhase phase;
  for(unsigned n=0;n<100;++n)phase.observe(base+n*2*p,p,2,1,3*p,2*n,0,0,false,nullptr,true);
  check(phase.trimmed()>0);
  check(!phase.observe(base+200*p,p,2,1,3*p,200,1,0,false,nullptr,true));
  check(!phase.observe(base+202*p,p,2,1,3*p,202,1,0));
  check(phase.trimmed()==0);
  // Fast native stereo and AER both permit one ready head, but must not
  // share qualification when switching between their different policies.
  for(unsigned n=0;n<100;++n)phase.observe(base+(300+n*2)*p,p,2,1,3*p,300+2*n,1,0,false,nullptr,true);
  check(phase.trimmed()>0);
  check(!phase.observe(base+500*p,p,2,1,3*p,500,1,0,true,nullptr,false));
  check(phase.trimmed()==0);
  std::printf("STEREO_RECOVERY Hz=%.1f: old single-head age/allowance remain; opted-in age recovers and allowance reaches zero; jitter/loss/mode controls PASS (model)\n",1e9/p);
  xrfg::IndependentDeliveryReserve switched;switched.set_limit(4*p);
  switched.observe(base+3*p,base,p);switched.observe(base+3*p,base,p);
  for(unsigned n=1;n<=24;++n) {
   const auto at=base+n*p;switched.delivered(at-p,at+p+switched.reserve(),at,p,true);
  }
  const auto held=switched.reserve();check(held>0 && held<3*p);
  for(unsigned n=25;n<=32;++n) {
   const auto at=base+n*p;switched.delivered(at-p,at+p+switched.reserve(),at,p);
   check(switched.reserve()==held); // default needs its own500ms evidence
  }
 }
}
void automatic_buffer_tests() {
 check(xrfg::kIndependentMaximumReserveFrames==4);
 for(auto p:{8'333'333LL,11'111'111LL,13'888'889LL}) {
  const long long start=1'000'000'000;
  xrfg::IndependentDeliveryReserve automatic;automatic.set_limit(3*p);
  automatic.observe(start+2*p,start,p);check(!automatic.reserve());
  automatic.observe(start+2*p,start,p);check(automatic.reserve()==2*p);
  automatic.observe(start+5*p,start+2*p,p);
  automatic.observe(start+5*p,start+2*p,p);check(automatic.reserve()==3*p);
  automatic.take_refill();
  // Provider failure, priming and idle/replay produce no successful receipt:
  // neither FPS equality nor elapsed wall time alone authorizes a reduction.
  for(int i=0;i<1000;++i)automatic.observe(start,start+3*p,p);
  check(automatic.reserve()==3*p);
  long long at=start,first_reduction=0;
  for(int i=0;i<120;++i) {
   at+=p;const auto before=automatic.reserve();
   automatic.delivered(at-p,at+p,at,p);
   const auto change=before-automatic.reserve();
   check(change>=0 && change<=200'000 && change<=p*8/1000);
   if(change && !first_reduction)first_reduction=at;
   if(at-start<500'000'000LL)check(!change);
  }
  check(first_reduction>=start+500'000'000LL && first_reduction<start+750'000'000LL && automatic.reserve()>0 && automatic.reserve()<3*p);
  check(3*p-automatic.reserve()<=(at-start-500'000'000LL)*8/1000+200'000);
  // Losing spare readiness, or a slow native presenter, immediately vetoes
  // further shrinkage even though images still count as delivered.
  const auto held=automatic.reserve();
  for(int i=0;i<2500;++i) {
   at+=p;automatic.delivered(at,at,at,p);
  }
  check(automatic.reserve()==held);
  for(int i=0;i<2500;++i) {
   at+=p;automatic.delivered(at-p,at,at+p/2,p);
  }
  check(automatic.reserve()==held);
  // New deadline loss cancels a previously qualified downward ramp.
  automatic.observe(at+p,at,p);
  for(int i=0;i<10;++i){at+=p;automatic.delivered(at-p,at+p,at,p);}
  check(automatic.reserve()==held);
  // A loading pause keeps the last good margin. It cannot cause a large
  // latency cut, and the first post-pause delivery starts a fresh hold.
  automatic.observe(at+20*p,at,p);check(automatic.reserve()==held);
  at+=20'000'000'000LL;automatic.delivered(at-p,at+p,at,p);
  check(automatic.reserve()==held);
  automatic.observe(at+2*p,at,p);automatic.observe(at+2*p,at,p);
  check(automatic.reserve()<=3*p);
  xrfg::IndependentDeliveryReserve four;four.set_limit(xrfg::kIndependentMaximumReserveFrames*p);
  check(!four.reserve());
  four.observe(start+3*p,start,p);four.observe(start+3*p,start,p);
  check(four.reserve()==3*p);
  four.observe(start+4*p,start+four.reserve(),p);
  four.observe(start+4*p,start+four.reserve(),p);
  check(four.reserve()==4*p && four.take_refill()==4*p);
  for(int n=0;n<10;++n)four.observe(start+8*p,start+four.reserve(),p);
  check(four.reserve()==4*p);
  // A fast recovery may approach an already-queued frame's expiry boundary,
  // but must stop before invalidating that frame. Margin alone cannot remove
  // the queue's age: a changed source phase has to recover the rest.
  xrfg::IndependentDeliveryReserve queued;queued.set_limit(4*p);
  queued.observe(start+3*p,start,p);queued.observe(start+3*p,start,p);
  check(queued.reserve()==3*p);queued.take_refill();
  long long now=start;
  for(int n=0;n<2000;++n) {
   now+=p;
   const auto due=now-3*p;
   queued.delivered(now-p,due+queued.reserve(),now,p);
   check(!xrfg::independent_expired(due,now,queued.reserve(),p));
  }
  check(queued.reserve()<3*p && queued.reserve()>3*p-p/2);
  // Half-rate output is not evidence for reducing latency even if each
  // delivered frame was ready early. Idle time cannot qualify recovery.
  xrfg::IndependentDeliveryReserve half;half.set_limit(3*p);
  half.observe(start+3*p,start,p);half.observe(start+3*p,start,p);
  for(int n=0;n<2000;++n){now+=2*p;half.delivered(now-p,now+p,now,p);}
  check(half.reserve()==3*p);
 }
 std::puts("AUTO_BUFFER 72/90/120Hz: deadline rise/cap3P,successful receipts only,500ms evidence,8ms-per-second slew,late-presenter veto,pause hold PASS");
}

void aer_copy_overlap_tests() {
 for(const long long p:{8'333'333LL,11'111'111LL,13'888'889LL})for(unsigned divisor:{1U,2U,4U}) {
  xrfg::IndependentSourcePhase prior,corrected;
  constexpr long long base=1'000'000'000;
  const auto target=p/2+2'000'000;
  long long at=base;
  std::uint64_t delivered=0;
  for(unsigned n=0;n<1200;++n) {
   at+=divisor*p;delivered+=divisor;
   const unsigned prepared=n%5?1:0,pending=2+n%2;
   const auto age=target+p/8+(n%7)*p/128;
   const auto old_step=prior.observe(at,p,divisor,pending,age,delivered,0,0);
   const auto step=corrected.observe(at,p,divisor,
       xrfg::independent_phase_ready_count(true,pending,prepared),age,delivered,0,0,true);
   check(old_step>=0 && !step);
  }
  check(prior.trimmed()>p && corrected.trimmed()==0);
  // Model the owner's missed low-latency equilibrium: one completed head,
  // peer already owned by native End; two-ready gating cannot recover it.
  xrfg::IndependentSourcePhase strict;
  const auto before=corrected.trimmed();
  for(unsigned n=0;n<1200;++n) {
   at+=divisor*p;delivered+=divisor;
   const auto age=target+p-(corrected.trimmed()-before);
   check(!strict.observe(at,p,divisor,1,age,delivered,0,0));
   const auto step=corrected.observe(at,p,divisor,1,age,delivered,0,0,true);
   check(step>=0 && step<=200'000);
  }
  check(corrected.trimmed()-before>=p && strict.trimmed()==0);
  const auto settled=corrected.trimmed();
  // V1022 kept shifting inside normal residence jitter. Once recovered,
  // the AER band holds phase here even with two ready heads.
  for(unsigned n=0;n<1200;++n) {
   at+=divisor*p;delivered+=divisor;
   check(!corrected.observe(at,p,divisor,1+n%2,target+p/8+(n%7)*p/128,delivered,0,0,true));
  }
  check(corrected.trimmed()==settled);
  for(unsigned n=0;n<1200;++n) {
   at+=divisor*p;delivered+=divisor;
   check(!corrected.observe(at,p,divisor,0,3*p,delivered,0,0,true));
  }
  check(corrected.trimmed()==settled);
  // A later genuine backlog re-arms; no lifetime recovery cap.
  for(unsigned n=0;n<1200;++n) {
   at+=divisor*p;delivered+=divisor;
   const auto age=target+p-(corrected.trimmed()-settled);
   corrected.observe(at,p,divisor,1,age,delivered,0,0,true);
  }
  check(corrected.trimmed()-settled>=p);
  for(std::size_t pending=0;pending<8;++pending)for(std::size_t prepared=0;prepared<=pending;++prepared)
   check(xrfg::independent_phase_ready_count(false,pending,prepared)==pending);
  std::printf("AER_BAND Hz=%.1f divisor=%u one-ready recovery %.3fms;settled band/zero-ready hold;later backlog re-arms;stereo unchanged\n",
      1e9/p,divisor,(settled-before)/1e6);
 }
}
