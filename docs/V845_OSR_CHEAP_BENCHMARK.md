# V845: explicit Luke Ross reconstruction matrix and OSR quality

V844 was withdrawn from nightly by revert583032f6. V845 replaces its incorrect
Luke Ross plan; valid ordinary RIFE benchmark policy remains included.

## Reconstruction controls

Select OSR in Reconstruction, then OSR (normal), the default, or
OSR (lower quality, faster). Both desktop and headset use the same control.
The choice applies live and is saved per profile as luke_osr_lower_quality.
The embedded temporary INI and applied-runtime receipt carry the same value.
AFW offers1x1/2x2; retired4x4 selections migrate to2x2.

Cheap mode computes one geometric decision per2x2 output pixels, then samples
original full-size colour at each output pixel. Inpainting remains full size.
Normal mode retains its original decisions, poses, history and scene detection.
Cheap resources/pipelines are allocated lazily; turning cheap off releases its
geometry map after the caller's existing submission fence has retired reads.
Without usable history, the normal resolve runs. Failed cheap setup also falls
back to normal; the active receipt reports the actual mode, not merely the wish.

The earlier controlled RTX4090 F8-derived benchmark at2986x2736 measured history
mode about0.85->0.70ms and0.88->0.72ms,17-18% less OSR work including history copies.
This is about0.15ms, NOT17-18% more gameFPS. The map adds about31MiB at that size.
Edges/disocclusions can differ; no headset temporal quality gain is claimed.
Stereo-only measured slower, hence it keeps the normal path in the product.

## Test Luke Ross

The reconstruction blocks are explicitly selected in this order:

1. AFW1x1.
2. AFW2x2.
3. OSR normal.
4. OSR cheap.

Each block has a reconstruction-only reference, the existing0.2.1/Djules2X/
0.3.0 optical routes, then RIFE4.4 Medium18 andHigh21. Preserve the existing
Framework/Director and buffer matrix; Advanced includes combined pacing.
Luke does not add Fast variants. AFW2x2 and OSR cheap are tested at every eye
resolution. The runtime's engine and cheap receipt must match before sampling.

Complete all four blocks and every2X combination before any RIFE3X trial.
Only test all supported3X combinations if no earlier valid result holds the
headset refresh stably; similarly4X after3X. Session capability caps multipliers.
Potential counts: private basic148, private Advanced112, LegacyVDXR24.
Counts shrink when higher tiers are omitted. No reconstruction block is skipped.
Labels and CSV identity distinguish AFW1x1/2x2 and OSR normal/cheap; detail CSV
also records OSRMode andOSRLowerQuality. Stop/errors restore the original profile.

Ordinary no-vector/complete suites retain RIFE Medium/High on SteamVR and VDXR.
Stable full refresh means median>=Hz and minimum>=Hz-1. Confirmed reconstruction
only at>=90%Hz retains priority; prefer normal quality if both qualify.
Latency equivalence3ms for every route; native vectors precede RIFE, then other
non-vector routes inside the existing FPS/latency groups. No RIFE fallback is
accepted without an actual worker-generation receipt. Existing stability ties
and Djules3X policy remain. TensorRT optimization must finish before the2.5s
settle and5s measurement; a late compile discards/restarts partial samples.

## Focused validation

Settings persistence/merge, shared desktop/headset widgets, exact benchmark plans,
CSV/controller simulations and adaptive tiers passed. Native GPU fixture toggles
normal->cheap->normal on both eyes, checks actual applied receipts, pose metadata
and D3D12 validation. Separate GTA headlock OSR fixture passed. No general game,
pacing, RIFE model inference, installer or driver matrix was run for this change.

ABI84501: restart the tray and game together before testing V845.
