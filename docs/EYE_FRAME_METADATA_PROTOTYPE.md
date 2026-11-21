# Per-eye frame metadata prototype

OFXR accepts the original render identity of each eye at swapchain release.
AER and Full OSR can use that identity to select the matching depth and
motion vectors, with pixel markers available as a fallback. The REAL VR
adapter verifies the loaded module and active runtime methods. It uses the
same capture and selection rules across supported builds.

In the r12 live runs, GTA V Enhanced recovered 206 marker misses through
metadata. TLOU1 used metadata for 1,954 guide selections that agreed with
readable markers. TLOU1 recorded no marker misses during the sustained AER
segment. Neither run produced a same-render release into a different slot.

## Publishing a release

The C ABI is in `include/xrfg/eye_frame_metadata.h`. Resolve
`ofxrReleaseSwapchainImageWithEyeMetadataV1` from the loaded OFXR layer. Pass
the application's normal `xrReleaseSwapchainImage` function, the swapchain
and release info, and the metadata for the waited image. If the export is
unavailable, call the application's normal release function directly.
The wrapper scopes the metadata to that synchronous release and preserves
the original OpenXR result. Use the same thread for metadata publication and
the release.

For each eye in the image, publish the source's stream ID and render ID.
Preserve both when the eye is held or copied to another image. Change stream
ID when the producer resets its render numbering. Use the index returned by
`xrAcquireSwapchainImage` for the destination slot. A split-eye release
has one record. A packed image can carry two with different render IDs.
Supply the captured OFXR guide token only when the producer can join that
publication to the source render. Use zero when a guide token is unavailable.

```cpp
OfxrEyeFrameMetadataV1 frame{
    sizeof(frame), producer, acquired_index, 1, {source_identity}
};
return publish ? publish(release_image, chain, release_info, &frame)
               : release_image(chain, release_info);
```

The observation is published once downstream accepts the release and OFXR
can identify the waited image. An ordinary release clears the previous
observation. A failed release leaves the last successful observation in
place. If OFXR defers a bridged release, it retains the sideband until the
downstream release. The optional getter returns the latest accepted
observation per swapchain for diagnostics.

## Producer integration points

- **REAL VR** carries the ID from source selection to the shared OpenXR
  colour release on supported module builds. The D3D12 guide producer
  records the active selected render after verifying its camera ring.
- **UEVR** can attach the ID at the D3D12 AFR eye copy, where the source
  texture, eye and destination chain are available. A producer adapter
  needs to preserve the original ID of a reused eye and correlate it
  with the captured guide. OFXR already observes releases and camera
  callbacks there.
- **MUTAR** can publish from the D3D12 eye copy shared by the pinned
  AnvilEngine2VR and Starfield2VR frameworks. Attach the render ID to
  the copied source. The presenter counter tracks submission cadence.

UEVR and MUTAR can call the exported ABI from their eye-copy paths when
they can identify the exact source render and its guide.

## Evidence to collect before switching guide selection

The `eye_frame_metadata` flight operation uses `a` for swapchain and `b`
for release serial. Result 0 records the destination index in `c`. Result
`2 * producer + eye` records the original render ID in `c`, and result
`16 + 2 * producer + eye` records the stream ID. Result 35 records the
associated history capture serial. Results 32, 33, and 34 mean guide
token match, mismatch, or unavailable marker. Their `c` packs the
metadata guide token above the decoded marker token.

The copied-forward check needs two successful releases for the same eye
with different destination slots and the same source stream and render ID.
The other eye should advance. For the REAL VR live check, correlate both
receipts with successful marker decodes for the captured inputs. Repeat
across a scene transition to exercise changes in the source stream.

## Validation on V1026

The isolated API-layer fixtures exercise the native and Independent
delivery paths. For each of the three producer IDs, two different image
slots carry left-eye render 101 while the right eye advances from 102 to
103. Invalid eye assignments and wrong slots produce no receipt. A
native runtime failure preserves the previous receipt and an ordinary
retry leaves the new release without metadata.

