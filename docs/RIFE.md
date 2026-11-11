# RIFE frame generation

RIFE uses NVIDIA CUDA/TensorRT to generate frames from colour images. It does
not require the game's DLSS motion vectors or depth. Luke Ross **OSR/AFW** and
**Neural Rendering** retain their own requirements when used alongside RIFE.

Radeon Vulkan support is still being tested. Its installation choice is disabled
in this release. The [Radeon testing guide](RIFE_VULKAN.md) covers that backend.

## Install or check RIFE

1. Extract the **complete** OFXR archive to a writable folder. Keep its `tools`
   and `python` folders beside `OFXRBridgeTray.exe`, and close any running games.
2. Open the tray menu and choose **RIFE > Check RIFE...**.
3. If the runtime is missing or needs repair, confirm **Install** with
   **NVIDIA (CUDA / TensorRT)** selected.
4. A PowerShell window shows progress. Wait for installation to finish and for
   the tray to report the result.
5. Choose **RIFE > Check RIFE...** again. Confirm that it reports a ready runtime
   and identifies the GPU and available backend. If it reports a problem, use the
   displayed log path and the [troubleshooting guide](TROUBLESHOOTING.md).
6. In Frame generation settings, select **RIFE**. Start with model **4.4**,
   **Medium** Quality and **2X**.

The runtime is installed in **`%LOCALAPPDATA%\ofxr-bridge\RIFE\runtime`**.
NVIDIA RIFE uses about **8 GB** installed and needs at least **14 GB free** during
setup. Optimization caches need additional space. Initial installation requires
an Internet connection.

Use **RIFE > Reinstall RIFE...** if you need to reinstall the runtime. To prepare
TensorRT engines ahead of play, first open each game once so OFXR records its
resolution, then close the games and select **RIFE > Pre-compile TensorRT for
found games...**. See the [pre-compilation guide](V863_RIFE_PRECOMPILE.md).

**Check RIFE** verifies the installation and runs a small RIFE 4.4 inference.
If the GPU is busy, unsupported or out of memory, or the check times out, close
other GPU applications and retry as appropriate. Use the reported log when
requesting help; a busy check does not necessarily mean reinstallation is needed.

## Models, Quality and multipliers

Normal mode always uses **4.4**, with optional TensorRT acceleration, and has
no model dropdown. **Advanced mode** exposes the installed compatible versions
found in the runtime's `models` folder: **4.6**, **4.4** and **4.3**, when their
weights are present. The list refreshes when opening the menu. Unknown or retired
Lite networks are not offered. Turning Advanced mode off immediately returns
to **4.4**, including for profiles which previously saved another model.

**Quality** offers **Potato**, **Low**, **Medium**, **High**, **Very high**,
**Ultra** and **Godlike**, with **Medium** as the default. It controls the resolution used to
estimate motion. Higher settings cost more GPU time and can improve fine-edge
and occlusion precision. Compare them in the same scene. Older intermediate
settings map to the closest available preset.

The generated colour output remains at the original image resolution. RIFE
predicts displacement and blending fields at the chosen Quality; OFXR then
uses them to sample the original full-resolution colour images. It does not
enlarge a low-resolution generated RGB frame. Real frames remain unchanged.

**2X** is the default. **3X** and **4X** are supported on the native
D3D12 route and on compatible Luke Ross routes after OSR/AFW has produced the
stereo pair, using either Framework or Director. Other legacy/shared/interop
routes retain 2X support. **AER framegen** uses 2X only.

RIFE produces images; the selected pacing route still controls delivery.
Its speed, artifacts and achievable headset frame rate depend on the game,
GPU, resolution, model and other enabled features.

## TensorRT optimization and caches

For model **4.4**, the runtime can build an FP16 TensorRT engine for the current
GPU, image dimensions and eye count. The information meter shows **Optimizing,
please wait** while a separate process compiles it. Optimized PyTorch generation
continues during compilation; a missing compiled engine is not an installation
failure.

Engines are reused from
**`%LOCALAPPDATA%\ofxr-bridge\RIFECache`**. Changing Quality, image dimensions or
model code can require a new engine or CUDA graph warm-up. First use can take
longer than later launches. TensorRT unavailability, a failed compilation or
an invalid engine cache leaves the PyTorch fallback available. Other RIFE
models use PyTorch. A valid AppData runtime can use the PyTorch fallback.

The two eyes share model weights while retaining separate image histories.
Reusing a runtime or cache does not mix the eyes or game sessions. Cache files
are not bundled with a release and are not automatically deleted on upgrade.

## Updating and troubleshooting

Close the old game and tray before switching OFXR builds, and keep the tray and
bridge files from the same archive together. Run **Check RIFE** after moving or
updating a package if RIFE cannot start.

Setup verifies a staged installation before replacing the active runtime. If a
worker is using it, replacement reports busy instead of terminating the game.
Interrupted activation retains or restores the previous installation. The
installer removes its own default download cache after success; optimization
caches are separate.

If a route stays on **Applying settings**, check that the requested multiplier
is supported on that route, then collect an OFXR diagnostic log and the RIFE
check result. For CUDA or device errors, include the GPU and driver version.
If TensorRT is still optimizing, allow compilation to finish before comparing
performance; repeatedly reinstalling the runtime does not create the engine.

## Reproducible runtime and modified RIFE code

The package uses fixed dependency versions, download URLs, byte counts and
SHA-256 hashes from [`runtime-lock.json`](../python/rife/runtime-lock.json):

| Component | Pinned version |
| --- | --- |
| Python | 3.12.10 |
| PyTorch | 2.9.1+cu128 |
| CUDA Python bindings | 12.9.10 |
| TensorRT | 10.14.1.48 |

Model code derives from [HolyWu/vs-rife](https://github.com/HolyWu/vs-rife) at
revision `3488617283db7c428a83ba4a19382285da698b6a`. Setup first downloads and
verifies that exact upstream source, then applies **all 17 modified OFXR
overlay files shipped with this package** over it. The accompanying overlay
manifest verifies the resulting files. Original model weights are unchanged;
model code retains its MIT license. Newer upstream releases are not substituted.

Release packages also include the pinned app-local Microsoft CRT files and
their license. Python environments, downloaded wheels, weights and generated
engines are installed or created separately; they are not copied into the
OFXR release archive.

For building and assembling a complete package, see
[`BUILDING_RIFE.md`](BUILDING_RIFE.md). For general OFXR setup and diagnostic
logging, see the [README](../README.md).
