# V979: keep AER DLSS-G running across valid clipping-distance changes

Camera movement in The Last of Us Part I could repeatedly send AER DLSS-G
into bypass even while both colour markers decoded correctly. Native guide
admission required identical near/far clipping distances in adjacent frames;
legitimate changes were reported as synthesis busy.

Accept individually valid clipping distances with unchanged reversed-depth and
infinite-depth conventions. When near/far changes, reseed NGX with the previous
colour aligned to the current camera. Describe this colour-only reset with the
current frame's projection, as for the following actual evaluation. Reset does
not consume the previous depth/motion. Keep stream, epoch, chronology, ownership,
resource, viewport, finite-plane and depth-convention checks.

Native guide refusals now report native stereo unavailable instead of busy.
Flight stage 1435 records accepted clipping changes (result = eye; b = old/new
near float bits; c = old/new far float bits). Stage 1436 records rejected depth
conventions (b = old/new reversed-depth flags; c = old/new infinite-depth flags).
Each old value occupies the upper 32 bits, each new value the lower 32 bits.
V978 adaptive marker brightness remains unchanged. Version 0.3.3; IPC ABI 97101.

## Validation

- x64/x86 shipping builds passed.
- Actual NVIDIA GPU tests passed for full/cropped stereo output, current-frame
  projection on reset and evaluation, changing clipping distances across AER
  N1/N2 histories, exact held-eye pixels and unchanged-clipping controls.
- Reversed/infinite-depth changes, NaN and invalid near/far ranges remain refused.
- V978 brightness policy, dark classification, eye canvas and colour codec checks
  passed. Full suite deliberately skipped at the user's request.
- Owner reports the local V978-based test build fixed movement dropouts. TLOU
  VDXR run pid51732 handled 1721 clipping changes with no previous detail-8 or
  synthesis-busy refusals. Separate marker loss and missing captures remain;
  brightness adjustment did not activate during that run.

The guide-policy change is shared by native DLSS-G consumers; live acceptance
here covers TLOU VDXR, not a new all-game or all-runtime validation matrix.
