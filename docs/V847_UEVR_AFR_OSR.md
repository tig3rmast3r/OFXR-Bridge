# V847: OSR for UEVR's Alternating/AFR rendering

This nightly update adds OSR (OFXR Stereo Reconstruction) for stock UEVR games
rendered with UEVR's Alternating/AFR method, as it already works for Luke Ross:
OFXR reconstructs the eye UEVR did not render in a frame, so every frame
reaches the runtime with two current eyes. It works on its own and under OFXR
frame generation, DLSS-G and Hybrid. Luke Ross games, frame generation without
UEVR and the delivery paths are unchanged.

## Setup

OFXR detects UEVR (`UEVRBackend.dll`) and reads UEVR's own settings through
its plugin API. In UEVR:

- **Rendering Method: Alternating/AFR** and **Synchronization: Very Late**
  (UEVR's default). With Late, UEVR releases the first eye before it begins
  the frame, so no frame can be inserted.
- **Ghosting Fix on** (required). It gives each eye its own UE view state, so
  the game's DLSS vectors point at the same eye's previous frame. Without it
  the reconstructed eye ghosts everywhere.
- If UEVR's Ghosting status stays *learning* or reads *failed closed*, turn on
  **Bootstrap Separate View States** (`VR_GhostingFixBootstrapViewStates`)
  and restart the game; do not toggle it live. Hogwarts Legacy and Atomic
  Heart need it. Only some UEVR builds offer it.

In OFXR, the Frame Reconstruction tab (formerly the Luke Ross tab) selects
**OSR**. The game must run on D3D12 with DLSS: OSR takes the real eye's depth
and motion vectors from the game's DLSS evaluation. FSR or XeSS games (also
through OptiScaler) are not supported yet.

## How it works

UEVR's Alternating/AFR renders the left eye on one game frame and the right
eye on the next, and ends one OpenXR frame per pair (`src/layer/uevr_afr_osr.inc`).
On each eye's release:

1. The released image is copied while UEVR still owns it.
2. The other eye is reconstructed with Full OSR from that frame's DLSS depth
   and vectors, both eyes' cameras (UEVR's stereo view callbacks), and the
   other eye's own previous frame for what the real eye cannot see
   (`src/d3d12/uevr_osr.cpp`). It is written into the other eye's swapchain
   through the layer's own acquire/wait/release. The real eye is relocated in
   place to the same pose, so both eyes of the frame share one head pose: the
   pose of the frame's display slot.
3. After the pair's first eye, the frame UEVR began is ended here with both
   eyes and a new frame is waited and begun, so the second eye gets a frame of
   its own. UEVR's own End then ends that one.

Details that matter for image quality:

- UEVR samples the head pose once per pair and holds the engine rotation for
  the pair. The pair's second frame turns both eyes by half of the last
  pair's stick yaw step, and onto the first frame's world where UEVR leaves
  them apart (Atomic Heart aims from a controller: 1-1.3 degrees while
  turning, limited to 10 degrees).
- Under SteamVR's private runtime, the slot pose is snapped to the grid of
  native slots and the native End carries the time the frame was relocated
  for, so SteamVR reprojects it from that pose.
- Full OSR gains two flags for camera-attached surfaces (UEVR's weapon hangs
  on the game camera, which the head turns away from): history texels follow
  that camera (`kOsrFullFlagLockedFollows`), and the warp keeps attached
  surfaces instead of taking them from the previous frame
  (`kOsrFullFlagWarpKeepsAttached`). Luke Ross OSR does not set them.
- Under frame generation each eye carries a guide of its own: its shown depth
  and its motion since its previous frame, one unbroken serial chain per eye
  (`src/d3d12/uevr_osr_guide.hlsl`). The game's evaluation covers only the
  real eye; bound to both eyes, DLSS-G refused every frame.

The tray and the headset meter show **OSR: UEVR**, green while it
reconstructs. The Frame Reconstruction tab gets a global Off and setup text
for Luke Ross and UEVR.

## Flight log

`luke_afw_stage` records, code in `a`:

| code | meaning |
|---|---|
| 1400 | OSR on (result 1) or off (0); b = rendering method; c = layout, settings read, Ghosting Fix, synchronisation << 8 |
| 1401 | each reconstruction: result = HRESULT; b = source eye, history 0x100, vectors 0x200, camera 0x400, reset 0x800, real eye relocated 0x1000, renderer refusal << 16; c = DLSS publication |
| 1402 | inserted frame: result = its End; b = Wait << 32 \| Begin; c = new display time |
| 1403 | once a second: reconstructions << 32 \| refusals; inserted frames, camera views |
| 1404 | a refused reconstruction: result = reason (1 no swapchain, 2 eye sizes differ, 3 no DLSS evaluation, 4 none newer than the last, 5 acquire, 6 wait, 7 release, 8 renderer; detail in c) |
| 1405 | UEVR's view callbacks registered: result = pre \| post << 1 |
| 1406 | source capture failed: result = HRESULT |
| 1407 | why OSR is off: 1 session (binding, Luke Ross, Reconstruction setting), 2 UEVR settings unreadable, 4 not Alternating/AFR, 8 not Very Late, 16 no two-eye projection |
| 1408-1413, 1415, 1417 | pose source, camera, submitted pose, geometry, yaw, stick step, GPU time and pair-turn receipts (described at each call) |
| 1416 | a frame without the two-eye layout while OSR ran |
| 1418 | inserted frame: b = presenter active \| pipelined << 1 |

Private runtime: trace 42 (transition 5752) logs a frame submitted for its
content time after its End, trace 43 (5753) a frame whose content time the
runtime refused, dropped alone.

## Test

Dead Island 2, Atomic Heart and Hogwarts Legacy under stock UEVR
(Alternating/AFR, Very Late, Ghosting Fix, bootstrap where needed): two
current eyes each frame, head and stick turns without doubling. OSR alone on
SteamVR and VDXR; under OFXR frame generation, DLSS-G and Hybrid on VDXR, and
under frame generation on SteamVR (with V846). Cyberpunk 2077 under Luke Ross
on SteamVR: menus and gameplay as on V845. `xrfg_uevr_afr_osr_tests` covers
the eye phase, camera queue, eye-to-eye geometry, image tangents, stick share
and pair turn.
