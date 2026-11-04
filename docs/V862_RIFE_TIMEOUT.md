# V862 — RIFE optimization errors and 0.3.3 version labels

The reported MSFS 3X run stopped TensorRT compilation after 180 seconds and
continued with the PyTorch CUDA Graph fallback. A matching `.failed.json` and
unfinished build log identify the timeout; the final flight-log receipts confirm
that TensorRT was not active. This was a compiler deadline, not a reported ONNX
validation or TensorRT network rejection.

Both the worker and its isolated compiler now allow **300 seconds**, including
shared-builder lock contention. Compilation remains asynchronous; generation can
continue while it runs. Parent-death cancellation, single-builder ownership and
the one-hour failure backoff remain. The compiler fingerprint changes, so engines
are compiled once for the new fingerprint. Existing cache files are preserved.

An optimization failure now crosses the Python/native/control channel. The meter
and settings show **RIFE optimization error** in red, with timeout/unavailable/
other-failure details. Frame counters also turn red when fallback is being used
after an error. Compiling normally remains a distinct warning. Switching routes
or disabling RIFE does not inherit a previous RIFE error. Automated benchmarks
stop explicitly on compilation failure rather than measuring fallback as a
successful optimized trial. The control ABI is 86201; use the tray and layer from
the same package and restart both when switching builds.

**Run Check RIFE after updating this package** to verify/repair the same-release
Python overlay in AppData. Updating the layer alone does not replace a previously
installed worker. Runtime dependencies, model versions and hashes stay pinned.
No driver, global Python, model quality, frame-delivery policy or queue depth is
changed by this fix.

The unhelpful V852 relative-HDR-colour resolve shader was removed following the
owner's game test. NR exposure, post-SR array staging, initialization and foveated
view fixes remain. NR colour/ghosting quality remains a separate investigation.

Product headers, meter, settings, tray resources, registration descriptions and
the ZIP now identify **0.3.3**. Engine choices such as NVIDIA 0.3.0 retain their
backend identifiers; historical documents are not release-version labels.

## Measurements and limits

In the retained comparable final log segments, RIFE averaged about 74.5 output
FPS, versus 80.5 for the complete Djules segment, which also reached 90 and dipped
below it. RIFE's native submission calls did not explain the plateau: producer
cycles were about 34 ms plus 5–6 ms before the next cycle. These are CPU wall
spans, including scheduling and GPU synchronization, not GPU-kernel durations.

An isolated RTX 4090 test at the same 440×403 single-eye inference shape compiled
successfully and measured four TensorRT inferences at about 2.6 ms versus 3.9 ms
for the Torch graph. A separate 4400×4030 two-eye/3X D3D12 transport probe measured
about 6.3–6.4 ms per pair including four inferences and full-size reconstruction.
That probe uses synthetic RGBA8 input, static poses and no game/compositor/NR
load; it does **not** certify 90 FPS in MSFS. In-game contention and the remaining
gap between that probe and the logged 34 ms cycle are still unisolated. This build
fixes the demonstrated compilation failure and makes any remaining one visible;
it does not claim that compilation alone explains the whole performance gap.
