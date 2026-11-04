# RIFE frame generation (nightly)

RIFE is an experimental NVIDIA CUDA frame-generation route. Select **RIFE** in
the Frame generation menu, between OFXR and Hybrid. The existing OFXR default
does not change when RIFE is installed.

RIFE estimates motion from colour images; it does not require the game's DLSS
motion vectors or depth. Luke Ross **OSR/AFW** and **Neural Rendering** retain
their own capture and model requirements when used alongside RIFE.

The tray also offers **RIFE → Reinstall RIFE...** for a clean runtime/cache reset
and **RIFE → Pre-compile TensorRT for found games...** to prepare the six Quality
presets using each game's last recorded resolution and eye layout. Open games
once with V863 or newer to populate the list; diagnostics need not be enabled.
See [offline preparation and progress](V863_RIFE_PRECOMPILE.md).

Installed source files are aligned automatically with the current tray at startup.
This updates only the source overlay, preserving dependency libraries, weights and
compatible engine caches. See [source updates](V864_RIFE_OVERLAY_UPDATE.md).

## Install or check RIFE

1. Extract the **complete** OFXR archive. Keep its `tools` and `python` folders
   beside `OFXRBridgeTray.exe`.
2. Open the tray menu and choose **RIFE → Check RIFE...**. The tray remains responsive
   while the check runs.
3. If RIFE is missing or needs repair, confirm the installation. **NVIDIA** is
   selected; **AMD Radeon (RDNA)** is disabled because that backend is not yet
   available.
4. A PowerShell window shows the installation progress. It needs no further
   keyboard input. Wait for the tray to report the result.
5. Select **RIFE** in the settings and start with the default **4.4** model,
   **Medium** Quality and **2X** multiplier.

The managed runtime is shared between OFXR builds and installed under
**`%LOCALAPPDATA%\ofxr-bridge\RIFE\runtime`**. It uses about **8 GB** after
installation and requires at least **14 GB free** for downloads and staging.
Optimization caches use additional space and can grow as you change settings.
An Internet connection is needed for the initial installation. No administrator
elevation, global Python/pip installation, PATH change or driver installation
is performed.

**Check RIFE** verifies the pinned dependencies, model and adapter files, and
runs a small actual RIFE 4.4 CUDA inference. A ready result reports the GPU and
available backend. No package, development or ancestor directories are searched. If the AppData
runtime is absent, the check offers installation even if another copy exists.

If the GPU is unsupported, busy or out of memory, or the check times out, the
tray reports the problem without offering automatic reinstallation. Close GPU
applications and retry a busy check. A repair offer is reserved for a missing
or damaged runtime. The result includes the log path for diagnosing failures.

## Explicit developer override

Normal installations use AppData only. For development, a runtime may be selected
by manually adding an absolute path to the global `%LOCALAPPDATA%\ofxr-bridge\ofxr-bridge.ini`:

```ini
[tray]
rife_runtime_dir=D:/RifeDevelopment/runtime
```

The tray preserves this manual value and copies it into the generated runtime
INI's `[ofxr]` section. Remove or clear the key to restore AppData. An invalid
explicit path fails instead of looking elsewhere. Restart the tray and game
after changing it. This has no menu control and is not saved in game profiles.
**Check RIFE** always checks/installs the AppData runtime, including when this
developer override is present. No automatic runtime lookup is performed.

## Models, Quality and multipliers

Normal mode always uses **4.4**, with optional TensorRT acceleration, and has
no model dropdown. **Advanced mode** exposes the installed compatible versions
found in the runtime's `models` folder: **4.6**, **4.4** and **4.3**, when their
weights are present. The list refreshes when opening the menu. Unknown or retired
Lite networks are not offered. Turning Advanced mode off immediately returns
to **4.4**, including for profiles which previously saved another model.

**Quality** offers **Potato**, **Low**, **Medium**, **High**, **Very high** and
**Ultra**, with **Medium** as the default. It controls the resolution used to
estimate motion. Higher settings cost more GPU time and can improve fine-edge
and occlusion precision. Compare them in the same scene. Older intermediate
settings map to the closest available preset.

The generated colour output remains at the original image resolution. RIFE
predicts displacement and blending fields at the chosen Quality; OFXR then
uses them to sample the original full-resolution colour images. It does not
enlarge a low-resolution generated RGB frame. Real frames remain unchanged.

**2X** is the default. Experimental **3X** and **4X** are supported on the native
D3D12 route and on compatible Luke Ross routes after OSR/AFW has produced the
stereo pair, using either Framework or Director. Other legacy/shared/interop
routes retain 2X support. An unsupported multi-output request is refused rather
than represented by repeated copies of a generated image.

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

V865 reduces native D3D12/CUDA handoff stalls and allows one owned next-source
capture during generation. Models and compatible engines are unchanged. See
[implementation and targeted validation](V865_RIFE_PIPELINE.md).
