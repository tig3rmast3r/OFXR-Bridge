# V767: OSR edges of objects that move by themselves

This nightly update changes only Full OSR's resolve. Frame generation, NR,
delivery/pacing, runtime providers, AFW and the UI are unchanged.

## Symptom

A running character under a follow camera left a thin outline of its
previous leg, boot, arm and head positions in the reconstructed eye only
(Spider-Man Remastered).

## Cause

A previous-frame pixel next to a nearer surface is that frame's anti-aliased
edge. It was dropped as stale only when the camera's parallax moved the edge
past half a texel. An object that moves by itself, with the camera barely
moving, leaves its old edge pixels in place with no parallax, and the fringe
rule then took them.

## Change

A halo is also dropped when this frame's warp holds no surface nearer than it
within the halo radius (two depth texels): the object has moved away. Each
test only adds a reason to drop a halo; the parallax test covers camera
motion, the new one object motion.

## Test

Full OSR, a third-person character running past a flat background: no
outline beside the legs, arms or head. Static scenes and strafing past poles,
signs and vegetation look as before.
