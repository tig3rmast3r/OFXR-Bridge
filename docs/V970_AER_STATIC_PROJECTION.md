# V970: AER scene selection with additional static projections

V969 excluded immutable OpenXR projections from temporal capture requirements.
The AER path still required exactly one projection and corrected cameras in
projection zero. This completes the change for AER framegen.

AER now selects one mutable stereo scene from its validated resource mappings.
Immutable projections may precede or follow it and remain in both output frames.
The same scene index governs source identity, fresh/held eye cameras, interpolated
cameras, paired endpoint metadata and native guide joins. Static projections do
not require scene motion/depth guides. Their own subimages/cameras are retained.

This uses OpenXR static-image flags and actual mapped references, never a game
name, image-size heuristic or layer-order guess. Multiple mutable scenes, missing
eye references, missing mutable captures and invalid bounds remain rejected.
There is no image-freshness detector: release/capture identity alone still cannot
prove whether a mod recopied old pixels into a newly released resource.

Tests cover packed/split/array scenes, both static layer orders, swap, paired and
alternating sources, held polls and unchanged static metadata. The shipping path
also runs with D3D11 and native D3D12 WARP resources, actual synthesis, mocked XR
delivery, exact stereo pixels and coherent cameras. Native guide ingress covers
plain/static-first/static-last scenes with real protected N1/N2 publications.
These fixtures do not establish Ghostwire headset appearance/performance or solve
the separately observed UEVR stack overflow. The AER swap control remains Advanced.

V967 accepted GTA guide ownership and V968 RIFE cadence are retained. No provider,
runtime installer, worker/model, GPU driver, profile or live deployment changes.
