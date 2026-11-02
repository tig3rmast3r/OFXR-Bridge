# V689 Luke original-frame identity during AFW recovery

Local candidate, 2026-10-26. Builds on the GTA V688 reset-origin fix. This
changes measurement of accepted content, not AFW admission or image generation.

## Problem and observed limits

The owner's V688 GTA run no longer stutters at scene transitions but still
shows intermittent total-FPS N/A. Among 26 gameplay N/A intervals, eight
overlap missing-CRC increments and 18 do not in the available compact counters.
The full flight ring only retains the final 12 seconds, so those 18 intervals
are not all individually proven to have the same downstream refusal.

AFW may need to prime a new camera history while both original color CRCs are
still readable. The application pair passes through unchanged, but GTA lacks
the separately pinned CPU render-identity probe. Previously this real frame had
no scene identity for the meter. A single such submission invalidated the entire
one-second measurement window, even while AFW resumed and its use counter grew.

The automatic V688 explosion snapshot independently shows a different case:
native completed DLSS colors carry valid tokens, while post-SR blur destroys
the barcode in the original XR colors. This candidate cannot invent the lost
origin and does not claim that case, the reported grey effect or aiming is fixed.

## Changes

After the existing paired readback, retain the exact left/right CRC pair solely
as an identity for the unmodified application scene. Both reads must succeed,
both tokens must be nonzero, and the logical-eye mapping must be valid. Mapping
enumeration order cannot change that identity. A different accepted pair counts
as new original content; held submissions of the same pair remain duplicates.

When AFW cannot prepare output, preserve an existing independently verified CPU
identity. If absent, pass the CRC-pair identity to the existing real-frame meter
and any existing raw optical handoff. It never authorizes a guide, labels raw
content as AFW-generated, or replaces the identity of an actual AFW result.
The originals are not edited by the scratch preparation that obtained the CRCs.

After a truly unknown frame, restart measurement on the fully known suffix of
the accepted timeline, with the same 100 ms minimum used at startup. Exclude
every pre-gap sample. Require a known receipt after the unknown one; a delayed
older confirmation cannot recover the counter. Ongoing unknown submissions
remain N/A. No previous FPS, headset refresh or generated/real sum fills holes.

This applies to Luke measurement paths using this shared paired preparation and
accepted-scene meter, including SteamVR and VDXR. Non-Luke measurement is
unchanged. No GPU copy, wait, snapshot slot, swapchain or polling work is added.
Generation/pacing, AFW native depth-aware subtraction, shaders, provider and
camera calibration are unchanged. The benchmark layout and presets are unchanged.

## Validation and next owner step

Focused checks cover 50 unique accepted images in 90 held XR calls, mixed AFW
and known-original recovery, swapped mapping order, missing/erased markers,
invalid eye mapping, exclusion of pre-gap history and late confirmations.
The actual GPU marker fixture proves that an intact CRC pair identifies real
content while erased markers do not. D11/D12 overlay integration checks that
an unknown-to-known original handoff recovers within a second and reports zero
generated FPS, without substituting refresh-rate FPS.

Seven preceding core/marker/capture/protected-guide/GTA-reset checks and three
meter/overlay checks pass in 15.76 seconds. Runtime acceptance remains
pending the owner's game run. Genuine unreadable-origin N/A may remain.

Use matching V689 tray/game, AFW 2x2, F9 on, AFW debug off. For the distinct
aiming defect, press manual F8 while head-aiming and rotating the headset with
the wrong image visible; switch F9 off about five seconds later to retain that
interval in the bounded flight log. No full benchmark is required.

Implementation 689 / IPC 68901 / AFW beta6 ABI 45601. Local source branch in the
public OFXR-Bridge repository, outputs in retained ofxr-development. No public
push, retired-private-Git access, live installation or profile changes.
