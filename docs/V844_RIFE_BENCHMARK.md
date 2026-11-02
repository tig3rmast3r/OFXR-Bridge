# V844: RIFE benchmark tiers and common automatic choice

Both the optical and complete/vector suites now include RIFE4.4 Medium18% and
High21%, with the existing pacing/Fast/Buffer combinations. Luke's dedicated suite
keeps its reconstruction-only reference and optical routes at1x1, followed by
the same two RIFE qualities. The old2x2/4x4 passes and export columns are removed.
All benchmark reconstruction trials use1x1; original preferences remain saved for
Stop, error, or completion without automatic selection.

RIFE runs on VDXR too. Legacy VDXR keeps its accepted2X contract; private VDXR and
SteamVR may test3X/4X when the actual graphics/session path supports them. The layer
reports that structural limit. A temporary optical overlap presenter which the
existing RIFE switch retires is not mistaken for an unsupported D12 session.
No rendering path or multiplier support is enabled by these benchmark changes.

## Adaptive sequence

1. Complete ordinary candidates and all RIFE2X Medium/High combinations.
2. Test all RIFE3X combinations only if no valid preceding result holds refresh.
3. After all3X combinations, test4X only if refresh still has not been held.

Full refresh requires the median to reach the headset's actual Hz and every
measured FPS sample to be at least Hz-1. At90Hz this is median>=90 and minimum>=89.
One90FPS median with a60FPS dip is insufficient. The criterion applies equally
to automatic choice and tier escalation. Skipped supported tiers remain `n.a.`
in the wide CSV. Detail rows record the real RIFE2X/3X/4X multiplier; quality and
pacing are explicit in route labels. RIFE never requests or claims native vectors.

TensorRT optimization is setup time. While optimizing, the suite waits with no
optimization timeout; Stop and game-disconnection handling stay available. After
completion it starts the normal2.5s settling and5s useful interval. If compilation
starts during measurement, partial samples are discarded and that interval is
restarted after optimization. Unavailable non-optimizing RIFE setup has the usual
30s bound, is recorded as skipped, and does not discard ordinary-route results.
No installer, model download or game launch is initiated by the suite.

## Winner

- Confirmed reconstruction-only at90% of headset refresh keeps precedence,
  including the ordinary suites when their Off reference is actually OSR/AFW.
- Otherwise a stable full-refresh result outranks sub-refresh results.
- Preserve existing FPS grouping, legacy Djules3X sufficiency and stability
  ordering. The latency tie allowance is3ms for every route; the old vector12ms
  exception is removed. Native vectors win qualifying ties, then RIFE over other
  non-vector routes. Within that group, FPS range, latency range, exact latency
  and exact FPS remain the deciding order. Tolerances stay globally anchored.
- Failed, partial or unmeasured candidates cannot win. RIFE additionally requires
  an actual active-worker generation receipt; generic generated FPS is insufficient.

Maximum basic private matrices are81 optical,105 complete and37 Luke; existing
adaptive skips usually shorten them. Advanced maxima are61/79/28. Legacy maxima
are13/19/6. They are potential tests, not a promise all will execute.

## Validation

CPU/shared-controller fixtures cover plans and exact settings, both qualities and
all multiplier boundaries, actual telemetry minima, confirmed RIFE/vector rules,
unconfirmed output, legacy/private VDXR, capability limits, optimization before
and during a trial, indefinite optimization wait, CSV identities/memory ownership,
Stop/manual override/disconnection and unchanged startup NVIDIA preset. Actual
shared widgets check displayed scope and buttons. No headset, game performance,
RIFE model inference or broad pacing matrix is rerun for this change.
