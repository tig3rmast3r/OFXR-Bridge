# V967: consume the exact owned guides already resolved for OSR

GTA V966 still refused FSR at pair failure20 and DLSS-G at guide detail11.
The loaded DLL matches the package. V966's forwarding-alias fixtures did not
cover opaque GetDevice/private-data responses; their pass was not game success.

The producer and exact colour-token resolver are shared with OSR. No additional
depth/vector hook, resource search or pixel capture is added. FSR/Hybrid/DLSS-G
now consume the same owned motion/depth snapshots using an immutable resource
and allocation-device pair recorded when those snapshots are created. N1
composition replaces that pair with its own created texture. The exact resource
must match and the consumer must match the recorded allocation device, or its
already proven backing. A later GetDevice proxy does not override this creation
receipt. Generic resources without this receipt retain their strict checks.

Receipts are created once per allocation and shared with existing guide frames;
no new GPU textures, copy pass, queue wait or repeated ownership search is added.
Eye assignment, chronology, camera crop, colour tokens, snapshot retention and
private NGX isolation are unchanged. RIFE/pacing/installer/model payload unchanged.
Shared control ABI remains96201; the allocation records are private CPU metadata.

The added opaque-device regressions fail with V966 admission and pass through
FSR, Hybrid and actual private DLSS-G with owned receipts. Tests also verify
publisher/composer resource bindings, ordinary aliases, missing receipts and
replacement-resource/wrong-owner refusal, held eyes and exact producer fences.
These fixtures do not prove GTA headset image quality or the precise opaque
proxy behaviour of the owner process; that remains an in-game test.

F9 records bounded CPU admission reasons at1490..1492:4resource mismatch,
5allocation owner mismatch,6accepted owned allocation,3legacy ownership refusal,
1failed device query. Hybrid consumer verdict moves to1496 to avoid collision
with producer marker1196. Diagnostics do no GPU work and stop with logging off.
