# Native runtime multiframe + status.json (V985, round 5 accepted)

This is the historical local-test record. The accepted local V984/V985 work
is consolidated for publication as [nightly V984](V984_NATIVE_MULTIFRAME.md),
the next version after origin/nightly V983. The test package names, hashes
and log paths below remain unchanged; they are not public version assignments.

Base: local 6794aa5, with status reporting folded into V984 (7fa3652) and the
accepted OSR extension in the following V985 commit. These native-runtime
corrections were built as uncommitted test rounds on work/vdxr-osr-multiframe.

## Acceptance, 2026-11-12

After testing r5, the user reports "Working great now" and authorizes a local
commit. This confirms the remaining DI2/Pimax headset-delivery problem is
resolved in the tested build. MSFS 2024 and DI2 on VDXR were already confirmed
working with r4. It does not establish that every runtime/mode combination or
the changed-resolution guide case was retested on the headset.

All 11 source/document hashes matched the sealed r5 provenance before adding
this acceptance note; the executable code is unchanged from that tested build.
ZIP SHA256: 3bda169e2d0d3961965c208f2ebf4a723a66b5acd0770c2f619fbe14274cd0ae.
Layer SHA256: 5309393488b6043a05358b1bc13fc97c55c876d5374d915dd09bb285654e0e0f.
The 100 focused checks passed as detailed below. No push or installation.

## Both fixes in this package

The old native multiframe predicate explicitly required VirtualDesktopXR and
its legacy transport flag. Pimax OpenXR in Auto therefore fell back to 2X.
Native multiframe eligibility now depends on the graphics/worker contract,
not the runtime name or that preference flag. Pimax and other non-SteamVR
native runtimes can use the same ordered 3X/4X path. Native SteamVR still uses
its existing private endpoint path.

Supported native configurations are ordinary D3D12, or compatible Luke Ross
OSR/AFW D3D11/D3D12 sessions with an isolated queue and unsplit frame loop.
Single-threaded D3D11 devices, ordinary D3D11/Vulkan bridge configurations,
AFW-only and other unsupported bindings retain their restrictions. RIFE 2X
retains legacy overlap admission. FSR/0.2.13 multiplier choices and DLSS-G
hardware/guide requirements remain unchanged.

status.json uses the effective multiplier reported by the layer, including
fallback and OSR/AFW, rather than the old triple flag or a fixed Luke 2X value.
Without a session it uses the configured per-route multiplier. JSON schema
remains 1; the shared status ABI is now 98401, so use the matching tray and
layer and restart the game when replacing them. The accepted OSR correction
that publishes each generated D3D11 image is included.

## Round 5: Pimax queue isolation and packed guide alignment

The user confirms R4 works in MSFS 2024 and DI2 with VDXR, but DI2 with
Pimax OpenXR still has no visible intermediate motion. At restored resolution,
PID 31860 has matching 4340x4284 guides/eyes, roughly 90 successful Ends/s,
successive predictions about 11.111 ms apart, and generally 28-30 ms positive
display-time lead. The mismatch and the delivery failure are separate.

Pimax's ordinary native D12 session was bound to the game queue. Completed
generated outputs could therefore be handed off behind the next application's
GPU work. SteamVR's private delivery uses a separate queue. The old native GPU
fixture accidentally gave Pimax that independent queue as well, hiding this
dependency. R5 binds ordinary native Pimax D12 to its own queue and uses the
existing per-source release and reacquire fences to retain resource ordering.
At application image release, new ordered 3X/4X retires that source's exact
GPU receipt on the producer thread, outside the GPU lock, before returning
the image to XR. A pending source must not put a GPU wait on the binding
queue ahead of the previous batch. The wait is bounded by the existing
one-second publication timeout and logged as publication detail 5531. 2X
keeps asynchronous queue ordering at release. This preserves the D3D12
image-availability contract at Release, not merely at EndFrame:
https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html
It keeps native frame-loop selection and the R4 pacing policy. VDXR, SteamVR,
other runtimes and the Luke/D11/Vulkan/AFW-only transports are excluded.
The binding is immutable at session creation: Pimax 2X uses it too, so live
2X/3X/4X switches do not depend on the multiplier selected at startup. Headset
delivery was unconfirmed at packaging; the user subsequently accepted r5 above.

