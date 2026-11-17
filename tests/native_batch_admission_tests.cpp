#include "xrfg/native_batch_admission.hpp"
#include <cstdio>
#include <cstdlib>
static void require(bool v,const char* why){if(!v){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
int main(){
    constexpr std::int64_t refresh=11'111'111,origin=1'000'000'000;
    {
        xrfg::NativeBatchAdmission gate;
        gate.released(origin,refresh);
        require(gate.first_target()==0,"bootstrap cannot claim an already elapsed first slot");
        const auto before=gate.budget(refresh,3);
        gate.first_output(origin+refresh/2,gate.first_target());
        require(gate.budget(refresh,3)==before,"bootstrap GPU work is not a scheduling miss");
        gate.admitted(3);gate.delivered(origin);
        gate.released(origin+refresh/4,refresh);
        require(gate.first_target()>origin+refresh/4,"live backlog retains a meaningful first slot");
        gate.cancelled(gate.pending());gate.released(origin+refresh*100,refresh);
        require(gate.first_target()==0,"starved output clock cannot train a false miss margin");
    }

    for(unsigned factor:{3u,4u}) {
        xrfg::NativeBatchAdmission gate;
        gate.released(origin);gate.source_ready(origin+7'000'000);
        gate.source_ready(origin+32'000'000); // inherited release wait must not retrain work
        gate.generated(5'000'000);
        require(gate.budget(refresh,factor)<14'000'000,"release backpressure is excluded from render budget");
        gate.admitted(factor);gate.delivered(origin);
        const auto due=gate.release_at(origin,refresh,factor);
        require(due>origin,"a fast producer must wait before rendering into occupied delivery capacity");
        gate.admitted(factor); // deferred GPU batch is not yet in FIFO
        require(gate.release_at(origin,refresh,factor)>due+refresh*2,"deferred batch credits cannot disappear from the backlog");
        require(gate.release_at(origin+refresh*100,refresh,factor)>origin+refresh*100,"stalled delivery cannot invent retired credits");
        gate.cancelled(gate.pending());require(!gate.pending(),"re-prime cancels outputs that were never generated");
        gate.reset();require(gate.release_at(origin,refresh,factor)==origin,"drain/restart resets old delivery credits");
        gate.admitted(factor);gate.released(origin);gate.source_ready(origin+refresh*factor);
        gate.generated(5'000'000);
        require(gate.budget(refresh,factor)==refresh*factor,"slow genuine rendering may use the full source interval");
    }
    // The source supplies the slot AFTER the preceding batch's final output.
    // Retirement must preserve that target through the final pending -> empty
    // transition, without waiting for a nonexistent future output to wake us.
    for(const std::int64_t physical:{6'944'444LL,8'333'333LL,11'111'111LL,13'888'889LL})
    for(unsigned factor:{3u,4u}) {
        xrfg::NativeBatchAdmission gate;
        gate.released(origin);gate.source_ready(origin+1'000'000);gate.generated(500'000);
        require(gate.release_at(origin,physical,factor)==origin,"an unseeded empty queue starts immediately");
        gate.admitted(factor);gate.delivered(origin);
        const auto first=origin+factor*physical;
        const auto expected=first-gate.budget(physical,factor);
        require(gate.release_at(origin,physical,factor)==expected,
            "production targets its own first slot, not the previous real output");
        for(unsigned retired=1;retired<factor;++retired) {
            const auto at=origin+retired*physical;
            gate.delivered(at);
            require(gate.release_at(at,physical,factor)==expected,
                "retirement including an empty queue must preserve the first-output deadline");
        }
        require(gate.release_at(first+physical*20,physical,factor)==first+physical*20,
            "an empty stalled queue starts now without accumulating pacing debt");
        gate.reset();gate.admitted(factor);gate.released(origin);
        gate.source_ready(origin+physical*factor);gate.generated(physical);
        gate.delivered(origin);
        require(gate.release_at(origin,physical,factor)==origin,
            "slow source work starts immediately to preserve output throughput");
    }
    {
        xrfg::NativeBatchAdmission gate;
        gate.released(origin,refresh);gate.source_ready(origin+7'000'000);gate.generated(5'000'000);
        const auto base=gate.budget(refresh,3);
        gate.admitted(3);gate.delivered(origin);
        gate.delivered(origin+refresh+refresh/3);
        require(gate.budget(refresh,3)==base,"delivery jitter alone cannot create preparation lead");
        gate.prepared(500'000);
        const auto guarded=gate.budget(refresh,3);
        require(guarded>=base && guarded<=base+500'000,"only measured handoff work adds lead");
        for(unsigned i=0;i<24;++i)gate.prepared(50'000);
        require(gate.budget(refresh,3)<guarded,"cheap actual preparation removes stale margin promptly");
        const auto recovered=gate.budget(refresh,3);
        gate.prepared(refresh*100);
        require(gate.budget(refresh,3)==recovered,"long external stalls do not train render-ahead");
        gate.first_output(origin+500'000,origin);
        require(gate.budget(refresh,3)==recovered,"sub-slot receipt jitter is not a missed refresh");
        gate.first_output(origin+refresh,origin);
        const auto missed=gate.budget(refresh,3);
        require(missed>recovered,"a genuine first-slot miss adds measured wakeup protection");
        gate.first_output(origin,origin,refresh/2);
        require(gate.budget(refresh,3)<missed,"intentional first-slot slack removes stale protection");
        gate.released(origin+refresh*101,8'333'333);
        require(gate.budget(8'333'333,3)==std::max<std::int64_t>(8'333'333/4,12'000'000+8'333'333/8),
            "a refresh change clears native handoff lead from the old clock");
    }
    {
        xrfg::NativeBatchAdmission gate;gate.released(origin,refresh);
        gate.source_ready(origin+7'000'000);gate.generated(5'000'000);
        gate.generated(7'000'000);const auto peak=gate.measurement();
        require(peak.jitter==2'000'000,"GPU variability reserves measured extra readiness time");
        for(unsigned i=0;i<64;++i)gate.generated(5'000'000);
        require(gate.measurement().jitter<peak.jitter,"stable work recovers the variability reserve");
        gate.released(origin+refresh*10,8'333'333);
        require(!gate.measurement().jitter,"new refresh clock resets old variability reserve");
    }
    {
        xrfg::NativeBatchAdmission gate;gate.released(origin,refresh);
        gate.source_ready(origin+7'000'000);gate.generated(5'000'000);
        gate.admitted(4);gate.delivered(origin);
        const auto native_entry=origin+refresh;
        gate.delivery_started(native_entry);
        const auto expected=native_entry+3*refresh;
        require(gate.pending()==3,"native entry must not retire an output credit");
        require(gate.first_output_at(native_entry+refresh/3,refresh)==expected,"in-flight native work cannot slide the known first slot");
        require(gate.release_at(native_entry+100*refresh,refresh,4)>native_entry+100*refresh,"long native stall cannot invent completed output credits");
        gate.delivered(native_entry);
        require(gate.first_output_at(native_entry+refresh/3,refresh)==expected,"native return preserves the in-flight target");
    }
    {
        xrfg::NativeBatchAdmission gate;gate.released(origin,refresh);
        gate.source_ready(origin+7'000'000);gate.generated(5'000'000);
        const auto conservative=gate.budget(refresh,3);
        gate.set_async_gpu_handoff(true);
        require(!gate.gpu_overlap_allowance(refresh),"unknown native prediction cannot authorize GPU overlap");
        gate.observe_gpu_submission_window(refresh);
        require(!gate.gpu_overlap_allowance(refresh),"short native prediction cannot authorize GPU overlap");
        gate.observe_gpu_submission_window(refresh*4);
        require(gate.gpu_overlap_allowance(refresh)==refresh/3,"fenced GPU overlap is bounded to a third-refresh");
        require(gate.budget(refresh,3)==conservative-refresh/3,"native CPU submission may overlap retained GPU execution");
        gate.set_async_gpu_handoff(false);
        require(gate.budget(refresh,3)==conservative,"private/unfenced paths retain full readiness lead");
        gate.reset();require(!gate.gpu_overlap_allowance(refresh),"reset cannot retain old asynchronous ownership eligibility");
    }
    std::puts("Shared optical admission: credits, first-slot targeting, work budget, bounded margin and refresh reset PASS");
}
