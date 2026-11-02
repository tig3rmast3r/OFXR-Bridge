# V709 Luke scene reset and marker-loss recovery

V708's targeted GTA run confirms that the native reset hold executes at every
real scripted camera cut. CRCs remain valid and no native prepare refusal is
recorded at those cuts. The later automatic capture is the pause map and must
not be used to classify the earlier gameplay scenes as 2D.

Two bridge costs remain visible. The first fresh frame synchronously rebuilds
the isolated beta6 renderer (about 40 ms device setup plus 15 ms frame buffers
in the first cut). Then, after three fresh submissions, the completed startup
rendezvous rearms on the new temporal epoch. All six cuts cause a full control
reconfiguration and an immediate invalid meter sample. A scene reset is not a
new producer/device startup: V709 keeps an already completed startup closed
across epochs of that producer. Incomplete startup and a different producer
still qualify normally; bounded retries and history isolation remain intact.

V709 prepares one fresh byte-identical isolated provider context in the
background. Frame-size buffers are prepared only when a native reset is
detected. The render thread polls readiness while retaining the last completed
pair, then swaps the fresh renderer and retires the old one asynchronously
against its actual GPU fence. The old frame's poses/FOV and scene identity stay
with its pixels; repeated images are not counted as new frames. Native history
and the post-AFW OFXR anchor still reset. Geometry/quality changes and failed or
late preparation retain the existing synchronous safety path. The hold remains
bounded to 250 ms; it does not admit old-epoch vectors or unsupported cameras.

An idle device-only provider context uses additional resources; during reset
old/new frame buffers can coexist temporarily. No XR swapchains are added.
Real-game VRAM overhead has not been measured. Proven physical unit calibration
is retained across an epoch when the producer domain/colour geometry is still
compatible; new stream/geometry continues to invalidate it.

Native warmup is now common to supported Luke streams, with both eye domains
recorded, strict current-source guide readiness and exact retained source CRC
reads before output staging. Producer histories and strict guide admission are
unchanged. The one-second marker-loss protection is also common: independent
models per logical eye learn stable positive publication/token/native counter
steps from at least three actual CRC confirmations. Inference expires after
one second from a real confirmation and cannot renew itself. Same device,
stream, owner, dimensions, epoch, exact real guide and known peer CRC are still
required. It is a bounded inferred association, not a recovered physical CRC.

Accepted V701 aiming, native depth-based camera subtraction, marker geometry,
shader/provider bytes, NR, generic OFXR pacing and V706 exclusions are preserved.
F9-only stages 1239/1240/1241/1242 identify metric resets/unknown scene windows;
they do not replace unknown samples with fabricated rates.

Validation includes actual provider Full/Half/Quarter resets, two-handle native
epochs, geometry/gap recovery, independent rotated-plane pixel oracles,
accepted GTA aiming/eye reference/calibration, strict history rejection,
origin CRC read receipts and bounded phase inference. Offline reset timing
measures the first fresh synth call, not complete recovery or headset scanout.
Headset acceptance and disappearance of every reported N/A remain unverified.
