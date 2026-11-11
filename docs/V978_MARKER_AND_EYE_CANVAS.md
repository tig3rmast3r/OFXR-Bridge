# V978: adaptive existing marker and asymmetric OSR eye canvas

Based on Djules V977; retains V972?977 execution, token-fit, capability, handoff
and global framegen-off fixes. Release version remains0.3.3; IPC ABI97101.

## Marker

No additional copies. Changes only the existing float marker's intensity;
original title geometry, coverage, scale floors and integer producers remain.
Initial calibration requires a completed GPU copy/search plus a localized nearly
black barcode footprint in the expected read ROI. Bootstrap additionally requires
weak intact Manchester sync on at least3rows, or the same footprint previously
localized by full valid CRC at unchanged geometry. All-black scene/ROI, partial
crop/overlay, wrong-size rectangle, strong corrupt marker, GPU/pending/ambiguous
results do not qualify. A fully erased first-ever marker with no localization
or residual sync is UNKNOWN, not guessed as a recognized marker.

At least16 qualifying observations over2seconds start the check. Raise intensity
by2 each500ms with8misses per step, preserving existing HDR unit shift cap8.
Only both eyes' full CRCs from fresh current-epoch stamped tokens can establish
a level:3new observations over200ms after500ms settling. Keep it in memory for
the game process/session, including resolution changes; no profile writes.

After a successful brightness adjustment, repeated generic non-ambiguous CRC
loss may recheck from normal too: this permits recovery from exposure/bloom.
A failed initial check returns normal; failed recheck restores the last working
level. Hard timeout15seconds, failed-check cooldown30seconds. GPU errors never
count as contrast failure. No frame-generation admission/CRC relaxation.

No new readback, scanning region, thread or sleeps. Initial dark classification
samples only the already-mapped ROI, throttled to10Hz during CRC loss. Immutable
per-level uploads stay cached; in-flight resources are not rewritten.
Flight stage1433 reports level transitions;1434 reports initial dark evidence.

## Unequal eye dimensions

OSR accepts bounded canvas padding up to32px and1percent (legacy4px retained),
including peer3651x3653/3669x3671. Original pixel centres retain their rays;
only private canvas right/bottom storage extends. Original XR FOV/rect submission
stays unchanged; no guessed crop side, image shift or stretch. Bounded copy plan
covers every padded pixel, with source-edge replication. OSR Cheap uses its
existing full-size scratch before downscaling the padded image. No new swapchain.

## Validation scope

Focused CPU policy/codec/dark-classification/canvas tests and actual captured dark
bands, x64/x86 build and package checks are recorded with the local build receipt.
Headset image, bloom and peer game acceptance remain live tests; not claimed by
CPU fixtures. No general game/pacing benchmark matrix.
