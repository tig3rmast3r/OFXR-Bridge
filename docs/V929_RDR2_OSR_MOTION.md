# V929: RDR2 OSR silhouettes and camera turns

RDR2 Full OSR now retains the original motion field with its own GPU interval
classification, uses exact camera-observation timing for N2 reconstruction,
cleans stale foreground colour from background history edges, and applies a
bounded two-eye game-yaw correction during steady camera turns. The owner
accepted these combined changes in the V927 r2 test on 2026-11-09: silhouette
ghosting was no longer visible and orbiting/turns felt smoother.

The yaw correction runs only with temporal FG off, valid adjacent camera and
head-pose history, stable game yaw, and no scene cut. Both eyes receive the same
world yaw; original history stays unmodified. Keep Luke Ross camera rotation
compensation off. This does not retime pitch, translation or object animation.
The history edge search can soften small occluded details, and the small yaw
warp clamps exposed borders and moves embedded game HUD pixels. No new wait,
readback, swapchain or pacing sleep is added. Other games keep their existing
selection and timing behavior; V928's Cyberpunk fix remains separate.

All four evaluations in PID71804's last two F8 captures have valid timing,
current/stored N2 history and no cut. The side-on horse images no longer show
the prior detached silhouette. The orbit source-eye deliveries match the
expected yaw reprojection within image quantization (mean error below 0.25 RGB
levels out of 255). No camera-pass preparation failures occur. Occasional
compositor repeats remain; perfectly uniform output and an FPS gain are not
claimed. Capture stalls are excluded from pacing statistics.

The tested r2 implementation built for x64/x86 and passed 33 focused tests,
including Cyberpunk/legacy controls, Vulkan transport, exact source timing,
classified history, both edge quality modes, and actual GPU phase warps in both
eyes/formats. The final normalized head-rotation guard also passed its CPU/GPU
rerun after rebuilding both layers. Full suite not run. The prepared V929
production code is identical to this accepted implementation; only version,
release documentation and package inputs change.

Details and diagnostics:
- [Classified history and source timing](RDR2_MOTION_TIMING_TRIAL.md)
- [Edge cleanup, camera phase and accepted F8 evidence](RDR2_EDGE_CAMERA_PHASE_TRIAL.md)

Original retained local commits: `2640926`, `40160c3`. Workspace evidence:
`logs/rdr2-motion-v927-r2-review`; sealed test artifact:
`OFXR-BRIDGE-0.3.3-v927-local-2640926-rdr2-motion-r2.zip`.
