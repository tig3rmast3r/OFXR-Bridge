# UEVR missing DLSS frame-time trial (V984, round 1)

Reporter svperstar99's V983 PID42056 trace repeatedly refuses DLSS guide
publication with stage1187 / reason2 / invalid bit64: depth exists but its frame
time is not finite and positive. Hybrid is selected without the active Hybrid
configuration bit. This establishes a guide-admission failure; it does not
establish the cause of the separately reported Ghosting Fix crash or menu freeze.

For D3D12 NGX capture in a process with UEVRBackend.dll loaded, missing or invalid
NGX frame time now uses the interval between successful application xrBeginFrame
calls. Internal generated frames and multiple DLSS evaluations/eyes do not update
this clock. Positive finite NGX timing is retained unchanged. Luke's existing
normalization policy and all resource, rectangle, ownership and publication checks
are retained.

No interval is invented at startup. Session changes, duplicate/backward starts,
and gaps above 250 ms invalidate the sample. Capture also rejects a sample older
than 250 ms. A subsequent pair of application starts restores measurement. This
is an observed application cadence estimate, not exact engine simulation timing;
the trial requires game validation, particularly for asynchronous rendering.

NGX diagnostics `uevr_frame_time_fallback` carry a=capture call, b=stream,
c=raw supplied float bits, d=measured float milliseconds bits. Logging is bounded
to the first eight corrected captures and every 300 thereafter. Normal publication
and Hybrid-use checks still decide whether these guides can be consumed.

Reporter check: repeat the same Rebirth scene and Hybrid settings, then return
the entire V984 runtime log folder. Check fallback entries, disappearance of
sustained stage1187/2/64, active Hybrid configuration/guide-use receipts, and visual
results. A remaining crash requires its own crash dump. This package excludes
the rife-vulkan payload and nvngx_dlssnr.dll. No version bump, install, arm,
game-setting change, commit or push is part of this trial.
