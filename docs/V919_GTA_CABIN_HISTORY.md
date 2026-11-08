# V919: GTA final-color markers and coherent cabin history

GTA5_Enhanced.exe keeps its origin marker inset from the left edge and moves
it inward, within a bounded margin, after sustained nonblack misses or a
CRC-valid band too close to the edge. Reader coverage retains previous marker
positions while this feedback moves the writer. This title-specific placement
works alongside the shared V918 full-eye recovery and its two-second cooldown.

The read-only GTA camera binding now recognizes REAL VR 2611.7.2.5696 using
its exact PE timestamp/image size, code witnesses, runtime pointer and vtable.
Existing October/November bindings remain; unknown layouts are not guessed.

In the verified GTA unlocked-camera route, cabin reprojection uses the native
one-publication motion field and its actual opposite-eye camera interval.
History stores that original N1 field, stereo reference and jitter rather than
reinterpreting a composed N2 field. The shader validates camera-following
attachment motion/depth and advances measured screen motion. Missing current
N1 data disables that history path. The binding/mode gate is independent of
diagnostic-only tracking flags.

Sky and cabin edges avoid unreliable composed-history displacement; a bounded,
depth-validated own-eye background lookup fills stereo holes. Old foreground
outside the current source FOV cannot qualify as a verified occluder. These
GTA-only policies reduce detached windshield/mirror ghosts, patterned background
patches and arm/wheel breakup; unrelated titles retain the default policy.

The owner confirmed substantial improvement with r5. Focused native-motion GPU
coverage includes both eyes/qualities, static-world/depth refusals, original N1
snapshot/camera interval, screen-motion advance, missing-N1 fallback and old
foreground outside the source FOV. Prior default-policy replays were identical
for six frames; the native regression failed with the r4 shader negative.
Only this combined V919 tip is built for the final pre-push headset run. No
commit or release has been pushed. Headset acceptance of this tip is pending.
