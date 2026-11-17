# UEVR private completion recovery — V984 round 8

## Reporter failures addressed

PID23360: second benchmark trial, NVIDIA reference 2X. Presenter readiness
wait times out after one second and future frames return runtime failure.
PID49556: RIFE 2X worker stage2 times out at fence2/completed1, device_hresult0.
Both eyes remain pending; later benchmark provider changes return acquire
call-order errors. These are separate from PID32220's earlier settings/stereo
change memory spike (112% process budget). This change targets completion
failure latching, not a claim to cure all memory-pressure slowdowns.

## Reproduction before the fix

Actual D3D12 queue is held on an injected fence, with its completion receipt
queued behind that fence. Shipping worker completion at 2X abandons the job
at one second for FidelityFX, reference NVIDIA, and RIFE configurations. The
actual three-slot consumer readiness path also fails the borrowed submission
and latches runtime failure. All four tests fail, then repeat independently.
Faults are released before cleanup; no completion receipt is forged.

## Correction

Separate completion eligibility from the optical multiframe cooldown policy.
For ordinary private SteamVR D3D12, readiness recovery no longer depends on
multiplier or provider. Continue waiting for the exact pending fence after the
one-second warning; retain images and descriptors. Poll stop/failure and device
removal. Presenter readiness now uses the same helper as worker completion.
Stopped readiness cancels its borrowed waiter without poisoning the session.
Actual API/device errors retain their error paths. Native delivery, Luke and
interop completion contracts are unchanged.

The optional optical 3X/4X slow-batch real-only policy remains distinct; this
trial does not impose that pacing policy on other generators. At any multiplier,
a healthy pending completion in the supported private path is not itself fatal.
A native/GPU call which never returns is not forcibly interrupted; genuinely
unfinished resources still cannot be reused. No software fence is signaled to
pretend completion.

## Validation

Before/after probes use real GPU fences on the shipping worker and consumer.
They retain pending work beyond1150ms, complete it, check no sticky error, and
check continuation after drained model/multiplier metadata changes. The consumer
also checks exact GPU pixels77/94 and poses3/7 once each. Stop tests verify pending
work is not falsely completed or submitted and no presenter failure is latched.

Shipping layer integration additionally gates actual 2X synthesis commands
for over one second in FidelityFX and NVIDIA reference modes, completes them,
then performs an actual drained provider change through apply_embedded_control.
Subsequent generation must continue normally. These are downstream-runtime
fixtures using actual GPU resources, not headset performance tests. The RIFE
case isolates its completion contract; no RIFE inference-quality acceptance is
claimed. Device-removal/structural error paths remain guarded by existing code.

Includes R7 and all earlier fixes. No Adaptive merge, version bump, commit,
push, installation, arming or live settings changes. The package excludes
rife-vulkan and nvngx_dlssnr.dll. Reporter acceptance remains pending.

Final validation: all eight new late-completion/shipping-transition/stop tests
pass. Of55 selected checks54 pass grouped; the existing standalone
xrfg_private_stalled_renewal_gpu timing-dependent latch assertion fails in the
group, then passes on isolated rerun. Its private runtime source is unchanged
from R7. All55 have passing results including that rerun. No full-suite run.
