# V860: a 32-bit layer for 32-bit games

## Symptom

32-bit games (Alien: Isolation through MotherVR) failed `xrCreateInstance`
with OFXR armed (0.2 issue #52).

## Cause

HKCU's OpenXR implicit layer key is shared by 32- and 64-bit processes, so a
32-bit game's loader found only the 64-bit layer, which it cannot load. A
loader fails only when no listed layer loads, so registering a 32-bit
manifest beside the 64-bit one lets each process skip the DLL it cannot load.
This ports 0.2's V576.

## What the 32-bit layer does

- D3D11 sessions only. An instance without `XR_KHR_D3D11_enable` passes every
  call to the next layer; a D3D12 or Vulkan session is left unbound and passes
  through.
- NVIDIA optical flow (`nvofapi.dll`) whatever engine is selected (FidelityFX's
  choice runs Nvidia 0.3.0). No FSR, DLSS-G, RIFE, Hybrid, NR or panels:
  FidelityFX, NGX and RIFE ship for x64 only. 2X only, as for every D3D11
  game.
- SteamVR through `bin\win32\openvr_api.dll`, delivered directly as in 0.2:
  through the private runtime, SteamVR refused Alien: Isolation's two-layer
  frames (`XR_ERROR_SWAPCHAIN_RECT_INVALID`).

## Tray

With `ofxr_x86` in the package, arming installs the 32-bit DLL beside the
64-bit one as `XR_APILAYER_XRFrameBridge_x86.dll`, sharing its INI, controls
and log folder, and registers `XR_APILAYER_OFXRBridge_x86-<pid>-<arm>.json`
once the watchdog runs. Per-user arms only; a failure is logged (`x86-layer`)
and leaves the 64-bit arm as it is. Disarm, the watchdog and start-up cleanup
retire it.

## Build

`Build-Autonomous.ps1 -X86Layer` builds the Win32 layer into
`<BuildRoot>-x86` first; the 64-bit build bundles it into `ofxr_x86`
(`xrfg_bundle_x86_layer`), and `tools\Package-RifeBuild.ps1` packages it when
present. FidelityFX and NGX stand-ins (`fidelityfx_unavailable.cpp`,
`ngx_unavailable.cpp`) fail at initialisation.

## Test

Alien: Isolation (MotherVR, 32-bit OpenComposite) on SteamVR starts and
generates at 2X.
