# V688 GTA in-flight origins across producer reset

Local candidate, 2026-10-26. Applies only to `GTA5_Enhanced.exe` protected
color-origin history; other executables retain their existing reset behavior.

## Captured failure

V687 PID 33192 retains an in-game interruption at 82.550 seconds, before the
exit at about 91 seconds. Frame 6931 resolves publication 2828 successfully.
Frame 6932 still decodes the exact CRC 2828, but lookup reports no retained
owner. The other eye's 2829 record is present, with same-stream predecessor
2827, and cannot find the required interleaved opposite publication 2828.
The subsequent 2832 record starts a new history. AFW falls back to original
frames until selection recovers; a raw submission with unknown content identity
also invalidates the total-FPS measurement for one second.

The marker did not disappear in this interval. A native producer reset removed
the old immutable guide records before XR finished consuming their colors.
The V687 full snapshot did not trigger: this loss was shorter than its 200 ms
threshold, and its later black exit was correctly excluded. The retained flight
log contains the exact token, owner/epoch, serial and publication receipts.

## Change and limits

For GTA, successful publication reset or geometry change still starts a new
producer epoch and clears its current temporal/calibration head. Its protected
color-origin deque remains within the existing 14-record limit and 16 snapshot
slots. Exact old CRCs may resolve their own retained pre-reset records, with
their original epochs, camera packets, motion/depth pixels and fence ownership.
Resolving an old key does not relabel it with the newest stream epoch.

New reset productions must still satisfy the strict same-epoch or proven A/B/A
history checks. A missing new opposite frame is never replaced with an old one.
Unknown/duplicate tokens, natural eviction, native failure invalidation, feature
retirement, tracking disable, canonical allocation identity and cross-queue
submission checks remain strict. No guessed guide or timing-based color match
is introduced.

There are no new snapshot slots, swapchains or presentation waits. AFW's native
depth-aware camera subtraction, shaders/provider, calibration policy, quality
and all generation/pacing algorithms are unchanged. Normal priming of a genuinely
new camera history can still temporarily prevent synthesis. This candidate
addresses the observed premature old-origin deletion; it does not claim every
reported scripted-sequence issue is proven fixed.

## Verification

The new named-executable WARP regression fails against the V687 runtime logic
with "GTA native reset discarded the exact in-flight old color N-1/N-2". With
the patch, the same fixture verifies both pre-reset colors, exact old epoch and
GPU motion/depth bytes, rejection of incomplete new A/B/A histories, recovery
with the exact new history, bounded eviction, continued ring reuse and explicit
failed-native invalidation. Its extra test-only CPU pins are released before
testing natural ring recycling.

Seven focused Release checks pass, including the ordinary non-GTA protected-guide
suite, core, marker transport and terminal capture. Hardware acceptance remains
pending a matching V688 tray/game restart and the same GTA scripted sequence,
AFW 2x2 with F9 enabled. No new timed F8 or benchmark is required.

Implementation 688 / IPC 68801 / AFW beta6 ABI 45601. Public OFXR-Bridge Git
provides the source branch; retained ofxr-development stores work and packages.
No public push, retired-private-Git access, live installation or profile changes.
