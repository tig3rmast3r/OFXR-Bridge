# The AFW token readback blocks the application's xrEndFrame

Base: nightly d0e3ae4. Evidence comes from the installed package
`OFXR-BRIDGE-0.3.1-v682.zip`, SHA256 `7D7D98A9...9B40C8A3` (layer DLL SHA256
`FE6EE767...223B1053`). Game: Ghost of Tsushima DC with R.E.A.L. VR 26.3.0.25587
(AER), AFW-only, mode 3, F9 flight logs, 2026-10-27.

## Measurements

Medians over the retained flight window. "EndFrame" is the application's
xrEndFrame through the layer; "runtime End" is the downstream call inside it.

| PID | Headset / runtime | Luke render -> eye | Scan ROI | Output FPS @90 Hz | EndFrame | runtime End | Token fence wait | Map + decode |
|---|---|---|---|---|---|---|---|---|
| 85912 | Pimax Crystal Super / SteamVR 2.18.4 | 3680x3680 -> 3680x3089 | 260x603 | 51-57 | 19.7 ms | 0.26 ms | 11.4 ms (p90 14.0) | 7.1 ms (p90 8.4) |
| 42556 | Quest 3 / VDXR, pacing Auto | 2624x2624 square, cropped at the sides | 481x12 | 88-90, dips to 84 | 8.9 ms (p90 12.1) | 0.05 ms | 7.2 ms (p90 10.2, max 14.6) | 1.1 ms |

On the Pimax, 18.5 of the 19.7 ms EndFrame is the token read. The layer
prepared AFW on 760 of 1257 frames (495 `missing_capture`) and every GPU
stage succeeded, so AFW was working, but this frame time caps the game at
about 55 FPS. On the Quest, the same wait leaves too little of the 11.1 ms
budget, so frames are lost whenever it runs long. Lowering the resolution
changed little, because the wait only shortens with the GPU frame.

## Mechanism

`prepare_luke_afw_frame` (openxr_layer.cpp) calls `read_luke_color_origin_pair`
on the application's thread inside xrEndFrame. `read_origins`
(luke_color_origin.cpp) submits the marker copy after the frozen AFW scratch on
the capture queue, waits for its fence, maps it and decodes every scan row on
the CPU. The fence wait lasts until the GPU has produced the eye color.

Moving that work to another thread does not remove the wait from REAL VR's
frame loop. Its next xrBeginFrame must follow this frame's xrEndFrame. That
EndFrame needs the AFW work recorded, which needs the guide, which needs the
token. Only choosing the guide before the token is read removes the wait, and
that changes the rule that a missing, ambiguous or expired marker is an error
with no selected-metadata fallback. So this branch does not do it.

## Change in this branch: decode the learned rows first

The CPU decode scales with scan height. REAL VR crops each eye from a taller
square producer at a fixed offset (591 Pimax rows here), so the scan covers
every possible offset, yet each read finds the band in the same rows.
`decode_luke_origin_images_windowed` decodes the rows where the previous read
of the same scan geometry found its CRC-valid band, plus 16 rows on each side
(the existing translation bound). A miss, an ambiguous result or a geometry
change decodes the whole scan in the same read, so no frame loses its token.
A conflicting token outside the window is seen at the next whole scan, not
before: the periodic whole scan that looked for one every 32nd read was
removed, as it cost a 3-6 ms hitch every 32 frames for a case never seen. The GPU
copy, the marker, the CRC and the identity rules are unchanged. Stage 127
records which colors were decoded from their learned rows.

The decode cost depends on the crop shape, not on the runtime. The scan
height is the producer height minus the eye height, plus 12 rows. With
REAL VR's square `ForcedRes`, an eye wider than tall (Pimax, aspect 1.19) is
cropped top and bottom, which gives the tall 603-row scan. An eye taller
than wide (Quest 3, aspect 0.92) is cropped at the sides, so its scan is
12 rows and the decode was already about 1 ms. The cropped rows scale with
`ForcedRes`. Wide-FOV headsets gain the most; the change is roughly neutral
on the Quest.

