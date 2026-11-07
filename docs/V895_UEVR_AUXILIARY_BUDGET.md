# V895: UEVR auxiliary publication budget and real-frame recovery

Build V895 reduces UEVR auxiliary native-handle usage and fixes recovery after the native swapchain limit is reached.

A V894 fatal report establishes the failure sequence: an auxiliary
copy is refused with XR_ERROR_LIMIT_REACHED; recovery transition763/a2 releases
the complete generation budget; the next synthesis job still enters the worker,
which cannot find either projection generator (reason4, missing_generation).
The graphics device itself reports no removal. This then latches runtime failure
for later application frames. V894's hidden-panel retirement is retained.

In the two seconds before the refusal the flight trace records three distinct
owned output handles for one application panel. They are independent
snapshots of the same source, not three different user interfaces. With the two
eye publication endpoints, this explains the odd five-handle overhead without
the OFXR menu/meter. Swapchains are not required to occur in eye pairs.

UEVR D3D12 private auxiliary copies now use one source-scoped native publication
destination per panel through the existing private-runtime pooling mechanism.
Their private GPU images and reader leases remain separate, so queued content
cannot be overwritten by the next UI update. Different sources keep separate
destinations. Three retained snapshots need one native handle instead of three.
Luke, Vulkan, D3D11 and non-UEVR auxiliary publication are unchanged.

When recovery must relinquish all generation, the triggering application frame
and later frames take the existing real-image path. They cannot enter a worker
whose generators were destroyed. This is reduced functionality under an
exhausted native budget, not a claim that generation can continue without any
available handles. An actual impossible real-image allocation can still return
its error; it must not manufacture a missing-generation worker failure.

The native cap reproductions fail on old production code and pass on candidate:
three retained UI snapshots under a three-native-handle cap retain ordered pixel
values; late menu opening after a meter consumes the remaining budget preserves
real delivery. The fallback check verifies the final four source images and
their own poses in order. Panel lifecycle, source pooling, atlas, metadata,
inventory and two-panel pixel identity regressions are targeted checks only.
No game/pacing matrix. Headset validation remains required.

Menu and meter still have separate swapchains in this candidate. Sharing an
atlas is possible but is not part of this worker/budget fix. No OSR pixel/pose,
RIFE worker/model, NR, benchmark-ranking or global pacing changes.
