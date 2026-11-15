# R1000: stock OptiScaler + standalone OFXR

Experimental Radeon lane, forked from nightly V985 (b1054bb7785d9be6745095522c3cd3c8f840e546). No Independent pacing changes.

The initial target is DirectX 12 games exposing DLSS SR inputs through stock OptiScaler, which performs FSR upscaling, with OFXR providing AER + Hybrid/FidelityFX or FSR 3.1 frame generation. Select DLSS in the game even though OptiScaler performs FSR. OptiScaler frame generation must remain disabled.

## Changes

- A separate, product-identified OptiScaler NGX interception point; Windows DLLs and unrelated injectors are not accepted merely by filename. Supported loaded proxy names include winmm.dll, OptiScaler.dll, dxgi.dll, version.dll, d3d12.dll and dbghelp.dll. No filesystem scan or DLL injection.
- NGX capture discovery runs without requiring an installed NVIDIA NGX core.
- The replacement SR implementation can own guide capture when no physical NVIDIA DLSS body executes. Existing public Luke camera/guide ownership is preserved.
- Actual successful FidelityFX SDK upscale dispatch is required on the same native command list and output before marking replacement color as complete. A public success during initialization is insufficient. Recursive aliases and nested NVIDIA passthrough cannot claim the production twice. Missing/ambiguous execution fails closed and does not invent vectors.
- Existing eye, creation, dimensions, device, camera, stream, final-color and GPU synchronization checks remain in force. No optical-only fallback is reclassified as successful vector processing.
- R-prefixed tray/settings/manifest/benchmark identity; installed DLLs live in RuntimeLayer/R1000. The control ABI remains 98401. One tray at a time; changing builds requires restarting the game to load its matching layer.

## Paired OptiScaler test package

Official nightly-20261112 asset: OptiScaler_v10.0.0-pre1_20261112.7z, SHA256 c32932ca340ffd605325101077b072209514ed0f110dc8b7ab92486eec1a5d93.
Release source revision: 2fff542e78e257a1e4ff5a1e21a69a166c78c335 (release body last-sha).
https://github.com/optiscaler/OptiScaler-nightly/releases/tag/nightly-20261112
https://github.com/optiscaler/OptiScaler/tree/2fff542e78e257a1e4ff5a1e21a69a166c78c335

The separate ZIP preserves every official payload file and license. Its root main DLL is named winmm.dll to avoid replacing Luke Ross's dxgi.dll; the bytes are unchanged. The original INI and setup scripts are retained under upstream-original, and two explicit test INIs are included. Do not load another copy of OptiScaler through a second proxy/ASI at the same time.

NVIDIA test defaults: DX12 FFX backend, FSR version selected automatically from supported SDK versions, spoofing off, OptiScaler FG off, output scaling/CAS off, information-level asynchronous file log. The actual bundled upscaler DLL is 4.1.2.4555. Its device-less enumeration offers 3.1.6 then 2.3.4; old INI comments about indices are not authoritative. Confirm the active version by name in the OptiScaler overlay/log.

Radeon profile: DXGI/Streamline spoofing enabled; the same FFX upscaler with automatic supported-version selection. On supported Radeon hardware/driver choose the bundled SDK 4.1.x entry by name, not a numeric index. R1000 observes the standalone SDK DLL dispatch; a driver-only/custom upscaler that bypasses that entry is not yet supported by the completion receipt. FSR 4.1 cannot be validated on NVIDIA and is not promised on every Radeon generation. The official DLLs/licenses and Agility SDK shipped upstream are included; no driver, registry or system Python changes are performed.

## First owner test

1. Close the game and previous tray before starting the R1000 tray. This package does not install files automatically.
2. Back up any existing game-local OptiScaler files. Copy the prepared OptiScaler ZIP contents next to the game executable; preserve Luke Ross dxgi.dll and realvr files. If winmm.dll already belongs to something else, do not overwrite it: resolve the proxy chain first.
3. Use the root NVIDIA test INI. Do not run upstream setup_windows.bat for this prepared layout; it can rename/remove files and offer additional downloads.
4. In the game select DLSS upscaling; confirm OptiScaler reports FSR 3.1.x, with its own FG off. Leave NR, OSR/AFW, RIFE and DLSS-G off for this first test. In Luke Ross keep AER enabled.
5. In OFXR choose Hybrid, engine FidelityFX; then compare with FSR 3.1. A valid guide indicator is required: Hybrid can otherwise fall back to optical-only, which is not proof of Radeon vector compatibility.
6. Record F9 briefly, and F8 if artifacts occur. Capture evidence includes optiscaler_sr_hook_installed (slot 5), optiscaler_ffx_dispatch_hooked and optiscaler_sr_completion (ready=1 for a witnessed production).

## Validation boundary

CPU/module identity and WARP shipping-dispatch checks exercise public/native nesting, original guides, exact final color and refusal of missing/wrong dispatch. These are not actual game, headset, Radeon hardware or FSR 4.1 acceptance tests. No game/profile/runtime installation, push, release or signing is performed by this preparation.
