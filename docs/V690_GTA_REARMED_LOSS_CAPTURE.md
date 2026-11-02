# V690: rearmed GTA marker-loss evidence

Local diagnostic candidate based on V689. No AFW rendering or motion correction
change is claimed. V689's original-frame meter recovery remains intact.

## Why this capture changed

The V689 GTA run (PID 17580) contains one earlier automatic snapshot and three
manual aiming snapshots with six actual AFW evaluations. It does not contain
the reported kill/grey effect. The previous automatic request was process-wide
one-shot and waited 200 ms; a short effect could also recover before recording.

With F9 logging on, GTA now starts the existing bounded recorder on the first
eligible nonblack loss of either verified eye marker, after a previously valid
pair. Guide priming with intact markers no longer consumes this diagnostic.
The frozen pair just decoded is copied immediately on its existing queue as
`luke_origin_loss_color`, before release/reuse. `origin_loss_crc` in the capture
timeline preserves each eye's original read result and token. This witness does
not rely on the following application frame still showing the effect.

One snapshot is taken per loss episode. The detector rearms after at least one
second of continuously valid pairs and ten seconds since its previous request.
An ongoing failure, short recovery, all-black readback, occupied recorder, RAM
ring, or disabled F9 cannot create overlapping captures. Manual F8 remains
available. Existing per-snapshot limits remain 48 images and 1.25 GiB; repeated
automatic episodes create separate folders. The cooldown limits their rate,
not the total count of a long diagnostic run.

## Aiming evidence and limits

In the three manual snapshots, color CRCs exactly identify publications
20027/20028, 20250/20251 and 21291/21292; current/middle/same histories are
contiguous. Source and returned images contain actual visible warp fragments.
All six 2x2 plugin input colors equal the area reduction of their full source
within 0.5/255 per channel (ordinary integer rounding). The resize is therefore
not an observed source of a color/guide displacement in these samples.

Image-flow and camera/motion comparisons show a discrepancy in the bad aiming
interval, but do not yet isolate its cause. Optical flow is only an offline
diagnostic, never a replacement AFW motion source or an admission oracle.
There is no paired known-good aiming F8 in this run, and no exact retained
post-SR native color for these delayed XR publications. Do not claim that a
particular post-SR transform, stale provider history, or motion interval is
proven responsible. Native depth-aware camera subtraction is unchanged.

## Validation scope

The terminal-capture test covers immediate recorder activation, real queued GPU
witness copy, refusal of overlap/RAM-ring/F9-OFF requests, bounded sealing without
AFW success, and later sessions. Detector cases cover startup, immediate first
loss, sustained failure, short recovery, cooldown and repeated rearming.
Existing original D11 pixels, allocator ownership, writer and budget checks
remain in that test. Core/color CRC checks are also run.

No source/provider/shader math, native guide admission, pacing, NR, runtime
installation, registry, game profile, public branch or release is changed.
Only the GTA/F9 diagnostic branch starts these additional copies; normal play
with F9 off does not create them. Hardware acceptance and aiming repair remain
pending.
