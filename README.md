# OFXR-BRIDGE

OFXR-BRIDGE is an experimental OpenXR API layer that inserts generated frames between rendered frames.

Current beta release: **0.3.1**. The internal build number is shown in the package name and the application.

Download builds from [GitHub Releases](https://github.com/tig3rmast3r/OFXR-Bridge/releases).

> [!WARNING]
> This is a beta release. It may not work with your game, VR mod, GPU or OpenXR runtime. It may produce visual artifacts, fail to activate, freeze the game or cause a crash. Use it at your own risk.

## Features

This build includes work from both the Djules and Elliottate forks, together with additional integrations:

- AMD FidelityFX Optical Flow.
- NVIDIA Optical Flow with Fast, Medium and Slow presets.
- 100%, 75%, 50% and 25% NVIDIA optical-flow calculation scales.
- Hybrid FidelityFX + motion vectors.
- FSR 3.1 Frame Generation.
- DLSS Frame Generation.
- AFW support for selected Luke Ross games.
- Optional DLSS 5 Neural Rendering, applied **before Super Resolution (pre-SR)**, with a separately supplied model DLL.
- Per-game settings and automated benchmarks.
- An optional in-headset FPS counter with four corner positions.
- An optional information overlay that can be attached to a motion controller or placed at the bottom of the view.

The default configuration is **NVIDIA OFXR 0.3.0, Medium, 50% flow resolution**. Flow resolution controls motion estimation; it does not reduce the final output resolution.

## GPU requirements

The NVIDIA optical-flow routes require a Turing-generation GPU or newer with Optical Flow hardware support and a compatible driver. RTX 20/30/40/50-series and GTX 1660-family cards have this hardware. TU117-based cards, including some GTX 1650 variants, do not. Pascal cards such as the GTX 10 series are not supported by these routes. See NVIDIA's [Optical Flow hardware requirements](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note/index.html).

FidelityFX Optical Flow is the alternative for GPUs without NVIDIA Optical Flow hardware.

In the current integration, the vector-based routes and AFW obtain their guides from **DLSS Super Resolution or Ray Reconstruction**. They require compatible DLSS inputs; selecting FSR Frame Generation here does not mean that the game must use FSR upscaling.

Native NVIDIA DLSS Frame Generation additionally requires supported hardware, DLLs and drivers. NVIDIA lists Frame Generation for RTX 40/50-series cards and native Multi Frame Generation for RTX 50-series cards in its [DLSS feature overview](https://www.nvidia.com/en-gb/geforce/news/nvidia-app-update-dlss-overrides-and-more/). The Djules route's 3X option is a separate implementation.

## Compatibility

- Tested extensively with Quest 3 + Virtual Desktop using VDXR, and Sboys3 with SteamVR using MeganeX and Crystal Super.
- Support for UEVR, REFramework-based mods and OpenComposite.
- Support for the Luke Ross framework, with AFW available for selected titles.
- Limited testing with native DirectX 11 and DirectX 12 games.
- Preliminary support for Vulkan games.

Other configurations may work, but have not received the same testing.

> [!WARNING]
> High-resolution rendering and Luke Ross AFW can require substantial VRAM. A 12 GB GPU may be insufficient for some combinations of game settings, headset resolution and enabled features. There is no single VRAM requirement for every game.

## Installation and use

1. Download the latest pre-release archive from GitHub Releases.
2. Extract the complete archive to a writable folder.
3. **Optional, for DLSS 5 Neural Rendering only:** place your own `nvngx_dlssnr.dll` model in the extracted `ofxr` folder. The model is not included and is not required for ordinary frame generation or AFW. Do not confuse it with the included `nvngx.dll_dlssnr.dll` forwarder.
4. Run `OFXRBridgeTray.exe` and select **Enable**.
5. Open **Settings...** from the tray, or press **F10** in-game.
6. Start the game normally. For injectors such as UEVR, enable OFXR-BRIDGE before starting the game and leave it enabled while the VR mod is injected.
7. When finished, close the game and select **Disable**, or exit the tray application.

The tray and bridge require the [Microsoft Visual C++ Redistributable x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist). OFXR-BRIDGE uses your active OpenXR runtime.

For supported NVIDIA GPUs, start with **OFXR 0.3.0 / Medium / 50% flow resolution**. Running optical flow at 100% can be expensive; compare its image quality and performance before keeping it enabled.

### SteamVR swapchain limits

Some combinations, particularly Luke Ross AFW and UEVR Synced Sequential, can approach SteamVR's OpenXR swapchain limit. The number used depends on the game, mod, route and enabled overlays; Luke Ross does not universally require exactly 16 swapchains.

If allocation fails, disable unnecessary **OpenXR overlays** and inspect the swapchain details in the **Luke Ross** tab where available. Compatible Luke Ross outputs share final native swapchains by default. This startup option is INI-only: [ofxr] luke_shared_swapchains=0 disables it after restarting the game. fpsVR uses a SteamVR overlay rather than an additional OpenXR swapchain.

Other graphics injectors can also affect compatibility and resource use. When troubleshooting, first test with only the components required for the game.

### Reading the meters

Some external FPS counters measure application frames before OFXR-BRIDGE. During frame generation, they may show roughly half the bridge's output rate.

The bridge distinguishes real frames, generated frames and output delivery. Producing a frame does not guarantee that it is delivered as a distinct output frame. The output meter measures delivery as observed by the bridge/runtime path; it is not a direct measurement of the headset panel's scanout.

**Generation**, **Delivery** and **Cycle minimum** describe the bridge's processing and delivery path. Cycle minimum covers the observed bridge/runtime delivery cycle; it is not complete input-to-photon latency.

## Luke Ross

AFW reconstructs the missing eye in selected Luke Ross games using depth and motion information. Compared with AER V2, enabling AFW can improve FPS and reduce latency, as well as improve image quality and smoothness. The size of the gain depends on the game, resolution and settings.

Version 0.3.1 adds Luke Ross AFW support for Spider-Man Remastered, Miles Morales, Dead Space (2023), and Grand Theft Auto V Enhanced.

> [!IMPORTANT]
> Use **Legacy AER**, not AER V2 or AER 1/2. These modes are not interchangeable for this AFW integration.

For initial setup:

1. Use an updated Luke Ross framework; testing for this integration used v2609.18.2 or newer.
2. Select **Legacy AER** in REAL VR.
3. Enable DLSS in the game and select **Combined** AFW in OFXR-BRIDGE.
4. Start with AFW **1x1**. At high resolutions, try **2x2** if AFW costs too much performance. **4x4** is a stronger quality compromise and is generally intended for very high resolutions.
5. If needed, adjust **Depth dilation** to reduce ghosting, flicker and shimmering. The trade-off is less precise object silhouettes.
6. **Ignore motion** controls the threshold for ignoring per-object motion vectors. It is not a general flicker-reduction control. For disappearing fine details during rotation, a higher value may help; around **10** has been useful for character hair in Star Wars Outlaws.

AFW quality changes can be applied while the game is running. The bridge rebuilds the affected AFW history when needed.

Reaching the headset refresh rate generally gives a smoother result, but does not eliminate HUD artifacts. If AFW alone cannot reach it, try pairing AFW with OFXR. In my tests, AFW alone at full refresh usually looks smoother than combining both, even when the combined output counter reaches the same rate.

### HUD limitations and tested titles

Many Luke Ross games have HUD doubling during movement or rotation with AFW. Its severity varies by title and scene. If it is distracting, disable AFW for that game.

| Game | HUD observations |
| --- | --- |
| Crimson Desert | HUD correct; overlay icons double. |
| DOOM: The Dark Ages | HUD doubling when moving, rotating, or strafing. |
| Ghost of Tsushima | HUD doubling when moving, rotating, or strafing. |
| Horizon Forbidden West | HUD doubling when moving, rotating, or strafing. |
| Indiana Jones and the Great Circle | HUD doubling when moving, rotating, or strafing. |
| Kingdom Come: Deliverance II | HUD correct |
| Star Wars Outlaws | HUD doubling when moving, rotating, or strafing. |

These are observations from tested builds, not guarantees for every configuration. Other titles may work, but can require additional capture support or fixes.

### AFW Only

**AFW Only (restart game)** is in the **Luke Ross** tab. It bypasses the OFXR generation route and uses direct AFW delivery. Enabling it reduces overhead for higher FPS and lower latency when using AFW without additional frame generation. DLSS 5 Neural Rendering remains available. Restart the game after changing this option.

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

**Reset to defaults** restores the built-in factory defaults for the selected configuration; it does not copy your customized main INI. Tray options remain global. **AFW Only** is a per-game setting in the Luke Ross tab.

### Pacing

There are two different controls:

- **Tray / Pacing (restart game)** selects the transport path. **Auto** is the recommended setting: it uses the VDXR path with VDXR and the SteamVR path otherwise. **Force VDXR** and **Force SteamVR** are compatibility overrides for other OpenXR runtimes that have not yet been tested. Restart the game after changing it.
- **Settings / Pacing** selects scheduling on the SteamVR-prepared path. **Framework** uses framework pacing with the director in bypass. **Director** delegates pacing to the common director.

The native VDXR path uses its existing framework behavior, so the scheduling dropdown is unavailable there.

### Frameworks

| Framework | Starting considerations |
| --- | --- |
| FidelityFX | Optical-flow alternative for non-NVIDIA GPUs; image quality can be lower. |
| OFXR 0.2.1 | Latency-oriented route; performance can vary substantially by configuration. |
| OFXR 0.2.13 | Djules-based route, including a 3X option. |
| OFXR 0.3.0 | Default NVIDIA route; compare its performance and latency in each game. |
| Hybrid | Combines FidelityFX with captured motion vectors. |
| FSR 3.1 | FSR frame generation using the captured guides. |
| DLSS-G | NVIDIA frame generation; requires compatible hardware and can have a higher cost. |

Vector-based routes depend on valid captured guides. Ordinary optical-flow OFXR does not require AFW to be active.

**DLSS-G preset (next launch)** selects the requested NVIDIA model preset. **A** is Full/quality and is the default request; **C** is Lite/faster. Restart the game after changing it. If the driver rejects C, the bridge attempts A. A saved driver-profile request does not by itself prove which model the driver ultimately loaded; global driver overrides can also apply. Benchmarks use the selected preset rather than forcing C.

### Fast mode

**Fast mode (lower vram, lower latency, may reduce performance)** uses private synthesis textures and a reduced swapchain allocation where supported. It can reduce VRAM use or latency, but its performance effect varies by game and route. 3X retains the capacity needed for its additional output frames, so its memory savings can differ.

Fast mode has **no effect on the Luke Ross path**. For other paths, benchmark both settings rather than assuming that it always helps or always hurts VDXR.

### Benchmarks

**Enable benchmarks** is on by default in the tray, making the benchmark controls available under **Display**. Choose the optical-flow, complete or Luke Ross suite, then remain in the same representative scene throughout the run. **Auto pick best one** is enabled by default; no benchmark starts automatically.

- Each test lasts **7.5 seconds**: **2.5 seconds** settling, then **5 seconds** measured.
- Diagnostic logging is not required. Benchmark CSV output is automatic.
- Results include FPS and Cycle minimum statistics. VRAM use is sampled near the end of each test.
- Results are saved under **`%LOCALAPPDATA%\ofxr-bridge\Benchmarks`**. Use **Open folder** in the tray, then open **Benchmarks**.
- Luke Ross tests compare suitable AFW qualities for the headset resolution, including AFW alone. They do not test Fast mode or 3X.
- Suite length adapts to measured results and headset resolution. Skipped tests remain n.a. in the CSV.
- At completion, the suite restores the previous settings unless **Auto pick best one** is enabled. Stopping restores them too; manually changing a tested setting cancels the automation and keeps that edit.

Benchmarks change the parameters covered by the selected suite. Other quality settings, such as your chosen flow resolution, are retained.

### Example configurations

Test system: Intel 265K and RTX 4090. Quest 3 uses VDXR; MeganeX uses SteamVR. Latency is shown only for runs measured with the current Cycle minimum meter, in milliseconds. **+ Buffer** means Increase buffer is enabled.

| Game | Quest 3 - Godlike / VDXR - top 2 | MeganeX - sboys3 / SteamVR - top 3 |
| --- | --- | --- |
| Assassin's Creed Valhalla | 1. 0.3.0 Fast 70 fps<br>2. 0.3.0 71 fps | 1. Director 0.2.13 Fast + Buffer 75 fps 65 ms<br>2. Framework 0.3.0 Fast + Buffer 74 fps 62 ms<br>3. Framework 0.2.1 Fast 73 fps 62 ms |
| Crimson Desert (RT, no RR) | 1. 0.2.13 AFW 1x1 81 fps<br>2. 0.3.0 AFW 1x1 82 fps | 1. Director 0.3.0 + AFW 1x1 84 fps 67 ms<br>2. Director 0.2.13 + Buffer + AFW 1x1 85 fps 68 ms<br>3. Framework 0.2.1 + AFW 1x1 74 fps 39 ms |
| Doom The Dark Ages | 1. 0.3.0 AFW 1x1 90 fps<br>2. 0.2.13 AFW 1x1 90 fps | 1. AFW only 2x2 73 fps 20 ms<br>2. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>3. Framework 0.3.0 + AFW 1x1 90 fps 56 ms |
| Ghosts of Tsushima | 1. AFW only 2x2 74 fps<br>2. 0.2.1 AFW 1x1 90 fps | 1. AFW only 2x2 86 fps 19 ms<br>2. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>3. Framework 0.2.13 + Buffer + AFW 1x1 90 fps 56 ms |
| Ghosts of Tsushima (dlss5) | 1. 0.2.13 AFW 1x1 84 fps<br>2. 0.3.0 AFW 1x1 84 fps | 1. Director 0.2.1 + AFW 1x1 90 fps 35 ms<br>2. Framework 0.2.13 + AFW 1x1 90 fps 56 ms<br>3. Framework 0.3.0 + AFW 1x1 90 fps 56 ms |
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

### Other tools: status.json

The tray writes OFXR's live state to a small file, `status.json`, once a
second: armed, paused, the multiplier, the game's and the headset's frame
rates and the frame latency. [MSFS AutoFPS](https://github.com/ResetXPDR/MSFS_AutoFPS)
reads the 0.2 file to keep the simulator at its target frame rate; the 0.3
file keeps that format. Other tools can read it too: see
[status.json](docs/STATUS_JSON.md).

## What the tray changes on your PC

When you select **Enable**, the tray prepares a versioned runtime cache under **`%LOCALAPPDATA%\ofxr-bridge\RuntimeLayer`** and registers the bridge's OpenXR implicit-layer manifest for the current Windows user. It also manages its Vulkan layer registration when that support is enabled. The OpenXR loader loads the bridge DLL into participating game processes. Your active OpenXR runtime is unchanged.

The standalone bridge does not replace game files. The optional Assetto Corsa add-on has its own installation procedure and does modify the documented game/CSP files.

**Disable** removes the registrations owned by the tray and signals active bridge instances to stop. Exiting the tray also disarms it, and a watchdog attempts cleanup after an unexpected tray exit. Disabling does not unload a DLL from an already running game: close the game to finish that session.

Profiles, benchmarks, cached runtime files and logs remain in the common folder. Use **Open folder** in the tray to inspect them. Avoid deleting files while the game or tray is using them.

Selecting a DLSS-G preset can update the game's NVIDIA driver profile. **Crash debug** also configures crash diagnostics, including DRED, for subsequent launches. These are separate from enabling the ordinary OpenXR layer.

## Known issues

- OFXR 0.2.13 3X is not supported with Luke Ross AFW.
- Many Luke Ross games have HUD doubling during movement with AFW.
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

See the [Windows build instructions](docs/BUILDING.md).

## License

The combined standalone distribution is licensed under [GPL-3.0](LICENSE). Original components and third-party libraries retain their respective notices and licenses; proprietary providers have their own terms. See [attribution and third-party notices](licenses/ATTRIBUTION.md).

## Credits

- **Djules:** improvements to the 0.2.x route, 3X frame generation, SteamVR behavior and Vulkan support.
- **Elliottate:** hybrid frame generation, DLSS-G integration and the on-screen information meter.
- **PureDark:** AFW provider.
- **OptiScaler and RenoDX contributors:** components used by the capture and integration paths.
- **Khronos, NVIDIA, AMD and Dear ImGui contributors:** APIs, SDKs and libraries used by the project.

See the attribution file for component details and licensing notices.

## Support

If you find OFXR-BRIDGE useful and want to support its development, you can [support the project on Ko-fi](https://ko-fi.com/tig3rmast3r).
