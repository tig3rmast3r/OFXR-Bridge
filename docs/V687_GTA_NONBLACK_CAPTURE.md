# V687 GTA diagnostic: exclude black exit sources

Local diagnostic candidate, 2026-10-26. This changes automatic snapshot targeting,
not AFW rendering, motion subtraction, token admission, history or pacing.

## Correction to the preceding diagnosis

The V686 automatic snapshot from GTA5_Enhanced PID 49372 contains a quad saying
"Stai uscendo da Grand Theft Auto V. Attendere...". Both original native-D11 eye
images are exactly RGB zero, and the captured AFW/history mirrors match them.
Thus this particular snapshot documents the exit screen. It does not prove
in-game source corruption or failure at a 50-degree camera FOV.

The compact origin log records 4,497 successful individual eye reads, without
missing reads before exiting. Meanwhile some benchmark total-FPS samples become
invalid even while the CRC remains readable. This is a separate observation,
not proof that vectors stop or that AFW synthesis fails. The renderer must not
be changed based on the exit snapshot or an unavailable meter value alone.

## Automatic snapshot targeting

With F9 enabled, the existing paired marker decoder can optionally report any
finite nonzero RGB luma in its existing readback region. Alpha alone does not
qualify. This receipt is not a whole-frame visibility test. It adds no GPU copy,
fence wait, token inference or CRC exception. Diagnostic and ordinary decoding
have identical token admission.

For GTA only, after one valid AFW color/guide selection, 200 ms of unavailable
selection requests the normal bounded snapshot if at least one existing marker
readback region has content. Valid CRC alone does not suppress the request if
its required guide is unavailable. All-black regions reset this diagnostic
episode, preventing the known black exit source from consuming the one-shot.
Recovered selection also resets the episode. Neither decision changes which
source, guide, frame or renderer is used.

There remains at most one automatic snapshot per game process; manual F8 is
available. F9 OFF does not request a snapshot. Original application D11 images,
AFW terminal/history images and submitted layer metadata from V686 remain
available. The full snapshot may cause a temporary diagnostic hitch.

## Verification and next run

Five focused Release checks pass: core, generic/GTA/Outlaws marker handling and
terminal capture. Coverage includes alpha-only black, nonzero markerless color,
exact CRC parity with/without observation, actual GPU black and valid-token
readbacks, startup suppression, recovery, black-episode cancellation, 200 ms
selection loss and one-shot bounds. Native original-image capture, immutable
CPU bytes and serialized output checks remain.

Restart matching V687 tray and game, keep AFW 2x2 and F9 enabled, then traverse
the actual scripted gameplay sequence that causes the reported persistent N/A
and stutter. Leave that failed in-game state for about ten seconds before
closing. Merely opening the level and quitting does not reproduce that fault.
No timed F8 or benchmark is required.

Version 0.3.1 / implementation 687 / IPC 68701 / AFW beta6 ABI 45601. Public
OFXR-Bridge Git provides the source worktree; retained ofxr-development stores
outputs. No retired private Git writes, public push, live installation or user
setting changes are part of this preparation. Actual GTA repair remains pending.
