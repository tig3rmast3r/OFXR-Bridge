# V692: GTA scene-transition color identity

Local candidate based on V691. This addresses three distinct observations in
the latest GTA capture set. Game/headset acceptance remains pending. Other
titles retain their existing guide boundaries and origin decoder policy.

## Observed failures

The original SR publications continue with valid marker CRCs. In a later XR
image with chromatic separation, the weighted-luminance reader loses token
8118, but its individual color channels retain that exact token and CRC.
Another captured image is strongly blurred: scanning the complete projection
and all its color channels finds no surviving token. Enlarging the search area
cannot recover information that was erased.

In the final continuous failure, both application projection images become
entirely black. The actual full scene is in a BOTH-eye XR quad, with token
9173. This is a change of application composition, rather than proof that
engine motion/depth publications have stopped. Subsequent original publications
remain valid. No scene identity or frame rate is inferred from black images.

## Changes

* GTA-only fallback to each color channel when ordinary luminance decoding
  finds no token. The full Manchester preamble and CRC remain mandatory.
  Disagreeing valid tokens are rejected; erased markers remain unavailable.
* Repeat the same original SR marker after postprocessing only when modern
  Streamline supplies an exact successful frame-token/viewport Backbuffer tag,
  `eValidUntilPresent` lifetime and successful PCL PresentStart. A bounded
  ledger joins these with the exact submitted native production receipt.
  Queue, device, dimensions, lifetime, duplicate, ambiguity and retirement
  checks remain mandatory. It never selects the latest production by age.
* The late write uses the original submitted direct queue and completed
  command-allocator slots, without waiting for a free slot. It preserves the
  application's API results and adds no new guide production or scene count.
* While stereo origins are unavailable, recognize an identified full BOTH-eye
  application quad. Suspend AFW and bypass OFXR for that composition instead
  of generating from black projections. Status reads `AFW: 2D scene (no stereo
  source)`. Ordinary stereo selection takes precedence on recovery; there is
  no persistent suspension latch. No combined-scene FPS identity is invented.
  With no fresh projection release, the identified quad takes precedence over
  holding an old AFW pair. When projections are released, both compact marker
  regions must be read successfully as black with no token, not a GPU read
  failure or chromatic erasure. These checks do not depend on F9 logging.
* The native D3D11 quad check reads only the existing compact marker ROI on the
  original release thread. This small staging Map can synchronize in the
  missing-stereo/2D path; it does not run during ordinary valid stereo frames
  or replace the negotiated eye geometry with the quad size.
* Full automatic GTA loss captures are opt-in via the runtime diagnostics key
  `gta_auto_capture=1`, default off. F9 logging and manual F8 remain available.
  This avoids repeated large automatic readbacks during normal play.

V691's original-caller N-1 guides, final wrapper color, typed-native NR
callbacks and strict ownership guards are preserved. AFW's mandatory native
depth-aware camera subtraction, shaders, provider, resize math, generator and
pacing are unchanged.

## Verification and limits

Twelve focused Release checks pass. They include the shipping decoder's RGB
fallback/ambiguity/erasure tests, actual cached modern Streamline API hooks with
real D3D12 resources, early-marker erasure and exact late restoration, invalid
frame/viewport/lifetime/duplicate/owner refusal, and an actual native D3D11 quad
ROI read. The actual layer test covers full-quad classification, missing or
conflicting CRCs, single-eye/partial overlays, no false frame identity and
status recovery. Existing GTA, Crimson, KCD and Outlaws ingress tests, GTA
native-view provider quality, gap recovery, core and capture tests pass.

The supplied game executable exposes the relevant modern Streamline APIs, but
the owner run does not yet prove that GTA submits a usable Backbuffer tag and
PresentStart at this exact boundary. The late path safely declines when that
proof is absent. Offline fixtures prove its implementation, not its activation
in a new headset run. A fully erased marker without that exact late receipt
still cannot be reconstructed. The candidate is not a claim that every N/A,
aiming defect or scripted-scene transition has been eliminated on hardware.

No public push, installation, registry or active game-profile change is part
of this local candidate.
