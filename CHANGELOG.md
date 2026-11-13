# Changelog

## V 0.1.0 (2026/09/05)

- [new] First release.

## V 0.1.1 (2026/09/06)

- [new] In-headset FPS counter shows whether frame generation is active.

## V 0.2.0 (2026/09/07)

- [fix] Steadier SteamVR frame delivery under heavy GPU load.
- [fix] Improved compatibility with UEVR and Luke Ross mods.
- [fix] More reliable recentering, resolution changes and session cleanup.

## V 0.2.1 (2026/09/15)

- [new] Visual diagnostic for checking whether generated frames reach the headset.
- [changed] Improved support for UEVR AFR, UEVR Native Stereo and Luke Ross mods.
- [fix] More stable SteamVR pacing at high headset resolutions.
- [fix] Improved compatibility with games running as administrator.
- [fix] More reliable activation, shutdown and cleanup after earlier versions.

## V 0.3.0 (2026/10/25)

- [new] In-game settings and on-the-fly changes (**F10**)
- [new] Added Djules' OFXR 0.2.13 route
- [new] New OFXR 0.3.0 route
- [new] Added Elliottate's vector-based Hybrid route (requires DLSS)
- [new] New FSR 3.1 Frame Generation route (requires DLSS)
- [new] Added Elliottate's DLSS Frame Generation integration
- [new] Added Elliottate's on-screen display
- [new] AFW support for selected Luke Ross games
- [new] Optional **DLSS 5 Neural Rendering**, applied before Super Resolution (**pre-SR**)
- [new] Latency meters
- [new] Automated benchmarks
- [new] New unified SteamVR Director
- [changed] Improved OFXR 0.2.1 route
- [fix] Countless minor fixes

## V 0.3.1 (2026/10/26)

- [new] Added Luke Ross AFW support for Spider-Man and Miles Morales
- [new] Added Luke Ross AFW Support for Dead Space
- [new] Added Luke Ross AFW Support for GTA5E (not fully working yet)
- [changed] improved VDXR route, it now maintains steadier fps once you reach headset refresh
- [fix] fixed an issue that may kill performance on almost all games with some configurations

## V 0.3.2 (2026/11/02)

- [new] **Star Citizen support:** OFXR's files are now digitally signed, so Easy Anti-Cheat lets OFXR load. Optical flow, Hybrid and DLSS-G work, with SteamVR and VDXR. Use at your own risk; see the README.
- [new] **More Luke Ross games:** fixed Grand Theft Auto V Enhanced support and added support for Cyberpunk 2077, Avatar: Frontiers of Pandora, The Last of Us Part I, and Uncharted: Legacy of Thieves Collection (Uncharted 4 and The Lost Legacy).
- [new] **New OSR option for Luke Ross:** OFXR Stereo Reconstruction replaces AFW as the default, with less ghosting and better performance. AFW remains available as **AFW (legacy)**.
- [new] **Motion-vector support for Ready or Not.**
- [new] **Motion-vector support for Microsoft Flight Simulator 2024 with foveated rendering.**
- [new] **Compatibility with Cheeky Foveated DLSS**, except in Luke Ross games, which still need a corresponding fix on the Cheeky Foveated DLSS side.
- [new] **Motion-vector support for Vulkan games.**
- [new] **AutoFPS support** through the optional `status.json` export.
- [new] **OpenKneeboard support**, including interpolation of compatible OpenXR panel images and poses.
- [changed] **Improved performance** with Luke Ross OSR and AFW.
- [changed] **Lower VRAM usage.**
- [fix] **Fixed Neural Rendering when using a reduced working resolution.**

## V 0.3.3 (2026/11/11)

- [new] RIFE frame generation.
- [new] New user interface.
- [new] Configurable keybinds.
- [new] 3X and 4X frame generation on selected routes.
- [new] CPU usage meter.
- [new] Individual game profile selection.
- [new] Added Red Dead Redemption 2 compatibility.
- [changed] Improved benchmarks.
- [fix] GTA V Enhanced now works correctly.
- [fix] Improved OSR visuals and compatibility in almost all games.
- [fix] Several SteamVR pacing improvements.
- [fix] Fixed swapchain allocation errors in UEVR.
- [fix] Fixed Neural Rendering in MSFS 2024, including foveated rendering.
- [fix] Fixed vector-based frame generation in MSFS 2024 with foveated rendering.
- [fix] Several other minor fixes.

## Nightly V984 (2026/11/12)

- [fix] 3X and 4X frame generation now activate on compatible native OpenXR sessions, including VDXR and Pimax OpenXR.
- [fix] Compatible Luke Ross OSR/AFW sessions publish every generated frame at 3X and 4X.
- [fix] Pimax OpenXR delivery no longer queues ready generated frames behind later game rendering.
- [fix] Small DLSS output-alignment differences no longer reject otherwise matching packed stereo guides.
- [fix] `status.json` reports the effective frame-generation multiplier, including OSR and fallback.

## Nightly V985 (2026/11/12)

- [fix] RIFE installation retries temporary file locks and provides clearer diagnostics when another program prevents activation.

## Nightly V986 (2026/11/13)

- [fix] Reduced output-buffer VRAM use for native OpenXR OFXR/Hybrid 3X and 4X, including Pimax OpenXR and VDXR, while retaining batch overlap and the existing pacing rules. SteamVR keeps its existing allocation path.
