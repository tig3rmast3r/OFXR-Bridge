# V994: Radeon route availability and nightly package

Physical rendering-adapter identity is obtained once through system DXCore,
outside frame submission. OptiScaler DXGI/User32 spoofing is not used for this
decision. The session LUID selects the adapter on mixed-GPU systems; before
connection a system with NVIDIA keeps NVIDIA choices available. Unknown
hardware leaves availability unchanged.

Both desktop and headset panels disable NVIDIA OFA, NVIDIA Hybrid engines and
DLSS-G on Radeon. Saved unavailable OFXR engines use FidelityFX; saved DLSS-G
uses Hybrid FidelityFX. RIFE's internal backend enum is preserved. Unavailable
NVIDIA routes are omitted from Radeon benchmarks. Settings/readback policy also
covers profile and automatic changes. RuntimeStatus adds the physical PCI
vendor; matching tray/layer use ABI99401.

This nightly includes selected R1000/R1001, R1002+R1003, R1005, R1006 and the
Independent V1132 ReShade menu fix/tests atop nightly987. R1004 preflight,
Independent pacing/metrics and R-lane runtime/CSV names are excluded. Nightly986
VRAM and987 NR padding remain. README.md is byte-identical to987.

The OptiScaler.ini targets stock nightly20261112 FFX upscaling with DLSS inputs
and OptiScaler FG off. It does not assume a universal FSR version index: select
FSR3.1 by name first. Optional Radeon RIFE installation uses the retained pinned
Vulkan payload, without GPU driver or global Python changes.
