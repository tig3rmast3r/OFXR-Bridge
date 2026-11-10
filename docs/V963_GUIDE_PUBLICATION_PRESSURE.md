# V963: prevent protected guide publication starvation

The shared Luke publisher has sixteen motion/depth snapshot slots and retains
fourteen exact colour-token records. A consumer holding three older frames
outside that window can fill the pool together with registry history. The next
publication then fails with ERROR_BUSY, while normal history eviction waits for
that publication to succeed. This can stall guide production without a settings
change. The pool/history policy predates the Cyberpunk original-motion update.

On snapshot pressure, the publisher may now evict an old registry-only token
record whose motion/depth writers have completed and whose snapshots have no
external CPU owner or outstanding GPU reader. It retries allocation through the
existing guarded slot selector. Recent stream anchors, external frame ownership,
unknown writers and unfinished producer/consumer fences remain protected. No
pool growth, extra GPU wait, stale-guide substitution or motion-math change is
introduced. Cyberpunk's accepted original-N1 driving fix remains intact.

Under pressure, a delayed colour may lose its old token lookup and use the
existing guarded refusal/fallback. Without pressure, the fourteen-record lookup
window is unchanged. This is a shared publisher fix, not a title-specific rule.

Diagnostics: luke_afw_stage 1189 records one reclaimed token (b) and the prior
history size (c), at most once per second. Stage 1187 keeps the first eight
refusals per reason, then at most one per second per reason; snapshot reasons
4/5/6 now carry the unsigned 32-bit HRESULT in c. This avoids losing the reason
for a later sustained refusal after startup's initial diagnostic budget.

Validation: the actual D3D12 history-pressure regression holds three old frames
while producing 96 frames with completed submissions and no setting changes.
It failed before the fix and passes afterward without overwriting held motion
or depth snapshots. Existing snapshot-lifetime and protected-guide GPU checks
pass, including exact token/history, CPU/GPU ownership and submission guards.
The owner reports no further loss in Cyberpunk and successful tests in other
games using the signed V962-r2 trial. Other titles were not individually named.
The full suite was not run. V963 versions that tested implementation for nightly;
the shared-control layout and ABI remain 96201.
