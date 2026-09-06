# Bridge flight recorder

The recorder belongs to the OFXR OpenXR layer and is independent from game or
VR-mod logging. It is disabled by default.

V066 also provides an independent transparent numeric FPS overlay. It is green
when a recent synthetic submission succeeded and red when generation is
inactive. It counts accepted nonempty OpenXR submissions, not physical headset
scanout, and does not replace the recorder for crash or fallback diagnosis. See
[FPS_OVERLAY.md](FPS_OVERLAY.md) for its controls and limitations.

Enable **Bridge flight recorder** from the tray before starting the game. Each
OpenXR process then creates a bounded file named
`ofxr-bridge-flight-YYYYMMDD-HHMMSS-pidNNNN.log`. Use **Open bridge logs** to
open its directory.

The log records layer negotiation, graphics binding, swapchain eligibility,
projection mapping, frame-generation stages, recovery events and potentially
blocked OpenXR call boundaries. An unmatched begin record can identify the
operation which did not return during a freeze.

For a compatibility report, reproduce the problem once and attach the newest
flight log. Also provide the game and version, VR mod, headset, OpenXR runtime,
GPU and driver, selected backend/options and exact reproduction steps. If no
log was created, state that explicitly: the layer may not have loaded or the
process may have stopped before recorder initialization.

The recorder does not capture video and is not a replacement for a native
crash dump. Game logs and crash dumps remain useful when available.
