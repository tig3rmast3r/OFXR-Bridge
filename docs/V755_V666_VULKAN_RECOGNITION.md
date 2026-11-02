# V755 restore V666 recognition for selected Luke Vulkan games

Owner: V754 still no AFW. Latest PID60988 no longer refuses264; XR color-origin
reader reports zero valid reads out of about2500, so do not claim AFW recovered.
V666 is the owner-confirmed startup-good boundary; its AFW production in this
bisect was not separately recorded. Avoid stating that AFW on V666 was retested.

This candidate starts from V748, excluding ALL V751-V754 capture/command/loader
experiments. It restores the exact V666 dynamic luke_detected function body for
DOOMTheDarkAges.exe and TheGreatCircle.exe only. Those processes skip V667 cached
module discovery, reference normalization and loader notifications; their hook
monitor keeps V666's startup pin behavior. Existing refresh calls become no-ops.
The original per-query recognition is deliberately restored in these two games
for this compatibility test; do not claim zero per-frame queries there. RealVR64
conventional-marker fast path normally avoids module enumeration once loaded.
All other processes retain current cached detection, including late DLL epochs.

Source audit proves the V666 function body is byte-equal and verifies Vulkan
capture/command/queue layer, OpenXR layer, private runtime and Director policy are
unchanged from V748. No new marker, AFW math, pacing or resource hooks. The cached
nonselected tests remain, plus late custom/name load/unload under both selected
executable names with no cache refresh. Existing native GPU bootstrap tests use
this real compatibility classification path.

Local candidate only, no RIFE, no optional V749 buffer-phase fix, no live profile/
registry/game/mod installation or push. Actual DOOM startup/AFW awaits owner.
