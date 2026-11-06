# V890: use history only to fill missing OSR surfaces

OSR now preserves the current stereo warp wherever the surface resolves. History
continues to record each real eye's colour, depth, motion and camera metadata,
but cannot replace or deform pixels already visible in the current stereo warp.
This removes character tremble that appeared when OSR was combined with frame
generation and disappeared with Use previous frame disabled.

The rule also applies to sky, near surfaces and object fringes. If the current
warp cannot resolve a pixel, or rejects a contaminated source-colour edge,
existing history visibility checks and inpainting still fill the missing region.
The old visible-history arbitration and HistoryFirst flag are removed.

History-origin selection remains deterministic, and residual screen-space motion
is retained for genuinely missing surfaces. These corrections do not reintroduce
history replacement over valid current pixels. Peer-eye pose, marker handling,
frame generation, pacing, NR and RIFE behavior are unchanged.

## Validation

Nine focused OSR tests pass, including both eyes, normal/lower quality, history
selection, motion, scene cuts, pose and inpainting. The visible-warp regression
fails against the preceding shader and passes here. A captured-frame GPU replay
preserves all 8,919,866 valid current pixels exactly; historical pixels occur only
in missing regions. The character-tremble correction was also confirmed in VR.
