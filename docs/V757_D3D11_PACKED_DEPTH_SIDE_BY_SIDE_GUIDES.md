# V757 D3D11 DLSS guides: packed depth and side-by-side eyes

Ready or Not with the VRO mod (UE, D3D11 session through the D3D11 bridge,
one 8344x3268 side-by-side swapchain) ran only plain optical flow. Hybrid showed
"Missing game vectors" and DLSS-G refused with `generation_prepare` 27
(native_stereo_unavailable). 0.2 had this working since V512/V521.

Two independent blocks, both measured in the headset (Pimax, 2026-10-30):

1. **Packed depth.** The game's DLSS depth is R32G8X24 (bind flags 0x48). The
   D3D11 transport refused packed depth-stencil formats, and the evaluation hook
   treated a refused depth as a failed publication: nothing was ever published
   (v735: no 811 record, capture log silent after NGX init).
2. **Side-by-side eyes.** One DLSS feature per eye (render 2837x2223 or
   2086x1634, output 4172x3268 in a 4176x3268 texture), each copied into its half
   of the swapchain image. Receipts only cross whole-resource copies, so the
   image never carried one (v757 test build: every 811 lookup returned none).

Changes:

- `D3D11GuideTransport`: a typeless R24G8/R32G8X24 depth crosses over as its
  depth plane in a shared R32_FLOAT texture, written by a cs_5_0 shader
  (`src/d3d11/d3d11_depth_unpack.hlsl`, 0.2 V521) on the game's immediate
  context. Compute slot 0 bindings are restored; a depth target still bound for
  writing is unbound for the read and rebound. Typed D24S8/D32S8X24 resources
  stay unsupported (no shader view).
- D3D11 evaluation: depth is optional (0.2 V505). A depth that cannot cross
  over publishes the vectors without it; Luke Ross titles still require depth.
- Bridge release: when no receipt resolves on a single-layer image, fall back
  to the size-based resolver already used for D3D12 Native Stereo, and keep its
  result only when it finds both eyes (two streams, each exactly half the image
  width). Receipts stay the first choice; an eye-sized image without a receipt
  still borrows nothing (`xrfg_native_dx11_guide_session`).
- Logging: `ofxr-ngx-capture-pid*.log` now gets `d3d11_feature_create`,
  `d3d11_evaluate` (outcome per change: 1 no owner, 2 transport, 3 inputs,
  4 begin, 5 motion, 6 Luke depth, 7 colour, 8 Luke camera, 9 refused,
  10 published with depth, 11 without; detail = bind flags<<32 | format),
  `d3d11_stream`, `d3d11_stream_rects` and `d3d11_stream_textures`. Flight
  record 811 result: 0 by receipt, 2 by size, 1 none; c carries the eye count
  in its top byte; logged on every change of outcome.

Verified: Ready or Not VRO confirmed fixed by the user in the headset.
Local v758 test build: `d3d11_evaluate` 10 (R32G8X24 unpacked), every 811
lookup resolved by size with two eyes, `generation_prepare` 0 throughout.
Each eye's motion texture is its own 4172x3268 (base 0,0); depth is one
4176x1636 texture with the right eye at x=2088.

Test: `xrfg_d3d11_dlss_guides_tests` unpacks R32G8X24 and R24G8 targets
cleared to 0.25 while bound as the game's depth target, reads 0.25 back on
D3D12, and checks the depth target and compute shader are restored, with
D3D11/D3D12 validation clean.
