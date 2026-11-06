# OSR nightly history after V884

These commits publish the retained final changes as a linear series. Intermediate
experiments are not imported as standalone builds. Original source identifiers
are recorded in commit trailers and below; the final runtime, shader and test
sources match the tested V891 checkpoint.

| Public build | Change | Original source |
| --- | --- | --- |
| V886 | Completed native D3D11 and D3D12 ordered F8 frame-generation capture | `c61c7c3aaec11a402ffe31ccc8e14f7fa8085537`, `d302dc0cec05ff5a4e8e007812691fe836e2708f` |
| V890 | Preserve the current visible stereo warp; consult history only for missing surfaces | `2c301294252ee20e141c6a460750ad6f7fed2d74` |
| V891 | Reject historical foreground contamination in revealed background | `2bfa77b6d238b310016339a2b40118cfafb1e1bb` |

V890 retains deterministic history-origin selection from
`0af6dd419bfaec942a39a3af14eec41261d219d0` and residual screen motion for missing
surfaces from `86cd7261a40ca69e660e920233742a9da2353679`. The superseded visible-history
arbitration and selection experiments are absent from the final shader.

For behavior and validation, see [V886](V886_D11_FG_CAPTURE.md),
[V890](V890_OSR_VISIBLE_WARP.md) and [V891](V891_OSR_DISOCCLUSION_DEPTH.md).
