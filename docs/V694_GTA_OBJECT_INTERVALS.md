# V694: original GTA native object intervals

Local candidate based on V693. Headset acceptance is pending.

GTA Enhanced's original public SR inputs describe the adjacent native frame.
The wrapper's typed input is a history-conditioned combined field. Capture the
original motion/depth before the wrapper modifies the allocations; continue
using the final caller colour after the enclosing wrapper completes.

The original camera subtraction reconstructed the current-to-same-eye camera
interval, but retained only the current-to-middle object-motion residual. GTA
now also follows the current native correspondence into the immutable middle
motion/depth pair, subtracts the exact middle-to-older depth-dependent camera
motion and adds that second object residual. This does not assume constant
camera velocity or multiply the head rotation. The measured middle interval
replaces the previous missing-interval jitter estimate.

Admission requires the exact current/middle/older production identities,
matching raw intervals, scale, dimensions and origins, retained middle guides
and matching snapshot device ownership. Invalid/disoccluded pixel lookups keep
the existing current residual rather than sampling outside the retained image.
The extra texture reads are enabled only for the original GTA input contract.
Other titles retain their existing correction. Shared shader binding changes
are covered by the existing native and non-native GPU cases.

The final-colour transaction also completes on the admitted public-capture
return path. It retains the outer creator/list/device/result/geometry checks;
no latest-frame lookup, marker guessing or rejection-threshold relaxation is
introduced. Native NR callbacks, the V693 flat-quad passthrough and manual
capture controls remain in place. Pacing and delivery policy are unchanged.

Validation includes GPU cases with a discontinuous camera, different object
motion in the two intervals, nonzero jitter, invalid history depth, both
projection handednesses and supported motion formats. It also includes the
actual wrapper-overwrite/final-colour fixture and real bundled AFW provider.
Private replay of the labelled V692 pair changes the aiming output while the
second unlocked output remains pixel-identical. This is not headset proof of
the aiming fix. No reliable GTA headlock flag has been validated or used.

No automatic captures, live configuration changes or public publication are
enabled by this candidate.
