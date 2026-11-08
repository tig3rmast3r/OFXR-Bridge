# V921: benchmark selection by mean output and stability

Full-refresh results take precedence over the ordinary one-FPS group and every
latency comparison. The existing reconstruction-only preference at 90% of the
headset refresh is retained. Refresh is dynamic, including 72/90/120/144 Hz.

Selection uses mean output FPS, not median FPS. When available, unique receipt
intervals are counted over their elapsed time. SteamVR compositor records are
cross-checked against accepted new-content receipts; repeated content cannot
inflate the mean. These are runtime/acceptance timing boundaries, not a new
hardware scanout measurement. Each result retains its source. When receipts
are unavailable, a time-weighted mean of sampled rates is explicitly estimated.
Unsupported, incomplete and reset traces do not certify a steady cadence.

A measured full-refresh result requires a mean within 0.05 FPS of refresh,
no estimated missed slots, and a P99 interval no longer than 1.25 refresh periods.
The small mean tolerance accommodates timestamp/boundary rounding; it does not
excuse a detected missed slot. An estimated result requires sampled minimum FPS
within 0.1 FPS of refresh. This is a conservative proxy, not proof of scanout.

Among equivalent FPS results, compare candidates within 5 ms of the lowest
latency, then prefer stability: late-interval percentage, frame-interval standard
deviation and P99. Missing frame intervals use the existing sampled FPS/latency
ranges as a labelled proxy. Stability ties tolerate 0.05 ms of jitter and 0.1 ms
of P99 (or 0.05 FPS and 0.1 ms for sampled ranges). Vector/RIFE preferences then
resolve quality ties, followed by lower latency. Existing vector eligibility and
OSR quality rules are retained. No benchmark matrix or UI option is removed.

The details CSV appends the mean, its provenance, interval count/duration,
cadence validity/source, standard deviation, P95, P99, estimated missed slots,
long-interval percentage and incomplete-trace flag. Existing columns and the
10-row summary/paste format remain unchanged; the summary's FPS row is still
explicitly labelled median. Historical min/median/max cannot recover the mean
or per-frame distribution and must not be presented as measured new results.

Collection uses bounded CPU timestamp rings and the existing telemetry/OpenVR
polls. It adds no GPU query, sensor polling or frame pacing change. Matching tray
and layer are required after a restart because the telemetry ABI changes.

Validation is CPU-only: known steady/bursty/dropped/repeated traces, telemetry
loss/reset, dynamic refresh, scoring priorities and real controller CSV tests.
Game/headset validation requires new owner runs with the matching build.
