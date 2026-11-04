# V867: F9 GPU timing for RIFE input dependencies

Diagnostic-only extension of V866. During normal F9 logging, async RIFE records
GPU timestamps before and after the existing A/B/history and output-acquire
queue waits, then before and after packing the first generated fraction.
It records current/required fence values on the CPU as supporting receipts.
No synchronization wait, sleep, queue depth, bank, model or NR policy is added.
CUDA inference retains its existing RPC wall timing; this change does not call
that interval a pure GPU inference duration.

One small reusable query/readback/command-list set is created per eye worker
only when F9 is enabled. The existing job completion fence retires the probe;
results are polled without blocking and a still-pending probe is skipped.
Turning F9 off starts no new probes; already queued resources remain protected
until retirement. Query/error failure suppresses diagnostics instead of refusing
generation. Standard GPU instrumentation need not be enabled separately.
These markers have diagnostic overhead while F9 is on; no zero-overhead claim.

## Receipts

Existing operation `gpu_fence_snapshot`, retained in flight AND benchmark sidecar:

-86700: a=source serial,b=job completion value,c=probe identity (per eye worker).
-86710/11/12: A/B/acquire respectively;a=fence address,b=required,c=observed value.
-86701: a=source serial,b=queue timestamp frequency,c=probe identity.
-86720..25: a=source serial,b=GPU timestamp on flight timeline in microseconds,
  c=probe identity. Boundaries are queue entry, after A, after B, after acquire,
  first pack begin, first pack end.
-86730..35: raw GPU ticks instead, if clock calibration is unavailable; convert
  differences using86701 frequency.
-86799: diagnostic failure;a=source serial,b=HRESULT,c=probe identity.

Consecutive timestamp differences isolate observed A wait, additional B wait,
additional acquire wait, submission/scheduling gap, and actual first packing
commands. These are incremental waits in the existing queue order: overlapping
upstream waits are not independent durations to sum twice. Compare GPU queue
entry with the corresponding CPU `rife_worker` begin to see earlier scheduling.
Use probe identity/worker thread and serial to keep the eye streams separate.

## Minimal validation

One actual D3D12 test, with three20ms controlled fence holds (A,B,acquire), checks
that each delay appears at its own GPU boundary and that all six markers reach
both log files. It also checks F9-off creates no GPU probe allocation, a pending
probe cannot be reset, and the D3D12 debug queue has no errors. No game/pacing/
RIFE quality matrix, model compilation or runtime installation was run.

Builds reuse the existing V866 source checkout/build directories incrementally
on a NEW local branch, preserving the sealed V866 ZIP/source bundle/symbols.
Release identity is V867/0.3.3, control ABI86201. Python/models/engine identity
and V866 normal output banks remain unchanged. Restart matching tray/game,
leave the same test settings, enable F9 for10-15seconds and send that log.
