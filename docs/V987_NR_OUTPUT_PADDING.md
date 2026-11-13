# V987: Neural Rendering with padded DLSS output

Pre-SR Neural Rendering now accepts DLSS output textures with alignment padding,
including the layout captured in DetNoir with UEVR. Both eyes use their active
DLSS output dimensions when validating full-resolution motion vectors.
Existing per-eye color and depth regions remain separate.

This update retains the V986 native multiframe VRAM improvements and V985 RIFE
installer recovery. Use matching V987 tray and layer files. Control ABI remains98401.
