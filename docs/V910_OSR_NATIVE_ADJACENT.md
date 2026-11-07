# V910: OSR with original adjacent-frame motion

GTA Enhanced supplies original current-to-previous native vectors. The current
OSR history alignment added in V906/V907 admitted only same-eye N2 vectors, so
GTA retained the older history extrapolation even with a valid native triplet.
Driving F8s show tearing of the generated car while its real colours and original
N1 correspondences remain coherent.

For a confirmed original N1 field, use that measured correspondence directly to
read the destination eye's own previous real colour. It is not half an N2 screen
displacement. Keep the existing composed N2 endpoints for the temporal-depth
guard, using the two measured frame intervals for its N1 time fraction. Preserve
scene rejection, hard-near protection, depth tolerance, trusted foreground edge
checks, bounds and the established fallback for rejected correspondences.

The original snapshot is borrowed as one extra SRV during resolve, including the
existing source rectangle and crop. It is returned to its incoming state. No new
image allocation, image copy, GPU pass, XR swapchain, wait or readback is added.
The composed field remains the field stored in OSR history and supplied to AFW.

Native N2 titles retain their existing V906/V907 algorithm. Unknown/default N1
values do not activate the direct route: original motion must be confirmed by
the observed native/public invocation, with exact source/peer/same histories and
producer epochs. This is an interval contract rather than a driving/aiming guess.

V909's November GTA camera receipt and the existing effective-headlock camera
corrections remain present. This change does not force the aiming camera into
unlocked driving or change AFW's provider, RIFE, frame delivery, NR or settings.
F8 timeline `osr_current_motion_interval` distinguishes disabled (0), direct N1
(1) and existing N2 (2); flight stage1310 adds bit3 for admitted direct N1.

Focused CPU/GPU tests and replay evidence are retained with the local build.
Offline image comparisons are not headset acceptance of driving or cinematics.
