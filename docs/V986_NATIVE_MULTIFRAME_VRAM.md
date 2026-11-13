# V986: Native OpenXR multiframe output VRAM

V986 adds the native optical output allocation change tested locally on V984
to latest nightly V985 (`b1054bb`). It retains V984's multiframe delivery fixes
and V985's RIFE installer recovery. One logical change and one version bump.

## Behavior and ownership

Ordinary native D3D12 OFXR/Hybrid 3X/4X now uses one real and one generated
native publication swapchain per source. Independent real work images and both
complete synthetic work banks remain: two real images and four synthetic
images at 3X, or six synthetic images at 4X. Sources never share sinks.

Each output's existing descriptor retains its exact work image. At its existing
EndFrame boundary, the consumer waits for that output's synthesis receipt,
acquires/waits its runtime sink, copies the work image on the selected runtime
binding queue, and releases it before EndFrame. A per-work-image copy-completion
fence protects subsequent GPU reuse. Teardown retires the handover copies;
discarded staging descriptors do not publish over a live sink.

The overlap safety proof checks independent staging resources rather than
requiring separate native real handles. Inline priming after a live multiplier
switch also publishes its exact real image at its existing EndFrame boundary.

Normal and Fast use this allocation path. Producer admission, batch overlap,
descriptor leases, queue depth, predictions, pacing clocks, Wait/Begin/End
ownership and binding-queue policy remain intact. A GPU copy is added for each
output; unchanged scheduling does not establish zero copy cost or measured
headset latency parity.

Native 2X, private SteamVR, RIFE, DLSS-G, the legacy 0.2.13 engine, Luke/OSR and
interop retain their allocation paths. No new setting, ABI or profile change;
control ABI remains 98401. Use matching V986 tray/runtime files and restart.

## Allocation evidence

For N runtime images per swapchain and I allocated bytes per source-shaped
image, output-buffer allocations change from 6*N*I to (2*N+6)*I at 3X and from
8*N*I to (2*N+8)*I at 4X. This excludes application images, history, flow/guide
resources, providers and opaque driver allocations.

Actual GPU resource counts and GetResourceAllocationInfo verify 33.33% less
output allocation at 3X and 41.67% at 4X with a three-image runtime. With two
runtime images, savings are 16.67% and 25%. These are output-buffer savings,
not total game VRAM percentages.

## Verification and limits

The V984 trial passed the existing 100-case focused matrix and four new
three-image array-stereo allocation/reused-index checks. It covers native
VDXR/Pimax/generic 2X/3X/4X, Normal/Fast, live switches, optical backends,
blocked-EndFrame overlap, per-output publication, native pacing/queue
readiness, SteamVR controls, OSR, guides, vectors and status reporting.

Two new packed-stereo pixel probes fail at 3X/4X. Each reproduces alone and
with pooled allocation disabled using the original direct runtime banks; the
3X control records identical per-output left/right pixel values. This exposes
an existing synthesis limitation, not a new publication regression. Shaders
and provider synthesis are unchanged. The full 106-case set is not clean;
no full-suite or physical headset latency claim is made.

The owner's DI2 Hybrid 3X comparison used private SteamVR in both runs, so it
correctly showed unchanged VRAM and about 90 FPS compositor delivery. It did
not exercise this native allocation change. Native Pimax/VDXR hardware VRAM
and performance comparison remains pending. Native submitted frame counts are
not independently measured physical headset output.

On latest V985 with V986 metadata, the repeated 106-case matrix passes 104;
the same two packed-stereo probes fail again individually. All 100 existing
cases and four new array-stereo allocation/reused-index cases pass. The
inherited V985 RIFE installer also passes all 32 CPU/Windows-lock checks.
V986 build checks and artifact hashes are recorded in BUILD_PROVENANCE.json
in the packaged release. Local trial logs remain outside git under repo/
native-vram-*.log and .xml. V986 runtime logs use RuntimeLayer/v986.
