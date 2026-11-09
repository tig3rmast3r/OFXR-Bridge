# V928: Cyberpunk OSR original motion

Base: local V927 `a95638d`, including V926 RDR2 and V927 GTA runtime discovery.
The owner accepted the V927 r1 test on 2026-11-09. This is the same
implementation, prepared as the next nightly change after V927.

Cyberpunk with REAL VR FixDLSS normally supplies OFXR with rewritten same-eye
N2 motion. OSR estimates intermediate screen motion from that interval; a cabin
or near hand can retain an older screen position between its real eye renders.

For Cyberpunk's full OSR mode only, observe the known public NGX SR invocation
before it forwards into REAL VR. Snapshot its original N1 motion/depth and
matching camera using the existing protected GPU snapshot mechanism. Mark its
retained original output after the wrapper completes, including when the typed
SR hook is bypassed. REAL VR still runs its DLSS fix unchanged. This replaces
the OFXR guide capture boundary, rather than adding a second N2 snapshot.

The exact N1 receipt enables existing native object-interval composition and
depth-validated cabin history. Retain original N1 history/stereo/jitter for that
path. Current N1 correspondence can select the previous own-eye color directly;
native history advances measured screen motion, including near surfaces when
validated, instead of interpreting a composed N2 residue as cabin movement.
Missing guides, depth disagreement, reset or broken publication identity keep
the existing guarded fallback. No new shader, readback or XR pacing is added.

Legacy AFW retains the typed rewritten N2 capture. Other games retain their
existing selectors. There is no change to LR camera rotation compensation or
to OSR camera orientation, and no game/profile/registry setting is written.

Validation: 25 distinct focused CTests pass. New actual NGX/GPU fixtures cover original vectors surviving both
in-place overwrite and private pointer replacement, final caller output after
wrapper composition, typed-hook bypass, immutable previous snapshots and the
legacy AFW N2 control. Existing OSR native-motion GPU checks cover both eyes,
both qualities, animated cabin correspondence, near rigs, head rotation,
depth/invalid-flow refusals and missing-original fallback. Build/test results
are recorded in `repo/cyberpunk-a95638d-v927-r1-*.log` at the workspace root.
The first Cyberpunk fixture reused wrapper-mutated inputs across caller frames;
resetting each caller's original bindings fixed that fixture, and all three
Cyberpunk capture checks pass. No production change was needed for that failure.

x64 and x86 Release payloads build. The full default build encounters the
unchanged V927 `RealVrRuntimeImage.h:73` Windows `min` macro conflict in the
ignored `xrfg_luke_depth_gpu_tests` target. Release and focused targets were
rebuilt explicitly; that unrelated test/header was not changed. The full test
suite was not run, and no claim is made about its historical failure families.

The owner confirmed the vehicle-turning fix on 2026-11-09. For regression checks, compare the same vehicle route/turns against
the V927 base with Camera rotation compensation still off and FixDLSS still
on. Check road/background smoothness separately from dashboard, pillars,
mirrors, hands and steering wheel; also check walking/weapon motion. F9 plus
F8 on any remaining breakup can verify the new `cyberpunk_original_n1` receipt
and exact current/peer guides. No local layer is installed or armed by this task.
