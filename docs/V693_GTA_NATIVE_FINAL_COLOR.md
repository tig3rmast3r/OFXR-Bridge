# V693: GTA native guides and final caller color

Local recovery candidate after the V692 owner run still showed N/A transitions
and incorrect head aiming. This is not headset acceptance of either issue.
The owner now reports an explicit correlation: aiming locks the HUD and menu
to the headset; AFW becomes wrong while locked and recovers a few seconds after
unlocking. Do not describe that correlation alone as proof of a particular
camera-matrix fault or claim a separately detected lock flag.

## What the new captures establish

Both aiming F8s contain valid CRCs and actual AFW evaluations. The original
depth edges align with their real source colors. Current N-1 raw motion agrees
with the native current-to-opposite camera field on the stationary scene, and
the two adjacent real colors provide an independent comparison without using
the provider's aliased previous-eye buffer as a supposed original-image truth.

The exact provider replay reproduces the second captured output pixel-for-pixel
on the first F8. All eight captured terminal-eye images exactly match the AFW
source/generated images for their assigned eyes. The terminal transport is not
observed to introduce a further pixel error in these captures.

This does not establish that the V691 pre-wrapper N-1 guide change repaired
aiming. An offline adjacent-object-motion experiment has only a small measured
effect and is not included as a purported fix. The V692 late Backbuffer path
does not stamp in the actual run: its exact receipt is missing the Backbuffer
tag. API availability alone was insufficient. That path is removed.

## Recovery change

Restore GTA's typed-native N-2 guide boundary used before V691. Preserve its
native camera, motion, depth, scalars, history and ordinary NR callback order.
Crimson continues to use its separately verified original-caller N-1 boundary.

Color marking is independent of that choice. A GTA-only enclosing transaction
retains the actual original caller Output and command-list/device identities.
Its single successful typed SR/RR child supplies the exact native publication
and token. After the original wrapper returns successfully, mark that caller
color with the same native token. Reject changed creators, a second child,
foreign list/device, failed native or outer completion, incompatible output
geometry and incomplete scope. No latest-publication lookup or frame-offset
inference is used. Existing producer submission receipts still govern AFW
admission. No additional queue or pacing wait is added.

The GTA RGB decoder fallback from V692 remains. Full automatic loss captures
remain opt-in/default off; F8 and F9 remain available.

## Actual 2D scenes

V692 briefly classified a full application quad as a 2D scene, then returned to
waiting when the effect erased the quad CRC too. CRC is required to generate a
stereo frame; it is not required simply to preserve the application's own flat
composition. With no fresh stereo release, or both successfully read compact
projection regions black with no origin, a full BOTH-eye application quad now
bypasses AFW/OFXR regardless of its marker. Ordinary valid stereo selection
still wins on recovery. No quad token is used to invent a guide or FPS identity.

Remove the additional native D3D11 staging probe: classification uses actual
application composition metadata and the existing projection read. Single-eye
and partial overlays are not classified as full scenes. This remains GTA-only.

## Scope and limits

The native depth-aware camera subtraction method, AFW shaders/provider/resize,
NR implementation and all generation/delivery pacing remain unchanged. This
candidate separates guide ownership from final-color marking and removes the
unconfirmed V691 guide-contract change. Its live effect on head aiming still
requires owner confirmation. Post-SR effects outside the enclosing wrapper can
still erase a marker; they cannot be assigned a guide by guessing. A truly 2D
scene has no stereo AFW output and its combined-scene FPS may remain unavailable.

No public push, live installation, active profile, registry or game-file change
is part of this candidate.

## Local checks

The actual GTA wrapper fixture overwrites native guide inputs in place before
typed SR and replaces final caller color after it. It now verifies the native
N-2 guide bits, final composite pixels and their exact matching native CRC,
producer receipts, retained current/previous GPU resources and callback-reused
feature generation. Existing Crimson N-1 and KCD/Outlaws boundaries remain
separate. The full application quad test no longer requires a marker to bypass,
and still refuses partial/single-eye overlays and false frame identities.
