# OpenXR panel support

Adapted from djules75/OFXR-Bridge V508, V509, V510, V511 and V574.
Upstream IDs refer to that project's independent numbering.

Panel identity includes its type, source swapchain, space, atlas rectangle,
array slice and eye visibility. Matching consumes each previous panel once.
A resized rectangle can match only the sole unmatched panel on both sides.
Immutable projection snapshots retain panel identities across private image
ownership and asynchronous workers; synthetic outputs use their own fraction.
REAL VR panels retain their original placement, following upstream V524.

Game/headset acceptance remains pending. No scheduler policy is imported.


Panel contents use independent full-resolution FidelityFX flow, even when the
eye images use another generator. At most two distinct panel swapchains are
generated, one array slice per source; any number of rectangles may share an
atlas. Other layer types, extension chains and unsupported bindings retain
their existing path. A missing panel pair keeps the actual application image.

V510's alpha-aware endpoint selection is adapted to this branch's shader:
try still and neighbouring measured motion at disagreeing edges. A nearby
integer estimate is considered only if its actual RGBA endpoints agree better;
fractional motion is otherwise retained. Bit 7 is panel-only; bit 2 keeps its
existing NVIDIA pose-compensated meaning. Eye-image synthesis is unchanged.

Actual D3D12 tests cover transparent backgrounds, icon centroids, small glyphs,
re-priming, two atlas rectangles, Framework/Director, 3X, FSR, Hybrid and DLSS-G.
Disabling the panel flag reproduces faded edges in the same GPU fixture.
Initial build/fixture failures and GPU-state regression receipts are preserved
locally. Full application/headset acceptance remains pending.

Stationary panel poses retain passthrough descriptor ownership. Known panel
sources retain auxiliary capture from their first armed frame, avoiding an
unnecessary unsafe-composition bypass at startup.
