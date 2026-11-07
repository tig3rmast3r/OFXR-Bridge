# V898: preserve benchmark continuity during live route initialization

The benchmark controller previously treated any frame-status heartbeat older than
five seconds as a disconnected game. A Ghostwire Tokyo V895 run with NR enabled
stopped on its first RIFE Medium 2X Framework Normal trial. The captured presenter
stream pauses for 8308 ms at that transition, then resumes in the same process
after the suite has already restored the original settings. This is not evidence
of a game exit or a Fast-only failure.

A stale heartbeat now checks the original game process nonblockingly. While its
session remains present, the controller waits up to 30 seconds of heartbeat age,
or 65 seconds for RIFE's existing 60-second cold worker handshake. It discards
current samples and memory receipts and restarts the entire settle/measurement
interval after fresh status returns. Closed sessions, replaced processes and
exited processes still stop immediately; a live process that exceeds the bound
gets an explicit frame-update timeout rather than a false disconnect. Stop and
manual edits retain their existing behavior. No stale result can win.

This does not claim to remove route-rebuild pauses in the headset counter. The
trace does not identify which internal initialization operation consumes the
whole 8.3 seconds. No pacing, Fast allocation policy, RIFE model or NR change.

Validation uses the actual controller with isolated profiles, NR enabled and an
8.31-second heartbeat gap at the first RIFE trial. It verifies the same route
resumes from a fresh interval, no false CSV result, exact settings restoration,
closed-session handling, and the two hard grace boundaries. Existing benchmark
matrix, adaptive-controller and control tests remain required.

F9 also records one UI progress sample per second in the compact benchmark log,
which survives verbose-log rotation: independent FPS/meter/menu attempt and
successful-publication counts, age since last upload, acquire/wait/fence stage,
UI/atlas flags, and a timed control-reconfiguration span (89830). This is passive
CPU-side diagnostics; it adds no GPU captures or additional XR allocations.
The owner's report of an image frozen for an entire trial remains unconfirmed:
it must be distinguished from a slow initialization using these records.
