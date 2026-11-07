# V899: RIFE readiness and bounded allocation recovery

A benchmark now waits for a successful inference receipt newer than the session's
actual configuration boundary, optimized TensorRT state and ready generation.
Acknowledging settings alone no longer starts the settle/measurement interval.
The existing optimization wait and Stop remain available; missing startup is
bounded by the existing 60-second worker handshake plus 5 seconds of margin.

The owner Ghostwire V898 run completed all 97 trials. Its later manual RIFE switch
left a retained IPC error `0x8007000E`, RIFE 2X High, Director, Buffer, Fast off.
All recorded RIFE inference calls succeeded before that allocation failure.
This proves an allocation refusal, not a Python crash or exhaustion of physical
system RAM specifically. Do not equate E_OUTOFMEMORY with one memory domain.

A healthy-device RIFE allocation refusal gets one retry per requested provider
revision. At the existing excluded frame boundary, pending presentations drain
and old generator working sets are retired together before fresh admission.
Application images and source histories remain. Structural and native-budget
refusals are not reopened; device loss and repeated allocation failure remain
errors. No unbounded retries, automatic quality downgrade or driver changes.

The owner also reports the entire meter image/countdown frozen in 3X. V898 shows
successful uploads around 4 Hz and matching submitted UI handles; the root cause
is still under investigation. V899 retains the exact meter snapshot revision,
benchmark index/start time and raster clock in F9, to distinguish stale display
data from downstream image retention. A focused GPU fixture verifies independent
changing UI pixels through buffered 2X/3X/4X delivery. This is not a claim that the
specific Ghostwire headset symptom is fixed.

F9 retains initialization failures and the memory state at an allocation refusal:
physical RAM available/total, system commit headroom/limit, process private commit
and working set, plus adapter-local usage/budget. These snapshots distinguish
memory domains on a future failure instead of guessing from E_OUTOFMEMORY.
