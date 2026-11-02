# V701: retain the native peer reference during GTA aiming

V700 fixes gamepad strafe, but headset rotation in effective GTA aiming/head-lock
still leaves a narrow doubled silhouette. Four additional owner captures cover
gamepad, headset Combined, headset Alternate and headset Previous. Alternate and
Previous use full colour resolution: only Combined applies 2x2/4x4 scaling.

The captured native camera bases alternate between eyes while physical head
rotation is monotonic. Transferring one eye's entire base to its peer imports
that alternating reference. Preserve the peer's native reference and advance it
using rigid gameplay motion measured between two productions of the SAME eye.
The existing two N1 frame intervals determine the fraction of that N2 motion.
Original submitted XR poses and matched peer projection remain unchanged.

Admission requires the exact consecutive source/peer/same-eye histories, matching
stream and epoch, effective headlock, a matching historical peer XR orientation,
positive frame intervals, and no reset. It also requires monotonic physical head
rotation, opposing native-base steps, and a same-eye change smaller than BOTH
cross-eye steps. Missing/ambiguous histories retain V700's target. This avoids
interpreting physical eye cant or a head-direction reversal as the observed case.
No additional producer tracking read, GPU resource, copy, dispatch or wait is added.

Tests include independent rigid-motion matrices for both eyes and unequal frame
intervals; invalid, monotonic-base and reversed-head rejection; actual bundled
provider reconstruction with distinct eye references, an off-axis camera motion,
foreground occlusion and textured background at 1x1/2x2. The actual provider camera
is checked relative to the fixed stream origin. The half-resolution image oracle
uses recoverable changed surfaces wider than its colour filter footprint; it does
not classify subpixel antialiasing as failed replacement. Existing V700 scene and
stereo tests remain unchanged in their expected targets and thresholds.

Offline replays of the labelled captures remove the pronounced extra silhouette
in Alternate/Previous. This is a local candidate, not headset acceptance. Combined
and the previously accepted strafe still need visual confirmation. Recurring N/A
recovery is separate. Depth-dependent native camera subtraction, motion/object
intervals, quality behaviour, provider binaries/ABI, admission, NR and pacing are
unchanged. No publication or live installation/settings changes are included.
