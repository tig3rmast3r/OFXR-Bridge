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

- [new] Change settings in VR with F10 while the game runs.
- [new] NVIDIA OFXR 0.3.0 and Djules optical-flow routes.
- [new] Hybrid frame generation for games using DLSS.
- [new] FSR 3.1 and DLSS Frame Generation.
- [new] AFW stereo reconstruction for selected Luke Ross games.
- [new] Optional DLSS 5 Neural Rendering.
- [new] Information overlay and latency meters.
- [new] Automated benchmarks help find suitable settings for each game.
- [new] SteamVR Director pacing.
- [changed] Improved NVIDIA OFXR 0.2.1 frame generation.

## V 0.3.1 (2026/10/26)

- [new] Luke Ross AFW support for Spider-Man Remastered, Miles Morales and Dead Space.
- [new] Preliminary Luke Ross AFW support for Grand Theft Auto V Enhanced.
- [changed] Steadier VDXR frame rates near headset refresh.
- [fix] Restored performance in configurations affected by severe frame-rate loss.

## V 0.3.2 (2026/11/02)

- [new] Signed builds support Star Citizen through Easy Anti-Cheat.
- [new] OSR stereo reconstruction for Luke Ross games.
- [new] Luke Ross support for Cyberpunk 2077, Avatar: Frontiers of Pandora, The Last of Us Part I and Uncharted: Legacy of Thieves Collection.
- [new] Motion-vector support for Ready or Not and MSFS 2024 with foveated rendering.
- [new] Motion-vector support for Vulkan games.
- [new] Partial Cheeky Foveated DLSS compatibility outside Luke Ross games.
- [new] OpenKneeboard panel interpolation.
- [new] Optional status.json export for AutoFPS and other tools.
- [changed] OSR is the default reconstruction method, with AFW available as a legacy alternative.
- [changed] Faster Luke Ross reconstruction and lower VRAM usage.
- [fix] Corrected Grand Theft Auto V Enhanced support.
- [fix] Neural Rendering works at reduced working resolutions.

## V 0.3.3 (2026/11/11)

- [new] RIFE neural frame generation for NVIDIA GPUs, with seven quality presets.
- [new] Install, check and optimize RIFE for your games from the tray.
- [new] AER framegen reconstructs the other eye with optical flow, RIFE or compatible DLSS-guided generators.
- [new] Dedicated AER status distinguishes confirmed reconstruction from bypass or missing output.
- [new] Additional 3X and 4X options on compatible frame-generation routes.
- [new] Frame generation for compatible 32-bit D3D11 VR games.
- [new] Attach the information overlay to a VR controller.
- [new] Reassign keyboard shortcuts from the tray.
- [new] Select and edit individual game profiles from the desktop settings window.
- [new] CPU usage, GPU usage and memory information in the information overlay.
- [changed] Benchmarks prioritize steady full-refresh output when choosing settings.
- [changed] Cleaner frame delivery when all image-processing features are disabled.
- [fix] Reduced Hybrid ghosting around occlusion edges with AER framegen.
- [fix] Corrected OSR eye alignment and output at smaller working resolutions.
- [fix] More reliable Luke Ross reconstruction across rendering resolutions and dark scenes.
- [fix] Static projection layers no longer block frame generation.
- [fix] Reduced overlay allocation failures near SteamVR's swapchain limit.
- [fix] Improved Neural Rendering compatibility with packed HDR game images.
