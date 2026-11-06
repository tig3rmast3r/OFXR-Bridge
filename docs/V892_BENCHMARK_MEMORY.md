# V892: benchmark VRAM and complete summary export

A RIFE worker can be alive before its first inference, or remain idle while an
optical-flow route is selected. Previously, worker VRAM was sampled only with an
inference reply. A missing or expired reply made combined VRAM unavailable even
though the game's own DXGI measurement was valid.

Requested telemetry now samples an idle worker on a background CPU thread, at
most once per second. The rendering caller only reads a cached receipt. The
sampler skips busy inference transactions, shares their existing pipe lock and
adds no CUDA synchronization, image operation or inference. Active inference
retains its existing piggybacked receipt. No sampling is performed without a
telemetry request.

Before CUDA is initialized, worker GPU-private usage is measured as zero. After
initialization, DXGI worker usage excludes imported shared-buffer charges, as in
the existing inference receipt. Retained idle CUDA allocations remain included;
missing DXGI measurements remain unavailable. An explicit diagnostic-command
refusal from an older worker does not terminate that worker.

The benchmark summary also stops comparing inactive RIFE quality/model/multiplier
settings for non-RIFE trials. Changing RIFE quality later in a suite therefore
cannot erase earlier completed reference and optical-flow measurements. RIFE
trials still distinguish every tested quality and multiplier.

System RAM working set and private commit sampling are unchanged. Previously
unrecorded VRAM cannot be reconstructed retrospectively. No reconstruction,
frame-generation, pacing or benchmark winner rules change.

Focused validation covers worker memory replies before CUDA, imported-memory
accounting, unavailable DXGI, idle refresh before/after a small real inference,
nonblocking receipt reads, complete adaptive benchmark exports and code-only
runtime overlay updates.
