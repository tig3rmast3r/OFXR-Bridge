# V911: N1 history during headset rotation

The owner accepts V910 driving without substantial headset rotation. New GTA
driving captures confirm its direct original N1 route activates while the head
turns, but part of one car still falls back to older history. The diagonal split
matches a false temporal-depth rejection and a change in attachment inference.

Compare the original N1 correspondence and the N1 world-camera displacement in
the same peer head pose. The existing stereo matrix already accounts for the
source-to-peer headset rotation. Removing that common rotation before measuring
gameplay-camera-relative motion keeps one followed object in one reference.

When this reference identifies a camera-following surface, validate the actual
own-eye N1 depth against the current surface in that same peer head pose, then
read the exact original N1 colour correspondence. It does not require an older
N2 depth interpolation for this directly observed N1 case. Retain the existing
camera-motion and attachment-ratio thresholds, 3% depth tolerance, scene checks,
source validity, near protection, bounds and stricter fringe tests. An occluded,
invalid or unrelated N1 point is not accepted merely because the head moved.

If the local N1 check does not apply, retain V910's established handling. Native
N2 titles retain their existing path. No new pose, camera gain, title override,
GPU allocation, image copy, dispatch, XR swapchain, wait or readback is added.
AFW, RIFE, NR, frame delivery and configuration are unchanged.

Focused GPU tests cover both eyes, both OSR qualities, opposite head rotations,
own-eye colour, and refusal for nearer history, invalid flow and hard-near input.
Actual driving capture replays and regression evidence accompany the local build;
headset acceptance of rotation and cinematics remains a separate check.