The native tests now make the mock compositor use the actual selected binding
queue. A GPU dependency test holds future game work after the current source
is complete: the shared-queue control blocks; the Pimax binding can finish its
handoff independently at 2X, 3X and 4X. It does not model physical scanout.
It also holds the next source before its release and checks that 3X/4X
defers that release without blocking the previous batch, while 2X retains
its asynchronous release dependency.

At the changed resolution (PID 30972), XR requested 3798x3748 per eye, while
DLSS published 3800x3748. Packed matching previously demanded exact halves.
R5 accepts two unambiguous recent eye streams rounded up to four pixels and
crops their motion/depth rectangles to the unpadded eye extent. It preserves
the original source publications and eye order, and places the right eye at
3798, not 3800. Exact matches retain their existing path; larger mismatches,
mono/UI images, stale companions and ambiguous third streams are refused.
This deliberately addresses small right/bottom padding, not arbitrary resizing.

R5's 100 focused checks pass after correcting the SteamVR queue-policy
fixture's Legacy default to its actual session routing. Initially 99/100
passed; that single fixture failure reproduced alone before the correction.
All native/SteamVR steady delivery, live switching, batch/publication, OSR,
status, guide alignment/vector GPU and pacing cases passed. The seven R4
switch failures did not recur with the corrected native queue model; this is
not an identical-fixture comparison or a full-suite/hardware claim.
Detailed results are recorded in BUILD_PROVENANCE.json. The queue change
requires restarting the game. No live settings, installation or arming were
performed. The local commit follows the user's acceptance; nothing is pushed.

## Round 4: restore runtime pacing for native 3X/4X outside VDXR

The user reports round 3 still has no visible intermediate motion. Its Pimax
DI2 log (20261112-131901, PID 61264) contains the new per-output releases and
successful EndFrame calls. Per-output publication therefore did not resolve
the reported issue. It remains included, with no claim that it fixed Pimax.

The working 0.2 source at OFXR-Bridge-4x/cf95f29 has a distinct native policy
in pace_presenter_submission: Pimax and other non-SteamVR/non-VD runtimes use
their own WaitFrame as the clock, with only a half-period minimum measured
from the previous EndFrame return. VDXR deliberately keeps a steady grid.
This policy dates to a6de876/404e566; those commits document the earlier
Pimax wait-plus-timer interference. It applies to 0.2's 3X/4X presenter too.

Our new ordered native 3X/4X OFXR/Hybrid path instead selected MidpointSchedule,
adding a second clock after the runtime wait. In the latest Pimax run's
18-25 second window, median Wait was 0.170 ms and subsequent OFXR pacing was
10.399 ms of an 11.111 ms refresh. There were 627 content Ends, with three
near-repeated predictions and three two-refresh advances. Those occasional
slot collisions alone do not explain complete loss of intermediate motion.
Converted display-time lead was normally positive (median 26.264 ms at End),
which rules out steady timestamp expiry, but does not prove compositor pickup.
Working VDXR DI2 (V983/PID 39320) also used a grid, intentionally; its success
is not evidence that the same policy suits Pimax.

Round 4 restores the 0.2 native clock policy only for newly supported ordered
native 3X/4X outside VDXR: use WaitFrame and a half-refresh handover floor,
measured from the actual End return, including when End blocks. The existing
helper already serves the legacy Djules native path. VDXR, private SteamVR,
2X and the legacy 0.2.13 path retain their previous scheduler selection.
The inherited pipelined 2X owner is also excluded when 3X/4X is only requested.
Pacing records use result 5 for this runtime-paced path. The ordered native
consumer also records the existing XR-to-QPC clock anchor every 64 outputs,
after submission/admission bookkeeping, so a pure 3X/4X run can be timed
without needing a 2X segment to establish the clock conversion.
No image capture or readback is added. This is a demonstrated scheduling
policy mismatch and a test candidate, not proof of the Pimax internal cause.

## Round 4 validation

The new integration test runs the actual consumer against early and blocking
runtime waits, including a slow End, at 2X/3X/4X with native and VDXR controls
(12 combinations). Before the change it fails on the independent native
timer; afterward it passes. The inherited 2X-owner exclusion also passes.

