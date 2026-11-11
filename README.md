# OFXR-BRIDGE

**Smoother VR with the graphics card you already have.**

OFXR-BRIDGE adds extra frames between the ones your game draws. Your headset
gets the smooth frame rate it needs while your graphics card does less of the
work. Play smoothly on a less powerful PC, or turn up the graphics without
losing your frame rate.

In Luke Ross R.E.A.L. VR games, it turns Legacy AER into real stereo: it fills
in the eye the game skips each frame, so the game feels like true stereo VR,
for little more than Legacy AER costs at your headset's frame rate.
See [Luke Ross usage](#luke-ross-usage) for setup and the two reconstruction methods.

It runs from a tray icon: turn it on before you start the game and it works
quietly in the background, without changing any game files. It works with
OpenXR games only (see below).

Download it from [GitHub Releases](https://github.com/tig3rmast3r/OFXR-Bridge/releases).

A complete list of changes is available in the [changelog](CHANGELOG.md).

Not working, or not feeling smoother? See [Troubleshooting](#troubleshooting).

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
> - **Hybrid, FSR 3.1 and DLSS-G:** require an **NVIDIA GPU** and the game's
>   **DLSS Super Resolution or Ray Reconstruction** enabled, with compatible
>   motion vectors and depth. **DLSS-G** additionally requires an RTX 40 or
>   50-series card; higher multipliers depend on hardware support.
> - **RIFE:** install its runtime using the **RIFE** menu in the tray before
>   selecting it. NVIDIA installation is available; Radeon Vulkan support is
>   still being tested and its installation choice is disabled in this release.
>   RIFE does not require DLSS motion vectors or depth. See [RIFE installation](#rife-installation).
> - **AER framegen:** requires **plain alternate-eye rendering**, without the
>   mod's additional AER reconstruction or interpolation fixes. In UEVR this
>   rendering method is called **Alternating / AFR**. Use ordinary frame
>   generation for native stereo.
> - **OSR and AFW:** are supported with **Luke Ross mods**, **DLSS enabled** and
>   a game in the [compatible Luke Ross list](#compatible-luke-ross-games).
> - **DLSS 5 Neural Rendering:** requires an **NVIDIA GPU**. Its model DLL is
>   not included and must be added manually; see [Neural Rendering installation](#neural-rendering-installation).
> - The [Microsoft Visual C++ Redistributable x64](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist).

## Features

**Frame generation**

- Optical flow: **NVIDIA** (Slow, Medium) at 100%, 75%, 50% or 25%
  flow resolution, or **AMD FidelityFX**.
- **RIFE** neural frame interpolation using NVIDIA CUDA/TensorRT.
- **Hybrid** combines optical flow with the game's motion vectors.
- **FSR 3.1** and **DLSS** Frame Generation, fed with the game's DLSS guides.
- **3X and 4X** frame generation on supported routes and hardware.
- **AER framegen** reconstructs temporally aligned stereo from alternate-eye
  rendering, using your selected generator.

**Luke Ross R.E.A.L. VR games**

- Two methods: **OSR (OFXR Stereo Reconstruction) / AFW (legacy)**, or
  **AER framegen**.
- **Frame Reconstruction only** disables the frame-generation stage to improve
  performance when you only need OSR/AFW stereo reconstruction.
- See [Luke Ross usage](#luke-ross-usage) to choose a method and configure the mod.

**Games and platforms**

- D3D12, D3D11 and Vulkan games, through VDXR or SteamVR.
- UEVR, REFramework, MUTAR and Luke Ross mods, plus OpenComposite.
- **Cheeky Foveated DLSS:** partial support.
- **Star Citizen:** use the **signed** build for Easy Anti-Cheat. Other games
  using Easy Anti-Cheat may also work; see [Star Citizen](#star-citizen-easy-anti-cheat).
- **OpenKneeboard** and other OpenXR panels are interpolated with the game.

**Tray and overlay**

- Per-game settings, changed in the headset (**F10**) or through the desktop
  profile drop-down.
- Automated benchmarks that can pick the best settings for each game.
- An in-headset FPS counter and an information overlay that can sit at the
  bottom of your view or follow a controller.
- Reconfigurable keyboard shortcuts through **Configure keybinds...** in the tray.
- Optional DLSS 5 Neural Rendering (model DLL supplied separately).
- A `status.json` export for tools such as MSFS AutoFPS.

## Installation

1. Extract the complete archive to a writable folder. For an Easy Anti-Cheat
   game, choose the **signed** ZIP from the release assets.
2. If you want DLSS 5 Neural Rendering on NVIDIA, add its model DLL to the
   `ofxr` folder as described below.
3. Run `OFXRBridgeTray.exe` and select **Enable**. For RIFE on NVIDIA, complete
   [RIFE installation](#rife-installation) before starting the game.
4. Optional: select **Configure keybinds...** in the tray to change the shortcuts.
5. Start the game. With injectors such as UEVR, leave OFXR enabled while the
   mod is injected.
6. Open the settings with **F10** (or your configured shortcut) in the headset,
   or **Settings...** in the tray. Most changes apply while playing; options
   marked **restart game** apply on the next launch.
7. When finished, close the game, then select **Disable** or exit the tray.

### RIFE installation

RIFE is installed in **`%LOCALAPPDATA%\ofxr-bridge\RIFE\runtime`** and is shared
by your OFXR installations. Allow about **8 GB** for the installed NVIDIA runtime
and at least **14 GB free** during setup. Optimization caches need additional space.

1. Close the game and open **RIFE -> Check RIFE...** in the tray.
2. If RIFE is missing, accept the installation prompt. To repair an existing
   installation, choose **RIFE -> Reinstall RIFE...**.
3. In **Install RIFE**, select **NVIDIA (CUDA / TensorRT)** and click **Install**.
   The Radeon Vulkan choice is greyed out while Radeon support is being tested.
4. Wait for the PowerShell installation window to finish. It shows download
   and setup progress and needs no further input.
5. Run **RIFE -> Check RIFE...** again and confirm that the runtime is ready.
   If it reports a problem, follow its message and check the reported log or
   consult the [RIFE guide](docs/RIFE.md) before selecting RIFE.
6. Start the game, select **RIFE** under Frame generation and begin with
   **Medium** Quality and **2X**.

The first use of a new resolution or Quality can take longer while TensorRT
optimization completes. The information overlay shows **Optimizing, please wait**.
For further installation and cache guidance, see the [RIFE guide](docs/RIFE.md).

### Neural Rendering installation

Place your own **`nvngx_dlssnr.dll`** model in the extracted package's **`ofxr`**
folder **before opening OFXR**. Turn on Neural Rendering in the settings when
you want to use it.

The model is not included in the ZIP. It is different from the included
**`nvngx.dll_dlssnr.dll`** forwarder; keep that file in place.

## Usage

### Global usage

The best settings depend on the game, mod, GPU, headset resolution and OpenXR
runtime. Compare options in the same representative scene and aim for a stable
headset refresh rate before increasing image quality.

Turn off your VR software's own frame interpolation, such as SteamVR Motion
Smoothing, Meta ASW, Virtual Desktop SSW, Pimax Smart Smoothing or Pico Frame
Interpolation, while using OFXR. Choose a refresh rate your game and generator
can sustain: ordinary 2X needs roughly half the headset frame rate from the game,
3X roughly a third and 4X roughly a quarter. AER framegen has a different role;
see the mod-specific sections below.

#### Pacing: Framework, Director, Buffer and Fast

Pacing controls when completed frames reach the OpenXR runtime. A generator can
be fast enough but still feel uneven if its frames are delivered at the wrong
cadence. Games and configurations react differently, so there is no single
combination that wins everywhere.

Compare **Framework** and **Director**, together with both **Buffer** and
**Fast** switches, while playing the same scene:

- **Framework / Director** change the delivery policy. On the SteamVR path,
  switch between them in Settings and compare smoothness, FPS and Cycle minimum.
  The VDXR path uses Framework pacing.
- **Buffer** gives delivery more headroom and can improve stability, at the cost
  of additional latency.
- **Fast** can reduce VRAM use and latency, but may reduce performance. Test
  both settings rather than assuming it always helps.

Leave **Tray -> Pacing (restart game) -> Auto** selected for **VDXR and SteamVR**.
Auto chooses the appropriate path for those runtimes. With another OpenXR
backend, you can compare **Force VDXR** and **Force SteamVR** and restart the game
between tests. Other backends have not been tested.

#### Choosing a frame generator

| Generator | What to expect |
| --- | --- |
| **FidelityFX** | The least expensive optical-flow option, with support across GPU vendors, but generally the lowest image quality. |
| **OFXR NVIDIA 0.2.1 / 0.2.14 / 0.3.0** | NVIDIA-only optical flow. Fast and generally better-looking than FidelityFX, but can ghost around moving characters, especially in third-person games. Compare the available engines on your system. |
| **RIFE** | Usually produces better results than OFXR optical flow and avoids its characteristic character ghosting. Low Quality presets can shimmer. It generally costs more, with cost depending on the preset and game resolution. A good choice for third-person games; avoid it for dense city scenes such as Spider-Man. Its 3X output generally looks smoother than optical-flow 3X. It does not need DLSS guides. |
| **Hybrid** | Combines optical flow with captured DLSS motion vectors and depth. A useful starting point when the game provides compatible guides. |
| **FSR 3.1** | Uses FSR frame generation with the game's captured DLSS guides; the game still needs DLSS enabled. |
| **DLSS-G** | NVIDIA frame generation using captured DLSS guides, with a supported GPU and driver. |

Quality and artifacts vary by scene and configuration. The flow-resolution
setting controls the cost of motion estimation, not the final output resolution.
3X/4X availability depends on the generator, graphics API, runtime and GPU;
**AER framegen is 2X-only**. The settings expose the multipliers supported by the
selected route.

**DLSS-G preset (next launch)** lets you choose **A** for quality or **C** for a
lighter model where supported. Restart the game after changing it.

#### Per-game settings

OFXR saves a profile for each game's executable under
**`%LOCALAPPDATA%\ofxr-bridge\Profiles`**. In the desktop Settings window, use the
**profile drop-down** to select and edit a game, including when that game is
closed. **Global default** supplies the starting settings for new game profiles.
Changes made through the in-game menu are saved to that game's profile.

#### Reading the meters

Use OFXR's own FPS counter or information overlay when comparing settings.
Other counters may report the game's input FPS rather than OFXR's output FPS.
Select a corner counter, a bottom information overlay or the controller-following
information overlay under **Display**.

- **Green information-overlay numbers:** the selected generation and processing
  stages are working. **Red:** a selected stage is not running; read the status.
- A **green FPS counter** means frame generation is active; **blue** means
  OSR/AFW reconstruction is producing frames; **red** means neither is active.
- With **AER framegen**, read its separate **ACTIVE / BYPASS / NO CONFIRMED OUTPUT**
  state and **Out / Byp** counts. The ordinary generator state alone does not
  confirm AER reconstruction. **Paired releases; AER phase assumed** means the
  input's temporal phase is being taken from your selected AER settings.

**Generation**, **Delivery** and **Cycle minimum** describe processing and delivery
through OFXR and the runtime. Cycle minimum is not complete input-to-photon latency.

#### Benchmarks

Enable **Enable benchmarks** in the tray, then open the benchmark controls under
**Display**. Choose the optical-flow, complete or Luke Ross suite and stay in the
same representative scene throughout the run. Turn **AER framegen off** before
running the ordinary suites.

- Each test has **2.5 seconds** to settle and **5 seconds** of measurement.
- Diagnostic logging is not required; benchmark CSV output is automatic.
- Results include FPS, Cycle minimum and VRAM use. They are saved under
  **`%LOCALAPPDATA%\ofxr-bridge\Benchmarks`**; use **Open folder** in the tray.
- Suite length adapts to the available routes and earlier results. Skipped tests
  remain n.a. in the CSV.
- **Auto pick best one** applies the winning settings at completion. Otherwise,
  the previous settings are restored. Stopping also restores them; manually
  changing a tested setting stops automation and keeps your edit.

The suite changes only its tested parameters. Other settings, such as your chosen
flow resolution and RIFE Quality, are retained.

### Luke Ross usage

Use a recent Luke Ross mod and choose between **OSR/AFW reconstruction with
ordinary frame generation** and **AER framegen**. OSR/AFW and the native-guide
AER methods require a game in the [compatible list](#compatible-luke-ross-games).

**Set up the Luke Ross mod first:**

1. Select **Legacy AER** in the REAL VR menu. Disable AER V2, AER 1/2 and other
   built-in interpolation or AER correction modes.
2. Lower the **Reduce shaking** slider.
3. Turn off **Camera rotation compensation**, where offered. It can make
   gamepad or vehicle turns look stepped when combined with OFXR reconstruction.
4. For **OSR/AFW**, enable **DLSS Super Resolution or Ray Reconstruction** in the
   game. DLSS is also required for Hybrid, FSR 3.1 or DLSS-G with AER framegen.

#### OSR / AFW

**OSR** is OFXR's own alternative to **AFW**, with less ghosting. It generally
works best together with ordinary frame generation: OSR rebuilds the eye skipped
by Legacy AER, then the frame generator adds frames between the reconstructed
stereo pairs. **AFW (legacy)** remains available as an alternative.

With either method, a game's HUD or overlay icons can double during movement,
including strafing or turning with a gamepad. Check the observations below.

**Frame Reconstruction only** disables the extra frame-generation stage while
keeping OSR/AFW stereo reconstruction. It can improve performance when the game
already reaches the required refresh rate. Restart the game after changing it.

#### AER framegen

Select your generator and enable **AER framegen** under **Frame generation**.
Leave **Frame Reconstruction only** off; restart the game if you changed it.
AER framegen uses the generator to reconstruct the other eye at the correct
point in time, rather than providing the usual full-stereo FPS multiplication.
Do not expect the same FPS gain as ordinary frame generation.

This can cost more performance than OSR with ordinary frame generation, but it
keeps the HUD clean in the tested cases. Choose the method that gives you the
best balance of HUD clarity, smoothness and performance.

For the games below, compatible DLSS guides also make **Hybrid**, **FSR 3.1** and
**DLSS-G** available with AER framegen. **Hybrid** is the recommended starting
point. Use the AER state in the information overlay to check that reconstruction
is active.

#### Compatible Luke Ross games

These games support the listed Luke Ross reconstruction paths. HUD observations
below concern **OSR/AFW**; empty cells have no recorded observation yet. HUD or
overlay icons may double while moving, strafing or rotating with a gamepad.

| Game | HUD observations |
| --- | --- |
| Avatar: Frontiers of Pandora | HUD correct |
| Crimson Desert | HUD correct; overlay icons double |
| Cyberpunk 2077 | HUD correct |
| Dead Space (2023) | HUD double; overlay icons correct |
| DOOM: The Dark Ages | HUD double |
| Ghost of Tsushima | HUD double |
| Grand Theft Auto V Enhanced | HUD double |
| Horizon Forbidden West | HUD double |
| Indiana Jones and the Great Circle | HUD double |
| Kingdom Come: Deliverance II | HUD correct |
| Marvel's Spider-Man Remastered | HUD correct |
| Marvel's Spider-Man: Miles Morales | HUD correct |
| Red Dead Redemption 2 | HUD correct |
| Star Wars Outlaws | HUD double |
| The Last of Us Part I | HUD double |
| Uncharted: Legacy of Thieves Collection | HUD double |

### UEVR

Select **OpenXR** in UEVR. With **Native Stereo**, use ordinary frame generation
and compare its generators, Framework/Director pacing, Buffer and Fast according
to the requirements above. Native Stereo remains the recommended choice for the
best performance in the tested configurations.

There is also **partial AER framegen support** for UEVR's **Alternating / AFR**
rendering method. Use plain alternating-eye output without additional AER
reconstruction or interpolation fixes, then enable **AER framegen** under
**Frame generation** in OFXR. Results depend on the game and mod configuration.

### REFramework and MUTAR

For REFramework games running in **stereo**, use ordinary OFXR frame generation.

For MUTAR's **AER** modes, turn off the mod's interpolation, warp and other AER
reconstruction fixes so that it supplies plain AER. Then enable **AER framegen**
under **Frame generation** in OFXR and choose your generator. Aim to reach the headset's
refresh rate for the best results. The difference can be subtle in a quiet scene;
turning the camera makes the temporal alignment easier to judge.

### Microsoft Flight Simulator 2024

MSFS 2024 has been tested extensively. **Director + Buffer** has given the most
stable pacing in our tests, at the cost of higher latency. Compare it with the
other combinations on your own system, keeping the same scene and settings.

#### OpenKneeboard

Compatible OpenXR panels, including OpenKneeboard, can have their images and poses
interpolated alongside the game. This includes panels with separate textures or
shared texture atlases. HUD elements already drawn into a game's eye images are
part of those images; SteamVR overlays use a separate path.

#### status.json and AutoFPS

Enable **Export status.json** under **Settings / Logging** to write OFXR's live
state once a second, including whether it is enabled or paused, the multiplier,
game and headset frame rates, and the latency.
[MSFS AutoFPS](https://github.com/ResetXPDR/MSFS_AutoFPS) can read it to maintain the
simulator's target frame rate. See the [status.json format](docs/STATUS_JSON.md).

### Star Citizen (Easy Anti-Cheat)

Use the **signed** ZIP attached to the release for Star Citizen. OFXR's tray,
OpenXR and Vulkan layers, and NGX forwarders need to be signed for Easy Anti-Cheat
to let them load. Enable the tray before starting the game.

Optical flow, Hybrid and DLSS-G have been tested with SteamVR and VDXR. Other
OpenXR games using Easy Anti-Cheat may also work, but have not been verified.
An unsigned build, including one you compile yourself, is not suitable for this
path unless it is signed.

> [!CAUTION]
> OFXR is an independent project, not endorsed by Cloud Imperium Games or Easy
> Anti-Cheat. Using it with an anti-cheat protected game is at your own risk:
> we accept no responsibility for any account action, ban or other
> consequence. Check the game's terms of service before you play.

### Assetto Corsa

An optional modified OptiScaler add-on provides DLSS upscaling, OFXR and optional DLSS 5 Neural Rendering for Assetto Corsa in VR. It is distributed separately from the standalone bridge.

1. Install an appropriate CSP version. Testing used **0.3.0-preview675**.
2. Disable MSAA under **Assetto Corsa / Video / Quality**.
3. Under **CSP / Extra FX**, enable **Allow in VR**.
4. Under CSP's upscaling settings, select the **OptiScaler** method and use **DLSS** as the base upscaler to intercept. Choose the desired DLSS quality.
5. Run the game once so CSP installs its OptiScaler integration, then close the game.
6. Extract the add-on archive and run its **`Install-Assetto.ps1`** installer, pointing it at the game folder containing **`acs.exe`**. Follow the add-on README. The installer backs up affected files and installs the modified DLL in the active CSP OptiScaler cache.
7. **Optional:** for DLSS 5 Neural Rendering, supply your own **`nvngx_dlssnr.dll`** beside `acs.exe`. Keep the OFXR tray enabled when launching the game.

Keep CSP's method set to **OptiScaler**. The add-on includes the required DLSS SR/FG runtime files; the DLSS 5 Neural Rendering model is not included. Do not place the modified `version.dll` beside `acs.exe`: it belongs in the CSP OptiScaler cache selected by the installer.

### Other games and mods

Testing outside the configurations described above is partial. If a specific
game or mod does not work, please open a [GitHub issue](https://github.com/tig3rmast3r/OFXR-Bridge/issues)
with the game, mod, GPU, OpenXR runtime and OFXR settings, plus a diagnostic log
when available. See [Reporting problems](#reporting-problems).

## Known issues

- OFXR 0.2.14 3X is not supported in Luke Ross games.
- OSR relies on the game's DLSS motion vectors, which are not perfect; artifacts
  can remain in the reconstructed eye. HUD observations are listed in the
  [Luke Ross table](#compatible-luke-ross-games).
- **Cheeky Foveated DLSS in Luke Ross games** needs a fix on the Cheeky Foveated DLSS side.
- DLSS 5 Neural Rendering can cause instability or device-hung crashes in some games.
- Extra OpenXR layers or overlays can exhaust the available swapchain budget.
- Vulkan support remains experimental.

## Troubleshooting

For setup problems, uneven motion, missing guides or crashes, see the
[troubleshooting guide](docs/TROUBLESHOOTING.md). If it does not resolve the
problem, collect the information below when reporting it.

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
