# V906: depth-aware current-vector transport for inferred screen attachment

Owner V905 F8 Tsushima PID15368,20261107-150143-216-1,normal OSR+OFXR2x,
Director,actual depth dilation0, current eligibility775 at9622/9623. Exact depth
snapshots verified: ownF2=sourceF1, nativeF2=ownF1; previous colourF2=sourceF1.
CPU reconstruction of current path matches actual selected RGB99.9-100% in head/
right arm/accepted leg samples. This is genuine shader entry, not only a CPU gate.

Remaining issue: the inferred screen-lock heuristic rejected moving followed
character surfaces and returned to stale history-motion/colour arbitration.
41x41 sample interiors: upperback58% rejected by screen lock, middleback/leftarm
100%,leftleg94%,skirt99%; head/rightarm mostly new. Old/new deformations therefore
coexisted on one body. These are local samples, not percentages of the whole body.

Owner clarified: use the CURRENT other-eye vectors, depth-reproject them per
point to the destination, apply to destination eye's previous colour. No delta or
mix with previous vectors. V906 retains the V905 world-coordinate current pull;
inferred screen attachment now ALSO consumes the current field, in camera-local
coordinates. Transform vector origin through current source->peer projection,
and endpoint through native-old inverse projection followed by current stereo
relative camera. Each endpoint uses its OWN depth, so disparity is per-point;
changing depth, FOV and rotations are represented by the matrices. Half of the
transferred native span2 displacement looks up the peer's own previous colour.
Intervening-time depth validation retains the same3% tolerance. Hard-near,
invalid data, fringes, true bands and scene cuts keep established fallbacks.

The extra old inverse-projection stereo matrix extends the existing uniform
upload256->320bytes/16->20float4s (allocation granularity unchanged); no new
texture/image/pass/swapchain/wait/readback. Root constants remain62DWORDs. Same
per-eye identity gate from V905. Neither previous-motion delta, colour blend,
vector-strength tuning nor new sampler/filter is introduced. Generic UEVR does
not enable this current field path.

Cheap map now explicitly records the history locked bit instead of inheriting
raw stereo-depth low bits. Unmasked low bits could randomly choose screen-space
rather than projective2x2 expansion. Full quality output is unaffected by that
metadata correction. Attached field uses screen expansion; world uses projective.

CPU camera-local depth transport trial (stable FOV for reconstruction) reduces
left-leg residual against current-other-eye geometric reference from(3,-7)px
NCC.842 to(2,-1)px NCC.930. Arm(-3,2)->(1,0)px NCC.971. Reference is NOT an
independently rendered simultaneous same-eye truth; no headset flicker proof.
Weak matches on repeating armour patterns are not usable offset measurements.
See local tsushima-v905-pid15368 REPLAY/TRANSPORT_TRIAL and diagnostic images.

Production shader tests cover both eyes/qualities, followed-camera nonzero motion,
stationary screen content, opposite directions, changing perspective depth,
nonidentity history projection for cheap metadata, and two depth planes that
MUST yield distinct transported shifts (3px/2px). Existing invariants remain.
Local trial only; headset acceptance pending. Pacing/FG/pose/RIFE/NR/AFW/settings
unchanged; no push. Detailed receipts in local osr-current-screen-motion-v906.
