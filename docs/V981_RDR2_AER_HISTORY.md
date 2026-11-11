# V981: RDR2 Vulkan AER history and motion interval

RDR2's markers decoded, but AER rejected its separate per-eye SR streams before
generation. The captured sequence had global publications95/94/93, same-eye
serials48/47, and an intervening stream's independent serial47. The original
single-stream N1 check required that intervening stream and counter to match.

Accept the already verified interleaved A/B/A history using its exact producer
clock, camera tokens, feature epochs, independent serials and adjacent global
publications. Retain all original resolver ownership/fence and reset checks.

RDR2's original field can point to either retained eye. Before attaching a
variable-interval guide, use the same whole-frame576-sample colour vote and
ambiguity thresholds as OSR. Read the exact CRC-bound XR capture and retain
private per-eye colour witnesses with their native crop coordinates. N2 fields
pass unchanged; N1 composition additionally requires a classified N1 middle
field. Unknown intervals stay unavailable rather than guessing or composing
an already same-eye field again.

The classifier is session-owned and allocated only on the variable-interval
path. History leases and producer receipts order its queue work. A small fenced
readback reports the decision, with a bounded32ms wait; held tokens reuse their
decision. In-flight input/snapshot resources remain pinned on timeout. No new
XR swapchains, runtime changes or game settings. Stage1450 records interval,
publication and packed N1/N2 vote counts; bounded stages1442-1449 describe guide
history refusals. Ordinary fixed-interval games retain their existing path.

## Validation

The owner confirmed RDR2 working with the combined V979-based r3 package; the
previous marker-only change was independently accepted in The Great Circle.

Focused actual GPU tests cover N1/N2/unknown colour votes, translated textured
scenes, exact completion fences, held-token reuse, and CRC guide joins with
independent RDR2-style streams. Wrong producer clocks and missing/reset history
remain refused. Existing single-stream N1/N2, static-layer placement, stalled
ingress isolation, Vulkan interop/camera/scope and codec checks passed.
x64/x86 builds passed; full suite deliberately skipped at the user's request.
Release version remains0.3.3; IPC ABI remains97101.
