Current shared routing: read [shared Autonomous handoff](../../docs/ofxr-autonomous/README.md). Build with `tools/Build-Autonomous.ps1` from the shared repository root; prepared dependencies are in `.local/ofxr-autonomous-deps` and outputs in `builds/ofxr-autonomous-new`. Earlier laboratory build paths below are historical.

## Fork 0.3.3 Windows candidate

The [Windows candidate workflow](../.github/workflows/windows-candidate.yml) runs
on pushes to `fix/ghost-osr-033`. It prepares pinned SDKs, builds the current
source, runs the native WARP canvas regression plus canvas geometry and optical
flow tests, and overlays the compiled binaries onto the official 0.3.3 package.
The resulting **OFXR-Ghost-033** Actions artifact is unsigned and includes source
identity and SHA-256 checksums. Download it directly on the Windows game PC,
extract separately, and keep an official rollback. Do not use it in anti-cheat
games. Passing GPU tests is not actual headset validation.

## V483 candidate: Luke stereo continuity

Horizon stationary flicker: repeated-eye source gaps collapse AFW stereo; real provider regression reproduced and corrected with current-source stereo warp for that transition. Ghost/Crimson native motion supports signed perspective W; actual game recovery pending. Focused30/30 pass. See docs/V483_LUKE_STEREO.md (V483_LUKE_STEREO.md within docs). FSR/OFA/providers/shaders/pacing retained; V474 fallback.

## V482 candidate: Horizon rolling flicker capture

V481 startup pass-through retained. Ctrl+F8 arms/stops the default-OFF buffer; each F8 saves the previous second and keeps recording, repeatable. Per-eye images/source IDs, AFW contracts and actual OpenXR poses accompany each snapshot. Focused29/29 pass. See docs/V482_LUKE_RING.md (V482_LUKE_RING.md within docs). Capture is diagnostic overhead; visible flicker remains unresolved. V474 fallback; sealed predecessors retained.

## V481 candidate: Horizon startup pass-through

Menus/loading pass until original AER production advances; gameplay duplicate filtering remains. Focused28/28 pass. See docs/V481_LUKE_STARTUP.md (V481_LUKE_STARTUP.md within docs). Owner confirms V480 still flickers: clock defect is absent, but GPU/readback gaps remain. V481 only fixes startup black output; F8 paired image/guide capture is needed for pixel/pose diagnosis. V474 fallback; all sealed predecessors retained.

## V480 candidate: Horizon clock continuity

Fix HFW producer prediction rebasing backward after physical output; fresh-only admission and V479 warm bypass retained. Exact V479 negative reproduces1ns, fix preserves21ms elapsed; focused27/27 pass. See docs/V480_LUKE_CLOCK.md (V480_LUKE_CLOCK.md in docs). Headset flicker validation pending; GPU/readback stalls remain. V474 fallback, V479 preserved. Earlier pointers below are historical.

## V479 candidate: Horizon fresh-content pacing

Pinned HFW AER source admission in all four AFW/OFXR states, no retained output repeats, unique-output FPS and both-Off warm GPU capture bypass. See docs/V479_LUKE_PACING.md (V479_LUKE_PACING.md within docs). Focused26/26 pass; Horizon fluidity/FPS recovery awaits owner run. V474 fallback, V478 preserved; earlier pointers below are historical.

## V478 candidate: Horizon parity with working OptiScaler V33018

HFW physical Forward contract restored selectively; game eye phase/provider preserved. F8 image capture, F9 live logging, F10 menu, Ctrl+Alt+F6 synchronized AFW debug. See docs/V478_LUKE_PARITY.md (V478_LUKE_PARITY.md from docs). Focused24/24 pass; actual headset parity pending. V474 fallback; V475/V476 excluded. Earlier candidate pointers below are historical.

## V477 candidate: Luke standalone native context

V474 plus original-game D12 queue/device handoff and safe delayed Luke initialization. Accepted Luke/AFW fixes retained; see V477_LUKE_CONTEXT.md in docs. Native Vulkan guides remain suspended. No V475/V476 import. Horizon validation pending.

## Current owner scope: V474 selective Djules fixes

