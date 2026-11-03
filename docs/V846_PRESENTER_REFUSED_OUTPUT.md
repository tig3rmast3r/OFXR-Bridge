# V846: a refused private output no longer stops frame generation

This nightly update changes only how the continuous presenter handles a frame
that SteamVR's private runtime refuses as out of order. Generation, pacing,
Luke Ross reconstruction and the UI are unchanged.

## Symptom

Under SteamVR, frame generation could freeze the headset view for good while
the game kept running, or freeze the game as well. It showed within seconds
when the application's frames arrive a few milliseconds apart, as with UEVR's
Alternating/AFR under OSR (V847) in Dead Island 2 and Atomic Heart. VDXR was
not affected: it has no private runtime.

## Cause

A presenter frame leases its private output swapchains only inside its End,
after the private runtime admits it, which can take a FIFO slot's wait. When
the producer acquired the same output again in between, the private runtime
refused the lease with `XR_ERROR_CALL_ORDER_INVALID`. The presenter treated
that as a failure: it failed every pending submission and left its loop, so
every later application End either failed or waited forever on a presenter
that was gone.

## Change

- A repeat or an owned output refused that way is dropped alone. The
  presenter carries on with the next frame, and the owned frame's submission
  completes with `XR_SUCCESS` (the application's call did nothing wrong).
  Transition 762 logs each drop: a = its display time, b = 1 for an owned
  output.
- The private runtime logs every frame call it refuses as out of order and
  which check refused it (presenter transition 5754, trace 44): code 1 End
  without a begun frame, 2 End reservation not this thread's, 3 begun frame
  gone before the End, 4 a layer's swapchain has no released image or that
  image is acquired again (a = swapchain, b = released index, -1 none),
  5 Begin without a Wait.

Holding the producer back until no presenter frame reads the output was tried
first and rejected: frame generation's outputs rotate across two slots of
swapchains, so that wait throttled nearly every frame (about 6.7 ms each,
Cyberpunk 2077 under Luke Ross).

## Test

Dead Island 2 under UEVR (Alternating/AFR, OSR) on SteamVR with frame
generation enabled on top: no freeze; drops appear as transition 762 and the
view keeps running. VDXR unchanged.