The final targeted matrix passed 87 of 94 cases. All steady native and SteamVR
GPU cases, 24 batch-overlap cases, 18 publication cases, OSR, status JSON,
multiplier policy, midpoint and the new timing test passed. Seven switching
stress cases failed and reproduced individually: VDXR switch-fast and
switch-unpaced; Pimax switch-fast and switch-overlap; generic native switch,
switch-fast and switch-overlap. They report the previously observed missing/
black real-output signature. A preliminary run additionally failed VDXR
switch-overlap, which also reproduced alone; it passed the final matrix.

Accepted V984 control binaries reproduce switch-fast and switch-unpaced with
the same signature in this round; switch-overlap passed that control run
(it reproduced in the round 3 accepted-build control). Pimax/generic cases
also failed in earlier rounds. This records existing switching failures,
not a clean switching suite or an exhaustive regression clearance.
Exact logs, reruns, source hashes and the full patch are in BUILD_PROVENANCE.
Physical Pimax intermediate motion remains for the user to validate.

## Round 3: native output publication candidate (unsuccessful headset test)

Comparison references: user-accepted native VDXR MSFS run V983/PID 49504,
native VDXR DI2 run V983/PID 39320, SteamVR's private delivery implementation,
and the failing native Pimax DI2 V985/PID 54308. Pimax accepted approximately
90 content submissions/s at 3X and 4X. Steady timestamps were generally in the
future; the observed submission cadence alone does not explain why the user
sees no intermediate motion. The existing unclassified-swapchain message is
an inventory warning and does not gate frame generation.

SteamVR publishes each final runtime image at its delivery boundary. Native
3X/4X previously released complete batches in the worker, ahead of their own
Wait/Begin/End calls. Native VDXR also used that early-release path, so this
comparison does not prove early release is the cause of Pimax's visible issue.
It identifies a concrete publication-order difference to test before requiring
an F8 diagnostic build. Installed Pimax runtime is PiOpenXR_64.dll under Pimax
Runtime, not the abandoned mbucchia runtime; no assumptions about its internal
compositor implementation are made.

Native D3D12 OFXR/Hybrid 3X/4X now keeps each acquired output until the presenter
publishes that exact image just before its own EndFrame. Its immutable GPU
completion ticket is joined to the binding queue before release. Real, S1,
S2 and S3 each carry their own publication lease through the ordered output
buffer. The worker still finishes GPU work before admission, and r2's separate
banks/compute overlap remain. This adds no image copy or new texture bank.
2X, private SteamVR, RIFE, DLSS-G, 0.2.13 and Luke/OSR keep their previous paths.

A stricter downstream fixture checks that each generated/current target is
the output published immediately before that target's native EndFrame. R2
fails this check; r3 passes the focused checks. This is a compatibility
contract test, not evidence of actual Pimax compositor requirements or proof
that the user's visible issue is fixed. Physical Pimax and VDXR validation
remain pending. No F8 is requested for this candidate.

### Round 3 validation

93 targeted cases ran: 89 passed initially. All 18 publication-order tests,
all 24 prior overlap tests, all steady native/SteamVR multiplier checks,
OSR GPU cases, status JSON, policy and midpoint checks passed. Four switching
stress cases failed initially. Individual reruns still fail VDXR
switch-overlap and generic-native switch; generic-native switch-fast and
switch-overlap pass individually. The matching VDXR switch-overlap test also
fails on the accepted V984 build with the same black-real-packet signature.
Generic switching failures were observed before r3 as well, but no clean
switching/full-suite claim is made. Full logs/results are in provenance.

The r2 negative control fails the new Pimax 3X publication-order test; r3
passes it. This verifies the changed publication contract in the fixture.
It does not establish the cause of the physical Pimax failure. Test this
candidate with matching tray/DLLs and a game restart, in the same DI2 scene
at 2X/3X/4X, observing actual motion. Recheck the accepted VDXR path too.
No F8 is required for this comparison. No commit, push or installation.

## Round 2: native optical batch overlap

