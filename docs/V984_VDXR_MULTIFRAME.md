# V984: Native VDXR 3X/4X for split frame loops

Original local acceptance record. The complete published scope, including
OSR, Pimax delivery and guide alignment, is [nightly V984](V984_NATIVE_MULTIFRAME.md).

Native VDXR previously admitted ordered multi-output generation only up to 3X
and only for unsplit D3D12 frame loops. MSFS 2024 separates Wait and End across
threads, so OFXR and Hybrid fell back to 2X with 3X selected.

## Behavior

Ordinary native D3D12 VDXR sessions now admit ordered 3X and 4X. OFXR and
Hybrid retain their synthesizers and use the existing ordered output worker.
RIFE uses the same eligibility. DLSS-G 4X still requires the GPU provider to
report three generated outputs. FSR 3.1 and the 0.2.13 engine retain their
existing multiplier choices. Native D3D11/Vulkan interop and Luke split-loop
guards are retained; native Pimax eligibility is unchanged.

Live transitions preserve an existing native Wait ticket instead of issuing
another Wait before its Begin. An inherited borrowed 2X presenter finishes the
current boundary through the existing route-change rendezvous. Fast 3X/4X uses
distinct leased direct runtime destinations because legacy native staging has
only two synthetic destinations. Normal 2X retains its previous buffers.

The native overlap exception applies only to requested 3X/4X. Native RIFE 2X
retains its previous borrowed-frame presenter admission. Private SteamVR RIFE
retains its existing overlap policy at 2X, 3X and 4X. Framework on SteamVR uses
the private endpoint; Auto can select native delivery for VDXR or Pimax.

## Validation

Owner acceptance, 2026-11-12: the user tested the V983 r2 package in MSFS 2024
on both SteamVR and VDXR, all modes, and reported no issue. A Dead Island 2
sanity test also reported no issue. V984 records that accepted runtime change
with the version bump; no further renderer or scheduling changes were made.
These are user-reported functional results, not an independent scanout trace.

The accepted r2 focused run passed 40/40 checks in 53.80 seconds. It covers:

- Native split-thread and overlapping Wait/End calls, live 2-3-4-3-2 changes,
  Normal 2X controls, and Fast 3X/4X.
- Actual GPU output counts, real-image identity, ordered metadata, distinct
  native display targets, and image leases against a mocked runtime.
- SteamVR Framework 2X/3X/4X and switching, existing private 3X/4X GPU tests,
  midpoint scheduling and route handoffs.
- RIFE overlap policy for native split/unsplit loops at 2X/3X/4X and private
  SteamVR RIFE at each multiplier.

The existing xrfg_transport_new_vdxr_11_gpu test repeats the same HUD-pixel
mismatch on the existing baseline build. The fixture also exposes black
current pixels on the separate legacy native Fast 2X staging path; the new
Fast transition matrix uses Normal 2X controls and Fast 3X/4X. No full-suite
pass is claimed. The unchanged xrfg_luke_depth_gpu_tests all-target build also
encounters the Windows max-macro compile error at RealVrRuntimeImage.h:73;
the runtime, tray and focused targets build separately.

## Diagnostics

presenter_transition 84834 (presenter starts) and 84835 (control applied)
record a=requested ordered multiplier, b=effective native multiplier,
c bit0=split frame loop, bit1=borrowed/pipelined owner. DLSS-G 3X uses its legacy
triple flag, so its ordered multiplier field can remain 2. Native Output FPS
counts accepted content submissions, not independently observed headset scanout.

status.json now exports the layer's effective session multiplier, including
3X/4X and OSR/AFW, instead of the old triple flag and fixed Luke 2X assumption.
Without a game it exports the configured per-route multiplier. A requested 4X
that falls back to 2X/3X reports that fallback, not the request. JSON schema
remains 1. Exported rates retain their existing measurement meanings.

Additional reporting validation: live native VDXR and SteamVR Framework
fixtures assert the effective telemetry field through 2X/3X/4X switches.
The isolated status.json test covers configured and active 2X/3X/4X, including
an active 2X fallback overriding configured 4X. The broader legacy launcher
test fails before the status tests on unrelated runtime-INI expectations
(input scale/AFW mode after sanitization); its isolated rerun reproduces.

The public version is 0.3.3 (v984); control ABI is 98401 for the effective
multiplier field. Matching tray and layer binaries are required. Test rounds stayed
on V983 and remain identified by their package round, PID and timestamp.
