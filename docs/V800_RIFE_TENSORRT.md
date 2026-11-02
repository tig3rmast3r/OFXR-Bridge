# OFXR-BRIDGE 0.3.2 experimental (v800)

V800 starts from the accepted RIFE V745 laboratory and merges the public
nightly through `b219ae7262cfbfd0bd8792eb7486401052b38409`. The nightly and the
original V745 remain separate references. This is a local test build.

## RIFE execution

- RIFE 4.4 uses FP16 TensorRT engines specialized for the actual input width,
  height and eye count. Two available eyes execute together in one batch.
- CUDA Graph replay, five-channel flow/mask output, FP32 warp coordinates and
  native-resolution colour reconstruction are retained.
- A missing engine is prepared by a separate compiler process. The Elliott
  meter displays **Optimizing, please wait** while the optimized PyTorch
  fallback continues to produce frames. Pause/applying settings keep priority.
- The PyTorch 4.4 fallback also batches stereo, uses channels-last convolution
  weights and a fused intermediate warp with FP32 coordinate arithmetic.
  Other models retain their existing path. INT8/FP8 are not enabled.
- Successful engines are cached under
  `%LOCALAPPDATA%\ofxr-bridge\RIFECache`. Later launches reuse them. Changing
  rendering resolution or the RIFE slider can require another optimization.
- Cache validation includes model/code hashes, GPU, TensorRT/CUDA version,
  actual dimensions and batch size. Corrupt entries, unavailable dependencies
  or compiler failures retain PyTorch generation rather than claiming success.

There is still only **one shared portable runtime**. It is upgraded for V800;
the small build ZIP does not duplicate Python, weights or TensorRT libraries.
The optional setup installs version-pinned vendor packages into that runtime,
without system pip/Python or global environment changes. Older bridge builds
do not send the V800 capability and keep their original serial behaviour.
See [RIFE setup](RIFE.md) for a different machine or a fresh runtime.

## Included public nightly changes

- Djules V733/V734: native scene-camera command-list tracking and producer
  device Execute hook coverage.
- Djules V735: optional OSR fill around near-object stereo bands.
- V746/V747: expired Framework cycle recovery and retained GPU history
  retirement after quarantine or presentation-domain changes.
- V748: verified camera placement for resized native DLSS-G guide canvases.
- V755: selected legacy Vulkan recognition for DOOM: The Dark Ages and
  Indiana Jones and the Great Circle.
- V756: DOOM: The Dark Ages HDR origin-marker contrast.
- Djules public V757: packed D3D11 depth transport and side-by-side guide
  matching, verified upstream for Ready or Not with VRO.

The public V757 is distinct from the rejected local Tsushima NR trial with the
same old build number. That rejected commit is not imported. V745's existing
RIFE pacing and buffer behaviour is preserved; V800 does not change delivery
cadence to obtain the inference speedup.

## Testing

Focused tests cover the worker protocol, legacy capability, cache integrity,
compiler lifecycle, stereo isolation, fractional inference, the meter and
native D3D12 output. The cold-cache integration test requires real fallback
output during optimization, then actual TensorRT activation and a cached
first inference in a newly launched worker, without F9 enabled.

Automated laboratory checks do not establish a new game/headset FPS result.
For the initial comparison select RIFE 4.4, keep the same scene/settings and
wait until the optimization message disappears. The F10 details identify
TensorRT FP16 when it is active. The slider only reduces motion inference;
the output colour stays at the original resolution.
