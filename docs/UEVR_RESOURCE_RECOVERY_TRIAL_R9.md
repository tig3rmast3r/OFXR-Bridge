# Private resource recovery — V984 round 9

## Reporter evidence and limited reproduction

R8 PID31804 repeatedly entered real-only cooldown20times; PID27524 did so36times.
All8 one-second timeout warnings in the latter completed successfully. Real-only
worker jobs still took118–142ms median in affected windows; process usage reached
102–103% of its dynamic VRAM budget. Those logs establish repeated slow work
and retained-generation cooldown, not the precise cause of game/driver pressure.

Before the fix, four GPU fixtures (FidelityFX/new NVIDIA,2X/4X) repeat three
140ms actual queue completion delays through shipping worker completion. The
scene's real generator/history/outputs are already warm. R8 retains the generator
and both native destinations; all four failures repeat individually. At the
small fixture size, actual output work allocations are1.0MiB at2X and1.25MiB
at4X. These are fixture allocation sizes, not claimed reporter savings.
This reproduces the retention/recovery contract failure, not full FF7 residency
paging or the complete game/cutscene workload.

## Change

Completion telemetry observes total stereo fence wait across a transaction.
Three >=100ms paired waits within10seconds request resource recovery; eight
healthy paired observations clear an incipient streak. Primes and high VRAM
percentage alone cannot trigger it. The private D12 eligibility is independent
of multiplier/provider; Luke, native runtimes and interop retain existing paths.

At application End admission, drain worker/descriptors, take content ownership,
and drain actual private runtime packets before touching resources. Retire old
continuity, generators/flow contexts/output destinations and allocated source
history. Preserve application/native images and presentation ownership. Skip
history and generation admission during recovery, and stop generation guide
capture when no independent reconstruction feature needs it. Required auxiliary
composition and reconstruction resources keep their existing contracts.

Recovery delivers raw application composition through the established borrowed
frame path, rather than preparing another generated real/prime. It retains the
physical presenter to preserve outstanding virtual frame-call ownership.

Rebuild requires at least3seconds AND16 consecutive real-frame observations
within max(50ms,2*physicalPeriod*selectedMultiplier). A hitch resets stability;
wall time alone cannot wake generation. Cadence uses a high-resolution clock,
not coarse GetTickCount ticks. Percent-budget is diagnostic, not a sticky95%
exit threshold. Re-enable capture and defer normal fresh-history priming when
stable. User generator, multiplier, resolution and pacing preferences are not
modified. Trace98483 marks released resources;98484 marks a stable restart.

## Verification

The tests verify generator weak ownership expires, native destinations drop,
old history tickets become invalid, resource rearm stays deferred through a
long gap, fresh generation resumes, exact final GPU pixels match and teardown
leaks no native handles. A held-native-reader variant must block retirement
while keeping the old generator alive until the real consumer is released.
Controller checks cover sustained versus isolated stalls, prime exclusion,
minimum recovery time, hitch reset and fast high-resolution observations.

The first candidate released resources but failed rearm because its observation
clock was too coarse; tests caught this and the high-resolution correction
passes the basic four cases. Validation logs also retain initial test-adapter
corrections (policy command dispatch and stereo metadata in held-reader case).
Final package results are appended below. No full suite/headset acceptance or
RIFE inference/model-memory reclaim measurement is claimed. Healthy unresolved
GPU work still cannot be recycled; device errors and stop retain R8 handling.

Includes all previous rounds. No Adaptive merge, version bump, commit, push,
install, arming or live game/profile setting changes. Excludes rife-vulkan.

Final selected results:60of61 have passing results, including the new four
2X/4X resource cases, held-native-consumer case and deterministic controller.
The initial held-consumer metadata and policy-dispatch fixture errors were
corrected. The late-provider test now verifies additional synthetic delivery
after the actual transition over nine subsequent inputs rather than requiring
a fixed count during startup. Both providers pass.

The remaining xrfg_private_stalled_renewal_gpu latch/timing assertion repeats
on isolated reruns. SHA256 confirms both its private runtime implementation
and test source are identical to packaged R8; R8 and earlier work already
document its intermittent failure. No change to that unrelated pacing path
is included to make this run green. This is not an all-tests-pass package.

Logs: repo/svperstar-resource-recovery-v984-r9-tests.log (initial selection),
repo/svperstar-resource-recovery-v984-r9-rerun.log, and
repo/svperstar-resource-recovery-v984-r9-final-focused.log. The four old-code
negative cases and individual repeats are under repo/svperstar-reclaim-r9-*.
