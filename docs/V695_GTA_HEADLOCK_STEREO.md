# V695: GTA headlock and stereo pose ownership

Local test candidate based on V694. Headset acceptance is pending.

Read-only observation of the owner's GTA runtime confirmed a distinct effective
lock state while aiming, including the delay after releasing aim. Capture that
state with the original SR production; do not infer it from an input button or
read today's state when consuming an older colour token. The reader requires
the observed module layout, active GTA runtime object and stable state/owner.
It selects the game-local proxy explicitly, without loading or pinning a module.
Unsupported versions or unavailable state keep the existing camera path.

While that retained production is headlocked, reconstruct the missing eye from
the source eye's head time and the physical relative eye rig measured by one
successful stereo LocateViews call. Preserve cant and relative translation.
Use this same destination pose for AFW's camera and for the resulting OpenXR
image metadata, including post-AFW consumers and held images. A fresh production
with owned poses is still a fresh production, not a held frame. Outside the
effective lock, use the previous path. No pacing or delivery scheduling changes
are made. Native depth/camera subtraction and V694 object intervals remain.

The owner's V694 log also showed final-caller marker writes but no successful
eye CRC reads in the recorded interval. Restore the native-colour carrier as
well as the final caller: bind the original N1 guide transaction before its
unique native child, stamp that child's successful output after native NR,
then stamp the final caller after wrapper composition. Both carry the same
immutable original production token. Keep creator/list/device/type/geometry,
single-child, native-result and outer-result checks. No guessed/latest token
or relaxed CRC acceptance is introduced. F9 stage 1200 reports native marking;
stage 1199 reports the retained lock state and whether the matching rig was used.

Tests cover pinned-layout/owner/race failures, delayed effective unlock,
simultaneous canted-eye geometry, actual provider camera origins and pixels,
fresh/held pose ownership, and matching native/final wrapper colour CRCs.
Other title boundaries and the existing native-motion tests remain covered.
No automatic captures, runtime installation, profile changes or publication
are enabled by this candidate. The aiming result still needs a headset test.
