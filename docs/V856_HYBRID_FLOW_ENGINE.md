# V856: Hybrid's own optical flow engine and flow settings

Hybrid always ran FidelityFX flow at a quarter per axis (V464), while OFXR
could use NVIDIA flow at a chosen resolution. In Returnal, OFXR on NVIDIA
0.3.0 showed less background-reveal ghosting than Hybrid. Hybrid now has the
same flow choices as OFXR, saved separately from OFXR's.

## Settings

- **Optical flow engine** under Hybrid: FidelityFX, Nvidia 0.2.1 or Nvidia
  0.3.0 (`hybrid_backend`), independent of OFXR's engine. 0.2.13 is left out:
  it is a pacing route as well as a flow. The fallback before guides arrive
  uses the same engine.
- Flow quality, flow resolution and bidirectional flow come from
  `[route.hybrid]`, never from the OFXR routes. FidelityFX shows flow
  resolution only; the layer no longer forces a quarter.
- Profiles saved before this get `[route.hybrid]` scale 25%, the resolution
  they ran at, and carry `hybrid_flow_revision=1` from then on. New profiles
  default to 25%. The shared-memory layout is unchanged.
- OFXR on FidelityFX now shows its flow resolution too. It always ran at
  `[route.ofxr_fidelityfx]` scale; only the control was missing.

## How it works

The NVIDIA hybrid shaders (`SynthesizeHybridNvidia*MidpointPS`) compose the
game's vectors with NVIDIA's flow by the FidelityFX hybrid's rule. A
double-wide image's extra guide blocks get its one flow. Benchmark
configuration bits record Hybrid's engine for Hybrid; the benchmark's Hybrid
step stays on FidelityFX.

## Test

`xrfg_d3d12_history_tests hybrid` adds Nvidia 0.2.1 and 0.3.0 at 50% and 25%.
Autonomous control checks cover the separate engine, no 0.2.13, the 25%
default and the migration.
