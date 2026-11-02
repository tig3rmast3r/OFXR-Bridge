# V803: keep private delivery backpressure outside the meter lock

This change removes avoidable serialization between RIFE production and private
endpoint delivery while retaining bounded admission and resource ownership.

## Evidence from the two GWT 3X runs

Both runs used RIFE 4.4 at 20%, native UEVR stereo, 90 Hz, Fast mode off and
Increase buffer off. The V801 flight files are
`ofxr-bridge-flight-20261030-204028-902-pid57156-0.log` (Director) and
`ofxr-bridge-flight-20261030-204055-600-pid57156-1.log` (Framework).
The second flight includes earlier circular-buffer events; its active
Framework span must be selected by timestamps/configuration, not the first line.

* In Director, the producer spent a median 10.57 ms before its buffer handoff;
  9.84 ms overlapped the presenter's endpoint call. That overlap accounts for
  91.7% of the measured pre-handoff time. The corresponding Framework medians
  were 3.70 ms and 3.48 ms (84.7%).
* `OpenXrFpsOverlay::end_frame()` held the same mutex needed by the producer's
  `application_frame()` while private EndFrame could wait for FIFO capacity.
  The producer additionally held its GPU mutex around that meter update.
  This couples rendering/synthesis admission to downstream delivery.
* V801's earlier fix is active: GPU-ready to output publication was about
  2 microseconds. The remaining delay is a different dependency.
* Auxiliary ownership has a second common head-of-line wait. One output was
  selected 238 times in Director and 443 times in Framework, with median image
  acquisition waits of 7.501 ms and 14.935 ms. A second output was used only
  twice in each span, with 0.001 ms acquisitions. A descriptor reference count
  of one does not prove the endpoint has finished reading its image: endpoint
  packets retain the image's chain/readers independently of that descriptor.
* The Director had 205 missed physical slots; 200 coincided with the first
  synthetic of a new source pair. Its native handoffs did not burst.
  Framework instead had 146 intervals shorter than 5 ms in its active span.
* The Director also had substantially more settled zero-present compositor
  records (117 of 781 versus 15 of 1392 in Framework). Real/generated counters
  count accepted private outputs, not physical presentation. These quantities
  must remain distinct; no counter adjustment can replace delivery analysis.

GPU utilization by itself does not establish the cause of an FPS ceiling.
The mutex overlap provides direct evidence of CPU serialization. It does not
prove that eliminating this dependency alone achieves 90 FPS in the headset.

## Delivery and ownership

Private endpoint capacity is reserved before acquiring the meter mutex. Once a
credit exists, metadata composition and deep ownership remain under the normal
meter lock. RAII cancels unconsumed credits. Bounded capacity, frame order,
image leases, policy/drain/shutdown behavior and diagnostic-off execution are
preserved. Borrowed quad/menu metadata is not unlocked around a blocking call.

For private native auxiliary images, all reusable existing slots are tried
atomically before waiting for one. An unused slot is allocated only when needed,
within the existing three-slot storage limit. If all slots remain in use,
normal backpressure is retained. The Luke D3D11 public-output path keeps its existing
publication-fence contract. Image availability must never be inferred merely
from a descriptor reference count or a round-robin index.

No new RIFE model, inference calculation, runtime cache, arbitrary presentation
margin, output counter or resource pool is part of this change.

F9 additionally records native pack submission/wait, inference receipt and
full-resolution composition submission/wait for each fraction. The backend
record comes from the actual worker reply, not the existence of a cached
engine. These records add no GPU queries, timers or formatted logs while F9 is
off. They allow any remaining 3X limitation to be attributed without another
diagnostic-only build.

## Focused verification

All 38 selected final checks passed: seven core checks and 31 GPU/runtime
checks. They cover the six new regressions, endpoint ownership/drain/stop,
unchanged virtual prediction, measured and unmeasured UI admission, D3D11/D3D12
overlay lifetime, RIFE 2X/3X/4X, Framework/Director, live switches and Luke
interop. Negative controls fail on the old meter lock and old auxiliary slot
selection; the same checks pass after the fixes. The all-slots-busy case still
waits and preserves every pixel, pose and output order.

The runtime in these tests is mocked at the native boundary. They verify
correctness and independence, rather than physical-headset FPS or steady 90 FPS.
