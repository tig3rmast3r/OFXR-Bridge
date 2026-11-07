# V904: current native motion for OSR own-eye history

Local trial based on V903 (V889 history base, no V890/V891). No pacing, framegen,
pose, NR, RIFE, AFW or settings changes. No push. Headset acceptance pending.

For uninterrupted native AER span2, use the CURRENT composed same-eye vectors to
locate the native source-eye sample two publications earlier. Reconstruct both
endpoint positions in the intervening peer-eye history camera and interpolate
3D positions (divide each clip endpoint by its inverse-projection homogeneous
view W before averaging). Sample that peer's OWN previous colour. This replaces
old-field extrapolation and colour arbitration only for validated visible interior
surfaces with at least half a colour pixel of object deformation.

The existing relative depth tolerance remains3%. Expected depth now corresponds
to the intervening publication, including motion in depth; it is not compared to
the current endpoint depth. This addresses the rejection seen in the initial
CPU-only current-vector prototype. No arbitrary vector gain or depth widening.

Conservative fallback: old V889 path on sky, near-camera surfaces, inferred
screen locking, subpixel/static motion, invalid/out-of-bounds motion/depth, missing
or discontinuous native history, scene cuts/reset, epoch/stream mismatch, span1,
fringes or true stereo holes. Use-previous OFF remains history OFF. Occluded fill
and its direction rules are unchanged. Generic UEVR does not enable the new path.

No new dispatch/image/swapchain, production readback, wait, or queue latency.
Two extra SRV bindings, existing source-eye depth history, one256-byte uniform
upload buffer (D3D12 allocation granularity applies), CPU camera products. Existing
serialized AFW fence ownership protects upload/descriptor reuse. Root constants
remain62DWORDs. Borrowed motion and both eye histories restore original states.

Diagnostics: existing flight event1310 bit2(value4) reports CPU eligibility;
GPU scene state can still veto it. F8 also snapshots osr_native_previous_depth
and osr_history_depth BEFORE the source history is overwritten. Normal operation
performs no diagnostic readback. Existing source motion/colour captures preserved.

Focused production-shader tests: both eyes and both qualities, both XY motion
signs, previous colour distinct from current colour, stale history field, changing
perspective depth, invalid depth/static/near fallbacks, D3D12 validation. Existing
history confidence/selection/screen-lock and scene-cut contracts retained. The
scene-cut exposure assertion was updated to V889 previous-base behavior (V890 had
left an always-current-colour expectation); cut detection itself is unchanged.

Paired synthetic RTX4090 timings,3296x3020 colour/1648x1510 depth,24 samples per
variant, alternating order, equal quality. Static new-old total medians about
+0.047ms normal/+0.008ms cheap. Uniform dynamic scene about-0.224ms normal/-0.052ms
cheap because valid pulls bypass expensive old selection. These are laboratory
scenarios, NOT game performance or proof that temporal flicker is solved. Full
GPU cost CSVs and test receipts are in the local V904 evidence folder.
