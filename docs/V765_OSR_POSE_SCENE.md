# V765: OSR peer position and scene boundaries

This nightly update changes only OSR reconstruction and its metadata. Frame
generation, NR, delivery/pacing, runtime providers and existing UI are unchanged.

## Reconstructed eye position

Successful Full OSR reconstruction returns the peer camera position used for
rendering. Its actual displacement from the source render camera is converted
through the source view basis, axis signs, XR/game baseline ratio and XR source
orientation. Only the reconstructed eye's XYZ changes. The real eye, peer
orientation, FOV and display time are preserved. Fresh submissions, retained
pairs and downstream snapshots carry the same corrected position. Legacy AFW
and failed reconstruction do not return a position receipt.

## Lightweight scene boundary

Group zero of the existing OSR clear dispatch compares a 32 x 18 grid against
the previous real peer frame using existing colour/depth and camera matrices.
A strong colour change across at least 75% of comparable samples, supported by
depth or chromaticity differences, discards history. A camera jump with almost
no overlap also discards it. Exposure-only changes and localized changes do not
satisfy the normal broad-change rule.

The GPU decision gates history splats and resolve in that same command list.
Both old eye histories are invalidated; only new-scene source frames repopulate
them. Motion spanning the boundary is also withheld. The cut frame uses the
current stereo warp and existing inpaint. There is no additional GPU dispatch,
normal CPU readback/wait, background task or frame-generation reset. The reader
runs only while Full OSR actually reconstructs an eye; OSR off and legacy AFW
do not execute it.

This is a conservative visual heuristic: similar-looking cuts and gradual fades
may not trigger. Actual decisions are available in the existing F8 capture as
`osr_scene_state` (five integer words: history permission, eye/motion validity
mask, cut flag, comparable/changed counts packed low/high 16 bits, motion
permission). Normal flight event 1310 reports CPU eligibility, not the GPU cut
decision. Standard logging does not yet provide a cumulative cut counter.

## Validation and use

Focused checks cover peer position math and recorded camera replay, fresh/held
terminal metadata and legacy behaviour, existing OSR history rules, and actual
OSR shaders on D3D12. GPU cases include hard cuts, unchanged-depth colour cuts,
same-frame new-scene output, repeated source eyes, motion boundary recovery,
exposure flashes, local changes, camera translation and no-overlap camera cuts.
Real-game scene detection still requires visual validation; automated fixtures
do not establish that every cinematic edit is detected.

Use the matching V765 tray/layer and restart the old game/tray for control ABI
76501. This update does not include the separate RIFE development lane.
