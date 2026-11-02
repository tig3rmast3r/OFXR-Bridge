# V696: GTA headlock uses the retained native peer camera

Local candidate based on V695; headset acceptance remains pending.

V695's effective-state detector was verified to run while aiming, but replacing
the submitted AER peer pose with a simultaneous eye pose did not resolve the
reported defect. Remove that pose replacement and keep the validated state
reader and original native/final colour-token recovery.

During GTA's effective headlock, the game camera base changes independently of
the XR head pose. A peer built from the current native camera plus the relative
XR rotation therefore differs from the actual peer render camera associated
with the destination colour and its existing AER pose. This also matters to the
AFW provider's retained source cameras. Use the exact original native peer
camera, including position, orientation and projection, when its immutable
publication matches the destination colour token. Keep the original submitted
AER poses. Require the same stream/epoch, adjacent publication and valid native
camera geometry; do not select a latest camera or reinterpret a missing peer.

The change is confined to the retained GTA headlock state, including delayed
unlock. The depth-dependent camera subtraction, motion field, provider mode,
native object intervals, colour ownership, pacing and other titles are retained.
No additional textures, copies, readbacks, GPU dispatches or waits are introduced.
F9 stage 1199 still reports effective lock state and whether the special camera
path was admitted, now meaning the exact native peer rather than a same-time rig.

Offline replay of the labelled aiming capture with the bundled provider aligns
the generated background with the retained peer view; the original camera showed
a substantial systematic shift. Moving regions are still updated. This is a
specific camera-ownership correction, not proof that headset aiming is solved.
Tests use independently rendered historical peer cameras with a changing game
camera and stationary XR eye rig, and reject missing peers, foreign epochs and
use outside headlock. Existing default camera/provider and capture tests remain.

Private provider inspection and replay instrumentation are not included in the
runtime or source changes. Automatic capture remains off.
