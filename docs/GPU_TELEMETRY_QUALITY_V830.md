# V830: live VRAM/GPU telemetry and RIFE Quality presets

Based on V829 `15a6ba588c7d68210ffbe33e8370273e3e72a0e3`. All its fixes,
V828 configurable keys, and the V827 pacing implementation are retained.

## VRAM refresh

The user's V829 MSFS minilog is
`ofxr-bridge-flight-20261101-231757-790-pid12308-1.log` (5,366,451 bytes).
It contains no `vram_usage` or `rife_memory` events. Source inspection found
`log_video_memory_periodically` defined but never called. Allocation-time
samples alone cannot keep the interface updated or continuously request the
separate RIFE worker's private memory.

The existing periodic sampler is now called from the runtime report scope at
the end of each application frame, including the worker handoff paths. Its
existing throttle remains: once per second while the interface is visible or
during the first diagnostic minute, otherwise every five diagnostic seconds;
no polling when neither diagnostics nor interface telemetry is requested.

The memory accounting remains game-process DXGI usage plus the worker's private
usage. Shared imported resources and the PyTorch-reserved subset are not added
again. Unknown active worker usage stays N/A until a real worker receipt arrives.

## GPU percentage

The shared desktop/headset header and Elliott meter show GPU utilization beside
VRAM. A session-owned background worker collects Windows PDH GPU Engine
utilization at approximately one-second intervals. The submission thread only
renews the worker's lease and reads a cached result with a nonblocking lock.
The background thread runs below normal priority and closes its query after
three seconds without requests. Session destruction stops and joins the worker.

Counters are filtered to the game's rendering-adapter LUID. Processes are summed
on each physical engine, and the busiest engine supplies the displayed value.
This includes the game, RIFE worker and compositor rather than just one PID.
Concurrent engines are not added together. Values are bounded to 0..100;
an actual idle sample is valid zero, while missing/failed/stale data remain N/A.
Samples expire after three seconds and survive ordinary runtime heartbeats.
Version/control ABI are 830/83001, so use a matching tray and restarted game.

Microsoft's definitions and APIs:

- <https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/>
- <https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw>
- <https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw>

## Quality dropdown

The RIFE slider is replaced by **Quality** on both interfaces. Only these names
are shown; the percentages remain the internal configuration values:

| Name | Internal inference percentage |
| --- | ---: |
| Potato | 10 |
| Low | 15 |
| Medium (factory default) | 18 |
| High | 21 |
| Very high | 25 |
| Ultra | 30 |

New profiles and reset settings use Medium. Existing exact presets are
preserved; intermediate persisted values migrate to the nearest preset, with
ties choosing the lower value. Previous 40 maps to Ultra. The same normalization
is used for profile parsing, public requests and embedded INI reads; embedded
requests reject values outside the preset set. Persisted values remain actual
percentages for compatibility. Selecting a different preset performs the
existing shape reconfiguration once; no inference, cache or pacing algorithm
was changed and no existing engine cache was removed.

## Focused verification

Release shipping targets built. Exactly six selected tests passed in 5.77s:

- `xrfg_standalone_launcher_tests`: generated runtime configuration uses the new default.
- `xrfg_gpu_usage_tests`: adapter filtering, per-engine aggregation, measured idle,
  unavailable/stale values, uppercase Windows LUID, and a real read-only PDH
  interval. The final live read was 17.7356%; maximum observed cached request
  duration was 14 microseconds (a sample, not a timing guarantee).
- `xrfg_gpu_telemetry_widgets`: desktop/headset labels, stale display, all six
  actual Quality selections, and synchronization between the two interfaces.
- `xrfg_autonomous_control_tests`: preset defaults/migration/persistence and
  independently sampled GPU data preserved over the versioned control mapping.
- `xrfg_elliott_meter_tests`: header rendering and unchanged metric-row area.
- `xrfg_rife-msfs-memory`: 64 real GPU RIFE sources with split eyes, overlapped
  Wait/Begin and advanced End timestamps, using a synthetic downstream runtime.
  Without starting a benchmark, periodic reporting supplied valid worker memory
  (448,790,528 bytes private, including 16,777,216 bytes Torch reserved) and
  38,473,728 bytes game usage. Total is game plus private once. All 64 native
  timing receipts remained readable.

The first GPU counter test failed because the initial LUID comparison was
case-sensitive and Windows emits uppercase hexadecimal digits. That failure
and the correction's passing output are retained. An initial build command used
an incorrect forwarder target name; the corrected shipping target build passed.
The Elliott raster was also exported and visually inspected for header overlap.
No game/headset scenes, broad RIFE matrix, or pacing benchmark was run.

Evidence is in `.local/diagnostics/gpu-telemetry-v830/`, including the sealed
user log hashes, initial/final tests, full output, preview, source audit, PDB,
source archive and incremental Git bundle. Private local delivery only; no
installation, runtime registration or publication. Further in-game RIFE and
pacing validation remains with the owner.
