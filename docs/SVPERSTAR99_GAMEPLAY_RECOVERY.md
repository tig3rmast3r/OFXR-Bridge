# svperstar99 gameplay recovery: local integration

Reporter confirmation on 2026-11-17: the R10 V984 test build recovered after slowdowns during about20minutes of FF7 Rebirth gameplay. The capture shows two internal Off-policy intervals and return above110 compositor FPS within roughly4-5seconds. This confirms observed recovery for that combined test build, not every scene or runtime.

The first RIFE2X Framework Normal benchmark trial remains a separate known issue: repeated resource recovery interrupts RIFE optimization. Retained backend records show optimization pending without TensorRT execution; the benchmark can continue extending setup indefinitely. The benchmark/optimization fix has not been implemented in this commit series.

Validation on the tested V984 base:70/71 selected checks have passing results, with16/16 final recovery/Off-policy checks passing. The pre-existing xrfg_private_stalled_renewal_gpu assertion fails and repeats alone. Six R9 negative cases failed twice before the recovery change. See the per-change trial notes for scope and limitations.

The changes are split by logical fix. No nightly version is assigned before integration and publication. No remote push is authorized by this local preparation. Latest-nightly integration and its validation are recorded separately.

## Nightly integration, 2026-11-17

Pulled origin/nightly into a clean integration worktree before integration;
base af36f0b (project version1002). Keep recovery on the existing Framework,
Director and combined legacy presentation paths. Independent pacing keeps its
own asynchronous submission/completion, admission and producer-acquire logic.

Completion-timeout extension, pressure cooldown and resource recovery explicitly
exclude Independent. The panel publication pooling, invalid-projection filter
and expanded depth-history omission are also limited to legacy paths, preserving
Independent's upstream allocation/input behavior. Existing NVIDIA depth-history
omission stays intact. The measured missing-NGX-time compatibility fix retains
valid game timing; no DLSS-G allocator, ReShade, OptiScaler or benchmark-controller
implementation is replaced.

Build: the diagnostic layer, both private runtime GPU hosts, NGX metadata,
Independent pacing, device-proxy and benchmark-suite test executables succeed.
Selected validation:188/190 pass, including all16 resource/Off-policy checks.
Of117 Independent checks,116 pass. Both failures repeat individually in the
candidate AND an unmodified af36f0b control build:

- xrfg_independent_input_timing: D3D12 reports resolving timestamp query index7
  before it was performed; resource-state validation fails.
- xrfg_private_stalled_renewal_gpu: the existing stalled-renewal latch assertion.

No new selected failure against the clean nightly control. This is not a full
suite or headset acceptance claim. The RIFE optimization/benchmark restart loop
remains separately documented and is not fixed by this series.

Validation logs under repo/: svperstar-nightly-validation-build.log,
svperstar-nightly-validation-tests.log, svperstar-nightly-input-timing-rerun.log,
svperstar-nightly-renewal-rerun.log, svperstar-nightly-control-input-timing.log,
svperstar-nightly-control-renewal.log. Machine-readable results are in
svperstar-nightly-validation.json. No push, version assignment, installation,
arming or live game/profile changes were performed.
