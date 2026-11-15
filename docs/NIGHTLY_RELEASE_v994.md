# OFXR 0.3.3 NIGHTLY RELEASE (v994)

## Radeon users

This nightly is for feedback from Radeon users trying **OptiScaler and/or RIFE**.
I tested the OptiScaler path on my NVIDIA GPU, using FSR 3.1 upscaling through
DLSS inputs. That is what I can verify on my own hardware.

**Please test FSR 3.1 first. If it works, try switching to FSR 4.** Availability
of FSR 4 depends on your GPU and driver.

The experimental **Radeon Vulkan RIFE installer** is included. Early tests
suggest RIFE can be demanding on Radeon, so I recommend testing OptiScaler
with Hybrid or FSR frame generation first.

### Setup

1. Install the complete, unmodified [OptiScaler nightly-20261112 package](https://github.com/optiscaler/OptiScaler-nightly/releases/tag/nightly-20261112)
   in the game folder. Use **winmm.dll** to avoid replacing DLLs used by other
   mods, such as Luke Ross.
2. Replace the game's **OptiScaler.ini** with the file included in this ZIP.
   It selects the FSR upscaling backend and disables OptiScaler's own framegen.
3. Open OptiScaler's menu and select **FSR 3.1 by name** for the first test.
   Its version index varies with GPU/driver, so the supplied INI does not force
   a fixed index. After a successful test, try FSR 4 if it is available.
4. Start the **OFXR tray**, then launch the game. Select DLSS in the game so
   OptiScaler receives its inputs; OptiScaler performs the FSR upscaling.
5. Start with **Hybrid + FidelityFX optical flow** or **FSR framegen** in OFXR.

Frame generation is provided by OFXR. Keep OptiScaler's own framegen disabled.
On Radeon, NVIDIA OFA engines (including the NVIDIA Hybrid variants) and DLSS-G
are greyed out. Old profiles using them are constrained to compatible FidelityFX
routes, and automatic tests omit these unavailable NVIDIA routes.

For Radeon RIFE, use **RIFE -> Reinstall RIFE -> AMD Radeon - Vulkan
(experimental)**, or **Install-RIFE-Vulkan.cmd** from this ZIP. Prepare Vulkan
for found games from the tray before testing when possible.

### Games checked through OptiScaler on NVIDIA

- **Ghost of Tsushima:** Luke Ross OSR/AFW and AER frame generation.
- **Microsoft Flight Simulator 2024:** standard Hybrid and FSR frame generation.

Let me know how it goes. If something fails, please include your GPU model,
driver version, selected upscaler, **OFXR logs and OptiScaler.log**.

## Other fixes (also for NVIDIA)

- **OFXR menu with ReShade:** fixed device identity checks that could prevent
  the menu from appearing, including the MSFS2024 + DLSS Ghost Fix setup.
- **ReShade/DXGI compatibility:** added protection against the internal
  D3D11-on-12 adapter crash when global OpenXR ReShade loads late with OptiScaler.
- **RIFE installer:** fixed transient activation/rename locks that could block
  installation or recovery, and added bounded lock diagnostics.
- **VRAM:** reduced output-buffer memory use for compatible native OFXR/Hybrid
  3x/4x routes.
- **Neural Rendering:** fixed pre-SR operation with padded DLSS output textures.
- **3x/4x framegen:** restored compatible native OpenXR routes, including VDXR,
  with correct delivery, Fast-mode output capacity and multiplier reporting.
- **Native Pimax delivery:** isolated the binding queue from pending game work.
- **Stereo guides:** handled compatible rounded/padded packed-stereo inputs
  while preserving the right-eye boundary and rejecting ambiguous matches.
- **OSR-only:** fixed capture/admission when FSR was selected but framegen was off.
- **Luke Ross through OptiScaler:** corrected adjacent-eye motion composition
  and retained valid OSR history across short publication gaps.
