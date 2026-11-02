# V685 GTA original-source and composition-layer diagnostic

Local diagnostic candidate, 2026-10-26. This does not claim a GTA rendering fix.
Source control now uses an isolated branch of the public OFXR-Bridge repository;
the retired private development Git is not used. Build/package/evidence output
remains under the retained ofxr-development directory.

## Missing evidence identified in V684

The automatic V684 capture did run, at 21:31:45.191 in PID43844. It caught the
first persistent missing-token state at 44.03-degree native vertical FOV, not
the later 50-degree state. All six captured projection source images are
exactly RGB zero with alpha 255. Four completed native DLSS colors contain
scene content and exact CRC tokens 2003 through 2006, eight valid rows each.
There are no AFW evaluations in that snapshot.

The retained frame log shows two submitted application layers and one projection
layer. The current capture cannot identify the second layer or photograph its
application image. It also cannot independently establish whether black pixels
originate in the application or the AFW history/interop path. A flat scripted
presentation, another layer and a capture defect must not be conflated.

## Diagnostic addition

- During an existing full F8 snapshot, capture one application-side shared
  color image per D11-bridged application swapchain, including nonprojection
  sources. The stage is `xr_application_original`; its serial is the actual
  acquired image index, not an inferred producer frame ID. Capture runs after
  the original bridge release, on that bridge's own ordered queue, before the
  existing read-retirement fence. It does not read the AFW history mirror or
  the downstream runtime image. Resolve/copy paths are represented by their
  application-side shared result; GTA's measured single-sample path is direct.
- Record submitted layer types, flags, exact source swapchains, image rectangles
  and array indices. Quad layers additionally record size and eye visibility.
  These records are in `xr-timeline.tsv` and do not change submitted metadata.
- Original snapshots are capped at one per swapchain and eight swapchains per
  F8 session. Protected/depth inputs are excluded. Existing image/byte bounds,
  recorder fences and source ownership remain in force.

V683's GTA marker offset and V684's F9-only one-shot automatic trigger are
retained. No new automatic captures occur with F9 disabled. Manual F8 remains
available. No normal renderer copies, vector/depth math, AFW provider, camera
profile, Director/pacing, benchmark behavior, controls or settings are changed.
The diagnostic capture can itself cause a brief hitch and is not a performance
benchmark. No game/headset correctness is claimed from the automated tests.

## Validation

Five focused Release checks pass: core, generic/GTA/Outlaws marker checks and
actual AFW terminal capture. The latter now also writes distinct pixels through
the application D11 image, poisons the downstream D12 runtime image, and verifies
that the new snapshot retains the original pixels in both BGRA array slices.
It exercises inactive bypass, bounded original-source claims, existing terminal
pixel/fence/budget checks and the V684 automatic recorder lifecycle.

Next owner run: matching V685 tray/game, same scripted sequence and AFW2x2,
F9 ON. The existing one-shot capture will include the missing original sources
and layer identities. No manual F8 timing or answer about the visual mode is
required. After the fault, allow ten seconds then close normally.

Version 0.3.1 / implementation 685 / IPC 68501 / official AFW beta6 ABI 45601.
This candidate stays local; no live installation, configuration, registration
or public release is performed as part of preparation.
