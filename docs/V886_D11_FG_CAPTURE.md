# V886: ordered frame-generation capture on D3D11 and D3D12

With logging enabled, manual F8 can capture the actual original and interpolated
images delivered through the private runtime, after stereo reconstruction. Both
native D3D11 and D3D12 publication paths are supported, with Framework or Director.

The recorder copies the acquired, waited native target after its normal image
copy and before release. Existing copy threads, staging helpers and completion
fences protect the readback. Released repeated images are not read again: their
metadata references the earlier exact captured generation when available.

Additional capture files:

- `fg-deliveries.tsv`: ordered packet/token, source identity, original or generated
  kind, output ordinal, content/submission times and native End result.
- `fg-delivery-views.tsv`: per-view image identity, indices, slice, pose, FOV,
  rectangle, repeated-content reference and explicit image-capture result.
- `manifest.tsv`: `fg_delivery_original` is the already stereo-reconstructed
  original FG input; `fg_delivery_interpolated` is the temporal FG output.
  Join its stream to the delivery token and view identity.

`image_api` is11 for D3D11 CPU staging,12 for D3D12 fenced readback and0 for
metadata or a repeat reference. Missing images retain their error instead of
being mistaken for a failed frame submission. Native End acceptance establishes
API acceptance, not physical visibility in the headset.

Manual delivery capture has a separate limit of eight packets,32 images and1 GiB,
with a128 MiB limit per image. Ordinary AFW/OSR and NR budgets are independent.
The delivery window can continue for up to one second after input capture closes.
Queued receipts that predate F8 can legitimately have unknown source metadata.

F9 without F8 adds no image staging copies. Manual D3D11 staging/Map and D3D12
readback can stall capture-time rendering: do not use these gaps as evidence of
normal pacing. The recorder does not change synthesis, pose or delivery policy.

Targeted endpoint tests cover exact stereo snapshots through native image reuse,
both graphics APIs and delivery paths, poses, refused End, late capture, repeats,
inactive capture, stale tokens, budgets and timeout. The AFW terminal-image
capture check remains.