Archived REAL VR flight logs confirm the held-eye case on stock builds.
PID 16524, frames 3772–3773, retained right-eye marker token 79 and
capture 999 while left-eye marker 78 advanced to 80. PID 19256, frames
5674–5675, retained left-eye token 74 and capture 1476 while right-eye
marker 73 advanced to 75. In both examples the held eye's capture serial
stayed the same. Those runs provide evidence for reuse of a captured eye.
A fresh REAL VR capture is required to verify a copy into a new slot.

## Live check with r2

The adapter recognizes the x64 REAL VR binaries for **RDR2 2611.18.2.5731**
and **GTA V Enhanced 2611.8.4.5703**. It validates the PE identity, the
eye-draw and source-selection instructions, and the OpenXR swapchain
release method. The adapter reads the render ID from the selected colour
record, carries it through that draw's release, and checks the acquired
slot against OFXR's ownership tracking. It covers the original eye draw
and the branch that selects one of two retained textures. Blended draws
do not emit a single-source ID.

1. Run the r2 build with the game's existing working OpenXR setup. Keep
   OSR or AER with Hybrid, FSR, or DLSS-G active to exercise the existing
   pixel marker reader.
2. Turn on flight logging with F9. Capture some normal gameplay and a scene
   transition, then stop logging or exit the game. The trace is under
   `%LOCALAPPDATA%\ofxr-bridge\RuntimeLayer\v1026`.
3. From the unpacked r2 folder run
   `python tools/Check-RealVrEyeMetadata.py "path-to-game-flight.log"`,
   or share that flight log for inspection.

The report looks for distinct release serials and image slots on the
same eye and stream. A confirmed copy has the same source render ID
and the same independently decoded pixel marker. If the game only
holds the old swapchain image, the report is inconclusive. The adapter
and marker have separate ID namespaces, so the report compares whether
the two IDs stay the same across releases.

For the REAL VR adapter, result 36 or 37 logs the independently decoded
marker for the corresponding eye. Result 38 or 39 records an unavailable
marker. Result 40 marks adapter installation if logging is already active.

## r4 metadata selection in RDR2

Each eye must first see two different releases where a readable pixel
marker identifies a native guide with the same original render ID. This
binds the compositor stream to that eye's native guide stream and epoch.
The lookup uses the provider owner and full render sequence. The offset
seen between the marker and render counters in r2 is observational.

A calibrated release prefers metadata when the exact guide and its
history are available. The existing markers are still written. Missing or
stale guides fall back to the original marker reader. During F9 logging,
the reader also audits metadata hits. A different readable marker
invalidates the binding and takes priority. An ambiguous marker clears
the binding and follows the existing refusal path.

Use r4 with the same RDR2 version above. Play a stretch with F9 enabled
to capture agreement and recovery, then turn F9 off to exercise the
normal metadata path without the extra audit reads. Test Hybrid AER and
OSR separately so the log identifies which guide consumer ran.

In `eye_frame_metadata`, results 42–47 use `a` for the swapchain and
`b` for the colour capture serial.

| Result | Meaning |
|---|---|
| 42 | Metadata selected and the marker agreed |
| 43 | The marker was selected, including calibration releases |
| 44 | The marker disagreed or was ambiguous, so metadata was discarded |
| 45 | Metadata selected a guide while the marker was unreadable |
| 46 | Metadata selected a guide without a marker read |
| 47 | The marker guide's native render ID differs from the release ID. Field `c` is the guide's native render ID |

## r5 motion interval decision in RDR2 AER

The release identity records which render produced the image. The original
motion vectors can reference either of two earlier renders. A decisive
current colour vote selects its interval immediately. A smaller majority
can select an interval when both exact preceding endpoints classified to
that same interval. Both history branches must have direct evidence within
four steps. Insufficient votes, ties, crossed history and a weak vote
against either endpoint still leave the interval unclassified.

