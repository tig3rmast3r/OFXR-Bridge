# V924: Advanced AER framegen

Local unsigned trial based on V923 dfd9b63a. Restart matching tray/game (ABI92401).
No push or automatic game installation. V923 package/source remain preserved.

## Controls

Frame generation > Advanced mode exposes two flags: **AER framegen** and **Swap**.
Reconstruction contains Off/OSR/AFW; there is no separate OFR generator anymore.
Select the usual generator and its normal quality/size. AER framegen modifies
its stereo temporal outputs; it does not select or enable another engine.
The master Enable frame generation switch still controls whether FG runs.

AER permits **2X only**. Higher multiplier buttons are disabled, effective output
count is capped in runtime controls too, and saved ordinary3X/4X preferences are
retained for when AER is turned off. Swap reverses temporal phase only, never
physical left/right image identity. Flags are saved per game and persist when
Advanced is hidden; the generation summary identifies active AER. The ordinary
benchmark suites require AER off to retain their published configuration matrix.

The old V923 ofr_enabled/ofr_swap serialized keys are retained for compatibility;
ofr_backend/ofr_scale no longer select the active engine or quality. On migration,
choose the normal generator/quality and enable the normal FG master if it was off.
OSR/AFW preferences remain saved and are bypassed while AER FG runs. They can
resume when FG or the AER modifier is turned off. Neural Rendering is independent.
A session started in Reconstruction-only transport needs a restart to run FG.
For Horizon's special fresh-source transport, choose AER before startup and
restart when changing that source policy; live transitions remain unvalidated.

## Shared output path

Endpoint selection is appended at the common2X generator completion boundary,
after the normal A/B/S recording. FidelityFX/NVIDIA flow, game-vector/Hybrid,
RIFE and valid native FSR/DLSS-G outputs use the same AER operation. Native methods
retain their original guide/hardware requirements; AER cannot supply absent
motion vectors/depth or turn a provider bypass into an interpolated sample.

The selected backend retains its ordinary worker/presenter, pacing, buffering
and memory policy. AER no longer excludes the ordinary three-slot worker or
forces an optical-flow-only route. RIFE synchronous and queue-ordered inference
share the endpoint selection; failure fallback copies do not execute a retime
recorded for successful inference. Common native-output refusals are re-primed
as whole raw transactions. Endpoint writes are included in output readiness and
history lifetimes. Raw previous camera metadata and release masks travel with
the immutable job, rather than being read from a subsequent application cycle.

Paired regular AER: A=(L0,R1), B=(L2,R3) -> (L1,R1), (L2,R2).
Swap assumes the opposite temporal phase. Separately released alternating eyes
publish one coherent midpoint instead of also publishing the unused endpoint.
The existing physical presenter remains the sole XR frame-call owner.

## Limits and validation

This is a half-phase assumption, not an automatic per-eye render-time detector.
XR release IDs cannot prove pixel freshness when a mod recopies an old image;
shared stereo releases do not reveal which eye was rendered last. The V923
Valhalla captures verified endpoint routing/Swap, not temporal benefit.

Targeted CPU/widget fixtures check every method's selection, per-game settings,
Advanced-only controls, 2X enforcement and saved multiplier restoration. GPU
fixtures exercise endpoint selection against actual FidelityFX/NVIDIA and
synchronous/asynchronous RIFE midpoints. Mock-runtime delivery fixtures exercise
ordinary NVIDIA Director, Swap and FidelityFX end-to-end publication. See the
local V924 evidence for actual outcomes. Native FSR/DLSS-G in-game AER, headset
quality, cadence, latency and other games remain owner tests; no acceptance is
inferred from fixture success. No retired Valhalla depth/vector code is restored.
