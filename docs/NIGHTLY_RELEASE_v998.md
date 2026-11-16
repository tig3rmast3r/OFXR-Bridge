# Nightly V998 ? Independent pacing integration

This nightly includes Independent pacing through V1137 and preserves the V994 fixes for native multiframe delivery, VRAM reuse, Neural Rendering, OptiScaler guides, ReShade and Radeon route availability.

## Pacing controls

With Advanced Mode off, Independent pacing is always selected. The previous pacing controls and the tray Auto / VDXR / SteamVR override are hidden. Advanced Mode exposes the Independent switch and the previous pacing options for comparisons.

Changing Independent requires restarting the game. Its selector shows a yellow restart indicator. If an already running session still uses the previous pacing path, the interface asks for a restart; changing the setting does not replace that session live.

Independent keeps input capture, generation and native delivery separate. Its reserve adapts automatically. Existing frame generation, reconstruction and Neural Rendering options remain subject to their hardware and game requirements. AER 4? remains available on supported paths; AER 3? is excluded.

Independent benchmarks use ten-second trials and test available 2? methods first. Higher multipliers run only if none of the 2? results reaches the headset refresh rate. FidelityFX remains outside automatic benchmarks.

## RIFE installation

The tray installer enables Radeon Vulkan when the packaged Vulkan payload is available. Without that payload the option is disabled and installation rejects a forced Radeon selection. NVIDIA remains the default. Radeon hardware compatibility is still being tested.

## Validation

The x64 and x86 builds passed, together with 120 CPU/software-GPU integration tests and 18 focused repeats. Coverage includes the Basic/Advanced controls, restart indicator, installer dialog and simulated helpers, native image ownership, Independent copy pipeline and delivery contracts.

Native NVIDIA optical-flow hardware cases were not validated in this session. Their software-GPU failures also reproduce on the original V994 baseline. Actual game performance and headset behavior on this merged nightly still require testing; the owner's positive V1137 results are not a live validation of V998.

This is an unsigned nightly build, not a new public release.
