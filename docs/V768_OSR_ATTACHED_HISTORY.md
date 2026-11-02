# V768: first-person weapons from the previous frame in OSR

This nightly update changes only Full OSR's resolve. Frame generation, NR,
delivery/pacing, runtime providers, AFW and the UI are unchanged.

## Symptoms

- A crossbow held in first person shimmered in the reconstructed eye
  (Kingdom Come Deliverance II).
- A band ran along the edges of the weapon and arms: their lit rim came and
  went every other frame (Cyberpunk 2077).

## Causes

- Where both eyes see a camera-attached surface, the other eye's warp was
  taken. That near, its disparity is about 1000 px, and the warp put the
  crossbow 26 px off both real frames: a 2.6% (1.6 mm) error in the eye
  separation. Far away the same error is invisible.
- The anti-aliased rim of a camera-attached surface in the previous frame was
  dropped as a stale halo: parallax fires whenever the camera moves, and the
  warp does not hold the surface where its edge is. The inpaint mirrored
  background into it.

## Change

- Where the warp shows a camera-attached surface (nearer than
  `kOsrFullLockedDistanceMetres`), this eye's previous frame is taken: it
  knows exactly where the weapon is in this eye's view, and it also covers the
  strip of background a misplaced warp draws over.
- The edge of a camera-attached surface is never a stale halo.

Weapon animation now shows at each eye's own rate; its placement no longer
depends on the eye-separation calibration.

## Test

Full OSR, weapon out: aim, strafe and walk. The weapon stays in place and
solid, and its edges keep their rim, with no band beside them.
