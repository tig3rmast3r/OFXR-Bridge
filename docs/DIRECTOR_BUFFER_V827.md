# V827: native SteamVR Director Buffer reserve

Base V826 `9097e4a636b15092fec0afa1ed1e842283d01ddf`; its asynchronous
RIFE eye-queue fix and the V825 desktop profile selector are retained.

V824 made Director Normal use the old Buffer capacity (three deliveries), but
its native consumer branch continued before the shared adaptive reserve code.
Consequently Director Buffer could not learn or refill any temporal margin.

Director Normal remains at three independently owned deliveries. Selecting
Increase buffer now adds one bounded fourth credit, allowing an additional
output to remain owned while the consumer builds ready lead. The native Director
branch now uses its own PrivatePacingBuffer instance, with the same learner,
0.5 ms maximum increment, 99%-104% measured refresh gate, and 1.75-period maximum
as the existing Framework buffer. Sustained below-refresh output does not train
additional timing delay; the manually selected fourth credit is still present.

Each learned increment is added to one future handoff deadline only, while the
FIFO and subsequent handoff chain retain the margin. Existing native prediction
renewals validate that delayed deadline. No acceptedAt plus historical work floor
is added, no images are repeated or discarded, and the minimum spacing protection
below refresh remains. Refresh and policy changes reset the Director learner;
an intentional refill is excluded from its own feedback. The normal qualified
future-Wait policy retains its original thresholds.

The Framework consumer and its timing learner are unchanged. Mode switching
restores Framework's requested two/three credits. Fast keeps its existing smaller
swapchain/image allocations. No model/provider/shader/NR/AFW changes. Benchmark
application waits now expect Director3/4 rather than waiting forever for3 after
a Buffer request. UI reports the actual active capacity; diagnostic configuration
bit46 records the actual Buffer checkbox independently of capacity bit44.

Focused validation includes the ACTUAL native Director branch with adaptive
learning/refill traces and exact ordered GPU pixels, four-credit/fifth-block
ownership, Normal3 and Framework2/3 restoration, near/sub-refresh Wait transitions,
aged-prediction renewal, existing Framework adaptive behavior, reserve limits/
reset/under-refresh exclusion, and benchmark application/reporting. Hardware game
pacing and MSFS RIFE startup acceptance still require the owner's game tests;
synthetic results are not a universal90fps or fixed11ms latency guarantee.

Validation: all10 focused checks passed in69.19s. A final native Director followup
additionally proves that each refill moves the actual handoff beyond the previous
handoff+P by the increment, and that native End respects the resulting deadline.
Both D11/D12 preserve all224 exact ordered pixels and clear learning when disabled.
One final link attempt hit transient LNK1104 on the test executable; immediate
retry succeeded with no source change. Initial/retry logs are retained. No runtime
or user process was stopped by the agent.
