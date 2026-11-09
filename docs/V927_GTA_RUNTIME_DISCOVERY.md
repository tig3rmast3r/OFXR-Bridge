# V927: discover GTA REAL VR camera bindings structurally

GTA's optional camera correction previously selected three layouts using exact
PE timestamps and fixed code addresses. REAL VR 2611.8.4 kept the relevant
camera layout but failed that allowlist, disabling the correction.

The replacement discovers the GTA runtime through its named complete-object
RTTI/vtable, an owner load with an independent class check, and the linked
native camera wrapper. That wrapper calls the mode snapshot and yaw correction.
Both code paths must agree on the live/cached mode fields; the named GTA
constructor must initialize the same cached pair. Render, yaw, mode, owner and
vtable offsets are derived from those witnesses. Missing or ambiguous evidence
refuses the correction. No timestamp, image-size allowlist or fixed field
address selects a layout. Future semantic/ABI changes can still need support.

The bounded read-only scan runs once on the already-loaded game-local REAL VR
proxy. No hooks, game writes, continuous search, GPU work or new readbacks are
added. Normal observations retain owner/vtable/state guards and reject a yaw
change during the read. Optional old raw tracking diagnostics are left invalid:
the camera witnesses do not prove that separate four-pose memory layout. Those
diagnostics never select a pose or gate rendering. Existing GTA cabin/history
and camera correction math are unchanged.

Focused validation passes against the user's 2611.7.2 and installed 2611.8.4
DLLs, including deliberately altered timestamps. Both derive owner0x7b6528,
vtable0x5abda8, render0x841790, yaw0x841118 and mode0x84112c in about35ms once.
Synthetic fixtures cover code/RTTI/field relocation, independent field spacing,
register changes, an owner in unread BSS, malformed bounds, foreign RTTI,
broken ownership/constructor/callback links and ambiguity. Effective lock/unlock,
invalid/racing state and canted-eye pose tests pass. The shared PE/RTTI helper
also preserves the RDR2 binding on both DLLs and its synthetic fixtures.

Flight stage1370 reports discovery (1 success; -1 image, -2 RTTI, -3 owner,
-4 missing camera witnesses, -5 ambiguity, -6 exception); b/c are owner/vtable.
Stage1371 reports render offset as result and yaw/mode as b/c. These are binding
receipts, not evidence that a particular frame reached the headset.

The owner confirmed the fix working in GTA V with the newest REAL VR DLL on
2026-11-09, using the signed V926 GTA-binding-r1 test package. Both x64 and x86
Release builds and the focused binding/reader checks passed. This V927 commit
keeps that tested implementation unchanged, following the separate V926 RDR2
commit. The full test suite remains deferred at the owner's request.
