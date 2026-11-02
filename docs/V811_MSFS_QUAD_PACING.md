# V811: four-view NR and stable temporal guide identity

The retained changes support NR array guides, preserve the association between
each eye and its guide stream, and reset FSR history across guide discontinuities.
The existing publication queue is unchanged.

## Four-view NR array guides

Each NR handle keeps an independent 2D guide snapshot and temporal history.
For a Texture2D array, the snapshot copies the base array slice, including its
mip subresources. The NGX D3D12 resource parameter supplies a resource and XY
origin without an array-slice selector; the implementation does not infer an
eye or slice from handle creation or discovery order.

Only the copied slice's subresources are transitioned and restored. Other
slices and their resource states are untouched. Multisampled guides and
multiplanar array guides remain unsupported. Per-handle retirement and exact
colour-region writeback are preserved, including separate central and
peripheral quad-view handles.

## Stable guide eyes

Changing native evaluation order or using different colour-release watermarks
can otherwise select the opposite eye's stream. A known, still-active stream
remains associated with its eye. Resolution selects a matching snapshot at or
before that colour's release and checks the exact GPU synchronization proof for
the chosen snapshot, including an older retained snapshot.

FSR and Hybrid use this stable identity rule. The rule does not alter their
placement geometry or infer an eye from whichever stream was evaluated first.

## FSR history gaps

The FSR provider tracks the last guide stream, epoch and serial consumed by
each context. It resets generation history when the stream or epoch changes,
or when the newly consumed guide's `previous_serial` does not match the last
serial consumed by that context. Initial and explicitly requested resets are
preserved.

These checks handle concrete temporal discontinuities. They do not establish
that all FSR or Hybrid foveated flicker and image-quality problems are resolved.
The subsequent [guide-chain correction](V813_BUFFER_RELEASE_GUIDE_CHAIN.md)
also preserves the native previous serial for independent eye streams.

## Queue scope and verification

This change retains the existing publication queue and copy synchronization
path. It introduces no additional direct copy queue or image-availability fence
transfer. Buffered preparation and Director scheduling are covered separately
by their respective changes.

Focused regression coverage includes four independent NR views at full and
half resolution, an array-backed depth guide, distinct sentinel slices and
mixed unused-slice states. The checks compare colour/guide pixels, guards and
independent histories. Layer fixtures vary evaluation and release order for
FSR and Hybrid, and actual FSR provider cases exercise continuous, skipped,
changed-stream, changed-epoch and explicit-reset sequences.

These cases verify guide extraction and temporal identity. They do not measure
physical-headset pacing or establish game-specific foveated image quality.
