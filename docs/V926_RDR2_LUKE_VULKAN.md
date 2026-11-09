# V926: RDR2 Luke Ross Vulkan stereo reconstruction

RDR2 now supplies OSR with an exact native camera, immutable DLSS guides and
CRC-verified source images through the Luke Vulkan path. The accepted r30
headset build is consolidated onto nightly V925. The temporary Luke Vulkan
pipeline test setting and its bypass/capture-only paths are removed.

## Camera and source identity

- Admit RDR2.exe through the existing Luke Vulkan gate. Flat RDR2 and other
  non-Luke processes remain outside capture.
- Discover REAL VR's RDR2 runtime through bounded PE/RTTI and linked camera
  code, rather than a version/timestamp allowlist or fixed addresses. Require
  a unique callback ABI, coherent selected/render counters and camera ring;
  validate the live owner and exact GPU-selected camera on every observation.
  Unknown or ambiguous layouts refuse capture. Structural ABI changes can
  still require an update.
- Freeze guides at the original public DLSS invocation and resolve its selected
  camera at the typed SR entry. Preserve exact command/feature, device, camera,
  resource and source-token ownership through successful submission. No latest
  camera, headset-pose or stale-frame substitutions.
- Decode RAGE right/up/view-Z once and preserve those axes in native history.
  Keep Vulkan HDR markers in the appropriate colour range and read them from
  immutable image history before reconstruction changes private scratch images.

## Reconstruction and lifetime

RDR2's raw vectors can describe either the adjacent production (N1) or the
previous same-eye production (N2). Classify the current field on the GPU using
retained exact cameras/depth; do not interpret an already-N2 field as N1 and
add a second stereo displacement. Near world geometry remains world geometry,
including the ground and character. Retain both verified real eye endpoints.

A motion field composed before its interval was classified must not become
trusted history motion. For RDR2's variable-interval input, retain colour,
depth and camera history but skip that motion copy and later extrapolation.
This removes the alternating displaced silhouette without increasing colour
or halo tolerances. Other games keep their verified history-motion path.

Reuse Vulkan transport slots only after both native and D3D12 work complete,
including when an executable command buffer retains a receipt. Order immutable
history consumers on the existing GPU queue when prior use is still pending;
do not reject an otherwise valid current source merely because that fence is
unfinished. Keep stale-ticket, active-lease, replay and cancellation guards.

## Evidence and validation

The owner confirmed r30 working in the headset with REAL VR 2611.8.4. In the
preceding F8 pair, all four resolved GPU classifications were N2. Raw character
motion was below 0.4 depth pixels while premature composition added roughly
+/-33 pixels; retaining half that displacement explained the alternating
silhouette. Corrected native and scene N2 matrices agreed within 1.7e-6.

The production GPU regression injects +/-32-pixel history motion, verifies it
changes output, and then proves rejection preserves exact colour/depth history
for both eyes and quality modes. It failed before the history fix and passed
afterward. The full current-motion GPU suite, camera configuration, structural
binding and Vulkan camera tests passed on r30, as did x64/x86 Release builds.
Structural discovery also passed on local REAL VR 2611.7.2 and 2611.8.4 images,
including altered timestamps, plus relocated and ambiguous synthetic fixtures.
No vendor DLL or shader bytes are included in the source or package.

The clean nightly build additionally checks settings, shipping Vulkan scope,
HDR markers, ordered history reuse and current-motion regression coverage.
Build and test logs accompany the local package outside the repository.
The full project suite is not a prerequisite for this focused change.

GTA's separate optional head-lock binding is unchanged. This change does not
claim compatibility for that reader with newer REAL VR builds, or a measured
FPS gain without a matched compositor-output comparison.
