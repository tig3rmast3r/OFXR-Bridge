# RDR2 history edge and camera phase trial, V927 r2

Base: 2640926, the owner-retained RDR2 motion/source-timing change, following
945b2ca, the owner-confirmed Cyberpunk driving fix. Both are local commits.
This test round kept V927; the owner accepted it on 2026-11-09 after PID71804.
No installation or game/profile/registry change was made by the agent.

## History edge colour

PID48968's side-on horse captures contain dark old foreground colour attached
to background depth in a history-filled strip. The current source legitimately
occludes that background, so rejecting only old foreground depth cannot remove
those pixels. RDR2 now checks such history colour near a depth edge against the
current source. If unconfirmed, it searches up to four depth texels outward
for a clean background sample, requiring a complete matching depth footprint
and no adjacent nearer surface. Unavailable clean samples use existing fill.
Clean background, current foreground and non-RDR2 paths retain their behavior.
The lower-quality path rechecks mixed-depth history footprints per colour
pixel so its shared 2x2 lookup cannot walk back into the contaminated edge.

This is a conservative local repair, not a claim to reconstruct the exact
unseen background texture. The short outward search can soften local detail.

## Camera yaw phase

The previous trial corrected N2 correspondence. This trial additionally rotates
BOTH delivered eyes by a bounded game-camera yaw correction after storing the
original history. It adds two private compute passes and one source-eye copy;
there is no new production wait, readback, swapchain or pacing sleep.

From exact native camera observation intervals, the phase offset is
`(previous interval - current interval) / 4`, bounded to 3.5 ms. At constant
angular velocity this cancels alternating 6/16 ms sampling into 11/11 ms steps.
It does not accumulate clock drift or intentionally delay a frame. Estimated
game yaw removes the XR head yaw; it requires the exact two previously consumed
XR head poses. Rapid head motion, invalid timing, resets, discontinuities,
reversals and materially changing angular velocity bypass the correction.
Correction is capped at 0.012 radians (0.69 degrees). The GPU independently
bypasses scene cuts and unavailable history.

This trial is RDR2 Full OSR only, with temporal frame generation OFF. Both eyes
receive the same world-Z yaw correction while retaining current head motion.
Pitch, translation and object-animation cadence are not retimed. Original
history and guides stay in the original camera; downstream FG is excluded
because its guides would otherwise require matching retiming. Eye-canvas
adaptations are excluded. This remains a camera-observation clock experiment,
not a verified simulation clock or a complete solution to presentation jitter.
Small exposed borders clamp; embedded game HUD pixels rotate with the image.

## Diagnostics and testing

Both x64 and x86 Release builds pass. All 33 focused CTests pass, including
the retained 31 checks plus the new edge and camera-phase fixtures. The latter
checks alternating sampling against a constant-speed reference, actual GPU
rotation in both eyes and RGBA/BGRA formats, scene-cut refusal, identity errors,
reversals, and head-motion separation. The edge fixture reproduces stale colour
on background depth and protects clean hidden background/current foreground.
Its initial failure reproduced alone: one asserted pixel belonged to source
fill, and the lower-quality lookup exposed a real edge-selection issue, now
fixed by the per-pixel mixed-depth check. The full suite was not run.

`luke_afw_stage` 1383: result=S_OK (0) when the two-eye correction was recorded,
S_FALSE (1) when no correction was needed/eligible, or a failing HRESULT if
private pass preparation failed. b=publication; c packs phase-seconds float bits
in high32 and yaw-radians float bits in low32. F8 `rdr2_camera_phase` uses the
same result with separate phase/yaw bit fields. Actual GPU eligibility is also
conditional on existing `osr_scene_state` available=1 and cut=0.

`afw_source_color` and `osr_resolved` remain BEFORE the final yaw correction;
`afw_generated_eye` and `afw_xr_terminal_eye`/exact delivery images are AFTER it.
This distinction matters when overlaying resolution tags on final images.
No additional full-size F8 readback is introduced.

Keep Luke Ross camera rotation compensation OFF and temporal FG OFF. Regression
checks should include horse-side silhouettes, steady controller camera orbit,
and headset turns to check that tracking remains responsive.
Build/test results and sealed package hashes are archived under
`Codex/patches/rdr2-motion-v927-r2` in the workspace.

## Accepted headset run, 2026-11-09

The owner reports no more silhouette ghosting and smoother orbiting/turns.
PID71804 used the exact sealed r2 layer, SHA256
`6a1c55f1d6f7add2f2acb75bb0383b3e8c1b41f00a34c0d1e6d5c1deb206cd71`.
RealVR.ini has all three YawPredict values at 0; the OFXR profile has Full OSR
and temporal FG off. The final two F8 captures are 23:37:37.962 (horse side)
and 23:37:49.581 (orbit). All four evaluations have valid source timing,
current/stored GPU N2 classification, available history and no scene cut.
The broad detached dark silhouette seen in r1 is not visible in these sampled
horse/rider edges. This supports the owner's report; it is not a matched
frame-by-frame A/B or a claim that every possible disocclusion is artifact-free.

The horse-side source images bypass yaw correction and are byte-identical to
their exact delivered source eyes. The orbit frames record correction in both
frames: +0.360875/-0.157475 ms, or +0.039353/-0.017266 degrees. Reprojecting each
captured original source with the logged camera/yaw matches its exact delivered
eye over 395025 sampled pixels per frame: mean RGB error 0.241/0.246 levels out
of 255 (99th percentile below 0.53), versus 9.43/5.66 without the correction.
Thus the correction reaches real delivered pixels, not merely diagnostic flags.
Generated-eye output also matches its exact delivery in each first F8 frame.
The second generated readbacks hit the diagnostic capture budget; exact packet
and eye-matched delivery copies provide those images instead.

Across the run, 1383 records 5649 corrections and 6713 bypasses, with no failed
preparation. In the 65-199 second window excluding 1.5 s before and 2.5 s after
each F8 evaluation, 4653/10666 evaluations record correction; eligibility is
deliberately selective. Source intervals still range from 6.09 to 15.95 ms at
the 5th/95th percentiles. The compositor logs 11112 unique frame records,
146 records with repeated presents and zero dropped presents in this window.
The 179-186 s window before the orbit F8 has 615 records, four with repeats,
zero drops. This confirms successful operation, not perfectly uniform output
or a measured FPS improvement; the run has no valid ongoing output-FPS sample.

Evidence, decoded images and the independent pixel check are archived under
`logs/rdr2-motion-v927-r2-review` in the workspace. The 33 focused checks passed
before this run; the final quaternion normalization guard additionally passed
the camera-phase CPU/GPU check after rebuilding both layer architectures.
