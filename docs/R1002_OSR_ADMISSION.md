# R1002: OSR-only with an inactive framegen selection

Ghost of Tsushima R1001 owner acceptance: AER with Hybrid/FidelityFX and FSR now works after the original N-1 guide correction. In the subsequent run, OSR alone is selected with OFXR frame generation disabled, while FSR remains the saved provider. The old stereo admission gate still runs and rejects every frame before OSR reconstruction.

Only active FSR or DLSS-G frame generation now requests native stereo guide admission. Switching FG off also clears the FSR admission recovery state. Retained provider selection no longer blocks OSR-only. Active FG guide checks are unchanged; no invented peer-eye depth or motion is supplied to an OSR-generated eye. Original N-1 OptiScaler and native NVIDIA title contracts from R1001 are preserved.

For the first trial use Ghost of Tsushima, OptiScaler FFX input interception off, OptiScaler FG off, OFXR framegen off, Luke Ross OSR on. Live OSR quality still needs owner validation. OSR plus FSR FG is not newly enabled by this correction: FSR requires guides matching both final eye colors. Test normal non-AER FG separately in a DirectX 12 native-stereo game, such as MSFS2024 without foveated rendering or NR, selecting DLSS in the game and FSR upscaling in OptiScaler.

Local lane only; no installation, profile edits or push. ABI98401 unchanged.

Validation: 15 focused regression checks pass (OptiScaler/native guide capture, OSR/AFW output ownership, FSR N-1/N-2 guide endpoints and tray identity). Two additional inline/pipelined FSR recovery fixtures fail the same dispatch/recovery assertion on both R1001 and R1002; retained as pre-existing, not declared fixed. An attempted single-array OSR regression fixture never reached OSR's split-swapchain admission, so it was discarded and is not evidence of this live fix. Runtime admission correction is confirmed by source review against the owner's decoded settings/log; live OSR acceptance is pending.