In the r4 AER trace for PID 52492, the 45 skipped attempts across four
dropouts came from 26 distinct publications. Replaying their recorded votes
and adjacent publications with this rule resolved all 26 to two renders.
Live delivery and visual quality still need a headset run.

With F9 on, `luke_afw_stage` stage `a=1450` reports the chosen interval and
the original votes. Stage `a=1451` reports a decision supported by both
previous endpoints. Its result is the interval, `b` is the publication and
`c` is how far the decision is from fresh direct evidence.

## r6 build on the RDR2 memory branch

r6 applies the shared eye metadata and r5 AER decision to local commit
`65ab5a6`. This base includes RDR2 guide reuse, reduced input mirroring,
and Hybrid scratch allocation sized for active eyes and outputs.

The current frame's point and patch votes run first. A conclusive GPU
decision takes precedence. The r5 history rule can resolve a still-close
point vote with support from both exact preceding endpoints.

For r6, stage `a=1451` records the patch vote and the GPU decision. Stage
`a=1452` records a decision that used history. Its result is the interval,
`b` is the publication and `c` is the corroboration age.

## r7 camera and depth evidence for AER

The RDR2 AER classifier can use the original depth and the cameras of
both exact earlier renders when the colour evidence is inconclusive.
It checks how closely the current motion agrees with the reprojection
of stationary scene geometry into each earlier image. A camera decision
requires at least 96 matching samples and a greater than 3-to-1 lead.
Missing depth, an unmatched camera history or two similar reprojections
cannot provide that evidence.

The classifier reads the existing depth snapshot in the same dispatch.
Stage `a=1453` reports the camera result and vote counts in the same
format as stages 1450 and 1451. A headset run is required to see how
often this resolves the remaining r6 hitches.

## r8 shared motion interval decision

AER and Full OSR use the same shader to compare single pixels, small
patches, and camera reprojection against the two exact earlier images.
OSR runs it in its existing GPU clear pass and retains the interval with
the original motion field. AER can also use its existing history
corroboration when the GPU result is inconclusive.

The producer declares whether the motion interval varies. Camera evidence
requires verified native view axes, depth and motion for the same render,
and three consecutive camera observations on the same producer clock.
Frame identity still tries metadata first where it is supported, with
pixel markers for calibration, comparison, and fallback.

## r9 shared metadata selection

AER and Full OSR try metadata from the exact captured swapchain release.
Any producer can supply an OFXR guide token with its render ID. An adapter
that supplies only a render ID can reuse a marker-verified guide for that
same eye, stream, and render while the guide is still retained. New render
IDs can resolve directly when the producer has a matching native guide
clock, as RDR2 does. A fresh GTA V Enhanced render still needs a marker
to establish its guide identity.

The shared policy falls back to the existing pixel reader for missing or
unresolvable metadata. F9 keeps the independent marker comparison. A readable
marker disagreement clears the binding and selects the marker. An ambiguous
marker uses the existing refusal path. Game integrations without a compatible
metadata feed continue on their existing paths. The UEVR AFR OSR path has
its own guide lookup.

## r10 guide render identity

A guide producer can tag a captured depth and motion pair with the same
producer, owner, and original render ID as the eye release. Each eye confirms
that match on two different renders with readable markers, within one guide
stream and epoch. After that, the exact ID can resolve a fresh or repeated
render. The lookup still checks guide history and GPU ownership. A readable
marker that identifies a different guide still takes priority and clears the
binding. An independently matched guide can now survive a missing or ambiguous
marker while F9 is comparing them.

The GTA V Enhanced adapter tags the original DLSS capture inside the accepted
public evaluation scope. For REAL VR 2611.8.4.5703 it reads the selected render
and its matching camera ring entry, using code and vtable checks on the
installed module. The game-thread render counter and the NGX publication
sequence are separate. Unsupported or unstable reads leave the marker path
available.

