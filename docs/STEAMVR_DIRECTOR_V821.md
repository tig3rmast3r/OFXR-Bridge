# V821: restore Framework and contain the SteamVR Director regression

Based on V820, preserving V819 OpenKneeboard, accepted NR, RIFE and all providers.
Local trial; no live installation or publication.

Framework uses the V819 consumer again, including its original normal/Buffer
clock, Wait worker and draining mode changes. New native timing and qualification
are Director-only, on true SteamVR. VDXR/Legacy, native Pimax and AFW-only keep
their previous routing. Modes can be changed live with the existing drain;
accepted images, native bindings and source leases stay owned throughout.

Director retains the qualified future Wait at physical refresh: 16 output
intervals, inverse median >refresh-1 and mean >=99%, continuously for one second.
The one-second entry does not add one second of latency. Mean below99% or idle
above two physical periods exits the policy. Already started Waits are consumed.

Below qualification, Director retains at least one physical interval between
native handoffs: at90Hz,11.111ms. It waits only the remaining interval. It does
not round late preparation onto another physical grid slot or add a fixed extra
period to every image. End keeps the current native predicted display time.

A second correction handles a SteamVR cycle obtained before lengthy preparation.
V820 waited only for the raw predicted display time to expire. That prediction
may remain in the future even after preparation held the native cycle for more
than a physical interval. Director now renews that unsubmitted cycle when
prepared_at-Wait_return >= physical_period. It preserves the SAME FIFO image,
uses the defined Wait/Begin frame-discard transition and serializes Begin/End.
This is a SteamVR workaround tested against presentation counts, not a claim
that the OpenXR specification mandates one-period expiry of display predictions.

Age is measured at completed preparation, not at the later artificial handoff
deadline. The latter variant repeatedly renewed ready images and reduced full
refresh throughput; it was rejected. A minimum interval applied indiscriminately
also to qualified full-refresh handoffs was rejected after measured regression.

Real SteamVR comparison,90Hz, controlled GPU load, settled windows:

| Case | Native End/s | Distinct compositor presentations/s | Unshown records |
|---|---:|---:|---:|
| V820 paired43+43 |85.97|65.15|203|
| V821 paired43+43,capacity2 |85.97|85.87|0|
| V821 full refresh |89.96|89.87|0|
| V821 source70 |70.00|70.10|0|
| V821 source89 |88.87|88.46|4|

Rates have one observer-poll/refresh boundary uncertainty. Unshown counts are
OpenVR compositor records in the measurement window, not optical pixel decoding.
All accepted source IDs reach native End in order, without duplicates or metadata
errors. That conservation alone did NOT prove presentation correctness in V820.
The paired GPU model uses16ms work/near-empty output pairs, averaging8ms/output;
it approximates burst preparation, not the entire MSFS rendering/encoding stack.
The lightweight replay alone did not reproduce loss; GPU load was necessary.
Capacity3 also tested: it did not improve this case and is not forced globally.

Limits:89fps has residual presentation losses; loading/startup/mode transitions
are not a universal zero-drop guarantee. Game/headset acceptance remains pending.
No renderer/shader/NR/FSR quality or RIFE worker/model change is included.

Reference contracts:
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrBeginFrame.html
- https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrEndFrame.html

Source tests exercise Director-only selection, untouched Framework normal/Buffer,
live switching, paired handoff spacing, future-prediction/aged-cycle recovery,
qualification, real D11/D12 pixels and native cycle balance. Retain the evidence
for rejected variants and the failing V820-equivalent aged-cycle control.
