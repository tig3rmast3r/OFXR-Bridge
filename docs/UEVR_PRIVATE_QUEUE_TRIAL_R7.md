# UEVR private queue trial — V984 round 7

## Change

Ordinary private D3D12 output acquisition already protects the exact image from
endpoint readers. Source history supplies its own producer fence. Synthesis
nevertheless imported all prior native binding-queue work before every prime
or pair. Exempt these private targets from that broad queue join. Retain native,
Luke and interop behavior, history producer fences, private image reader leases,
GPU completion, native copy synchronization, FIFO order and presentation policy.
Real-only recovery benefits because its prime traverses the same preparation.

This is independent of Adaptive pacing. This test build contains R6 and all
its previous fixes, plus this queue exception; no adaptive worktree is merged.

## Reproduction

Real D3D12 fence stalls the binding queue only. The application queue and
source/private targets are available. Production prepare_projection_frame
submits prime/pair work. R6 fails all four cases (FidelityFX/new NVIDIA x
prime/pair), repeated independently. Application progress succeeds while its
synthesis fence remains pending. Releasing the injected binding fault permits
completion and returns the expected GPU pixel 80.

The candidate completes all four while the unrelated binding fault remains
held. An independent negative control then blocks the actual source-history
producer: synthesis correctly remains pending until that source is released.
No producer completion is forged. Output pixels remain correct.

This proves unnecessary queue coupling. It does not reproduce the reporter's
sustained bog-down AFTER the injected fault is removed; in the baseline test
completion resumes when that fault clears. Reporter acceptance remains needed.
Previous test round: 4/4 new probes and 22/22 focused regressions passed.
The complete selected package validation is recorded with the R7 build.

Keep this logical fix separate from prior fixes if later committing. No commit,
push, installation, arming, live game/profile changes or adaptive edits.

Final production build succeeded. All 47 selected tests passed in one grouped
run, including queue-fault/source-protection controls and previous regressions.
Full suite and reporter hardware acceptance remain pending.
