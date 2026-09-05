# Third-party components

## Khronos OpenXR SDK headers

The project vendors only the four generated headers required by its Windows
layer build under `external/OpenXR-SDK/include/openxr`: `openxr.h`,
`openxr_loader_negotiation.h`, `openxr_platform.h`, and
`openxr_platform_defines.h`. They are copied without modification from Khronos
OpenXR-SDK revision `5267613edf3d937e3d77556a106a65c2f82b25c6`
(`release-1.1.61`) from
<https://github.com/KhronosGroup/OpenXR-SDK>. The files are licensed under
Apache-2.0 OR MIT; both license texts are preserved in
`external/OpenXR-SDK/LICENSES`.

| File | SHA-256 |
| --- | --- |
| `openxr.h` | `75AE85F7B1308D33572908342D1F3BF6D26A57769CC2092AA3163DFEAF1DCBED` |
| `openxr_loader_negotiation.h` | `6CEA662D58721AD77712BCDBC5620426D8C6E509ABF92AF75C9ED28B8DF9DD98` |
| `openxr_platform.h` | `150939F39AAF1159E9F59AB53B5D5863C08D5777EFA13820B5CBCD3562EB068D` |
| `openxr_platform_defines.h` | `7DC30AD27F1082A71F84F5F5ADF10762558FC9FE57FDC7628D796D22BE3B0DBE` |

This local subset makes the project build independently of any Witcher 3 VR
checkout.

## AMD FidelityFX SDK Optical Flow

The build uses the official FidelityFX SDK `v1.1.4` source at commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. Its checkout is intentionally not
committed; `docs/BUILDING.md` explains how to place it under
`external/FidelityFX-SDK-v1.1.4`. The bridge compiles and statically links only
the DX12 backend and Optical Flow 1.1.2 components:
`ffx_backend_dx12_x64.lib` and `ffx_opticalflow_x64.lib`. The checkpoint DLL has
no FidelityFX runtime-DLL dependency.

The SDK source and linked components carry AMD's MIT license; its notice is
preserved in `licenses/AMD-FidelityFX-MIT.txt`. SDK tools are build-time inputs
and are not linked into the bridge DLL.

## NVIDIA Optical Flow SDK interface

`XRFG-V012` uses the two public D3D12 interface headers from NVIDIA Optical
Flow SDK 5.0.7: `nvOpticalFlowCommon.h` and `nvOpticalFlowD3D12.h`. Each header
contains NVIDIA's MIT-style permission notice. The bridge does not link or
redistribute an NVIDIA SDK binary: it loads the display driver's
`nvofapi64.dll` from Windows System32 at runtime and obtains the Optical Flow
API entry points dynamically.

The NVIDIA backend uses one Optical Flow context and serializes the isolated
per-eye jobs through it. Two concurrent OFA contexts previously caused runtime
freezes, while treating both eyes as one repeated atlas produced cross-image
matches. The FidelityFX backend retains its single packed-stereo dispatch. The
full SDK package, programming guide, samples and license agreement are
development inputs and are not bridge release artifacts.

## Candidate: Khronos OpenXR-SDK-Source API layer scaffold

Not copied. `XRFG-V001` through `XRFG-V003` follow the public loader/API-layer
negotiation contract and were cross-checked against the Khronos API dump layer.
If source is imported later, retain its Apache-2.0/MIT notices and exact
revision.
