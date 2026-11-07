# V913: GTA RGB origin-reader CPU cost

Base V912 165158344525ad1414243d412b835b92190d79cd.

GTA can retain a CRC-valid marker only in individual colour channels. That
fallback decoded every row/channel on the caller and omitted the valid band row
coordinates. The reader could never learn its reduced window. The V912 run
contains approximately 20 ms of CPU decoding per read, while the captured OSR
GPU work is below 1 ms. This is not a diagnostic shader left active.

RGB recovery now reports the longest valid row run, like luminance. Subsequent
reads follow the existing 16-row margin and relearn immediately after a miss or
geometry change. All three channels and every row in the selected scan retain
full sync, payload, CRC and conflicting-token rejection. RGB rows share the same
three sleeping workers plus caller already used for luminance; no new thread
pool, GPU work, waits, asynchronous image ownership or guessed publication.

V912's HDR marker contrast, undilated OSR depth and disocclusion/head rotation
corrections are preserved. No shader, pose, RIFE, framegen, NR or pacing change.
Local unsigned test build; in-game FPS recovery requires owner confirmation.
