# V861: the private runtime stops renewing native cycles when renewal stalls

This nightly update changes the private runtime's cycle renewal only.

## Symptom

On a 3D monitor (Samsung G90XF at 165 Hz with VRto3D 5.0), frame generation
ran the game at 1-2 FPS, in Director and Framework pacing alike.

## Cause

Renewal discards an expired native cycle and waits again. Under SteamVR with
VRto3D, each renewed `xrWaitFrame` blocked about 81 ms and returned a cycle
that was already expired, so the renewal loop never caught up: 8-18 renewals
per frame.

## Change

A renewed Wait that blocked more than 4 refreshes and still returned an
expired cycle ends renewal for that packet, which is delivered on that cycle.
Two such stalls in a row, within 2 s and with no current renewal between
them, stop renewal for the transport. A normal hitch returns late but current
and resets the count. This applies to all four renewal loops (native
contract, Director-only, Director slots, Framework).

## Flight log

Trace stage 45, `presenter_transition` 5755: b = blocked ns, c = stalls in a
row (2 = renewal stopped).

## Test

`xrfg_private_stalled_renewal_gpu` and `xrfg_private_stalled_renewal_native_gpu`
pass, and fail with the check disabled. In the D3D11 Director fixture the
slots renew current cycles between the test's stalls, which resets the count
by design, so the latch is checked on D3D12 Director and Framework there.
Replaying the reporter's logs, renewal latches after 2-3 stalls, about 1 s in.
Not yet confirmed by the reporter.
