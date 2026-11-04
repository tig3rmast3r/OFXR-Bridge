# OFXR-BRIDGE

**Smoother VR with the graphics card you already have.**

OFXR-BRIDGE adds extra frames between the ones your game draws. Your headset
gets the smooth frame rate it needs while your graphics card does less of the
work. Play smoothly on a less powerful PC, or turn up the graphics without
losing your frame rate.

In Luke Ross R.E.A.L. VR games, it turns Legacy AER into real stereo: it fills
in the eye the game skips each frame, so the game feels like true stereo VR,
for little more than Legacy AER costs at your headset's frame rate.

It runs from a tray icon: turn it on before you start the game and it works
quietly in the background, without changing any game files. It works with
OpenXR games only (see below).

Download it from [GitHub Releases](https://github.com/tig3rmast3r/OFXR-Bridge/releases).

What changed in this version: see the [release notes](https://github.com/tig3rmast3r/OFXR-Bridge/releases).

**On nightly:** experimental NVIDIA **RIFE** frame generation is also available,
with models 4.6, 4.4 and 4.3, a Quality selector and optional TensorRT acceleration
for 4.4. Use **Check RIFE...** in the tray to set up its pinned per-user runtime.
RIFE is installed in `%LOCALAPPDATA%\ofxr-bridge\RIFE\runtime`; no package or parent folders are searched.
See the [RIFE guide](docs/RIFE.md). The stable release remains a separate download;
a nightly or self-compiled build must be signed separately for anti-cheat use.

Not working, or not feeling smoother? See the [troubleshooting guide](https://github.com/tig3rmast3r/OFXR-Bridge/blob/main/docs/TROUBLESHOOTING.md).

> [!WARNING]
> This is a beta release. It may not work with your game, VR mod, GPU or OpenXR
> runtime. It may produce visual artifacts, fail to activate, freeze the game or
> cause a crash. Use it at your own risk.

> [!IMPORTANT]
> **OpenXR games only.** OFXR works with games that use OpenXR, whatever your
> headset or VR software. OpenVR games can run through OpenXR with
> [OpenComposite](https://gitlab.com/znixian/OpenOVR). With UEVR, choose its
> OpenXR option.

> [!IMPORTANT]
> **What needs what:**
> - **Optical flow:** works on AMD and NVIDIA cards with the **FidelityFX**
>   engine. The **NVIDIA 0.2.1 / 0.2.14 / 0.3.0** engines use NVIDIA's Optical
>   Flow hardware: an RTX card or a GTX 1660. On an AMD card, select FidelityFX.
> - **Hybrid, FSR 3.1, DLSS-G and Luke Ross OSR/AFW:** the game must run
>   **DLSS** Super Resolution or Ray Reconstruction (OFXR takes its guides from it).
> - **DLSS-G:** an RTX 40 or 50-series card.
> - **RIFE (nightly):** an NVIDIA CUDA-compatible GPU and driver, plus its
>   separate runtime. RIFE itself does not need game motion vectors or depth.
> - The [Microsoft Visual C++ Redistributable x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

## Features

**Frame generation**

- Optical flow: **NVIDIA** (Fast, Medium, Slow) at 100%, 75%, 50% or 25%
  resolution, or **AMD FidelityFX**.
- **RIFE (nightly)**: neural frame interpolation on NVIDIA CUDA, with optional
  TensorRT acceleration for model 4.4.
- **Hybrid**: FidelityFX flow combined with the game's own motion vectors.
- **FSR 3.1** and **DLSS** Frame Generation, fed with the game's DLSS guides.
- **3X** frame generation on the OFXR 0.2.14 route.

**Luke Ross R.E.A.L. VR games**

- **OSR (OFXR Stereo Reconstruction)** rebuilds the eye the game skipped, with
  **AFW (legacy)** as an alternative.
- **Frame Reconstruction only**: a full stereo pair every frame, without
  OFXR-generated frames.

**Games and platforms**

- D3D12, D3D11 and Vulkan games, through VDXR or SteamVR.
- UEVR, REFramework mods, OpenComposite and Cheeky Foveated DLSS.
- **Star Citizen**: the official signed release can load through Easy Anti-Cheat.
- **OpenKneeboard** and other OpenXR panels are interpolated with the game.

**Tray and overlay**

- Per-game settings, changed in the headset (**F10**) or on the desktop.
- Automated benchmarks that can pick the best settings for each game.
- An in-headset FPS counter and an information overlay.
- Optional DLSS 5 Neural Rendering (model DLL supplied separately).
  Nightly includes [NR capture diagnostics](docs/V849_NR_CAPTURE.md) and [the V862 RIFE compilation correction](docs/V862_RIFE_TIMEOUT.md).
- A `status.json` export for tools such as MSFS AutoFPS.

The default is **NVIDIA OFXR 0.3.0, Medium, 50% flow resolution**. Flow
resolution sets the cost of motion estimation, not the output resolution.

## Installation and use

1. Extract the complete archive to a writable folder.
2. Run `OFXRBridgeTray.exe` and select **Enable**.
3. Start the game. With injectors such as UEVR, leave OFXR enabled while the
   mod is injected.
4. Open the settings with **F10** in the headset, or **Settings...** in the
   tray. Most settings apply at once; those marked "restart game" apply at the
   next launch.
5. When finished, close the game, then select **Disable** or exit the tray.

For DLSS 5 Neural Rendering, place your own `nvngx_dlssnr.dll` model in the
`ofxr` folder. It is not included, and nothing else needs it. Don't confuse it
with the included `nvngx.dll_dlssnr.dll` forwarder.

### RIFE on nightly

Choose **Check RIFE...** in the tray. If the runtime is missing or needs repair,
confirm the NVIDIA installation; a PowerShell window shows progress without
requiring further input. It installs under
**`%LOCALAPPDATA%\ofxr-bridge\RIFE\runtime`**, using about **8 GB** installed
and requiring at least **14 GB free** for download and staging. GPU optimization
caches need additional space. AMD Radeon (RDNA) is shown disabled for future support.

Start with **RIFE 4.4**, **Medium** Quality and **2X**. Experimental 3X/4X are
available on the native D3D12 route and compatible Luke Ross routes after
OSR/AFW reconstruction. Other legacy/shared/interop routes retain 2X support.
See the [RIFE guide](docs/RIFE.md) for supported paths, compilation, caches and
troubleshooting. Installing RIFE does not change the default OFXR engine.

### For the best results

- **Performance lower than expected, or motion uneven? Change
  [Pacing](#pacing) first.** Switch **Settings / Pacing** between **Director**
  (the default) and **Framework** and compare in the same scene: it applies
  while you play, and Framework can be better.
- **Turn off your VR software's own frame smoothing:** SteamVR Motion
  Smoothing, Meta ASW, Virtual Desktop SSW, Pimax Smart Smoothing, Pico Frame
  Interpolation. It fights with OFXR.
- **Pick a refresh rate close to double your game's frame rate** (triple with
  3X). If OFXR still stops short, your VR software repeats frames and the image
  judders.
- **Use OFXR's FPS counter.** Other counters (fpsVR, SteamVR frame timing,
  in-game) can show about half of what reaches the headset.

### SteamVR swapchain limits

Some combinations, particularly Luke Ross AFW and UEVR Synced Sequential, can approach SteamVR's OpenXR swapchain limit. The number used depends on the game, mod, route and enabled overlays; Luke Ross does not universally require exactly 16 swapchains.

If allocation fails, disable unnecessary **OpenXR overlays** and inspect the swapchain details in the **Luke Ross** tab where available. Compatible Luke Ross outputs share final native swapchains by default. This startup option is INI-only: [ofxr] luke_shared_swapchains=0 disables it after restarting the game. fpsVR uses a SteamVR overlay rather than an additional OpenXR swapchain.

Other graphics injectors can also affect compatibility and resource use. When troubleshooting, first test with only the components required for the game.

### Reading the meters

The **information overlay** (the default, at the bottom of your view) and the
**FPS counter** (in a corner, if you choose it instead) show whether OFXR is
working:

| In the headset | It means |
| --- | --- |
| No overlay or counter at all | OFXR is not loaded in the game (see [Installation](#installation-and-use)) |
| **Information overlay**, numbers in **green** | OFXR is working |
| **Information overlay**, numbers in **red** | OFXR is loaded, but not generating frames, or a stage you enabled (Luke Ross reconstruction, Neural Rendering) is not running. The status in the top right corner says what runs, for example **OSR**, **Waiting** or **Paused**. |
| A **green** FPS counter | OFXR is generating frames |
| A **blue** FPS counter | Luke Ross reconstruction (OSR or AFW) is producing the frames |
| A **red** FPS counter | OFXR is loaded, but not generating or reconstructing frames |

**Generation**, **Delivery** and **Cycle minimum** describe the bridge's processing and delivery path. Cycle minimum covers the observed bridge/runtime delivery cycle; it is not complete input-to-photon latency.

## GPU requirements

The NVIDIA optical-flow routes require a Turing-generation GPU or newer with Optical Flow hardware support and a compatible driver. RTX 20/30/40/50-series and GTX 1660-family cards have this hardware. TU117-based cards, including some GTX 1650 variants, do not. Pascal cards such as the GTX 10 series are not supported by these routes. See NVIDIA's [Optical Flow hardware requirements](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html).

FidelityFX Optical Flow is the alternative for GPUs without NVIDIA Optical Flow hardware.

In the current integration, the vector-based routes and Luke Ross OSR/AFW obtain their guides from **DLSS Super Resolution or Ray Reconstruction**. They require compatible DLSS inputs; selecting FSR Frame Generation here does not mean that the game must use FSR upscaling.

Native NVIDIA DLSS Frame Generation additionally requires supported hardware, DLLs and drivers. NVIDIA lists Frame Generation for RTX 40/50-series cards and native Multi Frame Generation for RTX 50-series cards in its [DLSS feature overview](https://www.nvidia.com/en-gb/geforce/news/nvidia-app-update-dlss-overrides-and-more/). The Djules route's 3X option is a separate implementation.

## Compatibility

- Tested extensively with Quest 3 + Virtual Desktop using VDXR, and Sboys3 with SteamVR using MeganeX and Crystal Super.
- Support for UEVR, REFramework-based mods and OpenComposite.
- Support for the Luke Ross framework, with OSR and AFW (legacy) available for selected titles.
- Limited testing with native DirectX 11 and DirectX 12 games.
- Preliminary support for Vulkan games.

Other configurations may work, but have not received the same testing.

> [!WARNING]
> High-resolution rendering and Luke Ross AFW can require substantial VRAM. A 12 GB GPU may be insufficient for some combinations of game settings, headset resolution and enabled features. There is no single VRAM requirement for every game.

## Luke Ross R.E.A.L. VR games

Luke Ross's Legacy AER mode renders one eye per frame, alternating. OFXR
rebuilds the other eye from the colour, depth and camera of the eye the game
rendered, so the headset gets a full stereo pair every frame.

**Reconstruction** has three choices:

| Reconstruction | |
| --- | --- |
| **Off** | Disables stereo reconstruction; frame generation and Neural Rendering remain independent. |
| **OSR** (default) | OFXR's own reconstruction. Less ghosting and a lower GPU cost than AFW. |
| **AFW** | PureDark's AFW provider, with its quality controls (below). |

Changing it applies while the game runs. Profiles from 0.3.1 start on OSR.
The green status reports **OSR: Off**, **OSR: Luke Ross**, or **OSR: Other**
from actual runtime activity. Other identifies a compatible mod without a Luke
Ross marker; it is a valid state. The legacy engine uses the AFW label.
Off is controlled here, rather than in the advanced Framewarp mode menus.

**Luke Ross setup:**

1. Use an up-to-date Luke Ross mod (tested with v2609.18.2 and newer).
2. In the REAL VR menu, select **Legacy AER**. AER V2 and AER 1/2 don't work
   with OFXR's reconstruction.
3. In the REAL VR menu, turn off **Camera rotation compensation** if it is
   offered. Left on, turning in vehicles looks stepped.
4. Turn on DLSS in the game.
5. Optional: **Frame Reconstruction only (AFW or OSR, restart game)** sends the
   reconstructed pairs straight to the headset, without OFXR-generated frames
   (it was called "AFW Only" in 0.3.1). Use it when the game reaches your
   refresh rate on its own: lower latency, fewer artifacts. Otherwise leave it
   off so OFXR generates frames too. Neural Rendering still works. Restart the
   game after changing it.

<details>
<summary>AFW (legacy) quality controls</summary>

When **AFW** is selected, use **Advanced parameters / Framewarp mode / Combined** for the normal AFW path.

- Start with AFW **1x1**. At high resolutions, try **2x2** if AFW costs too much performance. **4x4** is a stronger quality compromise and is generally intended for very high resolutions.
- **Depth dilation** can reduce ghosting, flicker and shimmering, with less precise object silhouettes.
- **Ignore motion** controls the threshold for ignoring per-object motion vectors. It is not a general flicker-reduction control. For disappearing fine details during rotation, a higher value may help; around **10** has been useful for character hair in Star Wars Outlaws with AFW.

These AFW-specific quality controls are hidden when OSR is selected. AFW quality changes apply while the game is running; the bridge rebuilds the affected history when needed.

</details>

### Luke Ross games and HUD observations

The table retains the HUD observations from 0.3.1 and adds the newly confirmed results. Empty cells are awaiting verification.
HUD or overlay icons may double when moving, strafing or rotating with gamepad

| Game | HUD observations |
| --- | --- |
| Avatar: Frontiers of Pandora | |
| Crimson Desert | HUD correct; overlay icons double |
| Cyberpunk 2077 | |
| Dead Space (2023) | HUD double; overlay icons correct |
| DOOM: The Dark Ages | HUD double |
| Ghost of Tsushima | HUD double |
| Grand Theft Auto V Enhanced | HUD correct |
| Horizon Forbidden West | HUD double |
| Indiana Jones and the Great Circle | HUD double |
| Kingdom Come: Deliverance II | HUD correct |
| Marvel's Spider-Man Remastered | HUD correct |
| Marvel's Spider-Man: Miles Morales | HUD correct |
| Star Wars Outlaws | HUD double |
| The Last of Us Part I | |
| Uncharted: Legacy of Thieves Collection | |

These are observations from tested builds, not guarantees for every configuration. Other titles may work, but can require additional capture support or fixes.

### OpenXR panels and OpenKneeboard

Version 0.3.2 can interpolate compatible OpenXR quad-panel images and poses separately from the main eye images. This includes panels using separate textures and panels sharing a texture atlas. It can help external overlays such as OpenKneeboard remain consistent with generated output frames.

This applies to OpenXR panel layers. HUD elements already drawn into a game's eye image are part of that image; their behaviour is covered by the per-game HUD observations above. SteamVR overlays follow their own overlay path.

## Star Citizen

The official release's OFXR files (the tray, the OpenXR and Vulkan layers,
and the NGX forwarders) are digitally signed, so Easy Anti-Cheat lets OFXR load in Star
Citizen. Tested with SteamVR and VDXR: optical flow, Hybrid and DLSS-G work.
Enable the tray before starting the game.

Only the official release archive is signed: a build you compile yourself is
not, and Easy Anti-Cheat refuses it.

> [!CAUTION]
> OFXR is an independent project, not endorsed by Cloud Imperium Games or Easy
> Anti-Cheat. Using it with an anti-cheat protected game is at your own risk:
> we accept no responsibility for any account action, ban or other
> consequence. Check the game's terms of service before you play.

## Assetto Corsa

An optional modified OptiScaler add-on provides DLSS upscaling, OFXR and optional DLSS 5 Neural Rendering for Assetto Corsa in VR. It is distributed separately from the standalone bridge.

1. Install an appropriate CSP version. Testing used **0.3.0-preview675**.
2. Disable MSAA under **Assetto Corsa / Video / Quality**.
3. Under **CSP / Extra FX**, enable **Allow in VR**.
4. Under CSP's upscaling settings, select the **OptiScaler** method and use **DLSS** as the base upscaler to intercept. Choose the desired DLSS quality.
5. Run the game once so CSP installs its OptiScaler integration, then close the game.
6. Extract the add-on archive and run its **`Install-Assetto.ps1`** installer, pointing it at the game folder containing **`acs.exe`**. Follow the add-on README. The installer backs up affected files and installs the modified DLL in the active CSP OptiScaler cache.
7. **Optional:** for DLSS 5 Neural Rendering, supply your own **`nvngx_dlssnr.dll`** beside `acs.exe`. Keep the OFXR tray enabled when launching the game.

Keep CSP's method set to **OptiScaler**. The add-on includes the required DLSS SR/FG runtime files; the DLSS 5 Neural Rendering model is not included. Do not place the modified `version.dll` beside `acs.exe`: it belongs in the CSP OptiScaler cache selected by the installer.

## Finding the best settings for each game

The best configuration depends on the game, mod, headset, GPU and runtime. Compare settings on your own system rather than assuming one route will always win.

### Per-game profiles

OFXR-BRIDGE stores shared data under **`%LOCALAPPDATA%\ofxr-bridge`**. Per-game profiles are stored in **`Profiles`**, using the game's executable name.

When a game has no profile, the bridge creates one from the current main **`ofxr-bridge.ini`** settings. Changes made in-game are saved to that game's profile. Desktop edits to the main configuration set the starting point for new profiles.

**Reset to defaults** restores the built-in factory defaults for the selected configuration; it does not copy your customized main INI. Tray options remain global.

### Pacing

If performance is lower than expected or motion looks uneven, **change Pacing
first**: it is often the setting that makes the biggest difference.

- **Settings / Pacing** (SteamVR path): **Director** (default) or
  **Framework**. It applies while you play, so compare both in the same
  scene; Framework can be better. With VDXR, OFXR always
  uses Framework pacing and the choice is unavailable.
- **Tray / Pacing (restart game)** selects the transport path. **Auto**
  (recommended) uses the VDXR path with VDXR and the SteamVR path otherwise.
  **Force VDXR** and **Force SteamVR** are for other OpenXR runtimes not yet
  tested. Restart the game after changing it.

### Frameworks

| Framework | Starting considerations |
| --- | --- |
| FidelityFX | Optical-flow alternative for non-NVIDIA GPUs; image quality can be lower. |
| OFXR 0.2.1 | Latency-oriented route; performance can vary substantially by configuration. |
| OFXR 0.2.14 | Djules-based route, including a 3X option. |
| OFXR 0.3.0 | Default NVIDIA route; compare its performance and latency in each game. |
| RIFE (nightly) | NVIDIA CUDA neural interpolation; model 4.4 has optional TensorRT acceleration. |
| Hybrid | Combines FidelityFX with captured motion vectors. |
| FSR 3.1 | FSR frame generation using the captured guides. |
| DLSS-G | NVIDIA frame generation; requires compatible hardware and can have a higher cost. |

Vector-based routes depend on valid captured guides. Ordinary optical-flow OFXR does not require Luke Ross reconstruction to be active.

**DLSS-G preset (next launch)** selects the requested NVIDIA model preset. **A** is Full/quality and is the default request; **C** is Lite/faster. Restart the game after changing it. If the driver rejects C, the bridge attempts A. A saved driver-profile request does not by itself prove which model the driver ultimately loaded; global driver overrides can also apply. Benchmarks use the selected preset rather than forcing C.

### Fast mode

**Fast mode (lower vram, lower latency, may reduce performance)** uses private synthesis textures and a reduced swapchain allocation where supported. It can reduce VRAM use or latency, but its performance effect varies by game and route. 3X retains the capacity needed for its additional output frames, so its memory savings can differ.

Fast mode has **no effect on the Luke Ross reconstruction path**. For other paths, benchmark both settings rather than assuming that it always helps or always hurts VDXR.

### Benchmarks

**Enable benchmarks** is on by default in the tray, making the benchmark controls available under **Display**. Choose the optical-flow, complete or Luke Ross suite, then remain in the same representative scene throughout the run. **Auto pick best one** is enabled by default; no benchmark starts automatically.

- Each test lasts **7.5 seconds**: **2.5 seconds** settling, then **5 seconds** measured.
- Diagnostic logging is not required. Benchmark CSV output is automatic.
- Results include FPS and Cycle minimum statistics. VRAM use is sampled near the end of each test.
- Results are saved under **`%LOCALAPPDATA%\ofxr-bridge\Benchmarks`**. Use **Open folder** in the tray, then open **Benchmarks**.
- Suite length adapts to measured results and headset resolution. Skipped tests remain n.a. in the CSV.
- At completion, the suite restores the previous settings unless **Auto pick best one** is enabled. Stopping restores them too; manually changing a tested setting cancels the automation and keeps that edit.

Benchmarks change the parameters covered by the selected suite. Other quality settings, such as your chosen flow resolution, are retained.

<details>
<summary>Example configurations (earlier measurements)</summary>

These are earlier measurements, retained for reference. Rows labelled AFW or AFW only were measured with the AFW provider; they are not OSR performance measurements. The old AFW only label corresponds to **Frame Reconstruction only** with **AFW (legacy)** selected.

Test system: Intel 265K and RTX 4090. Quest 3 uses VDXR; MeganeX uses SteamVR. Latency is shown only for runs measured with the current Cycle minimum meter, in milliseconds. **+ Buffer** means Increase buffer is enabled.

| Game | Quest 3 - Godlike / VDXR - top 2 | MeganeX - sboys3 / SteamVR - top 3 |
| --- | --- | --- |
| Assassin's Creed Valhalla | 1. 0.3.0 Fast 70 fps<br>2. 0.3.0 71 fps | 1. Director 0.2.13 Fast + Buffer 75 fps 65 ms<br>2. Framework 0.3.0 Fast + Buffer 74 fps 62 ms<br>3. Framework 0.2.1 Fast 73 fps 62 ms |
| Crimson Desert (RT, no RR) | 1. 0.2.13 AFW 1x1 81 fps<br>2. 0.3.0 AFW 1x1 82 fps | 1. Director 0.3.0 + AFW 1x1 84 fps 67 ms<br>2. Director 0.2.13 + Buffer + AFW 1x1 85 fps 68 ms<br>3. Framework 0.2.1 + AFW 1x1 74 fps 39 ms |
| Doom The Dark Ages | 1. 0.3.0 AFW 1x1 90 fps<br>2. 0.2.13 AFW 1x1 90 fps | 1. AFW only 2x2 73 fps 20 ms<br>2. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>3. Framework 0.3.0 + AFW 1x1 90 fps 56 ms |
| Ghost of Tsushima | 1. AFW only 2x2 74 fps<br>2. 0.2.1 AFW 1x1 90 fps | 1. AFW only 2x2 86 fps 19 ms<br>2. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>3. Framework 0.2.13 + Buffer + AFW 1x1 90 fps 56 ms |
| Ghost of Tsushima (dlss5) | 1. 0.2.13 AFW 1x1 84 fps<br>2. 0.3.0 AFW 1x1 84 fps | 1. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>2. Framework 0.2.13 + AFW 1x1 90 fps 56 ms<br>3. Framework 0.3.0 + AFW 1x1 90 fps 56 ms |
| Ghostwire Tokyo (Native + dlss5) | 1. Hybrid 77 fps<br>2. Hybrid Fast 77 fps | 1. Framework Hybrid + Buffer 71 fps 47 ms<br>2. Framework 0.3.0 Fast + Buffer 74 fps 52 ms<br>3. Framework 0.2.13 Fast + Buffer 72 fps 52 ms |
| Ghostwire Tokyo (Native) | 1. Hybrid Fast 90 fps<br>2. FSR 90 fps | 1. Director Hybrid 90 fps 35 ms<br>2. Framework FSR Fast + Buffer 90 fps 46 ms<br>3. Director 0.2.1 90 fps 36 ms |
| High on Life 2 (Native Joe) | 1. Hybrid 90 fps<br>2. Hybrid Fast 90 fps | 1. Framework Hybrid Fast + Buffer 90 fps 40 ms<br>2. Framework 0.3.0 Fast + Buffer 90 fps 41 ms<br>3. Framework 0.2.1 + Buffer 90 fps 41 ms |
| Horizon Forbidden West | 1. 0.3.0 AFW 1x1 82 fps<br>2. 0.2.13 AFW 1x1 82 fps | 1. AFW only 2x2 73 fps 18 ms<br>2. Director 0.3.0 + AFW 1x1 90 fps 67 ms<br>3. Director 0.2.13 + AFW 1x1 90 fps 67 ms |
| Indiana Jones and the Great Circle | 1. 0.2.13 AFW 1x1 90 fps<br>2. 0.3.0 AFW 1x1 90 fps | 1. AFW only 1x1 78 fps 21 ms<br>2. Director 0.2.1 + Buffer + AFW 1x1 90 fps 46 ms<br>3. Framework 0.3.0 + AFW 1x1 90 fps 56 ms |
| Kingdom Come Deliverance II | 1. AFW only 1x1 87 fps<br>2. AFW only 2x2 90 fps | AFW only 1x1 81 fps<br>AFW only 2x2 90 fps |
| Kingdom Come Deliverance II (dlss5) | 1. 0.2.13 AFW 1x1 90 fps<br>2. 0.3.0 AFW 1x1 90 fps | 1. Framework 0.2.13 + AFW 1x1 90 fps 56 ms<br>2. Framework 0.3.0 + Buffer + AFW 1x1 90 fps 56 ms<br>3. Director 0.2.1 + Buffer + AFW 1x1 88 fps 46 ms |
| MSFS2024 (fly, VR ultra, foveated) | 1. 0.2.13 Fast 89 fps 63 ms<br>2. 0.3.0 87 fps 45 ms | 1. Framework 0.3.0 85 fps 83 ms<br>2. Framework 0.2.13 85 fps 84 ms<br>3. Framework 0.2.1 Fast 83 fps 77 ms |
| MSFS2024 (ground, VR very high) | 1. Hybrid Fast 90 fps 57 ms<br>2. FSR 88 fps 40 ms | 1. Framework Hybrid Fast 90 fps 74 ms<br>2. Framework 0.3.0 Fast 90 fps 74 ms<br>3. Framework 0.2.1 Fast + Buffer 90 fps 74 ms |
| No man's sky VR | 1. 0.2.1 90 fps<br>2. 0.2.1 Fast 90 fps | 1. Director 0.3.0 Fast + Buffer 90 fps 55 ms<br>2. Director 0.2.13 Fast + Buffer 90 fps 55 ms<br>3. Framework 0.2.1 90 fps 65 ms |
| Nobody Wants to Die (Native) | 1. Hybrid 75 fps<br>2. 0.2.13 Fast 76 fps | 1. Director 0.3.0 Fast + Buffer 69 fps 59 ms<br>2. Framework 0.2.13 Fast 69 fps 58 ms<br>3. Director Hybrid 65 fps 54 ms |
| Returnal (syncseq) | 1. Hybrid 90 fps<br>2. Hybrid Fast 90 fps | 1. Framework Hybrid Fast 90 fps 51 ms<br>2. Framework FSR Fast 89 fps 52 ms<br>3. Framework 0.3.0 Fast 90 fps 54 ms |
| Returnal (syncseq+dlss5) | 1. 0.2.1 Fast 74 fps<br>2. 0.2.1 72 fps | 1. Director 0.2.13 70 fps 66 ms<br>2. Framework 0.3.0 Fast 69 fps 63 ms<br>3. Director Hybrid Fast 66 fps 55 ms |
| Skyrim VR | 1. 0.2.13 Fast 90 fps<br>2. 0.3.0 Fast 90 fps | 1. Framework 0.3.0 Fast 90 fps 46 ms<br>2. Framework 0.2.13 Fast + Buffer 90 fps 45 ms<br>3. Framework 0.2.1 90 fps 55 ms |
| Star Wars Outlaws (RT+RR) | 1. 0.3.0 AFW 1x1 90 fps<br>2. 0.2.13 AFW 1x1 90 fps | 1. Director 0.2.1 + Buffer + AFW 1x1 90 fps 46 ms<br>2. Framework 0.2.13 + Buffer + AFW 1x1 90 fps 56 ms<br>3. Framework 0.3.0 + Buffer + AFW 1x1 90 fps 56 ms |
| Stellar Blade (Native) | 1. FSR 90 fps<br>2. FSR Fast 90 fps | 1. Director Hybrid 90 fps 44 ms<br>2. Director 0.2.1 Fast 90 fps 37 ms<br>3. Framework FSR 90 fps 54 ms |
| The Callisto Protocol (Native) | 1. 0.2.13 89 fps<br>2. 0.3.0 89 fps | 1. Framework 0.2.13 + Buffer 87 fps 67 ms<br>2. Framework 0.3.0 86 fps 65 ms<br>3. Framework 0.2.1 + Buffer 83 fps 55 ms |
| The Planet Crafter | 1. 0.2.1 90 fps<br>2. 0.2.1 Fast 90 fps | 1. Framework 0.2.13 Fast 90 fps 52 ms<br>2. Framework 0.3.0 Fast 90 fps 52 ms<br>3. Framework 0.2.1 Fast 90 fps 54 ms |

</details>

### Other tools: status.json

**Export status.json** in **Settings / Display** (off by default) writes OFXR's
live state once a second: whether OFXR is enabled or paused, the multiplier, the
game and headset frame rates, and the latency.
[MSFS AutoFPS](https://github.com/ResetXPDR/MSFS_AutoFPS) reads it to keep the
simulator at its target frame rate. File format:
[status.json](https://github.com/tig3rmast3r/OFXR-Bridge/blob/main/docs/STATUS_JSON.md).

## What the tray changes on your PC

When you select **Enable**, the tray prepares a versioned runtime cache under **`%LOCALAPPDATA%\ofxr-bridge\RuntimeLayer`** and registers the bridge's OpenXR implicit-layer manifest for the current Windows user. It also manages its Vulkan layer registration when that support is enabled. The OpenXR loader loads the bridge DLL into participating game processes. Your active OpenXR runtime is unchanged. The official release's OFXR files are digitally signed (see [Star Citizen](#star-citizen)).

The standalone bridge does not replace game files. The optional Assetto Corsa add-on has its own installation procedure and does modify the documented game/CSP files.

**Disable** removes the registrations owned by the tray and signals active bridge instances to stop. Exiting the tray also disarms it, and a watchdog attempts cleanup after an unexpected tray exit. Disabling does not unload a DLL from an already running game: close the game to finish that session.

Profiles, benchmarks, cached runtime files and logs remain in the common folder. Use **Open folder** in the tray to inspect them. Avoid deleting files while the game or tray is using them.

Selecting a DLSS-G preset can update the game's NVIDIA driver profile. **Crash debug** also configures crash diagnostics, including DRED, for subsequent launches. These are separate from enabling the ordinary OpenXR layer.

## Known issues

- OFXR 0.2.14 3X is not supported in Luke Ross games.
- HUD behaviour varies by Luke Ross game and reconstruction choice; see the HUD observations table.
- **Cheeky Foveated DLSS in Luke Ross games** needs a fix on the Cheeky Foveated DLSS side.
- OSR reconstructs areas missing from the current source eye using temporal history and hole filling. Fast-changing objects and newly revealed regions can still show reconstruction artifacts.
- DLSS 5 Neural Rendering can cause instability or device-hung crashes in some games.
- Extra OpenXR layers or overlays can exhaust the available swapchain budget.
- Vulkan support remains experimental.

## Reporting problems

Please report working and non-working games, rendering problems, freezes and crashes in [GitHub Issues](https://github.com/tig3rmast3r/OFXR-Bridge/issues) or on the [Flat2VR Modding Discord](https://discord.gg/flat2vr).

To collect a log:

1. Enable **Diagnostic logging** in Settings, or use **F9**. If the failure happens at startup, enable it before launching the game.
2. Reproduce the problem once.
3. Close the game to flush its log to disk.
4. Select **Open folder** from the tray and locate the affected build's logs under **`RuntimeLayer`**.
5. Attach the relevant `ofxr-bridge-flight-*.log` file to the report.

For sporadic crashes, enable **Crash debug (restart game)** in the tray before the next launch. It prepares additional crash diagnostics, including DRED. Leave it off for ordinary play when those diagnostics are not needed.

Please include:

- Game name and version.
- VR mod or injector, if any.
- Headset and active OpenXR runtime.
- GPU, driver version and Windows version/build.
- OFXR-BRIDGE build number, selected framework, pacing and relevant options.
- Steps to reproduce and the observed result.

A comparison with a working game or older build is useful when available, but is not required to report a problem. If no log was created, mention that too; the layer may not have loaded, or the process may have stopped before logging started.

Diagnostic logs are independent of game/mod logging. They record bridge activity rather than video and do not replace a native crash dump.

## Building from source

See the [Windows build instructions](https://github.com/tig3rmast3r/OFXR-Bridge/blob/main/docs/BUILDING.md).

For the nightly RIFE runtime and complete release packaging, also see
[Building and packaging RIFE](docs/BUILDING_RIFE.md).

## License

The combined standalone distribution is licensed under [GPL-3.0](https://github.com/tig3rmast3r/OFXR-Bridge/blob/main/LICENSE). Original components and third-party libraries retain their respective notices and licenses; proprietary providers have their own terms. See [attribution and third-party notices](https://github.com/tig3rmast3r/OFXR-Bridge/blob/main/licenses/ATTRIBUTION.md).

## Credits

- **Djules:** improvements to the 0.2.x route, 3X frame generation, SteamVR behaviour, Vulkan support, expanded Luke Ross guide/camera handling and OSR (OFXR Stereo Reconstruction).
- **Elliottate:** hybrid frame generation, DLSS-G integration and the on-screen information meter.
- **PureDark:** AFW provider.
- **OptiScaler and RenoDX contributors:** components used by the capture and integration paths.
- **Khronos, NVIDIA, AMD and Dear ImGui contributors:** APIs, SDKs and libraries used by the project.

See the attribution file for component details and licensing notices.

## Support

If you find OFXR-BRIDGE useful and want to support its development, you can support the project on Ko-fi.
[Tig3rmast3r](https://ko-fi.com/tig3rmast3r)
[Djules](https://ko-fi.com/djules)
