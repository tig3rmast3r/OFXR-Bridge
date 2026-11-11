# V982: AER Hybrid occlusion correction

The Great Circle's generated images showed opposite wall silhouettes in the
two eyes. At a newly visible background edge, composing the current eye's
motion through the preceding opposite-eye image sampled the wall instead of
the background, producing a roughly 55-pixel false displacement.

AER Hybrid now checks current and exact-middle depth before composing an
N1 correspondence. Out-of-bounds or incompatible-depth correspondences are
marked invalid, and Hybrid uses optical flow for those pixels. Valid game
vectors remain unchanged. Native same-eye vectors, DLSS-G and FSR do not
enable this depth check.

F8 diagnostics now retain both eyes' actual Hybrid inputs, generated output,
guide crops and publication metadata with the submission fences.

Validation on V981 (993481b), local hybrid-occlusion r1:

- x64 and x86 builds passed.
- The captured Great Circle wall-row GPU regression reproduces the bad
  vector, rejects it at both mirrored edges, and preserves valid surfaces.
- Existing composition controls, both-eye Hybrid capture/fences, guide
  ownership and isolated native DLSS-G N1/N2 checks passed.
- Full suite skipped at the user's request.
- User confirmed the wall ghosting is completely gone in-game.

Published as one V982 change on V981. Release version remains 0.3.3; IPC ABI remains 97101.
