# V771: retire idle Luke colour-origin readers

This nightly update changes only how the Luke Ross colour-origin reader keeps
its per-queue readers. Marker layout, decoding, reconstruction, frame
generation, NR, delivery/pacing and the UI are unchanged.

## Symptom

After changing resolution or DLSS mode in game a few times, AFW and OSR stopped
engaging for the rest of the process. Every colour-origin read in both eyes
failed with `0x80070008` (`ERROR_NOT_ENOUGH_MEMORY`) and every generation
prepare reported a missing capture. Restarting the game recovered. VRAM was
not short (GTA V Enhanced under VDXR: 6-9 GB used of a 30.7 GB budget).

## Cause

The reader keeps one readback reader per D3D12 queue, at most 8, and never
released one. REAL VR recreates the OpenXR session, and with it the queue, on
every resolution or DLSS mode change: the GTA V Enhanced run had 10 sessions in
about 5 minutes. The ninth queue found the cache full, and `get_reader`
refused it from then on. The old readers also kept their dead queues alive.

## Change

When the cache is full, the least recently used reader that nobody holds and
whose last copy has completed (or whose device is removed) is retired, and the
new queue gets a reader. Flight log luke_afw_stage 1194 records each
retirement (b = retired queue, c = ms since its last use). Below 8 queues
nothing changes. If no reader is idle, the read still fails as before.

## Test

GTA V Enhanced and Cyberpunk 2077 engage AFW and OSR as before (fewer than 8
sessions, so no retirement). A run with 9 or more resolution/DLSS changes in
one process has not been checked in a headset yet: 1194 should appear and the
origin reads should keep succeeding. A retired reader only loses its learned
marker rows and bar measurement, which are relearned on the next reads.
