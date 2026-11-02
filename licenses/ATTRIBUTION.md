# Source and provider notices

V800 optionally downloads NVIDIA TensorRT 10.14.1.48 into the shared RIFE
runtime. Its original vendor license files remain with the extracted official
packages under `backends/tensorrt`. The bridge ZIP contains the pinned setup
metadata and source integration, not those vendor libraries. RIFE model code
and weights retain their upstream MIT notices.

The combined autonomous source and layer use GPL-3.0, supplied in the root
LICENSE. Original component notices and licences remain preserved below.

Original OFXR Bridge / Elliot Tate's native-DLSS-G fork: LGPL3;
original licence preserved as OFXR-LGPL-3.0.txt.
Accepted OptiScaler V33034 components and their extracted shaders: GPL3;
licence preserved as OptiScaler-GPL-3.0.txt and in the NR source notices.
Dear ImGui: MIT, notice preserved.
RenoDX colour composition: notice in src/neural/licenses/RenoDX_ATTRIBUTION.txt.
Khronos Vulkan headers: Apache2/MIT notices preserved.
NVIDIA DLSS SDK/runtime and AMD FidelityFX runtime: original vendor licences
are supplied. AFW provider is the official PureDark v1.0-beta.6 DLL from the UEVR release:
https://github.com/PureDark/UEVR/releases/tag/UEVR_AFW_v1.0-beta.6
Import hash, ABI provenance and reproduction: docs/AFW_BETA6_IMPORT.json.
The original provider binary is distributed as a package dependency, not source.

The external NVIDIA Neural Rendering model nvngx_dlssnr.dll is not included.
The included nvngx.dll_dlssnr.dll is this project's small source-built forwarder.

V460 adapts the sRGB colour palette and counter hierarchy from Elliot Tate's
status panel at upstream commit 9579149c341c19e31f1fa9e48d56dc56b96e6872.
The original Dear ImGui embedded font remains in use; no font asset is imported.

## V471 Elliottate visual meter

Palette, rounded canvas, Scale2x bitmap text, controller flip/placement and pose-only action integration adapted from elliotttate/OFXR-Bridge R19 under LGPL-3.0-or-later. Daniel Hepper font8x8_basic.h is public domain; its upstream notice is retained in the header. Only the visual meter is ported; our existing runtime values/diagnostics supply its contents.

## V474 Djules lifecycle and bridge improvements

V439⡂€“V443 improvements adapted from djules75/OFXR-Bridge 0.2.13.2, LGPL-3.0-or-later.
Original author djules75 <1819336+djules75@users.noreply.github.com>. Exact original
patches include their Co-Authored-By Claude Opus5.5 trailers; the provenance JSON
records every commit and hash. Route isolation and Autonomous-specific regression
fixtures are adaptations in this repository. See docs/imports/djules-v439-v443/.
DLSS-G per-game NVIDIA profile selection is adapted from elliotttate/OFXR-Bridge
release 36, b8048568c574f3a89847c18a4a0cd6d5aec22c39, under LGPL-3.0-or-later.
Only public NVAPI DRS profile read/write controls are imported; no upstream
pacing, shader, scheduler or model binary changes are included.

## Portable RIFE Microsoft runtime

The NVIDIA RIFE installer package includes the app-local Microsoft.VC143.CRT
14.44.35112 files identified by SHA256 in the RIFE runtime lock. The original
Microsoft license terms from that redistributable accompany them in
[MSVC_CRT_LICENSE.rtf](MSVC_CRT_LICENSE.rtf). These files are installed only
inside the per-user portable Python runtime.
