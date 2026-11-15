# R1005: OSR original-motion gap stability

The owner reports an occasional misplaced flash, with purple OSR debug, and does not request FPS tuning or additional F8 captures. R1004 was a timing experiment addressing the wrong symptom. R1005 restores R1003 marker-read ordering and targets reconstruction continuity.

R1003's retained log shows four skipped original-publication events. For each, the legacy AFW A/B/A prime forces mode1 and OSR discards its own valid history for two images before resuming history-first reconstruction. Successful EndFrame/ready status does not imply the visual path stayed the same.

Confirmed original N-1 full OSR now independently accepts its own recently stored peer colour/depth/camera receipt, bounded by the existing four-publication age limit and exact accepted publication, producer and epoch. Valid current camera/domain and no producer reset are required. No frame is relabelled. During a non-adjacent gap, old motion is not extrapolated across an unobserved endpoint; exact camera/depth reprojection remains subject to the existing surface and GPU scene-cut checks. Pinned AFW A/B/A priming, native N-2, variable-interval RDR2, normal contiguous vector behavior and pacing remain unchanged.

CPU cases cover producer/epoch/copy mismatch, reset and excessive age. GPU cases replay three/four-publication gaps for both eyes and both qualities: the old AFW gate excludes history; the new path must return the actual retained eye's colour at its original camera-transformed position, distinguishable from current colour, without changing its publication.

F9 stage1352 and mode1 stage1310 are retained in the diagnostic benchmark log so rare transitions survive flight-log rotation. No automatic image dump or additional capture key is needed. R1005 quality/flash suppression still needs owner confirmation; CPU/GPU fixture checks are not a headset acceptance result. Local unsigned build only, ABI98401, no deployment or push.
