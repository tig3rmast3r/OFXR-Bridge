# V857: 3X and 4X for OFXR and Hybrid, 4X for DLSS-G

0.2 had 3X (V394-V401) and 4X (V558) for OFXR and Hybrid. In 0.3 only the
0.2.13 route and DLSS-G had 3X, and RIFE alone had 4X.

## Settings

- **Multiplier** (2X, 3X, 4X) for OFXR's FidelityFX, 0.2.1 and 0.3.0 engines
  and for Hybrid, saved per route (`frame_multiplier`). 0.2.13 keeps its
  2X/3X.
- **DLSS-G 4X** (multi-frame generation). DLSS-G's 2X/3X keep
  `triple_frame_gen`, which 4X holds on, so a session that cannot run 4X runs
  3X.
- 3X/4X need the private SteamVR route with a D3D12 game (Luke Ross
  included); native VDXR and Pimax run D3D12 games up to 3X. Other sessions,
  D3D11 games among them, stay at 2X.

## How it works

OFXR/Hybrid at 3X/4X, and DLSS-G at 4X where the GPU makes three frames, take
RIFE's ordered multi-output route: the director worker, extra output rings,
ready clock, wait-for-prepared and fixed output times (1/3, 2/3; 1/4, 2/4,
3/4). `ordered_multiframe()` replaces RIFE's gates about the route and is true
for RIFE exactly as before; RIFE's own gates (async queue, panel queue,
history overlap, refusal) are unchanged. The synthesizer and the native
DLSS-G provider make a third output (4X).

The guide status: under MSFS 2024's foveated rendering no DLSS output is the
eye's size, so each eye's lookup by size fails and xrEndFrame places the
foveated periphery by its camera (V515). At 3X/4X the pair runs on the worker
after xrEndFrame has published the status, so the failed lookups showed
Hybrid red ("Queue mismatch") although it used the placed guides. A per-eye
lookup that the placement revisits is now tentative: its failure is reported
only if the placement finds nothing either.

## Flight log

- `synthesis_pair` 9001-9010: which check refused the extra outputs.
- `luke_afw_stage` a=1196: Hybrid's guide pair, result = its failure code
  (0 composed with the game's vectors, 1 no previous guides, 2 no current
  guides, 10+ per eye), b and c the current and previous first-eye serials.
  The first sixteen pairs, every change and every 256th.

## Test

Headset: OFXR and Hybrid 3X/4X in Dead Island 2 (UEVR), DLSS-G 3X/4X, Hybrid
3X/4X in MSFS 2024 with foveated rendering. Tests:
`xrfg_private_runtime_layer_tests` multi{3,4}-{new,fidelity,ref,hybrid} with
exact output counts, multiswitch live 2-3-4-3-2, multi4 pooled DLSS-G;
r19-camera at 4X; `xrfg_d3d12_history_tests release-bound-guides` (tentative
lookup); autonomous control checks for the per-route multiplier and DLSS-G
4X.
