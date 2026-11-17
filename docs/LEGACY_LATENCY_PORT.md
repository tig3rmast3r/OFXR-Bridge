# Framework / Director latency port

Local integration on work/svperstar99-nightly (base 31998cc), without a version bump or push.

## Source admission

Ordinary D3D12 optical 3X/4X Framework and Director rendering targets the first
output slot. The learned budget measures rendering, generation and actual native
preparation, rather than treating deliberate output spacing as preparation work.
Output credits retire after native EndFrame returns. In-flight calls temporarily
freeze the slot estimate; missed slots and generation jitter have bounded margins.
A separate precise timer wakes the source producer without changing output pacing.

Independent is excluded at eligibility, accounting and observer registration.
Its scheduling routines and header are byte-for-byte unchanged against 31998cc.
2X, RIFE, AFW and interop retain their existing source admission.
Private late-GPU recovery remains enabled and retains its original completion path.

Validation: eight focused checks pass, including native VDXR and SteamVR 3X/4X,
Independent paced delivery, admission math and a runtime exclusion test.
Headset smoothness and physical latency remain hardware observations.

## Fenced batch publication and private preparation

Eligible native optical 3X/4X batches publish immutable output descriptors before
GPU completion. The worker retains the batch until its completion fence signals;
each output still joins its own GPU readiness before native use. This path is
explicitly unavailable to Independent and private completion-recovery sessions.

Legacy private transport tags accepted optical packets by immutable ordinal so
its native Wait can overlap that packet's copy preparation. Untracked RIFE and
Independent retain the prepared-output wait condition. This does not bypass the
existing completion recovery or runtime prediction renewal checks.

Validation: held-generation 3X/4X tests pass (descriptors visible, resources held,
no incomplete native delivery). Tagged private clock checks pass in Framework
and Director with exact output pixels and native Wait overlapping copy work.

## Native GPU overlap budget

Eligible fenced native batches may overlap at most min(one third of a display
period, three quarters of measured generation time) with CPU submission. This
requires a successful XR-time-to-QPC conversion and at least two periods of
remaining native prediction headroom. Unknown/short windows, private transport,
non-fenced work, refresh changes and reset retain or restore the full budget.
Independent performs no new prediction conversion or budget accounting.
Controller tests cover unknown/short/ample predictions, bounded allowance,
private/unfenced fallback and reset.
