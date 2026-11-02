# V840: mirror from the unresolved band boundary

GTA V Enhanced F8 captures of 2026-11-02 show a second fragment of the FIRE DOOR
sign beside the character's head in the reconstructed right eye. A trace of 119
bright fragment pixels matches the old row-wise mirror exactly, byte for byte.
For example, destination x1430 samples x1504 around the old edge x1467.

The old inpaint scan stopped only at TagStereo. It therefore crossed valid green
same-eye history and put the mirror axis at that band's outer stereo boundary.
Green pixels themselves retained their colour, but their width incorrectly added
twice that distance to the mirror lookup for the purple holes beside them.

The scan now walks only holes and rejected/moved pixels. Both valid stereo and
valid history stop it. History pixels pass through bit-exactly, with their existing
green debug tint. Only unresolved pixels use fill. Reflection starts at the shared
boundary between the purple run and the first valid background pixel: the nearest
hole samples its nearest background neighbour. Farther holes progress outward
into the background. Existing nearer-surface protection and complex-hole fallback
remain. This is an anchor correction, with no new source-confidence classifier,
eye/direction heuristic, global dilation, or change to stereo/history resolve.

GPU tests cover both horizontal sides, valid stereo/history borders and holes/
rejected history, checking all valid pixels bit-exactly. Frozen colour/Resolved
crops from the reported F8s are replayed through the compiled shipping inpaint
shader. This isolates the fill stage; it is not a new live game capture or proof
of correctness in every possible scene.

RIFE, its V839 pacing changes and Advanced-only model menu, NR, OSR pose/scene
boundary handling, frame delivery, settings and defaults are preserved. No
additional textures, dispatches, synchronization or live runtime replacement.
