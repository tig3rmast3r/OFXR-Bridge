# OFXR-BRIDGE 0.3.2 experimental (v801)

RIFE V801 separates completed synthesis from the delivery admission of its
previous batch. V800 could keep `synthesis_busy` set after its GPU work and
output publication were finished, until all prior outputs had entered the
private OpenXR endpoint. The application's next colour Release/capture and
EndFrame both consult this flag, so presentation backpressure could also hold
the game's producer while synthesis itself was already idle.

With two independent real-output rings, RIFE now finishes that computation
transaction after publishing its output descriptors. The next source can use
the available upper-buffer capacity while the previous real output is still
awaiting endpoint admission. A position check handles both cases where the
old real retires before or after the next pair finishes computation.

The delivery FIFO still orders S1, S2, then real B for 3X, and S1, S2, S3, B for
4X. Full buffers apply backpressure. Output leases, history consumer fences,
source lifetimes and private endpoint credits continue to guard actual image
reuse. Single-ring configurations retain their required output-ownership
serialization. The optimization applies to compatible private RIFE transports
in Framework and Director; the normal 2X Framework path already ends its
transaction at output publication.

The worker, TensorRT engines, models and shared portable runtime are inherited
from V800. Existing compiled engines remain reusable. There is no runtime
upgrade or engine rebuild required by this delivery change.

## Verification

The regression holds a real downstream OpenXR End call after GPU generation
has finished, filling the normal two endpoint credits while the batch's real
B remains pending. With the V800 policy, the application's next Release/capture
cannot complete until the downstream gate is opened. The same test requires
both capture and computation of the next pair to proceed with the fix, within
the existing bounded capacity, then validates ordered output and shutdown.
Additional tests exercise the race where B retires while the next pair is
computing, 3X/4X, Framework/Director and the existing multiplier transitions.

The negative-control failure and final test receipts are kept in the local
`rife-producer-overlap-v801` diagnostic directory. This establishes removal of
the identified producer wait. The final headset FPS gain requires a game run;
an aggregate 70% GPU reading alone does not quantify the bottleneck.
