# V842: preserve the submitted OSR eye pose during GTA aiming

## Cause and scope

The GTA aiming camera corrections from V700/V701 already run before the AFW/OSR
renderer choice. Both renderers receive the same corrected native peer camera.
The later OSR-only position receipt introduced for headset translation changes
the submitted peer XR position; AFW does not produce this receipt.

Under GTA's effective headlock the native source and peer cameras may have
different gameplay references. Their corrected world-space displacement is
therefore not just a physical eye offset. Converting it back into XR metres
overrides the original AER pose that the accepted aiming correction preserves.

The owner's V840 F8 at 23:59:09 on 2026-11-02 contains both eyes in effective
headlock (mode2). It records generated-position changes of 70.093 and 75.223 mm
relative to the original submitted peer. Source-to-peer distance changes from
72.207/65.471 mm to 141.221/139.687 mm. This is direct metadata evidence of the
additional displacement, not a measured headset acceptance result.

V842 suppresses this OSR-only position receipt while the existing validated
`native_peer_camera` headlock path is active. The original peer position and
orientation are retained, as in AFW. The V700/V701 native camera correction is
unchanged. Ordinary OSR position correction remains enabled, including KCD2.
The native-peer flag still requires effective GTA headlock and matching native
peer publication, stream, epoch and camera token. No new detector is added.

OSR shaders, hole fill, history, scene detection, AFW provider, RIFE and pacing
are unchanged. No additional GPU work, latency, memory read or user setting.

## Marker and one-second recovery audit

The colour-origin writer/reader, marker inset, continuity model and loss-capture
sources match signed release V773 exactly. The continuity model still expires
1000 ms after a genuine CRC confirmation; inferred frames cannot renew it.
It uses fresh matching native guides and rejects ambiguous markers and actual
transport errors. Both AFW and OSR pass through that shared admission before
renderer selection. No duplicate import or change to this protection is needed.

The later automatic F8 at 23:59:13 and the retained flight tail are origin-loss
witnesses, not the aiming capture. They must not be used to classify the earlier
aiming scene. The 23:59:09 F8 retains the actual OSR evaluations and headlock state.

## Focused checks

The shipping native OSR synthesizer is exercised for both eyes, with headlock
and ordinary input: headlock returns no XR-position override; ordinary OSR still
returns its reconstructed position. Existing original-pose metadata and pose
math checks remain applicable. AFW current-scene and eye-reference provider
tests explicitly select AFW, preserving their intended renderer after OSR became
the default. Marker/black-bar and one-second continuity checks cover the audited
shared path. The next headset check must confirm GTA aiming and KCD2 translation.
