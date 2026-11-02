# V830: live VRAM/GPU telemetry and RIFE Quality presets

## VRAM refresh

Allocation-time samples alone cannot keep the interface updated or continuously
request the separate RIFE worker's private memory.

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
Use matching tray and layer binaries, and restart the game after updating.

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

Focused regression checks cover:

- `xrfg_standalone_launcher_tests`: generated runtime configuration uses the new default.
- `xrfg_gpu_usage_tests`: adapter filtering, per-engine aggregation, measured idle,
  unavailable/stale values, uppercase Windows LUID, and a real read-only PDH
  interval.
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

These checks validate telemetry and preset behavior. They do not establish game
performance, headset pacing or RIFE image quality.
