# V920: OSR with slightly different eye image extents

A V773 diagnostic run delivered 3369x3368 and 3367x3367 eye resources with valid
origin markers. Its recorded per-eye FOVs project to about3368.65x3367.62 and
3366.78x3367.18 native pixels, explaining the different rounded dimensions.
The identical-colour-extent check refused reconstruction before OSR ran.

OSR now permits up to four pixels of per-axis extent difference only for complete
single-slice/single-sample compatible-format images and verified raster-aligned
guide windows. Internal working size is the pair's per-axis maximum. Real input
pixels are copied without scaling or moving; the small added right/bottom border
is edge-clamped. The corresponding internal right/down FOV tangents extend by
exactly that many pixel pitches. Submitted OpenXR FOVs and rectangles are unchanged.
Output copies only the peer's original rectangle, so no original clipping side
is inferred and no original pixel is silently dropped or resampled.

Both eyes must produce the same padded depth/motion grid, or adaptation refuses:
60 means the raster/peer camera contract is unavailable;61 means the two padded
guide grids disagree. This prevents per-eye extent changes resetting history on
every alternating frame. Larger differences, partial/nonzero-origin rectangles,
nonfinite FOVs, incompatible formats/arrays/MSAA and unverified grids remain
refused. Equal-size inputs take their original path. Legacy AFW is unchanged.

Only a handful of border copy regions are added for the unequal-size OSR case.
No additional XR swapchain, full-image resampling pass, CPU/GPU wait, readback,
model change or new worker is added. Diagnostics1340/1341 record physical vs
working colour extents; F8 final-eye dimensions remain the physical output size.

Focused tests replay both directions of the observed pair, verify every original
pixel-centre ray horizontally and vertically, stable1685x1684 guide grids, exact
pixel preservation/clamped borders, equal-size identity and invalid-input refusal.
These are geometry/copy-plan checks, not an owner/headset acceptance claim.
