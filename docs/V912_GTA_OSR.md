# V912: GTA OSR disocclusion and scripted marker contrast

Base: V911 c87cf47eaf233f86b1d52cdf968391e1d9a1b38f. Local unsigned test build.

Confirmed native N1 OSR rejects a history sample only when a static-world
correspondence lands entirely on a nearer old foreground surface, the current
background quad agrees, and the other eye would see that foreground if still
present. Existing one-texel camera and 3% depth limits remain. Moving surfaces,
genuine own-eye occlusions, native N2 and hard-near handling keep existing paths.
This removes the large old bumper ghost on the captured road. Fine silhouette
fragments remain in the replay; no claim of complete temporal acceptance.

OSR always converts undilated depth. Its dilation control is hidden on desktop
and headset; saved AFW preferences are retained for AFW and cannot enable OSR
dilation.

GTA scripted F8s PID26384 contain the marker at the correct position but one eye
frequently has insufficient contrast for sync/CRC. Enable the existing full-white
HDR primary marker policy already used for DOOM. Same 256x8 band, position,
readback, row search, identity/CRC and channel conflict rejection. No guessed
publication and no reader threshold relaxation. Completely black loading frames
remain legitimately unavailable. Actual scripted-camera acceptance needs owner
retest; final processed captures cannot prove the brighter writer survives every
closed-source game pass.

No RIFE, framegen, NR, pacing, common camera or pose change. No push/live install.
