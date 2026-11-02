# V804: optional 2x Neural Rendering

Based on V803 (`3b2d11e0e343079528117a39c123efe2503bd400`). This local
experimental package retains its private-admission and auxiliary-image fixes.
RIFE models, Python worker, shared runtime, inference and pacing are unchanged.

The Neural Rendering tab has one additional checkbox, **2x NR**, default OFF.
It is available both in desktop settings and F10, persists in the existing
global/game settings, and can be toggled without restarting the game. It has
no effect while Neural Rendering is disabled. Reset to defaults turns it OFF.

## Processing contract

- Encode the original colour once, using the existing HDR/LDR contract.
- Apply the selected working resolution once and match the private guides.
- Run the first model, clamp its encoded RGB to finite [0,1] values, and run a
  second model on that result. Alpha is preserved by the intermediate clamp.
- Both model instances use exactly the same preset, intensity, style, tone,
  structure, mask, guide dimensions and motion-vector scales.
- Both run at the same working dimensions: 50% means 50% for both, not 25%
  for the second. Only the model edit is resolved back onto the full-size image.
- Keep independent temporal features for each native eye/handle and each pass.
  Resets propagate to both. The final resolve runs once, after the second model.

The same chain supports before-SR and after-SR injection. The feature adds
another model instance and two working-resolution textures per native handle,
so it uses additional GPU time and VRAM. It is a quality option, not an FPS mode.

Optional state retires behind the existing exact native submission fence when
disabled, resized, retuned, rebound to a device or shut down. No new queue wait,
pacing rule or diagnostic timer is introduced. An optional-stage error is
reported; it leaves the game colour unmodified and does not poison the original
one-pass model. Disabling 2x NR restores that original model.

Intermediate clamping is adapted from wilsjo2's OptiScaler-DLSSNR-PreSR-Multipass
v0.8.10, commit `6125a75b37f7e2a408e31934f4827e5660429203`, mode 8 in
`OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl`. Attribution and GPL-3.0
provenance are retained in `src/neural/NrClampEncoded.h` and the existing licences.

## Validation scope

The native host tests exercise the real pre/post-SR pipeline, codec, resource
transitions, command recording and GPU retirement with WARP. Only vendor NGX
model callbacks are replaced by a deterministic GPU transform, allowing pixel
verification that the second pass reads the first result. Tests verify stereo
history independence, matching settings/resolution/guides, reset propagation,
OFF recovery, optional-model refusal and release only after the submitted GPU
fence retires. D3D12 validation must report no errors.

The actual clamp shader is also exercised on WARP and the local NVIDIA GPU,
including NaN/Inf, out-of-range values, alpha preservation and guarded extents.
Control/IPC, profiles, defaults, hot settings, existing NR contracts/codec and
selected V803 handoff regressions are checked separately.

These checks do not measure the signed NVIDIA NR model's visual quality or
in-game FPS; those require the user's game comparison. No game profile,
installed DLL, registry entry, shared runtime or canonical nightly is modified.
The package does not bundle the signed `nvngx_dlssnr.dll`.