V473 plus V439/V441/V442 limited to ordinary Djules and V440/V443 on their
shared D11 bridge/lifecycle mechanisms. Existing shared history captures remain
nonblocking unless explicitly opted in by ordinary Djules input capture. All
other quarantine and resource lifetime protections remain. IPC47101/provider45601.
See docs/V474_DJULES_FIXES.md (or V474_DJULES_FIXES.md from this docs directory).

## Current owner scope: V473 Elliottate live display repair

Runtime display codec now supports values5/6, completes application and acknowledges the real revision. No renderer/provider/FSR/timing change. V472 black-bar changes retained. IPC47101/providerABI45601 retained. See [scope](V473_METER_LIVE.md).

## Current owner scope: V472 post-R19 black-bar timing

R20 inline real-output prediction and R21 actual xrLocateViews pose-time promise measurement selectively adapted to existing Autonomous consumers. ALL behind the same live/default-OFF Reduce black bars during headset rotation flag. No new settings/IPC layout. IPC47101/providerABI45601 retained. FSR/AFW SDK/shaders/providers and output cadence retained; ON changes shared OpenXR prediction/camera timing as requested. Existing meter/counters/auto-arm and R22/R23 retained. See [SCOPE](V472_BLACKBAR_TIMING.md). Older no-timing-import instructions are superseded only for this owner-requested opt-in scope.

## Current owner scope: V471 visual meter and auto arm

R19 Elliottate meter only, displaying existing shared counters/diagnostics in two rows followed by Vectors/Status. Display adds live controller/bottom modes after four corners. Original mode IDs retained. Djules label now medium latency. Normal tray launch auto-arms by explicit owner request; settings-only/preview/watchdog do not. IPC47101/providerABI45601. No synthesis/provider/scheduler changes. See [V471_ELLIOTT_METER.md](V471_ELLIOTT_METER.md). Earlier startup-disarmed wording below is historical and superseded for normal V471 tray launch.

## Current owner scope: V470 Cycle estimated

Replace Presentation estimated with separate pair-slot Cycle estimated; measured Cycle length retained. Same-frame QPC receipts only, no renderer/scheduling change. IPC47001/provider ABI45601. See [V470_CYCLE_ESTIMATE.md](V470_CYCLE_ESTIMATE.md).

## Current owner scope: V469 Stellar Blade native/vector fixes

Targeted R22 guide composition and R23 native object separation. FSR/AFW, Vulkan suspension, existing UI/metrics/cadence preserved. GPU/functional checks required for this renderer change; earlier text-only exceptions no longer apply. See [scope](V469_STELLAR_DLSSG.md).

## Current owner scope: V468 About and branding

Visible name OFXR 0.3.0 (v468); shared settings and tray/About captions only. Credits broaden Djules collaboration and acknowledge Elliotttate DLSS/Hybrid integration. Runtime behavior/profile paths/IPC retained. No new tests for this text-only change. See [scope](V468_ABOUT_BRANDING.md).

This UI-only delivery was built using the prepared V465 static renderer libraries and the targeted build receipt in evidence/UI_BUILD.json. Rebuilt targets: shared settings widgets, flight logger (version metadata), OpenXR layer and tray. A full build from source remains supported by the helper.

## Current owner scope: V467 menu descriptions

Shared desktop/F10 captions and expanded Luke Ross controls only. Existing route/backend profile IDs and runtime behavior are preserved. No GPU suites are rerun for this UI-only change. See [V467 scope](V467_MENU_DESCRIPTIONS.md). IPC46501/provider ABI45601.

## Current owner scope: V466 tray-only menu

V466 changes only the tray arm toggle to two rows, Enable and Disable, with opposite check states and the existing shared toggle command. The package reuses all V465 runtime DLLs unchanged (IPC46501/provider ABI45601). Owner explicitly requests no unnecessary tests; no new GPU/renderer suites are run. See [V466 scope](V466_TRAY_TOGGLE.md).

## Current owner scope: V465 frame-cycle timings

V465 inherits sealed V464 and replaces the displayed GPU render / OFXR delay pair with OFXR time and Cycle length. The latter measures frame production plus all API blocking and held-real residence until downstream acceptance and producer release, using the same Wait ticket. Presentation estimated remains separate. No renderer, shader, FSR guide/provider, route, camera or scheduler change. See [V465 scope](V465_CYCLE_METRICS.md). IPC46501; provider ABI45601. Game/headset timing comparison pending.