For a live test, play GTA in Hybrid AER or Full OSR with F9 on. In
`eye_frame_metadata`, result 49 records a guide's producer owner, original
render ID, and token in `a`, `b`, and `c`. Result 42 means the guide selected
from metadata agreed with the marker. Result 45 means metadata selected the
guide despite a failed decode. Result 50 identifies the ambiguous-marker
subset of those recoveries. Results 47 and 48 report a render or owner
disagreement during calibration.

## r11 shared REAL VR capture

The completed GTA r10 trace for PID 26656 has 9,500 marker-confirmed guide
matches. Its DLSS and compositor owners are separate, and every release
render ID is one ahead of the associated DLSS capture ID.

The same rule applies to any guide producer with a verified render ID.
Four distinct, advancing CRC-confirmed matches on one eye bind the release
owner to the guide owner and establish a stable offset of zero or one render
in either direction. The binding is scoped to one guide stream and epoch.
A token disagreement clears it and uses the readable pixel marker. Missing
or ambiguous markers can use the established binding if the exact guide
and its history are still available.

The shared D3D12 DLSS capture reads REAL VR's selected render only when the
loaded module and active runtime methods match the eye and camera-ring ABI.
GTA V Enhanced, Kingdom Come II, TLOU1, Ghost of Tsushima, and Spider-Man
Remastered installations were checked against those witnesses. Each game's
existing DLSS capture boundary is retained. RDR2's Vulkan path is unchanged.
At r11, older modules such as Cyberpunk 2077's REAL VR 26.3 build stayed on
the marker path.

With F9 enabled, result 51 confirms a binding, 42 confirms that metadata and
the readable marker agree, and 50 confirms recovery from an ambiguous marker.

## r12 stale-guide worker recovery

If either eye refuses a stale guide, the worker retires the partial stereo
transaction after its GPU fences complete. Independent pacing skips that
transaction and primes both eyes on the next source. The serial worker can
prime the current source as real-only, keeping its existing presentation slot.
GPU or swapchain release failures still propagate. The metadata lookup and
pixel marker fallback are unchanged.

With F9 on, `presenter_transition` result 99413 identifies an abandoned pair
that finished cleanup. Later successful `generation_prepare` entries in
the same session show the worker resumed.

The worker regression test forces a stale pair with one eye's GPU work
already submitted. The GTA r12 run did not hit 99413, so live verification
of this recovery remains open.

Test RDR2 in Hybrid AER and Full OSR separately. With F9 enabled, AER
stage 1453 records camera votes. In OSR, bit 3 of stage 1382 records
verified camera inputs. An F8 capture puts the GPU decision in word 5 of
`osr_scene_state` and its evidence in word 8. Evidence codes are 0 for
inconclusive, 1 for points, 2 for patches, and 3 for camera reprojection.

## REAL VR 26.3 support

The shared adapter recognizes the 26.3.0.25587 DLL used by Cyberpunk 2077.
The D3D12 DLSS evaluation wrapper supplies the active runtime owner. The
adapter verifies the selected render in that owner's camera ring and checks
that the depth method uses the same render to select the eye. On release,
the eye draw supplies the render ID beside the original or retained source
texture. The retained-source probe checks the texture choice before the draw.
Other draw paths can still use pixel markers.

Both handoffs were verified against the installed DLL offline. A headset
trace is still needed to confirm guide binding (result 51), metadata and
marker agreement (42), and metadata recovery on marker misses (45).

## Discovering compatible REAL VR updates

The eye metadata adapter discovers the eye draw and the DLSS-side runtime
owner from the loaded module's instructions. It checks the OpenXR
swapchain type, ties the source texture and render-ID instructions to
the same draw through unwind metadata, and verifies the camera and depth
methods on the active runtime object. The PE timestamp is logged for
diagnostics. It does not select a layout.

The same checks cover the installed Uncharted 4 and Lost Legacy DLL and
both runtime types in REAL VR 2611.18.2. Pixel markers remain the fallback
if a new build changes the handoffs or the adapter cannot verify them.
