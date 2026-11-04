# V853: D3D11 guides accept a zero frame time and realign offset inputs

This nightly update changes how D3D11 DLSS evaluations become game guides
for Hybrid and DLSS-G. Other routes are unchanged.

## Symptom

Skyrim VR with PureDark's DLSS upscaler never gave Hybrid or DLSS-G the
game's vectors: every D3D11 evaluation was refused (NGX capture
`d3d11_evaluate a=9`). With 0.2's fix ported, vectors were then offset from
their pixels, a halo round moving objects.

## Cause

- The mod passes `FrameTimeDeltaInMsec = 0`, and the guide registry refuses
  a publication that has depth but no positive frame time.
- The mod keeps its foveated offsets at scale 1.0: DLSS reads 2824 x 2212 per
  eye at (56, 88) and (2768, 88) of 9740 x 3813 vector and depth textures,
  while its output rectangle says (0, 0). Read at those bases, vectors sit
  about 97 px across and 152 px down from their pixel (0.2 V521).

## Change

- The D3D11 ingress falls back to 16.6667 ms for a frame time <= 0 or
  > 1000 ms, as the Vulkan guide ingress already did.
- An input base offset inside its eye's region goes back to the region's
  origin, by a rule with no game name (`include/xrfg/dlss_input_region.hpp`):
  per axis, the input start the output's placement does not explain must lie
  off the grid of region-sized cells by 1 texel to a tenth of a region. A base
  at the texture's half, or a region flush with its far edge, is an
  allocation slot and is never moved (side-by-side eyes at render scales
  0.9-1.0). Luke Ross streams keep their bases, since AFW/OSR crop them.

## Flight log (op `luke_afw_stage`)

- 1186: the first sixteen realignments. Result is the stream; a and b are the
  vectors' and depth's removed x << 32 | y.
- 1187: why the registry refuses a publication, the first eight per reason.
  b is the reason (1 tracking off, 2 invalid, 3 colour token, 4-6
  output/vectors/depth snapshot, 7 counter); c is the failed checks for
  reason 2 (64 = no positive frame time with depth).

## Test

Skyrim VR, PureDark DLSS, Hybrid and DLSS-G: guides published with depth
(`d3d11_evaluate a=10`), no 1187, 1186 shows (+56, +88) and (-56, +88).
`xrfg_dlss_input_region_tests`, 13 checks.
