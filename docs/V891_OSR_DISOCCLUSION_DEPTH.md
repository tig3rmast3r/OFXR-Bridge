# V891: keep historical foreground out of revealed OSR background

A bright detached contour could remain beside narrow objects such as a sword or
scabbard. When the current stereo geometry identified background but rejected a
contaminated colour edge, a historical foreground sample could fill that band.

OSR now retains the resolved background depth at rejected source-colour edges.
It prefers a camera-reprojected sample of the same eye's historical background,
without applying the foreground object's motion offset, and requires compatible
depth. For genuine geometry holes, a bounded horizontal depth search can identify
newly revealed background and reject historical foreground. If no compatible
background is available, the existing inpainting remains responsible for filling.

The change uses existing depth and edge parameters. It adds no global depth
dilation, slider, texture or rendering pass. Valid current-warp pixels remain
untouched, as required by V890. Frame generation, pacing, NR, RIFE and pose
handling are unchanged.

## Validation

All nine focused OSR tests pass. The visible-warp test now covers 62 cases across
both eyes, both quality modes, directions, missing regions, source-colour halos
and mixed foreground-colour/background-depth samples. New cases reproduce the
contour failure with the V890 shader and pass with V891.

A real captured-frame replay preserves all 9,252,176 valid current pixels exactly;
changes occur only in missing regions. Paired GPU replay medians add approximately
0.01 ms. This is a focused replay measurement, not a game benchmark. The contour
correction was subsequently confirmed in VR.
