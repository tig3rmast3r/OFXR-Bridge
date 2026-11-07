# V900: CPU usage in desktop and headset meters

The desktop panel and headset settings overlay show a CPU usage bar directly
below GPU and above VRAM. The wide FPS meter, at the bottom edge or on a flipped
controller, shows CPU percentage below GPU in its header. Its dimensions and
FPS, timing and status row positions are retained.

CPU is total system processor busy time averaged across logical processors and
all processes, read from the English PDH counter
`\Processor Information(_Total)\% Processor Time`. It measures processor time,
not frequency-adjusted processor utility or game-only usage.

An independent CPU query runs on the existing session GPU sampler worker; no
extra thread or synchronous PDH collection is added to OpenXR submission.
The first interval primes the query. Sampling follows the existing one-second
cadence and three-second interface visibility lease. Failed, disconnected,
non-finite, out-of-range, future or three-second-old readings show N/A.
Runtime heartbeats retain CPU telemetry. IPC ABI 90001 requires matching tray
and layer binaries. Providers and frame pacing are unchanged.

## Validation

The V899-based local r2 x64/x86 build was confirmed working in headset by
Julien on 2026-11-07, including the wide FPS meter. Ten focused CTest entries
passed across the two implementation rounds: GPU/CPU live sampling, shared
control, settings widgets, telemetry widgets, menu presentation, desktop-header
scope, FPS overlay model, wide-meter raster and D3D11/D3D12 OpenXR wide-meter
submission. The latter fixtures exercise bottom and controller placements.
Desktop, headset-settings and wide-meter previews were visually inspected.
Raster checks cover changing CPU values, stale expiry and preserved metric rows.

The full suite was not run; no new full-suite baseline is claimed. The layer
was not installed or armed by the agent. No NR model is included in packages.