Users do not choose the square. REAL VR's shipped Tsushima profile
(`RealRepo\GOT\RealVR.ini`) contains only `[RVR] ForcedRes=2048,2048`, and
its Horizon Zero Dawn profile has `2704,2704`. The DLL's startup smart resize
("Startup smart resize to ... ppd", "fromForcedRes") then writes other
square values from the target PPD: 3680, 3456 and 2624 in these runs. So
the tall scan is the default on wide-FOV headsets.

Measured on the Pimax/SteamVR in Tsushima, AFW-only, with REAL VR on legacy
AER, comparing stock nightly d0e3ae4 (PID 49936) with this change
(PID 25084, layer DLL SHA256 `B5A17E2F...`):

| | Stock | This change |
|---|---|---|
| Token decode | 7.2 ms (p90 9.0) | 0.48 ms (p90 0.74) |
| Token fence wait | 6.7 ms | 8.0 ms (unchanged by design) |
| Application xrEndFrame | 14.4 ms (p90 20.0) | 9.5 ms (p90 10.9) |
| Output FPS @ 90 Hz | 45-73, briefly 90 | 88-90 |

1,510 of 1,610 reads (94%) decoded both colors from their learned rows. The
rest fell back to a whole scan for one color, and no read lost its token.
The scenes differ between the runs, so the FPS row is indicative; the timing
rows compare directly. The 7-11 ms fence wait remains. It now fits the
11.1 ms budget here, but a heavier frame can still exceed it.

## Options for the fence wait (not implemented)

1. **Predict, verify on the GPU.** Predict the guide from the last verified
   sequence and record AFW immediately. A small compute pass compares the
   stamped bits with the prediction. On mismatch, a predicated copy restores
   REAL VR's original eye. Each frame is either correct or raw AER, never
   warped from the wrong guide. This needs new shader code.
2. **Predict, verify one frame later on the CPU.** This is smaller, but a
   misprediction shows one wrongly warped frame before the layer returns to
   blocking reads.
3. **Select on the GPU.** Record AFW for each candidate guide and keep one by
   predication. This multiplies the AFW GPU cost.

## Related findings

- **The resolution mismatch reaches beyond AFW.** A square `ForcedRes` never
  matches the eye: REAL VR's log reports "ideal AR would be 1.1912" (Pimax) or
  0.9159 (Quest). Pixels are rendered and then cropped in every mode. For
  array-size-one swapchains, `dlss_motion_vectors.cpp` (1267-1275) discards any
  DLSS stream whose output is not the eye size, which would exclude DLSS-G,
  FSR and motion-vector routes under REAL VR. That route path is inferred and
  not yet confirmed in a log. Removing `ForcedRes` brings back REAL VR's
  square default, and its smart resize may overwrite a non-square value
  (for example `3680,3089` on the Pimax); neither has been tested. Whether
  a non-square render is supported for a title is REAL VR's design
  question. With the learned-row decode, the remaining OFXR cost of the
  square is the discarded pixels, not the scan.
- **The nightly test build does not link.** `xrfg_private_release_readiness_tests`
  has 16 unresolved `D3D11D3D12SwapchainInterop` symbols on clean d0e3ae4, so
  `Build-Autonomous.ps1 -RunTests` stops before CTest.
- **Two tray settings act wider than they look.** Diagnostics is stored per
  game: enabling it with no game connected edits only the global defaults, so
  an existing profile keeps logging off. `[tray] pacing` is global:
  `force_steamvr` selects the private delivery runtime on VDXR too. In
  Quest/VDXR PID 87968 (with Director pacing), the downstream End waited a
  median 10.6 ms and output stayed at 79-81 FPS.

## Acceptance

Set REAL VR to legacy AER. In AER v2, Tsushima shows mirrored doubling per
eye and the AFW debug tint only flashes, although the flight log still shows
AFW publishing. Restart the tray and game, press F9 in gameplay with AFW on,
and keep 10 s. Stage 127 should report bit value 3 on most reads. Stage 124
to 125 (the decode) should be under 1 ms. Stage 123 to 124 (the fence wait)
is unchanged.
