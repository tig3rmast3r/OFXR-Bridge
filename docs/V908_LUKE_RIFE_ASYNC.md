# V908: preserve Luke RIFE asynchronous execution during settings changes

Changing provider, RIFE Quality or multiplier on Luke Ross could replace the
dedicated asynchronous generator with synchronous RIFE. The layer reused the
generic native-D12 settings policy, which intentionally excludes the Luke
D11/D12 bridge, and left the wrapper's execution flag stale.

Creation and drained reconfiguration now use the same Luke-specific policy.
Both the primary generator and the post-reconstruction temporal generator
receive those options, and the wrapper is updated after successful replacement.
The copy-only ingress remains on its existing synchronous capture lane.

Before draining shared Luke history, reconfiguration retires all old generator
consumer leases against their own completion receipts. Otherwise the ingress
helper can report busy while the primary generator still retains its rolling
source, even after physical output drain. GPU completion waits remain intact.

This preserves per-eye queues and workers after settings changes. Existing
producer, output-reuse and publication fences remain in place. The generic
native/MSFS policy and Framework/Director presentation schedules are unchanged.
The accepted V907 OSR reconstruction is unchanged.

Focused regression coverage checks the actual native RIFE worker and its
independent submission queue after a Quality-only change, a switch through
optical flow and back, and 2x/3x/4x multiplier changes. The fixture also verifies
complete physical output batches; a wrapper flag alone is insufficient.

The Crimson V907 diagnostic showed synchronous pack waits around 10 ms median
on one context and approximately 32–33 ms between inference calls per context.
These are CPU wall timings including dependencies, not pure GPU kernel costs.
V908 requires an in-game comparison to establish its FPS and latency benefit.
