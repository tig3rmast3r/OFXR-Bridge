# V850: MSFS2024 Neural Rendering candidate

Local candidate based on V849. F8 witnesses remain available. No publication or installer/runtime/model updates.

## Changes

- Post-SR outputs requiring a single-slice readable surface are staged before encoding and resolved through private UAV staging before copying the base subresource back. Unrelated array slices stay untouched. Encode, downsample and resolve refusal is reported and stops NR; model success alone no longer increments the successful composed-frame counter.
- Packed R11G11B10 HDR colour (the affected MSFS format) gets a GPU-only exposure scale from256 stratified log-luminance samples. The target is0.18linear, exposure scale at least1, with per-view temporal smoothing. The user Paper white remains a multiplier. This changes only the private NR proxy and matching resolve constants; other colour formats retain their established transform. No CPU exposure readback or global game exposure change.
- The accepted main codec bytecode is unchanged. Immutable per-dispatch GPU constants carry the same measured white point through encode and resolve. Zero Detail with no Compare/Inspect uses an exact original-colour copy.
- NR no longer resumes every frame after an8ms retirement timeout. It waits for the exact previous GPU receipt, with a100ms safety watchdog, and latches an explicit unavailable state on timeout/unsubmitted recording rather than oscillating NR on/off. This is not a100ms added latency reserve: completed work returns immediately. A GPU backlog can increase CPU wait and reduce throughput; full-rate headset pacing is not certified.
- An explicit NR OFF->ON after failure retires the old scratch safely and retries with fresh known-state resources.
- F8 also records `nr_exposure_scale` and resource flags/array/mip/sample descriptions. Effective white point = settings white_point times the captured exposure scale. The offline viewer accepts valid raw footprints without trailing last-row padding.

## Validation and limits

Real D3D12 host/codec tests use mocked NGX model callbacks. They cover pre/post SR, stereo, quad depth arrays, post-SR colour arrays, preservation of unrelated slices, a24ms fence delay beyond the former8ms cutoff, watchdog latching and explicit retry. GPU exposure tests use actual accepted codec plus new small helpers, including NVIDIA replay of the four captured MSFS HDR sources. F8 writer/fences and existing codec checks are included; no broad games/pacing suite.

Captured MSFS sources originally produced about82-85% of pixels with at least one proxy channel at1. The normalized replays produce about16-18%, with mean encoded RGB0.49-0.52. This validates proxy preparation, not the neural model's enhancement or live headset flicker. The actual post-SR black-input resource refusal in V849 was not uniquely identified because its source flags/array count were absent from the old manifest; the new checked/staged path covers the reproduced array failure and records those descriptions for remaining cases.

For the headset test start this tray, restart MSFS, keep NR enabled and Diagnostics on, then try the selected pre/post SR mode. F8 works without changing Inspect. If the watchdog fires, the explicit status/log reason matters; it does not silently restart NR every frame. NR OFF->ON retries. RAW files remain authoritative.
