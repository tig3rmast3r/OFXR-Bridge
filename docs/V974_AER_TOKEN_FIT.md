# V974: Recover CRC-valid AER colour tokens after chromatic postprocessing

Preserve the original decoder fit first, then refine partial midpoint geometry within bounded local phase and pitch offsets. Require every sync and payload bit, a nonzero token and the complete CRC. RGB-capable decoding checks conflicting channel identities even when luminance succeeds and preserves luminance-only row-window evidence. No missing token is inferred from a nearby publication.

Validation: Five exact captured GTA rows reproduce the previous rejection and now decode correctly. Codec, chromatic/spatial ambiguity and GTA/Outlaws GPU checks pass. User confirmed stable GTA Hybrid frame generation.

The full test suite was deliberately not run at the owner's request.
This change was accepted in local builds on the V971 baseline.
The complete V972-V977 assembly is built and packaged as V977.
