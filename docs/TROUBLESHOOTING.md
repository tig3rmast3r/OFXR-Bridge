# OFXR-BRIDGE troubleshooting

> [!IMPORTANT]
> **First, make sure you are on the latest release.** Every release
> fixes problems reported here, so check the
> [releases page](https://github.com/tig3rmast3r/OFXR-Bridge/releases) before
> anything else. **About** in the tray menu shows the version you are running.

> [!IMPORTANT]
> **Enable OFXR before you start the game.** Run the tray and select
> **Enable** first. If you enable it while a game is running, restart the game.
> This matters most for Hybrid, FSR 3.1, DLSS-G and Luke Ross games: OFXR has
> to be running when the game sets up DLSS.

## Low frame rate or stutter?

Try these in order. Most apply while you play; open the settings with **F10**
in the headset or **Settings...** in the tray.

| Step | Change |
| --- | --- |
| 1 | **Frame generation / Pacing**: switch between **Director** (default) and **Framework**, and compare in the same scene. Framework can be better. |
| 2 | **Flow resolution**: 25% |
| 3 | **Frame generation**: **OFXR** instead of Hybrid, FSR 3.1 or DLSS-G (the menu marks those slow and slowest) |
| 4 | **Neural Rendering**: off |
| 5 | Lower the per-eye resolution in your VR software |

Still not right? Click **Reset to defaults** in the **Frame generation** tab
and restart the game. You can also let OFXR find the best settings: with
**Enable benchmarks** ticked in the tray, run a suite from the **Display** tab
with **Auto pick best one** on. If the problem is still there, carry on below.

## Step 1: is OFXR running in your game?

OFXR shows an **information overlay** at the bottom of your view by default.
Use **Display / On-screen FPS counter** to show or hide it, or press **F6**
(configurable in the tray's **Configure keybinds...**). **Counter type** selects
a corner counter, controller overlay or bottom overlay and stays selected while hidden.

| You see | It means | Go to |
| --- | --- | --- |
| No overlay or counter with **On-screen FPS counter** enabled | OFXR may not be loaded in the game | [A](#a-ofxr-is-not-loaded) |
| Numbers in **red**, or a **red** corner counter | OFXR is loaded but not generating frames | [B](#b-ofxr-is-loaded-but-not-generating), or [C](#c-luke-ross-games) in a Luke Ross game |
| Numbers in **green**, or a **green** or **blue** corner counter | OFXR is working (blue: Luke Ross reconstruction is producing the frames) | [D](#d-ofxr-works-but-it-does-not-feel-better) |
| **Paused** in the top right corner | Frame generation is paused | Press the pause key again (**F7** unless you changed it; the tray menu shows it), or tick **Enable frame generation** in Settings |

The information overlay says what OFXR is doing:

- **Top right:** what runs, for example **OFXR 0.3.0**, **Hybrid + OSR**,
  **OSR**, **Real frames only**, **Waiting** or **Paused**.
- **Top centre:** VRAM use.
- **AER:** **ACTIVE** confirms recent reconstructed output submitted to the runtime;
  **BYPASS** or **NO CONFIRMED OUTPUT** means that AER output is not confirmed.
  Paired releases report an assumed phase, not automatic proof that the game is AER.
- **Vectors:** **DLSS** when OFXR sees the game's DLSS, **None** otherwise.
- **Status:** any problem, in plain words (the tables below use the same
  words), then **OFXR**, **OSR** or **AFW**, and **Neural Render**, each
  **Active**, **Waiting** or **Off**.

## A. OFXR is not loaded

| Cause | What to do |
| --- | --- |
| The game uses **OpenVR**, not OpenXR | OFXR only works with OpenXR games. Many OpenVR games run through OpenXR with [OpenComposite](https://gitlab.com/znixian/OpenOVR). With UEVR, choose its OpenXR option. |
| OFXR was not enabled when the game started | Select **Enable** in the tray, then restart the game. |
| The game runs **as administrator** | Run the game, and Steam or its launcher, normally, not as administrator. |
| The game runs under another Windows user | Run the tray and the game under the same Windows account. |
| The game is 32-bit | Extract the complete archive, including its `ofxr_x86` folder, so the tray can enable the 32-bit layer. |
| The game uses OpenGL | Not supported. |
| The game's anti-cheat blocks it | For Star Citizen, use the **signed ZIP** attached to the release. Other EAC games may work but are not verified, and other anti-cheats may still block OFXR. Using OFXR with an anti-cheat protected game is at your own risk. |
| The game crashes at start | Try without ReShade and other OpenXR layers, then [send a log](#still-stuck-send-a-log). |

## B. OFXR is loaded but not generating

Find the words shown after **Status:** on the information overlay.

| Status shows | What to do |
| --- | --- |
| **Waiting**, in a menu or loading screen | Normal. Generation starts once you are in the 3D world. |
| **OFXR: Off** | **Enable frame generation** is unticked in Settings, or OFXR was disabled in the tray. Tick it; after **Disable** in the tray, select **Enable** and restart the game. |
| **Missing game vectors**, or **Vectors: None** with Hybrid, FSR 3.1 or DLSS-G | These need the game's **DLSS** (Super Resolution or Ray Reconstruction). Turn DLSS on in the game: not FSR, XeSS or TAA. Enable OFXR before starting the game. Without DLSS, use **OFXR** (optical flow). |
| **Missing depth** | OFXR could not read the game's depth. [Send a log](#still-stuck-send-a-log), and use **OFXR** (optical flow) meanwhile. |
| **Game output does not match the eye image** | The game's DLSS image is not the size of the eye image. Usually a foveated-rendering mod or layer (for example Quad-Views-Foveated) or dynamic resolution: turn it off. MSFS 2024's own foveated rendering and Cheeky Foveated DLSS are supported, except Cheeky in Luke Ross games. |
| **Missing stereo pair**, **Frame mismatch / missing history pair**, **Invalid vectors / guide identity** | [Send a log](#still-stuck-send-a-log). |
| **Fallback** in the top right, with Hybrid | Not an error: the game's vectors cannot be used right now, so Hybrid uses optical flow only. |
| **DLSS-G provider unavailable** | DLSS-G needs an RTX 40 or 50-series card and a recent driver. The first time, restart the game once. Otherwise choose another frame generation. |
| **NGX refused interpolation** or **NGX rejected generation** | The NVIDIA driver declined DLSS-G for these frames. Choose Hybrid or OFXR, and [send a log](#still-stuck-send-a-log). |
| **FSR provider missing** | A file is missing from the `ofxr` folder. Extract the complete archive again. |
| **Swapchain allocation refused**, **Overlay swapchain limit reached** | SteamVR ran out of OpenXR swapchains. Turn off OpenXR overlays you don't need (OpenKneeboard, toolkits) and, in UEVR, depth submission. In Luke Ross games, the **Luke Ross** tab shows the swapchain details. |
| **Vulkan game-vector capture suspended** | In this Vulkan game, OFXR cannot use the game's vectors at the moment. Use **OFXR** or **Hybrid**, which fall back to optical flow; FSR 3.1 and DLSS-G show real frames only. |
| **Paused** | See [Step 1](#step-1-is-ofxr-running-in-your-game). If it pauses and resumes by itself, your game or a mod uses the same key: choose another with **Configure keybinds...** in the tray menu. |
| Anything else | [Send a log](#still-stuck-send-a-log). |

## C. Luke Ross games

In Luke Ross R.E.A.L. VR games, OFXR rebuilds the eye the game skipped. The
**Status:** line shows **OSR** (or **AFW** with AFW (legacy) selected).

| You see | What to do |
| --- | --- |
| **OSR: N/A** | OFXR did not detect the Luke Ross mod. Update the mod (tested with v2609.18.2 and newer), and enable OFXR before starting the game. |
| **OSR: Waiting**, numbers in red | In the REAL VR menu, select **Legacy AER** (not AER V2 or AER 1/2) and turn on DLSS in the game. Cheeky Foveated DLSS does not work in Luke Ross games yet: turn it off. |
| **OSR: 2D scene (no stereo source)** | Normal in menus and cutscenes the mod shows flat. It starts again in the 3D world. |
| A doubled or flashing image, or one eye wrong | Usually AER V2 instead of **Legacy AER**: change it in the REAL VR menu. |
| Turning in a vehicle looks stepped | In the REAL VR menu, turn off **Camera rotation compensation**. |
| The HUD or icons show twice when you move or turn | It depends on the game: see the HUD table in the [README](https://github.com/tig3rmast3r/OFXR-Bridge#readme). |
| **Frame Reconstruction only** has no effect | Restart the game after changing it. |
| You want to compare with AFW | Select **AFW (legacy)** under **Luke Ross / Reconstruction**. It applies while you play. |

## D. OFXR works, but it does not feel better

| Cause | What to do |
| --- | --- |
| The game already runs near your headset's refresh rate | In a Luke Ross game, try **Frame Reconstruction only** for OSR/AFW, or compare **AER framegen** when starting from plain Legacy AER. |
| The game runs **below half** your refresh rate (under 45 FPS at 90 Hz) | Lower the refresh rate (72 Hz needs 36 FPS) or the game's settings until it holds half. For games around a third, try **3X** under **Multiplier** on a supported route. Available 3X/4X choices depend on the generator, graphics path and GPU; **AER framegen** remains 2X. |
| **3X** is on for a game that can hold half your refresh rate | Go back to 2X: 3X holds the game to a third of the refresh rate, and 2X looks better. |
| Your VR software's own frame smoothing is on | Turn it off, it fights OFXR: **SteamVR** Motion Smoothing and "fixed frame rate at half" (per-application video settings), **Meta** ASW, **Virtual Desktop** SSW, **Pimax Play** Smart Smoothing, **Pico Streaming Assistant** Frame Interpolation. |
| Frames feel uneven although the number is high | Switch **Pacing** between Director and Framework (step 1 [above](#low-frame-rate-or-stutter)). |
| Your GPU runs out of VRAM | Signs: stutter, sudden drops, or a crash, often after a few minutes. Watch VRAM at the top of the information overlay. Lower the game's settings or the per-eye resolution first, then try **Fast mode** (not in Luke Ross games). See [VRAM](#vram). |
| The number stops short of the refresh rate (110 to 115 at 120 Hz, for example), although the game runs above half of it without OFXR | Generating frames costs GPU time too, so the game needs some headroom above half the refresh rate. Lower the refresh rate, the per-eye resolution or the flow resolution. |
| Dips only in heavy scenes | Disable OFXR and play the same scene. If the dips are still there, lower the per-eye resolution: OFXR can't make up frames the game doesn't render. |
| Another FPS counter shows half of OFXR's | fpsVR, SteamVR frame timing and in-game counters measure before OFXR. Trust OFXR's overlay. |
| You want to know whether a problem comes from OFXR | Select **Disable** in the tray and restart the game. Pausing is not enough: a paused game still runs through OFXR. |

## VRAM

OFXR needs video memory on top of the game's, and more as your per-eye
resolution grows. Luke Ross reconstruction and Neural Rendering add more.
The top of the information overlay shows the current use, and every benchmark
records it in its CSV.

- **Fast mode (lower vram, lower latency, may reduce performance)** keeps
  fewer working images. Compare both: its effect on performance depends on the
  game. It does not change Luke Ross OSR/AFW reconstruction itself.
- **Pimax headsets:** Pimax OpenXR keeps its own copy of every image, so
  everything costs more VRAM on it. If VRAM is tight, run your Pimax through
  SteamVR with
  [sboys3's native SteamVR driver](https://store.pimax.com/blogs/blogs/sboys3-native-steamvr-driver-setup-guide),
  especially for MSFS 2024.

### Know your real per-eye resolution

It is what your VR software asks the game to render, not the panel's
resolution or the "100%" figure. In SteamVR, the global resolution and the
game's own resolution **multiply**. On a Pimax Crystal Super:

| SteamVR global | Game's own setting | Pixels rendered | Per eye |
| --- | --- | --- | --- |
| 100% | 100% | 1× | about 4096×3206 |
| 200% | 100% | 2× | about 5793×4534 |
| 200% | 200% | 4× | 8192×6412 |

The `view_configuration` record near the top of a log shows the real per-eye
size. If you run out of VRAM, lower the per-eye resolution first: it lowers
both OFXR's use and the game's.

## Crashes

| Cause | What to do |
| --- | --- |
| A crash or "device hung" with **Neural Rendering** on | Turn Neural Rendering off. It can be unstable in some games. |
| A crash at start | Try without ReShade and other OpenXR layers. |
| A crash now and then | Tick **Crash debug (restart game)** in the tray, restart the game, and send the log after the next crash. Untick it for normal play. |

## Still stuck? Send a log

1. Tick **Diagnostic logging** in the **Display** tab, or press **F9** in the
   game. For a problem at start-up, tick it before starting the game.
2. Reproduce the problem once.
3. Close the game, so the log is complete.
4. Select **Open folder** in the tray, then open **RuntimeLayer** and the
   folder of your version (for example `v770`).
5. Send the newest `ofxr-bridge-flight-*.log` on
   [GitHub Issues](https://github.com/tig3rmast3r/OFXR-Bridge/issues) or the
   [Flat2VR Modding Discord](https://discord.gg/flat2vr), with:
   - the game and its version, and the VR mod, if any;
   - your headset and VR software (SteamVR, Virtual Desktop, Pimax Play...);
   - your GPU and driver version;
   - the OFXR version, frame generation, engine and pacing you chose;
   - what you did and what you saw.

If no log appeared, say so too: it usually means OFXR was not loaded
([A](#a-ofxr-is-not-loaded)).
