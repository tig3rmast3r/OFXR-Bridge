#include "NrBeforeSrContract.h"
#include "NrGuideRegion.h"
#include "NrSubmissionLedger.h"
#include "NrHistoryContract.h"
#include "NrForwarderContract.h"
#include "NrRetirementCadence.h"
#include <cstdio>

int main() {
    unsigned checks=0,failures=0;
    auto check=[&](bool result,const char* label){++checks;if(!result){++failures;printf("FAIL %s\n",label);}};
    using namespace DlssNr;
    check(GuideFits({3,2,7,5},12,8)&&!GuideFits({3,2,10,5},12,8),"independent private guide ROI stays in bounds");
    check(!GuideFits({0,0,0,5},12,8)&&!GuideFits({~0u,0,2,2},12,8),"empty and overflow guide rectangles rejected");
    for(unsigned percent:{100u,75u,50u,25u}) {
        check(GuideScale(1280.f,1280u*percent/100u,1280)==float(1280u*percent/100u),"low-resolution MV units match supplied working guide");
        check(GuideScale(1920.f,1280u*percent/100u,1920)==float(1280u*percent/100u),"high-resolution MV units scale independently of colour size");
        check(GuideScale(720.f,720u*percent/100u,720)==float(720u*percent/100u),"Y scale remains independent of X");
    }
    auto padded=BeforeSrExtent({1280,720},true,{1920,1080},{1920,1080});
    check(padded.width==1280&&padded.height==720,"active render extent survives padded allocation");
    auto partial=BeforeSrExtent({1280,0},false,{1600,900},{1920,1080});
    check(partial.width==1600&&partial.height==900,"partial subrect does not invent dimensions");
    check(!FitsBeforeSr({1921,1080},1920,1080),"out of allocation extent rejected");
    check(NrResetWithSource(true,true,0),"source zero cannot clear local reset");
    check(NrResetWithSource(false,true,1),"source reset preserved");
    check(!NrResetWithSource(false,false,1),"unavailable source reset not fabricated");
    check(!NrFenceRetired(~uint64_t{0},7),"removed device is not retirement");
    check(!NrFenceRetired(6,7)&&NrFenceRetired(7,7),"exact fence retirement");
    NrSubmissionLedger ledger;int first{},second{};
    auto old=ledger.Record(11,&first);auto detached=ledger.Detach(&first);
    check(detached.size()==1&&detached[0].recording==old.recording,"detach owns old recording before submission");
    auto next=ledger.Record(11,&first);
    check(!ledger.Complete(old),"reset list cannot complete new recording with stale receipt");
    check(ledger.Complete(next),"matching recording completes once");
    check(!ledger.Complete(next),"duplicate completion rejected");
    auto eye0=ledger.Record(20,&first);auto eye1=ledger.Record(21,&second);
    check(ledger.Complete(eye0)&&ledger.Complete(eye1),"same size eyes retain separate histories");
    auto retired=ledger.Record(30,&first);ledger.Forget(30);auto recreated=ledger.Record(30,&first);
    check(!ledger.Complete(retired)&&ledger.Complete(recreated),"handle recreation rejects prior receipt");
    auto cleared=ledger.Record(31,&first);ledger.Clear();auto fresh=ledger.Record(31,&first);
    check(fresh.recording>cleared.recording&&!ledger.Complete(cleared),"shutdown does not recycle recording serial");
    auto lifetime=std::make_shared<int>(73);std::weak_ptr<int> weak=lifetime;
    auto owned=ledger.Record(40,&second,lifetime);lifetime.reset();ledger.Forget(40);
    auto owned_detached=ledger.Detach(&second);
    check(!weak.expired()&&owned_detached.size()==1&&
        *std::static_pointer_cast<int>(owned_detached[0].lifetime)==73,
        "detached receipt retains exact resource token after native handle retired");
    owned={};owned_detached.clear();
    check(weak.expired(),"retired detached recording releases ownership without frame delay");
    check(nr_forwarder::complete(0x7f)&&!nr_forwarder::complete(0x6f),"complete exact forwarder ABI required");
    NrRetirementCadence cadence;cadence.Observe(3,true,false,false);cadence.Observe(8,false,true,true);
    check(cadence.calls==2&&cadence.waits==1&&cadence.skipped==1&&cadence.pending==1&&
        cadence.totalMs==11&&cadence.maximumMs==8,"bounded retirement diagnostics");
    printf("Neural rendering contracts: %u/%u passed\n",checks-failures,checks);return failures?1:0;
}
