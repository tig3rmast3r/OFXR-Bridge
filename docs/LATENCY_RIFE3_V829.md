# V829: steady-state latency telemetry and RIFE multiplier handover

This change fixes missing latency receipts with overlapped application frame
calls and an incomplete RIFE multiplier handover. Existing keybinds and pacing
settings are retained.

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

The handover regression requires acknowledgement, active multiplier 3, valid
frame-call ordering and the additional native output submissions, rather than
only a cleared UI label.

Unsupported transports still refuse multi-output RIFE. They now acknowledge
the rejected revision with E_NOTIMPL, restore the previous enabled state, and
retain the previous multiplier rather than leaving Applying settings pending.

## Focused verification

Focused regression checks cover:

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
latency/pacing.

The separate Wait thread is consistent with the OpenXR frame-loop model:
<https://github.com/KhronosGroup/OpenXR-Guide/blob/main/chapters/frame_submission.md>.

Use matching tray and layer binaries, and restart an already running game/tray
after updating. RIFE model/runtime data are separate from the bridge binaries.
