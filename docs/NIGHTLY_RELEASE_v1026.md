# OFXR 0.4.0 nightly V1026 ? maintainer handoff

Independent AER delivery now uses separate native publications as its fixed
policy. The temporary comparison switch and its profile/INI setting are removed;
previous saved choices cannot select the retired path. The main **AER framegen**
control remains optional. This consolidates the local V1017?V1025 work on V1016.

## Accepted baseline

The owner reports V1025 settling around **66 ms** in Alan Wake 2, improved from
about **71 ms** in V1024, and accepts it as the current baseline. This is a
single-title observation, not proof of equal performance in every game. The
owner plans broader testing. V1026 keeps that ON behavior and recovery tuning.

## Delivery and controls

- The first coherent AER endpoint can publish without waiting for its peer.
  AER 4X endpoints publish as they become ready. FIFO order and each image's
  poses remain tied to its own submission.
- Source recovery measures completed native-copy FIFO content. A single ready
  image can recover excess residence while the consumer owns its peer. Recovery
  arms above the target plus one quarter refresh, stops at the target and then
  holds phase within that band. It changes future producer appointments only.
- Stereo and the legacy pacing paths keep their existing recovery rules. The
  frame meter's latency origin is unchanged. A later 4X midpoint does not inflate
  the first mixed AER cycle's latency measurement.
- The all-effects-off path retains source timestamps/poses and bounded native
  lookahead. A narrow Alan Wake 2 window-thread handoff avoids forwarding a
  polling callback through a synchronous foreign-thread window/lock cycle.
- The meter identifies the generator: **AER Nvidia OFA**, **AER RIFE**, **AER Hybrid**,
  **AER FSR** or **AER DLSS-G**, with distinct waiting/bypass/error states.

## Benchmarks and RIFE preparation

Independent suites measure every supported multiplier even after reaching
headset refresh. Each trial has 2.5 seconds settling plus 7.5 seconds measured.
Normal/full suites keep the selected stereo/AER mode. Luke Ross includes OSR
normal/faster alone or with compatible generators, plus raw AER. FidelityFX,
AFW and Hybrid with OSR are excluded; Hybrid with AER is included. RIFE uses the
quality selected before the suite. CSV adds AER mode and mean latency. An Excel
exporter is intentionally not part of this update. Reconstruction setup labels
and documentation are limited to Luke Ross.

Lab executables cannot populate managed RIFE game history; the precompiler also
ignores any legacy lab records. Explicit profile isolation applies to history.
Preparation displays elapsed time while an engine is building. Cancellation
records distinguish the Cancel button from a window-close request and retain
its reason/time plus the helper's exit code. Completed engines remain cached.

## Verification and remaining work

Focused CPU, ImGui, hidden Win32 and WARP checks cover the source recovery,
AER 2X/4X, paired/alternate delivery, Off/On, profile migration, desktop/VR switch
removal, benchmark controller/matrix, typed meter and RIFE history/preparation.
No native NVIDIA/Radeon hardware matrix or headset test is implied by those
fixtures. The package requires matching tray/layer binaries (ABI 102601).

The reported unexpected RIFE precompile cancellation is **not yet attributed**.
Old signals recorded no origin; collect the new result/progress/cancel/log and
matching compiler build log if it recurs. The progress-window tests use a mocked
compiler and do not prove actual TensorRT compilation. Intermittent AW2 startup
and the headset's all-effects-off stability still need broader owner checks.

The older shared-publication-only one-slot recovery helper and its obsolete
fixtures are retired; the common source-phase controller is the single AER
recovery path. Previous local packages remain available for comparison.
