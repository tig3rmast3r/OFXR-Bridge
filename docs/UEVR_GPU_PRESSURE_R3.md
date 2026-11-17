# UEVR completion-delay recovery trial (V984, round 3)

This supersedes round 2's memory-only admission policy. It retains the round-1
DLSS frame-time correction and round-2 safe late-fence recovery. Version remains
V984; the package name, layer hash, PID and timestamp identify this round.

PID16576 proves round 2's guard suppressed generation: stage98470 enters at
310.397 s with 13.59/13.68 GiB, clears at 334.153 s with 11.32/13.46 GiB,
re-enters at 353.431 s with 13.09/13.56 GiB, and clears at 370.156 s with
11.11/13.46 GiB. All 7,134 recorded worker transactions succeed; there is no
recorded one-second recovery timeout. The high 85% recovery-headroom requirement
could hold frame generation off while resident resources remained allocated.

Round 3 changes:

- Memory-budget observations are diagnostic only. High residency never starts
  or extends a real-only period, and there is no 85% recovery gate.
- A successful paired worker transaction whose actual completion wait takes
  at least 100 ms starts a two-second real-only backoff. The measured interval
  is CPU time waiting for the exact GPU completion fence, not a claimed GPU
  timestamp or a guessed engine/render period.
- The completed transaction's real image and pose are retained; its late
  synthetic outputs are dropped. Subsequent real-only prime jobs cannot extend
  the backoff. At expiry, the route re-primes and can retry the selected 3X/4X
  setting even if video-memory usage stays high. A genuinely slow retry may
  start another bounded interval. These are trial thresholds, not game-validated
  tuning constants.
- The one-second healthy-device timeout still retains unfinished resources,
  polls one registered event with device/stop checks, and waits for safe
  completion before releasing images. Device loss and structural failures are
  not treated as recoverable completion delays.

Scope remains private SteamVR D3D12 optical OFXR/Hybrid 3X/4X, excluding Luke,
OFR, RIFE, D11 and Vulkan bridges. Selected settings and profiles are not rewritten.
This is not a cure for a hung GPU or a native EndFrame that never returns.

Diagnostics:

- 98470 now means memory at/above 95% (a=1) or below it (a=0), with usage/budget
  bytes in b/c. It no longer implies any generation admission change.
- 98471/98472 retain their one-second timeout/recovery meanings.
- 98473 records a measured slow completed pair: a=source sequence, b=completion
  wait in microseconds, c=backoff duration in milliseconds (2000).

Validation covers high-memory/fast-GPU 4X delivery with all expected outputs;
actual sub-timeout slow D3D12 completion; no early return on a one-second fence
timeout; real-frame pixel/pose identity during backoff; retry/re-prime/restored
4X at persistent high memory; repeated samples and prime jobs not extending the
deadline; normal/error/excluded/stop paths; and ordinary multiframe regressions.
Full suite and reporter/headset acceptance remain pending.

Reporter: repeat the same Rebirth scene at 4X for at least five minutes and
return the whole V984 log folder. No menu transition should be needed for a
retry. A short temporary drop to real-frame FPS can still occur after measured
slow GPU work. The package excludes rife-vulkan and nvngx_dlssnr.dll.
