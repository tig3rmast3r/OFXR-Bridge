# V758 Cheeky Foveated DLSS: side-by-side guides and deferred NGX evaluations

Cheeky Foveated DLSS (ClarkCheekyKent) splits a game's DLSS call into a
periphery pass at a fraction of the eye's pixels and a sharp inset, each a
private DLSS feature, and composites them itself. Two consequences for OFXR,
measured on 2026-10-30 (Pimax, Cheeky 0.6.2):

1. **UEVR Native Stereo (Hogwarts Legacy).** One 8394x3287 side-by-side
   swapchain, eyes 4197x3287. The release resolved its guides by size, which
   needs two DLSS outputs each exactly half the image. With foveation on, each
   eye publishes a 1826x1430 DLAA periphery (render = output, 2.3x smaller than
   the eye, its shape) and an 840x657 inset; the 2435x1907 -> 4197x3287 feature
   is evaluated only with foveation off. No stream carries a Streamline camera
   (Cheeky calls NGX directly). `generation_prepare` 27, menu "Missing game
   vectors". Fixed here; confirmed in the headset.
2. **Luke Ross R.E.A.L. VR (Cyberpunk 2077, legacy AER).** The layer's typed
   `nvngx_dlss.dll` hook was reached on every call (the deferred census below
   proved it), so the NGX side was never the blocker. The Luke route refused to
   capture (reason 12) because no exact native camera arrived: Cheeky patches
   `Cyberpunk2077.exe`'s import slots for `slSetConstants` and the other
   Streamline exports to point at its own detours, which forward through
   MinHook trampolines past the export OFXR hooks. That is Cheeky's to fix:
   branch `streamline-import-chaining` on djules75/CheekyFoveatedDLSS leaves
   those slots to the inline hooks, and with it AFW engages. OFXR keeps a
   defensive fallback for the case the census was built to detect.

## Changes

- `refresh_release_bound_projection_guides`: a projection with both eyes in one
  single-layer image takes `refresh_side_by_side_projection_guides`. When the
  release found nothing by size, each eye takes the stream whose camera covers
  its view, placed into its half (0.2's V515, as for MSFS 2024's per-eye
  images); without cameras, `resolve_enlarged_dlss_guides` (0.2's V537): two
  recent streams whose outputs are each eye's view enlarged, smaller than the
  view, of its shape to within 1%, at most three times smaller. Both eyes or
  neither; the first evaluated is the left unless the motion inputs sit apart.
  Not on the FSR route, which reads whole texels.
- Packed-stereo size match (`resolve_dlss_motion_vectors`, one-layer image,
  two half-width outputs): only streams published within the last eight
  publications. Cheeky toggled foveation off and on: the eye-sized pair kept
  its last frame from before the switch and was taken every frame, and DLSS-G
  refused it as discontinuous (`native_dlssg_record` skip 1, detail 5) for good.
- `NgxEvaluationBoundary`: a generic dispatch that deferred to the native body
  learns, once it returns, whether any hook inside it ran (`inner_entered`) and
  whether anyone observed the call (`unobserved`). `evaluate` observes a
  deferred SR evaluation itself when the call succeeded and no hook observed
  it, except for the titles whose generic and native fields differ (KCD,
  Outlaws, Crimson Desert, GTA), which keep their exact boundaries.
- Logging. Flight `presenter_transition` 753 for each placed eye (stream, placed
  motion rectangle in eighths, publication); 756 (live streams << 32 | resolver
  status, image size, outcome | release << 8) and 757 per stream (stream, motion
  x, flags 1 queue 2 camera 4 depth 8 reset; output and motion sizes; distance
  behind the newest, epoch) on a change of outcome, the first eight frames that
  find nothing and every 512th after. NGX capture `ngx_deferred_census` (slot;
  deferred calls << 32 | those no inner hook entered; observed here << 32 |
  published; handle), the first 16 and every 1000th.

## Verified

- Hogwarts Legacy, UEVR, Cheeky foveation on: DLSS-G attaches game guides
  (test rounds r2 and r3 of this change, user-confirmed in the headset); the
  r3 recency gate keeps them after toggling foveation off and on.
- Cyberpunk 2077 R.E.A.L. VR: the deferred census showed the inner hook entered
  on every gameplay call, so the fallback never fired there; AFW engages with
  the Cheeky branch above.

## Tests

`xrfg_d3d12_history_tests release-bound-guides` adds `test_enlarged_dlss_guides`:
two 100x110 peripheries over 200x220 eyes side by side are placed over their
halves with all of their vectors, in evaluation order or by their motion inputs'
position; 64x64 insets, a third periphery, a single one, a release before the
evaluation, and a pair not published within eight publications are refused.