The DI2 Pimax run at 12:20 (PID 20104) activated 3X/4X but delivered only about
68/69 accepted content submissions per second. 2X delivered about 86. Its
OFXR/Hybrid worker prepared each next batch only after the previous batch
finished delivery; preparation took about 13 ms at 3X and 18 ms at 4X.

Native D3D12 OFXR/Hybrid 3X/4X now has two independent synthetic output banks,
including Fast mode. One future source may be captured and computed while the
previous batch is delivered. Only GPU-ready descriptors enter the ordered
output buffer, after its previous synthetic slots retire. Output leases,
current-image ownership, GPU completion checks and native Wait/Begin/End order
are retained. There is one worker job and at most one future prepared batch;
no output is repeated or counted merely to raise the rate.

The scheduling change excludes 2X, private SteamVR, RIFE, DLSS-G, 0.2.13 and
Luke/OSR/interop. The earlier accepted OSR fix and effective status multiplier
remain included. Native optical multiframe uses additional output textures
for the second bank; this is required in Fast as well to avoid overwriting
images retained by the runtime. Physical DI2/Pimax throughput remains for user
testing; this build does not claim verified headset scanout or guarantee 90 FPS.

The new regression test holds an actual native synthetic EndFrame while the
next source is captured, submitted, and becomes GPU-ready in the other bank.
It then releases delivery and validates all real pixels, stereo images,
metadata ordering, exact steady output counts and balanced native frame calls.
With the overlap policy disabled, the Pimax 3X test reproduces the previous
capture wait. With the fix enabled, it passes.

## Round 2 validation

The targeted matrix ran 75 cases: 70 passed initially. All 24 new native
batch-overlap cases passed (VDXR/Pimax/generic native, 3X/4X, Normal/Fast,
NVIDIA OFXR, Hybrid and FidelityFX OFXR). All steady native and SteamVR
multiplier cases, OSR GPU cases, status JSON, overlap/ring policy and midpoint
schedule checks passed.

Five switching cases initially failed with the existing missing/black-real
signature: VDXR switch-fast, switch-overlap-fast and switch-unpaced, Pimax
switch, and generic native switch. All five passed when rerun individually.
The three matching VDXR cases also passed individually in the accepted V984
build. Earlier accepted-build checks reproduced other switching failures
with this signature; this does not establish that the current stress suite
is clean. Exact logs/results are listed in BUILD_PROVENANCE.json.

The negative control (native overlap disabled) failed the new Pimax 3X batch
test with "native optical capture waited for entire downstream batch". The
fixed build passes that same test. Neither this fixture nor submitted FPS
independently proves physical headset delivery; DI2/Pimax confirmation is
still required. No full-suite claim, commit, push, installation or arming.

## Round 1 checks and limits (historical)

The 33-case candidate run passed 28 cases. All steady 2X/3X/4X and Fast 3X/4X
cases for Pimax, generic native, VDXR and SteamVR passed, as did status JSON,
live multiplier telemetry, capability/overlap policy and post-OSR GPU checks.
These use actual GPU generation/publication with mocked downstream runtimes;
Pimax hardware and physical headset delivery remain for user validation.

Five switching stress cases reported missing/black real packets. Isolated
reruns passed Pimax switch-fast, generic switch and generic switch-overlap;
Pimax switch-overlap and generic switch-fast reproduced. The same failure
signature also reproduced in the accepted VDXR switch and switch-overlap
baseline checks. The broad pre-candidate run additionally reproduced the
unpaced baseline failure. These results are recorded in BUILD_PROVENANCE.json;
no clean switching/full-suite claim is made.

The legacy standalone launcher test fails in unrelated pre-JSON runtime-INI
expectations after settings sanitization. The isolated status.json test
passes configured/effective 2X/3X/4X and active fallback overriding configured
4X. The native/SteamVR fixtures assert the layer telemetry multiplier.

## User test

Use this package's matching tray and all runtime DLLs. With Pimax OpenXR and
Auto, test available 2X/3X/4X modes and return to 2X. Also check OSR + framegen
with AER off where applicable. With status.json export enabled, compare its
multiplier to the effective game mode after applying each setting. Keep the
scene and refresh fixed and retain F9/flight logs for any failed transition.
V985 runs log under RuntimeLayer/v985, separated by PID and timestamp.
Native output receipts are not independent proof of physical scanout.
