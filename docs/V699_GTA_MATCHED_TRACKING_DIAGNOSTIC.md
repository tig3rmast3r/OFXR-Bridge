# V699: GTA production-matched tracking diagnostic

V698 is rejected: headset rotation is still less smooth and gamepad rotation
also doubles during aiming. V696 remains only a partial baseline. Neither a
historical native peer camera nor a common native/XR world-map closure is a
validated complete aiming solution.

This build restores the exact V696 rendering/camera behaviour. It adds no aiming
correction. With F9 diagnostic logging active, the already-pinned GTA reader
also snapshots four tracking records with the original SR publication. Bounded
repeat reads, stable runtime ownership/render identity, finite values and unit
quaternions govern diagnostic validity only. A missing/racing diagnostic receipt
must never reject a guide, change the camera path or cause N/A.

F8 appends tracking records, validity, effective lock mode, render identity and
yaw offset for the current, exact previous-opposite and exact previous-same
guides. These receipts stay with their original colour/depth/motion production;
there is no latest-state query when the older guide is consumed.

With F9 off there are no added tracking-memory reads. F8 alone therefore does
not guarantee these new records: enable F9 before the comparison and wait for
producer history to fill. Existing F8 capture limits remain two AFW evaluations.
No automatic capture, GPU readback, dispatch, texture, scheduling or runtime
pose changes are added. All V696 shaders and provider binaries are retained.

The targeted comparison is one capture while rotating with gamepad/head still
and one while rotating the headset/gamepad still, both in aiming state. This is
a diagnostic candidate, not a claimed aiming or N/A fix.
