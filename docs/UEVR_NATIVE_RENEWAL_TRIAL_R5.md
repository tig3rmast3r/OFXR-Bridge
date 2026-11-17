# UEVR native renewal trial (V984, round 5)

Reporter archive: `new freeze v984.zip`, PID15660. Hybrid is selected and
active from startup; the actual UI engine switch is outside this capture.
At 58.008 seconds the application and internal EndFrame stop returning.
The native transport renews the same retained packet 4159 another 415 times
until the capture ends at 87.487 seconds. Begin calls take up to 101 ms.
No allocation refusal is recorded. Last memory sample: 13.284 GiB usage,
13.513 GiB budget. Memory pressure may contribute; the log does not prove
what causes the native Begin delay.

## Correction

The existing stalled-renewal check measured Wait only and checked freshness
at Wait return. A fast Wait followed by a slow Begin escaped that check.
Evaluate replacement-cycle freshness at the recorded successful Begin return.
If Wait returned a current prediction but Begin consumed over four refreshes
and expired it, escape this packet without changing the existing Wait-stall
latch semantics. Existing Wait-stall counting, reset and error/stop behavior
remain unchanged.

Independently cap expired clock renewal to eight replacement cycles or 100 ms elapsed
per pending packet, checked between native calls while the current prediction
is expired. Usable current predictions retain normal refresh behavior. This is not a timeout or
cancellation of a native call: a blocked Wait/Begin must return first.
The cap covers ordinary Framework/Director and native-contract paths and
emits trace stage 56 when reached. The existing submission path retains the
same owned prepared images, poses and native cycle. GPU readiness, endpoint
leases, FIFO order, device errors and native call serialization are unchanged.
This bounds renewal starvation, not total frame latency or GPU workload.

## Validation

New transport regression exercises D3D12/D3D11, Framework/Director and native
contract variants with (1) fast Wait followed by 60 ms Begin and (2) rapid
replacement cycles whose predictions remain expired. Uses actual GPU images,
checks exact pixels/poses once in FIFO order, finite replacement count and
balanced native Begin/End ownership. Old production code fails the delayed
Begin case twice independently (accepted output never delivered). Failure
injection bounds the old-code reproduction.

Includes the prior R4 pooled panel and nonblocking auxiliary retirement fixes,
R1 UEVR cadence repair and R3 completion-delay recovery. No version bump,
commit, push, installation or live settings changes. Headset acceptance pending.

Final focused run: 39/39 checks pass. Early candidates failed sparse-cycle and
stall-latch tests; the final code preserves the existing Wait-stall branch and
bounds only expired replacements. A separate unchanged-runtime comparison
passed both older transport/stall tests. Earlier logs are retained for audit.
No full-suite or reporter headset run was performed here.
