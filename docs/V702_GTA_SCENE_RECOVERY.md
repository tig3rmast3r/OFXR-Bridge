# V702: GTA scene calibration recovery and automatic loss witnesses

The owner accepts V701 aiming. Remaining reports are N/A at scripted scene
changes and intermittent incorrect AFW after returning to gameplay. The supplied
V701 capture has valid markers and native guides: AFW uses separation 0.1185703,
while its two exact native A/B/A camera triples measure 0.0618337 and 0.0620778.
The old target is about 0.05649 native units away from the exact peer; using the
measured separation reduces this to 0.000258 and 0.000070 units. No fixed IPD,
world-unit multiplier, camera rotation or V701 aiming change is introduced.

Two latches retained the first unit calibration: the producer stream and AFW.
For GTA only, retain an established calibration through a temporal reset, without
retaining native guide/camera or GPU history across epochs. Continue checking
complete nine-sample native-camera windows. Replace the established calibration
only if all nine samples agree within the existing 3% window and the new median
changes it by more than 3%. Duplicate/out-of-order/missing/unstable samples cannot
add confidence. A new producer feature still starts without calibration. Other
Luke/game paths keep the existing frozen per-epoch calibration.

AFW consumes a proven GTA update and retires the old private provider history
at the existing fenced lifetime boundary. New and old camera scales must not
share provider history. This handles the evidenced stale recovery and avoids
repeating the nine-sample unit bootstrap after every GTA temporal reset. Normal
same-eye/opposite-eye guide priming still applies; missing markers cannot be
replaced by guessed/latest guides.

At the owner's request, GTA automatic loss captures default ON in this local
candidate, only while F9 is ON. An explicit diagnostics/gta_auto_capture=0 still
opts out. Capture the first unreadable nonblack stereo colour immediately;
existing manual F8 remains unchanged. One witness per episode, sustained recovery
and 10-second cooldown, at most four automatic witnesses per session. Startup,
persistent failure and black 2D/exit projections do not trigger a capture storm.
The immediate colour receipt and following two AFW evaluations are retained.

The kill/slow-motion effect's missing-marker cause is not established by the
current F8 (which records AFW already running). Automatic witnesses are intended
to expose that boundary; do not claim every N/A or kill effect is fixed. The
late detailed log mostly records the final 2D quad/exit, not all earlier cuts.

Validation includes strict calibration-window/reset/unstable-order tests, actual
bundled provider recovery from a stale scale at 1x1 and 2x2 with camera/pixel and
GPU-state checks, bounded automatic-capture tests and all V701 aiming/scene
oracles. Native depth/camera subtraction, MV/object intervals, shaders, marker
admission, provider binaries, NR and pacing remain unchanged. No publication or
live installation/profile/registry changes are part of this local candidate.
