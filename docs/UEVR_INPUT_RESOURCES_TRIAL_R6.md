# UEVR input/resource trial (V984, round 6)

## Invalid projection rectangles

PID31380 records projection offsets/extents INT_MIN after session reinitialization,
followed by downstream XR_ERROR_SWAPCHAIN_RECT_INVALID at 477.505 seconds and
persistent errors. Ordinary UEVR private sessions now filter an entire projection
layer if a known application swapchain has negative offsets, nonpositive extent,
out-of-bounds rectangle (64-bit arithmetic) or an invalid array slice. No crop is
invented and a stereo projection is never partially forwarded. Other layers and
application memory remain unchanged. Empty filtered frames use the established
empty-frame path. Clear temporal continuity and accept later valid projections.
Unknown handles and other applications retain their normal validation contract.
Trace 98480 records rejected projection count and retained layer count.

## VRAM investigation and trim

Depth swapchains retained a D3D12 temporal color-history object on FidelityFX
and the legacy NVIDIA route, despite projection generation never reading that
history. The new NVIDIA route already omitted it. Apply the omission to ordinary
private SteamVR D3D12 across flow backends. Enumeration keeps the real depth
images and guide-source COM references. The native guide-capture path remains
independent. Live route reconciliation retires an inherited history only after
its GPU producer/consumer work is idle. Luke, bridges, native-runtime legacy,
protected and ambiguous-use resources retain their existing policy.

This removes up to three allocated history copies per affected depth source;
slots were lazy and unused-source cleanup could already release them during
normal valid projection submission. Consequently the saving may principally
occur during startup/loading/reinitialization and is not a claim of persistent
whole-game VRAM reduction. PID31380 recreated two 5608x2860 format-98 depth
sources. Three full 8-byte copies of both model about 734 MiB before alignment,
ONLY if all slots were actually backed. The capture does not establish that
occupancy, so this is an upper-bound model, not measured reporter savings.

Other reviewed groups: generated panels already share source-specific native
sinks (R4) and absent panels already retire generation/history; projection
history has three slots for source/consumer overlap; 4X needs independent
retained synthetic outputs. Removing these without changing admission/ownership
would risk reusing images still being presented. No such reduction is included.
No game textures, runtime depth, resolution, quality, HAGS or live settings changed.

## Validation scope

New checks cover sentinel/negative/zero/overflow/out-of-bounds rectangles and
array slices, preserved auxiliary layers, immutable app descriptors, empty-frame
closure and exact later GPU pixel delivery. Depth checks run production image
enumeration and route reconciliation with real D32 resources, preserve native
guide sources, and retire an inherited three-copy GPU history. Prior menu,
worker, 3X/4X, Framework, and renewal regressions are included in the build checks.
Includes R1/R3/R4/R5. No version bump, commit, push, install or arming.
Headset acceptance and the actual VRAM improvement remain unverified.

Final validation: 42/43 pass in the grouped run. The older standalone
xrfg_private_stalled_renewal_gpu fails its timing-dependent latch assertion
there, then passes in an isolated rerun. Its production transport source is
byte-identical to packaged R5. Strengthened shipping EndFrame tests for NVIDIA
and FidelityFX assert the native runtime never receives an invalid rectangle
and later source generation succeeds; both pass. Thus all 43 selected checks
have passing results, with the grouped timing failure disclosed. No full suite.
The first compile also caught a test-only shared_ptr/bool setup typo, corrected
before the final build; no production workaround was made for that error.
