# RDR2 motion and source timing trial, V927 r1

Base: local V927 a95638d plus the owner-confirmed Cyberpunk original-motion
trial. The Cyberpunk fix is retained. The owner requested retaining and
committing this change after testing on 2026-11-09; the test version stays V927.
No installation, registry/profile change or push.

## RDR2 history

The V926 safeguard still rejects the prematurely composed field. Instead,
retain the cropped original field and its own GPU N1/N2 classification with
the exact colour/depth/camera publication. Only classification N2 permits
later motion advancement; N1 and ambiguity retain the camera-only fallback.
The stored N2 field uses its own previous same-eye camera to separate scene
motion from camera motion. Current source depth rejects an advanced foreground
surface that the current eye sees through. Existing reset, exact identity,
scene-cut and history selection guards remain.

There are two private one-texel interval textures, and the already allocated
history motion textures now receive the original cropped motion for this path.
Classification remains on the GPU. No production readback, extra queue wait,
swapchain or pacing change is introduced.

## Source timing

QueryPerformanceCounter is observed immediately after the existing RDR2
native camera callback. Its owner/render identity follows the matching
GPU-selected camera into the immutable guide. A matching main-camera sample
is preferred over a later equivalent gun-camera callback.

This is a camera-observation clock, **not a verified game simulation clock**.
Its effectiveness is the hypothesis being tested. Current N2 correspondence
uses (current - peer) / (current - previous same eye); stored N2 advancement
uses (current - peer) / (peer - peer's previous same eye). Require matching
owner/frequency, exact render steps, monotonic nonzero stamps, 1-50 ms per
render and bounded ratios. Otherwise keep the existing half-step. No smoothing
or extrapolation of missing timestamps is performed. Other games keep their
existing temporal behavior.

## Diagnostic decoding

Flight `luke_afw_stage` records use `a` as the stage:

- 1380: result=render identity, b=publication, c=camera QPC.
- 1381: result=QPC frequency, b=publication, c=source owner.
- 1382: result bit0=valid midpoint, bit1=valid history advance,
  bit2=CPU classified-history eligibility; b=current publication;
  c=midpoint float bits in high32, advance float bits in low32.
- 1321 retains the N1 bits and adds bit2=stored classified raw field,
  bit3=CPU classified-history eligibility. The GPU still requires N2.

F8 `xr-timeline.tsv` has `osr_source_qpc` records for current/peer/same
(code 0/1/2, serial=publication, display_time=QPC, detail=frequency), and
`osr_source_fraction` with accepted timing bits and both fraction float bits.
`osr_scene_state` is now eight uint32 values: the first six are unchanged;
index6 is the older history field's stored classification (0 unavailable or
ambiguous, 1 N1, 2 N2); index7 is the current midpoint's float bit pattern.
This extends the existing tiny capture, without consuming more image slots.

## Validation and headset check

Both x64 and x86 Release payloads build. All 31 focused CTests pass, including
Cyberpunk OSR/bypass/legacy controls, RDR2 camera/HDR, Vulkan transport, OSR
current/history/screen motion, both qualities and snapshot lifetime. The full
project suite was not run. Three initially missing test executables were built
and passed individual reruns. Two new GPU fixture expectations were corrected
after isolated reproduction: existing colour arbitration blends a deliberately
colour-shifted source, and depth refusal needs distinct colours to be observable.
No production relaxation was made to pass those fixtures.
The tests cover asymmetric timing, negative clock/identity cases, original
history surviving overwritten producer inputs and a changed live classifier,
N1/ambiguous refusal, both eyes/qualities, depth rejection, and quarter/half/
three-quarter sampling with a non-RDR2 control.

## Owner run and retention, 2026-11-09

PID48968 used the sealed r1 layer (SHA256 c880025b7a016f26d98fe5966b8a8eecab5948ae3b7e16c91c370099e116e757).
The owner reports reduced silhouette ghosting and somewhat better orbiting,
with residual artifacts, and requests retaining both changes. All six F8
evaluations have valid timing and GPU N2 classification for current and stored
history. Across 4536 sampled noncapture gameplay evaluations midpoint timing
was valid throughout, with one history-advance fallback. In the orbit capture,
timing fractions 65.4%/47.8% agree with camera-rotation fractions 65.0%/47.1%.
This supports the camera observation proxy without proving simulation time.

Residual broken back/bedroll outlines include history-filled edge colours
carrying background depth. Original camera observations also remain uneven;
correcting correspondence does not retime original game frames. The next trial
addresses those separately. Full evidence is archived in the workspace under
logs/rdr2-motion-v927-r1-review. F8 itself adds stalls; exclude capture windows
from pacing comparisons. Camera rotation compensation remains off.
