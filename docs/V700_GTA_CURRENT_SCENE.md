# V700: GTA headlocked scene-time camera

V699's three owner-labelled captures are headset rotation with ghosting, gamepad
rotation without the defect, and gamepad strafe with ghosting. Matched producer
tracking confirms that submitted XR relative rotations agree with the actual
producer head rotations. Their constant reference-space yaw offset cancels.

The V696/V699 headlock branch retained the entire old native peer camera. In
the inspected static region its generated colour is almost the previous real
peer, so a coherent debug warp is not evidence of a newly reconstructed scene.

During validated GTA headlock only, reconstruct the peer in the CURRENT source
gameplay-camera base. Apply only the original source-to-peer XR head rotation
to that base, and keep the original submitted XR poses. Retain the matched
peer's projection. This is current scene time with the requested AER head pose,
not a same-time head-pose substitution or a shared native/XR world-map rewrite.

The measured producer rig anchors the right eye at zero in headlock; its left
offset follows the left head orientation. Preserve that direction with the
existing runtime-calibrated eye distance. No fixed IPD, guessed angular gain,
prediction or additional memory read is required by the correction.

Native camera/depth subtraction, raw motion and object intervals, producer
identity admission, shaders, provider binaries/modes, NR, quality scaling and
all scheduling/pacing remain unchanged. Outside effective GTA headlock the
rendering path is unchanged. V699's F9-only matched tracking diagnostic remains
available; no new GPU resource, copy, dispatch, readback or wait is added.

Independent actual-provider tests include a moving gameplay camera, historical
head pose, foreground occluder and textured background at 1x1 and 2x2. They test
recoverable changed-surface pixels against the current scene and explicitly
reject leaving the previous surface in place. The same new camera oracle
rejects the V699 renderer. Static pose/plane checks alone are not headset
acceptance. The owner must check all three movements; recurring N/A is separate.
