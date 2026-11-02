# V772: GTA V Enhanced with REAL VR FixDLSS

This nightly update changes only GTA V Enhanced's final-colour scope (which
DLSS evaluation may carry the colour origin). Other titles, reconstruction,
frame generation, NR, delivery/pacing and the UI are unchanged.

## Symptom

GTA V Enhanced with REAL VR's `FixDLSS=1` (`RealVR.ini`, `[GTAVE]`) never
engaged AFW or OSR, under SteamVR and VDXR alike: the left eye's origin
decoded, the right eye's never did, no AFW source was selected, and every
generation prepare reported a missing capture. With `FixDLSS=0` AFW and OSR
engaged, but the image ghosted heavily.

## Cause

With `FixDLSS` REAL VR keeps a second SR feature of the same shape and runs
every other eye's evaluation on it, under the game's own public call (in the
run: public creator 4, native creator 2), presumably so each eye keeps its own
DLSS history. The GTA final-colour scope required the native evaluation's
creator to be the public call's, so it rejected the right eye (luke_afw_stage
1196 result -8). The rejection also clears the stream history, so the left
eye's tokens found no guide either (1100 reason 3).

## Change

The scope now accepts a native evaluation on another feature when the creator
is its only difference and that feature is the public one's exact twin:
creation seen, same type, flags, input and output size, on the same command
list and device, with DLSS succeeding and motion vectors and depth present.
Flight log 1252 records accepted twins (b = native creator << 32 | public
creator, c = native identity; the first 8, then one a second). Any other
mismatch is still rejected; 1195, 1193 and 1251 then record which conditions
failed (result bit 0 earlier native claim, 1 already rejected, 2 no frame,
3 creation unseen, 4 creator, 5 type, 6 command list, 7 device, 8 DLSS
result, 9 motion vectors, 10 depth; bits 16+ earlier native claims), the
creators, types and DLSS result, the command lists and the devices.

## Test

GTA V Enhanced under VDXR with `FixDLSS=1` (user headset check, no ghosting
reported): 12029 ready generation prepares, both eyes' origins decoded about
12020 times each, 12111 AFW source selections. Before the change: none.
Every saved GTA capture on the test machine, including a 2026-10-28 V682 run
that engaged, has `FixDLSS=1`; the strict creator check arrived after V682.
