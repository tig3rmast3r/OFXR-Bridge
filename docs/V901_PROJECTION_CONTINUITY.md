# V901: retain recurring projection sources and their temporal continuity

Puntloos's Skyrim capture alternates separate-eye and packed-eye projection
swapchains. Retiring every source absent from the current frame repeatedly
rebuilt generation resources. Retaining those resources exposed a second issue:
each layout switch still triggered a global projection-change quarantine.

Recently projected sources now remain admitted for two seconds. Idle sources
retire through the existing drained arming path, including after a layout
settles. Never-projected auxiliary cleanup and Luke/panel lifetime rules remain.
This retains more GPU memory temporarily than immediate retirement; existing
allocation-refusal handling remains in force.

The private NVIDIA OFXR/Hybrid independent director additionally remembers up
to four disjoint stereo projection streams. Each stream retains its own temporal
metadata and anchor sequence, with weak history references and a two-second
expiry. Switching drains synthesis/presentation before selecting a compatible
stream whose capture owner, serial and fence have advanced. Images from
different streams are never interpolated together. Cold/expired streams,
capture gaps and real continuity resets prime again. FSR, native DLSS-G, RIFE,
AFW and legacy director continuity retain their existing paths.

## Validation

The V899-based signed candidate passed 73 focused CTest entries and four
post-sign DLL call-chain checks. Julien reports no issues in CP77 and Skyrim
on 2026-11-07. Puntloos's specific modded Skyrim headset result remains pending.

GPU fixtures cover D3D11/D3D12 OFXR and Hybrid, alternating split/packed sources,
per-stream pairing, pixel isolation, capture gaps, expiry, global reset and
teardown. Resource-lifetime tests cover 120 layout alternations, stable-layout
idle retirement and readmission. The Hybrid fixture now selects the requested
backend so its D3D11 and D3D12 cases exercise the intended routes.

V901 rebases this change onto V900, preserving its CPU meter and IPC ABI 90001.
The fresh V901 x64 Release build passed all 83 focused checks (the previous
73 plus ten CPU-meter checks). All five owned binaries received valid
timestamped Authenticode signatures; four DLL call-chain checks passed again
after signing. Build logs and package hashes accompany the local release.
No new full-suite baseline is claimed. No layer installation or arming was
performed, and packages exclude the NVIDIA NR model.
