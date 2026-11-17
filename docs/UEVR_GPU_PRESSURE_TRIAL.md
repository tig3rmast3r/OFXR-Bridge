# UEVR GPU-pressure recovery trial (V984, round 2)

Includes the round-1 DLSS frame-time correction. No version bump. Source is
V984 / 053903b with local trial edits, isolated from the main checkout.

Reporter evidence: PID36340 progressively slows and fails its worker when fence
1824 remains at 1823 after the one-second synthesis completion wait. The device
removal query returns S_OK. PID31032 also slows, reinitializes a session, and
finally leaves an internal EndFrame unfinished for about 71 seconds. Video-memory
usage approaches/exceeds the process budget in both runs. The logs establish a
timeout and pressure, not an allocation leak or its owner. Active configurations
use optical OFXR rather than Hybrid, so these runs do not validate round 1.

This trial applies to private SteamVR D3D12 OFXR/Hybrid 3X/4X, excluding Luke,
OFR, RIFE and the D11/Vulkan bridges. At 95% of the measured process video-memory
budget, new worker jobs retain only their real output. They keep their original
image and pose and do not submit a new synthetic batch. This does not destroy
active resources or change the user's selected multiplier. Recovery requires
usage at/below 85% and a two-second minimum interval; the first resumed job
primes before pairing resumes. If memory remains high, real-only delivery remains
active. This is admission control, not a claim to release all generation memory.

If the existing one-second completion wait expires with ERROR_BUSY on this
route, the worker retains the exact prepared transaction, source ownership and
output images. It checks device health/session stop between 20-ms completion
polls. No new synthesis job is admitted during that outstanding transaction.
Only after all relevant GPU fences finish and private releases succeed can the
worker publish that transaction's real output; its late synthetics are discarded.
A two-second real-only cooldown follows, then a fresh prime. Device loss,
structural failures and other routes retain failure behavior.

An actually stuck GPU cannot deliver a safe unfinished real image. The producer
can remain backpressured until its GPU transaction completes; this trial does
not pretend to cancel GPU work, recycle its resources, or force a native
EndFrame to return. It does not claim to repair PID31032's final native stall.

Diagnostic presenter_transition records:

- 98470: a=memory-pressure active, b=usage bytes, c=budget bytes.
- 98471: a=late fence target, b=completed fence value when recovery starts.
- 98472: a=target, b=completed value after recovery succeeds.

Reporter: repeat the same Rebirth scene at the same 4X setting and return the
whole V984 log folder after the slowdown or at least five minutes of stable play.
Temporary reduction to real-only FPS is intentional under pressure. No game,
registry, NVIDIA profile or live layer setting is changed by packaging.

Validation uses a real D3D12 fence held beyond the production one-second limit,
the shipping completion function, pressure hysteresis and route/stop/error guards.
A shipping worker/transport test exercises real-frame delivery under injected
budget pressure, re-prime, and restoration of 4X. Ordinary multiframe and timing
regressions are included. Full-suite and headset/game acceptance remain pending.
