# V773: no panel image generation in Fast mode

This nightly update changes only which OpenXR panels get generated images.
Frame generation, Luke Ross reconstruction, NR, delivery/pacing and the UI are
unchanged.

## Symptom

With **Fast mode** on, a game that submits an OpenXR quad or cylinder panel
froze on its first frame: Dead Island 2 under UEVR (HUD and menu panels),
and in principle OpenKneeboard or OpenComposite overlays. The flight log
stops inside the first `xrEndFrame`. Without Fast mode, panels work.

## Cause

Since 569de98 (independent panel images, V763), Fast mode hands the runtime a
generated panel image whose fence is never signalled, so the runtime's queue
deadlocks. `xrfg_three_slot_d3d12_tests lower-vram` reproduces it: before
569de98 it passes in under a second, from 569de98 on the panels show the
fixture's unwritten-image pattern on the first frame and the test hangs.

## Change

In Fast mode, `panel_sources()` selects no panels, so no panel takes the
image generation path; panels keep their real images on generated frames, as
before V763. Their pose interpolation (013bc32) still applies. Without Fast
mode nothing changes. REAL VR panels were never touched.

This is a stopgap: the Fast-mode output path for generated panel images
still needs a real fix, after which this guard can go.

## Test

Dead Island 2 under UEVR with Fast mode on: no freeze, HUD and UEVR menu
visible and steady. `xrfg_three_slot_d3d12_tests lower-vram` no longer hangs.
Its HUD position check and the first-frame output count in the other
three-slot modes still fail since 013bc32/569de98: those expectations predate
panel pose interpolation and the first-pair panel priming, and need updating
separately.
