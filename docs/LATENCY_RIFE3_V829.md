# V829: steady-state latency telemetry and RIFE multiplier handover

V829 is based on V828 (`f87f3fdcb999f5a446c1c33fdb6bdc883da158bc`).
It retains V828 Configure keybinds and the V827 pacing implementation.

## User evidence

Two sealed V827 MSFS captures, process 23192, were examined independently:

- `ofxr-bridge-flight-20261101-213320-707-pid23192-1.log`: latency values
  were invalid despite successful source processing and native delivery.
- `ofxr-bridge-flight-20261101-213634-255-pid23192-2.log`: requests 32 and
  35 did not receive a configuration acknowledgement; the active route
  continued producing 2X output. Later requests 33 and 36 returned to 2X.

The logs show native D3D12, separate eye images, asynchronous RIFE, overlapping
Wait/Begin on different threads, and End displayTime advanced by one headset
period relative to that source's Wait prediction. They do not record the exact
legacy presenter-mode flag at the failed request. The inherited-owner diagnosis
below is a reproduced matching failure condition, not proof of that unrecorded
state in the game.

Raw files and their hashes, initial failing experiments, build logs, final
test output, symbols and package receipts are retained outside the source tree
in `.local/diagnostics/latency-rife3-v829/`.

## Latency measurement identity

End previously recovered an interface timing cycle from the pending prediction
queue and required it to equal the active Begin cycle. With an overlapped next
Wait and a rewritten displayTime, the prediction selected by that lookup can
belong to another source. End then lost its timing ticket, and subsequent
producer/native completion could not populate the interface values.

End now takes the active, actually begun cycle directly. It only attaches it
when an application frame was in progress. The prediction queue still controls
frame submission as before; only measurement identity changes. Missing native
receipts remain missing: no timing estimates or validity checks were invented
to make the interface look populated.

A 64-source negative control on the old lookup retained native timing for only
the first four sources and failed at steady state. The corrected test retains
native receipts beyond warm-up (all 64 in the baseline run) and validates both
the cycle measurement and the independently bounded minimum-cycle estimate.

## RIFE 3X configuration transaction

The existing capability check refuses multi-output RIFE when the legacy
pipelined presenter owns the route. Preventing *new* promotion while RIFE is
selected does not retire a legacy owner inherited from an earlier optical
route. The old unsupported return also omitted the configuration receipt,
leaving the public control pending indefinitely.

On the native private D3D12 RIFE route, configuration now drains and joins an
inherited legacy presenter before validating and bootstrapping the native
multi-output worker. It explicitly opens the current prime's virtual Wait/Begin
cycle, because application Begin was a no-op while the old presenter owned it.
These calls target the private virtual endpoint; physical SteamVR frame-loop
ownership is unchanged. End also refreshes its mode snapshot for this actual
handover only, rather than enqueueing through the retired owner.

A partial experiment that only retired the owner acknowledged 3X but produced
XR_ERROR_CALL_ORDER_INVALID (-37); that rejected result is retained. The full
handover regression now requires acknowledgement, active multiplier 3, and
the additional native output submissions, not merely a cleared UI label.

Unsupported transports still refuse multi-output RIFE. They now acknowledge
the rejected revision with E_NOTIMPL, restore the previous enabled state, and
retain the previous multiplier rather than leaving Applying settings pending.

## Focused verification

Release targets built successfully. Exactly six selected tests passed in
31.69 seconds after the final source adjustment:

| Test | Required behavior |
| --- | --- |
| `xrfg_interface_metrics_tests` | Existing timing identity, source and validity contracts |
| `xrfg_rife-msfs` | 64 real GPU RIFE sources, split eyes, next-thread Wait before Begin, shifted End time, steady native timing receipts |
| `xrfg_rife-msfs-inherited` | Production legacy promotion followed by a public 3X request; completed receipt and additional native outputs |
| `xrfg_rife-msfs-inherited-buffer3-director` | Same inherited-owner handover with Director and Buffer selected |
| `xrfg_rife-msfs-switch` | Public 2X/3X/4X configuration changes with the overlapping application pattern |
| `xrfg_rife-unsupported-ack` | Unsupported transport reports an error, clears pending, and preserves working 2X settings |

The GPU cases use the shipping layer and real RIFE processing with a synthetic
downstream runtime. They do not measure game performance or prove headset
latency/pacing. No game scene, broad pacing matrix, or unrelated NR/UI suite was
run. In-game confirmation of both reported symptoms remains required.

The separate Wait thread is consistent with the OpenXR frame-loop model:
<https://github.com/KhronosGroup/OpenXR-Guide/blob/main/chapters/frame_submission.md>.

## Delivery

Private local V829 package only; no installation, runtime registration or
publication. Use the matching V829 tray and restart the old game/tray for
control ABI 82901. RIFE model/runtime data remain in the shared laboratory
runtime and are not duplicated into the ZIP.
