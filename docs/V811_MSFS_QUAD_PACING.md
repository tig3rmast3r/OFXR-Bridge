# V811: MSFS four-view NR, stable guide eyes and buffered copy isolation

Local successor of sealed V810 (`779c03cb`), on the 800/RIFE lane. Public V759
remains the frozen 0.3.2 candidate. Headset acceptance is pending.

## Evidence and changes

The V810 NR capture at 16:50 on 31 October identifies three successful native
handles and handle 23 repeatedly rejected: its 934x856 depth allocation has four
array slices. V811 snapshots the base subresource into an independent 2D guide
for that handle. NGX D3D12 supplies the resource and XY origin, without a slice
selector; no eye is inferred from handle/discovery order. Only the selected
slice's mip subresources are transitioned and copied. Other slices and their
states are untouched. Multisampled and multiplanar array guides remain refused.
Per-handle history, retirement and exact colour-region writeback are preserved.

The 16:46 FSR capture contains a roughly 489 ms exchange of the learned left and
right stream identities, followed by four rejected temporal pairs (reason 113).
Release watermarks choosing distinct publications could bypass the existing
identity check. V811 keeps a known, still-active stream bound to its eye, using
only a snapshot at or before that colour's release with its exact GPU proof.
Hybrid uses the same stable identity rule; its placement geometry remains
unchanged. FSR also resets its provider history when the newly consumed guide's
previous serial is not the last serial consumed by that context, or stream/epoch
changes. This addresses concrete discontinuities; it does not establish that all
reported FSR/Hybrid visual artifacts have the same cause.

The 16:32 Buffer trace measures median GPU copy execution of 0.141 ms but delays
before execution up to 14.6 ms. Sources were already complete in those retained
examples. V811 SteamVR D3D12 Buffer prepares the final copy on an independent
direct queue. After WaitImage, a binding-queue fence first transfers image
availability to that queue; the exact completed copy fence is then joined on the
runtime binding queue before release/EndFrame. This preserves the
[D3D12 image-availability contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html).
A refused optional queue or availability fence retains the previous
path. Normal/unbuffered, D3D11 and Legacy VDXR scheduling are unchanged. This is a
pacing candidate, not proof of stable headset delivery: another gap before
BeginFrame completion was not attributable in V810. Stage 33 / flight 5743 now
records real BeginFrame entry and return QPC to distinguish runtime blocking
from CPU scheduling. No further latency cap or phase correction is added.

## Verification and trial

New GPU regression cases cover four independent NR views at 100/50 percent with
one depth array, distinct sentinel slices and mixed unused-slice states; exact
colour/guide pixels, guards and independent histories are checked. Layer tests
exercise changing evaluation/release order for FSR and Hybrid. The actual FSR
provider is dispatched across continuous, skipped, stream, epoch and explicit
reset sequences. Both Framework and Director must prepare correct pixels while
the runtime queue is held behind an unrelated GPU fence.

Final suite: **82 of 84 checks pass**, no skips. The failures in
`xrfg_private_buffer_controls_gpu` and `xrfg_private_openxr_runtime_gpu` also
reproduce on the sealed V810 binaries in this session. They concern expired
native predictions and timing precision; their cause remains unresolved. Both
raw JUnit reports are retained, and packaging records these as failures, never
passes. This build is an experimental trial, not a fully validated release.
All five new regression cases pass, including both directions of image
availability transfer for Framework and Director.

The final JUnit and source/package receipts are retained under
`.local/diagnostics/msfs-quad-pacing-v811/` in the RIFE laboratory. Packaging
verifies matching symbols, pinned provider bytes and the unchanged shared RIFE
worker/models. No automatic installation, profile edit or publication occurs.

Owner trial: close game and old tray, start V811, relaunch. Compare Framework
Buffer OFF/ON with NR OFF; check NR BEFORE SR on both centres and peripheries;
then inspect FSR and Hybrid foveated quality on the ground. Director pacing can
be tested in another game because the owner reports MSFS Director unusable.
