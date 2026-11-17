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
