# V686 native D11 source and persistent-loss diagnostic

Local diagnostic candidate, 2026-10-26. GTA's actual binding remains native
D3D11 (`session_binding result=1`); the D11-to-D12 session substitution is not
active for this Luke session. Therefore V685's application-side bridge witness
alone would not photograph the actual GTA input. V685 was retained as a local
component checkpoint, not delivered as the next GTA test.

## Native-source capture

The existing full F8 recorder can now copy the exact enumerated application
D11 color texture at its release boundary. It uses the existing owner/context
thread and resource ownership, copies to a staging texture, maps there and
publishes immutable CPU bytes to the ordinary bounded recorder. The background
writer never calls the D11 context. D12 is used only to describe the typed
readback footprint; the AFW history mirror and downstream runtime image are
not used as pixel sources for this witness.

The stage is `xr_application_original_d11`. Swapchain and acquired image index
are recorded directly; no frame or eye identity is inferred from timestamps.
The original image is not changed. Source/context device identity, sample count,
array slice, format and existing image/byte limits are validated. Full snapshots
are capped at one per application swapchain (eight sources); protected/depth
inputs are excluded at the caller. Original CPU bytes are published ready only
after the owner-thread map/copy completes, so an asynchronous writer cannot
mistake the initially allocated zero buffer for a completed image.

The earlier ordered bridge witness and submitted composition-layer audit are
retained for their applicable paths. Layer type, flags, swapchain, rectangle,
slice and quad size/visibility identify the second submitted application layer.

## Trigger timing

V684 caught the first transient black scene at 44 degrees, with six all-black
projection captures and four still-valid native DLSS colors. It did not capture
the subsequent persistent 50-degree state. To avoid consuming the single
automatic request on that short fade, V686 requires five seconds of continuous
missing paired color tokens after a prior valid token, while F9 is enabled.
A recovered token resets the duration. This is observation of `GetTickCount64`,
not a sleep or presentation wait. It does not affect admission, AFW or pacing.
There is still at most one automatic capture per game process; manual F8 remains
available, and F9 OFF cannot initiate the automatic request.

## Validation and owner step

Five focused Release checks pass. The actual terminal-capture fixture covers
both bridged and native BGRA originals, both array slices, poisoned downstream
runtime pixels, source reuse after capture, immutable CPU bytes, native output
serialization, invalid slice and byte-budget refusal. Existing inactive/ring/
one-shot/automatic eight-frame completion checks remain; the pure loss detector
checks startup suppression, sub-five-second fades, recovery and persistent loss.
Core and generic/GTA/Outlaws marker checks pass as well.

The diagnostic can cause a temporary hitch when the full snapshot is taken;
these frames are not a performance benchmark. No GTA rendering fix is claimed.
Markers, native motion/depth/camera contracts, AFW provider/history and all
generation/pacing routes remain as in V684 outside the diagnostic recorder.

Next owner run: restart matching V686 tray/game, same AFW2x2 scripted sequence,
F9 ON. Stay in the persistent broken state for at least ten seconds, then close
normally. Do not time another F8 or answer questions about the visual mode:
the recorded original pixels and layer metadata are the required evidence.

Version 0.3.1 / implementation 686 / IPC 68601 / official beta6 ABI 45601.
Source Git is public OFXR-Bridge; output remains in ofxr-development. No retired
private Git writes, push, public release, live installation or user setting
changes are part of this preparation.
