# V813: unlock buffered preparation and preserve each eye's temporal guide chain

This change allows the next buffered image to prepare during native BeginFrame
and preserves each eye's native temporal guide chain. Four-view NR array-guide
handling is retained.

## Buffered preparation

The V811 diagnostic trace now records actual downstream BeginFrame entry and
return. In the final Framework window, the image was prepared before that call,
which blocked for a median 7.836 ms. A saved Buffer-ON window also shows the next
source complete during its predecessor's Begin, but its image acquisition/copy
starts only after that Begin returns, roughly 9 ms later in one example.

OFXR cleared its endpoint's `pending_target` only when releasing the prepared
image after Begin. That kept the next image from preparing during the native
call. With Buffer enabled, V813 releases only the GPU-complete FIFO head before
Begin. The following image can then be acquired and copied concurrently, and
remains unreleased until its own consumer turn. OpenXR's successful image-wait
requirement and native queue resource states are still honored; see the
[release contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrReleaseSwapchainImage.html).

Same-index alias protection remains active: a real image still referenced by an
earlier content End cannot be overwritten. The actual pixel/pose order, native
Begin/End serialization, source ownership and admission retirement are retained.
Failure is published before retiring the newly popped packet. Unbuffered order,
Legacy VDXR, scheduling clocks, latency caps and phase policy are unchanged.
The existing publication queue is retained. Stage 35 / flight 5745
records early release entry and return alongside the existing Begin trace.

A GPU fixture holds native Begin while submitting another image. With the same
test object linked against V812's runtime library, the next copy stays blocked
and the test fails. V813 prepares it during Begin, while still blocking a reused
physical index. Framework/Director, Buffer OFF, stop during Begin, Begin failure
and restart with fresh pixels/poses are covered. This demonstrates removal of
the OFXR gate, rather than a guarantee of stable SteamVR headset pacing.

## FSR and Hybrid guide continuity

The V811 identity fix could choose the correct eye but then mistake an older and
newer snapshot of that same eye for a single stream shared by both eyes. It
rewrote `previous_serial` as current minus two even though two independent eye
streams were active. That breaks the pair's native temporal chain and can cause
fallback/history resets. V813 preserves native previous serials on strict
multi-stream associations; the existing single-stream stereo rule and legacy
resolver behavior remain intact.

New assertions compare the resolved previous serial with the actual published
snapshot. Both FSR and Hybrid fixtures fail on the V811 resolver and pass after
the correction. Camera-placed foveated matching is also checked. This is a
concrete discontinuity fix, not confirmation that every reported foveated visual
artifact has this cause.

Rare FSR recovery/context events, existing sampled FSR inputs and changed eye
identities now also survive in the compact diagnostic sidecar when the verbose
flight file rotates. Deduplication occurs on the existing writer thread; no new
GPU capture, render-thread lock or per-frame diagnostic generation is added.

## Verification limits

Focused regressions cover the early-release ownership gate, reused physical
indices, shutdown/failure recovery, independent guide chains and actual FSR
provider dispatch. These checks do not establish stable headset pacing or
resolve all FSR/Hybrid foveated image-quality problems.
