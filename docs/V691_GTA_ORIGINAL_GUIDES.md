# V691: GTA original motion before REAL VR combination

Local candidate based on V690. Aiming repair requires headset confirmation;
the available captures reproduce the defect but cannot prove its absence in a
new live frame. No other title changes its guide boundary.

## Evidence

V689 GTA PID 17580 contains three manual aiming F8s, six actual AFW evaluations.
The exact source-color CRCs are valid and histories contiguous. The 2x2 color
reduction is exact within 0.5/255 rounding in every sample, so a resize shift is
not observed. The private offline replay uploads their actual BGRA8 1493x1368
colors, R32F depth, RG16F motion, camera matrices and provider parameters into
the pinned beta6 provider. Replayed second-frame output matches the captured
defective output at approximately 0.001-0.003/255 mean absolute RGB error in
the central measured region. Debug replay marks roughly 56-84% of that region
as motion on the same inputs. These are reproduction results, not a visual
improvement claim.

The typed SR motion field disagrees spatially with its native N-2 static-camera
prediction during aiming. Diagnostic pose fits show a different translation in
large regions while rotation remains close, with different regional solutions;
a common scale/depth/axis patch is not justified. Those fits never enter runtime
code or admission.

The installed REAL VR module contains a compute motion combiner with current
and previous motion, three depth inputs, previous colors, camera transforms and
a combined-motion output. Our former GTA boundary was inside typed SR, after
this processing. The resulting field is therefore not the untouched engine
motion needed for our own depth-aware camera reconstruction. The exact
unmodified caller field was not retained in the old F8s; no claim of an offline
pixel comparison against that missing field is made.

## Change

For `GTA5_Enhanced.exe`, use the existing original-caller guide boundary already
used by Crimson. Freeze motion, depth, original scalars and camera before REAL
VR processes them. Declare that original current-to-adjacent-frame field as
N-1. The original wrapper still executes unchanged, including native DLSS and
typed-native NR pre/post callbacks. Mark its final caller color after it
returns and correlate that color with the exact retained original guides.

AFW continues to perform its existing native depth-aware camera subtraction
and reconstruction of same-eye motion. No optical-flow replacement, empirical
gain, guessed publication, changed provider, AFW fallback mode or relaxed
camera/CRC/fence guard is introduced. Native-only fallback retains its declared
N-2 interval. Failure/recursive completion invalidates the public history,
following the existing boundary's guards.

V689 meter recovery, V688 reset-origin retention and V690 automatic loss
capture are included. With F9 on, a later marker-loss episode can record the
frozen failing pair immediately after stable recovery/cooldown. The new guide
boundary does not claim recovery of a marker erased after the wrapper.

## Verification

The actual GTA capture test now forwards through a wrapper that overwrites
motion/depth in place before typed SR and writes a final post-SR composite.
It checks retained original GPU guide bits on an independent queue, interval
N-1, camera and ownership/fence receipts, final composite pixels with exact CRC,
and reused-feature retirement. The production GTA title selector is exercised;
the Crimson title selector is not enabled to obtain the desired result.

Nine focused Release checks pass: core, terminal capture, actual GTA/Crimson
capture, KCD scene boundary, Outlaws public boundary, interleaved AFW quality,
2x2 beta6 quality and GTA native-view camera quality. The quality checks cover
both native intervals through the shipping depth/motion shaders and real AFW
provider. Public main/nightly, registry, live installation and game settings
remain unchanged. Hardware acceptance is pending.
