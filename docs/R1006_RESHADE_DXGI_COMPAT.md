# R1006: SteamVR, stock OptiScaler and late OpenXR ReShade

MSFS2024 on Windows10 can crash at entry into VR when global OpenXR ReShade6.8
loads after the game's D3D12 device. ReShade skips its D3D11on12 proxy for that
unproxied device but still proxies the internal DXGI factory. When D3D11 then
requests a private adapter interface, ReShade treats it as IDXGIAdapter and
calls SetPrivateData on an incompatible vtable (DXGI breakpoint0x80000003).
The exact crash is reproducible with a small D3D12/D3D11on12 program without
OFXR, OptiScaler or a headset session.

The verified stock-OptiScaler SteamVR path now installs a compatibility hook
when global OpenXR ReShade is present. Only factories returned to the system
D3D11 runtime are unwrapped through ReShade's public IID_UnwrappedObject,
then queried again for the originally requested IDXGIFactory version. Game
factories retain ReShade proxies; ordinary game/OptiScaler/Streamline callers
and unknown factory interfaces are untouched. No DLL loading or ReShade
registration/INI changes are needed. Installation occurs synchronously before
private SteamVR delivery starts; the existing discovery monitor also covers
later loading. Capture logs report installation and internal-factory counts.

Regression: original minimal late-load case and stock-OptiScaler late-load
control crash at the same DXGI offset; compatibility path creates and releases
D3D11on12 successfully and keeps the application's ReShade factory proxy.
Three stock-OptiScaler guarded repetitions pass, including OpenXR instance
creation/destruction through MSFS2024's loader with global ReShade retained.
No full MSFS/headset acceptance is claimed by these device tests.

An early-local-copy configuration was rejected because duplicate ReShade
instances suppress its global OpenXR layer. It was rolled back. Keep the
original OptiScaler INI and global ReShade installation when testing R1006.

This is a local unsigned Radeon-lane build based on R1005. It does not import
nightly986/987, modify OSR/history/vector timing, pacing, RIFE or the independent
experimental lane. Stock OptiScaler remains nightly20261112/2fff542e.
