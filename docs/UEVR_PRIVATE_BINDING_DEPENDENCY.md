# Private synthesis / unrelated binding queue regression

A fault-injection fixture now stalls only the session runtime binding queue on
an actual D3D12 fence. Previously accepted private outputs/readers are drained;
the app queue, current source history and private destinations remain usable.
The production prepare_projection_frame path submits a prime or pair and the
test checks its exact worker fence while the runtime fault is still held.

R6 code fails all four variants (FidelityFX/new NVIDIA x prime/pair), and each
failure repeats independently. The app queue progresses, synthesis does not.
Once the injected runtime fence is released, synthesis completes and GPU
readback returns expected pixel 80. Thus this is a reproduced unnecessary
queue dependency, NOT a reproduced permanent stall after the fault is removed.

Candidate excludes ordinary private D12, non-Luke, non-interop output targets
from the inherited whole binding-queue producer join. The accepted private
image acquire/reader protection and source-history producer fences remain.
All four variants then complete with runtime queue still blocked, preserve
pixel identity, and pass an additional negative control: an actually blocked
source-history fence still prevents synthesis completion, then resumes only
when its own producer is released. No completion fence is forged or signaled.

4/4 new tests and 22/22 selected previous generation, 3X/4X, panel, Framework,
invalid-input and depth-history checks pass. Existing native/interop policy is
excluded from the exception. Full suite and reporter acceptance not performed.
No product package/build, install, arming, commit or push was done in this round;
only the test target was rebuilt. This candidate is uncommitted over R6.

Evidence: repo/svperstar-binding-probe-final-tests.log,
repo/svperstar-binding-probe-regression.log, the four
repo/svperstar-binding-probe-*-old.log and *-old-repeat.log files, and the
candidate-only patch repo/svperstar-binding-probe-candidate.patch.
