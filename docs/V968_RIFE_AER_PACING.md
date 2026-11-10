# V968: preserve RIFE AER presentation cadence

The owner accepts V967 GTA game-vector framegen. The remaining RIFE report is
different: High initially produces about87..89fps, Medium later falls to63..66,
and returning to the same High inference dimensions does not recover throughput.
Inference becomes faster (about2.9ms initially versus below1ms after the drop).

The first learned RIFE native handoff gate appears at400.965s in the retained
run, and the first phased delivery at400.976s. Subsequent handoffs alternate
11.11/22.22ms slots. The phase persists across quality changes. This correlates
the sustained loss with the additional paired-RIFE handoff policy, not a model
cost increase. It does not by itself establish GPU headroom in every scene.

AER emits one mixed real/generated stereo packet per alternating input. Applying
the ordinary paired-output phase to it can defer an otherwise ready packet and
return source capture credit too late for the next refresh. For RIFE+AER only,
V968 therefore keeps the native delivery path used before that phase was learned.
Both initial endpoint setup and live reconfiguration use the same predicate.
Ready-image ownership, real OpenXR Wait/Begin/End, FIFO pixels/poses, Framework,
Director and Buffer remain active. No raw runtime hook is bypassed.

Ordinary non-AER RIFE retains the V955/V961 handoff correction. Other generators,
VDXR, RIFE inference/compiler/model/runtime and V967 vector admission are unchanged.
Shared-control ABI remains96201. No new option, forced quality or FPS cap.

The regression uses a mocked native compositor with real software-D3D12 images,
one AER output per source credit and precise source-work timing. With the old
policy it falls to about70fps after learning a native gate; the corrected test
maintains near90 with exact FIFO pixels/cameras through Framework/Director,
buffer capacities and source ring counts. The ordinary paired-RIFE consumption
window matrix still passes. These are fixture results, not a GTA FPS guarantee.

Owner test: fresh GTA process, fixed resolution, AER RIFE High->Medium->High,
allow enough time to cross the old delayed onset, F9 enabled. Compare steady FPS
and physical delivery, with the menu closed; confirm that the old5756 learned
phase no longer activates for AER. Non-AER RIFE may still use that phase normally.
