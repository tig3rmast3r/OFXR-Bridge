# Private Off-transport recovery - V984 round 10

Reporter evidence: Folios PID5628 initially sustains ~120 compositor FPS, then
~30-31 without R9 resource release. After UEVR reinitialization, R9 trace98483
marks release at268.291s, but generation never resumes and output remains
~10-12FPS. Peak observed usage/budget is97.6%, without an over-budget sample.
After release the median application Begin-return to End-entry interval is
81.954ms; End CPU duration median0.380ms. This identifies slow production
outside that End call, not its GPU/driver cause. OFXR GPU/transport effects
remain possible. No full FF7 or SteamVR headset reproduction is claimed.

## Reproduced differences

Six new R9-negative tests fail twice, including individual reruns:
- transport-4X/2X FidelityFX: resources retire, but clean_bypass remains false;
- budget-4X/2X FidelityFX/new NVIDIA: six actual60ms completed GPU queue
  dependencies leave generation allocated because no sample reaches100ms.

These are real D12 fence/ownership tests using the shipping layer and a mocked
native runtime. Queue gates model delayed completion. They do not simulate
full game residency paging or prove the remaining reporter slowdown's cause.

## Change

Recovery uses the established effects-Off private presentation policy while
ordinary frame generation is paused. It drains actual consumers, releases R9's
generation/history resources, disables optional delivery retiming and waits
for pending native delivery before admitting another application render.
Virtual callers use admission-only waiting: the existing presenter retains its
Wait/Begin ownership. Neither user controls nor profile preferences are toggled.
Independent AFW/Neural Rendering prevents the all-effects-Off policy override;
those features retain their existing processing policy. Luke/interop scope
remains excluded by the existing private D12 recovery eligibility.

The Wait path respects recovery until restart instead of resetting its policy
from the saved enabled preference. Restart restores the appropriate normal
policy before rebuilding generation resources. Native/application images stay
owned; unfinished GPU work and submitted native packets cannot be recycled.

Six consecutive completed paired jobs exceeding125% of the selected real-frame
budget request recovery; budget is measured physical display period times the
selected multiplier. A healthy paired cost, >5% budget change, or >2sec gap resets
that streak. Existing three >=100ms observations retain severe-stall coverage.
Primes and VRAM percentage alone cannot request recovery. Worker cost includes
preparation through stereo completion, since fence waits alone undercount this
capture. Empty readiness at presentation does not reset the worker streak;
reference S1/S2/S3 receipts are sampled once per source sequence.

Folios paired worker costs at225.167-225.600s are49853,67210,51410,48111,63012,
45576us. Replaying these costs at120Hz/4X requests recovery. Fence-only replay
would wait until~255.164s. A recorded-cost replay tests policy, not the entire
game or GPU outcome under a different policy.

Restart requires >=3sec plus16 consecutive real intervals within125% of the
selected real-frame budget and a known physical period. A hitch resets the
streak; time alone never restarts generation. Continued low game rate keeps
fallback active. There is no forced restart timeout.

Trace98485: a=session,b=completed cost(us),c=selected real budget(us), request.
Trace98483: a=session,b=clean_bypass policy(0/1),c=0, successful resource release.
Trace98484: stable restart, as in R9.

## Validation

GPU cases cover actual resource retirement, native-consumer protection,
2X/3X/4X FidelityFX/new NVIDIA stable rebuild and final real-source pixels,
preferences retained, policy retained across Wait and restored on restart.
A separate shipping-director test alternates actual60ms GPU completion faults
with already-ready presentation, so bookkeeping cannot erase GPU pressure.
The recorded Folios costs and trigger/stability controls are deterministic
checks. Existing clean-bypass D11/D12 tests additionally exercise virtual
admission, native ticket ownership and live-reenable wake-up while End is held.

Initial candidate fixtures inherited independent image-effect settings and therefore
correctly declined the all-effects-Off override; ordinary recovery fixtures now
explicitly select no AFW/NR, and separate checks preserve those features. The
2X trigger uses the existing atomic physical-period snapshot, available even
before a continuous presenter is promoted.

Final selected results are appended after validation. All prior trial fixes
remain included. No Adaptive merge, version bump, commit, push, install, arming,
live settings change or guaranteed FF7 recovery. Package excludes rife-vulkan
and the NVIDIA NR model, and retains V984 as test round10.

Final verification:70 of71 unique selected checks have passing results. Final
16/16 recovery/Off-policy checks pass after the physical-clock jitter guard.
The existing xrfg_private_stalled_renewal_gpu assertion fails in the broader
run and repeats alone, as in R9. The native renewal/latch code and corresponding
test are unchanged from R9; private transport changes extract only the clean
admission gate. No new selected failure remains. Six R9 negative cases each
failed twice before production changes. This remains a tentative reporter fix.

Logs: repo/svperstar-off-recovery-r10-tests.log (69/70),
repo/svperstar-off-recovery-r10-stalled-rerun.log (existing failure),
repo/svperstar-off-recovery-r10-final-focused.log (16/16), and
repo/svperstar-off-recovery-r10-negative-tests.log plus individual repeats.
Incremental review: repo/svperstar-off-recovery-r10-incremental.patch; previous
R9 tracked source was reconstructed and checked against package source hashes.
