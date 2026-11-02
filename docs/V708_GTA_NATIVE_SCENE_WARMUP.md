# V708 GTA native scene-history warmup

The targeted V707 run PID55036 identifies a separate failure from unreadable
markers. Two automatic witnesses capture normal game images, valid stereo
CRCs and an empty auxiliary quad. They must not be described as 2D cutscenes.

At source frame6161, tokens2077/2078 decode successfully. Exact token2078 is
native epoch4 serial1, while its preceding token2077 is epoch3 serial1615.
Resolver refusal6 correctly refuses a cross-epoch join. Subsequent sources2079
and2080 fail native-camera history with reasons1 and3;2081/2082 evaluate normally.
Frame7161 repeats it at tokens3075/3076, epoch9 versus8, followed by the same
two native-history warmups and recovery3079/3080. These are not bonus failures:
no CRC inference is requested in either transition.

The fix preserves the last completed stereo pair during the first three
native productions of a new epoch. It does not make those productions valid
AFW guides, cross epochs, reuse old vectors, synthesize missing cameras or
count repeated images as new frames. Accepted aiming, camera subtraction,
calibration, providers, NR and the marker-loss one-second bonus stay intact.

The old XR pair can be held safely only BEFORE staging acquires/releases its
next private output. A cheap control-only query first asks whether the last
accepted GTA stream has a newer native epoch. Only then are the two exact
committed source-history tickets leased and their physical CRCs read. The
decoded fresh alternating source must name the same stream, a newer epoch and
serial1..3. Existing layout/revision/space/handle checks must also approve the
held pair. There is a250ms maximum tied to the last successful pair; duplicate
colours and repeated resets cannot extend it. Startup without a completed pair
and unknown markers retain the normal refusal path.

History leases retire against the reader's exact GPU fence, including timeout;
pre-submission failure cancels safely. An unfenced submitted read quarantines
its leases and disables further preflight in that session. There is no read of
runtime-owned XR images. No extra history/colour buffers, per-frame read in
steady state or new provider GPU pass is introduced.

Returning fresh AFW output resets the post-AFW OFXR temporal anchor once, so it
cannot interpolate the old scene with the new one. Native histories keep their
original reset boundaries. The existing held-pair identity/poses remain attached
to repeated pixels; no fictional output rate is reported.

F9 stages1220(serial/publication/epoch) and1221(return) expose the transition.
V707 automatic captures remain for failures not covered by this reset warmup.
V706 opaque-quad behaviour is absent. This change is GTA-only; other Luke games
and generic OpenXR routes are not modified by the warmup policy.

Automated validation covers exact protected native reset metadata, continued
cross-epoch rejection, completed and timed-out CRC read receipts, and actual
leased source-history readback followed by immediate normal lease reuse. The
existing held-pair GPU test reads both retained XR images and verifies their
original pose/FOV and scene identity. Headset confirmation remains pending.