## Current owner scope: V464 FSR release-bound guides

V464 inherits sealed V463. The owner explicitly authorizes the targeted ordinary native-D3D12 FSR guide/eye association correction described in [V464 scope](V464_FSR_RELEASE_GUIDES.md). This supersedes historical FSR preservation instructions only for that association. FSR provider, shader math, SDK camera/motion parameters and scheduling remain unchanged. OFXR, native DLSS-G, D11 and Luke/AFW contracts are retained. The owner additionally requests a simpler Hybrid menu: fallback is fixed to FidelityFX and the duplicated fallback controls are removed. R19 stays live and default OFF; Vulkan native guide ingress stays suspended. Game/headset confirmation of the intermittent MSFS/Returnal flicker is pending.

## Current owner scope: V463 Hybrid

V463 inherits sealed V462 and adds selectable Hybrid between OFXR and FSR. R19 camera remains default OFF and live on every route. Existing OFXR/FSR/DLSS-G routes retain their behavior. Hybrid has its own profile and optical-flow fallback; native Vulkan guide capture remains suspended. See docs/V463_HYBRID.md. Earlier no-R19/no-Hybrid constraints below are historical and superseded only by these explicit V462/V463 owner requests.

# Building OFXR Autonomous V466

Build one optimized **Release** candidate for Windows x64 with Visual Studio 2022 BuildTools, its bundled CMake and a Windows SDK containing `fxc.exe`. Runtime diagnostic logging is a tray option; there is no separate diagnostic build.

Use the prepared shared dependency tree. It contains:

- `DLSS/`: official NVIDIA DLSS SDK commit `374959484e79a640feaba44c93ac8cfb0a03f5b5` (310.9.1), including the NGX static libraries and release `nvngx_dlssg.dll`.
- `FidelityFX/sdk/`: pinned v1.1.4 headers and Release libraries `bin/ffx_sdk/ofxr_ffx_backend_dx12_x64.lib` and `ofxr_ffx_opticalflow_x64.lib`. These use the isolated symbol prefix. Modular FSR headers live in `FidelityFX/ffx-api/include/ffx_api/`.
- `OpenVR/include/`, `Streamline/` interface headers and `Vulkan/vulkan-1.lib`.
- `runtime/fsr-optiscaler/ofxr_amd_fidelityfx_framegeneration_dx12.dll` and official AFW beta5 `runtime/PDAFWPlugin.dll`.

Pinned OpenXR, Vulkan and NVIDIA Optical Flow interface headers are kept with the source. CMake fetches pinned SafetyHook v0.6.9 and its dependencies on the first configure. Source/provider notices are in [licenses/ATTRIBUTION.md](../licenses/ATTRIBUTION.md). None of these build inputs requires an OptiScaler checkout or proxy DLL. The helper does not download or reconstruct the prepared vendor dependency tree.

Run from the source root:

```powershell
.\Build-Autonomous.ps1 -TestRoute new -RunTests
```

The laboratory defaults resolve dependencies to `../../shared/ofxr-autonomous/deps` and outputs to `../../shared/ofxr-autonomous/build-v466-new`. For a separate prepared layout:

```powershell
.\Build-Autonomous.ps1 -TestRoute new -DependencyRoot 'C:\ofxr-deps' -BuildRoot 'C:\ofxr-build-v466-new' -RunTests
```

Use `-TestRoute djules` for its additional scheduler checks; use a separate build root when retaining both validation results. Individual SDK roots and the FSR/AFW provider files also have CMake cache variables beginning with `XRFG_`.

The output is `<BuildRoot>/Release/OFXRBridgeTray.exe` with seven DLLs and the runtime INI under `Release/ofxr/`. Keep that directory and the licence notices together when preparing a package. The external NR runtime/model `nvngx_dlssnr.dll` is optional for the build and not included; actual NR evaluation requires it and compatible native SR/RR inputs.

Building and running tests do not activate the tray, register its layers or replace game DLLs. The tray starts disarmed. See [TESTING_IT.md](../TESTING_IT.md) for explicit manual activation and [VALIDATION.md](../VALIDATION.md) for the candidate's validation evidence.

Optional GPU compatibility regression: configure XRFG_TEST_LEGACY_DLSSG_PROVIDER
with a separate real older FG provider. It is test input only and is not shipped.
