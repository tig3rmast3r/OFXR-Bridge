# OFXR Bridge

OFXR Bridge is an experimental OpenXR API layer that inserts an optical-flow
generated frame between two rendered frames.

Current pre-release: **v0.1.0 (internal build V059)**.

> [!WARNING]
> This is a pre-release. It may not work with your game, VR mod, GPU or OpenXR
> runtime. It may produce visual artifacts, fail to activate, freeze the game
> or cause a crash. Use it at your own risk.

> [!IMPORTANT]
> The NVIDIA backend requires an NVIDIA Turing-generation GPU or newer with
> Optical Flow hardware support. TU117-based cards, including the GTX 1650,
> are not supported. RTX 20/30/40-series cards and GTX 1660-family cards are
> supported with a compatible NVIDIA driver. Older Pascal cards such as the
> GTX 10 series are not supported. FidelityFX remains available on other GPUs.

> [!TIP]
> A typical real-world result is a **30–50% frame-rate increase** when using
> FidelityFX, or NVIDIA Medium with NVIDIA OFA at 50% resolution. Actual results
> vary by game, GPU, resolution and base frame rate.

The current build provides:

- AMD FidelityFX Optical Flow (default)
- NVIDIA Optical Flow with Medium and Slow presets
- 100%, 75% and 50% NVIDIA optical-flow calculation scales
- manual persistent OpenXR Arm/Disarm from a tray icon
- an optional bridge flight recorder for diagnostics

OFXR Bridge uses color-only optical flow. It does not receive game motion
vectors or depth, so artifacts around moving objects, disocclusions and head
rotation are still possible.

## Installation and use

1. Download the latest pre-release archive from GitHub Releases.
2. Extract the complete archive to a writable folder.
3. Run `OFXRBridgeTray.exe`.
4. Right-click the tray icon and select the optical-flow backend and options.
5. Select **Arm bridge until manual disarm**.
6. Start the game normally. For injectors such as UEVR, arm OFXR Bridge before
   starting the game and leave it armed while the VR mod is injected.
7. Select **Disarm bridge** or close the tray application when finished.

For supported NVIDIA GPUs, the suggested starting configuration is **NVIDIA
Medium** with **50% NVIDIA OFA resolution**. It should provide a decent
performance boost with minimal visual-quality loss. Running NVIDIA OFA at
100% resolution is usually too expensive and often produces only a small or
negligible net performance gain, so it is not recommended for normal use.

> [!NOTE]
> Some FPS counters, including xrFPS in certain setups, measure the original
> application frames upstream of OFXR. While frame generation is active, they
> may therefore display roughly half the frames actually being submitted to
> the headset. This does not necessarily mean that OFXR is inactive.

Do not run the game as administrator unless the tray is running at the same
integrity level. OFXR Bridge does not replace your active OpenXR runtime.

FidelityFX is the most performing one but will produce artifacts during headset rotation in dark areas, this is known and cannot be avoided.

### What the tray changes on your PC

When you select **Arm**, the tray copies the versioned OFXR layer and its
configuration into `%LOCALAPPDATA%\OFXR Bridge`, creates an absolute-path
OpenXR implicit-layer manifest and registers that manifest for the current
Windows user. It does not inject a DLL into the game, replace game files or
replace the active OpenXR runtime.

Selecting **Disarm** removes the exact OpenXR registration. Closing the tray
also disarms it, with a watchdog providing cleanup if the tray exits
unexpectedly. After disarming and closing the application, no active OFXR hook
or OpenXR registration remains on the system. Versioned cache files, settings
and diagnostic logs may remain under `%LOCALAPPDATA%\OFXR Bridge`, but they are
inert and may be deleted manually at any time.

## Reporting problems

Please report both working and non-working games, rendering problems, freezes
and crashes in [GitHub Issues](https://github.com/tig3rmast3r/OFXR-Bridge/issues)
or on the [Flat2VR Modding Discord](https://discord.gg/flat2vr).

Before reproducing a problem:

1. Right-click the tray icon and enable **Bridge flight recorder**.
2. Start the game and reproduce the problem once.
3. Close the game, then select **Open bridge logs** from the tray.
4. Attach the newest `ofxr-bridge-flight-*.log` file to the issue.
5. Make sure OFXR has worked on your system on at least another game before claiming that is not working for the game you are reporting

Please also include:

- game name and version
- VR mod or injector, if any
- headset and OpenXR runtime
- GPU and driver version
- selected OFXR backend and options
- exact steps and the observed result

If no OFXR log was created, report that too: it usually means the layer was not
loaded or the process stopped before the recorder could start. Game logs and a
crash dump are also useful when available.

The recorder is independent from game and mod logging. It records OpenXR
negotiation, resource eligibility, frame-generation stages, recovery events
and potentially blocked call boundaries. It does not record video or replace a
native crash dump.

## Building from source

See the [Windows build instructions](docs/BUILDING.md).

## License

OFXR Bridge is licensed under [LGPL-3.0-or-later](LICENSE). Third-party
components retain their respective licenses.

## Support

If you find OFXR Bridge useful and want to support its development, you can
[support the project on Ko-fi](https://ko-fi.com/tig3rmast3r).
