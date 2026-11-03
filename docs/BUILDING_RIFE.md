# Building and packaging the RIFE distribution

Use Windows x64, Visual Studio 2022 C++ Build Tools, CMake and a Windows SDK
providing `fxc.exe`. The existing prepared OFXR dependency tree is required;
this helper does not fetch new SDK versions or copy a Python/model runtime.
The normal RIFE setup downloads its separately pinned dependencies for each user.

## Release build

From the source root, substitute your existing SDK location:

```powershell
cmake -S . -B build-rife -G "Visual Studio 17 2022" -A x64 `
  -DXRFG_DEPENDENCY_ROOT="C:/ofxr-deps"
cmake --build build-rife --config Release --parallel 4 --target `
  OFXRBridgeTray XR_APILAYER_XRFrameBridge_diagnostic OFXR_vulkan_queue_layer `
  xrfg_dlssg_forwarder xrfg_neural_forwarder
```

To reuse already prepared SafetyHook/Zydis sources, pass their existing directories
using `FETCHCONTENT_SOURCE_DIR_OFXR_SAFETYHOOK` and `FETCHCONTENT_SOURCE_DIR_ZYDIS`.
Do not duplicate dependency or runtime trees per numbered build.

## Installer checks

For the five targeted installer checks, configure the existing runtime path
without installing or downloading another runtime:

```powershell
cmake -S . -B build-rife -DXRFG_TEST_RIFE_GPU=ON `
  -DXRFG_TEST_RIFE_RUNTIME="C:/existing-ofxr-runtime/rife"
cmake --build build-rife --config Release --parallel 4 --target `
  xrfg_tray_lifecycle_tests xrfg_rife_runtime_locator_tests xrfg_rife_install_locator_tests
ctest --test-dir build-rife -C Release --output-on-failure `
  -R "^xrfg_(rife_runtime_locator_tests|rife_runtime_setup|check_rife_ui|rife_install_locator|rife_installer_backend)$"
```

These checks cover actual Win32 tray/helper processes, structured status handling,
runtime selection, Windows shared/exclusive leases, source-overlay integrity,
failure/rollback/restart recovery and cache ownership. They do not reinstall the
large runtime or claim a new game/headset result. `Check RIFE` can separately run
a small actual NVIDIA CUDA inference against an existing runtime.

## Package the complete payload

Building the tray alone does **not** supply the installer overlay, scripts or CRTs.
After building matching binaries and source, run:

```powershell
.\tools\Package-RifeBuild.ps1 -BuildDir .\build-rife -OutDir .\packages -Version 848
```

The script discovers the pinned Microsoft.VC143.CRT **14.44.35112** folder in the
installed Visual Studio redist directory, or accepts it explicitly:

```powershell
.\tools\Package-RifeBuild.ps1 -BuildDir .\build-rife -OutDir .\packages `
  -Version 848 -CrtSource "C:/pinned-redist/x64/Microsoft.VC143.CRT"
```

Every CRT DLL is checked against the existing runtime lock. A missing pinned
version is an error; no mutable `latest` redistributable is substituted. Its
original Microsoft license is included from `licenses/MSVC_CRT_LICENSE.rtf`.

The helper copies the five first-party Release binaries, three pinned providers,
the complete 17-file same-release RIFE overlay and manifest, four setup tools,
ten CRT DLLs, notices and documentation. It verifies binary architecture/version,
all source/model-overlay pins, every file copy and ZIP member, and writes
`SHA256SUMS.txt`, `BUILD_PROVENANCE.json` and a ZIP SHA256 receipt. Existing numbered
packages are preserved. No Python environment, model weights, engine cache, user
INI, PDB or log is bundled. `-SourceDir` exists for source-archive or packaging
validation overrides; it defaults to the directory containing the script's
`tools` folder.

Run `python tools/build_rife_overlay_manifest.py` to verify the overlay manifest.
Only after deliberately changing/reviewing the shipped RIFE source should a
maintainer regenerate it with `--write`. The runtime installer never regenerates
that manifest and never downloads a newer patch overlay.

The packaging helper does not sign binaries. If signing is required, the matching
build must be signed by the certificate holder before packaging. Preserve those
exact executable bytes and the release tag identifying their source.
