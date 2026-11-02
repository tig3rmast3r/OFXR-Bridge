# V766: first-person weapon in the OSR band

This nightly update changes only Full OSR's resolve. Frame generation, NR,
delivery/pacing, runtime providers, AFW and the UI are unchanged.

## Symptom

Strafing with a first-person weapon out, parts of the weapon showed a second,
shifted copy of it. With Debug framewarp on, those pixels were orange (moved).

## Cause

The weapon's side faces are seen only by the reconstructed eye, so they lie in
the band the other eye cannot fill and come from that eye's previous frame. A
previous-frame surface in the band is kept only if the source eye could not
see it, or if the camera has not translated. Both tests reproject it as world
geometry. A camera-attached texel (weapon, hands, cockpit) keeps its screen
place instead, so while strafing both tests failed, the side faces were tagged
moved and the inpaint mirrored weapon colour into them.

## Change

A camera-attached previous-frame texel is always legitimate band content, as
the resolve already keeps it where the other eye covers the pixel but cannot
see that surface. Standing still nothing changes: the unchanged-ray test
already accepted it.

## Test

Full OSR with Debug framewarp, strafe left and right with a weapon out: the
weapon stays solid on both sides, with no orange on it.
