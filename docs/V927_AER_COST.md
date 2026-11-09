# V927: raw Luke AER capture overlap and all-off admission

Based on V925; this does not merge the separate Radeon/Vulkan experiment.

Raw Luke D3D11 AER with RIFE may capture the next source after the worker has
queued every input and acquired history leases. One credit per source and job;
EndFrame still waits for job completion. Only isolated interop lanes qualify.
Shared interop, bridges, auxiliaries and post-AFW inputs keep strict ownership.
The existing three history slots, leases and reuse fences remain authoritative.
Repeated-eye inference is already skipped; this does not add pixel detection.

With FG, reconstruction and NR off, private Wait blocks while an accepted output
is pending. Backpressure occurs before the next game render instead of filling
the End FIFO. Live reenable wakes the wait and restores selected effect policies.
Required copies, conversions, native Wait/Begin/End and expired-cycle renewal stay.
This remains an intercepted route, not a promise of zero hook overhead.

F9 benchmark configuration adds bit48 active AER framegen, bit49 saved Swap,
bit50 clean admission. AER source decisions (presenter_transition9230) are now
retained in the compact benchmark log, so mixed sessions can be segmented.
Existing timestamps are processing/delivery measurements, not motion-to-photon.

A 90-to-70 FPS change corresponds to roughly 3.17ms extra per delivered frame.
Observed RIFE inference alone cannot establish total overhead or GPU overlap.
Fixture correctness does not certify in-game FPS or headset pacing; compare the
same scene and size with all off, OSR, and AER framegen after a matching restart.
