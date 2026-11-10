# V963: selected AER provider and cropped Luke guides

Luke's D3D11 bridge forced its primary generator to OFXR both at creation and
on control revisions, including when AER selected FSR or DLSS-G. Preserve that
AFW transport policy only outside raw AER framegen; AER now creates and switches
to the actual selected provider. Post-AFW/source-copy helpers remain OFXR.

Exact colour tokens prove which production supplied the guides, but not their
placement within the submitted image. GTA captures have a native3856x3088 SR
output and a centered3370x3088 XR eye: sampling from native x0 puts the guide
silhouette243 output pixels to the right of the corresponding colour edge.

Retain complete native grids during composition of the two original N1 vector
fields. Then place motion/depth sampling windows into the XR eye using the
native projection and submitted FOV. Prime and subsequent same-eye pairs retain
that mapping; the source metadata/resources, logical eyes and temporal keys are
not mutated. Unknown unequal-extent projection geometry declines the pair.
Luke AER FSR keeps its existing bounded whole-texel copy policy for the resulting
fractional camera window. Other FSR integrations retain strict crop validation.

F9's persistent benchmark sidecar now retains already-produced native DLSS-G
provider/evaluation records, so actual provider activity is observable even
after the circular flight log overwrites the relevant interval. No new GPU
readback or always-on debug pass was added.

Validation distinguishes CPU mapping/routing and GPU fixtures from owner GTA
headset acceptance. A visually correct earlier run is not proof of native
vector synthesis; the owner reproduced the defect on both V930 and V962.
